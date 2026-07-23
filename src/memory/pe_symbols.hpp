#pragma once
#include "memory.hpp"

// Reads a module's PE export/import tables from the target's memory. Headers are
// parsed by raw offset, not the IMAGE_NT_HEADERS structs, so 64-bit memview can
// read a 32-bit (WOW64) target's PE32 layout.
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

// Identifies the .pdb a module was built with, from its CodeView debug record.
// The (name, guid, age) triple is unique per build and is both the symbol-server
// path and the check that a .pdb found on disk actually belongs to this binary.
struct PdbRef {
    std::string name;     // "ntdll.pdb" (basename of origPath)
    std::string origPath; // path recorded at build time, often not on this machine
    uint8_t     guid[16] = {};
    uint32_t    age      = 0;
};

// `target` is the resolved function (same address as its owning DLL's export);
// `iatSlot` is the pointer cell the loader filled in inside the importing module.
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

// Find data-directory entry `dirIndex` (0 = export, 1 = import): its RVA, size,
// and the PE bitness. False on a malformed or unreadable header.
inline bool find_data_dir(const Process& proc, uintptr_t modBase, int dirIndex,
    uint32_t& dirRva, uint32_t& dirSize, bool& is64)
{
    uint16_t mz = 0;
    if (!read_raw(proc, modBase, &mz, sizeof(mz)) || mz != 0x5A4D) // "MZ"
        return false;

    int32_t lfanew = 0;
    if (!read_raw(proc, modBase + 0x3C, &lfanew, sizeof(lfanew)) || lfanew <= 0)
        return false;

    const uintptr_t nt = modBase + (uint32_t)lfanew;
    uint32_t sig = 0;
    if (!read_raw(proc, nt, &sig, sizeof(sig)) || sig != 0x00004550) // "PE\0\0"
        return false;

    // Optional header magic decides the data-directory offset (PE32 vs PE32+).
    const uintptr_t opt = nt + 4 + 20; // skip Signature + IMAGE_FILE_HEADER
    uint16_t magic = 0;
    if (!read_raw(proc, opt, &magic, sizeof(magic))) return false;
    is64 = (magic == 0x20B);
    const uint32_t ddOff = is64 ? 112 : 96; // offset of DataDirectory[0] in the optional header

    uint32_t dd[2] = {0, 0}; // { VirtualAddress, Size }
    if (!read_raw(proc, opt + ddOff + (uintptr_t)dirIndex * 8, dd, sizeof(dd)))
        return false;
    dirRva  = dd[0];
    dirSize = dd[1];
    return true;
}

} // namespace detail

// Sections of `mod`, in header order (.text, .rdata, ...). Parsed by raw offset
// like the export/import tables, so it works on a 32-bit (WOW64) target too.
inline std::vector<Section> read_sections(const Process& proc, const ModuleEntry& mod)
{
    std::vector<Section> out;

    uint16_t mz = 0;
    if (!read_raw(proc, mod.base, &mz, sizeof(mz)) || mz != 0x5A4D) // "MZ"
        return out;

    int32_t lfanew = 0;
    if (!read_raw(proc, mod.base + 0x3C, &lfanew, sizeof(lfanew)) || lfanew <= 0)
        return out;

    const uintptr_t nt = mod.base + (uint32_t)lfanew;
    uint32_t sig = 0;
    if (!read_raw(proc, nt, &sig, sizeof(sig)) || sig != 0x00004550) // "PE\0\0"
        return out;

    // IMAGE_FILE_HEADER (at nt+4): NumberOfSections @ +2, SizeOfOptionalHeader @ +16.
    uint16_t numSecs = 0, optSize = 0;
    if (!read_raw(proc, nt + 6,  &numSecs, sizeof(numSecs)) ||
        !read_raw(proc, nt + 20, &optSize, sizeof(optSize)))
        return out;
    if (numSecs == 0 || numSecs > 96) return out; // PE caps sections at 96

    // Section headers follow the optional header: 4 (sig) + 20 (file header).
    const uintptr_t secTable = nt + 24 + optSize;
    out.reserve(numSecs);
    for (uint16_t i = 0; i < numSecs; ++i)
    {
        uint8_t hdr[40]; // sizeof(IMAGE_SECTION_HEADER)
        if (!read_raw(proc, secTable + (uintptr_t)i * 40, hdr, sizeof(hdr))) break;

        Section s{};
        memcpy(s.name, hdr, 8);
        s.name[8] = '\0';
        uint32_t vsize = 0, vaddr = 0;
        memcpy(&vsize, hdr + 8,  4); // Misc.VirtualSize
        memcpy(&vaddr, hdr + 12, 4); // VirtualAddress
        s.base = mod.base + vaddr;
        // Page-align so a region split by protection still lands inside its section.
        s.size = (vsize + 0xFFF) & ~(size_t)0xFFF;
        out.push_back(s);
    }
    return out;
}

