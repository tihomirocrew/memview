#include "ui/memory_view.hpp"
#include "app/app.hpp"
#include "memory/memory.hpp"
#include <imgui.h>
#include <cstdio>

namespace ui {

namespace {

// "RWX"-style protection flags for a region.
void protString(DWORD prot, char out[4])
{
    const bool readable = prot & (PAGE_READONLY | PAGE_READWRITE |
        PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY);
    out[0] = readable            ? 'R' : '-';
    out[1] = mem::is_writable(prot)   ? 'W' : '-';
    out[2] = mem::is_executable(prot) ? 'X' : '-';
    out[3] = '\0';
}

const char* typeString(DWORD type)
{
    switch (type)
    {
    case MEM_IMAGE:   return "Image";
    case MEM_MAPPED:  return "Mapped";
    case MEM_PRIVATE: return "Private";
    default:          return "";
    }
}

} // namespace

void drawRegions(app::AppState& s)
{
    // Refresh the region snapshot every second (every frame would be thousands
    // of syscalls).
    const double now = ImGui::GetTime();
    if (now >= s.regionsNextRefresh)
    {
        s.memRegions = mem::query_regions(s.proc);
        s.regionsNextRefresh = now + 1.0;
    }

    // Mouse "back" button undoes the last jump in both panes, since a region
    // jump moves disasm and hex together.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(kMouseButtonBack))
    {
        const bool went = goBack(s.disasmHistory, s.disasmAddr);
        if (goBack(s.hexHistory, s.hexAddr) || went)
            snprintf(s.memGotoInput, sizeof(s.memGotoInput), "%llX",
                (unsigned long long)s.disasmAddr);
    }

    ImGui::Text("%d committed regions", (int)s.memRegions.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(double-click a row to view it)");

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##regions", 6, flags))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address");
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Protect");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("State");
    ImGui::TableSetupColumn("Module");
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin((int)s.memRegions.size());
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const mem::Region& rg = s.memRegions[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char label[24];
            snprintf(label, sizeof(label), "%016llX",
                (unsigned long long)rg.base);
            if (ImGui::Selectable(label, false,
                ImGuiSelectableFlags_SpanAllColumns |
                ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(0))
            {
                pushHistory(s.disasmHistory, s.disasmAddr);
                pushHistory(s.hexHistory, s.hexAddr);
                s.disasmAddr = rg.base;
                s.hexAddr    = rg.base;
                snprintf(s.memGotoInput, sizeof(s.memGotoInput), "%llX",
                    (unsigned long long)rg.base);
            }

            // Right-click to copy the base address. The popup binds to the
            // Selectable above, whose per-row label is unique.
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Copy Address"))
                {
                    // Unpadded, no leading zeros.
                    char copyBuf[24];
                    snprintf(copyBuf, sizeof(copyBuf), "%llX",
                        (unsigned long long)rg.base);
                    ImGui::SetClipboardText(copyBuf);
                }

                // Offset from the module base; disabled outside any module.
                const mem::ModuleEntry* rvaMod = app::findModule(s, rg.base);
                if (ImGui::MenuItem("Copy RVA", nullptr, false, rvaMod != nullptr))
                {
                    char rva[24];
                    snprintf(rva, sizeof(rva), "%llX",
                        (unsigned long long)(rg.base - rvaMod->base));
                    ImGui::SetClipboardText(rva);
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llX", (unsigned long long)rg.size);

            ImGui::TableSetColumnIndex(2);
            char prot[4];
            protString(rg.protect, prot);
            ImGui::TextUnformatted(prot);

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(typeString(rg.type));

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(rg.state == MEM_COMMIT ? "Commit"
                : rg.state == MEM_RESERVE ? "Reserve" : "Free");

            ImGui::TableSetColumnIndex(5);
            if (const mem::ModuleEntry* m = app::findModule(s, rg.base))
                ImGui::TextUnformatted(m->name.c_str());
            else
                ImGui::TextDisabled("-");
        }
    }

    ImGui::EndTable();
}

} // namespace ui
