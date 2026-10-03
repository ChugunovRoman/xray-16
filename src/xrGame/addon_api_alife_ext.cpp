#include "StdAfx.h"

#include "addon_api_alife_ext.h"
#include "addon_binders.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "Actor.h"
#include "Actor_Flags.h"
#include "Entity.h"
#include "Level.h"
#include "character_community.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrCore/clsid.h"
#include "xrNetServer/NET_Messages.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrServerEntities/xrServer_Objects.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <cctype>
#include <cstring>

namespace gw::addons
{
namespace
{
// A spawn is refused below these many free ids: the engine stops the game when the id generator runs dry
// (R_ASSERT "Not enough IDs"), and a creature spawns its supplies, a squad its members with theirs.
constexpr u16 kAlifeExtSpawnIdReserve = 64;
constexpr u16 kAlifeExtSquadIdReserve = 256;

constexpr u32 kAlifeExtKindFlags =
    GWP_ALIFE_STALKER | GWP_ALIFE_MONSTER | GWP_ALIFE_ACTOR | GWP_ALIFE_ITEM | GWP_ALIFE_SQUAD | GWP_ALIFE_SMART;
constexpr u32 kAlifeExtStateFlags =
    GWP_ALIFE_ONLINE | GWP_ALIFE_OFFLINE | GWP_ALIFE_ALIVE | GWP_ALIFE_DEAD | GWP_ALIFE_NO_PARENT;

// The Lua side of the squads (gw_plugin_sim.script).
constexpr pcstr kAlifeExtSimModule = "gw_plugin_sim.";

// Dispatches of server_object_on_register / _on_unregister in progress: a spawn or a release from inside one would
// change the registry while the engine may be walking it (the on_register loop of a load).
u32 g_alife_ext_register_depth = 0;

// Lua calls of this file in progress (squad_create -> a Lua callback -> a plugin -> squad_create ...), capped as
// script_call caps its own
constexpr u32 kAlifeExtMaxLuaDepth = 16;
u32 g_alife_ext_lua_depth = 0;

bool AlifeExtCheckCall(pcstr group, pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [%s] %s called outside the main thread, ignored", group, function);
    return false;
}

CALifeSimulator* AlifeExtSimulator()
{
    CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife());
    return alife && alife->initialized() ? alife : nullptr;
}

// The registry of ALife: the non-const objects() is protected, the const one is the public way (the objects are
// held by pointer, so the lookup still gives a mutable object).
CSE_ALifeDynamicObject* AlifeExtObject(const CALifeSimulator* alife, ALife::_OBJECT_ID id, bool no_assert)
{
    return alife ? alife->objects().object(id, no_assert) : nullptr;
}

CSE_ALifeDynamicObject* AlifeExtFind(GwpObjectId id)
{
    CALifeSimulator* alife = id != GWP_INVALID_OBJECT_ID ? AlifeExtSimulator() : nullptr;
    return alife ? AlifeExtObject(alife, id, true) : nullptr; // true: no fatal error for a missing id
}

// The online game object of a server object, when its client side exists already (an object switched online a
// moment ago has none until the network update).
CGameObject* AlifeExtOnline(GwpObjectId id)
{
    if (!g_pGameLevel || id == GWP_INVALID_OBJECT_ID)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object && !object->getDestroy() ? object : nullptr;
}

uint32_t AlifeExtCopy(pcstr text, char* out, uint32_t cap)
{
    const u32 length = text ? xr_strlen(text) : 0;
    if (out && cap)
    {
        const u32 count = length < cap - 1 ? length : cap - 1;
        if (count)
            std::memcpy(out, text, count);
        out[count] = '\0';
    }
    return length;
}

uint32_t AlifeExtNone(char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = '\0';
    return 0;
}

// The class id as text without the padding spaces of CLSID2TEXT ("AI_STL_S", "S_ACTOR").
void AlifeExtClassText(CLASS_ID id, string16& out)
{
    CLSID2TEXT(id, out);
    for (int i = 7; i >= 0 && out[i] == ' '; --i)
        out[i] = '\0';
}

bool AlifeExtSameClassText(pcstr a, pcstr b)
{
    // both without the trailing spaces: "S_ACTOR" == "S_ACTOR "
    u32 la = xr_strlen(a), lb = xr_strlen(b);
    while (la && a[la - 1] == ' ')
        --la;
    while (lb && b[lb - 1] == ' ')
        --lb;
    return la == lb && std::strncmp(a, b, la) == 0;
}

// '*' and '?' wildcards, case-insensitive (the section masks of binder_register work the same way).
bool AlifeExtWildcard(pcstr mask, pcstr text)
{
    pcstr star = nullptr;
    pcstr resume = nullptr;
    while (*text)
    {
        if (*mask == '*')
        {
            star = mask++;
            resume = text;
        }
        else if (*mask == '?' || (*mask && std::tolower(static_cast<u8>(*mask)) == std::tolower(static_cast<u8>(*text))))
        {
            ++mask;
            ++text;
        }
        else if (star)
        {
            mask = star + 1;
            text = ++resume;
        }
        else
            return false;
    }
    while (*mask == '*')
        ++mask;
    return *mask == '\0';
}

int32_t AlifeExtLevelOfGameVertex(u32 game_vertex)
{
    if (!ai().get_game_graph() || !ai().game_graph().valid_vertex_id(game_vertex))
        return -1;
    return static_cast<int32_t>(ai().game_graph().vertex(game_vertex)->level_id());
}

// Fills the vertices of a place: the given ones are checked, the missing ones are taken from the position on the
// current level. The level vertex of another level cannot be checked here (its graph is not loaded).
bool AlifeExtResolvePlace(const Fvector& position, u32& level_vertex, u32& game_vertex)
{
    if (game_vertex != GWP_INVALID_GAME_VERTEX)
    {
        const int32_t level = AlifeExtLevelOfGameVertex(game_vertex);
        if (level < 0)
            return false;
        const bool current = g_pGameLevel && ai().get_level_graph() &&
            static_cast<u32>(level) == ai().level_graph().level_id();
        if (level_vertex == GWP_INVALID_LEVEL_VERTEX)
            level_vertex = ai().game_graph().vertex(game_vertex)->level_vertex_id();
        return !current || ai().level_graph().valid_vertex_id(level_vertex);
    }
    // No game vertex: the place must be on the current level
    if (!g_pGameLevel || !ai().get_level_graph() || !ai().get_cross_table())
        return false;
    if (level_vertex == GWP_INVALID_LEVEL_VERTEX)
        level_vertex = ai().level_graph().vertex_id(position);
    if (!ai().level_graph().valid_vertex_id(level_vertex))
        return false;
    game_vertex = ai().cross_table().vertex(level_vertex).game_vertex_id();
    return ai().game_graph().valid_vertex_id(game_vertex);
}

// ---------------------------------------------------------------------------------------------
// Lua calls: the squads (gw_plugin_sim.script) and the helpers of _g.script the scripts keep their caches with
// ---------------------------------------------------------------------------------------------

struct AlifeExtLuaResult
{
    int type = LUA_TNIL;
    double number = 0.;
    bool boolean = false;
    xr_string text;
};

GwpResult AlifeExtCallLua(const GwpPlugin* self, pcstr api_function, pcstr lua_function, const GwpValue* argv,
    u32 argc, AlifeExtLuaResult* result)
{
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
        return GWP_ERROR_INVALID_STATE;
    // The thread of the plugins.call that led here, else the coroutine of a Lua emit, else the main state (the
    // registry is shared by all of them), as script_call does
    lua_State* L = CallingLuaThread();
    if (!L)
        L = events::ActiveLuaThread();
    if (!L)
        L = GEnv.ScriptEngine->lua();
    if (!lua_checkstack(L, static_cast<int>(argc) + 4))
        return GWP_ERROR;
    luabind::object function;
    // function_object loads the script of the namespace when needed, the same path as the engine's own calls
    if (!GEnv.ScriptEngine->function_object(lua_function, function, LUA_TFUNCTION))
    {
        Msg("! [plugin:%s] %s: no Lua function '%s'", PluginAddonId(self), api_function, lua_function);
        return GWP_ERROR_NOT_SUPPORTED;
    }
    if (g_alife_ext_lua_depth >= kAlifeExtMaxLuaDepth)
    {
        Msg("! [plugin:%s] %s: Lua calls nested too deep, refused", PluginAddonId(self), api_function);
        return GWP_ERROR_INVALID_STATE;
    }
    const int top = lua_gettop(L);
    function.push(L);
    for (u32 i = 0; i < argc; ++i)
        events::PushLuaValue(L, argv[i]);
    ++g_alife_ext_lua_depth;
    const int status = lua_pcall(L, static_cast<int>(argc), 1, 0);
    --g_alife_ext_lua_depth;
    if (status != 0)
    {
        const pcstr message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "error without a message";
        Msg("! [plugin:%s] %s: %s: %s", PluginAddonId(self), api_function, lua_function, message);
        lua_settop(L, top);
        return GWP_ERROR_CRASHED;
    }
    if (result)
    {
        result->type = lua_type(L, -1);
        switch (result->type)
        {
        case LUA_TNUMBER: result->number = lua_tonumber(L, -1); break;
        case LUA_TBOOLEAN: result->boolean = lua_toboolean(L, -1) != 0; break;
        case LUA_TSTRING:
        {
            size_t length = 0;
            const pcstr text = lua_tolstring(L, -1, &length);
            result->text.assign(text ? text : "", text ? length : 0);
            break;
        }
        default: break;
        }
    }
    lua_settop(L, top);
    return GWP_OK;
}

// A Lua number result read as an object id; -1 and anything that is not an id give GWP_INVALID_OBJECT_ID.
GwpObjectId AlifeExtResultId(const AlifeExtLuaResult& result)
{
    if (result.type != LUA_TNUMBER || result.number < 0. || result.number >= static_cast<double>(GWP_INVALID_OBJECT_ID))
        return GWP_INVALID_OBJECT_ID;
    return static_cast<GwpObjectId>(result.number);
}

xr_string AlifeExtSimFunction(pcstr name) { return xr_string(kAlifeExtSimModule) + name; }

GwpValue AlifeExtIdArg(GwpObjectId id)
{
    return events::Number(id == GWP_INVALID_OBJECT_ID ? -1. : static_cast<double>(id));
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group alife): reading and listing
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiAlifePresent()
{
    return AlifeExtCheckCall("alife", "alife_present") && AlifeExtSimulator() ? 1 : 0;
}

uint32_t GWP_CALL ApiAlifeFreeIds()
{
    if (!AlifeExtCheckCall("alife", "alife_free_ids"))
        return 0;
    CALifeSimulator* alife = AlifeExtSimulator();
    return alife ? alife->server().GetFreeIDs() : 0;
}

int GWP_CALL ApiAlifeObjectExists(GwpObjectId id)
{
    return AlifeExtCheckCall("alife", "alife_object_exists") && AlifeExtFind(id) ? 1 : 0;
}

uint32_t GWP_CALL ApiAlifeObjectSection(GwpObjectId id, char* out, uint32_t cap)
{
    const CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_section") ? AlifeExtFind(id) : nullptr;
    return object ? AlifeExtCopy(object->s_name.c_str(), out, cap) : AlifeExtNone(out, cap);
}

uint32_t GWP_CALL ApiAlifeObjectName(GwpObjectId id, char* out, uint32_t cap)
{
    const CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_name") ? AlifeExtFind(id) : nullptr;
    return object ? AlifeExtCopy(object->name_replace(), out, cap) : AlifeExtNone(out, cap);
}

GwpObjectId GWP_CALL ApiAlifeObjectParent(GwpObjectId id)
{
    const CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_parent") ? AlifeExtFind(id) : nullptr;
    return object && object->ID_Parent != ALife::_OBJECT_ID(-1) ? object->ID_Parent : GWP_INVALID_OBJECT_ID;
}

int GWP_CALL ApiAlifeObjectOnline(GwpObjectId id)
{
    const CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_online") ? AlifeExtFind(id) : nullptr;
    return object && object->m_bOnline ? 1 : 0;
}

int GWP_CALL ApiAlifeObjectAlive(GwpObjectId id)
{
    CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_alive") ? AlifeExtFind(id) : nullptr;
    const auto* creature = smart_cast<const CSE_ALifeCreatureAbstract*>(object);
    return creature && creature->g_Alive() ? 1 : 0;
}

uint32_t GWP_CALL ApiAlifeObjectCommunity(GwpObjectId id, char* out, uint32_t cap)
{
    CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_community") ? AlifeExtFind(id) : nullptr;
    const auto* trader = smart_cast<const CSE_ALifeTraderAbstract*>(object);
    // CommunityName() stops the game on NO_COMMUNITY_INDEX (a character whose profile is not chosen yet: a trader just
    // spawned)
    if (!trader || trader->m_community_index == NO_COMMUNITY_INDEX)
        return AlifeExtNone(out, cap);
    return AlifeExtCopy(trader->CommunityName(), out, cap);
}

int32_t GWP_CALL ApiAlifeObjectRank(GwpObjectId id)
{
    CSE_ALifeDynamicObject* object = AlifeExtCheckCall("alife", "alife_object_rank") ? AlifeExtFind(id) : nullptr;
    int rank = NO_RANK;
    // A stalker is both: the rank of the character (se_obj:rank() of Lua resolves to the trader one)
    if (auto* trader = smart_cast<CSE_ALifeTraderAbstract*>(object))
        rank = trader->Rank();
    else if (auto* monster = smart_cast<CSE_ALifeMonsterAbstract*>(object))
        rank = monster->Rank();
    return rank == NO_RANK ? INT32_MIN : static_cast<int32_t>(rank);
}

GwpObjectId GWP_CALL ApiAlifeObjectByName(const char* name)
{
    CALifeSimulator* alife = AlifeExtCheckCall("alife", "alife_object_by_name") && name && *name ? AlifeExtSimulator() : nullptr;
    if (!alife)
        return GWP_INVALID_OBJECT_ID;
    for (const auto& [id, object] : static_cast<const CALifeSimulator*>(alife)->objects().objects())
    {
        if (xr_strcmp(object->name_replace(), name) == 0)
            return id;
    }
    return GWP_INVALID_OBJECT_ID;
}

bool AlifeExtPasses(CSE_ALifeDynamicObject* object, const GwpAlifeFilter& filter)
{
    const u32 kinds = filter.flags & kAlifeExtKindFlags;
    const auto* creature = smart_cast<const CSE_ALifeCreatureAbstract*>(object);
    if (kinds)
    {
        const bool actor = smart_cast<const CSE_ALifeCreatureActor*>(object) != nullptr;
        const bool stalker = smart_cast<const CSE_ALifeHumanAbstract*>(object) != nullptr;
        bool match = false;
        match |= (kinds & GWP_ALIFE_STALKER) && stalker;
        match |= (kinds & GWP_ALIFE_ACTOR) && actor;
        match |= (kinds & GWP_ALIFE_MONSTER) && creature && !stalker && !actor;
        match |= (kinds & GWP_ALIFE_ITEM) && smart_cast<const CSE_ALifeInventoryItem*>(object);
        match |= (kinds & GWP_ALIFE_SQUAD) && smart_cast<const CSE_ALifeOnlineOfflineGroup*>(object);
        match |= (kinds & GWP_ALIFE_SMART) && smart_cast<const CSE_ALifeSmartZone*>(object);
        if (!match)
            return false;
    }
    if ((filter.flags & GWP_ALIFE_ONLINE) && !object->m_bOnline)
        return false;
    if ((filter.flags & GWP_ALIFE_OFFLINE) && object->m_bOnline)
        return false;
    if ((filter.flags & GWP_ALIFE_ALIVE) && !(creature && creature->g_Alive()))
        return false;
    if ((filter.flags & GWP_ALIFE_DEAD) && !(creature && !creature->g_Alive()))
        return false;
    if ((filter.flags & GWP_ALIFE_NO_PARENT) && object->ID_Parent != ALife::_OBJECT_ID(-1))
        return false;
    if (filter.level_id >= 0 && AlifeExtLevelOfGameVertex(object->m_tGraphID) != filter.level_id)
        return false;
    if (filter.class_id && *filter.class_id)
    {
        string16 text;
        CLSID2TEXT(object->m_tClassID, text);
        if (!AlifeExtSameClassText(text, filter.class_id))
            return false;
    }
    if (filter.section_mask && *filter.section_mask && !AlifeExtWildcard(filter.section_mask, object->s_name.c_str()))
        return false;
    return true;
}

uint32_t GWP_CALL ApiAlifeObjects(GwpObjectId* out, uint32_t max, const GwpAlifeFilter* filter)
{
    CALifeSimulator* alife = AlifeExtCheckCall("alife", "alife_objects") ? AlifeExtSimulator() : nullptr;
    if (!alife)
        return 0;
    GwpAlifeFilter local{};
    local.level_id = -1; // the default of a field beyond the size the plugin passed
    if (filter)
    {
        if (filter->size < offsetof(GwpAlifeFilter, flags) + sizeof(filter->flags))
            return 0;
        std::memcpy(&local, filter, filter->size < sizeof(local) ? filter->size : sizeof(local));
        if (filter->size < offsetof(GwpAlifeFilter, level_id) + sizeof(filter->level_id))
            local.level_id = -1;
        static bool logged = false;
        if (((local.flags & ~(kAlifeExtKindFlags | kAlifeExtStateFlags)) || local.reserved) && !logged)
        {
            logged = true;
            Msg("! [alife] alife_objects: unknown filter flags 0x%x or a non-zero reserved field, refused",
                local.flags & ~(kAlifeExtKindFlags | kAlifeExtStateFlags));
        }
        if ((local.flags & ~(kAlifeExtKindFlags | kAlifeExtStateFlags)) || local.reserved)
            return 0;
    }
    uint32_t total = 0;
    for (const auto& [id, object] : static_cast<const CALifeSimulator*>(alife)->objects().objects())
    {
        if (!AlifeExtPasses(object, local))
            continue;
        if (out && total < max)
            out[total] = id;
        ++total;
    }
    return total;
}

GwpObjectId GWP_CALL ApiStoryObjectId(const char* name)
{
    if (!AlifeExtCheckCall("alife", "story_object_id") || !name || !*name || !AlifeExtSimulator())
        return GWP_INVALID_OBJECT_ID;
    const GwpValue args[] = { events::String(name) };
    AlifeExtLuaResult result;
    if (AlifeExtCallLua(nullptr, "story_object_id", "get_story_object_id", args, 1, &result) != GWP_OK)
        return GWP_INVALID_OBJECT_ID;
    return AlifeExtResultId(result);
}

uint32_t GWP_CALL ApiObjectStoryId(GwpObjectId id, char* out, uint32_t cap)
{
    if (!AlifeExtCheckCall("alife", "object_story_id") || !AlifeExtFind(id))
        return AlifeExtNone(out, cap);
    const GwpValue args[] = { events::Number(id) };
    AlifeExtLuaResult result;
    if (AlifeExtCallLua(nullptr, "object_story_id", "get_object_story_id", args, 1, &result) != GWP_OK ||
        result.type != LUA_TSTRING)
        return AlifeExtNone(out, cap);
    return AlifeExtCopy(result.text.c_str(), out, cap);
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group alife): spawn, release, teleport, switch, kill
// ---------------------------------------------------------------------------------------------

// The common gate of the calls that change ALife: the thread, the plugin, a game, not inside a registry event.
GwpResult AlifeExtWriteGate(const GwpPlugin* self, pcstr group, pcstr function, CALifeSimulator*& alife)
{
    alife = nullptr;
    if (!AlifeExtCheckCall(group, function))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self)
        return GWP_ERROR_INVALID_ARGUMENT;
    alife = AlifeExtSimulator();
    if (!alife)
        return GWP_ERROR_INVALID_STATE;
    if (g_alife_ext_register_depth)
    {
        Msg("! [plugin:%s] %s: refused inside server_object_on_register / _on_unregister", PluginAddonId(self),
            function);
        return GWP_ERROR_INVALID_STATE;
    }
    // binder on_save while the game is saved: the ALife save stream is being built around the call
    if (binders::IsSaving())
    {
        Msg("! [plugin:%s] %s: refused inside binder on_save while the game is being saved", PluginAddonId(self),
            function);
        return GWP_ERROR_INVALID_STATE;
    }
    return GWP_OK;
}

