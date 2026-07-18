#include "ui/memory_view.hpp"
#include "app/app.hpp"
#include "memory/memory.hpp"
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder* for the layout, FindWindowByName for modals
#include <cstdio>

// The Memory View container: a separate OS window whose dockspace hosts the
// three panes (Disassembly, Hex View, Memory Regions), which can be dragged
// apart or torn out. Also owns the shared Go/Back bar driving both panes.
namespace ui {

namespace {

// NoResize drops ImGui's corner grip; a torn-out pane is still resizable by its
// native OS border. Docked panes use node-level splitters regardless.
const ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;

// "Go to Address" popup shared by the Disassembly and Hex View menus. Each
// caller passes its own show flag/input/target so the panes stay independent,
// plus an anchor window so the popup centers on that pane's OS window.
void drawGotoAddressModal(app::AppState& s, bool& show, char* input,
    size_t inputSize, uintptr_t& target, std::vector<uintptr_t>& history,
    const char* popupId, const char* anchorWindow)
{
    if (!show) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (ImGuiWindow* win = ImGui::FindWindowByName(anchorWindow))
        vp = win->Viewport;

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##goto_dim", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking);
    ImGui::GetWindowDrawList()->AddRectFilled(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
        IM_COL32(20, 20, 20, 110));
    ImGui::End();
    ImGui::PopStyleColor();

    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::OpenPopup(popupId);

    if (!ImGui::BeginPopupModal(popupId, &show,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;

    ImGui::TextUnformatted("Address (hex), or module+offset");
    ImGui::SetNextItemWidth(-1);
    // Focus the input only on the frame it appears; every frame would fight
    // clicks on Go/Cancel.
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    const bool submit = ImGui::InputText("##gotoaddrinput", input, inputSize,
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    uintptr_t parsed = 0;
    const bool valid = input[0] != '\0' && app::parseAddrExpr(s, input, parsed);

    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Go", ImVec2(120, 0)) || (submit && valid))
    {
        pushHistory(history, target);
        target = parsed;
        show   = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        show = false;

    ImGui::EndPopup();
}

} // namespace

void drawMemoryView(app::AppState& s)
{
    // Track visibility so a close+reopen can rebuild the dock layout below.
    static bool memViewWasOpen = false;
    if (!s.showMemView) { memViewWasOpen = false; return; }

    // Center the window on its first open; FirstUseEver then lets a dragged
    // position win on later opens. (The brief 0,0 flash on open is a separate,
    // DWM-composition issue handled by cloaking in main.cpp, not by this seed.)
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainVp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_FirstUseEver);
    // Always its own OS window: never dockable, never auto-merged into the main
    // window. The three panes inside still dock freely among themselves.
    ImGuiWindowClass alwaysOwnWindow;
    alwaysOwnWindow.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&alwaysOwnWindow);
    // NoResize: resizing is done by the native OS border (see kPaneFlags).
    const ImGuiWindowFlags memViewFlags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin("Memory View", &s.showMemView, memViewFlags))
    {
        ImGui::End();
        return;
    }

    if (!s.proc.is_open())
    {
        ImGui::TextUnformatted("No process attached.");
        ImGui::End();
        return;
    }

    // Auto-detect the disassembly mode from the target's bitness every frame, so
    // it stays correct even if the user re-attaches to a different process while
    // this window is open. A WOW64 process (32-bit on 64-bit Windows) is x86.
    s.memViewArch = mem::is_wow64(s.proc) ? 1 : 0;

    // Refresh the module list every couple seconds; modules change rarely, so
    // this still catches a freshly loaded DLL cheaply.
    const double modNow = ImGui::GetTime();
    if (modNow >= s.modulesNextRefresh)
    {
        s.modules = mem::list_modules(s.proc);
        s.modulesNextRefresh = modNow + 2.0;
    }

    // --- Shared address bar (seeds both the disasm and hex panes) -----------
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Go to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    const bool go = ImGui::InputText("##goto", s.memGotoInput,
        sizeof(s.memGotoInput), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hex address, or module+offset");
    ImGui::SameLine();
    uintptr_t gotoAddr = 0;
    const bool gotoValid = s.memGotoInput[0] && app::parseAddrExpr(s, s.memGotoInput, gotoAddr);
    ImGui::BeginDisabled(!gotoValid);
    if ((ImGui::Button("Go") || (go && gotoValid)))
    {
        pushHistory(s.disasmHistory, s.disasmAddr);
        pushHistory(s.hexHistory, s.hexAddr);
        s.disasmAddr = gotoAddr;
        s.hexAddr    = gotoAddr;
    }
    ImGui::EndDisabled();

    // Back undoes the shared Go above in both panes at once.
    ImGui::SameLine();
    ImGui::BeginDisabled(s.disasmHistory.empty() && s.hexHistory.empty());
    if (ImGui::Button("Back"))
    {
        goBack(s.disasmHistory, s.disasmAddr);
        goBack(s.hexHistory, s.hexAddr);
        // Reflect the restored address in the Go field so Back doesn't look like
        // a no-op when the Memory Regions tab is in front.
        snprintf(s.memGotoInput, sizeof(s.memGotoInput), "%llX",
            (unsigned long long)s.disasmAddr);
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 20);
    ImGui::Text("Mode: %s", s.memViewArch == 1 ? "x86" : "x64");

    ImGui::Separator();

    // --- IDA-style pane sync -------------------------------------------------
    // Whichever pane moved since last frame drags the opted-in other along.
    // Done once up front so every navigation source propagates the same way,
    // without pushing history (the follow isn't a jump the user asked for).
    if (s.syncHexToDisasm && s.disasmAddr != s.prevDisasmAddr)
        s.hexAddr = s.disasmAddr;
    if (s.syncDisasmToHex && s.hexAddr != s.prevHexAddr)
        s.disasmAddr = s.hexAddr;
    s.prevDisasmAddr = s.disasmAddr;
    s.prevHexAddr    = s.hexAddr;

    // --- Dockspace hosting the three panels ---------------------------------
    // Default layout: Disassembly on top with Memory Regions tabbed behind it,
    // Hex View below, both visible at once. The user can re-split freely.
    const ImGuiID dockId = ImGui::GetID("MemViewDockSpace");
    static bool layoutBuilt = false;
    static bool focusDisasm = false;
    // Remember the split nodes so a re-opened pane re-docks to its home.
    static ImGuiID topId = 0, bottomId = 0;
    // While hidden, ImGui eventually garbage-collects the dock node, so a reopen
    // must rebuild the layout rather than assume the old node still exists.
    const bool justReopened = !memViewWasOpen;
    memViewWasOpen = true;
    if (justReopened) layoutBuilt = false;

    // Tearing Hex View out leaves its split node behind as an empty gray gap.
    // Reclaim that space by collapsing to a single node with
    // Disassembly/Memory Regions; the floating Hex View is left as-is.
    if (layoutBuilt && bottomId != 0)
    {
        ImGuiDockNode* botNode = ImGui::DockBuilderGetNode(bottomId);
        if (botNode && botNode->IsEmpty())
        {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId,
                ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
            ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetContentRegionAvail());
            ImGui::DockBuilderDockWindow("Disassembly", dockId);
            ImGui::DockBuilderDockWindow("Memory Regions", dockId);
            ImGui::DockBuilderFinish(dockId);
            topId = dockId;
            bottomId = 0;
        }
    }

    if (!layoutBuilt)
    {
        layoutBuilt = true;
        focusDisasm = true;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId,
            ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoWindowMenuButton);
        ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetContentRegionAvail());

