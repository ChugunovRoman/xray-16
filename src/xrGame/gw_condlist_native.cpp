#include "StdAfx.h"

// Stage D, W4-2 (plans/lua_to_cpp/11-w4-condlist-native-spec.md): native pick_section_from_condlist.
//
// The interpreter lives in the engine on purpose (spec 0.1(a)): it reads the parsed condlist table straight
// from the Lua stack, so no source string and no plugin marshalling is needed. xr_logic.script calls
// _gw_internal.logic.pick_section(actor, npc, condlist); the result is
//   - the picked section (string, or nil when nothing matched) when the native side handled the call,
//     exactly as pick_section_lua returns it - a matched "never" section is the string "never" itself
//     (the `... and nil or ...` line of the script yields it, xr_logic.script:501),
//   - false when the condlist contains anything the native side cannot evaluate: then Lua stays
//     authoritative and re-runs the whole call from the beginning (fallback is per call, decided before
//     any math.random draw and before any side effect, so the random stream and Lua errors are the same
//     as in the pure Lua mode).
//
// Coverage of iteration W4-2.1 (all conditions are pure readers, mirrored line by line from
// xr_conditions.script):
//   fighting_dist_ge/le, dist_to_actor_le/ge, has_enemy, has_actor_enemy, talking, npc_talking,
//   actor_friend, is_npc_match, check_npc_name (plain substrings only - Lua patterns fall back),
//   actor_in_zone, npc_in_zone (zones come from db.zone_by_name, exactly as the script reads them).
// Coverage of iteration W4-2.2 (plan 60 P-fb: the most frequent fallback reasons of the mod corpus,
// see plans/lua_to_cpp/61-wave3-reports/pfb-p3.md):
//   is_squad_commander (ALife group of the npc and CSE_ALifeOnlineOfflineGroup::commander_id),
//   is_wounded (db.storage[id].wounded.wound_manager.state, the xr_wounded storage), actor_enemy
//   (faction relation registry + db.storage death + sim_brain.actor_faction), sameFaction
//   (sim_brain.initialized + character_community), random_chance (math.random(1, 100), same PRNG
//   stream as ~N). All of them read either ALife data or Lua script state read-only.
// Coverage of iteration W4-2.3 (63-wave3-fixes-spec.md: the run 8 fallbacks were 66 % check_enemy_name
// and 26 % check_squad_name):
//   check_enemy_name (enemy:alive()/enemy:name() plus plain-substring string.find over the params),
//   check_squad_name (get_object_squad(npc) matched against get_story_squad(v).id per param - the
//   story registry tables of story_objects.script are read the same direct way db.storage is).
// Checks ~N, +info/-info and the whole infop_set of +/- infoportions are native; every =func effect
// and every xr_actions[section] function stays in Lua. The native ones run only while has_alife_info,
// give_info, disable_info (_g.script), math.random and the mirrored helpers (kMirroredHelpers) are the
// functions they mirror (SweepIdentityBits, PrngStateOfMathRandom): a replaced one keeps the call in Lua.
// Plan 60 P3 + plan 63 S1 + plan 64 S1: the identity sweep runs once per frame, INCREMENTALLY - a call
// only pays for the bits of its funcMask no earlier call this frame has already looked up
// (g_identityCheckedMask); a mid-frame function replacement is still seen from the next frame -
// accepted, spec 61 wave 3, decision 3.6). The xr_actions[section] check is a shared cache keyed by
// section ref (g_actionsCache, lazily re-verified once a frame) and runs only for the section the call
// actually matched, after evaluation (as Lua does, xr_logic.script:500) - a condlist that needs the
// Lua action restores the PRNG first (PrngGuard) so Lua's full re-run draws the same ~N numbers.
// Registry keys are lightuserdata, fixed names are pre-interned strings (luaL_ref), section/zone names
// of a condlist are registry refs (lua_rawgeti instead of lua_pushlstring), and EnsureRegistryTables is
// a single registry flag check per call.
//
// Switch npc_perf_condlist_native: 0 = pure Lua (default), 1 = native decides, 2 = shadow (native runs
// a dry pass first - no infop_set, no effects - saving and restoring the LuaJIT math.random state, then
// the Lua pass decides and _gw_internal.logic.shadow_result counts the mismatches).
//
// Reentrancy: effects and give_info_portion can run script callbacks that call pick_section again
// (spec 4.2). The interpreter keeps the Lua stack empty while evaluating, so nested calls on the same
// lua_State are safe; no static buffers are used.

