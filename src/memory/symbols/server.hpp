#pragma once
#include <atomic>
#include <string>
#include <vector>

#include "memory/pe.hpp"

// Locating the .pdb for a module: where to look on disk, and the symbol-server
// download that backs it up when nothing local matches.
namespace mem {

// The symbol-server directory name: GUID as 32 uppercase hex digits (Data1/2/3
// byte-swapped from little-endian) + age in hex, unpadded - "8D4B...5B6C" + "1".
std::string pdb_key(const PdbRef& ref);

// Where a downloaded .pdb lives: <cache>\<name>\<key>\<name>, the same layout
// symsrv and every other debugger uses, so an existing cache is shared.
std::string pdb_cache_path(const std::string& cacheDir, const PdbRef& ref);

// %APPDATA%\MemView\symbols (next to config.json)
std::string default_symbol_cache();

// The .pdb sitting next to the module ("game.exe" -> "game.pdb"), the usual
// layout for your own builds. Empty if `modulePath` has no directory part.
std::string pdb_next_to_module(const std::string& modulePath, const PdbRef& ref);

// Local .pdb files worth trying for `ref`, best first: next to the module, the
// build-time path, the cache, then the user's extra dirs. Only existing files;
// load_pdb still validates each, so the caller just walks them in order.
std::vector<std::string> pdb_search_candidates(const std::string& modulePath,
    const PdbRef& ref, const std::string& cacheDir,
    const std::vector<std::string>& extraDirs);

// Shared with the UI while a fetch is in flight.
struct DownloadProgress {
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0};    // 0 when the server sends no length
    std::atomic<bool>     cancel{false};
};

// GET <server>/<name>/<key>/<name> into the cache via a .tmp, so an interrupted
// download can't leave a truncated .pdb. `outPath` is the cached file on success.
// Compressed (.pd_) / file.ptr responses aren't handled - msdl serves plain .pdb.
bool download_pdb(const std::string& serverUrl, const std::string& cacheDir,
    const PdbRef& ref, DownloadProgress& prog, std::string& outPath,
    std::string& error);

} // namespace mem
