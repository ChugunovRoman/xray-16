#include "StdAfx.h"

#include "PHWorld.h"
#include "tri-colliderknoopc/dTriList.h"
#include "PhysicsCommon.h"

#include "ExtendedGeom.h"
#include "dRayMotions.h"
#include "PHCollideValidator.h"

#include "params.h"
#ifdef DEBUG
#include "debug_output.h"
#endif

#include "xrServerEntities/PHSynchronize.h"
#include "xrServerEntities/PHNetState.h"
#include "GeometryBits.h"
#include "console_vars.h"
#include "PHCommander.h"
#include "PHSimpleCalls.h"

#include "xrCore/FS_internal.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCore/Threading/TaskManager.hpp"
#include "xrCore/Threading/ThreadUtil.h"

#include "xrCDB/xr_area.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrEngine/defines.h"
#include "xrEngine/device.h"
#include "xrEngine/GameFont.h"
#include "xrEngine/PerformanceAlert.hpp"
#include "xrEngine/xr_object.h"

#include "IPhysicsShellHolder.h"
#include "PHCharacter.h"
#include "PHShell.h"

#ifdef DEBUG
//				void DBG_ObjAfterPhDataUpdate	( CPHObject *obj );
//				void DBG_ObjBeforePhDataUpdate	( CPHObject *obj );
//				void DBG_ObjAfterStep			( CPHObject *obj );
//				void DBG_ObjBeforeStep			( CPHObject *obj );
//				void DBG_ObjeAfterPhTune		( CPHObject *obj );
//				void DBG_ObjBeforePhTune		( CPHObject *obj );
//				void DBG_ObjAfterCollision		( CPHObject *obj );
//				void DBG_ObjBeforeCollision		( CPHObject *obj );
#endif
//////////////////////////////////////////////////////////////
//////////////CPHMesh///////////////////////////////////////////
///////////////////////////////////////////////////////////
// BOOL		g_bDebugDumpPhysicsStep				= 0;
CPHWorld* ph_world = 0;

IPHWorld* physics_world() { return ph_world; }
void create_physics_world(
    bool mt, CObjectSpace* os, CObjectList* lo)
{
    ZoneScoped;
    ph_world = xr_new<CPHWorld>();
    VERIFY(os);
    //		VERIFY( lo );
    ph_world->Create(mt, os, lo);
}

void destroy_physics_world()
{
    ZoneScoped;
    ph_world->Destroy();
    xr_delete(ph_world);
}

void destroy_object_space(CObjectSpace*& os) { xr_delete(os); }
void CPHMesh::Create(dSpaceID space, dWorldID world)
{
    Geom = dCreateTriList(space, 0, 0);
    CPHGeometryBits::init_geom(*this);
}
/////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////

void CPHMesh::Destroy()
{
    dGeomDestroy(Geom);
    dTriListClass = -1;
}

////////////////////////////////////////////////////////////////////////////
///////////CPHWorld/////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
//#define PH_PLAIN
#ifdef PH_PLAIN
dGeomID plane;
#endif

#ifdef DEBUG

void CPHWorld::OnRender() { debug_output().PH_DBG_Render(); }
#endif

