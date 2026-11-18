#pragma once

// ImGui tools of the addons (plans/addons_api_support/20-imgui-addon-tools.md): windows of the Lua parts of the addons
// (and of the plugins, stage 4) in the menu "Addons" of the in-game editor (F10).
//  - The registry: one entry per tool - owner (the addon id), id, title / menu / tooltip (the id of a string or the
//    text, translated when shown and again on on_localization_change), the window flags of GW, the size and position
//    of the first open, light (draw in the light state or not), dev_only, the open flag, failed + reason, and the
//    callbacks of its kind (Lua: the functions of the addon; a plugin: its C functions, stage 4).
//  - The host AddonToolsHost: ONE ide_tool for all the addons (a member of CGamePersistent). It shows the menu
//    "Addons" in the main menu bar of the editor (full; -uidbg is not needed) and the windows of the open tools
//    (full and light). For each open tool: Begin("<title>###gw.<owner>.<id>"), the safety state of ImGui, draw through
//    the callbacks (a protected call), the shadow stack closes what draw left open, the recovery of ImGui, End.
//  - An error in draw (on_open): the window closes, the tool is failed (the item of the menu has "(error)" and the
//    reason as its tooltip; opening it again clears the mark), the callbacks log it, the other tools are drawn.
//  - The open flag is kept in imgui.ini ([GWAddonTool][<owner>/<id>] Open=1, a settings handler of ImGui): the lines
//    are kept for every tool ever seen - an addon switched off or a Lua state that is not there yet loses nothing.
//    The geometry is the business of ImGui itself (the window name after "###" does not change with the title).
//  - Registration, removal, set_open from draw or from the Game thread of an overlap frame do not break the loop of the
//    host: a removed entry is only marked dead and freed when no loop runs; the change of the state of the editor and
//    the marks of imgui.ini wait for the main thread (OnFrame).
//  - Console addon_imgui_tools 0/1 (user.ltx, 1 by default): 0 hides the menu and draws no window of the addons.
// Main thread (the thread of the game logic: the Lua state; never concurrent with the ImGui frame).

#include "xrEngine/editor_base.h"

#include <memory>

struct lua_State;

