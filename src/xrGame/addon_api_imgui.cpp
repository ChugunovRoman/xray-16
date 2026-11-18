#include "StdAfx.h"

// Plugin API, group "imgui": the ImGui tools of the plugins and the widget table GwpImGuiApi
// (plans/addons_api_support/20-imgui-addon-tools.md, 4.3). See addon_api_imgui.h.

#include "addon_api_imgui.h"
#include "addon_imgui_core.h"
#include "addon_imgui_tools.h"
#include "addon_event_bus.h" // CallPluginGuarded
#include "addon_host.h"
#include "addon_perf.h"

#include "xrCore/Profiling/PerfMetrics.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

#if defined(_MSC_VER)
#include <intrin.h> // _ReturnAddress
#endif
#if !defined(XR_PLATFORM_WINDOWS)
#include <dlfcn.h> // dladdr
#endif

namespace gw::addons::imgui_plugin
{
namespace
{
using imgui_tools::EToolKind;
using imgui_tools::FrameInfo;
using imgui_tools::ToolDef;

// The constants of the ABI are the values of the common layer (addon_imgui_core.h, addon_imgui_tools.h)
static_assert(GWP_IMGUI_WINDOW_AUTO_RESIZE == imgui::kWindowAutoResize && GWP_IMGUI_WINDOW_NO_RESIZE ==
    imgui::kWindowNoResize && GWP_IMGUI_WINDOW_NO_COLLAPSE == imgui::kWindowNoCollapse &&
    GWP_IMGUI_WINDOW_NO_SCROLLBAR == imgui::kWindowNoScrollbar && GWP_IMGUI_WINDOW_HORIZONTAL_SCROLLBAR ==
    imgui::kWindowHorizontalScrollbar && GWP_IMGUI_WINDOW_MENU_BAR == imgui::kWindowMenuBar,
    "GWP_IMGUI_WINDOW_* are the values of imgui::EWindowFlags");
static_assert(GWP_IMGUI_MAX_STACK_DEPTH == imgui::kMaxStackDepth && GWP_IMGUI_MAX_INPUT_TEXT == imgui::kMaxInputText &&
    GWP_IMGUI_MAX_INPUT_TEXT_MULTILINE == imgui::kMaxInputTextMultiline && GWP_IMGUI_MAX_LIST_ITEMS ==
    imgui::kMaxListItems && GWP_IMGUI_MAX_PLOT_VALUES == imgui::kMaxPlotValues && GWP_IMGUI_MAX_FORMAT ==
    imgui::kMaxFormatLength, "GWP_IMGUI_MAX_* are the limits of addon_imgui_core.h");
static_assert(GWP_IMGUI_MAX_ID == imgui_tools::kMaxIdLength && GWP_IMGUI_MAX_TITLE == imgui_tools::kMaxTitleLength &&
    GWP_IMGUI_MAX_MENU == imgui_tools::kMaxMenuLength && GWP_IMGUI_MAX_MENU_DEPTH == imgui_tools::kMaxMenuDepth &&
    GWP_IMGUI_MAX_TOOLTIP == imgui_tools::kMaxTooltipLength && GWP_IMGUI_MAX_TOOLS_PER_ADDON ==
    imgui_tools::kMaxToolsPerOwner && GWP_IMGUI_MAX_TOOLS == imgui_tools::kMaxTools,
    "GWP_IMGUI_MAX_* are the limits of addon_imgui_tools.h");

constexpr u32 kToolFlagsAll = GWP_IMGUI_TOOL_DEV_ONLY | GWP_IMGUI_TOOL_HIDE_IN_LIGHT | GWP_IMGUI_TOOL_NO_SAVE_OPEN;
// The structure of version 0.1: the smallest GwpImGuiToolDesc::size imgui_tool_register takes. Fixed at the end of
// reserved (the last field of 0.1), not sizeof: a field appended later must not refuse the plugins built before it
// (review 2026-11-18; with sizeof the minimum would grow silently with the first appended field)
constexpr u32 kToolDescMinSize =
    static_cast<u32>(offsetof(GwpImGuiToolDesc, reserved) + sizeof(static_cast<GwpImGuiToolDesc*>(nullptr)->reserved));
static_assert(kToolDescMinSize <= sizeof(GwpImGuiToolDesc), "GwpImGuiToolDesc of 0.1 ends with reserved[2]");

// The warnings of the table once per tool (DrawFrame::warned): the bits of the common layer take 0..15
constexpr u32 kWarnPair = 1u << 16;     // an end_* without its begin_*, the stack of the pairs is full
constexpr u32 kWarnArgument = 1u << 17; // a NULL label or pointer, a component count, an index out of range
constexpr u32 kWarnPlace = 1u << 18;    // a table_* outside a table, begin_tab_item outside a tab bar...

// ---------------------------------------------------------------------------------------------
// Calls outside of draw: a warning once per module of the caller (the table has no `self`)
// ---------------------------------------------------------------------------------------------
#if defined(_MSC_VER)
#define GWP_UI_CALLER() _ReturnAddress()
#else
#define GWP_UI_CALLER() __builtin_return_address(0)
#endif

std::mutex g_outside_mutex;
xr_vector<const void*>* g_outside_modules = nullptr; // the modules warned already (lives with the process)

void WarnOutsideDraw(pcstr function, const void* caller)
{
    const void* module = nullptr;
    string_path name = "?";
#if defined(XR_PLATFORM_WINDOWS)
    HMODULE handle = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(caller), &handle))
    {
        module = handle;
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(handle, path, MAX_PATH);
        if (length && length < MAX_PATH)
        {
            const wchar_t* file = path;
            for (const wchar_t* c = path; *c; ++c)
            {
                if (*c == L'\\' || *c == L'/')
                    file = c + 1;
            }
            if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, name, sizeof(name), nullptr, nullptr))
                xr_strcpy(name, "?");
        }
    }
#else
    Dl_info info{};
    if (dladdr(caller, &info))
    {
        module = info.dli_fbase;
        if (info.dli_fname)
        {
            pcstr file = strrchr(info.dli_fname, '/');
            xr_strcpy(name, file ? file + 1 : info.dli_fname);
        }
    }
#endif
    {
        std::lock_guard lock(g_outside_mutex);
        if (!g_outside_modules)
            g_outside_modules = xr_new<xr_vector<const void*>>();
        for (const void* known : *g_outside_modules)
        {
            if (known == module)
                return;
        }
        g_outside_modules->push_back(module);
    }
    // The addon of the plugin library when the host knows the module (Windows), else the file of the module
    // (the list of the addons is read on the main thread only: a call from a thread of the plugin names the file)
    if (const pcstr addon = IsMainThread() ? PluginAddonIdOfModule(module) : nullptr)
    {
        Msg("~ [plugin:%s] GwpImGuiApi::%s called outside draw of an ImGui tool (or outside the main thread): ignored. "
            "The table is valid only inside draw (this warning is shown once per plugin)", addon, function);
        return;
    }
    Msg("~ [plugin api] GwpImGuiApi::%s called outside draw of an ImGui tool (or outside the main thread) by the module "
        "'%s': ignored. The table is valid only inside draw (this warning is shown once per module)", function, name);
}

// The first line of every function of the table: there is an ImGui frame and a window of a tool
#define GWP_UI_ENTER(name, fail)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!imgui::InDraw())                                                                                          \
        {                                                                                                              \
            WarnOutsideDraw(name, GWP_UI_CALLER());                                                                    \
            return fail;                                                                                               \
        }                                                                                                              \
    } while (0)
