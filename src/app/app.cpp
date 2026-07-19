#include "app/app.hpp"
#include "app/config.hpp"
#include "ui/toolbar.hpp"
#include "ui/process_picker.hpp"
#include "ui/scan_panel.hpp"
#include "ui/addy_list.hpp"
#include "ui/memory_view.hpp"
#include "memory/value_format.hpp"
#include "memory/assembler.hpp"
#include "memory/signature.hpp"
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder* for the root dockspace layout
#include <Zydis/Zydis.h>    // decode overwritten instructions for NOP-pad sizing
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace app {

void App::setupStyle()
{
    // Restore persisted settings (theme) before the first style application.
    loadConfig(state_);
    applyTheme(state_, state_.darkTheme);

    ImGuiStyle& style       = ImGui::GetStyle();
    style.WindowRounding    = 0.f;
    style.FrameRounding     = 3.f;
    style.ScrollbarRounding = 3.f;
    style.GrabRounding      = 3.f;
    style.FramePadding      = ImVec2(5, 3);
    style.ItemSpacing       = ImVec2(6, 4);
    style.WindowPadding     = ImVec2(6, 6);
}

// Custom dark palette (used instead of ImGui::StyleColorsDark()): graphite
// blue-tinted backgrounds with a neutral grey accent.
static void styleColorsDarkCustom()
{
    ImVec4* c = ImGui::GetStyle().Colors;

    const ImVec4 text        (0.86f, 0.88f, 0.90f, 1.00f);
    const ImVec4 textDim     (0.46f, 0.49f, 0.53f, 1.00f);
    const ImVec4 bgWindow    (0.09f, 0.10f, 0.12f, 1.00f);
    const ImVec4 bgPopup     (0.11f, 0.12f, 0.14f, 0.98f);
    const ImVec4 bgTitle     (0.07f, 0.08f, 0.09f, 1.00f);
    const ImVec4 frame       (0.15f, 0.17f, 0.20f, 1.00f);
    const ImVec4 frameHover  (0.20f, 0.22f, 0.26f, 1.00f);
    const ImVec4 frameActive (0.25f, 0.27f, 0.32f, 1.00f);
    const ImVec4 border      (0.22f, 0.24f, 0.28f, 0.60f);
    const ImVec4 accent      (0.42f, 0.44f, 0.48f, 1.00f);
    const ImVec4 accentHi    (0.58f, 0.60f, 0.65f, 1.00f);

    // accent with a custom alpha (for selections/overlays)
    const auto accentA = [&](float a) { return ImVec4(accent.x, accent.y, accent.z, a); };

    c[ImGuiCol_Text]                      = text;
    c[ImGuiCol_TextDisabled]              = textDim;
    c[ImGuiCol_WindowBg]                  = bgWindow;
    c[ImGuiCol_ChildBg]                   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]                   = bgPopup;
    c[ImGuiCol_Border]                    = border;
    c[ImGuiCol_BorderShadow]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]                   = frame;
    c[ImGuiCol_FrameBgHovered]            = frameHover;
    c[ImGuiCol_FrameBgActive]             = frameActive;
    c[ImGuiCol_TitleBg]                   = bgTitle;
    c[ImGuiCol_TitleBgActive]             = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]          = ImVec4(bgTitle.x, bgTitle.y, bgTitle.z, 0.75f);
    c[ImGuiCol_MenuBarBg]                 = bgTitle;
    c[ImGuiCol_ScrollbarBg]               = ImVec4(0.07f, 0.08f, 0.09f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]             = frame;
    c[ImGuiCol_ScrollbarGrabHovered]      = frameHover;
    c[ImGuiCol_ScrollbarGrabActive]       = frameActive;
    c[ImGuiCol_CheckMark]                 = accentHi;
    c[ImGuiCol_CheckboxSelectedBg]        = frame;
    c[ImGuiCol_SliderGrab]                = accent;
    c[ImGuiCol_SliderGrabActive]          = accentHi;
    c[ImGuiCol_Button]                    = frame;
    c[ImGuiCol_ButtonHovered]             = frameHover;
    c[ImGuiCol_ButtonActive]              = frameActive;
    c[ImGuiCol_Header]                    = accentA(0.35f);
    c[ImGuiCol_HeaderHovered]             = accentA(0.50f);
    c[ImGuiCol_HeaderActive]              = accentA(0.65f);
    c[ImGuiCol_Separator]                 = border;
    c[ImGuiCol_SeparatorHovered]          = accentA(0.60f);
    c[ImGuiCol_SeparatorActive]           = accentHi;
    c[ImGuiCol_ResizeGrip]                = accentA(0.20f);
    c[ImGuiCol_ResizeGripHovered]         = accentA(0.55f);
    c[ImGuiCol_ResizeGripActive]          = accentHi;
    c[ImGuiCol_InputTextCursor]           = text;
    c[ImGuiCol_TabHovered]                = frameHover;
    c[ImGuiCol_Tab]                       = bgTitle;
    c[ImGuiCol_TabSelected]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]       = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmed]                 = ImVec4(0.06f, 0.07f, 0.08f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview]            = accentA(0.55f);
    c[ImGuiCol_DockingEmptyBg]            = ImVec4(0.07f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_PlotLines]                 = ImVec4(0.61f, 0.61f, 0.64f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]          = accentHi;
    c[ImGuiCol_PlotHistogram]             = accent;
    c[ImGuiCol_PlotHistogramHovered]      = accentHi;
    c[ImGuiCol_TableHeaderBg]             = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TableBorderStrong]         = ImVec4(0.24f, 0.26f, 0.31f, 1.00f);
    c[ImGuiCol_TableBorderLight]          = ImVec4(0.18f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TableRowBg]                = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]             = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextLink]                  = accentHi;
    c[ImGuiCol_TextSelectedBg]            = accentA(0.30f);
    c[ImGuiCol_TreeLines]                 = border;
    c[ImGuiCol_DragDropTarget]            = accentHi;
    c[ImGuiCol_DragDropTargetBg]          = accentA(0.20f);
    c[ImGuiCol_UnsavedMarker]             = text;
    c[ImGuiCol_NavCursor]                 = accentHi;
    c[ImGuiCol_NavWindowingHighlight]     = ImVec4(1, 1, 1, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]         = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]          = ImVec4(0, 0, 0, 0.50f);
}

