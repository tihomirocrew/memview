#pragma once
#include "memory/memory.hpp"
#include <cstdint>
#include <string>
#include <vector>

// AOB signature creation, ported from IDA-Fusion:
//   https://github.com/senator715/IDA-Fusion
// Grow a byte pattern one instruction at a time from the target, wildcarding
// operand displacements/immediates, and stop once it's unique in the module.
// Leading and trailing wildcards are then trimmed.
namespace mem {

enum class SigStyle : int {
    Ida  = 0,   // "48 89 ? ? 8B"
    Code = 1,   // "\x48\x89\x00\x00\x8B"  (wildcards as \x00, IDA-Fusion default)
};

// Builds up signature bytes, tracks wildcard positions, and renders a style.
// Mirrors IDA-Fusion's c_signature_generator.
struct SigGen {
    std::vector<uint8_t> bytes;
    std::vector<bool>    wild;   // parallel to bytes: true = wildcard

    void add(uint8_t b, bool isWild) { bytes.push_back(b); wild.push_back(isWild); }

    // Drop wildcards off both ends; they don't change the match set.
    void trim();

    // Search mask: 0xFF must match, 0x00 wildcard.
    std::vector<uint8_t> mask() const;

    std::string render(SigStyle style) const;
};

// Signature for the instruction at `targetAddr`. `arch`: 0 = x64, 1 = x86.
// `unique` reports whether it uniquely locates the target in its module
// (false = length cap hit first). Empty string only if nothing could be decoded.
std::string createSignature(const Process& proc,
    const std::vector<ModuleEntry>& modules,
    uintptr_t targetAddr, int arch, SigStyle style, bool& unique);

} // namespace mem