#define GWP_UI_ENTER_VOID(name)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!imgui::InDraw())                                                                                          \
        {                                                                                                              \
            WarnOutsideDraw(name, GWP_UI_CALLER());                                                                    \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------------------------
pcstr TextOr(pcstr text) { return text ? text : ""; }

float Finite(float value, float fallback = 0.f) { return std::isfinite(value) ? value : fallback; }

ImVec2 Size(float w, float h) { return ImVec2(Finite(w), Finite(h)); }

// A label (the id of the widget): NULL is refused
bool Label(pcstr label, pcstr function)
{
    if (label)
        return true;
    imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: the label (id) is NULL, the call is ignored (this warning is "
        "shown once)", function);
    return false;
}

// A pointer to a value: NULL is refused
bool Value(const void* value, pcstr function)
{
    if (value)
        return true;
    imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: the value pointer is NULL, the call is ignored (this warning is "
        "shown once)", function);
    return false;
}

// The count of the components of *_n: 2..4
bool Components(u32 n, pcstr function)
{
    if (n >= 2 && n <= 4)
        return true;
    imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: n = %u, 2..4 components are allowed, the call is ignored (this "
        "warning is shown once)", function, n);
    return false;
}

// 0xAARRGGBB -> the color of ImGui
ImVec4 Color(u32 argb)
{
    constexpr float k = 1.f / 255.f;
    return ImVec4(static_cast<float>((argb >> 16) & 0xFFu) * k, static_cast<float>((argb >> 8) & 0xFFu) * k,
        static_cast<float>(argb & 0xFFu) * k, static_cast<float>((argb >> 24) & 0xFFu) * k);
}

// Limits of the numbers of the sliders (ImGui asserts beyond them)
float SliderLimit(float value) { return std::clamp(Finite(value), -FLT_MAX / 2.f, FLT_MAX / 2.f); }
int SliderLimit(int value) { return std::clamp(value, INT_MIN / 2, INT_MAX / 2); }

// ---------------------------------------------------------------------------------------------
// Pairs (the shadow stack of addon_imgui_core.h): an error does nothing and warns once per tool
// ---------------------------------------------------------------------------------------------
bool CanPushOr(pcstr function)
{
    if (const pcstr error = imgui::CanPush())
    {
        imgui::WarnOnce(kWarnPair, "GwpImGuiApi::%s: %s; the call is ignored (this warning is shown once)", function,
            error);
        return false;
    }
    return true;
}

bool CheckPopOr(imgui::EScope scope, pcstr function)
{
    if (const pcstr error = imgui::CheckPop(scope))
    {
        imgui::WarnOnce(kWarnPair, "GwpImGuiApi::%s: %s; the call is ignored (this warning is shown once)", function,
            error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Flags of GW -> flags of ImGui
// ---------------------------------------------------------------------------------------------
struct FlagMap
{
    u32 gw;
    int imgui;
};

template <size_t N>
int MapFlags(u32 flags, const FlagMap (&map)[N], pcstr function)
{
    int result = 0;
    u32 known = 0;
    for (const FlagMap& entry : map)
    {
        known |= entry.gw;
        if (flags & entry.gw)
            result |= entry.imgui;
    }
    if (flags & ~known)
    {
        imgui::WarnOnce(imgui::kWarnFlags, "GwpImGuiApi::%s: unknown flag bits 0x%x are ignored (this warning is shown "
            "once)", function, flags & ~known);
    }
    return result;
}

// The lowest of the bits of `mask` set in `flags` (ImGui takes one of a group)
u32 OneOf(u32 flags, u32 mask)
{
    const u32 bits = flags & mask;
    return bits & (~bits + 1u);
}

ImGuiWindowFlags WindowFlags(u32 flags, pcstr function)
{
    if (flags & ~static_cast<u32>(imgui::kWindowAll))
    {
        imgui::WarnOnce(imgui::kWarnFlags, "GwpImGuiApi::%s: unknown window flag bits 0x%x are ignored (this warning "
            "is shown once)", function, flags & ~static_cast<u32>(imgui::kWindowAll));
    }
    return imgui::TranslateWindowFlags(flags & imgui::kWindowAll);
}

constexpr FlagMap kChildFlags[] = {
    { GWP_IMGUI_CHILD_BORDERS, ImGuiChildFlags_Borders },
    { GWP_IMGUI_CHILD_ALWAYS_USE_WINDOW_PADDING, ImGuiChildFlags_AlwaysUseWindowPadding },
    { GWP_IMGUI_CHILD_RESIZE_X, ImGuiChildFlags_ResizeX },
    { GWP_IMGUI_CHILD_RESIZE_Y, ImGuiChildFlags_ResizeY },
    { GWP_IMGUI_CHILD_AUTO_RESIZE_X, ImGuiChildFlags_AutoResizeX },
    { GWP_IMGUI_CHILD_AUTO_RESIZE_Y, ImGuiChildFlags_AutoResizeY },
    { GWP_IMGUI_CHILD_FRAME_STYLE, ImGuiChildFlags_FrameStyle },
};

constexpr u32 kTableSizing = GWP_IMGUI_TABLE_SIZING_FIXED_FIT | GWP_IMGUI_TABLE_SIZING_FIXED_SAME |
    GWP_IMGUI_TABLE_SIZING_STRETCH_PROP | GWP_IMGUI_TABLE_SIZING_STRETCH_SAME;
constexpr FlagMap kTableFlags[] = {
    { GWP_IMGUI_TABLE_RESIZABLE, ImGuiTableFlags_Resizable },
    { GWP_IMGUI_TABLE_REORDERABLE, ImGuiTableFlags_Reorderable },
    { GWP_IMGUI_TABLE_HIDEABLE, ImGuiTableFlags_Hideable },
    { GWP_IMGUI_TABLE_CONTEXT_MENU_IN_BODY, ImGuiTableFlags_ContextMenuInBody },
    { GWP_IMGUI_TABLE_ROW_BG, ImGuiTableFlags_RowBg },
    { GWP_IMGUI_TABLE_BORDERS_INNER_H, ImGuiTableFlags_BordersInnerH },
    { GWP_IMGUI_TABLE_BORDERS_OUTER_H, ImGuiTableFlags_BordersOuterH },
    { GWP_IMGUI_TABLE_BORDERS_INNER_V, ImGuiTableFlags_BordersInnerV },
    { GWP_IMGUI_TABLE_BORDERS_OUTER_V, ImGuiTableFlags_BordersOuterV },
    { GWP_IMGUI_TABLE_NO_BORDERS_IN_BODY, ImGuiTableFlags_NoBordersInBody },
    { GWP_IMGUI_TABLE_SIZING_FIXED_FIT, ImGuiTableFlags_SizingFixedFit },
    { GWP_IMGUI_TABLE_SIZING_FIXED_SAME, ImGuiTableFlags_SizingFixedSame },
    { GWP_IMGUI_TABLE_SIZING_STRETCH_PROP, ImGuiTableFlags_SizingStretchProp },
    { GWP_IMGUI_TABLE_SIZING_STRETCH_SAME, ImGuiTableFlags_SizingStretchSame },
    { GWP_IMGUI_TABLE_NO_HOST_EXTEND_X, ImGuiTableFlags_NoHostExtendX },
    { GWP_IMGUI_TABLE_NO_HOST_EXTEND_Y, ImGuiTableFlags_NoHostExtendY },
    { GWP_IMGUI_TABLE_NO_KEEP_COLUMNS_VISIBLE, ImGuiTableFlags_NoKeepColumnsVisible },
    { GWP_IMGUI_TABLE_PRECISE_WIDTHS, ImGuiTableFlags_PreciseWidths },
    { GWP_IMGUI_TABLE_NO_CLIP, ImGuiTableFlags_NoClip },
    { GWP_IMGUI_TABLE_PAD_OUTER_X, ImGuiTableFlags_PadOuterX },
    { GWP_IMGUI_TABLE_NO_PAD_OUTER_X, ImGuiTableFlags_NoPadOuterX },
    { GWP_IMGUI_TABLE_NO_PAD_INNER_X, ImGuiTableFlags_NoPadInnerX },
    { GWP_IMGUI_TABLE_SCROLL_X, ImGuiTableFlags_ScrollX },
    { GWP_IMGUI_TABLE_SCROLL_Y, ImGuiTableFlags_ScrollY },
    { GWP_IMGUI_TABLE_HIGHLIGHT_HOVERED_COLUMN, ImGuiTableFlags_HighlightHoveredColumn },
};

ImGuiTableFlags TableFlags(u32 flags)
{
    // The sizing policies of ImGui are values of one field, not bits: the first one in the order of the header
    const u32 sizing = flags & kTableSizing;
    if (sizing)
    {
        u32 first = 0;
        for (const u32 bit : { GWP_IMGUI_TABLE_SIZING_FIXED_FIT, GWP_IMGUI_TABLE_SIZING_FIXED_SAME,
                 GWP_IMGUI_TABLE_SIZING_STRETCH_PROP, GWP_IMGUI_TABLE_SIZING_STRETCH_SAME })
        {
            if (sizing & bit)
            {
                first = bit;
                break;
            }
        }
        flags = (flags & ~kTableSizing) | first;
    }
    return MapFlags(flags, kTableFlags, "begin_table");
}

constexpr FlagMap kColumnFlags[] = {
    { GWP_IMGUI_TABLE_COLUMN_DEFAULT_HIDE, ImGuiTableColumnFlags_DefaultHide },
    { GWP_IMGUI_TABLE_COLUMN_WIDTH_STRETCH, ImGuiTableColumnFlags_WidthStretch },
    { GWP_IMGUI_TABLE_COLUMN_WIDTH_FIXED, ImGuiTableColumnFlags_WidthFixed },
    { GWP_IMGUI_TABLE_COLUMN_NO_RESIZE, ImGuiTableColumnFlags_NoResize },
    { GWP_IMGUI_TABLE_COLUMN_NO_REORDER, ImGuiTableColumnFlags_NoReorder },
    { GWP_IMGUI_TABLE_COLUMN_NO_HIDE, ImGuiTableColumnFlags_NoHide },
    { GWP_IMGUI_TABLE_COLUMN_NO_CLIP, ImGuiTableColumnFlags_NoClip },
    { GWP_IMGUI_TABLE_COLUMN_NO_HEADER_LABEL, ImGuiTableColumnFlags_NoHeaderLabel },
    { GWP_IMGUI_TABLE_COLUMN_NO_HEADER_WIDTH, ImGuiTableColumnFlags_NoHeaderWidth },
    { GWP_IMGUI_TABLE_COLUMN_INDENT_ENABLE, ImGuiTableColumnFlags_IndentEnable },
    { GWP_IMGUI_TABLE_COLUMN_INDENT_DISABLE, ImGuiTableColumnFlags_IndentDisable },
};

ImGuiTableColumnFlags ColumnFlags(u32 flags)
{
    constexpr u32 width = GWP_IMGUI_TABLE_COLUMN_WIDTH_STRETCH | GWP_IMGUI_TABLE_COLUMN_WIDTH_FIXED;
    constexpr u32 indent = GWP_IMGUI_TABLE_COLUMN_INDENT_ENABLE | GWP_IMGUI_TABLE_COLUMN_INDENT_DISABLE;
    flags = (flags & ~(width | indent)) | OneOf(flags, width) | OneOf(flags, indent);
    return MapFlags(flags, kColumnFlags, "table_setup_column");
}

constexpr FlagMap kTreeFlags[] = {
    { GWP_IMGUI_TREE_SELECTED, ImGuiTreeNodeFlags_Selected },
    { GWP_IMGUI_TREE_FRAMED, ImGuiTreeNodeFlags_Framed },
    { GWP_IMGUI_TREE_ALLOW_OVERLAP, ImGuiTreeNodeFlags_AllowOverlap },
    { GWP_IMGUI_TREE_DEFAULT_OPEN, ImGuiTreeNodeFlags_DefaultOpen },
    { GWP_IMGUI_TREE_OPEN_ON_DOUBLE_CLICK, ImGuiTreeNodeFlags_OpenOnDoubleClick },
    { GWP_IMGUI_TREE_OPEN_ON_ARROW, ImGuiTreeNodeFlags_OpenOnArrow },
    { GWP_IMGUI_TREE_LEAF, ImGuiTreeNodeFlags_Leaf },
    { GWP_IMGUI_TREE_BULLET, ImGuiTreeNodeFlags_Bullet },
    { GWP_IMGUI_TREE_FRAME_PADDING, ImGuiTreeNodeFlags_FramePadding },
    { GWP_IMGUI_TREE_SPAN_AVAIL_WIDTH, ImGuiTreeNodeFlags_SpanAvailWidth },
    { GWP_IMGUI_TREE_SPAN_FULL_WIDTH, ImGuiTreeNodeFlags_SpanFullWidth },
    { GWP_IMGUI_TREE_SPAN_ALL_COLUMNS, ImGuiTreeNodeFlags_SpanAllColumns },
};

constexpr FlagMap kInputFlags[] = {
    { GWP_IMGUI_INPUT_CHARS_DECIMAL, ImGuiInputTextFlags_CharsDecimal },
    { GWP_IMGUI_INPUT_CHARS_HEXADECIMAL, ImGuiInputTextFlags_CharsHexadecimal },
    { GWP_IMGUI_INPUT_CHARS_SCIENTIFIC, ImGuiInputTextFlags_CharsScientific },
    { GWP_IMGUI_INPUT_CHARS_UPPERCASE, ImGuiInputTextFlags_CharsUppercase },
    { GWP_IMGUI_INPUT_CHARS_NO_BLANK, ImGuiInputTextFlags_CharsNoBlank },
    { GWP_IMGUI_INPUT_ALLOW_TAB_INPUT, ImGuiInputTextFlags_AllowTabInput },
    { GWP_IMGUI_INPUT_ENTER_RETURNS_TRUE, ImGuiInputTextFlags_EnterReturnsTrue },
    { GWP_IMGUI_INPUT_ESCAPE_CLEARS_ALL, ImGuiInputTextFlags_EscapeClearsAll },
    { GWP_IMGUI_INPUT_CTRL_ENTER_FOR_NEW_LINE, ImGuiInputTextFlags_CtrlEnterForNewLine },
    { GWP_IMGUI_INPUT_READ_ONLY, ImGuiInputTextFlags_ReadOnly },
    { GWP_IMGUI_INPUT_PASSWORD, ImGuiInputTextFlags_Password },
    { GWP_IMGUI_INPUT_AUTO_SELECT_ALL, ImGuiInputTextFlags_AutoSelectAll },
    { GWP_IMGUI_INPUT_NO_UNDO_REDO, ImGuiInputTextFlags_NoUndoRedo },
};

constexpr FlagMap kSliderFlags[] = {
    { GWP_IMGUI_SLIDER_LOGARITHMIC, ImGuiSliderFlags_Logarithmic },
    { GWP_IMGUI_SLIDER_NO_ROUND_TO_FORMAT, ImGuiSliderFlags_NoRoundToFormat },
    { GWP_IMGUI_SLIDER_NO_INPUT, ImGuiSliderFlags_NoInput },
    { GWP_IMGUI_SLIDER_ALWAYS_CLAMP, ImGuiSliderFlags_AlwaysClamp },
    { GWP_IMGUI_SLIDER_WRAP_AROUND, ImGuiSliderFlags_WrapAround },
};

// slider_*: WRAP_AROUND is a flag of drag_* only (ImGui asserts on it in a slider)
ImGuiSliderFlags SliderFlags(u32 flags, pcstr function)
{
    return MapFlags(flags & ~GWP_IMGUI_SLIDER_WRAP_AROUND, kSliderFlags, function);
}
ImGuiSliderFlags DragFlags(u32 flags, pcstr function) { return MapFlags(flags, kSliderFlags, function); }

constexpr FlagMap kColorFlags[] = {
    { GWP_IMGUI_COLOR_NO_ALPHA, ImGuiColorEditFlags_NoAlpha },
    { GWP_IMGUI_COLOR_NO_PICKER, ImGuiColorEditFlags_NoPicker },
    { GWP_IMGUI_COLOR_NO_OPTIONS, ImGuiColorEditFlags_NoOptions },
    { GWP_IMGUI_COLOR_NO_SMALL_PREVIEW, ImGuiColorEditFlags_NoSmallPreview },
    { GWP_IMGUI_COLOR_NO_INPUTS, ImGuiColorEditFlags_NoInputs },
    { GWP_IMGUI_COLOR_NO_TOOLTIP, ImGuiColorEditFlags_NoTooltip },
    { GWP_IMGUI_COLOR_NO_LABEL, ImGuiColorEditFlags_NoLabel },
    { GWP_IMGUI_COLOR_NO_DRAG_DROP, ImGuiColorEditFlags_NoDragDrop },
    { GWP_IMGUI_COLOR_NO_BORDER, ImGuiColorEditFlags_NoBorder },
    { GWP_IMGUI_COLOR_ALPHA_BAR, ImGuiColorEditFlags_AlphaBar },
    { GWP_IMGUI_COLOR_DISPLAY_RGB, ImGuiColorEditFlags_DisplayRGB },
    { GWP_IMGUI_COLOR_DISPLAY_HSV, ImGuiColorEditFlags_DisplayHSV },
    { GWP_IMGUI_COLOR_DISPLAY_HEX, ImGuiColorEditFlags_DisplayHex },
    { GWP_IMGUI_COLOR_UINT8, ImGuiColorEditFlags_Uint8 },
    { GWP_IMGUI_COLOR_FLOAT, ImGuiColorEditFlags_Float },
};

ImGuiColorEditFlags ColorFlags(u32 flags, pcstr function)
{
    // One of each group (ImGui asserts on two)
    constexpr u32 display = GWP_IMGUI_COLOR_DISPLAY_RGB | GWP_IMGUI_COLOR_DISPLAY_HSV | GWP_IMGUI_COLOR_DISPLAY_HEX;
    constexpr u32 data = GWP_IMGUI_COLOR_UINT8 | GWP_IMGUI_COLOR_FLOAT;
    flags = (flags & ~(display | data)) | OneOf(flags, display) | OneOf(flags, data);
    return MapFlags(flags, kColorFlags, function);
}

constexpr FlagMap kSelectableFlags[] = {
    { GWP_IMGUI_SELECTABLE_NO_AUTO_CLOSE_POPUPS, ImGuiSelectableFlags_NoAutoClosePopups },
    { GWP_IMGUI_SELECTABLE_SPAN_ALL_COLUMNS, ImGuiSelectableFlags_SpanAllColumns },
    { GWP_IMGUI_SELECTABLE_ALLOW_DOUBLE_CLICK, ImGuiSelectableFlags_AllowDoubleClick },
    { GWP_IMGUI_SELECTABLE_DISABLED, ImGuiSelectableFlags_Disabled },
    { GWP_IMGUI_SELECTABLE_ALLOW_OVERLAP, ImGuiSelectableFlags_AllowOverlap },
    { GWP_IMGUI_SELECTABLE_HIGHLIGHT, ImGuiSelectableFlags_Highlight },
};

constexpr FlagMap kComboFlags[] = {
    { GWP_IMGUI_COMBO_POPUP_ALIGN_LEFT, ImGuiComboFlags_PopupAlignLeft },
    { GWP_IMGUI_COMBO_HEIGHT_SMALL, ImGuiComboFlags_HeightSmall },
    { GWP_IMGUI_COMBO_HEIGHT_REGULAR, ImGuiComboFlags_HeightRegular },
    { GWP_IMGUI_COMBO_HEIGHT_LARGE, ImGuiComboFlags_HeightLarge },
    { GWP_IMGUI_COMBO_HEIGHT_LARGEST, ImGuiComboFlags_HeightLargest },
    { GWP_IMGUI_COMBO_NO_ARROW_BUTTON, ImGuiComboFlags_NoArrowButton },
    { GWP_IMGUI_COMBO_NO_PREVIEW, ImGuiComboFlags_NoPreview },
    { GWP_IMGUI_COMBO_WIDTH_FIT_PREVIEW, ImGuiComboFlags_WidthFitPreview },
};

ImGuiComboFlags ComboFlags(u32 flags)
{
    constexpr u32 height = GWP_IMGUI_COMBO_HEIGHT_SMALL | GWP_IMGUI_COMBO_HEIGHT_REGULAR | GWP_IMGUI_COMBO_HEIGHT_LARGE |
        GWP_IMGUI_COMBO_HEIGHT_LARGEST;
    flags = (flags & ~height) | OneOf(flags, height);
    // NO_PREVIEW goes alone: not with NO_ARROW_BUTTON (nothing left to show), not with WIDTH_FIT_PREVIEW (ImGui asserts)
    if (flags & GWP_IMGUI_COMBO_NO_PREVIEW)
        flags &= ~(GWP_IMGUI_COMBO_NO_ARROW_BUTTON | GWP_IMGUI_COMBO_WIDTH_FIT_PREVIEW);
    return MapFlags(flags, kComboFlags, "begin_combo");
}

constexpr FlagMap kTabBarFlags[] = {
    { GWP_IMGUI_TAB_BAR_REORDERABLE, ImGuiTabBarFlags_Reorderable },
    { GWP_IMGUI_TAB_BAR_AUTO_SELECT_NEW_TABS, ImGuiTabBarFlags_AutoSelectNewTabs },
    { GWP_IMGUI_TAB_BAR_TAB_LIST_POPUP_BUTTON, ImGuiTabBarFlags_TabListPopupButton },
    { GWP_IMGUI_TAB_BAR_NO_CLOSE_WITH_MIDDLE_MOUSE_BUTTON, ImGuiTabBarFlags_NoCloseWithMiddleMouseButton },
    { GWP_IMGUI_TAB_BAR_NO_TAB_LIST_SCROLLING_BUTTONS, ImGuiTabBarFlags_NoTabListScrollingButtons },
    { GWP_IMGUI_TAB_BAR_NO_TOOLTIP, ImGuiTabBarFlags_NoTooltip },
};

constexpr FlagMap kTabItemFlags[] = {
    { GWP_IMGUI_TAB_ITEM_UNSAVED_DOCUMENT, ImGuiTabItemFlags_UnsavedDocument },
    { GWP_IMGUI_TAB_ITEM_SET_SELECTED, ImGuiTabItemFlags_SetSelected },
    { GWP_IMGUI_TAB_ITEM_NO_CLOSE_WITH_MIDDLE_MOUSE_BUTTON, ImGuiTabItemFlags_NoCloseWithMiddleMouseButton },
    { GWP_IMGUI_TAB_ITEM_NO_TOOLTIP, ImGuiTabItemFlags_NoTooltip },
    { GWP_IMGUI_TAB_ITEM_NO_REORDER, ImGuiTabItemFlags_NoReorder },
};

constexpr FlagMap kCondFlags[] = {
    { GWP_IMGUI_COND_ALWAYS, ImGuiCond_Always },
    { GWP_IMGUI_COND_ONCE, ImGuiCond_Once },
    { GWP_IMGUI_COND_FIRST_USE_EVER, ImGuiCond_FirstUseEver },
    { GWP_IMGUI_COND_APPEARING, ImGuiCond_Appearing },
};

ImGuiCond Cond(u32 cond, pcstr function)
{
    constexpr u32 all = GWP_IMGUI_COND_ALWAYS | GWP_IMGUI_COND_ONCE | GWP_IMGUI_COND_FIRST_USE_EVER |
        GWP_IMGUI_COND_APPEARING;
    return MapFlags((cond & ~all) | OneOf(cond, all), kCondFlags, function); // one condition (ImGui asserts on two)
}

constexpr FlagMap kHoveredFlags[] = {
    { GWP_IMGUI_HOVERED_CHILD_WINDOWS, ImGuiHoveredFlags_ChildWindows },
    { GWP_IMGUI_HOVERED_ROOT_WINDOW, ImGuiHoveredFlags_RootWindow },
    { GWP_IMGUI_HOVERED_ANY_WINDOW, ImGuiHoveredFlags_AnyWindow },
    { GWP_IMGUI_HOVERED_ALLOW_WHEN_BLOCKED_BY_POPUP, ImGuiHoveredFlags_AllowWhenBlockedByPopup },
    { GWP_IMGUI_HOVERED_ALLOW_WHEN_BLOCKED_BY_ACTIVE_ITEM, ImGuiHoveredFlags_AllowWhenBlockedByActiveItem },
    { GWP_IMGUI_HOVERED_ALLOW_WHEN_OVERLAPPED, ImGuiHoveredFlags_AllowWhenOverlapped },
    { GWP_IMGUI_HOVERED_ALLOW_WHEN_DISABLED, ImGuiHoveredFlags_AllowWhenDisabled },
    { GWP_IMGUI_HOVERED_FOR_TOOLTIP, ImGuiHoveredFlags_ForTooltip },
    { GWP_IMGUI_HOVERED_STATIONARY, ImGuiHoveredFlags_Stationary },
    { GWP_IMGUI_HOVERED_DELAY_SHORT, ImGuiHoveredFlags_DelayShort },
    { GWP_IMGUI_HOVERED_DELAY_NORMAL, ImGuiHoveredFlags_DelayNormal },
};

constexpr FlagMap kFocusedFlags[] = {
    { GWP_IMGUI_FOCUSED_CHILD_WINDOWS, ImGuiFocusedFlags_ChildWindows },
    { GWP_IMGUI_FOCUSED_ROOT_WINDOW, ImGuiFocusedFlags_RootWindow },
    { GWP_IMGUI_FOCUSED_ANY_WINDOW, ImGuiFocusedFlags_AnyWindow },
};

constexpr FlagMap kPopupFlags[] = {
    { GWP_IMGUI_POPUP_MOUSE_LEFT, 0 }, // the mouse button: PopupFlags() below
    { GWP_IMGUI_POPUP_MOUSE_MIDDLE, 0 },
    { GWP_IMGUI_POPUP_NO_REOPEN, ImGuiPopupFlags_NoReopen },
    { GWP_IMGUI_POPUP_NO_OPEN_OVER_EXISTING_POPUP, ImGuiPopupFlags_NoOpenOverExistingPopup },
    { GWP_IMGUI_POPUP_ANY_POPUP_ID, ImGuiPopupFlags_AnyPopupId },
    { GWP_IMGUI_POPUP_ANY_POPUP_LEVEL, ImGuiPopupFlags_AnyPopupLevel },
};

// The popup flags; the mouse button for begin_popup_context_item: 0 - right (as ImGui by default), LEFT, MIDDLE
ImGuiPopupFlags PopupFlags(u32 flags, pcstr function, bool context)
{
    ImGuiPopupFlags result = MapFlags(flags, kPopupFlags, function);
    if (context)
    {
        result &= ~ImGuiPopupFlags_AnyPopup;
        if (flags & GWP_IMGUI_POPUP_MOUSE_LEFT)
            result |= ImGuiPopupFlags_MouseButtonLeft;
        else if (flags & GWP_IMGUI_POPUP_MOUSE_MIDDLE)
            result |= ImGuiPopupFlags_MouseButtonMiddle;
        else
            result |= ImGuiPopupFlags_MouseButtonRight;
    }
    return result;
}

constexpr ImGuiCol kColors[] = {
    ImGuiCol_Text,
    ImGuiCol_TextDisabled,
    ImGuiCol_WindowBg,
    ImGuiCol_ChildBg,
    ImGuiCol_PopupBg,
    ImGuiCol_Border,
    ImGuiCol_FrameBg,
    ImGuiCol_FrameBgHovered,
    ImGuiCol_FrameBgActive,
    ImGuiCol_CheckMark,
    ImGuiCol_SliderGrab,
    ImGuiCol_SliderGrabActive,
    ImGuiCol_Button,
    ImGuiCol_ButtonHovered,
    ImGuiCol_ButtonActive,
    ImGuiCol_Header,
    ImGuiCol_HeaderHovered,
    ImGuiCol_HeaderActive,
    ImGuiCol_Separator,
    ImGuiCol_Tab,
    ImGuiCol_TabHovered,
    ImGuiCol_TabSelected,
    ImGuiCol_PlotLines,
    ImGuiCol_PlotLinesHovered,
    ImGuiCol_PlotHistogram,
    ImGuiCol_PlotHistogramHovered,
    ImGuiCol_TableHeaderBg,
    ImGuiCol_TableBorderStrong,
    ImGuiCol_TableBorderLight,
    ImGuiCol_TableRowBg,
    ImGuiCol_TableRowBgAlt,
    ImGuiCol_TextSelectedBg,
};
static_assert(std::size(kColors) == GWP_IMGUI_COL_COUNT, "kColors: one row per GWP_IMGUI_COL_*");

struct StyleVarMap
{
    ImGuiStyleVar var;
    bool vec2;
};
constexpr StyleVarMap kStyleVars[] = {
    { ImGuiStyleVar_Alpha, false },
    { ImGuiStyleVar_DisabledAlpha, false },
    { ImGuiStyleVar_WindowPadding, true },
    { ImGuiStyleVar_FramePadding, true },
    { ImGuiStyleVar_FrameRounding, false },
    { ImGuiStyleVar_FrameBorderSize, false },
    { ImGuiStyleVar_ItemSpacing, true },
    { ImGuiStyleVar_ItemInnerSpacing, true },
    { ImGuiStyleVar_IndentSpacing, false },
    { ImGuiStyleVar_CellPadding, true },
    { ImGuiStyleVar_GrabMinSize, false },
    { ImGuiStyleVar_GrabRounding, false },
    { ImGuiStyleVar_ChildRounding, false },
    { ImGuiStyleVar_ChildBorderSize, false },
    { ImGuiStyleVar_ButtonTextAlign, true },
    { ImGuiStyleVar_SelectableTextAlign, true },
};
static_assert(std::size(kStyleVars) == GWP_IMGUI_STYLE_COUNT, "kStyleVars: one row per GWP_IMGUI_STYLE_*");

// ---------------------------------------------------------------------------------------------
// Places: functions that need a table / a tab bar right here
// ---------------------------------------------------------------------------------------------
ImGuiTable* CurrentTable(pcstr function)
{
    ImGuiContext& g = *GImGui;
    if (g.CurrentTable && g.CurrentTable->InnerWindow == g.CurrentWindow)
        return g.CurrentTable;
    imgui::WarnOnce(kWarnPlace, "GwpImGuiApi::%s: not right inside a table (begin_table), the call is ignored (this "
        "warning is shown once)", function);
    return nullptr;
}

// A table whose layout is not locked yet: table_setup_* go before the first row
ImGuiTable* SetupTable(pcstr function)
{
    ImGuiTable* table = CurrentTable(function);
    if (table && table->IsLayoutLocked)
    {
        imgui::WarnOnce(kWarnPlace, "GwpImGuiApi::%s: called after the first row of the table, ignored (this warning "
            "is shown once)", function);
        return nullptr;
    }
    return table;
}

// ---------------------------------------------------------------------------------------------
// GwpImGuiApi: layout
// ---------------------------------------------------------------------------------------------
void GWP_CALL UiSameLine(float offset_from_start_x, float spacing)
{
    GWP_UI_ENTER_VOID("same_line");
    ImGui::SameLine(Finite(offset_from_start_x), Finite(spacing, -1.f));
}

void GWP_CALL UiNewLine()
{
    GWP_UI_ENTER_VOID("new_line");
    ImGui::NewLine();
}

void GWP_CALL UiSpacing()
{
    GWP_UI_ENTER_VOID("spacing");
    ImGui::Spacing();
}

void GWP_CALL UiSeparator()
{
    GWP_UI_ENTER_VOID("separator");
    ImGui::Separator();
}

void GWP_CALL UiSeparatorText(const char* label)
{
    GWP_UI_ENTER_VOID("separator_text");
    ImGui::SeparatorText(TextOr(label));
}

void GWP_CALL UiDummy(float w, float h)
{
    GWP_UI_ENTER_VOID("dummy");
    ImGui::Dummy(Size(w, h));
}

void GWP_CALL UiIndent(float w)
{
    GWP_UI_ENTER_VOID("indent");
    if (!CanPushOr("indent"))
        return;
    const float width = Finite(w);
    ImGui::Indent(width);
    imgui::Push(imgui::EScope::Indent, width);
}

void GWP_CALL UiUnindent()
{
    GWP_UI_ENTER_VOID("unindent");
    if (!CheckPopOr(imgui::EScope::Indent, "unindent"))
        return;
    ImGui::Unindent(imgui::TopArg());
    imgui::Pop();
}

void GWP_CALL UiBeginGroup()
{
    GWP_UI_ENTER_VOID("begin_group");
    if (!CanPushOr("begin_group"))
        return;
    ImGui::BeginGroup();
    imgui::Push(imgui::EScope::Group);
}

void GWP_CALL UiEndGroup()
{
    GWP_UI_ENTER_VOID("end_group");
    if (!CheckPopOr(imgui::EScope::Group, "end_group"))
        return;
    ImGui::EndGroup();
    imgui::Pop();
}

int GWP_CALL UiBeginChild(const char* str_id, float w, float h, uint32_t child_flags, uint32_t window_flags)
{
    GWP_UI_ENTER("begin_child", 0);
    if (!Label(str_id, "begin_child") || !CanPushOr("begin_child"))
        return 0;
    const ImGuiChildFlags flags = MapFlags(child_flags, kChildFlags, "begin_child");
    // AUTO_RESIZE is not a window flag of a child (ImGui asserts): GWP_IMGUI_CHILD_AUTO_RESIZE_* instead
    const ImGuiWindowFlags wflags = WindowFlags(window_flags, "begin_child") & ~ImGuiWindowFlags_AlwaysAutoResize;
    const bool open = ImGui::BeginChild(str_id, Size(w, h), flags, wflags);
    imgui::Push(imgui::EScope::Child); // EndChild always
    return open ? 1 : 0;
}

void GWP_CALL UiEndChild()
{
    GWP_UI_ENTER_VOID("end_child");
    if (!CheckPopOr(imgui::EScope::Child, "end_child"))
        return;
    ImGui::EndChild();
    imgui::Pop();
}

void GWP_CALL UiSetNextItemWidth(float w)
{
    GWP_UI_ENTER_VOID("set_next_item_width");
    ImGui::SetNextItemWidth(Finite(w));
}

void GWP_CALL UiPushItemWidth(float w)
{
    GWP_UI_ENTER_VOID("push_item_width");
    if (!CanPushOr("push_item_width"))
        return;
    ImGui::PushItemWidth(Finite(w));
    imgui::Push(imgui::EScope::ItemWidth);
}

void GWP_CALL UiPopItemWidth()
{
    GWP_UI_ENTER_VOID("pop_item_width");
    if (!CheckPopOr(imgui::EScope::ItemWidth, "pop_item_width"))
        return;
    ImGui::PopItemWidth();
    imgui::Pop();
}

void GWP_CALL UiAlignTextToFramePadding()
{
    GWP_UI_ENTER_VOID("align_text_to_frame_padding");
    ImGui::AlignTextToFramePadding();
}

void Out(const ImVec2& value, float* x, float* y)
{
    if (x)
        *x = value.x;
    if (y)
        *y = value.y;
}

void GWP_CALL UiGetContentRegionAvail(float* w, float* h)
{
    GWP_UI_ENTER_VOID("get_content_region_avail");
    Out(ImGui::GetContentRegionAvail(), w, h);
}

void GWP_CALL UiGetWindowSize(float* w, float* h)
{
    GWP_UI_ENTER_VOID("get_window_size");
    Out(ImGui::GetWindowSize(), w, h);
}

void GWP_CALL UiGetCursorPos(float* x, float* y)
{
    GWP_UI_ENTER_VOID("get_cursor_pos");
    Out(ImGui::GetCursorPos(), x, y);
}

void GWP_CALL UiSetCursorPos(float x, float y)
{
    GWP_UI_ENTER_VOID("set_cursor_pos");
    ImGui::SetCursorPos(Size(x, y));
    // ImGui asserts (a user error) when End* of the window / child / group follows a SetCursorPos beyond the content:
    // the check is off for this call, as in ui.SetCursorPos of Lua (the window grows by the widgets: put a dummy after
    // it). Review 2026-11-18
    ImGui::GetCurrentWindow()->DC.IsSetPos = false;
}

void GWP_CALL UiCalcTextSize(const char* text, float wrap_width, float* w, float* h)
{
    GWP_UI_ENTER_VOID("calc_text_size");
    const float wrap = Finite(wrap_width);
    Out(ImGui::CalcTextSize(TextOr(text), nullptr, false, wrap > 0.f ? wrap : -1.f), w, h);
}

float GWP_CALL UiGetTextLineHeight()
{
    GWP_UI_ENTER("get_text_line_height", 0.f);
    return ImGui::GetTextLineHeight();
}

float GWP_CALL UiGetFrameHeight()
{
    GWP_UI_ENTER("get_frame_height", 0.f);
    return ImGui::GetFrameHeight();
}

// ---------------------------------------------------------------------------------------------
// Text: never a printf format
// ---------------------------------------------------------------------------------------------
void GWP_CALL UiText(const char* text)
{
    GWP_UI_ENTER_VOID("text");
    ImGui::TextUnformatted(TextOr(text));
}

void GWP_CALL UiTextN(const char* text, uint32_t length)
{
    GWP_UI_ENTER_VOID("text_n");
    if (!text || !length)
    {
        ImGui::TextUnformatted("");
        return;
    }
    ImGui::TextUnformatted(text, text + length);
}

void GWP_CALL UiTextColored(uint32_t argb, const char* text)
{
    GWP_UI_ENTER_VOID("text_colored");
    ImGui::TextColored(Color(argb), "%s", TextOr(text));
}

void GWP_CALL UiTextDisabled(const char* text)
{
    GWP_UI_ENTER_VOID("text_disabled");
    ImGui::TextDisabled("%s", TextOr(text));
}

void GWP_CALL UiTextWrapped(const char* text)
{
    GWP_UI_ENTER_VOID("text_wrapped");
    ImGui::TextWrapped("%s", TextOr(text));
}

void GWP_CALL UiLabelText(const char* label, const char* text)
{
    GWP_UI_ENTER_VOID("label_text");
    ImGui::LabelText(TextOr(label), "%s", TextOr(text));
}

void GWP_CALL UiBulletText(const char* text)
{
    GWP_UI_ENTER_VOID("bullet_text");
    ImGui::BulletText("%s", TextOr(text));
}

void GWP_CALL UiBullet()
{
    GWP_UI_ENTER_VOID("bullet");
    ImGui::Bullet();
}

void GWP_CALL UiPushTextWrapPos(float wrap_local_pos_x)
{
    GWP_UI_ENTER_VOID("push_text_wrap_pos");
    if (!CanPushOr("push_text_wrap_pos"))
        return;
    ImGui::PushTextWrapPos(Finite(wrap_local_pos_x));
    imgui::Push(imgui::EScope::TextWrapPos);
}

void GWP_CALL UiPopTextWrapPos()
{
    GWP_UI_ENTER_VOID("pop_text_wrap_pos");
    if (!CheckPopOr(imgui::EScope::TextWrapPos, "pop_text_wrap_pos"))
        return;
    ImGui::PopTextWrapPos();
    imgui::Pop();
}

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiButton(const char* label, float w, float h)
{
    GWP_UI_ENTER("button", 0);
    if (!Label(label, "button"))
        return 0;
    return ImGui::Button(label, Size(w, h)) ? 1 : 0;
}

int GWP_CALL UiSmallButton(const char* label)
{
    GWP_UI_ENTER("small_button", 0);
    if (!Label(label, "small_button"))
        return 0;
    return ImGui::SmallButton(label) ? 1 : 0;
}

int GWP_CALL UiInvisibleButton(const char* str_id, float w, float h)
{
    GWP_UI_ENTER("invisible_button", 0);
    if (!Label(str_id, "invisible_button"))
        return 0;
    const ImVec2 size = Size(w, h);
    if (size.x <= 0.f || size.y <= 0.f)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::invisible_button: the size must be above 0, the call is ignored "
            "(this warning is shown once)");
        return 0;
    }
    return ImGui::InvisibleButton(str_id, size) ? 1 : 0;
}

int GWP_CALL UiArrowButton(const char* str_id, uint32_t dir)
{
    GWP_UI_ENTER("arrow_button", 0);
    if (!Label(str_id, "arrow_button"))
        return 0;
    ImGuiDir d;
    switch (dir)
    {
    case GWP_IMGUI_DIR_LEFT: d = ImGuiDir_Left; break;
    case GWP_IMGUI_DIR_RIGHT: d = ImGuiDir_Right; break;
    case GWP_IMGUI_DIR_UP: d = ImGuiDir_Up; break;
    case GWP_IMGUI_DIR_DOWN: d = ImGuiDir_Down; break;
    default:
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::arrow_button: dir %u is not GWP_IMGUI_DIR_*, the call is ignored "
            "(this warning is shown once)", dir);
        return 0;
    }
    return ImGui::ArrowButton(str_id, d) ? 1 : 0;
}

int GWP_CALL UiCheckbox(const char* label, int* v)
{
    GWP_UI_ENTER("checkbox", 0);
    if (!Label(label, "checkbox") || !Value(v, "checkbox"))
        return 0;
    bool value = *v != 0;
    const bool changed = ImGui::Checkbox(label, &value);
    *v = value ? 1 : 0;
    return changed ? 1 : 0;
}

int GWP_CALL UiCheckboxFlags(const char* label, uint32_t* flags, uint32_t flags_value)
{
    GWP_UI_ENTER("checkbox_flags", 0);
    if (!Label(label, "checkbox_flags") || !Value(flags, "checkbox_flags"))
        return 0;
    unsigned int value = *flags;
    const bool changed = ImGui::CheckboxFlags(label, &value, static_cast<unsigned int>(flags_value));
    *flags = value;
    return changed ? 1 : 0;
}

int GWP_CALL UiRadioButton(const char* label, int active)
{
    GWP_UI_ENTER("radio_button", 0);
    if (!Label(label, "radio_button"))
        return 0;
    return ImGui::RadioButton(label, active != 0) ? 1 : 0;
}

void GWP_CALL UiProgressBar(float fraction, float w, float h, const char* overlay)
{
    GWP_UI_ENTER_VOID("progress_bar");
    const float width = Finite(w);
    ImGui::ProgressBar(Finite(fraction), ImVec2(width == 0.f ? -FLT_MIN : width, Finite(h)), overlay);
}

// ---------------------------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiSliderFloat(const char* label, float* v, float v_min, float v_max, const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("slider_float", 0);
    if (!Label(label, "slider_float") || !Value(v, "slider_float"))
        return 0;
    return ImGui::SliderFloat(label, v, SliderLimit(v_min), SliderLimit(v_max), imgui::SafeFormat(fmt, false, "%.3f"),
               SliderFlags(flags, "slider_float")) ? 1 : 0;
}

