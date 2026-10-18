#pragma once

#include "xrCore/xrstring.h"
#include "xrCore/FTimer.h"
#include "xrCore/_vector3d.h"

#include <atomic>
#include <mutex>
#include <thread>

class ISheduled;
class IGameObject;
class ISpatial;

class ENGINE_API CSheduler
{
private:
    struct Item
    {
        u32 dwTimeForExecute;
        u32 dwTimeOfLastExecute;
        shared_str scheduled_name;
        ISheduled* Object;
        // dynamic_cast<IGameObject*>(Object), once at internal_Register (nullptr: not a game object), and for the rest
        // dynamic_cast<ISpatial*>(Object) (particles; nullptr for a game object or a non-spatial service). Read only
        // while Object is not null (the object is still registered): the near/far class of the scheduler (IsNear)
        IGameObject* GameObject;
        ISpatial* Spatial;
        u32 dwPadding; // the category (ShedCat* in xrSheduler.cpp), set once at internal_Register
        u32 NearKind; // what the object is for the near/far class (ShedNear* in xrSheduler.cpp), set with dwPadding

        ICF bool operator<(const Item& I) const { return dwTimeForExecute > I.dwTimeForExecute; }
    };
    struct ItemReg
    {
        BOOL OP;
        BOOL RT;
        ISheduled* Object;
    };

    struct SchedulerStatistics
    {
        float Load;
        CStatTimer Update;

        SchedulerStatistics() { FrameStart(); }
        void FrameStart()
        {
            Load = 0.0f;
            Update.FrameStart();
        }

        void FrameEnd() { Update.FrameEnd(); }
    };

private:
    xr_vector<Item> ItemsRT;
    xr_vector<Item> Items;
    xr_vector<Item> ItemsProcessed;
    xr_vector<ItemReg> Registration;
    // GAME_RENDER_OVERLAP П1 (game/render overlap frame): Register/Unregister may come from the main thread
    // (render) while the Game thread drains Registration, so the queue itself is guarded. Only the queue:
    // internal_Registration() works on a swapped-out local copy, so the lock is never held across calls into
    // objects. Uncontended in a sequential frame.
    mutable std::mutex m_registration_lock;
    ISheduled* m_current_step_obj;
    std::atomic<bool> m_processing_now{}; // atomic: Unregister() may read it from another thread (see below)
    // Thread that runs UpdateRT()/UpdateNonRT() right now (empty in the classic Update(): there, as before,
    // any Unregister() during processing takes the immediate path). Unregister() from another thread while
    // a split part runs must not touch Items/ItemsRT directly - it is queued instead.
    std::atomic<std::thread::id> m_processing_tid{};
    SchedulerStatistics stats;

    // 22-game-thread.md, step 4 (plans/optimization_mt): the normal queue is split in two heaps by the kind of the
    // object and its distance to the camera (IsNear) - ItemsNear (served first, within the time budget) and Items (the
    // far ones, the rest of the budget and at least one step a frame).
    xr_vector<Item> ItemsNear;
    // Step 4d (the urgent near heap): the near objects that have to react now (ISheduled::shedule_Urgent) - served
    // before ItemsNear within shed_urgent_share % of the near budget. A part of the near class: everything that is said
    // of the near heap (the scans, the adaptive radius count) holds for it too
    xr_vector<Item> ItemsUrgent;
    u32 m_urgent_scan_cursor{}; // the round-robin scan of ItemsUrgent (ScanUrgent)
    bool m_was_loading{}; // the previous ProcessStep ran in a loading frame (RebuildNearFar once the loading is over)
    u32 m_far_scan_cursor{}; // the round-robin scan of Items for objects that came near (PromoteNearFromFar)
    // The camera snapshot for the near/far class, taken in UpdateRTImpl: on the main thread right before the Game
    // thread is woken (overlap frame) or before the non-RT part on the same thread (sequential frame) - never the live
    // Device.vCameraPosition that the render may move while the Game thread runs.
    Fvector m_view_pos{};
    float m_view_zoom{1.f}; // distance factor of a zoomed camera (tan(fov/2) / tan(g_fov/2), 1 unzoomed)

