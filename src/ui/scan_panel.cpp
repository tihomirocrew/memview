#include "ui/scan_panel.hpp"
#include "app/app.hpp"
#include "memory/value_format.hpp"
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue for the tri-state box
#include <cstdio>
#include <algorithm>

namespace ui {

void drawScanResults(app::AppState& s)
{
    // Header
    if (s.scan.running())
    {
        ImGui::TextDisabled("Scanning...");
    }
    else if (s.scan.firstScanDone())
    {
        char hdr[80];
        const size_t found = s.scan.totalFound();
        if (s.scan.capped())
            snprintf(hdr, sizeof(hdr), "Found: %zu addresses (showing %zu)",
                found, app::ScanSession::kDisplayCap);
        else
            snprintf(hdr, sizeof(hdr), "Found: %zu address%s",
                found, found == 1 ? "" : "es");
        ImGui::TextDisabled("%s", hdr);
    }
    else
    {
        ImGui::TextDisabled("Found: 0 addresses");
    }

    const auto& rows = s.scan.results();

    // Keep the selection array in sync with the visible results.
    if ((int)s.resultSel.size() != (int)rows.size())
    {
        s.resultSel.assign(rows.size(), 0);
        s.selAnchor = -1;
    }

    // Ctrl+A selects every result, but only while this pane has focus so it
    // doesn't fight Ctrl+A in the scan value input.
    if (!rows.empty() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
    {
        std::fill(s.resultSel.begin(), s.resultSel.end(), (char)1);
        s.selAnchor = 0;
    }

    // SizingFixedFit: ImGui doesn't support StretchProp with mixed Fixed/Stretch
    // columns. "Previous" is the lone Stretch column and sits last, so resizing
    // Address/Value only reflows it.
    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingFixedFit;

    // Reserve a row at the bottom for the "add to addy list" button.
    const float tableH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();

    if (ImGui::BeginTable("##scan_results", 3, flags, ImVec2(0, tableH)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address",  ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Value",    ImGuiTableColumnFlags_WidthFixed,  80);
        ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const auto& r = rows[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                // Absolute address for the clipboard; the label shows
                // "module+offset" when inside a module. "###row%d" keeps the
                // widget ID stable regardless of the label text.
                char addr[24];
                snprintf(addr, sizeof(addr), "%08llX", (unsigned long long)r.address);
                char addrLabel[48];
                app::formatAddrLabel(s, r.address, addrLabel, sizeof(addrLabel));
                char rowLabel[64];
                snprintf(rowLabel, sizeof(rowLabel), "%s###row%d", addrLabel, i);

                ImGui::PushID(i);
                const bool selected = s.resultSel[i] != 0;
                if (ImGui::Selectable(rowLabel, selected,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap))
                {
                    ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyCtrl)
                    {
                        s.resultSel[i] = s.resultSel[i] ? 0 : 1;
                        s.selAnchor = i;
                    }
                    else if (io.KeyShift && s.selAnchor >= 0)
                    {
                        int a = s.selAnchor < i ? s.selAnchor : i;
                        int b = s.selAnchor < i ? i : s.selAnchor;
                        std::fill(s.resultSel.begin(), s.resultSel.end(), (char)0);
                        for (int k = a; k <= b; ++k) s.resultSel[k] = 1;
                    }
                    else
                    {
                        std::fill(s.resultSel.begin(), s.resultSel.end(), (char)0);
                        s.resultSel[i] = 1;
                        s.selAnchor = i;
                    }
                }
                // Double-click adds the row straight to the addy list.
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    app::addAddyFromResult(s, i);

                // Right-click to copy the address.
                if (ImGui::BeginPopupContextItem("##rowctx"))
                {
                    if (ImGui::MenuItem("Copy Address"))
                    {
                        // Unpadded, no leading zeros.
                        char copyBuf[24];
                        snprintf(copyBuf, sizeof(copyBuf), "%llX",
                            (unsigned long long)r.address);
                        ImGui::SetClipboardText(copyBuf);
                    }

                    // Offset from the module base; disabled outside any module.
                    const mem::ModuleEntry* rvaMod = app::findModule(s, r.address);
                    if (ImGui::MenuItem("Copy RVA", nullptr, false, rvaMod != nullptr))
                    {
                        char rva[24];
                        snprintf(rva, sizeof(rva), "%llX",
                            (unsigned long long)(r.address - rvaMod->base));
                        ImGui::SetClipboardText(rva);
                    }
                    if (ImGui::MenuItem("Add to List"))
                        app::addAddyFromResult(s, i);
                    if (ImGui::MenuItem("Open in Memory View"))
                        app::openMemoryViewAt(s, r.address);
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", r.value.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", r.prev);
            }
        }

        ImGui::EndTable();
    }

    // "Add to addy list" button reflecting the current selection.
    int selCount = 0;
    for (char c : s.resultSel) selCount += (c != 0);

