#include "StdAfx.h"

#include "addon_event_bus.h"
#include "addon_host.h"

#include "xrScriptEngine/script_engine.hpp"
#include "script_game_object.h"
#include "GameObject.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"

#include <algorithm>
#include <iterator>

namespace gw::addons::events
{
namespace
{
enum class ESubscriberKind : u8
{
    Native,
    LuaFunction, // handler(...)
    LuaObject,   // handler[event_name](handler, ...): table or userdata with a method named as the event
};

struct Subscriber
{
    GwpSubscriptionId id = 0;
    ESubscriberKind kind = ESubscriberKind::Native;
    bool alive = true;

    // Native
    const GwpPlugin* plugin = nullptr;
    GwpEventHandler handler = nullptr;
    void* user = nullptr;

    // Lua
    int lua_ref = LUA_NOREF; // handler in the registry of Bus::lua
    u32 throttle_ms = 0;     // 0 = every dispatch
    u32 last_call_ms = 0;
    bool called = false;
};

struct Event
{
    xr_string name;
    bool declared = false;
    bool warned_undeclared = false;
    u32 flags = 0; // GWP_EVENT_*

    xr_vector<Subscriber> subscribers;
    u32 native_count = 0;
    u32 lua_count = 0;
    u32 dispatch_depth = 0;
    bool has_dead = false; // removed during a dispatch; erased when the outermost dispatch ends

    // Lua scripts pass the result of some events as a flags table ({ret_value = true}); plugins see it as
    // GwpEvent::result. 1-based argument index of that table and its field; 0 = the event has none.
    u32 lua_result_arg = 0;
    pcstr lua_result_field = nullptr;

    bool lua_profile = false; // per-handler rows of the Lua ACTOR_BINDER_PROFILE profiler
};

struct Bus
{
    xr_vector<xr_unique_ptr<Event>> events; // index = id - 1; Event objects never move
    xr_unordered_map<xr_string, GwpEventId> ids;
    xr_unordered_map<GwpSubscriptionId, GwpEventId> native_subscriptions;
    GwpSubscriptionId next_subscription = 1;
    u32 dispatch_depth = 0; // all events together: protection against endless recursion

    lua_State* lua = nullptr;         // Lua state the Lua subscribers belong to
    lua_State* current_lua = nullptr; // Lua thread (coroutine) running a Lua emit right now
    int lua_call_method_ref = LUA_NOREF;
};

Bus* g_bus = nullptr;

constexpr u32 kMaxDispatchDepth = 32;
constexpr size_t kMaxNameLength = 256;

// Names of EBuiltin, in the same order.
constexpr pcstr kBuiltinNames[] = {
    "engine_on_script_start",
    "game_on_start",
    "game_on_end",
    "alife_on_before_save",
    "alife_on_after_save",
    "alife_on_load",
    "alife_on_after_load",
    "level_on_start",
    "level_on_stop",
    "level_on_frame",
    "actor_on_spawn",
    "actor_on_destroy",
};
static_assert(std::size(kBuiltinNames) == static_cast<size_t>(EBuiltin::Count_) - 1, "kBuiltinNames != EBuiltin");

struct LuaResultBinding
{
    pcstr event;
    u32 arg;
    pcstr field;
};

// Script events whose result travels in a flags table. See the emitters in _g.script, bind_stalker_ext.script,
// level_input.script.
constexpr LuaResultBinding kLuaResultBindings[] = {
    { "actor_on_before_hit", 3, "ret_value" },
    { "actor_on_before_death", 2, "ret_value" },
    { "level_input_on_key_press", 3, "ret" },
    { "on_before_item_use", 3, "ret_value" },
};

constexpr pcstr kLuaProfiledEvents[] = { "actor_on_update", "actor_on_update_fast", "actor_on_update_slow" };

GwpEventId InternIn(Bus& bus, pcstr name);

Bus& GetBus()
{
    if (!g_bus)
    {
        g_bus = xr_new<Bus>();
        for (const pcstr name : kBuiltinNames)
            InternIn(*g_bus, name);
        for (auto& event : g_bus->events)
            event->declared = true;
    }
    return *g_bus;
}

GwpEventId InternIn(Bus& bus, pcstr name)
{
    if (!name || !name[0] || xr_strlen(name) > kMaxNameLength)
        return GWP_INVALID_EVENT_ID;

    const auto it = bus.ids.find(name);
    if (it != bus.ids.end())
        return it->second;

    auto event = xr_make_unique<Event>();
    event->name = name;
    for (const LuaResultBinding& binding : kLuaResultBindings)
    {
        if (xr_strcmp(binding.event, name) == 0)
        {
            event->lua_result_arg = binding.arg;
            event->lua_result_field = binding.field;
            event->flags |= GWP_EVENT_HAS_RESULT;
        }
    }
    for (const pcstr profiled : kLuaProfiledEvents)
        event->lua_profile |= xr_strcmp(profiled, name) == 0;

    bus.events.push_back(std::move(event));
    const GwpEventId id = static_cast<GwpEventId>(bus.events.size());
    bus.ids.emplace(name, id);
    return id;
}

Event* FindEvent(Bus& bus, GwpEventId id)
{
    if (id == GWP_INVALID_EVENT_ID || id > bus.events.size())
        return nullptr;
    return bus.events[id - 1].get();
}

void WarnUndeclared(Event& event, pcstr who, pcstr action)
{
    if (event.declared || event.warned_undeclared)
        return;
    event.warned_undeclared = true;
    Msg("~ [events] %s: %s of undeclared event '%s' (declare it: axr_main intercepts, event_bus.declare, "
        "event_declare)", who, action, event.name.c_str());
}

// Removes a subscriber. During a dispatch of this event the entry is only marked, so indices stay valid.
void RemoveAt(Bus& bus, Event& event, size_t index)
{
    Subscriber& subscriber = event.subscribers[index];
    if (!subscriber.alive)
        return;
    subscriber.alive = false;
    if (subscriber.kind == ESubscriberKind::Native)
    {
        --event.native_count;
        bus.native_subscriptions.erase(subscriber.id);
    }
    else
        --event.lua_count;

    if (event.dispatch_depth == 0)
        event.subscribers.erase(event.subscribers.begin() + index);
    else
        event.has_dead = true;
}

// Keeps the dispatch counters and the current Lua thread for the time of one dispatch.
class DispatchScope
{
public:
    DispatchScope(Bus& bus, Event& event, lua_State* lua_thread)
        : m_bus(bus), m_event(event), m_saved_lua(bus.current_lua)
    {
        ++m_bus.dispatch_depth;
        ++m_event.dispatch_depth;
        if (lua_thread)
            m_bus.current_lua = lua_thread;
    }

