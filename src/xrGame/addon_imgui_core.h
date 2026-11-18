#pragma once

// The common layer of the ImGui widgets of the addons (plans/addons_api_support/20-imgui-addon-tools.md, 4.2 "safety"):
// the Lua table `ui` (addon_imgui_lua_widgets.cpp) and the C table GwpImGuiApi of the plugins (stage 4) both call ImGui
// through these checks, so an addon cannot break the stack of ImGui or the process:
//  - InDraw(): a widget runs only inside draw of an ImGui tool (the host set the frame of the tool) on the main thread;
//    elsewhere there is no ImGui frame (level_on_frame, a timer, the Game thread of an overlap frame);
//  - the shadow stack: every Begin*/Push* that needs its End*/Pop* puts a record on the stack of the tool (Push), every
//    End*/Pop* checks the top first (CheckPop) - an End* without its Begin* or out of order is an error of the addon,
//    not an IM_ASSERT of ImGui. What draw left open the host closes in the reverse order (CloseAll) and warns once;
//  - SafeFormat: the format of a slider / drag / input is checked (one conversion, no %n, no %s);
//  - the limits of the texts, the lists and the plots;
//  - the window flags of GW (the plugin ABI and the Lua names) are translated to ImGuiWindowFlags here;
//  - StoreState / RecoverState: the safety net of ImGui itself (ErrorRecovery*) with its asserts off for the time.
// Main thread only (the thread of the ImGui frame, inside ide::OnFrame).
//
// How a widget is added (stages 3 and 4): see the comment at the end of this file.

#include <imgui.h>

struct ImGuiErrorRecoveryState;

