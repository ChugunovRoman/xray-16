#pragma once

// Plugin API, groups npc_body, npc_sight, npc_anim, inventory, smart_cover (addon_api_npc_control.cpp): what a
// native plugin needs to drive a stalker the way the Lua state manager does. This header holds only the hooks the
// engine calls; the API functions are reached through GwpEngineApi.

class CAI_Stalker;

namespace gw::addons::npcctl
{
// A script animation of the stalker ended (CStalkerAnimationManager::play_delayed_callbacks, right after the Lua
// callback.script_animation of the NPC). Emits npc_on_script_animation_end(npc, remaining).
void ScriptAnimationEnd(CAI_Stalker& npc);

// The event above has a subscriber: the engine then updates the animation tracks of every stalker with queued
// script animations, as it does for an NPC with a Lua script_animation callback (otherwise an animation of an
// invisible NPC would not end until it is seen).
bool WantsScriptAnimationEnd();

// The patrol path manager of an NPC reached a point of its path (CPatrolPathManager::select_point, right after the
// Lua callback.patrol_path_in_point of the object). Emits npc_on_patrol_point(npc, action_type, point) when the
// event has a subscriber.
void PatrolPoint(u16 npc, u32 action_type, u32 point);

// CPatrolPathManager::extrapolate_path - the NPC asks whether to go on past the point (before the Lua extrapolate
// callback of the object answers). Emits npc_on_patrol_extrapolate(npc, point) when the event has a subscriber:
// an observer only, the answer stays with the Lua callback and patrol_extrapolate_register.
void PatrolExtrapolate(u16 npc, u32 point);
} // namespace gw::addons::npcctl
