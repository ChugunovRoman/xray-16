#include "stdafx.h"
#include "xrSheduler.h"
#include "xr_object.h"
#include "Feel_Touch.h"
#include "GameFont.h"
#include "PerformanceAlert.hpp"
#include "Include/xrRender/RenderVisual.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <tracy/Tracy.hpp>

//#define DEBUG_SCHEDULER
//#define DEBUG_SCHEDULERMT

// The time budget of the non-RT pass for the stats only (stats.Load, shed/ps_current): psShedulerTarget is set to
// shed_budget_ms every pass and psShedulerCurrent follows it smoothly. The pass itself uses shed_budget_ms directly.
float psShedulerCurrent = 10.f;
float psShedulerTarget = 10.f;
bool isSheduleInProgress = false;

// The former cap of the normal-priority shedule_Update calls a frame. No longer read: the pass is limited by the
// time budget shed_budget_ms (and the safety cap shed_max_steps) below. The console command is kept for the saved
// configs and the options menu (ui_mm_opt_optimizations.script).
ENGINE_API int ps_scheduler_max_steps_per_frame = 128;

// 22-game-thread.md, step 4 (plans/optimization_mt/22-game-thread.md): near objects first and a real time budget.
//
// Why. With ~1000 NPCs around the actor the former non-RT pass (one heap, a limit of scheduler_max_steps_per_frame
// steps (128) and the adaptive budget psShedulerCurrent) always stopped by the step limit, never by time: the adaptive
// budget grew by 0.3 on every limit exit too and sat at its cap of 65 ms, so it limited nothing. The heap is ordered
// by the due time only, ~92 % of its 14-16k items were overdue and the pass became a carousel: every object, the
// stalker next to the player as well as a can on the other side of the level, got its turn once in 5-14 s.
// shedule_Scale() (the distance stretch of the interval, CGameObject: dist / 200) does not help under such a load: it
// only moves the next due time between t_min and (1000 + t_max) / 2, while the queue is seconds behind - a near
// stalker requeued at "now + 100 ms" still waits behind every overdue item.
//
// The pass:
//  - two heaps: ItemsNear and Items (the rest, the "far" heap). Near are only the objects whose own update matters for
//    how fast the world around the player reacts (IsNear, by the kind of the object, ShedNear*): living stalkers and
//    monsters and the vehicles within shed_near_dist m of the camera, or within shed_near_seen_dist m and drawn
//    recently; touch triggers (anomalies, script zones, level changers) whose bounds are within shed_near_dist m;
//    grenades and other explosives out in the world (their fuse runs in shedule_Update) and particles within
//    shed_near_seen_dist m. Items, weapons, props, inventory boxes and corpses - the bulk of the
//    objects around a crowd (one session: 10.7k of 18k items near, the near stalkers got lost among them) - are near
//    only within shed_near_item_dist m of the camera or in the actor's inventory. The services that are neither game
//    objects nor spatial (ALife, agent managers, autosave) are always far. The class is
//    taken when an item is registered and every time it is requeued after its update; a far item that came near is
//    found by a round-robin scan of shed_scan far items a frame (PromoteNearFromFar), a near item that went far by the
//    scan of the near heap (DemoteFarFromNear). Once a level is loaded every item is classified anew (RebuildNearFar,
//    see ProcessStep). Both are plain heaps by the due time, so t_min / t_max / shedule_Scale keep their meaning
//    inside a class;
//  - a fixed time budget shed_budget_ms for the whole pass (psShedulerCurrent only follows it, for the stats).
//    The near heap is served first, up to (100 - shed_far_share) % of the budget while the far heap has due items, and
//    at least shed_near_min_steps steps even past it; the far heap gets the rest of the budget and at least one step
//    a frame. The far heap is ordered by the due time (oldest first), so with one step a frame it still moves on and
//    nothing starves (the aging); a far object is late by "far backlog / far throughput" seconds;
//  - shed_max_steps: only a safety cap (512); the former step limit (scheduler_max_steps_per_frame) is not used -
//    with a real time budget the cheap items (~1 us) no longer have to be limited by count.
//
// DT. shedule_Update gets min(real elapsed, max(t_max, 1000)) ms - kept on purpose: shedule_Update code
// integrates with dt, e.g. the visual memory of an NPC (CCustomMonster / CAI_Stalker -> memory().update(dt) ->
// CVisualMemoryManager::get_visible_value: += time_delta / time_quant): a 5-14 s dt of an object late in the queue
// would make it "see" at once whom it should notice gradually. A near object is updated close to its interval now and
// does not hit the clamp; shed/dt_clamped and shed/dt_lost_ms show how much time is cut.
ENGINE_API int ps_shed_budget_ms = 8;
ENGINE_API int ps_shed_near_dist = 30;
ENGINE_API int ps_shed_near_seen_dist = 60;
ENGINE_API int ps_shed_near_item_dist = 5;
ENGINE_API int ps_shed_far_share = 25;
ENGINE_API int ps_shed_near_min_steps = 4;
ENGINE_API int ps_shed_max_steps = 512;
ENGINE_API int ps_shed_scan = 512;

// 22-game-thread.md, step 4c: the adaptive near radius (permanent). In a crowd of
// ~1000 NPCs around the actor nearly all of them are within shed_near_dist / shed_near_seen_dist: 600 living near
// creatures share the near part of the budget (~8 stalker steps a frame) and are still 5-8 s late. A fixed smaller
// radius fits the crowd but starves a normal scene. So the radius of the living creatures is scaled: a controller looks
// at the count of the living creatures in the near heap at most every ShedAdaptPeriodMs and multiplies both radii by
// 0.9 (0.8 far over the band) while the count is above shed_near_target + 25 %, by 1.05 (1.15 far under it) while it is
// below shed_near_target - 25 %, between shed_near_dist_min m for the plain radius and the configured values (the
// maxima). A step is taken only once the scan that shows its effect has gone over its whole heap since the last change
// (the near heap for a shrink: DemoteFarFromNear; the far heap for a growth: PromoteNearFromFar), otherwise the count
// would lag and the radius would overshoot. Only the living creatures follow the radius: vehicles, explosives, touch
// triggers, particles, items and the actor keep their own fixed rules.
ENGINE_API int ps_shed_near_target = 80;
ENGINE_API int ps_shed_near_dist_min = 8;

// 22-game-thread.md, step 4d (24-critical-path.md, the journal of 2026-10-15): inside the near class those who have to
// react go first (the urgent near heap; always on: in the A/B of a heavy fight the urgent near creatures were late
// 4.6 s -> 2.1 s, the calm near ones 0.79 -> 0.86 s, the far ones 8.4 -> 9.0 s). In a heavy fight the near heap holds
// up to ~370 living creatures, a stalker update costs 0.5-2.3 ms and only 5-11 stalker updates fit into a frame: a near
// stalker that is shot at would wait behind the calm ones by its due time, 0.1-6 s. So the near class is kept in two
// heaps: ItemsUrgent - the living creatures whose ISheduled::shedule_Urgent() is true (CAI_Stalker: its
// combat or danger planner is active, an enemy is selected, or it was hit within shed_urgent_hit_ms ms; CBaseMonster: an
// enemy or a recent hit) - and ItemsNear, the rest of the near class. The urgency of an item is taken only when it moves
// between the heaps: the requeue after its own update (its state is fresh then), the round-robin scans of the near heaps
// (DemoteFarFromNear moves a calm one that became urgent - a stalker hit while it waits is found in a few frames -,
// ScanUrgent a calm one back) and the promotion of a far one; never while it is inside a heap, the keys of a heap do not
// change. Both heaps are by the due time, so an urgent object never runs before its interval (t_min / t_max /
// shedule_Scale as before): only the order of the due ones changes, no update is added.
// The pass: the urgent heap first, up to shed_urgent_share % of the near budget while the calm near heap has due items
// (the whole near budget otherwise); then the calm near heap up to the near budget and at least one step a frame when the
// urgent heap took any (it is served oldest first, so it always moves on - the aging of the far heap); then the far heap
// as before. shed_near_min_steps holds for the two near heaps together.
ENGINE_API int ps_shed_urgent_share = 70;
ENGINE_API int ps_shed_urgent_hit_ms = 3000;

