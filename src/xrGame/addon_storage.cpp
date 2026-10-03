#include "StdAfx.h"

#include "addon_storage.h"
#include "addon_event_bus.h"
#include "addon_goap.h"
#include "addon_host.h"

#include "GameObject.h"
#include "xrScriptEngine/script_engine.hpp"

#include <cmath>
#include <cstring>
#include <string_view>
#include <iterator>

namespace gw::addons::storage
{
namespace
{
// Mirrored fields of db.storage[id]. "sub.field" lives in the scheme subtable db.storage[id].sub.
// Append-only: plugins resolve names to indices at run time, so a key added at the end changes nothing for them;
// removing or renaming one is a major change of the API. The Lua side takes the list from npc_storage.keys().
constexpr pcstr kKeys[] = {
    // the entry itself: xr_logic, xr_motivator, xr_danger, xr_combat, rx_ff, ...
    "active_scheme",
    "active_section",
    "section_logic",
    "gulag_name",
    "stype",
    "activation_time",
    "enemy_id",
    "danger_flag",
    "heli_enemy_flag",
    "death_by_id",
    "script_combat_type",
    "__update_lane",
    "victim_surrender",
    "corpse_already_selected",
    "wounded_already_selected",
    "rx_dont_shoot",
    "plugin_note", // reserved for plugins: a scalar a plugin hands to Lua, no script writes it
    // scheme subtables the evaluators of wave W2 read (09-stage-d-native-npc.md sec. 7.1)
    "meet.meet_set",
    "meet.gtfo",
    "meet.dtimer",
    "meet.commander_threat",
    "danger.danger_time",
    "danger.inertion",
    "danger.type",
    "help_wounded.selected_id",
    "gather_items.selected_id",
    "corpse_detection.selected_corpse_id",
    "kill_wounded.selected_id",
    "turn_on_campfire.selected_id",
    "overrides.min_post_combat_time",
    "overrides.max_post_combat_time",
    // smart cover of the NPC (xr_smartcover writes, state_mgr_smartcover reads): wave W1
    "smartcover.cover_name",
    "smartcover.loophole_name",
    // axr_stalker_panic: the NPC runs in panic (read by the heli evaluator of gw_npc)
    "panicked",
    // xr_combat: the combat scheme is on for the NPC (the script_combat evaluator of gw_npc, wave W2-1)
    "combat.enabled",
    // xr_wounded: the wounded state of the scheme, kept by update_wound_state from the binder ("nil" = not
    // wounded) and whether the NPC still fights while wounded ("true"/"false"): the native IsWounded
    "wounded.wound_state",
    "wounded.wound_fight",
    "wounded.wounded_set", // the wounded scheme is on for the NPC (the `wounded` evaluator of gw_npc)
    // xr_combat: the combat type the scheme picked (evaluator_check_combat reads the scheme table, not the entry:
    // set_combat_type also writes the entry from the overrides of the active section)
    "combat.script_combat_type",
    // bind_heli.heli_die: the helicopter is shot down (the wreck stays online, db.heli has it no more)
    "heli_dead",
    // xr_danger.update_danger (from the binder): the answers the danger evaluators read (W2-3)
    "danger.danger_result",
    "danger.script_danger_result",
    // xr_meet.update_contact (from the binder): the inputs of the meet_contact evaluator (W2-5)
    "meet.commander_threat_active",
    "meet.contact_distance",
    // rx_ff.update_dont_shoot (from the binder): the answer of eva_dont_shoot and the enemy it is for (W2-6)
    "rx_ff.__result",
    "rx_ff.__enemy_id",
    // the scanners of W2-4 (update_scan from the binder): switches of the schemes and the scan guard
    "corpse_detection.analse_mode",
    "corpse_detection.enabled",
    "corpse_detection.cstackprevent",
    "gather_items.items_enabled",
    "gather_items.cstackprevent",
    "help_wounded.help_wounded_enabled",
    // xr_danger.update_danger: the time_global() (ms) until which danger_result / script_danger_result are fresh
    // (last recompute + its TTL); past it the evaluators recompute at once instead of waiting for the binder
    "danger.__danger_eval_expire",
    "danger.__script_danger_eval_expire",
    // W2-7 (gw-a2): the inputs of the evaluators of combat and work schemes; every one is a scalar
    "post_combat_wait.timer",           // post_combat_idle: number/nil
    "post_combat_wait.anim_active",     // action_post_combat_wait initialize/finalize: bool
    "abuse.abused_until",               // CAbuseManager addAbuse/clearAbuse: time_global() ms
    "camper.radius",                    // xr_camper.set_scheme
    "camper.locked",                    // xr_camper: the close combat latch
    "patrol.patrol_commander",          // xr_patrol: the NPC commands its patrol
    "animpoint.am_i_reached",
    "animpoint.reach_distance",         // squared
    "animpoint.point_x",                // the position of the animation point (calculate_position)
    "animpoint.point_y",
    "animpoint.point_z",
    "animpoint.current_action",         // string/nil
    "facer.target_id",                  // number/false: the mirror of the pstor facer_target
    "facer.check_time",
    "facer.abtime",
    "fight_from_cover.fight_from_point", // number/false: the mirror of the pstor fight_from_point
    // W3-5.4 (gw-79): the native scanners read these instead of asking Lua
    "kill_wounded.scan_enabled",  // xrs_kill_wounded: the scheme is on for the NPC
    "help_wounded.scan_enabled",  // xr_help_wounded: the scheme is on for the NPC
    "wounded.not_for_help",       // xr_wounded: nobody helps this wounded (bool)
    "kill_wounded.vertex_id",     // xrs_kill_wounded: the vertex of the chosen victim
    "kill_wounded.timer",         // xrs_kill_wounded: time_global() (ms) of the choice
    // W3-5.3 (gw-ba round 2): the native wound input of gw_npc reads the gate thresholds instead of asking Lua.
    // xr_wounded.init_wounded computes them once per section (the first dist of hp_state / psy_state, *100 scale;
    // false - the table is empty, nothing ever wounds through it)
    "wounded.__hp_gate",
    "wounded.__psy_gate",
    // W3-5 (gw-ba round 2): the freshness marker of the meet manager (time_global() of its last update), kept by
    // Cmeet_manager:update next to its own field; a native meet input would read it to skip the catch-up call
    "meet.__last_update",
    // W1-5 (agent W): the target state of the native state manager (gw_npc), written by StateManager::set_state at
    // every switch; the Lua shim of the facade (state_mgr_native.script) reads it as shim.target_state, and the
    // native main-planner action act_state_mgr_to_idle relies on it instead of a per-switch script_call callback
    "state_target",
    // W3-5 (agent M): the enable_talk config of the wounded scheme, written by xr_wounded.init_wounded next to its
    // own field; the friendly half of the wounded branch of the native usability input (gw_npc) reads it to pick
    // enable_talk / disable_talk instead of asking Lua. The write already exists - the proxy mirrors it as is
    "wounded.enable_talk",
    // W3-5 (agent M): the effective use of the meet manager ("true" / "false" / "self" / nil), kept by
    // Cmeet_manager:update next to its own field (the parsed condlist itself lives in st.use_cfg); a native
    // usability input would read it for the talk tail of process_npc_usability
    "meet.use",
    // W5-2 (gw-48): the generation of the animation point, +1 at every xr_animpoint:calculate_position - the native
    // animpoint actions re-read the direction of the point when it changes (a new section may keep the coordinates
    // and change only the direction)
    "animpoint.point_gen",
    // W5-2 (gw-48): the started flag of the animpoint object (written by animpoint:start / :stop next to its own
    // field): the native play step calls start only while it is not started, as the Lua step does
    "animpoint.started",
    // Danger stage 2 (plan 20, gw-48): the evaluation snapshot of eval_danger as plain fields (the __danger_snapshot
    // subtable was a duplicate of these). Written by xr_danger update_danger_snapshot and by the native eval_danger
    "danger.last_danger_type",
    "danger.last_danger_time",
    "danger.last_danger_object_id",     // 0: no object
    "danger.last_eval_time",            // time_global, ms
    "danger.last_eval_result",
    "danger.last_script_candidate",     // the danger type may be a script danger (attacked, corpse, grenade)
    // Danger stage 3 (plan 20, gw-26): the 150 ms repeat window of eval_script_danger_inner
    "danger.last_script_danger_type",
    "danger.last_script_danger_time",
    "danger.last_script_danger_object_id",
    "danger.last_script_eval_time",     // time_global, ms
    "danger.last_script_eval_result",
    // Danger stage 3 (gw-26): the armed script danger of the NPC (the module table script_danger[id] of
    // xr_danger, mirrored into the subtable db.storage[id].script_danger). Written by xr_danger.set_script_danger
    // only: the native hit / hear handlers filter the events and call it for an arming (the module table feeds
    // action_danger_scripted, and only Lua can create the subtable); the native side reads. danger_time nil:
    // nothing armed; pos_x nil: no position
    "script_danger.danger_time",
    "script_danger.danger_inertion",    // ms
    "script_danger.who_id",
    "script_danger.pos_x",
    "script_danger.pos_y",
    "script_danger.pos_z",
    // A4 (plan 16, gw-28): the compute part of bind_monster.script (contract in the file, :52-74) as plain fields
    // of the entry; the native monster binder writes them, the Lua apply_* reads
    "monster_kick",          // the monster may be kicked (tushkano, rat): once a life
    "monster_enemy_release", // it has an enemy: the engine takes over
    "monster_follow_mode",   // "none" | "release" | "teleport" | "walk" | "run"
    "monster_follow_x",      // the target position of the walk / the teleport (nil: none)
    "monster_follow_y",
    "monster_follow_z",
    "monster_follow_lvid",   // teleport: the level vertex of the target
    "monster_follow_gvid",   // teleport: the game vertex of the target
    "monster_follow_squad",  // teleport: the squad id
};
constexpr u32 kKeyCount = static_cast<u32>(std::size(kKeys));
constexpr u32 kObjectSlots = 65536;
constexpr u16 kInvalidObjectId = u16(-1);

// One mirrored field. Lua numbers are doubles, so an integer written by a plugin is kept as a number too.
//
// A string is the own copy of the field (xr_alloc: `length` bytes plus a terminating zero), not a shared_str: the
// pool of shared_str and its reference counts belong to the whole engine (the counter is not atomic), and the
// mirror must not depend on them (bugs/2026-09-30_storage_field_corruption.md: the crash came through _dec of a
// bad p_). The bytes are kept by length: a Lua string may hold NUL bytes, storage_get hands out the same bytes and
// length the script wrote (the terminating zero is only for printing). Copying a Field would free the copy twice.
struct Field
{
    u32 type = GWP_T_NIL;   // NIL, BOOL, NUMBER, STRING, OBJECT (a game object Lua put there), LUA_REF (table, ...)
    u32 length = 0;         // STRING: the bytes of `string` without the terminating zero; 0 otherwise
    double number = 0.0;    // BOOL: 0/1; OBJECT: id
    char* string = nullptr; // STRING: never nullptr ("" is one zero byte); any other type: nullptr

