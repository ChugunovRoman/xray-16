#pragma once

#include "FHierrarhyVisual.h"
#include "xrCore/Animation/Bone.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "Include/xrRender/Kinematics.h"

class CInifile;
class CBoneData;
struct SEnumVerticesCallback;

namespace xray::render::RENDER_NAMESPACE
{
// consts
extern Lock UCalc_Mutex;

// refs
class CKinematics;
class CSkeletonX;

// MT-locker
inline thread_local int g_ucalc_lock_depth = 0;

struct UCalc_mtlock
{
    u64 t_hold0{};
    bool outer{};
    xray::perf::ThreadKind kind{xray::perf::ThreadKind::Worker};

    UCalc_mtlock()
    {
        outer = (g_ucalc_lock_depth == 0);
        ++g_ucalc_lock_depth;
#if XR_PERF_METRICS
        const bool metrics_active = xray::perf::g_perf_metrics_active;
        if (metrics_active)
            kind = xray::perf::CurrentThreadKind();
        if (!UCalc_Mutex.TryEnter())
        {
            PERF_INC(bones_lock_contend);
            const u64 t0 = metrics_active ? xray::perf::Ticks() : 0;
            UCalc_Mutex.Enter();
            if (metrics_active)
            {
                const u64 waited = xray::perf::Ticks() - t0;
                xray::perf::AddTimeSlow(xray::perf::Metric::bones_lock_wait, waited);
                xray::perf::AddTimeSlow(xray::perf::BonesLockWaitMetric(kind), waited);
            }
        }
        if (outer && metrics_active)
            t_hold0 = xray::perf::Ticks();
#else
        UCalc_Mutex.Enter();
#endif
    }

    ~UCalc_mtlock()
    {
#if XR_PERF_METRICS
        if (outer && t_hold0)
        {
            const u64 held = xray::perf::Ticks() - t_hold0;
            xray::perf::AddTimeSlow(xray::perf::Metric::bones_lock_hold, held);
            xray::perf::AddTimeSlow(xray::perf::BonesLockHoldMetric(kind), held);
        }
#endif
        UCalc_Mutex.Leave();
        --g_ucalc_lock_depth;
    }
};

// Per-model lock for the bone math of CKinematics::CalculateBones (phase B), see the phase comment there.
// Owner = the model whose lock this thread took last (nullptr = none held). Used by the deadlock rules below.
inline thread_local const void* g_ucalc_model_lock_owner = nullptr;

// Enters the per-model lock l of model; true when locked. The thread bookkeeping is UCalc_model_lock_take's.
// Blocking Enter is allowed only when this thread holds nothing that another thread may wait for in the opposite
// order: no other model's lock (ABBA across models) and not the global UCalc_Mutex (a thread holding a model lock
// may block on the global one, so the global holder must never block on a model lock). Otherwise TryEnter, and the
// caller skips its work on failure. Re-entering the same model is always safe (recursive lock).
inline bool UCalc_model_lock_enter(Lock& l, const void* model)
{
    const void* const prev_owner = g_ucalc_model_lock_owner;
    const bool may_block = (prev_owner == nullptr || prev_owner == model) && g_ucalc_lock_depth == 0;
#if XR_PERF_METRICS
    if (l.TryEnter())
        return true;
    if (!may_block)
        return false;
    const bool metrics_active = xray::perf::g_perf_metrics_active;
    PERF_INC(bones_model_lock_contend);
    const u64 t0 = metrics_active ? xray::perf::Ticks() : 0;
    l.Enter();
    if (metrics_active)
        xray::perf::AddTimeSlow(xray::perf::Metric::bones_model_lock_wait, xray::perf::Ticks() - t0);
    return true;
#else
    if (!may_block)
        return l.TryEnter();
    l.Enter();
    return true;
#endif
}

// The only acquire/release pair of a per-model lock (UCalc_model_lock, CKinematics::LockBones/UnlockBones), so the
// thread bookkeeping stays balanced: the owner (deadlock rules above) and xray::perf::TlBonesLocked (bone callbacks
// written under any model lock - phase B, KinematicsBonesLock - are serialized with the render, the overlap detector
// of CBoneInstance::set_callback skips them). prev_owner must be passed back to UCalc_model_lock_release as is.
inline bool UCalc_model_lock_take(Lock& l, const void* model, const void*& prev_owner)
{
    prev_owner = g_ucalc_model_lock_owner;
    if (!UCalc_model_lock_enter(l, model))
        return false;
    g_ucalc_model_lock_owner = model;
    ++xray::perf::TlBonesLocked();
    return true;
}

inline void UCalc_model_lock_release(Lock& l, const void* prev_owner)
{
    --xray::perf::TlBonesLocked();
    g_ucalc_model_lock_owner = prev_owner;
    l.Leave();
}

struct UCalc_model_lock
{
    Lock* lock{};
    const void* prev_owner{};
    bool locked{};