// What the section spawns, checked before the engine is asked: its spawn asserts on a section without a server
// class, and CALifeSimulator__spawn_ammo of Lua throws on a box too small. A probe object is created and destroyed.
enum class EAlifeExtKind
{
    Missing,
    Object,
    Item,
    Ammo,
    Squad,
    Smart
};

EAlifeExtKind AlifeExtSectionKind(pcstr section, u16* box_size)
{
    if (!section || !*section || !pSettings->section_exist(section) || !pSettings->line_exist(section, "class"))
        return EAlifeExtKind::Missing;
    CSE_Abstract* probe = F_entity_Create(section, true);
    if (!probe)
        return EAlifeExtKind::Missing;
    EAlifeExtKind kind = EAlifeExtKind::Missing;
    if (auto* ammo = smart_cast<CSE_ALifeItemAmmo*>(probe))
    {
        kind = EAlifeExtKind::Ammo;
        if (box_size)
            *box_size = ammo->m_boxSize;
    }
    else if (smart_cast<CSE_ALifeInventoryItem*>(probe))
        kind = EAlifeExtKind::Item;
    else if (smart_cast<CSE_ALifeOnlineOfflineGroup*>(probe))
        kind = EAlifeExtKind::Squad;
    else if (smart_cast<CSE_ALifeSmartZone*>(probe))
        kind = EAlifeExtKind::Smart;
    else if (smart_cast<CSE_ALifeDynamicObject*>(probe))
        kind = EAlifeExtKind::Object;
    F_entity_Destroy(probe);
    return kind;
}