    Field() = default;
    Field(const Field&) = delete;
    Field& operator=(const Field&) = delete;
};
static_assert(sizeof(Field) == 24, "the corruption reports and the bug notes count fields of 24 bytes");

struct Slot
{
    u16 generation = 0; // never 0 once the slot was live
    bool live = false;
    int backing_ref = LUA_NOREF; // registry ref of the backing table of the Lua entry, for plugin writes
    Field fields[kKeyCount];
    // The corruption watch (bugs/2026-09-30_storage_field_corruption.md): when the whole slot was last found
    // clean. A report gives the window [clean, found] in which a foreign write hit the slot.
    u32 clean_frame = 0;
    u32 clean_time_ms = 0;
};

Slot** g_slots = nullptr;     // [kObjectSlots], a slot is allocated at the first entry of its id and kept
lua_State* g_lua = nullptr;    // the Lua state the refs belong to (set by script_register for every new state)
u32 g_live = 0;

bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [storage] %s called outside the main thread, ignored", function);
    return false;
}

// The Lua state the refs belong to, or nullptr after a restart of the script engine.
lua_State* ActiveLua()
{
    if (!g_lua || !GEnv.ScriptEngine || GEnv.ScriptEngine->lua() != g_lua)
        return nullptr;
    return g_lua;
}

Slot* SlotOf(u16 id)
{
    return g_slots && id != kInvalidObjectId ? g_slots[id] : nullptr;
}

Slot& EnsureSlot(u16 id)
{
    if (!g_slots)
    {
        g_slots = xr_alloc<Slot*>(kObjectSlots);
        memset(g_slots, 0, sizeof(Slot*) * kObjectSlots);
    }
    if (!g_slots[id])
        g_slots[id] = xr_new<Slot>();
    return *g_slots[id];
}

// The string member is owned under a STRING header only: it is freed there. Under any other header a non-null
// pointer is a foreign write: it is let go without a free (not our memory) - a leak at worst, never a free or a
// dereference of garbage.
void DropString(Field& field)
{
    if (field.type == GWP_T_STRING)
        xr_free(field.string);
    else
        field.string = nullptr;
    field.length = 0;
}

void ClearFields(Slot& slot)
{
    for (Field& field : slot.fields)
    {
        DropString(field);
        field.type = GWP_T_NIL;
        field.number = 0.0;
    }
}

// ---------------------------------------------------------------------------------------------
// Corruption watch. Store() keeps every field consistent: the string member and its length are set only under a
// STRING header (and the member is never nullptr there), a BOOL holds 0/1, an OBJECT an object id, NIL and LUA_REF
// hold 0, a STRING holds 0 in the number. A field breaking that was written by someone
// else (the crash of 2026-09-30: p_ = 0x000254c200000000 under a BOOL header, writer not caught). The slot is
// checked on every write and release, at most once a frame in full, and a defect is logged with the raw bytes
// and the window since the last clean check, then neutralised so the next write or release cannot crash on it.
// ---------------------------------------------------------------------------------------------
constexpr u32 kMaxCorruptionReports = 32; // a corruption storm must not flood the log
u32 g_corruptions = 0;                    // all defects found (storage_list prints the count)

pcstr FieldDefect(const Field& field)
{
    switch (field.type)
    {
    case GWP_T_NIL:
    case GWP_T_LUA_REF:
        if (field.number != 0.0)
            return "a NIL/LUA_REF field with a non-zero number";
        break;
    case GWP_T_BOOL:
        if (field.number != 0.0 && field.number != 1.0)
            return "a BOOL field with a number other than 0/1";
        break;
    case GWP_T_NUMBER: break;
    case GWP_T_OBJECT:
        if (!(field.number >= 0.0 && field.number < 65535.0) || field.number != std::floor(field.number))
            return "an OBJECT field with a number that is not an object id";
        break;
    case GWP_T_STRING:
        // our pointer: its bytes cannot be checked without dereferencing it, only the members around it
        if (field.string == nullptr)
            return "a STRING field without a string";
        if (field.number != 0.0)
            return "a STRING field with a non-zero number";
        return nullptr;
    default: return "an unknown type header";
    }
    if (field.string != nullptr)
        return "a string pointer under a non-STRING header";
    if (field.length != 0)
        return "a string length under a non-STRING header";
    return nullptr;
}

void ReportCorruption(u16 id, const Slot& slot, u32 key, pcstr defect, pcstr where, pcstr detail)
{
    ++g_corruptions;
    if (g_corruptions > kMaxCorruptionReports)
        return;
    const Field& field = slot.fields[key];
    u64 number_bits = 0;
    std::memcpy(&number_bits, &field.number, sizeof(number_bits));
    const auto* bytes = reinterpret_cast<const u8*>(&field);
    string128 raw{};
    for (size_t i = 0; i < sizeof(Field) && i * 3 + 3 < sizeof(raw); ++i)
        xr_sprintf(raw + i * 3, sizeof(raw) - i * 3, "%02x ", bytes[i]);
    Msg("! [storage] corrupted field '%s' (#%u) of object %u (generation %u, %s): %s. Found by %s%s%s at frame "
        "%u, time %u ms; the slot was last found clean at frame %u, time %u ms",
        kKeys[key], key, id, slot.generation, slot.live ? "live" : "released", defect, where, detail ? " " : "",
        detail ? detail : "", Device.dwFrame, Device.dwTimeGlobal, slot.clean_frame, slot.clean_time_ms);
    Msg("! [storage]   field at %p (offset %u in the slot at %p, %u bytes): type=%u length=%u number=0x%016llx "
        "string=%p; raw: %s",
        &field, static_cast<u32>(reinterpret_cast<const u8*>(&field) - reinterpret_cast<const u8*>(&slot)), &slot,
        static_cast<u32>(sizeof(Slot)), field.type, field.length, static_cast<unsigned long long>(number_bits),
        static_cast<const void*>(field.string), raw);
    if (g_corruptions == kMaxCorruptionReports)
        Msg("! [storage] %u corrupted fields reported, further ones are only counted (storage_list)",
            kMaxCorruptionReports);
}

// Logs and neutralises one field: the value is lost (the mirror reads nil until the next write; the Lua table
// still holds the real one), the pointer is dropped without a dereference and without a free: under a broken STRING
// field it may be broken too (a leak at worst).
bool CheckField(u16 id, Slot& slot, u32 key, pcstr where, pcstr detail)
{
    const pcstr defect = FieldDefect(slot.fields[key]);
    if (!defect)
        return true;
    ReportCorruption(id, slot, key, defect, where, detail);
    Field& field = slot.fields[key];
    field.string = nullptr;
    field.length = 0;
    field.type = GWP_T_NIL;
    field.number = 0.0;
    return false;
}

// The whole slot, at most once a frame (writes come many times a frame); `key` (the field about to be touched)
// is checked always.
void CheckSlot(u16 id, Slot& slot, s32 key, pcstr where, pcstr detail)
{
    if (slot.clean_frame == Device.dwFrame)
    {
        if (key >= 0)
            CheckField(id, slot, static_cast<u32>(key), where, detail);
        return;
    }
    for (u32 i = 0; i < kKeyCount; ++i)
        CheckField(id, slot, i, where, detail);
    slot.clean_frame = Device.dwFrame;
    slot.clean_time_ms = Device.dwTimeGlobal;
}

GwpStorageHandle HandleOf(u16 id, const Slot& slot)
{
    return (static_cast<u32>(slot.generation) << 16) | id;
}

// The live slot a handle points to, or nullptr for a stale or invalid one.
Slot* SlotOfHandle(GwpStorageHandle handle)
{
    if (handle == GWP_INVALID_STORAGE_HANDLE)
        return nullptr;
    Slot* slot = SlotOf(static_cast<u16>(handle & 0xFFFFu));
    return slot && slot->live && slot->generation == static_cast<u16>(handle >> 16) ? slot : nullptr;
}

// Every write of a mirrored field in Lua comes here: a hash lookup instead of comparing all the names
s32 KeyIndex(pcstr key)
{
    if (!key || !key[0])
        return -1;
    static const auto* const index = [] {
        auto* map = xr_new<xr_unordered_map<std::string_view, s32>>(); // the names are literals: never freed
        for (u32 i = 0; i < kKeyCount; ++i)
            map->emplace(kKeys[i], static_cast<s32>(i));
        return map;
    }();
    const auto it = index->find(std::string_view(key));
    return it != index->end() ? it->second : -1;
}

constexpr s32 kActiveSectionKey = 1; // "active_section" in kKeys
static_assert(std::string_view(kKeys[kActiveSectionKey]) == "active_section");

// A plugin string without bytes (u.s.ptr == NULL) is no string: PushLuaValue writes nil to Lua for it, so the
// mirror takes it as NIL too and both sides agree (storage_set, SameValue, Store). Lua values never come so.
GwpValue Normalized(const GwpValue& value)
{
    return value.type == GWP_T_STRING && !value.u.s.ptr ? events::Nil() : value;
}

bool SameValue(const Field& field, const GwpValue& input)
{
    const GwpValue value = Normalized(input);
    if (value.type == GWP_T_STRING)
    {
        // The string member is only meaningful while type == STRING: a type of another kind means the field
        // never held a string (or its memory went bad - crash of 2026-09-30, corrupted pointer under a BOOL
        // header). Comparing as "different" makes the caller Store() a fresh string instead of reading the member.
        if (field.type != GWP_T_STRING)
            return false;
        return field.length == value.u.s.len &&
            (value.u.s.len == 0 || std::memcmp(field.string, value.u.s.ptr, value.u.s.len) == 0);
    }
    return value.type == GWP_T_NIL && field.type == GWP_T_NIL;
}

// The STRING branch of Store: the bytes are copied by length (NUL bytes inside included). The same bytes keep the
// copy as it is; the same length rewrites it in place; another length takes a new buffer. The new copy is made
// before the old one is freed: `text` may point into the old copy (a plugin writing back what storage_get gave
// it). A field of another type holds no string of ours: its member is overwritten, never freed.
void StoreString(Field& field, pcstr text, u32 length)
{
    const bool owned = field.type == GWP_T_STRING;
    if (owned && field.length == length && (length == 0 || std::memcmp(field.string, text, length) == 0))
        return;
    char* copy = owned && field.length == length ? field.string : xr_alloc<char>(static_cast<size_t>(length) + 1);
    if (length != 0)
        std::memmove(copy, text, length);
    copy[length] = 0;
    if (owned && copy != field.string)
        xr_free(field.string);
    field.string = copy;
    field.length = length;
    field.number = 0.0;
    field.type = GWP_T_STRING;
}

// Lua values come through LuaToValue (strings point into Lua data: copied here), plugin values through storage_set.
//
// The string member of Field is owned by STRING fields only: it is freed only while the header still says
// STRING, and a scalar write (bool/number/object) of a field of another type never touches it. A member under
// a non-STRING header may hold garbage (the crash of 2026-09-30: p_ = 0x000254c200000000 under a BOOL header):
// it is never freed nor read, only overwritten by the next string.
void Store(Field& field, const GwpValue& input)
{
    const GwpValue value = Normalized(input);
    if (value.type == GWP_T_STRING)
    {
        StoreString(field, value.u.s.ptr, value.u.s.len);
        return;
    }
    if (field.type == GWP_T_STRING)
    {
        // The previous value was a string, the new one is not: free it while the header still says STRING.
        DropString(field);
        field.type = GWP_T_NIL;
    }
    switch (value.type)
    {
    case GWP_T_NIL: field.type = GWP_T_NIL; field.number = 0.0; break;
    case GWP_T_BOOL: field.type = GWP_T_BOOL; field.number = value.u.b ? 1.0 : 0.0; break;
    case GWP_T_INT: field.type = GWP_T_NUMBER; field.number = static_cast<double>(value.u.i); break;
    case GWP_T_NUMBER: field.type = GWP_T_NUMBER; field.number = value.u.n; break;
    case GWP_T_OBJECT: field.type = GWP_T_OBJECT; field.number = value.u.id; break;
    default: field.type = GWP_T_LUA_REF; field.number = 0.0; break; // tables, vectors, server objects
    }
}

GwpValue Load(const Field& field)
{
    switch (field.type)
    {
    case GWP_T_BOOL: return events::Bool(field.number != 0.0);
    case GWP_T_NUMBER: return events::Number(field.number);
    case GWP_T_STRING:
    {
        // by length: the bytes Lua wrote, NUL bytes inside included; the pointer lives until the next write
        GwpValue value = events::Nil();
        value.type = GWP_T_STRING;
        value.u.s.ptr = field.string;
        value.u.s.len = field.length;
        return value;
    }
    case GWP_T_OBJECT: return events::Object(static_cast<u16>(field.number));
    case GWP_T_LUA_REF:
    {
        GwpValue value = events::Nil();
        value.type = GWP_T_LUA_REF;
        return value;
    }
    default: return events::Nil();
    }
}

pcstr TypeName(u32 type)
{
    switch (type)
    {
    case GWP_T_BOOL: return "bool";
    case GWP_T_NUMBER: return "number";
    case GWP_T_STRING: return "string";
    case GWP_T_OBJECT: return "object";
    case GWP_T_LUA_REF: return "lua";
    default: return "nil";
    }
}

void Release(u16 id)
{
    Slot* slot = SlotOf(id);
    if (!slot || !slot->live)
        return;
    if (slot->backing_ref != LUA_NOREF)
    {
        if (lua_State* L = ActiveLua())
            luaL_unref(L, LUA_REGISTRYINDEX, slot->backing_ref);
        slot->backing_ref = LUA_NOREF;
    }
    CheckSlot(id, *slot, -1, "release", nullptr);
    slot->live = false;
    ClearFields(*slot);
    --g_live;
}

// A new Lua state: the refs of the old one are gone with it, the entries are recreated by the binders of the
// next level from scratch.
void ResetAll()
{
    if (!g_slots)
        return;
    for (u32 id = 0; id < kObjectSlots; ++id)
    {
        Slot* slot = g_slots[id];
        if (!slot)
            continue;
        slot->live = false;
        slot->backing_ref = LUA_NOREF;
        ClearFields(*slot);
    }
    g_live = 0;
}

// ---------------------------------------------------------------------------------------------
// Lua API: global table npc_storage (used by npc_storage_bridge.script only)
// ---------------------------------------------------------------------------------------------

bool LuaObjectId(lua_State* L, int index, pcstr function, u16& out)
{
    if (lua_type(L, index) != LUA_TNUMBER)
    {
        Msg("! [storage] npc_storage.%s: the object id must be a number, got %s", function,
            lua_typename(L, lua_type(L, index)));
        return false;
    }
    const lua_Number number = lua_tonumber(L, index);
    if (!(number >= 0 && number < kInvalidObjectId)) // written so that NaN fails too: its cast to u16 is undefined
    {
        Msg("! [storage] npc_storage.%s: object id %g is out of range", function, number);
        return false;
    }
    out = static_cast<u16>(number);
    return true;
}

// npc_storage.acquire(id, backing): a new entry of the object; backing is the table that holds its fields on the
// Lua side. Returns the handle. An entry still live for the id (a binder replaced it without db.del_obj, or the
// id came back after a spawn that never reached net_Destroy) is ended first: a new generation, the old fields gone.
int LuaAcquire(lua_State* L)
{
    u16 id = 0;
    if (!LuaObjectId(L, 1, "acquire", id) || lua_type(L, 2) != LUA_TTABLE)
    {
        if (lua_type(L, 2) != LUA_TTABLE)
            Msg("! [storage] npc_storage.acquire: the backing table is missing");
        lua_pushnil(L);
        return 1;
    }
    Slot& slot = EnsureSlot(id);
    if (slot.live)
    {
        if (IsDebugLog())
            Msg("~ [storage] entry of object %u replaced while live: new generation", id);
        Release(id);
    }
    slot.generation = slot.generation == 0xFFFF ? 1 : slot.generation + 1;
    slot.live = true;
    ClearFields(slot);
    lua_pushvalue(L, 2);
    slot.backing_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ++g_live;
    lua_pushnumber(L, static_cast<lua_Number>(HandleOf(id, slot)));
    return 1;
}

// npc_storage.release(id): true when there was a live entry.
int LuaRelease(lua_State* L)
{
    u16 id = 0;
    if (!LuaObjectId(L, 1, "release", id))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    Slot* slot = SlotOf(id);
    const bool was_live = slot && slot->live;
    Release(id);
    lua_pushboolean(L, was_live ? 1 : 0);
    return 1;
}

// npc_storage.set(id, key, value): a mirrored field changed on the Lua side. false when the entry is not live
// (a write after net_Destroy) or the key is not mirrored; both are silent, the proxy only calls with known keys.
int LuaSet(lua_State* L)
{
    u16 id = 0;
    if (!LuaObjectId(L, 1, "set", id))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    const s32 key = lua_type(L, 2) == LUA_TSTRING ? KeyIndex(lua_tostring(L, 2)) : -1;
    Slot* slot = SlotOf(id);
    if (key < 0 || !slot || !slot->live)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    const GwpValue value = events::LuaToValue(L, 3);
    CheckSlot(id, *slot, key, "a Lua write of", kKeys[key]);
    // A switch of the section: the scheme may have added its evaluators, the native ones follow (goap)
    const bool logic_changed = key == kActiveSectionKey && !SameValue(slot->fields[key], value);
    Store(slot->fields[key], value);
    if (logic_changed)
        goap::OnLogicChanged(id);
    lua_pushboolean(L, 1);
    return 1;
}

// npc_storage.keys(): the mirrored keys, an array of strings in index order.
int LuaKeys(lua_State* L)
{
    lua_createtable(L, static_cast<int>(kKeyCount), 0);
    for (u32 i = 0; i < kKeyCount; ++i)
    {
        lua_pushstring(L, kKeys[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// npc_storage.handle(id): the handle of the live entry, nil without one.
int LuaHandle(lua_State* L)
{
    u16 id = 0;
    const Slot* slot = LuaObjectId(L, 1, "handle", id) ? SlotOf(id) : nullptr;
    if (slot && slot->live)
        lua_pushnumber(L, static_cast<lua_Number>(HandleOf(id, *slot)));
    else
        lua_pushnil(L);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group storage)
// ---------------------------------------------------------------------------------------------

GwpStorageHandle GWP_CALL ApiStorageHandle(GwpObjectId id)
{
    if (!CheckCall("storage_handle"))
        return GWP_INVALID_STORAGE_HANDLE;
    const Slot* slot = SlotOf(id);
    return slot && slot->live ? HandleOf(id, *slot) : GWP_INVALID_STORAGE_HANDLE;
}

int GWP_CALL ApiStorageValid(GwpStorageHandle handle)
{
    return CheckCall("storage_valid") && SlotOfHandle(handle) ? 1 : 0;
}

int32_t GWP_CALL ApiStorageKeyIndex(const char* key) { return KeyIndex(key); }

uint32_t GWP_CALL ApiStorageKeyCount() { return kKeyCount; }

const char* GWP_CALL ApiStorageKeyName(uint32_t key) { return key < kKeyCount ? kKeys[key] : nullptr; }

int GWP_CALL ApiStorageGet(GwpStorageHandle handle, uint32_t key, GwpValue* out)
{
    if (out)
        *out = events::Nil();
    if (!CheckCall("storage_get") || !out || key >= kKeyCount)
        return 0;
    Slot* slot = SlotOfHandle(handle);
    if (!slot)
        return 0;
    CheckField(static_cast<u16>(handle & 0xFFFFu), *slot, key, "storage_get of", kKeys[key]);
    *out = Load(slot->fields[key]);
    return 1;
}

// Writes the Lua side: backing[field] for a plain key, backing[sub]'s own backing[field] for "sub.field" (the
// subtable is a proxy too, its metatable names its backing; a plain subtable without one takes the value directly).
// Every access is raw: a metamethod of a script table must not run from here. A subtable with a metatable that is
// not a proxy of the bridge (no backing table in it) is not written: its __newindex may mean anything.
bool WriteLua(const Slot& slot, pcstr key, const GwpValue& value, pcstr addon)
{
    lua_State* L = ActiveLua();
    if (!L || slot.backing_ref == LUA_NOREF)
        return false;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, slot.backing_ref);
    if (!lua_istable(L, -1))
    {
        lua_settop(L, top);
        return false;
    }
    pcstr field = key;
    if (const pcstr dot = strchr(key, '.'))
    {
        string64 sub;
        const size_t length = std::min<size_t>(static_cast<size_t>(dot - key), sizeof(sub) - 1);
        strncpy_s(sub, sizeof(sub), key, length);
        sub[length] = 0;
        field = dot + 1;
        lua_pushstring(L, sub);
        lua_rawget(L, -2); // the subtable (a proxy) or nil
        if (!lua_istable(L, -1))
        {
            lua_settop(L, top);
            if (IsDebugLog())
                Msg("~ [plugin:%s] storage_set '%s': the scripts have no '%s' table for this object yet", addon, key, sub);
            return false;
        }
        if (lua_getmetatable(L, -1))
        {
            lua_pushliteral(L, "backing");
            lua_rawget(L, -2);
            if (!lua_istable(L, -1))
            {
                lua_settop(L, top);
                if (IsDebugLog())
                    Msg("~ [plugin:%s] storage_set '%s': the '%s' table of this object has a metatable of its own, "
                        "not written", addon, key, sub);
                return false;
            }
            lua_replace(L, -3); // the sub proxy is replaced by its backing
            lua_settop(L, top + 2);
        }
    }
    // the target: the backing of the entry or of a sub proxy, or a plain subtable without a metatable
    lua_pushstring(L, field);
    events::PushLuaValue(L, value);
    lua_rawset(L, -3);
    lua_settop(L, top);
    return true;
}

GwpResult GWP_CALL ApiStorageSet(const GwpPlugin* self, GwpStorageHandle handle, uint32_t key, const GwpValue* input)
{
    if (!CheckCall("storage_set"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    const pcstr addon = PluginAddonId(self);
    if (!self || key >= kKeyCount || !input)
        return GWP_ERROR_INVALID_ARGUMENT;
    // A string with ptr == NULL is a nil on both sides (Lua gets nil from PushLuaValue, the mirror NIL)
    const GwpValue normalized = Normalized(*input);
    const GwpValue* value = &normalized;
    switch (value->type)
    {
    case GWP_T_NIL:
    case GWP_T_BOOL:
    case GWP_T_INT:
    case GWP_T_NUMBER:
    case GWP_T_STRING: break;
    default:
        if (IsDebugLog())
            Msg("~ [plugin:%s] storage_set '%s': only nil, bool, int, number and string can be written", addon,
                kKeys[key]);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    Slot* slot = SlotOfHandle(handle);
    if (!slot)
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] storage_set '%s': stale handle 0x%08x", addon, kKeys[key], handle);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    if (!WriteLua(*slot, kKeys[key], *value, addon))
        return GWP_ERROR;
    const u16 id = static_cast<u16>(handle & 0xFFFFu);
    CheckSlot(id, *slot, static_cast<s32>(key), "storage_set of", kKeys[key]);
    // The same as LuaSet: WriteLua is a raw set, the proxy of the bridge does not see it and never calls LuaSet
    const bool logic_changed = static_cast<s32>(key) == kActiveSectionKey && !SameValue(slot->fields[key], *value);
    Store(slot->fields[key], *value);
    if (logic_changed)
        goap::OnLogicChanged(id);
    return GWP_OK;
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CNpcStorageScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CNpcStorageScript::script_register(lua_State* L)
{
    ResetAll(); // the entries of the previous state are gone with it
    g_lua = L;
    const luaL_Reg functions[] = {
        { "acquire", &LuaAcquire },
        { "release", &LuaRelease },
        { "set", &LuaSet },
        { "keys", &LuaKeys },
        { "handle", &LuaHandle },
    };
    lua_newtable(L);
    for (const luaL_Reg& function : functions)
    {
        lua_pushcfunction(L, function.func);
        lua_setfield(L, -2, function.name);
    }
    lua_setglobal(L, "npc_storage");
}

void FillEngineApi(GwpEngineApi& api)
{
    api.storage_handle = &ApiStorageHandle;
    api.storage_valid = &ApiStorageValid;
    api.storage_key_index = &ApiStorageKeyIndex;
    api.storage_key_count = &ApiStorageKeyCount;
    api.storage_key_name = &ApiStorageKeyName;
    api.storage_get = &ApiStorageGet;
    api.storage_set = &ApiStorageSet;
}

void OnObjectDestroy(const CGameObject* object)
{
    if (object && object->ID() != kInvalidObjectId)
        Release(object->ID());
}

void Shutdown()
{
    if (!g_slots)
        return;
    for (u32 id = 0; id < kObjectSlots; ++id)
    {
        // a Field has no destructor: the strings are freed by ClearFields (a foreign pointer is only dropped)
        if (g_slots[id])
            ClearFields(*g_slots[id]);
        xr_delete(g_slots[id]);
    }
    xr_free(g_slots);
    g_slots = nullptr;
    g_lua = nullptr;
    g_live = 0;
}

void PrintList(pcstr args)
{
    Msg("- [storage] %u live entr%s, %u mirrored key(s), %u corrupted field(s) found", g_live,
        g_live == 1 ? "y" : "ies", kKeyCount, g_corruptions);
    const int id = args && args[0] ? atoi(args) : -1;
    if (id < 0)
    {
        Msg("- [storage] storage_list <id> prints the mirrored fields of one entry");
        return;
    }
    const Slot* slot = id < static_cast<int>(kInvalidObjectId) ? SlotOf(static_cast<u16>(id)) : nullptr;
    if (!slot || !slot->live)
    {
        Msg("- [storage] object %d has no live entry", id);
        return;
    }
    Msg("- [storage] object %d, generation %u, handle 0x%08x:", id, slot->generation,
        HandleOf(static_cast<u16>(id), *slot));
    for (u32 i = 0; i < kKeyCount; ++i) // the printing below dereferences STRING members: check first
        CheckField(static_cast<u16>(id), const_cast<Slot&>(*slot), i, "storage_list", nullptr);
    for (u32 i = 0; i < kKeyCount; ++i)
    {
        const Field& field = slot->fields[i];
        if (field.type == GWP_T_NIL)
            continue;
        if (field.type == GWP_T_STRING)
            Msg("-   %-36s %-7s '%s'", kKeys[i], TypeName(field.type), field.string); // up to a NUL inside
        else if (field.type == GWP_T_LUA_REF)
            Msg("-   %-36s %-7s (not a scalar)", kKeys[i], TypeName(field.type));
        else
            Msg("-   %-36s %-7s %g", kKeys[i], TypeName(field.type), field.number);
    }
}
} // namespace gw::addons::storage