    // Lock rules: UCalc_model_lock_enter.
    UCalc_model_lock(Lock& l, const void* model) : lock(&l), locked(UCalc_model_lock_take(l, model, prev_owner)) {}

    ~UCalc_model_lock()
    {
        if (locked)
            UCalc_model_lock_release(*lock, prev_owner);
    }

    UCalc_model_lock(const UCalc_model_lock&) = delete;
    UCalc_model_lock& operator=(const UCalc_model_lock&) = delete;
};

#pragma warning(push)
#pragma warning(disable : 4275)
class CSkeletonWallmark : public intrusive_base // 4+4+4+12+4+16+16 = 60 + 4 = 64
{
#pragma warning(pop)
    CKinematics* m_Parent; // 4
    const Fmatrix* m_XForm; // 4
    ref_shader m_Shader; // 4
    Fvector3 m_ContactPoint; // 12      model space
    float m_fTimeStart; // 4
public:
#ifdef DEBUG
    u32 used_in_render;
#endif
    Fsphere m_LocalBounds; // 16        model space
    struct WMFace
    {
        Fvector3 vert[3];
        Fvector2 uv[3];
        u16 bone_id[3][4];
        float weight[3][3];
    };
    using WMFacesVec = xr_vector<WMFace>;
    WMFacesVec m_Faces; // 16
public:
    Fsphere m_Bounds; // 16     world space
public:
    CSkeletonWallmark(CKinematics* p, const Fmatrix* m, ref_shader s, const Fvector& cp, float ts)
        : m_Parent(p), m_XForm(m), m_Shader(s), m_ContactPoint(cp), m_fTimeStart(ts)
    {
#ifdef DEBUG
        used_in_render = u32(-1);
#endif
    }
    ~CSkeletonWallmark()
#ifdef DEBUG
    ;
#else
    {
    }
#endif

    CKinematics* Parent() { return m_Parent; }
    u32 VCount() { return m_Faces.size() * 3; }

    bool Similar(ref_shader& sh, const Fvector& cp, float eps)
    {
        return (m_Shader == sh) && m_ContactPoint.similar(cp, eps);
    }

    float TimeStart() { return m_fTimeStart; }
    const Fmatrix* XFORM() { return m_XForm; }
    const Fvector3& ContactPoint() { return m_ContactPoint; }
    ref_shader Shader() { return m_Shader; }
};
using SkeletonWMVec = xr_vector<intrusive_ptr<CSkeletonWallmark>>;

// sanity check
#ifdef DEBUG
struct dbg_marker
{
    BOOL* lock;
    dbg_marker(BOOL* b)
    {
        lock = b;
        VERIFY(*lock == FALSE);
        *lock = TRUE;
    }
    ~dbg_marker() { *lock = FALSE; }
};
#define _DBG_SINGLE_USE_MARKER dbg_marker _dbg_marker(&dbg_single_use_marker)
#else
#define _DBG_SINGLE_USE_MARKER
#endif

//////////////////////////////////////////////////////////////////

class CKinematics : public FHierrarhyVisual, public IKinematics
{
    typedef FHierrarhyVisual inherited;
    friend class CBoneData;
    friend class CSkeletonX;

protected: //--#SM+#--
    DEFINE_VECTOR(KinematicsABT::additional_bone_transform, BONE_TRANSFORM_VECTOR, BONE_TRANSFORM_VECTOR_IT)
    BONE_TRANSFORM_VECTOR m_bones_offsets;
    xr_map<u16, KinematicsABT::additional_bone_transform> m_bones_pos;

public:
#ifdef DEBUG
    BOOL dbg_single_use_marker;
#endif
    void Bone_Calculate(CBoneData* bd, Fmatrix* parent) override;
    void CLBone(const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 mask_channel = (1 << 0));

    void BoneChain_Calculate(const CBoneData* bd, CBoneInstance& bi, u8 channel_mask, bool ignore_callbacks);
    void Bone_GetAnimPos(Fmatrix& pos, u16 id, u8 channel_mask, bool ignore_callbacks) override;

