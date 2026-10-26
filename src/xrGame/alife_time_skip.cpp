#include "StdAfx.h"

// plans/lua_to_cpp/55-timeskip-squad-movement.md: the step-by-step catch-up of the offline movement after a jump of the
// game clock. The design and the order of a step: alife_time_skip.h.

#include "alife_time_skip.h"

#include "addon_alife_hooks.h"
#include "addon_event_bus.h"
#include "ai_space.h"
#include "alife_graph_registry.h"
#include "alife_monster_detail_path_manager.h"
#include "alife_monster_movement_manager.h"
#include "alife_object_registry.h"
#include "alife_online_offline_group_brain.h"
#include "alife_schedule_registry.h"
#include "alife_switch_manager.h"
#include "alife_time_manager.h"
#include "alife_update_manager.h"
#include "npc_cpp_profile.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrServer_Objects_ALife_Monsters.h"

namespace alife_time_skip
{
namespace
{
// The settings (alife_time_skip.h, LoadSettings): [alife] time_skip_* of configs/alife.ltx
int g_step_s = 300;
int g_max_steps = 288; // a day of game time at 5 min
int g_tick_steps = 6;  // a tick step every 30 game minutes at 5 min (the decision of 2026-10-25, was 3)
int g_budget_ms = 0;   // the decision of 2026-10-25: all at once under the dark screen

// The state of the running catch-up (one ALife at a time; the game logic thread only)
struct TimeSkipState
{
    CALifeUpdateManager* alife = nullptr;
    bool active = false;
    bool begun = false;   // alife_on_time_skip_begin was sent
    bool running = false; // inside Run (a Lua handler of a step may call change_game_time or save)
    ALife::_TIME_ID from = 0;
    ALife::_TIME_ID to = 0;
    ALife::_TIME_ID cursor = 0;
    u64 step_ms = 0;
    u32 index = 0;
    u32 steps = 0;
    // by object id: kTimeSkipHeld - held at the online radius, kTimeSkipRefused - asked, may not come online
    xr_vector<u8> flags;
    xr_vector<ALife::_OBJECT_ID> held;
    xr_vector<ALife::_OBJECT_ID> ids;
    // the counters of this skip (the log line at its end)
    u64 qpc_total = 0;
    u64 qpc_step_max = 0;
    u32 frames = 0;
    u64 moved = 0;      // squad moves (a step that changed the game vertex of a squad)
    u64 retargeted = 0; // C++ updates of the squads on the tick steps
    u32 refused = 0;    // squads that reached the radius and may not come online
};

constexpr u8 kTimeSkipHeld = 1;
constexpr u8 kTimeSkipRefused = 2;
constexpr u32 kTimeSkipIdCount = 0x10000;
// update_spread: the squads just gone offline updated in one call at most (a wave near the actor is spread)
constexpr u32 kFreshOfflinePerCall = 4;
// review2 G-m6 (E7 M2): a head deferred this many times in a row is let go instead of retried again - see
// UpdateFreshOffline.
constexpr u32 kFreshOfflineMaxHeadDeferrals = 2;

TimeSkipState g_skip;
xr_vector<ALife::_OBJECT_ID> g_fresh_offline;
// review2 G-m6: how many times in a row the CURRENT head of g_fresh_offline has been deferred by the search
// budget. Reset whenever the head id changes (a new squad reaches the front, by a pop or a fresh push at an
// empty queue).
ALife::_OBJECT_ID g_fresh_offline_head_id = ALife::_OBJECT_ID(-1);
u32 g_fresh_offline_head_deferrals = 0;

double TimeSkipMsF(u64 qpc) { return static_cast<double>(qpc) * 1000.0 / static_cast<double>(CPU::qpc_freq); }

void TimeSkipEmit(gw::addons::events::EBuiltin id, u64 total_ms, u32 steps)
{
    const GwpValue args[] = { gw::addons::events::Number(static_cast<double>(total_ms)),
        gw::addons::events::Number(static_cast<double>(steps)) };
    gw::addons::events::Emit(id, args, 2);
}

// The steps the rest of the skip takes at the current step length (the merge of a later jump grows it)
u32 TimeSkipStepsNow()
{
    const u64 left = g_skip.to > g_skip.cursor ? g_skip.to - g_skip.cursor : 0;
    return g_skip.index + static_cast<u32>((left + g_skip.step_ms - 1) / g_skip.step_ms);
}

// CALifeMonsterDetailPathManager::s_vertex_hook while the steps run: the rule of the online radius (alife_time_skip.h,
// step 4). Called after every change of the game vertex of an offline object that walks
bool TimeSkipVertexHook(CSE_ALifeDynamicObject& object)
{
    if (!g_skip.alife)
        return false;
    u8& flags = g_skip.flags[object.ID];
    if (flags & kTimeSkipRefused)
        return false;
    if (flags & kTimeSkipHeld)
        return true;
    CSE_ALifeOnlineOfflineGroup* squad = smart_cast<CSE_ALifeOnlineOfflineGroup*>(&object);
    if (!squad)
        return false;
    CALifeUpdateManager& alife = *g_skip.alife;
    const CSE_ALifeCreatureActor* actor = alife.graph().actor();
    if (!actor || !ai().game_graph().valid_vertex_id(object.m_tGraphID) ||
        !ai().game_graph().valid_vertex_id(actor->m_tGraphID))
        return false;
    if (ai().game_graph().vertex(object.m_tGraphID)->level_id() !=
        ai().game_graph().vertex(actor->m_tGraphID)->level_id())
        return false;
    if (actor->o_Position.distance_to(object.o_Position) > alife.online_distance())
        return false;
    // In the radius: the Lua can_switch_online of the switch pass, once per squad and skip
    if (!alife_switch_profiled_can_online(squad))
    {
        flags |= kTimeSkipRefused;
        ++g_skip.refused;
        return false;
    }
    flags |= kTimeSkipHeld;
    g_skip.held.push_back(object.ID);
    if (npc_cpp_profile::enabled())
        npc_cpp_profile::count(ENpcCppProfileStage::AlifeTimeSkipHeld);
    return true;
}

void TimeSkipStep()
{
    CALifeUpdateManager& alife = *g_skip.alife;
    CALifeTimeManager& clock = alife.time_manager();
    const bool profile = npc_cpp_profile::enabled();
    const u64 start = CPU::QPC();

    const u64 left = g_skip.to - g_skip.cursor;
    const u64 step_ms = std::min(g_skip.step_ms, left);
    g_skip.cursor += step_ms;
    ++g_skip.index;
    clock.set_skip_cursor(g_skip.cursor);
    const u32 tick_every = static_cast<u32>(std::max(g_tick_steps, 1));
    const bool last = g_skip.cursor >= g_skip.to;
    const bool tick = last || g_skip.index % tick_every == 0;
    g_skip.steps = TimeSkipStepsNow();

    // The clock of the step (alife_time_skip.h): the updates and the event see the cursor as the game time
    const ALife::_TIME_ID clock_shift = clock.shift_clock_to(g_skip.cursor);

    // The ids first: an update may release objects (the delete waits for the lock) and Lua may create new ones
    g_skip.ids.clear();
    for (const auto& it : alife.scheduled().objects())
        g_skip.ids.push_back(it.first);
    {
        const CALifeSimulatorBase::destruction_lock lock(alife);
        if (tick)
        {
            const u64 tick_start = profile ? CPU::QPC() : 0;
            u64 retarget_moves = 0;
            for (const ALife::_OBJECT_ID id : g_skip.ids)
            {
                if (g_skip.flags[id] & kTimeSkipHeld)
                    continue;
                CSE_ALifeSchedulable* object = alife.scheduled().object(id, true);
                CSE_ALifeOnlineOfflineGroup* squad =
                    object ? smart_cast<CSE_ALifeOnlineOfflineGroup*>(object->base()) : nullptr;
                if (!squad || !squad->bfActive())
                    continue;
                const GameGraph::_GRAPH_ID vertex = squad->m_tGraphID;
                // The C++ update of the class (non-virtual, as the native path of addon_alife_hooks.cpp): the target of
                // get_current_task and the walk up to the cursor; no Lua body of the squad
                squad->CSE_ALifeOnlineOfflineGroup::update();
                ++g_skip.retargeted;
                if (squad->m_tGraphID != vertex)
                {
                    ++g_skip.moved;
                    ++retarget_moves;
                }
            }
            if (profile)
            {
                npc_cpp_profile::add(ENpcCppProfileStage::AlifeTimeSkipRetarget, CPU::QPC() - tick_start);
                npc_cpp_profile::count(ENpcCppProfileStage::AlifeTimeSkipRetargetMoves, retarget_moves);
            }
        }
        // 57 E0 (diagnostics only): /move split - /move/squad (update_position of the squads, timed one by one: 2 QPC a
        // squad), /move/other (the rest of the pass: the other scheduled objects and the loop itself); the counters
        // /move/squad_moves and /retarget/moves - the squad moves of each pass (their sum is /squads_moved)
        const u64 move_start = profile ? CPU::QPC() : 0;
        u64 squad_qpc = 0, squad_qpc_max = 0, squad_calls = 0, other_calls = 0, move_moves = 0;
        for (const ALife::_OBJECT_ID id : g_skip.ids)
        {
            if (g_skip.flags[id] & kTimeSkipHeld)
                continue;
            CSE_ALifeSchedulable* object = alife.scheduled().object(id, true);
            if (!object)
                continue;
            CSE_ALifeOnlineOfflineGroup* squad = smart_cast<CSE_ALifeOnlineOfflineGroup*>(object->base());
            const GameGraph::_GRAPH_ID vertex = squad ? squad->m_tGraphID : 0;
            if (profile && squad)
            {
                const u64 squad_start = CPU::QPC();
                object->update_position();
                const u64 spent = CPU::QPC() - squad_start;
                squad_qpc += spent;
                squad_qpc_max = std::max(squad_qpc_max, spent);
                ++squad_calls;
            }
            else
            {
                object->update_position();
                ++other_calls;
            }
            if (squad && squad->m_tGraphID != vertex)
            {
                ++g_skip.moved;
                ++move_moves;
            }
        }
        if (profile)
        {
            const u64 move_spent = CPU::QPC() - move_start;
            npc_cpp_profile::add(ENpcCppProfileStage::AlifeTimeSkipMove, move_spent);
            npc_cpp_profile::add_many(ENpcCppProfileStage::AlifeTimeSkipMoveSquad, squad_qpc, squad_calls, squad_qpc_max);
            npc_cpp_profile::add_many(ENpcCppProfileStage::AlifeTimeSkipMoveOther,
                move_spent > squad_qpc ? move_spent - squad_qpc : 0, other_calls, 0);
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeTimeSkipMoveSquadMoves, move_moves);
        }
    }

