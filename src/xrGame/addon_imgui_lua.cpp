#include "StdAfx.h"

// The Lua side of the ImGui tools of the addons (plans/addons_api_support/20-imgui-addon-tools.md, 4.1): the functions
// _gw_internal.imgui_* (the facade - addons.register_imgui_tool & co., gw_imgui_tools.script), the callbacks of a Lua
// tool, the table `tool`, tool.defer. See addon_imgui_lua.h, addon_imgui_tools.h.

#include "addon_imgui_lua.h"
#include "addon_imgui_tools.h"
#include "addon_imgui_core.h"
#include "addon_host.h"
#include "addon_lua_entry.h" // ReportAddonProtectedFailure
#include "addon_perf.h"

#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrScriptEngine/script_engine.hpp"

#include <memory>

namespace gw::addons::imgui_lua
{
namespace
{
using imgui_tools::EToolKind;
using imgui_tools::FrameInfo;
using imgui_tools::ToolDef;

constexpr u32 kMaxDeferred = 1024; // tool.defer: the functions waiting for the next frame, all the tools together

// The Lua state the references below belong to: every new state counts one up (script_register), a reference of an
// older generation is never released - the state is gone, its address may be the one of the new state
u32 g_generation = 0;
lua_State* g_state = nullptr;
int g_ui_ref = LUA_NOREF; // the table `ui` of this state (in its registry)

struct Deferred
{
    xr_string owner;
    xr_string id;
    int ref = LUA_NOREF;
};
xr_vector<Deferred>* g_deferred = nullptr;

lua_State* MainState() { return GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr; }

// The message handler of the calls below: the message with the Lua stack (a message that has one already keeps it)
int Traceback(lua_State* L)
{
    const pcstr message = lua_tostring(L, 1);
    if (message && strstr(message, "\nstack traceback:"))
    {
        lua_settop(L, 1);
        return 1;
    }
    luaL_traceback(L, L, message && message[0] ? message : "(a Lua error without a message)", 1);
    return 1;
}

// A call of a function of the addon under lua_pcall: the function and its `nargs` arguments are on the top of L.
// false - a Lua error: logged (ReportAddonProtectedFailure), its text in `error`
bool ProtectedCall(lua_State* L, int nargs, pcstr owner, pcstr what, xr_string* error)
{
    const int function_index = lua_gettop(L) - nargs;
    lua_pushcfunction(L, &Traceback);
    lua_insert(L, function_index);
    const int status = lua_pcall(L, nargs, 0, function_index);
    if (status == 0)
    {
        lua_remove(L, function_index);
        return true;
    }
    const pcstr message = lua_tostring(L, -1);
    const xr_string text = message && message[0] ? message : "(a Lua error without a message)";
    lua_pop(L, 2); // the message, the handler
    ReportAddonProtectedFailure(owner, what, text);
    if (error)
        *error = text;
    return false;
}

class LuaToolCallbacks final : public imgui_tools::ToolCallbacks
{
public:
    LuaToolCallbacks(lua_State* L, int draw, int on_open, int on_close, int tool)
        : m_state(L), m_generation(g_generation), m_draw(draw), m_on_open(on_open), m_on_close(on_close), m_tool(tool)
    {
    }

    EToolKind Kind() const override { return EToolKind::Lua; }
    const void* Key() const override { return m_state; }