CPHWorld::CPHWorld()
    : m_object_space(0), m_level_objects(0), m_default_contact_shotmark(0),
      m_default_character_contact_shotmark(0), physics_step_time_callback(0)
{
    disable_count = 0;
    m_frame_time = 0.f;
    m_previous_frame_time = 0.f;
    b_frame_mark = false;
    m_steps_num = 0;
    m_steps_short_num = 0;
    m_frame_sum = 0.f;
    m_delay = 0;
    m_previous_delay = 0;
    m_reduce_delay = 0;
    m_update_delay_count = 0;
    b_world_freezed = false;
    b_processing = false;
    m_gravity = default_world_gravity;
    b_exist = false;
}
void CPHWorld::SetStep(float s)
{
    fixed_step = s;
    world_cfm = CFM(SPRING_S(base_cfm, base_erp, base_fixed_step), DAMPING(base_cfm, base_erp));
    world_erp = ERP(SPRING_S(base_cfm, base_erp, base_fixed_step), DAMPING(base_cfm, base_erp));
    world_spring = 1.0f * SPRING(world_cfm, world_erp);
    world_damping = 1.0f * DAMPING(world_cfm, world_erp);
    if (ph_world && ph_world->Exist())
    {
        float frame_time = Device.fTimeDelta;
        u32 it_number = iFloor(frame_time / fixed_step);
        frame_time -= it_number * fixed_step;
        ph_world->m_previous_frame_time = frame_time;
        ph_world->m_frame_time = frame_time;
    }
}
void CPHWorld::Create(bool mt, CObjectSpace* os, CObjectList* lo)
{
    ZoneScoped;

    LoadParams();
    dWorldID phWorld = 0;
    m_object_space = os;
    m_level_objects = lo;
    Device.AddSeqFrame(this, mt);
    // B-1 / Phase 3: overlap mode runs the step from ProcessFrame (after the GameThread launch) via
    // these callbacks - see KickPhysicsOverlap()/JoinPhysicsOverlap() for what each mode does.
    Device.PhysicsBeginOverlapFrameCallback = [this] { BeginSchedulerOverlapFrame(); };
    Device.PhysicsKickOverlapCallback = [this] { KickPhysicsOverlap(); };
    Device.PhysicsWaitOverlapCallback = [this] { WaitPhysicsStep(); };
    Device.PhysicsJoinOverlapCallback = [this] { JoinPhysicsOverlap(); };
    m_commander = xr_new<CPHCommander>();
    m_script_commander = xr_new<CPHCommander>();

// dVector3 extensions={2048,256,2048};
/*
Fbox	level_box		=	Level().ObjectSpace.GetBoundingVolume();
Fvector level_size,level_center;
level_box				.	getsize		(level_size);
level_box				.	getcenter	(level_center);
dVector3 extensions		=	{ level_size.x ,256.f,level_size.z};
dVector3 center			=	{level_center.x,0.f,level_center.z};
*/

#ifdef ODE_SLOW_SOLVER
#else

    dWorldSetAutoEnableDepthSF1(phWorld, 100000000);
/// dWorldSetContactSurfaceLayer(phWorld,0.f);
// phWorld->contactp.min_depth =0.f;

#endif
    ContactGroup = dJointGroupCreate(0);
    dWorldSetGravity(phWorld, 0, -Gravity(), 0); //-2.f*9.81f
    Mesh.Create(0, phWorld);
#ifdef PH_PLAIN
    plane = dCreatePlane(Space, 0, 1, 0, 0.3f);
#endif

    // const  dReal k_p=2400000.f;//550000.f;///1000000.f;
    // const dReal k_d=200000.f;
    dWorldSetERP(phWorld, ERP(world_spring, world_damping));
    dWorldSetCFM(phWorld, CFM(world_spring, world_damping));
    // dWorldSetERP(phWorld,  0.2f);
    // dWorldSetCFM(phWorld,  0.000001f);
    disable_count = 0;
    m_motion_ray = dCreateRayMotions(0);
    phBoundaries.set(inl_ph_world().ObjectSpace().GetBoundingVolume());
    phBoundaries.y1 -= 30.f;
    CPHCollideValidator::Init();
    b_exist = true;

    StepNumIterations(phIterations);
    SetStep(ph_console::ph_step_time);
}

/////////////////////////////////////////////////////////////////////////////

void CPHWorld::Destroy()
{
    ZoneScoped;

    // Phase 3: stop the dedicated physics thread first, before anything it could touch (Mesh,
    // m_commander, ODE world) is torn down below. By the time Destroy() runs the frame cycle for
    // this CPHWorld is normally not running (ProcessFrame always joins via PhysicsJoinOverlapCallback
    // before returning), so the thread should already be parked in m_physics_wake.Wait() - the waits
    // here are a safety net, not the normal per-frame path.
    WaitPhysicsStep();
    // Deferred game-side notifications target objects that are being destroyed with the level.
    m_deferred_processing_activate.clear();
    if (m_physics_thread.joinable())
    {
        m_physics_thread_exit.store(true, std::memory_order_release);
        m_physics_wake.Set();
        m_physics_thread.join();
        m_physics_thread_exit.store(false, std::memory_order_release);
    }

    xr_delete(m_commander);
    xr_delete(m_script_commander);
    Mesh.Destroy();
#ifdef PH_PLAIN
    dGeomDestroy(plane);
#endif
#ifdef DEBUG
    debug_output().PH_DBG_Clear();
#endif
    dGeomDestroy(m_motion_ray);
    dJointGroupEmpty(ContactGroup);
    dJointGroupDestroy(ContactGroup);
    ContactFeedBacks.clear();
    ContactEffectors.clear();
    dCloseODE();
    dCylinderClassUser = -1;
    dRayMotionsClassUser = -1;
    Device.PhysicsBeginOverlapFrameCallback = nullptr;
    Device.PhysicsKickOverlapCallback = nullptr;
    Device.PhysicsWaitOverlapCallback = nullptr;
    Device.PhysicsJoinOverlapCallback = nullptr;
    Device.RemoveSeqFrame(this);
    b_exist = false;
}
void CPHWorld::SetGravity(float g)
{
    m_gravity = g;
    dWorldID phWorld = 0;
    dWorldSetGravity(phWorld, 0, -m_gravity, 0); //-2.f*9.81f
}

void CPHWorld::OnFrame()
{
    // B-1 / Phase 3: the step always runs after the GameThread scheduler launch, via
    // Device.PhysicsKickOverlapCallback (set in Create) - on CPHWorld's dedicated physics thread,
    // parallel to DoRender(). This seqFrame callback is kept registered (Device.AddSeqFrame in Create)
    // but intentionally never steps anymore, to avoid double-stepping.
}