#include "ai_space.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "alife_registry_container.h"
#include "alife_simulator.h"
#include "character_community.h"
#include "npc_cpp_profile.h"
#include "performance_cvars.h"
#include "relation_registry.h"
#include "script_game_object.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrScriptEngine/script_space.hpp"
#include "xrServer_Object_Base.h"
#include "xrServerEntities/InfoPortionDefs.h"
#include "xrServerEntities/xrServer_Objects_ALife_Monsters.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace gw::condlist
{
namespace
{
// ---------------------------------------------------------------------------------------------
// Compiled (native) representation of a parsed condlist table
// ---------------------------------------------------------------------------------------------

enum class ParamKind : u8
{
    Number,
    String,
};

struct CompiledParam
{
    ParamKind kind = ParamKind::Number;
    double number = 0.0;
    xr_string text;
};

enum class CheckKind : u8
{
    Prob,  // ~N
    Func,  // =func / !func
    Infop, // +name / -name
};

enum class FuncId : u8
{
    FightingDistGe,
    FightingDistLe,
    DistToActorLe,
    DistToActorGe,
    HasEnemy,
    HasActorEnemy,
    Talking,
    NpcTalking,
    ActorFriend,
    IsNpcMatch,
    CheckNpcName,
    ActorInZone,
    NpcInZone,
    // W4-2.2 (plan 60 P-fb): the most frequent fallback reasons of the mod content
    IsSquadCommander,
    IsWounded,
    ActorEnemy,
    SameFaction,
    RandomChance,
    // W4-2.3 (plan 63): the run 8 unknown_func top (check_enemy_name 66 %, check_squad_name 26 %)
    CheckEnemyName,
    CheckSquadName,
    // plan 64 §4 C (last big W4-2.3 fallback, beh_companion.ltx:31, ~21/s in run 9)
    IsEnemyFightingActor,
    Count
};

struct FuncDesc
{
    pcstr name;
    FuncId id;
    bool needsFirstGo;    // the function calls a game_object method on pick's first argument
    bool needsSecondGo;   // ... on pick's second argument (npc)
    bool needsSecondName; // npc:name() - game object or server object
    bool needsSecondAny;  // the npc slot must be present at all (a nil npc indexes it in Lua)
    bool needsDbActor;    // reads db.actor itself
    bool zones;           // params are zone names looked up in db.zone_by_name
    bool communityFirst;  // character_community() on pick's first slot (a null one errors in Lua)
    bool communitySecond; // ... on pick's npc slot
    bool usesRandom;      // draws math.random (the stock generator is required, like ~N)
    // review2-condlist-lua.md SF-2: the file the function must be defined in the first time it is
    // remembered (lua_getinfo(">S")), so an addon patch installed before the first pick_section call
    // is not silently accepted as "the original"
    pcstr expectedSource;
};

// Argument flags mirror the script bodies (xr_conditions.script):
// fighting_dist_ge(enemy, npc, p) uses both objects, has_actor_enemy reads db.actor, npc_talking uses
// the npc only, actor_in_zone checks db.actor against db.zone_by_name, and so on.
constexpr pcstr kXrConditionsSource = "xr_conditions.script";
constexpr FuncDesc kFuncs[] = {
    { "fighting_dist_ge", FuncId::FightingDistGe, true, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "fighting_dist_le", FuncId::FightingDistLe, true, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "dist_to_actor_le", FuncId::DistToActorLe, true, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "dist_to_actor_ge", FuncId::DistToActorGe, true, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "has_enemy", FuncId::HasEnemy, false, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "has_actor_enemy", FuncId::HasActorEnemy, false, true, false, false, true, false, false, false, false, kXrConditionsSource },
    { "talking", FuncId::Talking, true, false, false, false, false, false, false, false, false, kXrConditionsSource },
    { "npc_talking", FuncId::NpcTalking, false, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "actor_friend", FuncId::ActorFriend, true, true, false, false, false, false, false, false, false, kXrConditionsSource },
    { "is_npc_match", FuncId::IsNpcMatch, false, false, true, false, false, false, false, false, false, kXrConditionsSource },
    { "check_npc_name", FuncId::CheckNpcName, false, false, true, false, false, false, false, false, false, kXrConditionsSource },
    { "actor_in_zone", FuncId::ActorInZone, false, false, false, false, true, true, false, false, false, kXrConditionsSource },
    { "npc_in_zone", FuncId::NpcInZone, false, false, false, false, false, true, false, false, false, kXrConditionsSource },
    { "is_squad_commander", FuncId::IsSquadCommander, false, false, false, true, false, false, false, false, false, kXrConditionsSource },
    { "is_wounded", FuncId::IsWounded, false, true, false, true, false, false, false, false, false, kXrConditionsSource },
    { "actor_enemy", FuncId::ActorEnemy, true, true, false, true, false, false, false, true, false, kXrConditionsSource },
    { "sameFaction", FuncId::SameFaction, true, true, false, true, false, false, true, true, false, kXrConditionsSource },
    { "random_chance", FuncId::RandomChance, false, false, false, false, false, false, false, false, true, kXrConditionsSource },
    // check_enemy_name guards its first slot in Lua (`if enemy and enemy:alive()`) - a nil one is a
    // plain false (firstOptionalNeeded in CompileCondlist), the npc slot is never touched
    { "check_enemy_name", FuncId::CheckEnemyName, false, false, false, false, false, false, false, false, false, kXrConditionsSource },
    // check_squad_name: get_object_squad(nil) is nil in Lua (a falsy answer, no error)
    { "check_squad_name", FuncId::CheckSquadName, false, false, false, false, false, false, false, false, false, kXrConditionsSource },
    // is_enemy_fighting_actor(enemy, npc) (xr_conditions.script:344-349): enemy:alive() guards the
    // table read, same precedent as fighting_dist_ge/le - a nil enemy becomes db.actor first (there is
    // no firstOptionalNeeded guard in the Lua body, unlike check_enemy_name)
    { "is_enemy_fighting_actor", FuncId::IsEnemyFightingActor, true, false, false, false, false, false, false, false, false, kXrConditionsSource },
};

constexpr size_t kFuncCount = std::size(kFuncs);

const FuncDesc* FindFunc(pcstr name)
{
    for (size_t i = 0; i < kFuncCount; ++i)
        if (std::strcmp(kFuncs[i].name, name) == 0)
            return &kFuncs[i];
    return nullptr;
}

struct CompiledCheck
{
    CheckKind kind = CheckKind::Infop;
    bool expected = true; // =func / !func
    bool required = true; // +name / -name
    double prob = 0.0;    // ~N
    FuncId func = FuncId::Count;
    u32 zoneBase = 0; // index into CompiledCondlist::zoneNames (zone functions only)
    xr_string name;   // infop name for Infop
    xr_vector<CompiledParam> params;
};

struct CompiledSet
{
    bool required = true; // +name / -name inside %...%
    xr_string name;
};

struct CompiledElement
{
    xr_string section;
    int sectionRef = LUA_NOREF; // registry ref of the interned section string (the result push)
    xr_vector<CompiledCheck> checks;
    xr_vector<CompiledSet> sets;
};

// Why a condlist is not compatible (plan 60 P-fb: the fallback map). failName carries the condition
// or effect name for the named kinds; the counters of LuaStats and the -npc_cpp_profile stages split
// the fallbacks by these kinds.
enum class CompileFail : u8
{
    None = 0,     // compiled fine
    Structure,    // malformed table content (the script would keep or error the same way)
    UnknownFunc,  // a condition function the native table does not have (failName = the function)
    Pattern,      // Lua pattern params of is_npc_match / check_npc_name (failName = the function)
    Effect,       // a =func effect in infop_set (failName = the effect)
    Param,        // params the native side cannot use (failName = the function)
};

struct CompiledCondlist
{
    xr_vector<CompiledElement> elements;
    xr_vector<xr_string> zoneNames;    // all zone lookups, flattened in evaluation order
    xr_vector<int> zoneNameRefs;       // registry refs of zoneNames (StashZones without string hashing)
    xr_vector<FuncId> usedFuncs;       // distinct conditions, for the identity pre-pass
    xr_vector<xr_string> usedSections; // distinct sections, for the xr_actions pre-pass
    xr_vector<int> sectionRefs;        // registry refs of usedSections (the keys of the actions check)
    bool compatible = false; // set at the end of a full compile; false keeps the call in Lua forever
    bool firstGoNeeded = false;
    bool firstOptionalNeeded = false; // check_enemy_name: a nil first slot is a plain false in Lua
    bool secondGoNeeded = false;
    bool secondNameNeeded = false;
    bool secondAnyNeeded = false; // some function indexes the npc slot (is_squad_commander, is_wounded, ...)
    bool dbActorNeeded = false;
    bool hasSets = false;      // some element carries %+name%/%-name%: db.actor gets resolved for it
    bool setsNeedNpcGo = false; // some %+npcx_...% would call npc:give/disable_info_portion
    bool usesRandom = false;    // some ~N check
    bool usesInfop = false;     // some +name/-name check: Lua reads it through has_alife_info
    bool usesCommunityFirst = false;  // character_community of the actor slot (resolved in the pre-pass)
    bool usesCommunitySecond = false; // ... of the npc slot
    CompileFail failKind = CompileFail::None;
    xr_string failName;             // the condition/effect name of the named fail kinds
    u64 funcMask = 0;               // identity bits this condlist needs (plan 63 S1, see SweepIdentityBits)
    // Plan 64 S1: the xr_actions[section] check moved to a shared cache keyed by section ref
    // (g_actionsCache) and runs only for the matched section after evaluation - no per-condlist stamp
    // is kept here any more (see SectionHasActionFunction).
};

// ---------------------------------------------------------------------------------------------
// Small Lua helpers (Lua 5.1 / LuaJIT API). Every helper leaves the stack balanced; StackTopGuard
// heals it on early returns.
// ---------------------------------------------------------------------------------------------

struct StackTopGuard
{
    lua_State* state;
    int top;
    explicit StackTopGuard(lua_State* L) : state(L), top(lua_gettop(L)) {}
    ~StackTopGuard() { lua_settop(state, top); }
};

// A field read through the raw path: condlist tables are plain arrays/maps built by
// xr_logic.parse_condlist / alun_utils.parse_condlist and have no metatables.
void RawGetField(lua_State* L, int tableIndex, pcstr key)
{
    lua_pushstring(L, key);
    lua_rawget(L, tableIndex);
}

// The globals of _g.script the native side stands in for: the infop checks read has_alife_info, the infop_set
// (ApplySets) calls give_info / disable_info (xr_logic.script:446-482). The native side gives, takes and reads the
// infoportions itself, so a replaced one must send the call to Lua like a replaced condition. Remembered under
// their own keys: an xr_conditions function of the same name (or the global through its __index) stays apart.
struct InfoGlobal
{
    pcstr name;
    pcstr refKey;
    bool forChecks; // needed by a condlist with +name/-name checks
    pcstr expectedSource; // SF-2: the file the global must be defined in (lua_getinfo(">S"))
};

constexpr pcstr kGScriptSource = "_g.script";
constexpr InfoGlobal kInfoGlobals[] = {
    { "has_alife_info", "_G.has_alife_info", true, kGScriptSource },
    { "give_info", "_G.give_info", false, kGScriptSource },
    { "disable_info", "_G.disable_info", false, kGScriptSource },
};
constexpr size_t kInfoGlobalCount = std::size(kInfoGlobals);

// The module helpers the condition mirrors stand in for (the same rule as the globals above): the
// bodies of is_squad_commander / is_wounded / actor_enemy / check_squad_name are mirrored in C++
// together with the helpers they call, so a replaced helper keeps the call in Lua too. module
// nullptr = a _g global. funcMask - the FuncId bits whose mirrors read through this helper.
struct MirroredHelper
{
    u32 funcMask;
    pcstr module;
    pcstr name;
    pcstr refKey;
    pcstr expectedSource; // SF-2
};

constexpr u32 FuncBit(FuncId id) { return u32(1) << static_cast<u32>(id); }
constexpr u32 kHelperSquad = FuncBit(FuncId::IsSquadCommander) | FuncBit(FuncId::CheckSquadName);

constexpr MirroredHelper kMirroredHelpers[] = {
    { kHelperSquad, nullptr, "get_object_squad", "_G.get_object_squad", kGScriptSource },
    { kHelperSquad, nullptr, "alife_object_of", "_G.alife_object_of", kGScriptSource },
    { kHelperSquad, nullptr, "alife_object", "_G.alife_object", kGScriptSource },
    { FuncBit(FuncId::IsWounded), "xr_wounded", "is_wounded", "_M.xr_wounded.is_wounded", "xr_wounded.script" },
    { FuncBit(FuncId::ActorEnemy), "game_relations", "get_factions_community", "_M.game_relations.get_factions_community", "game_relations.script" },
    { FuncBit(FuncId::ActorEnemy), "utils", "actor_base_faction", "_M.utils.actor_base_faction", "utils.script" },
    // check_squad_name: get_story_squad -> get_story_object_id -> story_objects.get_story_objects_registry
    { FuncBit(FuncId::CheckSquadName), nullptr, "get_story_squad", "_G.get_story_squad", kGScriptSource },
    { FuncBit(FuncId::CheckSquadName), nullptr, "get_story_object_id", "_G.get_story_object_id", kGScriptSource },
    { FuncBit(FuncId::CheckSquadName), "story_objects", "get_story_objects_registry", "_M.story_objects.get_story_objects_registry", "story_objects.script" },
    // SF-2 (review2-condlist-lua.md): LookupStorySquad reproduces the :get/:get_story_id bodies
    // (story_objects.script:108-128) by reading story_obj_registry's fields directly - these two
    // methods were missing from the sweep even though the mirror stands in for them
    { FuncBit(FuncId::CheckSquadName), "story_objects_registry", "get", "_G.story_objects_registry.get", "story_objects.script" },
    { FuncBit(FuncId::CheckSquadName), "story_objects_registry", "get_story_id", "_G.story_objects_registry.get_story_id", "story_objects.script" },
};
constexpr size_t kMirroredHelperCount = std::size(kMirroredHelpers);

// The identity bits of SweepIdentityBits / CompiledCondlist::funcMask (plan 63 S1): the function ids
// first, then the kInfoGlobals, then the kMirroredHelpers. plan 64 S1 mellow #2: a static assert on
// the bit budget of the u64 mask - the next function/helper that overflows it must widen the mask,
// not fail silently through an undefined shift.
constexpr u64 InfoGlobalBit(size_t i) { return u64(1) << (kFuncCount + i); }
constexpr u64 MirroredHelperBit(size_t i) { return u64(1) << (kFuncCount + kInfoGlobalCount + i); }
static_assert(kFuncCount <= 32, "FuncBit() shifts a u32 - FuncId::Count must stay within 32 bits");
static_assert(kFuncCount + kInfoGlobalCount + kMirroredHelperCount <= 64,
    "SweepIdentityBits/g_okMask pack every identity bit into a u64 - widen the mask before adding more");

// Pre-interned strings of one Lua state (plan 60 P3 b): one luaL_ref per fixed name, pushed with
// lua_rawgeti instead of re-hashing the C string on every lookup. The game keeps one script state;
// a different state pointer rebuilds the refs (the old state is gone together with its registry).
// Filled by EnsureRegistryTables before the first call goes through.
struct FixedIntern
{
    lua_State* L = nullptr;
    int db = LUA_NOREF;
    int actor = LUA_NOREF;
    int zoneByName = LUA_NOREF;
    int math = LUA_NOREF;
    int random = LUA_NOREF;
    int xrConditions = LUA_NOREF;
    int xrActions = LUA_NOREF;
    int storage = LUA_NOREF;
    int xrCombatIgnore = LUA_NOREF;        // is_enemy_fighting_actor (W4-2.3)
    int fightingWithActorNpcs = LUA_NOREF; // ... its data table (xr_combat_ignore.script:6)
    int infoNames[kInfoGlobalCount] = {};
    int infoRefKeys[kInfoGlobalCount] = {};
    int helperModules[kMirroredHelperCount] = {};
    int helperNames[kMirroredHelperCount] = {};
    int helperRefKeys[kMirroredHelperCount] = {};
    int funcNames[kFuncCount] = {};
};
FixedIntern g_intern;

// review2-condlist-lua.md SF-1: xr_actions[section] checked once per section per frame (not once per
// compiled condlist, M13's per-condlist stamp), and only for the section an evaluated call actually
// matched (as Lua does, xr_logic.script:500) instead of every section a condlist could reach. Entries
// are keyed by the section's registry ref (CompiledElement::sectionRef, already interned - P3 b);
// the frame tag makes a stale entry re-verify lazily instead of being swept on every frame change.
// Cleared in BuildFixedIntern: a new Lua state means a new xr_actions table.
struct ActionSectionCacheEntry
{
    u32 frame = ~u32(0);
    bool isFunction = false;
};
std::unordered_map<int, ActionSectionCacheEntry> g_actionsCache;

// Plan 64 S1 (review2-condlist-lua.md SF-1): the identity sweep is incremental per frame - a call only
// pays for the funcMask bits no earlier call this frame has already looked up (g_identityCheckedMask);
// g_okMask accumulates the verified-good ones for the rest of the frame. Declared here (not next to
// SweepIdentityBits, which uses them) so EnsureRegistryTables can reset them when a new Lua state is
// built, mellow #1 of review2: a last call of an old machine and a first call of a new one landing on
// the same Device.dwFrame must not share the old mask with an empty remembered-functions table.
u64 g_okMask = 0;
u64 g_identityCheckedMask = 0;
u32 g_identityFrame = ~u32(0); // Device.dwFrame of the current mask

int InternCStr(lua_State* L, pcstr text)
{
    lua_pushstring(L, text);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

void BuildFixedIntern(lua_State* L)
{
    g_intern = FixedIntern{};
    g_intern.L = L;
    g_actionsCache.clear(); // a new Lua state means a new (empty) xr_actions table - review2 SF-1
    g_intern.db = InternCStr(L, "db");
    g_intern.actor = InternCStr(L, "actor");
    g_intern.zoneByName = InternCStr(L, "zone_by_name");
    g_intern.math = InternCStr(L, "math");
    g_intern.random = InternCStr(L, "random");
    g_intern.xrConditions = InternCStr(L, "xr_conditions");
    g_intern.xrActions = InternCStr(L, "xr_actions");
    g_intern.storage = InternCStr(L, "storage");
    g_intern.xrCombatIgnore = InternCStr(L, "xr_combat_ignore");
    g_intern.fightingWithActorNpcs = InternCStr(L, "fighting_with_actor_npcs");
    for (size_t i = 0; i < kInfoGlobalCount; ++i)
    {
        g_intern.infoNames[i] = InternCStr(L, kInfoGlobals[i].name);
        g_intern.infoRefKeys[i] = InternCStr(L, kInfoGlobals[i].refKey);
    }
    for (size_t i = 0; i < kMirroredHelperCount; ++i)
    {
        g_intern.helperModules[i] =
            kMirroredHelpers[i].module ? InternCStr(L, kMirroredHelpers[i].module) : LUA_NOREF;
        g_intern.helperNames[i] = InternCStr(L, kMirroredHelpers[i].name);
        g_intern.helperRefKeys[i] = InternCStr(L, kMirroredHelpers[i].refKey);
    }
    for (size_t i = 0; i < kFuncCount; ++i)
        g_intern.funcNames[i] = InternCStr(L, kFuncs[i].name);
}

// table[name] through an interned string key (same semantics as lua_getfield). The table index is
// resolved to an absolute one first: the name push below must not shift a negative index.
void GetFieldByRef(lua_State* L, int tableIndex, int nameRef)
{
    if (tableIndex < 0 && tableIndex > LUA_REGISTRYINDEX)
        tableIndex = lua_gettop(L) + tableIndex + 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, nameRef);
    lua_gettable(L, tableIndex);
}

bool IsPlainSubstring(pcstr pattern)
{
    // Lua pattern magic (string.match/string.find): anything with these characters needs real Lua.
    for (pcstr p = pattern; *p; ++p)
    {
        switch (*p)
        {
        case '^': case '$': case '(': case ')': case '%': case '.': case '[': case ']': case '*':
        case '+': case '-': case '?':
            return false;
        default: break;
        }
    }
    return true;
}

// The LuaJIT PRNG state of math.random: userdata of 4 uint64 (PRNGState, Externals/LuaJIT/src/lj_def.h).
// Kept in sync with that header by review; the size check rejects any other layout.
constexpr size_t kPrngStateBytes = 32;

struct PrngGuard
{
    u64 saved[4];
    void* state = nullptr;
    bool savedOk = false;
};

// Returns the payload of upvalue 1 of math.random when it is the stock LuaJIT C function with the
// PRNGState userdata, nullptr otherwise (a replaced generator cannot be snapshotted - shadow falls back).
void* PrngStateOfMathRandom(lua_State* L)
{
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.math);
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return nullptr;
    }
    GetFieldByRef(L, -1, g_intern.random);
    if (lua_iscfunction(L, -1) == 0)
    {
        lua_pop(L, 2);
        return nullptr;
    }
    const int top = lua_gettop(L); // stack: ..., math, math.random
    void* payload = nullptr;
    if (lua_getupvalue(L, -1, 1)) // pushes the upvalue when it exists
    {
        if (lua_type(L, -1) == LUA_TUSERDATA && lua_objlen(L, -1) == kPrngStateBytes)
            payload = lua_touserdata(L, -1);
    }
    lua_settop(L, top - 2); // drop math.random and math
    return payload;
}

void SavePrng(lua_State* L, PrngGuard& guard)
{
    guard.state = PrngStateOfMathRandom(L);
    if (!guard.state)
        return;
    std::memcpy(guard.saved, guard.state, kPrngStateBytes);
    guard.savedOk = true;
}

void RestorePrng(const PrngGuard& guard)
{
    if (guard.savedOk && guard.state)
        std::memcpy(guard.state, guard.saved, kPrngStateBytes);
}

// One math.random(100) draw from the same LuaJIT generator, exactly as xr_logic.script:376,397 does it.
// An unprotected call: LuaPickSection lets a condlist with ~N through only while math.random is the stock C
// function (PrngStateOfMathRandom), which cannot fail for the argument 100 and runs no script code.
double LuaRandom100(lua_State* L)
{
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.math);
    GetFieldByRef(L, -1, g_intern.random);
    lua_pushinteger(L, 100);
    lua_call(L, 1, 1);
    const double value = lua_tonumber(L, -1);
    lua_pop(L, 2); // math table + the result
    return value;
}

// ---------------------------------------------------------------------------------------------
// Infoportions: the exact C++ path of the exported alife():has_info / db.actor:give_info_portion
// ---------------------------------------------------------------------------------------------

// Mirror of the free function has_info(self, id, info) exported in alife_simulator_script.cpp:334 -
// no alife().initialized() check, exactly like Lua sees it. The simulator itself stays const:
// registry() is a const member that hands out the mutable container (ai_space.h:88-89).
bool HasInfoPortion(const CALifeSimulator* sim, u16 id, pcstr infoId)
{
    if (!sim)
        return false;
    const KNOWN_INFO_VECTOR* portions = sim->registry(info_portions).object(id, true);
    if (!portions)
        return false;
    return std::find_if(portions->begin(), portions->end(), CFindByIDPred(infoId)) != portions->end();
}

// ---------------------------------------------------------------------------------------------
// Per-call context: objects resolved once (luabind casts are not free), before any evaluation
// ---------------------------------------------------------------------------------------------

enum class DbActorState : u8
{
    Nil,
    GameObject,
    Other,
};