    // The adaptive near radius of the living creatures (permanent, see UpdateNearRadius): shed_near_dist and
    // shed_near_seen_dist times m_near_scale, set at the start of ProcessStep on the Game thread and read by IsNear
    // (internal_Register on the main thread reads it too, but never at the same time - see UpdateRT)
    float m_near_scale{1.f};
    float m_near_dist{30.f};
    float m_near_seen_dist{60.f};
    u32 m_near_scan_cursor{}; // the round-robin scan of ItemsNear for objects that are no longer near (DemoteFarFromNear)
    // Items checked by the two scans so far (wrapping, only differences are used) and their values at the last change
    // of the radius: the count of the near creatures is trusted only once the scan in question has gone over its whole
    // heap since then
    u64 m_near_scanned{};
    u64 m_far_scanned{};
    u64 m_urgent_scanned{};
    u64 m_adapt_near_mark{};
    u64 m_adapt_far_mark{};
    u64 m_adapt_urgent_mark{};
    u32 m_adapt_time{}; // the last look of the controller (Device.dwTimeGlobal)
    u32 m_adapt_log_time{};
    float m_adapt_logged_scale{1.f};

    // 29-game-thread-next.md, the adaptive budget (see the comment at ps_shed_budget_min_ms in xrSheduler.cpp). The wait of the main thread for the Game thread (its join in a game/render overlap frame): an
    // EMA written by the main thread at the join (NoteGameWait) and read by the Game thread in the next ProcessStep -
    // the kick of that frame orders them; atomics only to keep it formally clean (no XR_PERF_METRICS needed)
    std::atomic<float> m_game_wait_ema_ms{};
    std::atomic<u32> m_game_wait_samples{}; // joins measured so far (wrapping): a new sample since the last pass
    std::atomic<bool> m_game_wait_restart{true}; // the next sample starts the EMA anew (set after a gap)
    u32 m_adapt_wait_seen{}; // m_game_wait_samples at the last pass (Game thread)
    u32 m_adapt_idle_passes{}; // passes in a row without a new sample (sequential frames, pause, loading)
    float m_budget_eff_ms{}; // the effective budget of the pass, ms (0 = not set yet: the base)

    IC static void Push(xr_vector<Item>& heap, Item& I);
    IC static void Pop(xr_vector<Item>& heap);
    static void HeapErase(xr_vector<Item>& heap, size_t index);
    void internal_Register(ISheduled* A, bool RT = false);
    bool internal_Unregister(ISheduled* A, bool RT, bool warn_on_not_found = true);
    void internal_Registration();
    void UpdateRTImpl(); // internal_Registration + ItemsRT; leaves m_processing_now == true
    void UpdateNonRTImpl(); // ProcessStep + adaptive budget + internal_Registration; clears m_processing_now
    bool ProcessingOnThisThread() const;
    // One step: run the due top item of `heap` (false: it was stale and only dropped, not a step). tier: the heap
    // it came from (ShedTier* in xrSheduler.cpp, the lateness metrics of the class).
    bool ExecuteTop(xr_vector<Item>& heap, u32 dwTime, u32 tier, shared_str& last_name);
    // keep: the test of the demotion scan - the creature radii a bit wider and "drawn" over a longer window, so that
    // a creature at the edge or hidden for a few frames does not flip between the heaps
    bool IsNear(const Item& item, bool keep = false) const;
    // Step 4d: a living creature of the near class that has to react now (Game thread, between the updates)
    static bool IsUrgent(const Item& item);
    // The near heap of a near item: ItemsUrgent if urgent, else ItemsNear
    xr_vector<Item>& NearHeapOf(const Item& item);
    u32 CountNearCreatures() const;
    void RebuildNearFar();
    void ScanUrgent();
    void ApplyNearRadius();
    void UpdateNearRadius(u32 now);
    void DemoteFarFromNear();
    void PromoteNearFromFar();
    // The effective time budget of this pass in ns (Game thread, once a pass)
    u64 AdaptBudget(u32 base_ms, bool timed);

public:
    // Main thread, right after it joined the Game thread of a game/render overlap frame: how long it waited (ns).
    // Feeds the adaptive budget; measured in every build, with or without the perf metrics
    void NoteGameWait(u64 wait_ns);
    void ProcessStep();
    void Process();
    // Classic single-call update: RT objects, then the normal heap. Used by a sequential frame
    // (XRay::Engine::GameThread); the order of calls is exactly UpdateRT() followed by UpdateNonRT().
    void Update();
    // Game/render overlap frame (MX f76479197 RT-split): the real-time part (the actor) on the main thread
    // before the Game thread starts, the rest on the Game thread in parallel with rendering.
    void UpdateRT();
    void UpdateNonRT();

#ifndef MASTER_GOLD
    bool Registered(ISheduled* object) const;
#endif
    void Register(ISheduled* A, bool RT = false);
    void Unregister(ISheduled* A);
    void EnsureOrder(ISheduled* Before, ISheduled* After);