bool beginBlockingModal(const char* title, bool* p_open,
                        const ImGuiViewport* vp, float width, float height)
{
    // Full-viewport blocker: shades the viewport and swallows any click meant for
    // the windows behind. NoBringToFrontOnFocus keeps it below the dialog.
    char blockerId[192];
    snprintf(blockerId, sizeof(blockerId), "##blocker_%s", title);

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const ImGuiWindowFlags blockerFlags =
        ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize        | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking       | ImGuiWindowFlags_NoNavInputs |
        ImGuiWindowFlags_NoNavFocus      | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin(blockerId, nullptr, blockerFlags);

    ImGui::GetWindowDrawList()->AddRectFilled(
    vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
    IM_COL32(20, 20, 20, 110));

    ImGuiWindow* blockerWin = ImGui::GetCurrentWindow();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // The dialog, centered on the viewport.
    if (width > 0.f || height > 0.f)
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags dialogFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse;
    const bool visible = ImGui::Begin(title, p_open, dialogFlags);
    ImGuiWindow* dialogWin = ImGui::GetCurrentWindow();

    // Raise the pair to the top once, on open. It stays there: the blocker can't
    // rise, and the windows under it never get focus to jump in front.
    if (ImGui::IsWindowAppearing())
    {
        ImGui::BringWindowToDisplayFront(blockerWin);
        ImGui::BringWindowToDisplayFront(dialogWin);
        ImGui::FocusWindow(dialogWin);
    }

    if (!visible)
    {
        ImGui::End();
        return false;
    }
    return true;
}

void applyTheme(AppState& s, bool dark)
{
    s.darkTheme = dark;
    dark ? styleColorsDarkCustom() : ImGui::StyleColorsLight();

    ImGuiStyle& style = ImGui::GetStyle();

    // Detached windows are real OS windows: keep them opaque.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    // Disable the built-in modal dim (it darkens every viewport); we draw our
    // own over just the main window in drawFrame().
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0);
}

// Disassembly syntax colors. Dark is x64dbg's bundled Dark theme verbatim; light
// keeps the same hues darkened, since the pastels wash out on white.
const DisasmPalette& disasmPalette(const AppState& s)
{
    static const DisasmPalette dark = {
        IM_COL32(0x89, 0xA2, 0xF6, 0xFF), // mnemonic - periwinkle (InstructionMnemonic/Prefix)
        IM_COL32(0xB7, 0x94, 0xF6, 0xFF), // reg      - purple (InstructionGeneralRegister)
        IM_COL32(0xF8, 0x84, 0x78, 0xFF), // num      - salmon (InstructionValue/Address)
        IM_COL32(0xB7, 0x94, 0xF6, 0xFF), // sym      - purple (DisassemblyLabel)
        IM_COL32(0xF5, 0x5F, 0x86, 0xFF), // call/ret - pink (InstructionCall/Ret)
        IM_COL32(0xF5, 0x5F, 0x86, 0xFF), // jump     - pink (Instruction*Jump: same as call)
    };
    static const DisasmPalette light = {
        IM_COL32(0x3B, 0x5B, 0xDB, 0xFF), // mnemonic - indigo
        IM_COL32(0x5F, 0x3D, 0xC4, 0xFF), // reg      - violet
        IM_COL32(0xC9, 0x2A, 0x2A, 0xFF), // num      - red
        IM_COL32(0x5F, 0x3D, 0xC4, 0xFF), // sym      - violet (matches reg, like x64dbg)
        IM_COL32(0xC2, 0x25, 0x5C, 0xFF), // call/ret - crimson pink
        IM_COL32(0xC2, 0x25, 0x5C, 0xFF), // jump     - crimson pink (same as call)
    };
    return s.darkTheme ? dark : light;
}