struct CallCtx
{
    lua_State* L = nullptr;
    CScriptGameObject* first = nullptr;  // pick's actor slot: the actor, or the enemy in combat_ignore
    CScriptGameObject* second = nullptr; // pick's npc slot: game object when online
    CSE_Abstract* secondServer = nullptr;
    bool firstIsGo = false;
    bool firstPresent = false; // the slot (or its db.actor substitute) holds a value
    bool secondIsGo = false;
    bool secondIsServer = false;
    bool hasNpcId = false; // npc ~= actor and has a server id (xr_logic.script:381-387)
    u16 npcId = 0;
    // npc_id as the Lua code sees it (pick_section_lua:399): npc:id() for a game object, the .id
    // property/field otherwise - may differ from npcId for a fractional table field (is_squad_commander
    // writes it into the global npc_id exactly like the script does)
    double npcIdLuaValue = 0.0;
    // group_id as the Lua code sees it (get_object_squad, _g.script:2116): a number property of
    // CSE_ALifeMonsterAbstract (or a plain field of a table npc), absent for game objects and for
    // server objects outside the monster hierarchy. Mirrors the first branch of get_object_squad.
    bool groupIdIsNumber = false;
    double groupIdValue = 0.0;
    const CALifeSimulator* sim = nullptr; // alife() of this call, resolved once like the script does
    bool dbActorResolved = false;
    DbActorState dbActorState = DbActorState::Nil;
    CScriptGameObject* dbActor = nullptr;
    bool communityResolved = false;   // CharacterCommunity of the slots (actor_enemy, sameFaction)
    bool communityEvaluable = false;  // every needed slot has one: a null one errors in Lua (pre-pass)
    pcstr firstCommunity = nullptr;
    pcstr secondCommunity = nullptr;
    bool woundedResolved = false; // is_wounded storage chain (ResolveWounded)
    bool woundedEvaluable = false;
    bool woundedState = false;
    bool deathResolved = false; // actor_enemy db.storage[].death chain (ResolveDeath)
    bool deathEvaluable = false;
    bool hasDeath = false;
    bool deathKillerIsId = false;
    bool actorFactionResolved = false; // the faction actor_enemy compares against (ResolveActorFaction)
    bool actorFactionEvaluable = false;
    // The effective faction of `sim_brain.actor_faction or utils.actor_base_faction()`: the field when
    // set, else sim:actor():community():sub(7). nullptr is not allowed at evaluation - the branch where
    // actor_base_faction itself answers the (falsy) field goes back to Lua (S2).
    pcstr actorFaction = nullptr;
    bool brainInitialized = false; // sim_brain.initialized of sameFaction (M10: the gate runs in the pre-pass)
    bool storyResolved = false;    // check_squad_name story registry shapes (ResolveStoryRegistry)
    bool storyEvaluable = false;
    bool combatIgnoreResolved = false; // is_enemy_fighting_actor's table (ResolveCombatIgnoreTable)
    bool combatIgnoreEvaluable = false;
    xr_vector<CScriptGameObject*> zones; // stashed db.zone_by_name values, aligned with zoneNames
};

// Only luabind instances are userdata; a cast of anything else must not even be attempted.
CScriptGameObject* CastGameObject(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TUSERDATA)
        return nullptr;
    const luabind::object object(luabind::from_stack(L, index));
    return luabind::object_cast_nothrow<CScriptGameObject*>(object, static_cast<CScriptGameObject*>(nullptr));
}

CSE_Abstract* CastServerObject(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TUSERDATA)
        return nullptr;
    const luabind::object object(luabind::from_stack(L, index));
    return luabind::object_cast_nothrow<CSE_Abstract*>(object, static_cast<CSE_Abstract*>(nullptr));
}

// npc:name() for a server object, with the same dangling-object guard the binding has
// (xrServerEntities/xrServer_Objects_script.cpp get_name). Kept in a function without C++ objects
// so it can host the SEH block on Windows.
pcstr SafeServerName(CSE_Abstract* serverObject)
{
#if defined(XR_PLATFORM_WINDOWS)
    __try
    {
        return serverObject->name_replace();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return "<null_cse_abstract>";
    }
#else
    return serverObject->name_replace();
#endif
}

pcstr NpcNameOf(const CallCtx& ctx)
{
    if (ctx.secondIsGo)
        return ctx.second->Name();
    if (ctx.secondIsServer)
        return SafeServerName(ctx.secondServer);
    return nullptr;
}

// db.actor read from Lua globals; Other means "present but not a game object" - such a value makes
// the script error out, so the whole call falls back to Lua (which reproduces the error).
void ResolveDbActor(lua_State* L, CallCtx& ctx)
{
    if (ctx.dbActorResolved)
        return;
    ctx.dbActorResolved = true;
    ctx.dbActorState = DbActorState::Nil;
    StackTopGuard guard(L);
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.db);
    if (!lua_istable(L, -1))
    {
        ctx.dbActorState = DbActorState::Other;
        return;
    }
    GetFieldByRef(L, -1, g_intern.actor);
    const int type = lua_type(L, -1);
    if (type == LUA_TNIL || type == LUA_TNONE)
        return; // stays Nil
    if (CScriptGameObject* actor = CastGameObject(L, -1))
    {
        ctx.dbActor = actor;
        ctx.dbActorState = DbActorState::GameObject;
    }
    else
        ctx.dbActorState = DbActorState::Other;
}

// ---------------------------------------------------------------------------------------------
// Lua script-state reads of the W4-2.2 conditions (xr_wounded storage, db.storage death,
// sim_brain fields). Read-only mirrors of the script accesses. The chain shape is probed in the
// call pre-pass (ResolveWounded/ResolveDeath): a chain that would make the Lua body error sends the
// whole call to Lua, which reproduces the error - the same contract as the argument type checks.
// ---------------------------------------------------------------------------------------------

// db.storage[id]: Ok - the entry table is pushed; Missing - the entry is nil (nothing kept);
// Broken - the lookup itself would error in Lua (db.storage is absent or not a table, or the entry
// is not a table - both scripts index it further).
enum class StorageEntry : u8
{
    Ok,
    Missing,
    Broken,
};

StorageEntry PushStorageEntry(lua_State* L, u16 npcId)
{
    const int top = lua_gettop(L);
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.db);
    if (!lua_istable(L, -1))
    {
        lua_settop(L, top);
        return StorageEntry::Broken;
    }
    GetFieldByRef(L, -1, g_intern.storage);
    if (!lua_istable(L, -1))
    {
        lua_settop(L, top);
        return StorageEntry::Broken;
    }
    lua_rawgeti(L, -1, static_cast<int>(npcId)); // db.storage is a plain id -> state table
    if (lua_isnil(L, -1))
    {
        lua_settop(L, top);
        return StorageEntry::Missing;
    }
    if (!lua_istable(L, -1))
    {
        lua_settop(L, top);
        return StorageEntry::Broken;
    }
    lua_replace(L, top + 1); // keep only the entry
    lua_settop(L, top + 1);
    return StorageEntry::Ok;
}

// is_wounded (xr_conditions.script:610 -> xr_wounded.script:537): false = the chain would error
// in Lua (the call falls back in the pre-pass then). The result mirrors
// tostring(wounded.wound_manager.state) ~= "nil": only nil and the string "nil" are false.
bool ResolveWounded(lua_State* L, CallCtx& ctx)
{
    if (ctx.woundedResolved)
        return ctx.woundedEvaluable;
    ctx.woundedResolved = true;
    ctx.woundedEvaluable = false;
    if (!ctx.hasNpcId)
        return false;
    const int top = lua_gettop(L);
    const StorageEntry entry = PushStorageEntry(L, ctx.npcId);
    if (entry == StorageEntry::Missing)
    {
        ctx.woundedEvaluable = true; // `if npc_storage == nil then return false end`
        ctx.woundedState = false;
        return true;
    }
    if (entry != StorageEntry::Ok)
        return false;
    lua_getfield(L, -1, "wounded");
    if (lua_isnil(L, -1))
    {
        lua_settop(L, top);
        ctx.woundedEvaluable = true; // the guard "if npc_storage.wounded ~= nil" -> false
        ctx.woundedState = false;
        return true;
    }
    if (!lua_istable(L, -1) && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return false; // .wound_manager on a non-table errors
    }
    lua_getfield(L, -1, "wound_manager");
    if (lua_isnil(L, -1))
    {
        lua_settop(L, top);
        return false; // .state on nil errors
    }
    if (!lua_istable(L, -1) && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return false;
    }
    lua_getfield(L, -1, "state");
    ctx.woundedState = !lua_isnil(L, -1) &&
        !(lua_type(L, -1) == LUA_TSTRING && std::strcmp(lua_tostring(L, -1), "nil") == 0);
    lua_settop(L, top);
    ctx.woundedEvaluable = true;
    return true;
}

// actor_enemy (xr_conditions.script:1123): db.storage[npc:id()].death - the entry must exist (the
// script indexes it unguarded), death itself may be absent. The killer id is read per call.
bool ResolveDeath(lua_State* L, CallCtx& ctx)
{
    if (ctx.deathResolved)
        return ctx.deathEvaluable;
    ctx.deathResolved = true;
    ctx.deathEvaluable = false;
    if (!ctx.hasNpcId)
        return false;
    const int top = lua_gettop(L);
    if (PushStorageEntry(L, ctx.npcId) != StorageEntry::Ok)
        return false; // a missing entry means `nil.death` - an error in Lua
    lua_getfield(L, -1, "death");
    if (lua_isnil(L, -1))
    {
        lua_settop(L, top);
        ctx.deathEvaluable = true; // t = nil -> the killer part is false
        ctx.hasDeath = false;
        return true;
    }
    if (!lua_istable(L, -1) && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return false; // t.killer on a non-table errors
    }
    ctx.hasDeath = true;
    lua_getfield(L, -1, "killer");
    ctx.deathKillerIsId = false;
    if (lua_type(L, -1) == LUA_TNUMBER)
    {
        const double killer = lua_tonumber(L, -1);
        // t.killer == actor:id() is a strict Lua comparison of numbers
        if (ctx.firstIsGo && killer == static_cast<double>(ctx.first->ID()))
            ctx.deathKillerIsId = true;
    }
    lua_settop(L, top);
    ctx.deathEvaluable = true;
    return true;
}

// sim_brain[key] truthiness: nil/false -> false, anything else ("" included) -> true.
bool SimBrainFieldTruthy(lua_State* L, pcstr key)
{
    const int top = lua_gettop(L);
    bool result = false;
    lua_getglobal(L, "sim_brain");
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, key);
        result = lua_toboolean(L, -1) != 0;
    }
    lua_settop(L, top);
    return result;
}

// `sim_brain.actor_faction or utils.actor_base_faction()` (xr_conditions.script:1127,
// utils.script:1848) as actor_enemy uses it: the truthy string field wins; otherwise
// actor_base_faction answers sim:actor():community():sub(7) and, with no server actor or a community
// shorter than 7 bytes, the (falsy) field itself. That last branch keeps the
// `npc:relation(actor) >= game_object.enemy` check and its edge cases in Lua (a false field even
// errors inside community_relation), so the whole call goes back to Lua there (plan 63 S2). A truthy
// non-string field cannot be a community name - the call goes to Lua too, which errors on it.
bool ResolveActorFaction(lua_State* L, CallCtx& ctx)
{
    if (ctx.actorFactionResolved)
        return ctx.actorFactionEvaluable;
    ctx.actorFactionResolved = true;
    ctx.actorFactionEvaluable = false;
    ctx.actorFaction = nullptr;
    const int top = lua_gettop(L);
    lua_getglobal(L, "sim_brain");
    if (!lua_istable(L, -1))
    {
        // `sim_brain.actor_faction` indexes a nil/non-table there - let Lua reproduce that error
        lua_settop(L, top);
        return false;
    }
    lua_getfield(L, -1, "actor_faction");
    if (lua_toboolean(L, -1))
    {
        if (lua_type(L, -1) != LUA_TSTRING)
        {
            lua_settop(L, top);
            return false; // a truthy non-string goes to the community lookup, which errors in Lua
        }
        ctx.actorFaction = lua_tostring(L, -1); // interned: alive as long as the table keeps it
        lua_settop(L, top);
        ctx.actorFactionEvaluable = true;
        return true;
    }
    lua_settop(L, top); // the falsy field: utils.actor_base_faction() runs and its actor branch decides
    const CSE_ALifeCreatureActor* simActor = ctx.sim ? ctx.sim->graph().actor() : nullptr;
    pcstr comm = simActor ? simActor->CommunityName() : nullptr;
    if (!comm || std::strlen(comm) < 7)
        return false; // S2: Lua keeps the npc:relation check here (and errors on a false field)
    ctx.actorFaction = comm + 6; // comm:sub(7) of the script
    ctx.actorFactionEvaluable = true;
    return true;
}

// character_community of the slots (xr_conditions.script:18, 1127). A null community (the object is
// not an InventoryOwner, CharacterCommunity logs and returns NULL) makes the Lua body error at the
// string call - the pre-pass sends the whole call to Lua then, which reproduces the error.
bool ResolveCommunities(CallCtx& ctx, bool needFirst, bool needSecond)
{
    if (!ctx.communityResolved)
    {
        ctx.communityResolved = true;
        ctx.communityEvaluable = true;
        if (needFirst)
        {
            ctx.firstCommunity = ctx.firstIsGo ? ctx.first->CharacterCommunity() : nullptr;
            ctx.communityEvaluable = ctx.communityEvaluable && ctx.firstCommunity != nullptr;
        }
        if (needSecond)
        {
            ctx.secondCommunity = ctx.secondIsGo ? ctx.second->CharacterCommunity() : nullptr;
            ctx.communityEvaluable = ctx.communityEvaluable && ctx.secondCommunity != nullptr;
        }
    }
    return ctx.communityEvaluable;
}

// The ALife lookup get_object_squad does through alife_object_of (_g.script:2101-2128): the object
// registry by id, then the group_id property of CSE_ALifeMonsterAbstract. A missing or non-monster
// object yields no squad (the script's error path for the missing property also ends with nil).
CSE_ALifeDynamicObject* AlifeObjectNoAssert(const CALifeSimulator* sim, double id)
{
    if (!sim || !(id >= 0.0 && id < 65535.0)) // NaN fails the range check here; 65535 is "no group"
        return nullptr;
    return sim->objects().object(static_cast<ALife::_OBJECT_ID>(id), true);
}

CSE_ALifeDynamicObject* LookupSquadOfNpc(const CallCtx& ctx)
{
    // First branch of get_object_squad (_g.script:2116): a numeric group_id field/property of the
    // npc object itself - a table npc or a CSE_ALifeMonsterAbstract server object. It RETURNS here,
    // even when the lookup misses (a group_id out of the u16 range is a logged nil in Lua - the
    // is_squad_commander "no squad = commander" answer, M9), so there is no fall-through below.
    if (ctx.groupIdIsNumber)
    {
        if (ctx.groupIdValue != 65535.0) // MAX_COUNT_ID_SIZE: only it takes the second branch
            return AlifeObjectNoAssert(ctx.sim, ctx.groupIdValue);
    }
    // Second branch (_g.script:2122-2128): the server object of the npc, then its group.
    CSE_ALifeDynamicObject* seObj = ctx.hasNpcId ? AlifeObjectNoAssert(ctx.sim, ctx.npcId) : nullptr;
    if (!seObj)
        return nullptr;
    const auto* monster = smart_cast<const CSE_ALifeMonsterAbstract*>(seObj);
    if (!monster || monster->m_group_id == 0xffff)
        return nullptr;
    return AlifeObjectNoAssert(ctx.sim, monster->m_group_id);
}

