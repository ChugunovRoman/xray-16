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
#include <atomic>
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
    xr_vector<GwpValue> values; // arguments of every record: deep copies, their strings and items live in `arena`
    xr_vector<u32> first;       // per record: index of its first argument in `values`
    xr_vector<u32> argc;        // per record: number of arguments
    xr_vector<u32> flags;       // per record: GwpEvent::flags (GWP_EVENT_FLAG_*)
    ValueArena arena;           // copies of strings, bytes and array items; addresses stable until the queue goes
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
    bool object_filter = false; // GWP_SUBSCRIBE_OBJECT / Lua {object = ...}: only emits about `object` (first argument)
    u16 object = 0;

    // Lua
    int lua_ref = LUA_NOREF; // handler in the registry of Bus::lua
    bool once = false;       // Lua {once = true}: removed right before its first call
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
    // Who actually emitted it (event_list): C++ of the engine and the host (Emit), Lua (event_bus.emit,
    // SendScriptCallback), plugins (event_emit). The declared source alone says nothing about host emits
    u32 engine_emits = 0;
    u32 lua_emits = 0;
    u32 plugin_emits = 0;
    u32 engine_log_ms = 0;      // gw_event_engine_log: time of the last logged emit
    u32 engine_log_skipped = 0; // emits not logged since then (rate limit)
    bool engine_log_any = false;
    int lua_adapter_ref = LUA_NOREF; // _gw_internal.event_bus_set_lua_adapter: Lua arguments of an engine emit
};

struct Bus
{
    xr_vector<xr_unique_ptr<Event>> events; // index = id - 1; Event objects never move
    xr_unordered_map<xr_string, GwpEventId> ids;
    xr_unordered_map<GwpSubscriptionId, GwpEventId> native_subscriptions;
    xr_unordered_map<GwpSubscriptionId, GwpEventId> lua_subscriptions; // event_bus.unsubscribe(id)
    GwpSubscriptionId next_subscription = 1; // one id space for Lua and native subscriptions
    u32 object_subscriptions = 0; // alive GWP_SUBSCRIBE_OBJECT subscribers: RemoveObjectSubscriptions skips the scan at 0
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
    "service_on_register",
    "service_on_unregister",
    "server_object_on_register",
    "server_object_on_unregister",
    "npc_on_before_hit",
    "monster_on_before_hit",
    "relation_on_changed",
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
    "sI", // service_on_register (name, version)
    "s",  // service_on_unregister (name)
    "Oss", // server_object_on_register (server object, section, class id)
    "Oss", // server_object_on_unregister (server object, section, class id)
    "oNvo?IIN", // npc_on_before_hit (npc, power, dir, who, bone, hit type, impulse)
    "oNvo?IIN", // monster_on_before_hit (monster, power, dir, who, bone, hit type, impulse)
    "s*?*?N?N?", // relation_on_changed (kind, a, b, old, new)
};
static_assert(std::size(kBuiltinSchemas) == std::size(kBuiltinNames), "kBuiltinSchemas != kBuiltinNames");

// Built-in events with a result (GWP_EVENT_HAS_RESULT): the engine emits them with a result value.
constexpr pcstr kBuiltinResultEvents[] = {
    "npc_on_before_hit",
    "monster_on_before_hit",
};

GwpEventId InternIn(Bus& bus, pcstr name);
GwpResult EmitFrom(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result, pcstr source, bool from_engine);

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
            for (const pcstr name : kBuiltinResultEvents)
            {
                if (xr_strcmp(name, kBuiltinNames[i]) == 0)
                    event.flags |= GWP_EVENT_HAS_RESULT;
            }
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
    case 't': return value.type == GWP_T_LUA_REF || value.type == GWP_T_ARRAY; // a Lua table: an array or not
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
        case GWP_T_BYTES: xr_sprintf(item, "bytes [%u]", v.u.s.len); break;
        case GWP_T_ARRAY: xr_sprintf(item, "array [%u]", v.u.a.count); break;
        case GWP_T_NIL: xr_strcpy(item, "nil"); break;
        default: xr_sprintf(item, "unknown type %u", v.type); break;
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

// The Lua state of the subscribers while it is the live state of the script engine; nullptr once it is closed or
// replaced (its registry references must not be touched then).
lua_State* LiveLua(const Bus& bus)
{
    return bus.lua && GEnv.ScriptEngine && GEnv.ScriptEngine->lua() == bus.lua ? bus.lua : nullptr;
}

// Removes a subscriber. During a dispatch of this event the entry is only marked, so indices stay valid.
// A Lua handler loses its registry reference here (the caller keeps the function on the stack if it still calls it).
void RemoveAt(Bus& bus, Event& event, size_t index)
{
    Subscriber& subscriber = event.subscribers[index];
    if (!subscriber.alive)
        return;
    subscriber.alive = false;
    if (subscriber.object_filter && bus.object_subscriptions)
        --bus.object_subscriptions;
    if (subscriber.kind == ESubscriberKind::Native)
    {
        --event.native_count;
        if (subscriber.batch_handler)
            --event.batch_count;
        bus.native_subscriptions.erase(subscriber.id);
    }
    else
    {
        --event.lua_count;
        bus.lua_subscriptions.erase(subscriber.id);
        if (subscriber.lua_ref != LUA_NOREF)
        {
            if (lua_State* L = LiveLua(bus))
                luaL_unref(L, LUA_REGISTRYINDEX, subscriber.lua_ref);
            subscriber.lua_ref = LUA_NOREF;
        }
    }

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

// One call of a plugin handler: a single event (handler), a batch (batch_handler), or any other plugin function
// wrapped by a host thunk (CallPluginGuarded: binder callbacks and the like).
struct NativeCall
{
    GwpEventHandler handler = nullptr;
    GwpEventBatchHandler batch_handler = nullptr;
    void* user = nullptr;
    const GwpEvent* events = nullptr;
    u32 count = 1;
    pcstr event_name = ""; // what is being called, for the crash message
    void (*thunk)(void*) = nullptr; // set: called instead of the handlers
    void* thunk_context = nullptr;
    const void* plugin_code = nullptr; // thunk: an address inside the plugin library (the crash filter needs it)
};

// Plugin calls in progress: see PluginCallDepth. Atomic: task_parallel_for (addon_api_threads.cpp) runs plugin code
// under the same guard on worker threads, while the game logic thread waits inside that plugin call
std::atomic<u32> g_plugin_call_depth{ 0 };

// A C++ exception thrown out of a plugin handler stops here; C++ unwinding runs every destructor on the way,
// so nested dispatch scopes of the engine stay consistent. Logged with the stack of the catch (on MSVC the frames
// of the throw are still below it while the catch block runs).
bool CallNativeCatching(const NativeCall& call)
{
    try
    {
        if (call.thunk)
            call.thunk(call.thunk_context);
        else if (call.batch_handler)
            call.batch_handler(call.user, call.count, call.events, static_cast<uint32_t>(sizeof(GwpEvent)));
        else
            call.handler(call.user, call.events);
        return true;
    }
    catch (const std::exception& e)
    {
        Msg("! [plugins] C++ exception out of a plugin function (%s): %s", call.event_name, e.what());
        xrDebug::LogStackTrace("! [plugins] stack of the exception:");
        return false;
    }
    catch (...)
    {
        Msg("! [plugins] C++ exception of an unknown type out of a plugin function (%s)", call.event_name);
        xrDebug::LogStackTrace("! [plugins] stack of the exception:");
        return false;
    }
}

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
// Crashes (access violation and other hardware exceptions) are caught only when they happen inside the plugin
// library itself: an engine crash under a plugin handler keeps going to the normal crash handler, and no engine
// frame with destructors is skipped (the engine is built with /EHsc, SEH unwinding does not run destructors).
// The filter runs before the stack is unwound: the crash block written here (xrDebug::LogCrashInfoGuarded: code,
// module + offset, modules, native and Lua stacks) shows the frames inside the plugin, named when its PDB is found
// (addon_host.cpp puts the cache folder of the plugin copies into the symbol search path). oneShot = false: the game
// goes on, a later real crash still gets its own block. Not for a stack overflow: too little stack is left to walk.
void LogPluginCrash(EXCEPTION_POINTERS* info, HMODULE plugin_module, pcstr what)
{
    string_path module_path{};
    GetModuleFileNameA(plugin_module, module_path, sizeof(module_path));
    pcstr module_name = strrchr(module_path, '\\');
    module_name = module_name ? module_name + 1 : module_path;
    string512 reason;
    xr_sprintf(reason, "plugin library %s crashed in %s; caught, the game goes on without the plugin", module_name,
        what ? what : "?");
    xrDebug::LogCrashInfoGuarded(info, reason, false);
}

// The short line after the crash block: the module + offset of the fault (symbolized offline with the PDB)
void LogPluginException(DWORD code, const void* address, pcstr what)
{
    HMODULE module = nullptr;
    string_path module_path{};
    pcstr module_name = "?";
    uintptr_t offset = reinterpret_cast<uintptr_t>(address);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module) &&
        module && GetModuleFileNameA(module, module_path, sizeof(module_path)))
    {
        const pcstr slash = strrchr(module_path, '\\');
        module_name = slash ? slash + 1 : module_path;
        offset -= reinterpret_cast<uintptr_t>(module);
    }
    Msg("! [events] exception 0x%08x at %s+0x%08llx in a plugin function (%s)%s", static_cast<unsigned>(code),
        module_name, static_cast<unsigned long long>(offset), what ? what : "?",
        code == EXCEPTION_STACK_OVERFLOW ? ": a stack overflow, no stack trace" : "");
}

