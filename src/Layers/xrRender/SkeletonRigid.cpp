//---------------------------------------------------------------------------
#include "stdafx.h"
#pragma hdrstop

#include "SkeletonCustom.h"

#include "xrCore/Threading/Lock.hpp"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCore/Threading/ScopeLock.hpp"
#include "xrRender_console.h"

namespace xray::render::RENDER_NAMESPACE
{
extern int psSkeletonUpdate;

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s);
#endif

namespace
{
// Pipelined prepass (BonesPrepass_PhaseBEarly): this thread computes early bones while main runs the UpdateCL of other
// objects. A nested CalculateBones of another model (a bone callback reading a neighbour) must not run that model's
// phase A here - its blend callbacks are game code (Lua) and would run beside main's.
thread_local bool t_bones_early_task = false;
} // namespace

void CKinematics::CalculateBones(BOOL bForceExact)
{
    PERF_HOT_ZONE("CKinematics::CalculateBones");
    PERF_INC(bones_calls);

    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
    {
        PERF_INC(bones_fast_out);
        // The game logic thread computed these bones this tick in an overlap frame and left a pending visibility
        // update to us (Visibility_UpdateLocked): the render traverses children right after this call.
        if (m_visibility_deferred.load(std::memory_order_relaxed))
            Visibility_ApplyDeferred();
        return;
    }
    // --- phases ------------------------------------------------------------------------------------------
    // A (global UCalc_Mutex): OnCalculateBones (UpdateTracks can fire blend callbacks into Lua; the global lock is
    //   what serializes that between render threads) and the slow_skip check. Never waits for a
    //   model lock, so a thread holding the global lock can't take part in a lock cycle: UpdateTracks only TryEnters
    //   the model lock (serialized with the game's PlayCycle & co.) and skips when it is busy. A render thread beside
    //   the Game thread defers the blend callbacks to main after the join (SkeletonAnimated.cpp, DeferBlendEvent).
    // B (per-model lock): Visibility_Update (not on the game logic thread beside the render, see
    //   Visibility_UpdateLocked), Bone_Calculate, visbox, Update_Callback. The game also holds this lock while it
    //   swaps bone callbacks of the live skeleton (IKinematics::LockBones, CSightManager::compute_aiming). Models
    //   are independent here, so different models compute in parallel.
    // Lock order: global -> (never) model. A thread holding a model lock may block on the global one (nested
    // CalculateBones), but a global holder only TryEnters a model lock (see UCalc_model_lock).

    // Returns true when phase B has to run.
    const auto phase_a = [&]() -> bool
    {
        // Computation of this tick already started (finished, or in progress on another thread / in our own
        // recursion). Skip A, but let phase B wait for the owner: the old global lock made a second caller wait
        // until the bones were complete, and the caller reads them right after we return.
        if (Device.dwTimeGlobal == UCalc_Time)
            return true;
        {
            PERF_SCOPE(bones_on_calc);
            OnCalculateBones();
        }
        if (!bForceExact && (Device.dwTimeGlobal < (UCalc_Time + UCalc_Interval)))
        {
            PERF_INC(bones_slow_skip);
            return false;
        }
        return true;
    };

    bool need_b;
    if (t_bones_early_task)
    {
        // Pipelined prepass task: never the global lock (main may hold it in a phase A - blend callback, Lua - while it
        // joins the early tasks, CLevel::BonesPrepassEarlyJoin) and never phase A of another model (game code): a model
        // not started this tick keeps its previous pose (as on a failed TryEnter); one started this tick is waited for
        // below (nested: TryEnter only). The unlocked read of UCalc_Time is the same check phase B repeats under the lock.
        if (Device.dwTimeGlobal != UCalc_Time)
        {
            PERF_INC(prepass_early_nested_skip);
            return;
        }
        need_b = true;
    }
    else
    {
        UCalc_mtlock lock;
        need_b = phase_a();
    } // global lock released before waiting for the model lock
    if (!need_b)
        return;
    UCalc_model_lock model_lock(UCalc_ModelLock, this);
    if (!model_lock.locked)
    {
        // We hold another model's (or the global) lock: waiting could deadlock, keep the previous pose.
        PERF_INC(bones_lock_try_fail);
        return;
    }
    CalculateBonesPhaseB(bForceExact);
}

