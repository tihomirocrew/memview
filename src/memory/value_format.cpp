#include "memory/value_format.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace app {

// First five are first-scan types; the rest are relative types needing a
// previous scan. Relative types stay trailing so kScanTypeFirstCount can slice.
const char* const kScanTypeNames[] = {
    "Exact Value", "Bigger than...", "Smaller than...",
    "Value between...", "Unknown initial value",
    "Increased value", "Decreased value",
    "Increased value by...", "Decreased value by...",
    "Changed value", "Unchanged value"
};
const int kScanTypeCount = (int)(sizeof(kScanTypeNames) / sizeof(kScanTypeNames[0]));
const int kScanTypeFirstCount = 5; // Exact, Bigger, Smaller, Between, Unknown

// UI order: Byte, 2 Bytes, 4 Bytes, 8 Bytes, Float, Double, String, Pattern
const char* const kValueTypeNames[] = {
    "Byte", "2 Bytes", "4 Bytes", "8 Bytes",
    "Float", "Double", "String", "Pattern"
};
const int kValueTypeCount = (int)(sizeof(kValueTypeNames) / sizeof(kValueTypeNames[0]));

mem::ValueType uiValueType(int idx)
{
    switch (idx)
    {
    case 0:  return mem::ValueType::UInt8;
    case 1:  return mem::ValueType::Int16;
    case 2:  return mem::ValueType::Int32;
    case 3:  return mem::ValueType::Int64;
    case 4:  return mem::ValueType::Float;
    case 5:  return mem::ValueType::Double;
    case 6:  return mem::ValueType::String;
    case 7:  return mem::ValueType::ArrayOfBytes;
    default: return mem::ValueType::Int32;
    }
}

mem::ScanType uiScanType(int idx)
{
    switch (idx)
    {
    case 0:  return mem::ScanType::Exact;
    case 1:  return mem::ScanType::GreaterThan;
    case 2:  return mem::ScanType::LessThan;
    case 3:  return mem::ScanType::Between;
    case 4:  return mem::ScanType::UnknownInitial;
    case 5:  return mem::ScanType::Increased;
    case 6:  return mem::ScanType::Decreased;
    case 7:  return mem::ScanType::IncreasedBy;
    case 8:  return mem::ScanType::DecreasedBy;
    case 9:  return mem::ScanType::Changed;
    case 10: return mem::ScanType::Unchanged;
    default: return mem::ScanType::Exact;
    }
}

bool scanNeedsValue(mem::ScanType st)
{
    return !(st == mem::ScanType::UnknownInitial ||
             st == mem::ScanType::Changed        ||
             st == mem::ScanType::Unchanged      ||
             st == mem::ScanType::Increased      ||
             st == mem::ScanType::Decreased);
}

size_t parseValue(const char* str, mem::ValueType vt, uint8_t* out, size_t cap,
                  bool utf16)
{
    if (!out || cap == 0 || !str || !str[0]) return 0;
    memset(out, 0, cap);

    auto put = [&](const void* p, size_t n) -> size_t {
        if (n > cap) return 0;
        memcpy(out, p, n);
        return n;
    };

    switch (vt)
    {
    case mem::ValueType::UInt8:  { uint8_t v = (uint8_t)strtoul(str,nullptr,0); return put(&v,1); }
    case mem::ValueType::Int16:  { int16_t v = (int16_t)strtol (str,nullptr,0); return put(&v,2); }
    case mem::ValueType::Int32:  { int32_t v = (int32_t)strtol (str,nullptr,0); return put(&v,4); }
    case mem::ValueType::Int64:  { int64_t v = (int64_t)strtoll(str,nullptr,0); return put(&v,8); }
    case mem::ValueType::Float:  { float   v = strtof(str,nullptr);             return put(&v,4); }
    case mem::ValueType::Double: { double  v = strtod(str,nullptr);             return put(&v,8); }
    case mem::ValueType::String: {
        // Exact characters, no trailing NUL.
        size_t n = strlen(str);
        if (!utf16)
        {
            if (n > cap) n = cap;
            memcpy(out, str, n);
            return n;
        }
        // UTF-16LE: decode the UTF-8 input (zero-padding only works for ASCII).
        int wn = MultiByteToWideChar(CP_UTF8, 0, str, (int)n, nullptr, 0);
        if (wn <= 0) return 0;
        std::vector<wchar_t> w(wn);
        MultiByteToWideChar(CP_UTF8, 0, str, (int)n, w.data(), wn);
        size_t bytes = (size_t)wn * 2;
        if (bytes > cap) bytes = cap & ~size_t(1); // keep whole code units
        memcpy(out, w.data(), bytes);
        return bytes;
    }
    default: return 0;
    }
}

