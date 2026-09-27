#include "StdAfx.h"

#include "addon_event_bus.h"
#include "addon_host.h"
#include "addon_object_events.h"

#include "xrScriptEngine/script_engine.hpp"
#include "script_game_object.h"
#include "GameObject.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"

#include <algorithm>
#include <cmath>
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

// Events collected for a batch subscriber (GWP_SUBSCRIBE_BATCH) until the next delivery.
struct BatchQueue
{
    xr_vector<GwpValue> values; // arguments of every record; a string keeps its offset into `text` in `reserved`
    xr_vector<u32> first;       // per record: index of its first argument in `values`
    xr_vector<u32> argc;        // per record: number of arguments
    xr_vector<char> text;       // copies of string arguments, zero-terminated
    bool warned_overflow = false;

    bool empty() const { return first.empty(); }
};

constexpr u32 kDefaultMaxBatch = 4096;

struct Subscriber
{
    GwpSubscriptionId id = 0;
    ESubscriberKind kind = ESubscriberKind::Native;
    bool alive = true;

    // Native
    const GwpPlugin* plugin = nullptr;
    GwpEventHandler handler = nullptr;
    void* user = nullptr;
    GwpEventBatchHandler batch_handler = nullptr; // batch subscriber when not null
    xr_unique_ptr<BatchQueue> batch;               // queue of a batch subscriber
    u32 max_batch = 0;

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
    u32 native_count = 0; // batch subscribers included
    u32 batch_count = 0;
    u32 lua_count = 0;
    u32 dispatch_depth = 0;
    bool has_dead = false; // removed during a dispatch; erased when the outermost dispatch ends

    // Lua scripts pass the result of some events as a flags table ({ret_value = true}); plugins see it as
    // GwpEvent::result. 1-based argument index of that table and its field; 0 = the event has none.
    u32 lua_result_arg = 0;
    pcstr lua_result_field = nullptr;

    bool lua_profile = false; // per-handler rows of the Lua ACTOR_BINDER_PROFILE profiler