// CALifeSimulator__spawn_item2 / __spawn_ammo of alife_simulator_script.cpp: an item given to an online owner goes
// through the spawn packet of the server, so the client creates it in that inventory; anything else is spawned
// into ALife directly. ammo < 0: not an ammo box.
CSE_Abstract* AlifeExtSpawn(CALifeSimulator& alife, pcstr section, const Fvector& position, u32 level_vertex,
    u32 game_vertex, GwpObjectId parent, int ammo)
{
    const auto graph_id = static_cast<GameGraph::_GRAPH_ID>(game_vertex);
    const auto parent_id = parent == GWP_INVALID_OBJECT_ID ? ALife::_OBJECT_ID(-1) : ALife::_OBJECT_ID(parent);
    CSE_ALifeDynamicObject* owner = parent_id != ALife::_OBJECT_ID(-1) ? AlifeExtObject(&alife, parent_id, true) : nullptr;
    if (!owner || !owner->m_bOnline)
    {
        CSE_Abstract* item = alife.spawn_item(section, position, level_vertex, graph_id, parent_id);
        if (ammo >= 0)
        {
            if (auto* box = smart_cast<CSE_ALifeItemAmmo*>(item))
                box->a_elapsed = static_cast<u16>(ammo);
        }
        return item;
    }
    NET_Packet packet;
    packet.w_begin(M_SPAWN);
    packet.w_stringZ(section);
    CSE_Abstract* item = alife.spawn_item(section, position, level_vertex, graph_id, parent_id, false);
    if (ammo >= 0)
    {
        if (auto* box = smart_cast<CSE_ALifeItemAmmo*>(item))
            box->a_elapsed = static_cast<u16>(ammo);
    }
    item->Spawn_Write(packet, FALSE);
    alife.server().FreeID(item->ID, 0);
    F_entity_Destroy(item);
    ClientID client;
    client.set(0xffff);
    u16 dummy;
    packet.r_begin(dummy);
    return alife.server().Process_spawn(packet, client);
}

