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