void CPHWorld::OnFrameStep()
{
    ZoneScoped;
    stats.FrameStart();
#ifdef DEBUG
// DBG_DrawFrameStart();
// DBG_DrawStatBeforeFrameStep();
#endif
    stats.MovCollision.Begin();
    FrameStep(Device.fTimeDelta);
    stats.MovCollision.End();
#ifdef DEBUG
// DBG_DrawStatAfterFrameStep();

#endif
    stats.FrameEnd();
}

void CPHWorld::BeginSchedulerOverlapFrame()
{
    m_scheduler_overlap_active.store(true, std::memory_order_release);
}

bool CPHWorld::ShouldDeferSchedulerPhysicsMutation() const
{
    if (!m_scheduler_overlap_active.load(std::memory_order_acquire))
        return false;
    // B-1 fix: a step must actually be IN FLIGHT to need deferring. Without this check,
    // flush_deferred_mutations() itself (called from JoinPhysicsOverlap() on the main thread, i.e.
    // this_thread::get_id() != m_step_thread_id == the dedicated physics thread, AFTER
    // m_physics_done.Wait() already returned) kept re-deferring its own queued ops forever: the
    // deferred action (e.g. CPHMovementControl::ApplyDeferredBoxActivation -> ActivateBoxDynamic, used
    // by crouch/walk-toggle) re-checks this predicate on every call and, seeing a non-matching thread
    // id, requeues itself instead of actually running - a permanent stall. m_step_running covers the
    // whole Kick..WaitPhysicsStep window (set on the main thread before the wake, cleared after
    // m_physics_done) - once it's false, nothing is concurrently stepping, so mutating from any thread
    // (including the post-join flush) is safe and must not defer again.
    if (!m_step_running.load(std::memory_order_acquire))
        return false;
    // Whichever thread runs FrameStep (CPHWorld's dedicated physics thread) is stable across the whole
    // window - only defer mutations from OTHER threads (GameThread scheduler, or render-path code
    // running on main while the physics thread is mid-step).
    return std::this_thread::get_id() != m_step_thread_id;
}

bool CPHWorld::defer_scheduler_mutation(std::function<void()> op)
{
    if (!ShouldDeferSchedulerPhysicsMutation())
        return false;
    std::lock_guard<std::mutex> guard(m_deferred_mutations_lock);
    m_deferred_mutations.emplace_back(std::move(op));
    return true;
}

const Fvector& CPHWorld::StepCameraPosition() const
{
    return m_step_running.load(std::memory_order_acquire) ? m_step_camera_pos : Device.vCameraPosition;
}

bool CPHWorld::IsPhysicsStepThread() const
{
    return m_step_running.load(std::memory_order_acquire) && std::this_thread::get_id() == m_step_thread_id;
}

void CPHWorld::flush_deferred_mutations()
{
    // Called on the main thread from JoinPhysicsOverlap(), after the step finished - apply owner-level
    // intents serially. Game-side activations first: they only re-register objects for processing.
    if (!m_deferred_processing_activate.empty())
    {
        ZoneScopedN("PhysicsDeferredProcessingActivate");
        xr_vector<IPhysicsShellHolder*> activations;
        activations.swap(m_deferred_processing_activate);
        for (IPhysicsShellHolder* holder : activations)
            holder->ObjectProcessingActivate();
    }
    xr_vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> guard(m_deferred_mutations_lock);
        if (m_deferred_mutations.empty())
        {
            m_scheduler_overlap_active.store(false, std::memory_order_release);
            return;
        }
        local.swap(m_deferred_mutations);
    }
    ZoneScopedN("PhysicsDeferredMutations");
    for (auto& op : local)
        op();
    m_scheduler_overlap_active.store(false, std::memory_order_release);
}

// Phase 3: body of m_physics_thread. By analogy with the SVP worker (r2_R_calculate.cpp's
// svp_worker_loop) - auto-reset Event handshake, no mid-job abort (a physics step always runs to
// completion, unlike SVP's lights stage). Thread name + per-thread FPU setup are done by
// Threading::RunThread() (KickPhysicsOverlap below), not here - that also calls
// _initialize_cpu_thread() (denormals-are-zero / xrDebug::OnThreadSpawn), which svp_worker_loop
// skips; physics is dense enough float work that this is worth not skipping too.
void CPHWorld::PhysicsWorkerLoop()
{
    for (;;)
    {
        m_physics_wake.Wait();
        if (m_physics_thread_exit.load(std::memory_order_acquire))
        {
            m_physics_done.Set(); // never leave a waiter hanging, whatever the call order
            return;
        }
        OnFrameStep();
        m_physics_done.Set();
    }
}