extern ENGINE_API float g_fov; // unzoomed camera fov (console "fov"), xr_ioc_cmd.cpp

namespace
{
enum : u32
{
    ShedCatOther = 0,
    ShedCatStalker,
    ShedCatMonster,
    ShedCatActor,
    ShedCatItem,
};

u32 ShedCategoryOf(IGameObject* go) // go: dynamic_cast<IGameObject*> of the scheduled object, may be nullptr
{
    if (!go)
        return ShedCatOther;
    if (go->cast_stalker())
        return ShedCatStalker;
    if (go->cast_base_monster())
        return ShedCatMonster;
    if (go->cast_actor())
        return ShedCatActor;
    if (go->cast_inventory_item())
        return ShedCatItem;
    return ShedCatOther;
}

// Step 4: what a scheduled object is for the near/far class of the scheduler (Item::NearKind, see IsNear). Taken once
// at internal_Register by the cheap cast_* virtuals of IGameObject (xrEngine does not know the game classes); the state
// that changes (alive or dead, the position, the owner) is read in IsNear
enum : u32
{
    ShedNearCreature = 0, // a stalker or a monster (CCustomMonster): by the distance rules while alive, a corpse is an item
    ShedNearActor, // the actor (normally in ItemsRT, but if it is in the normal queue): always near
    ShedNearMachine, // a vehicle or a mounted gun (CHolderCustom), a helicopter (CEntity, not alive): the distance rules
    ShedNearExplosive, // a grenade, a rocket, an explosive barrel (CExplosive): in the world by a wider distance, its fuse
                       // runs in shedule_Update (CMissile, CExplosiveItem); in an inventory - an item
    ShedNearTrigger, // a touch trigger (Feel::Touch: anomaly, script zone, level changer): by the distance of its bounds
    ShedNearItem, // any other game object (items, weapons, props, boxes, lamps, crows, traders): only very close
    ShedNearParticles, // spatial but not a game object (CPS_Instance)
    ShedNearService, // neither a game object nor spatial (ALife, agent managers, autosave)
};

u32 ShedNearKindOf(IGameObject* go, ISpatial* spatial)
{
    if (!go)
        return spatial ? ShedNearParticles : ShedNearService;
    if (go->cast_custom_monster())
        return ShedNearCreature;
    if (go->cast_actor())
        return ShedNearActor;
    // Before the explosive test: a car and a helicopter are CExplosive too
    if (go->cast_holder_custom() || (go->cast_entity() && !go->cast_entity_alive()))
        return ShedNearMachine;
    if (go->cast_explosive())
        return ShedNearExplosive;
    // Before the touch test: a PDA and some artefacts are Feel::Touch too
    if (go->cast_inventory_item())
        return ShedNearItem;
    // Its shedule_Update runs feel_touch_update: who entered (the actor too) is found only there, a far anomaly or
    // level changer would notice the player seconds late
    if (dynamic_cast<Feel::Touch*>(go))
        return ShedNearTrigger;
    return ShedNearItem;
}

// A stalker or a monster is alive: CGameObject::Load clears STYPE_REACTTOSOUND, CCustomMonster::net_Spawn sets it only
// for a living one and CEntityAlive::Die clears it - the same flag that keeps the dead out of the sound queries
bool ShedAlive(IGameObject& go)
{
    return (go.GetSpatialData().type.load(std::memory_order_relaxed) & STYPE_REACTTOSOUND) != 0;
}

// Step 4c: the controller of the adaptive near radius (see the comment at ps_shed_near_target)
constexpr u32 ShedAdaptPeriodMs = 250; // the count is looked at no more often
constexpr u32 ShedAdaptLogMs = 5000; // a changed radius is logged no more often
constexpr float ShedAdaptShrink = 0.9f;
constexpr float ShedAdaptShrinkFast = 0.8f; // the count is above twice the upper edge of the band
constexpr float ShedAdaptGrow = 1.05f;
constexpr float ShedAdaptGrowFast = 1.15f; // the count is below half the lower edge of the band
// The hysteresis of the demotion scan (IsNear with keep): a near creature leaves the near heap only beyond the radius
// times ShedKeepMargin, or (for the "seen" rule) when it was not drawn for ShedKeepSeenFrames frames
constexpr float ShedKeepMargin = 1.15f;
constexpr u32 ShedSeenFrames = 3;
constexpr u32 ShedKeepSeenFrames = 30;

// A creature or a machine: within near_dist m of the camera, or within seen_dist m and drawn in the last seen_frames
// frames. The distance is shrunk by the zoom (view_zoom), as in the UpdateCL LOD of CAI_Stalker
bool ShedNearByView(
    IGameObject& go, const Fvector& view_pos, float view_zoom, float near_dist, float seen_dist, u32 seen_frames)
{
    const float dist = go.Position().distance_to(view_pos) * view_zoom;
    if (dist <= near_dist)
        return true;
    if (dist > seen_dist)
        return false;
    // Drawn recently: the test of the crow rules (CGameObject::UpdateCLCrow, 3 frames). hom_frame is stamped by
    // renderable_Render and the HOM test of the render, which may run meanwhile (a plain aligned u32: any value read is
    // a valid frame number, the class is a heuristic anyway)
    IRenderVisual* visual = go.Visual();
    return visual && visual->getVisData().hom_frame + seen_frames > Device.dwFrame;
}

// An item, a prop, a corpse: near only when the player can touch it - within shed_near_item_dist m of the camera (no
// zoom: what is seen through a scope is not reached any sooner) or in the actor's inventory. An object in a container
// or an inventory goes by its root: the weapon of an NPC next to the camera is near, that of a far one is far
bool ShedNearByTouch(IGameObject& go, const Fvector& view_pos)
{
    IGameObject* root = go.H_Root();
    if (root->Position().distance_to(view_pos) <= float(ps_shed_near_item_dist))
        return true;
    return root != &go && root->cast_actor() != nullptr;
}

void ShedLate(u32 late, u32 cat)
{
    if (late < 10)
        PERF_INC(shed_late_0_10);
    else if (late < 50)
        PERF_INC(shed_late_10_50);
    else if (late < 100)
        PERF_INC(shed_late_50_100);
    else if (late < 250)
        PERF_INC(shed_late_100_250);
    else if (late < 500)
        PERF_INC(shed_late_250_500);
    else if (late < 1000)
        PERF_INC(shed_late_500_1000);
    else
        PERF_INC(shed_late_1000p);

    // Average lateness per category = *_ms / *_n. `cat` is read from Item::dwPadding, set once at
    // internal_Register time - no re-cast on every dequeue (see ShedCategoryOf).
    switch (cat)
    {
    case ShedCatStalker: PERF_ADD(shed_late_cat_stalker_ms, late); PERF_INC(shed_late_cat_stalker_n); break;
    case ShedCatMonster: PERF_ADD(shed_late_cat_monster_ms, late); PERF_INC(shed_late_cat_monster_n); break;
    case ShedCatActor:   PERF_ADD(shed_late_cat_actor_ms, late);   PERF_INC(shed_late_cat_actor_n);   break;
    case ShedCatItem:    PERF_ADD(shed_late_cat_item_ms, late);    PERF_INC(shed_late_cat_item_n);    break;
    default:             PERF_ADD(shed_late_cat_other_ms, late);   PERF_INC(shed_late_cat_other_n);   break;
    }
}

// The heap a step came from (CSheduler::ExecuteTop). Near and urgent are both the near class for the metrics of step 4
enum : u32
{
    ShedTierFar = 0,
    ShedTierNear,
    ShedTierUrgent,
};

// Step 4: lateness of an executed step by the near/far class (average = *_late_ms / *_late_n), stalkers apart
void ShedTierLate(u32 late, bool is_near, u32 cat)
{
    if (is_near)
    {
        PERF_ADD(shed_near_late_ms, late);
        PERF_INC(shed_near_late_n);
        if (cat == ShedCatStalker)
        {
            PERF_ADD(shed_near_stalker_late_ms, late);
            PERF_INC(shed_near_stalker_late_n);
        }
    }
    else
    {
        PERF_ADD(shed_far_late_ms, late);
        PERF_INC(shed_far_late_n);
        if (cat == ShedCatStalker)
        {
            PERF_ADD(shed_far_stalker_late_ms, late);
            PERF_INC(shed_far_stalker_late_n);
        }
    }
}

#if XR_PERF_METRICS
// P19 spike logger (plans/optimization_mt/14-spike-instrumentation.md): detail of a slow shedule_Update
struct ShedSpikeInfo
{
    pcstr name;         // Item::scheduled_name, a copy owned by the item
    ISheduled* object;  // set after the update only if the object is still registered (else may be gone)
};

void DescribeShedSpike(const void* ctx, char* buf, size_t size)
{
    const auto* info = static_cast<const ShedSpikeInfo*>(ctx);
    const IGameObject* go = info->object ? dynamic_cast<const IGameObject*>(info->object) : nullptr;
    if (go)
        xray::perf::DescribeSpikeObject<IGameObject>(go, buf, size);
    else
        xr_sprintf(buf, size, "sched=%s", info->name);
}
#endif

xray::perf::Metric ShedCatMetric(u32 cat)
{
    switch (cat)
    {
    case ShedCatStalker: return xray::perf::Metric::shed_cat_stalker;
    case ShedCatMonster: return xray::perf::Metric::shed_cat_monster;
    case ShedCatActor: return xray::perf::Metric::shed_cat_actor;
    case ShedCatItem: return xray::perf::Metric::shed_cat_item;
    default: return xray::perf::Metric::shed_cat_other;
    }
}
} // namespace

