#pragma once

#include "Common/Noncopyable.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "xrCore/Threading/Event.hpp"

#include "xrEngine/pure.h"

#include "Physics.h"
#include "PHUpdateObject.h"
#include "IPHWorld.h"

#include "xrScriptEngine/ScriptExporter.hpp"

// refs
struct SGameMtlPair;
class CPHCommander;
class CPHCondition;
class CPHAction;
struct SPHNetState;
class CPHSynchronize;
typedef xr_vector<std::pair<CPHSynchronize*, SPHNetState>> V_PH_WORLD_STATE;

class CPHMesh
{
    dGeomID Geom;

public:
    dGeomID GetGeom() { return Geom; }
    void Create(dSpaceID space, dWorldID world);
    void Destroy();
};

#define PHWORLD_SOUND_CACHE_SIZE 8

////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
class CObjectSpace;
class CObjectList;
class IPhysicsShellHolder;
class CPHWorld final : public IPHWorld,
                       public pureFrame,
                       private Noncopyable
#ifdef DEBUG
                       ,
                       public pureRender
#endif
{
private:
    PHWorldStatistics stats;
    double m_start_time;
    u32 m_delay;
    u32 m_previous_delay;
    u32 m_reduce_delay;
    u32 m_update_delay_count;
    bool b_world_freezed;
    bool b_processing;
    bool b_exist;
    static const u32 update_delay = 1;
    ///	dSpaceID					Space														;

    CPHMesh Mesh;
    PH_OBJECT_STORAGE m_objects;
    PH_OBJECT_STORAGE m_freezed_objects;
    PH_OBJECT_STORAGE m_recently_disabled_objects;
    PH_UPDATE_OBJECT_STORAGE m_update_objects;
    PH_UPDATE_OBJECT_STORAGE m_freezed_update_objects;
    // P1: reused snapshot of active-island roots for parallel solve (see CPHWorld::Step).
    xr_vector<CPHObject*> m_island_solve_batch;
    // Objects whose island was merged (NearCallback) during the current step. They may be absent from
    // m_objects / m_update_objects (frozen world, CPHStaticGeomShell, deactivated mid-step), so the regular
    // IslandReinit loops would leave a stale m_self_active pointing to a soon-destroyed island -> crash in
    // CPHIsland::GoActive on the next collision. ReinitMerged() guarantees Unmerge for every one of them.
    xr_vector<CPHObject*> m_merged_objects;
    // B-1: scheduler overlap frame window (GameThread launched until post-wait flush).
    std::atomic<bool> m_scheduler_overlap_active{false};
    // True for the whole Kick..Wait window (set on the main thread in KickPhysicsOverlap before the wake,
    // cleared in WaitPhysicsStep after m_physics_done) - not just while FrameStep itself runs, so a
    // mutation started on another thread right after the Kick is deferred too.
    std::atomic<bool> m_step_running{false};
    std::thread::id m_step_thread_id; // m_physics_thread's id; its own internal mutations are NOT deferred
    std::mutex m_deferred_mutations_lock;
    xr_vector<std::function<void()>> m_deferred_mutations;
    // CPHShell::vis_update_activate() on the physics thread: the game-side ObjectProcessingActivate()
    // (CObjectList active/sleeping lists, CGameObject::Props counter, crow) is replayed here on the main
    // thread after the join. Touched only by the physics thread during the step and by the main thread
    // after m_physics_done (the Event orders them), so no lock.
    xr_vector<IPhysicsShellHolder*> m_deferred_processing_activate;
    // Phase 3: dedicated physics-step thread, by analogy with the SVP worker
    // (Layers/xrRender_R2/r2_R_calculate.cpp's svp_worker_loop) - persistent, lazily started,
    // woken/joined once per frame via an auto-reset Event pair, torn down only in Destroy(). Physics is
    // not coupled to render targets, but Device.Reset() can still be triggered from inside DoRender()
    // (RenderBegin, DX device reset) - WaitForFrameTasks() waits for the step via
    // Device.PhysicsWaitOverlapCallback first (Reset's mem_compact etc. must not run beside the step).
    std::thread m_physics_thread;
    Event m_physics_wake, m_physics_done;
    std::atomic<bool> m_physics_thread_exit{false};
    // Main thread only: a Kick has woken the thread and its m_physics_done has not been consumed yet.
    // m_physics_done is auto-reset, so waiting twice for one Kick would hang - every wait goes through
    // WaitPhysicsStep(), which checks this.
    bool m_physics_in_flight = false;
    // Snapshot of Device.vCameraPosition taken on the main thread at Kick time: DoRender() temporarily
    // overwrites the live field (HUD scope overlay, weapon icons, preview scene) while the step runs.
    Fvector m_step_camera_pos{};
    u64 m_steps_num_at_kick = 0;
    dGeomID m_motion_ray;
    CPHCommander* m_commander;
    // physics_world():add_call() entries that wrap Lua (touches_script()): m_commander is drained on the
    // physics thread, so these are kept here instead and drained on the main thread after the per-frame
    // physics join (JoinPhysicsOverlap), only on frames that actually stepped.
    CPHCommander* m_script_commander;
    CObjectSpace* m_object_space;
    CObjectList* m_level_objects;

public:
    u64 m_steps_num;

private:
    u16 m_steps_short_num;

public:
    double m_frame_sum;
    dReal m_previous_frame_time;
    bool b_frame_mark;
    dReal m_frame_time;
    float m_update_time;
    u16 disable_count;
    float m_gravity;

private:
    ContactCallbackFun* m_default_contact_shotmark;
    ContactCallbackFun* m_default_character_contact_shotmark;
    PhysicsStepTimeCallback* physics_step_time_callback;

public:
    CPHWorld();

    // IC	dSpaceID					GetSpace						()			{return Space;}	;
    IC bool Exist() { return b_exist; }
    void Create(bool mt, CObjectSpace* os, CObjectList* lo);
    void SetGravity(float g);
    IC float Gravity() { return m_gravity; }
    void AddObject(CPHObject* object);
    void AddUpdateObject(CPHUpdateObject* object);
    void AddRecentlyDisabled(CPHObject* object);
    void RemoveFromRecentlyDisabled(PH_OBJECT_I i);
    void RemoveObject(PH_OBJECT_I i);
    void RemoveUpdateObject(CPHUpdateObject* object);
    dGeomID GetMeshGeom() { return Mesh.GetGeom(); }
    IC dGeomID GetMotionRayGeom() { return m_motion_ray; }
    void SetStep(float s);
    void Destroy();
    IC float FrameTime(bool frame_mark) { return b_frame_mark == frame_mark ? m_frame_time : m_previous_frame_time; }
    void FrameStep(dReal step = 0.025f);
    void Step();
    void StepTouch();
    void RegisterMerged(CPHObject* obj);
    void ReinitMerged();
    void CutVelocity(float l_limit, float a_limit);
    void GetState(V_PH_WORLD_STATE& state);
    void Freeze();
    void UnFreeze();
    void AddFreezedObject(CPHObject* obj);
    void RemoveFreezedObject(PH_OBJECT_I i);
    bool IsFreezed();
    IC bool Processing() { return b_processing; }
    u32 CalcNumSteps(u32 dTime);
    u16 ObjectsNumber();
    u16 UpdateObjectsNumber();
    IC u16 StepsShortCnt() { return m_steps_short_num; }
    u64& StepsNum() { return m_steps_num; }
    float FrameTime() { return m_frame_time; }
    ContactCallbackFun* default_contact_shotmark() { return m_default_contact_shotmark; }
    ContactCallbackFun* default_character_contact_shotmark() { return m_default_character_contact_shotmark; }
    void set_default_contact_shotmark(ContactCallbackFun* f) { m_default_contact_shotmark = f; }
    void set_default_character_contact_shotmark(ContactCallbackFun* f) { m_default_character_contact_shotmark = f; }
    void NetRelcase(CPhysicsShell* s);
    CObjectSpace& ObjectSpace()
    {
        VERIFY(m_object_space);
        return *m_object_space;
    }
    CObjectList& LevelObjects()
    {
        VERIFY(m_level_objects);
        return *m_level_objects;
    }

	void AddCall(CPHCondition* c, CPHAction* a) override;

#ifdef DEBUG
    virtual void OnRender();
#endif
    virtual void OnFrame();
    void OnFrameStep(); // B-1: actual physics step body; runs on m_physics_thread (PhysicsWorkerLoop) only
    // B-1: mark the overlap frame window (GameThread launched, flush not yet run).
    void BeginSchedulerOverlapFrame();
    // Phase 3: Device.PhysicsKickOverlapCallback / PhysicsWaitOverlapCallback / PhysicsJoinOverlapCallback.
    // Kick wakes m_physics_thread and returns; Wait only waits for the step (idempotent, used by
    // Device.WaitForFrameTasks() before Reset()); Join = Wait + flush of deferred mutations.
    void KickPhysicsOverlap();
    void WaitPhysicsStep();
    void JoinPhysicsOverlap();
    // B-1: true on a thread other than the physics thread while a step is in flight (Kick..Wait) - i.e.
    // main/render/PreRender/worker code during DoRender(). GameThread is joined before the Kick, so it never
    // sees true. Caller-level intents should defer, not low-level PHObject replay.
    bool ShouldDeferSchedulerPhysicsMutation() const override;
    // B-1: queue owner-level apply callback for post-wait flush; returns false if caller should run immediately.
    bool defer_scheduler_mutation(std::function<void()> op) override;
    void flush_deferred_mutations(); // run queued mutations on the main thread after the step (JoinPhysicsOverlap)
    // True on m_physics_thread while it runs this frame's step.
    bool IsPhysicsStepThread() const;
    // See m_deferred_processing_activate.
    void DeferProcessingActivate(IPhysicsShellHolder* holder) { m_deferred_processing_activate.push_back(holder); }
    // Camera position as of this frame's Kick (see m_step_camera_pos) while a kicked step is in flight; the
    // live Device.vCameraPosition otherwise (Step()/StepTouch() run synchronously on the main thread during
    // load/spawn, before any Kick). Read this, not Device.vCameraPosition, from code running inside a step.
    const Fvector& StepCameraPosition() const override;
    virtual const PHWorldStatistics& GetStats() override { return stats; }
    virtual void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert) override;

private:
    void PhysicsWorkerLoop(); // Phase 3: body of m_physics_thread
    void StepNumIterations(int num_it);
    void set_step_time_callback(PhysicsStepTimeCallback* cb) { physics_step_time_callback = cb; }

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

extern XRPHYSICS_API CPHWorld* ph_world;
IC CPHWorld& inl_ph_world() { return *ph_world; }