    // The bus: the plugins and Lua see the positions of this step (outside the passes over the registries: spawn and
    // release are allowed)
    if (gw::addons::events::HasSubscribers(gw::addons::events::EBuiltin::AlifeOnTimeSkipStep))
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AlifeTimeSkipEvent);
        const GwpValue args[] = { gw::addons::events::Number(static_cast<double>(step_ms)),
            gw::addons::events::Number(static_cast<double>(g_skip.index)),
            gw::addons::events::Number(static_cast<double>(g_skip.steps)), gw::addons::events::Bool(tick) };
        gw::addons::events::Emit(gw::addons::events::EBuiltin::AlifeOnTimeSkipStep, args, 4);
    }
    clock.shift_clock_back(clock_shift);

    const u64 spent = CPU::QPC() - start;
    g_skip.qpc_total += spent;
    g_skip.qpc_step_max = std::max(g_skip.qpc_step_max, spent);
    if (profile)
    {
        npc_cpp_profile::add(ENpcCppProfileStage::AlifeTimeSkipStep, spent);
        npc_cpp_profile::count(ENpcCppProfileStage::AlifeTimeSkipSteps);
    }
}

void TimeSkipEnd(pcstr reason)
{
    CALifeUpdateManager& alife = *g_skip.alife;
    CALifeTimeManager& clock = alife.time_manager();
    CALifeMonsterDetailPathManager::s_vertex_hook = nullptr;
    clock.set_skip_cursor(0);
    // The held squads walked nothing after their stop: the rest of the skip is not walked later either
    const ALife::_TIME_ID now = clock.movement_time();
    for (const ALife::_OBJECT_ID id : g_skip.held)
    {
        CSE_ALifeSchedulable* object = alife.scheduled().object(id, true); // offline squads are scheduled
        CSE_ALifeOnlineOfflineGroup* squad = object ? smart_cast<CSE_ALifeOnlineOfflineGroup*>(object->base()) : nullptr;
        if (squad && !squad->m_bOnline)
            squad->brain().movement().detail().catch_up_to(now);
    }
    const u64 total_ms = g_skip.to - g_skip.from;
    const u32 steps = g_skip.index;
    g_skip.active = false;
    g_skip.begun = false;
    TimeSkipEmit(gw::addons::events::EBuiltin::AlifeOnTimeSkipEnd, total_ms, steps);
    Msg("* [alife] time skip %.1f h (%s): %u steps of %u s, %.1f ms in %u frame(s), the longest step %.2f ms; squad "
        "moves %llu, squad updates %llu, held at the online radius %u, may not come online %u",
        static_cast<double>(total_ms) / 3600000.0, reason, steps, static_cast<u32>(g_skip.step_ms / 1000),
        TimeSkipMsF(g_skip.qpc_total), g_skip.frames, TimeSkipMsF(g_skip.qpc_step_max),
        static_cast<unsigned long long>(g_skip.moved), static_cast<unsigned long long>(g_skip.retargeted),
        static_cast<u32>(g_skip.held.size()), g_skip.refused);
    if (npc_cpp_profile::enabled())
    {
        npc_cpp_profile::add(ENpcCppProfileStage::AlifeTimeSkip, g_skip.qpc_total);
        npc_cpp_profile::count(ENpcCppProfileStage::AlifeTimeSkipSquadsMoved, g_skip.moved);
    }
    g_skip.held.clear();
}