GwpResult AlifeExtCreate(const GwpPlugin* self, pcstr function, const char* section, const float position[3],
    uint32_t level_vertex, uint32_t game_vertex, GwpObjectId parent, int ammo, GwpObjectId* out_id)
{
    if (out_id)
        *out_id = GWP_INVALID_OBJECT_ID;
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "alife", function, alife);
    if (gate != GWP_OK)
        return gate;
    if (!alife->server().HasEnoughIDs(kAlifeExtSpawnIdReserve))
    {
        Msg("! [plugin:%s] %s: '%s' refused, fewer than %u free object ids", PluginAddonId(self), function,
            section ? section : "", u32(kAlifeExtSpawnIdReserve));
        return GWP_ERROR_INVALID_STATE;
    }

    u16 box_size = 0;
    const EAlifeExtKind kind = AlifeExtSectionKind(section, &box_size);
    if (kind == EAlifeExtKind::Missing)
        return GWP_ERROR_NOT_FOUND;
    if (kind == EAlifeExtKind::Squad)
        return GWP_ERROR_INVALID_ARGUMENT; // squad_create: an empty squad without the bookkeeping of sim_board
    if (ammo >= 0 && (kind != EAlifeExtKind::Ammo || ammo < 1 || ammo > box_size))
        return GWP_ERROR_INVALID_ARGUMENT;

    Fvector place{};
    if (parent != GWP_INVALID_OBJECT_ID)
    {
        // An owner with an inventory: a character (the actor, a stalker, a trader) or a box
        CSE_ALifeDynamicObject* owner = AlifeExtObject(alife, parent, true);
        if (!owner || (kind != EAlifeExtKind::Item && kind != EAlifeExtKind::Ammo) ||
            (!smart_cast<CSE_ALifeTraderAbstract*>(owner) && !smart_cast<CSE_ALifeInventoryBox*>(owner)))
            return GWP_ERROR_INVALID_ARGUMENT;
        place = owner->o_Position;
        if (level_vertex == GWP_INVALID_LEVEL_VERTEX)
            level_vertex = owner->m_tNodeID;
        if (game_vertex == GWP_INVALID_GAME_VERTEX)
            game_vertex = owner->m_tGraphID;
    }
    else if (!position)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (position)
        place.set(position[0], position[1], position[2]);
    if (!AlifeExtResolvePlace(place, level_vertex, game_vertex))
        return GWP_ERROR_INVALID_ARGUMENT;

    CSE_Abstract* object = AlifeExtSpawn(*alife, section, place, level_vertex, game_vertex, parent, ammo);
    if (!object)
        return GWP_ERROR;
    if (IsDebugLog())
        Msg("~ [plugin:%s] %s: '%s' id=%u", PluginAddonId(self), function, section, u32(object->ID));
    if (out_id)
        *out_id = object->ID;
    return GWP_OK;
}

