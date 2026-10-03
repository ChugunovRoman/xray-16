#include "StdAfx.h"

// Stage D, W4-2 (plans/lua_to_cpp/11-w4-condlist-native-spec.md): native pick_section_from_condlist.
//
// The interpreter lives in the engine on purpose (spec 0.1(a)): it reads the parsed condlist table straight
// from the Lua stack, so no source string and no plugin marshalling is needed. xr_logic.script calls
// _gw_internal.logic.pick_section(actor, npc, condlist); the result is
//   - the picked section (string, or nil for "never"/no match) when the native side handled the call,
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
// Checks ~N, +info/-info and the whole infop_set of +/- infoportions are native; every =func effect
// and every xr_actions[section] function stays in Lua. The native ones run only while has_alife_info,
// give_info, disable_info (_g.script) and math.random are the functions they mirror (CheckFuncIdentities,
// PrngStateOfMathRandom): a replaced one keeps the call in Lua.
//
// Switch npc_perf_condlist_native: 0 = pure Lua (default), 1 = native decides, 2 = shadow (native runs
// a dry pass first - no infop_set, no effects - saving and restoring the LuaJIT math.random state, then
// the Lua pass decides and _gw_internal.logic.shadow_result counts the mismatches).
//
// Reentrancy: effects and give_info_portion can run script callbacks that call pick_section again
// (spec 4.2). The interpreter keeps the Lua stack empty while evaluating, so nested calls on the same
// lua_State are safe; no static buffers are used.

#include "ai_space.h"
#include "alife_registry_container.h"
#include "alife_simulator.h"
#include "performance_cvars.h"
#include "script_game_object.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrScriptEngine/script_space.hpp"
#include "xrServer_Object_Base.h"
#include "xrServerEntities/InfoPortionDefs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    Count
};

struct FuncDesc
{
    pcstr name;
    FuncId id;
    bool needsFirstGo;    // the function calls a game_object method on pick's first argument
    bool needsSecondGo;   // ... on pick's second argument (npc)
    bool needsSecondName; // npc:name() - game object or server object
    bool needsDbActor;    // reads db.actor itself
    bool zones;           // params are zone names looked up in db.zone_by_name
};

// Argument flags mirror the script bodies (xr_conditions.script):
// fighting_dist_ge(enemy, npc, p) uses both objects, has_actor_enemy reads db.actor, npc_talking uses
// the npc only, actor_in_zone checks db.actor against db.zone_by_name, and so on.
constexpr FuncDesc kFuncs[] = {
    { "fighting_dist_ge", FuncId::FightingDistGe, true, true, false, false, false },
    { "fighting_dist_le", FuncId::FightingDistLe, true, true, false, false, false },
    { "dist_to_actor_le", FuncId::DistToActorLe, true, true, false, false, false },
    { "dist_to_actor_ge", FuncId::DistToActorGe, true, true, false, false, false },
    { "has_enemy", FuncId::HasEnemy, false, true, false, false, false },
    { "has_actor_enemy", FuncId::HasActorEnemy, false, true, false, true, false },
    { "talking", FuncId::Talking, true, false, false, false, false },
    { "npc_talking", FuncId::NpcTalking, false, true, false, false, false },
    { "actor_friend", FuncId::ActorFriend, true, true, false, false, false },
    { "is_npc_match", FuncId::IsNpcMatch, false, false, true, false, false },
    { "check_npc_name", FuncId::CheckNpcName, false, false, true, false, false },
    { "actor_in_zone", FuncId::ActorInZone, false, false, false, true, true },
    { "npc_in_zone", FuncId::NpcInZone, false, false, false, false, true },
};

constexpr size_t kFuncCount = std::size(kFuncs);

const FuncDesc* FindFunc(pcstr name)
{
    for (size_t i = 0; i < kFuncCount; ++i)
        if (std::strcmp(kFuncs[i].name, name) == 0)
            return &kFuncs[i];
    return nullptr;
}

const FuncDesc& FuncDescOf(FuncId id) { return kFuncs[static_cast<size_t>(id)]; }

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
    xr_vector<CompiledCheck> checks;
    xr_vector<CompiledSet> sets;
};

