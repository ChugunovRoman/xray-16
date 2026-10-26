#pragma once

// plans/lua_to_cpp/55-timeskip-squad-movement.md: the step-by-step catch-up of the offline movement after a jump of the
// game clock (sleep, the anabiotic, fast travel, level.change_game_time, Plugin API game_time_advance).
//
// Before: the clock jumped (m_game_time += value) and the first pass of the positions after it walked a whole route in
// one call (follow_path of the whole skip): the squads were at their targets at once, the contact scanner of gw_eoc saw
// a "teleport" and checked only the end point, no fight happened on the way, and a squad aimed at a smart near the
// actor came online in its center.
// Now the clock still jumps at once (Lua reads the new time right after change_game_time), but the offline movement
// runs on CALifeTimeManager::movement_time(): the cursor of the skip, which the catch-up moves step by step
// (time_skip_step_s of game time, 300 by default). A step:
//   1. the cursor moves by the step;
//   2. every time_skip_tick_steps-th step (and the last one) - "a tick step": the C++ update of every offline
//      squad (CSE_ALifeOnlineOfflineGroup::update, non-virtual: brain().update() with the Lua get_current_task - the
//      target - and the walk), so the targets the plugins and Lua changed on the earlier steps are followed;
//   3. the positions of every scheduled object up to the cursor (update_position, no Lua);
//   4. the rule of the online radius: a squad that steps on a game vertex on the level of the actor closer than
//      online_distance stops there for the rest of the skip when it may come online (can_switch_online, the Lua call
//      of the switch pass - only at the first such vertex of a squad); the switch pass brings it online there, at the
//      edge of the radius, after the skip. A squad that may not come online (day / night monsters, excl_dist) goes on
//      as before;
//   5. the bus event alife_on_time_skip_step(step_ms, index, steps, tick) - synchronous, the positions are those of
//      the step; the gw_eoc plugin runs its contacts, its fights and (tick) the native ticks of the squads and smarts,
//      Lua applies the records at once.
// The clock of a step (always on; its switch alife_time_skip_step_clock removed on 2026-10-25): during the updates of
// step 2 and the event of step 5 the game clock shows the cursor of the step (game_time() of the engine,
// game.get_game_time() of Lua, the game time of the Plugin API), then it gets its time again. So what the ticks do by
// the game time - the arrival time and the wait of a squad, the spawn timers of a smart, get_current_task - goes as it
// would in a game of the same length: a squad may arrive, wait and go on within one sleep, a smart respawns by its
// periods.
// Around the steps: alife_on_time_skip_begin(total_ms, steps) before the first one, alife_on_time_skip_end(total_ms,
// steps) after the last one.
// When: all steps at once in CLevel::OnGameLogicFrame of the next frame, before level_on_frame (the screen is dark
// after the sleep; one hitch there - the decision of 2026-10-25), or in parts of time_skip_budget_ms per frame when
// that is > 0. A save, a level change (fast travel calls ChangeLevel right after change_game_time) finish the rest at
// once before they go on. The end of ALife (exit to the menu, a load, quit) drops the rest: nothing of it can be seen
// or saved any more.
// The settings: configs/alife.ltx, [alife] time_skip_* (LoadSettings; the console commands alife_time_skip_* were
// removed on 2026-10-25). The emergency path: time_skip_step_s = 0 - the single jump as before.
// Main (game logic) thread only.

#include "alife_space.h"

class CALifeUpdateManager;

namespace alife_time_skip
{
// The settings of [alife] in configs/alife.ltx (LoadSettings; the default when a line is missing):
//   time_skip_step_s     (300) - the game seconds of a step; 0 - no catch-up (the old single jump, the emergency path)
//   time_skip_max_steps  (288) - at most this many steps (a longer skip takes longer steps)
//   time_skip_tick_steps (6)   - every N-th step is a tick step (1 - every step; 6 - every 30 game minutes at 300 s)
//   time_skip_budget_ms  (0)   - 0: the whole catch-up in one frame; N > 0: N ms a frame
// CALifeUpdateManager::reload (a new game, every load); one line in the log
void LoadSettings(pcstr section);

// The clock of `alife` has just jumped by `ms` (change_game_time, game_time_advance): the catch-up starts, or a
// running one gets longer
void Begin(CALifeUpdateManager& alife, u64 ms);
// The rest of the catch-up now (a save, a level change); `reason` goes to the log line
void Finish(CALifeUpdateManager& alife, pcstr reason);
// CLevel::OnGameLogicFrame, before level_on_frame
void Frame(CALifeUpdateManager& alife);
// The end of ALife: the rest is dropped, the cursor goes
void Drop(CALifeUpdateManager& alife);
bool Pending();

// Plan 55, E2: a squad has just gone offline (CSE_ALifeOnlineOfflineGroup::switch_offline): its scheduled update comes
// in the next part of the ALife update (CALifeUpdateManager::update_spread) instead of at its turn (22-410 s later),
// so it gets its target and walks on at once instead of standing there
void NoteFreshOffline(ALife::_OBJECT_ID id);
// update_spread: the scheduled updates of the squads NoteFreshOffline queued, at most a few a call
void UpdateFreshOffline(CALifeUpdateManager& alife);
bool FreshOfflinePending();
} // namespace alife_time_skip