GwpResult GWP_CALL ApiAlifeCreate(const GwpPlugin* self, const char* section, const float position[3],
    uint32_t level_vertex, uint32_t game_vertex, GwpObjectId parent, GwpObjectId* out_id)
{
    return AlifeExtCreate(self, "alife_create", section, position, level_vertex, game_vertex, parent, -1, out_id);
}

GwpResult GWP_CALL ApiAlifeCreateAmmo(const GwpPlugin* self, const char* section, const float position[3],
    uint32_t level_vertex, uint32_t game_vertex, GwpObjectId parent, uint32_t count, GwpObjectId* out_id)
{
    const int ammo = count > 0xFFFFu ? 0x10000 : static_cast<int>(count); // above any box: refused by the check
    return AlifeExtCreate(self, "alife_create_ammo", section, position, level_vertex, game_vertex, parent,
        ammo, out_id);
}

GwpResult GWP_CALL ApiAlifeRelease(const GwpPlugin* self, GwpObjectId id)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "alife", "alife_release", alife);
    if (gate != GWP_OK)
        return gate;
    CSE_ALifeDynamicObject* object = AlifeExtObject(alife, id, true);
    if (!object)
        return GWP_ERROR_NOT_FOUND;
    if (smart_cast<CSE_ALifeCreatureActor*>(object) || smart_cast<CSE_ALifeOnlineOfflineGroup*>(object) ||
        smart_cast<CSE_ALifeSmartZone*>(object))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (IsDebugLog())
        Msg("~ [plugin:%s] alife_release: '%s' id=%u%s", PluginAddonId(self), object->name_replace(), u32(id),
            object->m_bOnline ? " (online: through GE_DESTROY)" : "");
    // CALifeSimulator__release of Lua: offline at once, online through the destroy event of the client
    if (!object->m_bOnline)
    {
        alife->release(object, true);
        return GWP_OK;
    }
    if (!g_pGameLevel)
        return GWP_ERROR_INVALID_STATE;
    NET_Packet packet;
    packet.w_begin(M_EVENT);
    packet.w_u32(Level().timeServer());
    packet.w_u16(GE_DESTROY);
    packet.w_u16(object->ID);
    Level().Send(packet, net_flags(TRUE, TRUE));
    return GWP_OK;
}

// The object of a teleport or a switch: in the world (no parent), not the actor. Squads go by their own functions.
GwpResult AlifeExtMovable(CSE_ALifeDynamicObject* object, bool allow_squad)
{
    if (!object)
        return GWP_ERROR_NOT_FOUND;
    if (smart_cast<CSE_ALifeCreatureActor*>(object))
        return GWP_ERROR_NOT_SUPPORTED;
    if (object->ID_Parent != ALife::_OBJECT_ID(-1))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!allow_squad && smart_cast<CSE_ALifeOnlineOfflineGroup*>(object))
        return GWP_ERROR_INVALID_ARGUMENT;
    return GWP_OK;
}

// A member of a squad moves and switches with its squad: teleported or switched alone it would live apart from it
// (graph registry, scheduler), and the squad pulls it back. squad_teleport moves the whole squad.
bool AlifeExtIsSquadMember(CSE_ALifeDynamicObject* object)
{
    const auto* monster = smart_cast<const CSE_ALifeMonsterAbstract*>(object);
    return monster && monster->m_group_id != ALife::_OBJECT_ID(-1);
}

// The place of a teleport by game vertex: the level vertex and the point default to the vertex's own.
bool AlifeExtTeleportPlace(uint32_t game_vertex, uint32_t& level_vertex, const float position[3], Fvector& place)
{
    if (game_vertex == GWP_INVALID_GAME_VERTEX || AlifeExtLevelOfGameVertex(game_vertex) < 0)
        return false;
    if (position)
        place.set(position[0], position[1], position[2]);
    else
        place = ai().game_graph().vertex(game_vertex)->level_point();
    u32 game = game_vertex;
    return AlifeExtResolvePlace(place, level_vertex, game);
}

GwpResult GWP_CALL ApiAlifeTeleport(const GwpPlugin* self, GwpObjectId id, uint32_t game_vertex, uint32_t level_vertex,
    const float position[3])
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "alife", "alife_teleport", alife);
    if (gate != GWP_OK)
        return gate;
    CSE_ALifeDynamicObject* object = AlifeExtObject(alife, id, true);
    const GwpResult movable = AlifeExtMovable(object, false);
    if (movable != GWP_OK)
        return movable;
    if (AlifeExtIsSquadMember(object))
        return GWP_ERROR_INVALID_ARGUMENT; // squad_teleport moves the squad with its members
    Fvector place{};
    if (!AlifeExtTeleportPlace(game_vertex, level_vertex, position, place))
        return GWP_ERROR_INVALID_ARGUMENT;
    // TeleportObject of _g.script: drops the vertex caches of the scripts (db.offline_objects, db.spawned_vertex_by_id),
    // then alife():teleport_object - which takes an online object offline first
    const GwpValue args[] = { events::Number(id), events::Vec3(place), events::Number(level_vertex),
        events::Number(game_vertex) };
    return AlifeExtCallLua(self, "alife_teleport", "TeleportObject", args, 4, nullptr);
}

