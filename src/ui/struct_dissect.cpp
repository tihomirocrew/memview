#include "ui/struct_dissect.hpp"
#include "app/app.hpp"
#include "memory/memory.hpp"
#include "memory/dissect/codegen.hpp"
#include "memory/dissect/def.hpp"
#include "memory/dissect/guess.hpp"
#include "memory/dissect/rtti.hpp"
#include "memory/value_format.hpp"
#include <windows.h>
#include <commdlg.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName, to anchor the modal on this window
#include <nlohmann/json.hpp>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// The Structure Dissector, laid out like ReClass: classes on the left, the
// selected one on the right, read at a single address. Pointer fields link to
// other classes and expand; RTTI supplies the names.
namespace ui {

namespace {

constexpr int kMaxDepth = 8; // pointer-chain recursion guard

bool typeHasSize(mem::FieldType t)
{
    return t == mem::FieldType::Text || t == mem::FieldType::TextW ||
           t == mem::FieldType::Bytes;
}

int findClassByName(const app::AppState& s, const std::string& name)
{
    for (int i = 0; i < (int)s.structDefs.size(); ++i)
        if (s.structDefs[i].name == name) return i;
    return -1;
}

mem::FieldType fieldTypeFromName(const std::string& n)
{
    for (mem::FieldType t : mem::kFieldTypeOrder)
        if (n == mem::field_type_name(t)) return t;
    return mem::FieldType::Hex32;
}

// --- Save / load the whole registry to a .json project ----------------------
// Types go by name rather than enum value, so reordering the enum later doesn't
// break old files. Offsets aren't stored; they fall out of the layout on load.

bool saveStructs(const mem::StructRegistry& regs, const std::string& path)
{
    nlohmann::json j;
    j["version"] = 1;
    nlohmann::json classes = nlohmann::json::array();
    for (const auto& d : regs)
    {
        nlohmann::json c;
        c["name"]    = d.name;
        c["address"] = d.addressExpr;
        nlohmann::json fields = nlohmann::json::array();
        for (const auto& f : d.fields)
            fields.push_back({
                {"type",  mem::field_type_name(f.type)},
                {"name",  f.name},
                {"size",  f.size},
                {"child", f.childStruct},
            });
        c["fields"] = std::move(fields);
        classes.push_back(std::move(c));
    }
    j["classes"] = std::move(classes);

    std::ofstream out(path);
    if (!out) return false;
    out << j.dump(2) << '\n';
    return true;
}

bool loadStructs(mem::StructRegistry& regs, const std::string& path)
{
    std::ifstream in(path);
    if (!in) return false;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("classes")) return false;

    mem::StructRegistry out;
    try
    {
        for (const auto& c : j["classes"])
        {
            mem::StructDef d;
            d.name        = c.value("name", std::string());
            d.addressExpr = c.value("address", std::string());
            if (c.contains("fields"))
                for (const auto& f : c["fields"])
                {
                    mem::StructField sf;
                    sf.type        = fieldTypeFromName(f.value("type", std::string("Hex32")));
                    sf.name        = f.value("name", std::string());
                    sf.size        = f.value("size", 0u);
                    sf.childStruct = f.value("child", -1);
                    d.fields.push_back(std::move(sf));
                }
            out.push_back(std::move(d));
        }
    }
    catch (const nlohmann::json::exception&)
    {
        return false; // malformed file, leave what the caller already had
    }
    regs = std::move(out);
    return true;
}

// Native save dialog for a .json project. False if the user cancelled.
bool pickSaveFile(std::string& out)
{
    wchar_t file[MAX_PATH] = L"structures.json";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Structure project (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Save structures";
    ofn.lpstrDefExt = L"json";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    char utf8[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr))
        return false;
    out = utf8;
    return true;
}

// Native open dialog for a .json project. False if the user cancelled.
bool pickOpenFile(std::string& out)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Structure project (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Load structures";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    char utf8[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr))
        return false;
    out = utf8;
    return true;
}

// Native save dialog for a generated .h header. False if the user cancelled.
bool pickSaveHeader(std::string& out)
{
    wchar_t file[MAX_PATH] = L"structs.h";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"C/C++ header (*.h;*.hpp)\0*.h;*.hpp\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Save generated header";
    ofn.lpstrDefExt = L"h";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    char utf8[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr))
        return false;
    out = utf8;
    return true;
}

// Anything that adds or removes entries is recorded here and done after the
// draw - growing the registry mid-iteration would dangle the references held.
struct PendingOp {
    enum Kind { None, AddField, DelField, SetChild, GuessInto } kind = None;
    int         structIdx = -1;
    int         fieldIdx  = -1;
    uintptr_t   base      = 0;
    int         childIdx  = -1; // SetChild: >=0 link existing, -1 create, -2 detach
    std::string childName;      // name for a new class; an existing one is reused
};

