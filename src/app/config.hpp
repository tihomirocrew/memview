#pragma once
#include "app/state.hpp"

namespace app {

void loadConfig(AppState& s);

void saveConfig(const AppState& s);

} // namespace app