// The steps until the end or until `deadline` (CPU::QPC; 0 - no limit)
void TimeSkipRun(u64 deadline, pcstr reason)
{
    if (!g_skip.active || g_skip.running || !g_skip.alife)
        return;
    g_skip.running = true;
    ++g_skip.frames;
    if (!g_skip.begun)
    {
        // The step: time_skip_step_s, longer when the skip would take more than time_skip_max_steps
        const u64 total = g_skip.to - g_skip.from;
        const u64 max_steps = static_cast<u64>(std::max(g_max_steps, 1));
        g_skip.step_ms = static_cast<u64>(std::max(g_step_s, 1)) * 1000;
        if ((total + g_skip.step_ms - 1) / g_skip.step_ms > max_steps)
            g_skip.step_ms = (total + max_steps - 1) / max_steps;
        g_skip.steps = TimeSkipStepsNow();
        g_skip.begun = true;
        CALifeMonsterDetailPathManager::s_vertex_hook = &TimeSkipVertexHook;
        TimeSkipEmit(gw::addons::events::EBuiltin::AlifeOnTimeSkipBegin, total, g_skip.steps);
    }
    while (g_skip.active && g_skip.cursor < g_skip.to)
    {
        TimeSkipStep();
        if (deadline && CPU::QPC() >= deadline)
            break;
    }
    g_skip.running = false;
    if (g_skip.active && g_skip.cursor >= g_skip.to)
        TimeSkipEnd(reason);
}
} // namespace

