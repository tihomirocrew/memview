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

// ImGui's multi-viewport backend registers its window class with a solid
// background brush, so Windows erases each newly exposed strip during an
// interactive resize before our Present() draws over it, a flicker on every
// drag step. The backend is a vcpkg dependency we can't edit, so subclass each
// viewport window as ImGui creates it and swallow WM_ERASEBKGND; our own frame
// repaints the whole client area anyway. (The main window has no brush, hence
// no flicker.)
WNDPROC g_origViewportWndProc = nullptr;
void (*g_origPlatformCreateWindow)(ImGuiViewport*) = nullptr;

LRESULT CALLBACK viewportWndProcNoErase(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ERASEBKGND) return 1; // report handled, skip the erase
    return CallWindowProcW(g_origViewportWndProc, hwnd, msg, wParam, lParam);
}

// ImGui creates a new viewport's OS window and shows it on the same frame it
// first appears; DWM's first composite of that brand-new window can land at the
// desktop origin before it catches up to the position set moments earlier -- the
// 0,0 flash seen on every open of the Memory View / Settings windows. Position
// is never actually wrong (the window's rect is correct the whole time), so this
// is purely a composition-timing artifact. We DWM-cloak the window as it's
// created: cloaking hides it from composition while leaving it "visible" to the
// swapchain (so Present still fills it), and we uncloak it later in the same
// frame -- once it's been positioned and drawn -- so its first real composite is
// already in place.
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

// Uncloak windows cloaked while created this frame, now that they've been
// positioned and presented. Call once per frame after RenderPlatformWindowsDefault().
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
    // Lets an elevated instance attach to SYSTEM-owned processes; no-op otherwise.
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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // dockable Memory View panels
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // allow OS-level child windows
    // Detached windows keep the native OS title bar.
    io.ConfigViewportsNoDecoration = false;
    io.IniFilename = nullptr;

    // ProggyClean (ImGui's default) has no Cyrillic glyphs. basis33 is a
    // Cyrillic-extended fork, embedded and fixed-width, so it also keeps the Hex
    // and Disassembly panes column-aligned.
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false; // bytes are constexpr, atlas must not free them
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<uint8_t*>(fonts::basis33::data.data()),
        static_cast<int>(fonts::basis33::data.size()), 16.0f, &fontCfg,
        io.Fonts->GetGlyphRangesCyrillic());

    // Font Awesome 6, merged onto the same font so icons and text mix in one
    // ImGui::Text()/Button() call without a separate PushFont().
    ImFontConfig iconCfg;
    iconCfg.FontDataOwnedByAtlas = false; // bytes are constexpr, atlas must not free them
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    iconCfg.GlyphMinAdvanceX = 16.0f; // keep icons monospace-ish alongside basis33
    // FA6 sits ~3px above basis33's baseline when merged, so nudge icons down.
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

    // Hook viewport creation to kill the resize flicker (see above).
    g_origPlatformCreateWindow = ImGui::GetPlatformIO().Platform_CreateWindow;
    ImGui::GetPlatformIO().Platform_CreateWindow = createViewportWindowNoErase;

    app::App app;
    app.setupStyle();

    // Also driven by Window's WM_TIMER so the app keeps rendering while a window
    // is being interactively resized/moved (the outer loop is blocked then).
    bool inRenderFrame = false;
    auto renderFrame = [&]()
    {
        // Reentrancy guard: some Win32 APIs pump the message queue while they
        // run, which can dispatch WM_TIMER and re-enter mid-frame; ImGui asserts
        // on a second NewFrame(). Skip the nested call; the outer frame finishes.
        if (inRenderFrame) return;
        inRenderFrame = true;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.drawFrame();

        ImGui::Render();
        // Clear to the theme's window background so the slivers around the root
        // window don't flash a mismatched color.
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = { bg.x, bg.y, bg.z, 1.f };
        d3d.bindAndClear(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Render windows that were dragged out into their own OS windows.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            // Reveal windows once they've been positioned + drawn this frame, so
            // their first composite is already at the right spot (see above).
            uncloakCreatedViewports();
        }

        d3d.present(true);

        inRenderFrame = false;
    };
    window.onRender(renderFrame);

    // The main window has the same first-appearance flash as the detached ones:
    // DWM can composite it at the desktop origin before it settles at its
    // centered spot. Cloak it across the initial render + show, then uncloak so
    // its first real composite is already centered and holding finished UI.
    const BOOL cloakMain = TRUE;
    DwmSetWindowAttribute(window.hwnd(), DWMWA_CLOAK, &cloakMain, sizeof(cloakMain));

    // Render one frame while hidden, then show, so the first thing on screen is
    // the finished UI rather than an uninitialized backbuffer.
    renderFrame();
    window.show();

    const BOOL uncloakMain = FALSE;
    DwmSetWindowAttribute(window.hwnd(), DWMWA_CLOAK, &uncloakMain, sizeof(uncloakMain));

    while (window.pumpMessages())
        renderFrame();

    // Let any in-flight scan finish before tearing down the process handle.
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
