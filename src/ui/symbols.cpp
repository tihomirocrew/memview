#include "ui/memory_view.hpp"
#include "app/app.hpp"
#include "memory/memory.hpp"

#include <windows.h>
#include <commdlg.h>

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ui {

namespace {

// Native open dialog for a .pdb. False if the user cancelled.
bool pickPdbFile(std::string& out)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFilter  = L"Program database (*.pdb)\0*.pdb\0All files (*.*)\0*.*\0";
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = L"Select a PDB";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;

    char utf8[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr))
        return false;
    out = utf8;
    return true;
}

} // namespace

void symbolStatusCell(const mem::ModuleSymbols* ms)
{
    if (!ms || ms->status == mem::SymStatus::None)
    {
        ImGui::TextDisabled("-");
        return;
    }
    switch (ms->status)
    {
    case mem::SymStatus::Queued:  ImGui::TextDisabled("queued");  break;
    case mem::SymStatus::Loading: ImGui::TextDisabled("loading"); break;
    case mem::SymStatus::Loaded:
        ImGui::Text("%zu", ms->syms.byRva.size());
        if (ImGui::IsItemHovered() && !ms->pdbPath.empty())
            ImGui::SetTooltip("%s", ms->pdbPath.c_str());
        break;
    case mem::SymStatus::Failed:
        ImGui::TextDisabled("none");
        if (ImGui::IsItemHovered() && !ms->note.empty())
            ImGui::SetTooltip("%s", ms->note.c_str());
        break;
    default: ImGui::TextDisabled("-"); break;
    }
}

void symbolsContextMenu(app::AppState& s, const mem::ModuleEntry& m)
{
    const mem::ModuleSymbols* ms = s.symbols.find(m.base);
    const bool loaded = ms && ms->status == mem::SymStatus::Loaded;
    const bool busy   = ms && (ms->status == mem::SymStatus::Queued ||
                               ms->status == mem::SymStatus::Loading);

    if (ImGui::MenuItem(loaded ? "Reload Symbols" : "Load Symbols", nullptr, false, !busy))
        app::requestModuleSymbols(s, m, /*force=*/true);

    // Explicit file: for a private build whose PDB isn't where the search looks.
    if (ImGui::MenuItem("Load PDB from File...", nullptr, false, !busy))
    {
        std::string path;
        if (pickPdbFile(path))
            app::requestModuleSymbols(s, m, /*force=*/true, path.c_str(),
                /*verify=*/!s.symbolIgnoreMismatch);
    }
    ImGui::MenuItem("Ignore PDB build mismatch", nullptr, &s.symbolIgnoreMismatch);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Accept a PDB whose build GUID doesn't match this module.\n"
            "Names will be wrong unless you know the files really do match.");

    if (loaded && ImGui::MenuItem("Copy PDB Path"))
        ImGui::SetClipboardText(ms->pdbPath.c_str());
}

} // namespace ui