namespace gw::addons::imgui_tools
{
// Console addon_imgui_tools (console_commands.cpp): 0 - no menu "Addons", no window of an addon is drawn
extern int g_enabled;

constexpr size_t kMaxIdLength = 63;      // a-z 0-9 _ . -, 2..63 bytes
constexpr size_t kMaxTitleLength = 127;  // title (the id of a string or the text)
constexpr size_t kMaxMenuLength = 127;   // menu "A/B/C"
constexpr u32 kMaxMenuDepth = 3;         // levels of the submenus
constexpr size_t kMaxTooltipLength = 511;
constexpr u32 kMaxToolsPerOwner = 32;
constexpr u32 kMaxTools = 256;

enum class EToolKind : u8
{
    Lua,
    Plugin,
};

// What a tool is (the copy of the registration)
struct ToolDef
{
    xr_string id;
    xr_string owner;
    xr_string title;   // the id of a string or the text; empty - the id
    xr_string menu;    // "A/B" (up to kMaxMenuDepth levels); empty - the owner
    xr_string tooltip; // empty - none
    int order = 0;
    u32 window_flags = 0; // imgui::EWindowFlags (addon_imgui_core.h)
    float size[2] = { 0.f, 0.f };     // the size of the first open, 0 - auto
    float min_size[2] = { 0.f, 0.f }; // the smallest size, 0 - none
    float pos[2] = { -1.f, -1.f };    // the position of the first open, < 0 - ImGui decides
    bool hide_in_light = false;       // not drawn in the light state
    bool dev_only = false;            // only with -uidbg / -dbg / -addon_debug
    bool no_save_open = false;        // the open flag is not kept in imgui.ini
};

// The frame of one draw
struct FrameInfo
{
    bool light = false;   // the light state: transparent, no input
    bool first = false;   // the first draw after the window opened
    bool in_game = false; // a level and the actor are there
    float dt = 0.f;       // real seconds since the previous frame
};

// The callbacks of a tool (one object per registration, owned by the registry: deleted by Release... see below)
class ToolCallbacks
{
public:
    virtual ~ToolCallbacks() = default;
    virtual EToolKind Kind() const = 0;
    // The thing the tool belongs to: lua_State* of the Lua part, GwpPlugin* of a plugin (RemoveByKey)
    virtual const void* Key() const = 0;
    // The content of the window: inside Begin/End of the host, the shadow stack frame is set. false - an error (the
    // callbacks logged it; error - its first line, the reason of the failed tool)
    virtual bool Draw(const ToolDef& def, const FrameInfo& frame, xr_string& error) = 0;
    // The window opens (before its first draw) / closed. false from OnOpen - the window does not open (logged)
    virtual bool OnOpen(const ToolDef& def, xr_string& error) = 0;
    virtual void OnClose(const ToolDef& def) = 0;
    // The entry goes: alive - the owner (the Lua state, the plugin) still lives, its references may be released
    virtual void Release(bool alive) = 0;
};

enum class ERegister
{
    Ok,
    BadDef,  // a length is over its limit, a bad size
    BadId,   // a-z 0-9 _ . -, 2..63 bytes
    BadOwner, // empty, longer than 63 bytes, '/', '[', ']', '#' or a control character
    Exists,  // the id is taken (by any addon)
    Limit,   // kMaxToolsPerOwner / kMaxTools
};
pcstr RegisterResultName(ERegister result); // "ok", "bad def", "bad id", "owner", "exists", "limit"
// The checks of Register for an id (a-z 0-9 _ . -, 2..kMaxIdLength) and an owner (ERegister::BadId / BadOwner): a caller
// that checks more about them (the prefix of the addon, addon_imgui_lua.cpp) reports these errors first
bool IsValidId(pcstr id);
bool IsValidOwner(pcstr owner);

// Takes the callbacks (deleted when the entry goes or the registration is refused). The saved open flag of
// <owner>/<id> applies: the window opens with the next frame of the host (on_open there).
ERegister Register(const ToolDef& def, std::unique_ptr<ToolCallbacks> callbacks);

enum class EUnregister
{
    Ok,
    NotFound,
    Owner, // the tool of another addon
};
// quiet = false: an open window gets OnClose, the saved open flag is cleared. quiet = true (the registrations of the
// addon go: addon_reload, a failed on_load): no OnClose, the saved open flag stays (the window opens again after the
// next registration).
EUnregister Unregister(pcstr id, pcstr owner, bool quiet);

// Open / close a window (the menu, set_open, tool.close). Close: OnClose at once. Open: OnOpen comes with the next
// frame of the host. show: the editor goes to full on the next frame (the window is seen at once).
// false - no such tool or it is hidden now (dev_only without the keys).
bool SetOpen(pcstr id, bool open, bool show);
// The window is open or asked to open (true right after SetOpen(id, true))
bool IsOpen(pcstr id);
// tool.set_title: the id of a string or the text (<= kMaxTitleLength). false - no such tool or a bad length
bool SetTitle(pcstr id, pcstr title);

// A tool as the list shows it
struct ToolInfo
{
    xr_string id, owner, title, menu; // title and menu translated
    EToolKind kind = EToolKind::Lua;
    bool open = false;
    pcstr state = "ok"; // "ok", "failed", "hidden" (dev_only without the keys)
    xr_string reason;
};
// The live tools in the order of the registration
xr_vector<ToolInfo> List();

// The editor: "hidden", "full", "light"
pcstr EditorStateName();
// Ask for a state ("hidden", "full", "light"): applied on the main thread at the next OnFrame. false - a bad name
bool RequestEditorState(pcstr state);
// The tools of the addons work: the console addon_imgui_tools is 1
bool Available();
// -uidbg / -dbg / -addon_debug: the dev_only tools are shown
bool DevToolsAllowed();

// addon_reload <owner> (addon_lua_entry.cpp, after the registrations of the Lua part went): the Lua tools of the addon go
// quietly (no on_close; the open flag stays for the next on_load). The tools of its plugin go with the plugin (stage 4).
void RemoveOwner(pcstr owner);
// The tools of one kind and key (Key()): a Lua state that ends (alive = false: its references died with it), a plugin
// that stops (stage 4). Quiet, as RemoveOwner. key nullptr - every tool of the kind.
void RemoveByKey(EToolKind kind, const void* key, bool alive);

// Every frame (CGamePersistent::OnFrame, the main thread, no Lua on the stack): the deferred functions of the tools
// (tool.defer), the asked state of the editor, the marks of imgui.ini, the dead entries are freed.
void OnFrame();
// on_localization_change: the titles, the menus and the tooltips are translated again
void LocalizationChanged();
// The host of the addons shuts down (addon_host.cpp Shutdown): every entry goes (quietly, the owners may be gone)
void Shutdown();

// The one ide_tool of all the tools of the addons: a member of CGamePersistent
class AddonToolsHost final : public xray::editor::ide_tool
{
public:
    AddonToolsHost();
    ~AddonToolsHost() override;

    void on_tool_frame() override;
    pcstr tool_name() const override { return "Addon tools"; }
    bool in_tools_menu() const override { return false; }
    void on_main_menu_bar() override;
    bool has_light_windows() const override;
};
} // namespace gw::addons::imgui_tools
