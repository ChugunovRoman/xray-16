#include "StdAfx.h"

// The table `ui` of the ImGui tools of the addons (plans/addons_api_support/20-imgui-addon-tools.md, 4.2): the widgets
// of ImGui for the draw(ui, tool) of a Lua tool - the set v1 (stage 3). Names as in C++ ImGui (decision B2), the values
// as `value, changed` (decision B3), the constants as sub-tables with the names of ImGui (ui.TableFlags.Borders for
// ImGuiTableFlags_Borders). Every function goes through addon_imgui_core: only inside draw (GW_UI_ENTER), the shadow
// stack for every pair, safe formats (SafeFormat), the limits of the texts, the buffers and the arrays, the flags masked
// by the names this table gives (a number with other bits loses them, a warning once per tool). The recipe of a pair is
// at the end of addon_imgui_core.h.
//
// The arguments are read before ImGui is called: a Lua error (a bad argument) never comes between a Begin of ImGui and
// imgui::Push, or between imgui::CheckPop and the End of ImGui + imgui::Pop. Not bound (4.2): Begin/End of the window
// (the host), the demo and metrics windows, reading the clipboard (SetClipboardText - write only), URLs, the ini
// settings, docking, fonts, the global style, logging; v2: DrawList, Image, the sort specs of tables, the keys.

#include "addon_imgui_lua.h"
#include "addon_imgui_core.h"

#include "xrScriptEngine/script_engine.hpp"

#include <imgui_internal.h>