namespace gw::addons::imgui
{
// ---------------------------------------------------------------------------------------------
// Limits (20-imgui-addon-tools.md 4.2, item 6)
// ---------------------------------------------------------------------------------------------
constexpr u32 kMaxStackDepth = 64;              // the shadow stack of one tool
constexpr u32 kMaxInputText = 16 * 1024;        // InputText / InputTextWithHint: the buffer in bytes
constexpr u32 kMaxInputTextMultiline = 64 * 1024; // InputTextMultiline
constexpr u32 kMaxListItems = 1024;             // Combo, ListBox: the items of an array
constexpr u32 kMaxPlotValues = 4096;            // PlotLines, PlotHistogram
constexpr u32 kMaxFormatLength = 64;            // the format of a slider / drag / input
constexpr u32 kMaxTextLength = 64 * 1024;       // Text*, LabelText, BulletText, SetTooltip: the bytes shown (the rest is cut)
constexpr u32 kMaxLabelLength = 1024;           // the label / id of a widget (the rest is cut)

// ---------------------------------------------------------------------------------------------
// The shadow stack
// ---------------------------------------------------------------------------------------------
// The pairs: the first group needs its End always, the second only when its Begin returned true (the caller pushes
// only then). The order is the table of the names in addon_imgui_core.cpp (kScopeNames) - keep both in step.
enum class EScope : u8
{
    // End always
    Child,       // BeginChild / EndChild (EndChild even when BeginChild returned false)
    Group,       // BeginGroup / EndGroup
    Id,          // PushID / PopID
    StyleColor,  // PushStyleColor / PopStyleColor
    StyleVar,    // PushStyleVar / PopStyleVar
    ItemWidth,   // PushItemWidth / PopItemWidth
    TextWrapPos, // PushTextWrapPos / PopTextWrapPos
    Disabled,    // BeginDisabled / EndDisabled
    Indent,      // Indent / Unindent (the width is kept: Unindent of the host gives it back)
    // End only after a true
    Table,       // BeginTable / EndTable
    Combo,       // BeginCombo / EndCombo
    ListBox,     // BeginListBox / EndListBox
    Menu,        // BeginMenu / EndMenu
    MenuBar,     // BeginMenuBar / EndMenuBar
    Popup,       // BeginPopup* / EndPopup
    Tooltip,     // BeginTooltip / BeginItemTooltip / EndTooltip
    TabBar,      // BeginTabBar / EndTabBar
    TabItem,     // BeginTabItem / EndTabItem
    TreeNode,    // TreeNode* (true) / TreePop
    Count
};

pcstr ScopeBeginName(EScope scope); // "BeginChild"
pcstr ScopeEndName(EScope scope);   // "EndChild"

// The warnings that are logged once per tool (DrawFrame::warned)
enum EWarnOnce : u32
{
    kWarnUnclosed = 1u << 0,   // draw left a Begin* open (CloseAll)
    kWarnFormat = 1u << 1,     // a bad format was replaced (SafeFormat)
    kWarnTruncated = 1u << 2,  // a text, an array or a buffer was cut to its limit
    kWarnFlags = 1u << 3,      // flags of the frame (docking, saved settings...) were masked
    // stage 3 (the Lua table `ui`): bits 8..15
    kWarnArgs = 1u << 8,       // an argument out of its range was fixed (a zero size, a bad index, a bad mix of flags)
    // stage 4 (GwpImGuiApi): bits 16..23
};

// The draw of one tool: the host makes it on its stack for the time of the call of draw (DrawScope)
struct DrawFrame
{
    pcstr owner = "?";   // the addon id
    pcstr id = "?";      // the tool id
    u32 depth = 0;       // records on the stack
    u32* warned = nullptr; // the EWarnOnce bits of the tool (live in the entry of the registry: once per registration)
    EScope stack[kMaxStackDepth]{};
    float args[kMaxStackDepth]{}; // Indent: the width given to Indent
};

// The frame of the draw that runs now; nullptr outside of draw
DrawFrame* Current();
// A widget may run: inside draw of a tool and on the main thread. One pointer check and one thread check
bool InDraw();

// The host sets the frame for the time of draw. Not copyable; a nested draw is not possible (the host draws one tool
// at a time), a second DrawScope while one is active is an error of the host (checked).
class DrawScope
{
public:
    explicit DrawScope(DrawFrame& frame);
    ~DrawScope();
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;

private:
    DrawFrame* m_previous;
};

// Call before the Begin*/Push* of ImGui: nullptr - there is room (call Push after a successful Begin), else the error
// text (the stack of the tool is full: kMaxStackDepth). Static strings.
pcstr CanPush();
// After the Begin*/Push* of ImGui (for the second group - only when it returned true). arg: the width of Indent
void Push(EScope scope, float arg = 0.f);
// Before the End*/Pop* of ImGui: nullptr - the top of the stack is this scope, call the End* of ImGui then Pop(); else
// the error text ("EndTable without BeginTable", "EndTable: the last open pair is BeginChild - close it first").
// The text lives in a thread-local buffer until the next call.
pcstr CheckPop(EScope scope);
// After the End*/Pop* of ImGui: the top record goes. Returns the arg of the record (Indent: the width)
float Pop();
// The arg of the top record (CheckPop passed): Unindent needs the width before it calls ImGui
float TopArg();

// After draw: everything left open is closed in the reverse order by the End* of ImGui. Returns how many records were
// closed; innermost - the scope of the innermost one left open (for the warning), untouched if none.
u32 CloseAll(DrawFrame& frame, EScope* innermost = nullptr);

// ---------------------------------------------------------------------------------------------
// Formats and texts
// ---------------------------------------------------------------------------------------------
// The printf format of a slider / drag / input from an addon: exactly one conversion (%[-+ #0]{0,5}\d{0,2}(\.\d{1,2})?[conv]:
// ImGui copies the conversion into char[32], see SafeFormat) plus
// any "%%", at most kMaxFormatLength bytes. integer: the conversion must be one of d i u x X (the int widgets), else
// one of f F e E g G (the float widgets: "%d" for a float is not patched by ImGui here, IMGUI_DISABLE_OBSOLETE_FUNCTIONS).
// No length modifiers, no '*'. nullptr / "" - fallback. A bad format - fallback and a warning once per tool (kWarnFormat).
pcstr SafeFormat(pcstr format, bool integer, pcstr fallback);

// A warning of the tool once per registration (bit - EWarnOnce): "~ [addon:<owner>] ImGui tool '<id>': <text>"
void WarnOnce(u32 bit, pcstr format, ...);

// ---------------------------------------------------------------------------------------------
// Window flags of GW (the def.flags of Lua, GwpImGuiToolDesc::window_flags of the plugins, stage 4)
// ---------------------------------------------------------------------------------------------
// The values are part of the plugin ABI (stage 4: GWP_IMGUI_WINDOW_* have the same values): only add new bits.
enum EWindowFlags : u32
{
    kWindowAutoResize = 1u << 0,          // ImGuiWindowFlags_AlwaysAutoResize
    kWindowNoResize = 1u << 1,            // ImGuiWindowFlags_NoResize
    kWindowNoCollapse = 1u << 2,          // ImGuiWindowFlags_NoCollapse
    kWindowNoScrollbar = 1u << 3,         // ImGuiWindowFlags_NoScrollbar
    kWindowHorizontalScrollbar = 1u << 4, // ImGuiWindowFlags_HorizontalScrollbar
    kWindowMenuBar = 1u << 5,             // ImGuiWindowFlags_MenuBar (the window has BeginMenuBar)
    kWindowAll = (1u << 6) - 1,
};
ImGuiWindowFlags TranslateWindowFlags(u32 gw_flags);

// Flags of ImGui an addon never gets through a number (docking, viewports, saved settings, the flags of the frame):
// the widgets of stage 3 that take ImGuiWindowFlags / ImGuiChildFlags as numbers mask them with these
constexpr ImGuiWindowFlags kWindowFlagsDenied = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus |
    ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal |
    ImGuiWindowFlags_ChildMenu;

// ---------------------------------------------------------------------------------------------
// The safety net of ImGui (imgui_internal.h ErrorRecovery*)
// ---------------------------------------------------------------------------------------------
// Right after Begin of the tool window: the sizes of the stacks of ImGui
void StoreState(ImGuiErrorRecoveryState& state);
// After draw (always; after an error the stacks may be anything): the windows above the tool window are ended, the
// stacks of the tool window go back to `state`. io.ConfigErrorRecoveryEnableAssert is off for the time (DEBUG has it
// on: Device_imgui.cpp). Returns true when ImGui had something to fix (an error of the addon the shadow stack missed:
// a raw ImGui call of a plugin in native mode, stage 5).
bool RecoverState(const ImGuiErrorRecoveryState& state);
} // namespace gw::addons::imgui

