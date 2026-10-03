#include "StdAfx.h"

// Plugin API, group "character": money, rank, reputation, name and community of a character, personal and community
// goodwill, community relations, the characteristics of creatures (GWP_COND_*); and the engine side of the events
// npc_on_before_hit, monster_on_before_hit (CAI_Stalker::Hit, CBaseMonster::Hit) and relation_on_changed (the
// setters of RELATION_REGISTRY). Each function does what the Lua method named in gwp_api.h does, through the same
// engine calls, without the script errors Lua logs.
// Docs: wiki/doc/plugins/api/character.md

#include "addon_api_character.h"
#include "addon_api_common.h"
#include "addon_event_bus.h"

#include "Actor.h"
#include "ActorCondition.h"
#include "EntityCondition.h"
#include "Hit.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "character_community.h"
#include "entity_alive.h"
#include "relation_registry.h"
#include "xrServerEntities/character_info.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>

// The numbers of gwp_api.h are the engine's own (creature_relation answers them, character_set_relation takes them)
static_assert(GWP_RELATION_FRIEND == ALife::eRelationTypeFriend);
static_assert(GWP_RELATION_NEUTRAL == ALife::eRelationTypeNeutral);
static_assert(GWP_RELATION_ENEMY == ALife::eRelationTypeEnemy);
static_assert(GWP_RELATION_WORST_ENEMY == ALife::eRelationTypeWorstEnemy);

namespace gw::addons::character
{
namespace
{
constexpr pcstr kGroup = "character";

// A reading function: the main thread only (no plugin handle to name)
bool CharacterReadAllowed(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [character] %s called outside the main thread, ignored", function);
    return false;
}

// The ALife server object of a character (the actor, a stalker, a trader), online or not; nullptr without a game
CSE_ALifeTraderAbstract* CharacterServerObject(GwpObjectId id)
{
    if (id == GWP_INVALID_OBJECT_ID || !ai().get_alife())
        return nullptr;
    return smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(id, true));
}

// The code for a character found neither online nor in ALife: another kind of object or nothing at all
GwpResult CharacterMissingCode(GwpObjectId id)
{
    if (!ai().get_alife())
        return GWP_ERROR_INVALID_STATE;
    if (id != GWP_INVALID_OBJECT_ID && ai().alife().objects().object(id, true))
        return GWP_ERROR_INVALID_ARGUMENT;
    return api::FindOnlineObject(id) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
}

// An online character whose data the Lua methods change: an inventory owner that is a living entity, not a monster
// (CBaseMonster keeps its rank in the ltx, CInventoryOwner::SetRank skips it)
CInventoryOwner* OnlineCharacter(CGameObject* object)
{
    if (!object || smart_cast<CBaseMonster*>(object) || !smart_cast<CEntityAlive*>(object))
        return nullptr;
    return smart_cast<CInventoryOwner*>(object);
}

// The online character with the id (OnlineCharacter), or nullptr
CInventoryOwner* FindOnlineCharacter(GwpObjectId id) { return OnlineCharacter(api::FindOnlineObject(id)); }

// Goodwill and relation values a plugin may write: far outside every config limit, far from the int overflow of
// the sums the engine makes (GetAttitude adds five of them) and from NO_GOODWILL (-INT_MAX)
constexpr int32_t kCharacterGoodwillLimit = 1000000;
bool IsCharacterGoodwillValue(int32_t value)
{
    return value >= -kCharacterGoodwillLimit && value <= kCharacterGoodwillLimit;
}

// The index of a community of game_relations.ltx; NO_COMMUNITY_INDEX for an unknown name (no assert, unlike Lua)
CHARACTER_COMMUNITY_INDEX CharacterCommunityIndex(const char* name)
{
    if (!name || !*name)
        return NO_COMMUNITY_INDEX;
    return CHARACTER_COMMUNITY::IdToIndex(name, NO_COMMUNITY_INDEX, true);
}

pcstr CharacterCommunityName(int index)
{
    const CHARACTER_COMMUNITY_ID id = CHARACTER_COMMUNITY::IndexToId(index, nullptr, true);
    return id.size() ? id.c_str() : "";
}

// Copies a string into (out, cap), cut to cap - 1; the full length is the result
uint32_t CharacterCopyString(pcstr text, char* out, uint32_t cap)
{
    const size_t length = text ? xr_strlen(text) : 0;
    if (out && cap)
    {
        const size_t copied = std::min<size_t>(length, cap - 1);
        if (copied)
            std::memcpy(out, text, copied);
        out[copied] = 0;
    }
    return static_cast<uint32_t>(length);
}

// ---------------------------------------------------------------------------------------------
// Money
// ---------------------------------------------------------------------------------------------

uint32_t GWP_CALL ApiCharacterMoney(GwpObjectId id)
{
    if (!CharacterReadAllowed("character_money"))
        return 0;
    const CInventoryOwner* owner = FindOnlineCharacter(id);
    return owner ? owner->get_money() : 0;
}

GwpResult GWP_CALL ApiCharacterGiveMoney(const GwpPlugin* self, GwpObjectId id, int32_t amount)
{
    constexpr pcstr function = "character_give_money";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    // A monster is an inventory owner too, without a character profile: set_money reads the profile (null there)
    CInventoryOwner* owner = FindOnlineCharacter(id);
    if (!owner)
        return api::PluginRefusal(self, function, api::MissingObjectCode(id), "not an online character");
    const s64 sum = static_cast<s64>(owner->get_money()) + amount;
    if (sum < 0)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "the character has less money");
    if (sum > static_cast<s64>(UINT32_MAX))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the sum does not fit 32 bits");
    // give_money of Lua: set_money with the network event (the server object keeps the sum)
    owner->set_money(static_cast<u32>(sum), true);
    return GWP_OK;
}