void CPHWorld::KickPhysicsOverlap()
{
    VERIFY(!m_physics_in_flight);
    // Lazy, one-time start - like svp_build_thread, not recreated every frame.
    if (!m_physics_thread.joinable())
        m_physics_thread = Threading::RunThread("Physics", [this] { PhysicsWorkerLoop(); });
    // Everything the step reads from the main thread's frame state is published before the wake (the
    // Event gives the physics thread a happens-before on these plain stores).
    m_step_camera_pos = Device.vCameraPosition;
    m_steps_num_at_kick = m_steps_num;
    m_step_thread_id = m_physics_thread.get_id();
    m_step_running.store(true, std::memory_order_release);
    m_physics_in_flight = true;
    m_physics_done.Reset();
    m_physics_wake.Set();
}

void CPHWorld::WaitPhysicsStep()
{
    // Only wait for this frame's job, not thread.join() - by analogy with
    // EndSecondVPCalculateParallel(). Full thread teardown is Destroy()'s job.
    if (!m_physics_in_flight)
        return;
    m_physics_done.Wait();
    m_physics_in_flight = false;
    m_step_running.store(false, std::memory_order_release);
}

void CPHWorld::JoinPhysicsOverlap()
{
    WaitPhysicsStep();
    flush_deferred_mutations();
    // Lua-wrapping physics_world():add_call() entries (see AddCall): drained here on the main thread, once
    // per frame that actually stepped (FrameStep may run zero substeps, e.g. paused or a short frame).
    if (m_steps_num != m_steps_num_at_kick)
        m_script_commander->update_threadsafety();
}

void CPHWorld::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    stats.FrameEnd();
    float engineTotal = Device.GetStats().EngineTotal.result;
    float percentage = 100.0f * stats.MovCollision.result / engineTotal;
    font.OutNext("Physics:      %2.2fms, %2.1f%%", stats.MovCollision.result, percentage);
    font.OutNext("- collider:   %2.2fms", stats.Collision.result);
    font.OutNext("- solver:     %2.2fms, %d", stats.Core.result, stats.Core.count);
    if (alert && stats.MovCollision.result > 5.0f)
        alert->Print(font, "Physics   > 5ms:  %3.1f", stats.MovCollision.result);
}

//////////////////////////////////////////////////////////////////////////////
// static dReal frame_time=0.f;
static u32 start_time = 0;
void CPHWorld::Step()
{
    // P1 (plans/optimization_spawn): object count under this Step() drives how many q_box queries it
    // issues (one CollideDynamicsBroadphase per object) — see ISpatial_DB::q_box call volume during spawn.
    ZoneNamedN(___tracy_ph_world_step, "CPHWorld::Step", true);
    ZoneTextVF(___tracy_ph_world_step, "objects=%u freezed=%d", (unsigned)m_objects.count(), (int)IsFreezed());

#ifdef DEBUG
    debug_output().dbg_reused_queries_per_step() = 0;
    debug_output().dbg_new_queries_per_step() = 0;
#endif

    VERIFY(b_processing || IsFreezed());

    PH_OBJECT_I i_object;
    PH_UPDATE_OBJECT_I i_update_object;

    if (disable_count == 0)
    {
        disable_count = worldDisablingParams.objects_params.L2frames;
        for (i_object = m_recently_disabled_objects.begin(); m_recently_disabled_objects.end() != i_object;)
        {
            CPHObject* obj = (*i_object);
            obj->check_recently_deactivated();
            ++i_object;
        }
    }
    if (!IsFreezed())
        --disable_count;

    ++m_steps_num;
    ++m_steps_short_num;
    stats.Collision.Begin();

    {
        ZoneScopedN("ph_collide");
        PERF_SCOPE(ph_collide);
        // Collide must stay on one thread: q_box + smart_cast + NearCallback/ODE must not span a gap where
        // another thread can spatial_unregister or destroy ISpatial* seen in the query (parallel broadphase
        // alone caused use-after-free / invalid vptr in smart_cast).
        for (i_object = m_objects.begin(); m_objects.end() != i_object;)
        {
            CPHObject* obj = (*i_object);
#ifdef DEBUG
            debug_output().DBG_ObjBeforeCollision(obj);
#endif
            obj->Collide();
#ifdef DEBUG
            debug_output().DBG_ObjAfterCollision(obj);
#endif
            ++i_object;
        }
    }

    stats.Collision.End();

#ifdef DEBUG
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        if (debug_output().ph_dbg_draw_mask().test(phDbgDrawEnabledAABBS))
            debug_output().DBG_DrawPHObject(obj);
        ++i_object;
    }