// The story registry shapes of check_squad_name (story_objects.story_obj_registry and the :get body
// of story_objects.script:108-128). A shape that would make the Lua body error sends the whole call
// to Lua (the pre-pass contract); a missing registry is fine - get_story_objects_registry() would
// create an empty one and every lookup would simply miss.
bool ResolveStoryRegistry(lua_State* L, CallCtx& ctx)
{
    if (ctx.storyResolved)
        return ctx.storyEvaluable;
    ctx.storyResolved = true;
    ctx.storyEvaluable = false;
    const int top = lua_gettop(L);
    lua_getglobal(L, "story_objects");
    if (!lua_istable(L, -1))
    {
        lua_settop(L, top); // `story_objects.get_story_objects_registry()` indexes a nil there
        return false;
    }
    lua_getfield(L, -1, "story_obj_registry");
    const int registryType = lua_type(L, -1);
    if (registryType == LUA_TNIL || registryType == LUA_TNONE)
    {
        lua_settop(L, top);
        ctx.storyEvaluable = true;
        return true;
    }
    if (registryType != LUA_TTABLE && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return false;
    }
    // the :get body indexes self.story_id / self.id_story unguarded
    lua_getfield(L, -1, "story_id");
    const bool storyOk = lua_istable(L, -1) || lua_isuserdata(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "id_story");
    const bool idStoryOk = lua_istable(L, -1) || lua_isuserdata(L, -1);
    lua_pop(L, 1);
    if (!storyOk || !idStoryOk)
    {
        lua_settop(L, top);
        return false;
    }
    // `axr_dynamic_spawn and axr_dynamic_spawn.dynamic_story_id and ...` - only truthy values are indexed
    lua_getglobal(L, "axr_dynamic_spawn");
    if (lua_toboolean(L, -1))
    {
        const int dynType = lua_type(L, -1);
        if (dynType != LUA_TTABLE && !lua_isuserdata(L, -1))
        {
            lua_settop(L, top);
            return false;
        }
        lua_getfield(L, -1, "dynamic_story_id");
        const bool mapOk = !lua_toboolean(L, -1) || lua_istable(L, -1) || lua_isuserdata(L, -1);
        if (!mapOk)
        {
            lua_settop(L, top);
            return false;
        }
    }
    lua_settop(L, top);
    ctx.storyEvaluable = true;
    return true;
}

// is_enemy_fighting_actor's data table (xr_conditions.script:344-349): xr_combat_ignore.fighting_with_actor_npcs
// must be a table for the Lua body not to error indexing it (it is set unconditionally at module scope,
// xr_combat_ignore.script:6, and never cleared - this only trips if the module itself failed to load).
// Validated once per call like the other W4-2.3 state chains above, cached in ctx like them too.
bool ResolveCombatIgnoreTable(lua_State* L, CallCtx& ctx)
{
    if (ctx.combatIgnoreResolved)
        return ctx.combatIgnoreEvaluable;
    ctx.combatIgnoreResolved = true;
    ctx.combatIgnoreEvaluable = false;
    StackTopGuard guard(L);
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.xrCombatIgnore);
    if (!lua_istable(L, -1))
        return false;
    GetFieldByRef(L, -1, g_intern.fightingWithActorNpcs);
    if (!lua_istable(L, -1))
        return false;
    ctx.combatIgnoreEvaluable = true;
    return true;
}