GwpResult GWP_CALL ApiCharacterTransferMoney(const GwpPlugin* self, GwpObjectId from, GwpObjectId to, uint32_t amount)
{
    constexpr pcstr function = "character_transfer_money";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    CInventoryOwner* giver = FindOnlineCharacter(from);
    if (!giver)
        return api::PluginRefusal(self, function, api::MissingObjectCode(from), "`from` is not an online character");
    CInventoryOwner* taker = FindOnlineCharacter(to);
    if (!taker)
        return api::PluginRefusal(self, function, api::MissingObjectCode(to), "`to` is not an online character");
    if (from == to)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "from == to");
    if (giver->get_money() < amount)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "`from` has less money");
    if (static_cast<u64>(taker->get_money()) + amount > UINT32_MAX)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the sum does not fit 32 bits");
    // transfer_money of Lua
    giver->set_money(giver->get_money() - amount, true);
    taker->set_money(taker->get_money() + amount, true);
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Rank, reputation, name, community
// ---------------------------------------------------------------------------------------------

int32_t GWP_CALL ApiCharacterRank(GwpObjectId id)
{
    if (!CharacterReadAllowed("character_rank"))
        return INT32_MIN;
    CGameObject* object = api::FindOnlineObject(id);
    if (!object)
        return INT32_MIN;
    // character_rank() of Lua: a monster answers the rank of its ltx
    if (CBaseMonster* monster = smart_cast<CBaseMonster*>(object))
        return monster->Rank();
    const CInventoryOwner* owner = smart_cast<CInventoryOwner*>(object);
    return owner ? owner->Rank() : INT32_MIN;
}

int32_t GWP_CALL ApiCharacterReputation(GwpObjectId id)
{
    if (!CharacterReadAllowed("character_reputation"))
        return INT32_MIN;
    const CInventoryOwner* owner = FindOnlineCharacter(id);
    return owner ? owner->Reputation() : INT32_MIN;
}