int GWP_CALL UiSliderFloatN(const char* label, float* v, uint32_t n, float v_min, float v_max, const char* fmt,
    uint32_t flags)
{
    GWP_UI_ENTER("slider_float_n", 0);
    if (!Label(label, "slider_float_n") || !Value(v, "slider_float_n") || !Components(n, "slider_float_n"))
        return 0;
    const float lo = SliderLimit(v_min), hi = SliderLimit(v_max);
    return ImGui::SliderScalarN(label, ImGuiDataType_Float, v, static_cast<int>(n), &lo, &hi,
               imgui::SafeFormat(fmt, false, "%.3f"), SliderFlags(flags, "slider_float_n")) ? 1 : 0;
}

int GWP_CALL UiSliderInt(const char* label, int* v, int v_min, int v_max, const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("slider_int", 0);
    if (!Label(label, "slider_int") || !Value(v, "slider_int"))
        return 0;
    return ImGui::SliderInt(label, v, SliderLimit(v_min), SliderLimit(v_max), imgui::SafeFormat(fmt, true, "%d"),
               SliderFlags(flags, "slider_int")) ? 1 : 0;
}

int GWP_CALL UiSliderIntN(const char* label, int* v, uint32_t n, int v_min, int v_max, const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("slider_int_n", 0);
    if (!Label(label, "slider_int_n") || !Value(v, "slider_int_n") || !Components(n, "slider_int_n"))
        return 0;
    const int lo = SliderLimit(v_min), hi = SliderLimit(v_max);
    return ImGui::SliderScalarN(label, ImGuiDataType_S32, v, static_cast<int>(n), &lo, &hi,
               imgui::SafeFormat(fmt, true, "%d"), SliderFlags(flags, "slider_int_n")) ? 1 : 0;
}

