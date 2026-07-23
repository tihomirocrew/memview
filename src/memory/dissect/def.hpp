#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Structure dissector data model. A StructDef is a template - a field list with
// no address of its own - laid out sequentially. Fields link to another def by
// index: Pointer dereferences into it, Class embeds it inline.
namespace mem {

// How a field's bytes are read.
enum class FieldType : int {
    Hex8 = 0, Hex16, Hex32, Hex64, // unsigned, shown as hex
    Int8, Int16, Int32, Int64,     // signed decimal
    UInt8, UInt16, UInt32, UInt64, // unsigned decimal
    NInt, NUInt,                   // pointer-sized signed / unsigned
    Float, Double,
    Bool,
    Vector2, Vector3, Vector4,     // float aggregates
    Quaternion,                    // 4 floats
    Matrix3x4, Matrix4x4,          // float matrices
    Pointer,   // pointer-sized; dereferences into a linked class (childStruct)
    Vtable,    // pointer to a vtable; shows the RTTI class name
    TextPtr,   // pointer to a UTF-8 string, shown dereferenced
    TextWPtr,  // pointer to a UTF-16 string, shown dereferenced
    Text,      // inline ASCII/UTF-8 text, `size` bytes
    TextW,     // inline UTF-16LE text, `size` bytes
    Bytes,     // inline raw hex dump, `size` bytes
    Class,     // another StructDef embedded inline by value (childStruct)
};

struct StructField {
    uint32_t    offset = 0;      // derived from the sizes before it
    uint32_t    size   = 0;      // Text/TextW/Bytes only; 0 = use the type's width
    FieldType   type   = FieldType::Hex32;
    std::string name;
    int         childStruct = -1;// linked def, for Pointer/Class
};

struct StructDef {
    std::string              name;
    std::vector<StructField> fields;
    std::string              addressExpr; // where the instance lives: hex or "mod+off"
};

// Every structure that exists; childStruct indexes into it.
using StructRegistry = std::vector<StructDef>;

// Width of a type on its own. Text/TextW/Bytes use `size`; Class has none - it
// depends on the linked def, so go through field_size().
inline uint32_t field_type_size(FieldType t, bool is64, uint32_t explicitSize = 0)
{
    switch (t)
    {
    case FieldType::Hex8:  case FieldType::Int8:  case FieldType::UInt8:
    case FieldType::Bool:                                   return 1;
    case FieldType::Hex16: case FieldType::Int16: case FieldType::UInt16: return 2;
    case FieldType::Hex32: case FieldType::Int32: case FieldType::UInt32:
    case FieldType::Float:                                  return 4;
    case FieldType::Hex64: case FieldType::Int64: case FieldType::UInt64:
    case FieldType::Double:                                 return 8;
    case FieldType::NInt:  case FieldType::NUInt:
    case FieldType::Pointer: case FieldType::Vtable:
    case FieldType::TextPtr: case FieldType::TextWPtr:      return is64 ? 8 : 4;
    case FieldType::Vector2:                                return 8;
    case FieldType::Vector3:                                return 12;
    case FieldType::Vector4: case FieldType::Quaternion:    return 16;
    case FieldType::Matrix3x4:                              return 48;
    case FieldType::Matrix4x4:                              return 64;
    case FieldType::Text: case FieldType::TextW: case FieldType::Bytes:
        return explicitSize ? explicitSize : 8;
    case FieldType::Class:                                  return 0; // see field_size
    }
    return 4;
}

// Shown as an address, and RTTI-nameable.
inline bool field_is_pointer(FieldType t)
{
    return t == FieldType::Pointer || t == FieldType::Vtable;
}

inline bool field_is_class(FieldType t) { return t == FieldType::Class; }

// Can link to another def and expand. Vtable is a pointer but hosts nothing.
inline bool field_can_have_child(FieldType t)
{
    return t == FieldType::Pointer || t == FieldType::Class;
}

// Floats in a vector/matrix type, 0 if it isn't one.
inline int field_float_count(FieldType t)
{
    switch (t)
    {
    case FieldType::Vector2:   return 2;
    case FieldType::Vector3:   return 3;
    case FieldType::Vector4:
    case FieldType::Quaternion:return 4;
    case FieldType::Matrix3x4: return 12;
    case FieldType::Matrix4x4: return 16;
    default:                   return 0;
    }
}

// What the inline editor can parse and write back.
inline bool field_is_editable(FieldType t)
{
    switch (t)
    {
    case FieldType::Class:
    case FieldType::TextPtr: case FieldType::TextWPtr:
        return false;
    default:
        return field_float_count(t) == 0;
    }
}

// Field size; Class resolves through the registry. Depth-capped in case a class
// ends up embedding itself.
inline uint32_t struct_size(const StructRegistry& regs, int idx, bool is64, int depth = 0);

inline uint32_t field_size(const StructField& f, bool is64,
                           const StructRegistry& regs, int depth = 0)
{
    if (f.type == FieldType::Class)
        return struct_size(regs, f.childStruct, is64, depth + 1);
    return field_type_size(f.type, is64, f.size);
}

inline uint32_t struct_size(const StructRegistry& regs, int idx, bool is64, int depth)
{
    if (idx < 0 || idx >= (int)regs.size() || depth > 16) return 0;
    uint32_t total = 0;
    for (const auto& f : regs[idx].fields)
        total += field_size(f, is64, regs, depth);
    return total;
}

// Offsets are the running sum of the sizes before them, so a retype shifts
// everything below and there are never gaps or overlaps.
inline void recompute_offsets(StructDef& def, bool is64, const StructRegistry& regs)
{
    uint32_t off = 0;
    for (auto& f : def.fields)
    {
        f.offset = off;
        off += field_size(f, is64, regs);
    }
}

inline const char* field_type_name(FieldType t)
{
    switch (t)
    {
    case FieldType::Hex8:      return "Hex8";
    case FieldType::Hex16:     return "Hex16";
    case FieldType::Hex32:     return "Hex32";
    case FieldType::Hex64:     return "Hex64";
    case FieldType::Int8:      return "Int8";
    case FieldType::Int16:     return "Int16";
    case FieldType::Int32:     return "Int32";
    case FieldType::Int64:     return "Int64";
    case FieldType::UInt8:     return "UInt8";
    case FieldType::UInt16:    return "UInt16";
    case FieldType::UInt32:    return "UInt32";
    case FieldType::UInt64:    return "UInt64";
    case FieldType::NInt:      return "NInt";
    case FieldType::NUInt:     return "NUInt";
    case FieldType::Float:     return "Float";
    case FieldType::Double:    return "Double";
    case FieldType::Bool:      return "Bool";
    case FieldType::Vector2:   return "Vector2";
    case FieldType::Vector3:   return "Vector3";
    case FieldType::Vector4:   return "Vector4";
    case FieldType::Quaternion:return "Quaternion";
    case FieldType::Matrix3x4: return "Matrix3x4";
    case FieldType::Matrix4x4: return "Matrix4x4";
    case FieldType::Pointer:   return "Pointer";
    case FieldType::Vtable:    return "Vtable";
    case FieldType::TextPtr:   return "TextPtr";
    case FieldType::TextWPtr:  return "TextWPtr";
    case FieldType::Text:      return "Text";
    case FieldType::TextW:     return "TextW";
    case FieldType::Bytes:     return "Bytes";
    case FieldType::Class:     return "Class";
    }
    return "?";
}

// Stable order, used for name lookup when loading a saved project.
inline constexpr FieldType kFieldTypeOrder[] = {
    FieldType::Hex8, FieldType::Hex16, FieldType::Hex32, FieldType::Hex64,
    FieldType::Int8, FieldType::Int16, FieldType::Int32, FieldType::Int64,
    FieldType::UInt8, FieldType::UInt16, FieldType::UInt32, FieldType::UInt64,
    FieldType::NInt, FieldType::NUInt,
    FieldType::Float, FieldType::Double, FieldType::Bool,
    FieldType::Vector2, FieldType::Vector3, FieldType::Vector4, FieldType::Quaternion,
    FieldType::Matrix3x4, FieldType::Matrix4x4,
    FieldType::Pointer, FieldType::Vtable, FieldType::TextPtr, FieldType::TextWPtr,
    FieldType::Text, FieldType::TextW, FieldType::Bytes,
    FieldType::Class,
};
inline constexpr int kFieldTypeCount =
    (int)(sizeof(kFieldTypeOrder) / sizeof(kFieldTypeOrder[0]));

} // namespace mem