    virtual void BuildBoneMatrix(
        const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 mask_channel = (1 << 0));
    virtual void OnCalculateBones() {}

    virtual void CalculateBonesAdditionalTransforms(
        const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 mask_channel = (1 << 0)); //--#SM+#--
    virtual void CalculateBonesNewTransforms(
        const CBoneData* bd, CBoneInstance& bi, const Fmatrix* parent, u8 mask_channel = (1 << 0)); //--#SM+#--
    virtual void LL_SetTransformToBone(KinematicsABT::additional_bone_transform& offset); //--#SM+#--
    virtual void LL_AddTransformToBone(KinematicsABT::additional_bone_transform& offset); //--#SM+#--
    virtual void LL_ClearAdditionalTransform(u16 bone_id = BI_NONE); //--#SM+#--
public:
    dxRender_Visual* m_lod;

protected:
    SkeletonWMVec wallmarks;
    u32 wm_frame;

    xr_vector<dxRender_Visual*> children_invisible;

    // Globals
    CInifile* pUserData;
    CBoneInstance* bone_instances; // bone instances
    vecBones* bones; // all bones (shared)
    u16 iRoot; // Root bone index

    // Fast search
    accel* bone_map_N; // bones associations (shared) - sorted by name
    accel* bone_map_P; // bones associations (shared) - sorted by name-pointer

    // Visibility_Update pending (children <-> children_invisible). Set from any thread (LL_SetBoneVisible), hence
    // atomic. Starts set (the BOOL it replaced was left uninitialized until Spawn): the first recompute sorts them.
    std::atomic<bool> Update_Visibility{true};
    // The game logic thread skipped a pending Visibility_Update in a game/render overlap frame (Visibility_UpdateLocked):
    // the next CalculateBones outside it applies it even on its fast-out path (Visibility_ApplyDeferred), before the
    // render traverses children.
    std::atomic<bool> m_visibility_deferred{false};
    // Start of the current/last recompute; recursion + re-check guard. Written under the per-model lock (phase B),
    // read under the global lock (phase A) too, hence atomic.
    std::atomic<u32> UCalc_Time{0};
    // Serializes the bone math of this model between render threads (phase B of CalculateBones), and its animation
    // blends (UpdateTracks, PlayCycle & co., Bone_GetAnimPos; SkeletonAnimated.cpp) with the game. Recursive.
#ifdef CONFIG_PROFILE_LOCKS
    Lock UCalc_ModelLock{"CKinematics::UCalc_ModelLock"};
#else
    Lock UCalc_ModelLock;
#endif
    // Device.dwTimeGlobal of the last *finished* recompute - the only value the unlocked fast-out may
    // trust: UCalc_Time is set before Bone_Calculate, so another render thread used to bail out on it and
    // read half-written bone_instances.
    std::atomic<u32> UCalc_DoneTime{0};
    s32 UCalc_Visibox;

    // Bones prepass (always), all Device.dwFrame values, 0 = never:
    // the last frame the render build asked for these bones (add_leafs_dynamic, BonesPrepass_MarkRendered);
    std::atomic<u32> m_render_bones_frame{0};
    // the frame the prepass recomputed them (phase B);
    std::atomic<u32> m_prepass_frame{0};
    // the last frame they were recomputed again after the prepass (invalidated after it: a miss). The prepass then
    // leaves the model to the render for a while (e.g. a weapon that invalidates its own bones in renderable_Render).
    std::atomic<u32> m_prepass_miss_frame{0};
    // Pipelined prepass (r__bones_prepass_pipeline): the frame phase B ran early (right after the UpdateCL batch of the
    // object, BonesPrepass_PhaseBEarly; reset by the first recompute after it - prepass/redone);
    std::atomic<u32> m_prepass_early_frame{0};
    // the frame the pose input changed after the early compute (an animation mutator, the owner's XFORM): the prepass
    // of that frame recomputes the bones (BonesPrepass_PhaseA);
    std::atomic<u32> m_prepass_early_dirty{0};
    // the last frame early bones were recomputed again: the pipeline then leaves the model to the prepass for a while.
    std::atomic<u32> m_prepass_early_redo_frame{0};

    // Perf metrics only (P15, render/main/skin_repeat): Device.dwFrame + 1 of the last GPU-skinned draw of this model in
    // the main view's dynamic submit. Read and written by CSkeletonX::_Render on the main thread only.
    u32 m_perf_rmain_palette_frame{0};

    Flags64 visimask;