    bool Draw(const ToolDef& def, const FrameInfo& frame, xr_string& error) override
    {
        lua_State* L = Live();
        if (!L || g_ui_ref == LUA_NOREF)
        {
            error = "the Lua state of the tool is gone";
            return false;
        }
        const int top = lua_gettop(L);
        // addon_perf (imgui:<id>): a copy of draw stays below the call - its owner for the report is the addon of its
        // script file
        const bool perf_on = perf::Enabled();
        if (perf_on)
            lua_rawgeti(L, LUA_REGISTRYINDEX, m_draw);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_draw);
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_ui_ref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_tool);
        // The fields of `tool` the engine keeps: the same table every frame, nothing is allocated
        lua_pushboolean(L, frame.light);
        lua_setfield(L, -2, "light");
        lua_pushboolean(L, frame.first);
        lua_setfield(L, -2, "first");
        lua_pushboolean(L, frame.in_game);
        lua_setfield(L, -2, "in_game");
        lua_pushnumber(L, frame.dt);
        lua_setfield(L, -2, "dt");
        const xr_string what = "ImGui tool '" + def.id + "' draw (the window is closed)";
        const u64 t0 = perf_on ? xray::perf::Ticks() : 0;
        const bool ok = ProtectedCall(L, 2, def.owner.c_str(), what.c_str(), &error);
        if (perf_on)
        {
            const xr_string zone = "imgui:" + def.id;
            perf::NoteLua(L, top + 1, false, zone.c_str(), xray::perf::Ticks() - t0);
        }
        lua_settop(L, top);
        return ok;
    }

    bool OnOpen(const ToolDef& def, xr_string& error) override
    {
        if (m_on_open == LUA_NOREF)
            return true;
        lua_State* L = Live();
        if (!L)
            return true;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_on_open);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_tool);
        const xr_string what = "ImGui tool '" + def.id + "' on_open (the window is not opened)";
        const bool ok = ProtectedCall(L, 1, def.owner.c_str(), what.c_str(), &error);
        lua_settop(L, top);
        return ok;
    }

    void OnClose(const ToolDef& def) override
    {
        if (m_on_close == LUA_NOREF)
            return;
        lua_State* L = Live();
        if (!L)
            return;
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_on_close);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_tool);
        const xr_string what = "ImGui tool '" + def.id + "' on_close";
        ProtectedCall(L, 1, def.owner.c_str(), what.c_str(), nullptr);
        lua_settop(L, top);
    }

    void Release(bool alive) override
    {
        lua_State* L = alive ? Live() : nullptr;
        if (!L)
            return; // the references died with their state
        for (int* ref : { &m_draw, &m_on_open, &m_on_close, &m_tool })
        {
            if (*ref != LUA_NOREF)
                luaL_unref(L, LUA_REGISTRYINDEX, *ref);
            *ref = LUA_NOREF;
        }
    }

private:
    // The state of the references when it is still the state of the game (the same generation), else nullptr
    lua_State* Live() const
    {
        lua_State* main = MainState();
        return main && main == m_state && m_generation == g_generation ? main : nullptr;
    }

    lua_State* m_state;
    u32 m_generation;
    int m_draw;
    int m_on_open;
    int m_on_close;
    int m_tool;
};

// ---------------------------------------------------------------------------------------------
// The methods of `tool`: C closures, upvalue 1 - the id, upvalue 2 - the owner
// ---------------------------------------------------------------------------------------------

// tool.close(): the window closes (on_close at once)
int ToolClose(lua_State* L)
{
    imgui_tools::SetOpen(lua_tostring(L, lua_upvalueindex(1)), false, false);
    return 0;
}

// tool.defer(fn): fn() runs at the start of the next frame, outside of the ImGui window (CGamePersistent::OnFrame)
// tool:defer(fn) works too: a table in front of the function (the tool itself) is skipped
int ToolDefer(lua_State* L)
{
    if (lua_type(L, 1) == LUA_TTABLE)
        lua_remove(L, 1);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!g_deferred)
        g_deferred = xr_new<xr_vector<Deferred>>();
    if (g_deferred->size() >= kMaxDeferred)
        return luaL_error(L, "tool.defer: too many deferred functions (%d) - they run once per frame", kMaxDeferred);
    lua_pushvalue(L, 1);
    Deferred& deferred = g_deferred->emplace_back();
    deferred.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    deferred.id = lua_tostring(L, lua_upvalueindex(1));
    deferred.owner = lua_tostring(L, lua_upvalueindex(2));
    return 0;
}