        topId = ImGui::DockBuilderSplitNode(
            dockId, ImGuiDir_Up, 0.58f, nullptr, &bottomId);

        // Disassembly + Memory Regions on top, Hex View below.
        ImGui::DockBuilderDockWindow("Disassembly", topId);
        ImGui::DockBuilderDockWindow("Memory Regions", topId);
        ImGui::DockBuilderDockWindow("Hex View", bottomId);
        ImGui::DockBuilderFinish(dockId);
    }
    // NoWindowMenuButton removes the little arrow/menu button on the tab bars.
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_NoWindowMenuButton);

    ImGui::End();

    // Each pane is a separate docked window with a null p_open, so it gets no
    // close button; the panes can be torn out but never closed. Submission order
    // sets the tab order, so Disassembly comes before Memory Regions.
    //
    // Re-docking on re-open: right after the whole Memory View window was
    // hidden and shown again, steer every pane back to its original node for
    // that one frame instead of letting it reappear as a loose float.
    if (justReopened) ImGui::SetNextWindowDockID(topId, ImGuiCond_Always);
    if (ImGui::Begin("Disassembly", nullptr, kPaneFlags))
        drawDisasm(s);
    ImGui::End();

    if (justReopened) ImGui::SetNextWindowDockID(topId, ImGuiCond_Always);
    if (ImGui::Begin("Memory Regions", nullptr, ImGuiWindowFlags_NoResize))
        drawRegions(s);
    ImGui::End();

    if (justReopened) ImGui::SetNextWindowDockID(bottomId, ImGuiCond_Always);
    if (ImGui::Begin("Hex View", nullptr, kPaneFlags))
        drawHex(s);
    ImGui::End();

    // A later-submitted sibling would be auto-selected, so force Disassembly
    // active once.
    if (focusDisasm)
    {
        focusDisasm = false;
        ImGui::SetWindowFocus("Disassembly");
    }

    drawAssembleModal(s);
    drawAsmNopConfirm(s);
    drawChangeOpcodeModal(s);
    drawSignatureModal(s);
    drawFindSignatureModal(s);
    drawGotoAddressModal(s, s.showGotoDisasm, s.gotoDisasmInput,
        sizeof(s.gotoDisasmInput), s.disasmAddr, s.disasmHistory,
        "Go to Address##disasm", "Disassembly");
    drawGotoAddressModal(s, s.showGotoHex, s.gotoHexInput,
        sizeof(s.gotoHexInput), s.hexAddr, s.hexHistory,
        "Go to Address##hex", "Hex View");
}

} // namespace ui
