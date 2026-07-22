#include <windows.h>
#include <dwmapi.h>
#include <vector>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "assets/fonts/basis33/basis33.hpp"
#include "assets/fonts/fontawesome6/fa_solid_900.hpp"
#include "assets/fonts/fontawesome6/fa_brands_400.hpp"
#include "assets/fonts/fontawesome6/IconsFontAwesome6.h"
#include "assets/fonts/fontawesome6/IconsFontAwesome6Brands.h"
#include "app/window/window.hpp"
#include "app/window/render.hpp"
#include "app/app.hpp"
#include "ui/process_picker.hpp"

namespace {

// Fixes flicker when resizing detached windows: their window class has a
// background brush, so Windows erases each strip before we redraw it.
WNDPROC g_origViewportWndProc = nullptr;
void (*g_origPlatformCreateWindow)(ImGuiViewport*) = nullptr;

LRESULT CALLBACK viewportWndProcNoErase(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ERASEBKGND) return 1; // claim it's handled so Windows skips the erase
    return CallWindowProcW(g_origViewportWndProc, hwnd, msg, wParam, lParam);
}

// Fixes the corner flash when a window opens: DWM composites it once before it
// picks up the position we set, so we cloak it until it's placed and drawn.
std::vector<HWND> g_cloaked; // cloaked on create this frame, uncloaked after present

void createViewportWindowNoErase(ImGuiViewport* viewport)
{
    g_origPlatformCreateWindow(viewport);
    if (HWND hwnd = (HWND)viewport->PlatformHandle)
    {
        if (!g_origViewportWndProc)
            g_origViewportWndProc = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)viewportWndProcNoErase);

        const BOOL cloak = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        g_cloaked.push_back(hwnd);
    }
}

// Reveals the windows cloaked above. Call once per frame, right after
// RenderPlatformWindowsDefault(), when they're placed and drawn.
void uncloakCreatedViewports()
{
    const BOOL uncloak = FALSE;
    for (HWND h : g_cloaked)
        if (::IsWindow(h))
            DwmSetWindowAttribute(h, DWMWA_CLOAK, &uncloak, sizeof(uncloak));
    g_cloaked.clear();
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // If we're running elevated, this lets us attach to SYSTEM processes too.
    mem::enable_debug_privilege();

    app::Window window;
    if (!window.create(L"MemView", 785, 642))
        return 1;

    render::D3D11Context d3d;
    if (!d3d.create(window.hwnd()))
    {
        d3d.cleanup();
        window.destroy();
        return 1;
    }

    window.onResize([&](UINT w, UINT h) { d3d.resize(w, h); });

    ui::icons::init(d3d.device());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // Memory View panels can be docked
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // panels can be dragged out into real windows
    // Keep the normal Windows title bar on dragged-out windows.
    io.ConfigViewportsNoDecoration = false;
    io.IniFilename = nullptr;

    // ImGui's default font has no Cyrillic; basis33 adds it, and being monospace
    // it also keeps the Hex and Disassembly panes lined up.
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false; // bytes are constexpr, the atlas must not free them
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<uint8_t*>(fonts::basis33::data.data()),
        static_cast<int>(fonts::basis33::data.size()), 16.0f, &fontCfg,
        io.Fonts->GetGlyphRangesCyrillic());

    // Font Awesome 6, merged into the same font so icons and text mix in one
    // ImGui::Text()/Button() call, no PushFont() needed.
    ImFontConfig iconCfg;
    iconCfg.FontDataOwnedByAtlas = false; // bytes are constexpr, the atlas must not free them
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    iconCfg.GlyphMinAdvanceX = 16.0f; // fixed icon width, like basis33
    // Merged icons sit ~3px too high next to basis33, so push them down.
    iconCfg.GlyphOffset = ImVec2(0.0f, 3.0f);

    static constexpr ImWchar kFaSolidRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<uint8_t*>(fonts::fa_solid_900::data.data()),
        static_cast<int>(fonts::fa_solid_900::data.size()), 16.0f, &iconCfg, kFaSolidRanges);

    static constexpr ImWchar kFaBrandsRanges[] = { ICON_MIN_FAB, ICON_MAX_FAB, 0 };
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<uint8_t*>(fonts::fa_brands_400::data.data()),
        static_cast<int>(fonts::fa_brands_400::data.size()), 16.0f, &iconCfg, kFaBrandsRanges);

    ImGui_ImplWin32_Init(window.hwnd());
    ImGui_ImplDX11_Init(d3d.device(), d3d.context());

    // Hook viewport creation to fix the resize flicker (see above).
    g_origPlatformCreateWindow = ImGui::GetPlatformIO().Platform_CreateWindow;
    ImGui::GetPlatformIO().Platform_CreateWindow = createViewportWindowNoErase;

    app::App app;
    app.setupStyle();

    // Window also calls this from its WM_TIMER, so we keep drawing while a window
    // is being dragged or resized (the loop at the bottom is blocked then).
    bool inRenderFrame = false;
    auto renderFrame = [&]()
    {
        // That timer can fire mid-frame and re-enter, since some Win32 calls pump
        // messages. ImGui asserts on a second NewFrame(), so drop the nested call.
        if (inRenderFrame) return;
        inRenderFrame = true;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.drawFrame();

        ImGui::Render();
        // Clear to the theme background so gaps around the root window don't
        // flash a different color.
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = { bg.x, bg.y, bg.z, 1.f };
        d3d.bindAndClear(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Draw the panels that were dragged out into separate windows.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            // They're placed and drawn now, so it's safe to show them (see above).
            uncloakCreatedViewports();
        }

        d3d.present(true);

        inRenderFrame = false;
    };
    window.onRender(renderFrame);

    // Same corner flash on startup, so keep the main window cloaked until it's
    // centered and holding a finished frame.
    const BOOL cloakMain = TRUE;
    DwmSetWindowAttribute(window.hwnd(), DWMWA_CLOAK, &cloakMain, sizeof(cloakMain));

    // Draw one frame while hidden, so the first thing on screen is the finished
    // UI and not an empty backbuffer.
    renderFrame();
    window.show();

    const BOOL uncloakMain = FALSE;
    DwmSetWindowAttribute(window.hwnd(), DWMWA_CLOAK, &uncloakMain, sizeof(uncloakMain));

    while (window.pumpMessages())
        renderFrame();

    // Wait for any running scan to stop before we close the process handle.
    app.state().scan.waitIdle();
    app.state().findSigScan.waitIdle();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ui::icons::shutdown();
    d3d.cleanup();
    window.destroy();
    return 0;
}
