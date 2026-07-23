#pragma once
#include <windows.h>
#include <dbghelp.h>   // UnDecorateSymbolName + UNDNAME_* flags only
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "memory/memory.hpp"

// Names the object at an address from the RTTI MSVC emits for polymorphic
// classes. Only the class identity is in there - no field layout - so this
// names structures, the offsets come from the guesser or the user.
//
//   obj -> [vtable] -> [method0, method1, ...]
//              [-1] -> Complete Object Locator -> TypeDescriptor (mangled name)
//                                              -> ClassHierarchyDescriptor -> bases
//
// x64 stores the inner pointers as image-relative offsets and the module base is
// recovered from COL.pSelf; x86 stores absolute addresses and has no pSelf.
namespace mem {

struct RttiInfo {
    std::string              className;       // demangled, e.g. "Game::Entity"
    std::string              rawName;         // ".?AVEntity@Game@@"
    uintptr_t                vtable      = 0;
    uintptr_t                col         = 0; // Complete Object Locator
    uintptr_t                imageBase   = 0;
    int                      methodCount = 0;
    std::vector<std::string> bases;           // most-derived first, self excluded
};

namespace rtti_detail {

// A WOW64 target's pointers are 4 bytes.
inline bool read_ptr(const Process& proc, uintptr_t addr, bool is64, uintptr_t& out)
{
    if (is64) return read(proc, addr, out);
    uint32_t v = 0;
    if (!read(proc, addr, v)) return false;
    out = v;
    return true;
}

// The vtable has to sit in a loaded image for the RTTI to be real.
inline const ModuleEntry* module_of(const std::vector<ModuleEntry>& mods, uintptr_t addr)
{
    for (const auto& m : mods)
        if (addr >= m.base && addr < m.base + m.size) return &m;
    return nullptr;
}

// Read a bounded, NUL-terminated ASCII string from the target.
inline std::string read_cstr(const Process& proc, uintptr_t addr, size_t cap = 512)
{
    std::string s;
    char buf[64];
    while (s.size() < cap)
    {
        const size_t got = read_tolerant(proc, addr + s.size(),
            reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (got == 0) break;
        const size_t n = strnlen(buf, got);
        s.append(buf, n);
        if (n < got) break; // hit the terminator
    }
    return s;
}

// The one bit of dbghelp we need; no SymInitialize, loaded on first use.
using UndecorateFn = DWORD (WINAPI*)(PCSTR, PSTR, DWORD, DWORD);
inline UndecorateFn undname()
{
    static UndecorateFn fn = [] {
        HMODULE h = LoadLibraryA("dbghelp.dll");
        return h ? (UndecorateFn)GetProcAddress(h, "UnDecorateSymbolName") : nullptr;
    }();
    return fn;
}

// ".?AVFoo@Bar@@" -> "Bar::Foo". Fallback for when dbghelp isn't there; handles
// everything except templates.
inline std::string demangle_manual(const std::string& raw)
{
    if (raw.size() < 4 || raw[0] != '.') return raw;
    std::string core = raw.substr(4); // drop ".?AV" / ".?AU" / ...
    if (core.size() >= 2 && core.compare(core.size() - 2, 2, "@@") == 0)
        core.erase(core.size() - 2);  // drop trailing "@@"

    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= core.size(); ++i)
        if (i == core.size() || core[i] == '@')
        {
            parts.emplace_back(core.substr(start, i - start));
            start = i + 1;
        }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) // scopes are reversed
    {
        if (it->empty()) continue;
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out.empty() ? raw : out;
}

// Reads a TypeDescriptor name. Goes through dbghelp by rebuilding the matching
// vftable symbol, which gets templates right too, then trims the tail it adds.
inline std::string demangle_type(const std::string& raw)
{
    if (raw.size() < 4 || raw[0] != '.') return raw;

    if (UndecorateFn fn = undname())
    {
        // ".?AVFoo@Bar@@" -> "??_7Foo@Bar@@6B@" -> "Bar::Foo::`vftable'"
        const std::string sym = "??_7" + raw.substr(4) + "6B@";
        char  buf[1024];
        DWORD n = fn(sym.c_str(), buf, (DWORD)sizeof(buf), UNDNAME_NAME_ONLY);
        if (n)
        {
            std::string s(buf, n);
            const size_t tail = s.rfind("::`vftable'");
            if (tail != std::string::npos) s.erase(tail);
            if (!s.empty()) return s;
        }
    }
    return demangle_manual(raw);
}

// Vtable slots pointing into a loaded image, until one doesn't.
inline int count_methods(const Process& proc, uintptr_t vtable, bool is64,
                         const std::vector<ModuleEntry>& mods)
{
    const size_t stride = is64 ? 8 : 4;
    int count = 0;
    for (int i = 0; i < 2048; ++i)
    {
        uintptr_t fn = 0;
        if (!read_ptr(proc, vtable + (size_t)i * stride, is64, fn)) break;
        if (!fn || !module_of(mods, fn)) break;
        ++count;
    }
    return count;
}

} // namespace rtti_detail

// nullopt whenever anything doesn't check out - usually that just means the
// address isn't a polymorphic object. `is64` is the target's pointer width.
inline std::optional<RttiInfo> resolve_rtti(
    const Process& proc, uintptr_t objAddr, bool is64,
    const std::vector<ModuleEntry>& mods)
{
    using namespace rtti_detail;
    const size_t ptr = is64 ? 8 : 4;

    uintptr_t vtable = 0;
    if (!read_ptr(proc, objAddr, is64, vtable) || !vtable) return std::nullopt;
    if (!module_of(mods, vtable)) return std::nullopt;

    // The COL sits just before the first method.
    uintptr_t col = 0;
    if (!read_ptr(proc, vtable - ptr, is64, col) || !col) return std::nullopt;

    // These fields are at the same byte offsets on both widths, only what they
    // mean differs.
    uint32_t signature = 0, tdRef = 0, chdRef = 0, pSelf = 0;
    if (!read(proc, col + 0x00, signature)) return std::nullopt;
    if (!read(proc, col + 0x0C, tdRef))     return std::nullopt; // pTypeDescriptor
    if (!read(proc, col + 0x10, chdRef))    return std::nullopt; // pClassHierarchyDescriptor

    uintptr_t imageBase = 0;
    if (is64)
    {
        if (signature != 1) return std::nullopt;
        if (!read(proc, col + 0x14, pSelf)) return std::nullopt;
        // pSelf is this record's own RVA, so it gives away the module base.
        imageBase = col - pSelf;
    }
    else if (const ModuleEntry* m = module_of(mods, vtable))
    {
        imageBase = m->base;
    }
    auto deref = [&](uint32_t ref) -> uintptr_t {
        return is64 ? imageBase + ref : (uintptr_t)ref;
    };

    // The name follows two pointers of header.
    const uintptr_t td   = deref(tdRef);
    const std::string raw = read_cstr(proc, td + 2 * ptr);
    if (raw.empty() || raw[0] != '.') return std::nullopt;

    RttiInfo info;
    info.rawName     = raw;
    info.className   = demangle_type(raw);
    info.vtable      = vtable;
    info.col         = col;
    info.imageBase   = imageBase;
    info.methodCount = count_methods(proc, vtable, is64, mods);

    // The base class array starts with the class itself, so skip index 0.
    const uintptr_t chd = deref(chdRef);
    uint32_t numBases = 0, bcaRef = 0;
    if (read(proc, chd + 0x08, numBases) && read(proc, chd + 0x0C, bcaRef) &&
        numBases <= 64)
    {
        const uintptr_t bca = deref(bcaRef);
        for (uint32_t i = 1; i < numBases; ++i)
        {
            uint32_t bcdRef = 0;
            if (!read(proc, bca + (uintptr_t)i * 4, bcdRef)) break;
            const uintptr_t bcd = deref(bcdRef);
            uint32_t baseTdRef = 0;
            if (!read(proc, bcd + 0x00, baseTdRef)) break; // BCD.pTypeDescriptor
            const std::string baseRaw = read_cstr(proc, deref(baseTdRef) + 2 * ptr);
            if (!baseRaw.empty() && baseRaw[0] == '.')
                info.bases.push_back(demangle_type(baseRaw));
        }
    }
    return info;
}

// Works the width and module list out itself.
inline std::optional<RttiInfo> resolve_rtti(const Process& proc, uintptr_t objAddr)
{
    return resolve_rtti(proc, objAddr, !is_wow64(proc), list_modules(proc));
}

} // namespace mem
