#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <span>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace mem {

// ============================================================================
// Process
// ============================================================================

struct ProcessEntry {
    DWORD       pid;
    std::string name;
    std::string path; // full path to the executable, empty if it couldn't be resolved
};

struct Process {
    DWORD  pid    = 0;
    HANDLE handle = nullptr;
    char   name[MAX_PATH] = {};

    bool is_open() const { return handle && handle != INVALID_HANDLE_VALUE; }
};

inline std::vector<ProcessEntry> list_processes()
{
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do {
            ProcessEntry e;
            e.pid = pe.th32ProcessID;
            char buf[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, buf, MAX_PATH, nullptr, nullptr);
            e.name = buf;

            // Full path for the icon lookup; left empty when the process denies
            // PROCESS_QUERY_LIMITED_INFORMATION (system/protected processes).
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid))
            {
                wchar_t pathW[MAX_PATH];
                DWORD   len = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, pathW, &len))
                {
                    char pbuf[MAX_PATH];
                    WideCharToMultiByte(CP_UTF8, 0, pathW, -1, pbuf, MAX_PATH, nullptr, nullptr);
                    e.path = pbuf;
                }
                CloseHandle(h);
            }

            out.push_back(std::move(e));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// SYNCHRONIZE is in the default access so is_alive() can wait on the handle.
inline bool open(Process& proc, DWORD pid,
    DWORD access = PROCESS_VM_READ | PROCESS_VM_WRITE |
                   PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION |
                   SYNCHRONIZE)
{
    proc.handle = OpenProcess(access, FALSE, pid);
    if (!proc.is_open()) return false;
    proc.pid = pid;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do {
                if (pe.th32ProcessID == pid)
                {
                    WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                        proc.name, MAX_PATH, nullptr, nullptr);
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return true;
}

inline bool open_by_name(Process& proc, const char* exe_name,
    DWORD access = PROCESS_VM_READ | PROCESS_VM_WRITE |
                   PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION |
                   SYNCHRONIZE)
{
    for (auto& e : list_processes())
        if (_stricmp(e.name.c_str(), exe_name) == 0)
            return open(proc, e.pid, access);
    return false;
}

// Enable SeDebugPrivilege so an elevated process can OpenProcess targets owned
// by other users/sessions (e.g. SYSTEM services). Returns false if not elevated
// or the privilege can't be granted.
inline bool enable_debug_privilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid;
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid))
    {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount           = 1;
        tp.Privileges[0].Luid       = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr)
            && GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

// False once the target has exited. is_open() can't tell: the handle stays
// valid as long as we hold it. A failed wait counts as alive.
inline bool is_alive(const Process& proc)
{
    if (!proc.is_open()) return false;
    return WaitForSingleObject(proc.handle, 0) != WAIT_OBJECT_0;
}

inline void close(Process& proc)
{
    if (proc.is_open()) { CloseHandle(proc.handle); proc.handle = nullptr; }
    proc.pid = 0;
    proc.name[0] = '\0';
}

// Base of the process's main module (the .exe), or 0 if unavailable. Toolhelp
// always reports the executable first.
inline uintptr_t main_module_base(const Process& proc)
{
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, proc.pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    uintptr_t base = 0;
    MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
        base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
    CloseHandle(snap);
    return base;
}

// A loaded module (exe or dll) in the target process's address space.
struct ModuleEntry {
    uintptr_t   base;
    size_t      size;
    std::string name; // short file name, e.g. "ntdll.dll"
};