#endif

    {
        ZoneScopedN("ph_phtune");
        PERF_SCOPE(ph_tune);
        for (i_object = m_objects.begin(); m_objects.end() != i_object;)
        {
            CPHObject* obj = (*i_object);
            ++i_object;

#ifdef DEBUG
            debug_output().DBG_ObjBeforePhTune(obj);
#endif

            obj->PhTune(fixed_step);

#ifdef DEBUG
            debug_output().DBG_ObjeAfterPhTune(obj);
#endif
        }

        for (i_update_object = m_update_objects.begin(); m_update_objects.end() != i_update_object;)
        {
            CPHUpdateObject* obj = (*i_update_object);
            ++i_update_object;
            obj->PhTune(fixed_step);
        }
    }

    stats.Core.Begin();

    {
        ZoneScopedN("ph_solve");
        PERF_SCOPE(ph_solve);
#ifdef DEBUG
        debug_output().dbg_bodies_num() = 0;
        debug_output().dbg_joints_num() = 0;
        debug_output().dbg_islands_num() = 0;
#endif
        //////////////////////////////////////////////////////////////////////
        m_commander->update_threadsafety();
        //////////////////////////////////////////////////////////////////////
        // P1: parallel island solve. Each active CPHIsland is an independent dxWorld, and the ODE
        // stepper uses only stack/local scratch, so solving distinct active islands on worker threads
        // is race-free. Collide (above) and PhDataUpdate/spatial_move (below) stay serial.
#ifdef DEBUG
        // DEBUG path stays serial: debug_output() touches global state.
        for (i_object = m_objects.begin(); m_objects.end() != i_object;)
        {
            CPHObject* obj = (*i_object);
            ++i_object;
            if (debug_output().ph_dbg_draw_mask().test(phDbgDrawObjectStatistics))
            {
                if (obj->Island().IsActive())
                {
                    debug_output().dbg_islands_num()++;
                    debug_output().dbg_joints_num() += obj->Island().nj;
                    debug_output().dbg_bodies_num() += obj->Island().nb;
                }
            }
            debug_output().DBG_ObjBeforeStep(obj);
            obj->IslandStep(fixed_step);
            debug_output().DBG_ObjAfterStep(obj);
        }
#else
        // Snapshot active-island roots. Valid only here: after Collide (merges done) and before
        // IslandReinit (Unmerge). Merged-away members report IsActive()==false and are skipped;
        // each active root steps its own merged dxWorld exactly once -> no cross-thread aliasing.
        m_island_solve_batch.clear();
        // Ф2 (PHYSICS_ACTIVE_SET_PLAN, distance-based corpse sleep): same O(N) pass - deliberately crude,
        // no velocity/settle check and no island-wide synchronization (that required every member of a
        // merged group to qualify at once, which in a busy fight almost never happened - any live NPC
        // transiently merged into the same island via contact blocked the whole group forever). Just:
        // old enough + far enough from the camera -> force-disable, full stop. A far corpse not reacting
        // correctly to a later impact is preferable to paying its simulation cost every step.
        const bool corpse_sleep_enabled = ph_console::ph_corpse_sleep_ms > 0;
        if (corpse_sleep_enabled)
            m_corpses_to_sleep.clear();
        for (i_object = m_objects.begin(); m_objects.end() != i_object; ++i_object)
        {
            CPHObject* obj = (*i_object);
            if (obj->Island().IsActive())
                m_island_solve_batch.push_back(obj);

            if (corpse_sleep_enabled && CPHCollideValidator::IsRagDollObject(*obj) &&
                ShouldForceSleepCorpse(obj))
            {
                m_corpses_to_sleep.push_back(obj);
            }
        }

        const size_t island_count = m_island_solve_batch.size();
        if (ph_console::ph_mt_island_solve != 0 && TaskScheduler &&
            island_count >= size_t(ph_console::ph_mt_island_min))
        {
            ZoneScopedN("ph_IslandStep_mt");
            CPHObject** const islands = m_island_solve_batch.data();
            xr_parallel_for(TaskRange(size_t(0), island_count), [islands](const TaskRange<size_t>& range)
            {
                for (size_t i = range.begin(); i != range.end(); ++i)
                    islands[i]->IslandStep(fixed_step);
            });
        }
        else
        {
            for (CPHObject* obj : m_island_solve_batch)
                obj->IslandStep(fixed_step);
        }
#endif
    }

    stats.Core.End();

    {
        ZoneScopedN("ph_post_integrate");
        PERF_SCOPE(ph_post_integrate);
        for (i_object = m_objects.begin(); m_objects.end() != i_object;)
        {
            CPHObject* obj = (*i_object);
            ++i_object;
            obj->IslandReinit();

#ifdef DEBUG
            debug_output().DBG_ObjBeforePhDataUpdate(obj);
#endif

            obj->PhDataUpdate(fixed_step);

#ifdef DEBUG
            debug_output().DBG_ObjAfterPhDataUpdate(obj);
#endif

            obj->spatial_move();
        }

        for (i_update_object = m_update_objects.begin(); m_update_objects.end() != i_update_object;)
        {
            CPHUpdateObject* obj = *i_update_object;
            ++i_update_object;
            obj->PhDataUpdate(fixed_step);
        }

        ReinitMerged();
    }

    // Ф2 (PHYSICS_ACTIVE_SET_PLAN, distance-based corpse sleep): must run after ReinitMerged() above, not
    // inside the post-Collide/pre-reinit window where m_corpses_to_sleep was collected - deactivate()
    // asserts the island is not mid-merge, which is only true once Unmerge() has run on this object.
    if (ph_console::ph_corpse_sleep_ms > 0)
    {
        ZoneScopedN("ph_corpse_sleep");
        PERF_SCOPE(ph_corpse_sleep_apply);
        SleepCorpses();
    }

