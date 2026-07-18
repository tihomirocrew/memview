#include "ui/memory_view.hpp"
#include "app/app.hpp"
#include "memory/memory.hpp"
#include <imgui.h>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

namespace {

// Copy the selected bytes to the clipboard as "AA BB CC". Re-reads memory
// (the selection may be offscreen); unreadable bytes come out as "??".
void copyHexSelection(app::AppState& s)
{
    if (!s.hexSelStart) return;
    size_t n = (size_t)(s.hexSelEnd - s.hexSelStart + 1);
    if (n > 4096) n = 4096; // sanity cap for the clipboard

    std::vector<uint8_t> bytes(n);
    const size_t got = mem::read_tolerant(s.proc, s.hexSelStart,
        bytes.data(), n);

    std::string out;
    out.reserve(n * 3);
    char pair[4];
    for (size_t i = 0; i < n; ++i)
    {
        if (i < got) snprintf(pair, sizeof(pair), "%02X", bytes[i]);
        else         snprintf(pair, sizeof(pair), "??");
        if (i) out += ' ';
        out += pair;
    }
    ImGui::SetClipboardText(out.c_str());
}

} // namespace

void drawHex(app::AppState& s)
{
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    int rows = (int)(ImGui::GetContentRegionAvail().y / lineH);
    if (rows < 1) rows = 1;

    // Size the address column to the longest visible label so a long
    // "module.exe+offset" never shifts the byte grid.
    std::vector<std::string> labels((size_t)rows);
    int labelW = kAddrLabelWidth;
    for (int r = 0; r < rows; ++r)
    {
        char lbl[40];
        app::formatAddrLabel(s, s.hexAddr + (uintptr_t)r * 16, lbl, sizeof(lbl));
        labels[(size_t)r] = lbl;
        const int L = (int)labels[(size_t)r].size();
        if (L > labelW) labelW = L;
    }

    // Geometry for mouse->cell mapping. The font is monospace, so a byte's cell
    // is a character offset: label, two spaces, then 16 "XX " cells.
    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const float  charW   = ImGui::CalcTextSize("0").x;
    const float  hexColX = (labelW + 2) * charW;
    const float  sbW     = ImGui::GetStyle().ScrollbarSize;
    const float  rowsY0  = origin.y;

    // Invisible catcher over the dump (minus the scrollbar strip, which has its
    // own button). Without it a left-drag on the non-interactive text would move
    // or undock the window instead of extending the selection; while it's active
    // the drag stays ours even past the window edge.
    const float selW = avail.x - sbW;
    ImGui::InvisibleButton("##hexsel",
        ImVec2(selW > 1.f ? selW : 1.f, avail.y > 1.f ? avail.y : 1.f));
    const bool selPressed = ImGui::IsItemActivated();
    const bool selHeld    = ImGui::IsItemActive();
    ImGui::SetCursorScreenPos(origin);

    // Byte cell under the mouse. `hovAddr` needs a real hit on a hex pair (to
    // start a selection); `dragAddr` clamps into the dump so a drag keeps
    // tracking when the mouse strays into the margins or ASCII column.
    const ImVec2 m    = ImGui::GetMousePos();
    const float  relY = m.y - rowsY0;
    const float  hx   = m.x - origin.x - hexColX;

    int r = (int)(relY / lineH);
    int c = (int)(hx / (3.f * charW));
    uintptr_t hovAddr = 0;
    if (relY >= 0.f && r >= 0 && r < rows && hx >= 0.f && c < 16
        && (hx - c * 3.f * charW) < 2.5f * charW) // not in the cell gap
        hovAddr = s.hexAddr + (uintptr_t)r * 16 + (uintptr_t)c;

    r = r < 0 ? 0 : (r >= rows ? rows - 1 : r);
    c = c < 0 ? 0 : (c >= 16 ? 15 : c);
    const uintptr_t dragAddr = s.hexAddr + (uintptr_t)r * 16 + (uintptr_t)c;

    // Click a byte to start a selection, drag to extend; clicking off the cells
    // clears it. The anchor is the fixed end of the drag.
    if (selPressed)
    {
        if (hovAddr)
        {
            s.hexSelAnchor = hovAddr;
            s.hexSelStart  = s.hexSelEnd = hovAddr;
            s.hexSelecting = true;
        }
        else
            s.hexSelStart = s.hexSelEnd = 0;
    }
    if (s.hexSelecting)
    {
        if (selHeld)
        {
            s.hexSelStart = dragAddr < s.hexSelAnchor ? dragAddr : s.hexSelAnchor;
            s.hexSelEnd   = dragAddr > s.hexSelAnchor ? dragAddr : s.hexSelAnchor;
        }
        else
            s.hexSelecting = false;
    }

    if (ImGui::IsWindowHovered())
    {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f)
        {
            const intptr_t delta =
                (intptr_t)(wheel * kLinesPerNotch) * 16;
            uintptr_t na = s.hexAddr - delta; // wheel-up scrolls to lower addrs
            if (na < kAddrFloor) na = kAddrFloor;
            s.hexAddr = na;
        }

        // Mouse "back" button.
        if (ImGui::IsMouseClicked(kMouseButtonBack))
            goBack(s.hexHistory, s.hexAddr);

        // Ctrl+G opens the same Go to Address popup as the context menu.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G))
        {
            s.gotoHexInput[0] = '\0';
            s.showGotoHex     = true;
        }

        // Ctrl+C copies the selected bytes as hex text.
        if (s.hexSelStart &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
            copyHexSelection(s);
    }

    // ASCII column starts after the 16 hex cells plus the single-space gap.
    const float asciiColX = hexColX + (16 * 3 + 1) * charW;

    for (int r = 0; r < rows; ++r)
    {
        const uintptr_t rowAddr = s.hexAddr + (uintptr_t)r * 16;
        uint8_t row[16];
        const size_t got = mem::read_tolerant(s.proc, rowAddr, row, 16);

        // Highlight selected bytes (hex pair + ASCII cell). A cell whose
        // successor is also selected extends over the gap for a continuous band.
        if (s.hexSelStart && rowAddr + 15 >= s.hexSelStart
            && rowAddr <= s.hexSelEnd)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
            const float y0 = rowsY0 + r * lineH;
            for (int c = 0; c < 16; ++c)
            {
                const uintptr_t a = rowAddr + (uintptr_t)c;
                if (a < s.hexSelStart || a > s.hexSelEnd) continue;
                const bool joinNext = c < 15 && a + 1 <= s.hexSelEnd;
                const float x0 = origin.x + hexColX + c * 3.f * charW;
                dl->AddRectFilled(ImVec2(x0, y0),
                    ImVec2(x0 + (joinNext ? 3.f : 2.f) * charW, y0 + lineH), col);
                const float ax = origin.x + asciiColX + c * charW;
                dl->AddRectFilled(ImVec2(ax, y0),
                    ImVec2(ax + charW, y0 + lineH), col);
            }
        }

        char line[200];
        int  p = snprintf(line, sizeof(line), "%-*s  ",
            labelW, labels[(size_t)r].c_str());

        for (int c = 0; c < 16; ++c)
            p += (c < (int)got)
                ? snprintf(line + p, sizeof(line) - p, "%02X ", row[c])
                : snprintf(line + p, sizeof(line) - p, "?? ");

        p += snprintf(line + p, sizeof(line) - p, " ");
        for (int c = 0; c < 16; ++c)
        {
            const char ch = (c < (int)got && row[c] >= 0x20 && row[c] < 0x7f)
                          ? (char)row[c] : '.';
            if (p < (int)sizeof(line) - 1) line[p++] = ch;
        }
        line[p] = '\0';

        if (got == 0) ImGui::TextDisabled("%s", line);
        else          ImGui::TextUnformatted(line);
    }

    // Window context menu (rows aren't selectable items).
    if (ImGui::BeginPopupContextWindow("##hexctx"))
    {
        if (ImGui::MenuItem("Go to Address", "Ctrl+G"))
        {
            s.gotoHexInput[0] = '\0';
            s.showGotoHex     = true;
        }
        if (ImGui::MenuItem("Back", "Mouse 4", false, !s.hexHistory.empty()))
            goBack(s.hexHistory, s.hexAddr);
        if (ImGui::MenuItem("Copy Bytes", "Ctrl+C", false, s.hexSelStart != 0))
            copyHexSelection(s);

        ImGui::Separator();
        // IDA-style sync: follow Disassembly's address. The snap on toggle-on
        // doesn't push history; Back is for explicit jumps only.
        if (ImGui::MenuItem("Sync with Disassembly", nullptr, s.syncHexToDisasm))
        {
            s.syncHexToDisasm = !s.syncHexToDisasm;
            if (s.syncHexToDisasm)
                s.hexAddr = s.disasmAddr;
        }
        ImGui::EndPopup();
    }

    // --- Region scrollbar ----------------------------------------------------
    // Like the Disassembly one: the thumb tracks the view within the current
    // module (falling back to the region, then a 64 KB window), frozen while
    // dragging. Dragging keeps the address 16-byte aligned.
    if (!s.hexSbDrag)
    {
        s.hexSbBase = 0;
        s.hexSbSize = 0;
        if (const mem::ModuleEntry* mod = app::findModule(s, s.hexAddr))
        {
            s.hexSbBase = mod->base;
            s.hexSbSize = mod->size;
        }
        else
            for (const mem::Region& rg : s.memRegions)
                if (s.hexAddr >= rg.base && s.hexAddr < rg.base + rg.size)
                {
                    s.hexSbBase = rg.base;
                    s.hexSbSize = rg.size;
                    break;
                }
        if (!s.hexSbSize)
        {
            constexpr uintptr_t half = 0x8000;
            s.hexSbBase = s.hexAddr > kAddrFloor + half
                ? s.hexAddr - half : kAddrFloor;
            s.hexSbSize = 2 * half;
        }
    }

    {
        const float  trackH = (float)rows * lineH;
        const ImVec2 tl(origin.x + avail.x - sbW, rowsY0);

        ImGui::SetCursorScreenPos(tl);
        ImGui::InvisibleButton("##hexsb",
            ImVec2(sbW, trackH > 1.f ? trackH : 1.f));
        s.hexSbDrag = ImGui::IsItemActive();

        if (s.hexSbDrag)
        {
            float frac = (ImGui::GetMousePos().y - tl.y) / trackH;
            frac = frac < 0.f ? 0.f : (frac > 1.f ? 1.f : frac);
            uintptr_t na = s.hexSbBase
                + (uintptr_t)((double)frac * (double)s.hexSbSize);
            na &= ~(uintptr_t)15;
            if (na < kAddrFloor) na = kAddrFloor;
            s.hexAddr = na;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(tl, ImVec2(tl.x + sbW, tl.y + trackH),
            ImGui::GetColorU32(ImGuiCol_ScrollbarBg));

        float thumbH = trackH
            * (float)((double)rows * 16.0 / (double)s.hexSbSize);
        if (thumbH < 24.f)    thumbH = 24.f;
        if (thumbH > trackH)  thumbH = trackH;

        double tf = (double)(s.hexAddr - s.hexSbBase) / (double)s.hexSbSize;
        tf = tf < 0.0 ? 0.0 : (tf > 1.0 ? 1.0 : tf);
        const float ty = tl.y + (float)tf * (trackH - thumbH);

        const ImU32 col = ImGui::GetColorU32(
            ImGui::IsItemActive()  ? ImGuiCol_ScrollbarGrabActive :
            ImGui::IsItemHovered() ? ImGuiCol_ScrollbarGrabHovered
                                   : ImGuiCol_ScrollbarGrab);
        dl->AddRectFilled(ImVec2(tl.x + 2.f, ty),
            ImVec2(tl.x + sbW - 2.f, ty + thumbH), col,
            ImGui::GetStyle().ScrollbarRounding);
    }
}

} // namespace ui
