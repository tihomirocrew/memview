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

std::string trim(const std::string& in)
{
    size_t b = 0, e = in.size();
    while (b < e && std::isspace((unsigned char)in[b])) ++b;
    while (e > b && std::isspace((unsigned char)in[e - 1])) --e;
    return in.substr(b, e - b);
}

// Case-insensitive module lookup by name.
const mem::ModuleEntry* findModuleByName(const AppState& s, const std::string& name)
{
    for (const mem::ModuleEntry& m : s.modules)
        if (_stricmp(m.name.c_str(), name.c_str()) == 0)
            return &m;
    return nullptr;
}

} // namespace

bool parseAddrExpr(const AppState& s, const char* text, uintptr_t& out)
{
    const char* plusPos = strchr(text, '+');
    const std::string head = trim(
        plusPos ? std::string(text, (size_t)(plusPos - text)) : std::string(text));
    if (head.empty()) return false;

    if (const mem::ModuleEntry* m = findModuleByName(s, head))
    {
        uintptr_t offset = 0;
        if (plusPos)
        {
            const std::string tail = trim(std::string(plusPos + 1));
            if (!tail.empty())
            {
                char* end = nullptr;
                offset = (uintptr_t)strtoull(tail.c_str(), &end, 16);
                if (end == tail.c_str() || *end != '\0') return false;
            }
        }
        out = m->base + offset;
        return true;
    }

    if (plusPos) return false; // unknown module, no base to add the offset to

    char* end = nullptr;
    const uintptr_t v = (uintptr_t)strtoull(head.c_str(), &end, 16);
    if (end == head.c_str() || *end != '\0') return false;
    out = v;
    return true;
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