namespace detail {

// Parse the CodeView PDB reference out of a module's debug directory, given the
// directory's RVA and size (data-directory entry 6). Reads the whole entry array
// in one shot, then the one RSDS record it points at - two reads, versus a read
// per entry and per field. False when there's no CODEVIEW record.
inline bool read_pdb_ref_from_dir(const Process& proc, const ModuleEntry& mod,
    uint32_t dirRva, uint32_t dirSize, PdbRef& out)
{
    if (dirRva == 0 || dirSize < 28) return false;

    // IMAGE_DEBUG_DIRECTORY is 28 bytes: Characteristics, TimeDateStamp,
    // MajorVersion, MinorVersion, Type, SizeOfData, AddressOfRawData, PointerToRawData.
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

        // This name goes into a cache path and a server URL, and it comes from
        // the target's own headers - strip path/URL metacharacters so a hostile
        // module can't smuggle any in. A real PDB name never has them.
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

// The .pdb `mod` was built with, from its debug directory. False when the module
// carries no CodeView record - most Microsoft binaries do, stripped ones don't.
inline bool read_pdb_ref(const Process& proc, const ModuleEntry& mod, PdbRef& out)
{
    uint32_t dirRva = 0, dirSize = 0;
    bool is64 = false;
    if (!detail::find_data_dir(proc, mod.base, 6, dirRva, dirSize, is64) || dirRva == 0)
        return false;
    return detail::read_pdb_ref_from_dir(proc, mod, dirRva, dirSize, out);
}

// Reads the section table and the CodeView PDB reference off a module in one go,
// so a bulk "load all" doesn't cost ~25 tiny reads per module. The PE headers
// live in the image's first page, so one read covers the sections and the
// debug-directory slot; the debug records and CodeView record are read after.
//
// `sections` is filled when the headers parse; `hasRef`/`outRef` only when a
// CodeView record is present. False means the PE headers were unreadable.
inline bool read_symbol_inputs(const Process& proc, const ModuleEntry& mod,
    std::vector<Section>& sections, PdbRef& outRef, bool& hasRef)
{
    hasRef = false;
    sections.clear();

    uint8_t hdr[0x1000];
    const size_t got = read_tolerant(proc, mod.base, hdr, sizeof(hdr));
    if (got < 0x40) return false;

    auto u16 = [&](size_t off) -> uint16_t { uint16_t v; memcpy(&v, hdr + off, 2); return v; };
    auto u32 = [&](size_t off) -> uint32_t { uint32_t v; memcpy(&v, hdr + off, 4); return v; };

    if (u16(0) != 0x5A4D) return false;                    // "MZ"
    const int32_t lfanew = (int32_t)u32(0x3C);
    if (lfanew <= 0 || (size_t)lfanew + 24 > got) return false;

    const size_t nt = (size_t)lfanew;
    if (u32(nt) != 0x00004550) return false;               // "PE\0\0"

    const uint16_t numSecs = u16(nt + 6);   // IMAGE_FILE_HEADER.NumberOfSections
    const uint16_t optSize = u16(nt + 20);  // .SizeOfOptionalHeader
    const uint16_t magic   = u16(nt + 24);  // optional header magic
    const bool     is64    = (magic == 0x20B);
    const uint32_t ddOff   = is64 ? 112 : 96;

    // Section table: right after the optional header. Parsed from the buffer when
    // it fits (it does for all but pathological section counts); otherwise fall
    // back to the per-section reader.
    const size_t secTable = nt + 24 + optSize;
    if (numSecs && numSecs <= 96 && secTable + (size_t)numSecs * 40 <= got)
    {
        sections.reserve(numSecs);
        for (uint16_t i = 0; i < numSecs; ++i)
        {
            const uint8_t* h = hdr + secTable + (size_t)i * 40;
            Section s{};
            memcpy(s.name, h, 8);
            s.name[8] = '\0';
            uint32_t vsize = 0, vaddr = 0;
            memcpy(&vsize, h + 8,  4); // Misc.VirtualSize
            memcpy(&vaddr, h + 12, 4); // VirtualAddress
            s.base = mod.base + vaddr;
            s.size = (vsize + 0xFFF) & ~(size_t)0xFFF;
            sections.push_back(s);
        }
    }
    else
    {
        sections = read_sections(proc, mod);
    }

    // Debug directory = data-directory entry 6. Its slot sits in the buffer; the
    // records it points at generally don't, so read_pdb_ref_from_dir fetches them.
    const size_t ddSlot = nt + 24 + ddOff + 6 * 8;
    if (ddSlot + 8 <= got)
    {
        const uint32_t dbgRva  = u32(ddSlot);
        const uint32_t dbgSize = u32(ddSlot + 4);
        hasRef = detail::read_pdb_ref_from_dir(proc, mod, dbgRva, dbgSize, outRef);
    }
    return true;
}

// Named exports of `mod`, resolved to absolute addresses. Forwarded exports
// (which point at a "Dll.Func" redirect string rather than code) are skipped.
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

    // Sanity clamp so a corrupt header can't drive a giant allocation.
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
        if (origThunk == 0 && nameRva == 0 && firstThunk == 0) break; // terminator

        const std::string dll = detail::read_cstr(proc, mod.base + nameRva, 256);
        // Names come from the INT (OriginalFirstThunk); the IAT (FirstThunk) holds
        // the resolved pointers. Fall back to the IAT for names if the INT is absent.
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
