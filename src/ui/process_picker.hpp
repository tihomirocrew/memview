#pragma once
#include "app/state.hpp"
#include <d3d11.h>
#include <string>

// Modal process list for attaching to a target (opened from the toolbar).
namespace ui {

void drawProcessPicker(app::AppState& s);

} // namespace ui

// Cache of process-icon textures, keyed by exe path. Each SRV is created on
// first request and reused on later lookups.
namespace ui::icons {

// Call once after the D3D11 device is created.
void init(ID3D11Device* device);

// Icon texture for `exePath`, extracted on first request. nullptr if the path
// is empty or the icon couldn't be extracted.
ID3D11ShaderResourceView* get(const std::string& exePath);

// Releases all cached textures. Call before destroying the D3D11 device.
void shutdown();

} // namespace ui::icons
