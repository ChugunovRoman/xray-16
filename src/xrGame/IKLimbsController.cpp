#include "StdAfx.h"

#include "IKLimbsController.h"

#include "ik/IKLimb.h"
#include "PhysicsShellHolder.h"

#include "ik_anim_state.h"
#include "xrPhysics/MathUtils.h"
#include "Include/xrRender/RenderVisual.h"
#include "Include/xrRender/Kinematics.h"

#include "CharacterPhysicsSupport.h"
#include "xrCore/Animation/Motion.hpp"
#include "xrCDB/xr_area.h"
#include "xrEngine/IGame_Level.h"
#include "performance_cvars.h"
#include "npc_cpp_profile.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <tracy/Tracy.hpp>
#ifdef DEBUG
#include "PHDebug.h"
#endif // DEBUG

CIKLimbsController::CIKLimbsController()
#ifdef DEBUG
    : m_legs_blend(nullptr), m_object(nullptr), anim_name(nullptr), anim_set_name(nullptr) {}
#else
    : m_legs_blend(nullptr), m_object(nullptr) {}
#endif

void CIKLimbsController::Create(CGameObject* O)
{
    VERIFY(O);
    m_legs_blend = 0;

    IKinematics* K = smart_cast<IKinematics*>(O->Visual());
    m_object = O;
    VERIFY(K);
    u16 sz = 2;
    if (K->LL_UserData() && K->LL_UserData()->section_exist("ik"))
        sz = K->LL_UserData()->r_u16("ik", "num_limbs");
    VERIFY(sz <= max_size);

    _bone_chains.reserve(sz);
    for (u16 i = 0; sz > i; ++i)
        LimbSetup();
    // U1: a generator per limb, different for every NPC and limb (the stalkers' IK in phase B)
    for (u16 i = 0; sz > i; ++i)
        _bone_chains[i].SeedRandom(s32(O->ID()) * s32(max_size) + s32(i) + 1);

    bool already_has_callbacks = !O->visual_callbacks().empty();
    O->add_visual_callback(IKVisualCallback);
    if (already_has_callbacks)
        std::swap(*(O->visual_callbacks().begin()), *(O->visual_callbacks().end() - 1));
    _pose_extrapolation.init(O->XFORM());
}

void CIKLimbsController::LimbSetup()
{
    _bone_chains.push_back(CIKLimb());

    IKinematicsAnimated* skeleton_animated = m_object->Visual()->dcast_PKinematicsAnimated();

    _bone_chains.back().Create((u16)_bone_chains.size() - 1, skeleton_animated, true);
}

void CIKLimbsController::LimbCalculate(SCalculateData& cd)
{
    cd.do_collide = m_legs_blend &&
        !cd.m_limb->KinematicsAnimated()->LL_GetMotionDef(m_legs_blend->motionID)->marks.empty(); // m_legs_blend->;
    cd.m_limb->ApplyState(cd);
}

IC void update_blend(CBlend*& b)
{
    if (b && CBlend::eFREE_SLOT == b->blend_state())
        b = 0;
}

/*
IC float lerp(float t, float a, float b) { return (a + t * (b - a)); }
void y_shift_bones(IKinematics* K, float shift)
{
    u16 bc = K->LL_BoneCount();
    for (u16 i = 0; bc > i; ++i)
        K->LL_GetTransform(i).c.y += shift;
}
*/