// tool.set_title(text): the id of a string or the text (<= 127 bytes); the window keeps its place
int ToolSetTitle(lua_State* L)
{
    if (lua_type(L, 1) == LUA_TTABLE)
        lua_remove(L, 1); // tool:set_title(text)
    const pcstr title = luaL_checkstring(L, 1);
    if (xr_strlen(title) > imgui_tools::kMaxTitleLength)
        return luaL_error(L, "tool.set_title: the title is longer than %d bytes", int(imgui_tools::kMaxTitleLength));
    imgui_tools::SetTitle(lua_tostring(L, lua_upvalueindex(1)), title);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// _gw_internal.imgui_*
// ---------------------------------------------------------------------------------------------

pcstr FieldString(lua_State* L, int table, pcstr name, pcstr fallback)
{
    lua_getfield(L, table, name);
    const pcstr value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1); // the string stays alive: it is a value of the table
    return value;
}

lua_Number FieldNumber(lua_State* L, int table, pcstr name, lua_Number fallback)
{
    lua_getfield(L, table, name);
    const lua_Number value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

bool FieldBool(lua_State* L, int table, pcstr name)
{
    lua_getfield(L, table, name);
    const bool value = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}

// A function field: its reference (LUA_NOREF - nil). false - the field is there and is not a function
bool FieldFunction(lua_State* L, int table, pcstr name, int& ref)
{
    ref = LUA_NOREF;
    lua_getfield(L, table, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return true;
    }
    if (lua_type(L, -1) != LUA_TFUNCTION)
    {
        lua_pop(L, 1);
        return false;
    }
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

int Refuse(lua_State* L, pcstr owner, pcstr id, pcstr reason, pcstr text)
{
    Msg("! [addon:%s] addons.register_imgui_tool('%s'): %s", owner, id ? id : "?", text);
    lua_pushboolean(L, false);
    lua_pushstring(L, reason);
    return 2;
}

constexpr pcstr kCoreOwner = "core";       // gw_imgui_tools.script CORE_OWNER: the registrations of the base itself
constexpr pcstr kUnknownOwner = "unknown"; // gw_addons_entry.UNKNOWN_OWNER: outside on_load, no opts.owner

// The id of a tool is in the namespace of its addon, as the cvars and the tools of the plugins (decision of the
// developer 2026-11-18, the review of plan 20): it starts with "<owner>_" and no addon with a longer id owns it
// (NameOwnerAddonId: addon "gw" may not take "gw_npc_*" while addon "gw_npc" is installed - the ids are unique among
// all the tools, it would block the tool of gw_npc). The base ("core") and an unknown owner (registered outside
// on_load without opts.owner) need no prefix, but may not take a name of an installed addon either.
// false - refused, the text in why (the reason "prefix")
bool CheckIdPrefix(const ToolDef& def, string256& why)
{
    // A bad id / owner: Register refuses them as "bad id" / "owner"
    if (!imgui_tools::IsValidId(def.id.c_str()) || !imgui_tools::IsValidOwner(def.owner.c_str()))
        return true;
    if (def.owner != kCoreOwner && def.owner != kUnknownOwner)
    {
        const size_t length = def.owner.size();
        if (def.id.size() <= length || def.id.compare(0, length, def.owner) != 0 || def.id[length] != '_')
        {
            xr_sprintf(why, "the id must start with \"%s_\" (the addon id)", def.owner.c_str());
            return false;
        }
    }
    const pcstr name_owner = NameOwnerAddonId(def.id.c_str());
    if (name_owner && def.owner != name_owner)
    {
        xr_sprintf(why, "the id belongs to addon '%s' (a longer addon id starts it)", name_owner);
        return false;
    }
    return true;
}

// imgui_tool_register(spec) -> true, tool | false, reason
//   spec = {id, owner, title, menu, tooltip, order, draw, on_open, on_close, window_flags (imgui::EWindowFlags),
//           size_w, size_h, min_w, min_h, pos_x, pos_y, hide_in_light, dev_only, no_save_open}
//   (gw_imgui_tools.script makes it from the def of the addon: the names of the flags, the tables of the sizes)
//   reason: "bad def", "bad id", "bad draw", "prefix" (CheckIdPrefix), "exists", "owner", "limit"
int LuaRegister(lua_State* L)
{
    if (lua_type(L, 1) != LUA_TTABLE)
    {
        lua_pushboolean(L, false);
        lua_pushstring(L, "bad def");
        return 2;
    }
    ToolDef def;
    def.owner = FieldString(L, 1, "owner", "unknown");
    def.id = FieldString(L, 1, "id", "");
    def.title = FieldString(L, 1, "title", "");
    def.menu = FieldString(L, 1, "menu", "");
    def.tooltip = FieldString(L, 1, "tooltip", "");
    def.order = static_cast<int>(FieldNumber(L, 1, "order", 0));
    const lua_Number flags = FieldNumber(L, 1, "window_flags", 0);
    def.window_flags = flags >= 0 && flags <= lua_Number(type_max<u32>) ? static_cast<u32>(flags) : type_max<u32>;
    def.size[0] = static_cast<float>(FieldNumber(L, 1, "size_w", 0));
    def.size[1] = static_cast<float>(FieldNumber(L, 1, "size_h", 0));
    def.min_size[0] = static_cast<float>(FieldNumber(L, 1, "min_w", 0));
    def.min_size[1] = static_cast<float>(FieldNumber(L, 1, "min_h", 0));
    def.pos[0] = static_cast<float>(FieldNumber(L, 1, "pos_x", -1));
    def.pos[1] = static_cast<float>(FieldNumber(L, 1, "pos_y", -1));
    def.hide_in_light = FieldBool(L, 1, "hide_in_light");
    def.dev_only = FieldBool(L, 1, "dev_only");
    def.no_save_open = FieldBool(L, 1, "no_save_open");

    lua_getfield(L, 1, "draw");
    const bool has_draw = lua_type(L, -1) == LUA_TFUNCTION;
    lua_pop(L, 1);
    if (!has_draw)
        return Refuse(L, def.owner.c_str(), def.id.c_str(), "bad draw", "draw must be a function");
    lua_State* main = MainState();
    if (!main || g_ui_ref == LUA_NOREF)
        return Refuse(L, def.owner.c_str(), def.id.c_str(), "not supported", "no Lua state of the game");
    string256 why;
    if (!CheckIdPrefix(def, why))
        return Refuse(L, def.owner.c_str(), def.id.c_str(), "prefix", why);

    int draw = LUA_NOREF, on_open = LUA_NOREF, on_close = LUA_NOREF;
    if (!FieldFunction(L, 1, "on_open", on_open) || !FieldFunction(L, 1, "on_close", on_close))
    {
        if (on_open != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, on_open);
        return Refuse(L, def.owner.c_str(), def.id.c_str(), "bad def", "on_open and on_close must be functions");
    }
    FieldFunction(L, 1, "draw", draw);

    // The table `tool`: one per registration, the engine sets light / first / in_game / dt before each draw
    lua_newtable(L);
    lua_pushstring(L, def.id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, def.owner.c_str());
    lua_setfield(L, -2, "owner");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "light");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "first");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "in_game");
    lua_pushnumber(L, 0);
    lua_setfield(L, -2, "dt");
    lua_newtable(L);
    lua_setfield(L, -2, "state"); // the fields of the addon
    const struct
    {
        pcstr name;
        lua_CFunction fn;
    } methods[] = { { "close", &ToolClose }, { "defer", &ToolDefer }, { "set_title", &ToolSetTitle } };
    for (const auto& method : methods)
    {
        lua_pushstring(L, def.id.c_str());
        lua_pushstring(L, def.owner.c_str());
        lua_pushcclosure(L, method.fn, 2);
        lua_setfield(L, -2, method.name);
    }
    lua_pushvalue(L, -1);
    const int tool = luaL_ref(L, LUA_REGISTRYINDEX); // the table stays on the stack: the second result

    auto callbacks = std::make_unique<LuaToolCallbacks>(main, draw, on_open, on_close, tool);
    const imgui_tools::ERegister result = imgui_tools::Register(def, std::move(callbacks));
    if (result != imgui_tools::ERegister::Ok)
    {
        // Register deleted the callbacks object; the references go with a fresh one
        LuaToolCallbacks(main, draw, on_open, on_close, tool).Release(true);
        pcstr text = "refused";
        switch (result)
        {
        case imgui_tools::ERegister::BadId: text = "the id must be 2..63 characters a-z 0-9 _ . -"; break;
        case imgui_tools::ERegister::BadOwner: text = "the owner is not an addon id"; break;
        case imgui_tools::ERegister::Exists: text = "the id is taken (by this or another addon)"; break;
        case imgui_tools::ERegister::Limit: text = "too many ImGui tools (32 per addon, 256 in all)"; break;
        case imgui_tools::ERegister::BadDef:
            text = "a bad field: title / menu longer than 127 bytes, more than 3 levels of menu, tooltip longer than 511 "
                   "bytes, a negative size or unknown flags";
            break;
        default: break;
        }
        lua_pop(L, 1);
        return Refuse(L, def.owner.c_str(), def.id.c_str(), imgui_tools::RegisterResultName(result), text);
    }
    lua_pushboolean(L, true);
    lua_insert(L, -2);
    return 2;
}

