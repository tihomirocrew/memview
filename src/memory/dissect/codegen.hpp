#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "memory/dissect/def.hpp"

// Spits out the whole project as C++, the way ReClass.NET does it: no options,
// offset comments, a size comment per class. pack(1) is what keeps the member
// offsets matching what we read at runtime. Pointers are written `class Foo*`,
// which declares Foo on the spot, so order and cycles sort themselves out.
namespace mem {

namespace codegen_detail {

inline bool is_keyword(std::string_view s)
{
    static const std::unordered_set<std::string_view> kw = {
        "alignas","alignof","and","asm","auto","bool","break","case","catch",
        "char","class","const","constexpr","continue","decltype","default",
        "delete","do","double","else","enum","explicit","export","extern",
        "false","float","for","friend","goto","if","inline","int","long",
        "mutable","namespace","new","operator","private","protected","public",
        "register","return","short","signed","sizeof","static","struct","switch",
        "template","this","throw","true","try","typedef","typename","union",
        "unsigned","using","virtual","void","volatile","wchar_t","while",
    };
    return kw.count(s) != 0;
}

// Beat arbitrary text into a usable identifier: Game::Entity -> Game_Entity.
inline std::string sanitize(const std::string& name, const std::string& fallback)
{
    std::string out;
    for (char c : name)
        out += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
    std::string collapsed;
    for (char c : out)
        if (!(c == '_' && !collapsed.empty() && collapsed.back() == '_'))
            collapsed += c;
    if (!collapsed.empty() && collapsed.front() == '_') collapsed.erase(0, 1);
    if (!collapsed.empty() && collapsed.back()  == '_') collapsed.pop_back();
    if (collapsed.empty()) collapsed = fallback;
    if (std::isdigit((unsigned char)collapsed.front())) collapsed.insert(0, "_");
    if (is_keyword(collapsed)) collapsed += '_';
    return collapsed;
}

inline const char* scalar_ctype(FieldType t)
{
    switch (t)
    {
    case FieldType::Hex8:  case FieldType::UInt8:  return "uint8_t";
    case FieldType::Int8:                          return "int8_t";
    case FieldType::Hex16: case FieldType::UInt16: return "uint16_t";
    case FieldType::Int16:                         return "int16_t";
    case FieldType::Hex32: case FieldType::UInt32: return "uint32_t";
    case FieldType::Int32:                         return "int32_t";
    case FieldType::Hex64: case FieldType::UInt64: return "uint64_t";
    case FieldType::Int64:                         return "int64_t";
    case FieldType::NInt:                          return "intptr_t";
    case FieldType::NUInt:                         return "uintptr_t";
    case FieldType::Float:                         return "float";
    case FieldType::Double:                        return "double";
    case FieldType::Bool:                          return "bool";
    default:                                       return nullptr;
    }
}

} // namespace codegen_detail

// Every class in the registry, as C++.
inline std::string generate_cpp(const StructRegistry& regs, bool is64)
{
    using namespace codegen_detail;
    if (regs.empty()) return "// No structures to generate.\n";

    // Names have to be unique - pointer fields refer back to them.
    std::unordered_map<int, std::string> cname;
    std::unordered_set<std::string>      usedC;
    for (int i = 0; i < (int)regs.size(); ++i)
    {
        std::string base = sanitize(regs[i].name, "Class" + std::to_string(i));
        std::string name = base;
        for (int n = 2; usedC.count(name); ++n) name = base + std::to_string(n);
        usedC.insert(name);
        cname[i] = name;
    }

    // A class embedded by value has to come before whoever embeds it. Only those
    // edges matter, and they can't form a cycle (that would be infinite size).
    std::vector<int> order;
    std::vector<int> mark(regs.size(), 0); // 0=unseen 1=active 2=done
    std::function<void(int)> visit = [&](int i)
    {
        if (i < 0 || i >= (int)regs.size() || mark[i]) return;
        mark[i] = 1;
        for (const auto& f : regs[i].fields)
            if (f.type == FieldType::Class && f.childStruct >= 0)
                visit(f.childStruct);
        mark[i] = 2;
        order.push_back(i);
    };
    for (int i = 0; i < (int)regs.size(); ++i) visit(i);

    char b[64];
    std::string out = "#pragma once\n#include <cstdint>\n\n#pragma pack(push, 1)\n";

    for (int i : order)
    {
        const StructDef& d = regs[i];
        out += "\nclass " + cname[i] + "\n{\npublic:\n";

        uint32_t off = 0;
        std::unordered_set<std::string> usedF;
        for (const auto& f : d.fields)
        {
            const uint32_t sz = field_size(f, is64, regs);

            // Unnamed fields get ReClass's N<offset> style name.
            snprintf(b, sizeof(b), "N%08X", off);
            std::string base  = f.name.empty() ? std::string(b)
                                               : sanitize(f.name, std::string(b));
            std::string mname = base;
            for (int n = 2; usedF.count(mname); ++n) mname = base + std::to_string(n);
            usedF.insert(mname);

            const bool haveChild = f.childStruct >= 0 && cname.count(f.childStruct);

            std::string decl;
            if (const char* st = scalar_ctype(f.type))
                decl = std::string(st) + " " + mname;
            else if (const int fc = field_float_count(f.type))
                decl = "float " + mname + "[" + std::to_string(fc) + "]";
            else if (f.type == FieldType::Pointer)
                decl = (haveChild ? "class " + cname[f.childStruct] + "*"
                                  : std::string("void*")) + " " + mname;
            else if (f.type == FieldType::Vtable)
                decl = "void** " + mname;
            else if (f.type == FieldType::TextPtr)
                decl = "char* " + mname;
            else if (f.type == FieldType::TextWPtr)
                decl = "wchar_t* " + mname;
            else if (f.type == FieldType::Class)
            {
                if (!haveChild) { out += "\t// " + mname + ": unresolved class\n"; continue; }
                decl = cname[f.childStruct] + " " + mname; // embedded by value
            }
            else if (f.type == FieldType::Text)
                decl = "char " + mname + "[" + std::to_string(sz ? sz : 1) + "]";
            else if (f.type == FieldType::TextW)
                decl = "wchar_t " + mname + "[" + std::to_string(sz >= 2 ? sz / 2 : 1) + "]";
            else // Bytes
                decl = "char " + mname + "[" + std::to_string(sz ? sz : 1) + "]";

            snprintf(b, sizeof(b), "//0x%04X", off);
            out += "\t" + decl + "; " + b + "\n";
            off += sz;
        }

        snprintf(b, sizeof(b), "}; //Size: 0x%04X\n", off);
        out += b;
    }

    out += "#pragma pack(pop)\n";
    return out;
}

} // namespace mem