    ~DispatchScope()
    {
        m_bus.current_lua = m_saved_lua;
        --m_bus.dispatch_depth;
        if (--m_event.dispatch_depth == 0 && m_event.has_dead)
        {
            m_event.has_dead = false;
            auto& list = m_event.subscribers;
            list.erase(std::remove_if(list.begin(), list.end(), [](const Subscriber& s) { return !s.alive; }),
                list.end());
        }
    }

    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;

    bool TooDeep() const { return m_bus.dispatch_depth > kMaxDispatchDepth; }

private:
    Bus& m_bus;
    Event& m_event;
    lua_State* m_saved_lua;
};

// ---------------------------------------------------------------------------------------------
// Native handlers
// ---------------------------------------------------------------------------------------------

// A C++ exception thrown out of a plugin handler stops here; C++ unwinding runs every destructor on the way,
// so nested dispatch scopes of the engine stay consistent.
bool CallNativeCatching(GwpEventHandler handler, void* user, const GwpEvent* event)
{
    try
    {
        handler(user, event);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
// Crashes (access violation and other hardware exceptions) are caught only when they happen inside the plugin
// library itself: an engine crash under a plugin handler keeps going to the normal crash handler, and no engine
// frame with destructors is skipped (the engine is built with /EHsc, SEH unwinding does not run destructors).
int PluginCrashFilter(const EXCEPTION_POINTERS* info, HMODULE plugin_module, DWORD& code, void*& address)
{
    code = info->ExceptionRecord->ExceptionCode;
    address = info->ExceptionRecord->ExceptionAddress;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP || !plugin_module)
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE fault_module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &fault_module) ||
        fault_module != plugin_module)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects with destructors here: __try cannot be mixed with them in one function.
bool CallNativeGuarded(GwpEventHandler handler, void* user, const GwpEvent* event)
{
    HMODULE plugin_module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(handler), &plugin_module);
    DWORD code = 0;
    void* address = nullptr;
    __try
    {
        return CallNativeCatching(handler, user, event);
    }
    __except (PluginCrashFilter(GetExceptionInformation(), plugin_module, code, address))
    {
        if (code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        Msg("! [events] exception 0x%08x at %p in a plugin handler of event '%s'", static_cast<unsigned>(code), address,
            event->name);
        return false;
    }
}
#else
bool CallNativeGuarded(GwpEventHandler handler, void* user, const GwpEvent* event)
{
    return CallNativeCatching(handler, user, event);
}
#endif

void RemovePluginSubscriptionsIn(Bus& bus, const GwpPlugin* plugin)
{
    for (auto& event : bus.events)
    {
        for (size_t i = event->subscribers.size(); i-- > 0;)
        {
            const Subscriber& subscriber = event->subscribers[i];
            if (subscriber.alive && subscriber.kind == ESubscriberKind::Native && subscriber.plugin == plugin)
                RemoveAt(bus, *event, i);
        }
    }
}

void DispatchNative(Bus& bus, Event& event, GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result)
{
    const GwpEvent data{ id, event.name.c_str(), argc, argv, result };
    // Index loop: handlers may subscribe (push_back) or unsubscribe (marked only) during the dispatch.
    const size_t count = event.subscribers.size();
    for (size_t i = 0; i < count; ++i)
    {
        const Subscriber& subscriber = event.subscribers[i];
        if (!subscriber.alive || subscriber.kind != ESubscriberKind::Native)
            continue;
        const GwpEventHandler handler = subscriber.handler;
        void* const user = subscriber.user;
        const GwpPlugin* const plugin = subscriber.plugin;
        if (!CallNativeGuarded(handler, user, &data))
        {
            Msg("! [plugin:%s] crashed in a handler of event '%s', all its event subscriptions are removed",
                PluginAddonId(plugin), event.name.c_str());
            RemovePluginSubscriptionsIn(bus, plugin);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Lua handlers
// ---------------------------------------------------------------------------------------------

// Lua state the bus may call into right now, or nullptr.
lua_State* ActiveLua(Bus& bus)
{
    if (!bus.lua || !GEnv.ScriptEngine || GEnv.ScriptEngine->lua() != bus.lua)
        return nullptr;
    return bus.current_lua ? bus.current_lua : bus.lua;
}

// Pushes the event arguments and returns their count.
struct LuaArgs
{
    // Lua emit: arguments already on the stack
    int first = 0;
    int count = 0;
    // Native emit
    const GwpValue* argv = nullptr;
    u32 argc = 0;
    int flags_table = 0; // stack index of the flags table appended as the last argument; 0 = none
};

CScriptGameObject* FindScriptObject(u16 id)
{
    if (!g_pGameLevel)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object ? object->lua_game_object() : nullptr;
}

void PushValue(lua_State* L, const GwpValue& value)
{
    switch (value.type)
    {
    case GWP_T_BOOL: lua_pushboolean(L, value.u.b != 0); break;
    case GWP_T_INT: lua_pushnumber(L, static_cast<lua_Number>(value.u.i)); break;
    case GWP_T_NUMBER: lua_pushnumber(L, value.u.n); break;
    case GWP_T_STRING:
        if (value.u.s.ptr)
            lua_pushlstring(L, value.u.s.ptr, value.u.s.len);
        else
            lua_pushnil(L);
        break;
    case GWP_T_VEC3:
    {
        Fvector v;
        v.set(value.u.v[0], value.u.v[1], value.u.v[2]);
        luabind::object(L, v).push(L);
        break;
    }
    case GWP_T_OBJECT:
        if (CScriptGameObject* object = FindScriptObject(value.u.id))
            luabind::object(L, object).push(L);
        else
            lua_pushnil(L);
        break;
    case GWP_T_SERVER_OBJECT:
    {
        CSE_ALifeDynamicObject* object = ai().get_alife() ? ai().alife().objects().object(value.u.id, true) : nullptr;
        if (object)
            luabind::object(L, object).push(L);
        else
            lua_pushnil(L);
        break;
    }
    default: lua_pushnil(L); break;
    }
}

// Lua value -> GwpValue. Strings point into Lua data: valid while the value stays on the stack.
GwpValue ToValue(lua_State* L, int index)
{
    switch (lua_type(L, index))
    {
    case LUA_TBOOLEAN: return Bool(lua_toboolean(L, index) != 0);
    case LUA_TNUMBER: return Number(lua_tonumber(L, index)); // Lua has only doubles: always NUMBER
    case LUA_TSTRING:
    {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        GwpValue value = Nil();
        value.type = GWP_T_STRING;
        value.u.s.ptr = text;
        value.u.s.len = static_cast<uint32_t>(length);
        return value;
    }
    case LUA_TUSERDATA:
    {
        const luabind::object object(luabind::from_stack(L, index));
        if (const auto* game_object =
                luabind::object_cast_nothrow<CScriptGameObject*>(object, static_cast<CScriptGameObject*>(nullptr)))
            return Object(game_object->ID());
        if (const auto* vector = luabind::object_cast_nothrow<Fvector*>(object, static_cast<Fvector*>(nullptr)))
            return Vec3(*vector);
        if (const auto* server_object =
                luabind::object_cast_nothrow<CSE_Abstract*>(object, static_cast<CSE_Abstract*>(nullptr)))
            return ServerObject(server_object->ID);
        break;
    }
    case LUA_TNIL:
    case LUA_TNONE: return Nil();
    default: break;
    }
    GwpValue value = Nil();
    value.type = GWP_T_LUA_REF;
    return value;
}

int PushArgs(lua_State* L, const LuaArgs& args)
{
    if (args.argv)
    {
        for (u32 i = 0; i < args.argc; ++i)
            PushValue(L, args.argv[i]);
        if (args.flags_table)
            lua_pushvalue(L, args.flags_table);
        return static_cast<int>(args.argc) + (args.flags_table ? 1 : 0);
    }
    for (int i = 0; i < args.count; ++i)
        lua_pushvalue(L, args.first + i);
    return args.count;
}

// Called under lua_pcall with (object, event_name, args...): object[event_name](object, args...).
// Looking the method up here keeps "attempt to index" errors of bad subscribers inside the protected call.
int LuaCallMethod(lua_State* L)
{
    const int top = lua_gettop(L);
    lua_pushvalue(L, 2);
    lua_gettable(L, 1);
    if (lua_type(L, -1) != LUA_TFUNCTION)
        return 0;
    lua_insert(L, 1);  // method, object, name, args...
    lua_remove(L, 3);  // method, object, args...
    lua_call(L, top - 1, 0);
    return 0;
}

// ACTOR_BINDER_PROFILE (see _g.script): one row per handler of actor_on_update*. Leaves the profiler handle
// (or nil) on the stack.
void ProfileBegin(lua_State* L, const Event& event, ESubscriberKind kind, int lua_ref)
{
    string512 label;
    lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
    if (kind == ESubscriberKind::LuaFunction)
    {
        lua_Debug info{};
        lua_getinfo(L, ">S", &info); // pops the function
        xr_sprintf(label, "callback_hnd/%s/anon@%s:%d", event.name.c_str(), info.short_src, info.linedefined);
    }
    else
    {
        xr_sprintf(label, "callback_hnd/%s/%s::%p/mtd:%s", event.name.c_str(), lua_typename(L, lua_type(L, -1)),
            lua_topointer(L, -1), event.name.c_str());
        lua_pop(L, 1);
    }
    lua_getglobal(L, "actor_binder_profile_begin");
    if (lua_type(L, -1) == LUA_TFUNCTION)
    {
        lua_pushstring(L, label);
        if (lua_pcall(L, 1, 1, 0) != 0)
        {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
    }
    else
    {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

void ProfileEnd(lua_State* L, int handle_index)
{
    lua_getglobal(L, "actor_binder_profile_end");
    if (lua_type(L, -1) != LUA_TFUNCTION)
    {
        lua_pop(L, 1);
        return;
    }
    lua_pushvalue(L, handle_index);
    if (lua_pcall(L, 1, 0, 0) != 0)
        lua_pop(L, 1);
}

void DispatchLua(Bus& bus, lua_State* L, Event& event, const LuaArgs& args)
{
    const int base = lua_gettop(L);
    const int max_args = (args.argv ? static_cast<int>(args.argc) + 1 : args.count) + 2;
    if (!lua_checkstack(L, max_args + 8))
    {
        Msg("! [events] Lua stack overflow while dispatching '%s'", event.name.c_str());
        return;
    }

    bool profile = false;
    if (event.lua_profile)
    {
        lua_getglobal(L, "ACTOR_BINDER_PROFILE");
        profile = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    u32 now = 0;
    bool now_set = false;
    const size_t count = event.subscribers.size();
    for (size_t i = 0; i < count; ++i)
    {
        Subscriber& subscriber = event.subscribers[i];
        if (!subscriber.alive || subscriber.kind == ESubscriberKind::Native)
            continue;
        if (subscriber.throttle_ms)
        {
            if (!now_set)
            {
                now = Device.dwTimeGlobal;
                now_set = true;
            }
            if (subscriber.called && now - subscriber.last_call_ms < subscriber.throttle_ms)
                continue;
            subscriber.called = true;
            subscriber.last_call_ms = now;
        }

        // Copies: any Lua call below may subscribe and reallocate the vector, `subscriber` would dangle.
        const ESubscriberKind kind = subscriber.kind;
        const int lua_ref = subscriber.lua_ref;

        int profile_handle = 0;
        if (profile)
        {
            ProfileBegin(L, event, kind, lua_ref);
            profile_handle = lua_gettop(L);
        }

        int nargs;
        if (kind == ESubscriberKind::LuaObject)
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, bus.lua_call_method_ref);
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
            lua_pushlstring(L, event.name.c_str(), event.name.size());
            nargs = 2 + PushArgs(L, args);
        }
        else
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, lua_ref);
            nargs = PushArgs(L, args);
        }

        if (lua_pcall(L, nargs, 0, 0) != 0)
        {
            const char* error = lua_tostring(L, -1);
            Msg("! Script callback [%s] failed: %s", event.name.c_str(), error ? error : "(non-string error)");
        }

        if (profile_handle)
            ProfileEnd(L, profile_handle);
        lua_settop(L, base);
    }
}

void ResetLuaSubscribers(Bus& bus)
{
    // The old Lua state is already closed: drop the references without luaL_unref.
    for (auto& event : bus.events)
    {
        for (size_t i = event->subscribers.size(); i-- > 0;)
        {
            if (event->subscribers[i].kind != ESubscriberKind::Native)
                RemoveAt(bus, *event, i);
        }
    }
    bus.lua = nullptr;
    bus.current_lua = nullptr;
    bus.lua_call_method_ref = LUA_NOREF;
}

// ---------------------------------------------------------------------------------------------
// Lua API: global table event_bus
// ---------------------------------------------------------------------------------------------

// Event id of the name at `index`. Upvalue 1 of every event_bus function caches name -> id, so a dispatch
// costs one table lookup instead of building a C++ string.
GwpEventId LuaEventId(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TSTRING)
        return GWP_INVALID_EVENT_ID;
    lua_pushvalue(L, index);
    lua_rawget(L, lua_upvalueindex(1));
    if (lua_type(L, -1) == LUA_TNUMBER)
    {
        const auto id = static_cast<GwpEventId>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return id;
    }
    lua_pop(L, 1);
    const GwpEventId id = InternIn(GetBus(), lua_tostring(L, index));
    if (id != GWP_INVALID_EVENT_ID)
    {
        lua_pushvalue(L, index);
        lua_pushnumber(L, static_cast<lua_Number>(id));
        lua_rawset(L, lua_upvalueindex(1));
    }
    return id;
}

// event_bus.declare(name)
int LuaDeclare(lua_State* L)
{
    if (Event* event = FindEvent(GetBus(), LuaEventId(L, 1)))
        event->declared = true;
    return 0;
}

// event_bus.subscribe(name, handler [, throttle_ms]): handler is a function or an object with a method `name`.
// Subscribing the same handler again only updates throttle_ms.
int LuaSubscribe(lua_State* L)
{
    Bus& bus = GetBus();
    Event* event = FindEvent(bus, LuaEventId(L, 1));
    if (!event || bus.lua == nullptr)
        return 0;

    const int type = lua_type(L, 2);
    if (type != LUA_TFUNCTION && type != LUA_TTABLE && type != LUA_TUSERDATA)
    {
        Msg("! [events] event_bus.subscribe('%s'): the handler must be a function or an object, got %s",
            event->name.c_str(), lua_typename(L, type));
        return 0;
    }
    WarnUndeclared(*event, "Lua", "subscription");

    u32 throttle_ms = 0;
    if (lua_type(L, 3) == LUA_TNUMBER && lua_tonumber(L, 3) > 0)
        throttle_ms = static_cast<u32>(lua_tonumber(L, 3));

    for (Subscriber& subscriber : event->subscribers)
    {
        if (!subscriber.alive || subscriber.kind == ESubscriberKind::Native)
            continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, subscriber.lua_ref);
        const bool same = lua_rawequal(L, -1, 2) != 0;
        lua_pop(L, 1);
        if (same)
        {
            subscriber.throttle_ms = throttle_ms;
            subscriber.called = false;
            return 0;
        }
    }

    Subscriber subscriber;
    subscriber.id = bus.next_subscription++;
    subscriber.kind = type == LUA_TFUNCTION ? ESubscriberKind::LuaFunction : ESubscriberKind::LuaObject;
    subscriber.throttle_ms = throttle_ms;
    lua_pushvalue(L, 2);
    subscriber.lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    event->subscribers.push_back(subscriber);
    ++event->lua_count;
    return 0;
}

// event_bus.unsubscribe(name, handler)
int LuaUnsubscribe(lua_State* L)
{
    Bus& bus = GetBus();
    Event* event = FindEvent(bus, LuaEventId(L, 1));
    if (!event || lua_isnoneornil(L, 2))
        return 0;
    for (size_t i = 0; i < event->subscribers.size(); ++i)
    {
        const Subscriber& subscriber = event->subscribers[i];
        if (!subscriber.alive || subscriber.kind == ESubscriberKind::Native)
            continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, subscriber.lua_ref);
        const bool same = lua_rawequal(L, -1, 2) != 0;
        lua_pop(L, 1);
        if (same)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, subscriber.lua_ref);
            RemoveAt(bus, *event, i);
            return 0;
        }
    }
    return 0;
}

