#pragma once

// Plugin API: the world (group world, addon_api_world.cpp): the game graph and the levels, moving online objects,
// the direction of the actor, explosions, weather, writing the game time.
// Docs: wiki/doc/plugins/api/world.md

#include "xrAddonHost/include/gwp/gwp_api.h"

namespace gw::addons
{
void FillWorldApi(GwpEngineApi& api);

// engine gaps P11 (plans/addon_api_ideas/spec.md): the weather lock. level_weathers.script applies the locked cycle
// instead of its own pick (level.weather_locked). One lock at a time; another owner is refused.
//  - LuaWeatherLock / LuaWeatherUnlock: level.weather_lock(name, forced[, owner]) / weather_unlock([owner]) of Lua,
//    owner "lua" by default; false (logged) on a refusal;
//  - WeatherLockedCycle / WeatherLockOwner: the lock now, nullptr without one;
//  - WeatherLockOnGameStart: ALife starts (alife_simulator.cpp) - a new game or a load drops the lock, a level change
//    keeps it;
//  - ReleasePluginWeatherLock: the plugin stops (addon_host.cpp StopPlugin) - its lock goes.
bool LuaWeatherLock(pcstr name, bool forced, pcstr owner);
bool LuaWeatherUnlock(pcstr owner);
pcstr WeatherLockedCycle();
pcstr WeatherLockOwner();
void WeatherLockOnGameStart(bool level_change);
void ReleasePluginWeatherLock(const GwpPlugin* plugin);
} // namespace gw::addons
