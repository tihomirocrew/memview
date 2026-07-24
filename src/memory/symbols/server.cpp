#include "memory/symbols/server.hpp"

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

namespace mem {
namespace {

// UTF-8 <-> UTF-16 at the Win32 boundary; every path in memview is UTF-8.
std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s)
{
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
        nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n,
        nullptr, nullptr);
    return out;
}

// widen(), but opts long absolute paths out of MAX_PATH so a deep cache or PDB
// is still found. File-system paths only - "\\?\" is meaningless in a URL.
std::wstring widen_path(const std::string& s)
{
    std::wstring w = widen(s);
    if (w.size() >= MAX_PATH && w.size() > 2 && w[1] == L':' &&
        w.compare(0, 4, L"\\\\?\\") != 0)
        w.insert(0, L"\\\\?\\");
    return w;
}

// mkdir -p: walk the path creating each component, ignoring the ones that exist.
bool ensure_dir(const std::wstring& path)
{
    for (size_t i = 3; i <= path.size(); ++i) // skip past "C:\"
    {
        if (i < path.size() && path[i] != L'\\' && path[i] != L'/') continue;
        const std::wstring part = path.substr(0, i);
        if (!CreateDirectoryW(part.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

struct FileOut {
    FILE* f = nullptr;
    ~FileOut() { if (f) fclose(f); }
    void close() { if (f) { fclose(f); f = nullptr; } }
};

} // namespace

std::string pdb_key(const PdbRef& ref)
{
    // GUID in memory: Data1 (u32), Data2/Data3 (u16) are little-endian, Data4 a
    // byte array. The server path spells them big-endian, so the first three swap.
    const uint8_t* g = ref.guid;
    char buf[48];
    snprintf(buf, sizeof(buf),
        "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%X",
        g[3], g[2], g[1], g[0],   // Data1
        g[5], g[4],               // Data2
        g[7], g[6],               // Data3
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15],
        ref.age);
    return buf;
}

std::string pdb_cache_path(const std::string& cacheDir, const PdbRef& ref)
{
    return cacheDir + "\\" + ref.name + "\\" + pdb_key(ref) + "\\" + ref.name;
}

std::string default_symbol_cache()
{
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
        return {};
    std::string out = narrow(roaming) + "\\MemView\\symbols";
    CoTaskMemFree(roaming);
    return out;
}

std::string pdb_next_to_module(const std::string& modulePath, const PdbRef& ref)
{
    const size_t slash = modulePath.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    return modulePath.substr(0, slash + 1) + ref.name;
}

std::vector<std::string> pdb_search_candidates(const std::string& modulePath,
    const PdbRef& ref, const std::string& cacheDir,
    const std::vector<std::string>& extraDirs)
{
    std::vector<std::string> out;
    auto add = [&out](std::string p) {
        if (p.empty()) return;
        const DWORD attr = GetFileAttributesW(widen_path(p).c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
            return;
        for (const std::string& have : out)
            if (_stricmp(have.c_str(), p.c_str()) == 0) return;
        out.push_back(std::move(p));
    };

    add(pdb_next_to_module(modulePath, ref));
    // Only useful on the machine that built the module, where it's exact.
    if (ref.origPath.size() > 2 && ref.origPath[1] == ':') add(ref.origPath);
    if (!cacheDir.empty()) add(pdb_cache_path(cacheDir, ref));
    for (const std::string& dir : extraDirs)
    {
        if (dir.empty()) continue;
        add(dir + "\\" + ref.name);
        // Extra directories are often symbol caches of their own.
        add(pdb_cache_path(dir, ref));
    }
    return out;
}

bool download_pdb(const std::string& serverUrl, const std::string& cacheDir,
    const PdbRef& ref, DownloadProgress& prog, std::string& outPath,
    std::string& error)
{
    // Trim a trailing slash - server URLs are often pasted with one.
    std::string base = serverUrl;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
        base.pop_back();

    const std::string url = base + "/" + ref.name + "/" + pdb_key(ref) +
                            "/" + ref.name;
    const std::wstring wurl = widen(url);

    // Split the URL so a user-supplied server (proxy, internal mirror) works.
    URL_COMPONENTS uc = {};
    uc.dwStructSize      = sizeof(uc);
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName     = host; uc.dwHostNameLength    = 255;
    uc.lpszUrlPath      = path; uc.dwUrlPathLength     = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
    {
        error = "bad symbol server URL";
        return false;
    }

    // The server only answers requests that look like they came from symsrv.
    Handle session{WinHttpOpen(L"Microsoft-Symbol-Server/10.0.0.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) { error = "WinHttpOpen failed"; return false; }

    // Cap every blocking phase - the cancel flag is only polled in the read loop,
    // so without these a dead server would wedge clear()'s join (~60s WinHTTP default).
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);

    Handle connect{WinHttpConnect(session, host, uc.nPort, 0)};
    if (!connect) { error = "cannot reach the symbol server"; return false; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS)
        ? WINHTTP_FLAG_SECURE : 0;
    Handle req{WinHttpOpenRequest(connect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!req) { error = "WinHttpOpenRequest failed"; return false; }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr))
    {
        error = "request failed (offline?)";
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    if (!WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX))
    {
        error = "no HTTP status from the symbol server";
        return false;
    }
    if (status != 200)
    {
        error = status == 404 ? "the symbol server has no PDB for this build"
                              : "symbol server returned HTTP " + std::to_string(status);
        return false;
    }

    uint64_t total = 0;
    wchar_t  lenBuf[32] = {};
    DWORD    lenSize    = sizeof(lenBuf);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX))
        total = (uint64_t)_wtoi64(lenBuf);
    prog.total.store(total);

    const std::string  dest  = pdb_cache_path(cacheDir, ref);
    const std::wstring wdest = widen(dest);
    const std::wstring wtmp  = wdest + L".tmp";

    const size_t slash = wdest.find_last_of(L'\\');
    if (slash == std::wstring::npos || !ensure_dir(wdest.substr(0, slash)))
    {
        error = "cannot create the symbol cache directory";
        return false;
    }

    FileOut out;
    if (_wfopen_s(&out.f, wtmp.c_str(), L"wb") != 0 || !out.f)
    {
        error = "cannot write into the symbol cache";
        return false;
    }

    std::vector<uint8_t> buf(64 * 1024);
    for (;;)
    {
        if (prog.cancel.load())
        {
            out.close();
            DeleteFileW(wtmp.c_str());
            error = "cancelled";
            return false;
        }

        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &got))
        {
            out.close();
            DeleteFileW(wtmp.c_str());
            error = "the download was cut short";
            return false;
        }
        if (got == 0) break;

        if (fwrite(buf.data(), 1, got, out.f) != got)
        {
            out.close();
            DeleteFileW(wtmp.c_str());
            error = "cannot write into the symbol cache (disk full?)";
            return false;
        }
        prog.received.fetch_add(got);
    }
    out.close();

    // Rename last, so the cache only ever contains complete files.
    if (!MoveFileExW(wtmp.c_str(), wdest.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(wtmp.c_str());
        error = "cannot move the download into the cache";
        return false;
    }

    outPath = dest;
    return true;
}

} // namespace mem