//-------------------------------------------------------------------------------------
void CSheduler::Initialize()
{
    m_current_step_obj = nullptr;
    m_processing_now = false;
}

void CSheduler::Destroy()
{
    internal_Registration();

    // The near and far heaps: one list again for the check below (the order does not matter any more, it is cleared)
    for (auto& item : ItemsNear)
        Items.emplace_back(std::move(item));
    ItemsNear.clear();
    for (auto& item : ItemsUrgent)
        Items.emplace_back(std::move(item));
    ItemsUrgent.clear();
    m_far_scan_cursor = 0;
    m_near_scan_cursor = 0;
    m_urgent_scan_cursor = 0;

    for (u32 it = 0; it < Items.size(); it++)
    {
        if (nullptr == Items[it].Object)
        {
            Items.erase(Items.begin() + it);
            it--;
        }
    }
#ifdef DEBUG
    if (!Items.empty())
    {
        Msg("! Sheduler work-list is not empty");
        for (const auto& item : Items)
            Log(item.Object->shedule_Name().c_str());
    }
#endif

    ItemsRT.clear();
    Items.clear();
    ItemsProcessed.clear();
    {
        std::lock_guard<std::mutex> guard(m_registration_lock);
        Registration.clear();
    }
}

void CSheduler::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    stats.FrameEnd();
    const float percentage = 100.f * stats.Update.result / Device.GetStats().EngineTotal.result;
    font.OutNext("Object Scheduler:");
    font.OutNext("- update:     %2.2fms, %2.1f%%", stats.Update.result, percentage);
    font.OutNext("- load:       %2.2fms", stats.Load);
    if (alert && stats.Update.result > 3.0f)
        alert->Print(font, "Update    > 3ms:  %3.1f", stats.Update.result);
    stats.FrameStart();
}

void CSheduler::internal_Registration()
{
    ZoneScopedN("CSheduler::internal_Registration");
    // Take the queue under the lock and process the local copy: internal_Register/internal_Unregister never
    // call Register/Unregister, so the result is the same as processing Registration in place, but a
    // Register()/Unregister() from another thread (game/render overlap frame) cannot reallocate the vector
    // under this loop. Never runs on two threads at once: the main thread calls it only in UpdateRT()
    // before the Game thread is woken, the Game thread only in UpdateNonRT() before the join.
    xr_vector<ItemReg> pending;
    {
        std::lock_guard<std::mutex> guard(m_registration_lock);
        pending.swap(Registration);
    }
    for (u32 it = 0; it < pending.size(); it++)
    {
        ItemReg& R = pending[it];
        if (R.OP)
        {
            ZoneScopedN("CSheduler::internal_Registration/register_op");
            // register
            // search for paired "unregister"
            bool foundAndErased = false;
            for (u32 pair = it + 1; pair < pending.size(); pair++)
            {
                ItemReg& R_pair = pending[pair];
                if (!R_pair.OP && R_pair.Object == R.Object)
                {
                    foundAndErased = true;
                    pending.erase(pending.begin() + pair);
                    break;
                }
            }

            // register if non-paired
            if (!foundAndErased)
            {
#ifdef DEBUG_SCHEDULER
                Msg("SCHEDULER: internal register [%s][%x][%s]", R.Object->shedule_Name().c_str(), R.Object,
                    R.RT ? "true" : "false");
#endif
                internal_Register(R.Object, R.RT);
            }
#ifdef DEBUG_SCHEDULER
            else
                Msg("SCHEDULER: internal register skipped, because unregister found [%s][%x][%s]", "unknown", R.Object,
                    R.RT ? "true" : "false");
#endif
        }
        else
        {
            ZoneScopedN("CSheduler::internal_Registration/unregister_op");
            // unregister
            internal_Unregister(R.Object, R.RT);
        }
    }
    // Give the (cleared) buffer back to keep its capacity, unless something was queued meanwhile.
    pending.clear();
    std::lock_guard<std::mutex> guard(m_registration_lock);
    if (Registration.empty())
        Registration.swap(pending);
}

void CSheduler::internal_Register(ISheduled* object, bool realTime)
{
    VERIFY(!object->GetSchedulerData().b_locked);

    // Fill item structure
    Item item;
    item.dwTimeForExecute = Device.dwTimeGlobal;
    item.dwTimeOfLastExecute = Device.dwTimeGlobal;
    item.scheduled_name = object->shedule_Name();
    item.Object = object;
    item.GameObject = dynamic_cast<IGameObject*>(object);
    item.Spatial = item.GameObject ? nullptr : dynamic_cast<ISpatial*>(object);
    item.dwPadding = ShedCategoryOf(item.GameObject);
    item.NearKind = ShedNearKindOf(item.GameObject, item.Spatial);

    if (realTime)
    {
        object->GetSchedulerData().b_RT = true;
        ItemsRT.emplace_back(std::move(item));
    }
    else
    {
        object->GetSchedulerData().b_RT = false;

        // Insert into the heap of its class - a new object near the camera must not wait behind the far backlog. By
        // the camera snapshot of this frame (UpdateRTImpl takes it before internal_Registration); the objects
        // registered while a level loads, when the camera may not be in place yet, are classified again once the
        // loading is over (RebuildNearFar, see ProcessStep). Step 4d: never to the urgent heap here - a new object has
        // not thought yet (and this may be the main thread); the scan of the near heap moves it there if it is urgent
        Push(IsNear(item) ? ItemsNear : Items, item);
    }
}

