#pragma once

// Plugin API, group "callbacks": native voices in two yes/no decisions of the engine that Lua takes part in through
// one slot per object (set_enemy_callback, set_patrol_extrapolate_callback). Wave W0 (W0.2 of the plan).
//  - Registered per plugin, asked for every NPC: nothing is stored in the object, so reinit() (which clears the Lua
//    slots at every spawn) does not touch them.
//  - The Lua callback of the object is asked first; the answer is "yes" only when it and every native one say yes.
//  - A crash in a callback removes every callback of that plugin; the decision is taken as if it had said yes.
// Per-object event subscriptions (the other half of W0.2) are a flag of the event bus: GWP_SUBSCRIBE_OBJECT.
// Docs: wiki/doc/plugins/api/callbacks.md; plan: plans/lua_to_cpp/08-wave-w0-native-npc-foundation.md (W0.2)

#include "xrAddonHost/include/gwp/gwp_api.h"

class CCustomMonster;
class CEntityAlive;
struct GwpPlugin;

namespace gw::addons::callbacks
{
void FillEngineApi(GwpEngineApi& api);

bool HasEnemyFilters();
// CEnemyManager::useful, after the checks of the engine and the Lua callback said yes.
bool EnemyUseful(const CCustomMonster* npc, const CEntityAlive* enemy);

bool HasPatrolExtrapolate();
// CPatrolPathManager::extrapolate_path, after the Lua callback (or without one).
bool PatrolExtrapolate(u16 npc, u32 point_index);

void RemovePluginCallbacks(const GwpPlugin* plugin);
void Shutdown();
void PrintList(); // console command "callback_list"
} // namespace gw::addons::callbacks
