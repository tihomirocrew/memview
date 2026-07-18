#pragma once
#include <windows.h>
#include <functional>

namespace app {

// Thin Win32 window wrapper. Forwards WM_SIZE to a resize callback and lets
// Dear ImGui's Win32 backend see raw messages.
class Window {
public:
    using ResizeCallback = std::function<void(UINT width, UINT height)>;
    using RenderCallback = std::function<void()>;

    bool create(const wchar_t* title, int width, int height);
    void show();
    void destroy();

    // Drains all pending messages. Returns false once WM_QUIT is received.
    bool pumpMessages();

    void onResize(ResizeCallback cb) { resizeCb_ = std::move(cb); }

    // Called on a background WM_TIMER tick. Windows runs its own modal message
    // loop while a window is interactively resized/moved, so pumpMessages()
    // stalls until the drag ends. Timers still fire through that loop, so
    // rendering from here keeps the app live during the drag.
    void onRender(RenderCallback cb) { renderCb_ = std::move(cb); }

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND         hwnd_  = nullptr;
    HINSTANCE    hinst_ = nullptr;
    WNDCLASSEXW  wc_    = {};
    ResizeCallback resizeCb_;
    RenderCallback renderCb_;
};

} // namespace app
