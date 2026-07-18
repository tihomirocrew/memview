#pragma once
#include <windows.h>
#include <d3d11.h>

namespace render {

// Owns the D3D11 device, swap chain and back-buffer render target view.
// One instance per window.
class D3D11Context {
public:
    bool create(HWND hwnd);
    void cleanup();

    // Resize the swap-chain buffers to the new client size.
    void resize(UINT width, UINT height);

    // Bind the back buffer and clear it to `clearColor` (RGBA).
    void bindAndClear(const float clearColor[4]);

    void present(bool vsync);

    bool valid() const { return device_ != nullptr; }

    ID3D11Device*        device()  const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }

private:
    void createRenderTarget();
    void cleanupRenderTarget();

    ID3D11Device*           device_  = nullptr;
    ID3D11DeviceContext*    context_ = nullptr;
    IDXGISwapChain*         swap_    = nullptr;
    ID3D11RenderTargetView* rtv_     = nullptr;
};

} // namespace render