uint32_t GWP_CALL ApiCharacterName(GwpObjectId id, char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = 0;
    if (!CharacterReadAllowed("character_name"))
        return 0;
    const CInventoryOwner* owner = api::FindOnline<CInventoryOwner>(id);
    return owner ? CharacterCopyString(owner->Name(), out, cap) : 0;
}

enum class ECharacterValue
{
    Rank,
    Reputation,
};

GwpResult SetCharacterValue(const GwpPlugin* self, GwpObjectId id, int32_t value, ECharacterValue what, pcstr function)
{
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    if (!ai().get_alife())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no game");
    CSE_ALifeTraderAbstract* trader = CharacterServerObject(id);
    if (CGameObject* object = api::FindOnlineObject(id))
    {
        CInventoryOwner* owner = OnlineCharacter(object);
        if (!owner || !trader)
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "not a character");
        // set_character_rank / set_character_reputation of Lua: the client value and the server object
        if (what == ECharacterValue::Rank)
            owner->SetRank(value);
        else
            owner->SetReputation(value);
        return GWP_OK;
    }
    if (!trader)
        return api::PluginRefusal(self, function, CharacterMissingCode(id), "not a character");
    // An offline character: its server object only. The profile is chosen lazily and sets rank and reputation when it
    // is: choose it first, so the value written here is not overwritten later
    trader->specific_character();
    if (what == ECharacterValue::Rank)
        trader->m_rank = value;
    else
        trader->m_reputation = value;
    return GWP_OK;
}

GwpResult GWP_CALL ApiCharacterSetRank(const GwpPlugin* self, GwpObjectId id, int32_t rank)
{
    return SetCharacterValue(self, id, rank, ECharacterValue::Rank, "character_set_rank");
}

GwpResult GWP_CALL ApiCharacterSetReputation(const GwpPlugin* self, GwpObjectId id, int32_t reputation)
{
    return SetCharacterValue(self, id, reputation, ECharacterValue::Reputation, "character_set_reputation");
}

GwpResult GWP_CALL ApiCharacterSetName(const GwpPlugin* self, GwpObjectId id, const char* name)
{
    constexpr pcstr function = "character_set_name";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    if (!name || !*name)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an empty name");
    if (!ai().get_alife())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no game");
    CSE_ALifeTraderAbstract* trader = CharacterServerObject(id);
    if (CGameObject* object = api::FindOnlineObject(id))
    {
        CInventoryOwner* owner = OnlineCharacter(object);
        if (!owner || !trader)
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "not a character");
        owner->SetName(name); // set_character_name of Lua: the client only
    }
    else if (!trader)
        return api::PluginRefusal(self, function, CharacterMissingCode(id), "not a character");
    // The server object keeps the name for the save and the next spawn (CInventoryOwner::net_Spawn copies it)
    trader->m_character_name = name;
    return GWP_OK;
}

GwpResult GWP_CALL ApiCharacterSetCommunity(const GwpPlugin* self, GwpObjectId id, const char* community)
{
    constexpr pcstr function = "character_set_community";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    const CHARACTER_COMMUNITY_INDEX index = CharacterCommunityIndex(community);
    if (index == NO_COMMUNITY_INDEX)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an unknown community");
    if (!ai().get_alife())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no game");
    CSE_ALifeTraderAbstract* trader = CharacterServerObject(id);
    if (CGameObject* object = api::FindOnlineObject(id))
    {
        CInventoryOwner* owner = OnlineCharacter(object);
        if (!owner || !trader)
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "not a character");
        // set_character_community(community, squad, group) of Lua with the current squad and group: SetCommunity
        // changes the team of a living character with them and writes the server object (a dead one keeps its team:
        // CEntity::ChangeTeam verifies a living entity)
        owner->SetCommunity(index);
        return GWP_OK;
    }
    if (!trader)
        return api::PluginRefusal(self, function, CharacterMissingCode(id), "not a character");
    trader->m_community_index = index;
    // The team of the engine comes online from the server object (s_team): the team of the new community, as the
    // server object gets it when its profile is chosen
    if (CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(trader->base()))
    {
        CHARACTER_COMMUNITY new_community;
        new_community.set(index);
        creature->s_team = new_community.team();
    }
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Personal goodwill
// ---------------------------------------------------------------------------------------------