uintptr_t readPtr(const mem::Process& proc, uintptr_t addr, bool is64)
{
    uintptr_t v = 0;
    if (is64) mem::read(proc, addr, v);
    else { uint32_t w = 0; if (mem::read(proc, addr, w)) v = w; }
    return v;
}

// One field's bytes as text. `ok` says whether the memory was readable.
std::string formatField(app::AppState& s, uintptr_t addr, const mem::StructField& f,
                        bool is64, bool& ok)
{
    using mem::FieldType;
    const uint32_t sz = mem::field_type_size(f.type, is64, f.size);
    uint8_t buf[512] = {};
    const uint32_t rd = (uint32_t)std::min<size_t>(sz, sizeof(buf));
    const size_t got = mem::read_tolerant(s.proc, addr, buf, rd);
    ok = (got >= rd && rd > 0);
    if (!ok) return "??";

    char out[600];
    switch (f.type)
    {
    case FieldType::Hex8:   snprintf(out, sizeof(out), "0x%02X",  buf[0]); break;
    case FieldType::Hex16:  { uint16_t v; memcpy(&v,buf,2); snprintf(out,sizeof(out),"0x%04X",v); break; }
    case FieldType::Hex32:  { uint32_t v; memcpy(&v,buf,4); snprintf(out,sizeof(out),"0x%08X",v); break; }
    case FieldType::Hex64:  { uint64_t v; memcpy(&v,buf,8); snprintf(out,sizeof(out),"0x%016" PRIX64,v); break; }
    case FieldType::Int8:   snprintf(out,sizeof(out),"%d",(int)(int8_t)buf[0]); break;
    case FieldType::Int16:  { int16_t v; memcpy(&v,buf,2); snprintf(out,sizeof(out),"%d",v); break; }
    case FieldType::Int32:  { int32_t v; memcpy(&v,buf,4); snprintf(out,sizeof(out),"%d",v); break; }
    case FieldType::Int64:  { int64_t v; memcpy(&v,buf,8); snprintf(out,sizeof(out),"%" PRId64,v); break; }
    case FieldType::UInt8:  snprintf(out,sizeof(out),"%u",(unsigned)buf[0]); break;
    case FieldType::UInt16: { uint16_t v; memcpy(&v,buf,2); snprintf(out,sizeof(out),"%u",v); break; }
    case FieldType::UInt32: { uint32_t v; memcpy(&v,buf,4); snprintf(out,sizeof(out),"%u",v); break; }
    case FieldType::UInt64: { uint64_t v; memcpy(&v,buf,8); snprintf(out,sizeof(out),"%" PRIu64,v); break; }
    case FieldType::Float:  { float v; memcpy(&v,buf,4); snprintf(out,sizeof(out),"%g",(double)v); break; }
    case FieldType::Double: { double v; memcpy(&v,buf,8); snprintf(out,sizeof(out),"%g",v); break; }
    case FieldType::Bool:   snprintf(out,sizeof(out),"%s",buf[0]?"true":"false"); break;
    case FieldType::Pointer:
    {
        uintptr_t v = 0; memcpy(&v, buf, is64 ? 8 : 4);
        snprintf(out, sizeof(out), "0x%" PRIXPTR, v);
        break;
    }
    case FieldType::Vtable:
    {
        uintptr_t v = 0; memcpy(&v, buf, is64 ? 8 : 4);
        // The field is the vtable pointer, so `addr` is the object itself.
        auto rtti = mem::resolve_rtti(s.proc, addr, is64, s.modules);
        if (rtti) snprintf(out, sizeof(out), "0x%" PRIXPTR " <%s>", v, rtti->className.c_str());
        else      snprintf(out, sizeof(out), "0x%" PRIXPTR, v);
        break;
    }
    case FieldType::Text:
        return app::formatValueStr(buf, rd, mem::ValueType::String, false);
    case FieldType::TextW:
        return app::formatValueStr(buf, rd, mem::ValueType::String, true);
    case FieldType::Bytes:
        return app::formatValueStr(buf, rd, mem::ValueType::ArrayOfBytes, false);

    case FieldType::NInt:
    {
        long long v;
        if (is64) { int64_t t; memcpy(&t, buf, 8); v = t; }
        else      { int32_t t; memcpy(&t, buf, 4); v = t; }
        snprintf(out, sizeof(out), "%lld", v);
        break;
    }
    case FieldType::NUInt:
    {
        unsigned long long v;
        if (is64) { uint64_t t; memcpy(&t, buf, 8); v = t; }
        else      { uint32_t t; memcpy(&t, buf, 4); v = t; }
        snprintf(out, sizeof(out), "%llu", v);
        break;
    }
    case FieldType::Vector2: case FieldType::Vector3: case FieldType::Vector4:
    case FieldType::Quaternion: case FieldType::Matrix3x4: case FieldType::Matrix4x4:
    {
        const int fc = mem::field_float_count(f.type);
        std::string v = "(";
        for (int k = 0; k < fc && (uint32_t)(k * 4 + 4) <= rd; ++k)
        {
            float fv; memcpy(&fv, buf + k * 4, 4);
            char t[32]; snprintf(t, sizeof(t), "%g", (double)fv);
            if (k) v += ", ";
            v += t;
        }
        v += ")";
        return v;
    }
    case FieldType::TextPtr:
    case FieldType::TextWPtr:
    {
        uintptr_t p = 0; memcpy(&p, buf, is64 ? 8 : 4);
        if (!p) return "null";
        uint8_t sbuf[256] = {};
        const size_t g = mem::read_tolerant(s.proc, p, sbuf, sizeof(sbuf));
        if (!g) { snprintf(out, sizeof(out), "0x%" PRIXPTR " -> ?", p); break; }
        return app::formatValueStr(sbuf, g, mem::ValueType::String,
                                   f.type == FieldType::TextWPtr);
    }
    case FieldType::Class:
        return ""; // it's a row you expand, not a value
    }
    return out;
}