struct CompiledCondlist
{
    xr_vector<CompiledElement> elements;
    xr_vector<xr_string> zoneNames;    // all zone lookups, flattened in evaluation order
    xr_vector<FuncId> usedFuncs;       // distinct conditions, for the identity pre-pass
    xr_vector<xr_string> usedSections; // distinct sections, for the xr_actions pre-pass
    bool compatible = false; // set at the end of a full compile; false keeps the call in Lua forever
    bool firstGoNeeded = false;
    bool secondGoNeeded = false;
    bool secondNameNeeded = false;
    bool dbActorNeeded = false;
    bool hasSets = false;      // some element carries %+name%/%-name%: db.actor gets resolved for it
    bool setsNeedNpcGo = false; // some %+npcx_...% would call npc:give/disable_info_portion
    bool usesRandom = false;    // some ~N check
    bool usesInfop = false;     // some +name/-name check: Lua reads it through has_alife_info
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
    lua_getglobal(L, "math");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return nullptr;
    }
    lua_getfield(L, -1, "random");
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
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "random");
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
    bool secondIsGo = false;
    bool secondIsServer = false;
    bool hasNpcId = false; // npc ~= actor and has a server id (xr_logic.script:381-387)
    u16 npcId = 0;
    const CALifeSimulator* sim = nullptr; // alife() of this call, resolved once like the script does
    bool dbActorResolved = false;
    DbActorState dbActorState = DbActorState::Nil;
    CScriptGameObject* dbActor = nullptr;
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
    lua_getglobal(L, "db");
    if (!lua_istable(L, -1))
    {
        ctx.dbActorState = DbActorState::Other;
        return;
    }
    lua_getfield(L, -1, "actor");
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
// pattern returns false: the call stays in Lua and behaves exactly as today.
bool ValidateFuncParams(FuncId id, const xr_vector<CompiledParam>& params)
{
    switch (id)
    {
    case FuncId::FightingDistGe:
    case FuncId::FightingDistLe:
    case FuncId::DistToActorLe:
    case FuncId::DistToActorGe:
        // p[1] must be a number: the script computes p[1]^2 / tonumber(d)^2 right away
        return params.size() >= 1 && params[0].kind == ParamKind::Number;
    case FuncId::IsNpcMatch:
        // p[1] is a Lua pattern used with string.match; only plain substrings go native
        return params.size() >= 1 && params[0].kind == ParamKind::String &&
            IsPlainSubstring(params[0].text.c_str());
    case FuncId::CheckNpcName:
        // every parameter is a pattern for string.find
        if (params.empty())
            return false;
        for (const auto& param : params)
            if (param.kind != ParamKind::String || !IsPlainSubstring(param.text.c_str()))
                return false;
        return true;
    case FuncId::ActorInZone:
    case FuncId::NpcInZone:
        // zone names index db.zone_by_name; #p is read, so params must exist
        if (params.empty())
            return false;
        for (const auto& param : params)
            if (param.kind != ParamKind::String)
                return false;
        return true;
    case FuncId::HasEnemy:
    case FuncId::HasActorEnemy:
    case FuncId::Talking:
    case FuncId::NpcTalking:
    case FuncId::ActorFriend:
        return true; // the script never touches p
    default:
        return false;
    }
}

void CompileCondlist(lua_State* L, int tableIndex, CompiledCondlist& out)
{
    StackTopGuard guard(L);
    out.compatible = false;

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
                        return; // unknown or not-yet-native condition: Lua evaluates it
                    check.kind = CheckKind::Func;
                    check.func = desc->id;
                    RawGetField(L, infopIndex, "expected");
                    check.expected = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);

                    RawGetField(L, infopIndex, "params");
                    if (lua_type(L, -1) == LUA_TNIL)
                    {
                        lua_pop(L, 1);
                    }
                    else
                    {
                        if (!lua_istable(L, -1))
                            return;
                        const int paramsIndex = lua_gettop(L);
                        if (!CompileParams(L, paramsIndex, check.params))
                            return;
                        lua_pop(L, 1);
                    }
                    if (!ValidateFuncParams(desc->id, check.params))
                        return;

                    out.firstGoNeeded |= desc->needsFirstGo;
                    out.secondGoNeeded |= desc->needsSecondGo;
                    out.secondNameNeeded |= desc->needsSecondName;
                    out.dbActorNeeded |= desc->needsDbActor;
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
                return; // =func effects always stay in Lua (spec 4.1)
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

    out.compatible = true;
}