int GWP_CALL UiSliderAngle(const char* label, float* v_rad, float v_degrees_min, float v_degrees_max, const char* fmt,
    uint32_t flags)
{
    GWP_UI_ENTER("slider_angle", 0);
    if (!Label(label, "slider_angle") || !Value(v_rad, "slider_angle"))
        return 0;
    // ImGui converts the limits to radians: far below FLT_MAX / 2 for any finite degrees an addon would give
    const float lo = std::clamp(Finite(v_degrees_min, -360.f), -1.0e30f, 1.0e30f);
    const float hi = std::clamp(Finite(v_degrees_max, 360.f), -1.0e30f, 1.0e30f);
    return ImGui::SliderAngle(label, v_rad, lo, hi, imgui::SafeFormat(fmt, false, "%.0f deg"),
               SliderFlags(flags, "slider_angle")) ? 1 : 0;
}

int GWP_CALL UiDragFloat(const char* label, float* v, float v_speed, float v_min, float v_max, const char* fmt,
    uint32_t flags)
{
    GWP_UI_ENTER("drag_float", 0);
    if (!Label(label, "drag_float") || !Value(v, "drag_float"))
        return 0;
    return ImGui::DragFloat(label, v, Finite(v_speed, 1.f), Finite(v_min), Finite(v_max),
               imgui::SafeFormat(fmt, false, "%.3f"), DragFlags(flags, "drag_float")) ? 1 : 0;
}

int GWP_CALL UiDragFloatN(const char* label, float* v, uint32_t n, float v_speed, float v_min, float v_max,
    const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("drag_float_n", 0);
    if (!Label(label, "drag_float_n") || !Value(v, "drag_float_n") || !Components(n, "drag_float_n"))
        return 0;
    const float lo = Finite(v_min), hi = Finite(v_max);
    return ImGui::DragScalarN(label, ImGuiDataType_Float, v, static_cast<int>(n), Finite(v_speed, 1.f), &lo, &hi,
               imgui::SafeFormat(fmt, false, "%.3f"), DragFlags(flags, "drag_float_n")) ? 1 : 0;
}

int GWP_CALL UiDragInt(const char* label, int* v, float v_speed, int v_min, int v_max, const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("drag_int", 0);
    if (!Label(label, "drag_int") || !Value(v, "drag_int"))
        return 0;
    return ImGui::DragInt(label, v, Finite(v_speed, 1.f), v_min, v_max, imgui::SafeFormat(fmt, true, "%d"),
               DragFlags(flags, "drag_int")) ? 1 : 0;
}

int GWP_CALL UiDragIntN(const char* label, int* v, uint32_t n, float v_speed, int v_min, int v_max, const char* fmt,
    uint32_t flags)
{
    GWP_UI_ENTER("drag_int_n", 0);
    if (!Label(label, "drag_int_n") || !Value(v, "drag_int_n") || !Components(n, "drag_int_n"))
        return 0;
    return ImGui::DragScalarN(label, ImGuiDataType_S32, v, static_cast<int>(n), Finite(v_speed, 1.f), &v_min, &v_max,
               imgui::SafeFormat(fmt, true, "%d"), DragFlags(flags, "drag_int_n")) ? 1 : 0;
}

// input_float* / input_int: InputScalar of ImGui asserts on EnterReturnsTrue (as ui.InputFloat / ui.InputInt of Lua,
// review 2026-11-18): the flag is dropped with a warning once per tool, the rest is mapped as for input_text
ImGuiInputTextFlags ScalarInputFlags(u32 flags, pcstr function)
{
    if (flags & GWP_IMGUI_INPUT_ENTER_RETURNS_TRUE)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: GWP_IMGUI_INPUT_ENTER_RETURNS_TRUE is for input_text* only, "
            "ignored - use is_item_deactivated_after_edit (this warning is shown once)", function);
        flags &= ~GWP_IMGUI_INPUT_ENTER_RETURNS_TRUE;
    }
    return MapFlags(flags, kInputFlags, function);
}

int GWP_CALL UiInputFloat(const char* label, float* v, float step, float step_fast, const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("input_float", 0);
    if (!Label(label, "input_float") || !Value(v, "input_float"))
        return 0;
    return ImGui::InputFloat(label, v, Finite(step), Finite(step_fast), imgui::SafeFormat(fmt, false, "%.3f"),
               ScalarInputFlags(flags, "input_float")) ? 1 : 0;
}