// One `get_story_squad(v)` of check_squad_name (_g.script:2131 -> get_story_object_id:2079 ->
// story_objects_registry:get:108 -> alife_object). The registry tables are read directly (the
// db.storage rule; their shapes are validated by ResolveStoryRegistry): a string v looks up
// story_id[v] and then the axr_dynamic_spawn.dynamic_story_id fallback; a number v goes through
// get_story_id and can only match a numeric id_story entry - its other answers are story names and
// alife_object() on a name logs and misses. nullptr = the Lua call would answer falsy.
CSE_ALifeDynamicObject* LookupStorySquad(lua_State* L, const CallCtx& ctx, const CompiledParam& param)
{
    const int top = lua_gettop(L);
    lua_getglobal(L, "story_objects");
    if (!lua_istable(L, -1))
    {
        lua_settop(L, top);
        return nullptr;
    }
    lua_getfield(L, -1, "story_obj_registry");
    if (lua_type(L, -1) != LUA_TTABLE && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return nullptr; // no registry yet: the real getter would create an empty one
    }
    const int registryIndex = lua_gettop(L);
    if (param.kind == ParamKind::Number)
    {
        lua_getfield(L, registryIndex, "id_story");
        lua_pushnumber(L, param.number);
        lua_gettable(L, -2); // id_story[param] - the normal get, as `self.id_story[obj_id]`
        const bool numeric = lua_type(L, -1) == LUA_TNUMBER;
        const double id = numeric ? lua_tonumber(L, -1) : 0.0;
        lua_settop(L, top);
        return numeric ? AlifeObjectNoAssert(ctx.sim, id) : nullptr;
    }
    lua_getfield(L, registryIndex, "story_id");
    lua_getfield(L, -1, param.text.c_str());
    if (lua_toboolean(L, -1))
    {
        const bool numeric = lua_type(L, -1) == LUA_TNUMBER;
        const double id = numeric ? lua_tonumber(L, -1) : 0.0;
        lua_settop(L, top);
        return numeric ? AlifeObjectNoAssert(ctx.sim, id) : nullptr;
    }
    lua_pop(L, 2); // the miss and story_id: the dynamic fallback of the :get body
    lua_getglobal(L, "axr_dynamic_spawn");
    if (lua_type(L, -1) != LUA_TTABLE && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return nullptr;
    }
    lua_getfield(L, -1, "dynamic_story_id");
    if (lua_type(L, -1) != LUA_TTABLE && !lua_isuserdata(L, -1))
    {
        lua_settop(L, top);
        return nullptr;
    }
    lua_getfield(L, -1, param.text.c_str());
    const bool numeric = lua_type(L, -1) == LUA_TNUMBER;
    const double id = numeric ? lua_tonumber(L, -1) : 0.0;
    lua_settop(L, top);
    return numeric ? AlifeObjectNoAssert(ctx.sim, id) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Compilation: condlist table -> CompiledCondlist
// ---------------------------------------------------------------------------------------------

bool CompileParams(lua_State* L, int paramsIndex, xr_vector<CompiledParam>& out)
{
    const lua_Integer count = lua_objlen(L, paramsIndex);
    out.reserve(static_cast<size_t>(count));
    for (lua_Integer j = 1; j <= count; ++j)
    {
        lua_rawgeti(L, paramsIndex, static_cast<int>(j));
        const int type = lua_type(L, -1);
        if (type == LUA_TNUMBER)
        {
            CompiledParam param;
            param.kind = ParamKind::Number;
            param.number = lua_tonumber(L, -1);
            // The exact Lua string of the number (lua_tolstring converts the stack copy in place):
            // string.find coerces numeric params with tostring semantics (check_enemy_name:5)
            size_t length = 0;
            pcstr text = lua_tolstring(L, -1, &length);
            param.text.assign(text, length);
            out.emplace_back(param);
        }
        else if (type == LUA_TSTRING)
        {
            size_t length = 0;
            pcstr text = lua_tolstring(L, -1, &length);
            CompiledParam param;
            param.kind = ParamKind::String;
            param.text.assign(text, length);
            out.emplace_back(param);
        }
        else
        {
            lua_pop(L, 1);
            return false; // booleans/tables in params never come from the parsers; Lua keeps them
        }
        lua_pop(L, 1);
    }
    return true;
}

// Per-function parameter rules. Everything that would make the script error, print or run a real Lua
// pattern returns false: the call stays in Lua and behaves exactly as today. failKind tells the
// fallback map which rule rejected the params (CompileFail::Pattern or CompileFail::Param).
// paramsTable - the condlist carries a params table at all ({=f} has none, {=f()} has an empty one).
bool ValidateFuncParams(FuncId id, const xr_vector<CompiledParam>& params, bool paramsTable, CompileFail& failKind)
{
    switch (id)
    {
    case FuncId::FightingDistGe:
    case FuncId::FightingDistLe:
    case FuncId::DistToActorLe:
    case FuncId::DistToActorGe:
        // p[1] must be a number: the script computes p[1]^2 / tonumber(d)^2 right away
        failKind = CompileFail::Param;
        return params.size() >= 1 && params[0].kind == ParamKind::Number;
    case FuncId::IsNpcMatch:
        // p[1] is a Lua pattern used with string.match; only plain substrings go native
        failKind = CompileFail::Pattern;
        return params.size() >= 1 && params[0].kind == ParamKind::String &&
            IsPlainSubstring(params[0].text.c_str());
    case FuncId::CheckNpcName:
        // every parameter is a pattern for string.find
        failKind = CompileFail::Pattern;
        if (params.empty())
            return false;
        for (const auto& param : params)
            if (param.kind != ParamKind::String || !IsPlainSubstring(param.text.c_str()))
                return false;
        return true;
    case FuncId::ActorInZone:
    case FuncId::NpcInZone:
        // zone names index db.zone_by_name; #p is read, so params must exist
        failKind = CompileFail::Param;
        if (params.empty())
            return false;
        for (const auto& param : params)
            if (param.kind != ParamKind::String)
                return false;
        return true;
    case FuncId::RandomChance:
        // r = p and p[1] and tonumber(p[1]) or 50 (xr_conditions.script:5). The parser has already
        // run tonumber on every token (xr_logic.parse_func_params:862), so a string param is exactly
        // what tonumber rejected there and it rejects it again - r is 50 for every string (M11: do
        // not parse it with strtod, which accepts inf/nan). A number param (inf included: "1e999"
        // parses to it) stays as it is.
        return true;
    case FuncId::CheckEnemyName:
        // check_enemy_name (xr_conditions.script:331): pairs(p) errors on a nil p (so the params
        // table is required) and every value goes to string.find as a pattern - plain substrings
        // only (a number param is coerced with its tostring form, filled into text by CompileParams)
        failKind = CompileFail::Pattern;
        if (!paramsTable)
            return false;
        for (const auto& param : params)
            if (!IsPlainSubstring(param.text.c_str()))
                return false;
        return true;
    case FuncId::CheckSquadName:
        // check_squad_name (xr_conditions.script:564): pairs(p) errors on a nil p; the values are
        // story ids (strings or numbers - both are plain registry keys)
        failKind = CompileFail::Param;
        return paramsTable;
    case FuncId::HasEnemy:
    case FuncId::HasActorEnemy:
    case FuncId::Talking:
    case FuncId::NpcTalking:
    case FuncId::ActorFriend:
    case FuncId::IsSquadCommander:
    case FuncId::IsWounded:
    case FuncId::ActorEnemy:
    case FuncId::SameFaction:
    case FuncId::IsEnemyFightingActor:
        return true; // the script never touches p
    default:
        return false;
    }
}

void CompileCondlist(lua_State* L, int tableIndex, CompiledCondlist& out)
{
    StackTopGuard guard(L);
    out.compatible = false;
    out.failKind = CompileFail::Structure;

    const lua_Integer elementCount = lua_objlen(L, tableIndex);
    if (elementCount < 0 || elementCount > 4096)
        return; // sanity: parsed lists are dozens at most
    out.elements.reserve(static_cast<size_t>(elementCount));

    for (lua_Integer i = 1; i <= elementCount; ++i)
    {
        lua_rawgeti(L, tableIndex, static_cast<int>(i));
        if (!lua_istable(L, -1))
            return;
        const int elementIndex = lua_gettop(L);
        out.elements.emplace_back();
        CompiledElement& element = out.elements.back();

        // section: always a string from the parsers, trailing spaces included (spec 1.4/G4)
        RawGetField(L, elementIndex, "section");
        if (lua_type(L, -1) != LUA_TSTRING)
            return;
        size_t length = 0;
        pcstr text = lua_tolstring(L, -1, &length);
        element.section.assign(text, length);
        lua_pop(L, 1);
        if (std::find(out.usedSections.begin(), out.usedSections.end(), element.section) == out.usedSections.end())
            out.usedSections.push_back(element.section);

        // infop_check
        RawGetField(L, elementIndex, "infop_check");
        if (!lua_istable(L, -1))
            return; // Lua would fail on #cond.infop_check the same way
        const int checkIndex = lua_gettop(L);
        const lua_Integer checkCount = lua_objlen(L, checkIndex);
        element.checks.reserve(static_cast<size_t>(checkCount));
        for (lua_Integer j = 1; j <= checkCount; ++j)
        {
            lua_rawgeti(L, checkIndex, static_cast<int>(j));
            if (!lua_istable(L, -1))
                return;
            const int infopIndex = lua_gettop(L);
            element.checks.emplace_back();
            CompiledCheck& check = element.checks.back();

            RawGetField(L, infopIndex, "prob");
            if (lua_type(L, -1) != LUA_TNIL)
            {
                if (!lua_isnumber(L, -1))
                    return; // a non-number prob errors inside the Lua comparison
                check.kind = CheckKind::Prob;
                check.prob = lua_tonumber(L, -1);
                out.usesRandom = true;
                lua_pop(L, 1);
            }
            else
            {
                lua_pop(L, 1);
                RawGetField(L, infopIndex, "func");
                if (lua_type(L, -1) != LUA_TNIL)
                {
                    if (lua_type(L, -1) != LUA_TSTRING)
                        return;
                    const FuncDesc* desc = FindFunc(lua_tostring(L, -1)); // used before the pop: the
                    lua_pop(L, 1);                                        // pointer points into the stack value
                    if (!desc)
                    {
                        // unknown or not-yet-native condition: Lua evaluates it (P-fb: the map key)
                        out.failKind = CompileFail::UnknownFunc;
                        RawGetField(L, infopIndex, "func");
                        if (lua_type(L, -1) == LUA_TSTRING)
                            out.failName = lua_tostring(L, -1);
                        lua_pop(L, 1);
                        return;
                    }
                    check.kind = CheckKind::Func;
                    check.func = desc->id;
                    RawGetField(L, infopIndex, "expected");
                    check.expected = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);

                    RawGetField(L, infopIndex, "params");
                    bool paramsTable = false;
                    if (lua_type(L, -1) == LUA_TNIL)
                    {
                        lua_pop(L, 1);
                    }
                    else
                    {
                        if (!lua_istable(L, -1))
                            return;
                        paramsTable = true;
                        const int paramsIndex = lua_gettop(L);
                        if (!CompileParams(L, paramsIndex, check.params))
                        {
                            out.failKind = CompileFail::Param;
                            out.failName = desc->name;
                            return;
                        }
                        lua_pop(L, 1);
                    }
                    CompileFail paramFail = CompileFail::None;
                    if (!ValidateFuncParams(desc->id, check.params, paramsTable, paramFail))
                    {
                        out.failKind = paramFail;
                        out.failName = desc->name;
                        return;
                    }

                    out.firstGoNeeded |= desc->needsFirstGo;
                    // check_enemy_name's Lua body guards its first slot (`if enemy and enemy:alive()`):
                    // a nil one (after the `actor or db.actor` substitution) answers false without an
                    // error, while a present non-game-object errors. One flag per compile keeps the
                    // positional FuncDesc rows unchanged.
                    out.firstOptionalNeeded |= desc->id == FuncId::CheckEnemyName;
                    out.secondGoNeeded |= desc->needsSecondGo;
                    out.secondNameNeeded |= desc->needsSecondName;
                    out.secondAnyNeeded |= desc->needsSecondAny;
                    out.dbActorNeeded |= desc->needsDbActor;
                    out.usesCommunityFirst |= desc->communityFirst;
                    out.usesCommunitySecond |= desc->communitySecond;
                    out.usesRandom |= desc->usesRandom;
                    if (desc->zones)
                    {
                        check.zoneBase = static_cast<u32>(out.zoneNames.size());
                        for (const auto& param : check.params)
                            out.zoneNames.push_back(param.text);
                    }
                    if (std::find(out.usedFuncs.begin(), out.usedFuncs.end(), desc->id) == out.usedFuncs.end())
                        out.usedFuncs.push_back(desc->id);
                }
                else
                {
                    lua_pop(L, 1);
                    RawGetField(L, infopIndex, "name");
                    if (lua_type(L, -1) != LUA_TSTRING)
                        return; // empty element (e.g. ~abc): Lua keeps it (spec G8)
                    size_t nameLength = 0;
                    pcstr nameText = lua_tolstring(L, -1, &nameLength);
                    check.kind = CheckKind::Infop;
                    out.usesInfop = true;
                    check.name.assign(nameText, nameLength); // copied before the pop
                    lua_pop(L, 1);
                    RawGetField(L, infopIndex, "required");
                    check.required = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // infop entry
        }
        lua_pop(L, 1); // infop_check

        // infop_set
        RawGetField(L, elementIndex, "infop_set");
        if (!lua_istable(L, -1))
            return;
        const int setIndex = lua_gettop(L);
        const lua_Integer setCount = lua_objlen(L, setIndex);
        element.sets.reserve(static_cast<size_t>(setCount));
        for (lua_Integer j = 1; j <= setCount; ++j)
        {
            lua_rawgeti(L, setIndex, static_cast<int>(j));
            if (!lua_istable(L, -1))
                return;
            const int infopIndex = lua_gettop(L);
            RawGetField(L, infopIndex, "func");
            if (lua_type(L, -1) != LUA_TNIL)
            {
                // =func effects always stay in Lua (spec 4.1); the map records the effect name
                out.failKind = CompileFail::Effect;
                if (lua_type(L, -1) == LUA_TSTRING)
                    out.failName = lua_tostring(L, -1);
                lua_pop(L, 1);
                return;
            }
            lua_pop(L, 1);
            RawGetField(L, infopIndex, "name");
            if (lua_type(L, -1) != LUA_TSTRING)
                return; // ~N inside %...% has no name and errors in Lua - Lua keeps it
            size_t nameLength = 0;
            pcstr nameText = lua_tolstring(L, -1, &nameLength);
            CompiledSet set;
            set.name.assign(nameText, nameLength); // copied before the pop
            lua_pop(L, 1);
            RawGetField(L, infopIndex, "required");
            set.required = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            // +npcx_.../-npcx_... would call npc:give/disable_info_portion (xr_logic.script:464-480)
            if (set.name.find("npcx_") != xr_string::npos)
                out.setsNeedNpcGo = true;
            out.hasSets = true;
            element.sets.emplace_back(set);
            lua_pop(L, 1); // infop entry
        }
        lua_pop(L, 1); // infop_set
        lua_pop(L, 1); // element
    }

    // Intern the section and zone name strings once (P3 b/c): the actions check uses the section
    // refs as keys and the result push takes the ref of the matched element - no lua_pushlstring.
    out.sectionRefs.reserve(out.usedSections.size());
    for (const xr_string& section : out.usedSections)
    {
        lua_pushlstring(L, section.c_str(), section.size());
        out.sectionRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    }
    for (CompiledElement& element : out.elements)
    {
        const auto it = std::find(out.usedSections.begin(), out.usedSections.end(), element.section);
        element.sectionRef = out.sectionRefs[static_cast<size_t>(it - out.usedSections.begin())];
    }
    out.zoneNameRefs.reserve(out.zoneNames.size());
    for (const xr_string& zone : out.zoneNames)
    {
        lua_pushlstring(L, zone.c_str(), zone.size());
        out.zoneNameRefs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    // The identity bits this condlist needs (plan 63 S1): its condition functions, the info globals
    // it reads or writes through, and the mirrored helpers of its functions. The per-frame sweep
    // fills the matching bits of g_okMask and a call only tests (funcMask & ~g_okMask).
    u64 funcBits = 0;
    for (FuncId id : out.usedFuncs)
        funcBits |= FuncBit(id);
    out.funcMask = funcBits;
    for (size_t i = 0; i < kInfoGlobalCount; ++i)
    {
        const InfoGlobal& global = kInfoGlobals[i];
        if (out.hasSets || (global.forChecks && out.usesInfop))
            out.funcMask |= InfoGlobalBit(i);
    }
    for (size_t i = 0; i < kMirroredHelperCount; ++i)
        if (kMirroredHelpers[i].funcMask & funcBits)
            out.funcMask |= MirroredHelperBit(i);

    out.compatible = true;
    out.failKind = CompileFail::None;
}

// ---------------------------------------------------------------------------------------------
// Registry caches (per Lua state): compiled condlists keyed weakly by the condlist table itself, and
// the remembered xr_conditions[name] functions for the identity pre-pass (spec 4.1). Registry keys
// are lightuserdata (no string interning on every lookup) and a single lightuserdata flag marks the
// state as initialized (plan 60 P3 d). The fixed names are pre-interned strings (luaL_ref) and the
// per-condlist section/zone names are refs too (P3 b/c).
// ---------------------------------------------------------------------------------------------

char kRegCompiledKey;
char kRegFuncRefsKey;
char kRegCompiledMtKey;
char kRegReadyKey;

int FreeCompiled(lua_State* L)
{
    const auto* holder = static_cast<CompiledCondlist**>(lua_touserdata(L, 1));
    if (holder && *holder)
    {
        CompiledCondlist* compiled = *holder;
        for (int ref : compiled->sectionRefs)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        for (int ref : compiled->zoneNameRefs)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        xr_delete(compiled);
    }
    return 0;
}

// Creates the registry tables when they are missing. Called lazily from LuaPickSection (always
// inside a valid Lua frame — NOT from script_register: the export nodes run frameless and this
// LuaJIT build reads the frame slot below L->base in lua_pushcclosure, see CGwCondlistScript).
// Idempotent: one registry flag lookup per call, the rest runs once per Lua state (P3 d).
bool EnsureRegistryTables(lua_State* L)
{
    lua_pushlightuserdata(L, &kRegReadyKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool ready = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    // Only the registry flag: it lives in the registry of the state itself, so a new state rebuilds
    // and a coroutine of the same state (its own lua_State, shared registry - CScriptThread) does
    // not (M7: comparing g_intern.L rebuilt BuildFixedIntern per switch and leaked registry slots).
    if (ready)
        return true;

    StackTopGuard guard(L);
    // Each block: the "no table" branch pops the nil itself and the rawset eats the new table, so only the
    // "table exists" branch has a value left to pop (a pop after both would go below the arguments of the caller)
    lua_pushlightuserdata(L, &kRegCompiledKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L); // weak-keyed cache: entries die together with the condlist table
        lua_newtable(L);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushlightuserdata(L, &kRegCompiledKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
    }
    else
        lua_pop(L, 1);

    lua_pushlightuserdata(L, &kRegCompiledMtKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushcfunction(L, &FreeCompiled);
        lua_setfield(L, -2, "__gc");
        lua_pushlightuserdata(L, &kRegCompiledMtKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
    }
    else
        lua_pop(L, 1);

    lua_pushlightuserdata(L, &kRegFuncRefsKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, &kRegFuncRefsKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
    }
    else
        lua_pop(L, 1);

    BuildFixedIntern(L); // also clears g_actionsCache (a new xr_actions table)

    // review2-condlist-lua.md mellow #1: a new Lua state means the remembered-functions table is
    // empty again - force the next call to re-sweep even if it lands on the same Device.dwFrame as
    // the old machine's last call (a bare frame match would otherwise let it inherit the old g_okMask
    // for the rest of that frame, with none of its bits actually re-verified).
    g_identityFrame = ~u32(0);
    g_identityCheckedMask = 0;
    g_okMask = 0;

    lua_pushlightuserdata(L, &kRegReadyKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    VERIFY(lua_gettop(L) == guard.top);
    return true;
}

// Condlists compiled (a cache miss: a new userdata in the weak cache). Near calls = the condlist tables are new every
// time (32-frame-spikes.md, U2); _gw_internal.logic.stats prints it
u64 g_compiled = 0;

CompiledCondlist* GetCompiled(lua_State* L, int tableIndex)
{
    StackTopGuard guard(L);

    // The registry tables are created lazily by EnsureRegistryTables. If they are missing anyway,
    // compile without caching - slower, but correct.
    lua_pushlightuserdata(L, &kRegCompiledKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool canCache = lua_type(L, -1) == LUA_TTABLE;
    if (canCache)
    {
        lua_pushvalue(L, tableIndex);
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TUSERDATA)
            if (const auto* holder = static_cast<CompiledCondlist**>(lua_touserdata(L, -1)))
                return *holder; // alive: the weak cache entry dies together with the condlist table
    }

    auto* compiled = xr_new<CompiledCondlist>();
    CompileCondlist(L, tableIndex, *compiled);
    ++g_compiled;

    if (canCache)
    {
        lua_pushlightuserdata(L, &kRegCompiledKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, tableIndex);
        auto* holder = static_cast<CompiledCondlist**>(lua_newuserdata(L, sizeof(CompiledCondlist*)));
        *holder = compiled;
        lua_pushlightuserdata(L, &kRegCompiledMtKey);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_setmetatable(L, -2);
        lua_rawset(L, -3);
    }
    return compiled;
}

// lua_getinfo(">S") pops the function given on top of the stack and fills ar.short_src - a file chunk
// without its leading '@' (script_engine.cpp:464 loads it as "@<full path>"), already cut to LUA_IDSIZE
// by LuaJIT's chunk id formatting (a path too long to fit keeps its tail, with a "..." prefix) - exactly
// what a suffix match on the file name wants. review2-condlist-lua.md SF-2.
bool SourceMatches(lua_State* L, pcstr expectedFile)
{
    lua_Debug ar;
    if (!lua_getinfo(L, ">S", &ar))
        return false; // not a Lua function with debug info (a C function: ar.source "=[C]")
    const size_t sourceLen = std::strlen(ar.short_src);
    const size_t fileLen = std::strlen(expectedFile);
    if (sourceLen < fileLen)
        return false;
    const pcstr tail = ar.short_src + (sourceLen - fileLen);
    if (_stricmp(tail, expectedFile) != 0)
        return false;
    if (sourceLen == fileLen)
        return true; // the whole (possibly truncated) name is just the file
    const char sep = ar.short_src[sourceLen - fileLen - 1];
    return sep == '/' || sep == '\\' || sep == '.'; // '.' covers the "..." truncation marker
}

// The function on top of the stack against the one remembered under the refKey string in the refs
// table: the first sight remembers it and accepts it (spec 4.1) - but only once review2 SF-2's source
// check passes (lua_getinfo(">S"), expectedSource): an addon that patches xr_conditions/_g/... before
// the first pick_section call must not be silently accepted as "the original". A rejected first sight
// stays unremembered - the bit is 0 for the rest of the Lua state's life, same as a missing function.
// Pops the function.
bool SameAsRemembered(lua_State* L, int refsIndex, int refKey, pcstr expectedSource)
{
    GetFieldByRef(L, refsIndex, refKey);
    if (lua_type(L, -1) == LUA_TNIL)
    {
        lua_pop(L, 1); // stack: ..., candidate
        lua_pushvalue(L, -1); // a copy for lua_getinfo(">S") to consume - it pops its argument
        const bool sourceOk = SourceMatches(L, expectedSource);
        if (!sourceOk)
        {
            lua_pop(L, 1); // candidate
            return false;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, refKey); // the interned key
        lua_pushvalue(L, -2);
        lua_rawset(L, refsIndex);
        lua_pop(L, 1);
        return true;
    }
    const bool same = lua_rawequal(L, -2, -1) != 0;
    lua_pop(L, 2);
    return same;
}

// The native path runs only while xr_conditions[name] is the very function remembered at its first
// use (lua_rawequal, the same lookup the script itself does). A replaced or missing function means
// Lua: missing prints "not defined in xr_conditions.script" there and counts as a failed condition.
// The same holds for the globals of kInfoGlobals and the kMirroredHelpers the mirrors read through.
// Plan 64 S1 (review2-condlist-lua.md SF-1): the sweep is INCREMENTAL PER FRAME - a call only asks for
// the bits of compiled->funcMask that no earlier call this frame has already verified
// (g_identityCheckedMask), and SweepIdentityBits looks up only those names, not all <= 35 of them.
// g_okMask accumulates the verified-good bits for the rest of the frame; a mid-frame replacement is
// seen from the next frame (decision 3.6 of the wave 3 spec) because g_identityCheckedMask (and
// g_okMask with it) resets once per frame, in LuaPickSection. (g_okMask/g_identityCheckedMask/
// g_identityFrame themselves are declared earlier, next to g_actionsCache - EnsureRegistryTables
// resets them too, on a new Lua state.)
void SweepIdentityBits(lua_State* L, u64 missingMask)
{
    StackTopGuard guard(L);
    lua_pushlightuserdata(L, &kRegFuncRefsKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int refsIndex = lua_gettop(L);

    bool moduleFetched = false;
    bool moduleOk = false;
    int moduleIndex = 0;
    for (size_t i = 0; i < kFuncCount; ++i)
    {
        const u64 bit = FuncBit(kFuncs[i].id);
        if (!(missingMask & bit))
            continue;
        if (!moduleFetched)
        {
            GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.xrConditions);
            moduleIndex = lua_gettop(L);
            moduleOk = lua_istable(L, -1) != 0; // not loaded: the script would fail its own lookup
            moduleFetched = true;
        }
        if (!moduleOk)
            continue; // the bit stays unset in g_okMask - the call falls back like a missing function
        GetFieldByRef(L, moduleIndex, g_intern.funcNames[i]); // honors __index = _G
        const bool isFunction = lua_type(L, -1) == LUA_TFUNCTION;
        const bool same = isFunction && SameAsRemembered(L, refsIndex, g_intern.funcNames[i], kFuncs[i].expectedSource);
        if (!isFunction)
            lua_pop(L, 1); // SameAsRemembered pops the function itself on the other path
        if (same)
            g_okMask |= bit;
    }

    for (size_t i = 0; i < kInfoGlobalCount; ++i)
    {
        const u64 bit = InfoGlobalBit(i);
        if (!(missingMask & bit))
            continue;
        GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.infoNames[i]);
        const bool isFunction = lua_type(L, -1) == LUA_TFUNCTION;
        const bool same = isFunction && SameAsRemembered(L, refsIndex, g_intern.infoRefKeys[i], kInfoGlobals[i].expectedSource);
        if (!isFunction)
            lua_pop(L, 1);
        if (same)
            g_okMask |= bit;
    }

    for (size_t i = 0; i < kMirroredHelperCount; ++i)
    {
        const u64 bit = MirroredHelperBit(i);
        if (!(missingMask & bit))
            continue;
        const MirroredHelper& helper = kMirroredHelpers[i];
        if (helper.module)
        {
            GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.helperModules[i]);
            if (!lua_istable(L, -1))
            {
                lua_pop(L, 1);
                continue; // a missing module: the mirror must not stand in for its helper
            }
            GetFieldByRef(L, -1, g_intern.helperNames[i]);
            lua_remove(L, -2);
        }
        else
            GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.helperNames[i]);
        const bool isFunction = lua_type(L, -1) == LUA_TFUNCTION;
        const bool same = isFunction && SameAsRemembered(L, refsIndex, g_intern.helperRefKeys[i], helper.expectedSource);
        if (!isFunction)
            lua_pop(L, 1);
        if (same)
            g_okMask |= bit;
    }
}

// Plan 65 item 1 follow-up (developer note 2026-10-26, run 11 analysis in plan 57 / 61-wave3-reports):
// the name behind a funcMask bit, same ordering SweepIdentityBits sweeps (kFuncs, then kInfoGlobals,
// then kMirroredHelpers) - so fb_identity can name the first rejected bit the same way FallbackCompiled's
// failName already names fb_unknown (CompileFail::UnknownFunc). Only called on the rejection path below.
pcstr NameOfIdentityBit(u64 bit)
{
    for (size_t i = 0; i < kFuncCount; ++i)
        if (bit == FuncBit(kFuncs[i].id))
            return kFuncs[i].name;
    for (size_t i = 0; i < kInfoGlobalCount; ++i)
        if (bit == InfoGlobalBit(i))
            return kInfoGlobals[i].name;
    for (size_t i = 0; i < kMirroredHelperCount; ++i)
        if (bit == MirroredHelperBit(i))
            return kMirroredHelpers[i].name;
    return nullptr;
}

// xr_actions[section] must not be a function for the section the call actually matched
// (xr_logic.script:500-505): if it is one, the script CALLS it directly rather than falling back -
// the native side cannot run a Lua action, so the whole call goes back to Lua instead. Plan 64 S1
// (review2-condlist-lua.md SF-1, "как Lua"): only the matched section is checked, never every section
// the condlist could reach - the caller (LuaPickSection) is responsible for restoring the PRNG first
// when the condlist draws ~N, so Lua's full re-evaluation draws the same numbers. Cached per section
// ref, lazily re-verified once a frame (g_actionsCache, cleared only on a new Lua state - BuildFixedIntern):
// xr_actions itself essentially never changes at runtime, so most frames pay nothing here at all.
bool SectionHasActionFunction(lua_State* L, int sectionRef, u32 frame)
{
    auto it = g_actionsCache.find(sectionRef);
    if (it != g_actionsCache.end() && it->second.frame == frame)
        return it->second.isFunction;
    StackTopGuard guard(L);
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.xrActions);
    bool isFunction = true; // nil module: the script would error indexing it - keep it in Lua
    if (lua_istable(L, -1))
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sectionRef);
        lua_gettable(L, -2);
        isFunction = lua_type(L, -1) == LUA_TFUNCTION;
    }
    g_actionsCache[sectionRef] = ActionSectionCacheEntry{ frame, isFunction };
    return isFunction;
}

