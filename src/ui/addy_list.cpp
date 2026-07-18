#include "ui/addy_list.hpp"
#include "app/app.hpp"
#include "memory/value_format.hpp"
#include <imgui.h>
#include <cstdio>
#include <cstdlib>

namespace ui {

void drawAddyList(app::AppState& s)
{
    // Refresh modules so "module.exe+offset" resolves even without Memory View
    // open. Shares s.modulesNextRefresh with it to avoid double refreshing.
    if (s.proc.is_open())
    {
        const double modNow = ImGui::GetTime();
        if (modNow >= s.modulesNextRefresh)
        {
            s.modules = mem::list_modules(s.proc);
            s.modulesNextRefresh = modNow + 2.0;
        }
    }

    ImGui::Separator();

    const float btnH   = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2;
    const float tableH = ImGui::GetContentRegionAvail().y - btnH;

    // Fixed columns with ScrollX and no Resizable: widths are locked, at the
    // cost of a horizontal scrollbar when they don't fit.
    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit;

    int removeIdx = -1; // row to erase after the loop (avoids invalidation)

    if (ImGui::BeginTable("##addylist", 6, flags, ImVec2(0, tableH)))
    {
        const float removeColW = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.x * 2.f;

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 200);
        ImGui::TableSetupColumn("Address",     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 120);
        ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,  85);
        ImGui::TableSetupColumn("Value",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize,  80);
        ImGui::TableSetupColumn("Frozen",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 54);
        // Sized to fit the "X" button plus cell padding.
        ImGui::TableSetupColumn("",            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, removeColW);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)s.addyList.size(); ++i)
        {
            auto& e = s.addyList[i];
            ImGui::TableNextRow();
            char id[32];

            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-1);
            snprintf(id, sizeof(id), "##desc%d", i);
            ImGui::InputText(id, e.desc, sizeof(e.desc));

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            // Sized for a "module.exe+offset" expression, not just a hex address.
            char addrBuf[128];
            snprintf(addrBuf, sizeof(addrBuf), "%08llX", (unsigned long long)e.address);
            snprintf(id, sizeof(id), "##addr%d", i);
            ImGui::InputText(id, addrBuf, sizeof(addrBuf));

            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                uintptr_t parsed = 0;
                if (app::parseAddrExpr(s, addrBuf, parsed))
                    e.address = parsed;
                // Unparseable input is discarded; the field reverts next frame.
            }

            snprintf(id, sizeof(id), "##addrctx%d", i);
            if (ImGui::BeginPopupContextItem(id))
            {
                if (ImGui::MenuItem("Copy Address"))
                {
                    // Unpadded, no leading zeros.
                    char copyBuf[24];
                    snprintf(copyBuf, sizeof(copyBuf), "%llX",
                        (unsigned long long)e.address);
                    ImGui::SetClipboardText(copyBuf);
                }

                // Offset from the module base; disabled outside any module.
                const mem::ModuleEntry* rvaMod = app::findModule(s, e.address);
                if (ImGui::MenuItem("Copy RVA", nullptr, false, rvaMod != nullptr))
                {
                    char rva[24];
                    snprintf(rva, sizeof(rva), "%llX",
                        (unsigned long long)(e.address - rvaMod->base));
                    ImGui::SetClipboardText(rva);
                }
                if (ImGui::MenuItem("Open in Memory View"))
                    app::openMemoryViewAt(s, e.address);
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            snprintf(id, sizeof(id), "##type%d", i);
            ImGui::Combo(id, &e.typeIdx, app::kValueTypeNames, app::kValueTypeCount);

            // Right-click the type to set how String / Pattern are read: the
            // byte length (there's no length column) and, for String, the
            // encoding used for reads and writes. Both are per-entry.
            const mem::ValueType ctxVt = app::uiValueType(e.typeIdx);
            if (ctxVt == mem::ValueType::String || ctxVt == mem::ValueType::ArrayOfBytes)
            {
                snprintf(id, sizeof(id), "##typectx%d", i);
                if (ImGui::BeginPopupContextItem(id))
                {
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputInt("Length", &e.length);
                    if (e.length < 1)   e.length = 1;
                    if (e.length > 256) e.length = 256;

                    if (ctxVt == mem::ValueType::String)
                    {
                        ImGui::SeparatorText("String encoding");
                        if (ImGui::MenuItem("UTF-8",  nullptr, e.stringEncoding == 0))
                            e.stringEncoding = 0;
                        if (ImGui::MenuItem("UTF-16", nullptr, e.stringEncoding == 1))
                            e.stringEncoding = 1;
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1);
            snprintf(id, sizeof(id), "##val%d", i);

            mem::ValueType vt = app::uiValueType(e.typeIdx);

            const bool committed = ImGui::InputText(id, e.value, sizeof(e.value),
                ImGuiInputTextFlags_EnterReturnsTrue);
            const bool editingValue = ImGui::IsItemActive();
            const bool justEdited   = committed || ImGui::IsItemDeactivatedAfterEdit();

            const bool utf16 = (vt == mem::ValueType::String && e.stringEncoding == 1);

            bool wroteThisFrame = false;
            // User committed an edit: write it into the process.
            if (justEdited && s.proc.is_open())
            {
                uint8_t buf[256];
                if (size_t n = app::parseValue(e.value, vt, buf, sizeof(buf), utf16))
                {
                    bool ok = mem::write_raw(s.proc, e.address, buf, n);
                    e.writeStatus  = ok ? 1 : 2;
                    wroteThisFrame = true;
                }
                else
                {
                    e.writeStatus = 2; // couldn't parse the text
                }
            }

            // Live sync with process memory.
            if (s.proc.is_open() && e.address)
            {
                if (e.frozen)
                {
                    // Keep writing the frozen value.
                    uint8_t buf[256];
                    if (size_t n = app::parseValue(e.value, vt, buf, sizeof(buf), utf16))
                        mem::write_raw(s.proc, e.address, buf, n);
                }
                else if (!editingValue && !wroteThisFrame)
                {
                    // Refresh the displayed value from memory, but not on the
                    // frame we just wrote (keep the typed value as feedback).
                    // Scalars read their exact width; String/Pattern read the
                    // per-entry length (settable via the type's right-click menu).
                    uint8_t buf[256] = {};
                    size_t width = mem::value_size(vt);
                    if (width == 0)
                    {
                        int len = e.length;
                        if (len < 1) len = 1;
                        if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
                        width = (size_t)len;
                    }
                    if (mem::read_raw(s.proc, e.address, buf, width))
                        app::formatValue(buf, width, vt, e.value, sizeof(e.value), utf16);
                }
            }

            ImGui::TableSetColumnIndex(4);
            snprintf(id, sizeof(id), "##frz%d", i);
            ImGui::Checkbox(id, &e.frozen);
            // Write feedback dot.
            if (e.writeStatus)
            {
                ImGui::SameLine();
                if (e.writeStatus == 1)
                {
                    ImGui::TextColored(ImVec4(0.1f, 0.7f, 0.1f, 1.f), "OK");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("WriteProcessMemory succeeded");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.85f, 0.1f, 0.1f, 1.f), "FAIL");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Write failed (err %lu)\nAddress may be unwritable, "
                            "or value couldn't be parsed.", (unsigned long)GetLastError());
                }
            }

            // Remove button, sized to match a checkbox and centered in its column.
            ImGui::TableSetColumnIndex(5);
            snprintf(id, sizeof(id), "X##del%d", i);
            const float btnSz  = ImGui::GetFrameHeight();
            const float availW = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - btnSz) * 0.5f);
            if (ImGui::Button(id, ImVec2(btnSz, btnSz)))
                removeIdx = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove this address");
        }

        ImGui::EndTable();
    }

    if (removeIdx >= 0 && removeIdx < (int)s.addyList.size())
        s.addyList.erase(s.addyList.begin() + removeIdx);

    ImGui::Spacing();
    if (ImGui::Button("Add Address Manually"))
    {
        // Open the modal with fresh fields. Refresh modules so
        // "module.exe+offset" resolves even without Memory View open.
        if (s.proc.is_open())
            s.modules = mem::list_modules(s.proc);
        s.addAddrInput[0] = '\0';
        snprintf(s.addAddrDesc, sizeof(s.addAddrDesc), "No description");
        s.addAddrType  = 2; // default to 4 Bytes
        s.addAddrStringEncoding = 0; // default to UTF-8
        s.addAddrLength = 8; // default width for String/Pattern
        s.showAddAddr  = true;
    }

    // Bulk-remove every saved address, behind a confirmation (the list isn't
    // persisted, so entries can't be recovered).
    ImGui::SameLine();
    ImGui::BeginDisabled(s.addyList.empty());
    if (ImGui::Button("Clear All"))
        s.showClearAddy = true;
    ImGui::EndDisabled();
}

