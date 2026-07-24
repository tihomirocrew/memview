#pragma once
#include "memory.hpp"

// Reads a module's PE tables (exports, imports, sections, debug records) from the
// target's memory, all by raw offset - so 64-bit memview can read a WOW64 target.
namespace mem {

struct ExportSym {
    std::string name;
    uintptr_t   addr;    // absolute: mod.base + function RVA
    uint16_t    ordinal;
};

// A mapped PE section (".text", ".rdata", ...) as an absolute VA range.
struct Section {
    uintptr_t base;    // mod.base + VirtualAddress
    size_t    size;    // VirtualSize, rounded up to a page
    char      name[9]; // 8-char section name + NUL
};

// Identifies the .pdb a module was built with (from its CodeView record). The
// (name, guid, age) triple is unique per build - the server path and the match check.
struct PdbRef {
    std::string name;     // "ntdll.pdb" (basename of origPath)
    std::string origPath; // path recorded at build time, often not on this machine
    uint8_t     guid[16] = {};
    uint32_t    age      = 0;
};

// `target` is the resolved function; `iatSlot` is the IAT cell the loader filled in.
struct ImportSym {
    std::string name;    // empty when imported by ordinal
    std::string fromDll;
    uint16_t    ordinal;
    uintptr_t   iatSlot;
    uintptr_t   target;
};

namespace detail {

// Read a NUL-terminated ASCII string from the target, capped at `cap` bytes.
inline std::string read_cstr(const Process& proc, uintptr_t addr, size_t cap = 512)
{
    char buf[512];
    if (cap > sizeof(buf)) cap = sizeof(buf);
    size_t got = read_tolerant(proc, addr, reinterpret_cast<uint8_t*>(buf), cap);
    if (got == 0) return {};
    size_t len = 0;
    while (len < got && buf[len] != '\0') ++len;
    return std::string(buf, len);
}

// Decode one 40-byte IMAGE_SECTION_HEADER into an absolute VA range.
inline Section parse_section_header(const uint8_t* hdr, uintptr_t modBase)
{
    Section s{};
    memcpy(s.name, hdr, 8);
    s.name[8] = '\0';
    uint32_t vsize = 0, vaddr = 0;
    memcpy(&vsize, hdr + 8,  4); // Misc.VirtualSize
    memcpy(&vaddr, hdr + 12, 4); // VirtualAddress
    s.base = modBase + vaddr;
    // Page-align so a region split by protection still lands inside its section.
    s.size = (vsize + 0xFFF) & ~(size_t)0xFFF;
    return s;
}

// A module's PE headers, read once from its first page (that's where they all
// live). Every parser below finds its data through this instead of re-walking.
struct PeHeaders {
    uint8_t  page[0x1000];
    size_t   got     = 0;   // bytes read; the header page is one committed region
    size_t   nt      = 0;   // offset of "PE\0\0" within page
    bool     valid   = false;
    bool     is64    = false;
    uint16_t numSecs = 0;
    uint16_t optSize = 0;

    uint16_t u16(size_t off) const
    { uint16_t v = 0; if (off + 2 <= got) memcpy(&v, page + off, 2); return v; }
    uint32_t u32(size_t off) const
    { uint32_t v = 0; if (off + 4 <= got) memcpy(&v, page + off, 4); return v; }