// Stashes db.zone_by_name values before evaluation. false means a value is present but is not a
// game object: Lua would fail its zone:inside() call, so the whole call goes back to Lua. The zone
// keys are the interned refs of the compiled condlist (P3 b).
bool StashZones(lua_State* L, const CompiledCondlist& compiled, CallCtx& ctx)
{
    if (compiled.zoneNames.empty())
        return true;
    StackTopGuard guard(L);
    GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.db);
    if (!lua_istable(L, -1))
        return false;
    GetFieldByRef(L, -1, g_intern.zoneByName);
    if (!lua_istable(L, -1))
        return false; // the script would error indexing db.zone_by_name
    const int zonesIndex = lua_gettop(L);
    ctx.zones.assign(compiled.zoneNames.size(), nullptr);
    for (size_t i = 0; i < compiled.zoneNames.size(); ++i)
    {
        GetFieldByRef(L, zonesIndex, compiled.zoneNameRefs[i]);
        const int type = lua_type(L, -1);
        if (type == LUA_TNIL || type == LUA_TNONE)
        {
            ctx.zones[i] = nullptr; // an unregistered zone never matches, as in the script
        }
        else if (CScriptGameObject* zone = CastGameObject(L, -1))
        {
            ctx.zones[i] = zone;
        }
        else
        {
            lua_pop(L, 1);
            return false;
        }
        lua_pop(L, 1);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Evaluation (mirror of xr_logic.script:390-495)
// ---------------------------------------------------------------------------------------------

bool EvalFunc(const CallCtx& ctx, const CompiledCheck& check)
{
    const auto& params = check.params;
    switch (check.func)
    {
    case FuncId::FightingDistGe:
    case FuncId::FightingDistLe:
    {
        // fighting_dist_ge/le(enemy, npc, p): enemy:alive() and
        // enemy:position():distance_to_sqr(npc:position()) >= p[1]^2 or false
        if (!ctx.first->Alive())
            return false;
        const float distanceSqr = ctx.first->Position().distance_to_sqr(ctx.second->Position());
        const double limit = std::pow(params[0].number, 2.0); // Lua ^, double precision
        return check.func == FuncId::FightingDistGe ? static_cast<double>(distanceSqr) >= limit
                                                    : static_cast<double>(distanceSqr) <= limit;
    }
    case FuncId::DistToActorLe:
    case FuncId::DistToActorGe:
    {
        // dist_to_actor_le/ge(actor, npc, p): npc:position():distance_to_sqr(actor:position()) <= tonumber(d)^2
        const float distanceSqr = ctx.second->Position().distance_to_sqr(ctx.first->Position());
        const double limit = std::pow(params[0].number, 2.0);
        return check.func == FuncId::DistToActorLe ? static_cast<double>(distanceSqr) <= limit
                                                   : static_cast<double>(distanceSqr) >= limit;
    }
    case FuncId::HasEnemy:
    {
        // be = npc:best_enemy(); return be and be:alive() or false
        CScriptGameObject* bestEnemy = ctx.second->GetBestEnemy();
        return bestEnemy && bestEnemy->Alive();
    }
    case FuncId::HasActorEnemy:
    {
        // local best_enemy = npc:best_enemy();  <- computed first, as in the script
        // return db.actor and best_enemy ~= nil and best_enemy:id() == db.actor:id()
        CScriptGameObject* bestEnemy = ctx.second->GetBestEnemy();
        if (ctx.dbActorState != DbActorState::GameObject)
            return false; // db.actor nil -> the Lua expression yields nil (falsy)
        return bestEnemy && bestEnemy->ID() == ctx.dbActor->ID();
    }
    case FuncId::Talking:
        return ctx.first->IsTalking(); // talking(actor, npc) = actor:is_talking()
    case FuncId::NpcTalking:
        return ctx.second->IsTalking(); // npc_talking(actor, npc) = npc:is_talking()
    case FuncId::ActorFriend:
        // npc:relation(actor) == game_object.friend (ALife::eRelationTypeFriend = 0)
        return ctx.second->GetRelationType(ctx.first) == ALife::eRelationTypeFriend;
    case FuncId::IsNpcMatch:
    {
        // npc:name() == nil -> false; string.match(name, p[1]) with a plain pattern = substring
        const pcstr name = NpcNameOf(ctx);
        return name && std::strstr(name, params[0].text.c_str()) != nullptr;
    }
    case FuncId::CheckNpcName:
    {
        // name()==nil -> false; any parameter found as a substring -> true
        const pcstr name = NpcNameOf(ctx);
        if (!name)
            return false;
        for (const auto& param : params)
            if (std::strstr(name, param.text.c_str()) != nullptr)
                return true;
        return false;
    }
    case FuncId::ActorInZone:
    {
        // if not db.actor -> false; db.zone_by_name[p[i]]:inside(db.actor:position())
        if (ctx.dbActorState != DbActorState::GameObject)
            return false;
        for (size_t i = 0; i < params.size(); ++i)
        {
            CScriptGameObject* zone = ctx.zones[check.zoneBase + i];
            if (zone && zone->inside(ctx.dbActor->Position()))
                return true;
        }
        return false;
    }
    case FuncId::NpcInZone:
    {
        // not (npc and type(npc.position)=="function") -> false; only game objects qualify
        if (!ctx.secondIsGo)
            return false;
        for (size_t i = 0; i < params.size(); ++i)
        {
            CScriptGameObject* zone = ctx.zones[check.zoneBase + i];
            if (zone && zone->inside(ctx.second->Position()))
                return true;
        }
        return false;
    }
    case FuncId::IsSquadCommander:
    {
        // is_squad_commander (xr_conditions.script:1598): get_object_squad(npc) and
        // `squad and squad:commander_id() == npc_id or squad == nil` - no squad counts as commander
        // The script writes the global npc_id first (npc.id / npc:id(), :1600-1603) - addons can
        // read it after the call, so the mirror writes the very same value (M9)
        lua_pushnumber(ctx.L, ctx.npcIdLuaValue);
        lua_setglobal(ctx.L, "npc_id");
        CSE_ALifeDynamicObject* squad = LookupSquadOfNpc(ctx);
        if (!squad)
            return true;
        auto* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(squad);
        // a non-group object at the group id is broken content (Lua errors on :commander_id());
        // the native side answers "not the commander" instead of aborting the call. The comparison
        // is the Lua one: a fractional npc_id of a table npc never equals the u16 commander id
        return group && static_cast<double>(group->commander_id()) == ctx.npcIdLuaValue;
    }
    case FuncId::IsWounded:
        // xr_wounded.is_wounded(npc) = the state of the xr_wounded storage (pre-pass ResolveWounded)
        return ctx.woundedState;
    case FuncId::ActorEnemy:
    {
        // actor_enemy (xr_conditions.script:1123):
        //   local t = db.storage[npc:id()].death  <- resolved in the pre-pass
        //   local relation = game_relations.get_factions_community(npc:character_community(),
        //     sim_brain.actor_faction or utils.actor_base_faction())  <- the pre-pass fills
        //     ctx.actorFaction with the effective faction (ResolveActorFaction; the branch where
        //     actor_base_faction answers the falsy field itself goes back to Lua - S2)
        //   if relation and relation > -500 then return false end
        //   return npc:relation(actor) >= game_object.enemy or (t ~= nil and t.killer == actor:id())
        const pcstr npcFaction = ctx.secondCommunity;
        if (npcFaction && std::strcmp(npcFaction, "none") != 0 && ctx.actorFaction &&
            std::strcmp(ctx.actorFaction, "none") != 0)
        {
            // game_relations.get_factions_community (game_relations.script:306) =
            // relation_registry.community_relation (level_script.cpp:595)
            CHARACTER_COMMUNITY from;
            CHARACTER_COMMUNITY to;
            from.set(npcFaction);
            to.set(ctx.actorFaction);
            if (RELATION_REGISTRY().GetCommunityRelation(from.index(), to.index()) > -500)
                return false;
        }
        // ERelationType is unsigned: eRelationTypeDummy = u32(-1) reaches Lua as the int -1, where
        // `-1 >= game_object.enemy` is false. A plain u32 comparison would answer true (M8)
        return static_cast<int>(ctx.second->GetRelationType(ctx.first)) >=
            static_cast<int>(ALife::eRelationTypeEnemy) ||
            (ctx.hasDeath && ctx.deathKillerIsId);
    }
    case FuncId::SameFaction:
    {
        // sameFaction (xr_conditions.script:13): the sim_brain gate first (its result is resolved
        // in the pre-pass - M10: Lua returns false before any character_community call), then
        // first:character_community():sub(7) == second:character_community() (sub(7) = skip 6 bytes;
        // a shorter string yields "", which the `and` chain treats as truthy)
        if (!ctx.brainInitialized)
            return false;
        pcstr actorFaction = "";
        if (ctx.firstCommunity)
            actorFaction = std::strlen(ctx.firstCommunity) >= 7 ? ctx.firstCommunity + 6 : "";
        return std::strcmp(actorFaction, ctx.secondCommunity) == 0;
    }
    case FuncId::RandomChance:
    {
        // random_chance (xr_conditions.script:5): r = p and p[1] and tonumber(p[1]) or 50;
        // math.random(1, 100) draws one u64 and maps it exactly like math.random(100) (LuaJIT
        // lib_math.c: floor(d*(r2-r1+1))+r1 with r1=1, r2=100 = floor(d*100)+1)
        double limit = 50.0;
        if (!params.empty() && params[0].kind == ParamKind::Number)
            limit = params[0].number; // M11: a string param is what tonumber already rejected -> 50
        return LuaRandom100(ctx.L) < limit;
    }
    case FuncId::CheckEnemyName:
    {
        // check_enemy_name (xr_conditions.script:331):
        //   if enemy and enemy:alive() then name = enemy:name(); for i, v in pairs(p) do
        //     if string.find(name, v) ~= nil then return true end end end
        //   return false
        // A nil first slot is a plain false (the pre-pass keeps it, firstOptionalNeeded); a present
        // non-game-object would error on :alive() and never reaches here
        if (!ctx.firstIsGo)
            return false;
        if (!ctx.first->Alive())
            return false;
        const pcstr name = ctx.first->Name();
        if (!name)
            return false;
        for (const auto& param : params)
            if (std::strstr(name, param.text.c_str()) != nullptr)
                return true; // a plain-substring string.find (the patterns fall back at compile)
        return false;
    }
    case FuncId::CheckSquadName:
    {
        // check_squad_name (xr_conditions.script:564):
        //   local squad = get_object_squad(npc); if not squad then return end
        //   for k, v in pairs(p) do sq = get_story_squad(v); if sq and sq.id == squad.id then
        //     return true end end
        //   return  (nil - a falsy answer, the same as false for a condlist)
        CSE_ALifeDynamicObject* squad = LookupSquadOfNpc(ctx);
        if (!squad)
            return false;
        for (const auto& param : params)
        {
            CSE_ALifeDynamicObject* sq = LookupStorySquad(ctx.L, ctx, param);
            if (sq && sq->ID == squad->ID) // sq.id == squad.id
                return true;
        }
        return false;
    }
    case FuncId::IsEnemyFightingActor:
    {
        // is_enemy_fighting_actor (xr_conditions.script:344-349):
        //   if (enemy:alive() and xr_combat_ignore.fighting_with_actor_npcs[enemy:id()]) then
        //     return true end
        //   return false
        // ctx.first is the enemy slot (needsFirstGo: a nil one already became db.actor in the
        // pre-pass, matching fighting_dist_ge/le - there is no firstOptionalNeeded guard here)
        if (!ctx.first->Alive())
            return false;
        // ResolveCombatIgnoreTable already ran in the usedFuncs pre-pass: the module and its table
        // exist, so this is a plain read, like db.storage/db.zone_by_name elsewhere in this file
        lua_State* L = ctx.L;
        StackTopGuard guard(L);
        GetFieldByRef(L, LUA_GLOBALSINDEX, g_intern.xrCombatIgnore);
        GetFieldByRef(L, -1, g_intern.fightingWithActorNpcs);
        lua_pushnumber(L, ctx.first->ID());
        lua_gettable(L, -2);
        return lua_toboolean(L, -1) != 0;
    }
    default:
        return false; // unreachable: ValidateFuncParams lets only known ids compile
    }
}

// infop_set of the matched element: +/- infoportions only (no =func effects can be here).
// xr_logic.script:446-482, including the npcx_ quirk: an NPC-owned portion makes the branch fall
// through to the actor side (give_info/disable_info themselves check db.actor).
void ApplySets(const CallCtx& ctx, const CompiledElement& element)
{
    for (const CompiledSet& set : element.sets)
    {
        const pcstr name = set.name.c_str();
        const bool npcOwned = ctx.hasNpcId && ctx.sim && std::strstr(name, "npcx_") != nullptr;
        if (set.required)
        {
            if (npcOwned && !HasInfoPortion(ctx.sim, ctx.npcId, name))
                ctx.second->GiveInfoPortion(name);
            else if (ctx.dbActor && !HasInfoPortion(ctx.sim, 0, name))
                ctx.dbActor->GiveInfoPortion(name); // Lua give_info: only when db.actor exists
        }
        else
        {
            if (npcOwned && HasInfoPortion(ctx.sim, ctx.npcId, name))
                ctx.second->DisableInfoPortion(name);
            else if (ctx.dbActor && HasInfoPortion(ctx.sim, 0, name))
                ctx.dbActor->DisableInfoPortion(name); // Lua disable_info checks db.actor and has_alife_info
        }
    }
}

// Returns true when an element matched and fills sectionOut with its (byte-exact) section plus the
// registry ref of the interned string (P3 c).
bool EvaluateCompiled(const CallCtx& ctx, const CompiledCondlist& compiled, bool dry, xr_string& sectionOut,
    int& sectionRefOut)
{
    for (const CompiledElement& element : compiled.elements)
    {
        bool conditionsMet = true;
        for (const CompiledCheck& check : element.checks)
        {
            bool pass;
            switch (check.kind)
            {
            case CheckKind::Prob:
                pass = !(check.prob < LuaRandom100(ctx.L)); // prob < math.random(100) fails
                break;
            case CheckKind::Func:
            {
                const bool truthy = EvalFunc(ctx, check);
                pass = check.expected ? truthy : !truthy;
                break;
            }
            case CheckKind::Infop:
            default:
                // xr_logic.script:419-438: NPC first, then the actor, then "not set"
                if (ctx.hasNpcId && ctx.sim && HasInfoPortion(ctx.sim, ctx.npcId, check.name.c_str()))
                    pass = check.required;
                else if (HasInfoPortion(ctx.sim, 0, check.name.c_str()))
                    pass = check.required;
                else
                    pass = !check.required;
                break;
            }
            if (!pass)
            {
                conditionsMet = false;
                break;
            }
        }
        if (conditionsMet)
        {
            if (!dry)
                ApplySets(ctx, element);
            sectionOut = element.section;
            sectionRefOut = element.sectionRef;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Counters (_gw_internal.logic.stats prints them)
// ---------------------------------------------------------------------------------------------

u64 g_calls = 0;        // entered with the switch on
u64 g_covered = 0;      // native evaluated the call
u64 g_fbStructure = 0;  // unsupported condlist content
u64 g_fbUnknown = 0;    // a condition function outside the native table (failName = the function)
u64 g_fbPattern = 0;    // Lua pattern params of is_npc_match / check_npc_name
u64 g_fbEffect = 0;     // a =func effect in infop_set
u64 g_fbParam = 0;      // params the native side cannot use
u64 g_fbIdentity = 0;   // xr_conditions[name] or has_alife_info / give_info / disable_info replaced or missing
u64 g_fbActions = 0;    // xr_actions[section] is a function
u64 g_fbTypes = 0;      // argument types/zone values/the W4-2.2 state chains the native side cannot use
u64 g_fbPrng = 0;       // math.random is not the stock LuaJIT generator (a ~N check needs it)
u64 g_shadowCompared = 0;
u64 g_shadowMismatch = 0;
// Plan 63 S1 / plan 64 S1: the incremental identity sweep (SweepIdentityBits) - sweeps is how often a
// call needed at least one fresh bit looked up, cached is the calls that needed none (every bit of
// their funcMask was already verified this frame) and were accepted, rejected is the calls the mask
// kept in Lua (a replaced/missing name). stats() prints them; the -npc_cpp_profile counter
// condlist/pick/identity_cached counts the cached calls alongside.
u64 g_identitySweeps = 0;
u64 g_identityCached = 0;
u64 g_identityRejected = 0;
// Plan 60 P-fb: the names behind the named kinds, counted per call (stats() prints the top ones).
std::map<xr_string, u64> g_fbNames;

// One fallback counted in the map and in the -npc_cpp_profile breakdown; returns the "Lua decides" result.
int FallbackResult(lua_State* L, ENpcCppProfileStage stage, u64& counter, const xr_string* name)
{
    ++counter;
    // M12: the name map is a report feature of -npc_cpp_profile - a std::map insert per fallback is
    // not something Release should pay for when no report will ever read it
    if (name && !name->empty() && npc_cpp_profile::enabled())
        ++g_fbNames[*name];
    npc_cpp_profile::count(stage);
    lua_pushboolean(L, 0);
    lua_pushboolean(L, 0);
    return 2;
}

// The structure kinds of an incompatible condlist (plan 60 P-fb): which rule of the compile sent it back.
int FallbackCompiled(lua_State* L, const CompiledCondlist& compiled)
{
    switch (compiled.failKind)
    {
    case CompileFail::UnknownFunc:
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbUnknown, g_fbUnknown, &compiled.failName);
    case CompileFail::Pattern:
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbPattern, g_fbPattern, &compiled.failName);
    case CompileFail::Effect:
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbEffect, g_fbEffect, &compiled.failName);
    case CompileFail::Param:
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbParam, g_fbParam, &compiled.failName);
    case CompileFail::None:
    case CompileFail::Structure:
    default:
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbStructure, g_fbStructure, nullptr);
    }
}