void App::drawFrame()
{
    state_.scan.poll(state_.proc);

    // Pin the root window to the main viewport, so the UI stays inside the main
    // OS window rather than spawning its own.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize  |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar(3);

    // Root dockspace: hosts the permanent Scanner panel plus any floating
    // windows dragged in. AutoHideTabBar keeps a solo window chrome-less and
    // brings the tab bar back once a second window joins the node.
    const ImGuiID rootDockId = ImGui::GetID("RootDockSpace");
    static bool rootLayoutBuilt = false;
    if (!rootLayoutBuilt)
    {
        rootLayoutBuilt = true;
        ImGui::DockBuilderRemoveNode(rootDockId);
        ImGui::DockBuilderAddNode(rootDockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(rootDockId, vp->Size);
        ImGui::DockBuilderDockWindow("Scanner", rootDockId);
        ImGui::DockBuilderFinish(rootDockId);
    }
    ImGui::DockSpace(rootDockId, ImVec2(0, 0), ImGuiDockNodeFlags_AutoHideTabBar);
    ImGui::End();

    // Scanner panel: the permanent main content, never closable. It must stay
    // put in the root window, so NoUndocking blocks dragging it out,
    // NoDockingOverMe/NoDockingSplit stop anything merging into its node, and
    // NoTabBar drops the reveal-tab-bar handle (nothing can ever join it).
    ImGuiWindowClass scannerClass;
    scannerClass.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoDockingOverMe |
        ImGuiDockNodeFlags_NoDockingSplit | ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&scannerClass);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::Begin("Scanner", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();

    ui::drawToolbar(state_);

    const float totalH = ImGui::GetContentRegionAvail().y;
    const float topH   = totalH * 0.60f;
    const float ctrlW  = 190.f;

    ImGui::BeginChild("##results_pane", ImVec2(-ctrlW - 6, topH), false);
    ui::drawScanResults(state_);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##ctrl_pane", ImVec2(ctrlW, topH), false);
    ui::drawScanControls(state_);
    ImGui::EndChild();

    ui::drawAddyList(state_);

    ImGui::End();

    ui::drawProcessPicker(state_);
    ui::drawAddAddressModal(state_);
    ui::drawClearAddyConfirm(state_);

    // Floating windows (outside the root).
    ui::drawSettings(state_);
    ui::drawMemoryView(state_);

    // TEMP: ImGui demo window for testing.
    //ImGui::ShowDemoWindow();
}

// --- Actions ----------------------------------------------------------------

// Build the search needle for the current value type. For ArrayOfBytes, `mask`
// receives the per-byte match mask. Returns the byte length (0 = fail/empty).
static size_t buildNeedle(const AppState& s, mem::ValueType vt,
                          uint8_t* needle, uint8_t* mask, size_t cap)
{
    if (vt == mem::ValueType::ArrayOfBytes)
        return parseAob(s.scanValue, needle, mask, cap);

    // parseValue re-encodes to UTF-16LE itself, sharing one conversion with the
    // addy-list write path.
    const bool utf16 = (vt == mem::ValueType::String && s.stringEncoding == 1);
    size_t nlen = parseValue(s.scanValue, vt, needle, cap, utf16);

    // "Value between" packs the upper bound right after the first, so compare()
    // reads both from one buffer. Numeric only.
    if (nlen && uiScanType(s.scanType) == mem::ScanType::Between)
    {
        const size_t w = mem::value_size(vt);
        if (2 * w > cap) return 0;
        if (parseValue(s.scanValue2, vt, needle + w, cap - w) == 0) return 0;
        nlen = 2 * w;
    }
    return nlen;
}

void startFirstScan(AppState& s)
{
    if (!s.proc.is_open() || s.scan.running()) return;

    mem::ValueType vt = uiValueType(s.valueType);
    mem::ScanType  st = uiScanType(s.scanType);

    uint8_t needle[2048] = {};
    uint8_t mask[2048]   = {};
    size_t  nlen = 0;
    // Only "unknown initial value" needs no value on the first scan.
    if (st != mem::ScanType::UnknownInitial)
    {
        nlen = buildNeedle(s, vt, needle, mask, sizeof(needle));
        if (nlen == 0) return;
    }

    const uint8_t* maskPtr = (vt == mem::ValueType::ArrayOfBytes) ? mask : nullptr;

    s.resultSel.clear();
    s.selAnchor = -1;
    s.scan.firstScan(s.proc, st, vt, needle, nlen,
        static_cast<mem::TriState>(s.writableFilter),
        static_cast<mem::TriState>(s.executableFilter),
        s.stringCaseSensitive, s.stringEncoding == 1, maskPtr);
}

void startNextScan(AppState& s)
{
    if (!s.proc.is_open() || s.scan.running() || !s.scan.firstScanDone()) return;

    mem::ValueType vt = uiValueType(s.valueType);
    mem::ScanType  st = uiScanType(s.scanType);

    uint8_t needle[2048] = {};
    uint8_t mask[2048]   = {};
    size_t  nlen = 0;
    if (scanNeedsValue(st))
    {
        nlen = buildNeedle(s, vt, needle, mask, sizeof(needle));
        if (nlen == 0) return;
    }

    const uint8_t* maskPtr = (vt == mem::ValueType::ArrayOfBytes) ? mask : nullptr;

    s.resultSel.clear();
    s.selAnchor = -1;
    s.scan.nextScan(s.proc, st, vt, needle, nlen,
        s.stringCaseSensitive, s.stringEncoding == 1, maskPtr);
}

bool attachToProcess(AppState& s, const mem::ProcessEntry& entry)
{
    mem::close(s.proc);
    if (!mem::open(s.proc, entry.pid))
    {
        const DWORD err = GetLastError();
        snprintf(s.attachError, sizeof(s.attachError),
            "Failed to attach to %s (PID: %lu) - error %lu",
            entry.name.c_str(), entry.pid, (unsigned long)err);
        printf("OpenProcess failed for %s (PID: %lu): error %lu\n",
            entry.name.c_str(), entry.pid, (unsigned long)err);
        return false;
    }

    s.attachError[0] = '\0';
    snprintf(s.processLabel, sizeof(s.processLabel),
        "%s  (PID: %lu)", entry.name.c_str(), entry.pid);
    s.procIconPath = entry.path;
    s.resultSel.clear();
    s.selAnchor = -1;
    s.scan.reset();

    // Drop any in-flight Find Signature so a stale hit can't jump after re-attach.
    s.findSigScan.reset();
    s.findSigPending = false;
    s.showFindSig    = false;

    // Clear the Memory View position so the next open re-seeds from the new
    // process's main module.
    s.disasmAddr = 0;
    s.hexAddr    = 0;
    s.disasmHistory.clear();
    s.hexHistory.clear();
    if (s.showMemView) openMemoryView(s);
    return true;
}

void addAddyFromResult(AppState& s, int displayIndex)
{
    const auto& rows = s.scan.results();
    if (displayIndex < 0 || displayIndex >= (int)rows.size()) return;

    const auto& r = rows[displayIndex];

    // Don't add an address that's already tracked.
    for (const auto& w : s.addyList)
        if (w.address == r.address) return;

    AddyEntry e = {};
    snprintf(e.desc,  sizeof(e.desc),  "No description");
    e.value = r.value;
    e.address = r.address;
    e.typeIdx = s.valueType;             // inherit the scan's value type
    e.stringEncoding = s.stringEncoding; // ...and encoding, so writes match
    // Read strings at the needle length, not the default 8.
    mem::ValueType vt = uiValueType(s.valueType);
    if (mem::is_bytewise(vt))
    {
        size_t nlen = s.scan.lastNeedleLen();
        if (nlen > 0) e.length = (int)nlen;
    }
    s.addyList.push_back(e);
}

void addSelectedResultsToAddy(AppState& s)
{
    const auto& rows = s.scan.results();
    const int n = (int)rows.size();
    for (int i = 0; i < n && i < (int)s.resultSel.size(); ++i)
        if (s.resultSel[i])
            addAddyFromResult(s, i);
}

void openMemoryView(AppState& s)
{
    s.showMemView = true;
    if (!s.proc.is_open()) return;

    s.memViewArch = mem::is_wow64(s.proc) ? 1 : 0;

    // Keep the previous position on reopen; seed from the main module base only
    // on the first open (or after attaching, which clears the address).
    if (s.disasmAddr == 0)
    {
        uintptr_t base = mem::main_module_base(s.proc);
        if (base == 0)
        {
            // No module list (e.g. protected process): use the first exec region.
            for (const auto& r : mem::query_regions(s.proc))
                if (mem::is_executable(r.protect)) { base = r.base; break; }
        }

        s.disasmAddr = base;
        s.hexAddr    = base;
        snprintf(s.memGotoInput, sizeof(s.memGotoInput), "%llX",
            (unsigned long long)base);
    }

    s.memRegions = mem::query_regions(s.proc);
}

void openMemoryViewAt(AppState& s, uintptr_t address)
{
    s.showMemView = true;
    if (!s.proc.is_open()) return;

    s.memViewArch = mem::is_wow64(s.proc) ? 1 : 0;

    s.disasmAddr = address;
    s.hexAddr    = address;
    snprintf(s.memGotoInput, sizeof(s.memGotoInput), "%llX",
        (unsigned long long)address);

    s.memRegions = mem::query_regions(s.proc);
}

const mem::ModuleEntry* findModule(const AppState& s, uintptr_t addr)
{
    for (const mem::ModuleEntry& m : s.modules)
        if (addr >= m.base && addr < m.base + m.size)
            return &m;
    return nullptr;
}

void formatAddrLabel(const AppState& s, uintptr_t addr, char* out, size_t n)
{
    if (const mem::ModuleEntry* m = findModule(s, addr))
    {
        const uintptr_t off = addr - m->base;
        if (off)
            snprintf(out, n, "%s+%llX", m->name.c_str(), (unsigned long long)off);
        else
            snprintf(out, n, "%s", m->name.c_str());
        return;
    }
    snprintf(out, n, "%llX", (unsigned long long)addr);
}

namespace {

// True where a term ends: string end, whitespace, or a +/- operator.
bool isTermBoundary(char c)
{
    return c == '\0' || c == '+' || c == '-' || std::isspace((unsigned char)c);
}

// If a loaded module name matches at `p` (case-insensitive, ending on a term
// boundary), consume it and return its base. Matches the full name including
// any dashes, so "api-ms-win-...dll" isn't mistaken for a subtraction.
bool matchModuleAt(const AppState& s, const char* p, uintptr_t& base,
    const char*& next)
{
    for (const mem::ModuleEntry& m : s.modules)
    {
        const size_t len = m.name.size();
        if (len && _strnicmp(p, m.name.c_str(), len) == 0 &&
            isTermBoundary(p[len]))
        {
            base = m.base;
            next = p + len;
            return true;
        }
    }
    return false;
}

// ASCII lower-case copy, for case-insensitive symbol/module keys.
std::string toLowerStr(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A module's name without its final extension: "kernel32.dll" -> "kernel32".
std::string moduleBaseName(const std::string& name)
{
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// Parse and cache module `m`'s export table on first use; return the cache entry.
const AppState::ModuleExports& ensureExports(const AppState& s, const mem::ModuleEntry& m)
{
    std::string key = toLowerStr(m.name);
    auto it = s.exportCache.find(key);
    if (it != s.exportCache.end()) return it->second;

    AppState::ModuleExports me;
    for (const mem::ExportSym& e : mem::read_exports(s.proc, m))
    {
        me.byName.emplace(toLowerStr(e.name), e.addr);
        me.names.push_back(e.name);
    }
    std::sort(me.names.begin(), me.names.end());
    return s.exportCache.emplace(std::move(key), std::move(me)).first->second;
}

// Resolve "<module>.<export>" (module with or without its extension, e.g.
// "kernel32.CreateFileW") to the export's address; `next` gets the token's end.
bool matchSymbolAt(const AppState& s, const char* p, uintptr_t& addr, const char*& next)
{
    for (const mem::ModuleEntry& m : s.modules)
    {
        const std::string full = m.name;
        const std::string base = moduleBaseName(m.name);
        for (const std::string* mn : { &full, &base })
        {
            const size_t len = mn->size();
            if (len == 0 || _strnicmp(p, mn->c_str(), len) != 0 || p[len] != '.')
                continue;

            // The export name runs to the next term boundary. Names may hold '.',
            // '@', '?', '$' (decorated/mangled symbols), so only +/-/space end it.
            const char* symStart = p + len + 1;
            const char* symEnd   = symStart;
            while (!isTermBoundary(*symEnd)) ++symEnd;
            if (symEnd == symStart) continue; // "module." with no symbol

            const std::string sym = toLowerStr(std::string(symStart, symEnd - symStart));
            const AppState::ModuleExports& ex = ensureExports(s, m);
            auto it = ex.byName.find(sym);
            if (it != ex.byName.end())
            {
                addr = it->second;
                next = symEnd;
                return true;
            }
        }
    }
    return false;
}

} // namespace

// Evaluates an address expression: a sum/difference of terms, where each term
// is a loaded module name (resolved to its base), a "module.Export" symbol
// (resolved via the module's PE export table, e.g. "kernel32.CreateFileW"), or a
// hex number. So "module.dll+1A", "kernel32.CreateFileW+5", "40000+8-4",
// "module+10+20" and a bare "7FF6..." all parse.
bool parseAddrExpr(const AppState& s, const char* text, uintptr_t& out)
{
    const char* p = text;
    auto skipws = [&] { while (std::isspace((unsigned char)*p)) ++p; };

    uintptr_t acc  = 0;
    int       sign = 1;   // sign to apply to the next term
    bool      any  = false;

    skipws();
    for (;;)
    {
        // A binary operator joins terms; a leading operator is rejected (an
        // address expression must start with a term).
        if (*p == '+' || *p == '-')
        {
            if (!any) return false;
            if (*p == '-') sign = -sign;
            ++p;
            skipws();
        }
        else if (any)
        {
            break; // no more operators: expression is complete
        }

        uintptr_t term = 0;
        const char* next = nullptr;
        if (matchModuleAt(s, p, term, next))
        {
            p = next;
        }
        else if (matchSymbolAt(s, p, term, next))
        {
            p = next;
        }
        else
        {
            // A hex term must begin with a hex digit. This rejects strtoull's
            // own leading +/- (so "40000+-8" and "40000++8" don't slip through
            // as sign-wrapped values) and any other stray character.
            if (!std::isxdigit((unsigned char)*p)) return false;
            char* end = nullptr;
            errno = 0;
            term = (uintptr_t)strtoull(p, &end, 16);
            if (end == p || errno == ERANGE) return false; // not hex, or overflow
            p = end;
        }

        acc += (sign >= 0) ? term : (uintptr_t)0 - term;
        sign = 1;
        any  = true;
        skipws();

        if (*p == '\0') break;
        if (*p != '+' && *p != '-') return false; // trailing garbage
    }

    if (!any) return false;
    out = acc;
    return true;
}

namespace {

// Cap matches per frame: export tables run to thousands, and the user narrows by
// typing anyway. The dropdown scrolls up to this many.
constexpr int kMaxSuggest = 50;

// Completions for the term under the cursor, as full replacement tokens:
//   "module.<partial>" -> that module's exports ("kernel32.CreateFileW")
//   bare "<partial>"    -> loaded module names
// Reports the term's span via termStart/termLen.
int collectSuggestions(const AppState& s, const char* buf, int cursor,
    int& termStart, int& termLen, std::vector<std::string>& out)
{
    out.clear();
    int start = cursor;
    while (start > 0 && !isTermBoundary(buf[start - 1])) --start;
    int end = cursor;
    while (buf[end] != '\0' && !isTermBoundary(buf[end])) ++end;
    termStart = start;
    termLen   = end - start;
    if (termLen <= 0) return 0;

    const std::string term(buf + start, (size_t)termLen);

    // "module.<partial>": complete an export name. Match the module by its full or
    // base name, spelled exactly as typed up to the '.', then filter its exports.
    if (term.find('.') != std::string::npos)
    {
        for (const mem::ModuleEntry& m : s.modules)
        {
            const std::string spellings[2] = { m.name, moduleBaseName(m.name) };
            for (const std::string& mn : spellings)
            {
                const size_t len = mn.size();
                if (len == 0 || term.size() <= len ||
                    _strnicmp(term.c_str(), mn.c_str(), len) != 0 || term[len] != '.')
                    continue;

                const std::string typedPrefix = term.substr(0, len + 1); // "kernel32."
                const std::string sympart     = term.substr(len + 1);
                const AppState::ModuleExports& ex = ensureExports(s, m);

                for (const std::string& name : ex.names)
                {
                    if ((int)out.size() >= kMaxSuggest) break;
                    if (name.size() >= sympart.size() &&
                        _strnicmp(name.c_str(), sympart.c_str(), sympart.size()) == 0)
                        out.push_back(typedPrefix + name);
                }
                return (int)out.size();
            }
        }
    }

    // Otherwise: complete a module name.
    for (const mem::ModuleEntry& m : s.modules)
    {
        if ((int)out.size() >= kMaxSuggest) break;
        if ((int)m.name.size() >= termLen &&
            _strnicmp(buf + termStart, m.name.c_str(), (size_t)termLen) == 0)
            out.push_back(m.name);
    }
    return (int)out.size();
}

// Shared with the InputText callback: Tab reads `highlight`, the callback writes
// back the cursor and whether it inserted a completion.
struct AddrAcCtx {
    const AppState* s;
    int  highlight;
    int  cursor;
    bool accepted;
};

int addrAcCallback(ImGuiInputTextCallbackData* data)
{
    AddrAcCtx* ctx = (AddrAcCtx*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
    {
        ctx->cursor = data->CursorPos;
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        int ts = 0, tl = 0;
        std::vector<std::string> sug;
        const int n = collectSuggestions(*ctx->s, data->Buf, data->CursorPos,
            ts, tl, sug);
        if (n > 0)
        {
            int sel = ctx->highlight;
            if (sel < 0 || sel >= n) sel = 0;
            data->DeleteChars(ts, tl);
            data->InsertChars(ts, sug[sel].c_str());
            ctx->accepted = true;
        }
    }
    return 0;
}

} // namespace

bool addrInput(AppState& s, const char* id, char* buf, size_t bufSize,
    int flags, bool* deactivatedAfterEdit, bool* accepted)
{
    const ImGuiID itemId = ImGui::GetID(id);

    AddrAcCtx ctx;
    ctx.s         = &s;
    ctx.cursor    = -1;
    ctx.accepted  = false;
    ctx.highlight = (s.addrAcOwner == itemId) ? s.addrAcHighlight : 0;

    const bool submit = ImGui::InputText(id, buf, bufSize,
        (ImGuiInputTextFlags)flags | ImGuiInputTextFlags_CallbackAlways |
        ImGuiInputTextFlags_CallbackCompletion, addrAcCallback, &ctx);

    // Grab the field's state before the dropdown becomes the caller's "last item".
    if (deactivatedAfterEdit) *deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
    const bool   active  = ImGui::IsItemActive();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const ImGuiLastItemData savedItem = ImGui::GetCurrentContext()->LastItemData;

    int termStart = 0, termLen = 0;
    std::vector<std::string> sug;
    int n = 0;
    if (active && ctx.cursor >= 0)
        n = collectSuggestions(s, buf, ctx.cursor, termStart, termLen, sug);

    // Skip a lone suggestion that just echoes a fully-typed term.
    if (n == 1 && (int)sug[0].size() == termLen &&
        _strnicmp(buf + termStart, sug[0].c_str(), (size_t)termLen) == 0)
        n = 0;

    if (!active || n == 0)
    {
        if (s.addrAcOwner == itemId) { s.addrAcOwner = 0; s.addrAcHighlight = 0; }
        if (accepted) *accepted = ctx.accepted;
        return submit;
    }

    if (s.addrAcOwner != itemId) { s.addrAcOwner = itemId; s.addrAcHighlight = 0; }
    int& hl = s.addrAcHighlight;
    if (hl >= n) hl = n - 1;
    if (hl < 0)  hl = 0;
    bool navMoved = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { hl = (hl + 1) % n;     navMoved = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   { hl = (hl + n - 1) % n; navMoved = true; }

    // Replace the term with `name`, staying within bufSize.
    auto splice = [&](const std::string& name)
    {
        const int oldLen  = (int)strlen(buf);
        const int tailLen = oldLen - (termStart + termLen);
        int nameLen = (int)name.size();
        if (termStart + nameLen + tailLen + 1 > (int)bufSize)
            nameLen = (int)bufSize - 1 - termStart - tailLen;
        if (nameLen < 0) nameLen = 0;
        memmove(buf + termStart + nameLen, buf + termStart + termLen, (size_t)tailLen);
        memcpy(buf + termStart, name.c_str(), (size_t)nameLen);
        buf[termStart + nameLen + tailLen] = '\0';
    };

    // Cap the visible rows; the rest scrolls (wheel, scrollbar, or arrow keys).
    constexpr int kVisibleRows = 10;
    const ImGuiStyle& acStyle = ImGui::GetStyle();
    const int   rows   = std::min<int>(n, kVisibleRows);
    const float rowH   = ImGui::GetTextLineHeightWithSpacing();
    const float padY   = 2.f; // matches the WindowPadding pushed below
    const float listH  = rows * rowH + padY * 2.f;
    const bool  scrolls = n > kVisibleRows;

    // Width to the longest entry (export names outrun the field), but not narrower
    // than the field, not past the viewport edge, with room for the scrollbar.
    float contentW = 0.f;
    for (int i = 0; i < n; ++i)
        contentW = std::max<float>(contentW, ImGui::CalcTextSize(sug[i].c_str()).x);
    float acWidth = std::max<float>(itemMax.x - itemMin.x,
                                    contentW + padY * 2.f + 4.f +
                                    (scrolls ? acStyle.ScrollbarSize : 0.f));
    const ImGuiViewport* acVp = ImGui::GetWindowViewport();
    const float maxRight = acVp->WorkPos.x + acVp->WorkSize.x - itemMin.x;
    if (maxRight > 0.f) acWidth = std::min<float>(acWidth, maxRight);

    // Borderless list under the field, raised over the modal without stealing
    // focus. Screen-space pos + the input's viewport keep it right when torn out.
    ImGui::SetNextWindowViewport(ImGui::GetWindowViewport()->ID);
    ImGui::SetNextWindowPos(ImVec2(itemMin.x, itemMax.y));
    ImGui::SetNextWindowSize(ImVec2(acWidth, listH));
    const ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove        |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNavInputs   | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    char winId[64];
    snprintf(winId, sizeof(winId), "##addrac_%08X", (unsigned)itemId);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, padY));
    ImGui::Begin(winId, nullptr, wflags);
    ImGuiWindow* acWin = ImGui::GetCurrentWindow();
    for (int i = 0; i < n; ++i)
    {
        ImGui::PushID(i);
        if (ImGui::Selectable(sug[i].c_str(), i == hl))
        {
            splice(sug[i]);
            ctx.accepted = true;
        }
        // Keep the keyboard-highlighted row in view without fighting the wheel.
        if (i == hl && navMoved) ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::BringWindowToDisplayFront(acWin);

    // Hand the field back as the caller's "last item".
    ImGui::GetCurrentContext()->LastItemData = savedItem;

    if (accepted) *accepted = ctx.accepted;
    return submit;
}

void addAddyAddress(AppState& s, uintptr_t address)
{
    for (const auto& w : s.addyList)
        if (w.address == address) return;

    AddyEntry e = {};
    snprintf(e.desc,  sizeof(e.desc),  "No description");
    e.value = "0";
    e.address = address;
    e.typeIdx = 2; // default: 4 Bytes
    s.addyList.push_back(e);
}

// Length of the original instruction(s) that writing `newLen` bytes at `address`
// would touch: decode forward until the summed length covers newLen. Falls back
// to newLen (exact fit, no padding) if the bytes can't be read or decoded.
static size_t coveredOpcodeLen(const AppState& s, uintptr_t address,
    size_t newLen, bool arch64)
{
    uint8_t buf[64];
    if (!s.proc.is_open() || !mem::read_raw(s.proc, address, buf, sizeof(buf)))
        return newLen;

    ZydisDecoder dec;
    if (arch64)
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    else
        ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_COMPAT_32,
            ZYDIS_STACK_WIDTH_32);

    size_t covered = 0;
    while (covered < newLen && covered < sizeof(buf))
    {
        ZydisDecodedInstruction ins;
        if (ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&dec, nullptr,
                buf + covered, sizeof(buf) - covered, &ins)))
            covered += ins.length;
        else
            covered += 1; // undecodable byte: step one, like the disasm view
    }
    return covered < newLen ? newLen : covered;
}

// Write `out` at s.asmAddress, padding the tail with NOPs up to padTo. Clears
// the modals on success, sets s.asmError on failure. `out` is by value so it can
// be resized for padding.
static bool writeAsmResult(AppState& s, std::vector<uint8_t> out, size_t padTo)
{
    if (padTo > out.size())
        out.resize(padTo, 0x90);
    if (!s.proc.is_open() ||
        !mem::write_raw(s.proc, s.asmAddress, out.data(), out.size()))
    {
        snprintf(s.asmError, sizeof(s.asmError), "Failed to write to process memory");
        return false;
    }
    s.showAssemble      = false;
    s.showAsmNopConfirm = false;
    s.asmError[0]       = '\0';
    return true;
}

bool assembleAndWrite(AppState& s)
{
    std::vector<uint8_t> bytes;
    std::string err;
    const bool arch64 = (s.memViewArch == 0);
    if (!mem::assemble(s.asmInput, s.asmAddress, arch64, bytes, err))
    {
        snprintf(s.asmError, sizeof(s.asmError), "%s", err.c_str());
        return false;
    }

    const size_t covered = coveredOpcodeLen(s, s.asmAddress, bytes.size(), arch64);
    if (bytes.size() < covered && bytes.size() <= sizeof(s.asmNopBytes))
    {
        // Shorter than what it overwrites: defer to the NOP-pad prompt. Stash the
        // bytes and hide the Assemble box so the modals don't stack.
        memcpy(s.asmNopBytes, bytes.data(), bytes.size());
        s.asmNopByteCount   = bytes.size();
        s.asmNopNewLen      = bytes.size();
        s.asmNopCoverLen    = covered;
        s.showAssemble      = false;
        s.showAsmNopConfirm = true;
        return true;
    }
    return writeAsmResult(s, std::move(bytes), 0);
}

bool commitAsmBytes(AppState& s, bool padWithNops)
{
    std::vector<uint8_t> bytes(s.asmNopBytes, s.asmNopBytes + s.asmNopByteCount);
    return writeAsmResult(s, std::move(bytes),
        padWithNops ? s.asmNopCoverLen : 0);
}

// Parse a hex byte string ("48 89", "4889", "0x48 0x89") into raw bytes.
// Returns false + error on a non-hex character or an odd number of digits.
static bool parseHexBytes(const char* text, std::vector<uint8_t>& out,
    std::string& err)
{
    out.clear();
    int hi = -1; // pending high nibble, or -1 if none
    for (const char* p = text; *p; ++p)
    {
        const unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == ',')
            continue;
        // Skip a "0x"/"0X" prefix on each byte.
        if (c == '0' && (p[1] == 'x' || p[1] == 'X'))
        {
            ++p;
            continue;
        }
        int nib;
        if (c >= '0' && c <= '9')      nib = c - '0';
        else if (c >= 'a' && c <= 'f') nib = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nib = c - 'A' + 10;
        else { err = "Invalid hex character"; return false; }

        if (hi < 0) hi = nib;
        else { out.push_back((uint8_t)((hi << 4) | nib)); hi = -1; }
    }
    if (hi >= 0) { err = "Odd number of hex digits"; return false; }
    if (out.empty()) { err = "No bytes entered"; return false; }
    return true;
}

