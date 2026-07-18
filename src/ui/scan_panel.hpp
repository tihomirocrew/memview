#pragma once
#include "app/state.hpp"

// Value-scanner UI of the main window: the results table on the left and the
// scan controls (value input, scan type, First/Next Scan) on the right.
namespace ui {

void drawScanResults(app::AppState& s);
void drawScanControls(app::AppState& s);

} // namespace ui
