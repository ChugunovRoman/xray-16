#pragma once

#include "xrCore/xrstring.h"
#include "xrCore/FTimer.h"

#include <atomic>
#include <mutex>
#include <thread>

class ISheduled;

class ENGINE_API CSheduler
{
private:
    struct Item
    {
        u32 dwTimeForExecute;
        u32 dwTimeOfLastExecute;
        shared_str scheduled_name;
        ISheduled* Object;
        u32 dwPadding; // for align-issues

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

    IC void Push(Item& I);
    IC void Pop();
    IC Item& Top() { return Items.front(); }
    void internal_Register(ISheduled* A, bool RT = false);
    bool internal_Unregister(ISheduled* A, bool RT, bool warn_on_not_found = true);
    void internal_Registration();
    void UpdateRTImpl(); // internal_Registration + ItemsRT; leaves m_processing_now == true
    void UpdateNonRTImpl(); // ProcessStep + adaptive budget + internal_Registration; clears m_processing_now
    bool ProcessingOnThisThread() const;

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