#ifdef DEBUG
    debug_output().dbg_contacts_num() = ContactGroup->num;
#endif
    {
        ZoneScopedN("ph_contact_cleanup");
        PERF_SCOPE(ph_contact_cleanup);
        dJointGroupEmpty(ContactGroup); // this is to be called after PhDataUpdate!!!-the order is critical!!!
        ContactFeedBacks.empty();
        ContactEffectors.empty();
    }

    if (physics_step_time_callback)
    {
        physics_step_time_callback(start_time, start_time + u32(fixed_step * 1000));
        start_time += u32(fixed_step * 1000);
    };
}

void CPHWorld::StepTouch()
{
    // P1 (plans/optimization_spawn): see CPHWorld::Step — same q_box volume concern.
    ZoneNamedN(___tracy_ph_world_step_touch, "CPHWorld::StepTouch", true);
    ZoneTextVF(___tracy_ph_world_step_touch, "objects=%u", (unsigned)m_objects.count());

    PH_OBJECT_I i_object;
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        obj->Collide();

        ++i_object;
    }

    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;
        obj->Island().Enable();
    }
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        ++i_object;
        obj->IslandReinit();
        obj->spatial_move();
    }
    ReinitMerged();
    dJointGroupEmpty(ContactGroup);
    ContactFeedBacks.empty();
    ContactEffectors.empty();
}

void CPHWorld::RegisterMerged(CPHObject* obj)
{
    VERIFY(obj);
    m_merged_objects.push_back(obj);
}

void CPHWorld::ReinitMerged()
{
    for (CPHObject* obj : m_merged_objects)
        obj->IslandReinit();
    m_merged_objects.clear();
}

bool CPHWorld::ShouldForceSleepCorpse(CPHObject* obj)
{
    // Ф2 (PHYSICS_ACTIVE_SET_PLAN, distance-based corpse sleep): deliberately crude - no "settled for N ms"
    // wait, no island-wide synchronization. Old enough + far enough from the camera, plus two cheap guards
    // against freezing a broken pose (see below). safe cast: cbClassRagDoll is only ever set by
    // CPHShell::SetRagDoll().
    CPHShell* shell = static_cast<CPHShell*>(obj);
    const u32 age_ms = Device.dwTimeGlobal - shell->m_ragdoll_since_ms;
    if (age_ms < static_cast<u32>(ph_console::ph_corpse_sleep_ms))
        return false;
    IPhysicsShellHolder* holder = shell->PhysicsRefObject();
    if (!holder)
        return false; // нет привязки к игровому объекту - не трогаем
    Fvector to_cam;
    to_cam.sub(holder->ObjectPosition(), StepCameraPosition()); // snapshot: DoRender() may be overriding the live camera
    const float far_dist_sq = ph_console::ph_corpse_sleep_far_dist * ph_console::ph_corpse_sleep_far_dist;
    if (to_cam.square_magnitude() <= far_dist_sq)
        return false;
    // After an explosion the elements fly apart and ODE pulls them back to the joints over the next steps
    // (fast corrective motion). Disabling during that window froze the ragdoll mid-air or with metres-long
    // limbs/neck forever. Distance/age are checked first: these walk all elements/joints of the shell.
    const float max_stretch = ph_console::ph_corpse_sleep_max_stretch;
    if (max_stretch > 0.f && shell->get_MaxJointStretchSq() > max_stretch * max_stretch)
        return false;
    const float max_vel = ph_console::ph_corpse_sleep_max_vel;
    if (max_vel > 0.f && shell->get_MaxElementLinearVelSq() > max_vel * max_vel)
        return false;
    return true;
}

void CPHWorld::SleepCorpses()
{
    // Called after ReinitMerged() (see Step()) - deactivate()'s VERIFY2(m_island.IsActive()) only holds
    // once Unmerge() has run for this object this step.
    for (CPHObject* obj : m_corpses_to_sleep)
    {
        CPHShell* shell = static_cast<CPHShell*>(obj);
        shell->Disable();
        shell->m_force_slept = true;
        PERF_INC(ph_corpse_force_slept);
    }
    m_corpses_to_sleep.clear();
}

u32 CPHWorld::CalcNumSteps(u32 dTime)
{
    if (dTime < m_frame_time * 1000)
        return 0;
    u32 res = iCeil((float(dTime) - m_frame_time * 1000) / (fixed_step * 1000));
    //	if (dTime < fixed_step*1000) return 0;
    //	u32 res = iFloor((float(dTime) / 1000 / fixed_step)+0.5f);
    return res;
};