// Phase B of CalculateBones; the caller holds the per-model lock.
void CKinematics::CalculateBonesPhaseB(BOOL bForceExact)
{
    // Re-check under the lock: another thread may have recomputed this tick between phase A and now, we may have
    // waited for it (the bones are complete then), or this is our own recursion from inside the recompute below.
    if (Device.dwTimeGlobal == UCalc_Time)
    {
        PERF_INC(bones_fast_out);
        if (m_visibility_deferred.load(std::memory_order_relaxed))
            Visibility_ApplyDeferred(); // see the fast-out in CalculateBones (we hold the lock: recursive)
        return;
    }
    PERF_INC(bones_recompute);
    // bones_forced is a tag on top of bones_recompute (every forced call is also a recompute), not a
    // separate exclusive bucket - don't "fix" this into bones_recompute -= bones_forced.
    if (bForceExact)
        PERF_INC(bones_forced);
    // Bones prepass: these bones were already computed by this frame's prepass and are recomputed again, i.e.
    // something invalidated them after it (CalculateBones_Invalidate, bone visibility). A miss: the prepass work
    // was wasted, so BonesPrepass_Wanted() leaves this model to the render for a while.
    // Pipelined prepass: bones computed early in this frame and computed again (a mutator or a moved owner - by the
    // prepass, or an invalidation - by whoever reads them next): a redo. The pipeline leaves the model to the prepass
    // for a while; the redo itself is not a prepass miss (the regular stamp holds again after it).
    if (m_prepass_early_frame.load(std::memory_order_relaxed) == Device.dwFrame)
    {
        m_prepass_early_frame.store(0, std::memory_order_relaxed);
        m_prepass_early_redo_frame.store(Device.dwFrame, std::memory_order_relaxed);
        PERF_INC(prepass_redone);
    }
    else if (m_prepass_frame.load(std::memory_order_relaxed) == Device.dwFrame)
    {
        const u8 pass = xray::perf::TlPass();
        if (pass != xray::perf::pass::Prepass)
        {
            m_prepass_miss_frame.store(Device.dwFrame, std::memory_order_relaxed);
            if (pass == xray::perf::pass::None)
                PERF_INC(prepass_miss_other);
            else
                PERF_INC(prepass_miss_render);
        }
    }
#if XR_PERF_METRICS
    // bones/bone_calc: the bone math (Visibility_Update + Bone_Calculate + visbox), as opposed to UpdateTracks
    // (bones/on_calc) and Update_Callback.
    u64 perf_bone_calc_t0 = 0;
    if (xray::perf::g_perf_metrics_active)
    {
        xray::perf::Add(xray::perf::BonesRecomputePassMetric(xray::perf::TlPass()));
        perf_bone_calc_t0 = xray::perf::Ticks();
    }
#endif
    if (Update_Visibility.load(std::memory_order_relaxed))
        Visibility_UpdateLocked();

    _DBG_SINGLE_USE_MARKER;
    // here we have either:
    //	1:	timeout elapsed
    //	2:	exact computation required
    UCalc_Time = Device.dwTimeGlobal;

// exact computation
// Calculate bones
#ifdef DEBUG
    RImplementation.BasicStats.Animation.Begin();
#endif

    {
        xray::perf::ScopedTime bones_caller_time(xray::perf::BoneCallerMetric(xray::perf::TlBoneCaller()));
        Bone_Calculate(bones->at(iRoot), &Fidentity);
    }
#ifdef DEBUG
    check_kinematics(this, dbg_name.c_str());
    RImplementation.BasicStats.Animation.End();
#endif
    VERIFY(LL_GetBonesVisible() != 0);
    // Calculate BOXes/Spheres if needed
    UCalc_Visibox++;
    if (UCalc_Visibox >= psSkeletonUpdate)
    {
        PERF_INC(bones_visbox);
        ZoneScopedN("Skeleton update");

        // mark
        // Own generator per thread: ::Random is a shared plain counter, a data race once models compute in parallel.
        static thread_local CRandom s_visbox_random;
        UCalc_Visibox = -(s_visbox_random.randI(psSkeletonUpdate - 1));

        // the update itself
        Fbox Box;
        Box.invalidate();
        const u32 bone_count = static_cast<u32>(bones->size());
        const auto accumulate_bone_aabb = [this](u32 b, Fbox& acc)
        {
            if (!LL_GetBoneVisible(u16(b)))
                return;
            Fobb& obb = (*bones)[b]->obb;
            Fmatrix& Mbone = bone_instances[b].mTransform;
            Fmatrix Mbox;
            obb.xform_get(Mbox);
            Fmatrix X;
            X.mul_43(Mbone, Mbox);
            Fvector& S = obb.m_halfsize;

            Fvector P, A;
            A.set(-S.x, -S.y, -S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(-S.x, -S.y, S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(S.x, -S.y, S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(S.x, -S.y, -S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(-S.x, S.y, -S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(-S.x, S.y, S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(S.x, S.y, S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
            A.set(S.x, S.y, -S.z);
            X.transform_tiny(P, A);
            acc.modify(P);
        };
        // Keep r__parallel_skeleton_visbox = 0 (default): xr_parallel_for lets this thread steal render tasks while
        // it holds the model lock; a stolen task only TryEnters other models (UCalc_model_lock), so it can't
        // deadlock, but it would skip recomputes (bones/lock_try_fail) and the nesting is hard to reason about.
        const bool use_parallel = r__parallel_skeleton_visbox != 0 &&
            static_cast<int>(bone_count) >= r__parallel_skeleton_visbox_min_bones;
        if (use_parallel)
        {
            PERF_INC(bones_visbox_parallel);
            Lock merge_lock;
            xr_parallel_for(TaskRange<u32>(0, bone_count), true,
                [&](const TaskRange<u32>& r)
                {
                    Fbox local;
                    local.invalidate();
                    for (u32 b = r.begin(); b != r.end(); ++b)
                        accumulate_bone_aabb(b, local);
                    if (!local.is_valid())
                        return;
                    ScopeLock sl(&merge_lock);
                    if (!Box.is_valid())
                        Box.set(local);
                    else
                        Box.merge(local);
                });
        }
        else
        {
            for (u32 b = 0; b < bone_count; b++)
                accumulate_bone_aabb(b, Box);
        }
        if (bone_count)
        {
            // previous frame we have updated box - update sphere
            vis.box.vMin = (Box.vMin);
            vis.box.vMax = (Box.vMax);
            vis.box.getsphere(vis.sphere.P, vis.sphere.R);
        }
#ifdef DEBUG
        // Validate
        VERIFY3(_valid(vis.box.vMin) && _valid(vis.box.vMax), "Invalid bones-xform in model", dbg_name.c_str());
        if (vis.sphere.R > 1000.f)
        {
            for (u16 ii = 0; ii < LL_BoneCount(); ++ii)
            {
                Fmatrix tr;
                tr = LL_GetTransform(ii);
                Log("bone ", LL_BoneName_dbg(ii));
                Log("bone_matrix", tr);
            }
            Log("end-------");
        }
        VERIFY3(vis.sphere.R < 1000.f, "Invalid bones-xform in model", dbg_name.c_str());
#endif
    }

#if XR_PERF_METRICS
    if (perf_bone_calc_t0)
        xray::perf::AddTimeSlow(xray::perf::Metric::bones_bone_calc, xray::perf::Ticks() - perf_bone_calc_t0);
#endif

    //
    if (Update_Callback)
    {
        PERF_INC(bones_callback);
        PERF_SCOPE(bones_update_cb);
        Update_Callback(this);
    }
    UCalc_DoneTime.store(UCalc_Time, std::memory_order_release);
}

// --- Bones prepass (always, CLevel::BonesPrepass, plans/optimization_mt/04-bones-prepass.md) ----------------------
// The game calls these once per frame between the game logic and the render (device.cpp, RunBonesPrepass): phase A
// for every candidate sequentially on the main thread, then phase B for all of them in parallel. Same work, locks and
// lock order as CalculateBones(TRUE) - just phase A of all models before phase B of any. Afterwards UCalc_DoneTime ==
// Device.dwTimeGlobal, so the render's CalculateBones(TRUE) takes the unlocked fast-out.

// A model the prepass missed on (recomputed again after it in the same frame) is left to the render for this many
// frames; then the prepass tries it again. Keeps the wasted work of always-missing models (a weapon with
// bUseAttachmentSystem invalidates its bones in every renderable_Render) at about 1/64.
static constexpr u32 bones_prepass_miss_cooldown = 64;

void CKinematics::BonesPrepass_MarkRendered()
{
    const u32 frame = Device.dwFrame;
    // Many render threads (main view, cascades, lights, SVP) hit the same model: store only on change.
    if (m_render_bones_frame.load(std::memory_order_relaxed) != frame)
        m_render_bones_frame.store(frame, std::memory_order_relaxed);
}

bool CKinematics::BonesPrepass_Wanted() const
{
    const u32 frame = Device.dwFrame;
    const u32 rendered = m_render_bones_frame.load(std::memory_order_relaxed);
    // The prepass runs after FrameMove (dwFrame already advanced), so the previous render stamped frame - 1.
    if (rendered == 0 || rendered + 1 < frame)
        return false;
    const u32 miss = m_prepass_miss_frame.load(std::memory_order_relaxed);
    if (miss != 0 && frame - miss < bones_prepass_miss_cooldown)
    {
        PERF_INC(prepass_cooldown);
        return false;
    }
    return true;
}

bool CKinematics::BonesPrepass_PhaseA()
{
    // Already computed this tick (e.g. by UpdateCL: eye_pp, weapon xform, CCF) - nothing to do, the render fast-outs.
    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
    {
        // Pipelined prepass: computed early, and the pose input changed after it (BonesPrepass_MarkEarlyDirty) -
        // recompute now. UpdateTracks of this tick is done (Update_LastTime), the bone math takes the blends as they
        // are now. Nothing else computes bones at this point (the pipeline tasks are joined, the render has not
        // started), the model lock is taken for the order of the stores only.
        const u32 frame = Device.dwFrame;
        if (m_prepass_early_frame.load(std::memory_order_relaxed) != frame ||
            m_prepass_early_dirty.load(std::memory_order_relaxed) != frame)
            return false;
        UCalc_model_lock model_lock(UCalc_ModelLock, this);
        if (!model_lock.locked)
            return false;
        PERF_INC(prepass_dirty);
        CalculateBones_Invalidate();
    }
    UCalc_mtlock lock;
    // Started this tick by someone else (or our own recursion) - phase B would only wait for it; let the render
    // handle the model as before.
    if (Device.dwTimeGlobal == UCalc_Time)
        return false;
    {
        PERF_SCOPE(bones_on_calc);
        OnCalculateBones();
    }
    return true;
}

bool CKinematics::BonesPrepass_PhaseB()
{
    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
        return false;
    // Same rule as CalculateBones: blocking Enter only while this thread holds no other model lock and not the
    // global one (a prepass task holds nothing when it gets here); nested computations only TryEnter.
    UCalc_model_lock model_lock(UCalc_ModelLock, this);
    if (!model_lock.locked)
    {
        PERF_INC(bones_lock_try_fail);
        return false;
    }
    // Under the lock: computed in between (a bone callback of another model computed this one) - done.
    if (Device.dwTimeGlobal == UCalc_Time)
        return false;
    m_prepass_frame.store(Device.dwFrame, std::memory_order_relaxed);
    CalculateBonesPhaseB(TRUE);
    return true;
}

// --- Pipelined prepass (r__bones_prepass_pipeline, CLevel pipeline in xrGame/Level.cpp) ---------------------------------
// Phase A of a model runs on main right after the UpdateCL batch that finished its object (and everything the object
// holds), phase B on a worker while main goes on with the next batches. What changes the pose input after that marks
// the bones dirty (animation mutators: BonesPrepass_NoteMutation; the owner's XFORM: the game's check at the prepass)
// and the prepass recomputes them; invalidated bones are recomputed by whoever needs them next, as before.

// Early bones recomputed again are left to the prepass for this many frames (same idea as the miss cooldown).
// Shorter than the miss cooldown: a redo costs one extra phase B at the prepass (not a render-side recompute), and the
// models that redo (a mutator after their chunk, a moved owner) mostly do it in bursts (an attack start, a teleport).
static constexpr u32 bones_prepass_early_redo_cooldown = 16;

bool CKinematics::BonesPrepass_EarlyWanted() const
{
    const u32 frame = Device.dwFrame;
    const u32 rendered = m_render_bones_frame.load(std::memory_order_relaxed);
    if (rendered == 0 || rendered + 1 < frame)
        return false;
    const u32 miss = m_prepass_miss_frame.load(std::memory_order_relaxed);
    if (miss != 0 && frame - miss < bones_prepass_miss_cooldown)
        return false;
    const u32 redo = m_prepass_early_redo_frame.load(std::memory_order_relaxed);
    if (redo != 0 && frame - redo < bones_prepass_early_redo_cooldown)
    {
        PERF_INC(prepass_early_cooldown);
        return false;
    }
    return true;
}

bool CKinematics::BonesPrepass_PhaseBEarly()
{
    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
        return false;
    // The task holds no lock here: blocking Enter (main may hold the lock for a mutator - we wait for it). When main runs
    // this chunk inside a Wait of its own while it holds another model's lock, only TryEnter - then the prepass does it.
    UCalc_model_lock model_lock(UCalc_ModelLock, this);
    if (!model_lock.locked)
    {
        PERF_INC(bones_lock_try_fail);
        return false;
    }
    if (Device.dwTimeGlobal == UCalc_Time)
        return false;
    const u32 frame = Device.dwFrame;
    m_prepass_frame.store(frame, std::memory_order_relaxed);
    {
        const bool prev = t_bones_early_task;
        t_bones_early_task = true;
        CalculateBonesPhaseB(TRUE);
        t_bones_early_task = prev;
    }
    // After the compute, still under the lock: a mutator that waited for it marks the bones dirty from now on, one that
    // came before is already in them.
    m_prepass_early_frame.store(frame, std::memory_order_relaxed);
    return true;
}

bool CKinematics::BonesPrepass_Done() const
{
    return Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire);
}

// Phase C owners (pipeline on): the same work as CalculateBones(FALSE) of the item's UpdateXForm, split like the prepass.
bool CKinematics::BonesPrepass_LazyPhaseA()
{
    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
        return false;
    UCalc_mtlock lock;
    if (Device.dwTimeGlobal == UCalc_Time)
        return false;
    {
        PERF_SCOPE(bones_on_calc);
        OnCalculateBones();
    }
    if (Device.dwTimeGlobal < UCalc_Time + UCalc_Interval)
    {
        PERF_INC(bones_slow_skip);
        return false;
    }
    return true;
}

bool CKinematics::BonesPrepass_LazyPhaseB()
{
    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
        return false;
    UCalc_model_lock model_lock(UCalc_ModelLock, this);
    if (!model_lock.locked)
    {
        PERF_INC(bones_lock_try_fail);
        return false;
    }
    if (Device.dwTimeGlobal == UCalc_Time)
        return false;
    CalculateBonesPhaseB(FALSE);
    return true;
}

// Early kick (r__bones_prepass_early_kick, CLevel in xrGame/Level.cpp): a phase C owner computed on a worker while main
// still runs the sound update, the UI and the RT scheduler. No prepass stamp (not the render's bones, no miss
// bookkeeping), but the early one: a mutator that waited for this compute marks the bones dirty, and the prepass
// recomputes them (BonesPrepass_PhaseA); a nested computation of another model never runs that model's phase A here.
bool CKinematics::BonesPrepass_LazyPhaseBEarly()
{
    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
        return false;
    UCalc_model_lock model_lock(UCalc_ModelLock, this);
    if (!model_lock.locked)
    {
        PERF_INC(bones_lock_try_fail);
        return false;
    }
    if (Device.dwTimeGlobal == UCalc_Time)
        return false;
    {
        const bool prev = t_bones_early_task;
        t_bones_early_task = true;
        CalculateBonesPhaseB(FALSE);
        t_bones_early_task = prev;
    }
    m_prepass_early_frame.store(Device.dwFrame, std::memory_order_relaxed);
    return true;
}

void CKinematics::BonesPrepass_MarkEarlyDirty()
{
    const u32 frame = Device.dwFrame;
    if (m_prepass_early_frame.load(std::memory_order_relaxed) != frame)
        return;
    if (m_prepass_early_dirty.load(std::memory_order_relaxed) != frame)
        m_prepass_early_dirty.store(frame, std::memory_order_relaxed);
}

#ifdef DEBUG
void check_kinematics(CKinematics* _k, LPCSTR s)
{
    CKinematics* K = _k;
    Fmatrix& MrootBone = K->LL_GetBoneInstance(K->LL_GetBoneRoot()).mTransform;
    if (MrootBone.c.y > 10000)
    {
        Msg("all bones transform:--------[%s]", s);

        for (u16 ii = 0; ii < K->LL_BoneCount(); ++ii)
        {
            Fmatrix tr;

            tr = K->LL_GetTransform(ii);
            Log("bone ", K->LL_BoneName_dbg(ii));
            Log("bone_matrix", tr);
        }
        Log("end-------");
        VERIFY3(0, "check_kinematics failed for ", s);
    }
}
#endif

// Добавить скриптовое смещение для кости --#SM+#--
void CKinematics::LL_AddTransformToBone(KinematicsABT::additional_bone_transform& offset)
{
    m_bones_offsets.push_back(offset);
}
void CKinematics::LL_SetTransformToBone(KinematicsABT::additional_bone_transform& offset)
{
    m_bones_pos[offset.m_bone_id] = offset;
}

// Обнулить скриптовое смещение для конкретной кости или всех сразу (bone_id = BI_NONE) --#SM+#--
void CKinematics::LL_ClearAdditionalTransform(u16 bone_id)
{
    if (bone_id == BI_NONE)
    {
        m_bones_offsets.clear();
        return;
    }

    BONE_TRANSFORM_VECTOR_IT it = m_bones_offsets.begin();
    while (it != m_bones_offsets.end())
    {
        if (it->m_bone_id == bone_id)
        {
            it = m_bones_offsets.erase(it);
        } 
        else
            ++it;
    }
}

void CKinematics::BuildBoneMatrix(
    const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /*= (1<<0)*/)
{
    bi.mTransform.mul_43(*parent, bd->bind_transform);
    CalculateBonesNewTransforms(bd, bi, parent, channel_mask); //--#SM+#--
    CalculateBonesAdditionalTransforms(bd, bi, parent, channel_mask); //--#SM+#--
}

// Добавляем константные смещения к нужным костям --#SM+#--
void CKinematics::CalculateBonesAdditionalTransforms(
    const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /* = (1<<0)*/)
{
    // bi.mTransform.c - содержит смещение относительно первой кости модели\центра сцены (0, 0, 0)
    for (auto& it : m_bones_offsets)
    {
        if (it.m_bone_id == bd->GetSelfID())
        {
            const Fvector vOldPos = bi.mTransform.c;
            bi.mTransform.mulB_43(it.m_transform); // Rotation
            bi.mTransform.c.add(vOldPos, it.m_transform.c); // Translation
        }
    }
}

void CKinematics::CalculateBonesNewTransforms(
    const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /* = (1<<0)*/)
{
    // bi.mTransform.c - содержит смещение относительно первой кости модели\центра сцены (0, 0, 0)
    for (auto& it : m_bones_pos)
    {
        if (it.first == bd->GetSelfID())
        {
            bi.mTransform.mulB_43(it.second.m_transform); // Rotation
            bi.mTransform.c.set(it.second.m_transform.c); // Translation
        }
    }
}

void CKinematics::CLBone(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 channel_mask /*= (1<<0)*/)
{
    u16 SelfID = bd->GetSelfID();

    if (LL_GetBoneVisible(SelfID))
    {
        if (bi.callback_overwrite())
        {
            if (bi.callback())
                bi.callback()(&bi);
        }
        else
        {
            BuildBoneMatrix(bd, bi, parent, channel_mask);
#ifndef MASTER_GOLD
            R_ASSERT2(_valid(bi.mTransform), "anim kils bone matrix");
#endif // #ifndef MASTER_GOLD
            if (bi.callback())
            {
                bi.callback()(&bi);
#ifndef MASTER_GOLD
                R_ASSERT2(_valid(bi.mTransform), make_string("callback kils bone matrix bone: %s ", bd->name.c_str()));
#endif // #ifndef MASTER_GOLD
            }
        }
        bi.mRenderTransform.mul_43(bi.mTransform, bd->m2b_transform);
    }
}

void CKinematics::Bone_GetAnimPos(Fmatrix& pos, u16 id, u8 mask_channel, bool ignore_callbacks)
{
    R_ASSERT(id < LL_BoneCount());
    // Reads the blends (CLBone -> BuildBoneMatrix) the render's UpdateTracks advances and the game's PlayCycle & co.
    // change: under the model lock like them (lock rules: UCalc_model_lock_enter; declined - unlocked, as before).
    UCalc_model_lock model_lock(UCalc_ModelLock, this);
    if (!model_lock.locked)
        PERF_INC(anim_mut_unlocked);
    CBoneInstance bi = LL_GetBoneInstance(id);
    BoneChain_Calculate(&LL_GetData(id), bi, mask_channel, ignore_callbacks);
#ifndef MASTER_GOLD
    R_ASSERT(_valid(bi.mTransform));
#endif
    pos.set(bi.mTransform);
}

void CKinematics::Bone_Calculate(CBoneData* bd, Fmatrix* parent)
{
    u16 SelfID = bd->GetSelfID();
    CBoneInstance& BONE_INST = LL_GetBoneInstance(SelfID);
    CLBone(bd, BONE_INST, parent, u8(-1));
    // Calculate children
    for (xr_vector<CBoneData*>::iterator C = bd->children.begin(); C != bd->children.end(); ++C)
        Bone_Calculate(*C, &BONE_INST.mTransform);
}

void CKinematics::BoneChain_Calculate(const CBoneData* bd, CBoneInstance& bi, u8 mask_channel, bool ignore_callbacks)
{
    u16 SelfID = bd->GetSelfID();
    // CBlendInstance& BLEND_INST	= LL_GetBlendInstance(SelfID);
    // CBlendInstance::BlendSVec &Blend = BLEND_INST.blend_vector();
    // bi is always a scratch copy (Bone_GetAnimPos / parrent_bi below); only its mTransform is
    // used afterwards. So the callback is dropped with the plain reset_callback() (CLBone then
    // just builds the anim matrix, as with the old nullptr/no-overwrite set_callback) and never
    // restored: set_callback() on these thread-local copies only did useless stores and flooded
    // the overlap hazard detector (CBoneInstance::set_callback, ~13M hits per session).
    if (ignore_callbacks)
        bi.reset_callback();
    if (SelfID == LL_GetBoneRoot())
    {
        CLBone(bd, bi, &Fidentity, mask_channel);
        return;
    }
    u16 ParentID = bd->GetParentID();
    R_ASSERT(ParentID != BI_NONE);
    CBoneData* ParrentDT = &LL_GetData(ParentID);
    CBoneInstance parrent_bi = LL_GetBoneInstance(ParentID);
    BoneChain_Calculate(ParrentDT, parrent_bi, mask_channel, ignore_callbacks);
    CLBone(bd, bi, &parrent_bi.mTransform, mask_channel);
}
} // namespace xray::render::RENDER_NAMESPACE