float CIKLimbsController::LegLengthShiftLimit(float current_shift, const SCalculateData cd[max_size])
{
    float shift_down = -phInfinity;
    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        if (cd[j].state.foot_step)
        {
            float s_down = cd[j].m_limb->ObjShiftDown(current_shift, cd[j]);
            if (shift_down < s_down)
                shift_down = s_down;
        }
    return shift_down;
}
static const float static_shift_object_speed = .2f;
float CIKLimbsController::StaticObjectShift(const SCalculateData cd[max_size])
{
    const float current_shift = _object_shift.shift();

    u16 cnt = 0;
    float shift_up = 0;
    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        if (cd[j].state.foot_step)
        {
            float s_up = cd[j].cl_shift.y + current_shift;
            if (0.f < s_up)
            {
                shift_up += s_up;
                ++cnt;
            }
        }
    if (0 < cnt)
        shift_up /= cnt;
    float shift_down = LegLengthShiftLimit(current_shift, cd);
    float shift = 0;
    if (shift_down > 0.f)
        shift = -shift_down;
    else if (-shift_down < shift_up)
        shift = -shift_down;
    else
        shift = shift_up;
    VERIFY(_valid(shift));
    _object_shift.set_taget(shift, _abs(current_shift - shift) / static_shift_object_speed);
    return shift;
}

static float doun_shift_to_correct = 0.3f;
//static float doun_shift_correct = 0.1f;

bool CIKLimbsController::PredictObjectShift(const SCalculateData cd[max_size])
{
    float predict_time_shift_down = FLT_MAX;
    float predict_time_shift_up = FLT_MAX;
    float predict_shift_down = 0.f;
    // float predict_shift_up = 0.f;
    bool shift_down = false;
    bool shift_up = false;
    const u16 sz = (u16)_bone_chains.size();
    float current_shift = _object_shift.shift();
    for (u16 j = 0; sz > j; ++j)
        if (!cd[j].state.foot_step)
        {
            float time = cd[j].m_limb->time_to_footstep();

            float lshift = cd[j].m_limb->footstep_shift();
            if (lshift < 0.f)
            {
                if (time < predict_time_shift_down)
                {
                    predict_time_shift_down = time;
                    predict_shift_down = lshift;
                    shift_down = true;
                }
            }
            else if (current_shift < -doun_shift_to_correct && time < predict_time_shift_up)
            {
                predict_time_shift_up = time;

                shift_up = true;
            }
        }
    float predict_shift = 0;
    float predict_time_shift = FLT_MAX;

    if (shift_down)
    {
        predict_shift = predict_shift_down;
        predict_time_shift = predict_time_shift_down;
    }
    else if (shift_up)
    {
        predict_shift = 0;
        predict_time_shift = predict_time_shift_up;
    }
    else
        return false;
    //{
    //	predict_shift = 0;
    //	predict_time_shift = Device.fTimeDelta;
    //}

    if (predict_time_shift < EPS_S)
        predict_time_shift = Device.fTimeDelta;
    _object_shift.set_taget(predict_shift, predict_time_shift);
    return true;
}

void CIKLimbsController::ObjectShift(float static_shift, const SCalculateData cd[max_size])
{
    u16 cnt_in_step = 0;
    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        if (cd[j].m_limb->foot_step())
            ++cnt_in_step;

    [[maybe_unused]] auto sh = smart_cast<CPhysicsShellHolder*>(m_object);
    VERIFY(sh);
    // CCharacterPhysicsSupport *ch = sh->character_physics_support();
    _object_shift.freeze(!!Device.Paused()); // ch->is_interactive_motion() ||

    if (cnt_in_step != sz && PredictObjectShift(cd)) // cnt_in_step > 0 &&
        return;
    StaticObjectShift(cd);
}

void CIKLimbsController::ShiftObject(const SCalculateData cd[max_size])
{
    IKinematics* skeleton_animated = m_object->Visual()->dcast_PKinematics();
    VERIFY(skeleton_animated);
    //	u16 root = skeleton_animated->LL_GetBoneRoot( ) ;

    // CBoneData &BD=skeleton_animated->LL_GetData(root);

    const float y_shift = _object_shift.shift();
    const u16 bones_count = skeleton_animated->LL_BoneCount();
    for (u16 i = 0; i < bones_count; ++i)
        skeleton_animated->LL_GetTransform(i).c.y += y_shift;

    for (u16 i = 0; i < bones_count; ++i)
    {
        CBoneInstance& bi = skeleton_animated->LL_GetBoneInstance(i);
        if (bi.callback())
            bi.callback()(&bi);
        skeleton_animated->LL_GetTransform_R(i).c.y += y_shift;
    }
    //	skeleton_animated->LL_GetTransform(root).c.y += _object_shift.shift();
    //	skeleton_animated->Bone_Calculate(&BD, &Fidentity );
}