// ---------------------------------------------------------------------------------------------
// Registry caches (per Lua state): compiled condlists keyed weakly by the condlist table itself, and
// the remembered xr_conditions[name] functions for the identity pre-pass (spec 4.1)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kRegCompiled = "gw_condlist.compiled";
constexpr pcstr kRegFuncRefs = "gw_condlist.funcs";
constexpr pcstr kRegCompiledMt = "gw_condlist.compiled_mt";

int FreeCompiled(lua_State* L)
{
    const auto* holder = static_cast<CompiledCondlist**>(lua_touserdata(L, 1));
    if (holder && *holder)
    {
        CompiledCondlist* compiled = *holder;
        xr_delete(compiled);
    }
    return 0;
}

// Creates the registry tables when they are missing. Called lazily from LuaPickSection (always
// inside a valid Lua frame — NOT from script_register: the export nodes run frameless and this
// LuaJIT build reads the frame slot below L->base in lua_pushcclosure, see CGwCondlistScript).
// Idempotent, leaves the stack balanced.
void EnsureRegistryTables(lua_State* L)
{
    StackTopGuard guard(L);
    // Each block: the "no table" branch pops the nil itself and lua_setfield eats the new table, so only the
    // "table exists" branch has a value left to pop (a pop after both would go below the arguments of the caller)
    lua_getfield(L, LUA_REGISTRYINDEX, kRegCompiled);
    if (lua_type(L, -1) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L); // weak-keyed cache: entries die together with the condlist table
        lua_newtable(L);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_setfield(L, LUA_REGISTRYINDEX, kRegCompiled);
    }
    else
        lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, kRegCompiledMt);
    if (lua_type(L, -1) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushcfunction(L, &FreeCompiled);
        lua_setfield(L, -2, "__gc");
        lua_setfield(L, LUA_REGISTRYINDEX, kRegCompiledMt);
    }
    else
        lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, kRegFuncRefs);
    if (lua_type(L, -1) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, kRegFuncRefs);
    }
    else
        lua_pop(L, 1);
    VERIFY(lua_gettop(L) == guard.top);
}

CompiledCondlist* GetCompiled(lua_State* L, int tableIndex)
{
    StackTopGuard guard(L);

    // The registry tables are created by script_register for every Lua state. If they are missing
    // anyway (a state that skipped the export), compile without caching - slower, but correct.
    lua_getfield(L, LUA_REGISTRYINDEX, kRegCompiled);
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

    if (canCache)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, kRegCompiled);
        lua_pushvalue(L, tableIndex);
        auto* holder = static_cast<CompiledCondlist**>(lua_newuserdata(L, sizeof(CompiledCondlist*)));
        *holder = compiled;
        lua_getfield(L, LUA_REGISTRYINDEX, kRegCompiledMt);
        lua_setmetatable(L, -2);
        lua_rawset(L, -3);
    }
    return compiled;
}

// The function on top of the stack against the one remembered under refKey in the refs table: the first sight
// remembers it and accepts it (spec 4.1). Pops the function.
bool SameAsRemembered(lua_State* L, int refsIndex, pcstr refKey)
{
    lua_getfield(L, refsIndex, refKey);
    if (lua_type(L, -1) == LUA_TNIL)
    {
        lua_pop(L, 1);
        lua_pushstring(L, refKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, refsIndex);
        lua_pop(L, 1);
        return true;
    }
    const bool same = lua_rawequal(L, -2, -1) != 0;
    lua_pop(L, 2);
    return same;
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
};

constexpr InfoGlobal kInfoGlobals[] = {
    { "has_alife_info", "_G.has_alife_info", true },
    { "give_info", "_G.give_info", false },
    { "disable_info", "_G.disable_info", false },
};

