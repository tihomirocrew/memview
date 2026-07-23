#include "memory/symbol_store.hpp"

#include <cstring>

namespace mem {

SymbolStore::~SymbolStore() { clear(); }

void SymbolStore::ensureWorker()
{
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread(&SymbolStore::workerMain, this);
}

void SymbolStore::request(SymbolJob job, bool force)
{
    if (!force)
    {
        if (inflight_.count(job.modBase)) return;
        auto it = map_.find(job.modBase);
        if (it != map_.end() && it->second.status != SymStatus::None) return;
    }

    job.cfg = cfg_;
    if (job.cfg.cacheDir.empty()) job.cfg.cacheDir = default_symbol_cache();

    inflight_.insert(job.modBase);
    map_[job.modBase].status = SymStatus::Queued;

    {
        std::lock_guard<std::mutex> lk(mu_);
        // A forced reload drops any pending job for the same module first, so
        // Reload pressed twice doesn't parse the same PDB twice.
        if (force)
            for (auto it = queue_.begin(); it != queue_.end(); )
            {
                if (it->modBase == job.modBase) it = queue_.erase(it);
                else                            ++it;
            }
        queue_.push_back(std::move(job));
    }
    ensureWorker();
    cv_.notify_one();
}

void SymbolStore::setFailed(uintptr_t modBase, std::string note)
{
    ModuleSymbols& m = map_[modBase];
    m.status = SymStatus::Failed;
    m.note   = std::move(note);
}

bool SymbolStore::pump()
{
    std::vector<std::pair<uintptr_t, ModuleSymbols>> ready;
    uintptr_t active = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ready.swap(done_);
        active = activeBase_;
    }

    for (auto& [base, result] : ready)
    {
        map_[base] = std::move(result);
        inflight_.erase(base);
    }

    // The worker can't touch map_ (main thread only), so flip the module it's
    // working on right now to Loading here. A result that landed above already
    // set the final status and wins.
    if (active)
        if (auto it = map_.find(active);
            it != map_.end() && it->second.status == SymStatus::Queued)
            it->second.status = SymStatus::Loading;

    return !ready.empty();
}

const ModuleSymbols* SymbolStore::find(uintptr_t modBase) const
{
    auto it = map_.find(modBase);
    return it == map_.end() ? nullptr : &it->second;
}

void SymbolStore::clear()
{
    if (worker_.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            queue_.clear();
        }
        // Cancel any download in flight, or the join below waits for megabytes.
        prog_.cancel.store(true);
        cv_.notify_all();
        worker_.join();
        prog_.cancel.store(false);
    }

    std::lock_guard<std::mutex> lk(mu_);
    done_.clear();
    active_.clear();
    activeBase_ = 0;
    map_.clear();
    inflight_.clear();
}

void SymbolStore::cancelPending()
{
    // Empty the queue and cancel the in-flight download, both under the lock the
    // worker takes to pop a job - so a job popped in the same instant still sees
    // the cancel instead of slipping past. The worker clears the flag on its next
    // job; the aborted module lands as Failed("cancelled").
    std::deque<SymbolJob> dropped;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dropped.swap(queue_);
        prog_.cancel.store(true);
    }

    // map_/inflight_ are main-thread only, so clearing the dropped modules here
    // is safe. Otherwise they'd stay "queued" and request() wouldn't re-queue them.
    for (const SymbolJob& job : dropped)
    {
        map_.erase(job.modBase);
        inflight_.erase(job.modBase);
    }
}

bool SymbolStore::busy() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return !queue_.empty() || !active_.empty();
}

size_t SymbolStore::pending() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size() + (active_.empty() ? 0 : 1);
}

std::string SymbolStore::activeName() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

void SymbolStore::workerMain()
{
    for (;;)
    {
        SymbolJob job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            active_     = job.modName;
            activeBase_ = job.modBase;

            // Clear a leftover cancel from a previous cancelPending(), under the
            // same lock it sets the flag with - so a Cancel arriving right as this
            // job is popped is honoured, not lost. (clear() returns above, so its
            // cancel flag survives for the join.)
            prog_.cancel.store(false);
        }

        // Reset the byte counters per job: a job that loads locally never touches
        // the download path, and stale totals would mislabel it as "Downloading".
        prog_.received.store(0);
        prog_.total.store(0);

        ModuleSymbols result = runJob(job);

        {
            std::lock_guard<std::mutex> lk(mu_);
            done_.emplace_back(job.modBase, std::move(result));
            active_.clear();
            activeBase_ = 0;
        }
    }
}

ModuleSymbols SymbolStore::runJob(const SymbolJob& job)
{
    ModuleSymbols out;
    out.status = SymStatus::Failed;
    out.note   = "no PDB found for this module";

    PdbLoadRequest req;
    req.sections = job.sections;
    req.age      = job.ref.age;
    req.verify   = job.verify;
    memcpy(req.guid, job.ref.guid, sizeof(req.guid));

    std::vector<std::string> candidates;
    if (!job.forcedPath.empty())
        candidates.push_back(job.forcedPath);
    else
        candidates = pdb_search_candidates(job.modPath, job.ref,
            job.cfg.cacheDir, job.cfg.extraDirs);

    for (const std::string& path : candidates)
    {
        req.path = path;
        std::string err;
        if (load_pdb(req, out.syms, err))
        {
            out.status  = SymStatus::Loaded;
            out.pdbPath = path;
            out.note.clear();
            return out;
        }
        // Keep the last reason: with several candidates, the one that came
        // closest to working is the useful thing to show.
        out.note = path + ": " + err;
    }

    // Nothing local matched. Servers are keyed by GUID, so anything one returns
    // is the right build - trying them in a row is safe, the rest just 404.
    if (!job.forcedPath.empty() || !job.cfg.useServer) return out;

    for (const std::string& server : job.cfg.serverUrls)
    {
        if (server.empty()) continue;
        if (prog_.cancel.load()) break;

        prog_.received.store(0);
        prog_.total.store(0);

        std::string downloaded, err;
        if (!download_pdb(server, job.cfg.cacheDir, job.ref, prog_, downloaded, err))
        {
            out.note = server + ": " + err;
            continue; // 404 here just means the next store might have it
        }

        req.path = downloaded;
        if (load_pdb(req, out.syms, err))
        {
            out.status  = SymStatus::Loaded;
            out.pdbPath = downloaded;
            out.note.clear();
            return out;
        }
        // A server that answers with an unusable file is worth reporting, but
        // shouldn't stop the ones behind it.
        out.note = server + ": downloaded PDB is unusable: " + err;
    }
    return out;
}

} // namespace mem