// event_bus.emit(name, ...): synchronous dispatch to Lua and native subscribers.
int LuaEmit(lua_State* L)
{
    Bus& bus = GetBus();
    const GwpEventId id = LuaEventId(L, 1);
    Event* event = FindEvent(bus, id);
    if (!event)
        return 0;
    WarnUndeclared(*event, "Lua", "emit");
    if (event->subscribers.empty())
        return 0;

    DispatchScope scope(bus, *event, L);
    if (scope.TooDeep())
    {
        Msg("! [events] event '%s': nested emits deeper than %u, dispatch skipped", event->name.c_str(),
            kMaxDispatchDepth);
        return 0;
    }

    const int argc = lua_gettop(L) - 1;
    if (event->lua_count)
    {
        LuaArgs args;
        args.first = 2;
        args.count = argc;
        DispatchLua(bus, L, *event, args);
    }

    if (event->native_count)
    {
        constexpr int kInlineArgs = 16;
        GwpValue inline_values[kInlineArgs];
        xr_vector<GwpValue> heap_values;
        GwpValue* values = inline_values;
        if (argc > kInlineArgs)
        {
            heap_values.resize(argc);
            values = heap_values.data();
        }
        for (int i = 0; i < argc; ++i)
            values[i] = ToValue(L, 2 + i);

        // Flags table of the script event -> GwpEvent::result, written back after the native handlers.
        GwpValue result = Nil();
        GwpValue* result_ptr = nullptr;
        const int flags_index = event->lua_result_arg ? 1 + static_cast<int>(event->lua_result_arg) : 0;
        if (flags_index && flags_index <= lua_gettop(L) && lua_type(L, flags_index) == LUA_TTABLE)
        {
            lua_getfield(L, flags_index, event->lua_result_field);
            if (lua_type(L, -1) == LUA_TBOOLEAN || lua_type(L, -1) == LUA_TNUMBER)
                result = ToValue(L, -1);
            lua_pop(L, 1);
            result_ptr = &result;
        }

        const GwpValue before = result;
        DispatchNative(bus, *event, id, values, static_cast<u32>(argc), result_ptr);

        // Written back only when a plugin changed it. All script result fields are bool flags: keep them bool.
        if (result_ptr && memcmp(&before, &result, sizeof(result)) != 0)
        {
            bool flag = false;
            if (result.type == GWP_T_BOOL)
                flag = result.u.b != 0;
            else if (result.type == GWP_T_NUMBER)
                flag = result.u.n != 0.0;
            else if (result.type == GWP_T_INT)
                flag = result.u.i != 0;
            lua_pushboolean(L, flag);
            lua_setfield(L, flags_index, event->lua_result_field);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group events)
// ---------------------------------------------------------------------------------------------

bool CheckMainThread(const GwpPlugin* self, pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [plugin:%s] %s called outside the main thread, ignored", PluginAddonId(self), function);
    return false;
}

GwpEventId GWP_CALL ApiEventId(const char* name)
{
    if (!CheckMainThread(nullptr, "event_id"))
        return GWP_INVALID_EVENT_ID;
    return InternIn(GetBus(), name);
}

GwpSubscriptionId GWP_CALL ApiEventSubscribe(const GwpPlugin* self, const char* name, GwpEventHandler handler, void* user)
{
    if (!CheckMainThread(self, "event_subscribe") || !self || !handler)
        return GWP_INVALID_SUBSCRIPTION_ID;
    Bus& bus = GetBus();
    const GwpEventId id = InternIn(bus, name);
    Event* event = FindEvent(bus, id);
    if (!event)
        return GWP_INVALID_SUBSCRIPTION_ID;

    Subscriber subscriber;
    subscriber.id = bus.next_subscription++;
    subscriber.kind = ESubscriberKind::Native;
    subscriber.plugin = self;
    subscriber.handler = handler;
    subscriber.user = user;
    event->subscribers.push_back(subscriber);
    ++event->native_count;
    bus.native_subscriptions.emplace(subscriber.id, id);
    return subscriber.id;
}

void GWP_CALL ApiEventUnsubscribe(const GwpPlugin* self, GwpSubscriptionId subscription)
{
    if (!CheckMainThread(self, "event_unsubscribe"))
        return;
    Bus& bus = GetBus();
    const auto it = bus.native_subscriptions.find(subscription);
    if (it == bus.native_subscriptions.end())
        return;
    Event* event = FindEvent(bus, it->second);
    if (!event)
        return;
    for (size_t i = 0; i < event->subscribers.size(); ++i)
    {
        const Subscriber& subscriber = event->subscribers[i];
        if (subscriber.alive && subscriber.id == subscription && subscriber.plugin == self)
        {
            RemoveAt(bus, *event, i);
            return;
        }
    }
}

GwpResult GWP_CALL ApiEventDeclare(const GwpPlugin* self, const char* name, uint32_t flags)
{
    if (!CheckMainThread(self, "event_declare"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Event* event = FindEvent(GetBus(), InternIn(GetBus(), name));
    if (!event)
        return GWP_ERROR_INVALID_ARGUMENT;
    event->declared = true;
    event->flags |= flags;
    return GWP_OK;
}

GwpResult GWP_CALL ApiEventEmit(const GwpPlugin* self, GwpEventId id, uint32_t argc, const GwpValue* argv, GwpValue* result)
{
    if (!CheckMainThread(self, "event_emit"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Event* event = FindEvent(GetBus(), id);
    if (!event || (argc > 0 && !argv))
        return GWP_ERROR_INVALID_ARGUMENT;
    string128 who;
    xr_sprintf(who, "plugin:%s", PluginAddonId(self));
    WarnUndeclared(*event, who, "emit");
    Emit(id, argv, argc, result);
    return GWP_OK;
}

} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CEventBusScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CEventBusScript::script_register(lua_State* L)
{
    Bus& bus = GetBus();
    ResetLuaSubscribers(bus);
    bus.lua = L;
    lua_pushcfunction(L, &LuaCallMethod);
    bus.lua_call_method_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    const luaL_Reg functions[] = {
        { "declare", &LuaDeclare },
        { "subscribe", &LuaSubscribe },
        { "unsubscribe", &LuaUnsubscribe },
        { "emit", &LuaEmit },
    };
    lua_newtable(L); // event_bus
    lua_newtable(L); // name -> id cache, shared by all functions as upvalue 1
    for (const luaL_Reg& function : functions)
    {
        lua_pushvalue(L, -1);
        lua_pushcclosure(L, function.func, 1);
        lua_setfield(L, -3, function.name);
    }
    lua_pop(L, 1);
    lua_setglobal(L, "event_bus");
}

// ---------------------------------------------------------------------------------------------
// Engine side
// ---------------------------------------------------------------------------------------------

namespace
{
void DispatchLuaFromNative(Bus& bus, lua_State* L, Event& event, const GwpValue* argv, u32 argc, GwpValue* result);
} // namespace

GwpEventId Intern(pcstr name) { return InternIn(GetBus(), name); }

void Declare(pcstr name, u32 flags)
{
    if (Event* event = FindEvent(GetBus(), Intern(name)))
    {
        event->declared = true;
        event->flags |= flags;
    }
}

bool HasSubscribers(GwpEventId id)
{
    const Event* event = g_bus ? FindEvent(*g_bus, id) : nullptr;
    return event && (event->native_count || event->lua_count);
}

void Emit(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result)
{
    if (!g_bus)
        return;
    if (!IsMainThread())
    {
        Msg("! [events] event %u emitted outside the main thread, ignored", id);
        return;
    }
    Bus& bus = *g_bus;
    Event* event = FindEvent(bus, id);
    if (!event || event->subscribers.empty())
        return;

    DispatchScope scope(bus, *event, nullptr);
    if (scope.TooDeep())
    {
        Msg("! [events] event '%s': nested emits deeper than %u, dispatch skipped", event->name.c_str(),
            kMaxDispatchDepth);
        return;
    }

    // Same order as a Lua emit: Lua subscribers first, then plugins.
    lua_State* L = event->lua_count ? ActiveLua(bus) : nullptr;
    if (L && lua_checkstack(L, 4))
        DispatchLuaFromNative(bus, L, *event, argv, argc, result);

    if (event->native_count)
        DispatchNative(bus, *event, id, argv, argc, result);
}

namespace
{
void DispatchLuaFromNative(Bus& bus, lua_State* L, Event& event, const GwpValue* argv, u32 argc, GwpValue* result)
{
    const pcstr field = event.lua_result_field ? event.lua_result_field : "ret_value";
    const int base = lua_gettop(L);
    LuaArgs args;
    args.argv = argv;
    args.argc = argc;
    if (result)
    {
        // Lua handlers get the result as a trailing {ret_value = ...} table, like the script events do.
        lua_newtable(L);
        PushValue(L, *result);
        lua_setfield(L, -2, field);
        args.flags_table = lua_gettop(L);
    }
    DispatchLua(bus, L, event, args);
    if (result)
    {
        lua_getfield(L, args.flags_table, field);
        const int type = lua_type(L, -1);
        if (type == LUA_TBOOLEAN || type == LUA_TNUMBER)
            *result = ToValue(L, -1);
        else if (type == LUA_TNIL)
            *result = Nil();
    }
    lua_settop(L, base);
}
} // namespace

GwpValue Nil()
{
    GwpValue value;
    memset(&value, 0, sizeof(value));
    value.type = GWP_T_NIL;
    return value;
}

GwpValue Bool(bool v)
{
    GwpValue value = Nil();
    value.type = GWP_T_BOOL;
    value.u.b = v ? 1 : 0;
    return value;
}

GwpValue Int(s64 v)
{
    GwpValue value = Nil();
    value.type = GWP_T_INT;
    value.u.i = v;
    return value;
}

GwpValue Number(double v)
{
    GwpValue value = Nil();
    value.type = GWP_T_NUMBER;
    value.u.n = v;
    return value;
}

GwpValue String(pcstr v)
{
    GwpValue value = Nil();
    value.type = GWP_T_STRING;
    value.u.s.ptr = v ? v : "";
    value.u.s.len = static_cast<uint32_t>(v ? xr_strlen(v) : 0);
    return value;
}

GwpValue Vec3(const Fvector& v)
{
    GwpValue value = Nil();
    value.type = GWP_T_VEC3;
    value.u.v[0] = v.x;
    value.u.v[1] = v.y;
    value.u.v[2] = v.z;
    return value;
}

GwpValue Object(u16 id)
{
    GwpValue value = Nil();
    value.type = GWP_T_OBJECT;
    value.u.id = id;
    return value;
}

GwpValue ServerObject(u16 id)
{
    GwpValue value = Nil();
    value.type = GWP_T_SERVER_OBJECT;
    value.u.id = id;
    return value;
}

void FillEngineApi(GwpEngineApi& api)
{
    GetBus(); // plugins subscribe from gwp_plugin_init: the bus must exist before
    api.event_id = &ApiEventId;
    api.event_subscribe = &ApiEventSubscribe;
    api.event_unsubscribe = &ApiEventUnsubscribe;
    api.event_declare = &ApiEventDeclare;
    api.event_emit = &ApiEventEmit;
}

void RemovePluginSubscriptions(const GwpPlugin* plugin)
{
    if (g_bus)
        RemovePluginSubscriptionsIn(*g_bus, plugin);
}

void Shutdown()
{
    // Runs after the Lua state is closed (~CAI_Space) and after every plugin is unloaded.
    xr_delete(g_bus);
}

void PrintList()
{
    if (!g_bus)
    {
        Msg("- [events] the bus is not created");
        return;
    }
    u32 declared = 0, with_subscribers = 0;
    Msg("- [events] events with subscribers (lua / native):");
    for (const auto& event : g_bus->events)
    {
        declared += event->declared ? 1 : 0;
        if (!event->lua_count && !event->native_count)
            continue;
        ++with_subscribers;
        Msg("-   %-40s %3u / %-3u%s", event->name.c_str(), event->lua_count, event->native_count,
            event->declared ? "" : "  (undeclared)");
    }
    Msg("- [events] %u event(s) known, %u declared, %u with subscribers", static_cast<u32>(g_bus->events.size()),
        declared, with_subscribers);
}
} // namespace gw::addons::events