#include <cfloat>
#include <climits>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace gw::addons::imgui_lua
{
namespace
{
using imgui::EScope;

// ---------------------------------------------------------------------------------------------
// The checks of every function
// ---------------------------------------------------------------------------------------------

// The first line of every widget: a widget runs only inside draw of a tool (there is an ImGui frame and a window).
// fn_name - the name for the errors and the warnings of the function
#define GW_UI_ENTER(L, name)                                                                                           \
    const pcstr fn_name = (name);                                                                                      \
    if (!imgui::InDraw())                                                                                              \
    return luaL_error(L, "ui.%s: only inside draw of an ImGui tool", fn_name)

// Before a Begin* / Push* of ImGui: the shadow stack has room
#define GW_UI_CAN_PUSH(L)                                                                                              \
    if (const pcstr push_error = imgui::CanPush())                                                                     \
    return luaL_error(L, "ui.%s: %s", fn_name, push_error)

// The text of CheckPop for "ui.<fn>: <text>": a text that starts with "<fn>: " loses that part
pcstr PopErrorText(pcstr fn_name, pcstr error)
{
    const size_t length = xr_strlen(fn_name);
    const bool prefixed = strncmp(error, fn_name, length) == 0 && error[length] == ':' && error[length + 1] == ' ';
    return prefixed ? error + length + 2 : error;
}

// Before an End* / Pop* of ImGui: the top of the shadow stack is its pair
#define GW_UI_CHECK_POP(L, scope)                                                                                      \
    if (const pcstr pop_error = imgui::CheckPop(scope))                                                                \
    return luaL_error(L, "ui.%s: %s", fn_name, PopErrorText(fn_name, pop_error))

constexpr u32 ScopeBit(EScope scope) { return 1u << static_cast<u32>(scope); }

// The pairs that change the window (or the layout) the next widgets go to: a table function or a tab item must have
// its table / tab bar as the innermost of them
constexpr u32 kWindowScopes = ScopeBit(EScope::Child) | ScopeBit(EScope::Combo) | ScopeBit(EScope::ListBox) |
    ScopeBit(EScope::Menu) | ScopeBit(EScope::MenuBar) | ScopeBit(EScope::Popup) | ScopeBit(EScope::Tooltip) |
    ScopeBit(EScope::Table);

// The innermost open pair among `mask` (bits of ScopeBit); EScope::Count - none
EScope Innermost(u32 mask)
{
    const imgui::DrawFrame* frame = imgui::Current();
    if (!frame)
        return EScope::Count;
    for (u32 i = frame->depth; i-- > 0;)
    {
        if (mask & ScopeBit(frame->stack[i]))
            return frame->stack[i];
    }
    return EScope::Count;
}

// A table function (TableNextRow, TableSetupColumn...): its table must be the innermost window-like pair. nullptr - yes
pcstr TableScopeError()
{
    const EScope innermost = Innermost(kWindowScopes);
    if (innermost == EScope::Table && ImGui::GetCurrentTable())
        return nullptr;
    if (innermost == EScope::Count)
        return "only between BeginTable (that returned true) and EndTable";
    static string256 text;
    xr_sprintf(text, "only between BeginTable and EndTable: the innermost open pair is %s - close it with %s first",
        imgui::ScopeBeginName(innermost), imgui::ScopeEndName(innermost));
    return text;
}

#define GW_UI_REQUIRE_TABLE(L)                                                                                         \
    if (const pcstr table_error = TableScopeError())                                                                   \
    return luaL_error(L, "ui.%s: %s", fn_name, table_error)

// ---------------------------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------------------------

// Lua numbers to C: NaN - the fallback; clamped (a cast of an out-of-range double to int is undefined)
float ToFloat(lua_Number value, float fallback)
{
    if (!(value == value))
        return fallback;
    if (value > FLT_MAX)
        return FLT_MAX;
    if (value < -FLT_MAX)
        return -FLT_MAX;
    return static_cast<float>(value);
}

int ToInt(lua_Number value, int fallback)
{
    if (!(value == value))
        return fallback;
    if (value >= static_cast<lua_Number>(INT_MAX))
        return INT_MAX;
    if (value <= static_cast<lua_Number>(INT_MIN))
        return INT_MIN;
    return static_cast<int>(value);
}

// 32 bits from Lua: bit.bor gives a signed 32-bit number, a literal 0xAARRGGBB is above INT_MAX - the same bits.
// Out of both ranges - all bits (the caller masks them and warns)
u32 ToBits(lua_Number value)
{
    if (!(value == value) || value >= 4294967296.0 || value < -2147483648.0)
        return type_max<u32>;
    return static_cast<u32>(static_cast<s64>(value));
}

float CheckFloat(lua_State* L, int index) { return ToFloat(luaL_checknumber(L, index), 0.f); }

float OptFloat(lua_State* L, int index, float fallback)
{
    return lua_isnoneornil(L, index) ? fallback : ToFloat(luaL_checknumber(L, index), fallback);
}

int CheckInt(lua_State* L, int index) { return ToInt(luaL_checknumber(L, index), 0); }

int OptInt(lua_State* L, int index, int fallback)
{
    return lua_isnoneornil(L, index) ? fallback : ToInt(luaL_checknumber(L, index), fallback);
}

bool OptBool(lua_State* L, int index, bool fallback)
{
    return lua_isnoneornil(L, index) ? fallback : lua_toboolean(L, index) != 0;
}

pcstr OptString(lua_State* L, int index) { return lua_isnoneornil(L, index) ? nullptr : luaL_checkstring(L, index); }

// The length of a UTF-8 text cut to at most `limit` bytes without splitting a character
size_t Utf8Cut(pcstr text, size_t length, size_t limit)
{
    if (length <= limit)
        return length;
    size_t cut = limit;
    while (cut > 0 && (static_cast<u8>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return cut;
}

// A label (the visible text and the id of a widget: "Text##id"): a string (a number is converted, as tostring would),
// cut to kMaxLabelLength. A cut label lives in one of four buffers: enough for the labels of one call
char g_labels[4][imgui::kMaxLabelLength + 1];
u32 g_label_next = 0;

pcstr CutLabel(pcstr fn_name, pcstr text, size_t length)
{
    if (length <= imgui::kMaxLabelLength)
        return text;
    imgui::WarnOnce(imgui::kWarnTruncated, "ui.%s: a label of %u bytes is cut to %u (this warning is shown once)",
        fn_name, static_cast<u32>(length), imgui::kMaxLabelLength);
    char* buffer = g_labels[g_label_next++ % std::size(g_labels)];
    const size_t cut = Utf8Cut(text, length, imgui::kMaxLabelLength);
    memcpy(buffer, text, cut);
    buffer[cut] = 0;
    return buffer;
}

pcstr CheckLabel(lua_State* L, int index, pcstr fn_name)
{
    size_t length = 0;
    const pcstr text = luaL_checklstring(L, index, &length);
    return CutLabel(fn_name, text, length);
}

// nil - nullptr
pcstr OptLabel(lua_State* L, int index, pcstr fn_name)
{
    if (lua_isnoneornil(L, index))
        return nullptr;
    return CheckLabel(L, index, fn_name);
}

// A text shown as it is (never a printf format: formatting is the business of Lua), cut to kMaxTextLength
struct Text
{
    pcstr data;
    int length;
};

Text CutText(pcstr fn_name, pcstr text, size_t length)
{
    if (length > imgui::kMaxTextLength)
    {
        imgui::WarnOnce(imgui::kWarnTruncated, "ui.%s: a text of %u bytes is cut to %u (this warning is shown once)",
            fn_name, static_cast<u32>(length), imgui::kMaxTextLength);
        length = Utf8Cut(text, length, imgui::kMaxTextLength);
    }
    return { text, static_cast<int>(length) };
}

Text CheckText(lua_State* L, int index, pcstr fn_name)
{
    size_t length = 0;
    const pcstr text = luaL_checklstring(L, index, &length);
    return CutText(fn_name, text, length);
}

// Flags from Lua: only the bits of `allowed` (the names of the constant table) - other bits are dropped with a warning
// once per tool (a flag dangerous for the frame of the host, an internal flag, a typo)
int OptFlags(lua_State* L, int index, int allowed, pcstr fn_name)
{
    if (lua_isnoneornil(L, index))
        return 0;
    const u32 bits = ToBits(luaL_checknumber(L, index));
    const u32 masked = bits & static_cast<u32>(allowed);
    if (masked != bits)
    {
        imgui::WarnOnce(imgui::kWarnFlags, "ui.%s: the flags 0x%X are not supported here and are ignored (this "
            "warning is shown once)", fn_name, bits & ~static_cast<u32>(allowed));
    }
    return static_cast<int>(masked);
}

// A group of flags where only one may be set (ImGui asserts on more): the lowest one stays
int OneOf(int flags, int group, pcstr fn_name)
{
    const int set = flags & group;
    if (set == 0 || (set & (set - 1)) == 0)
        return flags;
    imgui::WarnOnce(imgui::kWarnArgs, "ui.%s: only one flag of the group 0x%X may be set, 0x%X is used (this warning "
        "is shown once)", fn_name, static_cast<u32>(group), static_cast<u32>(set & -set));
    return (flags & ~group) | (set & -set);
}

void WarnArgs(pcstr fn_name, pcstr what)
{
    imgui::WarnOnce(imgui::kWarnArgs, "ui.%s: %s (this warning is shown once)", fn_name, what);
}

// Colors: a number 0xAARRGGBB (as level.draw_* and the draw_* of the Plugin API) or a table {r, g, b[, a]} of 0..1
ImVec4 ArgbToColor(u32 argb)
{
    return ImVec4(static_cast<float>((argb >> 16) & 0xFF) / 255.f, static_cast<float>((argb >> 8) & 0xFF) / 255.f,
        static_cast<float>(argb & 0xFF) / 255.f, static_cast<float>((argb >> 24) & 0xFF) / 255.f);
}

u32 ColorToArgb(const ImVec4& color)
{
    const auto channel = [](float value) {
        if (!(value == value) || value < 0.f)
            value = 0.f;
        if (value > 1.f)
            value = 1.f;
        return static_cast<u32>(value * 255.f + 0.5f);
    };
    return (channel(color.w) << 24) | (channel(color.x) << 16) | (channel(color.y) << 8) | channel(color.z);
}

void ReadColorTable(lua_State* L, int index, float color[4])
{
    for (int i = 0; i < 4; ++i)
    {
        lua_rawgeti(L, index, i + 1);
        color[i] = lua_type(L, -1) == LUA_TNUMBER ? ToFloat(lua_tonumber(L, -1), 0.f) : (i == 3 ? 1.f : 0.f);
        lua_pop(L, 1);
    }
}

ImVec4 CheckColor(lua_State* L, int index)
{
    if (lua_type(L, index) == LUA_TNUMBER)
        return ArgbToColor(ToBits(lua_tonumber(L, index)));
    if (lua_type(L, index) == LUA_TTABLE)
    {
        float color[4];
        ReadColorTable(L, index, color);
        return ImVec4(color[0], color[1], color[2], color[3]);
    }
    luaL_argerror(L, index, "a color: a number 0xAARRGGBB or a table {r, g, b[, a]} of 0..1");
    return ImVec4();
}

void PushArgb(lua_State* L, u32 argb) { lua_pushnumber(L, static_cast<lua_Number>(argb)); }

// ---------------------------------------------------------------------------------------------
// The constant tables: ui.<Group>.<Name> = the value of ImGui<Group>_<Name> of this engine. Only the names safe for the
// window of the host; the bits of the table are what a function of the group accepts (OptFlags)
// ---------------------------------------------------------------------------------------------
struct Constant
{
    pcstr name;
    int value;
};

template <size_t N>
constexpr int MaskOf(const Constant (&constants)[N])
{
    int mask = 0;
    for (const Constant& constant : constants)
        mask |= constant.value;
    return mask;
}

#define GW_C(prefix, name) { #name, prefix##name }

// BeginChild (window_flags), BeginPopup, BeginPopupModal. No NoNav / NoInputs: they contain NoNavFocus (denied)
constexpr Constant kWindowFlags[] = {
    GW_C(ImGuiWindowFlags_, None), GW_C(ImGuiWindowFlags_, NoTitleBar), GW_C(ImGuiWindowFlags_, NoResize),
    GW_C(ImGuiWindowFlags_, NoMove), GW_C(ImGuiWindowFlags_, NoScrollbar), GW_C(ImGuiWindowFlags_, NoScrollWithMouse),
    GW_C(ImGuiWindowFlags_, NoCollapse), GW_C(ImGuiWindowFlags_, AlwaysAutoResize), GW_C(ImGuiWindowFlags_, NoBackground),
    GW_C(ImGuiWindowFlags_, NoMouseInputs), GW_C(ImGuiWindowFlags_, MenuBar), GW_C(ImGuiWindowFlags_, HorizontalScrollbar),
    GW_C(ImGuiWindowFlags_, AlwaysVerticalScrollbar), GW_C(ImGuiWindowFlags_, AlwaysHorizontalScrollbar),
    GW_C(ImGuiWindowFlags_, NoNavInputs), GW_C(ImGuiWindowFlags_, UnsavedDocument), GW_C(ImGuiWindowFlags_, NoDecoration),
};
constexpr int kWindowFlagsAllowed = MaskOf(kWindowFlags);
static_assert((kWindowFlagsAllowed & imgui::kWindowFlagsDenied) == 0, "ui.WindowFlags: a denied flag in the table");

constexpr Constant kChildFlags[] = {
    GW_C(ImGuiChildFlags_, None), GW_C(ImGuiChildFlags_, Borders), GW_C(ImGuiChildFlags_, AlwaysUseWindowPadding),
    GW_C(ImGuiChildFlags_, ResizeX), GW_C(ImGuiChildFlags_, ResizeY), GW_C(ImGuiChildFlags_, AutoResizeX),
    GW_C(ImGuiChildFlags_, AutoResizeY), GW_C(ImGuiChildFlags_, AlwaysAutoResize), GW_C(ImGuiChildFlags_, FrameStyle),
    GW_C(ImGuiChildFlags_, NavFlattened),
};

constexpr Constant kTableFlags[] = {
    GW_C(ImGuiTableFlags_, None), GW_C(ImGuiTableFlags_, Resizable), GW_C(ImGuiTableFlags_, Reorderable),
    GW_C(ImGuiTableFlags_, Hideable), GW_C(ImGuiTableFlags_, Sortable), GW_C(ImGuiTableFlags_, NoSavedSettings),
    GW_C(ImGuiTableFlags_, ContextMenuInBody), GW_C(ImGuiTableFlags_, RowBg), GW_C(ImGuiTableFlags_, BordersInnerH),
    GW_C(ImGuiTableFlags_, BordersOuterH), GW_C(ImGuiTableFlags_, BordersInnerV), GW_C(ImGuiTableFlags_, BordersOuterV),
    GW_C(ImGuiTableFlags_, BordersH), GW_C(ImGuiTableFlags_, BordersV), GW_C(ImGuiTableFlags_, BordersInner),
    GW_C(ImGuiTableFlags_, BordersOuter), GW_C(ImGuiTableFlags_, Borders), GW_C(ImGuiTableFlags_, NoBordersInBody),
    GW_C(ImGuiTableFlags_, NoBordersInBodyUntilResize), GW_C(ImGuiTableFlags_, SizingFixedFit),
    GW_C(ImGuiTableFlags_, SizingFixedSame), GW_C(ImGuiTableFlags_, SizingStretchProp),
    GW_C(ImGuiTableFlags_, SizingStretchSame), GW_C(ImGuiTableFlags_, NoHostExtendX), GW_C(ImGuiTableFlags_, NoHostExtendY),
    GW_C(ImGuiTableFlags_, NoKeepColumnsVisible), GW_C(ImGuiTableFlags_, PreciseWidths), GW_C(ImGuiTableFlags_, NoClip),
    GW_C(ImGuiTableFlags_, PadOuterX), GW_C(ImGuiTableFlags_, NoPadOuterX), GW_C(ImGuiTableFlags_, NoPadInnerX),
    GW_C(ImGuiTableFlags_, ScrollX), GW_C(ImGuiTableFlags_, ScrollY), GW_C(ImGuiTableFlags_, SortMulti),
    GW_C(ImGuiTableFlags_, SortTristate), GW_C(ImGuiTableFlags_, HighlightHoveredColumn),
};

// TableSetupColumn: no status flags (IsEnabled... - ImGui asserts on them)
constexpr Constant kTableColumnFlags[] = {
    GW_C(ImGuiTableColumnFlags_, None), GW_C(ImGuiTableColumnFlags_, Disabled), GW_C(ImGuiTableColumnFlags_, DefaultHide),
    GW_C(ImGuiTableColumnFlags_, DefaultSort), GW_C(ImGuiTableColumnFlags_, WidthStretch),
    GW_C(ImGuiTableColumnFlags_, WidthFixed), GW_C(ImGuiTableColumnFlags_, NoResize), GW_C(ImGuiTableColumnFlags_, NoReorder),
    GW_C(ImGuiTableColumnFlags_, NoHide), GW_C(ImGuiTableColumnFlags_, NoClip), GW_C(ImGuiTableColumnFlags_, NoSort),
    GW_C(ImGuiTableColumnFlags_, NoSortAscending), GW_C(ImGuiTableColumnFlags_, NoSortDescending),
    GW_C(ImGuiTableColumnFlags_, NoHeaderLabel), GW_C(ImGuiTableColumnFlags_, NoHeaderWidth),
    GW_C(ImGuiTableColumnFlags_, PreferSortAscending), GW_C(ImGuiTableColumnFlags_, PreferSortDescending),
    GW_C(ImGuiTableColumnFlags_, IndentEnable), GW_C(ImGuiTableColumnFlags_, IndentDisable),
    GW_C(ImGuiTableColumnFlags_, AngledHeader),
};

constexpr Constant kTableRowFlags[] = { GW_C(ImGuiTableRowFlags_, None), GW_C(ImGuiTableRowFlags_, Headers) };

constexpr Constant kTreeNodeFlags[] = {
    GW_C(ImGuiTreeNodeFlags_, None), GW_C(ImGuiTreeNodeFlags_, Selected), GW_C(ImGuiTreeNodeFlags_, Framed),
    GW_C(ImGuiTreeNodeFlags_, AllowOverlap), GW_C(ImGuiTreeNodeFlags_, NoTreePushOnOpen),
    GW_C(ImGuiTreeNodeFlags_, NoAutoOpenOnLog), GW_C(ImGuiTreeNodeFlags_, DefaultOpen),
    GW_C(ImGuiTreeNodeFlags_, OpenOnDoubleClick), GW_C(ImGuiTreeNodeFlags_, OpenOnArrow), GW_C(ImGuiTreeNodeFlags_, Leaf),
    GW_C(ImGuiTreeNodeFlags_, Bullet), GW_C(ImGuiTreeNodeFlags_, FramePadding), GW_C(ImGuiTreeNodeFlags_, SpanAvailWidth),
    GW_C(ImGuiTreeNodeFlags_, SpanFullWidth), GW_C(ImGuiTreeNodeFlags_, SpanLabelWidth),
    GW_C(ImGuiTreeNodeFlags_, SpanAllColumns), GW_C(ImGuiTreeNodeFlags_, LabelSpanAllColumns),
    GW_C(ImGuiTreeNodeFlags_, NavLeftJumpsToParent), GW_C(ImGuiTreeNodeFlags_, CollapsingHeader),
    GW_C(ImGuiTreeNodeFlags_, DrawLinesNone), GW_C(ImGuiTreeNodeFlags_, DrawLinesFull),
    GW_C(ImGuiTreeNodeFlags_, DrawLinesToNodes),
};

// InputText*: no Callback* (there is no callback), no Multiline (InputTextMultiline sets it)
constexpr Constant kInputTextFlags[] = {
    GW_C(ImGuiInputTextFlags_, None), GW_C(ImGuiInputTextFlags_, CharsDecimal), GW_C(ImGuiInputTextFlags_, CharsHexadecimal),
    GW_C(ImGuiInputTextFlags_, CharsScientific), GW_C(ImGuiInputTextFlags_, CharsUppercase),
    GW_C(ImGuiInputTextFlags_, CharsNoBlank), GW_C(ImGuiInputTextFlags_, AllowTabInput),
    GW_C(ImGuiInputTextFlags_, EnterReturnsTrue), GW_C(ImGuiInputTextFlags_, EscapeClearsAll),
    GW_C(ImGuiInputTextFlags_, CtrlEnterForNewLine), GW_C(ImGuiInputTextFlags_, ReadOnly), GW_C(ImGuiInputTextFlags_, Password),
    GW_C(ImGuiInputTextFlags_, AlwaysOverwrite), GW_C(ImGuiInputTextFlags_, AutoSelectAll),
    GW_C(ImGuiInputTextFlags_, ParseEmptyRefVal), GW_C(ImGuiInputTextFlags_, DisplayEmptyRefVal),
    GW_C(ImGuiInputTextFlags_, NoHorizontalScroll), GW_C(ImGuiInputTextFlags_, NoUndoRedo),
    GW_C(ImGuiInputTextFlags_, ElideLeft), GW_C(ImGuiInputTextFlags_, WordWrap),
};
// InputFloat* / InputInt (InputScalar asserts on EnterReturnsTrue; the text-only flags make no sense there)
constexpr int kInputScalarAllowed = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsScientific |
    ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AlwaysOverwrite |
    ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ParseEmptyRefVal | ImGuiInputTextFlags_DisplayEmptyRefVal |
    ImGuiInputTextFlags_NoHorizontalScroll | ImGuiInputTextFlags_NoUndoRedo;

constexpr Constant kSliderFlags[] = {
    GW_C(ImGuiSliderFlags_, None), GW_C(ImGuiSliderFlags_, Logarithmic), GW_C(ImGuiSliderFlags_, NoRoundToFormat),
    GW_C(ImGuiSliderFlags_, NoInput), GW_C(ImGuiSliderFlags_, WrapAround), GW_C(ImGuiSliderFlags_, ClampOnInput),
    GW_C(ImGuiSliderFlags_, ClampZeroRange), GW_C(ImGuiSliderFlags_, NoSpeedTweaks), GW_C(ImGuiSliderFlags_, AlwaysClamp),
};
// Slider*: WrapAround only works for Drag* (ImGui asserts)
constexpr int kSliderAllowed = MaskOf(kSliderFlags) & ~ImGuiSliderFlags_WrapAround;

constexpr Constant kColorEditFlags[] = {
    GW_C(ImGuiColorEditFlags_, None), GW_C(ImGuiColorEditFlags_, NoAlpha), GW_C(ImGuiColorEditFlags_, NoPicker),
    GW_C(ImGuiColorEditFlags_, NoOptions), GW_C(ImGuiColorEditFlags_, NoSmallPreview), GW_C(ImGuiColorEditFlags_, NoInputs),
    GW_C(ImGuiColorEditFlags_, NoTooltip), GW_C(ImGuiColorEditFlags_, NoLabel), GW_C(ImGuiColorEditFlags_, NoSidePreview),
    GW_C(ImGuiColorEditFlags_, NoDragDrop), GW_C(ImGuiColorEditFlags_, NoBorder), GW_C(ImGuiColorEditFlags_, AlphaOpaque),
    GW_C(ImGuiColorEditFlags_, AlphaNoBg), GW_C(ImGuiColorEditFlags_, AlphaPreviewHalf), GW_C(ImGuiColorEditFlags_, AlphaBar),
    GW_C(ImGuiColorEditFlags_, HDR), GW_C(ImGuiColorEditFlags_, DisplayRGB), GW_C(ImGuiColorEditFlags_, DisplayHSV),
    GW_C(ImGuiColorEditFlags_, DisplayHex), GW_C(ImGuiColorEditFlags_, Uint8), GW_C(ImGuiColorEditFlags_, Float),
    GW_C(ImGuiColorEditFlags_, PickerHueBar), GW_C(ImGuiColorEditFlags_, PickerHueWheel),
    GW_C(ImGuiColorEditFlags_, InputRGB), GW_C(ImGuiColorEditFlags_, InputHSV),
};

constexpr Constant kSelectableFlags[] = {
    GW_C(ImGuiSelectableFlags_, None), GW_C(ImGuiSelectableFlags_, NoAutoClosePopups),
    GW_C(ImGuiSelectableFlags_, SpanAllColumns), GW_C(ImGuiSelectableFlags_, AllowDoubleClick),
    GW_C(ImGuiSelectableFlags_, Disabled), GW_C(ImGuiSelectableFlags_, AllowOverlap), GW_C(ImGuiSelectableFlags_, Highlight),
    GW_C(ImGuiSelectableFlags_, SelectOnNav),
};

constexpr Constant kComboFlags[] = {
    GW_C(ImGuiComboFlags_, None), GW_C(ImGuiComboFlags_, PopupAlignLeft), GW_C(ImGuiComboFlags_, HeightSmall),
    GW_C(ImGuiComboFlags_, HeightRegular), GW_C(ImGuiComboFlags_, HeightLarge), GW_C(ImGuiComboFlags_, HeightLargest),
    GW_C(ImGuiComboFlags_, NoArrowButton), GW_C(ImGuiComboFlags_, NoPreview), GW_C(ImGuiComboFlags_, WidthFitPreview),
};

constexpr Constant kTabBarFlags[] = {
    GW_C(ImGuiTabBarFlags_, None), GW_C(ImGuiTabBarFlags_, Reorderable), GW_C(ImGuiTabBarFlags_, AutoSelectNewTabs),
    GW_C(ImGuiTabBarFlags_, TabListPopupButton), GW_C(ImGuiTabBarFlags_, NoCloseWithMiddleMouseButton),
    GW_C(ImGuiTabBarFlags_, NoTabListScrollingButtons), GW_C(ImGuiTabBarFlags_, NoTooltip),
    GW_C(ImGuiTabBarFlags_, DrawSelectedOverline), GW_C(ImGuiTabBarFlags_, FittingPolicyMixed),
    GW_C(ImGuiTabBarFlags_, FittingPolicyShrink), GW_C(ImGuiTabBarFlags_, FittingPolicyScroll),
};

constexpr Constant kTabItemFlags[] = {
    GW_C(ImGuiTabItemFlags_, None), GW_C(ImGuiTabItemFlags_, UnsavedDocument), GW_C(ImGuiTabItemFlags_, SetSelected),
    GW_C(ImGuiTabItemFlags_, NoCloseWithMiddleMouseButton), GW_C(ImGuiTabItemFlags_, NoPushId),
    GW_C(ImGuiTabItemFlags_, NoTooltip), GW_C(ImGuiTabItemFlags_, NoReorder), GW_C(ImGuiTabItemFlags_, Leading),
    GW_C(ImGuiTabItemFlags_, Trailing), GW_C(ImGuiTabItemFlags_, NoAssumedClosure),
};

constexpr Constant kCond[] = {
    GW_C(ImGuiCond_, None), GW_C(ImGuiCond_, Always), GW_C(ImGuiCond_, Once), GW_C(ImGuiCond_, FirstUseEver),
    GW_C(ImGuiCond_, Appearing),
};

constexpr Constant kHoveredFlags[] = {
    GW_C(ImGuiHoveredFlags_, None), GW_C(ImGuiHoveredFlags_, ChildWindows), GW_C(ImGuiHoveredFlags_, RootWindow),
    GW_C(ImGuiHoveredFlags_, AnyWindow), GW_C(ImGuiHoveredFlags_, NoPopupHierarchy),
    GW_C(ImGuiHoveredFlags_, AllowWhenBlockedByPopup), GW_C(ImGuiHoveredFlags_, AllowWhenBlockedByActiveItem),
    GW_C(ImGuiHoveredFlags_, AllowWhenOverlappedByItem), GW_C(ImGuiHoveredFlags_, AllowWhenOverlappedByWindow),
    GW_C(ImGuiHoveredFlags_, AllowWhenDisabled), GW_C(ImGuiHoveredFlags_, NoNavOverride),
    GW_C(ImGuiHoveredFlags_, AllowWhenOverlapped), GW_C(ImGuiHoveredFlags_, RectOnly),
    GW_C(ImGuiHoveredFlags_, RootAndChildWindows), GW_C(ImGuiHoveredFlags_, ForTooltip), GW_C(ImGuiHoveredFlags_, Stationary),
    GW_C(ImGuiHoveredFlags_, DelayNone), GW_C(ImGuiHoveredFlags_, DelayShort), GW_C(ImGuiHoveredFlags_, DelayNormal),
    GW_C(ImGuiHoveredFlags_, NoSharedDelay),
};

constexpr Constant kFocusedFlags[] = {
    GW_C(ImGuiFocusedFlags_, None), GW_C(ImGuiFocusedFlags_, ChildWindows), GW_C(ImGuiFocusedFlags_, RootWindow),
    GW_C(ImGuiFocusedFlags_, AnyWindow), GW_C(ImGuiFocusedFlags_, NoPopupHierarchy),
    GW_C(ImGuiFocusedFlags_, RootAndChildWindows),
};

constexpr Constant kPopupFlags[] = {
    GW_C(ImGuiPopupFlags_, None), GW_C(ImGuiPopupFlags_, MouseButtonLeft), GW_C(ImGuiPopupFlags_, MouseButtonRight),
    GW_C(ImGuiPopupFlags_, MouseButtonMiddle), GW_C(ImGuiPopupFlags_, NoReopen),
    GW_C(ImGuiPopupFlags_, NoOpenOverExistingPopup), GW_C(ImGuiPopupFlags_, NoOpenOverItems),
    GW_C(ImGuiPopupFlags_, AnyPopupId), GW_C(ImGuiPopupFlags_, AnyPopupLevel), GW_C(ImGuiPopupFlags_, AnyPopup),
};

constexpr Constant kButtonFlags[] = {
    GW_C(ImGuiButtonFlags_, None), GW_C(ImGuiButtonFlags_, MouseButtonLeft), GW_C(ImGuiButtonFlags_, MouseButtonRight),
    GW_C(ImGuiButtonFlags_, MouseButtonMiddle), GW_C(ImGuiButtonFlags_, EnableNav),
};

constexpr Constant kDir[] = {
    GW_C(ImGuiDir_, Left), GW_C(ImGuiDir_, Right), GW_C(ImGuiDir_, Up), GW_C(ImGuiDir_, Down),
};

constexpr Constant kMouseButton[] = {
    GW_C(ImGuiMouseButton_, Left), GW_C(ImGuiMouseButton_, Right), GW_C(ImGuiMouseButton_, Middle),
};

constexpr Constant kCol[] = {
    GW_C(ImGuiCol_, Text), GW_C(ImGuiCol_, TextDisabled), GW_C(ImGuiCol_, WindowBg), GW_C(ImGuiCol_, ChildBg),
    GW_C(ImGuiCol_, PopupBg), GW_C(ImGuiCol_, Border), GW_C(ImGuiCol_, BorderShadow), GW_C(ImGuiCol_, FrameBg),
    GW_C(ImGuiCol_, FrameBgHovered), GW_C(ImGuiCol_, FrameBgActive), GW_C(ImGuiCol_, TitleBg), GW_C(ImGuiCol_, TitleBgActive),
    GW_C(ImGuiCol_, TitleBgCollapsed), GW_C(ImGuiCol_, MenuBarBg), GW_C(ImGuiCol_, ScrollbarBg), GW_C(ImGuiCol_, ScrollbarGrab),
    GW_C(ImGuiCol_, ScrollbarGrabHovered), GW_C(ImGuiCol_, ScrollbarGrabActive), GW_C(ImGuiCol_, CheckMark),
    GW_C(ImGuiCol_, SliderGrab), GW_C(ImGuiCol_, SliderGrabActive), GW_C(ImGuiCol_, Button), GW_C(ImGuiCol_, ButtonHovered),
    GW_C(ImGuiCol_, ButtonActive), GW_C(ImGuiCol_, Header), GW_C(ImGuiCol_, HeaderHovered), GW_C(ImGuiCol_, HeaderActive),
    GW_C(ImGuiCol_, Separator), GW_C(ImGuiCol_, SeparatorHovered), GW_C(ImGuiCol_, SeparatorActive),
    GW_C(ImGuiCol_, ResizeGrip), GW_C(ImGuiCol_, ResizeGripHovered), GW_C(ImGuiCol_, ResizeGripActive),
    GW_C(ImGuiCol_, InputTextCursor), GW_C(ImGuiCol_, TabHovered), GW_C(ImGuiCol_, Tab), GW_C(ImGuiCol_, TabSelected),
    GW_C(ImGuiCol_, TabSelectedOverline), GW_C(ImGuiCol_, TabDimmed), GW_C(ImGuiCol_, TabDimmedSelected),
    GW_C(ImGuiCol_, TabDimmedSelectedOverline), GW_C(ImGuiCol_, PlotLines), GW_C(ImGuiCol_, PlotLinesHovered),
    GW_C(ImGuiCol_, PlotHistogram), GW_C(ImGuiCol_, PlotHistogramHovered), GW_C(ImGuiCol_, TableHeaderBg),
    GW_C(ImGuiCol_, TableBorderStrong), GW_C(ImGuiCol_, TableBorderLight), GW_C(ImGuiCol_, TableRowBg),
    GW_C(ImGuiCol_, TableRowBgAlt), GW_C(ImGuiCol_, TextLink), GW_C(ImGuiCol_, TextSelectedBg), GW_C(ImGuiCol_, TreeLines),
    GW_C(ImGuiCol_, DragDropTarget), GW_C(ImGuiCol_, NavCursor), GW_C(ImGuiCol_, NavWindowingHighlight),
    GW_C(ImGuiCol_, NavWindowingDimBg), GW_C(ImGuiCol_, ModalWindowDimBg),
};

constexpr Constant kStyleVar[] = {
    GW_C(ImGuiStyleVar_, Alpha), GW_C(ImGuiStyleVar_, DisabledAlpha), GW_C(ImGuiStyleVar_, WindowPadding),
    GW_C(ImGuiStyleVar_, WindowRounding), GW_C(ImGuiStyleVar_, WindowBorderSize), GW_C(ImGuiStyleVar_, WindowMinSize),
    GW_C(ImGuiStyleVar_, WindowTitleAlign), GW_C(ImGuiStyleVar_, ChildRounding), GW_C(ImGuiStyleVar_, ChildBorderSize),
    GW_C(ImGuiStyleVar_, PopupRounding), GW_C(ImGuiStyleVar_, PopupBorderSize), GW_C(ImGuiStyleVar_, FramePadding),
    GW_C(ImGuiStyleVar_, FrameRounding), GW_C(ImGuiStyleVar_, FrameBorderSize), GW_C(ImGuiStyleVar_, ItemSpacing),
    GW_C(ImGuiStyleVar_, ItemInnerSpacing), GW_C(ImGuiStyleVar_, IndentSpacing), GW_C(ImGuiStyleVar_, CellPadding),
    GW_C(ImGuiStyleVar_, ScrollbarSize), GW_C(ImGuiStyleVar_, ScrollbarRounding), GW_C(ImGuiStyleVar_, GrabMinSize),
    GW_C(ImGuiStyleVar_, GrabRounding), GW_C(ImGuiStyleVar_, TabRounding), GW_C(ImGuiStyleVar_, TabBorderSize),
    GW_C(ImGuiStyleVar_, TabBarBorderSize), GW_C(ImGuiStyleVar_, ButtonTextAlign), GW_C(ImGuiStyleVar_, SelectableTextAlign),
    GW_C(ImGuiStyleVar_, SeparatorTextBorderSize), GW_C(ImGuiStyleVar_, SeparatorTextAlign),
    GW_C(ImGuiStyleVar_, SeparatorTextPadding),
};

#undef GW_C

constexpr int kChildFlagsAllowed = MaskOf(kChildFlags);
constexpr int kTableFlagsAllowed = MaskOf(kTableFlags);
constexpr int kTableColumnFlagsAllowed = MaskOf(kTableColumnFlags);
constexpr int kTableRowFlagsAllowed = MaskOf(kTableRowFlags);
constexpr int kTreeNodeFlagsAllowed = MaskOf(kTreeNodeFlags);
constexpr int kInputTextAllowed = MaskOf(kInputTextFlags);
constexpr int kDragAllowed = MaskOf(kSliderFlags);
constexpr int kColorEditAllowed = MaskOf(kColorEditFlags);
constexpr int kSelectableAllowed = MaskOf(kSelectableFlags);
constexpr int kComboAllowed = MaskOf(kComboFlags);
constexpr int kTabBarAllowed = MaskOf(kTabBarFlags);
constexpr int kTabItemAllowed = MaskOf(kTabItemFlags);
constexpr int kCondAllowed = MaskOf(kCond);
constexpr int kFocusedAllowed = MaskOf(kFocusedFlags);
constexpr int kPopupAllowed = MaskOf(kPopupFlags);
constexpr int kButtonAllowed = MaskOf(kButtonFlags);
constexpr int kItemHoveredAllowed = MaskOf(kHoveredFlags) & ImGuiHoveredFlags_AllowedMaskForIsItemHovered;
constexpr int kWindowHoveredAllowed = MaskOf(kHoveredFlags) & ImGuiHoveredFlags_AllowedMaskForIsWindowHovered;

struct ConstantTable
{
    pcstr name;
    const Constant* constants;
    size_t count;
};

#define GW_T(name, array) { name, array, std::size(array) }
constexpr ConstantTable kConstantTables[] = {
    GW_T("WindowFlags", kWindowFlags), GW_T("ChildFlags", kChildFlags), GW_T("TableFlags", kTableFlags),
    GW_T("TableColumnFlags", kTableColumnFlags), GW_T("TableRowFlags", kTableRowFlags),
    GW_T("TreeNodeFlags", kTreeNodeFlags), GW_T("InputTextFlags", kInputTextFlags), GW_T("SliderFlags", kSliderFlags),
    GW_T("ColorEditFlags", kColorEditFlags), GW_T("SelectableFlags", kSelectableFlags), GW_T("ComboFlags", kComboFlags),
    GW_T("TabBarFlags", kTabBarFlags), GW_T("TabItemFlags", kTabItemFlags), GW_T("Cond", kCond),
    GW_T("HoveredFlags", kHoveredFlags), GW_T("FocusedFlags", kFocusedFlags), GW_T("PopupFlags", kPopupFlags),
    GW_T("ButtonFlags", kButtonFlags), GW_T("Dir", kDir), GW_T("MouseButton", kMouseButton), GW_T("Col", kCol),
    GW_T("StyleVar", kStyleVar),
};
#undef GW_T

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

// ui.SameLine([offset_from_start_x, spacing]): the next widget on the same line
int UiSameLine(lua_State* L)
{
    GW_UI_ENTER(L, "SameLine");
    ImGui::SameLine(OptFloat(L, 1, 0.f), OptFloat(L, 2, -1.f));
    return 0;
}

// ui.NewLine()
int UiNewLine(lua_State* L)
{
    GW_UI_ENTER(L, "NewLine");
    ImGui::NewLine();
    return 0;
}

// ui.Spacing()
int UiSpacing(lua_State* L)
{
    GW_UI_ENTER(L, "Spacing");
    ImGui::Spacing();
    return 0;
}

// ui.Separator(): a horizontal line
int UiSeparator(lua_State* L)
{
    GW_UI_ENTER(L, "Separator");
    ImGui::Separator();
    return 0;
}

// ui.SeparatorText(label): a line with a text
int UiSeparatorText(lua_State* L)
{
    GW_UI_ENTER(L, "SeparatorText");
    ImGui::SeparatorText(CheckLabel(L, 1, fn_name));
    return 0;
}

// ui.Dummy(w, h): an empty item of a size
int UiDummy(lua_State* L)
{
    GW_UI_ENTER(L, "Dummy");
    ImGui::Dummy(ImVec2(CheckFloat(L, 1), CheckFloat(L, 2)));
    return 0;
}

// ui.Indent([w]): a pair - Unindent() gives the same width back (its own argument is not needed)
int UiIndent(lua_State* L)
{
    GW_UI_ENTER(L, "Indent");
    const float width = OptFloat(L, 1, 0.f);
    GW_UI_CAN_PUSH(L);
    ImGui::Indent(width);
    imgui::Push(EScope::Indent, width);
    return 0;
}

// ui.Unindent(): closes the last Indent with its width
int UiUnindent(lua_State* L)
{
    GW_UI_ENTER(L, "Unindent");
    GW_UI_CHECK_POP(L, EScope::Indent);
    ImGui::Unindent(imgui::TopArg());
    imgui::Pop();
    return 0;
}

// ui.BeginGroup() / ui.EndGroup(): EndGroup always
int UiBeginGroup(lua_State* L)
{
    GW_UI_ENTER(L, "BeginGroup");
    GW_UI_CAN_PUSH(L);
    ImGui::BeginGroup();
    imgui::Push(EScope::Group);
    return 0;
}

int UiEndGroup(lua_State* L)
{
    GW_UI_ENTER(L, "EndGroup");
    GW_UI_CHECK_POP(L, EScope::Group);
    ImGui::EndGroup();
    imgui::Pop();
    return 0;
}

// ui.BeginChild(str_id[, w, h, child_flags, window_flags]) -> visible. EndChild always, even after false.
// child_flags: ui.ChildFlags (true - Borders, as the old bool border of ImGui)
int UiBeginChild(lua_State* L)
{
    GW_UI_ENTER(L, "BeginChild");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const ImVec2 size(OptFloat(L, 2, 0.f), OptFloat(L, 3, 0.f));
    int child_flags = lua_type(L, 4) == LUA_TBOOLEAN ? (lua_toboolean(L, 4) ? ImGuiChildFlags_Borders : 0) :
                                                       OptFlags(L, 4, kChildFlagsAllowed, fn_name);
    int window_flags = OptFlags(L, 5, kWindowFlagsAllowed, fn_name);
    if (window_flags & ImGuiWindowFlags_AlwaysAutoResize)
    {
        WarnArgs(fn_name, "WindowFlags.AlwaysAutoResize is not for a child (ChildFlags.AlwaysAutoResize is), ignored");
        window_flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
    }
    if (child_flags & ImGuiChildFlags_AlwaysAutoResize)
    {
        if (child_flags & (ImGuiChildFlags_ResizeX | ImGuiChildFlags_ResizeY))
        {
            WarnArgs(fn_name, "ChildFlags.AlwaysAutoResize does not go with ResizeX / ResizeY, they are ignored");
            child_flags &= ~(ImGuiChildFlags_ResizeX | ImGuiChildFlags_ResizeY);
        }
        if (!(child_flags & (ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY)))
        {
            WarnArgs(fn_name, "ChildFlags.AlwaysAutoResize needs AutoResizeX or AutoResizeY, ignored");
            child_flags &= ~ImGuiChildFlags_AlwaysAutoResize;
        }
    }
    GW_UI_CAN_PUSH(L);
    const bool visible = ImGui::BeginChild(id, size, child_flags, window_flags);
    imgui::Push(EScope::Child);
    lua_pushboolean(L, visible);
    return 1;
}

int UiEndChild(lua_State* L)
{
    GW_UI_ENTER(L, "EndChild");
    GW_UI_CHECK_POP(L, EScope::Child);
    ImGui::EndChild();
    imgui::Pop();
    return 0;
}

// ui.SetNextItemWidth(w): the width of the next widget (< 0 - up to the right edge minus |w|)
int UiSetNextItemWidth(lua_State* L)
{
    GW_UI_ENTER(L, "SetNextItemWidth");
    ImGui::SetNextItemWidth(CheckFloat(L, 1));
    return 0;
}

// ui.PushItemWidth(w) / ui.PopItemWidth()
int UiPushItemWidth(lua_State* L)
{
    GW_UI_ENTER(L, "PushItemWidth");
    const float width = CheckFloat(L, 1);
    GW_UI_CAN_PUSH(L);
    ImGui::PushItemWidth(width);
    imgui::Push(EScope::ItemWidth);
    return 0;
}

int UiPopItemWidth(lua_State* L)
{
    GW_UI_ENTER(L, "PopItemWidth");
    GW_UI_CHECK_POP(L, EScope::ItemWidth);
    ImGui::PopItemWidth();
    imgui::Pop();
    return 0;
}

// ui.AlignTextToFramePadding(): the text of this line on the base line of the framed widgets
int UiAlignTextToFramePadding(lua_State* L)
{
    GW_UI_ENTER(L, "AlignTextToFramePadding");
    ImGui::AlignTextToFramePadding();
    return 0;
}

int PushVec2(lua_State* L, const ImVec2& value)
{
    lua_pushnumber(L, value.x);
    lua_pushnumber(L, value.y);
    return 2;
}

// ui.GetContentRegionAvail() -> w, h
int UiGetContentRegionAvail(lua_State* L)
{
    GW_UI_ENTER(L, "GetContentRegionAvail");
    return PushVec2(L, ImGui::GetContentRegionAvail());
}

// ui.GetWindowSize() -> w, h (the current window: the tool window, a child, a popup)
int UiGetWindowSize(lua_State* L)
{
    GW_UI_ENTER(L, "GetWindowSize");
    return PushVec2(L, ImGui::GetWindowSize());
}

// ui.GetCursorPos() -> x, y (in the window)
int UiGetCursorPos(lua_State* L)
{
    GW_UI_ENTER(L, "GetCursorPos");
    return PushVec2(L, ImGui::GetCursorPos());
}

// ui.SetCursorPos(x, y). The window grows by the widgets, not by the cursor: put a widget (Dummy) after it.
// ImGui asserts when End* follows a SetCursorPos beyond the content - the check is off for this call (the old behavior
// without the growth)
int UiSetCursorPos(lua_State* L)
{
    GW_UI_ENTER(L, "SetCursorPos");
    const ImVec2 pos(CheckFloat(L, 1), CheckFloat(L, 2));
    ImGui::SetCursorPos(pos);
    ImGui::GetCurrentWindow()->DC.IsSetPos = false;
    return 0;
}

// ui.CalcTextSize(text[, hide_text_after_double_hash, wrap_width]) -> w, h
int UiCalcTextSize(lua_State* L)
{
    GW_UI_ENTER(L, "CalcTextSize");
    const Text text = CheckText(L, 1, fn_name);
    const bool hide = OptBool(L, 2, false);
    const float wrap = OptFloat(L, 3, -1.f);
    return PushVec2(L, ImGui::CalcTextSize(text.data, text.data + text.length, hide, wrap));
}

// ui.GetTextLineHeight() -> h
int UiGetTextLineHeight(lua_State* L)
{
    GW_UI_ENTER(L, "GetTextLineHeight");
    lua_pushnumber(L, ImGui::GetTextLineHeight());
    return 1;
}

// ui.GetFrameHeight() -> h (of a framed widget: a button, an input)
int UiGetFrameHeight(lua_State* L)
{
    GW_UI_ENTER(L, "GetFrameHeight");
    lua_pushnumber(L, ImGui::GetFrameHeight());
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Text: shown as it is - never a printf format (format in Lua: strformat / string.format)
// ---------------------------------------------------------------------------------------------

// ui.Text(text)
int UiText(lua_State* L)
{
    GW_UI_ENTER(L, "Text");
    const Text text = CheckText(L, 1, fn_name);
    ImGui::TextUnformatted(text.data, text.data + text.length);
    return 0;
}

// ui.TextColored(color, text): color - 0xAARRGGBB or {r, g, b[, a]}
int UiTextColored(lua_State* L)
{
    GW_UI_ENTER(L, "TextColored");
    const ImVec4 color = CheckColor(L, 1);
    const Text text = CheckText(L, 2, fn_name);
    ImGui::TextColored(color, "%.*s", text.length, text.data);
    return 0;
}

// ui.TextDisabled(text)
int UiTextDisabled(lua_State* L)
{
    GW_UI_ENTER(L, "TextDisabled");
    const Text text = CheckText(L, 1, fn_name);
    ImGui::TextDisabled("%.*s", text.length, text.data);
    return 0;
}

// ui.TextWrapped(text): wrapped at the right edge of the window
int UiTextWrapped(lua_State* L)
{
    GW_UI_ENTER(L, "TextWrapped");
    const Text text = CheckText(L, 1, fn_name);
    ImGui::TextWrapped("%.*s", text.length, text.data);
    return 0;
}

// ui.LabelText(label, text): a value with a label on the right, as the framed widgets
int UiLabelText(lua_State* L)
{
    GW_UI_ENTER(L, "LabelText");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const Text text = CheckText(L, 2, fn_name);
    ImGui::LabelText(label, "%.*s", text.length, text.data);
    return 0;
}

// ui.BulletText(text)
int UiBulletText(lua_State* L)
{
    GW_UI_ENTER(L, "BulletText");
    const Text text = CheckText(L, 1, fn_name);
    ImGui::BulletText("%.*s", text.length, text.data);
    return 0;
}

// ui.Bullet(): a bullet, the next widget on the same line
int UiBullet(lua_State* L)
{
    GW_UI_ENTER(L, "Bullet");
    ImGui::Bullet();
    return 0;
}

// ui.PushTextWrapPos([x]) / ui.PopTextWrapPos(): x 0 - the right edge of the window, < 0 - no wrap
int UiPushTextWrapPos(lua_State* L)
{
    GW_UI_ENTER(L, "PushTextWrapPos");
    const float x = OptFloat(L, 1, 0.f);
    GW_UI_CAN_PUSH(L);
    ImGui::PushTextWrapPos(x);
    imgui::Push(EScope::TextWrapPos);
    return 0;
}

int UiPopTextWrapPos(lua_State* L)
{
    GW_UI_ENTER(L, "PopTextWrapPos");
    GW_UI_CHECK_POP(L, EScope::TextWrapPos);
    ImGui::PopTextWrapPos();
    imgui::Pop();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------

// ui.Button(label[, w, h]) -> pressed
int UiButton(lua_State* L)
{
    GW_UI_ENTER(L, "Button");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const ImVec2 size(OptFloat(L, 2, 0.f), OptFloat(L, 3, 0.f));
    lua_pushboolean(L, ImGui::Button(label, size));
    return 1;
}

// ui.SmallButton(label) -> pressed
int UiSmallButton(lua_State* L)
{
    GW_UI_ENTER(L, "SmallButton");
    lua_pushboolean(L, ImGui::SmallButton(CheckLabel(L, 1, fn_name)));
    return 1;
}

// ui.InvisibleButton(str_id, w, h[, flags]) -> pressed. w, h not 0 (< 0 - up to the edge); flags - ui.ButtonFlags
int UiInvisibleButton(lua_State* L)
{
    GW_UI_ENTER(L, "InvisibleButton");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const ImVec2 size(CheckFloat(L, 2), CheckFloat(L, 3));
    const int flags = OptFlags(L, 4, kButtonAllowed, fn_name);
    if (size.x == 0.f || size.y == 0.f)
        return luaL_error(L, "ui.%s: the size must not be 0 (a negative size means up to the edge)", fn_name);
    lua_pushboolean(L, ImGui::InvisibleButton(id, size, flags));
    return 1;
}

// ui.ArrowButton(str_id, dir) -> pressed. dir - ui.Dir.Left / Right / Up / Down
int UiArrowButton(lua_State* L)
{
    GW_UI_ENTER(L, "ArrowButton");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const int dir = CheckInt(L, 2);
    if (dir < ImGuiDir_Left || dir > ImGuiDir_Down)
        return luaL_argerror(L, 2, "ui.Dir.Left, Right, Up or Down");
    lua_pushboolean(L, ImGui::ArrowButton(id, static_cast<ImGuiDir>(dir)));
    return 1;
}

// ui.Checkbox(label, value) -> value, changed
int UiCheckbox(lua_State* L)
{
    GW_UI_ENTER(L, "Checkbox");
    const pcstr label = CheckLabel(L, 1, fn_name);
    bool value = lua_toboolean(L, 2) != 0;
    const bool changed = ImGui::Checkbox(label, &value);
    lua_pushboolean(L, value);
    lua_pushboolean(L, changed);
    return 2;
}

// ui.CheckboxFlags(label, flags, flag) -> flags, changed: the checkbox shows and toggles the bits `flag` of `flags`
int UiCheckboxFlags(lua_State* L)
{
    GW_UI_ENTER(L, "CheckboxFlags");
    const pcstr label = CheckLabel(L, 1, fn_name);
    unsigned int flags = ToBits(luaL_checknumber(L, 2));
    const unsigned int flag = ToBits(luaL_checknumber(L, 3));
    const bool changed = ImGui::CheckboxFlags(label, &flags, flag);
    PushArgb(L, flags);
    lua_pushboolean(L, changed);
    return 2;
}

// ui.RadioButton(label, active) -> pressed
// ui.RadioButton(label, value, button_value) -> value, changed (value becomes button_value when pressed)
int UiRadioButton(lua_State* L)
{
    GW_UI_ENTER(L, "RadioButton");
    const pcstr label = CheckLabel(L, 1, fn_name);
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        int value = CheckInt(L, 2);
        const int button = CheckInt(L, 3);
        const bool changed = ImGui::RadioButton(label, &value, button);
        lua_pushinteger(L, value);
        lua_pushboolean(L, changed);
        return 2;
    }
    lua_pushboolean(L, ImGui::RadioButton(label, lua_toboolean(L, 2) != 0));
    return 1;
}

// ui.ProgressBar(fraction[, w, h, overlay]): 0..1; w nil - the whole width; overlay - a text instead of the percents
int UiProgressBar(lua_State* L)
{
    GW_UI_ENTER(L, "ProgressBar");
    const float fraction = ToFloat(luaL_checknumber(L, 1), 0.f);
    const ImVec2 size(OptFloat(L, 2, -FLT_MIN), OptFloat(L, 3, 0.f));
    const pcstr overlay = OptLabel(L, 4, fn_name);
    ImGui::ProgressBar(fraction, size, overlay);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Numbers: sliders, drags, inputs. format - one printf conversion of the type (SafeFormat), else the default
// ---------------------------------------------------------------------------------------------

// Sliders assert on ranges beyond half of the type
float SliderLimit(float value) { return value < -FLT_MAX / 2.f ? -FLT_MAX / 2.f : (value > FLT_MAX / 2.f ? FLT_MAX / 2.f : value); }
int SliderLimit(int value) { return value < INT_MIN / 2 ? INT_MIN / 2 : (value > INT_MAX / 2 ? INT_MAX / 2 : value); }

constexpr pcstr kSliderFloatNames[] = { "", "SliderFloat", "SliderFloat2", "SliderFloat3", "SliderFloat4" };
constexpr pcstr kSliderIntNames[] = { "", "SliderInt", "SliderInt2", "SliderInt3", "SliderInt4" };
constexpr pcstr kDragFloatNames[] = { "", "DragFloat", "DragFloat2", "DragFloat3", "DragFloat4" };
constexpr pcstr kDragIntNames[] = { "", "DragInt", "DragInt2", "DragInt3", "DragInt4" };
constexpr pcstr kInputFloatNames[] = { "", "InputFloat", "InputFloat2", "InputFloat3", "InputFloat4" };

template <typename T>
T CheckNumber(lua_State* L, int index)
{
    if constexpr (std::is_same_v<T, float>)
        return CheckFloat(L, index);
    else
        return CheckInt(L, index);
}

template <typename T>
T OptNumber(lua_State* L, int index, T fallback)
{
    if constexpr (std::is_same_v<T, float>)
        return OptFloat(L, index, fallback);
    else
        return OptInt(L, index, fallback);
}

template <typename T>
int PushValues(lua_State* L, const T* values, int count, bool changed)
{
    for (int i = 0; i < count; ++i)
        lua_pushnumber(L, static_cast<lua_Number>(values[i]));
    lua_pushboolean(L, changed);
    return count + 1;
}

// ui.SliderFloat[N](label, v1..vN, min, max[, format, flags]) -> v1..vN, changed (the same for SliderInt[N])
template <typename T, int N>
int UiSliderN(lua_State* L)
{
    constexpr bool integer = std::is_same_v<T, int>;
    GW_UI_ENTER(L, integer ? kSliderIntNames[N] : kSliderFloatNames[N]);
    const pcstr label = CheckLabel(L, 1, fn_name);
    T values[N];
    for (int i = 0; i < N; ++i)
        values[i] = CheckNumber<T>(L, 2 + i);
    const T min = SliderLimit(CheckNumber<T>(L, 2 + N));
    const T max = SliderLimit(CheckNumber<T>(L, 3 + N));
    const pcstr format = imgui::SafeFormat(OptString(L, 4 + N), integer, integer ? "%d" : "%.3f");
    const int flags = OptFlags(L, 5 + N, kSliderAllowed, fn_name);
    constexpr ImGuiDataType type = integer ? ImGuiDataType_S32 : ImGuiDataType_Float;
    bool changed;
    if constexpr (N == 1)
        changed = ImGui::SliderScalar(label, type, values, &min, &max, format, flags);
    else
        changed = ImGui::SliderScalarN(label, type, values, N, &min, &max, format, flags);
    return PushValues(L, values, N, changed);
}

// ui.SliderAngle(label, radians[, min_degrees, max_degrees, format, flags]) -> radians, changed
int UiSliderAngle(lua_State* L)
{
    GW_UI_ENTER(L, "SliderAngle");
    const pcstr label = CheckLabel(L, 1, fn_name);
    float radians = CheckFloat(L, 2);
    const float min = SliderLimit(OptFloat(L, 3, -360.f));
    const float max = SliderLimit(OptFloat(L, 4, 360.f));
    const pcstr format = imgui::SafeFormat(OptString(L, 5), false, "%.0f deg");
    const int flags = OptFlags(L, 6, kSliderAllowed, fn_name);
    const bool changed = ImGui::SliderAngle(label, &radians, min, max, format, flags);
    return PushValues(L, &radians, 1, changed);
}

// ui.DragFloat[N](label, v1..vN[, speed, min, max, format, flags]) -> v1..vN, changed (min >= max - no bounds);
// the same for DragInt[N]
template <typename T, int N>
int UiDragN(lua_State* L)
{
    constexpr bool integer = std::is_same_v<T, int>;
    GW_UI_ENTER(L, integer ? kDragIntNames[N] : kDragFloatNames[N]);
    const pcstr label = CheckLabel(L, 1, fn_name);
    T values[N];
    for (int i = 0; i < N; ++i)
        values[i] = CheckNumber<T>(L, 2 + i);
    const float speed = OptFloat(L, 2 + N, 1.f);
    const T min = SliderLimit(OptNumber<T>(L, 3 + N, T(0)));
    const T max = SliderLimit(OptNumber<T>(L, 4 + N, T(0)));
    const pcstr format = imgui::SafeFormat(OptString(L, 5 + N), integer, integer ? "%d" : "%.3f");
    const int flags = OptFlags(L, 6 + N, kDragAllowed, fn_name);
    constexpr ImGuiDataType type = integer ? ImGuiDataType_S32 : ImGuiDataType_Float;
    bool changed;
    if constexpr (N == 1)
        changed = ImGui::DragScalar(label, type, values, speed, &min, &max, format, flags);
    else
        changed = ImGui::DragScalarN(label, type, values, N, speed, &min, &max, format, flags);
    return PushValues(L, values, N, changed);
}

// ui.InputFloat(label, v[, step, step_fast, format, flags]) -> v, changed
// ui.InputFloat2..4(label, v1..vN[, format, flags]) -> v1..vN, changed
template <int N>
int UiInputFloatN(lua_State* L)
{
    GW_UI_ENTER(L, kInputFloatNames[N]);
    const pcstr label = CheckLabel(L, 1, fn_name);
    float values[N];
    for (int i = 0; i < N; ++i)
        values[i] = CheckFloat(L, 2 + i);
    bool changed;
    if constexpr (N == 1)
    {
        const float step = OptFloat(L, 3, 0.f);
        const float step_fast = OptFloat(L, 4, 0.f);
        const pcstr format = imgui::SafeFormat(OptString(L, 5), false, "%.3f");
        const int flags = OptFlags(L, 6, kInputScalarAllowed, fn_name);
        changed = ImGui::InputFloat(label, values, step, step_fast, format, flags);
    }
    else
    {
        const pcstr format = imgui::SafeFormat(OptString(L, 2 + N), false, "%.3f");
        const int flags = OptFlags(L, 3 + N, kInputScalarAllowed, fn_name);
        changed = ImGui::InputScalarN(label, ImGuiDataType_Float, values, N, nullptr, nullptr, format, flags);
    }
    return PushValues(L, values, N, changed);
}

// ui.InputInt(label, v[, step, step_fast, flags]) -> v, changed
int UiInputInt(lua_State* L)
{
    GW_UI_ENTER(L, "InputInt");
    const pcstr label = CheckLabel(L, 1, fn_name);
    int value = CheckInt(L, 2);
    const int step = OptInt(L, 3, 1);
    const int step_fast = OptInt(L, 4, 100);
    const int flags = OptFlags(L, 5, kInputScalarAllowed, fn_name);
    const bool changed = ImGui::InputInt(label, &value, step, step_fast, flags);
    return PushValues(L, &value, 1, changed);
}

// ---------------------------------------------------------------------------------------------
// Text input: the text of Lua goes to a buffer of the host (capacity - the bytes of the text, up to the limit), a new
// string is made only when the text changed
// ---------------------------------------------------------------------------------------------
xr_vector<char> g_text_buffer;
size_t g_text_filled = 0; // the bytes of the text of Lua FillTextBuffer put into g_text_buffer (PushTextResult)

// The capacity of a widget that is active (being edited). ImGui copies the text to its own buffer
// (ImGuiInputTextState::TextA) of buf_size bytes when the widget becomes active, and then takes the buf_size of every
// frame as the room for the typing without growing that copy (only CallbackResize grows it; imgui_internal.h: "TextA.Size
// ... should always be >= buf_size passed by user"). The capacity of Lua follows the text (nil - the text + 256) or the
// argument, so it changed while the player typed: a bigger one let the typing and a paste write past TextA (a heap
// overflow), a smaller one gave ImGui a negative room. While the widget is active the capacity stays the one of the
// activation (TextA.Size == capacity + 2), the next activation takes the new one. A widget that was ReadOnly in the last
// frame stays ReadOnly until it loses the focus: ImGui did not copy a read-only text to TextA (its TextLen is the length
// of the text of Lua, TextA is of another widget).
size_t ActiveCapacity(pcstr label, size_t capacity, int& flags)
{
    const ImGuiContext& g = *GImGui;
    const ImGuiInputTextState& state = g.InputTextState;
    if (g.ActiveId == 0 || g.ActiveId != state.ID || ImGui::GetID(label) != state.ID)
        return capacity;
    if (state.Flags & ImGuiInputTextFlags_ReadOnly)
    {
        flags |= ImGuiInputTextFlags_ReadOnly;
        return capacity;
    }
    return state.TextA.Size >= 2 ? static_cast<size_t>(state.TextA.Size - 2) : capacity;
}

// The buffer with the text of the argument `index`. capacity_index: the argument of the capacity (nil - the default:
// `base` bytes more than the text, so it can grow). label, flags: of the widget (ActiveCapacity). Returns the buffer size
// (capacity + 1)
size_t FillTextBuffer(lua_State* L, int index, int capacity_index, size_t base, size_t limit, pcstr fn_name, pcstr label,
    int& flags)
{
    size_t length = 0;
    const pcstr text = luaL_checklstring(L, index, &length);
    size_t capacity;
    if (lua_isnoneornil(L, capacity_index))
        capacity = length + base;
    else
    {
        const int asked = CheckInt(L, capacity_index);
        capacity = asked < 1 ? 1 : static_cast<size_t>(asked);
    }
    if (capacity > limit)
    {
        if (!lua_isnoneornil(L, capacity_index) || length > limit)
        {
            imgui::WarnOnce(imgui::kWarnTruncated, "ui.%s: the buffer is limited to %u bytes (this warning is shown "
                "once)", fn_name, static_cast<u32>(limit));
        }
        capacity = limit;
    }
    capacity = ActiveCapacity(label, capacity, flags);
    if (length > capacity)
    {
        imgui::WarnOnce(imgui::kWarnTruncated, "ui.%s: a text of %u bytes is cut to the buffer of %u (this warning is "
            "shown once)", fn_name, static_cast<u32>(length), static_cast<u32>(capacity));
        length = Utf8Cut(text, length, capacity);
    }
    g_text_buffer.resize(capacity + 1);
    memcpy(g_text_buffer.data(), text, length);
    g_text_buffer[length] = 0;
    g_text_filled = length;
    return capacity + 1;
}

// The result: the new text when it changed, else the string of the argument (nothing is allocated). ImGui writes the
// buffer whenever the text was edited, but with InputTextFlags.EnterReturnsTrue returns true only on Enter: the edited
// text is returned by IsItemEdited (as the buffer of C++ keeps it), else the typing was lost when the widget lost the
// focus without Enter
int PushTextResult(lua_State* L, int index, bool changed)
{
    bool edited = changed;
    if (!edited && ImGui::IsItemEdited())
    {
        // The last item may be another widget (ImGui skipped this one: SkipItems) - the buffer is compared with what
        // FillTextBuffer put there (a text cut to the capacity is not an edit)
        const pcstr text = lua_tostring(L, index);
        edited = memcmp(g_text_buffer.data(), text, g_text_filled) != 0 || g_text_buffer[g_text_filled] != 0;
    }
    if (edited)
        lua_pushstring(L, g_text_buffer.data());
    else
        lua_pushvalue(L, index);
    lua_pushboolean(L, changed);
    return 2;
}

int InputTextFlags(lua_State* L, int index, bool multiline, pcstr fn_name)
{
    int flags = OptFlags(L, index, kInputTextAllowed, fn_name);
    if (!multiline && (flags & ImGuiInputTextFlags_WordWrap))
    {
        WarnArgs(fn_name, "InputTextFlags.WordWrap is only for InputTextMultiline, ignored");
        flags &= ~ImGuiInputTextFlags_WordWrap;
    }
    if (multiline && (flags & ImGuiInputTextFlags_ElideLeft))
    {
        WarnArgs(fn_name, "InputTextFlags.ElideLeft is not for InputTextMultiline, ignored");
        flags &= ~ImGuiInputTextFlags_ElideLeft;
    }
    if ((flags & ImGuiInputTextFlags_WordWrap) && (flags & ImGuiInputTextFlags_Password))
    {
        WarnArgs(fn_name, "InputTextFlags.WordWrap does not go with Password, ignored");
        flags &= ~ImGuiInputTextFlags_WordWrap;
    }
    return flags;
}

// ui.InputText(label, text[, capacity, flags]) -> text, changed. capacity - bytes (UTF-8), nil - the text + 256, at most
// 16 KiB
int UiInputText(lua_State* L)
{
    GW_UI_ENTER(L, "InputText");
    const pcstr label = CheckLabel(L, 1, fn_name);
    int flags = InputTextFlags(L, 4, false, fn_name);
    const size_t size = FillTextBuffer(L, 2, 3, 256, imgui::kMaxInputText, fn_name, label, flags);
    const bool changed = ImGui::InputText(label, g_text_buffer.data(), size, flags);
    return PushTextResult(L, 2, changed);
}

// ui.InputTextWithHint(label, hint, text[, capacity, flags]) -> text, changed: the hint is shown while the text is empty
int UiInputTextWithHint(lua_State* L)
{
    GW_UI_ENTER(L, "InputTextWithHint");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const pcstr hint = CheckLabel(L, 2, fn_name);
    int flags = InputTextFlags(L, 5, false, fn_name);
    const size_t size = FillTextBuffer(L, 3, 4, 256, imgui::kMaxInputText, fn_name, label, flags);
    const bool changed = ImGui::InputTextWithHint(label, hint, g_text_buffer.data(), size, flags);
    return PushTextResult(L, 3, changed);
}

// ui.InputTextMultiline(label, text[, capacity, w, h, flags]) -> text, changed. capacity nil - the text + 4 KiB, at
// most 64 KiB
int UiInputTextMultiline(lua_State* L)
{
    GW_UI_ENTER(L, "InputTextMultiline");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const ImVec2 box(OptFloat(L, 4, 0.f), OptFloat(L, 5, 0.f));
    int flags = InputTextFlags(L, 6, true, fn_name);
    const size_t size = FillTextBuffer(L, 2, 3, 4096, imgui::kMaxInputTextMultiline, fn_name, label, flags);
    const bool changed = ImGui::InputTextMultiline(label, g_text_buffer.data(), size, box, flags);
    return PushTextResult(L, 2, changed);
}

// ---------------------------------------------------------------------------------------------
// Choice: the arrays of Lua (1-based) - the index of the item is 1-based too (items[current]), 0 - nothing chosen
// ---------------------------------------------------------------------------------------------
struct LuaItems
{
    lua_State* L;
    int index;
};

char g_item_number[32];

// The getter of Combo / ListBox: the item of the Lua array (a string stays alive in its table; a number is shown through
// a buffer - ImGui uses an item before it asks for the next one)
const char* LuaItemGetter(void* user, int index)
{
    const LuaItems& items = *static_cast<const LuaItems*>(user);
    lua_rawgeti(items.L, items.index, index + 1);
    pcstr text = "";
    if (lua_type(items.L, -1) == LUA_TSTRING)
        text = lua_tostring(items.L, -1);
    else if (lua_type(items.L, -1) == LUA_TNUMBER)
    {
        xr_sprintf(g_item_number, "%.14g", lua_tonumber(items.L, -1));
        text = g_item_number;
    }
    lua_pop(items.L, 1);
    return text;
}

int CheckItems(lua_State* L, int index, pcstr fn_name)
{
    luaL_checktype(L, index, LUA_TTABLE);
    size_t count = lua_objlen(L, index);
    if (count > imgui::kMaxListItems)
    {
        imgui::WarnOnce(imgui::kWarnTruncated, "ui.%s: an array of %u items is cut to %u (this warning is shown once)",
            fn_name, static_cast<u32>(count), imgui::kMaxListItems);
        count = imgui::kMaxListItems;
    }
    return static_cast<int>(count);
}

// ui.Combo(label, current, items[, popup_max_height_in_items]) -> current, changed
int UiCombo(lua_State* L)
{
    GW_UI_ENTER(L, "Combo");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const int count = CheckItems(L, 3, fn_name);
    // 1-based; out of 1..count - nothing chosen (no "- 1" on INT_MIN: OptInt clamps a huge negative number to it)
    int current = OptInt(L, 2, 0);
    current = current >= 1 && current <= count ? current - 1 : -1;
    int height = OptInt(L, 4, -1);
    if (height < -1)
        height = -1;
    LuaItems items{ L, 3 };
    const bool changed = ImGui::Combo(label, &current, &LuaItemGetter, &items, count, height);
    lua_pushinteger(L, current + 1);
    lua_pushboolean(L, changed);
    return 2;
}

// ui.BeginCombo(label, preview[, flags]) -> open. EndCombo only after true; the items - Selectable
int UiBeginCombo(lua_State* L)
{
    GW_UI_ENTER(L, "BeginCombo");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const pcstr preview = OptLabel(L, 2, fn_name);
    int flags = OptFlags(L, 3, kComboAllowed, fn_name);
    if ((flags & ImGuiComboFlags_NoArrowButton) && (flags & ImGuiComboFlags_NoPreview))
    {
        WarnArgs(fn_name, "ComboFlags.NoArrowButton does not go with NoPreview, ignored");
        flags &= ~ImGuiComboFlags_NoArrowButton;
    }
    if ((flags & ImGuiComboFlags_WidthFitPreview) && (flags & ImGuiComboFlags_NoPreview))
    {
        WarnArgs(fn_name, "ComboFlags.WidthFitPreview does not go with NoPreview, ignored");
        flags &= ~ImGuiComboFlags_WidthFitPreview;
    }
    flags = OneOf(flags, ImGuiComboFlags_HeightMask_, fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginCombo(label, preview, flags);
    if (open)
        imgui::Push(EScope::Combo);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndCombo(lua_State* L)
{
    GW_UI_ENTER(L, "EndCombo");
    GW_UI_CHECK_POP(L, EScope::Combo);
    ImGui::EndCombo();
    imgui::Pop();
    return 0;
}

// ui.Selectable(label[, selected, flags, w, h]) -> pressed
int UiSelectable(lua_State* L)
{
    GW_UI_ENTER(L, "Selectable");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const bool selected = OptBool(L, 2, false);
    const int flags = OptFlags(L, 3, kSelectableAllowed, fn_name);
    const ImVec2 size(OptFloat(L, 4, 0.f), OptFloat(L, 5, 0.f));
    lua_pushboolean(L, ImGui::Selectable(label, selected, flags, size));
    return 1;
}

// ui.ListBox(label, current, items[, height_in_items]) -> current, changed
int UiListBox(lua_State* L)
{
    GW_UI_ENTER(L, "ListBox");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const int count = CheckItems(L, 3, fn_name);
    // 1-based; out of 1..count - nothing chosen (no "- 1" on INT_MIN: OptInt clamps a huge negative number to it)
    int current = OptInt(L, 2, 0);
    current = current >= 1 && current <= count ? current - 1 : -1;
    int height = OptInt(L, 4, -1);
    if (height < -1)
        height = -1;
    LuaItems items{ L, 3 };
    const bool changed = ImGui::ListBox(label, &current, &LuaItemGetter, &items, count, height);
    lua_pushinteger(L, current + 1);
    lua_pushboolean(L, changed);
    return 2;
}

// ui.BeginListBox(label[, w, h]) -> open. EndListBox only after true
int UiBeginListBox(lua_State* L)
{
    GW_UI_ENTER(L, "BeginListBox");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const ImVec2 size(OptFloat(L, 2, 0.f), OptFloat(L, 3, 0.f));
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginListBox(label, size);
    if (open)
        imgui::Push(EScope::ListBox);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndListBox(lua_State* L)
{
    GW_UI_ENTER(L, "EndListBox");
    GW_UI_CHECK_POP(L, EScope::ListBox);
    ImGui::EndListBox();
    imgui::Pop();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------------------------
int ColorEditFlags(lua_State* L, int index, pcstr fn_name)
{
    int flags = OptFlags(L, index, kColorEditAllowed, fn_name);
    flags = OneOf(flags, ImGuiColorEditFlags_DisplayMask_, fn_name);
    flags = OneOf(flags, ImGuiColorEditFlags_DataTypeMask_, fn_name);
    flags = OneOf(flags, ImGuiColorEditFlags_PickerMask_, fn_name);
    flags = OneOf(flags, ImGuiColorEditFlags_InputMask_, fn_name);
    return flags;
}

constexpr pcstr kColorEditNames[] = { "", "", "", "ColorEdit3", "ColorEdit4" };

// ui.ColorEdit3(label, r, g, b[, flags]) -> r, g, b, changed
// ui.ColorEdit4(label, r, g, b, a[, flags]) -> r, g, b, a, changed           (0..1)
// ui.ColorEditN(label, 0xAARRGGBB[, flags]) -> 0xAARRGGBB, changed            (ColorEdit3 keeps the alpha byte)
// ui.ColorEditN(label, {r, g, b[, a]}[, flags]) -> changed                   (the table is changed in place)
template <int N>
int UiColorEditN(lua_State* L)
{
    GW_UI_ENTER(L, kColorEditNames[N]);
    const pcstr label = CheckLabel(L, 1, fn_name);
    float color[4] = { 0.f, 0.f, 0.f, 1.f };
    enum class EForm
    {
        Floats,
        Packed,
        Table
    } form;
    int flags_index;
    u32 packed = 0;
    if (lua_type(L, 2) == LUA_TTABLE)
    {
        form = EForm::Table;
        ReadColorTable(L, 2, color);
        flags_index = 3;
    }
    else if (lua_type(L, 2) == LUA_TNUMBER && lua_type(L, 4) == LUA_TNUMBER)
    {
        form = EForm::Floats;
        for (int i = 0; i < 3; ++i)
            color[i] = CheckFloat(L, 2 + i);
        if (N == 4)
            color[3] = OptFloat(L, 5, 1.f);
        flags_index = 2 + N;
    }
    else if (lua_type(L, 2) == LUA_TNUMBER)
    {
        form = EForm::Packed;
        packed = ToBits(lua_tonumber(L, 2));
        const ImVec4 value = ArgbToColor(packed);
        color[0] = value.x;
        color[1] = value.y;
        color[2] = value.z;
        color[3] = value.w;
        flags_index = 3;
    }
    else
        return luaL_argerror(L, 2, "a color: r, g, b[, a] of 0..1, a number 0xAARRGGBB or a table {r, g, b[, a]}");
    const int flags = ColorEditFlags(L, flags_index, fn_name);
    const bool changed = N == 3 ? ImGui::ColorEdit3(label, color, flags) : ImGui::ColorEdit4(label, color, flags);
    switch (form)
    {
    case EForm::Floats: return PushValues(L, color, N, changed);
    case EForm::Packed:
        if (N == 3)
            color[3] = static_cast<float>(packed >> 24) / 255.f;
        PushArgb(L, ColorToArgb(ImVec4(color[0], color[1], color[2], color[3])));
        lua_pushboolean(L, changed);
        return 2;
    case EForm::Table:
    default:
        if (changed)
        {
            for (int i = 0; i < N; ++i)
            {
                lua_pushnumber(L, color[i]);
                lua_rawseti(L, 2, i + 1);
            }
        }
        lua_pushboolean(L, changed);
        return 1;
    }
}

// ui.ColorButton(desc_id, color[, flags, w, h]) -> pressed: a colored square, the color in its tooltip
int UiColorButton(lua_State* L)
{
    GW_UI_ENTER(L, "ColorButton");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const ImVec4 color = CheckColor(L, 2);
    const int flags = ColorEditFlags(L, 3, fn_name);
    const ImVec2 size(OptFloat(L, 4, 0.f), OptFloat(L, 5, 0.f));
    lua_pushboolean(L, ImGui::ColorButton(id, color, flags, size));
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Trees
// ---------------------------------------------------------------------------------------------

// ui.TreeNode(label) -> open. TreePop only after true
int UiTreeNode(lua_State* L)
{
    GW_UI_ENTER(L, "TreeNode");
    const pcstr label = CheckLabel(L, 1, fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::TreeNode(label);
    if (open)
        imgui::Push(EScope::TreeNode);
    lua_pushboolean(L, open);
    return 1;
}

// ui.TreeNodeEx(label[, flags]) -> open. TreePop only after true and without TreeNodeFlags.NoTreePushOnOpen
int UiTreeNodeEx(lua_State* L)
{
    GW_UI_ENTER(L, "TreeNodeEx");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const int flags = OptFlags(L, 2, kTreeNodeFlagsAllowed, fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::TreeNodeEx(label, flags);
    if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen))
        imgui::Push(EScope::TreeNode);
    lua_pushboolean(L, open);
    return 1;
}

int UiTreePop(lua_State* L)
{
    GW_UI_ENTER(L, "TreePop");
    GW_UI_CHECK_POP(L, EScope::TreeNode);
    ImGui::TreePop();
    imgui::Pop();
    return 0;
}

// ui.CollapsingHeader(label[, flags]) -> open (no TreePop)
// ui.CollapsingHeader(label, visible[, flags]) -> open, visible (a close button; visible false - the header is hidden)
int UiCollapsingHeader(lua_State* L)
{
    GW_UI_ENTER(L, "CollapsingHeader");
    const pcstr label = CheckLabel(L, 1, fn_name);
    if (lua_type(L, 2) == LUA_TBOOLEAN)
    {
        bool visible = lua_toboolean(L, 2) != 0;
        const int flags = OptFlags(L, 3, kTreeNodeFlagsAllowed, fn_name);
        const bool open = ImGui::CollapsingHeader(label, &visible, flags);
        lua_pushboolean(L, open);
        lua_pushboolean(L, visible);
        return 2;
    }
    const int flags = OptFlags(L, 2, kTreeNodeFlagsAllowed, fn_name);
    lua_pushboolean(L, ImGui::CollapsingHeader(label, flags));
    return 1;
}

int CondArg(lua_State* L, int index, pcstr fn_name) { return OneOf(OptFlags(L, index, kCondAllowed, fn_name), kCondAllowed, fn_name); }

// ui.SetNextItemOpen(open[, cond]): the open state of the next TreeNode / CollapsingHeader. cond - ui.Cond
int UiSetNextItemOpen(lua_State* L)
{
    GW_UI_ENTER(L, "SetNextItemOpen");
    const bool open = lua_toboolean(L, 1) != 0;
    ImGui::SetNextItemOpen(open, CondArg(L, 2, fn_name));
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------------------------

// ui.BeginTabBar(str_id[, flags]) -> open. EndTabBar only after true
int UiBeginTabBar(lua_State* L)
{
    GW_UI_ENTER(L, "BeginTabBar");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const int flags = OneOf(OptFlags(L, 2, kTabBarAllowed, fn_name), ImGuiTabBarFlags_FittingPolicyMask_, fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginTabBar(id, flags);
    if (open)
        imgui::Push(EScope::TabBar);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndTabBar(lua_State* L)
{
    GW_UI_ENTER(L, "EndTabBar");
    GW_UI_CHECK_POP(L, EScope::TabBar);
    ImGui::EndTabBar();
    imgui::Pop();
    return 0;
}

// ui.BeginTabItem(label[, open, flags]) -> selected, open. EndTabItem only after selected == true.
// open: a boolean - the tab has a close button (open false - the tab is not shown); nil - no button (open is true)
int UiBeginTabItem(lua_State* L)
{
    GW_UI_ENTER(L, "BeginTabItem");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const bool closable = lua_type(L, 2) == LUA_TBOOLEAN;
    bool open = closable ? lua_toboolean(L, 2) != 0 : true;
    int flags = OptFlags(L, closable || lua_isnoneornil(L, 2) ? 3 : 2, kTabItemAllowed, fn_name);
    if ((flags & ImGuiTabItemFlags_Leading) && (flags & ImGuiTabItemFlags_Trailing))
    {
        WarnArgs(fn_name, "TabItemFlags.Leading does not go with Trailing, Trailing ignored");
        flags &= ~ImGuiTabItemFlags_Trailing;
    }
    const EScope innermost = Innermost(kWindowScopes | ScopeBit(EScope::TabBar) | ScopeBit(EScope::TabItem));
    if (innermost != EScope::TabBar)
    {
        return luaL_error(L, "ui.%s: only between BeginTabBar (that returned true) and EndTabBar%s%s", fn_name,
            innermost == EScope::Count ? "" : ": the innermost open pair is ",
            innermost == EScope::Count ? "" : imgui::ScopeBeginName(innermost));
    }
    GW_UI_CAN_PUSH(L);
    const bool selected = ImGui::BeginTabItem(label, closable ? &open : nullptr, flags);
    if (selected)
        imgui::Push(EScope::TabItem);
    lua_pushboolean(L, selected);
    lua_pushboolean(L, open);
    return 2;
}

int UiEndTabItem(lua_State* L)
{
    GW_UI_ENTER(L, "EndTabItem");
    GW_UI_CHECK_POP(L, EScope::TabItem);
    ImGui::EndTabItem();
    imgui::Pop();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------------------------

// ui.BeginTable(str_id, columns[, flags, outer_w, outer_h, inner_width]) -> open. EndTable only after true.
// columns: 1..511
int UiBeginTable(lua_State* L)
{
    GW_UI_ENTER(L, "BeginTable");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const int columns = CheckInt(L, 2);
    if (columns < 1 || columns >= IMGUI_TABLE_MAX_COLUMNS)
        return luaL_argerror(L, 2, "the columns must be 1..511");
    int flags = OptFlags(L, 3, kTableFlagsAllowed, fn_name);
    const int sizing = flags & ImGuiTableFlags_SizingMask_;
    if (sizing != 0 && sizing != ImGuiTableFlags_SizingFixedFit && sizing != ImGuiTableFlags_SizingFixedSame &&
        sizing != ImGuiTableFlags_SizingStretchProp && sizing != ImGuiTableFlags_SizingStretchSame)
    {
        WarnArgs(fn_name, "only one TableFlags.Sizing* may be set, the sizing is left to ImGui");
        flags &= ~ImGuiTableFlags_SizingMask_;
    }
    const ImVec2 outer(OptFloat(L, 4, 0.f), OptFloat(L, 5, 0.f));
    float inner_width = OptFloat(L, 6, 0.f);
    if (inner_width < 0.f)
    {
        WarnArgs(fn_name, "a negative inner_width is 0");
        inner_width = 0.f;
    }
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginTable(id, columns, flags, outer, inner_width);
    if (open)
        imgui::Push(EScope::Table);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndTable(lua_State* L)
{
    GW_UI_ENTER(L, "EndTable");
    GW_UI_CHECK_POP(L, EScope::Table);
    ImGui::EndTable();
    imgui::Pop();
    return 0;
}

// ui.TableNextRow([row_flags, min_height]): row_flags - ui.TableRowFlags
int UiTableNextRow(lua_State* L)
{
    GW_UI_ENTER(L, "TableNextRow");
    const int flags = OptFlags(L, 1, kTableRowFlagsAllowed, fn_name);
    const float height = OptFloat(L, 2, 0.f);
    GW_UI_REQUIRE_TABLE(L);
    ImGui::TableNextRow(flags, height);
    return 0;
}

// ui.TableNextColumn() -> visible: the next cell (a new row after the last column)
int UiTableNextColumn(lua_State* L)
{
    GW_UI_ENTER(L, "TableNextColumn");
    GW_UI_REQUIRE_TABLE(L);
    lua_pushboolean(L, ImGui::TableNextColumn());
    return 1;
}

// ui.TableSetColumnIndex(column) -> visible: column 0-based, as in ImGui (0..columns-1)
int UiTableSetColumnIndex(lua_State* L)
{
    GW_UI_ENTER(L, "TableSetColumnIndex");
    const int column = CheckInt(L, 1);
    GW_UI_REQUIRE_TABLE(L);
    if (column < 0 || column >= ImGui::TableGetColumnCount())
        return luaL_error(L, "ui.%s: the column %d is not 0..%d", fn_name, column, ImGui::TableGetColumnCount() - 1);
    lua_pushboolean(L, ImGui::TableSetColumnIndex(column));
    return 1;
}

// Setup functions: before the first row
pcstr TableSetupError()
{
    const ImGuiTable* table = ImGui::GetCurrentTable();
    if (table->IsLayoutLocked)
        return "call it right after BeginTable, before the first TableNextRow / TableNextColumn / TableHeadersRow";
    return nullptr;
}

// ui.TableSetupColumn(label[, flags, init_width_or_weight, user_id]): flags - ui.TableColumnFlags
int UiTableSetupColumn(lua_State* L)
{
    GW_UI_ENTER(L, "TableSetupColumn");
    const pcstr label = CheckLabel(L, 1, fn_name);
    int flags = OneOf(OptFlags(L, 2, kTableColumnFlagsAllowed, fn_name), ImGuiTableColumnFlags_WidthMask_, fn_name);
    const float width = OptFloat(L, 3, 0.f);
    const ImGuiID user_id = lua_isnoneornil(L, 4) ? 0 : ToBits(luaL_checknumber(L, 4));
    GW_UI_REQUIRE_TABLE(L);
    if (const pcstr error = TableSetupError())
        return luaL_error(L, "ui.%s: %s", fn_name, error);
    const ImGuiTable* table = ImGui::GetCurrentTable();
    if (table->DeclColumnsCount >= table->ColumnsCount)
        return luaL_error(L, "ui.%s: more columns than BeginTable has (%d)", fn_name, int(table->ColumnsCount));
    // A width with the sizing of ImGui by default: the column is fixed (ImGui asserts otherwise)
    if (width > 0.f && table->IsDefaultSizingPolicy && !(flags & ImGuiTableColumnFlags_WidthMask_))
        flags |= ImGuiTableColumnFlags_WidthFixed;
    ImGui::TableSetupColumn(label, flags, width, user_id);
    return 0;
}

// ui.TableSetupScrollFreeze(columns, rows): the columns on the left and the rows on the top stay (with ScrollX / Y)
int UiTableSetupScrollFreeze(lua_State* L)
{
    GW_UI_ENTER(L, "TableSetupScrollFreeze");
    int columns = CheckInt(L, 1);
    int rows = CheckInt(L, 2);
    GW_UI_REQUIRE_TABLE(L);
    if (const pcstr error = TableSetupError())
        return luaL_error(L, "ui.%s: %s", fn_name, error);
    const int count = ImGui::TableGetColumnCount();
    if (columns < 0 || columns > count || rows < 0 || rows > 127)
    {
        WarnArgs(fn_name, "columns 0..columns of the table, rows 0..127: clamped");
        columns = columns < 0 ? 0 : (columns > count ? count : columns);
        rows = rows < 0 ? 0 : (rows > 127 ? 127 : rows);
    }
    ImGui::TableSetupScrollFreeze(columns, rows);
    return 0;
}

// ui.TableHeadersRow(): the row of the headers from TableSetupColumn
int UiTableHeadersRow(lua_State* L)
{
    GW_UI_ENTER(L, "TableHeadersRow");
    GW_UI_REQUIRE_TABLE(L);
    ImGui::TableHeadersRow();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Plots: an array of Lua numbers, at most 4096 (the values are copied)
// ---------------------------------------------------------------------------------------------
xr_vector<float> g_plot_values;

// ui.PlotLines / ui.PlotHistogram(label, values[, offset, overlay, scale_min, scale_max, w, h]).
// offset - 0-based, the first value shown (a ring buffer); scale nil - from the values
template <bool Histogram>
int UiPlot(lua_State* L)
{
    GW_UI_ENTER(L, Histogram ? "PlotHistogram" : "PlotLines");
    const pcstr label = CheckLabel(L, 1, fn_name);
    luaL_checktype(L, 2, LUA_TTABLE);
    size_t count = lua_objlen(L, 2);
    if (count > imgui::kMaxPlotValues)
    {
        imgui::WarnOnce(imgui::kWarnTruncated, "ui.%s: an array of %u values is cut to %u (this warning is shown once)",
            fn_name, static_cast<u32>(count), imgui::kMaxPlotValues);
        count = imgui::kMaxPlotValues;
    }
    g_plot_values.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        lua_rawgeti(L, 2, static_cast<int>(i + 1));
        g_plot_values[i] = lua_type(L, -1) == LUA_TNUMBER ? ToFloat(lua_tonumber(L, -1), 0.f) : 0.f;
        lua_pop(L, 1);
    }
    int offset = OptInt(L, 3, 0);
    offset = count ? ((offset % static_cast<int>(count)) + static_cast<int>(count)) % static_cast<int>(count) : 0;
    const pcstr overlay = OptLabel(L, 4, fn_name);
    const float scale_min = OptFloat(L, 5, FLT_MAX);
    const float scale_max = OptFloat(L, 6, FLT_MAX);
    const ImVec2 size(OptFloat(L, 7, 0.f), OptFloat(L, 8, 0.f));
    if constexpr (Histogram)
        ImGui::PlotHistogram(label, g_plot_values.data(), static_cast<int>(count), offset, overlay, scale_min, scale_max, size);
    else
        ImGui::PlotLines(label, g_plot_values.data(), static_cast<int>(count), offset, overlay, scale_min, scale_max, size);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Tooltips
// ---------------------------------------------------------------------------------------------

// ui.SetTooltip(text): a tooltip at the mouse this frame
int UiSetTooltip(lua_State* L)
{
    GW_UI_ENTER(L, "SetTooltip");
    const Text text = CheckText(L, 1, fn_name);
    ImGui::SetTooltip("%.*s", text.length, text.data);
    return 0;
}

// ui.SetItemTooltip(text): the tooltip of the last widget (when it is hovered)
int UiSetItemTooltip(lua_State* L)
{
    GW_UI_ENTER(L, "SetItemTooltip");
    const Text text = CheckText(L, 1, fn_name);
    ImGui::SetItemTooltip("%.*s", text.length, text.data);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Clipboard: write only (an addon never reads what the player copied elsewhere)
// ---------------------------------------------------------------------------------------------

// ui.SetClipboardText(text): the text goes to the clipboard of the OS (ImGui -> the platform backend of the engine,
// SDL_SetClipboardText). At most kMaxTextLength bytes (cut on a UTF-8 boundary, a warning once). There is no
// GetClipboardText: reading the clipboard is not given to addons (privacy)
int UiSetClipboardText(lua_State* L)
{
    GW_UI_ENTER(L, "SetClipboardText");
    const Text text = CheckText(L, 1, fn_name);
    // A Lua string ends with a NUL; a cut one needs its own copy
    if (text.data[text.length] == 0)
        ImGui::SetClipboardText(text.data);
    else
    {
        const xr_string copy(text.data, static_cast<size_t>(text.length));
        ImGui::SetClipboardText(copy.c_str());
    }
    return 0;
}

// ui.BeginTooltip() -> open / ui.BeginItemTooltip() -> open (the last widget is hovered). EndTooltip only after true
template <bool Item>
int UiBeginTooltip(lua_State* L)
{
    GW_UI_ENTER(L, Item ? "BeginItemTooltip" : "BeginTooltip");
    GW_UI_CAN_PUSH(L);
    const bool open = Item ? ImGui::BeginItemTooltip() : ImGui::BeginTooltip();
    if (open)
        imgui::Push(EScope::Tooltip);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndTooltip(lua_State* L)
{
    GW_UI_ENTER(L, "EndTooltip");
    GW_UI_CHECK_POP(L, EScope::Tooltip);
    ImGui::EndTooltip();
    imgui::Pop();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------------------------

// ui.OpenPopup(str_id[, flags]): BeginPopup(str_id) returns true from now on (call it once, not every frame)
int UiOpenPopup(lua_State* L)
{
    GW_UI_ENTER(L, "OpenPopup");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const int flags = OptFlags(L, 2, kPopupAllowed, fn_name);
    ImGui::OpenPopup(id, flags);
    return 0;
}

// ui.BeginPopup(str_id[, window_flags]) -> open. EndPopup only after true
int UiBeginPopup(lua_State* L)
{
    GW_UI_ENTER(L, "BeginPopup");
    const pcstr id = CheckLabel(L, 1, fn_name);
    const int flags = OptFlags(L, 2, kWindowFlagsAllowed, fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginPopup(id, flags);
    if (open)
        imgui::Push(EScope::Popup);
    lua_pushboolean(L, open);
    return 1;
}

// ui.BeginPopupModal(name[, open, window_flags]) -> visible, open. EndPopup only after visible == true.
// open: a boolean - the modal has a close button; nil - no button
int UiBeginPopupModal(lua_State* L)
{
    GW_UI_ENTER(L, "BeginPopupModal");
    const pcstr name = CheckLabel(L, 1, fn_name);
    const bool closable = lua_type(L, 2) == LUA_TBOOLEAN;
    bool open = closable ? lua_toboolean(L, 2) != 0 : true;
    const int flags = OptFlags(L, closable || lua_isnoneornil(L, 2) ? 3 : 2, kWindowFlagsAllowed, fn_name);
    GW_UI_CAN_PUSH(L);
    const bool visible = ImGui::BeginPopupModal(name, closable ? &open : nullptr, flags);
    if (visible)
        imgui::Push(EScope::Popup);
    lua_pushboolean(L, visible);
    lua_pushboolean(L, open);
    return 2;
}

// ui.BeginPopupContextItem([str_id, flags]) -> open: a popup of the last widget on its click (right by default).
// str_id nil - the id of the last widget (it must have one: not after Text). EndPopup only after true
int UiBeginPopupContextItem(lua_State* L)
{
    GW_UI_ENTER(L, "BeginPopupContextItem");
    const pcstr id = OptLabel(L, 1, fn_name);
    int flags = lua_isnoneornil(L, 2) ? ImGuiPopupFlags_MouseButtonRight : OptFlags(L, 2, kPopupAllowed, fn_name);
    if ((flags & ImGuiPopupFlags_MouseButtonMask_) > ImGuiPopupFlags_MouseButtonMiddle)
        flags = (flags & ~ImGuiPopupFlags_MouseButtonMask_) | ImGuiPopupFlags_MouseButtonRight;
    if (!id && ImGui::GetItemID() == 0)
        return luaL_error(L, "ui.%s: the last widget has no id (Text, Separator...): pass str_id", fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginPopupContextItem(id, flags);
    if (open)
        imgui::Push(EScope::Popup);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndPopup(lua_State* L)
{
    GW_UI_ENTER(L, "EndPopup");
    GW_UI_CHECK_POP(L, EScope::Popup);
    ImGui::EndPopup();
    imgui::Pop();
    return 0;
}

// ui.CloseCurrentPopup(): inside BeginPopup* / a menu - closes it (EndPopup is still called)
int UiCloseCurrentPopup(lua_State* L)
{
    GW_UI_ENTER(L, "CloseCurrentPopup");
    ImGui::CloseCurrentPopup();
    return 0;
}

// ui.IsPopupOpen(str_id[, flags]) -> bool. flags: PopupFlags.AnyPopupId (str_id ignored), AnyPopupLevel (with AnyPopupId)
int UiIsPopupOpen(lua_State* L)
{
    GW_UI_ENTER(L, "IsPopupOpen");
    int flags = OptFlags(L, 2, kPopupAllowed, fn_name);
    const pcstr id = (flags & ImGuiPopupFlags_AnyPopupId) ? "" : CheckLabel(L, 1, fn_name);
    if ((flags & ImGuiPopupFlags_AnyPopupLevel) && !(flags & ImGuiPopupFlags_AnyPopupId))
    {
        WarnArgs(fn_name, "PopupFlags.AnyPopupLevel needs AnyPopupId (a string id is tested at its own level), ignored");
        flags &= ~ImGuiPopupFlags_AnyPopupLevel;
    }
    lua_pushboolean(L, ImGui::IsPopupOpen(id, flags));
    return 1;
}

// ---------------------------------------------------------------------------------------------
// The menu bar of the window (the tool needs flags = {menu_bar = true})
// ---------------------------------------------------------------------------------------------

// ui.BeginMenuBar() -> open (false without menu_bar). EndMenuBar only after true
int UiBeginMenuBar(lua_State* L)
{
    GW_UI_ENTER(L, "BeginMenuBar");
    if (ImGui::GetCurrentWindow()->DC.MenuBarAppending)
        return luaL_error(L, "ui.%s: the menu bar is open already (EndMenuBar first)", fn_name);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginMenuBar();
    if (open)
        imgui::Push(EScope::MenuBar);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndMenuBar(lua_State* L)
{
    GW_UI_ENTER(L, "EndMenuBar");
    GW_UI_CHECK_POP(L, EScope::MenuBar);
    ImGui::EndMenuBar();
    imgui::Pop();
    return 0;
}

// ui.BeginMenu(label[, enabled]) -> open. EndMenu only after true
int UiBeginMenu(lua_State* L)
{
    GW_UI_ENTER(L, "BeginMenu");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const bool enabled = OptBool(L, 2, true);
    GW_UI_CAN_PUSH(L);
    const bool open = ImGui::BeginMenu(label, enabled);
    if (open)
        imgui::Push(EScope::Menu);
    lua_pushboolean(L, open);
    return 1;
}

int UiEndMenu(lua_State* L)
{
    GW_UI_ENTER(L, "EndMenu");
    GW_UI_CHECK_POP(L, EScope::Menu);
    ImGui::EndMenu();
    imgui::Pop();
    return 0;
}

// ui.MenuItem(label[, shortcut, selected, enabled]) -> pressed. shortcut - only a text (no key is bound)
int UiMenuItem(lua_State* L)
{
    GW_UI_ENTER(L, "MenuItem");
    const pcstr label = CheckLabel(L, 1, fn_name);
    const pcstr shortcut = OptLabel(L, 2, fn_name);
    const bool selected = OptBool(L, 3, false);
    const bool enabled = OptBool(L, 4, true);
    lua_pushboolean(L, ImGui::MenuItem(label, shortcut, selected, enabled));
    return 1;
}

// ---------------------------------------------------------------------------------------------
// The state of the last widget and of the window
// ---------------------------------------------------------------------------------------------

// ui.IsItemHovered([flags]) -> bool: flags - ui.HoveredFlags (those for an item)
int UiIsItemHovered(lua_State* L)
{
    GW_UI_ENTER(L, "IsItemHovered");
    lua_pushboolean(L, ImGui::IsItemHovered(OptFlags(L, 1, kItemHoveredAllowed, fn_name)));
    return 1;
}

// ui.IsItemActive() -> bool (held, being edited)
int UiIsItemActive(lua_State* L)
{
    GW_UI_ENTER(L, "IsItemActive");
    lua_pushboolean(L, ImGui::IsItemActive());
    return 1;
}

// ui.IsItemClicked([mouse_button]) -> bool: mouse_button - ui.MouseButton (Left by default)
int UiIsItemClicked(lua_State* L)
{
    GW_UI_ENTER(L, "IsItemClicked");
    const int button = OptInt(L, 1, ImGuiMouseButton_Left);
    if (button < 0 || button >= ImGuiMouseButton_COUNT)
        return luaL_argerror(L, 1, "ui.MouseButton.Left, Right or Middle");
    lua_pushboolean(L, ImGui::IsItemClicked(button));
    return 1;
}

// ui.IsItemEdited() -> bool (the value changed this frame)
int UiIsItemEdited(lua_State* L)
{
    GW_UI_ENTER(L, "IsItemEdited");
    lua_pushboolean(L, ImGui::IsItemEdited());
    return 1;
}

// ui.IsItemDeactivatedAfterEdit() -> bool (the editing ended with a change: the moment to apply a value)
int UiIsItemDeactivatedAfterEdit(lua_State* L)
{
    GW_UI_ENTER(L, "IsItemDeactivatedAfterEdit");
    lua_pushboolean(L, ImGui::IsItemDeactivatedAfterEdit());
    return 1;
}

// ui.IsWindowFocused([flags]) -> bool: flags - ui.FocusedFlags
int UiIsWindowFocused(lua_State* L)
{
    GW_UI_ENTER(L, "IsWindowFocused");
    lua_pushboolean(L, ImGui::IsWindowFocused(OptFlags(L, 1, kFocusedAllowed, fn_name)));
    return 1;
}

// ui.IsWindowHovered([flags]) -> bool: flags - ui.HoveredFlags (those for a window)
int UiIsWindowHovered(lua_State* L)
{
    GW_UI_ENTER(L, "IsWindowHovered");
    lua_pushboolean(L, ImGui::IsWindowHovered(OptFlags(L, 1, kWindowHoveredAllowed, fn_name)));
    return 1;
}

// ui.SetKeyboardFocusHere([offset]): the keyboard focus to the next widget (offset 1 - the one after it, -1 - the last)
int UiSetKeyboardFocusHere(lua_State* L)
{
    GW_UI_ENTER(L, "SetKeyboardFocusHere");
    int offset = OptInt(L, 1, 0);
    if (offset < -1)
        offset = -1;
    ImGui::SetKeyboardFocusHere(offset);
    return 0;
}

// ui.SetItemDefaultFocus(): the last widget is the default of the navigation (a popup, a combo)
int UiSetItemDefaultFocus(lua_State* L)
{
    GW_UI_ENTER(L, "SetItemDefaultFocus");
    ImGui::SetItemDefaultFocus();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// ID and style
// ---------------------------------------------------------------------------------------------

// ui.PushID(id) / ui.PopID(): id - a string or a number; the ids of the widgets inside are unique per id
int UiPushID(lua_State* L)
{
    GW_UI_ENTER(L, "PushID");
    const int type = lua_type(L, 1);
    if (type != LUA_TSTRING && type != LUA_TNUMBER)
        return luaL_argerror(L, 1, "a string or a number");
    GW_UI_CAN_PUSH(L);
    if (type == LUA_TNUMBER)
    {
        // An integer is its own id (as PushID(int) of C++); a fraction or a big number - by its text
        const lua_Number number = lua_tonumber(L, 1);
        if (number >= INT_MIN && number <= INT_MAX && number == static_cast<lua_Number>(static_cast<int>(number)))
            ImGui::PushID(static_cast<int>(number));
        else
        {
            char text[32];
            xr_sprintf(text, "%.17g", number);
            ImGui::PushID(text);
        }
    }
    else
    {
        size_t length = 0;
        const pcstr text = lua_tolstring(L, 1, &length);
        ImGui::PushID(text, text + length);
    }
    imgui::Push(EScope::Id);
    return 0;
}

int UiPopID(lua_State* L)
{
    GW_UI_ENTER(L, "PopID");
    GW_UI_CHECK_POP(L, EScope::Id);
    ImGui::PopID();
    imgui::Pop();
    return 0;
}

// ui.PushStyleColor(idx, color) / ui.PopStyleColor([count]): idx - ui.Col, color - 0xAARRGGBB or {r, g, b[, a]}
int UiPushStyleColor(lua_State* L)
{
    GW_UI_ENTER(L, "PushStyleColor");
    const int index = CheckInt(L, 1);
    if (index < 0 || index >= ImGuiCol_COUNT)
        return luaL_argerror(L, 1, "a value of ui.Col");
    const ImVec4 color = CheckColor(L, 2);
    GW_UI_CAN_PUSH(L);
    ImGui::PushStyleColor(index, color);
    imgui::Push(EScope::StyleColor);
    return 0;
}

// Pop*(count): each record is checked and closed in turn (an error leaves the stack consistent)
int PopCount(lua_State* L)
{
    const int count = OptInt(L, 1, 1);
    const int most = static_cast<int>(imgui::kMaxStackDepth);
    return count < 1 ? 1 : (count > most ? most : count);
}

int UiPopStyleColor(lua_State* L)
{
    GW_UI_ENTER(L, "PopStyleColor");
    const int count = PopCount(L);
    for (int i = 0; i < count; ++i)
    {
        GW_UI_CHECK_POP(L, EScope::StyleColor);
        ImGui::PopStyleColor(1);
        imgui::Pop();
    }
    return 0;
}

// ui.PushStyleVar(idx, x[, y]) / ui.PopStyleVar([count]): idx - ui.StyleVar; a pair variable (padding, spacing...)
// takes x, y (y nil - x)
int UiPushStyleVar(lua_State* L)
{
    GW_UI_ENTER(L, "PushStyleVar");
    const int index = CheckInt(L, 1);
    if (index < 0 || index >= ImGuiStyleVar_COUNT)
        return luaL_argerror(L, 1, "a value of ui.StyleVar");
    const float x = CheckFloat(L, 2);
    const float y = OptFloat(L, 3, x);
    const ImGuiStyleVarInfo* info = ImGui::GetStyleVarInfo(index);
    if (info->DataType != ImGuiDataType_Float || (info->Count != 1 && info->Count != 2))
        return luaL_argerror(L, 1, "this style variable is not supported");
    GW_UI_CAN_PUSH(L);
    if (info->Count == 1)
        ImGui::PushStyleVar(index, x);
    else
        ImGui::PushStyleVar(index, ImVec2(x, y));
    imgui::Push(EScope::StyleVar);
    return 0;
}

int UiPopStyleVar(lua_State* L)
{
    GW_UI_ENTER(L, "PopStyleVar");
    const int count = PopCount(L);
    for (int i = 0; i < count; ++i)
    {
        GW_UI_CHECK_POP(L, EScope::StyleVar);
        ImGui::PopStyleVar(1);
        imgui::Pop();
    }
    return 0;
}

// ui.BeginDisabled([disabled]) / ui.EndDisabled(): the widgets inside are grey and take no input (disabled true by
// default; EndDisabled always)
int UiBeginDisabled(lua_State* L)
{
    GW_UI_ENTER(L, "BeginDisabled");
    const bool disabled = OptBool(L, 1, true);
    GW_UI_CAN_PUSH(L);
    ImGui::BeginDisabled(disabled);
    imgui::Push(EScope::Disabled);
    return 0;
}

int UiEndDisabled(lua_State* L)
{
    GW_UI_ENTER(L, "EndDisabled");
    GW_UI_CHECK_POP(L, EScope::Disabled);
    ImGui::EndDisabled();
    imgui::Pop();
    return 0;
}

// The functions of `ui`: name as in C++ ImGui -> the C function
const luaL_Reg kUiFunctions[] = {
    // Layout
    { "SameLine", &UiSameLine },
    { "NewLine", &UiNewLine },
    { "Spacing", &UiSpacing },
    { "Separator", &UiSeparator },
    { "SeparatorText", &UiSeparatorText },
    { "Dummy", &UiDummy },
    { "Indent", &UiIndent },
    { "Unindent", &UiUnindent },
    { "BeginGroup", &UiBeginGroup },
    { "EndGroup", &UiEndGroup },
    { "BeginChild", &UiBeginChild },
    { "EndChild", &UiEndChild },
    { "SetNextItemWidth", &UiSetNextItemWidth },
    { "PushItemWidth", &UiPushItemWidth },
    { "PopItemWidth", &UiPopItemWidth },
    { "AlignTextToFramePadding", &UiAlignTextToFramePadding },
    { "GetContentRegionAvail", &UiGetContentRegionAvail },
    { "GetWindowSize", &UiGetWindowSize },
    { "GetCursorPos", &UiGetCursorPos },
    { "SetCursorPos", &UiSetCursorPos },
    { "CalcTextSize", &UiCalcTextSize },
    { "GetTextLineHeight", &UiGetTextLineHeight },
    { "GetFrameHeight", &UiGetFrameHeight },
    // Text
    { "Text", &UiText },
    { "TextColored", &UiTextColored },
    { "TextDisabled", &UiTextDisabled },
    { "TextWrapped", &UiTextWrapped },
    { "LabelText", &UiLabelText },
    { "BulletText", &UiBulletText },
    { "Bullet", &UiBullet },
    { "PushTextWrapPos", &UiPushTextWrapPos },
    { "PopTextWrapPos", &UiPopTextWrapPos },
    // Buttons
    { "Button", &UiButton },
    { "SmallButton", &UiSmallButton },
    { "InvisibleButton", &UiInvisibleButton },
    { "ArrowButton", &UiArrowButton },
    { "Checkbox", &UiCheckbox },
    { "CheckboxFlags", &UiCheckboxFlags },
    { "RadioButton", &UiRadioButton },
    { "ProgressBar", &UiProgressBar },
    // Numbers
    { "SliderFloat", &UiSliderN<float, 1> },
    { "SliderFloat2", &UiSliderN<float, 2> },
    { "SliderFloat3", &UiSliderN<float, 3> },
    { "SliderFloat4", &UiSliderN<float, 4> },
    { "SliderInt", &UiSliderN<int, 1> },
    { "SliderInt2", &UiSliderN<int, 2> },
    { "SliderInt3", &UiSliderN<int, 3> },
    { "SliderInt4", &UiSliderN<int, 4> },
    { "SliderAngle", &UiSliderAngle },
    { "DragFloat", &UiDragN<float, 1> },
    { "DragFloat2", &UiDragN<float, 2> },
    { "DragFloat3", &UiDragN<float, 3> },
    { "DragFloat4", &UiDragN<float, 4> },
    { "DragInt", &UiDragN<int, 1> },
    { "DragInt2", &UiDragN<int, 2> },
    { "DragInt3", &UiDragN<int, 3> },
    { "DragInt4", &UiDragN<int, 4> },
    { "InputFloat", &UiInputFloatN<1> },
    { "InputFloat2", &UiInputFloatN<2> },
    { "InputFloat3", &UiInputFloatN<3> },
    { "InputFloat4", &UiInputFloatN<4> },
    { "InputInt", &UiInputInt },
    // Text input
    { "InputText", &UiInputText },
    { "InputTextWithHint", &UiInputTextWithHint },
    { "InputTextMultiline", &UiInputTextMultiline },
    // Choice
    { "Combo", &UiCombo },
    { "BeginCombo", &UiBeginCombo },
    { "EndCombo", &UiEndCombo },
    { "Selectable", &UiSelectable },
    { "ListBox", &UiListBox },
    { "BeginListBox", &UiBeginListBox },
    { "EndListBox", &UiEndListBox },
    // Colors
    { "ColorEdit3", &UiColorEditN<3> },
    { "ColorEdit4", &UiColorEditN<4> },
    { "ColorButton", &UiColorButton },
    // Trees
    { "TreeNode", &UiTreeNode },
    { "TreeNodeEx", &UiTreeNodeEx },
    { "TreePop", &UiTreePop },
    { "CollapsingHeader", &UiCollapsingHeader },
    { "SetNextItemOpen", &UiSetNextItemOpen },
    // Tabs
    { "BeginTabBar", &UiBeginTabBar },
    { "EndTabBar", &UiEndTabBar },
    { "BeginTabItem", &UiBeginTabItem },
    { "EndTabItem", &UiEndTabItem },
    // Tables
    { "BeginTable", &UiBeginTable },
    { "EndTable", &UiEndTable },
    { "TableNextRow", &UiTableNextRow },
    { "TableNextColumn", &UiTableNextColumn },
    { "TableSetColumnIndex", &UiTableSetColumnIndex },
    { "TableSetupColumn", &UiTableSetupColumn },
    { "TableSetupScrollFreeze", &UiTableSetupScrollFreeze },
    { "TableHeadersRow", &UiTableHeadersRow },
    // Plots
    { "PlotLines", &UiPlot<false> },
    { "PlotHistogram", &UiPlot<true> },
    // Tooltips
    { "SetTooltip", &UiSetTooltip },
    { "SetItemTooltip", &UiSetItemTooltip },
    { "BeginTooltip", &UiBeginTooltip<false> },
    { "BeginItemTooltip", &UiBeginTooltip<true> },
    { "EndTooltip", &UiEndTooltip },
    // Clipboard (write only)
    { "SetClipboardText", &UiSetClipboardText },
    // Popups
    { "OpenPopup", &UiOpenPopup },
    { "BeginPopup", &UiBeginPopup },
    { "BeginPopupModal", &UiBeginPopupModal },
    { "BeginPopupContextItem", &UiBeginPopupContextItem },
    { "EndPopup", &UiEndPopup },
    { "CloseCurrentPopup", &UiCloseCurrentPopup },
    { "IsPopupOpen", &UiIsPopupOpen },
    // The menu bar of the window
    { "BeginMenuBar", &UiBeginMenuBar },
    { "EndMenuBar", &UiEndMenuBar },
    { "BeginMenu", &UiBeginMenu },
    { "EndMenu", &UiEndMenu },
    { "MenuItem", &UiMenuItem },
    // State
    { "IsItemHovered", &UiIsItemHovered },
    { "IsItemActive", &UiIsItemActive },
    { "IsItemClicked", &UiIsItemClicked },
    { "IsItemEdited", &UiIsItemEdited },
    { "IsItemDeactivatedAfterEdit", &UiIsItemDeactivatedAfterEdit },
    { "IsWindowFocused", &UiIsWindowFocused },
    { "IsWindowHovered", &UiIsWindowHovered },
    { "SetKeyboardFocusHere", &UiSetKeyboardFocusHere },
    { "SetItemDefaultFocus", &UiSetItemDefaultFocus },
    // ID and style
    { "PushID", &UiPushID },
    { "PopID", &UiPopID },
    { "PushStyleColor", &UiPushStyleColor },
    { "PopStyleColor", &UiPopStyleColor },
    { "PushStyleVar", &UiPushStyleVar },
    { "PopStyleVar", &UiPopStyleVar },
    { "BeginDisabled", &UiBeginDisabled },
    { "EndDisabled", &UiEndDisabled },
};

#undef GW_UI_REQUIRE_TABLE
#undef GW_UI_CHECK_POP
#undef GW_UI_CAN_PUSH
#undef GW_UI_ENTER
} // namespace

void RegisterUiFunctions(lua_State* L)
{
    for (const luaL_Reg& function : kUiFunctions)
    {
        lua_pushcfunction(L, function.func);
        lua_setfield(L, -2, function.name);
    }
    // The constant tables (ui.TableFlags.Borders...): the values of the ImGui of this engine by the names of ImGui,
    // without the flags that are dangerous for the frame of the host (addon_imgui_core.h)
    for (const ConstantTable& table : kConstantTables)
    {
        lua_createtable(L, 0, static_cast<int>(table.count));
        for (size_t i = 0; i < table.count; ++i)
        {
            lua_pushinteger(L, table.constants[i].value);
            lua_setfield(L, -2, table.constants[i].name);
        }
        lua_setfield(L, -2, table.name);
    }
}
} // namespace gw::addons::imgui_lua