int GWP_CALL UiInputFloatN(const char* label, float* v, uint32_t n, const char* fmt, uint32_t flags)
{
    GWP_UI_ENTER("input_float_n", 0);
    if (!Label(label, "input_float_n") || !Value(v, "input_float_n") || !Components(n, "input_float_n"))
        return 0;
    return ImGui::InputScalarN(label, ImGuiDataType_Float, v, static_cast<int>(n), nullptr, nullptr,
               imgui::SafeFormat(fmt, false, "%.3f"), ScalarInputFlags(flags, "input_float_n")) ? 1 : 0;
}

int GWP_CALL UiInputInt(const char* label, int* v, int step, int step_fast, uint32_t flags)
{
    GWP_UI_ENTER("input_int", 0);
    if (!Label(label, "input_int") || !Value(v, "input_int"))
        return 0;
    return ImGui::InputInt(label, v, step, step_fast, ScalarInputFlags(flags, "input_int")) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Text input: the buffer of the plugin, at most the limit, NUL-terminated
// ---------------------------------------------------------------------------------------------
bool TextBuffer(char* buf, uint32_t& cap, u32 limit, pcstr function)
{
    if (!buf || !cap)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: the buffer is NULL or its capacity is 0, the call is ignored "
            "(this warning is shown once)", function);
        return false;
    }
    if (cap > limit)
        cap = limit; // the widget never writes past the limit; the rest of the buffer stays as it is
    if (!memchr(buf, 0, cap))
    {
        buf[cap - 1] = '\0';
        imgui::WarnOnce(imgui::kWarnTruncated, "GwpImGuiApi::%s: the text of the buffer has no NUL in %u bytes, cut "
            "(this warning is shown once)", function, cap);
    }
    return true;
}

// The capacity of an input_text* while its widget is active (review 2026-11-18). ImGui sizes its own copy of the text
// (InputTextState::TextA) by the buf_size of the activation and writes up to the buf_size of every later frame into it:
// a capacity grown while the widget is active is a heap overflow inside the engine (imgui_internal.h: the size must not
// change while active). The capacity is cut to the one of the activation (never above the buffer of the plugin), and a
// widget activated read-only stays read-only until it loses the focus (ImGui did not copy a read-only text to TextA:
// its TextLen would be read past TextA). As ActiveCapacity of ui.InputText* (addon_imgui_lua_widgets.cpp)
uint32_t ActiveCap(const char* label, uint32_t cap, ImGuiInputTextFlags& flags)
{
    const ImGuiContext& g = *GImGui;
    const ImGuiInputTextState& state = g.InputTextState;
    if (g.ActiveId == 0 || g.ActiveId != state.ID || ImGui::GetID(label) != state.ID)
        return cap;
    if (state.Flags & ImGuiInputTextFlags_ReadOnly)
    {
        flags |= ImGuiInputTextFlags_ReadOnly;
        return cap;
    }
    if (state.TextA.Size >= 2 && static_cast<uint32_t>(state.TextA.Size - 1) < cap)
        return static_cast<uint32_t>(state.TextA.Size - 1);
    return cap;
}

int GWP_CALL UiInputText(const char* label, char* buf, uint32_t cap, uint32_t flags)
{
    GWP_UI_ENTER("input_text", 0);
    if (!Label(label, "input_text") || !TextBuffer(buf, cap, imgui::kMaxInputText, "input_text"))
        return 0;
    ImGuiInputTextFlags input_flags = MapFlags(flags, kInputFlags, "input_text");
    cap = ActiveCap(label, cap, input_flags);
    return ImGui::InputText(label, buf, cap, input_flags) ? 1 : 0;
}

int GWP_CALL UiInputTextWithHint(const char* label, const char* hint, char* buf, uint32_t cap, uint32_t flags)
{
    GWP_UI_ENTER("input_text_with_hint", 0);
    if (!Label(label, "input_text_with_hint") || !TextBuffer(buf, cap, imgui::kMaxInputText, "input_text_with_hint"))
        return 0;
    ImGuiInputTextFlags input_flags = MapFlags(flags, kInputFlags, "input_text_with_hint");
    cap = ActiveCap(label, cap, input_flags);
    return ImGui::InputTextWithHint(label, TextOr(hint), buf, cap, input_flags) ? 1 : 0;
}

int GWP_CALL UiInputTextMultiline(const char* label, char* buf, uint32_t cap, float w, float h, uint32_t flags)
{
    GWP_UI_ENTER("input_text_multiline", 0);
    if (!Label(label, "input_text_multiline") ||
        !TextBuffer(buf, cap, imgui::kMaxInputTextMultiline, "input_text_multiline"))
    {
        return 0;
    }
    ImGuiInputTextFlags input_flags = MapFlags(flags & ~GWP_IMGUI_INPUT_PASSWORD, kInputFlags, "input_text_multiline");
    cap = ActiveCap(label, cap, input_flags);
    return ImGui::InputTextMultiline(label, buf, cap, Size(w, h), input_flags) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Choice
// ---------------------------------------------------------------------------------------------
// The items of combo / list_box: at most the limit, none NULL. false - refused (warned)
bool Items(const char* const* items, uint32_t& count, pcstr function)
{
    if (count && !items)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: items is NULL, the call is ignored (this warning is shown "
            "once)", function);
        return false;
    }
    if (count > imgui::kMaxListItems)
    {
        imgui::WarnOnce(imgui::kWarnTruncated, "GwpImGuiApi::%s: %u items, the first %u are shown (this warning is "
            "shown once)", function, count, imgui::kMaxListItems);
        count = imgui::kMaxListItems;
    }
    for (u32 i = 0; i < count; ++i)
    {
        if (!items[i])
        {
            imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: item %u is NULL, the call is ignored (this warning is "
                "shown once)", function, i);
            return false;
        }
    }
    return true;
}

int GWP_CALL UiCombo(const char* label, int* current, const char* const* items, uint32_t count,
    int popup_max_height_in_items)
{
    GWP_UI_ENTER("combo", 0);
    if (!Label(label, "combo") || !Value(current, "combo") || !Items(items, count, "combo"))
        return 0;
    return ImGui::Combo(label, current, items, static_cast<int>(count), popup_max_height_in_items) ? 1 : 0;
}

int GWP_CALL UiBeginCombo(const char* label, const char* preview, uint32_t flags)
{
    GWP_UI_ENTER("begin_combo", 0);
    if (!Label(label, "begin_combo") || !CanPushOr("begin_combo"))
        return 0;
    const bool open = ImGui::BeginCombo(label, preview, ComboFlags(flags));
    if (open)
        imgui::Push(imgui::EScope::Combo);
    return open ? 1 : 0;
}

void GWP_CALL UiEndCombo()
{
    GWP_UI_ENTER_VOID("end_combo");
    if (!CheckPopOr(imgui::EScope::Combo, "end_combo"))
        return;
    ImGui::EndCombo();
    imgui::Pop();
}

int GWP_CALL UiSelectable(const char* label, int selected, uint32_t flags, float w, float h)
{
    GWP_UI_ENTER("selectable", 0);
    if (!Label(label, "selectable"))
        return 0;
    return ImGui::Selectable(label, selected != 0, MapFlags(flags, kSelectableFlags, "selectable"), Size(w, h)) ? 1 : 0;
}

int GWP_CALL UiListBox(const char* label, int* current, const char* const* items, uint32_t count, int height_in_items)
{
    GWP_UI_ENTER("list_box", 0);
    if (!Label(label, "list_box") || !Value(current, "list_box") || !Items(items, count, "list_box"))
        return 0;
    return ImGui::ListBox(label, current, items, static_cast<int>(count), height_in_items) ? 1 : 0;
}

int GWP_CALL UiBeginListBox(const char* label, float w, float h)
{
    GWP_UI_ENTER("begin_list_box", 0);
    if (!Label(label, "begin_list_box") || !CanPushOr("begin_list_box"))
        return 0;
    const bool open = ImGui::BeginListBox(label, Size(w, h));
    if (open)
        imgui::Push(imgui::EScope::ListBox);
    return open ? 1 : 0;
}

void GWP_CALL UiEndListBox()
{
    GWP_UI_ENTER_VOID("end_list_box");
    if (!CheckPopOr(imgui::EScope::ListBox, "end_list_box"))
        return;
    ImGui::EndListBox();
    imgui::Pop();
}

// ---------------------------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiColorEdit3(const char* label, float* rgb, uint32_t flags)
{
    GWP_UI_ENTER("color_edit3", 0);
    if (!Label(label, "color_edit3") || !Value(rgb, "color_edit3"))
        return 0;
    return ImGui::ColorEdit3(label, rgb, ColorFlags(flags, "color_edit3")) ? 1 : 0;
}

int GWP_CALL UiColorEdit4(const char* label, float* rgba, uint32_t flags)
{
    GWP_UI_ENTER("color_edit4", 0);
    if (!Label(label, "color_edit4") || !Value(rgba, "color_edit4"))
        return 0;
    return ImGui::ColorEdit4(label, rgba, ColorFlags(flags, "color_edit4")) ? 1 : 0;
}

int GWP_CALL UiColorButton(const char* desc_id, uint32_t argb, uint32_t flags, float w, float h)
{
    GWP_UI_ENTER("color_button", 0);
    if (!Label(desc_id, "color_button"))
        return 0;
    return ImGui::ColorButton(desc_id, Color(argb), ColorFlags(flags, "color_button"), Size(w, h)) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Trees
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiTreeNode(const char* label)
{
    GWP_UI_ENTER("tree_node", 0);
    if (!Label(label, "tree_node") || !CanPushOr("tree_node"))
        return 0;
    const bool open = ImGui::TreeNode(label);
    if (open)
        imgui::Push(imgui::EScope::TreeNode);
    return open ? 1 : 0;
}

int GWP_CALL UiTreeNodeEx(const char* label, uint32_t flags)
{
    GWP_UI_ENTER("tree_node_ex", 0);
    if (!Label(label, "tree_node_ex") || !CanPushOr("tree_node_ex"))
        return 0;
    // No NoTreePushOnOpen among the flags of GW: an open node always needs its tree_pop
    const bool open = ImGui::TreeNodeEx(label, MapFlags(flags, kTreeFlags, "tree_node_ex"));
    if (open)
        imgui::Push(imgui::EScope::TreeNode);
    return open ? 1 : 0;
}

void GWP_CALL UiTreePop()
{
    GWP_UI_ENTER_VOID("tree_pop");
    if (!CheckPopOr(imgui::EScope::TreeNode, "tree_pop"))
        return;
    ImGui::TreePop();
    imgui::Pop();
}

int GWP_CALL UiCollapsingHeader(const char* label, uint32_t flags)
{
    GWP_UI_ENTER("collapsing_header", 0);
    if (!Label(label, "collapsing_header"))
        return 0;
    return ImGui::CollapsingHeader(label, MapFlags(flags, kTreeFlags, "collapsing_header")) ? 1 : 0;
}

void GWP_CALL UiSetNextItemOpen(int is_open, uint32_t cond)
{
    GWP_UI_ENTER_VOID("set_next_item_open");
    ImGui::SetNextItemOpen(is_open != 0, Cond(cond, "set_next_item_open"));
}

// ---------------------------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiBeginTabBar(const char* str_id, uint32_t flags)
{
    GWP_UI_ENTER("begin_tab_bar", 0);
    if (!Label(str_id, "begin_tab_bar") || !CanPushOr("begin_tab_bar"))
        return 0;
    const bool open = ImGui::BeginTabBar(str_id, MapFlags(flags, kTabBarFlags, "begin_tab_bar"));
    if (open)
        imgui::Push(imgui::EScope::TabBar);
    return open ? 1 : 0;
}

void GWP_CALL UiEndTabBar()
{
    GWP_UI_ENTER_VOID("end_tab_bar");
    if (!CheckPopOr(imgui::EScope::TabBar, "end_tab_bar"))
        return;
    ImGui::EndTabBar();
    imgui::Pop();
}

int GWP_CALL UiBeginTabItem(const char* label, int* p_open, uint32_t flags)
{
    GWP_UI_ENTER("begin_tab_item", 0);
    if (!Label(label, "begin_tab_item") || !CanPushOr("begin_tab_item"))
        return 0;
    ImGuiContext& g = *GImGui;
    if (!g.CurrentTabBar || g.CurrentTabBar->Window != g.CurrentWindow)
    {
        imgui::WarnOnce(kWarnPlace, "GwpImGuiApi::begin_tab_item: not right inside a tab bar (begin_tab_bar), the call "
            "is ignored (this warning is shown once)");
        return 0;
    }
    bool open = p_open ? *p_open != 0 : true;
    const bool selected = ImGui::BeginTabItem(label, p_open ? &open : nullptr,
        MapFlags(flags, kTabItemFlags, "begin_tab_item"));
    if (p_open)
        *p_open = open ? 1 : 0;
    if (selected)
        imgui::Push(imgui::EScope::TabItem);
    return selected ? 1 : 0;
}

void GWP_CALL UiEndTabItem()
{
    GWP_UI_ENTER_VOID("end_tab_item");
    if (!CheckPopOr(imgui::EScope::TabItem, "end_tab_item"))
        return;
    ImGui::EndTabItem();
    imgui::Pop();
}

// ---------------------------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiBeginTable(const char* str_id, int columns, uint32_t flags, float outer_w, float outer_h)
{
    GWP_UI_ENTER("begin_table", 0);
    if (!Label(str_id, "begin_table") || !CanPushOr("begin_table"))
        return 0;
    if (columns < 1 || columns >= IMGUI_TABLE_MAX_COLUMNS)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::begin_table: %d columns, 1..%d are allowed, the call is ignored "
            "(this warning is shown once)", columns, IMGUI_TABLE_MAX_COLUMNS - 1);
        return 0;
    }
    const bool open = ImGui::BeginTable(str_id, columns, TableFlags(flags), Size(outer_w, outer_h));
    if (open)
        imgui::Push(imgui::EScope::Table);
    return open ? 1 : 0;
}

void GWP_CALL UiEndTable()
{
    GWP_UI_ENTER_VOID("end_table");
    if (!CheckPopOr(imgui::EScope::Table, "end_table"))
        return;
    ImGui::EndTable();
    imgui::Pop();
}