int ik_shift_object = 1;
void CIKLimbsController::Calculate()
{
    update_blend(m_legs_blend);

    Fmatrix& obj = m_object->XFORM();
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(phDbgDrawIKSHiftObject))
        _object_shift.dbg_draw(obj, _pose_extrapolation, Fvector().set(0, 2.5f, 0));
#endif

    SCalculateData cd[max_size];

    // U1: phase B on a worker - the limbs' own generators instead of ::Random (stalkers only)
    const bool own_random = m_in_bones.load(std::memory_order_relaxed);
    xr_vector<CIKLimb>::iterator i, b = _bone_chains.begin(), e = _bone_chains.end();
    for (i = b; e != i; ++i)
    {
        i->SetOwnRandom(own_random);
        cd[i - b] = SCalculateData(*i, obj);
        LimbCalculate(cd[i - b]);
    }

    IKinematics* K = m_object->Visual()->dcast_PKinematics();
    u16 root = K->LL_GetBoneRoot();
    CBoneInstance& root_bi = K->LL_GetBoneInstance(root);

    BOOL sv_root_cb_ovwr = root_bi.callback_overwrite();
    BoneCallback sv_root_cb = root_bi.callback();

    root_bi.set_callback(root_bi.callback_type(), 0, root_bi.callback_param(), TRUE);

    if (ik_shift_object) //&& ! m_object->animation_movement_controlled( )
    {
        ShiftObject(cd);
    }

    const u16 sz = (u16)_bone_chains.size();
    for (u16 j = 0; sz > j; ++j)
        cd[j].m_limb->SetGoal(cd[j]);

    for (u16 j = 0; sz > j; ++j)
    {
        cd[j].m_limb->SolveBones(cd[j]);

#ifdef DEBUG
        if (ph_dbg_draw_mask1.test(phDbgDrawIKPredict))
        {
            // IKinematics *K = m_object->Visual()->dcast_PKinematics( );
            u16 ref_bone_id = cd[j].m_limb->dbg_ref_bone_id();
            Fmatrix m = Fmatrix().mul(obj, K->LL_GetTransform(ref_bone_id));
            Fvector toe;
            cd[j].m_limb->dbg_ik_foot().ToePosition(toe);
            m.transform_tiny(toe);
            DBG_DrawLine(toe, Fvector().add(toe, Fvector().set(0, -_object_shift.shift(), 0)), color_xrgb(255, 0, 0));
        }
#endif
    }
    ObjectShift(0, cd);

    root_bi.set_callback(root_bi.callback_type(), sv_root_cb, root_bi.callback_param(), sv_root_cb_ovwr);
}

void CIKLimbsController::Destroy(CGameObject* O)
{
#ifdef _DEBUG
    CPhysicsShellHolder* Sh = smart_cast<CPhysicsShellHolder*>(O);
    VERIFY(Sh);
    CIKLimbsController* ik = Sh->character_ik_controller();
    VERIFY(ik);
    VERIFY(ik == this);
#endif

    O->remove_visual_callback(IKVisualCallback);
    xr_vector<CIKLimb>::iterator i = _bone_chains.begin(), e = _bone_chains.end();
    for (; e != i; ++i)
        i->Destroy();
    _bone_chains.clear();
}

