#pragma once

// Event bus: one dispatcher in the engine for Lua scripts and native plugins.
//  - Lua: global table event_bus (declare/subscribe/unsubscribe/emit/schema/has_subscribers/list);
//    axr_main.callback_set/make_callback and
//    RegisterScriptCallback/SendScriptCallback are thin wrappers over it.
//  - Plugins: event_* functions of GwpEngineApi (xrAddonHost/include/gwp/gwp_api.h).
//  - Engine: built-in lifecycle events below, emitted from C++.
// Main thread only. Docs: wiki/doc/plugins/api/events.md; plan: plans/lua_to_cpp/04-plugin-api-events-and-data.md

#include "xrAddonHost/include/gwp/gwp_api.h"

struct lua_State;
class IReader;
class IWriter;

namespace gw::addons::events
{
// Events emitted by the engine itself. Registered first, so their ids are fixed: id == value.
enum class EBuiltin : GwpEventId
{
    EngineOnScriptStart = 1, // Lua state (re)created and common scripts loaded
    AlifeOnStart,            // (reason: "new_game" | "load" | "level_change") ALife created, before alife_on_load
    AlifeOnEnd,              // ALife is being destroyed (exit to menu, another save, level change, quit)
    AlifeOnBeforeSave,       // (save_name: file name with extension) before the ALife save is written; save_write here
    AlifeOnAfterSave,        // (save_name) the save is on disk
    AlifeOnLoad,             // (save_name) plugin save data is read; objects registered, on_register not called yet
    AlifeOnAfterLoad,        // (save_name) on_register of every object is done (comes on every load)
    LevelOnStart,            // (level_name) the level is loaded and the client started
    LevelOnStop,             // the level is being stopped; object ids are still valid
    LevelOnFrame,            // (dt seconds) every frame of an active, unpaused level
    ActorOnSpawn,            // (actor object) CActor::net_Spawn succeeded
    ActorOnDestroy,          // (actor object) CActor::net_Destroy
    DataOnChanged,           // (key, value) a value of the data bus changed; value is nil after an erase
    ServiceOnRegister,       // (name, version) a plugin published a service (addon_api_services.cpp)
    ServiceOnUnregister,     // (name) a service is gone: its plugin removed it, stopped or is being unloaded
    ServerObjectOnRegister,   // (server object, section, class id) registered in ALife (addon_api_alife_ext.cpp)
    ServerObjectOnUnregister, // (server object, section, class id) about to leave ALife (addon_api_alife_ext.cpp)
    NpcOnBeforeHit,           // (npc, power, dir, who, bone, hit type, impulse) -> power / false (addon_api_character.cpp)
    MonsterOnBeforeHit,       // the same for a monster (addon_api_character.cpp)
    RelationOnChanged,        // (kind, a, b, old, new) a goodwill or a community relation changed (addon_api_character.cpp)
    Count_
};

GwpEventId Intern(pcstr name); // id of the event, registering the name when needed; 0 for an empty name

// schema: argument codes (b bool, I integer, N number, s string, v vector, o game object, O server object,
// t Lua table/userdata (LUA_REF or ARRAY), * anything; '?' after a code = may be nil). nullptr = keep the current one.
void Declare(pcstr name, u32 flags = 0, pcstr schema = nullptr);

// Synchronous dispatch to Lua and native subscribers. result may be nullptr.
void Emit(GwpEventId id, const GwpValue* argv = nullptr, u32 argc = 0, GwpValue* result = nullptr);
inline void Emit(EBuiltin id, const GwpValue* argv = nullptr, u32 argc = 0, GwpValue* result = nullptr)
{
    Emit(static_cast<GwpEventId>(id), argv, argc, result);
}
bool HasSubscribers(GwpEventId id);

// Delivers the collected events to batch subscribers (GWP_SUBSCRIBE_BATCH). Called at the start of every frame
// (CGamePersistent::OnFrame) and right before level_on_stop. force: ignore throttle_ms (level stop: the ids are
// valid only now).
void FlushBatches(bool force = false);
inline bool HasSubscribers(EBuiltin id) { return HasSubscribers(static_cast<GwpEventId>(id)); }

GwpValue Nil();
GwpValue Bool(bool value);
GwpValue Int(s64 value);
GwpValue Number(double value);
GwpValue String(pcstr value); // the string must outlive the Emit call
GwpValue Vec3(const Fvector& value);
GwpValue Object(u16 id);
GwpValue ServerObject(u16 id);

// Host integration (addon_host.cpp).
void FillEngineApi(GwpEngineApi& api);

// Stage B (plans/lua_to_cpp/06): events whose source moves from Lua binders to the engine, grouped
// (addon_object_events.h). An event of an active group sent from Lua is dropped with a log line.
void DeclareEngineSource(pcstr name, u32 group);
void SetActiveEngineGroups(u32 mask);

// Argument schema check (console gw_event_schema_check): -1 = on with -addon_debug (default), 0 = off, 1 = on.
void SetSchemaCheck(int mode);
int GetSchemaCheck();

// Log of every emit of the events moved to the engine (console gw_event_engine_log, rate-limited per event):
// -1 = on with -addon_debug (default), 0 = off, 1 = on.
void SetEngineLog(int mode);
int GetEngineLog();

// Console command event_trace: logs every emit of the event with its arguments. false for an invalid name.
bool SetTrace(pcstr name, bool on);

// alife_on_start reason: a level change goes through an autosave and a new ALife loading it. change_level marks it,
// the next ALife start takes the mark.
void MarkLevelChange();
bool TakeLevelChangeMark();

void RemovePluginSubscriptions(const GwpPlugin* plugin);
// The object went offline (CGameObject::net_Destroy, after its Lua binder): its GWP_SUBSCRIBE_OBJECT subscriptions end.
void RemoveObjectSubscriptions(u16 object);
void Shutdown();
void PrintList(); // console command "event_list"

// Calls fn(context), a host thunk that calls one plugin function, with the guards of the event bus: a C++
// exception, and on Windows a crash inside the plugin library, stop here and give false (logged with `what`).
// plugin_code: an address inside the plugin library (the callback itself), so the crash filter knows the module.
// Shared with the binders (addon_binders.cpp).
bool CallPluginGuarded(const void* plugin_code, pcstr what, void (*fn)(void*), void* context);

// Plugin functions running right now on this thread (guarded calls nested in each other). 0: no plugin code is on
// the stack, a plugin library may be unloaded (addon_host.cpp: the unload and the reload of a crashed plugin).
u32 PluginCallDepth();

// The Lua thread to call Lua on right now: the coroutine that runs a Lua emit, else the main state; nullptr when
// the bus has no Lua state (addon_api_npc.cpp: script_call from a handler of such an emit).
lua_State* ActiveLuaThread();

// ---------------------------------------------------------------------------------------------
// Values (GwpValue): one set of rules for events, the data bus, script_call, plugins.call, timers
// ---------------------------------------------------------------------------------------------
// Arrays (GWP_T_ARRAY, gwp_api.h): at most kMaxArrayItems items, at most kMaxArrayDepth levels of arrays.
// GWP_T_BYTES travels as GwpString (u.s) like a string; Lua has no separate type for it (a Lua string).
constexpr u32 kMaxArrayItems = 4096;
constexpr u32 kMaxArrayDepth = 8;
// Node budget of one value: the value itself plus the items of every array in it, nested ones included, at most
// kMaxValueNodes. An array shared by several items (a DAG) counts each time: without the budget 4096 x 4096 x ...
// items through one shared row would be billions of copies. A plugin value over it is invalid (CheckValue), a Lua
// table over it is GWP_T_LUA_REF (LuaToValue), a copy or a save of one is NIL, Lua gets nil (all logged once).
constexpr u32 kMaxValueNodes = 65536;
// Length of a string or bytes a plugin may pass (CheckValue), and the longest one ReadValue reads: 1 MiB.
constexpr u32 kMaxValueBytes = 1024u * 1024u;
// Byte budget of one value: the sum of `len` of every STRING and BYTES in it, nested ones included, at most
// kMaxValueTotalBytes; a string referenced by several items counts each time (that is what a copy makes). Without it
// 65536 references to one 1 MiB string would copy into 64 GiB. Enforced together with kMaxValueNodes, in the same
// places and the same way ("the value budget").
constexpr u64 kMaxValueTotalBytes = 16ull * 1024u * 1024u;

// Storage for the parts of values that are not inside GwpValue itself: the items of arrays and the bytes of strings.
// Everything it hands out stays at its address until Clear() or the destruction of the arena (blocks never move;
// moving the arena keeps the addresses too). Not copyable: copy a value with OwnedValue or Copy into another arena.
//  - call-time arena (a local of the function that converts Lua arguments): LuaToValue(L, index, &arena) puts the
//    items of arrays here; their strings still point into Lua data (see LuaToValue);
//  - storage arena: Copy() puts deep copies here (strings, bytes and items), independent of Lua and of the caller.
class ValueArena
{
public:
    ValueArena() = default;
    ValueArena(ValueArena&&) = default;
    ValueArena& operator=(ValueArena&&) = default;
    ValueArena(const ValueArena&) = delete;
    ValueArena& operator=(const ValueArena&) = delete;