// plans/lua_to_cpp/60-npc-xr-logic-pick-section.md, P2 (measurement only): the stages condlist/pick, /prepare, /eval
// and the counter /fallback of the -npc_cpp_profile report for one native call (the switch on). Without the profiler
// it is one test of a static flag and nothing else. prepare runs from the entry to evaluate() - or to the return of
// a fallback, which is counted then; eval from evaluate() to the return (a nested pick_section of an effect or a
// give_info callback is inside the eval of the outer call, and has its own stages too).
class CondlistPickProfile
{
public:
    CondlistPickProfile() : m_on(npc_cpp_profile::enabled()), m_start(m_on ? CPU::QPC() : 0) {}
    ~CondlistPickProfile()
    {
        if (!m_on)
            return;
        const u64 now = CPU::QPC();
        if (m_evaluated)
            npc_cpp_profile::add(ENpcCppProfileStage::CondlistPickEval, now - m_evaluated);
        else
        {
            npc_cpp_profile::add(ENpcCppProfileStage::CondlistPickPrepare, now - m_start);
            npc_cpp_profile::count(ENpcCppProfileStage::CondlistPickFallback);
        }
        npc_cpp_profile::add(ENpcCppProfileStage::CondlistPick, now - m_start);
    }
    CondlistPickProfile(const CondlistPickProfile&) = delete;
    CondlistPickProfile& operator=(const CondlistPickProfile&) = delete;

    // The checks are done, the native side decides the call
    void evaluate()
    {
        if (!m_on)
            return;
        m_evaluated = CPU::QPC();
        npc_cpp_profile::add(ENpcCppProfileStage::CondlistPickPrepare, m_evaluated - m_start);
    }

private:
    const bool m_on;
    const u64 m_start;
    u64 m_evaluated = 0; // QPC at evaluate(), 0 before it
};

// ---------------------------------------------------------------------------------------------
// Lua entry points
// ---------------------------------------------------------------------------------------------

