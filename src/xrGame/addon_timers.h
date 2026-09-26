#pragma once

// Plugin timers: native analogue of the Lua CreateTimeEvent queue and of game-time timers such as level_timers.
//  - A timer belongs to an addon and has a name unique inside it; starting a timer with the same name replaces it.
//  - Time base: game time (ALife, scaled by the time factor, default) or level time (real milliseconds of
//    unpaused frames, GWP_TIMER_REAL_TIME). Both stop on pause and outside a level.
//  - When the timer expires the engine emits the event chosen by the owner with two arguments: the timer name and
//    its key "<owner>/<name>". An event, not a callback: a persistent timer comes back from a save, where neither
//    a native pointer nor a Lua function survives.
//  - Persistent timers (GWP_TIMER_PERSISTENT) go into the game save (own chunk of the ALife save stream).
//    A new game or a load drops all timers (like the Lua queue, which lives in the restarted Lua state),
//    then the load restores the persistent ones.
//  - Plugin timers of addons without a loaded plugin are kept (and saved) but never fire. Lua timers always fire.
// Lua: global table timer_bus (keys "<owner>/<name>", any owner). Plugins: timer_* functions of GwpEngineApi
// (xrAddonHost/include/gwp/gwp_api.h), the owner is the addon id.
// Main thread only. Docs: wiki/doc/plugins/api/timers.md; plan: plans/lua_to_cpp/04-plugin-api-events-and-data.md

#include "xrAddonHost/include/gwp/gwp_api.h"

class IReader;
class IWriter;

namespace gw::addons::timers
{
void FillEngineApi(GwpEngineApi& api);

void Update(u32 dt_ms); // every frame of an active, unpaused level (CLevel::OnFrame); dt_ms: real frame time

// While a save is being loaded the game time of ALife is not the one of the save yet: game-time timers cannot be
// started (timer_start fails with a message). Set by CALifeStorageManager::load around the time manager load.
void SetLoadingGameTime(bool loading);

void Reset();                    // new game or load: drops every timer
void WriteSave(IWriter& stream); // own chunk of the ALife save stream: persistent timers
void ReadSave(IReader& stream);  // restores the persistent timers; a save without the chunk gives none

void Shutdown();
void PrintList(); // console command "timer_list"
} // namespace gw::addons::timers
