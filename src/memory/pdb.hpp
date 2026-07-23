#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Reads public symbols out of a .pdb: the MSF container, the DBI header, and the
// symbol record stream. Only what it takes to name an address and to find an
// address by name - types, locals and line info are ignored.
//
// Nothing here touches the target process: a caller collects the module's PDB
// reference and section table first, then hands this plain data to a worker.
namespace mem {

struct PdbSymbol {
    uint32_t         rva;
    uint32_t         size;   // to the next symbol; publics carry no size of their own
    bool             isFunc;
    std::pmr::string name;   // undecorated where the PDB held a mangled C++ name
};

// A module section as (rva, size), to turn a symbol's section:offset into an RVA.
struct PdbSection { uint32_t rva; uint32_t size; };

// Transparent hash/equality so byName can be looked up by std::string_view (or a
// std::string) without materialising a std::pmr::string key just to search.
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

// All of a module's symbol strings live in one arena (see ModuleSymbols), so
// these containers allocate from it, not the process heap - freeing the whole
// module is then a couple of buffer releases, not a string free per symbol. The
// resource is passed in at construction and must outlive the PdbSymbols.
struct PdbSymbols {
    std::pmr::vector<PdbSymbol> byRva;  // sorted by rva, then name
    // lower-cased name -> rva. Mangled names are indexed alongside their
    // undecorated form, so either spelling resolves.
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

    // Polled while parsing a large PDB, so a process that exits mid-load doesn't
    // wedge the worker's join(). Null means the parse always runs to completion.
    const std::atomic<bool>* cancel = nullptr;
};

// Parse `req.path` into `out`. False with `error` set when the file is missing,
// malformed, stripped of symbols, belongs to a different build, or `req.cancel`
// was raised while parsing.
bool load_pdb(const PdbLoadRequest& req, PdbSymbols& out, std::string& error);

} // namespace mem
