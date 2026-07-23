#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "memory/memory.hpp"
#include "memory/dissect/def.hpp"

// Reads a block of memory and guesses what each slot is, like Cheat Engine's
// auto-guess. Layout only - the names have to come from RTTI or from you.
namespace mem {

namespace detail {

// Answers "does this value point somewhere real?" without a syscall per guess.
struct AddrSpace {
    std::vector<Region> regions; // sorted by base

    explicit AddrSpace(const Process& proc)
    {
        for (auto& r : query_regions(proc))
        {
            const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
            if (r.state == MEM_COMMIT && !(r.protect & bad))
                regions.push_back(r);
        }
        std::sort(regions.begin(), regions.end(),
            [](const Region& a, const Region& b) { return a.base < b.base; });
    }

    bool readable(uintptr_t addr) const
    {
        auto it = std::upper_bound(regions.begin(), regions.end(), addr,
            [](uintptr_t v, const Region& r) { return v < r.base; });
        if (it == regions.begin()) return false;
        --it;
        return addr >= it->base && addr < it->base + it->size;
    }
};

// Floats in the range game values actually live in (positions, health, timers).
// Small integers fall out on their own - as floats they're absurd or denormal.
inline bool looks_like_float(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    if (!std::isfinite(f) || f == 0.0f) return false;
    const float a = std::fabs(f);
    return a >= 1e-3f && a <= 1e9f;
}

} // namespace detail

// Walks `size` bytes at `base` in 4-byte steps, taking an aligned slot for a
// pointer when it addresses committed memory.
inline StructDef guess_struct(const Process& proc, uintptr_t base, uint32_t size,
                              bool is64, const char* name = "AutoStruct")
{
    const uint32_t ptr = is64 ? 8 : 4;
    detail::AddrSpace space(proc);

    std::vector<uint8_t> buf(size);
    const size_t got = read_tolerant(proc, base, buf.data(), size);
    buf.resize(got);

    StructDef def;
    def.name = name;

    uint32_t off = 0;
    while (off + 4 <= buf.size())
    {
        StructField f;
        f.offset = off;

        // Left as a plain Pointer even when the target looks like an object -
        // that's RTTI's job, not the guesser's.
        bool isPtr = false;
        if (off % ptr == 0 && off + ptr <= buf.size())
        {
            uintptr_t v = 0;
            std::memcpy(&v, buf.data() + off, ptr);
            if (v && space.readable(v)) isPtr = true;
        }

        if (isPtr)
        {
            f.type = FieldType::Pointer;
            off += ptr;
        }
        else
        {
            uint32_t bits;
            std::memcpy(&bits, buf.data() + off, 4);
            f.type = detail::looks_like_float(bits) ? FieldType::Float
                                                    : FieldType::Hex32;
            off += 4;
        }
        def.fields.push_back(f);
    }
    return def;
}

} // namespace mem
