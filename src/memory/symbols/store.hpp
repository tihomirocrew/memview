#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "memory/symbols/pdb.hpp"
#include "memory/symbols/server.hpp"

// Per-module symbols, loaded off the UI thread.
//
// Kept safe by a split: the caller reads everything that needs the target process
// (CodeView record, section table) into a job; the worker only touches disk/net.
namespace mem {

enum class SymStatus {
    None,     // never asked for
    Queued,
    Loading,
    Loaded,
    Failed,   // no PDB found, or it wouldn't parse; `note` says which
};

struct ModuleSymbols {
    // The arena behind every string in `syms`. Declared first so it outlives them;
    // non-movable, so ModuleSymbols is too - hence the unique_ptr in the maps below.
    std::pmr::monotonic_buffer_resource arena;

    SymStatus   status = SymStatus::None;

    bool        permanent = false; // nothing to search: no debug record at all
    bool        cancelled = false; // search cut short, so the miss proves nothing

    std::string note;    // where they came from, or why they're missing
    std::string pdbPath; // the file that was actually loaded
    PdbSymbols  syms{&arena};
};

// Value type of the store's maps: symbols are non-movable (arena), so they live
// behind a pointer and only the pointer ever gets passed around.
using ModuleSymbolsPtr = std::unique_ptr<ModuleSymbols>;

struct SymbolSettings {
    bool enabled   = true;  // master switch for PDB symbols
    bool useServer = false; // off until the user opts in: it's network traffic

    // Tried in order until one has the build: Microsoft's for system DLLs, Unity's
    // for Unity games. Same protocol as Mozilla/Chromium/NVIDIA, so it's open-ended.
    std::vector<std::string> serverUrls{
        "https://msdl.microsoft.com/download/symbols",
        "https://symbolserver.unity3d.com",
    };

    // Where downloads land, and the first place a local search looks. Point it at
    // an existing x64dbg/WinDbg cache and it's reused as-is - same on-disk layout.
    std::string cacheDir;          // empty -> default_symbol_cache()
    std::vector<std::string> extraDirs;
};

// Everything the worker needs, with no reference back to the process.
struct SymbolJob {
    uintptr_t               modBase = 0;
    std::string             modName;
    std::string             modPath;
    PdbRef                  ref;
    std::vector<PdbSection> sections;
    std::string             forcedPath; // set by "Load PDB..."; skips the search
    bool                    verify = true;
    SymbolSettings          cfg;
};

class SymbolStore {
public:
    ~SymbolStore();

    SymbolSettings&       settings()       { return cfg_; }
    const SymbolSettings& settings() const { return cfg_; }

    // Queue a load. Ignored when that module is already loaded or in flight,
    // unless `force` (used by Reload and by an explicit "Load PDB...").
    void request(SymbolJob job, bool force = false);

    // Record a module we can't even try: no debug directory, no section table.
    void setFailed(uintptr_t modBase, std::string note);

    // Call once a frame. Publishes finished loads; true if anything landed.
    bool pump();

    // Null until a load for that module has finished. Stays valid until the
    // next pump() that replaces it, retryFailed() that drops it, or clear().
    const ModuleSymbols* find(uintptr_t modBase) const;
    const std::unordered_map<uintptr_t, ModuleSymbolsPtr>& all() const { return map_; }

    // Stop the worker and drop every symbol. Called when the target goes away.
    void clear();

    // Cancel the batch in flight: abort the current download, empty the queue,
    // and reset the queued-but-unstarted modules to "never asked" so a later
    // Load All picks them up again. The worker stays alive for the next request.
    void cancelPending();

    // Forget "no PDB found" so it gets looked for again: pass false when the
    // search settings changed, true for just the cancelled ones. Returns the
    // count dropped.
    size_t retryFailed(bool cancelledOnly);

    bool                    busy() const;
    size_t                  pending() const;
    std::string             activeName() const; // module being worked on
    const DownloadProgress& progress() const { return prog_; }

private:
    void workerMain();
    void ensureWorker();
    ModuleSymbolsPtr runJob(const SymbolJob& job);

    SymbolSettings jobSettings() const; // cfg_ with the default cache filled in

    // Main thread only.
    std::unordered_map<uintptr_t, ModuleSymbolsPtr> map_;
    std::unordered_set<uintptr_t>                   inflight_;
    SymbolSettings                                  cfg_;

    // Shared with the worker.
    mutable std::mutex                                    mu_;
    std::condition_variable                               cv_;
    std::deque<SymbolJob>                                 queue_;
    std::vector<std::pair<uintptr_t, ModuleSymbolsPtr>>   done_;
    std::string                                           active_;
    uintptr_t                                             activeBase_ = 0;
    std::thread                                           worker_;
    bool                                                  stop_ = false;

    DownloadProgress prog_;
};

} // namespace mem