bool CSheduler::internal_Unregister(ISheduled* object, bool realTime, bool warn_on_not_found)
{
    // the object may be already dead
    // VERIFY (!O->shedule.b_locked);
    if (realTime)
    {
        for (u32 i = 0; i < ItemsRT.size(); i++)
        {
            if (ItemsRT[i].Object == object)
            {
#ifdef DEBUG_SCHEDULER
                Msg("SCHEDULER: internal unregister [%s][%x][%s]", "unknown", object, "true");
#endif
                ItemsRT.erase(ItemsRT.begin() + i);
                return true;
            }
        }
    }
    else
    {
        // ItemsNear: the near heap. ItemsProcessed: an object updated earlier in this pass and unregistered later in
        // it (by another object's update) - it used to be nulled only after the requeue, by the queued unregister in
        // internal_Registration; now at once, the same result. Required by the near/far classes: the requeue
        // classifies the processed items (IsNear reads the object) before that internal_Registration. ItemsUrgent: the
        // urgent near heap (step 4d).
        for (xr_vector<Item>* list : { &Items, &ItemsNear, &ItemsUrgent, &ItemsProcessed })
        {
            for (auto& item : *list)
            {
                if (item.Object == object)
                {
#ifdef DEBUG_SCHEDULER
                    Msg("SCHEDULER: internal unregister [%s][%x][%s]", item.scheduled_name.c_str(), object, "false");
#endif
                    item.Object = nullptr;
                    return true;
                }
            }
        }
    }
    if (m_current_step_obj == object)
    {
#ifdef DEBUG_SCHEDULER
        Msg("SCHEDULER: internal unregister (self unregistering) [%x][%s]", object, "false");
#endif

        m_current_step_obj = nullptr;
        return true;
    }

#ifdef DEBUG
    if (warn_on_not_found)
        Msg("! scheduled object %s tries to unregister but is not registered", object->shedule_Name().c_str());
#endif

    return false;
}

#ifndef MASTER_GOLD
bool CSheduler::Registered(ISheduled* object) const
{
    u32 count = 0;

    for (const auto& it : ItemsRT)
    {
        if (it.Object == object)
        {
            // Msg ("0x%8x found in RT",object);
            count = 1;
            break;
        }
    }

    for (const auto& it : Items)
    {
        if (it.Object == object)
        {
            // Msg ("0x%8x found in non-RT",object);
            VERIFY(!count);
            count = 1;
            break;
        }
    }

    for (const xr_vector<Item>* list : { &ItemsNear, &ItemsUrgent }) // the near heaps
    {
        for (const auto& it : *list)
        {
            if (it.Object == object)
            {
                VERIFY(!count);
                count = 1;
                break;
            }
        }
    }

    for (const auto& it : ItemsProcessed)
    {
        if (it.Object == object)
        {
            // Msg ("0x%8x found in process items",object);
            VERIFY(!count);
            count = 1;
            break;
        }
    }

    std::lock_guard<std::mutex> guard(m_registration_lock);
    for (const auto& it : Registration)
    {
        if (it.Object == object)
        {
            if (it.OP)
            {
                // Msg ("0x%8x found in registration on register",object);
                VERIFY(!count);
                ++count;
            }
            else
            {
                // Msg ("0x%8x found in registration on UNregister",object);
                VERIFY(count == 1);
                --count;
            }
        }
    }

    if (!count && m_current_step_obj == object)
    {
        VERIFY2(m_processing_now, "trying to unregister self unregistering object while not processing now");
        count = 1;
    }
    VERIFY(!count || count == 1);
    return count == 1;
}
#endif // !MASTER_GOLD

void CSheduler::Register(ISheduled* A, bool RT)
{
#ifndef MASTER_GOLD
    VERIFY(!Registered(A));
#endif

    ItemReg R;
    R.OP = true;
    R.RT = RT;
    R.Object = A;
    R.Object->GetSchedulerData().b_RT = RT;

#ifdef DEBUG_SCHEDULER
    Msg("SCHEDULER: register [%s][%x]", A->shedule_Name().c_str(), A);
#endif

    std::lock_guard<std::mutex> guard(m_registration_lock);
    Registration.push_back(R);
}

void CSheduler::Unregister(ISheduled* A)
{
#ifndef MASTER_GOLD
    VERIFY(Registered(A));
#endif

#ifdef DEBUG_SCHEDULER
    Msg("SCHEDULER: unregister [%s][%x]", A->shedule_Name().c_str(), A);
#endif

    if (ProcessingOnThisThread())
    {
        if (internal_Unregister(A, A->GetSchedulerData().b_RT, false))
            return;
    }

    ItemReg R;
    R.OP = false;
    R.RT = A->GetSchedulerData().b_RT;
    R.Object = A;

    std::lock_guard<std::mutex> guard(m_registration_lock);
    Registration.push_back(R);
}

bool CSheduler::ProcessingOnThisThread() const
{
    if (!m_processing_now)
        return false;
    // Classic Update(): no owner recorded - same as before the split (m_processing_now alone).
    const std::thread::id owner = m_processing_tid.load(std::memory_order_acquire);
    return owner == std::thread::id{} || owner == std::this_thread::get_id();
}

void CSheduler::EnsureOrder(ISheduled* Before, ISheduled* After)
{
    VERIFY(Before->GetSchedulerData().b_RT && After->GetSchedulerData().b_RT);

    for (u32 i = 0; i < ItemsRT.size(); i++)
    {
        if (ItemsRT[i].Object == After)
        {
            Item A = ItemsRT[i];
            ItemsRT.erase(ItemsRT.begin() + i);
            ItemsRT.push_back(A);
            return;
        }
    }
}

void CSheduler::Push(xr_vector<Item>& heap, Item& item)
{
    heap.emplace_back(std::move(item));
    std::push_heap(heap.begin(), heap.end());
}

void CSheduler::Pop(xr_vector<Item>& heap)
{
    std::pop_heap(heap.begin(), heap.end());
    heap.pop_back();
}

// Removes heap[index] and keeps the heap valid for std::push_heap / std::pop_heap: the same layout (the children of i
// are 2i+1 and 2i+2) and the same order (Item::operator<, a parent is never "less" than its children). O(log n).
void CSheduler::HeapErase(xr_vector<Item>& heap, size_t index)
{
    const size_t last = heap.size() - 1;
    if (index != last)
        heap[index] = std::move(heap[last]);
    heap.pop_back();
    if (index >= heap.size())
        return;

    // The former last item may belong above or below the hole
    size_t i = index;
    while (i > 0)
    {
        const size_t parent = (i - 1) / 2;
        if (!(heap[parent] < heap[i]))
            break;
        std::swap(heap[parent], heap[i]);
        i = parent;
    }
    if (i != index)
        return;

    const size_t size = heap.size();
    for (;;)
    {
        const size_t left = 2 * i + 1;
        if (left >= size)
            break;
        size_t best = left;
        const size_t right = left + 1;
        if (right < size && heap[left] < heap[right])
            best = right;
        if (!(heap[i] < heap[best]))
            break;
        std::swap(heap[i], heap[best]);
        i = best;
    }
}

