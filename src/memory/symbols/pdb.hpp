#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Reads public symbols out of a .pdb - enough to name an address and look one up
// by name (walks the MSF container, DBI header and symbol stream; types, locals
// and line info ignored). Touches no process memory: the caller hands it data.
namespace mem {

struct PdbSymbol {
    uint32_t         rva;
    uint32_t         size;   // to the next symbol; publics carry no size of their own
    bool             isFunc;
    std::pmr::string name;   // undecorated where the PDB held a mangled C++ name
};

// A module section as (rva, size), to turn a symbol's section:offset into an RVA.
struct PdbSection { uint32_t rva; uint32_t size; };

// Transparent hash/equality: lets byName be searched by string_view without
// building a throwaway std::pmr::string key for every lookup.
struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept
    { return std::hash<std::string_view>{}(s); }
};
struct SvEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept
    { return a == b; }
};

// Every string for a module lives in one arena (see ModuleSymbols), so dropping
// the module is a few buffer releases, not a free per symbol. Must outlive this.
struct PdbSymbols {
    std::pmr::vector<PdbSymbol> byRva;  // sorted by rva, then name
    // lower-cased name -> rva; mangled names indexed alongside the undecorated form.
    std::pmr::unordered_map<std::pmr::string, uint32_t, SvHash, SvEq> byName;
    // Display case, sorted case-insensitively, for the autocomplete's prefix scan.
    std::pmr::vector<std::pmr::string> names;

    explicit PdbSymbols(std::pmr::memory_resource* mr)
        : byRva(mr), byName(mr), names(mr) {}

    bool empty() const { return byRva.empty(); }
};

struct PdbLoadRequest {
    std::string             path;      // the .pdb to read
    std::vector<PdbSection> sections;  // module sections, in PE header order
    uint8_t                 guid[16] = {};
    uint32_t                age      = 0;
    bool                    verify   = true; // reject a .pdb from another build

    // Polled while parsing a large PDB so an exiting process doesn't wedge the
    // worker's join(). Null means the parse always runs to completion.
    const std::atomic<bool>* cancel = nullptr;
};

// Parse `req.path` into `out`. False + `error` when the file is missing, malformed,
// stripped, from another build, or cancelled mid-parse.
bool load_pdb(const PdbLoadRequest& req, PdbSymbols& out, std::string& error);

} // namespace mem