    GwpValue* AllocValues(u32 count); // `count` NIL values; nullptr for count == 0
    char* AllocBytes(size_t size);    // `size` bytes, not initialized; nullptr for size == 0

    // Deep copy of a value tree into this arena: strings and bytes (a string keeps a zero after its `len` bytes),
    // the items of arrays. reserved becomes 0, a type unknown to this engine becomes GWP_T_LUA_REF, a string or bytes
    // with ptr == NULL becomes an empty string (as WriteValue/ReadValue and the data bus see it). The source must be
    // valid (CheckValue), else the result is cut to NIL there; an array value over kMaxValueNodes or
    // kMaxValueTotalBytes is NIL as a whole.
    GwpValue Copy(const GwpValue& value);

    void Clear(); // forgets everything given out before (reuse in a loop); keeps the first block of each kind

private:
    template <typename T>
    static T* Alloc(xr_vector<xr_vector<T>>& blocks, size_t count, size_t block_size);
    GwpValue CopyDepth(const GwpValue& value, u32 depth);

    xr_vector<xr_vector<GwpValue>> m_values; // each block is reserved once and never grows past its capacity
    xr_vector<xr_vector<char>> m_bytes;
};

// A value that owns everything it points to (deep copy): for values kept beyond the call that gave them - batch
// records of the event bus, data bus entries, timer arguments. view() is valid until the OwnedValue is assigned,
// destroyed or read again; a move keeps the addresses of view() (the arena moves its blocks), a copy makes new ones.
class OwnedValue
{
public:
    OwnedValue();
    explicit OwnedValue(const GwpValue& value) { Assign(value); }
    OwnedValue(const OwnedValue& other) { Assign(other.m_value); }
    OwnedValue& operator=(const OwnedValue& other);
    OwnedValue(OwnedValue&& other) noexcept; // `other` becomes NIL
    OwnedValue& operator=(OwnedValue&& other) noexcept;

