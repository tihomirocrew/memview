#pragma once
#include <atomic>
#include <string>
#include <vector>

#include "memory/pe_symbols.hpp"

// Finding the .pdb that belongs to a module: the local search order, and the
// Microsoft symbol server fetch that backs it up.
namespace mem {

// The symbol-server directory name for a build: the GUID as 32 uppercase hex
// digits (Data1/2/3 byte-swapped out of their little-endian layout), then the
// age in hex with no padding - "8D4B...5B6C" + "1".
std::string pdb_key(const PdbRef& ref);

// Where a downloaded .pdb lives: <cache>\<name>\<key>\<name>, the same layout
// symsrv and every other debugger uses, so an existing cache is shared.
std::string pdb_cache_path(const std::string& cacheDir, const PdbRef& ref);

// %APPDATA%\MemView\symbols (next to config.json)
std::string default_symbol_cache();

// The .pdb sitting next to the module on disk ("game.exe" -> "game.pdb"), the
// usual layout for a build you made yourself. Empty if `modulePath` has no
// directory part.
std::string pdb_next_to_module(const std::string& modulePath, const PdbRef& ref);

// Local .pdb files worth trying for `ref`, best first: next to the module, the
// path from build time, the download cache, then the user's extra dirs. Only
// existing files are listed; load_pdb still validates each and rejects a stale
// namesake, so the caller just tries them in order.
std::vector<std::string> pdb_search_candidates(const std::string& modulePath,
    const PdbRef& ref, const std::string& cacheDir,
    const std::vector<std::string>& extraDirs);

// Shared with the UI while a fetch is in flight.
struct DownloadProgress {
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0};    // 0 when the server sends no length
    std::atomic<bool>     cancel{false};
};

// GET <server>/<name>/<key>/<name> into the cache, writing through a .tmp file
// so an interrupted download can't leave a truncated .pdb behind. On success
// `outPath` is the cached file. Compressed (.pd_) and file.ptr responses aren't
// handled - msdl serves plain .pdb for everything current.
bool download_pdb(const std::string& serverUrl, const std::string& cacheDir,
    const PdbRef& ref, DownloadProgress& prog, std::string& outPath,
    std::string& error);

} // namespace mem
