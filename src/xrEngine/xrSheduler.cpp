#include "stdafx.h"
#include "xrSheduler.h"
#include "xr_object.h"
#include "GameFont.h"
#include "PerformanceAlert.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <tracy/Tracy.hpp>

//#define DEBUG_SCHEDULER
//#define DEBUG_SCHEDULERMT

float psShedulerCurrent = 10.f;
float psShedulerTarget = 10.f;
const float psShedulerReaction = 0.1f;
bool isSheduleInProgress = false;

// Spread scheduler load: cap how many normal-priority shedule_Update calls run per frame (0 = vanilla, no cap).
ENGINE_API int ps_scheduler_max_steps_per_frame = 128;

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

u32 ShedCategoryOf(ISheduled* object)
{
    auto* go = dynamic_cast<IGameObject*>(object);
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
    item.dwPadding = ShedCategoryOf(object);

    if (realTime)
    {
        object->GetSchedulerData().b_RT = true;
        ItemsRT.emplace_back(std::move(item));
    }
    else
    {
        object->GetSchedulerData().b_RT = false;

        // Insert into priority Queue
        Push(item);
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
        for (auto& item : Items)
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

void CSheduler::Push(Item& item)
{
    Items.emplace_back(std::move(item));
    std::push_heap(Items.begin(), Items.end());
}

void CSheduler::Pop()
{
    std::pop_heap(Items.begin(), Items.end());
    Items.pop_back();
}

void CSheduler::ProcessStep()
{
    ZoneScopedN("CSheduler::ProcessStep");
    // shedule_Update is not parallelized: objects touch shared state and Register/Unregister the scheduler.

    // Normal priority
    const u32 dwTime = Device.dwTimeGlobal;
    u32 steps_done = 0;
    const u32 step_budget = ps_scheduler_max_steps_per_frame > 0 ? u32(ps_scheduler_max_steps_per_frame) : u32(-1);

    CTimer process_step_timer;
    process_step_timer.Start();
    const u32 time_budget_ms = _max(1u, u32(iFloor(psShedulerCurrent)));
    xray::perf::SetGauge(xray::perf::Metric::shed_budget, time_budget_ms);
    bool exit_limit = false;
    bool exit_time = false;

    auto check_process_step_time = [&](pcstr last_sched_name) -> bool
    {
        if (Device.dwPrecacheFrame != 0)
            return false;
        if (process_step_timer.GetElapsed_ms() <= time_budget_ms)
            return false;
        psShedulerTarget += psShedulerReaction * 3;
#ifndef MASTER_GOLD
        if (steps_done == 1 && last_sched_name)
            Msg("! [scheduler] ProcessStep time budget (~%u ms) exhausted after a single shedule_Update: \"%s\"",
                time_budget_ms, last_sched_name);
#endif
        return true;
    };

#ifdef DEBUG
    CTimer eTimer;
#endif

    {
        ZoneScopedN("CSheduler::ProcessStep/drain_normal_heap");
        while (!Items.empty() && Top().dwTimeForExecute < dwTime)
        {
        // Update
        Item item = Top();
        ShedLate(dwTime - item.dwTimeForExecute, item.dwPadding);

        if (!item.Object || !item.Object->shedule_Needed())
        {
#ifdef DEBUG_SCHEDULER
            Msg("SCHEDULER: process unregister [%s][%x][%s]", item.scheduled_name.c_str(), item.Object, "false");
#endif
            ZoneScopedN("CSheduler::ProcessStep/PopStale");
            PERF_INC(shed_pop_stale);
            // Erase element
            Pop();
            continue;
        }

        auto& schedulerData = item.Object->GetSchedulerData();

#ifdef DEBUG_SCHEDULER
        Msg("SCHEDULER: process step [%s][%x][false]", item.scheduled_name.c_str(), item.Object);
#endif

        // Insert into priority Queue
        Pop();

        u32 Elapsed = dwTime - item.dwTimeOfLastExecute;

        // Real update call
        // Msg("------- %d:", Device.dwFrame);
#ifdef DEBUG
        schedulerData.dbg_startframe = Device.dwFrame;
        eTimer.Start();
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
            item.Object->shedule_Update(
                clampr(Elapsed, u32(1), u32(_max(u32(schedulerData.t_max), u32(1000)))));
#if XR_PERF_METRICS
            if (m_current_step_obj)
                spike_info.object = item.Object;
#endif
        }
        if (!m_current_step_obj)
        {
#ifdef DEBUG_SCHEDULER
            Msg("SCHEDULER: process unregister (self unregistering) [%s][%x][%s]", item.scheduled_name.c_str(), item.Object,
                "false");
#endif
            ++steps_done;
            if (steps_done >= step_budget)
            {
                psShedulerTarget += psShedulerReaction * 3;
                exit_limit = true;
                break;
            }
            {
                pcstr sn = item.scheduled_name.c_str();
                if (!sn || !sn[0])
                    sn = "(no name)";
                if (check_process_step_time(sn))
                {
                    exit_time = true;
                    break;
                }
            }
            continue;
        }

        m_current_step_obj = nullptr;

        // Fill item structure
        pcstr sched_name_for_time = item.scheduled_name.c_str();
        if (!sched_name_for_time || !sched_name_for_time[0])
            sched_name_for_time = "(no name)";
        item.dwTimeForExecute = dwTime + dwUpdate;
        item.dwTimeOfLastExecute = dwTime;
        ItemsProcessed.emplace_back(std::move(item));
        ++steps_done;
        if (steps_done >= step_budget)
        {
            psShedulerTarget += psShedulerReaction * 3;
            exit_limit = true;
            break;
        }
        if (check_process_step_time(sched_name_for_time))
        {
            exit_time = true;
            break;
        }

#if 0 //def DEBUG
        auto itemName = item.Object->shedule_Name().c_str();
        const u32 delta_ms = dwTime - item.dwTimeForExecute;
        const u32 execTime = eTimer.GetElapsed_ms();
        VERIFY3(item.Object->dbg_update_shedule == item.Object->dbg_startframe,
            "Broken sequence of calls to 'shedule_Update'", itemName);

        if (delta_ms > 3 * dwUpdate)
            Msg("! xrSheduler: failed to shedule object [%s] (%dms)", itemName, delta_ms);

        if (execTime > 15)
            Msg("* xrSheduler: too much time consumed by object [%s] (%dms)", itemName, execTime);
#endif
        }
    }

    PERF_ADD(shed_steps, steps_done);
    if (exit_limit)
        PERF_INC(shed_exit_limit);
    else if (exit_time)
        PERF_INC(shed_exit_time);
    else
        PERF_INC(shed_exit_empty);

    if (xray::perf::g_perf_metrics_active)
    {
        u32 behind = 0;
        for (const auto& it : Items)
        {
            if (it.dwTimeForExecute < dwTime)
                ++behind;
        }
        xray::perf::SetGauge(xray::perf::Metric::shed_behind, behind);
    }

    // Push "processed" back
    {
        ZoneScopedN("CSheduler::ProcessStep/RequeueHeap");
        while (ItemsProcessed.size())
        {
            Push(ItemsProcessed.back());
            ItemsProcessed.pop_back();
        }
    }

    // always try to decrease target
    psShedulerTarget -= psShedulerReaction;
}

void CSheduler::UpdateRTImpl()
{
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
    xray::perf::SetGauge(xray::perf::Metric::shed_items, s64(Items.size()));
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
