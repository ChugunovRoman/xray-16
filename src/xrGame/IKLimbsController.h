#pragma once

#include "ik/IKLimb.h"
#include "pose_extrapolation.h"
#include "ik_object_shift.h"
#include "xrCDB/xr_area.h"
#include <atomic>
class IKinematicsAnimated;
class CGameObject;
class CBlend;
struct SIKCrlCalcData;

// U1 of plans/optimization_mt/28-updatecl-main.md (always on since 2026-10-18, its switch npc_ik_in_bones removed,
// 41-cvar-cleanup.md): the IK of the feet of stalkers (not the actor) runs in phase B of the bones of their own model
// (IKVisualCallback, on the worker that computes them, the model lock held) instead of their UpdateCL on main.

class CIKLimbsController
{
private:
    static const u16 max_size = 4;

public:
    CIKLimbsController();
    void Create(CGameObject* O);
    void Destroy(CGameObject* O);

public:
    void PlayLegs(CBlend* b);
    void Update();
    // U1: on main what Update() does before the rays (the legs blend, the early out, the animation
    // tracks, the pose extrapolation), the rest (GatherRays, the rays one by one, ProcessGroup) is left to phase B of the
    // bones of this model (IKVisualCallback, before Calculate)
    void RequestUpdateInBones();
    float Shift() { return _object_shift.shift(); }
private:
    void Calculate();
    void LimbCalculate(SCalculateData& cd);
    void ShiftObject(const SCalculateData cd[max_size]);
    float StaticObjectShift(const SCalculateData cd[max_size]);
    float LegLengthShiftLimit(float current_shift, const SCalculateData cd[max_size]);
    bool PredictObjectShift(const SCalculateData cd[max_size]);
    void ObjectShift(float static_shift, const SCalculateData cd[max_size]);
    void LimbSetup();

    /** Per-NPC IK ray batch state. */
    void ExecuteIKRayBatch(bool allow_batch);
    // GatherRays of every limb, the rays (one RayPickBatch when allow_batch and there are enough, else one by one),
    // ProcessGroup of every group
    void SolveFootRays(bool allow_batch);
    // U1: the pending part of RequestUpdateInBones, phase B of this model's bones (IKVisualCallback)
    void UpdateInBones();
    xr_vector<CObjectSpace::RayPickBatchItem> m_ik_batch_items;
    xr_vector<collide::rq_result> m_ik_batch_results;
    xr_vector<IKRayGroup> m_ik_batch_groups;

private:
    static void IKVisualCallback(IKinematics* K);

private:
    CBlend* m_legs_blend;
    CGameObject* m_object;
    xr_vector<CIKLimb> _bone_chains;
    object_shift _object_shift;
    extrapolation::points _pose_extrapolation;

    // U1: set by RequestUpdateInBones (main, UpdateCL), taken by the first phase B of this model's bones after it
    std::atomic<bool> m_in_bones_pending{false};
    u32 m_in_bones_frame{0}; // Device.dwFrame of that request (ik/deferred_late), written before the flag
    // The last update came through RequestUpdateInBones: the limbs use their own CRandom in Calculate (phase B on a
    // worker) instead of the global ::Random; Update() resets it
    std::atomic<bool> m_in_bones{false};

#ifdef DEBUG
    LPCSTR anim_name;
    LPCSTR anim_set_name;
#endif
};
