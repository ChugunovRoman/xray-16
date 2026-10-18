#pragma once
#include "limb.h"
#include "IKFoot.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "xrCommon/xr_vector.h"
#include "ik_anim_state.h"
#include "ik_calculate_data.h"
#include "ik_limb_state.h"
#include "ik_collide_data.h"
#include "ik_limb_state_predict.h"

class IKinematics;
struct SCalculateData;
struct SIKCollideData;
class CGameObject;
class motion_marks;
class ik_goal_matrix;

/** One group of 3 primary IK-foot rays for the per-NPC batch path. */
struct IKRayGroup
{
    CIKLimb* limb{};
    ik_foot_collider* collider{};
    ik_foot_geom foot_geom;
    CGameObject* owner{};
    bool foot_step{};
    bool is_predict{};
    u16 predict_ref_bone{ u16(-1) }; // valid only if is_predict
    Fmatrix gl_goal;                 // valid only if is_predict
    u32 first_ray{};                 // index into the global batch results array
    ik_pick_query queries[3];
};
namespace CDB
{
class TRI;
}
namespace extrapolation
{
class points;
};
class CIKLimb
{
private:
    //			friend		class	ik_limb_state;
public:
    CIKLimb();
    CIKLimb(const CIKLimb& l);

    CIKLimb& operator=(const CIKLimb& l);

public:
    void Create(u16 id, IKinematicsAnimated* K, bool collide_);
    void Destroy();

public:
    void SolveBones(SCalculateData& cd);
    void ApplyState(SCalculateData& cd);
    void SetGoal(SCalculateData& cd);

    /** Per-NPC batch path (the only one; replaced the vanilla per-limb Update / step_predict).
     *  GatherRays fills out with ray groups that need execution (current foot + step prediction);
     *  ProcessGroup reads back the results of one group after CIKLimbsController::ExecuteIKRayBatch. */
    void GatherRays(CGameObject* O, const CBlend* b, const extrapolation::points& object_pose_extrapolation,
        xr_vector<IKRayGroup>& out);
    void ProcessGroup(const IKRayGroup& group, const collide::rq_result* results);

public:
    IC u16 get_id() const { return m_id; }
    float ObjShiftDown(float current_shift, const SCalculateData& cd) const;
    IC Fmatrix& ref_bone_to_foot(Fmatrix& ref_bone) const;
    IC IKinematics* Kinematics() const { return m_foot.Kinematics(); }
    IC IKinematicsAnimated* KinematicsAnimated() const { return m_K; }
    IC u16 ref_bone() const { return m_foot.ref_bone(); }
    Fmatrix& transform(Fmatrix& m, u16 bone0, u16 bone1) const;
    float time_to_footstep() const { return state_predict.time_to_footstep; }
    float footstep_shift() const { return state_predict.footstep_shift; }
    bool foot_step() const { return sv_state.foot_step(); }
    u16 foot_matrix_predict(Fmatrix& foot, Fmatrix& toe, float time, IKinematicsAnimated* K) const;
    // U1 of 28-updatecl-main.md: the random of the unstuck time of a new foot position comes from the
    // own generator of this limb (seeded from the object ID) instead of the global ::Random, which is not thread safe -
    // Calculate runs in phase B of the bones, on a worker. own = false: ::Random as before
    void SeedRandom(s32 seed) { m_random.seed(seed); }
    void SetOwnRandom(bool own) { m_own_random = own; }
#ifdef DEBUG
    u16 dbg_ref_bone_id() const { return m_bones[m_foot.ref_bone()]; }
    const CIKFoot& dbg_ik_foot() const { return m_foot; }
#endif
private:
    float get_time_to_step_begin(const CBlend& B) const;
    void Invalidate();
    CRandom& random() { return m_own_random ? m_random : ::Random; }

private:
    void Solve(SCalculateData& cd);
    IC void AnimGoal(Fmatrix& gl);
    void SetAnimGoal(SCalculateData& cd);
    void SetNewGoal(const SIKCollideData& cld, SCalculateData& cd);
    void SetNewStepGoal(const SIKCollideData& cld, SCalculateData& cd);
    void Blending(SCalculateData& cd);
    bool blend_collide(
        ik_goal_matrix& m, const SCalculateData& cd, const ik_goal_matrix& m0, const ik_goal_matrix& m1) const;
    bool SetGoalToLimb(const SCalculateData& cd);
    void CalculateBones(SCalculateData& cd);
    Matrix& Goal(Matrix& gl, const Fmatrix& xm, const SCalculateData& cd);
    Fmatrix& GetHipInvert(Fmatrix& ihip, const SCalculateData& cd);

    float SwivelAngle(const Fmatrix& ihip, const SCalculateData& cd);
    void GetKnee(Fvector& knee, const SCalculateData& cd) const;
    void GetPickDir(Fvector& v, SCalculateData& cd) const;
    void ToeTimeDiff(Fvector& v, const SCalculateData& cd) const;
    void ToeTimeDiffPredict(Fvector& v) const;
    IC static void get_start(Fmatrix& start, SCalculateData& D, u16 bone);
#ifdef DEBUG
    void DBGDrawSetNewGoal(SCalculateData& cd, const SIKCollideData& cld);
#endif
private:
    static void BonesCallback0(CBoneInstance* B);
    static void BonesCallback1(CBoneInstance* B);
    static void BonesCallback2(CBoneInstance* B);

private:
    Limb m_limb;
    IKinematicsAnimated* m_K;
    CIKFoot m_foot;
    ik_foot_collider collider;
    u16 m_bones[4];
    u16 m_id;

    bool m_collide;
    SIKCollideData collide_data;

    ik_anim_state anim_state;
    ik_limb_state sv_state;
    ik_limb_state_predict state_predict;
    CRandom m_random; // U1, see SeedRandom
    bool m_own_random{false};
#ifdef DEBUG
    bool dbg_disabled;
#endif
#ifdef IK_DBG_STATE_SEQUENCE
    friend struct dbg_matrises;
    dbg_matrises m_dbg_matrises;
#endif
};