// Parse what was typed and write it back. Numbers take an optional 0x prefix.
// False if it didn't parse or the write was refused.
bool writeField(app::AppState& s, uintptr_t addr, const mem::StructField& f, bool is64,
                const char* text)
{
    using mem::FieldType;
    if (!s.proc.is_open() || !addr) return false;
    const uint32_t sz = mem::field_type_size(f.type, is64, f.size);
    uint8_t buf[512] = {};

    auto putInt = [&](unsigned long long v, uint32_t n) {
        memcpy(buf, &v, n < 8 ? n : 8);
        return mem::write_raw(s.proc, addr, buf, n);
    };

    char* end = nullptr;
    switch (f.type)
    {
    case FieldType::Hex8: case FieldType::Hex16: case FieldType::Hex32: case FieldType::Hex64:
    {
        const unsigned long long v = strtoull(text, &end, 16);
        return end != text && putInt(v, sz);
    }
    case FieldType::Pointer: case FieldType::Vtable:
    {
        const unsigned long long v = strtoull(text, &end, 0);
        return end != text && putInt(v, sz);
    }
    case FieldType::Int8: case FieldType::Int16: case FieldType::Int32: case FieldType::Int64:
    {
        const long long v = strtoll(text, &end, 0);
        return end != text && putInt((unsigned long long)v, sz);
    }
    case FieldType::UInt8: case FieldType::UInt16: case FieldType::UInt32: case FieldType::UInt64:
    case FieldType::NUInt:
    {
        const unsigned long long v = strtoull(text, &end, 0);
        return end != text && putInt(v, sz);
    }
    case FieldType::NInt:
    {
        const long long v = strtoll(text, &end, 0);
        return end != text && putInt((unsigned long long)v, sz);
    }
    case FieldType::Bool:
    {
        const bool v = (text[0]=='1') || (text[0]=='t') || (text[0]=='T') ||
                       (text[0]=='y') || (text[0]=='Y');
        buf[0] = v ? 1 : 0;
        return mem::write_raw(s.proc, addr, buf, 1);
    }
    case FieldType::Float:  { float  v = strtof(text, &end); return end != text && mem::write_raw(s.proc, addr, &v, 4); }
    case FieldType::Double: { double v = strtod(text, &end); return end != text && mem::write_raw(s.proc, addr, &v, 8); }
    case FieldType::Text:
    case FieldType::TextW:
    case FieldType::Bytes:
    {
        const mem::ValueType vt = (f.type == FieldType::Bytes) ? mem::ValueType::ArrayOfBytes
                                                               : mem::ValueType::String;
        const bool utf16 = (f.type == FieldType::TextW);
        const size_t n = app::parseValue(text, vt, buf, sizeof(buf), utf16);
        return n && mem::write_raw(s.proc, addr, buf, n);
    }
    default: break; // the rest aren't editable in place
    }
    return false;
}

// Hand the class the address it was linked at, so opening it from the list shows
// the same instance. Never overwrites one that's already set.
void seedClassAddress(app::AppState& s, int classIdx, uintptr_t addr)
{
    if (classIdx < 0 || classIdx >= (int)s.structDefs.size()) return;
    if (!addr || !s.structDefs[classIdx].addressExpr.empty()) return;
    // A field that isn't really a pointer just hands back whatever bytes are
    // there, so make sure the address leads somewhere before keeping it.
    uint8_t probe = 0;
    if (!mem::read_raw(s.proc, addr, &probe, 1)) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIXPTR, addr);
    s.structDefs[classIdx].addressExpr = buf;
}

