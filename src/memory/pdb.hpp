#pragma once
#include <cstdint>
#include <string>
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
    uint32_t    rva;
    uint32_t    size;   // to the next symbol; publics carry no size of their own
    bool        isFunc;
    std::string name;   // undecorated where the PDB held a mangled C++ name
};

// A module section as (rva, size), to turn a symbol's section:offset into an RVA.
struct PdbSection { uint32_t rva; uint32_t size; };

struct PdbSymbols {
    std::vector<PdbSymbol> byRva;  // sorted by rva, then name
    // lower-cased name -> rva. Mangled names are indexed alongside their
    // undecorated form, so either spelling resolves.
    std::unordered_map<std::string, uint32_t> byName;
    // Display case, sorted case-insensitively, for the autocomplete's prefix scan.
    std::vector<std::string> names;

    bool empty() const { return byRva.empty(); }
};

struct PdbLoadRequest {
    std::string             path;      // the .pdb to read
    std::vector<PdbSection> sections;  // module sections, in PE header order
    uint8_t                 guid[16] = {};
    uint32_t                age      = 0;
    bool                    verify   = true; // reject a .pdb from another build
};

// Parse `req.path` into `out`. False with `error` set when the file is missing,
// malformed, stripped of symbols, or belongs to a different build.
bool load_pdb(const PdbLoadRequest& req, PdbSymbols& out, std::string& error);

} // namespace mem
