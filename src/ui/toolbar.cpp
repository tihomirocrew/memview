#include "ui/toolbar.hpp"
#include "ui/process_picker.hpp"
#include "app/app.hpp"
#include "app/config.hpp"
#include <imgui.h>
#include <shellapi.h>
#include <cstdio>
#include "assets/fonts/fontawesome6/IconsFontAwesome6.h"
#include "assets/fonts/fontawesome6/IconsFontAwesome6Brands.h"
#include "version.h"

namespace ui {

void drawToolbar(app::AppState& s)
{
    ImGui::BeginChild("##toolbar", ImVec2(0, 30), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(5);

    // Draw the process icon inside the read-only input: widen the field's left
    // padding, then paint the icon into that gap once the field is laid out.
    constexpr float kIconSize = 16.0f;
    ID3D11ShaderResourceView* icon = icons::get(s.procIconPath);
    const ImVec2 basePad = ImGui::GetStyle().FramePadding;
    if (icon)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(basePad.x + kIconSize + 4.0f, basePad.y));

    // A dead session shouldn't look like a live one reading zeroes.
    if (s.procExited)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.1f, 0.1f, 1.f));
    ImGui::SetNextItemWidth(240);
    ImGui::InputText("##proc", s.processLabel, sizeof(s.processLabel),
        ImGuiInputTextFlags_ReadOnly);
    if (s.procExited) ImGui::PopStyleColor();

    if (icon)
    {
        ImGui::PopStyleVar();
        const ImVec2 rectMin = ImGui::GetItemRectMin();
        const float  rectH   = ImGui::GetItemRectSize().y;
        const ImVec2 iconMin(rectMin.x + basePad.x, rectMin.y + (rectH - kIconSize) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)icon,
            iconMin, ImVec2(iconMin.x + kIconSize, iconMin.y + kIconSize));
    }

    ImGui::SameLine();
    if (ImGui::Button("Attach"))
    {
        s.procList      = mem::list_processes();
        s.procSelected  = -1;
        s.procFilter[0] = '\0';
        s.showProcPicker = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.proc.is_open());
    if (ImGui::Button("Memory View"))
    {
        // Toggle: close if already open.
        if (s.showMemView) s.showMemView = false;
        else               app::openMemoryView(s);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Settings")) s.showSettings = !s.showSettings;
    ImGui::EndChild();
    ImGui::Separator();
}

void drawSettings(app::AppState& s)
{
    if (!s.showSettings) return;

    // Center the window on its first open; FirstUseEver then lets a dragged
    // position win on later opens. (The brief 0,0 flash on open is a separate,
    // DWM-composition issue handled by cloaking in main.cpp, not by this seed.)
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainVp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    // Always its own OS window: never dockable, never auto-merged into the main
    // window even when it overlaps.
    ImGuiWindowClass alwaysOwnWindow;
    alwaysOwnWindow.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&alwaysOwnWindow);
    const ImGuiWindowFlags settingsFlags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoResize;
    if (ImGui::Begin("Settings", &s.showSettings, settingsFlags))
    {
        // NoResize only blocks resizing from inside ImGui; the native OS
        // decoration still carries WS_MAXIMIZEBOX, so the maximize button and
        // Win+Up would fullscreen it. Strip that bit from the HWND.
        if (HWND hwnd = (HWND)ImGui::GetWindowViewport()->PlatformHandle)
        {
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if (style & WS_MAXIMIZEBOX)
                SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_MAXIMIZEBOX);
        }

        if (ImGui::BeginTabBar("##settingsTabs"))
        {
            if (ImGui::BeginTabItem("Settings"))
            {
                static const char* const kThemeNames[] = { "Light", "Dark" };
                int themeIdx = s.darkTheme ? 1 : 0;
                ImGui::Text("Theme");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##theme", &themeIdx, kThemeNames,
                                  (int)(sizeof(kThemeNames) / sizeof(kThemeNames[0]))))
                {
                    app::applyTheme(s, themeIdx == 1);
                    app::saveConfig(s);
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("About"))
            {
                // Centered app icon at 3x: scale the font for this one glyph,
                // then reset before the rest of the tab.
                constexpr float kIconScale = 3.0f;
                ImGui::SetWindowFontScale(kIconScale);
                const float iconW = ImGui::CalcTextSize(ICON_FA_MEMORY).x;
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - iconW) * 0.5f);
                ImGui::TextUnformatted(ICON_FA_MEMORY);
                ImGui::SetWindowFontScale(1.0f);
                // The scaled glyph leaves a big gap below it; pull the title back
                // up into that dead space, accounting for the icon's GlyphOffset.
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 12.0f + 3.0f * kIconScale);

                char title[32];
                snprintf(title, sizeof(title), "MemView v%s", MEMVIEW_VERSION_STRING);
                const float titleW = ImGui::CalcTextSize(title).x;
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - titleW) * 0.5f);
                ImGui::TextUnformatted(title);
                ImGui::TextWrapped(
                    "Fast, lightweight memory scanner and editor for Windows.");
                ImGui::TextDisabled("Built %s %s", __DATE__, __TIME__);
                ImGui::Spacing();
                ImGui::TextUnformatted("MIT License");
                ImGui::TextDisabled("Copyright (c) 2026 tihomirocrew");
                ImGui::Spacing();

                if (ImGui::Button(ICON_FA_GITHUB " GitHub"))
                    ShellExecuteW(nullptr, L"open",
                        L"https://github.com/tihomirocrew/memview",
                        nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::Spacing();

                ImGui::TextUnformatted("Third-party");
                ImGui::BulletText("Dear ImGui (MIT)");
                ImGui::BulletText("Zydis (MIT)");
                ImGui::BulletText("asmjit (Zlib)");
                ImGui::BulletText("asmtk (Zlib)");
                ImGui::BulletText("nlohmann/json (MIT)");
                ImGui::BulletText("Font Awesome 6 Free (SIL OFL 1.1 / CC BY 4.0)");
                ImGui::BulletText("basis33 font (MIT)");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace ui