// _gw_internal.logic.pick_section(actor, npc, condlist) -> result, is_shadow
// result: the picked section (string or nil), or false when Lua must run the call.
int LuaPickSection(lua_State* L)
{
    if (npc_perf_condlist_native == 0)
    {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    // Lazy init of the registry tables: called from Lua (a valid frame), never from the export node
    // (see CGwCondlistScript::script_register). After the first time this is one registry flag read (P3 d).
    CondlistPickProfile profile; // plan 60 P2: the stages of this call (the switch is on)
    EnsureRegistryTables(L);
    ++g_calls;

    const CompiledCondlist* compiled = nullptr;
    {
        StackTopGuard guard(L);
        if (lua_type(L, 3) == LUA_TTABLE)
            compiled = GetCompiled(L, 3);
    }
    if (!compiled || !compiled->compatible)
    {
        if (!compiled)
            return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbStructure, g_fbStructure, nullptr);
        return FallbackCompiled(L, *compiled);
    }

    CallCtx ctx;
    ctx.L = L;
    ctx.sim = ai().get_alife(); // the script resolves alife() once at the top of the call

    // Plan 64 S1 (review2-condlist-lua.md: "вложенные стадии condlist/pick/prepare/* ... чтобы видеть
    // куда уходят ~2 мкс"): everything up to the identity sweep - argument/type marshalling and the
    // W4-2.2/W4-2.3 state chains - is timed as "the rest of prepare" (the same tag is reused further
    // down, after the identity block, for the zone stash + PRNG stock check).
    {
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::CondlistPickPrepareRest);

    // npc slot: game object (online), server object, or a Lua table with an id field (smart/squad)
    const int secondType = lua_type(L, 2);
    if (secondType != LUA_TNIL && secondType != LUA_TNONE)
    {
        if (CScriptGameObject* object = CastGameObject(L, 2))
        {
            ctx.second = object;
            ctx.secondIsGo = true;
            ctx.npcId = object->ID();
            ctx.npcIdLuaValue = object->ID();
        }
        else if (CSE_Abstract* server = CastServerObject(L, 2))
        {
            ctx.secondServer = server;
            ctx.secondIsServer = true;
            ctx.npcId = server->ID;
            ctx.npcIdLuaValue = server->ID;
            // group_id of a server npc: the CSE_ALifeMonsterAbstract property get_object_squad reads
            // in its first branch (_g.script:2116); a non-monster server object has no such field
            if (const auto* monster = smart_cast<const CSE_ALifeMonsterAbstract*>(server))
            {
                ctx.groupIdValue = monster->m_group_id;
                ctx.groupIdIsNumber = true;
            }
        }
        else if (secondType == LUA_TTABLE)
        {
            // npc.id is a plain field here, as in the script
            lua_getfield(L, 2, "id");
            // An id out of the u16 range (or NaN: written so that it fails too) is no object: the Lua path decides
            const lua_Number id = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
            if (id >= 0 && id < 65535.0)
            {
                ctx.npcId = static_cast<u16>(id);
                ctx.npcIdLuaValue = id; // the raw field: Lua keeps it fractional (M9 npc_id)
                lua_pop(L, 1);
            }
            else
            {
                lua_pop(L, 1);
                return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
            }
            // group_id of a table npc (get_object_squad, _g.script:2116): a numeric field takes the first branch
            lua_getfield(L, 2, "group_id");
            if (lua_type(L, -1) == LUA_TNUMBER)
            {
                ctx.groupIdValue = lua_tonumber(L, -1);
                ctx.groupIdIsNumber = true;
            }
            lua_pop(L, 1);
        }
        else
        {
            // numbers/strings have no .id: the script errors indexing them
            return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
        }
        ctx.hasNpcId = ctx.npcId != 0; // 0 is the actor: treated as "no npc id" (script :385-387)
    }
    else if (compiled->secondAnyNeeded || compiled->secondGoNeeded || compiled->secondNameNeeded)
    {
        // is_squad_commander / is_wounded / actor_enemy / sameFaction index the npc slot unguarded:
        // a nil npc errors there, and the call goes to Lua to reproduce the error
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
    }

    // actor slot: actor or db.actor (script :367)
    const int firstType = lua_type(L, 1);
    ctx.firstPresent = firstType != LUA_TNIL && firstType != LUA_TNONE;
    if (ctx.firstPresent)
    {
        ctx.first = CastGameObject(L, 1);
        ctx.firstIsGo = ctx.first != nullptr;
    }
    else if (compiled->firstGoNeeded || compiled->firstOptionalNeeded)
    {
        ResolveDbActor(L, ctx); // nil actor becomes db.actor before any condition runs
        ctx.first = ctx.dbActor;
        ctx.firstIsGo = ctx.dbActorState == DbActorState::GameObject;
        ctx.firstPresent = ctx.dbActorState != DbActorState::Nil;
    }

    // Type pre-pass: everything is decided before any draw or side effect, so a fallback here is
    // invisible to the game state.
    const bool typesOk = (!compiled->firstGoNeeded || ctx.firstIsGo) &&
        (!compiled->firstOptionalNeeded || !ctx.firstPresent || ctx.firstIsGo) &&
        (!compiled->secondGoNeeded || ctx.secondIsGo) &&
        (!compiled->secondNameNeeded || ctx.secondIsGo || ctx.secondIsServer) &&
        !(compiled->setsNeedNpcGo && ctx.hasNpcId && !ctx.secondIsGo);
    if (!typesOk)
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
    if (compiled->dbActorNeeded || compiled->hasSets)
    {
        ResolveDbActor(L, ctx);
        if (ctx.dbActorState == DbActorState::Other)
        {
            // db.actor present but not a game object: Lua errors reading it, let it do exactly that
            return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
        }
    }
    // W4-2.2/W4-2.3 state chains (plan 60 P-fb): a chain that would make the Lua body error goes
    // back to Lua
    for (const FuncId id : compiled->usedFuncs)
    {
        switch (id)
        {
        case FuncId::IsWounded:
            if (!ResolveWounded(L, ctx))
                return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
            break;
        case FuncId::ActorEnemy:
            if (!ResolveDeath(L, ctx) || !ResolveActorFaction(L, ctx))
                return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
            break;
        case FuncId::CheckSquadName:
            if (!ResolveStoryRegistry(L, ctx))
                return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
            break;
        case FuncId::IsEnemyFightingActor:
            if (!ResolveCombatIgnoreTable(L, ctx))
                return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
            break;
        default:
            break;
        }
    }
    // M10: sameFaction answers false on `not sim_brain.initialized` before it touches
    // character_community (xr_conditions.script:14) - the gate runs here so the first slot is not
    // asked for a community it does not have. The second slot is still needed by actor_enemy.
    bool usesActorEnemy = false;
    for (FuncId id : compiled->usedFuncs)
        usesActorEnemy = usesActorEnemy || id == FuncId::ActorEnemy;
    bool needFirst = compiled->usesCommunityFirst;
    bool needSecond = compiled->usesCommunitySecond;
    if (needFirst || needSecond)
    {
        for (FuncId id : compiled->usedFuncs)
            if (id == FuncId::SameFaction)
            {
                ctx.brainInitialized = SimBrainFieldTruthy(L, "initialized");
                if (!ctx.brainInitialized)
                    needFirst = false;
                needSecond = needSecond && (usesActorEnemy || ctx.brainInitialized);
            }
        if ((needFirst || needSecond) && !ResolveCommunities(ctx, needFirst, needSecond))
            return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
    }
    } // CondlistPickPrepareRest (1st half)

    // Plan 64 S1 (review2-condlist-lua.md SF-1): the identity sweep is incremental per frame - a call
    // only pays for the funcMask bits no earlier call this frame has already looked up
    // (g_identityCheckedMask accumulates them; g_okMask accumulates which of them were good). Most
    // frames need only the handful of distinct functions the condlists actually called that frame
    // touch, not a sweep of every one of the <= 35 remembered names.
    const u32 frame = Device.dwFrame;
    {
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::CondlistPickPrepareIdentity);
    if (g_identityFrame != frame)
    {
        g_identityFrame = frame;
        g_identityCheckedMask = 0;
        g_okMask = 0;
    }
    const u64 missing = compiled->funcMask & ~g_identityCheckedMask;
    if (missing)
    {
        SweepIdentityBits(L, missing);
        g_identityCheckedMask |= missing;
        ++g_identitySweeps;
    }
    if ((compiled->funcMask & ~g_okMask) != 0)
    {
        ++g_identityRejected;
        // Plan 65 item 1 follow-up: name the first bit the mask kept in Lua (lowest set bit of the
        // still-missing ones), same mechanism FallbackCompiled already uses for fb_unknown's failName -
        // so stats()'s fb_identity breakdown can show which function/global triggered the fallback.
        const u64 missingBits = compiled->funcMask & ~g_okMask;
        const u64 firstMissingBit = missingBits & (~missingBits + 1);
        // review3: the name only feeds g_fbNames, which FallbackResult fills under -npc_cpp_profile alone (M12) -
        // without it neither the bit lookup nor the xr_string (a heap copy for names past the SSO) is paid
        const pcstr rejectedName = npc_cpp_profile::enabled() ? NameOfIdentityBit(firstMissingBit) : nullptr;
        const xr_string rejectedNameStr = rejectedName ? rejectedName : xr_string();
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbIdentity, g_fbIdentity,
            rejectedName ? &rejectedNameStr : nullptr);
    }
    if (!missing)
    {
        // review2 mellow #3: counted only for calls the mask actually accepted (moved past the
        // rejection above), not every call that happened to need no fresh lookup this frame
        ++g_identityCached;
        npc_cpp_profile::count(ENpcCppProfileStage::CondlistPickIdentityCached);
    }
    } // CondlistPickPrepareIdentity

    {
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::CondlistPickPrepareRest);
    if (!StashZones(L, *compiled, ctx))
        return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbTypes, g_fbTypes, nullptr);
    } // CondlistPickPrepareRest (2nd half)

    const bool shadow = npc_perf_condlist_native == 2;
    PrngGuard prng;
    if (compiled->usesRandom)
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::CondlistPickPrepareRest);
        // Plan 64 S1: every mode now snapshots the generator (not only shadow) - the action-section
        // check below runs AFTER evaluation, for the matched section only, and a condlist that turns
        // out to need the Lua action must restore the PRNG so Lua's full re-run draws the same ~N
        // numbers (same reasoning shadow already used, just no longer shadow-only). LuaRandom100
        // itself calls math.random without protection (a replaced generator may be a Lua function that
        // errors or has effects: Lua calls it), so the stock check still gates both modes the same way.
        SavePrng(L, prng);
        if (!prng.savedOk)
        {
            // math.random is not the stock LuaJIT generator: the native side cannot draw (or replay) for it
            return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbPrng, g_fbPrng, nullptr);
        }
    }

    profile.evaluate(); // plan 60 P2: from here on the call is decided natively

    xr_string section;
    int sectionRef = LUA_NOREF;
    const bool picked = EvaluateCompiled(ctx, *compiled, shadow, section, sectionRef);

    if (picked)
    {
        // Plan 64 S1 (review2-condlist-lua.md SF-1, "как Lua"): xr_actions[section] is checked only
        // for the section this call actually matched, after evaluation - not every section the
        // condlist could reach, before it (the old conservative pre-pass, CheckActionsSections).
        bool actionIsFunction;
        {
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::CondlistPickEvalActions);
            actionIsFunction = SectionHasActionFunction(L, sectionRef, frame);
        }
        if (actionIsFunction)
        {
            // xr_logic.script:500-505 would CALL xr_actions[section] directly rather than fall back -
            // the native side cannot run a Lua action, so the whole call goes back to Lua instead, as
            // if nothing had been decided yet. Any ~N draws already made while picking this element
            // must be undone first, or Lua's full re-run (which redraws them from the top) would
            // desync the random stream for everything after this call in the frame.
            if (compiled->usesRandom)
                RestorePrng(prng);
            // profile.evaluate() already ran: CondlistPickProfile's destructor now counts this call's
            // time under /eval, not /prepare, and would not count it in the /fallback aggregate on its
            // own (that branch only fires while m_evaluated is still 0) - count it here instead so the
            // aggregate still matches "every call that went back to Lua"
            npc_cpp_profile::count(ENpcCppProfileStage::CondlistPickFallback);
            return FallbackResult(L, ENpcCppProfileStage::CondlistPickFbActions, g_fbActions, nullptr);
        }
    }

    if (shadow)
        RestorePrng(prng); // the authoritative Lua pass must draw the same ~N numbers

    ++g_covered;
    if (picked)
    {
        // P3 c: the section string is already interned in the registry (no lua_pushlstring here).
        // "never" is returned as the section itself: the Lua line `cond.section == "never" and nil
        // or cond.section` (xr_logic.script:501) is a `and nil or` trap and yields "never", and the
        // native side mirrors the script as it runs (the stand p60 pins this quirk down)
        if (sectionRef != LUA_NOREF)
            lua_rawgeti(L, LUA_REGISTRYINDEX, sectionRef);
        else
            lua_pushlstring(L, section.c_str(), section.size()); // byte-exact, trailing spaces included
    }
    else
        lua_pushnil(L); // no match returns nil (script :508)
    lua_pushboolean(L, shadow ? 1 : 0);
    return 2;
}

// _gw_internal.logic.shadow_result(nativeDryResult, luaResult): shadow bookkeeping, called by the seam.
int LuaShadowResult(lua_State* L)
{
    ++g_shadowCompared;
    const int nativeType = lua_type(L, 1);
    const int luaType = lua_type(L, 2);
    bool same;
    if (nativeType != luaType)
        same = false;
    else if (nativeType == LUA_TNIL)
        same = true;
    else if (nativeType == LUA_TSTRING)
        same = std::strcmp(lua_tostring(L, 1), lua_tostring(L, 2)) == 0;
    else
        same = lua_rawequal(L, 1, 2) != 0;
    if (!same)
    {
        ++g_shadowMismatch;
        // first mismatches are logged in full; later ones only grow the counter (see _gw_internal.logic.stats)
        if (g_shadowMismatch <= 20)
            Msg("! [gw_condlist] shadow mismatch #%llu: native '%s' vs lua '%s'",
                static_cast<unsigned long long>(g_shadowMismatch),
                nativeType == LUA_TSTRING ? lua_tostring(L, 1) : (nativeType == LUA_TNIL ? "(nil)" : "?"),
                luaType == LUA_TSTRING ? lua_tostring(L, 2) : (luaType == LUA_TNIL ? "(nil)" : "?"));
    }
    return 0;
}

// _gw_internal.logic.stats(): counters of the native interpreter (npc_perf_condlist_native).
// Plan 60 P-fb: the fallback map - the split by reason plus the condition/effect names of the named
// kinds (unknown conditions, Lua patterns, effects in infop_set, bad params), counted per call.
int LuaStats(lua_State*)
{
    Msg("- [gw_condlist] npc_perf_condlist_native=%d calls=%llu covered=%llu compiled=%llu", npc_perf_condlist_native,
        static_cast<unsigned long long>(g_calls), static_cast<unsigned long long>(g_covered),
        static_cast<unsigned long long>(g_compiled));
    Msg("-   fallbacks: structure=%llu unknown_func=%llu pattern=%llu effect=%llu param=%llu identity=%llu actions=%llu types=%llu prng=%llu",
        static_cast<unsigned long long>(g_fbStructure), static_cast<unsigned long long>(g_fbUnknown),
        static_cast<unsigned long long>(g_fbPattern), static_cast<unsigned long long>(g_fbEffect),
        static_cast<unsigned long long>(g_fbParam), static_cast<unsigned long long>(g_fbIdentity),
        static_cast<unsigned long long>(g_fbActions), static_cast<unsigned long long>(g_fbTypes),
        static_cast<unsigned long long>(g_fbPrng));
    if (!g_fbNames.empty())
    {
        // the names by descending per-call count, at most 16 lines (the full map is in the debugger)
        xr_vector<std::pair<xr_string, u64>> top(g_fbNames.begin(), g_fbNames.end());
        std::sort(top.begin(), top.end(),
            [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        const size_t shown = std::min<size_t>(top.size(), 16);
        for (size_t i = 0; i < shown; ++i)
            Msg("-   fallback name: %s %llu", top[i].first.c_str(), static_cast<unsigned long long>(top[i].second));
    }
    Msg("-   identity (plan 63 S1): sweeps=%llu cached=%llu rejected=%llu",
        static_cast<unsigned long long>(g_identitySweeps), static_cast<unsigned long long>(g_identityCached),
        static_cast<unsigned long long>(g_identityRejected));
    Msg("-   shadow: compared=%llu mismatch=%llu", static_cast<unsigned long long>(g_shadowCompared),
        static_cast<unsigned long long>(g_shadowMismatch));
    return 0;
}
} // anonymous namespace

// Registered through the script export list: called for every new Lua state, before any script is
// loaded (the same mechanism that creates the global `plugins` table).
struct CGwCondlistScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CGwCondlistScript::script_register(lua_State* L)
{
    // Exactly the shape of CPluginExportsScript::script_register (the `plugins` table): newtable +
    // pushcfunction + setfield + setglobal, nothing before the pushes. The export nodes run from C++
    // with no Lua frame on the stack, and this LuaJIT build reads the frame slot below L->base
    // inside lua_pushcclosure (curr_func of lj_obj.h); registry/GC/string work before the pushes can
    // hit uninitialized memory there (access violation at 0xFFFF...FF right at engine start). The
    // registry tables are created lazily by LuaPickSection instead, which always runs inside a
    // valid Lua frame.
    lua_newtable(L);
    lua_pushcfunction(L, &LuaPickSection);
    lua_setfield(L, -2, "pick_section");
    lua_pushcfunction(L, &LuaShadowResult);
    lua_setfield(L, -2, "shadow_result");
    lua_pushcfunction(L, &LuaStats);
    lua_setfield(L, -2, "stats");
    // Internal to xr_logic.script, not an API for addons: _gw_internal.logic. The global table _gw_internal is
    // shared with addon_event_bus.cpp and addon_storage.cpp: the first one creates it. Only after the pushes above.
    lua_getglobal(L, "_gw_internal");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_gw_internal");
    }
    lua_insert(L, -2);
    lua_setfield(L, -2, "logic");
    lua_pop(L, 1);
}
} // namespace gw::condlist