int32_t GWP_CALL ApiCharacterGoodwill(GwpObjectId from, GwpObjectId to)
{
    if (!CharacterReadAllowed("character_goodwill") || !ai().get_alife() || from == GWP_INVALID_OBJECT_ID ||
        to == GWP_INVALID_OBJECT_ID)
        return NEUTRAL_GOODWILL;
    return RELATION_REGISTRY().GetGoodwill(from, to);
}

// Both ids are ALife characters and differ; GWP_OK or the code of the refusal
GwpResult CheckCharacterPair(const GwpPlugin* self, GwpObjectId from, GwpObjectId to, pcstr function)
{
    if (!ai().get_alife())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no game");
    if (!CharacterServerObject(from))
        return api::PluginRefusal(self, function, CharacterMissingCode(from), "`from` is not a character");
    if (!CharacterServerObject(to))
        return api::PluginRefusal(self, function, CharacterMissingCode(to), "`to` is not a character");
    if (from == to)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "from == to");
    return GWP_OK;
}

GwpResult GWP_CALL ApiCharacterSetGoodwill(const GwpPlugin* self, GwpObjectId from, GwpObjectId to, int32_t goodwill)
{
    constexpr pcstr function = "character_set_goodwill";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    if (!IsCharacterGoodwillValue(goodwill))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "a goodwill out of -1000000 .. 1000000");
    const GwpResult checked = CheckCharacterPair(self, from, to, function);
    if (checked != GWP_OK)
        return checked;
    RELATION_REGISTRY().SetGoodwill(from, to, goodwill); // clamps to personal_goodwill_limits
    return GWP_OK;
}

GwpResult GWP_CALL ApiCharacterForceGoodwill(const GwpPlugin* self, GwpObjectId from, GwpObjectId to, int32_t goodwill)
{
    constexpr pcstr function = "character_force_goodwill";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    // ForceSetGoodwill does not clamp: the range keeps the subtraction of the community parts from overflowing
    if (!IsCharacterGoodwillValue(goodwill))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "a goodwill out of -1000000 .. 1000000");
    const GwpResult checked = CheckCharacterPair(self, from, to, function);
    if (checked != GWP_OK)
        return checked;
    RELATION_REGISTRY().ForceSetGoodwill(from, to, goodwill);
    return GWP_OK;
}

int32_t GWP_CALL ApiCharacterAttitude(GwpObjectId from, GwpObjectId to)
{
    if (!CharacterReadAllowed("character_attitude") || !ai().get_alife())
        return NEUTRAL_GOODWILL;
    const CInventoryOwner* owner_from = FindOnlineCharacter(from);
    const CInventoryOwner* owner_to = FindOnlineCharacter(to);
    if (!owner_from || !owner_to)
        return NEUTRAL_GOODWILL;
    return RELATION_REGISTRY().GetAttitude(owner_from, owner_to); // general_goodwill of Lua
}

GwpResult GWP_CALL ApiCharacterSetRelation(const GwpPlugin* self, GwpObjectId from, GwpObjectId to, uint32_t relation)
{
    constexpr pcstr function = "character_set_relation";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    pcstr line = nullptr;
    switch (relation)
    {
    case GWP_RELATION_FRIEND: line = "goodwill_friend"; break;
    case GWP_RELATION_NEUTRAL: line = "goodwill_neutal"; break; // the spelling of game_relations.ltx
    case GWP_RELATION_ENEMY: line = "goodwill_enemy"; break;
    default: return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "not FRIEND, NEUTRAL or ENEMY");
    }
    const GwpResult checked = CheckCharacterPair(self, from, to, function);
    if (checked != GWP_OK)
        return checked;
    // RELATION_REGISTRY::SetRelationType without its online objects: the same goodwill of the same lines, so offline
    // characters work too
    RELATION_REGISTRY().SetGoodwill(from, to, pSettings->r_s16(GAME_RELATIONS_SECT, line));
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Communities
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiCommunityExists(const char* community)
{
    if (!CharacterReadAllowed("community_exists"))
        return 0;
    return CharacterCommunityIndex(community) != NO_COMMUNITY_INDEX ? 1 : 0;
}

