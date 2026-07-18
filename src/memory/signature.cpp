#include "memory/signature.hpp"
#include <Zydis/Zydis.h>
#include <cstdio>

// Ported from IDA-Fusion: https://github.com/senator715/IDA-Fusion
namespace mem {

namespace {

using OperandArray = ZydisDecodedOperand[ZYDIS_MAX_OPERAND_COUNT];

void initDecoder(ZydisDecoder& dec, int arch)
{
    if (arch == 1)
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_COMPAT_32, ZYDIS_STACK_WIDTH_32);
    else
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

// Offset of the first displacement/immediate (IDA-Fusion's "imm offset").
// From here to the instruction end is operand data, which gets wildcarded.
// 0 = none.
uint8_t immOffset(const ZydisDecodedInstruction& ins)
{
    uint8_t off = 0; // 0 doubles as "no operand data" (opcodes never start at 0)
    auto take = [&](uint8_t candidate, uint8_t size) {
        if (size && (off == 0 || candidate < off)) off = candidate;
    };
    take(ins.raw.disp.offset,   ins.raw.disp.size);
    take(ins.raw.imm[0].offset, ins.raw.imm[0].size);
    take(ins.raw.imm[1].offset, ins.raw.imm[1].size);
    return off;
}

// Range to test uniqueness against: the target's module, else the committed
// region containing it (heap/private code).
void searchRange(const Process& proc, const std::vector<ModuleEntry>& modules,
    uintptr_t addr, uintptr_t& base, uintptr_t& end)
{
    for (const ModuleEntry& m : modules)
        if (addr >= m.base && addr < m.base + m.size)
        {
            base = m.base;
            end  = m.base + m.size;
            return;
        }
    for (const Region& r : query_regions(proc))
        if (addr >= r.base && addr < r.base + r.size)
        {
            base = r.base;
            end  = r.base + r.size;
            return;
        }
    base = addr;
    end  = addr + 0x1000;
}

// True if `pattern`/`mask` matches anywhere in [base, end) other than `ignore`.
// Scans every committed, readable section (like IDA-Fusion), so a collision in
// .rdata/.data grows the signature too. Patterns always start with a concrete
// opcode byte, so no leading wildcard matches everywhere.
bool occursElsewhere(const Process& proc, uintptr_t base, uintptr_t end,
    const uint8_t* pattern, const uint8_t* mask, size_t n, uintptr_t ignore)
{
    if (n == 0) return true; // an empty pattern matches everything

    for (const Region& r : query_regions(proc))
    {
        if (r.state != MEM_COMMIT) continue;
        if (r.protect & (PAGE_NOACCESS | PAGE_GUARD)) continue; // unreadable

        const uintptr_t lo = r.base > base ? r.base : base;
        const uintptr_t hi = (r.base + r.size) < end ? (r.base + r.size) : end;
        if (lo >= hi) continue;

        std::vector<uint8_t> buf((size_t)(hi - lo));
        const size_t got = read_tolerant(proc, lo, buf.data(), buf.size());
        if (got < n) continue;

        for (size_t off = 0; off + n <= got; ++off)
        {
            const uintptr_t at = lo + off;
            if (at == ignore) continue;
            bool eq = true;
            for (size_t i = 0; i < n; ++i)
                if (((buf[off + i] ^ pattern[i]) & mask[i]) != 0) { eq = false; break; }
            if (eq) return true;
        }
    }
    return false;
}

} // namespace

void SigGen::trim()
{
    while (!wild.empty() && wild.back())  { bytes.pop_back();       wild.pop_back(); }
    while (!wild.empty() && wild.front()) { bytes.erase(bytes.begin()); wild.erase(wild.begin()); }
}

std::vector<uint8_t> SigGen::mask() const
{
    std::vector<uint8_t> m(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) m[i] = wild[i] ? 0x00 : 0xFF;
    return m;
}

std::string SigGen::render(SigStyle style) const
{
    std::string out;
    char tmp[8];

    if (style == SigStyle::Ida)
    {
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            if (i) out += ' ';
            if (wild[i]) out += '?';
            else { snprintf(tmp, sizeof(tmp), "%02X", bytes[i]); out += tmp; }
        }
        return out;
    }

    // Code style: escaped bytes, wildcards as \x00 (IDA-Fusion's default, no mask).
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        snprintf(tmp, sizeof(tmp), "\\x%02X", wild[i] ? 0 : bytes[i]);
        out += tmp;
    }
    return out;
}

std::string createSignature(const Process& proc,
    const std::vector<ModuleEntry>& modules,
    uintptr_t targetAddr, int arch, SigStyle style, bool& unique)
{
    unique = false;
    if (!proc.is_open()) return {};

    ZydisDecoder dec;
    initDecoder(dec, arch);

    uintptr_t rangeBase = 0, rangeEnd = 0;
    searchRange(proc, modules, targetAddr, rangeBase, rangeEnd);

    SigGen gen;
    uintptr_t rip = targetAddr;

    // Bound the growth so a pattern in repetitive code can't run away.
    constexpr int kMaxInstr = 256;
    for (int i = 0; i < kMaxInstr; ++i)
    {
        uint8_t buf[16];
        const size_t got = read_tolerant(proc, rip, buf, sizeof(buf));
        if (got == 0) break;

        ZydisDecodedInstruction ins;
        OperandArray ops;

        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, buf, got, &ins, ops)))
            break;

        const uint8_t imm = immOffset(ins);
        for (uint8_t b = 0; b < ins.length; ++b)
            gen.add(buf[b], imm != 0 && b >= imm);
        rip += ins.length;

        const std::vector<uint8_t> m = gen.mask();
        if (!occursElsewhere(proc, rangeBase, rangeEnd,
                gen.bytes.data(), m.data(), gen.bytes.size(), targetAddr))
        {
            unique = true;
            break;
        }
    }

    gen.trim();
    return gen.render(style);
}

} // namespace mem
