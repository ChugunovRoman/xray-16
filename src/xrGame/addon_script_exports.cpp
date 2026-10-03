#include "StdAfx.h"

// Plugin API, group "script" (second half): functions of a plugin that Lua calls.
//  - A plugin exports a function under "<addon id>.<name>"; Lua calls it as plugins.call("<addon id>.<name>", ...)
//    and gets its result back. The arguments and the result convert as event arguments do: a game object becomes
//    GWP_T_OBJECT (and back), tables and functions have no native form (GWP_T_LUA_REF).
//  - The call is guarded like a handler of the event bus: a crash removes every export of the plugin and the call
//    returns nil.
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

struct ExportCall
{
    const Export* entry;
    uint32_t argc;
    const GwpValue* argv;
    GwpValue* result;
    xr_string* text; // the copy of a string result: Lua gets it after the plugin's buffer may have changed
};

void ExportThunk(void* context)
{
    const ExportCall& call = *static_cast<const ExportCall*>(context);
    call.entry->fn(call.entry->user, call.argc, call.argv, call.result);
    // The copy is made right after the call (gwp_api.h, GwpExportFn). It does not make a bad pointer safe: the fault
    // would be in the copy code (CRT, xrGame), outside the module of the plugin, which the guard does not catch
    if (call.result->type == GWP_T_STRING)
    {
        const GwpString& s = call.result->u.s;
        call.text->assign(s.ptr ? s.ptr : "", s.ptr ? s.len : 0);
        call.result->u.s.ptr = call.text->c_str();
        call.result->u.s.len = static_cast<uint32_t>(call.text->size());
    }
}

// plugins.call(name, ...): the result of the function, nil when there is no such export or it failed.
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
        lua_pushnil(L);
        return 1;
    }
    const int top = lua_gettop(L);
    const uint32_t argc = static_cast<uint32_t>(std::min(top - 1, static_cast<int>(kMaxArgs)));
    GwpValue argv[kMaxArgs];
    for (uint32_t i = 0; i < argc; ++i)
        argv[i] = events::LuaToValue(L, static_cast<int>(i) + 2); // strings point into the stack: alive here

    Export& entry = it->second;
    GwpValue result = events::Nil();
    xr_string text;
    const ExportCall call{ &entry, argc, argv, &result, &text };
    ++entry.calls;
    // Kept before the call: a nested plugins.call of the same plugin that crashes erases `entry`
    const GwpPlugin* const plugin = entry.plugin;
    lua_State* const previous_thread = g_calling_thread;
    g_calling_thread = L;
    const u64 serial = g_erase_serial;
    ZoneScopedN("plugin/plugins.call"); // Tracy: the plugin function Lua calls, text = its name
    ZoneTextF("%s", name);
    const u64 start = CPU::QPC();
    const bool ok = events::CallPluginGuarded(
        reinterpret_cast<const void*>(entry.fn), name, &ExportThunk, const_cast<ExportCall*>(&call));
    const u64 ticks = CPU::QPC() - start;
    g_calling_thread = previous_thread;
    // A crash of a nested plugins.call of the same plugin (or this one) erases `entry`: then its time is dropped
    if (serial == g_erase_serial)
        entry.ticks += ticks;
    if (!ok)
    {
        string256 what;
        xr_sprintf(what, "its exported function '%s'", name);
        OnPluginCrashed(plugin, what); // the whole plugin stops, its exports with it
        RemovePluginExports(plugin);   // and for sure (`entry` is gone from here on)
        lua_pushnil(L);
        return 1;
    }
    events::PushLuaValue(L, result);
    return 1;
}

// plugins.has(name): true when the function is exported now (a Lua facade checks it once to choose its path).
int LuaHas(lua_State* L)
{
    const pcstr name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    lua_pushboolean(L, name && g_exports && g_exports->count(name) ? 1 : 0);
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