// How a widget is added (the contract for the stages 3 and 4)
// ---------------------------------------------------------------------------------------------
// Lua (addon_imgui_lua_widgets.cpp): a C function `int UiFoo(lua_State* L)` in the table kUiFunctions, name as in C++
// ImGui ("Foo"). Its first line is GW_UI_ENTER(L, "Foo") (the InDraw check, luaL_error outside of draw). The labels -
// CheckText(L, n); a format - imgui::SafeFormat; arrays and buffers - the limits above; numbers from Lua that become
// ImGui flags - masked (kWindowFlagsDenied and the like; the constant tables give the names, stage 3).
// A pair:
//   Begin that needs End always (BeginChild):
//       if (pcstr e = imgui::CanPush()) return luaL_error(L, "ui.BeginChild: %s", e);
//       const bool r = ImGui::BeginChild(...); imgui::Push(imgui::EScope::Child); lua_pushboolean(L, r); return 1;
//   Begin that needs End only after true (BeginTable):
//       if (pcstr e = imgui::CanPush()) return luaL_error(...);
//       const bool r = ImGui::BeginTable(...); if (r) imgui::Push(imgui::EScope::Table); ...
//   End:
//       if (pcstr e = imgui::CheckPop(imgui::EScope::Table)) return luaL_error(L, "ui.EndTable: %s", e);
//       ImGui::EndTable(); imgui::Pop(); return 0;
// Never raise a Lua error between the Begin of ImGui and Push (or between CheckPop and the End of ImGui + Pop): the
// shadow stack must be what ImGui has. Values: `value, changed` (20-imgui-addon-tools.md, decision B3).
// C table of the plugins (stage 4, addon_api_imgui.cpp): the same calls; outside of draw (InDraw false) a function
// returns 0 and warns once per plugin; an error of the pair (CanPush / CheckPop) is not a Lua error there - the
// function does nothing, returns 0 and warns once per tool (WarnOnce).