// imgui_tool_unregister(id, owner[, quiet]) -> true | false, "not found" | "owner"
//   quiet (gw_imgui_tools.remove_owner: addon_reload, a failed on_load): no on_close, the open flag stays
int LuaUnregister(lua_State* L)
{
    const pcstr id = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const pcstr owner = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "unknown";
    const bool quiet = lua_toboolean(L, 3) != 0;
    switch (imgui_tools::Unregister(id, owner, quiet))
    {
    case imgui_tools::EUnregister::Ok:
        if (quiet)
            Msg("* [addon:%s] ImGui tool '%s' removed with the registrations of the addon", owner, id);
        lua_pushboolean(L, true);
        return 1;
    case imgui_tools::EUnregister::Owner:
        Msg("! [addon:%s] addons.unregister_imgui_tool('%s'): the tool belongs to another addon", owner, id);
        lua_pushboolean(L, false);
        lua_pushstring(L, "owner");
        return 2;
    default:
        lua_pushboolean(L, false);
        lua_pushstring(L, "not found");
        return 2;
    }
}

// imgui_tool_set_open(id, open, show) -> bool
int LuaSetOpen(lua_State* L)
{
    const pcstr id = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const bool open = lua_toboolean(L, 2) != 0;
    const bool show = lua_toboolean(L, 3) != 0;
    if (show && IsDebugLog() && id)
        Msg("* [addons] ImGui tool '%s': set_open with show (the editor goes to full)", id);
    lua_pushboolean(L, imgui_tools::SetOpen(id, open, show));
    return 1;
}

