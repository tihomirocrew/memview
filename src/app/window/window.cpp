#include "app/window/window.hpp"
#include <imgui.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace app {

// Single-window app, so a static pointer reaches the instance from WndProc.
static Window* s_instance = nullptr;

// Drives onRender() during an interactive resize/move (see onRender()). ~60Hz;
// doesn't need to be exact.
constexpr UINT_PTR kRenderTimerId = 1;
constexpr UINT     kRenderTimerMs = 16;

LRESULT WINAPI Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && s_instance && s_instance->resizeCb_)
            s_instance->resizeCb_(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == kRenderTimerId && s_instance && s_instance->renderCb_)
            s_instance->renderCb_();
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Window::create(const wchar_t* title, int width, int height)
{
    s_instance = this;
    hinst_ = GetModuleHandleW(nullptr);

    wc_.cbSize        = sizeof(wc_);
    wc_.style         = CS_CLASSDC;
    wc_.lpfnWndProc   = WndProc;
    wc_.hInstance     = hinst_;
    wc_.lpszClassName = L"mem-view";
    wc_.hIcon   = LoadIconW(hinst_, MAKEINTRESOURCEW(1));
    wc_.hIconSm = (HICON)LoadImageW(hinst_, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    RegisterClassExW(&wc_);

    // Center on the primary monitor's work area (excluding the taskbar).
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
    {
        const int workW = workArea.right - workArea.left;
        const int workH = workArea.bottom - workArea.top;
        x = workArea.left + (workW - width) / 2;
        y = workArea.top + (workH - height) / 2;
    }

    hwnd_ = CreateWindowW(wc_.lpszClassName, title,
        WS_OVERLAPPEDWINDOW, x, y, width, height,
        nullptr, nullptr, hinst_, nullptr);

    if (hwnd_) SetTimer(hwnd_, kRenderTimerId, kRenderTimerMs, nullptr);

    return hwnd_ != nullptr;
}

void Window::show()
{
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
}

bool Window::pumpMessages()
{
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT)
            return false;
    }
    return true;
}

void Window::destroy()
{
    if (hwnd_) { KillTimer(hwnd_, kRenderTimerId); DestroyWindow(hwnd_); hwnd_ = nullptr; }
    UnregisterClassW(wc_.lpszClassName, hinst_);
    s_instance = nullptr;
}

} // namespace app
