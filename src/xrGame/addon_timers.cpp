#include "StdAfx.h"

#include "addon_timers.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"

#include "xrScriptEngine/script_engine.hpp"

#include <algorithm>
#include <cmath>

namespace gw::addons::timers
{
namespace
{
struct Timer
{
    xr_string owner; // addon id (plugin timers) or the part of a Lua key before '/'
    xr_string name;
    xr_string event;
    GwpEventId event_id = GWP_INVALID_EVENT_ID;
    u64 due = 0;    // ms of its time base: game time, or g_level_ms for GWP_TIMER_REAL_TIME
    u64 period = 0; // ms; 0 = one-shot
    u32 flags = 0;  // GWP_TIMER_* and kLuaOwned
    u64 serial = 0; // new on every start: a timer replaced from a handler is not fired by the old scan
};

// Key "<addon id>/<timer name>". xr_map: stable order for timer_list and the save.
using Timers = xr_map<xr_string, Timer>;
Timers* g_timers = nullptr;

// Level time: real milliseconds of the frames that ran Update. Only differences matter (saves keep remaining time).
u64 g_level_ms = 0;
u64 g_serial = 0;
bool g_loading_game_time = false; // see SetLoadingGameTime

// One frame of level time. Device.dwTimeDelta is not clamped (fTimeDelta is, to 0.1 s): the first frame after a
// load, an alt-tab or a breakpoint brings seconds, which must not count as unpaused play.
constexpr u32 kMaxFrameMs = 100;

struct Due
{
    xr_string key; // a copy: handlers of earlier timers may erase the node
    u64 serial;
    u64 due;
};
xr_vector<Due>* g_due = nullptr; // scratch list of Update, kept to avoid an allocation every frame

constexpr size_t kMaxNameLength = 256;
constexpr u32 kKnownFlags = GWP_TIMER_REAL_TIME | GWP_TIMER_PERSISTENT;
// Internal flag: started from Lua (timer_bus). Such a timer fires without a loaded plugin of its owner.
constexpr u32 kLuaOwned = 0x80000000u;
constexpr double kMaxSeconds = 100.0 * 365.0 * 24.0 * 3600.0; // 100 years: guards the ms conversion
constexpr u64 kMaxMs = static_cast<u64>(kMaxSeconds * 1000.0);
constexpr u64 kMaxGameTimeMs = u64(-1) / 4; // an absolute game time from a save above this is corrupted
// Chunk of the ALife save stream (plugin save data is 0x0100, data bus 0x0101).
constexpr u32 kSaveChunkId = 0x0102;
constexpr u32 kSaveFormat = 1;

Timers& GetTimers()
{
    if (!g_timers)
        g_timers = xr_new<Timers>();
    return *g_timers;
}

// A game exists: ALife is created and reloaded (new game or load). Before CALifeSimulatorBase::reload the time
// manager does not exist yet, though ai().get_alife() is already set (Lua on_game_start runs in that window).
bool HasGame() { return ai().get_alife() && ai().alife().initialized(); }

// Game time of ALife in ms (what Lua game.get_game_time() shows). false without a single player game.
bool GameTimeMs(u64& out)
{
    if (!HasGame())
        return false;
    out = ai().alife().time_manager().game_time();
    return true;
}

// Timers of both time bases live inside a game only: a timer started before it would outlive the game reset.
bool NowMs(u32 flags, u64& out)
{
    if (!(flags & GWP_TIMER_REAL_TIME))
        return GameTimeMs(out);
    if (!HasGame())
        return false;
    out = g_level_ms;
    return true;
}

bool SecondsToMs(double seconds, u64& out)
{
    if (!(seconds >= 0.0 && seconds <= kMaxSeconds)) // also false for NaN
        return false;
    out = static_cast<u64>(std::llround(seconds * 1000.0));
    return true;
}

// Period: 0 = one-shot; any period > 0 repeats, so a sub-millisecond one becomes 1 ms instead of 0.
bool PeriodToMs(double seconds, u64& out)
{
    if (!SecondsToMs(seconds, out))
        return false;
    if (seconds > 0.0 && out == 0)
        out = 1;
    return true;
}

// Events of the engine itself (level_on_frame, actor_on_spawn, ...) have their own arguments: a timer must not
// send them. Built-in events are registered first, so their ids are below EBuiltin::Count_.
bool IsBuiltinEvent(pcstr event)
{
    return events::Intern(event) < static_cast<GwpEventId>(events::EBuiltin::Count_);
}

bool IsValidName(pcstr name)
{
    if (!name)
        return false;
    const size_t length = xr_strlen(name);
    return length > 0 && length <= kMaxNameLength;
}

xr_string KeyOf(pcstr owner, pcstr name)
{
    xr_string key = owner;
    key += '/';
    key += name;
    return key;
}

// Fires when its time comes: Lua timers always, plugin timers only while the plugin of the owner is loaded.
bool CanFire(const Timer& timer) { return (timer.flags & kLuaOwned) || IsPluginLoaded(timer.owner.c_str()); }

// The caller has validated the arguments. false when there is no game.
bool Start(pcstr owner, pcstr name, pcstr event, u64 delay, u64 period, u32 flags)
{
    if (g_loading_game_time && !(flags & GWP_TIMER_REAL_TIME))
    {
        Msg("! [timers] %s/%s: a game-time timer cannot be started while a save is loading (the game time of the "
            "save is not read yet); start it from alife_on_load or actor_on_spawn", owner, name);
        return false;
    }
    u64 now = 0;
    if (!NowMs(flags, now))
        return false; // no game: timers live inside a game only
    Timers& timers = GetTimers();
    const xr_string key = KeyOf(owner, name);
    const auto existing = timers.find(key);
    if (existing != timers.end() && ((existing->second.flags ^ flags) & kLuaOwned))
    {
        Msg("~ [timers] %s replaces a timer of %s: the same key started from %s", key.c_str(),
            existing->second.flags & kLuaOwned ? "Lua" : "a plugin", flags & kLuaOwned ? "Lua" : "a plugin");
    }
    Timer& timer = timers[key];
    timer.owner = owner;
    timer.name = name;
    timer.event = event;
    timer.event_id = events::Intern(event);
    // The event belongs to whoever fires it; declaring it here keeps the bus from warning about it.
    events::Declare(event);
    timer.due = now + delay;
    timer.period = period;
    timer.flags = flags;
    timer.serial = ++g_serial;
    return true;
}

bool Stop(const xr_string& key) { return g_timers && g_timers->erase(key) > 0; }

// Seconds left in the timer's own time base; false when there is no such timer (or no game for a game timer).
bool Remaining(const xr_string& key, double& out)
{
    out = 0.0;
    if (!g_timers)
        return false;
    const auto it = g_timers->find(key);
    u64 now = 0;
    if (it == g_timers->end() || !NowMs(it->second.flags, now))
        return false;
    out = it->second.due > now ? static_cast<double>(it->second.due - now) / 1000.0 : 0.0;
    return true;
}

GwpResult CheckCall(const GwpPlugin* self, pcstr name, pcstr function)
{
    if (!IsMainThread())
    {
        Msg("! [plugin:%s] %s called outside the main thread, ignored", PluginAddonId(self), function);
        return GWP_ERROR_NOT_MAIN_THREAD;
    }
    if (!self || !IsValidName(name))
        return GWP_ERROR_INVALID_ARGUMENT;
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group timers)
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiTimerStart(const GwpPlugin* self, const char* name, const char* event, double delay_seconds,
    double period_seconds, uint32_t flags)
{
    const GwpResult check = CheckCall(self, name, "timer_start");
    if (check != GWP_OK)
        return check;
    u64 delay = 0, period = 0;
    if (!IsValidName(event) || IsBuiltinEvent(event) || (flags & ~kKnownFlags) || !SecondsToMs(delay_seconds, delay) ||
        !PeriodToMs(period_seconds, period))
    {
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    return Start(PluginAddonId(self), name, event, delay, period, flags) ? GWP_OK : GWP_ERROR;
}

GwpResult GWP_CALL ApiTimerStop(const GwpPlugin* self, const char* name)
{
    const GwpResult check = CheckCall(self, name, "timer_stop");
    if (check != GWP_OK)
        return check;
    return Stop(KeyOf(PluginAddonId(self), name)) ? GWP_OK : GWP_ERROR;
}

GwpResult GWP_CALL ApiTimerRemaining(const GwpPlugin* self, const char* name, double* out_seconds)
{
    if (out_seconds)
        *out_seconds = 0.0;
    const GwpResult check = CheckCall(self, name, "timer_remaining");
    if (check != GWP_OK)
        return check;
    double left = 0.0;
    if (!Remaining(KeyOf(PluginAddonId(self), name), left))
        return GWP_ERROR;
    if (out_seconds)
        *out_seconds = left;
    return GWP_OK;
}

uint64_t GWP_CALL ApiGameTimeMs()
{
    u64 now = 0;
    return IsMainThread() && GameTimeMs(now) ? now : 0;
}

double GWP_CALL ApiGameTimeFactor()
{
    if (!IsMainThread() || !HasGame())
        return 0.0;
    return ai().alife().time_manager().time_factor();
}

pcstr BaseName(u32 flags) { return flags & GWP_TIMER_REAL_TIME ? "level" : "game"; }

// ---------------------------------------------------------------------------------------------
// Lua API: global table timer_bus
// ---------------------------------------------------------------------------------------------

// Key argument "<owner>/<name>"; logs and returns false when it is not valid. owner and name get the two parts.
bool LuaKey(lua_State* L, int index, pcstr function, xr_string& owner, xr_string& name)
{
    const char* key = lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : nullptr;
    const char* slash = key ? strchr(key, '/') : nullptr;
    if (slash && slash != key && IsValidName(slash + 1) && static_cast<size_t>(slash - key) <= kMaxNameLength)
    {
        owner.assign(key, slash);
        name = slash + 1;
        return true;
    }
    Msg("! [timers] timer_bus.%s: the key must be a string \"<owner>/<name>\", got %s", function,
        key ? key : lua_typename(L, lua_type(L, index)));
    return false;
}

// Optional number argument in seconds (nil = 0); logs and returns false when it is not a valid time.
bool LuaSeconds(lua_State* L, int index, pcstr function, pcstr what, u64& out)
{
    out = 0;
    if (lua_isnoneornil(L, index))
        return true;
    if (lua_type(L, index) == LUA_TNUMBER &&
        (index == 4 ? PeriodToMs(lua_tonumber(L, index), out) : SecondsToMs(lua_tonumber(L, index), out)))
        return true;
    Msg("! [timers] timer_bus.%s: %s must be a number of seconds from 0 to 100 years", function, what);
    return false;
}

// timer_bus.start(key, event, delay [, period [, flags]]): true on success; false without a game or on bad
// arguments (logged). flags: timer_bus.REAL_TIME, timer_bus.PERSISTENT (sum them or use bit.bor).
int LuaStart(lua_State* L)
{
    xr_string owner, name;
    u64 delay = 0, period = 0;
    const char* event = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
    // Not a number (a string, a boolean) is an error, as for delay and period: -1 fails the check below.
    const lua_Number flags = lua_isnoneornil(L, 5) ? 0 : lua_type(L, 5) == LUA_TNUMBER ? lua_tonumber(L, 5) : -1;
    bool ok = LuaKey(L, 1, "start", owner, name);
    if (ok && !IsValidName(event))
    {
        Msg("! [timers] timer_bus.start(%s/%s): the event name must be a non-empty string", owner.c_str(),
            name.c_str());
        ok = false;
    }
    if (ok && IsBuiltinEvent(event))
    {
        Msg("! [timers] timer_bus.start(%s/%s): '%s' is an event of the engine, use an own event", owner.c_str(),
            name.c_str(), event);
        ok = false;
    }
    ok = ok && LuaSeconds(L, 3, "start", "delay", delay) && LuaSeconds(L, 4, "start", "period", period);
    if (ok && !(flags >= 0 && flags == std::floor(flags) && flags <= kKnownFlags))
    {
        Msg("! [timers] timer_bus.start(%s/%s): unknown flags %g", owner.c_str(), name.c_str(), flags);
        ok = false;
    }
    ok = ok && Start(owner.c_str(), name.c_str(), event, delay, period, static_cast<u32>(flags) | kLuaOwned);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// timer_bus.stop(key): true when the timer existed.
int LuaStop(lua_State* L)
{
    xr_string owner, name;
    lua_pushboolean(L, LuaKey(L, 1, "stop", owner, name) && Stop(KeyOf(owner.c_str(), name.c_str())) ? 1 : 0);
    return 1;
}

// timer_bus.remaining(key): seconds left in the timer's time base, nil when there is no such timer.
int LuaRemaining(lua_State* L)
{
    xr_string owner, name;
    double left = 0.0;
    if (LuaKey(L, 1, "remaining", owner, name) && Remaining(KeyOf(owner.c_str(), name.c_str()), left))
        lua_pushnumber(L, left);
    else
        lua_pushnil(L);
    return 1;
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CTimerBusScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CTimerBusScript::script_register(lua_State* L)
{
    const luaL_Reg functions[] = {
        { "start", &LuaStart },
        { "stop", &LuaStop },
        { "remaining", &LuaRemaining },
    };
    lua_newtable(L);
    for (const luaL_Reg& function : functions)
    {
        lua_pushcfunction(L, function.func);
        lua_setfield(L, -2, function.name);
    }
    lua_pushinteger(L, GWP_TIMER_REAL_TIME);
    lua_setfield(L, -2, "REAL_TIME");
    lua_pushinteger(L, GWP_TIMER_PERSISTENT);
    lua_setfield(L, -2, "PERSISTENT");
    lua_setglobal(L, "timer_bus");
}

void FillEngineApi(GwpEngineApi& api)
{
    api.timer_start = &ApiTimerStart;
    api.timer_stop = &ApiTimerStop;
    api.timer_remaining = &ApiTimerRemaining;
    api.game_time_ms = &ApiGameTimeMs;
    api.game_time_factor = &ApiGameTimeFactor;
}

void SetLoadingGameTime(bool loading) { g_loading_game_time = loading; }

void Update(u32 dt_ms)
{
    g_level_ms += std::min(dt_ms, kMaxFrameMs);
    if (!g_timers || g_timers->empty())
        return;
    u64 game_now = 0;
    const bool has_game = GameTimeMs(game_now);
    if (!has_game)
        return; // no game (ALife already destroyed): timers of both bases wait, as they cannot be started either

    if (!g_due)
        g_due = xr_new<xr_vector<Due>>();
    xr_vector<Due>& due = *g_due;
    due.clear();
    for (const auto& [key, timer] : *g_timers)
    {
        const bool real = (timer.flags & GWP_TIMER_REAL_TIME) != 0;
        if ((real ? g_level_ms : game_now) >= timer.due && CanFire(timer))
            due.push_back({ key, timer.serial, timer.due });
    }
    if (due.empty())
        return;
    // Earliest first; equal times in key order (the map order), so the order does not depend on the run.
    std::stable_sort(due.begin(), due.end(), [](const Due& a, const Due& b) { return a.due < b.due; });

    for (const Due& item : due)
    {
        // Handlers of the previous timers may have stopped or restarted this one: find it again by key and serial.
        const auto it = g_timers->find(item.key);
        if (it == g_timers->end() || it->second.serial != item.serial)
            continue;
        Timer& timer = it->second;
        const xr_string name = timer.name; // a copy: the handler may replace or stop the timer
        const GwpEventId event_id = timer.event_id;
        if (timer.period > 0)
        {
            // A repeating timer fires at most once per frame; periods missed during a big jump of the time
            // (sleep, time factor) are skipped instead of firing all of them at once.
            const u64 now = timer.flags & GWP_TIMER_REAL_TIME ? g_level_ms : game_now;
            timer.due += timer.period;
            if (timer.due <= now)
                timer.due = now + timer.period;
        }
        else
            g_timers->erase(it);

        const GwpValue args[] = { events::String(name.c_str()), events::String(item.key.c_str()) };
        events::Emit(event_id, args, 2);
    }
    due.clear();
}

void Reset()
{
    if (g_timers)
        g_timers->clear();
}

void WriteSave(IWriter& stream)
{
    u32 count = 0;
    if (g_timers)
    {
        for (const auto& [key, timer] : *g_timers)
            count += timer.flags & GWP_TIMER_PERSISTENT ? 1 : 0;
    }
    stream.open_chunk(kSaveChunkId);
    stream.w_u32(kSaveFormat);
    stream.w_u32(count);
    if (g_timers)
    {
        for (const auto& [key, timer] : *g_timers)
        {
            if (!(timer.flags & GWP_TIMER_PERSISTENT))
                continue;
            stream.w_stringZ(timer.owner.c_str());
            stream.w_stringZ(timer.name.c_str());
            stream.w_stringZ(timer.event.c_str());
            stream.w_u32(timer.flags);
            // Game time is saved with the game: keep the moment. Level time is not: keep what is left.
            if (timer.flags & GWP_TIMER_REAL_TIME)
                stream.w_u64(timer.due > g_level_ms ? timer.due - g_level_ms : 0);
            else
                stream.w_u64(timer.due);
            stream.w_u64(timer.period);
        }
    }
    stream.close_chunk();
}

void ReadSave(IReader& stream)
{
    // The loaded game replaces the persistent timers; the others were dropped by Reset when the game was created.
    if (g_timers)
    {
        for (auto it = g_timers->begin(); it != g_timers->end();)
            it = it->second.flags & GWP_TIMER_PERSISTENT ? g_timers->erase(it) : std::next(it);
    }
    IReader* reader = stream.open_chunk(kSaveChunkId);
    if (!reader)
        return; // save made by a build without timers
    const auto has = [reader](size_t bytes) { return reader->elapsed() >= static_cast<intptr_t>(bytes); };
    const u32 format = has(sizeof(u32)) ? reader->r_u32() : 0;
    if (format != kSaveFormat)
    {
        Msg("! [timers] save data: format %u is not supported, ignored", format);
        reader->close();
        return;
    }
    const u32 count = has(sizeof(u32)) ? reader->r_u32() : 0;
    Timers& timers = GetTimers();
    u32 loaded = 0;
    for (u32 i = 0; i < count; ++i)
    {
        Timer timer;
        if (!ReadStringZChecked(*reader, timer.owner) || !ReadStringZChecked(*reader, timer.name) ||
            !ReadStringZChecked(*reader, timer.event) || !has(sizeof(u32) + 2 * sizeof(u64)))
        {
            Msg("! [timers] save data is truncated, the rest is ignored");
            break;
        }
        timer.flags = reader->r_u32();
        const u64 due = reader->r_u64();
        timer.period = reader->r_u64();
        const bool real = (timer.flags & GWP_TIMER_REAL_TIME) != 0;
        if (!IsValidName(timer.owner.c_str()) || !IsValidName(timer.name.c_str()) ||
            !IsValidName(timer.event.c_str()) || IsBuiltinEvent(timer.event.c_str()) ||
            (timer.flags & ~(kKnownFlags | kLuaOwned)) ||
            !(timer.flags & GWP_TIMER_PERSISTENT) || timer.period > kMaxMs || due > (real ? kMaxMs : kMaxGameTimeMs))
        {
            Msg("! [timers] save data is corrupted, the rest is ignored");
            break;
        }
        timer.due = timer.flags & GWP_TIMER_REAL_TIME ? g_level_ms + due : due;
        timer.event_id = events::Intern(timer.event.c_str());
        events::Declare(timer.event.c_str());
        timer.serial = ++g_serial;
        const xr_string key = KeyOf(timer.owner.c_str(), timer.name.c_str());
        timers[key] = std::move(timer);
        ++loaded;
    }
    reader->close();
    if (IsDebugLog())
        Msg("  [timers] save data: %u timer(s) loaded", loaded);
}

void Shutdown()
{
    xr_delete(g_timers);
    xr_delete(g_due);
}

void PrintList()
{
    u64 game_now = 0;
    const bool has_game = GameTimeMs(game_now);
    Msg("- [timers] timers (p = persistent, l = started from Lua, x = its plugin is not loaded, the timer waits):");
    if (g_timers)
    {
        for (const auto& [key, timer] : *g_timers)
        {
            const bool real = (timer.flags & GWP_TIMER_REAL_TIME) != 0;
            string64 left;
            if (real || has_game)
            {
                const u64 now = real ? g_level_ms : game_now;
                xr_sprintf(left, "%.3f s", timer.due > now ? static_cast<double>(timer.due - now) / 1000.0 : 0.0);
            }
            else
                xr_strcpy(left, "? (no game)");
            string64 period;
            if (timer.period)
                xr_sprintf(period, "every %.3f s", static_cast<double>(timer.period) / 1000.0);
            else
                xr_strcpy(period, "once");
            Msg("-   %s%s %-40s %-5s in %-14s %-18s -> %s", timer.flags & GWP_TIMER_PERSISTENT ? "p" : " ",
                timer.flags & kLuaOwned ? "l" : CanFire(timer) ? " " : "x", key.c_str(), BaseName(timer.flags),
                left, period, timer.event.c_str());
        }
    }
    Msg("- [timers] %u timer(s)", g_timers ? static_cast<u32>(g_timers->size()) : 0u);
}
} // namespace gw::addons::timers
