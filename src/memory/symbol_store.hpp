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

#include "memory/pdb.hpp"
#include "memory/symbol_server.hpp"

// Per-module symbols, loaded off the UI thread.
//
// The split that keeps this safe: the caller reads everything that needs the
// target process (the CodeView record, the section table) and puts it in a job.
// The worker then only touches the file system and the network, so a process
// that exits mid-load can't be read through a stale handle.
namespace mem {

enum class SymStatus {
    None,     // never asked for
    Queued,
    Loading,
    Loaded,
    Failed,   // no PDB found, or it wouldn't parse; `note` says which
};

struct ModuleSymbols {
    // The arena backing every string in `syms`. Declared first so it outlives
    // them (members destruct in reverse order), and monotonic so freeing it is a
    // handful of buffer releases regardless of how many symbols it holds. Its
    // non-movable resource makes ModuleSymbols non-movable too - hence the
    // unique_ptr in the maps below, so only pointers ever move.
    std::pmr::monotonic_buffer_resource arena;

    SymStatus   status = SymStatus::None;
    std::string note;    // where they came from, or why they're missing
    std::string pdbPath; // the file that was actually loaded
    PdbSymbols  syms{&arena};
};

// Value type of the store's maps: the symbols are non-movable (arena), so they
// live behind a pointer and only the pointer is ever handed around.
using ModuleSymbolsPtr = std::unique_ptr<ModuleSymbols>;

struct SymbolSettings {
    bool enabled   = true;  // master switch for PDB symbols
    bool useServer = false; // off until the user opts in: it's network traffic

    // Tried in order until one has the build. Microsoft's covers the system
    // DLLs, Unity's covers Unity games. Same protocol as Mozilla, Chromium,
    // NVIDIA and in-house stores, so the list is open-ended.
    std::vector<std::string> serverUrls{
        "https://msdl.microsoft.com/download/symbols",
        "https://symbolserver.unity3d.com",
    };

    // Where downloads land, and the first place a local search looks. Worth
    // pointing at an existing x64dbg/WinDbg cache: the on-disk layout is the
    // same, so a cache built by another debugger is reused as-is.
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
    // next pump() that replaces it, or clear().
    const ModuleSymbols* find(uintptr_t modBase) const;
    const std::unordered_map<uintptr_t, ModuleSymbolsPtr>& all() const { return map_; }

    // Stop the worker and drop every symbol. Called when the target goes away.
    void clear();

    // Cancel the batch in flight: abort the current download, empty the queue,
    // and drop the queued-but-unstarted modules back to "never asked" so a later
    // Load All re-queues them. The worker keeps running for the next request.
    void cancelPending();

    bool                    busy() const;
    size_t                  pending() const;
    std::string             activeName() const; // module being worked on
    const DownloadProgress& progress() const { return prog_; }

private:
    void workerMain();
    void ensureWorker();
    ModuleSymbolsPtr runJob(const SymbolJob& job);

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