void LoadSettings(pcstr section)
{
    g_step_s = std::clamp(pSettings->read_if_exists<int>(section, "time_skip_step_s", 300), 0, 86400);
    g_max_steps = std::clamp(pSettings->read_if_exists<int>(section, "time_skip_max_steps", 288), 1, 100000);
    g_tick_steps = std::clamp(pSettings->read_if_exists<int>(section, "time_skip_tick_steps", 6), 1, 1000);
    g_budget_ms = std::clamp(pSettings->read_if_exists<int>(section, "time_skip_budget_ms", 0), 0, 10000);
    if (g_step_s > 0)
    {
        Msg("* [alife] time skip settings: a step of %d s, at most %d steps, a tick step every %d, %d ms a frame "
            "(0 - all in one frame)", g_step_s, g_max_steps, g_tick_steps, g_budget_ms);
    }
    else
        Msg("* [alife] time skip settings: time_skip_step_s 0 - no catch-up, the single jump of the clock");
}

void Begin(CALifeUpdateManager& alife, u64 ms)
{
    if (!ms)
        return;
    CALifeTimeManager& clock = alife.time_manager();
    if (g_skip.active && g_skip.alife == &alife)
    {
        // Another jump while one is caught up: the same catch-up goes further
        g_skip.to += ms;
        if (g_skip.begun)
            g_skip.steps = TimeSkipStepsNow();
        return;
    }
    if (g_step_s <= 0 || ms <= static_cast<u64>(g_step_s) * 1000)
        return; // the emergency path (time_skip_step_s 0), or a jump of one step: the single jump as before
    g_skip = TimeSkipState{};
    g_skip.alife = &alife;
    g_skip.active = true;
    g_skip.to = clock.game_time(); // the clock has jumped already
    g_skip.from = g_skip.to - ms;
    g_skip.cursor = g_skip.from;
    g_skip.flags.assign(kTimeSkipIdCount, 0);
    clock.set_skip_cursor(g_skip.cursor);
}

