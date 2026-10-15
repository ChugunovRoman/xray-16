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
        // while Object is not null (the object is still registered): the near/far class of shed_priority (IsNear)
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

    // 22-game-thread.md, step 4 (plans/optimization_mt): shed_priority 1 splits the normal queue in two heaps by the
    // kind of the object and its distance to the camera (IsNear) - ItemsNear (served first, within the time budget) and Items (the far ones, the rest of
    // the budget and at least one step a frame). With shed_priority 0 ItemsNear is empty and Items is the only heap.
    xr_vector<Item> ItemsNear;
    bool m_priority_active{}; // the heaps are split (follows ps_shed_priority at the start of ProcessStep)
    u32 m_far_scan_cursor{}; // the round-robin scan of Items for objects that came near (PromoteNearFromFar)
    // The camera snapshot for the near/far class, taken in UpdateRTImpl: on the main thread right before the Game
    // thread is woken (overlap frame) or before the non-RT part on the same thread (sequential frame) - never the live
    // Device.vCameraPosition that the render may move while the Game thread runs.
    Fvector m_view_pos{};
    float m_view_zoom{1.f}; // distance factor of a zoomed camera (tan(fov/2) / tan(g_fov/2), 1 unzoomed)

    // The adaptive near radius of the living creatures (shed_near_adaptive, see UpdateNearRadius): shed_near_dist and
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
    u64 m_adapt_near_mark{};
    u64 m_adapt_far_mark{};
    u32 m_adapt_time{}; // the last look of the controller (Device.dwTimeGlobal)
    u32 m_adapt_log_time{};
    float m_adapt_logged_scale{1.f};

    IC static void Push(xr_vector<Item>& heap, Item& I);
    IC static void Pop(xr_vector<Item>& heap);
    static void HeapErase(xr_vector<Item>& heap, size_t index);
    IC Item& Top() { return Items.front(); }
    void internal_Register(ISheduled* A, bool RT = false);
    bool internal_Unregister(ISheduled* A, bool RT, bool warn_on_not_found = true);
    void internal_Registration();
    void UpdateRTImpl(); // internal_Registration + ItemsRT; leaves m_processing_now == true
    void UpdateNonRTImpl(); // ProcessStep + adaptive budget + internal_Registration; clears m_processing_now
    bool ProcessingOnThisThread() const;
    // One step: run the due top item of `heap` (false: it was stale and only dropped, not a step). tier: 1 near,
    // 0 far (the heap it came from), -1 classify here for the metrics only (shed_priority 0).
    bool ExecuteTop(xr_vector<Item>& heap, u32 dwTime, int tier, shared_str& last_name);
    // keep: the test of the demotion scan - the creature radii a bit wider and "drawn" over a longer window, so that
    // a creature at the edge or hidden for a few frames does not flip between the heaps
    bool IsNear(const Item& item, bool keep = false) const;
    u32 CountNearCreatures() const;
    void SetPriorityMode(bool on);
    void ApplyNearRadius();
    void UpdateNearRadius(u32 now);
    void DemoteFarFromNear();
    void PromoteNearFromFar();
    void ProcessStepPriority();

public:
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

// Console: scheduler_max_steps_per_frame — max shedule_Update calls per ProcessStep per frame (0 = unlimited).
// ProcessStep also stops when elapsed wall time exceeds floor(psShedulerCurrent) ms (skipped during precache), same scale as adaptive scheduler load.
ENGINE_API extern int ps_scheduler_max_steps_per_frame;

// 22-game-thread.md, step 4 (plans/optimization_mt). shed_priority (TEMPORARY A/B switch, not saved, default 0):
// near objects first and a fixed time budget instead of the adaptive one and the step limit above.
ENGINE_API extern int ps_shed_priority;
ENGINE_API extern int ps_shed_budget_ms; // time budget of the non-RT pass, ms
ENGINE_API extern int ps_shed_near_dist; // m from the camera: a living creature or a vehicle is near
ENGINE_API extern int ps_shed_near_seen_dist; // m from the camera: near if drawn recently
ENGINE_API extern int ps_shed_near_item_dist; // m from the camera: an item, a prop or a corpse is near (else far)
ENGINE_API extern int ps_shed_far_share; // % of the budget kept for the far heap while it has due items
ENGINE_API extern int ps_shed_near_min_steps; // near steps a frame even past the budget
ENGINE_API extern int ps_shed_max_steps; // safety cap of steps a frame (the time budget is the real limit)
ENGINE_API extern int ps_shed_scan; // far items checked a frame for coming near
ENGINE_API extern int ps_shed_services_near; // 1: ALife, agent managers and other non-object services are near
// Under shed_priority 1 only: the near radius of the living creatures follows their count (see UpdateNearRadius)
ENGINE_API extern int ps_shed_near_adaptive; // 1: shed_near_dist / shed_near_seen_dist are the maxima, the radius adapts
ENGINE_API extern int ps_shed_near_target; // the count of the living near creatures to keep
ENGINE_API extern int ps_shed_near_dist_min; // m: the adaptive plain radius never goes below it