// The native path runs only while xr_conditions[name] is the very function remembered at its first
// use (lua_rawequal, the same lookup the script itself does). A replaced or missing function means
// Lua: missing prints "not defined in xr_conditions.script" there and counts as a failed condition.
// The same holds for the globals of kInfoGlobals the condlist needs (any infop_set needs all three).
bool CheckFuncIdentities(lua_State* L, const CompiledCondlist& compiled)
{
    const bool needsInfoGlobals = compiled.usesInfop || compiled.hasSets;
    if (compiled.usedFuncs.empty() && !needsInfoGlobals)
        return true;
    StackTopGuard guard(L);
    lua_getfield(L, LUA_REGISTRYINDEX, kRegFuncRefs);
    const int refsIndex = lua_gettop(L);

    if (!compiled.usedFuncs.empty())
    {
        lua_getglobal(L, "xr_conditions");
        if (!lua_istable(L, -1))
            return false; // module not loaded: the script would fail its own lookup the same way
        const int moduleIndex = lua_gettop(L);
        for (const FuncId id : compiled.usedFuncs)
        {
            const pcstr name = FuncDescOf(id).name;
            lua_getfield(L, moduleIndex, name); // honors __index = _G like the script lookup
            if (lua_type(L, -1) != LUA_TFUNCTION)
                return false; // nil or not a function: keep the call in Lua
            if (!SameAsRemembered(L, refsIndex, name))
                return false;
        }
        lua_pop(L, 1); // xr_conditions
    }

    for (const InfoGlobal& global : kInfoGlobals)
    {
        if (!(compiled.hasSets || (global.forChecks && compiled.usesInfop)))
            continue;
        lua_getglobal(L, global.name);
        if (lua_type(L, -1) != LUA_TFUNCTION)
            return false; // nil: the script would fail calling it, let it do exactly that
        if (!SameAsRemembered(L, refsIndex, global.refKey))
            return false;
    }
    return true;
}

// xr_actions[section] must not be a function for any section of the condlist (xr_logic.script:484);
// such a condlist goes back to Lua, which calls the action itself. Checking all sections (not only
// the matched one) is the conservative direction: it only adds fallbacks, never wrong results.
bool CheckActionsSections(lua_State* L, const CompiledCondlist& compiled)
{
    if (compiled.usedSections.empty())
        return true;
    StackTopGuard guard(L);
    lua_getglobal(L, "xr_actions");
    if (!lua_istable(L, -1))
        return false; // nil module: the script would error indexing it
    const int moduleIndex = lua_gettop(L);
    for (const xr_string& section : compiled.usedSections)
    {
        lua_getfield(L, moduleIndex, section.c_str());
        const bool isFunction = lua_type(L, -1) == LUA_TFUNCTION;
        lua_pop(L, 1);
        if (isFunction)
            return false;
    }
    return true;
}