// Loaded modules in Toolhelp order (main .exe first). Used to label addresses
// as "module+offset".
inline std::vector<ModuleEntry> list_modules(const Process& proc)
{
    std::vector<ModuleEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, proc.pid);
    if (snap == INVALID_HANDLE_VALUE) return out;

    MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do {
            char buf[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, buf, sizeof(buf), nullptr, nullptr);
            out.push_back({
                reinterpret_cast<uintptr_t>(me.modBaseAddr),
                (size_t)me.modBaseSize,
                buf
            });
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

// True if the target is WOW64 (32-bit on 64-bit Windows), so disassemble as x86.
inline bool is_wow64(const Process& proc)
{
    BOOL wow = FALSE;
    return IsWow64Process(proc.handle, &wow) && wow;
}

// Read as many bytes as possible from `addr`, stopping at the first unreadable
// page. Returns the count of contiguous readable bytes (0 if `addr` is unreadable).
inline size_t read_tolerant(const Process& proc, uintptr_t addr,
    uint8_t* buf, size_t n)
{
    constexpr size_t kPage = 0x1000;
    size_t done = 0;
    while (done < n)
    {
        // Clamp to the next page boundary so one bad page doesn't abort the rest.
        const size_t toBoundary = kPage - ((addr + done) & (kPage - 1));
        const size_t chunk = std::min<size_t>(toBoundary, n - done);
        SIZE_T rd = 0;
        ReadProcessMemory(proc.handle, reinterpret_cast<LPCVOID>(addr + done),
            buf + done, chunk, &rd);
        done += rd;
        if (rd != chunk) break; // hit an unreadable page
    }
    return done;
}

// ============================================================================
// Raw read / write
// ============================================================================

inline bool read_raw(const Process& proc, uintptr_t addr, void* buf, size_t n)
{
    SIZE_T read = 0;
    return ReadProcessMemory(proc.handle,
        reinterpret_cast<LPCVOID>(addr), buf, n, &read) && read == n;
}

inline bool write_raw(const Process& proc, uintptr_t addr, const void* buf, size_t n)
{
    LPVOID target = reinterpret_cast<LPVOID>(addr);

    SIZE_T written = 0;
    if (WriteProcessMemory(proc.handle, target, buf, n, &written) && written == n)
        return true;

    // Retry after temporarily lifting page protection.
    DWORD oldProtect = 0;
    if (!VirtualProtectEx(proc.handle, target, n, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    written = 0;
    bool ok = WriteProcessMemory(proc.handle, target, buf, n, &written) && written == n;

    DWORD tmp = 0;
    VirtualProtectEx(proc.handle, target, n, oldProtect, &tmp);
    return ok;
}

// Typed convenience wrappers

template<typename T>
bool read(const Process& proc, uintptr_t addr, T& out)
{
    return read_raw(proc, addr, &out, sizeof(T));
}

template<typename T>
std::optional<T> read(const Process& proc, uintptr_t addr)
{
    T val{};
    if (!read_raw(proc, addr, &val, sizeof(T))) return std::nullopt;
    return val;
}

template<typename T>
bool write(const Process& proc, uintptr_t addr, const T& value)
{
    return write_raw(proc, addr, &value, sizeof(T));
}

// ============================================================================
// Memory regions
// ============================================================================

struct Region {
    uintptr_t base;
    size_t    size;
    DWORD     protect; // PAGE_READWRITE etc.
    DWORD     type;    // MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE
    DWORD     state;   // MEM_COMMIT / MEM_FREE / MEM_RESERVE
};

inline std::vector<Region> query_regions(const Process& proc, bool committed_only = true)
{
    std::vector<Region> out;
    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi;

    while (VirtualQueryEx(proc.handle, reinterpret_cast<LPCVOID>(addr),
        &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        if (!committed_only || mbi.State == MEM_COMMIT)
        {
            out.push_back({
                reinterpret_cast<uintptr_t>(mbi.BaseAddress),
                mbi.RegionSize,
                mbi.Protect,
                mbi.Type,
                mbi.State
            });
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (addr == 0) break; // wrapped
    }
    return out;
}

// ============================================================================
// Scanning
// ============================================================================

enum class ScanType : int {
    Exact = 0,
    NotEqual,
    GreaterThan,
    LessThan,
    GreaterOrEqual,
    LessOrEqual,
    Between,       // needle packs [lo, hi] (two value_size-wide bounds): lo <= current <= hi
    Changed,
    Unchanged,
    Increased,
    Decreased,
    IncreasedBy,   // current - prev == needle
    DecreasedBy,   // prev - current == needle
    UnknownInitial,
};

// Tri-state page-protection filter (CE's Writable/Executable checkboxes).
// First scan only; later scans revisit addresses already found.
enum class TriState : int {
    DontCare = 0, // scan regardless of this attribute (indeterminate checkbox)
    Only     = 1, // only scan pages that have it       (checked)
    Exclude  = 2, // do not scan pages that have it     (unchecked)
};

enum class ValueType : int {
    Int8   = 0,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Double,
    String,       // variable length; scan width comes from the needle, not value_size
    ArrayOfBytes, // variable length hex signature with optional wildcards (see mask)
};

// Fixed byte width of a value type; 0 for the variable-length String/ArrayOfBytes
// (use the needle length instead).
inline size_t value_size(ValueType vt)
{
    switch (vt)
    {
    case ValueType::Int8:   case ValueType::UInt8:   return 1;
    case ValueType::Int16:  case ValueType::UInt16:  return 2;
    case ValueType::Int32:  case ValueType::UInt32:
    case ValueType::Float:                           return 4;
    case ValueType::Int64:  case ValueType::UInt64:
    case ValueType::Double:                          return 8;
    case ValueType::String:
    case ValueType::ArrayOfBytes:                    return 0;
    }
    return 4;
}

// True for value types that scan byte-by-byte at needle length (String/AOB).
inline bool is_bytewise(ValueType vt)
{
    return vt == ValueType::String || vt == ValueType::ArrayOfBytes;
}

struct ScanResult {
    uintptr_t address;
    uint8_t   snapshot[8]; // value at time of last scan
};

// Case-insensitive byte-string equality (ASCII folding).
inline bool str_eq_ascii_ci(const uint8_t* a, const uint8_t* b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    return true;
}

// Case-insensitive UTF-16LE equality (`n` = byte length). Folds via CharUpperW
// so non-ASCII text (e.g. Cyrillic) matches too; its single-char form packs the
// code unit into the pointer. Surrogate halves fold to themselves.
inline bool str_eq_utf16_ci(const uint8_t* a, const uint8_t* b, size_t n)
{
    auto upper = [](uint16_t c) -> uint16_t {
        return (uint16_t)(uintptr_t)CharUpperW(
            reinterpret_cast<LPWSTR>((uintptr_t)c));
    };
    for (size_t i = 0; i + 1 < n; i += 2)
    {
        uint16_t ca, cb;
        memcpy(&ca, a + i, 2);
        memcpy(&cb, b + i, 2);
        if (upper(ca) != upper(cb)) return false;
    }
    return true;
}

// Compare raw value buffers by scan/value type. `needle` = target, `current` =
// memory now, `prev` = last scan's snapshot (for Changed/Increased/...). `sz` is
// the width (value_size, or needle length for String/AOB). String: `ci` folds
// case, `wide` selects UTF-16. AOB: `mask` selects which bits must match per byte.
inline bool compare(ScanType st, ValueType vt,
    const void* current, const void* prev, const void* needle, size_t sz,
    bool ci = false, bool wide = false, const void* mask = nullptr)
{
    // Byte signature: match each position through its mask. Equality only; prev
    // is unused (snapshots hold at most 8 bytes).
    if (vt == ValueType::ArrayOfBytes)
    {
        const auto* c = static_cast<const uint8_t*>(current);
        const auto* n = static_cast<const uint8_t*>(needle);
        const auto* m = static_cast<const uint8_t*>(mask);
        bool eq = true;
        for (size_t i = 0; i < sz; ++i)
            if (((c[i] ^ n[i]) & m[i]) != 0) { eq = false; break; }
        if (st == ScanType::Exact)    return eq;
        if (st == ScanType::NotEqual) return !eq;
        return false;
    }

    // Text: equality only (ordering/deltas undefined). prev is unused; snapshots
    // hold at most 8 bytes, which need not cover a longer needle.
    if (vt == ValueType::String)
    {
        const auto* c = static_cast<const uint8_t*>(current);
        const auto* n = static_cast<const uint8_t*>(needle);
        const bool eq = !ci ? (memcmp(current, needle, sz) == 0)
                      : wide ? str_eq_utf16_ci(c, n, sz)
                             : str_eq_ascii_ci(c, n, sz);
        if (st == ScanType::Exact)    return eq;
        if (st == ScanType::NotEqual) return !eq;
        return false;
    }

    // Numeric compares go through double.
    auto asF64 = [&](const void* p) -> double {
        switch (vt)
        {
        case ValueType::Int8:   { int8_t   v; memcpy(&v, p, 1); return v; }
        case ValueType::Int16:  { int16_t  v; memcpy(&v, p, 2); return v; }
        case ValueType::Int32:  { int32_t  v; memcpy(&v, p, 4); return v; }
        case ValueType::Int64:  { int64_t  v; memcpy(&v, p, 8); return (double)v; }
        case ValueType::UInt8:  { uint8_t  v; memcpy(&v, p, 1); return v; }
        case ValueType::UInt16: { uint16_t v; memcpy(&v, p, 2); return v; }
        case ValueType::UInt32: { uint32_t v; memcpy(&v, p, 4); return v; }
        case ValueType::UInt64: { uint64_t v; memcpy(&v, p, 8); return (double)v; }
        case ValueType::Float:  { float    v; memcpy(&v, p, 4); return v; }
        case ValueType::Double: { double   v; memcpy(&v, p, 8); return v; }
        case ValueType::String:
        case ValueType::ArrayOfBytes: return 0; // handled above
        }
        return 0;
    };

    switch (st)
    {
    case ScanType::Exact:
        // Floats rarely match bit-for-bit, so compare within a relative tolerance.
        // Integers use an exact byte compare.
        if (vt == ValueType::Float)
        {
            float a, b; memcpy(&a, current, 4); memcpy(&b, needle, 4);
            return std::fabs(a - b) <= 0.001f * std::fmax(1.0f, std::fabs(b));
        }
        if (vt == ValueType::Double)
        {
            double a, b; memcpy(&a, current, 8); memcpy(&b, needle, 8);
            return std::fabs(a - b) <= 0.001 * std::fmax(1.0, std::fabs(b));
        }
        return memcmp(current, needle, sz) == 0;
    case ScanType::NotEqual:     return memcmp(current, needle, sz) != 0;
    case ScanType::GreaterThan:  return asF64(current) >  asF64(needle);
    case ScanType::LessThan:     return asF64(current) <  asF64(needle);
    case ScanType::GreaterOrEqual: return asF64(current) >= asF64(needle);
    case ScanType::LessOrEqual:    return asF64(current) <= asF64(needle);
    case ScanType::Between:
    {
        // needle holds two `sz`-wide bounds back to back. Inclusive, order-independent.
        const double v  = asF64(current);
        const double lo = asF64(needle);
        const double hi = asF64(static_cast<const uint8_t*>(needle) + sz);
        return lo <= hi ? (v >= lo && v <= hi) : (v >= hi && v <= lo);
    }
    case ScanType::Changed:      return memcmp(current, prev, sz) != 0;
    case ScanType::Unchanged:    return memcmp(current, prev, sz) == 0;
    case ScanType::Increased:    return asF64(current) >  asF64(prev);
    case ScanType::Decreased:    return asF64(current) <  asF64(prev);
    // "by N": the delta from the previous scan must equal the typed value.
    // Floats use a relative tolerance; integers compare exactly.
    case ScanType::IncreasedBy:
    case ScanType::DecreasedBy:
    {
        const double d = st == ScanType::IncreasedBy
                       ? asF64(current) - asF64(prev)
                       : asF64(prev)    - asF64(current);
        const double n = asF64(needle);
        if (vt == ValueType::Float || vt == ValueType::Double)
            return std::fabs(d - n) <= 0.001 * std::fmax(1.0, std::fabs(n));
        return d == n;
    }
    case ScanType::UnknownInitial: return true;
    }
    return false;
}

// True if the page protection grants write access.
inline bool is_writable(DWORD protect)
{
    const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (protect & w) != 0;
}

// True if the page protection grants execute access.
inline bool is_executable(DWORD protect)
{
    const DWORD x = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (protect & x) != 0;
}

// Readable, committed regions, constrained by the writable/executable filters.
inline bool is_scannable(const Region& r,
    TriState writable   = TriState::DontCare,
    TriState executable = TriState::DontCare)
{
    if (r.state != MEM_COMMIT) return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD | PAGE_NOCACHE;
    if (r.protect & bad)       return false;
    // PAGE_EXECUTE (execute-only) is excluded: reading it faults.
    const DWORD rw  = PAGE_READWRITE | PAGE_WRITECOPY |
                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY |
                      PAGE_READONLY | PAGE_EXECUTE_READ;
    if ((r.protect & rw) == 0) return false;

    const bool w = is_writable(r.protect);
    if (writable == TriState::Only    && !w) return false;
    if (writable == TriState::Exclude &&  w) return false;

    const bool x = is_executable(r.protect);
    if (executable == TriState::Only    && !x) return false;
    if (executable == TriState::Exclude &&  x) return false;
    return true;
}

inline std::vector<ScanResult> scan_first(
    const Process& proc,
    ScanType       type,
    ValueType      vtype,
    const void*    needle,
    size_t         needle_len,
    TriState       wfilter = TriState::DontCare,
    TriState       xfilter = TriState::DontCare,
    bool           caseSensitive = true,
    bool           utf16         = false,
    const void*    mask          = nullptr,
    const std::atomic<bool>* cancel = nullptr)
{
    // Numeric types scan aligned to their fixed width; strings/AOB scan
    // byte-by-byte at needle length.
    const bool   bw    = is_bytewise(vtype);
    const size_t width = bw ? needle_len : value_size(vtype);
    const size_t step  = bw ? 1          : width;
    const bool   ci    = (vtype == ValueType::String) && !caseSensitive;
    std::vector<ScanResult> results;
    std::vector<uint8_t>    chunk;

    if (width == 0) return results; // empty needle / nothing to match

    auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };

    // Scan an already-read buffer, appending matches to `results`.
    auto scanBuffer = [&](uintptr_t base, const uint8_t* data, size_t len)
    {
        const uint8_t zero[8]{};
        const size_t  limit = len >= width ? len - width + 1 : 0;
        const size_t  snap  = width < sizeof(ScanResult::snapshot)
                            ? width : sizeof(ScanResult::snapshot);
        size_t ticks = 0;
        for (size_t off = 0; off < limit; off += step)
        {
            // Poll for cancellation periodically so a huge region can still abort.
            if ((++ticks & 0xFFFF) == 0 && cancelled()) return;
            const uint8_t* cur = data + off;
            if (compare(type, vtype, cur, zero, needle, width, ci, utf16, mask))
            {
                ScanResult r{};
                r.address = base + off;
                memcpy(r.snapshot, cur, snap);
                results.push_back(r);
            }
        }
    };

    for (auto& region : query_regions(proc))
    {
        if (cancelled()) return results;
        if (!is_scannable(region, wfilter, xfilter)) continue;

        chunk.resize(region.size);
        SIZE_T got = 0;
        if (ReadProcessMemory(proc.handle,
            reinterpret_cast<LPCVOID>(region.base),
            chunk.data(), region.size, &got))
        {
            scanBuffer(region.base, chunk.data(), got);
            continue;
        }

        // Whole-region read failed (a guard/decommitted page inside): fall back
        // to page-by-page so the rest survives.
        constexpr size_t kPage = 0x1000;
        for (size_t p = 0; p < region.size; p += kPage)
        {
            if (cancelled()) return results;
            const size_t pageLen = std::min<size_t>(kPage, region.size - p);
            SIZE_T pgot = 0;
            if (ReadProcessMemory(proc.handle,
                reinterpret_cast<LPCVOID>(region.base + p),
                chunk.data(), pageLen, &pgot) && pgot)
                scanBuffer(region.base + p, chunk.data(), pgot);
        }
    }
    return results;
}

inline std::vector<ScanResult> scan_next(
    const Process&                 proc,
    const std::vector<ScanResult>& prev,
    ScanType                       type,
    ValueType                      vtype,
    const void*                    needle,
    size_t                         needle_len,
    bool                           caseSensitive = true,
    bool                           utf16         = false,
    const void*                    mask          = nullptr,
    const std::atomic<bool>*       cancel        = nullptr)
{
    const bool   bw    = is_bytewise(vtype);
    const size_t width = bw ? needle_len : value_size(vtype);
    const size_t snap  = width < sizeof(ScanResult::snapshot)
                       ? width : sizeof(ScanResult::snapshot);
    const bool   ci    = (vtype == ValueType::String) && !caseSensitive;
    std::vector<ScanResult> results;
    if (width == 0) return results;
    results.reserve(prev.size());

    // `prev` is address-sorted, so consecutive entries mostly fall in one read.
    // Cache a sliding window instead of a ReadProcessMemory per address (millions
    // of syscalls on an unknown-initial follow-up scan).
    constexpr size_t     kWindow = 64 * 1024;
    std::vector<uint8_t> win(kWindow);
    uintptr_t            winBase = 0;
    size_t               winLen  = 0; // valid bytes starting at winBase

    auto keep = [&](const ScanResult& r, const uint8_t* cur)
    {
        if (compare(type, vtype, cur, r.snapshot, needle, width, ci, utf16, mask))
        {
            ScanResult nr = r;
            memcpy(nr.snapshot, cur, snap);
            results.push_back(nr);
        }
    };

    size_t ticks = 0;
    for (auto& r : prev)
    {
        // Poll for cancellation so an abort doesn't wait for the whole prev list.
        if ((++ticks & 0xFFF) == 0 && cancel &&
            cancel->load(std::memory_order_relaxed))
            return results;

        // Fast path: value fully inside the cached window.
        if (winLen && r.address >= winBase && r.address + width <= winBase + winLen)
        {
            keep(r, win.data() + (r.address - winBase));
            continue;
        }

        // Refill the window starting at this address.
        SIZE_T got = 0;
        ReadProcessMemory(proc.handle, reinterpret_cast<LPCVOID>(r.address),
            win.data(), kWindow, &got);
        if (got >= width)
        {
            winBase = r.address;
            winLen  = got;
            keep(r, win.data());
        }
        else
        {
            // Isolated unreadable spot: single-value read, sized to the needle.
            winLen = 0;
            std::vector<uint8_t> one(width);
            if (read_raw(proc, r.address, one.data(), width)) keep(r, one.data());
        }
    }
    return results;
}

// Read the current value of a scan result back as a double (for display)
inline double read_as_f64(const Process& proc, uintptr_t addr, ValueType vt)
{
    uint8_t buf[8]{};
    read_raw(proc, addr, buf, value_size(vt));
    switch (vt)
    {
    case ValueType::Int8:   { int8_t   v; memcpy(&v,buf,1); return v; }
    case ValueType::Int16:  { int16_t  v; memcpy(&v,buf,2); return v; }
    case ValueType::Int32:  { int32_t  v; memcpy(&v,buf,4); return v; }
    case ValueType::Int64:  { int64_t  v; memcpy(&v,buf,8); return (double)v; }
    case ValueType::UInt8:  { uint8_t  v; memcpy(&v,buf,1); return v; }
    case ValueType::UInt16: { uint16_t v; memcpy(&v,buf,2); return v; }
    case ValueType::UInt32: { uint32_t v; memcpy(&v,buf,4); return v; }
    case ValueType::UInt64: { uint64_t v; memcpy(&v,buf,8); return (double)v; }
    case ValueType::Float:  { float    v; memcpy(&v,buf,4); return v; }
    case ValueType::Double: { double   v; memcpy(&v,buf,8); return v; }
    case ValueType::String:
    case ValueType::ArrayOfBytes: return 0;
    }
    return 0;
}

} // namespace mem