// Step 4: the near/far class (see the comment at ps_shed_budget_ms). Called on the Game thread (the step, the requeue,
// the scan) and in internal_Register (main or Game thread, never both at once). Reads the position of a registered
// object - as shedule_Scale() did there already - and m_view_pos / m_view_zoom, the snapshot of UpdateRTImpl.
bool CSheduler::IsNear(const Item& item, bool keep) const
{
    const float margin = keep ? ShedKeepMargin : 1.f;
    const u32 seen_frames = keep ? ShedKeepSeenFrames : ShedSeenFrames;
    switch (item.NearKind)
    {
    case ShedNearCreature:
        // Step 4c: by the adaptive radii (m_near_dist / m_near_seen_dist)
        if (ShedAlive(*item.GameObject))
        {
            return ShedNearByView(*item.GameObject, m_view_pos, m_view_zoom, m_near_dist * margin,
                m_near_seen_dist * margin, seen_frames);
        }
        // A corpse: its updates no longer drive any reaction, it is an item for the class (its loot is near when the
        // player comes up to it)
        return ShedNearByTouch(*item.GameObject, m_view_pos);
    case ShedNearActor: return true;
    case ShedNearMachine:
        // A few objects with a crew or a gun of their own: the configured radii, not the adaptive ones of the crowd
        return ShedNearByView(*item.GameObject, m_view_pos, m_view_zoom, float(ps_shed_near_dist) * margin,
            float(ps_shed_near_seen_dist) * margin, seen_frames);
    case ShedNearExplosive:
    {
        // In the world (thrown, lying, flying): its fuse - CMissile::shedule_Update destroys (explodes) a grenade at its
        // time, CExplosiveItem::shedule_Update a burning barrel - must not wait for the far heap. Within the "seen"
        // distance, no zoom and no visibility test: a grenade thrown behind a wall is heard and hurts as well. The
        // grenades in the NPC inventories (thousands in a crowd) are items
        IGameObject& go = *item.GameObject;
        if (go.H_Root() != &go)
            return ShedNearByTouch(go, m_view_pos);
        return go.Position().distance_to(m_view_pos) <= float(ps_shed_near_seen_dist);
    }
    case ShedNearTrigger:
    {
        // By the distance to its bounds (the spatial sphere, a large zone may be entered far from its center), no zoom:
        // only the player's presence matters to it
        const Fsphere& bounds = item.GameObject->GetSpatialData().sphere;
        return bounds.P.distance_to(m_view_pos) - bounds.R <= float(ps_shed_near_dist);
    }
    case ShedNearItem: return ShedNearByTouch(*item.GameObject, m_view_pos);
    // (explosives, triggers, items and particles: no hysteresis, their rules do not flicker with the drawing)
    case ShedNearParticles:
        // CPS_Instance: by the center of their spatial sphere, no visibility test - cheap updates, all of those that
        // can be seen well are within the "seen" distance
        return item.Spatial->GetSpatialData().sphere.P.distance_to(m_view_pos) * m_view_zoom <=
            float(ps_shed_near_seen_dist);
    default:
        // Neither a game object nor spatial: a global service (ALife, the agent managers of the NPC groups, autosave,
        // the game). Always far: served oldest first as before, at the pace of the far heap. Near would put them back
        // to their nominal intervals - ALife (schedule_min/max 1 ms -> ~265 ms) would then run ~4 times a second
        // instead of once in 5 s, and one ALife update costs 10-37 ms in a heavy scene (22-game-thread.md, 1.2): its
        // cadence is a separate topic (V9; the untested switch shed_services_near is removed)
        return false;
    }
}

// Step 4d: see the comment at ps_shed_urgent_share. Only the living creatures are asked (one virtual call): the rest of
// the near class is never urgent. On the Game thread between the updates (the requeue, the scans, RebuildNearFar);
// shedule_Urgent reads the state of the game logic (see CAI_Stalker::shedule_Urgent)
bool CSheduler::IsUrgent(const Item& item)
{
    return item.Object && item.NearKind == ShedNearCreature && ShedAlive(*item.GameObject) &&
        item.Object->shedule_Urgent();
}

xr_vector<CSheduler::Item>& CSheduler::NearHeapOf(const Item& item)
{
    return IsUrgent(item) ? ItemsUrgent : ItemsNear;
}

// The living stalkers and monsters in the near heaps (the log line of RebuildNearFar, shed/near_creatures, the adaptive
// radius): the urgent heap is a part of the near class
u32 CSheduler::CountNearCreatures() const
{
    u32 count = 0;
    for (const xr_vector<Item>* list : { &ItemsNear, &ItemsUrgent })
    {
        for (const auto& item : *list)
        {
            if (item.Object && item.NearKind == ShedNearCreature && ShedAlive(*item.GameObject))
                ++count;
        }
    }
    return count;
}

// Classifies every item of the normal queue anew: both heaps are merged and split again by IsNear. Once a level is
// loaded (see ProcessStep): the objects registered during the loading frames were classified by the camera of those
// frames, which may not be in place yet. At the start of ProcessStep on the Game thread, between the steps: the order
// of each heap is rebuilt, nothing is lost or run twice.
void CSheduler::RebuildNearFar()
{
    ZoneScopedN("CSheduler::RebuildNearFar");
    // Step 4c: the radius starts from the configured one (the controller shrinks it from there)
    m_near_scale = 1.f;
    ApplyNearRadius();
    xr_vector<Item> all;
    all.swap(Items);
    for (xr_vector<Item>* list : { &ItemsNear, &ItemsUrgent })
    {
        for (auto& item : *list)
            all.emplace_back(std::move(item));
        list->clear();
    }
    for (auto& item : all)
    {
        if (item.Object && IsNear(item))
            NearHeapOf(item).emplace_back(std::move(item)); // step 4d: the urgent ones apart
        else
            Items.emplace_back(std::move(item));
    }
    std::make_heap(ItemsNear.begin(), ItemsNear.end());
    std::make_heap(ItemsUrgent.begin(), ItemsUrgent.end());
    std::make_heap(Items.begin(), Items.end());
    m_far_scan_cursor = 0;
    m_near_scan_cursor = 0;
    m_urgent_scan_cursor = 0;
    // Every item was just classified by the current radius: as if the scans had gone over their heaps (the counters
    // wrap, only differences are used)
    m_adapt_near_mark = m_near_scanned - ItemsNear.size();
    m_adapt_far_mark = m_far_scanned - Items.size();
    m_adapt_urgent_mark = m_urgent_scanned - ItemsUrgent.size();
    m_adapt_time = Device.dwTimeGlobal;
    m_adapt_log_time = Device.dwTimeGlobal;
    m_adapt_logged_scale = m_near_scale;
#ifndef MASTER_GOLD
    Msg("* [scheduler] near/far rebuilt: near %u (urgent %u, living creatures %u), far %u",
        u32(ItemsNear.size() + ItemsUrgent.size()), u32(ItemsUrgent.size()), CountNearCreatures(), u32(Items.size()));
#endif
}

// Step 4c: the radii of the living creatures by m_near_scale, kept between shed_near_dist_min m (for the plain radius;
// none if shed_near_dist is not above it) and the configured ones - the cvars may change at any time
void CSheduler::ApplyNearRadius()
{
    const float max_dist = float(ps_shed_near_dist);
    const float min_dist = float(_max(0, ps_shed_near_dist_min));
    const float scale_min = max_dist > min_dist ? min_dist / max_dist : 1.f;
    m_near_scale = std::clamp(m_near_scale, scale_min, 1.f);
    m_near_dist = max_dist * m_near_scale;
    m_near_seen_dist = float(ps_shed_near_seen_dist) * m_near_scale;
    xray::perf::SetGauge(xray::perf::Metric::shed_near_radius, s64(iFloor(m_near_dist * 100.f)));
}