    // Stage B: schema, source, diagnostics
    xr_string schema;            // argument codes, see Declare in addon_event_bus.h
    bool has_schema = false;     // "" is a valid schema (no arguments)
    bool warned_schema = false;  // an argument mismatch was logged
    bool warned_schema_conflict = false; // a conflicting declaration was logged
    bool builtin = false;        // a built-in event: the engine is its only source
    bool engine_source = false;  // listed in addon_object_events.cpp
    u32 engine_group = 0;        // objevents::EGroup of an engine-sourced event
    bool warned_lua_emit = false;
    bool warned_adapter = false;
    bool trace = false;          // event_trace
    u32 emit_count = 0;
    u32 engine_log_ms = 0;      // gw_event_engine_log: time of the last logged emit
    u32 engine_log_skipped = 0; // emits not logged since then (rate limit)
    bool engine_log_any = false;
    int lua_adapter_ref = LUA_NOREF; // event_bus.set_lua_adapter: builds the Lua arguments of an engine emit
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
bool g_level_change_mark = false;
u32 g_active_engine_groups = 0; // objevents: implemented & enabled groups
int g_schema_check = -1;        // gw_event_schema_check
int g_engine_log = -1;          // gw_event_engine_log
constexpr u32 kEngineLogIntervalMs = 500; // one line per event at most this often

constexpr u32 kMaxDispatchDepth = 32;
constexpr size_t kMaxNameLength = 256;

// Names of EBuiltin, in the same order.
constexpr pcstr kBuiltinNames[] = {
    "engine_on_script_start",
    "alife_on_start",
    "alife_on_end",
    "alife_on_before_save",
    "alife_on_after_save",
    "alife_on_load",
    "alife_on_after_load",
    "level_on_start",
    "level_on_stop",
    "level_on_frame",
    "actor_on_spawn",
    "actor_on_destroy",
    "data_on_changed",
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

constexpr pcstr kLuaProfiledEvents[] = { "actor_on_update", "actor_on_update_fast", "actor_on_update_slow",
    "npc_on_hit_callback", "npc_on_death_callback" };

// Schemas of the built-in events, in the order of kBuiltinNames.
constexpr pcstr kBuiltinSchemas[] = {
    "",   // engine_on_script_start
    "s",  // alife_on_start (reason)
    "",   // alife_on_end
    "s",  // alife_on_before_save (save name)
    "s",  // alife_on_after_save
    "s",  // alife_on_load
    "s",  // alife_on_after_load
    "s",  // level_on_start (level name)
    "",   // level_on_stop
    "N",  // level_on_frame (dt)
    "o",  // actor_on_spawn
    "o",  // actor_on_destroy
    "s*", // data_on_changed (key, value)
};
static_assert(std::size(kBuiltinSchemas) == std::size(kBuiltinNames), "kBuiltinSchemas != kBuiltinNames");

GwpEventId InternIn(Bus& bus, pcstr name);
void EmitFrom(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result, pcstr source, bool from_engine);

Bus& GetBus()
{
    if (!g_bus)
    {
        g_bus = xr_new<Bus>();
        for (size_t i = 0; i < std::size(kBuiltinNames); ++i)
        {
            Event& event = *g_bus->events[InternIn(*g_bus, kBuiltinNames[i]) - 1];
            event.declared = true;
            event.builtin = true;
            event.schema = kBuiltinSchemas[i];
            event.has_schema = true;
        }
        objevents::RegisterEngineEvents(); // g_bus is set: the nested GetBus() calls return it
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

// ---------------------------------------------------------------------------------------------
// Schemas, sources, trace (stage B)
// ---------------------------------------------------------------------------------------------

bool IsEngineSourceActive(const Event& event)
{
    return event.engine_source && (g_active_engine_groups & (1u << event.engine_group)) != 0;
}

bool SchemaCheckEnabled() { return g_schema_check < 0 ? IsDebugLog() : g_schema_check != 0; }

bool IsValidSchema(pcstr schema)
{
    for (pcstr p = schema; *p; ++p)
    {
        if (!strchr("bINsvoOt*", *p))
            return false;
        if (p[1] == '?')
            ++p;
    }
    return true;
}

void SetSchema(Event& event, pcstr schema, pcstr who)
{
    if (!schema)
        return;
    if (!IsValidSchema(schema))
    {
        Msg("! [events] %s: invalid schema '%s' of '%s' (codes b I N s v o O t *, '?' = may be nil)", who, schema,
            event.name.c_str());
        return;
    }
    if (!event.has_schema)
    {
        event.schema = schema;
        event.has_schema = true;
    }
    else if (event.schema != schema && !event.warned_schema_conflict)
    {
        event.warned_schema_conflict = true;
        Msg("~ [events] %s: schema '%s' of '%s' ignored, the event already has '%s'", who, schema,
            event.name.c_str(), event.schema.c_str());
    }
}

bool ValueMatches(char code, const GwpValue& value)
{
    switch (code)
    {
    case 'b': return value.type == GWP_T_BOOL;
    case 'I': return value.type == GWP_T_INT || (value.type == GWP_T_NUMBER && std::floor(value.u.n) == value.u.n);
    case 'N': return value.type == GWP_T_NUMBER || value.type == GWP_T_INT;
    case 's': return value.type == GWP_T_STRING;
    case 'v': return value.type == GWP_T_VEC3;
    case 'o': return value.type == GWP_T_OBJECT;
    case 'O': return value.type == GWP_T_SERVER_OBJECT;
    case 't': return value.type == GWP_T_LUA_REF;
    case '*': return true;
    default: return false;
    }
}

// "object 12, number 1.5, string "abc", nil"
xr_string DescribeArgs(const GwpValue* argv, u32 argc)
{
    xr_string text;
    for (u32 i = 0; i < argc; ++i)
    {
        string256 item;
        const GwpValue& v = argv[i];
        switch (v.type)
        {
        case GWP_T_BOOL: xr_strcpy(item, v.u.b ? "true" : "false"); break;
        case GWP_T_INT: xr_sprintf(item, "integer %lld", static_cast<long long>(v.u.i)); break;
        case GWP_T_NUMBER: xr_sprintf(item, "number %g", v.u.n); break;
        case GWP_T_STRING: xr_sprintf(item, "string \"%.*s\"", static_cast<int>(std::min<u32>(v.u.s.len, 64)),
                               v.u.s.ptr ? v.u.s.ptr : ""); break;
        case GWP_T_VEC3: xr_sprintf(item, "vector (%g, %g, %g)", v.u.v[0], v.u.v[1], v.u.v[2]); break;
        case GWP_T_OBJECT: xr_sprintf(item, "object %u", static_cast<u32>(v.u.id)); break;
        case GWP_T_SERVER_OBJECT: xr_sprintf(item, "server object %u", static_cast<u32>(v.u.id)); break;
        case GWP_T_LUA_REF: xr_strcpy(item, "table/userdata"); break;
        default: xr_strcpy(item, "nil"); break;
        }
        if (i)
            text += ", ";
        text += item;
    }
    return text;
}

// Logs once per event when the arguments do not match the schema.
void CheckSchema(Event& event, const GwpValue* argv, u32 argc, pcstr source)
{
    if (!event.has_schema || event.warned_schema || !SchemaCheckEnabled())
        return;
    bool ok = true;
    u32 index = 0;
    for (pcstr p = event.schema.c_str(); *p; ++p, ++index)
    {
        const char code = *p;
        const bool optional = p[1] == '?' || code == '*';
        if (p[1] == '?')
            ++p;
        if (index >= argc || argv[index].type == GWP_T_NIL)
            ok = ok && optional;
        else
            ok = ok && ValueMatches(code, argv[index]);
    }
    ok = ok && argc <= index;
    if (ok)
        return;
    event.warned_schema = true;
    Msg("~ [events] '%s' sent from %s: arguments (%s) do not match the schema '%s'", event.name.c_str(), source,
        DescribeArgs(argv, argc).c_str(), event.schema.c_str());
}

void Trace(const Event& event, const GwpValue* argv, u32 argc, pcstr source)
{
    Msg("  [events] trace '%s' from %s: %s", event.name.c_str(), source, DescribeArgs(argv, argc).c_str());
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
        if (subscriber.batch_handler)
            --event.batch_count;
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

// One call of a plugin handler: a single event (handler) or a batch (batch_handler).
struct NativeCall
{
    GwpEventHandler handler = nullptr;
    GwpEventBatchHandler batch_handler = nullptr;
    void* user = nullptr;
    const GwpEvent* events = nullptr;
    u32 count = 1;
    pcstr event_name = "";
};

// A C++ exception thrown out of a plugin handler stops here; C++ unwinding runs every destructor on the way,
// so nested dispatch scopes of the engine stay consistent.
bool CallNativeCatching(const NativeCall& call)
{
    try
    {
        if (call.batch_handler)
            call.batch_handler(call.user, call.count, call.events);
        else
            call.handler(call.user, call.events);
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
bool CallNativeGuarded(const NativeCall& call)
{
    const void* code_address = call.batch_handler ? reinterpret_cast<const void*>(call.batch_handler)
                                                  : reinterpret_cast<const void*>(call.handler);
    HMODULE plugin_module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(code_address), &plugin_module);
    DWORD code = 0;
    void* address = nullptr;
    __try
    {
        return CallNativeCatching(call);
    }
    __except (PluginCrashFilter(GetExceptionInformation(), plugin_module, code, address))
    {
        if (code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        Msg("! [events] exception 0x%08x at %p in a plugin handler of event '%s'", static_cast<unsigned>(code), address,
            call.event_name);
        return false;
    }
}
#else
bool CallNativeGuarded(const NativeCall& call) { return CallNativeCatching(call); }
#endif

void Enqueue(Subscriber& subscriber, const Event& event, const GwpValue* argv, u32 argc)
{
    BatchQueue& queue = *subscriber.batch;
    if (queue.first.size() >= subscriber.max_batch)
    {
        if (!queue.warned_overflow)
        {
            queue.warned_overflow = true;
            Msg("~ [plugin:%s] batch of event '%s' is full (%u records), new records are dropped until the next "
                "delivery", PluginAddonId(subscriber.plugin), event.name.c_str(), subscriber.max_batch);
        }
        return;
    }
    queue.first.push_back(static_cast<u32>(queue.values.size()));
    queue.argc.push_back(argc);
    for (u32 i = 0; i < argc; ++i)
    {
        GwpValue value = argv[i];
        if (value.type == GWP_T_STRING)
        {
            value.reserved = static_cast<uint32_t>(queue.text.size());
            if (value.u.s.ptr && value.u.s.len)
                queue.text.insert(queue.text.end(), value.u.s.ptr, value.u.s.ptr + value.u.s.len);
            queue.text.push_back(0);
            value.u.s.ptr = nullptr; // set at delivery: `text` may reallocate until then
        }
        queue.values.push_back(value);
    }
}

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
    u32 now = 0;
    bool now_set = false;
    // Index loop: handlers may subscribe (push_back) or unsubscribe (marked only) during the dispatch.
    const size_t count = event.subscribers.size();
    for (size_t i = 0; i < count; ++i)
    {
        Subscriber& subscriber = event.subscribers[i];
        if (!subscriber.alive || subscriber.kind != ESubscriberKind::Native)
            continue;
        if (subscriber.batch_handler)
        {
            Enqueue(subscriber, event, argv, argc); // delivered by FlushBatches; throttle applies there
            continue;
        }
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
        NativeCall call;
        call.handler = subscriber.handler;
        call.user = subscriber.user;
        call.events = &data;
        call.event_name = event.name.c_str();
        const GwpPlugin* const plugin = subscriber.plugin;
        ZoneScopedN("events/plugin_handler"); // Tracy: one zone per handler call, text = addon id and event
        ZoneTextF("%s: %s", PluginAddonId(plugin), event.name.c_str());
        if (!CallNativeGuarded(call))
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
                luabind::object_cast_nothrow<const CScriptGameObject*>(object, static_cast<const CScriptGameObject*>(nullptr)))
            return Object(game_object->ID());
        // const: vectors the engine hands to callbacks by const reference are const instances for luabind
        if (const auto* vector = luabind::object_cast_nothrow<const Fvector*>(object, static_cast<const Fvector*>(nullptr)))
            return Vec3(*vector);
        if (const auto* server_object =
                luabind::object_cast_nothrow<const CSE_Abstract*>(object, static_cast<const CSE_Abstract*>(nullptr)))
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
    if (args.flags_table)
        lua_pushvalue(L, args.flags_table);
    return args.count + (args.flags_table ? 1 : 0);
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
    ZoneScopedN("events/dispatch_lua"); // Tracy: all Lua subscribers of one emit
    ZoneTextF("%s", event.name.c_str());
    const int base = lua_gettop(L);
    const int max_args = (args.argv ? static_cast<int>(args.argc) : args.count) + 3;
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
    for (auto& event : bus.events)
        event->lua_adapter_ref = LUA_NOREF;
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

// event_bus.declare(name [, schema])
int LuaDeclare(lua_State* L)
{
    if (Event* event = FindEvent(GetBus(), LuaEventId(L, 1)))
    {
        event->declared = true;
        if (lua_type(L, 2) == LUA_TSTRING)
            SetSchema(*event, lua_tostring(L, 2), "Lua");
    }
    return 0;
}

// event_bus.schema(name): the argument schema, nil when the event has none.
int LuaSchema(lua_State* L)
{
    const Event* event = FindEvent(GetBus(), LuaEventId(L, 1));
    if (event && event->has_schema)
        lua_pushlstring(L, event->schema.c_str(), event->schema.size());
    else
        lua_pushnil(L);
    return 1;
}

// event_bus.set_lua_adapter(name, fn | nil): when the ENGINE emits the event, Lua subscribers get
// fn(<native arguments>) instead of the native arguments (plugins keep the native ones). For arguments that exist
// only in Lua, e.g. the actor binder of actor_on_* events. Dropped with the Lua state.
int LuaSetLuaAdapter(lua_State* L)
{
    Event* event = FindEvent(GetBus(), LuaEventId(L, 1));
    if (!event)
        return 0;
    if (event->lua_adapter_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, event->lua_adapter_ref);
    event->lua_adapter_ref = LUA_NOREF;
    event->warned_adapter = false;
    if (lua_type(L, 2) == LUA_TFUNCTION)
    {
        lua_pushvalue(L, 2);
        event->lua_adapter_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// event_bus.is_engine_source(name): true when the engine sends the event now (its stage B group is active), so a
// Lua sender must not do what the event handling does (e.g. the actor_before_death slot handler must not kill).
int LuaIsEngineSource(lua_State* L)
{
    const Event* event = FindEvent(GetBus(), LuaEventId(L, 1));
    lua_pushboolean(L, event && IsEngineSourceActive(*event) ? 1 : 0);
    return 1;
}

// Lua value -> GwpValue for every argument (strings point into the Lua stack). 16 without an allocation.
struct ArgBuffer
{
    GwpValue inline_values[16];
    xr_vector<GwpValue> heap;
    GwpValue* data = inline_values;

    void Fill(lua_State* L, int first, int count)
    {
        if (count > static_cast<int>(std::size(inline_values)))
        {
            heap.resize(count);
            data = heap.data();
        }
        for (int i = 0; i < count; ++i)
            data[i] = ToValue(L, first + i);
    }
};

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
    event->subscribers.push_back(std::move(subscriber));
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
    if (IsEngineSourceActive(*event))
    {
        // The engine emits this event now: a Lua emit would deliver it twice.
        // Expected while the SendScriptCallback lines of the binders are not removed yet (stage B-5): logged only
        // with -addon_debug, once per event, with the Lua stack of the sender.
        if (!event->warned_lua_emit && IsDebugLog())
        {
            event->warned_lua_emit = true;
            Msg("~ [events] Lua emit of engine-sourced event '%s' dropped (the engine sends it; console "
                "gw_event_engine_sources disables engine groups)", event->name.c_str());
            luaL_traceback(L, L, nullptr, 1);
            Msg("%s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        return 0;
    }
    WarnUndeclared(*event, "Lua", "emit");
    ++event->emit_count;

    const int argc = lua_gettop(L) - 1;
    // The flags table of a script event with a result is not an argument for plugins: they get GwpEvent::result.
    const int flags_index = event->lua_result_arg ? 1 + static_cast<int>(event->lua_result_arg) : 0;
    const bool has_flags = flags_index && flags_index <= lua_gettop(L) && lua_type(L, flags_index) == LUA_TTABLE;
    const int native_argc = has_flags && flags_index == lua_gettop(L) ? argc - 1 : argc;

    ArgBuffer values;
    bool converted = false;
    if (event->trace || (event->has_schema && !event->warned_schema && SchemaCheckEnabled()))
    {
        values.Fill(L, 2, native_argc);
        converted = true;
        CheckSchema(*event, values.data, static_cast<u32>(native_argc), "Lua");
        if (event->trace)
            Trace(*event, values.data, static_cast<u32>(native_argc), "Lua");
    }
    if (event->subscribers.empty())
        return 0;

    DispatchScope scope(bus, *event, L);
    if (scope.TooDeep())
    {
        Msg("! [events] event '%s': nested emits deeper than %u, dispatch skipped", event->name.c_str(),
            kMaxDispatchDepth);
        return 0;
    }

    if (event->lua_count)
    {
        LuaArgs args;
        args.first = 2;
        args.count = argc;
        DispatchLua(bus, L, *event, args);
    }

    if (event->native_count)
    {
        if (!converted)
            values.Fill(L, 2, native_argc);

        // Flags table of the script event -> GwpEvent::result, written back after the native handlers.
        GwpValue result = Nil();
        GwpValue* result_ptr = nullptr;
        if (has_flags)
        {
            lua_getfield(L, flags_index, event->lua_result_field);
            if (lua_type(L, -1) == LUA_TBOOLEAN || lua_type(L, -1) == LUA_TNUMBER)
                result = ToValue(L, -1);
            lua_pop(L, 1);
            result_ptr = &result;
        }

        const GwpValue before = result;
        DispatchNative(bus, *event, id, values.data, static_cast<u32>(native_argc), result_ptr);

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

GwpSubscriptionId SubscribeNative(const GwpPlugin* self, const char* name, const GwpSubscribeOptions& options,
    GwpEventHandler handler, void* user)
{
    const bool batch = (options.flags & GWP_SUBSCRIBE_BATCH) != 0;
    if (!self || (batch ? !options.batch_handler : !handler))
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
    subscriber.handler = batch ? nullptr : handler;
    subscriber.user = user;
    subscriber.throttle_ms = options.throttle_ms;
    if (batch)
    {
        subscriber.batch_handler = options.batch_handler;
        subscriber.batch = xr_make_unique<BatchQueue>();
        subscriber.max_batch = options.max_batch ? options.max_batch : kDefaultMaxBatch;
        ++event->batch_count;
    }
    const GwpSubscriptionId subscription = subscriber.id;
    event->subscribers.push_back(std::move(subscriber));
    ++event->native_count;
    bus.native_subscriptions.emplace(subscription, id);
    return subscription;
}

GwpSubscriptionId GWP_CALL ApiEventSubscribe(const GwpPlugin* self, const char* name, GwpEventHandler handler, void* user)
{
    if (!CheckMainThread(self, "event_subscribe"))
        return GWP_INVALID_SUBSCRIPTION_ID;
    const GwpSubscribeOptions options{};
    return SubscribeNative(self, name, options, handler, user);
}

GwpSubscriptionId GWP_CALL ApiEventSubscribeEx(const GwpPlugin* self, const char* name,
    const GwpSubscribeOptions* options, GwpEventHandler handler, void* user)
{
    if (!CheckMainThread(self, "event_subscribe_ex"))
        return GWP_INVALID_SUBSCRIPTION_ID;
    // Copy only what the plugin knows: a plugin built with an older header has a shorter struct.
    GwpSubscribeOptions copy{};
    if (options)
    {
        if (options->size < sizeof(uint32_t))
            return GWP_INVALID_SUBSCRIPTION_ID;
        memcpy(&copy, options, std::min<size_t>(options->size, sizeof(copy)));
    }
    return SubscribeNative(self, name, copy, handler, user);
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

GwpResult GWP_CALL ApiEventDeclareEx(const GwpPlugin* self, const char* name, const char* schema, uint32_t flags)
{
    if (!CheckMainThread(self, "event_declare_ex"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Event* event = FindEvent(GetBus(), InternIn(GetBus(), name));
    if (!event)
        return GWP_ERROR_INVALID_ARGUMENT;
    string128 who;
    xr_sprintf(who, "plugin:%s", PluginAddonId(self));
    if (schema && !IsValidSchema(schema))
    {
        SetSchema(*event, schema, who); // logs the invalid schema
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    event->declared = true;
    event->flags |= flags;
    SetSchema(*event, schema, who);
    return GWP_OK;
}

const char* GWP_CALL ApiEventSchema(GwpEventId id)
{
    if (!g_bus || !IsMainThread())
        return nullptr;
    const Event* event = FindEvent(*g_bus, id);
    return event && event->has_schema ? event->schema.c_str() : nullptr;
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
    EmitFrom(id, argv, argc, result, who, false);
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
        { "schema", &LuaSchema },
        { "set_lua_adapter", &LuaSetLuaAdapter },
        { "is_engine_source", &LuaIsEngineSource },
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
void DispatchLuaFromNative(Bus& bus, lua_State* L, Event& event, const GwpValue* argv, u32 argc, GwpValue* result,
    bool use_adapter);

void DeliverBatch(Bus& bus, Event& event, GwpEventId id, size_t index, u32 now, bool force)
{
    Subscriber& subscriber = event.subscribers[index];
    if (!subscriber.alive || !subscriber.batch_handler || subscriber.batch->empty())
        return;
    ZoneScopedN("events/plugin_batch"); // Tracy: one batch delivery, text = addon id, event and record count
    ZoneTextF("%s: %s x%u", PluginAddonId(subscriber.plugin), event.name.c_str(), static_cast<u32>(subscriber.batch->first.size()));
    if (subscriber.throttle_ms && !force)
    {
        if (subscriber.called && now - subscriber.last_call_ms < subscriber.throttle_ms)
            return;
    }
    subscriber.called = true;
    subscriber.last_call_ms = now;

    // Take the records out: the handler may emit this event again (records go into a fresh queue) or unsubscribe.
    BatchQueue queue;
    std::swap(queue, *subscriber.batch);
    NativeCall call;
    call.batch_handler = subscriber.batch_handler;
    call.user = subscriber.user;
    call.event_name = event.name.c_str();
    const GwpPlugin* const plugin = subscriber.plugin;

    for (GwpValue& value : queue.values)
    {
        if (value.type == GWP_T_STRING)
        {
            value.u.s.ptr = queue.text.data() + value.reserved;
            value.reserved = 0;
        }
    }
    xr_vector<GwpEvent> events(queue.first.size());
    for (size_t i = 0; i < events.size(); ++i)
        events[i] = GwpEvent{ id, event.name.c_str(), queue.argc[i], queue.values.data() + queue.first[i], nullptr };
    call.events = events.data();
    call.count = static_cast<u32>(events.size());

    if (!CallNativeGuarded(call))
    {
        Msg("! [plugin:%s] crashed in a batch handler of event '%s', all its event subscriptions are removed",
            PluginAddonId(plugin), event.name.c_str());
        RemovePluginSubscriptionsIn(bus, plugin);
    }
}
} // namespace

void FlushBatches(bool force)
{
    if (!g_bus || !IsMainThread())
        return;
    ZoneScopedN("events/flush_batches"); // Tracy: per-frame batch delivery (CGamePersistent::OnFrame)
    Bus& bus = *g_bus;
    const u32 now = Device.dwTimeGlobal;
    for (size_t e = 0; e < bus.events.size(); ++e)
    {
        Event& event = *bus.events[e];
        if (!event.batch_count)
            continue;
        DispatchScope scope(bus, event, nullptr);
        const size_t count = event.subscribers.size();
        for (size_t i = 0; i < count; ++i)
            DeliverBatch(bus, event, static_cast<GwpEventId>(e + 1), i, now, force);
    }
}

GwpEventId Intern(pcstr name) { return InternIn(GetBus(), name); }

void Declare(pcstr name, u32 flags, pcstr schema)
{
    if (Event* event = FindEvent(GetBus(), Intern(name)))
    {
        event->declared = true;
        event->flags |= flags;
        SetSchema(*event, schema, "engine");
    }
}

void DeclareEngineSource(pcstr name, u32 group)
{
    if (Event* event = FindEvent(GetBus(), Intern(name)))
    {
        event->declared = true;
        event->engine_source = true;
        event->engine_group = group;
    }
}

void SetActiveEngineGroups(u32 mask) { g_active_engine_groups = mask; }
void SetSchemaCheck(int mode) { g_schema_check = mode < 0 ? -1 : mode ? 1 : 0; }
int GetSchemaCheck() { return g_schema_check; }
void SetEngineLog(int mode) { g_engine_log = mode < 0 ? -1 : mode ? 1 : 0; }
int GetEngineLog() { return g_engine_log; }

bool SetTrace(pcstr name, bool on)
{
    Event* event = FindEvent(GetBus(), Intern(name));
    if (!event)
        return false;
    event->trace = on;
    return true;
}

bool HasSubscribers(GwpEventId id)
{
    const Event* event = g_bus ? FindEvent(*g_bus, id) : nullptr;
    return event && (event->native_count || event->lua_count);
}

namespace
{
// source: "engine" or "plugin:<id>". The Lua adapter is applied to engine emits only: a plugin emitting an event
// passes what it has, there is nothing to translate.
void EmitFrom(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result, pcstr source, bool from_engine);
} // namespace

void Emit(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result)
{
    EmitFrom(id, argv, argc, result, "engine", true);
}

namespace
{
void EmitFrom(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result, pcstr source, bool from_engine)
{
    ZoneScopedN("events/emit"); // Tracy: the whole emit (schema check, log, native and Lua dispatch)
    if (!g_bus)
        return;
    Bus& bus = *g_bus;
    Event* event = FindEvent(bus, id);
    if (event)
        ZoneTextF("%s%s", event->name.c_str(), from_engine ? " (engine)" : "");
    if (!IsMainThread())
    {
        // The name is read without a lock: events are declared at script start, on the logic thread, and never removed.
        Msg("! [events] event %u '%s' emitted outside the game logic thread, ignored", id, event ? event->name.c_str() : "?");
        return;
    }
    if (!event)
        return;
    ++event->emit_count;
    CheckSchema(*event, argv, argc, source);
    if (event->trace)
        Trace(*event, argv, argc, source);
    else if (from_engine && IsEngineSourceActive(*event) && (g_engine_log < 0 ? IsDebugLog() : g_engine_log != 0))
    {
        // gw_event_engine_log: every event moved to the engine (stage B) shows that it fires, with its arguments.
        // Frequent ones (on_key_hold, hits, hud animations) are rate-limited per event.
        const u32 now = Device.dwTimeGlobal;
        if (!event->engine_log_any || now - event->engine_log_ms >= kEngineLogIntervalMs)
        {
            string64 skipped;
            skipped[0] = 0;
            if (event->engine_log_skipped)
                xr_sprintf(skipped, " (+%u not logged)", event->engine_log_skipped);
            Msg("  [events] engine emit '%s' #%u: %s%s", event->name.c_str(), event->emit_count,
                DescribeArgs(argv, argc).c_str(), skipped);
            event->engine_log_any = true;
            event->engine_log_ms = now;
            event->engine_log_skipped = 0;
        }
        else
            ++event->engine_log_skipped;
    }
    if (event->subscribers.empty())
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
        DispatchLuaFromNative(bus, L, *event, argv, argc, result, from_engine);

    if (event->native_count)
        DispatchNative(bus, *event, id, argv, argc, result);
}
} // namespace

namespace
{
void DispatchLuaFromNative(Bus& bus, lua_State* L, Event& event, const GwpValue* argv, u32 argc, GwpValue* result,
    bool use_adapter)
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
    // Lua adapter (event_bus.set_lua_adapter): Lua subscribers get what it returns, e.g. the actor binder that the
    // engine does not know. Called once per emit; on an error the native arguments are used.
    if (use_adapter && event.lua_adapter_ref != LUA_NOREF && lua_checkstack(L, static_cast<int>(argc) + 4))
    {
        const int before = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, event.lua_adapter_ref);
        for (u32 i = 0; i < argc; ++i)
            PushValue(L, argv[i]);
        if (lua_pcall(L, static_cast<int>(argc), LUA_MULTRET, 0) == 0)
        {
            args.argv = nullptr;
            args.argc = 0;
            args.first = before + 1;
            args.count = lua_gettop(L) - before;
        }
        else
        {
            if (!event.warned_adapter)
            {
                event.warned_adapter = true;
                const char* error = lua_tostring(L, -1);
                Msg("! [events] Lua adapter of '%s' failed: %s", event.name.c_str(), error ? error : "?");
            }
            lua_settop(L, before);
        }
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
    api.event_subscribe_ex = &ApiEventSubscribeEx;
    api.event_declare_ex = &ApiEventDeclareEx;
    api.event_schema = &ApiEventSchema;
}

void PushLuaValue(lua_State* L, const GwpValue& value) { PushValue(L, value); }
GwpValue LuaToValue(lua_State* L, int index) { return ToValue(L, index); }

void MarkLevelChange() { g_level_change_mark = true; }

bool TakeLevelChangeMark()
{
    const bool mark = g_level_change_mark;
    g_level_change_mark = false;
    return mark;
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
    u32 declared = 0, with_subscribers = 0, engine = 0;
    Msg("- [events] events with subscribers or emits: name, lua / native subscribers, source, emits, schema");
    for (const auto& event : g_bus->events)
    {
        declared += event->declared ? 1 : 0;
        engine += IsEngineSourceActive(*event) ? 1 : 0;
        with_subscribers += event->lua_count || event->native_count ? 1 : 0;
        if (!event->lua_count && !event->native_count && !event->emit_count)
            continue;
        Msg("-   %-40s %3u / %-3u %-6s %8u  %s%s%s%s", event->name.c_str(), event->lua_count, event->native_count,
            event->builtin || IsEngineSourceActive(*event) ? "engine" : "lua", event->emit_count,
            event->has_schema ? (event->schema.empty() ? "()" : event->schema.c_str()) : "-",
            event->batch_count ? " (batch)" : "", event->trace ? " (trace)" : "",
            event->declared ? "" : " (undeclared)");
    }
    Msg("- [events] %u event(s) known, %u declared, %u with subscribers, %u moved to the engine (stage B)",
        static_cast<u32>(g_bus->events.size()), declared, with_subscribers, engine);
}
} // namespace gw::addons::events