void CPHWorld::FrameStep(dReal step)
{
    if (IsFreezed())
        return;

    VERIFY(_valid(step));
    step *= phTimefactor;
    // compute contact joints and forces

    // step+=astep;

    // const  dReal k_p=24000000.f;//550000.f;///1000000.f;
    // const dReal k_d=400000.f;
    u32 it_number;
    float frame_time = m_frame_time;
    frame_time += step;
// m_frame_sum+=step;
#ifdef DEBUG
    if (debug_output().ph_dbg_draw_mask().test(phDbgDrawObjectStatistics))
    {
        static float dbg_iterations = 0.f;
        dbg_iterations = dbg_iterations * 0.9f + step / fixed_step * 0.1f;
        b_processing = true;
        debug_output().DBG_OutText("phys steps per frame %2.1f", dbg_iterations);
        b_processing = false;
    }
#endif
    if (!(frame_time < fixed_step))
    {
        it_number = iFloor(frame_time / fixed_step);
        frame_time -= it_number * fixed_step;
        // Anti death-spiral: frame_time already drained the full backlog above, so capping it_number
        // here DISCARDS the excess simulated time (physics runs slightly slow under extreme load instead
        // of spiraling: slow frame -> more substeps -> slower frame). 0 = uncapped (legacy) for A/B.
        if (ph_console::ph_max_substeps > 0 && it_number > u32(ph_console::ph_max_substeps))
        {
            // it_number before the clamp is otherwise lost: ph_substeps below reports only what actually ran,
            // so without this there is no way to tell a 1-step clamp from a 50-step death-spiral clamp.
            PERF_ADD(ph_substeps_discarded, it_number - u32(ph_console::ph_max_substeps));
            it_number = u32(ph_console::ph_max_substeps);
            PERF_INC(ph_substeps_clipped);
        }
        PERF_ADD(ph_substeps, it_number);
        m_previous_frame_time = m_frame_time;
        m_frame_time = frame_time;
        b_frame_mark = !b_frame_mark;
    }
    else
    {
        m_frame_time = frame_time;
        return;
    }
// for(UINT i=0;i<(m_reduce_delay+1);++i)
#ifdef DEBUG
    debug_output().DBG_DrawFrameStart();
    debug_output().DBG_DrawStatBeforeFrameStep();
#endif
    b_processing = true;
    // m_step_thread_id / m_step_running are owned by KickPhysicsOverlap()/WaitPhysicsStep() on the main
    // thread (they span the whole Kick..Wait window, not just this function).

    start_time = Device.dwTimeGlobal; // - u32(m_frame_time*1000);
    if (ph_console::g_bDebugDumpPhysicsStep && it_number > 20)
        Msg("!!! TOO MANY PHYSICS STEPS PER FRAME = %d !!!", it_number);
    for (u32 i = 0; i < it_number; ++i)
    {
        if (i == 0 && xray::perf::g_perf_metrics_active)
        {
            u32 n_actor = 0, n_ai = 0, n_ragdoll = 0, n_vehicle = 0, n_item = 0, n_static = 0, n_other = 0;
            for (PH_OBJECT_I it = m_objects.begin(); it != m_objects.end(); ++it)
            {
                CPHObject* obj = *it;
                const CPHObject::ECastType tp = obj->CastType();
                if (tp == CPHObject::tpCharacter)
                {
                    CPHCharacter* ch = static_cast<CPHCharacter*>(obj);
                    if (ch->CastActorCharacter())
                        ++n_actor;
                    else if (ch->CastAICharacter())
                        ++n_ai;
                    else
                        ++n_other;
                }
                else if (CPHCollideValidator::IsRagDollObject(*obj))
                    ++n_ragdoll;
                else if (tp == CPHObject::tpStaticShell)
                    ++n_static;
                else if (tp == CPHObject::tpShell)
                {
                    IPhysicsShellHolder* holder = static_cast<CPHShell*>(obj)->PhysicsRefObject();
                    IGameObject* go = holder ? holder->IObject() : nullptr;
                    if (go && go->cast_holder_custom())
                        ++n_vehicle;
                    else if (holder && holder->IsInventoryItem())
                        ++n_item;
                    else
                        ++n_other;
                }
                else
                    ++n_other;
            }
            PERF_ADD(ph_obj_actor, n_actor);
            PERF_ADD(ph_obj_ai, n_ai);
            PERF_ADD(ph_obj_ragdoll, n_ragdoll);
            PERF_ADD(ph_obj_vehicle, n_vehicle);
            PERF_ADD(ph_obj_item, n_item);
            PERF_ADD(ph_obj_static, n_static);
            PERF_ADD(ph_obj_other, n_other);
            xray::perf::SetGauge(xray::perf::Metric::ph_update_objects, m_update_objects.count());
            xray::perf::SetGauge(xray::perf::Metric::ph_recently_disabled, m_recently_disabled_objects.count());
            xray::perf::SetGauge(xray::perf::Metric::ph_frozen, m_freezed_objects.count());
        }
        Step();
        if (i == 0 && xray::perf::g_perf_metrics_active)
            xray::perf::SetGauge(xray::perf::Metric::ph_island_batch, static_cast<s64>(m_island_solve_batch.size()));
    }
    b_processing = false;
#ifdef DEBUG
    debug_output().DBG_DrawStatAfterFrameStep();
#endif
}