GwpResult AlifeExtSetSwitch(const GwpPlugin* self, pcstr function, GwpObjectId id, bool online, int value)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "alife", function, alife);
    if (gate != GWP_OK)
        return gate;
    CSE_ALifeDynamicObject* object = AlifeExtObject(alife, id, true);
    const GwpResult movable = AlifeExtMovable(object, true);
    if (movable != GWP_OK)
        return movable == GWP_ERROR_NOT_SUPPORTED ? GWP_ERROR_INVALID_ARGUMENT : movable;
    // A squad switches its members itself and expects both flags of every member set (VERIFY3 in
    // CSE_ALifeOnlineOfflineGroup::try_switch_online / _offline): set the flags of the squad instead
    if (AlifeExtIsSquadMember(object))
        return GWP_ERROR_INVALID_ARGUMENT;
    // The flags of CALifeUpdateManager::set_switch_online / _offline, without its lookup that asserts on a bad id
    if (online)
        object->can_switch_online(value != 0);
    else
        object->can_switch_offline(value != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiAlifeSetSwitchOnline(const GwpPlugin* self, GwpObjectId id, int value)
{
    return AlifeExtSetSwitch(self, "alife_set_switch_online", id, true, value);
}

GwpResult GWP_CALL ApiAlifeSetSwitchOffline(const GwpPlugin* self, GwpObjectId id, int value)
{
    return AlifeExtSetSwitch(self, "alife_set_switch_offline", id, false, value);
}

GwpResult GWP_CALL ApiAlifeSwitchOffline(const GwpPlugin* self, GwpObjectId id)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "alife", "alife_switch_offline", alife);
    if (gate != GWP_OK)
        return gate;
    CSE_ALifeDynamicObject* object = AlifeExtObject(alife, id, true);
    const GwpResult movable = AlifeExtMovable(object, true);
    if (movable != GWP_OK)
        return movable == GWP_ERROR_NOT_SUPPORTED ? GWP_ERROR_INVALID_ARGUMENT : movable;
    if (AlifeExtIsSquadMember(object))
        return GWP_ERROR_INVALID_ARGUMENT; // a member switches with its squad: switch the squad itself
    if (!object->m_bOnline)
        return GWP_ERROR_INVALID_STATE;
    // The switch the engine makes itself (CALifeSwitchManager::switch_offline): a squad takes its members along
    alife->switch_offline(object);
    return GWP_OK;
}

GwpResult GWP_CALL ApiObjectKill(const GwpPlugin* self, GwpObjectId id, GwpObjectId who)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "alife", "object_kill", alife);
    if (gate != GWP_OK)
        return gate;
    CSE_ALifeDynamicObject* object = AlifeExtObject(alife, id, true);
    if (!object)
        return GWP_ERROR_NOT_FOUND;
    auto* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
        return GWP_ERROR_INVALID_ARGUMENT;

    if (object->m_bOnline)
    {
        // The usual death of the engine (CScriptGameObject::Kill): GE_DIE, the death events and callbacks, the corpse
        auto* entity = smart_cast<CEntity*>(AlifeExtOnline(id));
        if (!entity)
            return GWP_ERROR_INVALID_STATE; // online on the server, the client object is not there yet
        if (entity->AlreadyDie() || !entity->g_Alive())
            return GWP_ERROR_INVALID_STATE;
        if (entity->cast_actor() && psActorFlags.test(AF_GODMODE))
            return GWP_ERROR_INVALID_STATE;
        // The server finds the killer of GE_DIE among the online entities and drops the death without one
        // (xrServer_process_event.cpp): an offline killer would leave the victim alive with AlreadyDie() set
        const u16 killer = who != GWP_INVALID_OBJECT_ID && AlifeExtOnline(who) ? who : entity->ID();
        if (IsDebugLog())
            Msg("~ [plugin:%s] object_kill: '%s' id=%u by %u", PluginAddonId(self), entity->cName().c_str(), u32(id),
                u32(killer));
        entity->KillEntity(killer, false);
        return GWP_OK;
    }

    // Offline: the health goes to zero where the object is (se_obj:kill() of the scripts does the same), then what the
    // server does at a death (alife().on_death: the Lua on_death of the server object, the squad learns it lost a
    // member). Not kill_entity of the offline combat: it asserts on a creature with items (its detach is empty) and
    // moves the corpse to a death point. Not CSE_ALifeMonsterAbstract::kill() either: it drops the squad membership
    // before on_death, and the squad would miss the death.
    auto* monster = smart_cast<CSE_ALifeMonsterAbstract*>(object);
    if (!monster)
        return GWP_ERROR_INVALID_ARGUMENT; // a creature that never lives offline (a crow)
    if (!monster->g_Alive())
        return GWP_ERROR_INVALID_STATE;
    CSE_ALifeDynamicObject* killer = who != GWP_INVALID_OBJECT_ID ? AlifeExtObject(alife, who, true) : nullptr;
    if (IsDebugLog())
        Msg("~ [plugin:%s] object_kill: '%s' id=%u offline by %u", PluginAddonId(self), object->name_replace(),
            u32(id), u32(killer ? killer->ID : id));
    monster->set_health(0.f);
    alife->on_death(monster, killer ? killer : monster);
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group squads): the Lua side of the simulation (gw_plugin_sim.script)
// ---------------------------------------------------------------------------------------------

CSE_ALifeOnlineOfflineGroup* AlifeExtSquad(CALifeSimulator* alife, GwpObjectId id)
{
    return alife && id != GWP_INVALID_OBJECT_ID ?
        smart_cast<CSE_ALifeOnlineOfflineGroup*>(AlifeExtObject(alife, id, true)) : nullptr;
}

bool AlifeExtIsSmart(CALifeSimulator* alife, GwpObjectId id)
{
    return alife && id != GWP_INVALID_OBJECT_ID && smart_cast<CSE_ALifeSmartZone*>(AlifeExtObject(alife, id, true));
}

