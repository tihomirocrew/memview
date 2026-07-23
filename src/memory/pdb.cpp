#include "memory/pdb.hpp"

#define NOMINMAX // keep windows.h's min/max macros away from std::min
#include <windows.h>
#include <dbghelp.h>   // UnDecorateSymbolName's prototype and UNDNAME_* flags only

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mem {
namespace {

// "Microsoft C/C++ MSF 7.00\r\n\x1ADS" then three padding bytes, 32 in total.
constexpr char   kMsfMagic[] = "Microsoft C/C++ MSF 7.00\r\n\x1A" "DS";
constexpr size_t kMagicLen   = sizeof(kMsfMagic) - 1;

// Reject absurd sizes so a corrupt header can't ask for a giant mapping.
constexpr uint64_t kMaxPdbSize = 2ull * 1024 * 1024 * 1024;

// UTF-8 path -> UTF-16. A long absolute path gets the "\\?\" prefix so it isn't
// capped at MAX_PATH (260 chars).
std::wstring widen_path(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    if (w.size() >= MAX_PATH && w.size() > 2 && w[1] == L':' &&
        w.compare(0, 4, L"\\\\?\\") != 0)
        w.insert(0, L"\\\\?\\");
    return w;
}

// Maps the file read-only, so reading a stream is just memcpy - no seeking.
struct MappedFile {
    HANDLE         file    = INVALID_HANDLE_VALUE;
    HANDLE         mapping = nullptr;
    const uint8_t* data    = nullptr;
    size_t         size    = 0;

    ~MappedFile()
    {
        if (data)                       UnmapViewOfFile(data);
        if (mapping)                    CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    bool open(const std::string& utf8Path)
    {
        const std::wstring wide = widen_path(utf8Path);
        if (wide.empty()) return false;

        // Shared read: symbol files are often open in another debugger already.
        file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER len = {};
        if (!GetFileSizeEx(file, &len) || len.QuadPart < 64 ||
            (uint64_t)len.QuadPart > kMaxPdbSize)
            return false;

        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) return false;

        data = (const uint8_t*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!data) return false;

        size = (size_t)len.QuadPart;
        return true;
    }
};

uint32_t read_u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t read_u16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

// Blocks needed to hold `bytes`.
uint32_t block_count(uint32_t bytes, uint32_t blockSize)
{
    return (bytes + blockSize - 1) / blockSize;
}

// The MSF container: the file is split into blocks, and a "stream" is just a
// list of block indices. Everything below reads streams, not file offsets.
struct Msf {
    const uint8_t* data = nullptr;
    size_t         size = 0;

    uint32_t blockSize = 0;
    uint32_t numBlocks = 0;
    std::vector<uint32_t>              streamSizes;
    std::vector<std::vector<uint32_t>> streamBlocks;

    // Byte range of block `i`, or nullptr if it falls outside the file.
    const uint8_t* block(uint32_t i) const
    {
        const uint64_t off = (uint64_t)i * blockSize;
        if (i >= numBlocks || off + blockSize > size) return nullptr;
        return data + off;
    }

    // Concatenate `blocks` and truncate to `bytes`.
    bool gather(const std::vector<uint32_t>& blocks, uint32_t bytes,
        std::vector<uint8_t>& out) const
    {
        out.clear();
        out.reserve(bytes);
        for (uint32_t b : blocks)
        {
            const uint8_t* p = block(b);
            if (!p) return false;
            const size_t take = std::min<size_t>(blockSize, bytes - out.size());
            out.insert(out.end(), p, p + take);
            if (out.size() >= bytes) break;
        }
        return out.size() == bytes;
    }

    bool parse(std::string& error)
    {
        if (size < 56 || memcmp(data, kMsfMagic, kMagicLen) != 0)
        {
            error = "not a PDB file (bad MSF signature)";
            return false;
        }

        blockSize                    = read_u32(data + 32);
        numBlocks                    = read_u32(data + 40);
        const uint32_t numDirBytes   = read_u32(data + 44);
        const uint32_t blockMapAddr  = read_u32(data + 52);

        // 512..65536 and a power of two; anything else is a corrupt header.
        if (blockSize < 512 || blockSize > 65536 || (blockSize & (blockSize - 1)))
        {
            error = "corrupt PDB (bad block size)";
            return false;
        }
        if ((uint64_t)numBlocks * blockSize > size || numDirBytes < 4)
        {
            error = "truncated PDB";
            return false;
        }

        // The stream directory is itself a stream, whose block list sits in
        // consecutive blocks starting at blockMapAddr.
        const uint32_t dirBlocks = block_count(numDirBytes, blockSize);
        const uint64_t mapOff    = (uint64_t)blockMapAddr * blockSize;
        if (mapOff + (uint64_t)dirBlocks * 4 > size)
        {
            error = "corrupt PDB (bad directory block map)";
            return false;
        }

        std::vector<uint32_t> dirBlockList(dirBlocks);
        for (uint32_t i = 0; i < dirBlocks; ++i)
            dirBlockList[i] = read_u32(data + mapOff + (uint64_t)i * 4);

        std::vector<uint8_t> dir;
        if (!gather(dirBlockList, numDirBytes, dir))
        {
            error = "corrupt PDB (unreadable stream directory)";
            return false;
        }

        // Directory layout: u32 numStreams, u32 sizes[n], then each stream's
        // block indices back to back.
        size_t         off        = 0;
        const uint32_t numStreams = read_u32(dir.data());
        off += 4;
        if ((uint64_t)numStreams * 4 + 4 > dir.size())
        {
            error = "corrupt PDB (bad stream count)";
            return false;
        }

        streamSizes.resize(numStreams);
        for (uint32_t i = 0; i < numStreams; ++i, off += 4)
        {
            const uint32_t sz = read_u32(dir.data() + off);
            // 0xFFFFFFFF marks a stream that was deleted; treat it as empty.
            streamSizes[i] = (sz == 0xFFFFFFFFu) ? 0 : sz;
        }

        streamBlocks.resize(numStreams);
        for (uint32_t i = 0; i < numStreams; ++i)
        {
            const uint32_t n = block_count(streamSizes[i], blockSize);
            if (off + (uint64_t)n * 4 > dir.size())
            {
                error = "corrupt PDB (truncated stream directory)";
                return false;
            }
            streamBlocks[i].resize(n);
            for (uint32_t b = 0; b < n; ++b, off += 4)
                streamBlocks[i][b] = read_u32(dir.data() + off);
        }
        return true;
    }

    bool read_stream(uint32_t idx, std::vector<uint8_t>& out) const
    {
        if (idx >= streamSizes.size()) return false;
        return gather(streamBlocks[idx], streamSizes[idx], out);
    }
};

// dbghelp's UnDecorateSymbolName, loaded on first use. The only bit of dbghelp
// we need - no SymInitialize, no symsrv.dll to ship.
using UndecorateFn = DWORD (WINAPI*)(PCSTR, PSTR, DWORD, DWORD);

std::string undecorate(const std::string& name)
{
    if (name.size() < 2 || name[0] != '?') return name; // only MSVC C++ names

    static UndecorateFn undname = [] {
        HMODULE h = LoadLibraryA("dbghelp.dll");
        return h ? (UndecorateFn)GetProcAddress(h, "UnDecorateSymbolName") : nullptr;
    }();
    if (!undname) return name;

    // Drop the noise we don't need, but keep the qualified name and parameters
    // so overloads stay distinct.
    constexpr DWORD kFlags =
        UNDNAME_NO_MS_KEYWORDS | UNDNAME_NO_ACCESS_SPECIFIERS |
        UNDNAME_NO_MEMBER_TYPE | UNDNAME_NO_FUNCTION_RETURNS |
        UNDNAME_NO_THROW_SIGNATURES | UNDNAME_NO_ALLOCATION_MODEL |
        UNDNAME_NO_ALLOCATION_LANGUAGE;

    char        buf[1024];
    DWORD       n = undname(name.c_str(), buf, (DWORD)sizeof(buf), kFlags);
    if (!n) return name;

    // Trailing qualifiers leave a space behind ("Window::hwnd(void)const ").
    while (n && buf[n - 1] == ' ') --n;
    return std::string(buf, n);
}

std::string lower_copy(std::string_view sv)
{
    std::string s(sv);
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool less_nocase(const std::pmr::string& a, const std::pmr::string& b)
{
    return _stricmp(a.c_str(), b.c_str()) < 0;
}

} // namespace

bool load_pdb(const PdbLoadRequest& req, PdbSymbols& out, std::string& error)
{
    // `out` is already bound to its module's arena; reset the contents without
    // touching the allocator. All symbol strings below are built on this arena.
    out.byRva.clear();
    out.byName.clear();
    out.names.clear();
    std::pmr::memory_resource* mr = out.byRva.get_allocator().resource();

    MappedFile file;
    if (!file.open(req.path))
    {
        error = "cannot open the file";
        return false;
    }

    Msf msf;
    msf.data = file.data;
    msf.size = file.size;
    if (!msf.parse(error)) return false;

    // Stream 1 (PDB info) identifies the build: u32 version, u32 signature,
    // u32 age, then the 16-byte GUID.
    if (req.verify)
    {
        std::vector<uint8_t> info;
        if (!msf.read_stream(1, info) || info.size() < 28)
        {
            error = "corrupt PDB (no info stream)";
            return false;
        }
        // Only the GUID decides. Age gets bumped by incremental linking and can
        // run ahead of the module's, so we don't check it.
        if (memcmp(info.data() + 12, req.guid, 16) != 0)
        {
            error = "PDB is from a different build of this module";
            return false;
        }
    }

    // Stream 3 (DBI) points at the stream that actually holds the records.
    std::vector<uint8_t> dbi;
    if (!msf.read_stream(3, dbi) || dbi.size() < 64)
    {
        error = "PDB has no DBI stream";
        return false;
    }
    if ((int32_t)read_u32(dbi.data()) != -1)
    {
        error = "unsupported (pre-VC7) DBI format";
        return false;
    }

    const uint16_t symStream = read_u16(dbi.data() + 20); // SymRecordStream
    if (symStream == 0xFFFF)
    {
        error = "PDB carries no symbols (stripped)";
        return false;
    }

    std::vector<uint8_t> syms;
    if (!msf.read_stream(symStream, syms) || syms.empty())
    {
        error = "PDB symbol stream is empty";
        return false;
    }

    // Cheap poll of the cancel flag: an atomic load per record would be waste, so
    // only every few thousand. Enough that a mid-parse cancel is seen in well
    // under a frame, even on a PDB with hundreds of thousands of publics.
    auto cancelled = [&req](size_t n) {
        return (n & 0x1FFF) == 0 && req.cancel &&
               req.cancel->load(std::memory_order_relaxed);
    };

    // Records are { u16 length; u16 kind; payload }, where length covers the
    // kind and payload but not itself.
    out.byRva.reserve(syms.size() / 48);
    // Sized up front so the mangled-name inserts below and the second index pass
    // don't trigger a run of rehashes mid-load - that rehash churn is the worker's
    // heaviest heap traffic, and it contends with the render thread's allocations.
    out.byName.reserve(syms.size() / 24);
    size_t recNo = 0;
    for (size_t off = 0; off + 4 <= syms.size(); ++recNo)
    {
        if (cancelled(recNo)) { error = "cancelled"; return false; }

        const uint16_t len  = read_u16(syms.data() + off);
        const uint16_t kind = read_u16(syms.data() + off + 2);
        if (len < 2) break;                       // malformed: no forward progress
        const size_t next = off + 2 + len;
        if (next > syms.size()) break;

        const uint8_t* rec    = syms.data() + off + 4;
        const size_t   recLen = len - 2;
        off = next;

        // S_PUB32  { u32 flags;  u32 offset; u16 segment; char name[] }
        // S_GDATA32/S_LDATA32 { u32 typeIndex; u32 offset; u16 segment; char name[] }
        // Same layout, only the first word differs, so one decode handles both.
        constexpr uint16_t kSPub32   = 0x110E;
        constexpr uint16_t kSLData32 = 0x110C;
        constexpr uint16_t kSGData32 = 0x110D;
        const bool isPub  = kind == kSPub32;
        const bool isData = kind == kSLData32 || kind == kSGData32;
        if ((!isPub && !isData) || recLen <= 10) continue;

        const uint32_t word0 = read_u32(rec);
        const uint32_t symOff = read_u32(rec + 4);
        const uint16_t seg    = read_u16(rec + 8);
        if (seg == 0 || seg > req.sections.size()) continue; // absolute/bogus symbol

        const PdbSection& sec = req.sections[seg - 1];
        if (symOff >= sec.size) continue; // outside its own section: corrupt record

        const char*  raw    = (const char*)rec + 10;
        const size_t rawCap = recLen - 10;
        const size_t rawLen = strnlen(raw, rawCap);
        if (rawLen == 0) continue;

        std::string mangled(raw, rawLen);
        std::string demangled = undecorate(mangled);

        // cvpsfFunction (bit 1) separates code from the data publics.
        const bool isFunc = isPub && (word0 & 0x2) != 0;

        out.byRva.push_back({sec.rva + symOff, 0, isFunc,
            std::pmr::string(demangled.data(), demangled.size(), mr)});
        // Index the mangled spelling too, but only when it differs from the
        // undecorated name (C names and data are already plain).
        if (mangled != demangled)
        {
            const std::string low = lower_copy(mangled);
            out.byName.emplace(std::pmr::string(low.data(), low.size(), mr),
                sec.rva + symOff);
        }
    }

    if (out.byRva.empty())
    {
        error = "PDB holds no public symbols";
        return false;
    }

    // The sorts and index builds below are the other slow half of a big load;
    // bail before each so a cancel never waits out more than one of them.
    if (req.cancel && req.cancel->load(std::memory_order_relaxed))
    {
        error = "cancelled";
        return false;
    }

    std::sort(out.byRva.begin(), out.byRva.end(),
        [](const PdbSymbol& a, const PdbSymbol& b) {
            return a.rva != b.rva ? a.rva < b.rva : a.name < b.name;
        });

    // Publics carry no size, so each symbol runs to the next one, capped at its
    // own section end (otherwise the last .text symbol would cover all of .data).
    for (size_t i = 0; i < out.byRva.size(); ++i)
    {
        PdbSymbol& s = out.byRva[i];

        uint32_t end = 0;
        for (const PdbSection& sec : req.sections)
            if (s.rva >= sec.rva && s.rva < sec.rva + sec.size)
            {
                end = sec.rva + sec.size;
                break;
            }
        if (i + 1 < out.byRva.size() && out.byRva[i + 1].rva > s.rva)
            end = end ? std::min(end, out.byRva[i + 1].rva) : out.byRva[i + 1].rva;

        s.size = end > s.rva ? end - s.rva : 0;
    }

    if (req.cancel && req.cancel->load(std::memory_order_relaxed))
    {
        error = "cancelled";
        return false;
    }

    out.names.reserve(out.byRva.size());
    for (const PdbSymbol& s : out.byRva)
    {
        const std::string low = lower_copy(s.name); // first spelling wins
        out.byName.emplace(std::pmr::string(low.data(), low.size(), mr), s.rva);
        out.names.push_back(s.name); // copied onto the arena (element is pmr)
    }
    std::sort(out.names.begin(), out.names.end(), less_nocase);
    out.names.erase(std::unique(out.names.begin(), out.names.end()), out.names.end());

    return true;
}

} // namespace mem
