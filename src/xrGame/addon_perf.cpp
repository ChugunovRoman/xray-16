#include "StdAfx.h"

#include "addon_perf.h"

#include "xrCore/ContentPatches.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrScriptEngine/script_engine.hpp"

#include <algorithm>

namespace gw::addons::perf
{
bool g_enabled = false;

namespace
{
enum class EKind : u8
{
    Lua,
    Native,
    Zone, // addon_perf.add from Lua
};
constexpr pcstr kKindNames[] = { "lua", "native", "zone" };

struct Row
{
    xr_string owner;
    EKind kind = EKind::Lua;
    xr_string name; // the event or the zone
    u64 calls = 0;
    double ms = 0.0;
    double max_ms = 0.0;
};

struct State
{
    // key: owner + '\n' + kind + '\n' + name
    xr_unordered_map<xr_string, Row> rows;
    // the source of a Lua function ("@<full path>") -> its owner: FileOwner walks the file index, once per script
    xr_unordered_map<xr_string, xr_string> lua_owners;
};
State* g_state = nullptr;

State& GetState()
{
    if (!g_state)
        g_state = xr_new<State>();
    return *g_state;
}

void Add(pcstr owner, EKind kind, pcstr name, double ms)
{
    State& state = GetState();
    xr_string key = owner;
    key += '\n';
    key += kKindNames[static_cast<u8>(kind)];
    key += '\n';
    key += name;
    auto [it, inserted] = state.rows.try_emplace(std::move(key));
    Row& row = it->second;
    if (inserted)
    {
        row.owner = owner;
        row.kind = kind;
        row.name = name;
    }
    ++row.calls;
    row.ms += ms;
    row.max_ms = std::max(row.max_ms, ms);
}

int LuaGetMethod(lua_State* L)
{
    lua_settop(L, 2);
    lua_gettable(L, 1);
    return 1;
}

// The owner of the Lua function on the top of the stack (pops it): the addon of its script file, "base" for a file of
// the game, "lua:?" for anything else (not a function, a C function, a chunk loaded from a string)
xr_string LuaOwner(lua_State* L)
{
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return "lua:?";
    }
    lua_Debug info{};
    if (!lua_getinfo(L, ">S", &info) || !info.source || info.source[0] != '@') // pops the function
        return "lua:?";
    State& state = GetState();
    auto [it, inserted] = state.lua_owners.try_emplace(info.source);
    if (inserted)
    {
        xray::content_patches::FileOwner(info.source + 1, it->second); // chunk name "@<full path>" (script_engine)
        if (it->second.empty())
            it->second = xray::content_patches::kBaseOwner;
    }
    return it->second;
}

xr_vector<const Row*> SortedRows()
{
    xr_vector<const Row*> rows;
    if (!g_state)
        return rows;
    rows.reserve(g_state->rows.size());
    for (const auto& [key, row] : g_state->rows)
        rows.push_back(&row);
    std::sort(rows.begin(), rows.end(), [](const Row* a, const Row* b) { return a->ms > b->ms; });
    return rows;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Lua API: global table addon_perf (registered by CAddonPerfScript below)
// ---------------------------------------------------------------------------------------------

namespace lua
{
// addon_perf.enable(on): switches the measuring on (the counters start from zero) or off
int Enable(lua_State* L)
{
    SetEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

// addon_perf.enabled(): true while measuring
int Enabled(lua_State* L)
{
    lua_pushboolean(L, g_enabled ? 1 : 0);
    return 1;
}

// addon_perf.reset(): the counters start from zero
int ResetCounters(lua_State*)
{
    Reset();
    return 0;
}

// addon_perf.now_ms(): a monotonic clock in milliseconds with a fraction (the timer of the perf metrics), for measuring
// a zone in Lua: local t = addon_perf.now_ms() ... addon_perf.add(owner, zone, addon_perf.now_ms() - t)
int NowMs(lua_State* L)
{
    lua_pushnumber(L, xray::perf::TicksToMilliseconds(xray::perf::Ticks()));
    return 1;
}

// addon_perf.add(owner, zone, ms): one measured call of a zone of the owner (an addon id). Counted only while
// measuring; bad arguments are ignored (logged once)
int AddZone(lua_State* L)
{
    if (!g_enabled)
        return 0;
    const pcstr owner = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const pcstr zone = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    const bool ms_ok = lua_type(L, 3) == LUA_TNUMBER && lua_tonumber(L, 3) >= 0;
    if (!owner || !*owner || !zone || !*zone || !ms_ok)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Msg("! [addon_perf] addon_perf.add(owner, zone, ms): owner and zone must be non-empty strings, ms a number "
                ">= 0 (reported once)");
        }
        return 0;
    }
    Add(owner, EKind::Zone, zone, lua_tonumber(L, 3));
    return 0;
}

// addon_perf.report(): an array of rows, the slowest first: {owner, kind = "lua" | "native" | "zone", name (the event
// or the zone), calls, ms (the sum), max_ms (the slowest call)}
int Report(lua_State* L)
{
    const xr_vector<const Row*> rows = SortedRows();
    lua_createtable(L, static_cast<int>(rows.size()), 0);
    int index = 0;
    for (const Row* row : rows)
    {
        lua_createtable(L, 0, 6);
        lua_pushstring(L, row->owner.c_str());
        lua_setfield(L, -2, "owner");
        lua_pushstring(L, kKindNames[static_cast<u8>(row->kind)]);
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, row->name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, static_cast<lua_Number>(row->calls));
        lua_setfield(L, -2, "calls");
        lua_pushnumber(L, row->ms);
        lua_setfield(L, -2, "ms");
        lua_pushnumber(L, row->max_ms);
        lua_setfield(L, -2, "max_ms");
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}
} // namespace lua

void SetEnabled(bool on)
{
    if (on && !g_enabled)
        Reset();
    g_enabled = on;
    Msg("* [addon_perf] the time of the addons on the event bus is %s", on ? "measured" : "not measured");
}

void Reset()
{
    if (g_state)
        g_state->rows.clear(); // the owners of the scripts stay: the files did not move
}

void NoteNative(pcstr addon_id, pcstr event, u64 ticks)
{
    Add(addon_id && *addon_id ? addon_id : "plugin:?", EKind::Native, event, xray::perf::TicksToMilliseconds(ticks));
}

void NoteLua(lua_State* L, int index, bool object, pcstr event, u64 ticks)
{
    const double ms = xray::perf::TicksToMilliseconds(ticks);
    if (!index || !lua_checkstack(L, 4))
    {
        Add("lua:?", EKind::Lua, event, ms);
        return;
    }
    const int top = lua_gettop(L);
    if (object)
    {
        // The method the bus called: object[event] (a protected call: the __index of a userdata may fail)
        lua_pushcfunction(L, LuaGetMethod);
        lua_pushvalue(L, index);
        lua_pushstring(L, event);
        if (lua_pcall(L, 2, 1, 0) != 0)
        {
            lua_pop(L, 1);
            lua_pushnil(L); // in place of the error message
        }
    }
    else
        lua_pushvalue(L, index);
    const xr_string owner = LuaOwner(L); // pops the function
    lua_settop(L, top);
    Add(owner.c_str(), EKind::Lua, event, ms);
}

void PrintReport(u32 top)
{
    const xr_vector<const Row*> rows = SortedRows();
    if (rows.empty())
    {
        Msg("* [addon_perf] no data%s", g_enabled ? " yet" : ": switch the measuring on with addon_perf 1");
        return;
    }
    struct OwnerTotal
    {
        xr_string owner;
        u64 calls = 0;
        double ms[3] = {};
        double Total() const { return ms[0] + ms[1] + ms[2]; }
    };
    xr_vector<OwnerTotal> owners;
    for (const Row* row : rows)
    {
        auto it = std::find_if(
            owners.begin(), owners.end(), [row](const OwnerTotal& total) { return total.owner == row->owner; });
        if (it == owners.end())
        {
            owners.emplace_back();
            it = owners.end() - 1;
            it->owner = row->owner;
        }
        it->calls += row->calls;
        it->ms[static_cast<u8>(row->kind)] += row->ms;
    }
    std::sort(owners.begin(), owners.end(),
        [](const OwnerTotal& a, const OwnerTotal& b) { return a.Total() > b.Total(); });
    Msg("* [addon_perf] by owner, ms since addon_perf 1 / reset: owner | total | lua | native | zone | calls");
    for (const OwnerTotal& total : owners)
    {
        Msg("  %-32.64s %10.2f %10.2f %10.2f %10.2f %10llu", total.owner.c_str(), total.Total(), total.ms[0],
            total.ms[1], total.ms[2], static_cast<unsigned long long>(total.calls));
    }
    Msg("* [addon_perf] the %u slowest rows: owner | kind | event or zone | ms | calls | avg ms | max ms", top);
    u32 printed = 0;
    for (const Row* row : rows)
    {
        if (printed++ >= top)
            break;
        Msg("  %-32.64s %-6s %-40.80s %10.2f %10llu %8.3f %8.3f", row->owner.c_str(),
            kKindNames[static_cast<u8>(row->kind)], row->name.c_str(), row->ms,
            static_cast<unsigned long long>(row->calls),
            row->calls ? row->ms / static_cast<double>(row->calls) : 0.0, row->max_ms);
    }
}
} // namespace gw::addons::perf

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in a namespace: the export node must have a normal, always-emitted definition.
struct CAddonPerfScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CAddonPerfScript::script_register(lua_State* L)
{
    namespace lua = gw::addons::perf::lua;
    const luaL_Reg functions[] = {
        { "enable", &lua::Enable },
        { "enabled", &lua::Enabled },
        { "reset", &lua::ResetCounters },
        { "now_ms", &lua::NowMs },
        { "add", &lua::AddZone },
        { "report", &lua::Report },
    };
    lua_newtable(L);
    for (const luaL_Reg& function : functions)
    {
        lua_pushcfunction(L, function.func);
        lua_setfield(L, -2, function.name);
    }
    lua_setglobal(L, "addon_perf");
}