    CSkeletonX* LL_GetChild(u32 idx);

    // internal functions
    virtual CBoneData* CreateBoneData(u16 ID) { return xr_new<CBoneData>(ID); }
    virtual void IBoneInstances_Create();
    virtual void IBoneInstances_Destroy();
    void Visibility_Invalidate() { Update_Visibility.store(true, std::memory_order_relaxed); }
    void LL_SetBoneVisibleImpl(u16 bone_id, BOOL val, BOOL bRecursive); // LL_SetBoneVisible minus the model lock
    // Reorders children. Caller holds the model lock; another render thread may still traverse children unlocked
    // meanwhile (a recompute or a deferred update mid-frame), see the comment in the definition.
    void Visibility_Update();
    // Phase B (model lock held): Visibility_Update, or mark it deferred on the game logic thread in an overlap frame.
    void Visibility_UpdateLocked();
    // Applies a deferred Visibility_Update (takes the model lock); no-op on the game logic thread in an overlap frame.
    void Visibility_ApplyDeferred();

    void LL_Validate();

public:
    UpdateCallback Update_Callback;
    void* Update_Callback_Param;

public:
    // wallmarks
    void AddWallmark(const Fmatrix* parent, const Fvector3& start, const Fvector3& dir, ref_shader shader, float size);
    void CalculateWallmarks(bool hud);
    void RenderWallmark(intrusive_ptr<CSkeletonWallmark> wm, FVF::LIT*& verts);
    void ClearWallmarks();

public:
    bool PickBone(const Fmatrix& parent_xform, IKinematics::pick_result& r, float dist, const Fvector& start,
        const Fvector& dir, u16 bone_id) override;
    void EnumBoneVertices(SEnumVerticesCallback& C, u16 bone_id) override;

public:
    CKinematics();
    virtual ~CKinematics();

    // Low level interface
    u16 LL_BoneID(LPCSTR B) override;
    u16 LL_BoneID(const shared_str& B) override;
    LPCSTR LL_BoneName_dbg(u16 ID) override;

    CInifile* LL_UserData() override { return pUserData; }
    accel* LL_Bones() override { return bone_map_N; }
    ICF CBoneInstance& LL_GetBoneInstance(u16 bone_id) override
    {
        VERIFY(bone_id < LL_BoneCount());
        VERIFY(bone_instances);
        return bone_instances[bone_id];
    }
    ICF const CBoneInstance& LL_GetBoneInstance(u16 bone_id) const
    {
        VERIFY(bone_id < LL_BoneCount());
        VERIFY(bone_instances);
        return bone_instances[bone_id];
    }
    // LOD .ogf is a separate CKinematics without live animation blends; copy hi skeleton matrices for skinning.
    void CopyBoneTransformsFrom(const CKinematics& src);
    CBoneData& LL_GetData(u16 bone_id) override
    {
        VERIFY(bone_id < LL_BoneCount());
        VERIFY(bones);
        CBoneData& bd = *((*bones)[bone_id]);
        return bd;
    }

    const IBoneData& GetBoneData(u16 bone_id) const override
    {
        VERIFY(bone_id < LL_BoneCount());
        VERIFY(bones);
        CBoneData& bd = *((*bones)[bone_id]);
        return bd;
    }
    CBoneData* LL_GetBoneData(u16 bone_id)
    {
        R_ASSERT1_CURE(bones && bone_id < LL_BoneCount(), { return nullptr; });
        CBoneData* bd = ((*bones)[bone_id]);
        return bd;
    }
    u16 LL_BoneCount() const override { return u16(bones->size()); }
    u16 LL_VisibleBoneCount() override
    {
        u64 F = visimask.flags & ((u64(1) << u64(LL_BoneCount())) - 1);
        return (u16)btwCount1(F);
    }
    ICF Fmatrix& LL_GetTransform(u16 bone_id) override { return LL_GetBoneInstance(bone_id).mTransform; }
    ICF const Fmatrix& LL_GetTransform(u16 bone_id) const override { return LL_GetBoneInstance(bone_id).mTransform; }
    ICF Fmatrix& LL_GetTransform_R(u16 bone_id) override
    {
        return LL_GetBoneInstance(bone_id).mRenderTransform;
    } // rendering only
    Fobb& LL_GetBox(u16 bone_id) override
    {
        VERIFY(bone_id < LL_BoneCount());
        return (*bones)[bone_id]->obb;
    }
    const Fbox& GetBox() const override { return vis.box; }
    void LL_GetBindTransform(xr_vector<Fmatrix>& matrices) override;
    int LL_GetBoneGroups(xr_vector<xr_vector<u16>>& groups) override;