    // { rva, size } of data-directory entry `i` (0 = export, 1 = import, 6 =
    // debug), or {0, 0} when the optional header doesn't reach that far.
    void data_dir(int i, uint32_t& rva, uint32_t& size) const
    {
        const uint32_t ddOff = is64 ? 112 : 96; // DataDirectory[0] in the optional header
        const size_t   slot  = nt + 24 + ddOff + (size_t)i * 8;
        rva  = u32(slot);
        size = u32(slot + 4);
    }
};

// Read and validate a module's header page; `valid` is false on a non-PE image.
inline PeHeaders read_pe_headers(const Process& proc, uintptr_t modBase)
{
    PeHeaders h;
    h.got = read_tolerant(proc, modBase, h.page, sizeof(h.page));
    if (h.got < 0x40 || h.u16(0) != 0x5A4D) return h;       // "MZ"

    const int32_t lfanew = (int32_t)h.u32(0x3C);
    if (lfanew <= 0 || (size_t)lfanew + 24 > h.got) return h;
    h.nt = (size_t)lfanew;
    if (h.u32(h.nt) != 0x00004550) return h;                // "PE\0\0"

    h.numSecs = h.u16(h.nt + 6);              // IMAGE_FILE_HEADER.NumberOfSections
    h.optSize = h.u16(h.nt + 20);             // .SizeOfOptionalHeader
    h.is64    = (h.u16(h.nt + 24) == 0x20B);  // optional header magic: PE32+ vs PE32
    h.valid   = true;
    return h;
}

// Decode the section table from an already-read header page. Empty if it spills
// past the page (~90+ sections) or the count is bogus; read_sections handles that.
inline void parse_sections(const PeHeaders& h, uintptr_t modBase,
    std::vector<Section>& out)
{
    out.clear();
    if (!h.valid || h.numSecs == 0 || h.numSecs > 96) return; // PE caps sections at 96

    // Section headers follow the optional header: 4 (sig) + 20 (file header).
    const size_t secTable = h.nt + 24 + h.optSize;
    if (secTable + (size_t)h.numSecs * 40 > h.got) return;    // 40 = sizeof(IMAGE_SECTION_HEADER)

    out.reserve(h.numSecs);
    for (uint16_t i = 0; i < h.numSecs; ++i)
        out.push_back(parse_section_header(h.page + secTable + (size_t)i * 40, modBase));
}

// Data-directory entry `dirIndex` (0 = export, 1 = import): RVA, size, bitness.
// False only on a non-PE module; a missing entry returns true with zero rva/size.
inline bool find_data_dir(const Process& proc, uintptr_t modBase, int dirIndex,
    uint32_t& dirRva, uint32_t& dirSize, bool& is64)
{
    const PeHeaders h = read_pe_headers(proc, modBase);
    if (!h.valid) return false;
    is64 = h.is64;
    h.data_dir(dirIndex, dirRva, dirSize);
    return true;
}

} // namespace detail

// Sections of `mod`, in header order (.text, .rdata, ...).
inline std::vector<Section> read_sections(const Process& proc, const ModuleEntry& mod)
{
    const detail::PeHeaders h = detail::read_pe_headers(proc, mod.base);
    std::vector<Section> out;
    detail::parse_sections(h, mod.base, out);
    if (!out.empty() || !h.valid || h.numSecs == 0 || h.numSecs > 96)
        return out;

    // Table spilled past the header page (~90+ sections); read those directly.
    const size_t secTable = h.nt + 24 + h.optSize;
    out.reserve(h.numSecs);
    for (uint16_t i = 0; i < h.numSecs; ++i)
    {
        uint8_t hdr[40];
        if (!read_raw(proc, mod.base + secTable + (uintptr_t)i * 40, hdr, sizeof(hdr)))
            break;
        out.push_back(detail::parse_section_header(hdr, mod.base));
    }
    return out;
}

namespace detail {

// Pull the CodeView PDB reference out of the debug directory (data-dir entry 6).
// One read for the entry array, one for the RSDS record. False if there's none.
inline bool read_pdb_ref_from_dir(const Process& proc, const ModuleEntry& mod,
    uint32_t dirRva, uint32_t dirSize, PdbRef& out)
{
    if (dirRva == 0 || dirSize < 28) return false;

    // Each IMAGE_DEBUG_DIRECTORY entry is 28 bytes; we want Type (@12) and
    // AddressOfRawData (@20).
    uint32_t n = dirSize / 28;
    if (n > 64) n = 64;
    uint8_t dir[28 * 64];
    const size_t got = read_tolerant(proc, mod.base + dirRva, dir, (size_t)n * 28);
    n = (uint32_t)(got / 28);

    for (uint32_t i = 0; i < n; ++i)
    {
        const uint8_t* e = dir + (size_t)i * 28;
        uint32_t type, addrRaw;
        memcpy(&type,    e + 12, 4);
        if (type != 2) continue; // IMAGE_DEBUG_TYPE_CODEVIEW
        memcpy(&addrRaw, e + 20, 4); // AddressOfRawData is an RVA once mapped

        // CV_INFO_PDB70 { DWORD 'RSDS'; GUID guid; DWORD age; char pdbName[]; }
        uint8_t rec[4 + 16 + 4 + 260];
        const size_t rgot = read_tolerant(proc, mod.base + addrRaw, rec, sizeof(rec));
        if (rgot < 24) continue;
        uint32_t sig;
        memcpy(&sig, rec, 4);
        if (sig != 0x53445352) continue; // "RSDS"
        memcpy(out.guid, rec + 4,  16);
        memcpy(&out.age, rec + 20, 4);

        size_t len = 0;
        while (24 + len < rgot && rec[24 + len] != '\0') ++len;
        if (len == 0) continue;
        out.origPath.assign((const char*)rec + 24, len);

        const size_t slash = out.origPath.find_last_of("\\/");
        out.name = slash == std::string::npos ? out.origPath
                                              : out.origPath.substr(slash + 1);

        // Goes into a cache path and a server URL, straight from the target's own
        // headers - strip path/URL metacharacters a hostile module might smuggle in.
        for (char& c : out.name)
        {
            const unsigned char u = (unsigned char)c;
            if (u < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        }
        return !out.name.empty();
    }
    return false;
}

} // namespace detail

// The .pdb `mod` was built with. False when it carries no CodeView record
// (stripped binaries).
inline bool read_pdb_ref(const Process& proc, const ModuleEntry& mod, PdbRef& out)
{
    uint32_t dirRva = 0, dirSize = 0;
    bool is64 = false;
    if (!detail::find_data_dir(proc, mod.base, 6, dirRva, dirSize, is64) || dirRva == 0)
        return false;
    return detail::read_pdb_ref_from_dir(proc, mod, dirRva, dirSize, out);
}

// Section table + CodeView PDB reference in one page read (a bulk "load all"
// would otherwise be ~25 tiny reads/module). False if the headers won't read.
inline bool read_symbol_inputs(const Process& proc, const ModuleEntry& mod,
    std::vector<Section>& sections, PdbRef& outRef, bool& hasRef)
{
    hasRef = false;
    sections.clear();

    const detail::PeHeaders h = detail::read_pe_headers(proc, mod.base);
    if (!h.valid) return false;

    detail::parse_sections(h, mod.base, sections);
    // Only a ~90+ section module spills past the header page; read those directly.
    if (sections.empty() && h.numSecs != 0 && h.numSecs <= 96)
        sections = read_sections(proc, mod);

    // Debug directory = data-directory entry 6. Its slot is in the header page;
    // the records it points at usually aren't, so read_pdb_ref_from_dir fetches them.
    uint32_t dbgRva = 0, dbgSize = 0;
    h.data_dir(6, dbgRva, dbgSize);
    hasRef = detail::read_pdb_ref_from_dir(proc, mod, dbgRva, dbgSize, outRef);
    return true;
}

// Named exports of `mod`, resolved to absolute addresses. Forwarders are skipped.
inline std::vector<ExportSym> read_exports(const Process& proc, const ModuleEntry& mod)
{
    std::vector<ExportSym> out;

    uint32_t dirRva = 0, dirSize = 0;
    bool is64 = false;
    if (!detail::find_data_dir(proc, mod.base, 0, dirRva, dirSize, is64) || dirRva == 0)
        return out;

    // IMAGE_EXPORT_DIRECTORY fields we need, by offset.
    const uintptr_t dir = mod.base + dirRva;
    uint32_t ordinalBase = 0, numFuncs = 0, numNames = 0;
    uint32_t rvaFuncs = 0, rvaNames = 0, rvaOrds = 0;
    if (!read_raw(proc, dir + 16, &ordinalBase, 4) ||
        !read_raw(proc, dir + 20, &numFuncs, 4) ||
        !read_raw(proc, dir + 24, &numNames, 4) ||
        !read_raw(proc, dir + 28, &rvaFuncs, 4) ||
        !read_raw(proc, dir + 32, &rvaNames, 4) ||
        !read_raw(proc, dir + 36, &rvaOrds, 4))
        return out;

    // Clamp so a corrupt header can't drive a giant allocation.
    if (numNames == 0 || numNames > 1'000'000 || numFuncs > 1'000'000) return out;

    std::vector<uint32_t> nameRvas(numNames);
    std::vector<uint16_t> ordIdx(numNames);
    std::vector<uint32_t> funcRvas(numFuncs);
    if (!read_raw(proc, mod.base + rvaNames, nameRvas.data(), numNames * 4) ||
        !read_raw(proc, mod.base + rvaOrds,  ordIdx.data(),   numNames * 2) ||
        !read_raw(proc, mod.base + rvaFuncs, funcRvas.data(), numFuncs * 4))
        return out;

    out.reserve(numNames);
    for (uint32_t i = 0; i < numNames; ++i)
    {
        const uint16_t fi = ordIdx[i];
        if (fi >= numFuncs) continue;
        const uint32_t frva = funcRvas[fi];
        if (frva == 0) continue;
        // A forwarder's "address" lands inside the export directory itself.
        if (frva >= dirRva && frva < dirRva + dirSize) continue;

        std::string name = detail::read_cstr(proc, mod.base + nameRvas[i], 256);
        if (name.empty()) continue;
        out.push_back({std::move(name), mod.base + frva, (uint16_t)(fi + ordinalBase)});
    }
    return out;
}

// Imports of `mod`, with both the IAT slot and the resolved target for each.
inline std::vector<ImportSym> read_imports(const Process& proc, const ModuleEntry& mod)
{
    std::vector<ImportSym> out;

    uint32_t dirRva = 0, dirSize = 0;
    bool is64 = false;
    if (!detail::find_data_dir(proc, mod.base, 1, dirRva, dirSize, is64) || dirRva == 0)
        return out;

    const uint32_t thunkSize = is64 ? 8 : 4;
    const uint64_t ordinalFlag = is64 ? 0x8000000000000000ull : 0x80000000ull;

    // Walk IMAGE_IMPORT_DESCRIPTOR[] until the zero terminator.
    for (uint32_t d = 0; d < 4096; ++d)
    {
        const uintptr_t desc = mod.base + dirRva + (uintptr_t)d * 20;
        uint32_t origThunk = 0, nameRva = 0, firstThunk = 0;
        if (!read_raw(proc, desc + 0,  &origThunk,  4) ||
            !read_raw(proc, desc + 12, &nameRva,    4) ||
            !read_raw(proc, desc + 16, &firstThunk, 4))
            break;
        if (origThunk == 0 && nameRva == 0 && firstThunk == 0) break;

        const std::string dll = detail::read_cstr(proc, mod.base + nameRva, 256);
        // Names live in the INT (OriginalFirstThunk), resolved pointers in the
        // IAT (FirstThunk). If the INT is missing, read names from the IAT.
        const uint32_t intRva = origThunk ? origThunk : firstThunk;

        for (uint32_t i = 0; i < 65536; ++i)
        {
            const uintptr_t intSlot = mod.base + intRva + (uintptr_t)i * thunkSize;
            uint64_t entry = 0;
            if (!read_raw(proc, intSlot, &entry, thunkSize)) break;
            if (entry == 0) break; // end of this DLL's thunks

            ImportSym s = {};
            s.fromDll = dll;
            s.iatSlot = mod.base + firstThunk + (uintptr_t)i * thunkSize;
            read_raw(proc, s.iatSlot, &s.target, is64 ? 8 : 4);

            if (entry & ordinalFlag)
            {
                s.ordinal = (uint16_t)(entry & 0xFFFF);
            }
            else
            {
                // entry is an RVA to IMAGE_IMPORT_BY_NAME { WORD Hint; char Name[]; }.
                s.name = detail::read_cstr(proc, mod.base + (uint32_t)entry + 2, 256);
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

} // namespace mem