HMODULE ModuleOfAddress(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module))
        return nullptr;
    return module;
}

// The C/C++ runtime a plugin calls directly: memcpy, strlen and the like fault there on a bad argument of the plugin
bool IsRuntimeModule(HMODULE module)
{
    string_path path{};
    if (!GetModuleFileNameA(module, path, sizeof(path)))
        return false;
    const pcstr slash = strrchr(path, '\\');
    const pcstr name = slash ? slash + 1 : path;
    return !_strnicmp(name, "vcruntime", 9) || !_strnicmp(name, "ucrtbase", 8) || !_strnicmp(name, "msvcp", 5);
}

// The fault is the plugin's: its own code faulted, or the first frame outside the runtime is in its module - a call
// through a null or dangling function pointer (the fault address is no code at all) or a bad argument to the
// runtime. The frames above are found with the unwind data (x64); a fault in the engine stays the engine's.
bool FaultOfPlugin(const EXCEPTION_POINTERS* info, HMODULE plugin_module, DWORD code)
{
    HMODULE module = ModuleOfAddress(info->ExceptionRecord->ExceptionAddress);
    if (module == plugin_module)
        return true;
#if defined(_M_X64)
    // Not for a stack overflow: too little stack is left to walk
    if (code == EXCEPTION_STACK_OVERFLOW || (module && !IsRuntimeModule(module)))
        return false;
    CONTEXT context = *info->ContextRecord;
    for (int frame = 0; frame < 8; ++frame)
    {
        DWORD64 image_base = 0;
        if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr))
        {
            void* handler_data = nullptr;
            DWORD64 establisher_frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                &establisher_frame, nullptr);
        }
        else
        {
            // A leaf function, or no code at all (the call through a bad pointer): the return address is on top
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        module = context.Rip ? ModuleOfAddress(reinterpret_cast<const void*>(context.Rip)) : nullptr;
        if (module == plugin_module)
            return true;
        if (!module || !IsRuntimeModule(module))
            return false;
    }
#else
    (void)code;
#endif
    return false;
}

int PluginCrashFilter(EXCEPTION_POINTERS* info, HMODULE plugin_module, pcstr what, DWORD& code, void*& address)
{
    code = info->ExceptionRecord->ExceptionCode;
    address = info->ExceptionRecord->ExceptionAddress;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP || !plugin_module)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!FaultOfPlugin(info, plugin_module, code))
        return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_STACK_OVERFLOW)
        LogPluginCrash(info, plugin_module, what);
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects with destructors here: __try cannot be mixed with them in one function.
bool CallNativeGuardedSeh(const NativeCall& call)
{
    const void* code_address = call.thunk ? call.plugin_code
        : call.batch_handler              ? reinterpret_cast<const void*>(call.batch_handler)
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
    __except (PluginCrashFilter(GetExceptionInformation(), plugin_module, call.event_name, code, address))
    {
        if (code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        LogPluginException(code, address, call.event_name);
        return false;
    }
}
#else
bool CallNativeGuardedSeh(const NativeCall& call) { return CallNativeCatching(call); }
#endif

// The depth comes back down also when something unwinds through here (a fault the guard leaves to an outer one):
// a depth stuck above zero would stop every unload and reload of the plugins for the rest of the run
struct CallDepthScope
{
    CallDepthScope() { ++g_plugin_call_depth; }
    ~CallDepthScope() { --g_plugin_call_depth; }
    CallDepthScope(const CallDepthScope&) = delete;
    CallDepthScope& operator=(const CallDepthScope&) = delete;
};

bool CallNativeGuarded(const NativeCall& call)
{
    const CallDepthScope depth;
    return CallNativeGuardedSeh(call);
}

} // namespace

u32 PluginCallDepth() { return g_plugin_call_depth.load(); }

bool CallPluginGuarded(const void* plugin_code, pcstr what, void (*fn)(void*), void* context)
{
    NativeCall call;
    call.thunk = fn;
    call.thunk_context = context;
    call.plugin_code = plugin_code;
    call.event_name = what ? what : "?";
    return CallNativeGuarded(call);
}

namespace
{
void Enqueue(Subscriber& subscriber, const Event& event, const GwpValue* argv, u32 argc, u32 flags)
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
    queue.flags.push_back(flags);
    // Deep copies: strings, bytes and array items go to the arena of the queue, their addresses never move
    for (u32 i = 0; i < argc; ++i)
        queue.values.push_back(queue.arena.Copy(argv[i]));
}

