#pragma once
#include "app/state.hpp"

struct ImGuiViewport;

namespace app {

// Owns the shared state and draws one UI frame.
class App {
public:
    // Apply the light theme + style tweaks (once, after context init).
    void setupStyle();

    // Build the UI for the current frame.
    void drawFrame();

    AppState& state() { return state_; }

private:
    AppState state_;
};

// --- Actions shared between UI panels (implemented in app.cpp) --------------

// Switch between the light/dark color scheme, reapplying setupStyle()'s tweaks.
void applyTheme(AppState& s, bool dark);

// Disassembly syntax colors (packed IM_COL32). Kept small; punctuation stays
// in the theme text color so the listing doesn't turn into a christmas tree.
struct DisasmPalette {
    uint32_t mnemonic; // regular mnemonics + prefixes (rep, lock, ...)
    uint32_t reg;      // registers
    uint32_t num;      // immediates, displacements and raw addresses
    uint32_t sym;      // resolved "module+offset" symbols (label color)
    uint32_t call;     // call/ret mnemonics
    uint32_t jump;     // jmp/jcc mnemonics
};

// Palette for the active theme: x64dbg Dark on dark, the same hues darkened on
// light (derived from s.darkTheme).
const DisasmPalette& disasmPalette(const AppState& s);

// Parse the scan inputs and kick off a first/next scan.
void startFirstScan(AppState& s);
void startNextScan(AppState& s);

// Attach to a process: open a handle, update the label, reset the scan. Returns
// false on failure, with s.attachError set for the picker to show.
bool attachToProcess(AppState& s, const mem::ProcessEntry& entry);

// Call once per frame: notices the target exiting and tears the session down.
// Rate-limited, so it costs a wait syscall a few times a second.
void pollProcessAlive(AppState& s);

// Stop the scans, close the handle, drop everything read from the dead address
// space. The addy list and scan results are left alone.
void onProcessExited(AppState& s);

// Copy a scan result (by display index) into the addy list.
void addAddyFromResult(AppState& s, int displayIndex);

// Copy every selected scan result into the addy list.
void addSelectedResultsToAddy(AppState& s);

// Open Memory View at the target's main module, picking x86/x64 from bitness.
void openMemoryView(AppState& s);

// Open Memory View and jump both panes to `address`.
void openMemoryViewAt(AppState& s, uintptr_t address);

// Add an addy-list entry for a raw address (used by the disasm context menu).
void addAddyAddress(AppState& s, uintptr_t address);

// The module `addr` falls inside, or nullptr (heap, stack, private mappings).
const mem::ModuleEntry* findModule(const AppState& s, uintptr_t addr);

// Name of the section of `mod` containing `addr` (".text", ".rdata", ...), or
// nullptr for addresses before the first section (the PE header region). Parses
// and caches the module's section headers on first use.
const char* findSectionName(const AppState& s, const mem::ModuleEntry& mod, uintptr_t addr);

// Label `addr` as "module+offset" inside a loaded module, else raw hex.
void formatAddrLabel(const AppState& s, uintptr_t addr, char* out, size_t n);

// Parse a "Go to"/"Add Address" expression: plain hex, a module name (its base),
// or "module+offset" with a hex offset, e.g. "notepad.exe+1a2b". Module names
// match case-insensitively. Returns false if nothing resolves.
bool parseAddrExpr(const AppState& s, const char* text, uintptr_t& out);

// Like ImGui::InputText, plus a module-name autocomplete dropdown (case-insensitive
// prefix; Tab/click to accept, Up/Down to choose). `flags` are ImGuiInputTextFlags.
// Optional outs: `deactivatedAfterEdit` mirrors IsItemDeactivatedAfterEdit() for the
// field; `accepted` is set the frame a suggestion is inserted into `buf`.
bool addrInput(AppState& s, const char* id, char* buf, size_t bufSize,
    int flags, bool* deactivatedAfterEdit = nullptr, bool* accepted = nullptr);

// Assemble s.asmInput at s.asmAddress and write it into the target. Closes the
// modal on success; leaves it open with s.asmError on failure.
bool assembleAndWrite(AppState& s);

// Resolve the NOP-pad prompt: write the bytes stashed by assembleAndWrite. If
// padWithNops, pad the tail up to s.asmNopCoverLen. Closes the modals.
bool commitAsmBytes(AppState& s, bool padWithNops);

// Parse the hex bytes in s.opcodeInput and write them at s.opcodeAddress
// (whitespace-tolerant). Short edits pad the tail with NOPs up to s.opcodeOrigLen.
// Closes the modal on success; leaves it open with s.opcodeError on failure.
bool changeOpcodeAndWrite(AppState& s);

// Overwrite `length` bytes at `address` with single-byte 0x90 NOPs.
void nopFill(AppState& s, uintptr_t address, size_t length);

// (Re)generate the signature for s.sigAddress in style s.sigStyle, filling
// s.sigOutput/s.sigUnique. Called when the modal opens or its style changes.
void generateSignature(AppState& s);

// A modal that blocks only its own viewport, used like BeginPopupModal/EndPopup.
// The real thing freezes every window in every OS viewport, so a dialog in the
// main window also locks up a torn-out Memory View. `vp` is the viewport to
// block and center on; width/height <= 0 auto-size that axis.
bool beginBlockingModal(const char* title, bool* p_open,
                        const ImGuiViewport* vp, float width, float height);

} // namespace app