void Finish(CALifeUpdateManager& alife, pcstr reason)
{
    if (g_skip.active && g_skip.alife == &alife)
        TimeSkipRun(0, reason);
}

void Frame(CALifeUpdateManager& alife)
{
    if (!g_skip.active || g_skip.alife != &alife)
        return;
    // time_skip_step_s 0 (a load reads the settings again): the rest at once, as a save does
    const u64 deadline = g_budget_ms > 0 && g_step_s > 0 ?
        CPU::QPC() + static_cast<u64>(g_budget_ms) * CPU::qpc_freq / 1000 :
        0;
    TimeSkipRun(deadline, "frame");
}

void Drop(CALifeUpdateManager& alife)
{
    g_fresh_offline.clear();
    g_fresh_offline_head_id = ALife::_OBJECT_ID(-1);
    g_fresh_offline_head_deferrals = 0;
    if (!g_skip.alife || g_skip.alife != &alife)
        return;
    if (g_skip.active)
    {
        Msg("* [alife] time skip: the end of ALife drops the rest (%u of %u steps done)", g_skip.index, g_skip.steps);
        alife.time_manager().set_skip_cursor(0);
        CALifeMonsterDetailPathManager::s_vertex_hook = nullptr;
    }
    g_skip = TimeSkipState{};
}

bool Pending() { return g_skip.active; }

void NoteFreshOffline(ALife::_OBJECT_ID id) { g_fresh_offline.push_back(id); }

bool FreshOfflinePending() { return !g_fresh_offline.empty(); }

void UpdateFreshOffline(CALifeUpdateManager& alife)
{
    if (g_fresh_offline.empty())
        return;
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AlifeFreshOfflineUpdate);
    const CALifeSimulatorBase::destruction_lock lock(alife);
    u32 done = 0;
    while (!g_fresh_offline.empty() && done < kFreshOfflinePerCall)
    {
        const ALife::_OBJECT_ID id = g_fresh_offline.front();
        if (id != g_fresh_offline_head_id)
        {
            g_fresh_offline_head_id = id;
            g_fresh_offline_head_deferrals = 0;
        }
        CSE_ALifeSchedulable* object = alife.scheduled().object(id, true);
        CSE_ALifeOnlineOfflineGroup* squad = object ? smart_cast<CSE_ALifeOnlineOfflineGroup*>(object->base()) : nullptr;
        if (!squad || squad->m_bOnline)
        {
            g_fresh_offline.erase(g_fresh_offline.begin());
            continue;
        }
        // 63-wave3-fixes.md M2: the id leaves the fresh queue only after the update. When the search budget defers
        // the squad's search, the id stays at the front and the batch stops - the next frame retries with a fresh
        // budget (plan 55 E2: "the squad that has just gone offline goes at once"). Before, the id was dropped here
        // and a deferred search fell back to the regular scheduler turn (~1-1.5 s), losing the immediate update.
        // review2 G-m6: "this batch is the only budget spender before update_spread" does not hold - Lua
        // (bind_monster.script, xr_motivator.script, xr_effects.script call squad:update()) and plugin hooks can
        // spend the same per-frame search budget earlier in the very same frame (spread_frame runs at the end of
        // Level::OnFrame). When that happens the head keeps re-running its own full object->update() (the whole
        // Lua squad body) every frame while every other id behind it in the queue never gets a turn. After
        // kFreshOfflineMaxHeadDeferrals misses in a row this head is let go instead: it drops out of the "goes at
        // once" fast path and falls back to the regular scheduler turn (~1-1.5 s, same as before plan 55 E2 ever
        // existed), and the rest of this frame's batch budget goes to the ids behind it.
        const u32 deferred_before = alife_offline_path_search_deferred_total();
        if (!gw::addons::alife_hooks::g_active || !gw::addons::alife_hooks::Update(*object))
            object->update();
        ++done;
        if (alife_offline_path_search_deferred_total() != deferred_before)
        {
            if (++g_fresh_offline_head_deferrals < kFreshOfflineMaxHeadDeferrals)
                break;
            g_fresh_offline.erase(g_fresh_offline.begin());
            g_fresh_offline_head_id = ALife::_OBJECT_ID(-1);
            g_fresh_offline_head_deferrals = 0;
            continue;
        }
        g_fresh_offline.erase(g_fresh_offline.begin());
    }
}
} // namespace alife_time_skip