// Step 4c: the controller of the adaptive near radius (see the comment at ps_shed_near_target; permanent). At the
// start of ProcessStep, on the Game thread
void CSheduler::UpdateNearRadius(u32 now)
{
    if (now - m_adapt_time < ShedAdaptPeriodMs)
    {
        ApplyNearRadius();
        return;
    }
    m_adapt_time = now;

    const u32 count = CountNearCreatures();
    const u32 target = u32(_max(1, ps_shed_near_target));
    const u32 band = target / 4;
    const u32 high = target + band;
    const u32 low = target - band;
    // Has the scan shown the effect of the last change yet (with no scan at all - shed_scan 0 - only the requeue
    // reclassifies, nothing to wait for)
    const bool scans = ps_shed_scan > 0;
    // (the near class: both near heaps, step 4d)
    const bool near_seen = !scans ||
        (m_near_scanned - m_adapt_near_mark >= u64(ItemsNear.size()) &&
            m_urgent_scanned - m_adapt_urgent_mark >= u64(ItemsUrgent.size()));
    const bool far_seen = !scans || m_far_scanned - m_adapt_far_mark >= u64(Items.size());

    const float before = m_near_scale;
    if (count > high && near_seen)
        m_near_scale *= count > 2 * high ? ShedAdaptShrinkFast : ShedAdaptShrink;
    else if (count < low && far_seen)
        m_near_scale *= count < low / 2 ? ShedAdaptGrowFast : ShedAdaptGrow;
    ApplyNearRadius();
    if (m_near_scale != before)
    {
        m_adapt_near_mark = m_near_scanned;
        m_adapt_far_mark = m_far_scanned;
        m_adapt_urgent_mark = m_urgent_scanned;
    }
#ifndef MASTER_GOLD
    // Rarely: the radius moves every few hundred ms while it adapts (the value it settled at is logged too, at the
    // first look of the controller ShedAdaptLogMs after the previous line)
    if (now - m_adapt_log_time >= ShedAdaptLogMs && std::abs(m_near_scale - m_adapt_logged_scale) > 0.01f)
    {
        Msg("* [scheduler] adaptive near: near radius %.1f m (seen %.1f m), living near creatures %u, target %u",
            m_near_dist, m_near_seen_dist, count, target);
        m_adapt_log_time = now;
        m_adapt_logged_scale = m_near_scale;
    }
#endif
}

// Step 4c: a near object that is no longer near (the radius shrank, the camera moved away, the creature died) would
// stay in the near heap until its next update - or for good if the near heap is overloaded. Each frame shed_scan / 2 near
// items are checked round-robin (the near heap is small: ~1000 items at most in a crowd, a whole pass in a few frames)
// and those clearly far (IsNear with keep - the hysteresis) are moved to the far heap, their due time kept. O(log n)
// a move, no full rebuild when the radius shrinks.
void CSheduler::DemoteFarFromNear()
{
    ZoneScopedN("CSheduler::DemoteFarFromNear");
    u32 count = ps_shed_scan > 0 ? u32(_max(1, ps_shed_scan / 2)) : 0;
    if (count > ItemsNear.size())
        count = u32(ItemsNear.size());
    for (u32 k = 0; k < count && !ItemsNear.empty(); ++k)
    {
        if (m_near_scan_cursor >= ItemsNear.size())
            m_near_scan_cursor = 0;
        ++m_near_scanned;
        Item& item = ItemsNear[m_near_scan_cursor];
        // A nulled (unregistered) item stays: it is dropped as stale when it comes to the top
        if (item.Object && !IsNear(item, true))
        {
            Item moved = std::move(item);
            HeapErase(ItemsNear, m_near_scan_cursor);
            Push(Items, moved);
            PERF_INC(shed_demoted);
            continue; // another item is at the cursor now
        }
        // Step 4d: a calm near creature that has to react now (hit, saw an enemy while it waited) - to the urgent heap,
        // its due time kept
        if (IsUrgent(item))
        {
            Item moved = std::move(item);
            HeapErase(ItemsNear, m_near_scan_cursor);
            Push(ItemsUrgent, moved);
            PERF_INC(shed_urgent_in);
            continue;
        }
        ++m_near_scan_cursor;
    }
}

// Step 4d: the scan of the urgent heap, the pair of DemoteFarFromNear (shed_scan / 2 items a frame, round-robin): an
// item that is no longer near goes to the far heap (the same hysteresis), one that is no longer urgent to the calm near
// heap - both with their due time kept. Most urgent items are requeued (and classified anew) after their own update
// sooner; the scan covers those that wait while the urgent heap is overloaded
void CSheduler::ScanUrgent()
{
    ZoneScopedN("CSheduler::ScanUrgent");
    u32 count = ps_shed_scan > 0 ? u32(_max(1, ps_shed_scan / 2)) : 0;
    if (count > ItemsUrgent.size())
        count = u32(ItemsUrgent.size());
    for (u32 k = 0; k < count && !ItemsUrgent.empty(); ++k)
    {
        if (m_urgent_scan_cursor >= ItemsUrgent.size())
            m_urgent_scan_cursor = 0;
        ++m_urgent_scanned;
        Item& item = ItemsUrgent[m_urgent_scan_cursor];
        if (item.Object)
        {
            const bool to_far = !IsNear(item, true);
            if (to_far || !IsUrgent(item))
            {
                Item moved = std::move(item);
                HeapErase(ItemsUrgent, m_urgent_scan_cursor);
                if (to_far)
                {
                    Push(Items, moved);
                    PERF_INC(shed_demoted);
                }
                else
                {
                    Push(ItemsNear, moved);
                    PERF_INC(shed_urgent_out);
                }
                continue; // another item is at the cursor now
            }
        }
        ++m_urgent_scan_cursor;
    }
}

// A far object that came near (the camera moved, it was drawn) would wait for its turn in the far heap - seconds.
// Each frame shed_scan far items are checked round-robin and the near ones are moved to the near heap (~1-2 s for the
// whole heap of 14-16k at 20 FPS). The other way: the requeue after the update and DemoteFarFromNear (step 4c).
void CSheduler::PromoteNearFromFar()
{
    ZoneScopedN("CSheduler::PromoteNearFromFar");
    u32 count = ps_shed_scan > 0 ? u32(ps_shed_scan) : 0;
    if (count > Items.size())
        count = u32(Items.size());
    for (u32 k = 0; k < count && !Items.empty(); ++k)
    {
        if (m_far_scan_cursor >= Items.size())
            m_far_scan_cursor = 0;
        ++m_far_scanned;
        Item& item = Items[m_far_scan_cursor];
        if (item.Object && IsNear(item))
        {
            Item moved = std::move(item);
            HeapErase(Items, m_far_scan_cursor);
            Push(NearHeapOf(moved), moved); // step 4d: an urgent one to the urgent heap
            PERF_INC(shed_promoted);
            continue; // another item is at the cursor now
        }
        ++m_far_scan_cursor;
    }
}