// A code result of the Lua side (gw_plugin_sim.script): 0 done, -1 not a squad, -2 no simulation board yet, -3 refused
// by the simulation, -4 a target or a smart the board does not know, -5 a community the squads do not have.
GwpResult AlifeExtSimCode(GwpResult call, const AlifeExtLuaResult& result)
{
    if (call != GWP_OK)
        return call;
    const int code = result.type == LUA_TNUMBER ? static_cast<int>(result.number) : -1;
    switch (code)
    {
    case 0: return GWP_OK;
    case -2: return GWP_ERROR_INVALID_STATE;
    case -3: return GWP_ERROR_INVALID_STATE;
    default: return GWP_ERROR_INVALID_ARGUMENT;
    }
}

// A smart or a target argument: missing from the registry = NOT_FOUND, there but of another class = INVALID_ARGUMENT.
GwpResult AlifeExtCheckSmart(CALifeSimulator* alife, GwpObjectId id)
{
    if (!AlifeExtObject(alife, id, true))
        return GWP_ERROR_NOT_FOUND;
    return AlifeExtIsSmart(alife, id) ? GWP_OK : GWP_ERROR_INVALID_ARGUMENT;
}

// A boolean result of the Lua side: true = done, false = the Lua side refused (the object is not what it expects).
GwpResult AlifeExtBoolResult(GwpResult call, const AlifeExtLuaResult& result)
{
    if (call != GWP_OK)
        return call;
    return result.type == LUA_TBOOLEAN && result.boolean ? GWP_OK : GWP_ERROR_INVALID_ARGUMENT;
}

GwpResult GWP_CALL ApiSquadCreate(const GwpPlugin* self, const char* section, GwpObjectId smart,
    const float position[3], uint32_t level_vertex, uint32_t game_vertex, GwpObjectId* out_id)
{
    if (out_id)
        *out_id = GWP_INVALID_OBJECT_ID;
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "squads", "squad_create", alife);
    if (gate != GWP_OK)
        return gate;
    if (!alife->server().HasEnoughIDs(kAlifeExtSquadIdReserve))
    {
        Msg("! [plugin:%s] squad_create: '%s' refused, fewer than %u free object ids", PluginAddonId(self),
            section ? section : "", u32(kAlifeExtSquadIdReserve));
        return GWP_ERROR_INVALID_STATE;
    }
    const EAlifeExtKind kind = AlifeExtSectionKind(section, nullptr);
    if (kind == EAlifeExtKind::Missing)
        return GWP_ERROR_NOT_FOUND;
    if (kind != EAlifeExtKind::Squad)
        return GWP_ERROR_INVALID_ARGUMENT;

    Fvector place{};
    if (smart != GWP_INVALID_OBJECT_ID)
    {
        const GwpResult checked = AlifeExtCheckSmart(alife, smart);
        if (checked != GWP_OK)
            return checked;
    }
    else
    {
        if (!position)
            return GWP_ERROR_INVALID_ARGUMENT;
        place.set(position[0], position[1], position[2]);
        if (!AlifeExtResolvePlace(place, level_vertex, game_vertex))
            return GWP_ERROR_INVALID_ARGUMENT;
    }
    const GwpValue args[] = { events::String(section), AlifeExtIdArg(smart), events::Vec3(place),
        events::Number(level_vertex), events::Number(game_vertex) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_create");
    const GwpResult call = AlifeExtCallLua(self, "squad_create", function.c_str(), args, 5, &result);
    if (call != GWP_OK)
        return call;
    const GwpObjectId id = AlifeExtResultId(result);
    if (id == GWP_INVALID_OBJECT_ID)
        return AlifeExtSimCode(GWP_OK, result); // -4: the smart is not on the board (disabled, another level set)
    if (IsDebugLog())
        Msg("~ [plugin:%s] squad_create: '%s' id=%u", PluginAddonId(self), section, u32(id));
    if (out_id)
        *out_id = id;
    return GWP_OK;
}

GwpResult GWP_CALL ApiSquadRemove(const GwpPlugin* self, GwpObjectId squad)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "squads", "squad_remove", alife);
    if (gate != GWP_OK)
        return gate;
    if (!AlifeExtSquad(alife, squad))
        return AlifeExtObject(alife, squad, true) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
    const GwpValue args[] = { events::Number(squad) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_remove");
    return AlifeExtBoolResult(AlifeExtCallLua(self, "squad_remove", function.c_str(), args, 1, &result), result);
}

GwpResult GWP_CALL ApiSquadAssignSmart(const GwpPlugin* self, GwpObjectId squad, GwpObjectId smart)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "squads", "squad_assign_smart", alife);
    if (gate != GWP_OK)
        return gate;
    if (!AlifeExtSquad(alife, squad))
        return AlifeExtObject(alife, squad, true) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
    if (smart != GWP_INVALID_OBJECT_ID)
    {
        const GwpResult checked = AlifeExtCheckSmart(alife, smart);
        if (checked != GWP_OK)
            return checked;
    }
    const GwpValue args[] = { events::Number(squad), AlifeExtIdArg(smart) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_assign_smart");
    return AlifeExtSimCode(AlifeExtCallLua(self, "squad_assign_smart", function.c_str(), args, 2, &result), result);
}

GwpResult GWP_CALL ApiSquadSetTarget(const GwpPlugin* self, GwpObjectId squad, GwpObjectId target)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "squads", "squad_set_target", alife);
    if (gate != GWP_OK)
        return gate;
    if (!AlifeExtSquad(alife, squad))
        return AlifeExtObject(alife, squad, true) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
    if (!AlifeExtObject(alife, target, true))
        return GWP_ERROR_NOT_FOUND;
    if (!AlifeExtIsSmart(alife, target) && !AlifeExtSquad(alife, target))
        return GWP_ERROR_INVALID_ARGUMENT;
    const GwpValue args[] = { events::Number(squad), events::Number(target) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_set_target");
    return AlifeExtSimCode(AlifeExtCallLua(self, "squad_set_target", function.c_str(), args, 2, &result), result);
}

GwpResult GWP_CALL ApiSquadTeleport(const GwpPlugin* self, GwpObjectId squad, uint32_t game_vertex,
    uint32_t level_vertex, const float position[3])
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "squads", "squad_teleport", alife);
    if (gate != GWP_OK)
        return gate;
    if (!AlifeExtSquad(alife, squad))
        return AlifeExtObject(alife, squad, true) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
    Fvector place{};
    if (!AlifeExtTeleportPlace(game_vertex, level_vertex, position, place))
        return GWP_ERROR_INVALID_ARGUMENT;
    const GwpValue args[] = { events::Number(squad), events::Vec3(place), events::Number(level_vertex),
        events::Number(game_vertex) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_teleport");
    return AlifeExtBoolResult(AlifeExtCallLua(self, "squad_teleport", function.c_str(), args, 4, &result), result);
}