// Stashes db.zone_by_name values before evaluation. false means a value is present but is not a
// game object: Lua would fail its zone:inside() call, so the whole call goes back to Lua.
bool StashZones(lua_State* L, const CompiledCondlist& compiled, CallCtx& ctx)
{
    if (compiled.zoneNames.empty())
        return true;
    StackTopGuard guard(L);
    lua_getglobal(L, "db");
    if (!lua_istable(L, -1))
        return false;
    lua_getfield(L, -1, "zone_by_name");
    if (!lua_istable(L, -1))
        return false; // the script would error indexing db.zone_by_name
    const int zonesIndex = lua_gettop(L);
    ctx.zones.assign(compiled.zoneNames.size(), nullptr);
    for (size_t i = 0; i < compiled.zoneNames.size(); ++i)
    {
        lua_getfield(L, zonesIndex, compiled.zoneNames[i].c_str());
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

// Returns true when an element matched and fills sectionOut with its (byte-exact) section.
bool EvaluateCompiled(const CallCtx& ctx, const CompiledCondlist& compiled, bool dry, xr_string& sectionOut)
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
u64 g_fbIdentity = 0;   // xr_conditions[name] or has_alife_info / give_info / disable_info replaced or missing
u64 g_fbActions = 0;    // xr_actions[section] is a function
u64 g_fbTypes = 0;      // argument types/zone values the native side cannot use
u64 g_fbPrng = 0;       // math.random is not the stock LuaJIT generator (a ~N check needs it)
u64 g_shadowCompared = 0;
u64 g_shadowMismatch = 0;

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
    // (see CGwCondlistScript::script_register). Idempotent: three registry lookups, no work when the
    // tables exist; negligible next to the interpreted pick_section call itself.
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
        ++g_fbStructure;
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }

    CallCtx ctx;
    ctx.L = L;

    // npc slot: game object (online), server object, or a Lua table with an id field (smart/squad)
    const int secondType = lua_type(L, 2);
    if (secondType != LUA_TNIL && secondType != LUA_TNONE)
    {
        if (CScriptGameObject* object = CastGameObject(L, 2))
        {
            ctx.second = object;
            ctx.secondIsGo = true;
            ctx.npcId = object->ID();
        }
        else if (CSE_Abstract* server = CastServerObject(L, 2))
        {
            ctx.secondServer = server;
            ctx.secondIsServer = true;
            ctx.npcId = server->ID;
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
                lua_pop(L, 1);
            }
            else
            {
                lua_pop(L, 1);
                ++g_fbTypes;
                lua_pushboolean(L, 0);
                lua_pushboolean(L, 0);
                return 2;
            }
        }
        else
        {
            // numbers/strings have no .id: the script errors indexing them
            ++g_fbTypes;
            lua_pushboolean(L, 0);
            lua_pushboolean(L, 0);
            return 2;
        }
        ctx.hasNpcId = ctx.npcId != 0; // 0 is the actor: treated as "no npc id" (script :385-387)
    }

    // actor slot: actor or db.actor (script :367)
    const int firstType = lua_type(L, 1);
    if (firstType != LUA_TNIL && firstType != LUA_TNONE)
    {
        ctx.first = CastGameObject(L, 1);
        ctx.firstIsGo = ctx.first != nullptr;
    }
    else if (compiled->firstGoNeeded)
    {
        ResolveDbActor(L, ctx); // nil actor becomes db.actor before any condition runs
        ctx.first = ctx.dbActor;
        ctx.firstIsGo = ctx.dbActorState == DbActorState::GameObject;
    }

    // Type pre-pass: everything is decided before any draw or side effect, so a fallback here is
    // invisible to the game state.
    const bool typesOk = (!compiled->firstGoNeeded || ctx.firstIsGo) &&
        (!compiled->secondGoNeeded || ctx.secondIsGo) &&
        (!compiled->secondNameNeeded || ctx.secondIsGo || ctx.secondIsServer) &&
        !(compiled->setsNeedNpcGo && ctx.hasNpcId && !ctx.secondIsGo);
    if (!typesOk)
    {
        ++g_fbTypes;
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    if (compiled->dbActorNeeded || compiled->hasSets)
    {
        ResolveDbActor(L, ctx);
        if (ctx.dbActorState == DbActorState::Other)
        {
            // db.actor present but not a game object: Lua errors reading it, let it do exactly that
            ++g_fbTypes;
            lua_pushboolean(L, 0);
            lua_pushboolean(L, 0);
            return 2;
        }
    }

    if (!CheckFuncIdentities(L, *compiled))
    {
        ++g_fbIdentity;
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    if (!CheckActionsSections(L, *compiled))
    {
        ++g_fbActions;
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    if (!StashZones(L, *compiled, ctx))
    {
        ++g_fbTypes;
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }

    const bool shadow = npc_perf_condlist_native == 2;
    PrngGuard prng;
    if (compiled->usesRandom)
    {
        // Both modes need the stock generator: shadow snapshots its state, and LuaRandom100 calls it without
        // protection (a replaced math.random may be a Lua function that errors or has effects: Lua calls it)
        bool stock;
        if (shadow)
        {
            SavePrng(L, prng);
            stock = prng.savedOk;
        }
        else
            stock = PrngStateOfMathRandom(L) != nullptr;
        if (!stock)
        {
            // math.random is not the stock LuaJIT generator: the native side cannot draw (or replay) for it
            ++g_fbPrng;
            lua_pushboolean(L, 0);
            lua_pushboolean(L, 0);
            return 2;
        }
    }

    ctx.sim = ai().get_alife(); // the script resolves alife() once at the top of the call

    xr_string section;
    const bool picked = EvaluateCompiled(ctx, *compiled, shadow, section);

    if (shadow)
        RestorePrng(prng); // the authoritative Lua pass must draw the same ~N numbers

    ++g_covered;
    if (picked && section != "never")
        lua_pushlstring(L, section.c_str(), section.size()); // byte-exact, trailing spaces included
    else
        lua_pushnil(L); // "never" and "no match" both return nil (script :489, :495)
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
int LuaStats(lua_State*)
{
    Msg("- [gw_condlist] npc_perf_condlist_native=%d calls=%llu covered=%llu", npc_perf_condlist_native,
        static_cast<unsigned long long>(g_calls), static_cast<unsigned long long>(g_covered));
    Msg("-   fallbacks: structure=%llu identity=%llu actions=%llu types=%llu prng=%llu",
        static_cast<unsigned long long>(g_fbStructure), static_cast<unsigned long long>(g_fbIdentity),
        static_cast<unsigned long long>(g_fbActions), static_cast<unsigned long long>(g_fbTypes),
        static_cast<unsigned long long>(g_fbPrng));
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
