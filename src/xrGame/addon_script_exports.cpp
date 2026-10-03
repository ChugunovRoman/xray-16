#include "StdAfx.h"

// Plugin API, group "script" (second half): functions of a plugin that Lua calls.
//  - A plugin exports a function under "<addon id>.<name>"; Lua calls it as plugins.call("<addon id>.<name>", ...)
//    and gets its result back. The arguments and the result convert as event arguments do: a game object becomes
//    GWP_T_OBJECT (and back), a table with the keys 1..n becomes GWP_T_ARRAY (and an array a new table), other
//    tables and functions have no native form (GWP_T_LUA_REF).
//  - The function returns a GwpResult: GWP_OK gives Lua the result, any other code gives nil and the code as text
//    ("not_found", "invalid_argument", ...), the usual nil, err pair of Lua.
//  - The call is guarded like a handler of the event bus: a crash removes every export of the plugin and the call
//    returns nil, "crashed".
//  - plugins.version(addon_id): the Plugin API version the plugin of the addon was built with.
// This is the other direction of script_call: together they let a Lua facade (state_mgr.set_state and the like)
// stay in Lua while its body moves to the plugin.
// Docs: wiki/doc/plugins/api/script.md

#include "addon_event_bus.h"
#include "addon_host.h"

#include "xrScriptEngine/script_engine.hpp"