void formatValue(const uint8_t* buf, size_t len, mem::ValueType vt, char* out,
                 size_t sz, bool utf16)
{
    if (sz == 0) return;
    switch (vt)
    {
    // Scalars ignore `len` and read their own fixed width from `buf`.
    case mem::ValueType::UInt8:  { uint8_t v; memcpy(&v,buf,1); snprintf(out,sz,"%u",  v); break; }
    case mem::ValueType::Int16:  { int16_t v; memcpy(&v,buf,2); snprintf(out,sz,"%d",  v); break; }
    case mem::ValueType::Int32:  { int32_t v; memcpy(&v,buf,4); snprintf(out,sz,"%d",  v); break; }
    case mem::ValueType::Int64:  { int64_t v; memcpy(&v,buf,8); snprintf(out,sz,"%lld",(long long)v); break; }
    case mem::ValueType::Float:  { float   v; memcpy(&v,buf,4); snprintf(out,sz,"%.4g",v); break; }
    case mem::ValueType::Double: { double  v; memcpy(&v,buf,8); snprintf(out,sz,"%.4g",v); break; }
    case mem::ValueType::String: {
        if (utf16)
        {
            // Up to len/2 UTF-16LE units (stop at NUL), re-encoded to UTF-8.
            const size_t units = len / 2;
            std::vector<wchar_t> w(units ? units : 1);
            memcpy(w.data(), buf, units * 2);
            size_t wlen = 0;
            while (wlen < units && w[wlen]) ++wlen;

            // WideCharToMultiByte returns 0 and writes nothing if the whole
            // result doesn't fit in `out`. So convert into a temp buffer, then
            // copy as many whole code points as fit.
            std::string u8;
            if (wlen)
            {
                int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)wlen,
                                               nullptr, 0, nullptr, nullptr);
                if (need > 0)
                {
                    u8.resize((size_t)need);
                    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)wlen,
                                        u8.data(), need, nullptr, nullptr);
                }
            }
            size_t o = 0, i = 0;
            while (i < u8.size())
            {
                const uint8_t c = (uint8_t)u8[i];
                const int clen = (c < 0x80) ? 1 : (c >= 0xF0) ? 4
                               : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
                if (i + (size_t)clen > u8.size() || o + (size_t)clen >= sz) break;
                for (int k = 0; k < clen; ++k) out[o++] = u8[i++];
            }
            out[o] = '\0';
            break;
        }

        // UTF-8: copy whole code points, dropping a multibyte sequence cut off
        // by the end so the preview stays valid. Control bytes render as '.'.
        size_t avail = 0;
        while (avail < len && buf[avail]) ++avail;
        size_t o = 0, i = 0;
        while (i < avail)
        {
            const uint8_t c = buf[i];
            const int clen = (c < 0x80) ? 1 : (c >= 0xF0) ? 4
                           : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            if (i + (size_t)clen > avail || o + (size_t)clen >= sz) break;
            if (clen == 1)
                out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.', ++i;
            else
                for (int k = 0; k < clen; ++k) out[o++] = (char)buf[i++];
        }
        out[o] = '\0';
        break;
    }
    case mem::ValueType::ArrayOfBytes: {
        size_t pos = 0;
        for (size_t n = 0; n < len && pos + 3 < sz; ++n)
            pos += snprintf(out + pos, sz - pos, n ? " %02X" : "%02X", buf[n]);
        out[pos < sz ? pos : sz - 1] = '\0';
        break;
    }
    default: snprintf(out,sz,"?"); break;
    }
}