bool changeOpcodeAndWrite(AppState& s)
{
    std::vector<uint8_t> bytes;
    std::string err;
    if (!parseHexBytes(s.opcodeInput, bytes, err))
    {
        snprintf(s.opcodeError, sizeof(s.opcodeError), "%s", err.c_str());
        return false;
    }
    // Pad short edits with NOPs so no half-decoded instruction is left dangling.
    if (bytes.size() < s.opcodeOrigLen)
        bytes.resize(s.opcodeOrigLen, 0x90);

    if (!s.proc.is_open() ||
        !mem::write_raw(s.proc, s.opcodeAddress, bytes.data(), bytes.size()))
    {
        snprintf(s.opcodeError, sizeof(s.opcodeError),
            "Failed to write to process memory");
        return false;
    }
    s.showChangeOpcode = false;
    s.opcodeError[0]   = '\0';
    return true;
}

void nopFill(AppState& s, uintptr_t address, size_t length)
{
    if (!s.proc.is_open() || length == 0) return;

    // Plain single-byte 0x90 NOPs. Multi-byte NOP forms would decode as one
    // different-looking instruction rather than N separate NOPs.
    std::vector<uint8_t> buf(length, 0x90);
    mem::write_raw(s.proc, address, buf.data(), buf.size());
}

void generateSignature(AppState& s)
{
    const mem::SigStyle style = s.sigStyle == 1
        ? mem::SigStyle::Code : mem::SigStyle::Ida;
    const std::string sig = mem::createSignature(
        s.proc, s.modules, s.sigAddress, s.memViewArch, style, s.sigUnique);
    snprintf(s.sigOutput, sizeof(s.sigOutput), "%s", sig.c_str());
}

} // namespace app