void GWP_CALL UiTableNextRow(uint32_t row_flags, float min_row_height)
{
    GWP_UI_ENTER_VOID("table_next_row");
    if (!CurrentTable("table_next_row"))
        return;
    if (row_flags & ~GWP_IMGUI_TABLE_ROW_HEADERS)
    {
        imgui::WarnOnce(imgui::kWarnFlags, "GwpImGuiApi::table_next_row: unknown flag bits 0x%x are ignored (this "
            "warning is shown once)", row_flags & ~GWP_IMGUI_TABLE_ROW_HEADERS);
    }
    ImGui::TableNextRow((row_flags & GWP_IMGUI_TABLE_ROW_HEADERS) ? ImGuiTableRowFlags_Headers : ImGuiTableRowFlags_None,
        Finite(min_row_height));
}

int GWP_CALL UiTableNextColumn()
{
    GWP_UI_ENTER("table_next_column", 0);
    if (!CurrentTable("table_next_column"))
        return 0;
    return ImGui::TableNextColumn() ? 1 : 0;
}

int GWP_CALL UiTableSetColumnIndex(int column_n)
{
    GWP_UI_ENTER("table_set_column_index", 0);
    const ImGuiTable* table = CurrentTable("table_set_column_index");
    if (!table)
        return 0;
    if (column_n < 0 || column_n >= table->ColumnsCount)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::table_set_column_index: column %d of %d, the call is ignored "
            "(this warning is shown once)", column_n, table->ColumnsCount);
        return 0;
    }
    return ImGui::TableSetColumnIndex(column_n) ? 1 : 0;
}

void GWP_CALL UiTableSetupColumn(const char* label, uint32_t flags, float init_width_or_weight)
{
    GWP_UI_ENTER_VOID("table_setup_column");
    const ImGuiTable* table = SetupTable("table_setup_column");
    if (!table)
        return;
    if (table->DeclColumnsCount >= table->ColumnsCount)
    {
        imgui::WarnOnce(kWarnPlace, "GwpImGuiApi::table_setup_column: more columns than begin_table gave (%d), "
            "ignored (this warning is shown once)", table->ColumnsCount);
        return;
    }
    const ImGuiTableColumnFlags column_flags = ColumnFlags(flags);
    float width = Finite(init_width_or_weight);
    // A width or a weight only with a sizing policy (ImGui asserts on it without one). Also with SCROLL_X: the "grace"
    // of ImGui for ScrollX tests the column flags, not the table ones (imgui_tables.cpp TableSetupColumn), so it
    // asserts there too (review 2026-11-18; ui.TableSetupColumn of Lua has no exception either)
    if (width > 0.f && table->IsDefaultSizingPolicy && !(column_flags & ImGuiTableColumnFlags_WidthMask_))
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::table_setup_column: init_width_or_weight needs a sizing policy "
            "(GWP_IMGUI_TABLE_SIZING_* or GWP_IMGUI_TABLE_COLUMN_WIDTH_*), ignored (this warning is shown once)");
        width = 0.f;
    }
    ImGui::TableSetupColumn(TextOr(label), column_flags, width);
}

void GWP_CALL UiTableSetupScrollFreeze(int cols, int rows)
{
    GWP_UI_ENTER_VOID("table_setup_scroll_freeze");
    if (!SetupTable("table_setup_scroll_freeze"))
        return;
    ImGui::TableSetupScrollFreeze(std::clamp(cols, 0, IMGUI_TABLE_MAX_COLUMNS - 1), std::clamp(rows, 0, 127));
}

void GWP_CALL UiTableHeadersRow()
{
    GWP_UI_ENTER_VOID("table_headers_row");
    if (!CurrentTable("table_headers_row"))
        return;
    ImGui::TableHeadersRow();
}

// ---------------------------------------------------------------------------------------------
// Plots
// ---------------------------------------------------------------------------------------------
using PlotFn = void (*)(const char*, const float*, int, int, const char*, float, float, ImVec2, int);

void Plot(PlotFn fn, pcstr function, const char* label, const float* values, uint32_t count, uint32_t values_offset,
    const char* overlay, float scale_min, float scale_max, float w, float h)
{
    if (!Label(label, function))
        return;
    if (count && !values)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: values is NULL, the call is ignored (this warning is shown "
            "once)", function);
        return;
    }
    if (count > imgui::kMaxPlotValues)
    {
        imgui::WarnOnce(imgui::kWarnTruncated, "GwpImGuiApi::%s: %u values, the first %u are shown (this warning is "
            "shown once)", function, count, imgui::kMaxPlotValues);
        count = imgui::kMaxPlotValues;
    }
    const int offset = count ? static_cast<int>(values_offset % count) : 0;
    const float lo = std::isnan(scale_min) ? FLT_MAX : scale_min;
    const float hi = std::isnan(scale_max) ? FLT_MAX : scale_max;
    fn(label, values, static_cast<int>(count), offset, overlay, lo, hi, Size(w, h), static_cast<int>(sizeof(float)));
}

void GWP_CALL UiPlotLines(const char* label, const float* values, uint32_t count, uint32_t values_offset,
    const char* overlay, float scale_min, float scale_max, float w, float h)
{
    GWP_UI_ENTER_VOID("plot_lines");
    Plot([](const char* l, const float* v, int n, int o, const char* t, float a, float b, ImVec2 s, int stride) {
        ImGui::PlotLines(l, v, n, o, t, a, b, s, stride);
    }, "plot_lines", label, values, count, values_offset, overlay, scale_min, scale_max, w, h);
}

void GWP_CALL UiPlotHistogram(const char* label, const float* values, uint32_t count, uint32_t values_offset,
    const char* overlay, float scale_min, float scale_max, float w, float h)
{
    GWP_UI_ENTER_VOID("plot_histogram");
    Plot([](const char* l, const float* v, int n, int o, const char* t, float a, float b, ImVec2 s, int stride) {
        ImGui::PlotHistogram(l, v, n, o, t, a, b, s, stride);
    }, "plot_histogram", label, values, count, values_offset, overlay, scale_min, scale_max, w, h);
}

// ---------------------------------------------------------------------------------------------
// Tooltips
// ---------------------------------------------------------------------------------------------
void GWP_CALL UiSetTooltip(const char* text)
{
    GWP_UI_ENTER_VOID("set_tooltip");
    ImGui::SetTooltip("%s", TextOr(text));
}

void GWP_CALL UiSetItemTooltip(const char* text)
{
    GWP_UI_ENTER_VOID("set_item_tooltip");
    ImGui::SetItemTooltip("%s", TextOr(text));
}

// ---------------------------------------------------------------------------------------------
// Clipboard: write only (a plugin never reads what the player copied elsewhere), as ui.SetClipboardText of Lua
// ---------------------------------------------------------------------------------------------
void GWP_CALL UiSetClipboardText(const char* text)
{
    GWP_UI_ENTER_VOID("set_clipboard_text");
    if (!text)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::set_clipboard_text: the text is NULL, the call is ignored (this "
            "warning is shown once)");
        return;
    }
    // At most kMaxTextLength bytes, cut on a UTF-8 boundary (the engine backend: SDL_SetClipboardText)
    const size_t length = strnlen(text, imgui::kMaxTextLength + 1);
    if (length <= imgui::kMaxTextLength)
    {
        ImGui::SetClipboardText(text);
        return;
    }
    size_t cut = imgui::kMaxTextLength;
    while (cut > 0 && (static_cast<u8>(text[cut]) & 0xC0) == 0x80)
        --cut;
    imgui::WarnOnce(imgui::kWarnTruncated, "GwpImGuiApi::set_clipboard_text: the text is longer than %u bytes, cut "
        "(this warning is shown once)", imgui::kMaxTextLength);
    const xr_string copy(text, cut);
    ImGui::SetClipboardText(copy.c_str());
}

int GWP_CALL UiBeginTooltip()
{
    GWP_UI_ENTER("begin_tooltip", 0);
    if (!CanPushOr("begin_tooltip"))
        return 0;
    const bool open = ImGui::BeginTooltip();
    if (open)
        imgui::Push(imgui::EScope::Tooltip);
    return open ? 1 : 0;
}

int GWP_CALL UiBeginItemTooltip()
{
    GWP_UI_ENTER("begin_item_tooltip", 0);
    if (!CanPushOr("begin_item_tooltip"))
        return 0;
    const bool open = ImGui::BeginItemTooltip();
    if (open)
        imgui::Push(imgui::EScope::Tooltip);
    return open ? 1 : 0;
}

void GWP_CALL UiEndTooltip()
{
    GWP_UI_ENTER_VOID("end_tooltip");
    if (!CheckPopOr(imgui::EScope::Tooltip, "end_tooltip"))
        return;
    ImGui::EndTooltip();
    imgui::Pop();
}

// ---------------------------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------------------------
void GWP_CALL UiOpenPopup(const char* str_id, uint32_t popup_flags)
{
    GWP_UI_ENTER_VOID("open_popup");
    if (!Label(str_id, "open_popup"))
        return;
    ImGui::OpenPopup(str_id, PopupFlags(popup_flags, "open_popup", false) & ~ImGuiPopupFlags_AnyPopup);
}

int GWP_CALL UiBeginPopup(const char* str_id, uint32_t window_flags)
{
    GWP_UI_ENTER("begin_popup", 0);
    if (!Label(str_id, "begin_popup") || !CanPushOr("begin_popup"))
        return 0;
    const bool open = ImGui::BeginPopup(str_id, WindowFlags(window_flags, "begin_popup"));
    if (open)
        imgui::Push(imgui::EScope::Popup);
    return open ? 1 : 0;
}

int GWP_CALL UiBeginPopupModal(const char* name, int* p_open, uint32_t window_flags)
{
    GWP_UI_ENTER("begin_popup_modal", 0);
    if (!Label(name, "begin_popup_modal") || !CanPushOr("begin_popup_modal"))
        return 0;
    bool open_flag = p_open ? *p_open != 0 : true;
    const bool open = ImGui::BeginPopupModal(name, p_open ? &open_flag : nullptr,
        WindowFlags(window_flags, "begin_popup_modal"));
    if (p_open)
        *p_open = open_flag ? 1 : 0;
    if (open)
        imgui::Push(imgui::EScope::Popup);
    return open ? 1 : 0;
}

int GWP_CALL UiBeginPopupContextItem(const char* str_id, uint32_t popup_flags)
{
    GWP_UI_ENTER("begin_popup_context_item", 0);
    if (!CanPushOr("begin_popup_context_item"))
        return 0;
    if (!str_id && GImGui->LastItemData.ID == 0)
    {
        // ImGui asserts: the last item has no id to open the popup by (a text, a separator)
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::begin_popup_context_item: str_id is NULL and the last item has "
            "no id (a text?), the call is ignored (this warning is shown once)");
        return 0;
    }
    const bool open = ImGui::BeginPopupContextItem(str_id, PopupFlags(popup_flags, "begin_popup_context_item", true));
    if (open)
        imgui::Push(imgui::EScope::Popup);
    return open ? 1 : 0;
}

void GWP_CALL UiEndPopup()
{
    GWP_UI_ENTER_VOID("end_popup");
    if (!CheckPopOr(imgui::EScope::Popup, "end_popup"))
        return;
    ImGui::EndPopup();
    imgui::Pop();
}

void GWP_CALL UiCloseCurrentPopup()
{
    GWP_UI_ENTER_VOID("close_current_popup");
    ImGui::CloseCurrentPopup(); // outside of a popup ImGui does nothing
}

int GWP_CALL UiIsPopupOpen(const char* str_id, uint32_t popup_flags)
{
    GWP_UI_ENTER("is_popup_open", 0);
    const ImGuiPopupFlags flags = PopupFlags(popup_flags, "is_popup_open", false);
    if (!str_id && !(flags & ImGuiPopupFlags_AnyPopupId))
    {
        Label(str_id, "is_popup_open");
        return 0;
    }
    // A string id with ANY_POPUP_LEVEL: IsPopupOpen(const char*) of ImGui asserts on it, the overload with the id
    // does what the header says (the popup open at any level). Review 2026-11-18
    if (!(flags & ImGuiPopupFlags_AnyPopupId))
        return ImGui::IsPopupOpen(ImGui::GetID(str_id), flags) ? 1 : 0;
    return ImGui::IsPopupOpen(static_cast<ImGuiID>(0), flags) ? 1 : 0; // ANY_POPUP_ID: the id 0, as ImGui itself
}

// ---------------------------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiBeginMenuBar()
{
    GWP_UI_ENTER("begin_menu_bar", 0);
    if (!CanPushOr("begin_menu_bar"))
        return 0;
    // A second begin_menu_bar inside the open one: ImGui asserts (!DC.MenuBarAppending), as ui.BeginMenuBar of Lua
    // refuses it. Review 2026-11-18
    if (ImGui::GetCurrentWindow()->DC.MenuBarAppending)
    {
        imgui::WarnOnce(kWarnPlace, "GwpImGuiApi::begin_menu_bar: the menu bar of this window is open already, the "
            "call is ignored (this warning is shown once)");
        return 0;
    }
    const bool open = ImGui::BeginMenuBar();
    if (open)
        imgui::Push(imgui::EScope::MenuBar);
    return open ? 1 : 0;
}

void GWP_CALL UiEndMenuBar()
{
    GWP_UI_ENTER_VOID("end_menu_bar");
    if (!CheckPopOr(imgui::EScope::MenuBar, "end_menu_bar"))
        return;
    ImGui::EndMenuBar();
    imgui::Pop();
}

int GWP_CALL UiBeginMenu(const char* label, int enabled)
{
    GWP_UI_ENTER("begin_menu", 0);
    if (!Label(label, "begin_menu") || !CanPushOr("begin_menu"))
        return 0;
    const bool open = ImGui::BeginMenu(label, enabled != 0);
    if (open)
        imgui::Push(imgui::EScope::Menu);
    return open ? 1 : 0;
}

void GWP_CALL UiEndMenu()
{
    GWP_UI_ENTER_VOID("end_menu");
    if (!CheckPopOr(imgui::EScope::Menu, "end_menu"))
        return;
    ImGui::EndMenu();
    imgui::Pop();
}

