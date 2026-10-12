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

void CKinematics::CalculateBones(BOOL bForceExact)
{
    PERF_HOT_ZONE("CKinematics::CalculateBones");
    PERF_INC(bones_calls);

    if (Device.dwTimeGlobal == UCalc_DoneTime.load(std::memory_order_acquire))
    {
        PERF_INC(bones_fast_out);
        return;
    }
    // --- phases ------------------------------------------------------------------------------------------
    // A (global UCalc_Mutex): OnCalculateBones (UpdateTracks can fire blend callbacks into Lua; the global lock is
    //   what serializes that between render threads) and the slow_skip check. Never waits for a
    //   model lock, so a thread holding the global lock can't take part in a lock cycle.
    // B (per-model lock): Visibility_Update, Bone_Calculate,
    //   visbox, Update_Callback. Models are independent here, so different models compute in parallel.
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
    if (m_prepass_frame.load(std::memory_order_relaxed) == Device.dwFrame)
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
    if (Update_Visibility)
        Visibility_Update();

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
        return false;
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
    // ignore callbacks
    BoneCallback bc = bi.callback();
    BOOL ow = bi.callback_overwrite();
    if (ignore_callbacks)
    {
        bi.set_callback(bi.callback_type(), nullptr, bi.callback_param(), 0);
    }
    if (SelfID == LL_GetBoneRoot())
    {
        CLBone(bd, bi, &Fidentity, mask_channel);
        // restore callback
        bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
        return;
    }
    u16 ParentID = bd->GetParentID();
    R_ASSERT(ParentID != BI_NONE);
    CBoneData* ParrentDT = &LL_GetData(ParentID);
    CBoneInstance parrent_bi = LL_GetBoneInstance(ParentID);
    BoneChain_Calculate(ParrentDT, parrent_bi, mask_channel, ignore_callbacks);
    CLBone(bd, bi, &parrent_bi.mTransform, mask_channel);
    // restore callback
    bi.set_callback(bi.callback_type(), bc, bi.callback_param(), ow);
}
} // namespace xray::render::RENDER_NAMESPACE