// imgui_tool_is_open(id) -> bool
int LuaIsOpen(lua_State* L)
{
    const pcstr id = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    lua_pushboolean(L, imgui_tools::IsOpen(id));
    return 1;
}

// imgui_tool_list() -> { {id, owner, title, menu, kind = "lua" | "plugin", open, state, reason}, ... }
int LuaList(lua_State* L)
{
    const xr_vector<imgui_tools::ToolInfo> tools = imgui_tools::List();
    lua_createtable(L, static_cast<int>(tools.size()), 0);
    int index = 0;
    for (const imgui_tools::ToolInfo& info : tools)
    {
        lua_createtable(L, 0, 8);
        lua_pushstring(L, info.id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushstring(L, info.owner.c_str());
        lua_setfield(L, -2, "owner");
        lua_pushstring(L, info.title.c_str());
        lua_setfield(L, -2, "title");
        lua_pushstring(L, info.menu.c_str());
        lua_setfield(L, -2, "menu");
        lua_pushstring(L, info.kind == EToolKind::Plugin ? "plugin" : "lua");
        lua_setfield(L, -2, "kind");
        lua_pushboolean(L, info.open);
        lua_setfield(L, -2, "open");
        lua_pushstring(L, info.state);
        lua_setfield(L, -2, "state");
        if (!info.reason.empty())
        {
            lua_pushstring(L, info.reason.c_str());
            lua_setfield(L, -2, "reason");
        }
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

// imgui_editor_state([state]) -> "hidden" | "full" | "light" (the current one; a new state applies with the next frame)
//   state: a new state; false, "bad state" for an unknown name
int LuaEditorState(lua_State* L)
{
    if (lua_type(L, 1) == LUA_TSTRING)
    {
        if (!imgui_tools::RequestEditorState(lua_tostring(L, 1)))
        {
            lua_pushboolean(L, false);
            lua_pushstring(L, "bad state");
            return 2;
        }
    }
    lua_pushstring(L, imgui_tools::EditorStateName());
    return 1;
}

// imgui_available() -> bool: the console addon_imgui_tools is 1
int LuaAvailable(lua_State* L)
{
    lua_pushboolean(L, imgui_tools::Available());
    return 1;
}
} // namespace

void RunDeferred()
{
    if (!g_deferred || g_deferred->empty() || !IsMainThread())
        return;
    lua_State* L = MainState();
    // A function may defer another one: take the queue away first (the new ones wait for the next frame)
    xr_vector<Deferred> calls;
    calls.swap(*g_deferred);
    // The references of `calls` belong to this generation. A function that restarts the scripts makes a new state that
    // may get the address of the old one (L == g_state again): the generation tells them apart - a stale reference
    // would be called and unref'd in the registry of the new state (review 2026-11-18)
    const u32 generation = g_generation;
    for (Deferred& call : calls)
    {
        if (!L || L != g_state || generation != g_generation)
            break; // a new state took the place: the references died with the old one
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, call.ref);
        luaL_unref(L, LUA_REGISTRYINDEX, call.ref);
        const xr_string what = "ImGui tool '" + call.id + "' defer";
        ProtectedCall(L, 0, call.owner.c_str(), what.c_str(), nullptr);
        lua_settop(L, top);
        L = MainState(); // the function may have restarted the scripts
    }
}

void DropDeferred(pcstr owner)
{
    if (!g_deferred || !owner)
        return;
    lua_State* L = MainState();
    for (auto it = g_deferred->begin(); it != g_deferred->end();)
    {
        if (it->owner != owner)
        {
            ++it;
            continue;
        }
        if (L && L == g_state)
            luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
        it = g_deferred->erase(it);
    }
}
} // namespace gw::addons::imgui_lua

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CLuaImGuiToolsScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CLuaImGuiToolsScript::script_register(lua_State* L)
{
    using namespace gw::addons::imgui_lua;
    // A new Lua state: the tools of the old one go quietly (their references died with it; the open flags stay, on_load
    // registers the tools again and their windows open), their deferred functions too
    gw::addons::imgui_tools::RemoveByKey(gw::addons::imgui_tools::EToolKind::Lua, nullptr, false);
    if (g_deferred)
        g_deferred->clear();
    ++g_generation;
    g_state = L;

    // The table `ui`: the widgets (addon_imgui_lua_widgets.cpp), in the registry - draw gets it as its first argument,
    // there is no global (a widget cannot be called outside of a frame by mistake)
    lua_newtable(L);
    RegisterUiFunctions(L);
    g_ui_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // _gw_internal is shared with addon_event_bus.cpp, addon_lua_entry.cpp, ...: the first one creates it
    lua_getglobal(L, "_gw_internal");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_gw_internal");
    }
    const struct
    {
        pcstr name;
        lua_CFunction fn;
    } functions[] = {
        { "imgui_tool_register", &LuaRegister },
        { "imgui_tool_unregister", &LuaUnregister },
        { "imgui_tool_set_open", &LuaSetOpen },
        { "imgui_tool_is_open", &LuaIsOpen },
        { "imgui_tool_list", &LuaList },
        { "imgui_editor_state", &LuaEditorState },
        { "imgui_available", &LuaAvailable },
    };
    for (const auto& function : functions)
    {
        lua_pushcfunction(L, function.fn);
        lua_setfield(L, -2, function.name);
    }
    lua_pop(L, 1);
}