// The type button and its grouped menu - thirty types in one flat combo was
// unusable. The last two submenus link the field to a class in a single click,
// and once linked the button shows the target ("Weapon*" / "Weapon").
void typeMenuButton(app::AppState& s, int structIdx, int fieldIdx,
                    mem::StructField& f, bool is64, uintptr_t fieldAddr,
                    PendingOp& op)
{
    using enum mem::FieldType;

    const bool linked = mem::field_can_have_child(f.type) &&
        f.childStruct >= 0 && f.childStruct < (int)s.structDefs.size();
    std::string label;
    if (linked && f.type == Pointer) label = s.structDefs[f.childStruct].name + "*";
    else if (linked && f.type == Class) label = s.structDefs[f.childStruct].name;
    else label = mem::field_type_name(f.type);

    if (ImGui::Button(label.c_str(), ImVec2(-1, 0)))
        ImGui::OpenPopup("##typemenu");
    if (!ImGui::BeginPopup("##typemenu")) return;

    auto item = [&](const char* lbl, mem::FieldType v) {
        if (ImGui::MenuItem(lbl, nullptr, f.type == v)) f.type = v;
    };
    if (ImGui::BeginMenu("Hex"))
    { item("Hex8",Hex8); item("Hex16",Hex16); item("Hex32",Hex32); item("Hex64",Hex64); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Integer"))
    {
        item("Int8",Int8); item("Int16",Int16); item("Int32",Int32); item("Int64",Int64);
        ImGui::Separator();
        item("UInt8",UInt8); item("UInt16",UInt16); item("UInt32",UInt32); item("UInt64",UInt64);
        ImGui::Separator();
        item("NInt",NInt); item("NUInt",NUInt); item("Bool",Bool);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Float"))
    { item("Float",Float); item("Double",Double); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Vector"))
    { item("Vector2",Vector2); item("Vector3",Vector3); item("Vector4",Vector4); item("Quaternion",Quaternion); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Matrix"))
    { item("Matrix3x4",Matrix3x4); item("Matrix4x4",Matrix4x4); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Text"))
    { item("Text",Text); item("TextW",TextW); item("Bytes",Bytes); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Pointer"))
    { item("Pointer",Pointer); item("Vtable",Vtable); item("TextPtr",TextPtr); item("TextWPtr",TextWPtr); ImGui::EndMenu(); }

    ImGui::Separator();

    // A pointer hands over what it points at, an inline class the field itself.
    // Only read while the menu is open - per frame, per field would be brutal.
    const uintptr_t ptrTarget = fieldAddr ? readPtr(s.proc, fieldAddr, is64) : 0;

    // skipSelf keeps a class from embedding itself, which would be infinitely
    // large. Pointing at itself is fine - that's just a linked list.
    auto linkMenu = [&](const char* title, mem::FieldType ptype, bool skipSelf) {
        if (!ImGui::BeginMenu(title)) return;
        const uintptr_t seed = (ptype == Pointer) ? ptrTarget : fieldAddr;
        for (int i = 0; i < (int)s.structDefs.size(); ++i)
        {
            if (skipSelf && i == structIdx) continue;
            const bool sel = f.type == ptype && f.childStruct == i;
            if (ImGui::MenuItem(s.structDefs[i].name.c_str(), nullptr, sel))
            {
                f.type = ptype; f.childStruct = i;
                seedClassAddress(s, i, seed);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New class..."))
        {
            f.type = ptype; // the class itself shows up next frame
            op = {PendingOp::SetChild, structIdx, fieldIdx, seed, -1, ""};
        }
        ImGui::EndMenu();
    };
    linkMenu("Pointer to class", Pointer, false);
    linkMenu("Class instance",   Class,   true);

    ImGui::EndPopup();
}

// Same linking, from the right-click menu. `allowRtti` is for pointers only.
void childMenu(app::AppState& s, int structIdx, int fieldIdx,
               uintptr_t fieldTargetAddr, bool is64, bool allowRtti, PendingOp& op)
{
    if (ImGui::BeginMenu("Set child class"))
    {
        // `base` is the pointer's target, for the class to adopt as its address.
        for (int i = 0; i < (int)s.structDefs.size(); ++i)
            if (ImGui::MenuItem(s.structDefs[i].name.c_str()))
                op = {PendingOp::SetChild, structIdx, fieldIdx, fieldTargetAddr, i, ""};
        ImGui::Separator();
        if (ImGui::MenuItem("New class..."))
            op = {PendingOp::SetChild, structIdx, fieldIdx, fieldTargetAddr, -1, ""};
        if (ImGui::MenuItem("Detach"))
            op = {PendingOp::SetChild, structIdx, fieldIdx, 0, -2, ""};
        ImGui::EndMenu();
    }
    // Ask the target what it is, then link a class under that name.
    if (allowRtti && ImGui::MenuItem("Identify (RTTI)"))
    {
        auto r = fieldTargetAddr
               ? mem::resolve_rtti(s.proc, fieldTargetAddr, is64, s.modules)
               : std::nullopt;
        op = {PendingOp::SetChild, structIdx, fieldIdx, fieldTargetAddr, -1,
              r ? r->className : std::string()};
    }
}

// One class, one table. Expanding a field doesn't add rows here - the table is
// closed, the linked class drawn as a table of its own, and a fresh one opened
// for what's left. That's why an opened class looks like any other class.
void renderClassTable(app::AppState& s, int structIdx, uintptr_t base, bool is64,
                      int depth, PendingOp& op)
{
    if (structIdx < 0 || structIdx >= (int)s.structDefs.size()) return;
    if (depth > kMaxDepth) return;
    mem::StructDef& def = s.structDefs[structIdx];

    constexpr ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable;

    int  frag       = 0;    // a new table starts after each expansion
    bool showHeader = true; // only the first one gets column headers
    bool tableOpen  = false;

    auto beginFrag = [&]() {
        ImGui::PushID(frag);
        tableOpen = ImGui::BeginTable("##fields", 4, tflags);
        if (tableOpen)
        {
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch);
            if (showHeader) ImGui::TableHeadersRow();
        }
    };
    auto endFrag = [&]() {
        if (tableOpen) ImGui::EndTable();
        ImGui::PopID();
        tableOpen  = false;
        showHeader = false;
        ++frag;
    };

    beginFrag();
    for (int fi = 0; fi < (int)def.fields.size(); ++fi)
    {
        if (!tableOpen) break;

        mem::StructField& f = def.fields[fi];
        // Needed after the field's ID scope closes, for the nested table.
        bool expandOpen = false;

        ImGui::PushID(fi);
        ImGui::TableNextRow();

        const bool canChild = mem::field_can_have_child(f.type);
        const bool isClass  = mem::field_is_class(f.type);
        const bool hasChild = canChild && f.childStruct >= 0 &&
                              f.childStruct < (int)s.structDefs.size();
        const bool expandable = hasChild && depth < kMaxDepth;
        const uintptr_t fieldAddr = base ? base + f.offset : 0;

        // --- Offset column: expander for linked fields -----------------------
        ImGui::TableNextColumn();
        // NoTreePushOnOpen: the arrow just reports open/closed, nothing indents
        // inside the table, so offsets line up at any depth.
        // FramePadding: without it the label rides the top of the row while the
        // button and input beside it are frame height.
        ImGuiTreeNodeFlags tn = ImGuiTreeNodeFlags_SpanAvailWidth |
                                ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                ImGuiTreeNodeFlags_FramePadding;
        // Centring by hand: a tree node puts its label an arrow's width right of
        // the cursor, so a leaf claws that back and an expandable row centres the
        // arrow and label together.
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%04X", f.offset);
        const float arrowW = ImGui::GetTreeNodeToLabelSpacing();
        const float textW  = ImGui::CalcTextSize(lbl).x;
        float shift = (ImGui::GetContentRegionAvail().x -
                       (expandable ? arrowW + textW : textW)) * 0.5f;
        if (!expandable) shift -= arrowW;
        // Indent(0) would apply the default indent, so only shift when non-zero.
        if (shift != 0.0f) ImGui::Indent(shift);

        if (expandable)
            expandOpen = ImGui::TreeNodeEx("##row", tn, "%s", lbl);
        else
        {
            tn |= ImGuiTreeNodeFlags_Leaf; // no arrow, just the offset
            ImGui::TreeNodeEx("##row", tn, "%s", lbl);
        }
        if (shift != 0.0f) ImGui::Unindent(shift);
        // The absolute address lives in a hover tooltip instead of its own column.
        if (fieldAddr && ImGui::IsItemHovered())
            ImGui::SetTooltip("0x%" PRIXPTR, fieldAddr);
        if (ImGui::BeginPopupContextItem("##fieldctx"))
        {
            const bool isPtr = f.type == mem::FieldType::Pointer;
            // Pointer hands over its target, inline class the field itself. RTTI
            // below is pointers-only, so it always sees the dereferenced one.
            const uintptr_t seed = !fieldAddr ? 0
                : (isPtr ? readPtr(s.proc, fieldAddr, is64) : fieldAddr);
            if (canChild) childMenu(s, structIdx, fi, seed, is64, isPtr, op);
            if (ImGui::MenuItem("Add field below"))
                op = {PendingOp::AddField, structIdx, fi, 0, -1, ""};
            if (ImGui::MenuItem("Delete field"))
                op = {PendingOp::DelField, structIdx, fi, 0, -1, ""};
            ImGui::EndPopup();
        }

        // --- Type column ----------------------------------------------------
        ImGui::TableNextColumn();
        typeMenuButton(s, structIdx, fi, f, is64, fieldAddr, op);

        // --- Name column, plus a width for the types that need one ----------
        ImGui::TableNextColumn();
        {
            char nb[64];
            snprintf(nb, sizeof(nb), "%s", f.name.c_str());
            const bool sized = typeHasSize(f.type);
            ImGui::SetNextItemWidth(sized ? -58.f : -1.f);
            if (ImGui::InputText("##name", nb, sizeof(nb)))
                f.name = nb;
            if (sized)
            {
                ImGui::SameLine();
                int sz = (int)(f.size ? f.size : 8);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt("##sz", &sz, 0, 0))
                {
                    if (sz < 1)    sz = 1;
                    if (sz > 4096) sz = 4096;
                    f.size = (uint32_t)sz;
                }
            }
        }

        // --- Value column, double-click to edit -----------------------------
        ImGui::TableNextColumn();
        if (!fieldAddr) ImGui::TextDisabled("-");
        else if (isClass)
        {
            const char* cn = (f.childStruct >= 0 && f.childStruct < (int)s.structDefs.size())
                ? s.structDefs[f.childStruct].name.c_str() : "{ ... }";
            ImGui::TextDisabled("%s", cn);
        }
        else if (s.dissectEditAddr == fieldAddr)
        {
            // Focus once, when the edit starts.
            static uintptr_t focusedAddr = 0;
            ImGui::SetNextItemWidth(-1);
            if (focusedAddr != fieldAddr) { ImGui::SetKeyboardFocusHere(); focusedAddr = fieldAddr; }
            const bool commit = ImGui::InputText("##val", s.dissectEditBuf,
                sizeof(s.dissectEditBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            if (commit)
            {
                writeField(s, fieldAddr, f, is64, s.dissectEditBuf);
                s.dissectEditAddr = 0;
                focusedAddr = 0;
            }
            else if (ImGui::IsItemDeactivated()) // clicked away / Esc
            {
                s.dissectEditAddr = 0;
                focusedAddr = 0;
            }
        }
        else
        {
            bool ok = false;
            const std::string v = formatField(s, fieldAddr, f, is64, ok);
            if (ok) ImGui::TextUnformatted(v.c_str());
            else    ImGui::TextDisabled("%s", v.c_str());
            // Nothing to edit if it wasn't readable in the first place.
            if (ok && mem::field_is_editable(f.type) &&
                ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            {
                s.dissectEditAddr = fieldAddr;
                snprintf(s.dissectEditBuf, sizeof(s.dissectEditBuf), "%s", v.c_str());
            }
        }

        ImGui::PopID();

        // Close this table, put the linked class below as its own, then carry on.
        if (expandOpen && hasChild)
        {
            // Inline lives at the field, a pointer somewhere else entirely.
            const uintptr_t childBase = isClass ? fieldAddr
                : (fieldAddr ? readPtr(s.proc, fieldAddr, is64) : 0);

            endFrag();
            ImGui::PushID(fi);
            ImGui::Indent(12.0f);
            renderClassTable(s, f.childStruct, childBase, is64, depth + 1, op);
            ImGui::Unindent(12.0f);
            ImGui::PopID();
            beginFrag();
        }
    }
    endFrag();
}

// Left pane: the class list.
void drawClassList(app::AppState& s)
{
    if (ImGui::Button("New"))
    {
        mem::StructDef d;
        d.name = "Class" + std::to_string(s.structDefs.size() + 1);
        s.structDefs.push_back(std::move(d));
        s.structCur = (int)s.structDefs.size() - 1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.structCur < 0);
    if (ImGui::Button("Delete") && s.structCur >= 0)
    {
        s.structDefs.erase(s.structDefs.begin() + s.structCur);
        // Everything above the hole keeps its index, everything below shifts.
        for (auto& d : s.structDefs)
            for (auto& f : d.fields)
            {
                if (f.childStruct == s.structCur)     f.childStruct = -1;
                else if (f.childStruct > s.structCur) --f.childStruct;
            }
        if (s.structCur >= (int)s.structDefs.size())
            s.structCur = (int)s.structDefs.size() - 1;
        s.structRenaming = -1;
    }
    ImGui::EndDisabled();

    // Loading replaces everything, like opening a project.
    ImGui::BeginDisabled(s.structDefs.empty());
    if (ImGui::Button("Save"))
    {
        std::string path;
        if (pickSaveFile(path)) saveStructs(s.structDefs, path);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        std::string path;
        if (pickOpenFile(path) && loadStructs(s.structDefs, path))
        {
            s.structCur        = s.structDefs.empty() ? -1 : 0;
            s.structRenaming   = -1;
            s.dissectAddrClass = -1; // forces the address buffer to reload
            s.dissectEditAddr  = 0;
        }
    }

    ImGui::BeginDisabled(s.structDefs.empty());
    if (ImGui::Button("Gen C++"))
        s.showStructCodegen = true; // the modal generates on its first frame
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginChild("##classlist");
    for (int i = 0; i < (int)s.structDefs.size(); ++i)
    {
        ImGui::PushID(i);
        if (s.structRenaming == i)
        {
            ImGui::SetNextItemWidth(-1);
            // Once, at the start - every frame would fight clicks elsewhere.
            static int focusedFor = -1;
            if (focusedFor != i) { ImGui::SetKeyboardFocusHere(); focusedFor = i; }
            const bool done = ImGui::InputText("##rename", s.structNameEdit,
                sizeof(s.structNameEdit), ImGuiInputTextFlags_EnterReturnsTrue);
            if (done || ImGui::IsItemDeactivated())
            {
                if (s.structNameEdit[0]) s.structDefs[i].name = s.structNameEdit;
                s.structRenaming = -1;
                focusedFor = -1;
            }
        }
        else
        {
            if (ImGui::Selectable(s.structDefs[i].name.c_str(), s.structCur == i))
                s.structCur = i;
            // Double-click to rename inline.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            {
                s.structRenaming = i;
                snprintf(s.structNameEdit, sizeof(s.structNameEdit), "%s",
                    s.structDefs[i].name.c_str());
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Right pane header: address bar, the actions, and whatever RTTI says about the
// object sitting there. Returns the parsed address, 0 if it doesn't resolve.
uintptr_t drawEditorHeader(app::AppState& s, bool is64, PendingOp& op)
{
    mem::StructDef& def = s.structDefs[s.structCur];

    // Pull the newly selected class's address into the input.
    if (s.dissectAddrClass != s.structCur)
    {
        snprintf(s.dissectAddr, sizeof(s.dissectAddr), "%s", def.addressExpr.c_str());
        s.dissectAddrClass = s.structCur;
    }

    ImGui::SetNextItemWidth(180);
    app::addrInput(s, "##classaddr", s.dissectAddr, sizeof(s.dissectAddr), 0,
        nullptr, nullptr, "Address");
    def.addressExpr = s.dissectAddr; // and put edits back on the class

    uintptr_t base = 0;
    if (!(s.dissectAddr[0] && app::parseAddrExpr(s, s.dissectAddr, base)))
        base = 0;

    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##guesssize", &s.dissectGuessSize, 0, 0);
    if (s.dissectGuessSize < 8)    s.dissectGuessSize = 8;
    if (s.dissectGuessSize > 8192) s.dissectGuessSize = 8192;
    ImGui::SameLine();
    ImGui::BeginDisabled(!base);
    if (ImGui::Button("Guess") && base)
        op = {PendingOp::GuessInto, s.structCur, -1, base, -1, ""};
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Auto-detect fields by scanning %d bytes at the address.",
            s.dissectGuessSize);

    ImGui::SameLine();
    ImGui::BeginDisabled(!base);
    if (ImGui::Button("Auto (RTTI)") && base)
    {
        if (auto r = mem::resolve_rtti(s.proc, base, is64, s.modules))
        {
            def.name = r->className;
            if (def.fields.empty() || def.fields.front().type != mem::FieldType::Vtable)
                def.fields.insert(def.fields.begin(),
                    {0, 0, mem::FieldType::Vtable, "__vftable", -1});
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Add field"))
        op = {PendingOp::AddField, s.structCur, -1, 0, -1, ""};

    if (base)
        if (auto r = mem::resolve_rtti(s.proc, base, is64, s.modules))
        {
            std::string line = "RTTI: " + r->className;
            if (!r->bases.empty())
            {
                line += " : ";
                for (size_t i = 0; i < r->bases.size(); ++i)
                    line += (i ? ", " : "") + r->bases[i];
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "  (%d vfuncs)", r->methodCount);
            line += buf;
            ImGui::TextDisabled("%s", line.c_str());
        }

    return base;
}

// Run the recorded edit, now that no table is open.
void applyPending(app::AppState& s, const PendingOp& op, bool is64)
{
    if (op.kind == PendingOp::None) return;
    if (op.structIdx < 0 || op.structIdx >= (int)s.structDefs.size()) return;
    mem::StructDef& def = s.structDefs[op.structIdx];

    switch (op.kind)
    {
    case PendingOp::AddField:
    {
        mem::StructField nf{0, 0, mem::FieldType::Hex32, "", -1};
        if (op.fieldIdx >= 0 && op.fieldIdx < (int)def.fields.size())
            def.fields.insert(def.fields.begin() + op.fieldIdx + 1, nf);
        else
            def.fields.push_back(nf);
        break;
    }
    case PendingOp::DelField:
        if (op.fieldIdx >= 0 && op.fieldIdx < (int)def.fields.size())
            def.fields.erase(def.fields.begin() + op.fieldIdx);
        break;
    case PendingOp::SetChild:
    {
        if (op.fieldIdx < 0 || op.fieldIdx >= (int)def.fields.size()) break;
        if (op.childIdx == -2) { def.fields[op.fieldIdx].childStruct = -1; break; }

        int idx = op.childIdx;
        if (idx < 0)
        {
            // A class of that name may already exist - RTTI hands out repeats.
            idx = op.childName.empty() ? -1 : findClassByName(s, op.childName);
            if (idx < 0)
            {
                mem::StructDef child;
                child.name = !op.childName.empty() ? op.childName
                           : ("Class" + std::to_string(s.structDefs.size() + 1));
                s.structDefs.push_back(std::move(child));
                idx = (int)s.structDefs.size() - 1;
            }
        }
        s.structDefs[op.structIdx].fields[op.fieldIdx].childStruct = idx;
        // `base` is where the field was pointing, so the class can be opened on
        // its own later.
        seedClassAddress(s, idx, op.base);
        break;
    }
    case PendingOp::GuessInto:
    {
        mem::StructDef g = mem::guess_struct(s.proc, op.base,
            (uint32_t)s.dissectGuessSize, is64, def.name.c_str());
        def.fields = std::move(g.fields);
        break;
    }
    case PendingOp::None: break;
    }
}

// The "Generate C++" window: the code, and somewhere to put it.
void drawCodegenModal(app::AppState& s, bool is64)
{
    if (!s.showStructCodegen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (ImGuiWindow* w = ImGui::FindWindowByName("Structure Dissector"))
        vp = w->Viewport;
    if (!app::beginBlockingModal("Generate C++##codegen", &s.showStructCodegen, vp, 580, 0))
        return;

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        s.showStructCodegen = false;

    // Once on open is enough - nothing can change while this is up.
    if (ImGui::IsWindowAppearing())
        s.structCodeText = mem::generate_cpp(s.structDefs, is64);

    // The '+1' is the terminator std::string keeps past its size.
    ImGui::InputTextMultiline("##code", s.structCodeText.data(),
        s.structCodeText.size() + 1, ImVec2(-1, 380), ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();

    if (ImGui::Button("Copy"))
        ImGui::SetClipboardText(s.structCodeText.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Save .h..."))
    {
        std::string path;
        if (pickSaveHeader(path))
        {
            std::ofstream f(path);
            if (f) f << s.structCodeText;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        s.showStructCodegen = false;

    ImGui::End();
}

} // namespace

void drawStructDissect(app::AppState& s)
{
    if (!s.showStructDissect) return;

    // Same treatment as Memory View: its own OS window, never docked or merged
    // into the main one, resized by the native border.
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainVp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820, 540), ImGuiCond_FirstUseEver);
    ImGuiWindowClass alwaysOwnWindow;
    alwaysOwnWindow.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&alwaysOwnWindow);
    const ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("Structure Dissector", &s.showStructDissect, winFlags))
    {
        ImGui::End();
        return;
    }

    if (!s.proc.is_open())
    {
        ImGui::TextUnformatted("No process attached.");
        ImGui::End();
        drawCodegenModal(s, true); // generating needs no process
        return;
    }

    const bool is64 = !mem::is_wow64(s.proc);

    // Both "module+offset" addresses and RTTI naming need these current.
    const double now = ImGui::GetTime();
    if (s.modules.empty() || now >= s.modulesNextRefresh)
    {
        app::refreshModules(s);
        s.modulesNextRefresh = now + 2.0;
    }

    // Before anything reads f.offset.
    for (auto& d : s.structDefs)
        mem::recompute_offsets(d, is64, s.structDefs);

    PendingOp op;

    // --- Left pane: class list ----------------------------------------------
    ImGui::BeginChild("##classpane", ImVec2(180, 0), true);
    drawClassList(s);
    ImGui::EndChild();

    ImGui::SameLine();

    // --- Right pane: editor for the selected class --------------------------
    ImGui::BeginChild("##editorpane", ImVec2(0, 0), true);
    if (s.structCur < 0 || s.structCur >= (int)s.structDefs.size())
    {
        ImGui::TextDisabled("Create a class (New), then set its address and press "
                            "Guess or Auto (RTTI).");
    }
    else
    {
        const uintptr_t base = drawEditorHeader(s, is64, op);
        ImGui::Separator();

        // All the class tables scroll together in here. The ID is per class so
        // each keeps its own scroll position - otherwise switching to a shorter
        // class clamps the offset and you lose your place on the way back.
        ImGui::PushID(s.structCur);
        ImGui::BeginChild("##fieldsscroll");
        renderClassTable(s, s.structCur, base, is64, 0, op);
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();

    applyPending(s, op, is64);

    ImGui::End();

    drawCodegenModal(s, is64);
}

} // namespace ui