void CPHWorld::AddObject(CPHObject* object)
{
    m_objects.push_back(object);
    // xr_list <CPHObject*> ::iterator i= m_objects.end();
    // return (--m_objects.end());
};
void CPHWorld::AddRecentlyDisabled(CPHObject* object) { m_recently_disabled_objects.push_back(object); }
void CPHWorld::RemoveFromRecentlyDisabled(PH_OBJECT_I i) { m_recently_disabled_objects.erase(i); }
void CPHWorld::AddUpdateObject(CPHUpdateObject* object)
{
    //.	if(object->IsFreezed())m_freezed_update_objects.erase(i);
    m_update_objects.push_back(object);
}

void CPHWorld::RemoveUpdateObject(CPHUpdateObject* object)
{
    for (PH_UPDATE_OBJECT_I it = m_update_objects.begin(), e = m_update_objects.end(); it != e; ++it)
    {
        if (*it == object)
        {
            m_update_objects.erase(it);
            return;
        }
    }
}
void CPHWorld::RemoveObject(PH_OBJECT_I i) { m_objects.erase((i)); };
void CPHWorld::AddFreezedObject(CPHObject* obj) { m_freezed_objects.push_back(obj); }
void CPHWorld::RemoveFreezedObject(PH_OBJECT_I i) { m_freezed_objects.erase(i); }
void CPHWorld::Freeze()
{
    R_ASSERT2(!b_world_freezed, "already freezed!!!");
    m_freezed_objects.move_items(m_objects);

    PH_OBJECT_I iter = m_freezed_objects.begin(), e = m_freezed_objects.end();

    for (; e != iter; ++iter)
        (*iter)->FreezeContent();

    m_freezed_update_objects.move_items(m_update_objects);

    b_world_freezed = true;
}
void CPHWorld::UnFreeze()
{
    R_ASSERT2(b_world_freezed, "is not freezed!!!");
    PH_OBJECT_I iter = m_freezed_objects.begin(), e = m_freezed_objects.end();
    for (; e != iter; ++iter)
        (*iter)->UnFreezeContent();

    m_objects.move_items(m_freezed_objects);

    m_update_objects.move_items(m_freezed_update_objects);
    b_world_freezed = false;
}
bool CPHWorld::IsFreezed() { return b_world_freezed; }
void CPHWorld::CutVelocity(float l_limit, float a_limit)
{
    PH_OBJECT_I i_object;
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        obj->CutVelocity(l_limit, a_limit);
        ++i_object;
    }
}
void CPHWorld::NetRelcase(CPhysicsShell* s)
{
    CPHReqComparerHasShell c(s);
    m_commander->remove_calls_threadsafety(&c);
    m_script_commander->remove_calls_threadsafety(&c);

    PH_UPDATE_OBJECT_I i_update_object;
    for (i_update_object = m_update_objects.begin(); m_update_objects.end() != i_update_object;)
    {
        CPHUpdateObject* obj = (*i_update_object);
        ++i_update_object;
        obj->NetRelcase(s);
        // obj->PhTune(fixed_step);
    }
}

void CPHWorld::AddCall(CPHCondition* c,CPHAction* a)
{
    // GAME_RENDER_OVERLAP_PLAN Phase 3: this is the sole Lua entry point (physics_world():add_call(),
    // PHWorldScript.cpp) reaching m_commander, which is drained from inside Step() on CPHWorld's
    // dedicated physics thread - a lua_State must never be touched from there. Lua-wrapping calls go to
    // m_script_commander instead, drained on the main thread after the physics join (once per frame that
    // stepped, rather than once per substep as before - same as level.add_call granularity).
    if (c->touches_script() || a->touches_script())
    {
        m_script_commander->add_call_threadsafety(c, a);
        return;
    }
    m_commander->add_call_threadsafety(c, a);
}

u16 CPHWorld::ObjectsNumber() { return m_objects.count(); }
u16 CPHWorld::UpdateObjectsNumber() { return m_update_objects.count(); }
void CPHWorld::GetState(V_PH_WORLD_STATE& state)
{
    state.clear();
    PH_OBJECT_I i_object;
    for (i_object = m_objects.begin(); m_objects.end() != i_object;)
    {
        CPHObject* obj = (*i_object);
        const u16 els = obj->get_elements_number();
        for (u16 i = 0; els > i; ++i)
        {
            std::pair<CPHSynchronize*, SPHNetState> s;
            s.first = obj->get_element_sync(i);
            s.first->get_State(s.second);
            state.push_back(s);
        }
        ++i_object;
    }
}

void CPHWorld::StepNumIterations(int num_it) { dWorldSetQuickStepNumIterations(NULL, num_it); }