namespace gw::addons
{
namespace
{
constexpr u32 kMaxArgs = 16;

struct Export
{
    const GwpPlugin* plugin = nullptr;
    GwpExportFn fn = nullptr;
    void* user = nullptr;
    u64 calls = 0; // counters of script_call_list
    u64 ticks = 0; // CPU::QPC ticks inside the function, nested plugins.call of it included
    bool warned_invalid_result = false; // GWP_OK with a result that fails CheckValue: logged once per export
};

xr_map<xr_string, Export>* g_exports = nullptr; // "<addon id>.<name>" -> export
xr_set<xr_string>* g_missing_logged = nullptr;   // names already reported as missing: a facade calls every frame
lua_State* g_calling_thread = nullptr;           // the Lua thread of the plugins.call in progress (script_call uses it)
u64 g_missing_calls = 0;       // plugins.call of a name that is not exported (script_call_list)
u64 g_erase_serial = 0;        // bumped whenever exports are erased: a reference to an entry is stale after it
u32 g_export_stats_since_ms = 0; // Device.dwTimeGlobal of the last reset of the counters

bool ValidName(pcstr name)
{
    if (!name || !name[0] || xr_strlen(name) > 64)
        return false;
    for (pcstr p = name; *p; ++p)
    {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

GwpResult GWP_CALL ApiScriptExport(const GwpPlugin* self, const char* name, GwpExportFn fn, void* user)
{
    if (!IsMainThread())
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !fn || !ValidName(name))
    {
        Msg("! [plugin:%s] script_export '%s': the name must be 1-64 characters a-z 0-9 _, and a function is needed",
            PluginAddonId(self), name ? name : "");
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    if (!g_exports)
        g_exports = xr_new<xr_map<xr_string, Export>>();
    const xr_string key = xr_string(PluginAddonId(self)) + "." + name;
    Export& entry = (*g_exports)[key];
    entry.plugin = self;
    entry.fn = fn;
    entry.user = user;
    return GWP_OK;
}

// The text Lua gets for a GwpResult other than GWP_OK (the second value of plugins.call): the name of the code
// without the GWP_ERROR_ prefix, lowercase; "error" for GWP_ERROR and for a code this engine does not know.
pcstr ExportResultText(GwpResult code)
{
    switch (code)
    {
    case GWP_OK: return "ok";
    case GWP_ERROR_VERSION_MISMATCH: return "version_mismatch";
    case GWP_ERROR_NOT_MAIN_THREAD: return "not_main_thread";
    case GWP_ERROR_INVALID_ARGUMENT: return "invalid_argument";
    case GWP_ERROR_ACCESS_DENIED: return "access_denied";
    case GWP_ERROR_NOT_FOUND: return "not_found";
    case GWP_ERROR_NOT_SUPPORTED: return "not_supported";
    case GWP_ERROR_INVALID_STATE: return "invalid_state";
    case GWP_ERROR_CRASHED: return "crashed";
    default: return "error"; // GWP_ERROR and codes of a later version
    }
}

// nil, err: the failure pair of plugins.call
int ExportPushFailure(lua_State* L, GwpResult code)
{
    lua_pushnil(L);
    lua_pushstring(L, ExportResultText(code));
    return 2;
}

struct ExportCall
{
    const Export* entry;
    uint32_t argc;
    const GwpValue* argv;
    GwpValue* result;
    GwpResult code;            // what the function returned
};

// Only the plugin function runs under the guard: the copy of its result is made by LuaCall after it, so an engine
// failure there (bad_alloc) is not taken for a crash of the plugin.
void ExportThunk(void* context)
{
    ExportCall& call = *static_cast<ExportCall*>(context);
    call.code = call.entry->fn(call.entry->user, call.argc, call.argv, call.result);
}

// plugins.call(name, ...): the result of the function; nil, err when there is no such export ("not_found"), the
// name is not a string ("invalid_argument"), the function returned a code other than GWP_OK (its text) or it
// crashed ("crashed").
int LuaCall(lua_State* L)
{
    const pcstr name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const auto it = name && g_exports ? g_exports->find(name) : decltype(g_exports->end()){};
    if (!name || !g_exports || it == g_exports->end())
    {
        ++g_missing_calls;
        if (!g_missing_logged)
            g_missing_logged = xr_new<xr_set<xr_string>>();
        if (g_missing_logged->insert(name ? name : "").second)
        {
            Msg("! [plugins] plugins.call: no exported function '%s' (logged once)",
                name ? name : "(not a string)");
        }
        return ExportPushFailure(L, name ? GWP_ERROR_NOT_FOUND : GWP_ERROR_INVALID_ARGUMENT);
    }
    const int top = lua_gettop(L);
    const uint32_t argc = static_cast<uint32_t>(std::min(top - 1, static_cast<int>(kMaxArgs)));
    GwpValue argv[kMaxArgs];
    // Strings of the arguments point into the stack, alive until the function returns. Arrays are deep copies in the
    // arena: the strings of their items would point into the Lua tables, and the function may call Lua
    // (script_call), which may change such a table and free them
    events::ValueArena arena;
    for (uint32_t i = 0; i < argc; ++i)
    {
        argv[i] = events::LuaToValue(L, static_cast<int>(i) + 2, &arena);
        if (argv[i].type == GWP_T_ARRAY)
            argv[i] = arena.Copy(argv[i]); // the converted items stay in the arena, unused
    }

    Export& entry = it->second;
    GwpValue result = events::Nil();
    ExportCall call{ &entry, argc, argv, &result, GWP_ERROR };
    ++entry.calls;
    // Kept before the call: a nested plugins.call of the same plugin that crashes erases `entry`
    const GwpPlugin* const plugin = entry.plugin;
    lua_State* const previous_thread = g_calling_thread;
    g_calling_thread = L;
    const u64 serial = g_erase_serial;
    ZoneScopedN("plugin/plugins.call"); // Tracy: the plugin function Lua calls, text = its name
    ZoneTextF("%s", name);
    const u64 start = CPU::QPC();
    const bool ok = events::CallPluginGuarded(reinterpret_cast<const void*>(entry.fn), name, &ExportThunk, &call);
    const u64 ticks = CPU::QPC() - start;
    g_calling_thread = previous_thread;
    // A crash of a nested plugins.call of the same plugin (or this one) erases `entry`: then its time is dropped
    const bool entry_alive = serial == g_erase_serial;
    if (entry_alive)
        entry.ticks += ticks;
    if (!ok)
    {
        string256 what;
        xr_sprintf(what, "its exported function '%s'", name);
        OnPluginCrashed(plugin, what); // the whole plugin stops, its exports with it
        RemovePluginExports(plugin);   // and for sure (`entry` is gone from here on)
        return ExportPushFailure(L, GWP_ERROR_CRASHED);
    }
    if (call.code != GWP_OK)
        return ExportPushFailure(L, call.code);
    // An invalid value (CheckValue) becomes nil instead of being copied, logged once per export
    if (events::CheckValue(result) != GWP_OK)
    {
        if (!entry_alive || !entry.warned_invalid_result)
        {
            if (entry_alive)
                entry.warned_invalid_result = true;
            Msg("! [plugin:%s] plugins.call '%s': GWP_OK with an invalid result value (reserved not 0, unknown type, "
                "a string or an array without data or over its limits, more than 65536 values or 16 MiB of strings), "
                "Lua gets nil (logged once per function)",
                PluginAddonId(plugin), name);
        }
        lua_pushnil(L);
        return 1;
    }
    // The copy is made right after the call (gwp_api.h, GwpExportFn): strings, bytes and the items of arrays, so Lua
    // gets the result even when pushing it runs Lua code (a game object, the GC) that changes the plugin's buffers.
    // It does not make a bad pointer safe: that fault is in the engine, outside the module of the plugin.
    const events::OwnedValue kept(result);
    events::PushLuaValue(L, kept.view());
    return 1;
}

// plugins.has(name): true when the function is exported now (a Lua facade checks it once to choose its path).
int LuaHas(lua_State* L)
{
    const pcstr name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    lua_pushboolean(L, name && g_exports && g_exports->count(name) ? 1 : 0);
    return 1;
}

// plugins.version(addon_id): "MAJOR.MINOR.PATCH" of the Plugin API the plugin of the addon was built with
// (GwpPluginDesc::api_built), nil when the addon is unknown, its plugin is not loaded now or did not report it.
int LuaVersion(lua_State* L)
{
    const pcstr addon_id = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const u32 version = addon_id ? PluginApiBuilt(addon_id) : 0u;
    if (!version)
    {
        lua_pushnil(L);
        return 1;
    }
    string64 text; // the layout of GWP_MAKE_VERSION
    xr_sprintf(text, "%u.%u.%u", (version >> 22) & 0x3FFu, (version >> 12) & 0x3FFu, version & 0xFFFu);
    lua_pushstring(L, text);
    return 1;
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CPluginExportsScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CPluginExportsScript::script_register(lua_State* L)
{
    lua_newtable(L);
    lua_pushcfunction(L, &LuaCall);
    lua_setfield(L, -2, "call");
    lua_pushcfunction(L, &LuaHas);
    lua_setfield(L, -2, "has");
    lua_pushcfunction(L, &LuaVersion);
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "plugins");
}

void FillExportsApi(GwpEngineApi& api) { api.script_export = &ApiScriptExport; }

void RemovePluginExports(const GwpPlugin* plugin)
{
    if (!g_exports)
        return;
    ++g_erase_serial;
    for (auto e = g_exports->begin(); e != g_exports->end();)
        e = e->second.plugin == plugin ? g_exports->erase(e) : std::next(e);
}

void PrintExportCallList(bool reset)
{
    const u32 now = Device.dwTimeGlobal;
    const float seconds = std::max(0.001f, (now - g_export_stats_since_ms) / 1000.f);
    xr_vector<std::pair<const xr_string*, const Export*>> rows;
    if (g_exports)
    {
        for (const auto& [name, entry] : *g_exports)
        {
            if (entry.calls)
                rows.emplace_back(&name, &entry);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second->ticks != b.second->ticks ? a.second->ticks > b.second->ticks : a.second->calls > b.second->calls;
    });
    Msg("- [script] plugins.call, Lua -> plugin: %u function(s) over %.1f s, %llu call(s) of names not exported",
        static_cast<u32>(rows.size()), seconds, g_missing_calls);
    Msg("-   %10s %9s %10s %8s  %s", "calls", "calls/s", "total_ms", "avg_us", "function");
    const double ms_per_tick = 1000.0 / static_cast<double>(CPU::qpc_freq);
    for (const auto& [name, entry] : rows)
    {
        const double total_ms = entry->ticks * ms_per_tick;
        Msg("-   %10llu %9.1f %10.2f %8.2f  %s", static_cast<unsigned long long>(entry->calls),
            entry->calls / seconds, total_ms, total_ms * 1000.0 / static_cast<double>(entry->calls), name->c_str());
    }
    if (!reset)
        return;
    if (g_exports)
    {
        for (auto& [name, entry] : *g_exports)
            entry.calls = entry.ticks = 0;
    }
    g_missing_calls = 0;
    g_export_stats_since_ms = now;
}

void ShutdownExports()
{
    xr_delete(g_exports);
    xr_delete(g_missing_logged);
    g_calling_thread = nullptr;
    g_missing_calls = 0;
    g_export_stats_since_ms = 0;
}

lua_State* CallingLuaThread() { return g_calling_thread; }
} // namespace gw::addons