void drawAddAddressModal(app::AppState& s)
{
    if (!s.showAddAddr) return;

    // Center on the main window.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::OpenPopup("Add Address");

    if (!ImGui::BeginPopupModal("Add Address", &s.showAddAddr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;

    ImGui::TextUnformatted("Address (hex), or module+offset");
    ImGui::SetNextItemWidth(-1);
    bool submit = ImGui::InputText("##addaddr", s.addAddrInput, sizeof(s.addAddrInput),
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();
    ImGui::TextUnformatted("Description");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##adddesc", s.addAddrDesc, sizeof(s.addAddrDesc));

    ImGui::Spacing();
    ImGui::TextUnformatted("Type");
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##addtype", &s.addAddrType, app::kValueTypeNames, app::kValueTypeCount);

    const mem::ValueType addVt = app::uiValueType(s.addAddrType);

    // String/Pattern have no fixed width: let the user set how many bytes to
    // read/display. Hidden for numeric types (their size is implied).
    if (addVt == mem::ValueType::String || addVt == mem::ValueType::ArrayOfBytes)
    {
        ImGui::Spacing();
        ImGui::TextUnformatted("Length (bytes)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##addlen", &s.addAddrLength);
        if (s.addAddrLength < 1)   s.addAddrLength = 1;
        if (s.addAddrLength > 256) s.addAddrLength = 256;
    }

    // String needs an encoding (UTF-8 vs UTF-16LE); hidden for numeric types.
    if (addVt == mem::ValueType::String)
    {
        ImGui::Spacing();
        ImGui::TextUnformatted("Encoding");
        ImGui::SetNextItemWidth(-1);
        const char* encNames[] = { "UTF-8", "UTF-16" };
        ImGui::Combo("##addenc", &s.addAddrStringEncoding, encNames, 2);
    }

    uintptr_t parsedAddr = 0;
    const bool valid = s.addAddrInput[0] != '\0' &&
        app::parseAddrExpr(s, s.addAddrInput, parsedAddr);

    // Live preview of the current value at the address.
    // Re-reads every frame while the modal is open (the read is tiny), so it
    // tracks memory and reacts immediately to type/length/encoding changes.
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Value");
    ImGui::SameLine();
    if (!valid)
    {
    }
    else if (!s.proc.is_open())
    {
    }
    else
    {
        const bool utf16 = (addVt == mem::ValueType::String &&
                            s.addAddrStringEncoding == 1);
        // Scalars read their fixed width; String/Pattern read addAddrLength.
        size_t width = mem::value_size(addVt);
        if (width == 0)
        {
            int len = s.addAddrLength;
            if (len < 1) len = 1;
            if (len > 256) len = 256;
            width = (size_t)len;
        }
        uint8_t buf[256] = {};
        if (mem::read_raw(s.proc, parsedAddr, buf, width))
        {
            char preview[128];
            app::formatValue(buf, width, addVt, preview, sizeof(preview), utf16);
            ImGui::TextUnformatted(preview);
        }
        else
        {
            ImGui::TextDisabled("Unreadable");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Add", ImVec2(120, 0)) || (submit && valid))
    {
        app::AddyEntry e = {};
        snprintf(e.desc, sizeof(e.desc), "%s", s.addAddrDesc);
        snprintf(e.value, sizeof(e.value), "0");
        e.address = parsedAddr;
        e.typeIdx = s.addAddrType;
        e.stringEncoding = s.addAddrStringEncoding;
        e.length = s.addAddrLength;
        s.addyList.push_back(e);
        s.showAddAddr = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        s.showAddAddr = false;

    ImGui::EndPopup();
}

void drawClearAddyConfirm(app::AppState& s)
{
    if (!s.showClearAddy) return;

    // Nothing to confirm if the list emptied out while the dialog was open.
    if (s.addyList.empty()) { s.showClearAddy = false; return; }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::OpenPopup("Clear All Addresses");

    if (!ImGui::BeginPopupModal("Clear All Addresses", &s.showClearAddy,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;

    const int n = (int)s.addyList.size();
    ImGui::Text("Remove all %d address%s from the list?", n, n == 1 ? "" : "es");
    ImGui::TextDisabled("This can't be undone.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Clear All", ImVec2(120, 0)))
    {
        s.addyList.clear();
        s.showClearAddy = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        s.showClearAddy = false;

    ImGui::EndPopup();
}

} // namespace ui