void CIKLimbsController::IKVisualCallback(IKinematics* K)
{
// if (Device.Paused())
//	return;

#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(phDbgIKOff))
        return;
#endif

    CGameObject* O = ((CGameObject*)K->GetUpdateCallbackParam());
    CPhysicsShellHolder* Sh = smart_cast<CPhysicsShellHolder*>(O);
    VERIFY(Sh);
    CIKLimbsController* ik = Sh->character_ik_controller();
    VERIFY(ik);
    // U1: the foot rays this NPC's UpdateCL asked for, here in phase B of its bones - the bone math
    // is done, the model lock is held (the blends, the bone callbacks of foot_matrix_predict), the XFORM of the owner
    // is the one this phase B computes with. Once per request: a second phase B of the frame (a redo) only calculates
    if (ik->m_in_bones_pending.load(std::memory_order_relaxed) &&
        ik->m_in_bones_pending.exchange(false, std::memory_order_acquire))
        ik->UpdateInBones();
    ik->Calculate();
}

void CIKLimbsController::PlayLegs(CBlend* b)
{
    m_legs_blend = b;
#ifdef DEBUG
    IKinematicsAnimated* skeleton_animated = m_object->Visual()->dcast_PKinematicsAnimated();
    VERIFY(skeleton_animated);
    anim_name = skeleton_animated->LL_MotionDefName_dbg(b->motionID).first;
    anim_set_name = skeleton_animated->LL_MotionDefName_dbg(b->motionID).second;

    CMotionDef& MD = *skeleton_animated->LL_GetMotionDef(b->motionID);
    if (MD.marks.empty() && (MD.flags & esmUseFootSteps))
        Msg("! No foot stseps for animation: animation name: %s, animation set: %s ", anim_name, anim_set_name);
#endif
}
void CIKLimbsController::ExecuteIKRayBatch(bool allow_batch)
{
    ZoneScopedN("CIKLimbsController::ExecuteIKRayBatch");
    if (m_ik_batch_groups.empty())
        return;

    const size_t total_rays = m_ik_batch_groups.size() * 3;
    if (npc_cpp_profile::enabled())
        npc_cpp_profile::count(ENpcCppProfileStage::IkRays, total_rays);
    PERF_ADD(ik_rays_n, total_rays);
    m_ik_batch_items.resize(total_rays);
    m_ik_batch_results.resize(total_rays);

    for (size_t g = 0; g < m_ik_batch_groups.size(); ++g)
    {
        IKRayGroup& group = m_ik_batch_groups[g];
        group.first_ray = static_cast<u32>(g * 3);
        for (u32 r = 0; r < 3; ++r)
        {
            const ik_pick_query& q = group.queries[r];
            CObjectSpace::RayPickBatchItem& item = m_ik_batch_items[group.first_ray + r];
            item.start = q.pos();
            item.dir = q.dir();
            item.range = q.range();
            item.tgt = collide::rqtStatic; // the static geometry only, see ik_foot_target() in ik_foot_collider.cpp
            item.ignore_object = group.owner;
            item.result = &m_ik_batch_results[group.first_ray + r];
        }
    }

    // Never batched in phase B of the bones (U1): a nested xr_parallel_for there would wait inside a bone task
    if (allow_batch && static_cast<int>(m_ik_batch_items.size()) >= npc_perf_ik_foot_raypick_batch_min_rays)
    {
        // ik/rays_batch: the wall time of the batch on main; inside its wait main runs other tasks too
        // (task/wait/ik_exec)
        PERF_SCOPE(ik_rays_batch);
        PERF_WAIT_SITE(xray::perf::wait_site::Ik);
        g_pGameLevel->ObjectSpace.RayPickBatch(m_ik_batch_items.data(), m_ik_batch_items.size());
    }
    else
    {
        for (CObjectSpace::RayPickBatchItem& item : m_ik_batch_items)
            g_pGameLevel->ObjectSpace.RayPick(
                item.start, item.dir, item.range, item.tgt, *item.result, item.ignore_object);
    }
}

