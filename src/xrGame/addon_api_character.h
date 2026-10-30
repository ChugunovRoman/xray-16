#pragma once

// Plugin API, group "character" (addon_api_character.cpp): money, rank, reputation, name, community, goodwill and
// community relations, the characteristics of creatures; the engine side of the events npc_on_before_hit,
// monster_on_before_hit and relation_on_changed.
// Docs: wiki/doc/plugins/api/character.md

#include "xrAddonHost/include/gwp/gwp_api.h"

class CEntityAlive;
struct SHit;

namespace gw::addons::character
{
void FillEngineApi(GwpEngineApi& api);

// npc_on_before_hit / monster_on_before_hit: called by CAI_Stalker::Hit / CBaseMonster::Hit for a living victim,
// before the engine works on the hit. The subscribers may change hit.power; false = the hit is cancelled (the
// caller returns at once). Without subscribers: true and nothing else.
bool BeforeHit(CEntityAlive* victim, SHit& hit, bool monster);

// relation_on_changed, from the setters of RELATION_REGISTRY (relation_registry.cpp). WantsRelationEvents tells the
// setter whether to read the old value at all (an emit without subscribers is skipped).
bool WantsRelationEvents();
void GoodwillChanged(u16 from, u16 to, int old_goodwill, int new_goodwill);
void CommunityGoodwillChanged(int community, u16 to, int old_goodwill, int new_goodwill);
void CommunityRelationChanged(int from, int to, int old_goodwill, int new_goodwill);
void RelationsReset();

// relation_on_before_action, from RELATION_REGISTRY::Action (relation_registry_actions.cpp) right before it changes
// the goodwill of the group of `to` toward `from`. action: "attack", "kill", "fight_help_human", "fight_help_monster".
// The subscribers may write another delta; false = no change of goodwill (the reputation and rank still change).
// Without subscribers: true and delta as it was.
bool BeforeRelationAction(pcstr action, u16 from, u16 to, int relation, int& delta, bool danger, int hit_type,
    float health_lost);
} // namespace gw::addons::character