bool CSheduler::ExecuteTop(xr_vector<Item>& heap, u32 dwTime, u32 tier, shared_str& last_name)
{
    const bool is_near = tier != ShedTierFar;
    // Update
    Item item = heap.front();
    const u32 late = dwTime - item.dwTimeForExecute;
    ShedLate(late, item.dwPadding);

    if (!item.Object || !item.Object->shedule_Needed())
    {
#ifdef DEBUG_SCHEDULER
        Msg("SCHEDULER: process unregister [%s][%x][%s]", item.scheduled_name.c_str(), item.Object, "false");
#endif
        ZoneScopedN("CSheduler::ProcessStep/PopStale");
        PERF_INC(shed_pop_stale);
        // Erase element
        Pop(heap);
        return false;
    }

    if (xray::perf::g_perf_metrics_active)
    {
        ShedTierLate(late, is_near, item.dwPadding);
        // Step 4d: the lateness of the living near creatures, urgent or calm by their state right now (not by the heap
        // they came from: one that became urgent while it waited in the calm heap counts as urgent)
        if (is_near && item.NearKind == ShedNearCreature && ShedAlive(*item.GameObject))
        {
            if (IsUrgent(item))
            {
                PERF_ADD(shed_urgent_late_ms, late);
                PERF_INC(shed_urgent_late_n);
            }
            else
            {
                PERF_ADD(shed_calm_late_ms, late);
                PERF_INC(shed_calm_late_n);
            }
        }
    }
    if (tier == ShedTierUrgent)
        PERF_INC(shed_urgent_steps);

    auto& schedulerData = item.Object->GetSchedulerData();

#ifdef DEBUG_SCHEDULER
    Msg("SCHEDULER: process step [%s][%x][false]", item.scheduled_name.c_str(), item.Object);
#endif

    // Insert into priority Queue
    Pop(heap);

    const u32 Elapsed = dwTime - item.dwTimeOfLastExecute;

    // Real update call
    // Msg("------- %d:", Device.dwFrame);
#ifdef DEBUG
    schedulerData.dbg_startframe = Device.dwFrame;
#endif

    // Calc next update interval
    u32 dwUpdate;
    {
        ZoneScopedN("CSheduler::ProcessStep/ComputeInterval");
        const u32 dwMin = _max(u32(30), schedulerData.t_min);
        u32 dwMax = (1000 + schedulerData.t_max) / 2;
        const float scale = item.Object->shedule_Scale();
        dwUpdate = dwMin + iFloor(float(dwMax - dwMin) * scale);
        clamp(dwUpdate, u32(_max(dwMin, u32(20))), dwMax);
    }

    // The DT clamp: see the comment at ps_shed_budget_ms
    const u32 dt_max = _max(u32(schedulerData.t_max), u32(1000));
    if (Elapsed > dt_max)
    {
        PERF_INC(shed_dt_clamped);
        PERF_ADD(shed_dt_lost_ms, Elapsed - dt_max);
    }

    m_current_step_obj = item.Object;

    {
        pcstr sched_name = item.scheduled_name.c_str();
        if (!sched_name || !sched_name[0])
            sched_name = "(no name)";
        ZoneTransientN(___sched_su, sched_name, !xray::perf::HideHotZones());
        xray::perf::ScopedTime cat_time(ShedCatMetric(item.dwPadding));
#if XR_PERF_METRICS
        // P19 spike logger: the object is described only if it is still registered after its update
        ShedSpikeInfo spike_info{sched_name, nullptr};
        PERF_SPIKE_NAMED(shed_spike, "shedule_update", &spike_info, &DescribeShedSpike,
            xray::perf::SpikeActive() && item.dwPadding == ShedCatActor);
#endif
        item.Object->shedule_Update(clampr(Elapsed, u32(1), dt_max));
#if XR_PERF_METRICS
        if (m_current_step_obj)
            spike_info.object = item.Object;
#endif
    }
    last_name = item.scheduled_name;
    if (!m_current_step_obj)
    {
#ifdef DEBUG_SCHEDULER
        Msg("SCHEDULER: process unregister (self unregistering) [%s][%x][%s]", item.scheduled_name.c_str(), item.Object,
            "false");
#endif
        return true;
    }

    m_current_step_obj = nullptr;

    // Fill item structure
    item.dwTimeForExecute = dwTime + dwUpdate;
    item.dwTimeOfLastExecute = dwTime;
    ItemsProcessed.emplace_back(std::move(item));
    return true;
}

void CSheduler::ProcessStep()
{
    ZoneScopedN("CSheduler::ProcessStep");
    // shedule_Update is not parallelized: objects touch shared state and Register/Unregister the scheduler.
    // Step 4: the near heap first, then the far one, both within one fixed time budget (see the comment at
    // ps_shed_budget_ms). The adaptive psShedulerCurrent is not used: it only follows the budget (stats).

    // A level was loaded: at the first pass after its loading frames (the load screen, the precache) every item is
    // classified anew by the camera that is in place now. A pass that never saw a loading frame needs none: the
    // objects were registered by the camera of a normal frame
    const bool loading = load_screen_renderer.IsActive() || Device.dwPrecacheFrame != 0;
    if (m_was_loading && !loading)
        RebuildNearFar();
    m_was_loading = loading;
    // Step 4c: the radii of the living creatures for this frame
    UpdateNearRadius(Device.dwTimeGlobal);

    const u32 dwTime = Device.dwTimeGlobal;
    // Precache frames: no time limit (the step cap still holds)
    const bool timed = Device.dwPrecacheFrame == 0;
    const u32 budget_ms = u32(_max(1, ps_shed_budget_ms));
    const u64 budget_ns = u64(budget_ms) * 1000000ull;
    const u32 max_steps = ps_shed_max_steps > 0 ? u32(ps_shed_max_steps) : u32(-1);
    const u32 near_min_steps = u32(_max(0, ps_shed_near_min_steps));
    xray::perf::SetGauge(xray::perf::Metric::shed_budget, budget_ms);

    {
        // Both scans of the class, shed/promote_scan is their time. A demoted item is not promoted back by the far scan:
        // it failed the wider test (keep), so it fails the plain one too
        PERF_SCOPE(shed_promote_scan);
        DemoteFarFromNear();
        ScanUrgent(); // step 4d
        PromoteNearFromFar();
    }

    CTimer timer;
    timer.Start();
    // The far heap keeps its share only while it has due items; otherwise the near one may use the whole budget
    const bool far_due = !Items.empty() && Items.front().dwTimeForExecute < dwTime;
    const u64 far_share = u64(std::clamp(ps_shed_far_share, 0, 90));
    const u64 near_budget_ns = far_due ? budget_ns * (100 - far_share) / 100 : budget_ns;

    u32 steps = 0;
    bool exit_limit = false;
    bool exit_time = false;
    shared_str last_name;
    // The steps of both near heaps (shed_near_min_steps holds for them together)
    u32 near_steps = 0;
    u32 urgent_steps = 0;

    if (!ItemsUrgent.empty())
    {
        // Step 4d: the urgent near heap first (see the comment at ps_shed_urgent_share). Its share of the near budget only
        // while the calm near heap has due items, otherwise the whole near budget
        ZoneScopedN("CSheduler::ProcessStep/urgent");
        PERF_SCOPE(shed_phase_urgent);
        const bool calm_due = !ItemsNear.empty() && ItemsNear.front().dwTimeForExecute < dwTime;
        const u64 urgent_share = u64(std::clamp(ps_shed_urgent_share, 10, 100));
        const u64 urgent_budget_ns = calm_due ? near_budget_ns * urgent_share / 100 : near_budget_ns;
        while (!ItemsUrgent.empty() && ItemsUrgent.front().dwTimeForExecute < dwTime)
        {
            if (near_steps >= near_min_steps)
            {
                if (steps >= max_steps)
                {
                    exit_limit = true;
                    break;
                }
                if (timed && timer.GetElapsed_ns() >= urgent_budget_ns)
                {
                    exit_time = true;
                    break;
                }
            }
            if (!ExecuteTop(ItemsUrgent, dwTime, ShedTierUrgent, last_name))
                continue;
            ++steps;
            ++near_steps;
            ++urgent_steps;
        }
    }

    {
        ZoneScopedN("CSheduler::ProcessStep/near");
        PERF_SCOPE(shed_phase_near);
        // Step 4d: at least one calm near step a frame once the urgent heap took any - the calm heap is served oldest
        // first, so it always moves on (no starvation). None needed otherwise (no urgent step: the near heap as before)
        const u32 calm_min_steps = urgent_steps > 0 ? 1 : 0;
        u32 calm_steps = 0;
        while (!ItemsNear.empty() && ItemsNear.front().dwTimeForExecute < dwTime)
        {
            if (near_steps >= near_min_steps && calm_steps >= calm_min_steps)
            {
                if (steps >= max_steps)
                {
                    exit_limit = true;
                    break;
                }
                if (timed && timer.GetElapsed_ns() >= near_budget_ns)
                {
                    exit_time = true;
                    break;
                }
            }
            if (!ExecuteTop(ItemsNear, dwTime, ShedTierNear, last_name))
                continue;
            ++steps;
            ++near_steps;
            ++calm_steps;
        }
    }

    {
        ZoneScopedN("CSheduler::ProcessStep/far");
        PERF_SCOPE(shed_phase_far);
        // At least one far step a frame, whatever the near heap took: the far heap is served oldest first, so it
        // always moves on (no starvation)
        u32 far_steps = 0;
        while (!Items.empty() && Items.front().dwTimeForExecute < dwTime)
        {
            if (far_steps >= 1)
            {
                if (steps >= max_steps)
                {
                    exit_limit = true;
                    break;
                }
                if (timed && timer.GetElapsed_ns() >= budget_ns)
                {
                    exit_time = true;
                    break;
                }
            }
            if (!ExecuteTop(Items, dwTime, ShedTierFar, last_name))
                continue;
            ++steps;
            ++far_steps;
        }
    }

    PERF_ADD(shed_steps, steps);
    if (exit_limit)
        PERF_INC(shed_exit_limit);
    else if (exit_time)
        PERF_INC(shed_exit_time);
    else
        PERF_INC(shed_exit_empty);

    if (xray::perf::g_perf_metrics_active)
    {
        u32 behind = 0;
        for (const xr_vector<Item>* list : { &Items, &ItemsNear, &ItemsUrgent })
        {
            for (const auto& it : *list)
            {
                if (it.dwTimeForExecute < dwTime)
                    ++behind;
            }
        }
        // Step 4d: the living near creatures that are urgent right now, in both near heaps (the calm heap holds those
        // that became urgent while they wait, until the scan moves them); one virtual call each, only with metrics on
        u32 urgent_creatures = 0;
        for (const xr_vector<Item>* list : { &ItemsNear, &ItemsUrgent })
        {
            for (const auto& it : *list)
            {
                if (IsUrgent(it))
                    ++urgent_creatures;
            }
        }
        xray::perf::SetGauge(xray::perf::Metric::shed_behind, behind);
        // The near class: both near heaps (step 4d)
        xray::perf::SetGauge(xray::perf::Metric::shed_near_items, s64(ItemsNear.size() + ItemsUrgent.size()));
        xray::perf::SetGauge(xray::perf::Metric::shed_near_creatures, s64(CountNearCreatures()));
        xray::perf::SetGauge(xray::perf::Metric::shed_urgent_creatures, s64(urgent_creatures));
    }

    // Push "processed" back, each by its class now (an unregistered one - Object nulled - to the far heap, it is
    // dropped there as stale). Step 4d: a near one by its urgency after its update - its state is fresh now
    {
        ZoneScopedN("CSheduler::ProcessStep/RequeueHeap");
        while (ItemsProcessed.size())
        {
            Item& item = ItemsProcessed.back();
            Push(item.Object && IsNear(item) ? NearHeapOf(item) : Items, item);
            ItemsProcessed.pop_back();
        }
    }

    // The adaptive budget only follows the fixed one: stats.Load and shed/ps_current show it
    psShedulerTarget = float(budget_ms);
}