int32_t GWP_CALL ApiCommunityGoodwill(const char* community, GwpObjectId who)
{
    if (!CharacterReadAllowed("community_goodwill") || !ai().get_alife() || who == GWP_INVALID_OBJECT_ID)
        return NEUTRAL_GOODWILL;
    const CHARACTER_COMMUNITY_INDEX index = CharacterCommunityIndex(community);
    return index != NO_COMMUNITY_INDEX ? RELATION_REGISTRY().GetCommunityGoodwill(index, who) : NEUTRAL_GOODWILL;
}

GwpResult ChangeCommunityGoodwill(const GwpPlugin* self, const char* community, GwpObjectId who, int32_t value,
    bool delta, pcstr function)
{
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    if (!IsCharacterGoodwillValue(value))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "a value out of -1000000 .. 1000000");
    const CHARACTER_COMMUNITY_INDEX index = CharacterCommunityIndex(community);
    if (index == NO_COMMUNITY_INDEX)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an unknown community");
    if (!ai().get_alife())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no game");
    if (!CharacterServerObject(who))
        return api::PluginRefusal(self, function, CharacterMissingCode(who), "`who` is not a character");
    if (delta)
        RELATION_REGISTRY().ChangeCommunityGoodwill(index, who, value);
    else
        RELATION_REGISTRY().SetCommunityGoodwill(index, who, value); // clamps to community_goodwill_limits
    return GWP_OK;
}

GwpResult GWP_CALL ApiCommunitySetGoodwill(const GwpPlugin* self, const char* community, GwpObjectId who,
    int32_t goodwill)
{
    return ChangeCommunityGoodwill(self, community, who, goodwill, false, "community_set_goodwill");
}

GwpResult GWP_CALL ApiCommunityChangeGoodwill(const GwpPlugin* self, const char* community, GwpObjectId who,
    int32_t delta)
{
    return ChangeCommunityGoodwill(self, community, who, delta, true, "community_change_goodwill");
}

int32_t GWP_CALL ApiCommunityRelation(const char* from, const char* to)
{
    if (!CharacterReadAllowed("community_relation"))
        return NEUTRAL_GOODWILL;
    const CHARACTER_COMMUNITY_INDEX index_from = CharacterCommunityIndex(from);
    const CHARACTER_COMMUNITY_INDEX index_to = CharacterCommunityIndex(to);
    if (index_from == NO_COMMUNITY_INDEX || index_to == NO_COMMUNITY_INDEX)
        return NEUTRAL_GOODWILL;
    return RELATION_REGISTRY().GetCommunityRelation(index_from, index_to);
}

GwpResult GWP_CALL ApiCommunitySetRelation(const GwpPlugin* self, const char* from, const char* to, int32_t goodwill)
{
    constexpr pcstr function = "community_set_relation";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    if (!IsCharacterGoodwillValue(goodwill))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "a goodwill out of -1000000 .. 1000000");
    const CHARACTER_COMMUNITY_INDEX index_from = CharacterCommunityIndex(from);
    const CHARACTER_COMMUNITY_INDEX index_to = CharacterCommunityIndex(to);
    if (index_from == NO_COMMUNITY_INDEX || index_to == NO_COMMUNITY_INDEX)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an unknown community");
    RELATION_REGISTRY().SetCommunityRelation(index_from, index_to, goodwill);
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Characteristics (GWP_COND_*)
// ---------------------------------------------------------------------------------------------

bool IsActorOnlyCondition(uint32_t condition)
{
    return condition == GWP_COND_SATIETY || condition == GWP_COND_ALCOHOL;
}