// GWP_SUBSCRIBE_OBJECT: the first argument names the object (a game object or its server object).
bool IsAboutObject(const GwpValue* argv, u32 argc, u16 object)
{
    return argc > 0 && (argv[0].type == GWP_T_OBJECT || argv[0].type == GWP_T_SERVER_OBJECT) &&
        argv[0].u.id == object;
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

GwpEvent MakeEvent(GwpEventId id, const Event& event, u32 flags, const GwpValue* argv, u32 argc, GwpValue* result)
{
    GwpEvent data;
    data.size = sizeof(GwpEvent);
    data.flags = flags;
    data.id = id;
    data.argc = argc;
    data.name = event.name.c_str();
    data.argv = argv;
    data.result = result;
    return data;
}

// flags: GwpEvent::flags of this emit (GWP_EVENT_FLAG_ENGINE for an emit of the engine itself)
void DispatchNative(Bus& bus, Event& event, GwpEventId id, u32 flags, const GwpValue* argv, u32 argc, GwpValue* result)
{
    const GwpEvent data = MakeEvent(id, event, flags, argv, argc, result);
    u32 now = 0;
    bool now_set = false;
    // Index loop: handlers may subscribe (push_back) or unsubscribe (marked only) during the dispatch.
    const size_t count = event.subscribers.size();
    for (size_t i = 0; i < count; ++i)
    {
        Subscriber& subscriber = event.subscribers[i];
        if (!subscriber.alive || subscriber.kind != ESubscriberKind::Native)
            continue;
        if (subscriber.object_filter && !IsAboutObject(argv, argc, subscriber.object))
            continue;
        if (subscriber.batch_handler)
        {
            Enqueue(subscriber, event, argv, argc, flags); // delivered by FlushBatches; throttle applies there
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
            string256 what;
            xr_sprintf(what, "a handler of event '%s'", event.name.c_str());
            OnPluginCrashed(plugin, what); // every subscription of the plugin goes, the whole plugin stops
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
    // The first argument of the emit before any Lua adapter, for Lua subscribers with {object = ...}: a native
    // emit sets subject_argv (its argv), a Lua emit leaves it null (then the stack value at `first` is used).
    const GwpValue* subject_argv = nullptr;
    u32 subject_argc = 0;
};

CScriptGameObject* FindScriptObject(u16 id)
{
    if (!g_pGameLevel)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object ? object->lua_game_object() : nullptr;
}

void PushValueDepth(lua_State* L, const GwpValue& value, u32 depth);

// The value budget (addon_event_bus.h) of one value so far: its values (kMaxValueNodes: the value itself plus the
// items of every array) and the bytes of its strings and bytes (kMaxValueTotalBytes), an array or a string met
// several times counted every time.
struct ValueBudget
{
    u32 nodes = 1; // the value itself
    u64 bytes = 0;
    bool Over() const { return nodes > kMaxValueNodes || bytes > kMaxValueTotalBytes; }
};

// Counts the tree into `budget`. The walk stops as soon as it is over the budget, so an array shared by many items
// (a DAG) costs at most about kMaxValueNodes steps. An array over its own limits adds nothing: the callers refuse
// or cut it themselves.
void CountValueBudgetDepth(const GwpValue& value, u32 depth, ValueBudget& budget)
{
    if (value.type == GWP_T_STRING || value.type == GWP_T_BYTES)
    {
        budget.bytes += value.u.s.len;
        return;
    }
    const GwpArray& array = value.u.a;
    if (value.type != GWP_T_ARRAY || depth >= kMaxArrayDepth || array.count > kMaxArrayItems ||
        (array.count && !array.items))
        return;
    budget.nodes += array.count;
    for (u32 i = 0; i < array.count && !budget.Over(); ++i)
        CountValueBudgetDepth(array.items[i], depth + 1, budget);
}

// true when an array value with everything in it is within the value budget. Any other value is: only arrays
// multiply what a copy or a push makes (a single string is limited where it comes in, kMaxValueBytes)
bool ValueWithinBudget(const GwpValue& value)
{
    if (value.type != GWP_T_ARRAY)
        return true;
    ValueBudget budget;
    CountValueBudgetDepth(value, 0, budget);
    return !budget.Over();
}

// One log line per place (`warned` is a static of the caller) for a value over the value budget
void WarnValueOverBudget(bool& warned, pcstr what)
{
    if (warned)
        return;
    warned = true;
    Msg("! [events] a value with more than %u values or %u MiB of strings in total (nested arrays included, a shared "
        "array or string counted each time): %s (reported once)", kMaxValueNodes,
        static_cast<u32>(kMaxValueTotalBytes / (1024u * 1024u)), what);
}

// GWP_T_ARRAY -> a new table {items[0], ..., items[count - 1]}. depth: arrays around this one.
void PushArray(lua_State* L, const GwpValue& value, u32 depth)
{
    const GwpArray& array = value.u.a;
    if (depth >= kMaxArrayDepth || array.count > kMaxArrayItems || (array.count && !array.items) ||
        !lua_checkstack(L, 3))
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Msg("! [events] an array deeper than %u levels, longer than %u items or without its items went to Lua as "
                "nil (reported once)", kMaxArrayDepth, kMaxArrayItems);
        }
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, static_cast<int>(array.count), 0);
    for (u32 i = 0; i < array.count; ++i)
    {
        PushValueDepth(L, array.items[i], depth + 1);
        lua_rawseti(L, -2, static_cast<int>(i + 1)); // nil (an object gone offline) leaves a hole
    }
}

void PushValue(lua_State* L, const GwpValue& value)
{
    // Checked once at the root: a shared row repeated through every level would make billions of tables
    if (!ValueWithinBudget(value))
    {
        static bool warned = false;
        WarnValueOverBudget(warned, "went to Lua as nil");
        lua_pushnil(L);
        return;
    }
    PushValueDepth(L, value, 0);
}

void PushValueDepth(lua_State* L, const GwpValue& value, u32 depth)
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
    case GWP_T_BYTES: // Lua strings hold any bytes
        if (value.u.s.ptr)
            lua_pushlstring(L, value.u.s.ptr, value.u.s.len);
        else if (!value.u.s.len)
            lua_pushliteral(L, "");
        else
            lua_pushnil(L);
        break;
    case GWP_T_ARRAY: PushArray(L, value, depth); break;
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

GwpValue LuaRef()
{
    GwpValue value = Nil();
    value.type = GWP_T_LUA_REF;
    return value;
}

GwpValue ToValueDepth(lua_State* L, int index, ValueArena* arena, u32 depth, ValueBudget& budget);

// A Lua table over the value budget: logged once for all of them
GwpValue TableOverBudget()
{
    static bool warned = false;
    WarnValueOverBudget(warned, "a Lua table has no native form, GWP_T_LUA_REF");
    return LuaRef(); // every table around it becomes GWP_T_LUA_REF too: an item without a native form
}

// The table at the absolute stack index -> GWP_T_ARRAY when its keys are exactly 1..n (n <= kMaxArrayItems) and
// every item has a native form (nested tables by the same rule), else GWP_T_LUA_REF. Items go to the arena; their
// strings point into the strings the table holds. depth: arrays around this one. budget: the value budget of the
// whole value so far (a table or a string met twice counts twice; the items of a table are reserved before the
// recursion).
GwpValue TableToValue(lua_State* L, int index, ValueArena& arena, u32 depth, ValueBudget& budget)
{
    if (depth >= kMaxArrayDepth || !lua_checkstack(L, 4))
        return LuaRef();
    // Keys: distinct integers in 1..kMaxArrayItems, as many as the largest of them -> exactly 1..n
    u32 count = 0;
    lua_Number max_key = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0)
    {
        lua_pop(L, 1); // the item; the key stays for lua_next
        bool ok = lua_type(L, -1) == LUA_TNUMBER;
        if (ok)
        {
            const lua_Number key = lua_tonumber(L, -1);
            ok = key >= 1 && key <= kMaxArrayItems && std::floor(key) == key;
            if (ok && key > max_key)
                max_key = key;
        }
        if (!ok || ++count > kMaxArrayItems)
        {
            lua_pop(L, 1); // the key: the iteration stops here
            return LuaRef();
        }
    }
    if (static_cast<lua_Number>(count) != max_key)
        return LuaRef();
    budget.nodes += count;
    if (budget.Over())
        return TableOverBudget();

    GwpValue* items = arena.AllocValues(count);
    for (u32 i = 0; i < count; ++i)
    {
        lua_rawgeti(L, index, static_cast<int>(i + 1));
        items[i] = ToValueDepth(L, lua_gettop(L), &arena, depth + 1, budget);
        lua_pop(L, 1); // a string item stays alive: the table holds it
        if (items[i].type == GWP_T_LUA_REF || items[i].type == GWP_T_NIL)
            return LuaRef(); // a function, a foreign userdata, a table that is no array: no native form
        if (budget.Over()) // the bytes of a string item
            return TableOverBudget();
    }
    GwpValue value = Nil();
    value.type = GWP_T_ARRAY;
    value.u.a.items = items;
    value.u.a.count = count;
    return value;
}

// Lua value -> GwpValue. Strings point into Lua data: valid while the value stays on the stack.
// arena == nullptr: tables are GWP_T_LUA_REF (the conversion before arrays).
GwpValue ToValue(lua_State* L, int index, ValueArena* arena = nullptr)
{
    ValueBudget budget; // per value
    return ToValueDepth(L, index, arena, 0, budget);
}

