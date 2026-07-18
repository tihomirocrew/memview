#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <asmjit/x86.h>
#include <asmtk/asmtk.h>

// Text -> bytes x86/x64 assembler (Intel syntax), via asmtk (parse) + asmjit
// (encode). The inverse of Zydis, used by Memory View's "Assemble" action.
namespace mem {

// Assemble ';'/newline-separated instructions as if placed at `address`, so
// relative branches encode correctly. `arch64` picks 64- vs 32-bit mode.
// Returns true + `out` bytes on success, false + `err` on failure.
inline bool assemble(const char* text, uintptr_t address, bool arch64,
    std::vector<uint8_t>& out, std::string& err)
{
    using namespace asmjit;

    // Encode relative to `address` so rel32 branches to absolute targets resolve.
    Environment env = Environment::host();
    env.set_arch(arch64 ? Arch::kX64 : Arch::kX86);

    CodeHolder code;
    if (code.init(env, address) != Error::kOk)
    {
        err = "Failed to initialize the assembler engine";
        return false;
    }

    x86::Assembler a(&code);
    asmtk::AsmParser parser(&a);

    // asmtk treats ';' as a comment; map it to a newline so the single-line
    // Assemble box can still separate instructions with ';'.
    std::string src(text);
    for (char& c : src)
        if (c == ';') c = '\n';

    const Error e = parser.parse(src.c_str(), src.size());
    if (e != Error::kOk)
    {
        err = DebugUtils::error_as_string(e);
        return false;
    }

    // Single section, no external labels: the buffer already holds final bytes.
    const CodeBuffer& buf = code.section_by_id(0)->buffer();
    out.assign(buf.data(), buf.data() + buf.size());
    return true;
}

} // namespace mem
