#include "app/window/render.hpp"

namespace render {

bool D3D11Context::create(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &swap_, &device_, &level, &context_);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &swap_, &device_, &level, &context_);
    if (hr != S_OK) return false;

    createRenderTarget();
    return true;
}

void D3D11Context::createRenderTarget()
{
    ID3D11Texture2D* bb = nullptr;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    device_->CreateRenderTargetView(bb, nullptr, &rtv_);
    bb->Release();
}

void D3D11Context::cleanupRenderTarget()
{
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void D3D11Context::resize(UINT width, UINT height)
{
    if (!device_) return;
    cleanupRenderTarget();
    swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

void D3D11Context::bindAndClear(const float clearColor[4])
{
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clearColor);
}

void D3D11Context::present(bool vsync)
{
    swap_->Present(vsync ? 1 : 0, 0);
}

void D3D11Context::cleanup()
{
    cleanupRenderTarget();
    if (swap_)    { swap_->Release();    swap_    = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_)  { device_->Release();  device_  = nullptr; }
}

} // namespace render
