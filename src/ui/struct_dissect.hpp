#pragma once

namespace app { struct AppState; }

// Structure Dissector: lays a structure over an address, follows the pointers
// from there, and leans on RTTI for the names.
namespace ui {

void drawStructDissect(app::AppState& s);

} // namespace ui