void CSheduler::UpdateRTImpl()
{
    // Step 4: the camera snapshot for the near/far class of this frame (see m_view_pos). Overlap frame: the main thread
    // right before the Game thread is woken (the wake orders it for the Game thread); sequential frame: the task that
    // runs Update() while the main thread waits for it. A zoomed camera (2D scope, binoculars) shows far objects as big
    // as near ones: the distance is shrunk by the zoom, as in the UpdateCL LOD of CAI_Stalker
    m_view_pos = Device.vCameraPosition;
    m_view_zoom = 1.f;
    if (Device.fFOV > EPS && g_fov > EPS && Device.fFOV < g_fov)
        m_view_zoom = std::tan(deg2rad(Device.fFOV) * 0.5f) / std::tan(deg2rad(g_fov) * 0.5f);

    {
        ZoneScopedN("CSheduler::Update/internal_Registration_pre");
        internal_Registration();
    }
    isSheduleInProgress = true;

#ifdef DEBUG_SCHEDULER
    Msg("SCHEDULER: PROCESS STEP %d", Device.dwFrame);
#endif
    // Realtime priority
    m_processing_now = true;
    const u32 dwTime = Device.dwTimeGlobal;
    {
        ZoneScopedN("CSheduler::Update/ItemsRT");
        for (auto& item : ItemsRT)
        {
            R_ASSERT(item.Object);
#ifdef DEBUG_SCHEDULER
            Msg("SCHEDULER: process step [%s][%x][true]", item.Object->shedule_Name().c_str(), item.Object);
#endif
            if (!item.Object->shedule_Needed())
            {
#ifdef DEBUG_SCHEDULER
                Msg("SCHEDULER: process unregister [%s][%x][%s]", item.Object->shedule_Name().c_str(), item.Object, "false");
#endif
                item.dwTimeOfLastExecute = dwTime;
                continue;
            }

            const u32 Elapsed = dwTime - item.dwTimeOfLastExecute;
#ifdef DEBUG
            VERIFY(item.Object->GetSchedulerData().dbg_startframe != Device.dwFrame);
            item.Object->GetSchedulerData().dbg_startframe = Device.dwFrame;
#endif
            {
                pcstr sched_name = item.scheduled_name.c_str();
                if (!sched_name || !sched_name[0])
                    sched_name = "(no name)";
                ZoneTransientN(___sched_rt, sched_name, !xray::perf::HideHotZones());
                xray::perf::ScopedTime cat_time(ShedCatMetric(item.dwPadding));
#if XR_PERF_METRICS
                ShedSpikeInfo spike_info{sched_name, nullptr};
                PERF_SPIKE_NAMED(shed_spike, "shedule_update_rt", &spike_info, &DescribeShedSpike,
                    xray::perf::SpikeActive() && item.dwPadding == ShedCatActor);
#endif
                item.Object->shedule_Update(Elapsed);
#if XR_PERF_METRICS
                spike_info.object = item.Object;
#endif
            }
            item.dwTimeOfLastExecute = dwTime;
        }
    }
}

void CSheduler::UpdateNonRTImpl()
{
    // Normal (sheduled)
    ProcessStep();
    m_processing_now = false;
#ifdef DEBUG_SCHEDULER
    Msg("SCHEDULER: PROCESS STEP FINISHED %d", Device.dwFrame);
#endif
    clamp(psShedulerTarget, 3.f, 66.f);
    psShedulerCurrent = 0.9f * psShedulerCurrent + 0.1f * psShedulerTarget;
    stats.Load = psShedulerCurrent;
    xray::perf::SetGauge(xray::perf::Metric::shed_current, s64(iFloor(psShedulerCurrent)));
    xray::perf::SetGauge(xray::perf::Metric::shed_items, s64(Items.size() + ItemsNear.size() + ItemsUrgent.size()));
    xray::perf::SetGauge(xray::perf::Metric::shed_urgent_items, s64(ItemsUrgent.size()));
    xray::perf::SetGauge(xray::perf::Metric::shed_items_rt, s64(ItemsRT.size()));

    // Finalize
    isSheduleInProgress = false;
    {
        ZoneScopedN("CSheduler::Update/internal_Registration_post");
        internal_Registration();
    }
}

void CSheduler::Update()
{
    ZoneScopedN("CSheduler::Update");

    // Initialize
    stats.Update.Begin();
    UpdateRTImpl();
    UpdateNonRTImpl();
    stats.Update.End();
}

// Game/render overlap frame (GAME_RENDER_OVERLAP_PLAN P1, MX f76479197): the device calls UpdateRT() on the
// main thread right before it wakes the Game thread, and the Game thread calls UpdateNonRT(). They never run
// at the same time (the wake comes after UpdateRT returns, the join before the next frame), so ItemsRT/Items
// and internal_Registration stay single-threaded; only Registration may be appended from both (its lock).
void CSheduler::UpdateRT()
{
    ZoneScopedN("CSheduler::UpdateRT");
    stats.Update.Begin();
    m_processing_tid.store(std::this_thread::get_id(), std::memory_order_release);
    UpdateRTImpl();
    m_processing_now = false;
    m_processing_tid.store(std::thread::id{}, std::memory_order_release);
    stats.Update.End();
}

void CSheduler::UpdateNonRT()
{
    ZoneScopedN("CSheduler::UpdateNonRT");
    stats.Update.Begin();
    m_processing_tid.store(std::this_thread::get_id(), std::memory_order_release);
    m_processing_now = true;
    UpdateNonRTImpl();
    m_processing_tid.store(std::thread::id{}, std::memory_order_release);
    stats.Update.End();
}