int GWP_CALL UiMenuItem(const char* label, const char* shortcut, int* p_selected, int enabled)
{
    GWP_UI_ENTER("menu_item", 0);
    if (!Label(label, "menu_item"))
        return 0;
    bool selected = p_selected ? *p_selected != 0 : false;
    const bool clicked = ImGui::MenuItem(label, shortcut, p_selected ? &selected : nullptr, enabled != 0);
    if (p_selected)
        *p_selected = selected ? 1 : 0;
    return clicked ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
int GWP_CALL UiIsItemHovered(uint32_t flags)
{
    GWP_UI_ENTER("is_item_hovered", 0);
    const ImGuiHoveredFlags hovered =
        MapFlags(flags, kHoveredFlags, "is_item_hovered") & ImGuiHoveredFlags_AllowedMaskForIsItemHovered;
    return ImGui::IsItemHovered(hovered) ? 1 : 0;
}

int GWP_CALL UiIsItemActive()
{
    GWP_UI_ENTER("is_item_active", 0);
    return ImGui::IsItemActive() ? 1 : 0;
}

int GWP_CALL UiIsItemClicked(int mouse_button)
{
    GWP_UI_ENTER("is_item_clicked", 0);
    if (mouse_button < GWP_IMGUI_MOUSE_LEFT || mouse_button > GWP_IMGUI_MOUSE_MIDDLE)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::is_item_clicked: mouse button %d is not GWP_IMGUI_MOUSE_*, 0 is "
            "returned (this warning is shown once)", mouse_button);
        return 0;
    }
    return ImGui::IsItemClicked(mouse_button) ? 1 : 0;
}

int GWP_CALL UiIsItemEdited()
{
    GWP_UI_ENTER("is_item_edited", 0);
    return ImGui::IsItemEdited() ? 1 : 0;
}

int GWP_CALL UiIsItemDeactivatedAfterEdit()
{
    GWP_UI_ENTER("is_item_deactivated_after_edit", 0);
    return ImGui::IsItemDeactivatedAfterEdit() ? 1 : 0;
}

int GWP_CALL UiIsWindowFocused(uint32_t flags)
{
    GWP_UI_ENTER("is_window_focused", 0);
    return ImGui::IsWindowFocused(MapFlags(flags, kFocusedFlags, "is_window_focused")) ? 1 : 0;
}

int GWP_CALL UiIsWindowHovered(uint32_t flags)
{
    GWP_UI_ENTER("is_window_hovered", 0);
    const ImGuiHoveredFlags hovered =
        MapFlags(flags, kHoveredFlags, "is_window_hovered") & ImGuiHoveredFlags_AllowedMaskForIsWindowHovered;
    return ImGui::IsWindowHovered(hovered) ? 1 : 0;
}

void GWP_CALL UiSetKeyboardFocusHere(int offset)
{
    GWP_UI_ENTER_VOID("set_keyboard_focus_here");
    ImGui::SetKeyboardFocusHere(offset < -1 ? -1 : offset);
}

void GWP_CALL UiSetItemDefaultFocus()
{
    GWP_UI_ENTER_VOID("set_item_default_focus");
    ImGui::SetItemDefaultFocus();
}

// ---------------------------------------------------------------------------------------------
// Ids and style
// ---------------------------------------------------------------------------------------------
void GWP_CALL UiPushIdStr(const char* str_id)
{
    GWP_UI_ENTER_VOID("push_id_str");
    if (!CanPushOr("push_id_str"))
        return;
    ImGui::PushID(TextOr(str_id));
    imgui::Push(imgui::EScope::Id);
}

void GWP_CALL UiPushIdInt(int int_id)
{
    GWP_UI_ENTER_VOID("push_id_int");
    if (!CanPushOr("push_id_int"))
        return;
    ImGui::PushID(int_id);
    imgui::Push(imgui::EScope::Id);
}

void GWP_CALL UiPopId()
{
    GWP_UI_ENTER_VOID("pop_id");
    if (!CheckPopOr(imgui::EScope::Id, "pop_id"))
        return;
    ImGui::PopID();
    imgui::Pop();
}

void GWP_CALL UiPushStyleColor(uint32_t idx, uint32_t argb)
{
    GWP_UI_ENTER_VOID("push_style_color");
    if (idx >= GWP_IMGUI_COL_COUNT)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::push_style_color: idx %u is not GWP_IMGUI_COL_*, the call is "
            "ignored (this warning is shown once)", idx);
        return;
    }
    if (!CanPushOr("push_style_color"))
        return;
    ImGui::PushStyleColor(kColors[idx], Color(argb));
    imgui::Push(imgui::EScope::StyleColor);
}

void GWP_CALL UiPopStyleColor()
{
    GWP_UI_ENTER_VOID("pop_style_color");
    if (!CheckPopOr(imgui::EScope::StyleColor, "pop_style_color"))
        return;
    ImGui::PopStyleColor(1);
    imgui::Pop();
}

// The variable of the style for push_style_var_*: nullptr - a bad index or the other kind (warned)
const StyleVarMap* StyleVar(uint32_t idx, bool vec2, pcstr function)
{
    if (idx >= GWP_IMGUI_STYLE_COUNT || kStyleVars[idx].vec2 != vec2)
    {
        imgui::WarnOnce(kWarnArgument, "GwpImGuiApi::%s: idx %u is not a %s variable of GWP_IMGUI_STYLE_*, the call is "
            "ignored (this warning is shown once)", function, idx, vec2 ? "(vec2)" : "(float)");
        return nullptr;
    }
    return &kStyleVars[idx];
}

void GWP_CALL UiPushStyleVarFloat(uint32_t idx, float value)
{
    GWP_UI_ENTER_VOID("push_style_var_float");
    const StyleVarMap* var = StyleVar(idx, false, "push_style_var_float");
    if (!var || !CanPushOr("push_style_var_float"))
        return;
    ImGui::PushStyleVar(var->var, Finite(value));
    imgui::Push(imgui::EScope::StyleVar);
}

void GWP_CALL UiPushStyleVarVec2(uint32_t idx, float x, float y)
{
    GWP_UI_ENTER_VOID("push_style_var_vec2");
    const StyleVarMap* var = StyleVar(idx, true, "push_style_var_vec2");
    if (!var || !CanPushOr("push_style_var_vec2"))
        return;
    ImGui::PushStyleVar(var->var, Size(x, y));
    imgui::Push(imgui::EScope::StyleVar);
}

void GWP_CALL UiPopStyleVar()
{
    GWP_UI_ENTER_VOID("pop_style_var");
    if (!CheckPopOr(imgui::EScope::StyleVar, "pop_style_var"))
        return;
    ImGui::PopStyleVar(1);
    imgui::Pop();
}

void GWP_CALL UiBeginDisabled(int disabled)
{
    GWP_UI_ENTER_VOID("begin_disabled");
    if (!CanPushOr("begin_disabled"))
        return;
    ImGui::BeginDisabled(disabled != 0);
    imgui::Push(imgui::EScope::Disabled);
}

void GWP_CALL UiEndDisabled()
{
    GWP_UI_ENTER_VOID("end_disabled");
    if (!CheckPopOr(imgui::EScope::Disabled, "end_disabled"))
        return;
    ImGui::EndDisabled();
    imgui::Pop();
}

#undef GWP_UI_ENTER
#undef GWP_UI_ENTER_VOID
#undef GWP_UI_CALLER

GwpImGuiApi MakeUiTable()
{
    GwpImGuiApi t{};
    t.size = sizeof(GwpImGuiApi);
    t.reserved = 0;
    t.same_line = &UiSameLine;
    t.new_line = &UiNewLine;
    t.spacing = &UiSpacing;
    t.separator = &UiSeparator;
    t.separator_text = &UiSeparatorText;
    t.dummy = &UiDummy;
    t.indent = &UiIndent;
    t.unindent = &UiUnindent;
    t.begin_group = &UiBeginGroup;
    t.end_group = &UiEndGroup;
    t.begin_child = &UiBeginChild;
    t.end_child = &UiEndChild;
    t.set_next_item_width = &UiSetNextItemWidth;
    t.push_item_width = &UiPushItemWidth;
    t.pop_item_width = &UiPopItemWidth;
    t.align_text_to_frame_padding = &UiAlignTextToFramePadding;
    t.get_content_region_avail = &UiGetContentRegionAvail;
    t.get_window_size = &UiGetWindowSize;
    t.get_cursor_pos = &UiGetCursorPos;
    t.set_cursor_pos = &UiSetCursorPos;
    t.calc_text_size = &UiCalcTextSize;
    t.get_text_line_height = &UiGetTextLineHeight;
    t.get_frame_height = &UiGetFrameHeight;
    t.text = &UiText;
    t.text_n = &UiTextN;
    t.text_colored = &UiTextColored;
    t.text_disabled = &UiTextDisabled;
    t.text_wrapped = &UiTextWrapped;
    t.label_text = &UiLabelText;
    t.bullet_text = &UiBulletText;
    t.bullet = &UiBullet;
    t.push_text_wrap_pos = &UiPushTextWrapPos;
    t.pop_text_wrap_pos = &UiPopTextWrapPos;
    t.button = &UiButton;
    t.small_button = &UiSmallButton;
    t.invisible_button = &UiInvisibleButton;
    t.arrow_button = &UiArrowButton;
    t.checkbox = &UiCheckbox;
    t.checkbox_flags = &UiCheckboxFlags;
    t.radio_button = &UiRadioButton;
    t.progress_bar = &UiProgressBar;
    t.slider_float = &UiSliderFloat;
    t.slider_float_n = &UiSliderFloatN;
    t.slider_int = &UiSliderInt;
    t.slider_int_n = &UiSliderIntN;
    t.slider_angle = &UiSliderAngle;
    t.drag_float = &UiDragFloat;
    t.drag_float_n = &UiDragFloatN;
    t.drag_int = &UiDragInt;
    t.drag_int_n = &UiDragIntN;
    t.input_float = &UiInputFloat;
    t.input_float_n = &UiInputFloatN;
    t.input_int = &UiInputInt;
    t.input_text = &UiInputText;
    t.input_text_with_hint = &UiInputTextWithHint;
    t.input_text_multiline = &UiInputTextMultiline;
    t.combo = &UiCombo;
    t.begin_combo = &UiBeginCombo;
    t.end_combo = &UiEndCombo;
    t.selectable = &UiSelectable;
    t.list_box = &UiListBox;
    t.begin_list_box = &UiBeginListBox;
    t.end_list_box = &UiEndListBox;
    t.color_edit3 = &UiColorEdit3;
    t.color_edit4 = &UiColorEdit4;
    t.color_button = &UiColorButton;
    t.tree_node = &UiTreeNode;
    t.tree_node_ex = &UiTreeNodeEx;
    t.tree_pop = &UiTreePop;
    t.collapsing_header = &UiCollapsingHeader;
    t.set_next_item_open = &UiSetNextItemOpen;
    t.begin_tab_bar = &UiBeginTabBar;
    t.end_tab_bar = &UiEndTabBar;
    t.begin_tab_item = &UiBeginTabItem;
    t.end_tab_item = &UiEndTabItem;
    t.begin_table = &UiBeginTable;
    t.end_table = &UiEndTable;
    t.table_next_row = &UiTableNextRow;
    t.table_next_column = &UiTableNextColumn;
    t.table_set_column_index = &UiTableSetColumnIndex;
    t.table_setup_column = &UiTableSetupColumn;
    t.table_setup_scroll_freeze = &UiTableSetupScrollFreeze;
    t.table_headers_row = &UiTableHeadersRow;
    t.plot_lines = &UiPlotLines;
    t.plot_histogram = &UiPlotHistogram;
    t.set_tooltip = &UiSetTooltip;
    t.set_item_tooltip = &UiSetItemTooltip;
    t.begin_tooltip = &UiBeginTooltip;
    t.begin_item_tooltip = &UiBeginItemTooltip;
    t.end_tooltip = &UiEndTooltip;
    t.open_popup = &UiOpenPopup;
    t.begin_popup = &UiBeginPopup;
    t.begin_popup_modal = &UiBeginPopupModal;
    t.begin_popup_context_item = &UiBeginPopupContextItem;
    t.end_popup = &UiEndPopup;
    t.close_current_popup = &UiCloseCurrentPopup;
    t.is_popup_open = &UiIsPopupOpen;
    t.begin_menu_bar = &UiBeginMenuBar;
    t.end_menu_bar = &UiEndMenuBar;
    t.begin_menu = &UiBeginMenu;
    t.end_menu = &UiEndMenu;
    t.menu_item = &UiMenuItem;
    t.is_item_hovered = &UiIsItemHovered;
    t.is_item_active = &UiIsItemActive;
    t.is_item_clicked = &UiIsItemClicked;
    t.is_item_edited = &UiIsItemEdited;
    t.is_item_deactivated_after_edit = &UiIsItemDeactivatedAfterEdit;
    t.is_window_focused = &UiIsWindowFocused;
    t.is_window_hovered = &UiIsWindowHovered;
    t.set_keyboard_focus_here = &UiSetKeyboardFocusHere;
    t.set_item_default_focus = &UiSetItemDefaultFocus;
    t.push_id_str = &UiPushIdStr;
    t.push_id_int = &UiPushIdInt;
    t.pop_id = &UiPopId;
    t.push_style_color = &UiPushStyleColor;
    t.pop_style_color = &UiPopStyleColor;
    t.push_style_var_float = &UiPushStyleVarFloat;
    t.push_style_var_vec2 = &UiPushStyleVarVec2;
    t.pop_style_var = &UiPopStyleVar;
    t.begin_disabled = &UiBeginDisabled;
    t.end_disabled = &UiEndDisabled;
    // appended after the first version (GWP_IMGUI_HAS(ui, set_clipboard_text))
    t.set_clipboard_text = &UiSetClipboardText;
    return t;
}

// Every slot of the table is set: a forgotten one would be a NULL call in a plugin
bool TableComplete(const GwpImGuiApi& table)
{
    // The slots are function pointers of one size, right after the header (static_assert in gwp_api.h)
    const char* bytes = reinterpret_cast<const char*>(&table);
    for (size_t offset = offsetof(GwpImGuiApi, same_line); offset + sizeof(void*) <= sizeof(GwpImGuiApi);
         offset += sizeof(void*))
    {
        const void* slot = nullptr;
        memcpy(&slot, bytes + offset, sizeof(slot));
        if (!slot)
            return false;
    }
    return true;
}