uint32_t GWP_CALL ApiSquadCommunity(GwpObjectId squad, char* out, uint32_t cap)
{
    if (!AlifeExtCheckCall("squads", "squad_community") || !AlifeExtSquad(AlifeExtSimulator(), squad))
        return AlifeExtNone(out, cap);
    const GwpValue args[] = { events::Number(squad) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_community");
    if (AlifeExtCallLua(nullptr, "squad_community", function.c_str(), args, 1, &result) != GWP_OK ||
        result.type != LUA_TSTRING)
        return AlifeExtNone(out, cap);
    return AlifeExtCopy(result.text.c_str(), out, cap);
}

GwpResult GWP_CALL ApiSquadSetCommunity(const GwpPlugin* self, GwpObjectId squad, const char* community)
{
    CALifeSimulator* alife = nullptr;
    const GwpResult gate = AlifeExtWriteGate(self, "squads", "squad_set_community", alife);
    if (gate != GWP_OK)
        return gate;
    if (!community || !*community)
        return GWP_ERROR_INVALID_ARGUMENT;
    const CSE_ALifeOnlineOfflineGroup* group = AlifeExtSquad(alife, squad);
    if (!group)
        return AlifeExtObject(alife, squad, true) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
    // Checked before Lua, where a mistake is not an error but a stop of the game: an unknown community asserts in
    // set_community / set_character_community (CHARACTER_COMMUNITY without no_assert), and a member that is not a
    // character (a monster squad) has no set_community at all - Lua fails half way through the members
    if (CHARACTER_COMMUNITY::IdToIndex(community, NO_COMMUNITY_INDEX, true) == NO_COMMUNITY_INDEX)
        return GWP_ERROR_INVALID_ARGUMENT;
    for (const auto& member : group->squad_members())
    {
        if (!smart_cast<const CSE_ALifeTraderAbstract*>(member.second))
            return GWP_ERROR_INVALID_ARGUMENT;
    }
    const GwpValue args[] = { events::Number(squad), events::String(community) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("squad_set_community");
    return AlifeExtSimCode(AlifeExtCallLua(self, "squad_set_community", function.c_str(), args, 2, &result), result);
}

GwpObjectId GWP_CALL ApiSmartFind(const char* name)
{
    if (!AlifeExtCheckCall("squads", "smart_find") || !name || !*name || !AlifeExtSimulator())
        return GWP_INVALID_OBJECT_ID;
    const GwpValue args[] = { events::String(name) };
    AlifeExtLuaResult result;
    const xr_string function = AlifeExtSimFunction("smart_find");
    if (AlifeExtCallLua(nullptr, "smart_find", function.c_str(), args, 1, &result) != GWP_OK)
        return GWP_INVALID_OBJECT_ID;
    return AlifeExtResultId(result);
}

// ---------------------------------------------------------------------------------------------
// Events server_object_on_register / server_object_on_unregister
// ---------------------------------------------------------------------------------------------

void AlifeExtEmitRegistry(events::EBuiltin event, CSE_ALifeDynamicObject* object)
{
    if (!object || !events::HasSubscribers(event))
        return;
    if (!IsMainThread())
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Msg("! [alife] server object registry changed outside the main thread: its events are not sent");
        }
        return;
    }
    string16 class_id;
    AlifeExtClassText(object->m_tClassID, class_id); // without the padding: the form of alife_object_class_id
    const GwpValue args[] = { events::ServerObject(object->ID), events::String(object->s_name.c_str()),
        events::String(class_id) };
    ++g_alife_ext_register_depth;
    events::Emit(event, args, 3);
    --g_alife_ext_register_depth;
}
} // namespace

namespace alife_ext
{
void OnServerObjectRegistered(CSE_ALifeDynamicObject* object)
{
    AlifeExtEmitRegistry(events::EBuiltin::ServerObjectOnRegister, object);
}

void OnServerObjectUnregistering(CSE_ALifeDynamicObject* object)
{
    AlifeExtEmitRegistry(events::EBuiltin::ServerObjectOnUnregister, object);
}

bool InRegistryEvent() { return g_alife_ext_register_depth != 0; }
} // namespace alife_ext

void FillAlifeExtApi(GwpEngineApi& api)
{
    api.alife_present = &ApiAlifePresent;
    api.alife_free_ids = &ApiAlifeFreeIds;
    api.alife_object_exists = &ApiAlifeObjectExists;
    api.alife_object_section = &ApiAlifeObjectSection;
    api.alife_object_name = &ApiAlifeObjectName;
    api.alife_object_parent = &ApiAlifeObjectParent;
    api.alife_object_online = &ApiAlifeObjectOnline;
    api.alife_object_alive = &ApiAlifeObjectAlive;
    api.alife_object_community = &ApiAlifeObjectCommunity;
    api.alife_object_rank = &ApiAlifeObjectRank;
    api.alife_object_by_name = &ApiAlifeObjectByName;
    api.alife_objects = &ApiAlifeObjects;
    api.story_object_id = &ApiStoryObjectId;
    api.object_story_id = &ApiObjectStoryId;
    api.alife_create = &ApiAlifeCreate;
    api.alife_create_ammo = &ApiAlifeCreateAmmo;
    api.alife_release = &ApiAlifeRelease;
    api.alife_teleport = &ApiAlifeTeleport;
    api.alife_set_switch_online = &ApiAlifeSetSwitchOnline;
    api.alife_set_switch_offline = &ApiAlifeSetSwitchOffline;
    api.alife_switch_offline = &ApiAlifeSwitchOffline;
    api.object_kill = &ApiObjectKill;
    api.squad_create = &ApiSquadCreate;
    api.squad_remove = &ApiSquadRemove;
    api.squad_assign_smart = &ApiSquadAssignSmart;
    api.squad_set_target = &ApiSquadSetTarget;
    api.squad_teleport = &ApiSquadTeleport;
    api.squad_community = &ApiSquadCommunity;
    api.squad_set_community = &ApiSquadSetCommunity;
    api.smart_find = &ApiSmartFind;
}
} // namespace gw::addons
