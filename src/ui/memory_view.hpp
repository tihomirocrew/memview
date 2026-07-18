#pragma once
#include "app/state.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// Memory View: a separate OS window hosting four dockable panes (Disassembly,
// Hex View, Memory Regions, Modules) that can be dragged apart or torn out. This
// file owns the container window, the shared Go/Back bar and the modals.
namespace ui {

// Entry point, called once per frame.
void drawMemoryView(app::AppState& s);

// The four panes (each in its own dockable window).
void drawDisasm(app::AppState& s);
void drawHex(app::AppState& s);
void drawRegions(app::AppState& s);
void drawModules(app::AppState& s);

// Modals opened from the Disassembly context menu (all in disasm.cpp).
void drawAssembleModal(app::AppState& s);      // assemble text -> bytes
void drawChangeOpcodeModal(app::AppState& s);  // write raw hex bytes
void drawAsmNopConfirm(app::AppState& s);      // NOP-pad prompt for short writes
void drawSignatureModal(app::AppState& s);     // show an AOB pattern
void drawFindSignatureModal(app::AppState& s); // find a pasted AOB pattern

// --- Shared bits used by the panes ------------------------------------------

constexpr float kLinesPerNotch = 3.f;    // scroll speed
constexpr uintptr_t kAddrFloor = 0x1000; // never page below the first page
constexpr int kAddrLabelWidth = 24;      // padding width for the address/label column

// Mouse "back" side button (XButton1). ImGui names indices 0-2; 3/4 are the
// raw extra-button slots.
constexpr int kMouseButtonBack = 3;
constexpr size_t kMaxHistory = 128;

// Push `addr` onto a pane's Back history, capped at kMaxHistory.
inline void pushHistory(std::vector<uintptr_t>& history, uintptr_t addr)
{
    history.push_back(addr);
    if (history.size() > kMaxHistory)
        history.erase(history.begin());
}

// Back: pop the last address and restore it into `addr`.
inline bool goBack(std::vector<uintptr_t>& history, uintptr_t& addr)
{
    if (history.empty()) return false;
    addr = history.back();
    history.pop_back();
    return true;
}

} // namespace ui