float GWP_CALL ApiCreatureCondition(GwpObjectId id, uint32_t condition)
{
    if (!CharacterReadAllowed("creature_condition"))
        return -1.f;
    CEntityAlive* creature = api::FindOnline<CEntityAlive>(id);
    if (!creature)
        return -1.f;
    CActor* actor = smart_cast<CActor*>(creature);
    if (IsActorOnlyCondition(condition) && !actor)
        return -1.f;
    CEntityCondition& conditions = creature->conditions();
    switch (condition)
    {
    case GWP_COND_HEALTH: return conditions.GetHealth();
    case GWP_COND_POWER: return conditions.GetPower();
    case GWP_COND_RADIATION: return conditions.GetRadiation();
    case GWP_COND_BLEEDING: return conditions.BleedingSpeed();
    case GWP_COND_PSY_HEALTH: return conditions.GetPsyHealth();
    case GWP_COND_MORALE: return conditions.GetEntityMorale();
    // CEntityCondition::GetSatiety (what Lua reads) is a constant 1: the real values live in CActorCondition
    case GWP_COND_SATIETY: return actor->conditions().GetSatiety();
    case GWP_COND_ALCOHOL: return actor->conditions().GetAlcohol();
    case GWP_COND_HEALTH_MAX: return conditions.GetMaxHealth();
    case GWP_COND_POWER_MAX: return conditions.GetMaxPower();
    default: return -1.f;
    }
}

// The creature a write may change: GWP_OK with `creature` / `actor` set, or the code of the refusal
GwpResult FindWritableCreature(const GwpPlugin* self, GwpObjectId id, uint32_t condition, float value, pcstr function,
    CEntityAlive*& creature, CActor*& actor)
{
    creature = api::FindOnline<CEntityAlive>(id);
    if (!creature)
        return api::PluginRefusal(self, function, api::MissingObjectCode(id), "not an online creature");
    actor = smart_cast<CActor*>(creature);
    if (condition >= GWP_COND_COUNT)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an unknown characteristic");
    if (IsActorOnlyCondition(condition) && !actor)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "satiety and alcohol: the actor only");
    if (!std::isfinite(value))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "NaN or infinity");
    if (!creature->g_Alive())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "a dead creature");
    return GWP_OK;
}

GwpResult GWP_CALL ApiCreatureSetCondition(const GwpPlugin* self, GwpObjectId id, uint32_t condition, float value)
{
    constexpr pcstr function = "creature_set_condition";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    CEntityAlive* creature = nullptr;
    CActor* actor = nullptr;
    const GwpResult found = FindWritableCreature(self, id, condition, value, function, creature, actor);
    if (found != GWP_OK)
        return found;
    CEntityCondition& conditions = creature->conditions();
    switch (condition)
    {
    case GWP_COND_HEALTH:
        // set_health_ex of Lua: at once; at 0 or below the next update kills the creature
        creature->SetfHealth(std::clamp(value, -0.01f, conditions.GetMaxHealth()));
        break;
    case GWP_COND_POWER: conditions.SetPower(value); break;
    case GWP_COND_RADIATION: conditions.SetRadiation(value); break;
    case GWP_COND_PSY_HEALTH: conditions.SetPsyHealth(value); break;
    case GWP_COND_MORALE: conditions.SetEntityMorale(value); break;
    case GWP_COND_BLEEDING:
        if (value != 0.f)
            return api::PluginRefusal(self, function, GWP_ERROR_NOT_SUPPORTED, "bleeding: only 0 (close every wound)");
        conditions.ClearWounds();
        break;
    case GWP_COND_SATIETY:
        actor->conditions().ChangeSatiety(std::clamp(value, 0.f, 1.f) - actor->conditions().GetSatiety());
        break;
    case GWP_COND_ALCOHOL:
        actor->conditions().ChangeAlcohol(std::clamp(value, 0.f, 1.f) - actor->conditions().GetAlcohol());
        break;
    case GWP_COND_HEALTH_MAX:
        if (value <= 0.f || value > 1.f)
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the maximum of health: 0 < max <= 1");
        creature->SetMaxHealth(value);
        break;
    case GWP_COND_POWER_MAX: conditions.SetMaxPower(value); break; // clamps to 0.1 .. 1
    default: return GWP_ERROR_INVALID_ARGUMENT; // FindWritableCreature refused it already
    }
    return GWP_OK;
}

