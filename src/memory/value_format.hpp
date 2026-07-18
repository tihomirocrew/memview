#pragma once
#include <cstdint>
#include <cstddef>
#include "memory/memory.hpp"

// UI (combo indices, text input) <-> typed mem:: backend.
namespace app {

// Combo box labels.
extern const char* const kScanTypeNames[];
extern const int         kScanTypeCount;
// Scan types available on the first scan (the leading kScanTypeNames entries).
// The trailing ones are relative types, only offered once a scan exists.
extern const int         kScanTypeFirstCount;
extern const char* const kValueTypeNames[];
extern const int         kValueTypeCount;

// Combo index -> backend enum.
mem::ValueType uiValueType(int idx);
mem::ScanType  uiScanType(int idx);

// Whether the scan type compares against a typed value.
bool scanNeedsValue(mem::ScanType st);

// Parse text into `out` (little-endian numbers, raw bytes for String). Returns
// bytes written, 0 on failure. For String, `utf16` encodes to UTF-16LE.
size_t parseValue(const char* str, mem::ValueType vt, uint8_t* out, size_t cap,
                  bool utf16 = false);

// Parse an IDA-style byte pattern into value/mask buffers, e.g.
//   "E8 ? ? ? ? 48 83 3D 0C B0 20 00 00"
// Tokens: two hex nibbles, "?"/"??" wildcard, or a nibble wildcard ("A?"/"?B").
// mask per byte: 0xFF fixed, 0x00 wildcard, 0xF0/0x0F one nibble. Returns bytes
// parsed, 0 on empty/malformed input.
size_t parseAob(const char* str, uint8_t* out, uint8_t* mask, size_t cap);

// Format `len` bytes to text. Scalars read their own fixed width; String
// decodes up to `len` bytes (per `utf16`), ArrayOfBytes hex-dumps `len` bytes.
void formatValue(const uint8_t* buf, size_t len, mem::ValueType vt, char* out,
                 size_t sz, bool utf16 = false);

// 8-byte-snapshot convenience (scan results). For String, `utf16` picks decoding.
void formatValue(const uint8_t snap[8], mem::ValueType vt, char* out, size_t sz,
                 bool utf16 = false);

} // namespace app