GwpValue ToValueDepth(lua_State* L, int index, ValueArena* arena, u32 depth, ValueBudget& budget)
{
    if (index < 0 && index > LUA_REGISTRYINDEX)
        index = lua_gettop(L) + index + 1; // absolute: TableToValue pushes onto the stack
    switch (lua_type(L, index))
    {
    case LUA_TTABLE:
        if (arena)
            return TableToValue(L, index, *arena, depth, budget);
        break;
    case LUA_TBOOLEAN: return Bool(lua_toboolean(L, index) != 0);
    case LUA_TNUMBER: return Number(lua_tonumber(L, index)); // Lua has only doubles: always NUMBER
    case LUA_TSTRING:
    {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        budget.bytes += length; // matters inside a table only: TableToValue checks it
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
    // {object = ...} subscribers: the first argument of the emit as GwpValue, converted once on the first need
    GwpValue subject = Nil();
    u32 subject_count = 0;
    bool subject_set = false;
    const size_t count = event.subscribers.size();
    for (size_t i = 0; i < count; ++i)
    {
        Subscriber& subscriber = event.subscribers[i];
        if (!subscriber.alive || subscriber.kind == ESubscriberKind::Native)
            continue;
        if (subscriber.object_filter)
        {
            if (!subject_set)
            {
                subject_set = true;
                if (args.subject_argv || args.argv)
                {
                    const GwpValue* argv = args.subject_argv ? args.subject_argv : args.argv;
                    subject_count = args.subject_argv ? args.subject_argc : args.argc;
                    if (subject_count)
                        subject = argv[0];
                }
                else if (args.count > 0)
                {
                    subject = ToValue(L, args.first); // a game object or a server object (userdata): no arena needed
                    subject_count = 1;
                }
            }
            if (!IsAboutObject(&subject, subject_count, subscriber.object))
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

        // Copies: any Lua call below may subscribe and reallocate the vector, `subscriber` would dangle.
        const ESubscriberKind kind = subscriber.kind;
        const int lua_ref = subscriber.lua_ref;
        const bool once = subscriber.once;

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
        // {once = true}: gone before the call (a nested emit of this event does not call it again); the handler is
        // on the stack already, so dropping its registry reference here is safe
        if (once)
            RemoveAt(bus, event, i);

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
    // The old Lua state is already closed: drop the references without luaL_unref. bus.lua is cleared first, so
    // RemoveAt does not unref them (a new state may even have the address of the old one).
    bus.lua = nullptr;
    bus.current_lua = nullptr;
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

// The id of the name at `index` when the bus knows the event already, GWP_INVALID_EVENT_ID otherwise; never
// registers it. For the functions that only look (schema, has_subscribers, unsubscribe by name): a name built at
// run time must not grow bus.events forever. Uses the same name -> id cache as LuaEventId.
GwpEventId LuaFindEventId(lua_State* L, int index)
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
    const Bus& bus = GetBus();
    const auto it = bus.ids.find(lua_tostring(L, index));
    if (it == bus.ids.end())
        return GWP_INVALID_EVENT_ID;
    lua_pushvalue(L, index);
    lua_pushnumber(L, static_cast<lua_Number>(it->second));
    lua_rawset(L, lua_upvalueindex(1));
    return it->second;
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

// event_bus.schema(name): the argument schema, nil when the event has none (or the bus does not know it).
int LuaSchema(lua_State* L)
{
    const Event* event = FindEvent(GetBus(), LuaFindEventId(L, 1));
    if (event && event->has_schema)
        lua_pushlstring(L, event->schema.c_str(), event->schema.size());
    else
        lua_pushnil(L);
    return 1;
}

// _gw_internal.event_bus_set_lua_adapter(name, fn | nil): when the ENGINE emits the event, Lua subscribers get
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

// _gw_internal.event_bus_is_engine_source(name): true when the engine sends the event now (its stage B group is
// active), so a Lua sender must not do what the event handling does (e.g. the actor_before_death slot handler must
// not kill).
int LuaIsEngineSource(lua_State* L)
{
    const Event* event = FindEvent(GetBus(), LuaFindEventId(L, 1));
    lua_pushboolean(L, event && IsEngineSourceActive(*event) ? 1 : 0);
    return 1;
}

// Lua value -> GwpValue for every argument (strings point into the Lua stack, array items into `arena`).
// 16 arguments without an allocation.
struct ArgBuffer
{
    GwpValue inline_values[16];
    xr_vector<GwpValue> heap;
    GwpValue* data = inline_values;
    ValueArena arena; // items of the arrays: Lua tables with the keys 1..n

    void Fill(lua_State* L, int first, int count)
    {
        arena.Clear();
        if (count > static_cast<int>(std::size(inline_values)))
        {
            heap.resize(count);
            data = heap.data();
        }
        for (int i = 0; i < count; ++i)
            data[i] = ToValue(L, first + i, &arena);
    }

    // Before native handlers: the strings inside arrays point into the Lua tables, and a handler that calls Lua
    // (script_call) may change such a table and free them. Deep copies of the arrays (into the same arena: the
    // items it holds stay where they are) own their strings; the strings of the arguments themselves stay on the
    // Lua stack of the emit and need no copy.
    void OwnArrays(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if (data[i].type == GWP_T_ARRAY)
                data[i] = arena.Copy(data[i]);
        }
    }
};

// Options of event_bus.subscribe: the third argument, a number (throttle_ms) or a table.
struct LuaSubscribeOptions
{
    u32 throttle_ms = 0;
    bool object_filter = false;
    u16 object = 0;
    bool once = false;
};

u32 LuaThrottle(lua_State* L, int index)
{
    return lua_type(L, index) == LUA_TNUMBER && lua_tonumber(L, index) > 0 ?
        static_cast<u32>(lua_tonumber(L, index)) :
        0;
}

// false (logged) for a bad object: not a game object, a server object or an id, or not online now.
bool ReadLuaSubscribeOptions(lua_State* L, int index, const Event& event, LuaSubscribeOptions& options)
{
    if (lua_type(L, index) != LUA_TTABLE)
    {
        options.throttle_ms = LuaThrottle(L, index);
        return true;
    }
    lua_getfield(L, index, "throttle_ms");
    options.throttle_ms = LuaThrottle(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, index, "once");
    options.once = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, index, "object");
    bool ok = true;
    if (!lua_isnil(L, -1))
    {
        // A game object, a server object or an id: the same subject check as GWP_SUBSCRIBE_OBJECT
        const GwpValue value = ToValue(L, -1);
        lua_Number id = -1;
        if (value.type == GWP_T_OBJECT || value.type == GWP_T_SERVER_OBJECT)
            id = value.u.id;
        else if (value.type == GWP_T_NUMBER)
            id = value.u.n;
        // An object that is not online now: nothing would end the subscription (the end is its net_Destroy), and
        // its id may be given to another object
        ok = id >= 0 && id < GWP_INVALID_OBJECT_ID && std::floor(id) == id && g_pGameLevel &&
            Level().Objects.net_Find(static_cast<u16>(id));
        if (ok)
        {
            options.object_filter = true;
            options.object = static_cast<u16>(id);
        }
        else
            Msg("! [events] event_bus.subscribe('%s'): `object` is not an online object (a game object, a server "
                "object or an id), the subscription is not made", event.name.c_str());
    }
    lua_pop(L, 1);
    return ok;
}

void ApplyLuaSubscribeOptions(Bus& bus, Subscriber& subscriber, const LuaSubscribeOptions& options)
{
    if (subscriber.object_filter && bus.object_subscriptions)
        --bus.object_subscriptions;
    subscriber.object_filter = options.object_filter;
    subscriber.object = options.object;
    if (subscriber.object_filter)
        ++bus.object_subscriptions;
    subscriber.throttle_ms = options.throttle_ms;
    subscriber.once = options.once;
    subscriber.called = false;
}

// event_bus.subscribe(name, handler [, throttle_ms | {throttle_ms = N, object = <object or id>, once = true}]):
// handler is a function or an object with a method `name`. Returns the id of the subscription (a number), nil when
// nothing was subscribed. Subscribing the same handler to the same event with the same `object` (or again without
// one) keeps one subscription: its options are replaced and the same id comes back (scripts call
// RegisterScriptCallback again on every load). Another `object`, or none instead of one, is another subscription
// with its own id: one handler may follow several objects, and a plain re-registration does not end a {once} one.
int LuaSubscribe(lua_State* L)
{
    Bus& bus = GetBus();
    const GwpEventId event_id = LuaEventId(L, 1);
    Event* event = FindEvent(bus, event_id);
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

    LuaSubscribeOptions options;
    if (!ReadLuaSubscribeOptions(L, 3, *event, options))
        return 0;

    for (Subscriber& subscriber : event->subscribers)
    {
        if (!subscriber.alive || subscriber.kind == ESubscriberKind::Native ||
            subscriber.object_filter != options.object_filter ||
            (options.object_filter && subscriber.object != options.object))
            continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, subscriber.lua_ref);
        const bool same = lua_rawequal(L, -1, 2) != 0;
        lua_pop(L, 1);
        if (same)
        {
            ApplyLuaSubscribeOptions(bus, subscriber, options);
            lua_pushnumber(L, static_cast<lua_Number>(subscriber.id));
            return 1;
        }
    }

    Subscriber subscriber;
    subscriber.id = bus.next_subscription++;
    subscriber.kind = type == LUA_TFUNCTION ? ESubscriberKind::LuaFunction : ESubscriberKind::LuaObject;
    ApplyLuaSubscribeOptions(bus, subscriber, options);
    lua_pushvalue(L, 2);
    subscriber.lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    const GwpSubscriptionId id = subscriber.id;
    event->subscribers.push_back(std::move(subscriber));
    ++event->lua_count;
    bus.lua_subscriptions.emplace(id, event_id);
    lua_pushnumber(L, static_cast<lua_Number>(id));
    return 1;
}

// event_bus.unsubscribe(id) or event_bus.unsubscribe(name, handler): true when a subscription was removed.
int LuaUnsubscribe(lua_State* L)
{
    Bus& bus = GetBus();
    if (lua_type(L, 1) == LUA_TNUMBER)
    {
        const auto subscription = static_cast<GwpSubscriptionId>(lua_tonumber(L, 1));
        const auto it = bus.lua_subscriptions.find(subscription);
        Event* event = it != bus.lua_subscriptions.end() ? FindEvent(bus, it->second) : nullptr;
        if (event)
        {
            for (size_t i = 0; i < event->subscribers.size(); ++i)
            {
                const Subscriber& subscriber = event->subscribers[i];
                if (subscriber.alive && subscriber.kind != ESubscriberKind::Native && subscriber.id == subscription)
                {
                    RemoveAt(bus, *event, i); // drops the registry reference of the handler too
                    lua_pushboolean(L, 1);
                    return 1;
                }
            }
        }
        lua_pushboolean(L, 0);
        return 1;
    }

    // By name: every subscription of the handler to the event goes, whatever its {object = ...} (one per object)
    Event* event = FindEvent(bus, LuaFindEventId(L, 1));
    bool removed = false;
    if (event && !lua_isnoneornil(L, 2))
    {
        // Backwards: RemoveAt erases at once outside a dispatch, the indices below stay valid
        for (size_t i = event->subscribers.size(); i-- > 0;)
        {
            const Subscriber& subscriber = event->subscribers[i];
            if (!subscriber.alive || subscriber.kind == ESubscriberKind::Native)
                continue;
            lua_rawgeti(L, LUA_REGISTRYINDEX, subscriber.lua_ref);
            const bool same = lua_rawequal(L, -1, 2) != 0;
            lua_pop(L, 1);
            if (same)
            {
                RemoveAt(bus, *event, i); // drops the registry reference of the handler too
                removed = true;
            }
        }
    }
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

// event_bus.has_subscribers(name): true when the event has a Lua or a native subscriber now. The filters of the
// subscriptions ({object = ...}, throttle) are not looked at: an emit may still reach nobody.
int LuaHasSubscribers(lua_State* L)
{
    const Event* event = FindEvent(GetBus(), LuaFindEventId(L, 1));
    lua_pushboolean(L, event && (event->lua_count || event->native_count) ? 1 : 0);
    return 1;
}

// event_bus.list(): every event the bus knows, in the order of their ids - an array of tables
// {name, declared, schema (nil without one), lua_subscribers, native_subscribers (batch ones included),
// batch_subscribers, builtin, engine (the engine sends it now), emits}. The data of the console command event_list.
int LuaList(lua_State* L)
{
    const Bus& bus = GetBus();
    lua_createtable(L, static_cast<int>(bus.events.size()), 0);
    for (size_t i = 0; i < bus.events.size(); ++i)
    {
        const Event& event = *bus.events[i];
        lua_createtable(L, 0, 9);
        lua_pushlstring(L, event.name.c_str(), event.name.size());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, event.declared ? 1 : 0);
        lua_setfield(L, -2, "declared");
        if (event.has_schema)
        {
            lua_pushlstring(L, event.schema.c_str(), event.schema.size());
            lua_setfield(L, -2, "schema");
        }
        lua_pushnumber(L, static_cast<lua_Number>(event.lua_count));
        lua_setfield(L, -2, "lua_subscribers");
        lua_pushnumber(L, static_cast<lua_Number>(event.native_count));
        lua_setfield(L, -2, "native_subscribers");
        lua_pushnumber(L, static_cast<lua_Number>(event.batch_count));
        lua_setfield(L, -2, "batch_subscribers");
        lua_pushboolean(L, event.builtin ? 1 : 0);
        lua_setfield(L, -2, "builtin");
        lua_pushboolean(L, IsEngineSourceActive(event) ? 1 : 0);
        lua_setfield(L, -2, "engine");
        lua_pushnumber(L, static_cast<lua_Number>(event.emit_count));
        lua_setfield(L, -2, "emits");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
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
    ++event->lua_emits;

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
        // Converted again after Lua handlers: they may have changed a table, and the strings of array items point
        // into the strings the table holds
        if (!converted || event->lua_count)
            values.Fill(L, 2, native_argc);
        values.OwnArrays(native_argc);

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
        DispatchNative(bus, *event, id, 0, values.data, static_cast<u32>(native_argc), result_ptr);

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
    // Functions without self (event_id) have no plugin to name
    if (self)
        Msg("! [plugin:%s] %s called outside the main thread, ignored", PluginAddonId(self), function);
    else
        Msg("! [events] %s called outside the main thread, ignored", function);
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
    if (!self)
    {
        Msg("! [events] event_subscribe '%s': self is NULL, the subscription is not made", name ? name : "");
        return GWP_INVALID_SUBSCRIPTION_ID;
    }
    if (batch ? !options.batch_handler : !handler)
    {
        Msg("! [plugin:%s] event_subscribe '%s': no %s, the subscription is not made", PluginAddonId(self),
            name ? name : "", batch ? "batch_handler (GWP_SUBSCRIBE_BATCH)" : "handler");
        return GWP_INVALID_SUBSCRIPTION_ID;
    }
    Bus& bus = GetBus();
    const GwpEventId id = InternIn(bus, name);
    Event* event = FindEvent(bus, id);
    if (!event)
    {
        Msg("! [plugin:%s] event_subscribe: invalid event name '%s'", PluginAddonId(self), name ? name : "(null)");
        return GWP_INVALID_SUBSCRIPTION_ID;
    }

    // A subscription to one object needs the object now: its end (net_Destroy) is what removes the subscription,
    // and an id that is not online may be given to another object before anything ends it.
    const bool object_filter = (options.flags & GWP_SUBSCRIBE_OBJECT) != 0;
    if (object_filter && (!g_pGameLevel || !Level().Objects.net_Find(options.object)))
    {
        Msg("! [plugin:%s] event_subscribe_ex '%s': object %u is not online", PluginAddonId(self), name ? name : "",
            options.object);
        return GWP_INVALID_SUBSCRIPTION_ID;
    }

    Subscriber subscriber;
    subscriber.id = bus.next_subscription++;
    subscriber.kind = ESubscriberKind::Native;
    subscriber.plugin = self;
    subscriber.handler = batch ? nullptr : handler;
    subscriber.user = user;
    subscriber.throttle_ms = options.throttle_ms;
    subscriber.object_filter = object_filter;
    subscriber.object = object_filter ? options.object : 0;
    if (object_filter)
        ++bus.object_subscriptions;
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
    // Copy only what the plugin knows: a plugin built with a later header has a longer struct (its new fields are
    // not read by this engine). Every field up to `object` is required: the first version of the struct had them.
    constexpr size_t kMinOptionsSize = offsetof(GwpSubscribeOptions, object) + sizeof(GwpObjectId);
    constexpr uint32_t kKnownFlags = GWP_SUBSCRIBE_BATCH | GWP_SUBSCRIBE_OBJECT;
    GwpSubscribeOptions copy{};
    if (options)
    {
        pcstr error = nullptr;
        if (options->size < kMinOptionsSize)
            error = "options->size is below the size of the first version of GwpSubscribeOptions";
        else
        {
            memcpy(&copy, options, std::min<size_t>(options->size, sizeof(copy)));
            if (copy.flags & ~kKnownFlags)
                error = "unknown flag bits (GWP_SUBSCRIBE_* of a later version?)";
            else if (copy.reserved != 0)
                error = "options->reserved is not 0";
            // Only when options->size covers the whole field: a struct of an earlier version ends before it
            else if (options->size >= offsetof(GwpSubscribeOptions, reserved2) + sizeof(copy.reserved2) &&
                copy.reserved2 != 0)
                error = "options->reserved2 is not 0";
        }
        if (error)
        {
            Msg("! [plugin:%s] event_subscribe_ex '%s': %s, the subscription is not made", PluginAddonId(self),
                name ? name : "", error);
            return GWP_INVALID_SUBSCRIPTION_ID;
        }
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

constexpr uint32_t kKnownDeclareFlags = GWP_EVENT_HAS_RESULT;

GwpResult GWP_CALL ApiEventDeclare(const GwpPlugin* self, const char* name, uint32_t flags)
{
    if (!CheckMainThread(self, "event_declare"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (flags & ~kKnownDeclareFlags)
    {
        Msg("! [plugin:%s] event_declare '%s': unknown flag bits 0x%x", PluginAddonId(self), name ? name : "",
            flags & ~kKnownDeclareFlags);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
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
    if (flags & ~kKnownDeclareFlags)
    {
        Msg("! [plugin:%s] event_declare_ex '%s': unknown flag bits 0x%x", PluginAddonId(self), name ? name : "",
            flags & ~kKnownDeclareFlags);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
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
    if (id == GWP_INVALID_EVENT_ID)
        return GWP_ERROR_INVALID_ARGUMENT;
    Event* event = FindEvent(GetBus(), id);
    if (!event)
        return GWP_ERROR_NOT_FOUND; // not an id that event_id gave in this run
    // reserved != 0, a type of a later version, a broken string or array: refused before anybody sees it
    if (CheckValues(argv, argc) != GWP_OK || (result && CheckValue(*result) != GWP_OK))
    {
        Msg("! [plugin:%s] event_emit '%s': invalid argument or result value (reserved not 0, unknown type, a string "
            "or an array without data, a string over 1 MiB, an array too long or too deep, more than 65536 values "
            "or 16 MiB of strings in one value)", PluginAddonId(self), event->name.c_str());
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    string128 who;
    xr_sprintf(who, "plugin:%s", PluginAddonId(self));
    WarnUndeclared(*event, who, "emit");
    if (event->subscribers.empty() || !argc)
        return EmitFrom(id, argv, argc, result, who, false);

    // Deep copies of the strings, bytes and arrays of the plugin: a handler may free or change what they point to
    // before the next handler reads them (e.g. the plugin emits the result of its script_call, and a subscriber
    // calls script_call of the same plugin, which replaces that result). A string with ptr == NULL has nothing to
    // copy and stays such (nil in Lua); the other values are copied as they are.
    ValueArena arena;
    GwpValue inline_args[16];
    xr_vector<GwpValue> heap_args;
    GwpValue* args = inline_args;
    if (argc > std::size(inline_args))
    {
        heap_args.resize(argc);
        args = heap_args.data();
    }
    for (u32 i = 0; i < argc; ++i)
    {
        const GwpValue& arg = argv[i];
        const bool has_data =
            arg.type == GWP_T_ARRAY || ((arg.type == GWP_T_STRING || arg.type == GWP_T_BYTES) && arg.u.s.ptr);
        args[i] = has_data ? arena.Copy(arg) : arg;
    }
    return EmitFrom(id, args, argc, result, who, false);
}

int GWP_CALL ApiEventHasSubscribers(GwpEventId id)
{
    if (!g_bus || !IsMainThread())
        return 0;
    const Event* event = FindEvent(*g_bus, id);
    return event && (event->native_count || event->lua_count) ? 1 : 0;
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
        { "has_subscribers", &LuaHasSubscribers },
        { "list", &LuaList },
    };
    // Used by axr_main.script only, not an API for addons: kept out of event_bus, in the global table _gw_internal
    const luaL_Reg internal_functions[] = {
        { "event_bus_set_lua_adapter", &LuaSetLuaAdapter },
        { "event_bus_is_engine_source", &LuaIsEngineSource },
    };
    lua_newtable(L); // event_bus
    lua_newtable(L); // name -> id cache, shared by all functions as upvalue 1
    for (const luaL_Reg& function : functions)
    {
        lua_pushvalue(L, -1);
        lua_pushcclosure(L, function.func, 1);
        lua_setfield(L, -3, function.name);
    }
    lua_getglobal(L, "_gw_internal"); // shared with addon_storage.cpp, gw_condlist_native.cpp: the first one creates it
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_gw_internal");
    }
    for (const luaL_Reg& function : internal_functions)
    {
        lua_pushvalue(L, -2);
        lua_pushcclosure(L, function.func, 1);
        lua_setfield(L, -2, function.name);
    }
    lua_pop(L, 2); // _gw_internal, the cache
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

    // The values already point into queue.arena (deep copies made by Enqueue): nothing to fix up here
    xr_vector<GwpEvent> events(queue.first.size());
    for (size_t i = 0; i < events.size(); ++i)
    {
        events[i] = MakeEvent(id, event, queue.flags[i], queue.values.data() + queue.first[i], queue.argc[i],
            nullptr);
    }
    call.events = events.data();
    call.count = static_cast<u32>(events.size());

    if (!CallNativeGuarded(call))
    {
        string256 what;
        xr_sprintf(what, "a batch handler of event '%s'", event.name.c_str());
        OnPluginCrashed(plugin, what); // every subscription of the plugin goes, the whole plugin stops
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
GwpResult EmitFrom(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result, pcstr source, bool from_engine);
} // namespace

void Emit(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result)
{
    EmitFrom(id, argv, argc, result, "engine", true);
}

namespace
{
GwpResult EmitFrom(GwpEventId id, const GwpValue* argv, u32 argc, GwpValue* result, pcstr source, bool from_engine)
{
    ZoneScopedN("events/emit"); // Tracy: the whole emit (schema check, log, native and Lua dispatch)
    if (!IsMainThread())
    {
        // Before anything of the bus: the logic thread may grow bus.events right now, so not even the name is read.
        // Once per run: a thread that emits does it often, and the log would fill up
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed))
            Msg("! [events] event %u emitted outside the game logic thread, ignored (reported once)", id);
        return GWP_ERROR_NOT_MAIN_THREAD;
    }
    if (!g_bus)
        return GWP_ERROR_NOT_FOUND;
    Bus& bus = *g_bus;
    Event* event = FindEvent(bus, id);
    if (!event)
        return GWP_ERROR_NOT_FOUND;
    ZoneTextF("%s%s", event->name.c_str(), from_engine ? " (engine)" : "");
    ++event->emit_count;
    ++(from_engine ? event->engine_emits : event->plugin_emits);
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
        return GWP_OK;

    DispatchScope scope(bus, *event, nullptr);
    if (scope.TooDeep())
    {
        Msg("! [events] event '%s': nested emits deeper than %u, dispatch skipped", event->name.c_str(),
            kMaxDispatchDepth);
        return GWP_ERROR_INVALID_STATE;
    }

    // Same order as a Lua emit: Lua subscribers first, then plugins.
    lua_State* L = event->lua_count ? ActiveLua(bus) : nullptr;
    if (L && lua_checkstack(L, 4))
        DispatchLuaFromNative(bus, L, *event, argv, argc, result, from_engine);

    if (event->native_count)
        DispatchNative(bus, *event, id, from_engine ? GWP_EVENT_FLAG_ENGINE : 0u, argv, argc, result);
    return GWP_OK;
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
    args.subject_argv = argv; // {object = ...} subscribers look at the native arguments, not at the adapter's
    args.subject_argc = argc;
    if (result)
    {
        // Lua handlers get the result as a trailing {ret_value = ...} table, like the script events do.
        lua_newtable(L);
        PushValue(L, *result);
        lua_setfield(L, -2, field);
        args.flags_table = lua_gettop(L);
    }
    // Lua adapter (_gw_internal.event_bus_set_lua_adapter): Lua subscribers get what it returns, e.g. the actor
    // binder that the engine does not know. Called once per emit; on an error the native arguments are used.
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
    api.event_has_subscribers = &ApiEventHasSubscribers;
}

void PushLuaValue(lua_State* L, const GwpValue& value) { PushValue(L, value); }
lua_State* ActiveLuaThread() { return g_bus ? ActiveLua(*g_bus) : nullptr; }
GwpValue LuaToValue(lua_State* L, int index) { return ToValue(L, index); }
GwpValue LuaToValue(lua_State* L, int index, ValueArena* arena) { return ToValue(L, index, arena); }

// ---------------------------------------------------------------------------------------------
// Values: arena, owned copies, checks, save format (see addon_event_bus.h)
// ---------------------------------------------------------------------------------------------

namespace
{
constexpr size_t kArenaValueBlock = 64;  // GwpValue items per block
constexpr size_t kArenaByteBlock = 1024; // bytes per block

// budget: the value budget of the whole value so far. The items of an array are counted before they are checked,
// so a shared row repeated through every level stops the check after about kMaxValueNodes steps.
GwpResult CheckValueDepth(const GwpValue& value, u32 depth, ValueBudget& budget)
{
    if (value.reserved != 0 || value.type > GWP_T_ARRAY)
        return GWP_ERROR_INVALID_ARGUMENT;
    switch (value.type)
    {
    case GWP_T_STRING:
    case GWP_T_BYTES:
        // The length limit also keeps len + 1 (the zero of a copy) inside u32
        if ((value.u.s.len && !value.u.s.ptr) || value.u.s.len > kMaxValueBytes)
            return GWP_ERROR_INVALID_ARGUMENT;
        budget.bytes += value.u.s.len; // every reference to a shared string: each one is copied
        if (budget.Over())
            return GWP_ERROR_INVALID_ARGUMENT;
        break;
    case GWP_T_ARRAY:
        if (depth >= kMaxArrayDepth || value.u.a.count > kMaxArrayItems || (value.u.a.count && !value.u.a.items))
            return GWP_ERROR_INVALID_ARGUMENT;
        budget.nodes += value.u.a.count;
        if (budget.Over())
            return GWP_ERROR_INVALID_ARGUMENT;
        for (u32 i = 0; i < value.u.a.count; ++i)
        {
            if (CheckValueDepth(value.u.a.items[i], depth + 1, budget) != GWP_OK)
                return GWP_ERROR_INVALID_ARGUMENT;
        }
        break;
    default: break;
    }
    return GWP_OK;
}

bool ValuesEqualDepth(const GwpValue& a, const GwpValue& b, u32 depth)
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
    case GWP_T_NIL: return true;
    case GWP_T_BOOL: return (a.u.b != 0) == (b.u.b != 0);
    case GWP_T_INT: return a.u.i == b.u.i;
    case GWP_T_NUMBER: return a.u.n == b.u.n;
    case GWP_T_STRING:
    case GWP_T_BYTES:
        // ptr == NULL (len 0) equals "": a copy (ValueArena::Copy) makes it an empty string
        return a.u.s.len == b.u.s.len && (a.u.s.len == 0 || memcmp(a.u.s.ptr, b.u.s.ptr, a.u.s.len) == 0);
    case GWP_T_VEC3: return a.u.v[0] == b.u.v[0] && a.u.v[1] == b.u.v[1] && a.u.v[2] == b.u.v[2];
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT: return a.u.id == b.u.id;
    case GWP_T_ARRAY:
        if (a.u.a.count != b.u.a.count || depth >= kMaxArrayDepth)
            return false;
        for (u32 i = 0; i < a.u.a.count; ++i)
        {
            if (!ValuesEqualDepth(a.u.a.items[i], b.u.a.items[i], depth + 1))
                return false;
        }
        return true;
    default: return false; // LUA_REF and unknown types: the content is unknown
    }
}

void WriteValueDepth(IWriter& stream, const GwpValue& value, u32 depth)
{
    switch (value.type)
    {
    case GWP_T_BOOL:
        stream.w_u32(value.type);
        stream.w_u8(value.u.b ? 1 : 0);
        return;
    case GWP_T_INT:
        stream.w_u32(value.type);
        stream.w_s64(value.u.i);
        return;
    case GWP_T_NUMBER:
        stream.w_u32(value.type);
        stream.w(&value.u.n, sizeof(value.u.n));
        return;
    case GWP_T_STRING:
    case GWP_T_BYTES:
        if (!value.u.s.ptr)
            break; // nil in Lua: written as NIL
        stream.w_u32(value.type);
        stream.w_u32(value.u.s.len);
        if (value.u.s.len)
            stream.w(value.u.s.ptr, value.u.s.len);
        return;
    case GWP_T_VEC3:
        stream.w_u32(value.type);
        stream.w(value.u.v, sizeof(value.u.v));
        return;
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT:
        stream.w_u32(value.type);
        stream.w_u16(value.u.id);
        return;
    case GWP_T_ARRAY:
        if (depth >= kMaxArrayDepth || value.u.a.count > kMaxArrayItems || (value.u.a.count && !value.u.a.items))
            break; // invalid: written as NIL
        stream.w_u32(value.type);
        stream.w_u32(value.u.a.count);
        for (u32 i = 0; i < value.u.a.count; ++i)
            WriteValueDepth(stream, value.u.a.items[i], depth + 1);
        return;
    default: break;
    }
    stream.w_u32(GWP_T_NIL); // NIL, LUA_REF, unknown types
}

// budget: the value budget of what is read so far, counted as CheckValueDepth does
bool ReadValueDepth(IReader& stream, ValueArena& arena, GwpValue& out, u32 max_string, u32 depth,
    ValueBudget& budget)
{
    const auto has = [&stream](size_t bytes) { return stream.elapsed() >= static_cast<intptr_t>(bytes); };
    out = Nil();
    if (!has(sizeof(u32)))
        return false;
    const u32 type = stream.r_u32();
    switch (type)
    {
    case GWP_T_NIL: return true;
    case GWP_T_BOOL:
        if (!has(1))
            return false;
        out.u.b = stream.r_u8() ? 1 : 0;
        break;
    case GWP_T_INT:
        if (!has(sizeof(s64)))
            return false;
        out.u.i = stream.r_s64();
        break;
    case GWP_T_NUMBER:
        if (!has(sizeof(double)))
            return false;
        stream.r(&out.u.n, sizeof(out.u.n));
        break;
    case GWP_T_STRING:
    case GWP_T_BYTES:
    {
        if (!has(sizeof(u32)))
            return false;
        const u32 length = stream.r_u32();
        if (length > max_string || !has(length))
            return false;
        budget.bytes += length;
        if (budget.Over())
            return false;
        char* bytes = arena.AllocBytes(static_cast<size_t>(length) + 1); // + a zero, as ValueArena::Copy does
        if (length)
            memcpy(bytes, stream.pointer(), length);
        bytes[length] = 0;
        stream.advance(length);
        out.u.s.ptr = bytes;
        out.u.s.len = length;
        break;
    }
    case GWP_T_VEC3:
        if (!has(sizeof(out.u.v)))
            return false;
        stream.r(out.u.v, sizeof(out.u.v));
        break;
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT:
        if (!has(sizeof(u16)))
            return false;
        out.u.id = stream.r_u16();
        break;
    case GWP_T_ARRAY:
    {
        if (depth >= kMaxArrayDepth || !has(sizeof(u32)))
            return false;
        const u32 count = stream.r_u32();
        if (count > kMaxArrayItems)
            return false;
        budget.nodes += count;
        if (budget.Over())
            return false;
        GwpValue* items = arena.AllocValues(count);
        for (u32 i = 0; i < count; ++i)
        {
            if (!ReadValueDepth(stream, arena, items[i], max_string, depth + 1, budget))
                return false;
        }
        out.u.a.items = items;
        out.u.a.count = count;
        break;
    }
    default: return false; // a type of a later version: its payload size is unknown
    }
    out.type = type;
    return true;
}
} // namespace

template <typename T>
T* ValueArena::Alloc(xr_vector<xr_vector<T>>& blocks, size_t count, size_t block_size)
{
    if (!count)
        return nullptr;
    if (blocks.empty() || blocks.back().capacity() - blocks.back().size() < count)
    {
        // A new block: the old ones keep their buffers (moving a vector keeps its data), so the pointers given
        // out before stay valid
        blocks.emplace_back();
        blocks.back().reserve(std::max(count, block_size));
    }
    xr_vector<T>& block = blocks.back();
    const size_t offset = block.size();
    block.resize(offset + count); // within the capacity: no reallocation
    return block.data() + offset;
}

GwpValue* ValueArena::AllocValues(u32 count)
{
    GwpValue* values = Alloc(m_values, count, kArenaValueBlock);
    for (u32 i = 0; i < count; ++i)
        values[i] = Nil();
    return values;
}

char* ValueArena::AllocBytes(size_t size) { return Alloc(m_bytes, size, kArenaByteBlock); }

void ValueArena::Clear()
{
    if (m_values.size() > 1)
        m_values.resize(1);
    if (!m_values.empty())
        m_values.front().clear();
    if (m_bytes.size() > 1)
        m_bytes.resize(1);
    if (!m_bytes.empty())
        m_bytes.front().clear();
}

GwpValue ValueArena::Copy(const GwpValue& value)
{
    // Counted first: a shared row repeated through every level would make billions of copies
    if (!ValueWithinBudget(value))
    {
        static bool warned = false;
        WarnValueOverBudget(warned, "its copy is NIL");
        return Nil();
    }
    return CopyDepth(value, 0);
}

GwpValue ValueArena::CopyDepth(const GwpValue& value, u32 depth)
{
    GwpValue copy = value;
    copy.reserved = 0;
    switch (value.type)
    {
    case GWP_T_NIL:
    case GWP_T_BOOL:
    case GWP_T_INT:
    case GWP_T_NUMBER:
    case GWP_T_VEC3:
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT: return copy;
    case GWP_T_STRING:
    case GWP_T_BYTES:
    {
        if (!value.u.s.ptr)
        {
            // An empty string, at any depth: the save (WriteValue writes ptr == NULL as NIL), Lua (an empty BYTES)
            // and the data bus would each see something else otherwise
            copy.u.s.ptr = "";
            copy.u.s.len = 0;
            return copy;
        }
        char* bytes = AllocBytes(static_cast<size_t>(value.u.s.len) + 1);
        if (value.u.s.len)
            memcpy(bytes, value.u.s.ptr, value.u.s.len);
        bytes[value.u.s.len] = 0;
        copy.u.s.ptr = bytes;
        return copy;
    }
    case GWP_T_ARRAY:
    {
        const GwpArray& array = value.u.a;
        if (depth >= kMaxArrayDepth || array.count > kMaxArrayItems || (array.count && !array.items))
            return Nil();
        GwpValue* items = AllocValues(array.count);
        for (u32 i = 0; i < array.count; ++i)
            items[i] = CopyDepth(array.items[i], depth + 1);
        copy.u.a.items = items;
        return copy;
    }
    default: return LuaRef(); // LUA_REF has no content; a type of a later version is read as LUA_REF
    }
}

OwnedValue::OwnedValue() : m_value(Nil()) {}

OwnedValue::OwnedValue(OwnedValue&& other) noexcept : m_arena(std::move(other.m_arena)), m_value(other.m_value)
{
    other.m_value = Nil(); // its parts moved here with the arena
}

OwnedValue& OwnedValue::operator=(OwnedValue&& other) noexcept
{
    if (this != &other)
    {
        m_arena = std::move(other.m_arena);
        m_value = other.m_value;
        other.m_value = Nil();
    }
    return *this;
}

OwnedValue& OwnedValue::operator=(const OwnedValue& other)
{
    if (this != &other)
        Assign(other.m_value);
    return *this;
}

void OwnedValue::Assign(const GwpValue& value)
{
    // Into a new arena first: `value` may point into the current one
    ValueArena arena;
    const GwpValue copy = arena.Copy(value);
    m_arena = std::move(arena); // the blocks move with their buffers: `copy` stays valid
    m_value = copy;
}

void OwnedValue::Write(IWriter& stream) const { WriteValue(stream, m_value); }

bool OwnedValue::Read(IReader& stream)
{
    ValueArena arena;
    GwpValue value;
    const bool ok = ReadValue(stream, arena, value);
    m_arena = std::move(arena);
    m_value = ok ? value : Nil();
    return ok;
}

GwpResult CheckValue(const GwpValue& value)
{
    ValueBudget budget; // per value
    return CheckValueDepth(value, 0, budget);
}

GwpResult CheckValues(const GwpValue* argv, u32 argc)
{
    if (argc && !argv)
        return GWP_ERROR_INVALID_ARGUMENT;
    for (u32 i = 0; i < argc; ++i)
    {
        if (CheckValue(argv[i]) != GWP_OK) // the node budget is per value
            return GWP_ERROR_INVALID_ARGUMENT;
    }
    return GWP_OK;
}

bool ValuesEqual(const GwpValue& a, const GwpValue& b) { return ValuesEqualDepth(a, b, 0); }

void WriteValue(IWriter& stream, const GwpValue& value)
{
    // ReadValue refuses such a value: written as NIL, the rest of the save stays readable
    if (!ValueWithinBudget(value))
    {
        static bool warned = false;
        WarnValueOverBudget(warned, "written to the save as NIL");
        stream.w_u32(GWP_T_NIL);
        return;
    }
    WriteValueDepth(stream, value, 0);
}

bool ReadValue(IReader& stream, ValueArena& arena, GwpValue& out, u32 max_string)
{
    ValueBudget budget; // per value
    if (ReadValueDepth(stream, arena, out, max_string, 0, budget))
        return true;
    out = Nil();
    return false;
}

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

void RemoveObjectSubscriptions(u16 object)
{
    if (!g_bus || !g_bus->object_subscriptions)
        return;
    Bus& bus = *g_bus;
    for (auto& event : bus.events)
    {
        for (size_t i = event->subscribers.size(); i-- > 0;)
        {
            const Subscriber& subscriber = event->subscribers[i];
            if (subscriber.alive && subscriber.object_filter && subscriber.object == object)
                RemoveAt(bus, *event, i);
        }
    }
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
    Msg("- [events] events with subscribers or emits: name, lua / native subscribers, who emitted it (engine = C++ "
        "of the engine or the host, lua, plugin; before any emit: the declared source), emits, schema");
    for (const auto& event : g_bus->events)
    {
        declared += event->declared ? 1 : 0;
        engine += IsEngineSourceActive(*event) ? 1 : 0;
        with_subscribers += event->lua_count || event->native_count ? 1 : 0;
        if (!event->lua_count && !event->native_count && !event->emit_count)
            continue;
        string64 source;
        source[0] = 0;
        if (event->engine_emits)
            xr_strcat(source, "engine");
        if (event->lua_emits)
            xr_strcat(source, source[0] ? "+lua" : "lua");
        if (event->plugin_emits)
            xr_strcat(source, source[0] ? "+plugin" : "plugin");
        if (!source[0])
            xr_strcpy(source, event->builtin || IsEngineSourceActive(*event) ? "engine" : "lua");
        Msg("-   %-40s %3u / %-3u %-13s %8u  %s%s%s%s", event->name.c_str(), event->lua_count, event->native_count,
            source, event->emit_count,
            event->has_schema ? (event->schema.empty() ? "()" : event->schema.c_str()) : "-",
            event->batch_count ? " (batch)" : "", event->trace ? " (trace)" : "",
            event->declared ? "" : " (undeclared)");
    }
    Msg("- [events] %u event(s) known, %u declared, %u with subscribers, %u moved to the engine (stage B)",
        static_cast<u32>(g_bus->events.size()), declared, with_subscribers, engine);
}
} // namespace gw::addons::events