const GwpImGuiApi& UiTable()
{
    static const GwpImGuiApi table = MakeUiTable();
    return table;
}

// ---------------------------------------------------------------------------------------------
// The callbacks of a plugin tool: its C functions under the guards of the host
// ---------------------------------------------------------------------------------------------
struct DrawCall
{
    GwpImGuiDrawFn fn;
    void* user;
    const GwpImGuiApi* ui;
    const GwpImGuiFrame* frame;
};

void DrawThunk(void* context)
{
    const DrawCall& call = *static_cast<const DrawCall*>(context);
    call.fn(call.user, call.ui, call.frame);
}

struct ToolCall
{
    GwpImGuiToolFn fn;
    void* user;
};

void ToolThunk(void* context)
{
    const ToolCall& call = *static_cast<const ToolCall*>(context);
    call.fn(call.user);
}

class PluginToolCallbacks final : public imgui_tools::ToolCallbacks
{
public:
    PluginToolCallbacks(const GwpPlugin* plugin, const xr_string& id, const GwpImGuiToolDesc& desc)
        : m_plugin(plugin), m_draw(desc.draw), m_on_open(desc.on_open), m_on_close(desc.on_close), m_user(desc.user),
          m_where("the ImGui tool '" + id + "'"), m_draw_what("ImGui tool '" + id + "' draw"),
          m_open_what("ImGui tool '" + id + "' on_open"), m_close_what("ImGui tool '" + id + "' on_close"),
          m_perf_zone("imgui:" + id)
    {
    }

    EToolKind Kind() const override { return EToolKind::Plugin; }
    const void* Key() const override { return m_plugin; }

    bool Draw(const ToolDef& def, const FrameInfo& frame, xr_string& error) override
    {
        if (!IsPluginRunning(m_plugin))
        {
            error = "the plugin is stopped";
            return false;
        }
        GwpImGuiFrame info{};
        info.size = sizeof(GwpImGuiFrame);
        info.flags = (frame.light ? GWP_IMGUI_FRAME_LIGHT : 0u) | (frame.first ? GWP_IMGUI_FRAME_FIRST : 0u) |
            (frame.in_game ? GWP_IMGUI_FRAME_IN_GAME : 0u);
        info.dt = frame.dt;
        info.reserved = 0;
        info.id = def.id.c_str();
        DrawCall call{ m_draw, m_user, &UiTable(), &info };
        const u64 t0 = perf::Enabled() ? xray::perf::Ticks() : 0; // addon_perf: imgui:<id>
        const bool ok = events::CallPluginGuarded(reinterpret_cast<const void*>(m_draw), m_draw_what.c_str(),
            &DrawThunk, &call);
        if (t0)
            perf::NoteNative(PluginAddonId(m_plugin), m_perf_zone.c_str(), xray::perf::Ticks() - t0);
        if (!ok)
        {
            // The plugin stops: its tools are marked dead (this one too), the host closes what draw left open
            error = "the plugin crashed in draw";
            OnPluginCrashed(m_plugin, m_where.c_str());
            return false;
        }
        return true;
    }

    bool OnOpen(const ToolDef& /*def*/, xr_string& error) override
    {
        if (!m_on_open || !IsPluginRunning(m_plugin))
            return true;
        ToolCall call{ m_on_open, m_user };
        if (events::CallPluginGuarded(reinterpret_cast<const void*>(m_on_open), m_open_what.c_str(), &ToolThunk, &call))
            return true;
        error = "the plugin crashed in on_open";
        OnPluginCrashed(m_plugin, m_where.c_str());
        return false;
    }

    void OnClose(const ToolDef& /*def*/) override
    {
        if (!m_on_close || !IsPluginRunning(m_plugin))
            return;
        ToolCall call{ m_on_close, m_user };
        if (!events::CallPluginGuarded(reinterpret_cast<const void*>(m_on_close), m_close_what.c_str(), &ToolThunk,
                &call))
        {
            OnPluginCrashed(m_plugin, m_where.c_str());
        }
    }

    // Nothing of the plugin to release: its functions are only called, the strings are copies
    void Release(bool /*alive*/) override {}

private:
    const GwpPlugin* m_plugin;
    GwpImGuiDrawFn m_draw;
    GwpImGuiToolFn m_on_open;
    GwpImGuiToolFn m_on_close;
    void* m_user;
    xr_string m_where;      // OnPluginCrashed: "crashed in the ImGui tool '<id>'"
    xr_string m_draw_what;  // CallPluginGuarded: the crash block of the log
    xr_string m_open_what;
    xr_string m_close_what;
    xr_string m_perf_zone;  // addon_perf_report
};

// ---------------------------------------------------------------------------------------------
// GwpEngineApi: imgui_tool_*
// ---------------------------------------------------------------------------------------------
// The common checks: self and the main thread. false - `result` is the code
bool CheckToolCall(const GwpPlugin* self, pcstr function, GwpResult& result)
{
    if (!self)
    {
        Msg("! [imgui] %s called without the plugin handle (self), ignored", function);
        result = GWP_ERROR_INVALID_ARGUMENT;
        return false;
    }
    if (!IsMainThread())
    {
        Msg("! [plugin:%s] [imgui] %s called outside the main thread, ignored", PluginAddonId(self), function);
        result = GWP_ERROR_NOT_MAIN_THREAD;
        return false;
    }
    return true;
}

GwpResult Refuse(const GwpPlugin* self, pcstr id, GwpResult code, pcstr why)
{
    Msg("! [plugin:%s] imgui_tool_register('%s'): %s", PluginAddonId(self), id ? id : "?", why);
    return code;
}

bool FiniteSize(float w, float h) { return std::isfinite(w) && std::isfinite(h) && w >= 0.f && h >= 0.f; }

GwpResult GWP_CALL ApiImGuiToolRegister(const GwpPlugin* self, const GwpImGuiToolDesc* desc)
{
    GwpResult result = GWP_OK;
    if (!CheckToolCall(self, "imgui_tool_register", result))
        return result;
    if (!desc)
        return Refuse(self, nullptr, GWP_ERROR_INVALID_ARGUMENT, "desc is NULL");
    if (desc->size < kToolDescMinSize)
    {
        string128 why;
        xr_sprintf(why, "desc->size %u is below sizeof(GwpImGuiToolDesc) of version 0.1 (%u)", desc->size,
            kToolDescMinSize);
        return Refuse(self, nullptr, GWP_ERROR_INVALID_ARGUMENT, why);
    }
    // The fields this engine knows (a newer plugin may give a longer structure: its tail is not read; an older one a
    // shorter one of a later engine: the fields past its size stay 0)
    GwpImGuiToolDesc d{};
    memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(d)));
    if (!d.id)
        return Refuse(self, nullptr, GWP_ERROR_INVALID_ARGUMENT, "id is NULL");
    if (!IsPluginRunning(self))
        return Refuse(self, d.id, GWP_ERROR_INVALID_STATE, "the plugin is stopped");
    if (d.reserved[0] || d.reserved[1])
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, "reserved must be 0");
    if (!d.draw)
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, "draw is NULL");
    if (d.flags & ~kToolFlagsAll)
        return Refuse(self, d.id, GWP_ERROR_NOT_SUPPORTED, "unknown bits of flags (GWP_IMGUI_TOOL_*)");
    if (d.window_flags & ~static_cast<u32>(imgui::kWindowAll))
        return Refuse(self, d.id, GWP_ERROR_NOT_SUPPORTED, "unknown bits of window_flags (GWP_IMGUI_WINDOW_*)");
    if (!FiniteSize(d.size_w, d.size_h) || !FiniteSize(d.pos_x, d.pos_y) || !FiniteSize(d.min_w, d.min_h))
    {
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, "a size, a position or a min size is negative, NaN or "
            "infinite");
    }
    // The id: "<addon id>_..." (as the cvars of a plugin), the charset and the length - the registry checks them
    const pcstr owner = PluginAddonId(self);
    const size_t owner_length = xr_strlen(owner);
    if (strncmp(d.id, owner, owner_length) != 0 || d.id[owner_length] != '_')
    {
        string256 why;
        xr_sprintf(why, "the id must start with \"%s_\" (the addon id)", owner);
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, why);
    }
    // A longer addon id owns its names, as with the cvars (addon_api_console.cpp CheckOwnName): addon "gw" may not
    // take "gw_npc_*" while addon "gw_npc" is installed - the ids are unique among all the tools, so it would block
    // the tool of gw_npc (review 2026-11-18)
    if (const pcstr name_owner = NameOwnerAddonId(d.id); name_owner && xr_strcmp(name_owner, owner) != 0)
    {
        string256 why;
        xr_sprintf(why, "the id belongs to addon '%s' (a longer addon id)", name_owner);
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, why);
    }

    ToolDef def;
    def.id = d.id;
    def.owner = owner;
    def.title = d.title ? d.title : "";
    def.menu = d.menu ? d.menu : "";
    def.tooltip = d.tooltip ? d.tooltip : "";
    def.order = d.order;
    def.window_flags = d.window_flags;
    def.size[0] = d.size_w;
    def.size[1] = d.size_h;
    def.min_size[0] = d.min_w;
    def.min_size[1] = d.min_h;
    if (d.pos_x > 0.f || d.pos_y > 0.f)
    {
        def.pos[0] = d.pos_x;
        def.pos[1] = d.pos_y;
    }
    def.dev_only = (d.flags & GWP_IMGUI_TOOL_DEV_ONLY) != 0;
    def.hide_in_light = (d.flags & GWP_IMGUI_TOOL_HIDE_IN_LIGHT) != 0;
    def.no_save_open = (d.flags & GWP_IMGUI_TOOL_NO_SAVE_OPEN) != 0;

    using imgui_tools::ERegister;
    const ERegister registered =
        imgui_tools::Register(def, std::make_unique<PluginToolCallbacks>(self, def.id, d));
    switch (registered)
    {
    case ERegister::Ok:
        if (IsDebugLog())
            Msg("* [plugin:%s] ImGui tool '%s' registered", owner, d.id);
        return GWP_OK;
    case ERegister::BadId:
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, "a bad id (a-z 0-9 _ . -, 2..63 bytes)");
    case ERegister::BadOwner:
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, "the addon id cannot be the owner of a tool");
    case ERegister::BadDef:
        return Refuse(self, d.id, GWP_ERROR_INVALID_ARGUMENT, "a length over its limit (title 127, menu 127 and 3 "
            "levels, tooltip 511 bytes) or a bad size");
    case ERegister::Exists:
        return Refuse(self, d.id, GWP_ERROR_INVALID_STATE, "the id is taken");
    case ERegister::Limit:
        return Refuse(self, d.id, GWP_ERROR_INVALID_STATE, "too many tools (32 per addon, 256 in all)");
    }
    return Refuse(self, d.id, GWP_ERROR, "refused");
}

// The tool `id` of the plugin `self`: GWP_OK, GWP_ERROR_NOT_FOUND, GWP_ERROR_ACCESS_DENIED (another addon, the Lua part)
GwpResult OwnTool(const GwpPlugin* self, pcstr id)
{
    for (const imgui_tools::ToolInfo& info : imgui_tools::List())
    {
        if (info.id != id)
            continue;
        return info.kind == EToolKind::Plugin && info.owner == PluginAddonId(self) ? GWP_OK : GWP_ERROR_ACCESS_DENIED;
    }
    return GWP_ERROR_NOT_FOUND;
}

GwpResult GWP_CALL ApiImGuiToolUnregister(const GwpPlugin* self, const char* id)
{
    GwpResult result = GWP_OK;
    if (!CheckToolCall(self, "imgui_tool_unregister", result))
        return result;
    if (!id)
        return GWP_ERROR_INVALID_ARGUMENT;
    result = OwnTool(self, id);
    if (result != GWP_OK)
    {
        if (IsDebugLog())
        {
            Msg("~ [plugin:%s] imgui_tool_unregister('%s'): %s", PluginAddonId(self), id,
                result == GWP_ERROR_NOT_FOUND ? "no such tool" : "a tool of another addon or of the Lua part");
        }
        return result;
    }
    return imgui_tools::Unregister(id, PluginAddonId(self), false) == imgui_tools::EUnregister::Ok ? GWP_OK
                                                                                                  : GWP_ERROR_NOT_FOUND;
}

GwpResult GWP_CALL ApiImGuiToolSetOpen(const GwpPlugin* self, const char* id, int open, uint32_t flags)
{
    GwpResult result = GWP_OK;
    if (!CheckToolCall(self, "imgui_tool_set_open", result))
        return result;
    if (!id)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (flags & ~GWP_IMGUI_SHOW_EDITOR)
        return GWP_ERROR_NOT_SUPPORTED;
    return imgui_tools::SetOpen(id, open != 0, open != 0 && (flags & GWP_IMGUI_SHOW_EDITOR) != 0) ? GWP_OK
                                                                                                  : GWP_ERROR_NOT_FOUND;
}

int GWP_CALL ApiImGuiToolIsOpen(const GwpPlugin* self, const char* id)
{
    if (!self || !id || !IsMainThread())
        return 0;
    return imgui_tools::IsOpen(id) ? 1 : 0;
}
} // namespace
} // namespace gw::addons::imgui_plugin

namespace gw::addons::imgui
{
void FillEngineApi(GwpEngineApi& api)
{
    R_ASSERT2(imgui_plugin::TableComplete(imgui_plugin::UiTable()), "GwpImGuiApi: a slot of the table is not set");
    api.imgui_tool_register = &imgui_plugin::ApiImGuiToolRegister;
    api.imgui_tool_unregister = &imgui_plugin::ApiImGuiToolUnregister;
    api.imgui_tool_set_open = &imgui_plugin::ApiImGuiToolSetOpen;
    api.imgui_tool_is_open = &imgui_plugin::ApiImGuiToolIsOpen;
}

void RemovePluginTools(const GwpPlugin* plugin)
{
    if (plugin)
        imgui_tools::RemoveByKey(imgui_tools::EToolKind::Plugin, plugin, false);
}
} // namespace gw::addons::imgui