    u16 LL_GetBoneRoot() override { return iRoot; }
    void LL_SetBoneRoot(u16 bone_id) override
    {
        R_ASSERT1_CURE(bone_id < LL_BoneCount(), { return; });
        iRoot = bone_id;
    }

    BOOL LL_GetBoneVisible(u16 bone_id) override
    {
        R_ASSERT1_CURE(bone_id < LL_BoneCount(), { return false; });
        return visimask.is(u64(1) << bone_id);
    }
    void LL_SetBoneVisible(u16 bone_id, BOOL val, BOOL bRecursive) override;
    u64 LL_GetBonesVisible() override { return visimask.get(); }
    void LL_SetBonesVisible(u64 mask) override;

    // Main functionality
    void CalculateBones(BOOL bForceExact = FALSE) override; // Recalculate skeleton
    void CalculateBonesPhaseB(BOOL bForceExact); // bone math under the model lock, see CalculateBones
    void CalculateBones_Invalidate() override;

    // Bones prepass (always), see IKinematics. The render build calls BonesPrepass_MarkRendered() right
    // before its own CalculateBones(TRUE): that is the candidate list of the next frame's prepass.
    void BonesPrepass_MarkRendered();
    bool BonesPrepass_Wanted() const override;
    bool BonesPrepass_PhaseA() override;
    bool BonesPrepass_PhaseB() override;
    bool BonesPrepass_EarlyWanted() const override;
    bool BonesPrepass_PhaseBEarly() override;
    void BonesPrepass_MarkEarlyDirty() override;
    bool BonesPrepass_LazyPhaseA() override;
    bool BonesPrepass_LazyPhaseB() override;
    bool BonesPrepass_LazyPhaseBEarly() override;
    bool BonesPrepass_Done() const override;
    // Pipelined prepass: an animation mutator (PlayCycle & co., under the model lock) changed the blends - early bones
    // of this frame are stale, the prepass recomputes them.
    void BonesPrepass_NoteMutation() { BonesPrepass_MarkEarlyDirty(); }
    // Game-side access to UCalc_ModelLock, see IKinematics. The token is the thread's previous lock owner.
    bool LockBones(const void*& unlock_token) override;
    void UnlockBones(const void* unlock_token) override;
    void Callback(UpdateCallback C, void* Param) override
    {
        Update_Callback = C;
        Update_Callback_Param = Param;
    }

    // Callback: data manipulation
    void SetUpdateCallback(UpdateCallback pCallback) override { Update_Callback = pCallback; }
    void SetUpdateCallbackParam(void* pCallbackParam) override { Update_Callback_Param = pCallbackParam; }
    UpdateCallback GetUpdateCallback() override { return Update_Callback; }
    void* GetUpdateCallbackParam() override { return Update_Callback_Param; }
// debug
#ifdef DEBUG
    void DebugRender(Fmatrix& XFORM) override;

protected:
    shared_str getDebugName() override { return dbg_name; }

public:
#endif

    // General "Visual" stuff
    void Copy(dxRender_Visual* pFrom) override;
    void Load(const char* N, IReader* data, u32 dwFlags) override;
    void Load(const char* N, LPCSTR suffix, IReader* data, u32 dwFlags) override;
    void Spawn() override;
    void Depart() override;
    void Release() override;

    IKinematicsAnimated* dcast_PKinematicsAnimated() override { return nullptr; }
    IRenderVisual* dcast_RenderVisual() override { return this; }
    IKinematics* dcast_PKinematics() override { return this; }
    //virtual CKinematics* dcast_PKinematics() { return this; }

    virtual u32 mem_usage(bool bInstance)
    {
        u32 sz = sizeof(*this);
        sz += bone_instances ? bone_instances->mem_usage() : 0;
        if (!bInstance)
        {
            // sz += pUserData?pUserData->mem_usage() : 0;
            for (vecBonesIt b_it = bones->begin(); b_it != bones->end(); ++b_it)
                sz += sizeof(vecBones::value_type) + (*b_it)->mem_usage();
        }
        return sz;
    }

private:
    bool m_is_original_lod;
};

IC CKinematics* PCKinematics(dxRender_Visual* V) { return V ? (CKinematics*)V->dcast_PKinematics() : 0; }
} // namespace xray::render::RENDER_NAMESPACE