    void Assign(const GwpValue& value); // deep copy (ValueArena::Copy); `value` may point into this OwnedValue
    const GwpValue& view() const { return m_value; }
    u32 type() const { return m_value.type; }

    // Save format, see WriteValue / ReadValue. Read: false for truncated or unknown data, the value is NIL then.
    void Write(IWriter& stream) const;
    bool Read(IReader& stream);

private:
    ValueArena m_arena;
    GwpValue m_value;
};

// GWP_OK for a value a plugin may pass in: reserved == 0, a known type (<= GWP_T_ARRAY), a string or bytes with
// ptr != NULL when len > 0 and len <= kMaxValueBytes, an array with items != NULL when count > 0,
// count <= kMaxArrayItems, nesting <= kMaxArrayDepth, every item valid, at most kMaxValueNodes values and at most
// kMaxValueTotalBytes bytes of strings and bytes in total. GWP_ERROR_INVALID_ARGUMENT otherwise.
GwpResult CheckValue(const GwpValue& value);
// CheckValue of every item; argv may be nullptr only when argc == 0.
GwpResult CheckValues(const GwpValue* argv, u32 argc);
// Deep comparison: same type and content (strings and bytes by bytes, arrays item by item). LUA_REF values are
// never equal to anything (their content is unknown).
bool ValuesEqual(const GwpValue& a, const GwpValue& b);

// Save format of a value tree (IWriter / IReader of the ALife save stream), little endian:
//   u32 type, then by type: NIL, LUA_REF - nothing (LUA_REF and unknown types are written as NIL); BOOL u8;
//   INT s64; NUMBER f64; STRING, BYTES u32 length + the bytes (no zero); VEC3 3 x f32; OBJECT, SERVER_OBJECT u16 id;
//   ARRAY u32 count + `count` values in this format.
// The same as the values of the data bus save format 1 (addon_data_bus.cpp, chunk 0x0101) for the types it had,
// so that format reads with ReadValue as it is. Objects are written as ids: valid in the save they were written to.
// ReadValue: items and bytes go to `arena`; false (and `out` NIL) for truncated data, an unknown type, a string
// longer than max_string, an array longer than kMaxArrayItems or deeper than kMaxArrayDepth, more than
// kMaxValueNodes values or kMaxValueTotalBytes bytes of strings. WriteValue writes an array value over that budget
// as NIL (logged once).
// Use: data bus persistent values and persistent timer arguments (plans/lua_to_cpp/26, 1.7 / 1.11) - store an
// OwnedValue per value, OwnedValue::Write in the chunk writer, OwnedValue::Read in the chunk reader.
void WriteValue(IWriter& stream, const GwpValue& value);
bool ReadValue(IReader& stream, ValueArena& arena, GwpValue& out, u32 max_string = kMaxValueBytes);

// Lua <-> GwpValue, the same conversion as for event arguments.
// PushLuaValue: BYTES -> a Lua string, ARRAY -> a new table {1..count} (an array deeper than kMaxArrayDepth: nil,
// logged once), OBJECT -> the game object or nil when it is not online, SERVER_OBJECT -> the server object or nil,
// LUA_REF and unknown types -> nil, an array value over kMaxValueNodes or kMaxValueTotalBytes -> nil (logged once).
void PushLuaValue(lua_State* L, const GwpValue& value);
// LuaToValue: strings point into Lua data, valid while the value stays on the Lua stack (and, for strings inside an
// array, while its table is not changed). A Lua number is always GWP_T_NUMBER, a Lua string GWP_T_STRING.
//  - without an arena (old callers, not yet moved to arrays): every table is GWP_T_LUA_REF;
//  - with an arena: a table with the keys exactly 1..n (n <= kMaxArrayItems, an empty table included) whose items all
//    have a native form, nested tables by the same rule (at most kMaxArrayDepth levels, at most kMaxValueNodes
//    values and kMaxValueTotalBytes bytes of strings in total, a table or a string met twice counted twice), is
//    GWP_T_ARRAY with its items in the arena; any other table is GWP_T_LUA_REF. The arena must live as long as the
//    value is used.
GwpValue LuaToValue(lua_State* L, int index);
GwpValue LuaToValue(lua_State* L, int index, ValueArena* arena);
} // namespace gw::addons::events