// 8-byte-snapshot convenience (scan results): an 8-byte preview for all types.
void formatValue(const uint8_t snap[8], mem::ValueType vt, char* out, size_t sz,
                 bool utf16)
{
    formatValue(snap, size_t(8), vt, out, sz, utf16);
}

std::string formatValueStr(const uint8_t* buf, size_t len, mem::ValueType vt,
                           bool utf16)
{
    if (vt == mem::ValueType::String)
    {
        if (utf16)
        {
            const size_t units = len / 2;
            std::vector<wchar_t> w(units ? units : 1);
            memcpy(w.data(), buf, units * 2);
            size_t wlen = 0;
            while (wlen < units && w[wlen]) ++wlen;
            if (!wlen) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)wlen,
                                        nullptr, 0, nullptr, nullptr);
            std::string out(n > 0 ? (size_t)n : 0, '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)wlen,
                                    out.data(), n, nullptr, nullptr);
            return out;
        }

        // UTF-8: whole code points up to the first NUL; controls become '.'.
        size_t avail = 0;
        while (avail < len && buf[avail]) ++avail;
        std::string out;
        for (size_t i = 0; i < avail; )
        {
            const uint8_t c = buf[i];
            const int clen = (c < 0x80) ? 1 : (c >= 0xF0) ? 4
                           : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            if (i + (size_t)clen > avail) break;
            if (clen == 1) { out += (c >= 0x20 && c < 0x7f) ? (char)c : '.'; ++i; }
            else for (int k = 0; k < clen; ++k) out += (char)buf[i++];
        }
        return out;
    }

    if (vt == mem::ValueType::ArrayOfBytes)
    {
        std::string out;
        char b[4];
        for (size_t n = 0; n < len; ++n)
        {
            snprintf(b, sizeof(b), "%02X", buf[n]);
            if (n) out += ' ';
            out += b;
        }
        return out;
    }

    // Scalars are short; reuse the fixed-buffer path.
    char tmp[32];
    formatValue(buf, len, vt, tmp, sizeof(tmp), utf16);
    return tmp;
}

size_t parseAob(const char* str, uint8_t* out, uint8_t* mask, size_t cap)
{
    if (!str || !out || !mask || cap == 0) return 0;

    // Decode one nibble character: hex digit -> value; '?' -> wildcard.
    auto nibble = [](char c, int& v, bool& wild) -> bool {
        if (c == '?')             { v = 0; wild = true;  return true; }
        if (c >= '0' && c <= '9') { v = c - '0';      wild = false; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; wild = false; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; wild = false; return true; }
        return false;
    };

    size_t      count = 0;
    const char* p     = str;
    while (*p)
    {
        while (*p == ' ' || *p == '\t') ++p; // skip token separators
        if (!*p) break;

        int hv = 0; bool hw = false;
        if (!nibble(*p, hv, hw)) return 0; // malformed
        ++p;

        uint8_t val, msk;
        const char c2 = *p;
        if (c2 && c2 != ' ' && c2 != '\t')
        {
            // Two-character token: high + low nibble.
            int lv = 0; bool lw = false;
            if (!nibble(c2, lv, lw)) return 0;
            ++p;
            val = (uint8_t)((hv << 4) | lv);
            msk = (uint8_t)((hw ? 0x00 : 0xF0) | (lw ? 0x00 : 0x0F));
        }
        else if (hw)
        {
            val = 0x00; msk = 0x00; // lone '?' -> full-byte wildcard
        }
        else
        {
            val = (uint8_t)hv; msk = 0xFF; // lone hex digit -> exact low byte
        }

        if (count >= cap) break; // truncate to the buffer capacity
        out[count]  = val;
        mask[count] = msk;
        ++count;
    }
    return count;
}

} // namespace app