// All foot rays of this NPC (current feet and step prediction of every limb) go in one batch: gather, cast
// (ExecuteIKRayBatch), then solve each limb from the results. Vanilla cast them limb by limb (CIKLimb::Update).
void CIKLimbsController::SolveFootRays(bool allow_batch)
{
    {
        PERF_SCOPE(ik_gather);
        m_ik_batch_groups.clear();
        for (CIKLimb& limb : _bone_chains)
            limb.GatherRays(m_object, m_legs_blend, _pose_extrapolation, m_ik_batch_groups);
    }
    {
        PERF_SCOPE(ik_rays);
        ExecuteIKRayBatch(allow_batch);
    }
    {
        PERF_SCOPE(ik_process);
        for (const IKRayGroup& group : m_ik_batch_groups)
            group.limb->ProcessGroup(group, m_ik_batch_results.data());
    }
}

// U1, main, the stalker's UpdateCL. What stays here: the legs blend, the early out, UpdateTracks (its
// blend callbacks are game code, Lua) and the pose extrapolation (sampled at the UpdateCL as before). The rest
// (UpdateInBones) casts rays at the static geometry only (thread_local buffers of CObjectSpace) and reads this model,
// its limbs and the XFORM of its owner - nothing of the game logic, no Lua, no event bus, no other model.
void CIKLimbsController::RequestUpdateInBones()
{
    ZoneScopedN("CIKLimbsController::RequestUpdateInBones");
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(phDbgIKOff))
        return;
#endif
    m_in_bones.store(true, std::memory_order_relaxed);
    update_blend(m_legs_blend);

    // As in Update(): nothing to refine. A request of an earlier frame no bones took yet stays pending
    if (!m_legs_blend && fis_zero(_object_shift.shift(), EPS_L))
        return;

    IKinematicsAnimated* skeleton_animated = m_object->Visual()->dcast_PKinematicsAnimated();
    VERIFY(skeleton_animated);

    {
        PERF_SCOPE(ik_tracks);
        skeleton_animated->UpdateTracks();
    }

    _pose_extrapolation.update(m_object->XFORM());

    m_in_bones_frame = Device.dwFrame;
    m_in_bones_pending.store(true, std::memory_order_release);
}

// U1: phase B of this model's bones (IKVisualCallback): a worker of the bones prepass, a render thread, or main when
// game code needs the bones first (the fire point of the weapon in the hands). The rays one by one: no xr_parallel_for
// inside a bone task. ik/deferred_late = a request of an earlier frame (no bones were computed in its own frame: an
// NPC nobody drew)
void CIKLimbsController::UpdateInBones()
{
    ZoneScopedN("CIKLimbsController::UpdateInBones");
    PERF_SCOPE_BY_THREAD(ik_deferred_run_main, ik_deferred_run_game, ik_deferred_run_worker);
    if (m_in_bones_frame != Device.dwFrame)
        PERF_INC(ik_deferred_late);
    update_blend(m_legs_blend);
    SolveFootRays(false);
}

void CIKLimbsController::Update()
{
    ZoneScopedN("CIKLimbsController::Update");
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(phDbgIKOff))
        return;
#endif
    // U1: back on main (the switch went off): a request still pending is replaced by this run
    m_in_bones_pending.store(false, std::memory_order_relaxed);
    m_in_bones.store(false, std::memory_order_relaxed);
    update_blend(m_legs_blend);

    // No active leg animation and no residual body shift means IK has nothing useful to refine this frame.
    if (!m_legs_blend && fis_zero(_object_shift.shift(), EPS_L))
        return;

    IKinematicsAnimated* skeleton_animated = m_object->Visual()->dcast_PKinematicsAnimated();
    VERIFY(skeleton_animated);

    {
        PERF_SCOPE(ik_tracks);
        skeleton_animated->UpdateTracks();
    }

    _pose_extrapolation.update(m_object->XFORM());

    SolveFootRays(true);

    /*
    Fmatrix predict;
    _pose_extrapolation.extrapolate( predict, Device.dwTimeGlobal  );




    DBG_DrawMatrix( m_object->XFORM(), 1 );
    DBG_DrawMatrix( predict, 1 );

    _pose_extrapolation.extrapolate( predict, Device.dwTimeGlobal + 1  );
    DBG_DrawMatrix( predict, 1 );
    */
}
