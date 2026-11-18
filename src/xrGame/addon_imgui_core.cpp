#include "StdAfx.h"

// The common layer of the ImGui widgets of the addons (plans/addons_api_support/20-imgui-addon-tools.md, 4.2). See
// addon_imgui_core.h.

#include "addon_imgui_core.h"
#include "addon_host.h"

#include <imgui_internal.h>

#include <cstdarg>
#include <cstring>

namespace gw::addons::imgui
{
namespace
{
// The names of the pairs, in the order of EScope
struct ScopeNames
{
    pcstr begin;
    pcstr end;
};
constexpr ScopeNames kScopeNames[] = {
    { "BeginChild", "EndChild" },
    { "BeginGroup", "EndGroup" },
    { "PushID", "PopID" },
    { "PushStyleColor", "PopStyleColor" },
    { "PushStyleVar", "PopStyleVar" },
    { "PushItemWidth", "PopItemWidth" },
    { "PushTextWrapPos", "PopTextWrapPos" },
    { "BeginDisabled", "EndDisabled" },
    { "Indent", "Unindent" },
    { "BeginTable", "EndTable" },
    { "BeginCombo", "EndCombo" },
    { "BeginListBox", "EndListBox" },
    { "BeginMenu", "EndMenu" },
    { "BeginMenuBar", "EndMenuBar" },
    { "BeginPopup", "EndPopup" },
    { "BeginTooltip", "EndTooltip" },
    { "BeginTabBar", "EndTabBar" },
    { "BeginTabItem", "EndTabItem" },
    { "TreeNode", "TreePop" },
};
static_assert(std::size(kScopeNames) == static_cast<size_t>(EScope::Count), "kScopeNames: one row per EScope");

DrawFrame* g_current = nullptr;

// CheckPop: the text of the error (one at a time: the caller raises it at once)
thread_local string512 g_error_text;

// The End* of ImGui for a record (CloseAll)
void CloseScope(EScope scope, float arg)
{
    switch (scope)
    {
    case EScope::Child: ImGui::EndChild(); break;
    case EScope::Group: ImGui::EndGroup(); break;
    case EScope::Id: ImGui::PopID(); break;
    case EScope::StyleColor: ImGui::PopStyleColor(1); break;
    case EScope::StyleVar: ImGui::PopStyleVar(1); break;
    case EScope::ItemWidth: ImGui::PopItemWidth(); break;
    case EScope::TextWrapPos: ImGui::PopTextWrapPos(); break;
    case EScope::Disabled: ImGui::EndDisabled(); break;
    case EScope::Indent: ImGui::Unindent(arg); break;
    case EScope::Table: ImGui::EndTable(); break;
    case EScope::Combo: ImGui::EndCombo(); break;
    case EScope::ListBox: ImGui::EndListBox(); break;
    case EScope::Menu: ImGui::EndMenu(); break;
    case EScope::MenuBar: ImGui::EndMenuBar(); break;
    case EScope::Popup: ImGui::EndPopup(); break;
    case EScope::Tooltip: ImGui::EndTooltip(); break;
    case EScope::TabBar: ImGui::EndTabBar(); break;
    case EScope::TabItem: ImGui::EndTabItem(); break;
    case EScope::TreeNode: ImGui::TreePop(); break;
    case EScope::Count: break;
    }
}

bool IsFlagChar(char c) { return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
} // namespace

pcstr ScopeBeginName(EScope scope)
{
    const size_t index = static_cast<size_t>(scope);
    return index < std::size(kScopeNames) ? kScopeNames[index].begin : "?";
}

pcstr ScopeEndName(EScope scope)
{
    const size_t index = static_cast<size_t>(scope);
    return index < std::size(kScopeNames) ? kScopeNames[index].end : "?";
}

DrawFrame* Current() { return g_current; }

bool InDraw() { return g_current != nullptr && IsMainThread(); }

DrawScope::DrawScope(DrawFrame& frame) : m_previous(g_current)
{
    R_ASSERT2(!g_current, "ImGui tools of the addons: a draw inside a draw");
    g_current = &frame;
}

DrawScope::~DrawScope() { g_current = m_previous; }

pcstr CanPush()
{
    if (!g_current)
        return "only inside draw of an ImGui tool";
    if (g_current->depth >= kMaxStackDepth)
        return "too many open Begin*/Push* pairs (64): close them with their End*/Pop*";
    return nullptr;
}

void Push(EScope scope, float arg)
{
    DrawFrame* frame = g_current;
    if (!frame || frame->depth >= kMaxStackDepth)
        return; // CanPush said so before the Begin of ImGui; a caller that skipped it is a bug of the binding
    frame->stack[frame->depth] = scope;
    frame->args[frame->depth] = arg;
    ++frame->depth;
}

pcstr CheckPop(EScope scope)
{
    const DrawFrame* frame = g_current;
    if (!frame)
        return "only inside draw of an ImGui tool";
    if (frame->depth == 0)
    {
        xr_sprintf(g_error_text, "%s without %s (nothing is open)", ScopeEndName(scope), ScopeBeginName(scope));
        return g_error_text;
    }
    const EScope top = frame->stack[frame->depth - 1];
    if (top == scope)
        return nullptr;
    // Is the pair open deeper? Then the order is wrong; else there is no such Begin at all
    bool deeper = false;
    for (u32 i = 0; i + 1 < frame->depth; ++i)
    {
        if (frame->stack[i] == scope)
        {
            deeper = true;
            break;
        }
    }
    if (deeper)
    {
        xr_sprintf(g_error_text, "%s: the last open pair is %s - close it with %s first", ScopeEndName(scope),
            ScopeBeginName(top), ScopeEndName(top));
    }
    else
    {
        xr_sprintf(g_error_text, "%s without %s (the last open pair is %s)", ScopeEndName(scope),
            ScopeBeginName(scope), ScopeBeginName(top));
    }
    return g_error_text;
}

float Pop()
{
    DrawFrame* frame = g_current;
    if (!frame || frame->depth == 0)
        return 0.f;
    --frame->depth;
    return frame->args[frame->depth];
}

float TopArg()
{
    const DrawFrame* frame = g_current;
    if (!frame || frame->depth == 0)
        return 0.f;
    return frame->args[frame->depth - 1];
}

u32 CloseAll(DrawFrame& frame, EScope* innermost)
{
    if (innermost && frame.depth)
        *innermost = frame.stack[frame.depth - 1];
    const u32 closed = frame.depth;
    while (frame.depth)
    {
        --frame.depth;
        CloseScope(frame.stack[frame.depth], frame.args[frame.depth]);
    }
    return closed;
}

pcstr SafeFormat(pcstr format, bool integer, pcstr fallback)
{
    if (!format || !format[0])
        return fallback;
    u32 conversions = 0;
    bool valid = xr_strlen(format) <= kMaxFormatLength;
    for (pcstr c = format; valid && *c; ++c)
    {
        if (*c != '%')
            continue;
        ++c;
        if (*c == '%')
            continue; // "%%" - a percent sign
        // The conversion stays short: ImGui copies it into char[32] without a bound in release (RoundScalarWithFormatT
        // -> ImParseFormatSanitizeForPrinting: a stack overflow when a float slider / drag changes) and cuts it to 31
        // bytes for the Ctrl+Click input (TempInputScalar: a cut conversion is an invalid printf format); a precision
        // of 9 digits makes printf write and allocate that many digits every frame. At most 5 flags, 2 digits of the
        // width, 2 of the precision: at most 12 bytes
        const pcstr flags_start = c;
        while (IsFlagChar(*c))
            ++c;
        const pcstr width_start = c;
        while (IsDigit(*c))
            ++c;
        if (width_start - flags_start > 5 || c - width_start > 2)
        {
            valid = false;
            break;
        }
        if (*c == '.')
        {
            ++c;
            const pcstr precision_start = c;
            while (IsDigit(*c))
                ++c;
            if (c == precision_start || c - precision_start > 2)
            {
                valid = false;
                break;
            }
        }
        // No length modifiers (l, h, ll ...), no '*': the widget gives one value of its own type
        const bool int_conv = *c == 'd' || *c == 'i' || *c == 'u' || *c == 'x' || *c == 'X';
        const bool float_conv = *c == 'f' || *c == 'F' || *c == 'e' || *c == 'E' || *c == 'g' || *c == 'G';
        if (integer ? !int_conv : !float_conv)
        {
            valid = false;
            break;
        }
        ++conversions;
    }
    if (valid && conversions == 1)
        return format;
    WarnOnce(kWarnFormat, "the format \"%.64s\" is not one %s conversion (%s), \"%s\" is used instead (this warning "
        "is shown once)", format, integer ? "integer" : "float", integer ? "%d %i %u %x %X" : "%f %e %g", fallback);
    return fallback;
}

void WarnOnce(u32 bit, pcstr format, ...)
{
    DrawFrame* frame = g_current;
    if (!frame)
        return;
    if (frame->warned)
    {
        if (*frame->warned & bit)
            return;
        *frame->warned |= bit;
    }
    string1024 text;
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    text[sizeof(text) - 1] = 0;
    Msg("~ [addon:%s] ImGui tool '%s': %s", frame->owner, frame->id, text);
}

ImGuiWindowFlags TranslateWindowFlags(u32 gw_flags)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (gw_flags & kWindowAutoResize)
        flags |= ImGuiWindowFlags_AlwaysAutoResize;
    if (gw_flags & kWindowNoResize)
        flags |= ImGuiWindowFlags_NoResize;
    if (gw_flags & kWindowNoCollapse)
        flags |= ImGuiWindowFlags_NoCollapse;
    if (gw_flags & kWindowNoScrollbar)
        flags |= ImGuiWindowFlags_NoScrollbar;
    if (gw_flags & kWindowHorizontalScrollbar)
        flags |= ImGuiWindowFlags_HorizontalScrollbar;
    if (gw_flags & kWindowMenuBar)
        flags |= ImGuiWindowFlags_MenuBar;
    return flags;
}

void StoreState(ImGuiErrorRecoveryState& state) { ImGui::ErrorRecoveryStoreState(&state); }

bool RecoverState(const ImGuiErrorRecoveryState& state)
{
    ImGuiErrorRecoveryState now;
    ImGui::ErrorRecoveryStoreState(&now);
    if (memcmp(&now, &state, sizeof(now)) == 0)
    {
        // The stacks are as they were after Begin; a table or a tab bar left open does not show in them
        ImGuiContext& g = *GImGui;
        const bool table = g.CurrentTable && g.CurrentTable->InnerWindow == g.CurrentWindow;
        const bool tab_bar = g.CurrentTabBar && g.CurrentTabBar->Window == g.CurrentWindow;
        const bool menu_bar = g.CurrentWindow && g.CurrentWindow->DC.MenuBarAppending;
        if (!table && !tab_bar && !menu_bar)
            return false;
    }
    // The asserts and the error tooltip of ImGui are off for the time: the error is the addon's, it is logged by the
    // host; the tooltip would open a window in the middle of the recovery
    ImGuiIO& io = ImGui::GetIO();
    const bool assert_on = io.ConfigErrorRecoveryEnableAssert;
    const bool tooltip_on = io.ConfigErrorRecoveryEnableTooltip;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::ErrorRecoveryTryToRecoverState(&state);
    io.ConfigErrorRecoveryEnableAssert = assert_on;
    io.ConfigErrorRecoveryEnableTooltip = tooltip_on;
    return true;
}
} // namespace gw::addons::imgui