    ImGui::BeginDisabled(selCount == 0);
    char btn[48];
    if (selCount > 0) snprintf(btn, sizeof(btn), "Add %d to List", selCount);
    else              snprintf(btn, sizeof(btn), "Add to List");
    if (ImGui::Button(btn, ImVec2(-1, 0)))
        app::addSelectedResultsToAddy(s);
    ImGui::EndDisabled();
}

void drawScanControls(app::AppState& s)
{
    // String and byte-pattern types only match for equality, so pin Exact.
    const bool isString = (s.valueType == 6);
    const bool isAob    = (s.valueType == 7);
    if (isString || isAob) s.scanType = 0; // Exact

    ImGui::Spacing();
    ImGui::Text("Value");

    const mem::ScanType stEnum    = app::uiScanType(s.scanType);
    const bool          needsValue = app::scanNeedsValue(stEnum);
    const bool          isBetween  = (stEnum == mem::ScanType::Between);

    if (isBetween)
    {
        // Two bounds (lo .. hi) side by side.
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float w  = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
        ImGui::SetNextItemWidth(w);
        ImGui::InputText("##val", s.scanValue, sizeof(s.scanValue));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        ImGui::InputText("##val2", s.scanValue2, sizeof(s.scanValue2));
    }
    else
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(!needsValue);
        ImGui::InputText("##val", s.scanValue, sizeof(s.scanValue));
        ImGui::EndDisabled();
        if (isAob && ImGui::IsItemHovered())
            ImGui::SetTooltip("Byte pattern, e.g.\nE8 ? ? ? ? 48 83 3D 0C B0 20 00 00\n"
                              "? or ?? = wildcard byte, A? / ?B = nibble wildcard");
    }

    ImGui::Spacing();
    ImGui::Text("Scan Type");
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(isString || isAob);
    // Relative scan types diff against the previous scan, so they only appear
    // once a first scan exists. Until then, cap the combo to the first-scan
    // entries and clamp any stale relative selection back to Exact.
    const int typeCount = s.scan.firstScanDone()
                        ? app::kScanTypeCount
                        : app::kScanTypeFirstCount;
    if (s.scanType >= typeCount) s.scanType = 0;
    ImGui::Combo("##scantype", &s.scanType, app::kScanTypeNames, typeCount);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Text("Value Type");
    ImGui::SetNextItemWidth(-1);
    
    ImGui::BeginDisabled(s.scan.firstScanDone());
    ImGui::Combo("##valtype", &s.valueType, app::kValueTypeNames, app::kValueTypeCount);
    ImGui::EndDisabled();

    // String-only options: case folding and encoding.
    if (isString)
    {
        ImGui::Spacing();
        ImGui::Checkbox("Case sensitive", &s.stringCaseSensitive);

        ImGui::Text("Encoding");
        ImGui::SetNextItemWidth(-1);
        static const char* const kEncNames[] = { "UTF-8", "UTF-16" };
        ImGui::Combo("##strenc", &s.stringEncoding, kEncNames,
                     (int)(sizeof(kEncNames) / sizeof(kEncNames[0])));
    }

    ImGui::Spacing();
    ImGui::Text("Scan Options");

    // Tri-state page-attribute filter, cycling on click:
    //   dash = don't care -> checked = only -> unchecked = exclude -> dash.
    // Only offered before the first scan, which commits to its regions.
    auto triFilter = [&](const char* label, int& state,
                         const char* onlyDesc, const char* excludeDesc,
                         const char* anyDesc)
    {
        const bool mixed   = (state == 0);
        bool       checked = (state == 1);
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
        if (ImGui::Checkbox(label, &checked))
            state = (state + 1) % 3; // cycle 0->1->2->0
        ImGui::PopItemFlag();
        if (ImGui::IsItemHovered())
        {
            const char* desc = state == 1 ? onlyDesc
                             : state == 2 ? excludeDesc
                                          : anyDesc;
            ImGui::SetTooltip("%s: %s\n(click to cycle)", label, desc);
        }
    };

    ImGui::BeginDisabled(s.scan.firstScanDone());
    triFilter("Writable", s.writableFilter,
        "Only scan writable memory",
        "Do not scan writable memory",
        "Scan writable and read-only memory");
    triFilter("Executable", s.executableFilter,
        "Only scan executable memory",
        "Do not scan executable memory",
        "Scan executable and non-executable memory");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float btnW = -1;
    ImGui::BeginDisabled(!s.proc.is_open() || s.scan.running());

    if (!s.scan.firstScanDone())
    {
        if (ImGui::Button("First Scan", ImVec2(btnW, 0))) app::startFirstScan(s);
    }
    else
    {
        if (ImGui::Button("Next Scan", ImVec2(btnW, 0))) app::startNextScan(s);
        ImGui::Spacing();
        if (ImGui::Button("New Scan",  ImVec2(btnW, 0))) s.scan.reset();
    }

    ImGui::EndDisabled();

    if (s.scan.running())
    {
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(btnW, 0))) s.scan.cancel();
        ImGui::Spacing();
        ImGui::TextDisabled("Scanning memory...");
    }
    else if (!s.proc.is_open())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Attach to a process\nto start scanning.");
    }
}

} // namespace ui