GwpResult GWP_CALL ApiCreatureChangeCondition(const GwpPlugin* self, GwpObjectId id, uint32_t condition, float delta)
{
    constexpr pcstr function = "creature_change_condition";
    if (!api::CheckPluginMutation(self, kGroup, function))
        return api::PluginCallRefused(self);
    CEntityAlive* creature = nullptr;
    CActor* actor = nullptr;
    const GwpResult found = FindWritableCreature(self, id, condition, delta, function, creature, actor);
    if (found != GWP_OK)
        return found;
    CEntityCondition& conditions = creature->conditions();
    switch (condition)
    {
    // The setters of the Lua properties (obj.health = delta, ...): deltas applied at the next UpdateCondition
    case GWP_COND_HEALTH: conditions.ChangeHealth(delta); break;
    case GWP_COND_POWER: conditions.ChangePower(delta); break;
    case GWP_COND_RADIATION: conditions.ChangeRadiation(delta); break;
    case GWP_COND_PSY_HEALTH: conditions.ChangePsyHealth(delta); break;
    case GWP_COND_MORALE: conditions.ChangeEntityMorale(delta); break;
    case GWP_COND_BLEEDING:
        if (delta > 0.f)
            return api::PluginRefusal(self, function, GWP_ERROR_NOT_SUPPORTED, "bleeding grows only from hits");
        if (delta < 0.f)
            conditions.ChangeBleeding(-delta); // obj.bleeding = amount of Lua: every wound closes by `amount`
        break;
    case GWP_COND_SATIETY: actor->conditions().ChangeSatiety(delta); break;
    case GWP_COND_ALCOHOL:
        // ChangeAlcohol does not clamp (the next update does): clamped here, a read right after the call is in range
        actor->conditions().ChangeAlcohol(
            std::clamp(actor->conditions().GetAlcohol() + delta, 0.f, 1.f) - actor->conditions().GetAlcohol());
        break;
    case GWP_COND_HEALTH_MAX:
        if (conditions.GetMaxHealth() + delta <= 0.f || conditions.GetMaxHealth() + delta > 1.f)
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the maximum of health: 0 < max <= 1");
        creature->SetMaxHealth(conditions.GetMaxHealth() + delta);
        break;
    case GWP_COND_POWER_MAX: conditions.SetMaxPower(conditions.GetMaxPower() + delta); break;
    default: return GWP_ERROR_INVALID_ARGUMENT;
    }
    return GWP_OK;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Events of the engine
// ---------------------------------------------------------------------------------------------

bool BeforeHit(CEntityAlive* victim, SHit& hit, bool monster)
{
    const events::EBuiltin event = monster ? events::EBuiltin::MonsterOnBeforeHit : events::EBuiltin::NpcOnBeforeHit;
    if (!victim || !events::HasSubscribers(event))
        return true;
    const IGameObject* who = hit.who;
    const GwpValue args[] = {
        events::Object(victim->ID()),
        events::Number(hit.power),
        events::Vec3(hit.direction()),
        who ? events::Object(who->ID()) : events::Nil(),
        events::Int(hit.boneID == BI_NONE ? -1 : static_cast<s64>(hit.boneID)),
        events::Int(static_cast<s64>(hit.hit_type)),
        events::Number(hit.impulse),
    };
    // The result: the power of the hit; a subscriber writes another number, or false to cancel the hit. A power of 0
    // or below cancels it too: the engine adds damage of its own after this point (a finishing shot at a wounded
    // stalker, the extra power of the skin armor of a monster), so "no damage" holds only when the hit does not run
    GwpValue result = events::Number(hit.power);
    events::Emit(event, args, static_cast<u32>(std::size(args)), &result);
    double power = hit.power;
    switch (result.type)
    {
    case GWP_T_BOOL: return result.u.b != 0;
    case GWP_T_NUMBER:
        if (!std::isfinite(result.u.n))
            return true; // the hit as it was
        power = result.u.n;
        break;
    case GWP_T_INT: power = static_cast<double>(result.u.i); break;
    default: return true; // nil (a Lua handler cleared ret_value) or another type: the hit as it was
    }
    if (power <= 0.)
        return false;
    hit.power = static_cast<float>(std::min(power, static_cast<double>(FLT_MAX)));
    return true;
}

bool WantsRelationEvents() { return events::HasSubscribers(events::EBuiltin::RelationOnChanged); }

void GoodwillChanged(u16 from, u16 to, int old_goodwill, int new_goodwill)
{
    const GwpValue args[] = { events::String("goodwill"), events::Int(from), events::Int(to),
        events::Number(old_goodwill), events::Number(new_goodwill) };
    events::Emit(events::EBuiltin::RelationOnChanged, args, static_cast<u32>(std::size(args)));
}

void CommunityGoodwillChanged(int community, u16 to, int old_goodwill, int new_goodwill)
{
    const GwpValue args[] = { events::String("community_goodwill"), events::String(CharacterCommunityName(community)),
        events::Int(to), events::Number(old_goodwill), events::Number(new_goodwill) };
    events::Emit(events::EBuiltin::RelationOnChanged, args, static_cast<u32>(std::size(args)));
}

void CommunityRelationChanged(int from, int to, int old_goodwill, int new_goodwill)
{
    const GwpValue args[] = { events::String("community_relation"), events::String(CharacterCommunityName(from)),
        events::String(CharacterCommunityName(to)), events::Number(old_goodwill), events::Number(new_goodwill) };
    events::Emit(events::EBuiltin::RelationOnChanged, args, static_cast<u32>(std::size(args)));
}

void RelationsReset()
{
    const GwpValue args[] = { events::String("reset"), events::Nil(), events::Nil(), events::Nil(), events::Nil() };
    events::Emit(events::EBuiltin::RelationOnChanged, args, static_cast<u32>(std::size(args)));
}

void FillEngineApi(GwpEngineApi& api)
{
    api.character_money = &ApiCharacterMoney;
    api.character_give_money = &ApiCharacterGiveMoney;
    api.character_transfer_money = &ApiCharacterTransferMoney;
    api.character_rank = &ApiCharacterRank;
    api.character_reputation = &ApiCharacterReputation;
    api.character_name = &ApiCharacterName;
    api.character_set_rank = &ApiCharacterSetRank;
    api.character_set_reputation = &ApiCharacterSetReputation;
    api.character_set_name = &ApiCharacterSetName;
    api.character_set_community = &ApiCharacterSetCommunity;
    api.character_goodwill = &ApiCharacterGoodwill;
    api.character_set_goodwill = &ApiCharacterSetGoodwill;
    api.character_force_goodwill = &ApiCharacterForceGoodwill;
    api.character_attitude = &ApiCharacterAttitude;
    api.character_set_relation = &ApiCharacterSetRelation;
    api.community_exists = &ApiCommunityExists;
    api.community_goodwill = &ApiCommunityGoodwill;
    api.community_set_goodwill = &ApiCommunitySetGoodwill;
    api.community_change_goodwill = &ApiCommunityChangeGoodwill;
    api.community_relation = &ApiCommunityRelation;
    api.community_set_relation = &ApiCommunitySetRelation;
    api.creature_condition = &ApiCreatureCondition;
    api.creature_set_condition = &ApiCreatureSetCondition;
    api.creature_change_condition = &ApiCreatureChangeCondition;
}
} // namespace gw::addons::character
