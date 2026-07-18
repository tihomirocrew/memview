#pragma once
#include "app/state.hpp"

// Top toolbar of the main window (attach button, process name, settings gear)
// and the Settings window it opens.
namespace ui {

void drawToolbar(app::AppState& s);
void drawSettings(app::AppState& s);

} // namespace ui