    void Initialize();
    void Destroy();
    void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert);
    const CStatTimer& GetUpdateTime()
    {
        stats.FrameEnd();
        return stats.Update;
    }
};

// Console: scheduler_max_steps_per_frame — the former cap of shedule_Update calls per ProcessStep. No longer read by
// the scheduler (the time budget shed_budget_ms below replaced it); kept for the saved configs and the options menu.
ENGINE_API extern int ps_scheduler_max_steps_per_frame;

// 22-game-thread.md, step 4 (plans/optimization_mt): ProcessStep serves the near objects first, within a fixed time
// budget (see xrSheduler.cpp, the comment at ps_shed_budget_ms).
ENGINE_API extern int ps_shed_budget_ms; // time budget of the non-RT pass, ms
ENGINE_API extern int ps_shed_near_dist; // m from the camera: a living creature or a vehicle is near
ENGINE_API extern int ps_shed_near_seen_dist; // m from the camera: near if drawn recently
ENGINE_API extern int ps_shed_near_item_dist; // m from the camera: an item, a prop or a corpse is near (else far)
ENGINE_API extern int ps_shed_far_share; // % of the budget kept for the far heap while it has due items
ENGINE_API extern int ps_shed_near_min_steps; // near steps a frame even past the budget
ENGINE_API extern int ps_shed_max_steps; // safety cap of steps a frame (the time budget is the real limit)
ENGINE_API extern int ps_shed_scan; // far items checked a frame for coming near
// The near radius of the living creatures follows their count (see UpdateNearRadius): shed_near_dist /
// shed_near_seen_dist are the maxima
ENGINE_API extern int ps_shed_near_target; // the count of the living near creatures to keep
ENGINE_API extern int ps_shed_near_dist_min; // m: the adaptive plain radius never goes below it
// Step 4d: inside the near class those who have to react go first (the urgent near heap, see the comment at
// ps_shed_urgent_share in xrSheduler.cpp)
ENGINE_API extern int ps_shed_urgent_share; // % of the near budget the urgent heap may take while calm near ones are due
ENGINE_API extern int ps_shed_urgent_hit_ms; // ms: an NPC hit this recently is urgent (read by xrGame)
// 29-game-thread-next.md: the adaptive budget, always on (see the comment at ps_shed_budget_min_ms in xrSheduler.cpp):
// it cuts the budget while the main thread waits for the Game thread. Tunables, not saved
ENGINE_API extern int ps_shed_budget_min_ms; // ms: the adaptive budget never goes below it
ENGINE_API extern int ps_shed_budget_wait_target_us; // us: the wait of the main thread the controller aims at
ENGINE_API extern int ps_shed_budget_far_keep_pct; // % of the far window the cut leaves to the far heap
// Read-only for the console and the plugins (cvar shed_budget_scale_pct): 100 * effective / base budget of the last
// pass, written by the scheduler on the Game thread. gw_npc scales its own budgets by
// it (gw_npc_budget_adaptive)
ENGINE_API extern int ps_shed_budget_scale_pct;
