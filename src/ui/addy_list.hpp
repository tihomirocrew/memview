#pragma once
#include "app/state.hpp"

// Saved-address list at the bottom of the main window, plus the
// "Add Address Manually" modal.
namespace ui {

void drawAddyList(app::AppState& s);
void drawAddAddressModal(app::AppState& s);
void drawClearAddyConfirm(app::AppState& s);

} // namespace ui
