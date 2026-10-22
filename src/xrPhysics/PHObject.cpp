#include "StdAfx.h"

#include "xrEngine/IGame_Persistent.h"

#include "Physics.h"
#include "PHObject.h"
#include "PHWorld.h"
#include "PHMoveStorage.h"
#include "dRayMotions.h"
#include "PHCollideValidator.h"
#include "console_vars.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "PHStepDefer.h"
#include "PHCharacter.h"
#include <atomic>
#ifdef DEBUG
#include "debug_output.h"
#endif

extern CPHWorld* ph_world;

namespace
{
thread_local xr_vector<ISpatial*> tls_ph_spatial;
IC qResultVec& PhTlsSpatial() { return tls_ph_spatial; }

// Crowd broadphase (plans/optimization_mt/21-physics-step.md, journal 2026-10-15): ISpatial* -> CPHObject* without
// dynamic_cast. ISpatial is a virtual base of SpatialBase, so a static_cast cannot reach CPHObject from it and smart_cast
// is a full dynamic_cast (~33 ns hot, MSVC, measured on the same class layout) - paid for every q_box result: ~46 000
// per substep in a crowd of ~600 NPCs. Every ISpatial of SpatialSpacePhysic is a CPHObject (its constructor is the only
// SpatialBase built on that DB), and SpatialBase::GetSpatialData() (final) returns its own member `spatial`, which lies
// at the same offset in every SpatialBase subobject (the non-virtual part of a class has one layout). The offset is
// taken from a real object in the constructor below - no offsetof on a class with a virtual base. Before the first
// CPHObject is built the offset is unknown and the dynamic_cast is used; DEBUG checks the fast result against it.
std::atomic<ptrdiff_t> s_spatial_data_offset{-1};

IC CPHObject* PhObjectFromSpatial(ISpatial* sp)
{
    const ptrdiff_t offset = s_spatial_data_offset.load(std::memory_order_relaxed);
    if (offset < 0)
        return smart_cast<CPHObject*>(sp);
    char* data = reinterpret_cast<char*>(&sp->GetSpatialData());
    CPHObject* obj = static_cast<CPHObject*>(reinterpret_cast<SpatialBase*>(data - offset));
    VERIFY(obj == smart_cast<CPHObject*>(sp));
    return obj;
}

// Objects whose CPHObject::AABB is the exact AABB of the geometry NearCallback collides (dSpacedGeom): shells
// (CPHShell::get_spatial_params, the split shell too) and characters (CPHSimpleCharacter::get_spatial_params, the
// movement-test cap included). Not CPHStaticGeomShell (its get_spatial_params fills a local AABB - the member is never
// set; ladders also need near_callback for every q_box hit) and not the activation shape.
IC bool HasExactPhAABB(CPHObject* obj)
{
    const CPHObject::ECastType tp = obj->CastType();
    return tp == CPHObject::tpShell || tp == CPHObject::tpCharacter;
}

// A live NPC's movement controller (CPHAICharacter), not the actor's.
IC bool IsAiController(CPHObject* obj)
{
    return obj->CastType() == CPHObject::tpCharacter && static_cast<CPHCharacter*>(obj)->CastAICharacter() != nullptr;
}

// Corpse pairs dropped in the broadphase (plans/optimization_mt/21-physics-step.md, journal 2026-10-16; permanent after
// the A/B test, limits ph_corpse_vs_corpse_dist / ph_corpse_vs_npc_dist, see console_vars.h). A heavy fight (log s_m)
// spends most of the narrowphase on pairs with a corpse: ~1.9 us per pair against ~0.5 us for two NPC capsules (a ragdoll
// is ~11 bodies, space x space in NearCallback). The developer's call: corpse-vs-corpse contacts do not matter,
// corpse-vs-NPC ones far from the camera do not either - such a pair never reaches NearCallback (no dCollide, no merge,
// no wake). Since plans/npc-corpse-collision.md corpse-vs-NPC is dropped at any distance (ph_npc_corpse_collide 0): a
// live NPC stopped dead at a corpse it could not push (a force-slept one is a wall for it, see Physics.cpp).
// "Corpse" = a ragdoll-class shell: cbClassRagDoll is set only by CPHShell::SetRagDoll(), called by
// CCharacterPhysicsSupport for the death shell of a stalker / monster / the actor - never for props, items, vehicles,
// the hit shell of a live NPC (physics_shell_animated) or the activation shape of a death (it is not a shell).
enum class CorpseRuleSide : u8
{
    None, // neither rule applies to this object
    Corpse,
    Npc, // a live NPC's controller (not the actor: actor-corpse stays as it is)
};

IC CorpseRuleSide GetCorpseRuleSide(CPHObject* obj)
{
    if (CPHCollideValidator::IsRagDollObject(*obj))
        return CorpseRuleSide::Corpse;
    if (IsAiController(obj))
        return CorpseRuleSide::Npc;
    return CorpseRuleSide::None;
}

// dist > 0: the object's broadphase centre (spatial.sphere.P, the last spatial_move) is farther than dist from cam.
IC bool FarFromCamera(const Fvector& pos, const Fvector& cam, float dist)
{
    return pos.distance_to_sqr(cam) > dist * dist;
}

#if XR_PERF_METRICS
// physics/broadphase/pair/*: the candidates that go on to NearCallback, by pair type (this object x the other one).
void CountBroadphasePair(CPHObject* obj1, CPHObject* obj2)
{
    using M = xray::perf::Metric;
    const bool ai1 = IsAiController(obj1);
    const bool ai2 = IsAiController(obj2);
    const bool rd1 = CPHCollideValidator::IsRagDollObject(*obj1);
    const bool rd2 = CPHCollideValidator::IsRagDollObject(*obj2);
    if (ai1 && ai2)
        xray::perf::Add(M::ph_bp_pair_ai_ai, 1);
    else if ((ai1 && rd2) || (rd1 && ai2))
        xray::perf::Add(M::ph_bp_pair_ai_ragdoll, 1);
    else if (ai1 || ai2)
        xray::perf::Add(M::ph_bp_pair_ai_other, 1);
    else if (rd1 || rd2)
    {
        xray::perf::Add(M::ph_bp_pair_ragdoll, 1);
        // The corpse-vs-corpse part of pair/ragdoll (the rest: corpse x item / prop / vehicle / actor).
        if (rd1 && rd2)
            xray::perf::Add(M::ph_bp_pair_ragdoll_ragdoll, 1);
    }
    else
        xray::perf::Add(M::ph_bp_pair_other, 1);
}
#endif
} // namespace

CPHObject::CPHObject() : SpatialBase(g_pGamePersistent->SpatialSpacePhysic)
{
    m_flags.flags = 0;
    spatial.type |= STYPE_PHYSIC;
    m_island.Init();
    m_check_count = 0;
    m_post_flags = 0;
    CPHCollideValidator::InitObject(*this);
    // The same value from every object (see PhObjectFromSpatial).
    s_spatial_data_offset.store(
        reinterpret_cast<char*>(&spatial) - reinterpret_cast<char*>(static_cast<SpatialBase*>(this)),
        std::memory_order_relaxed);
}

void CPHObject::activate()
{
    R_ASSERT2(dSpacedGeom(), "trying to activate destroyed or not created object!");
    if (m_flags.test(st_activated))
        return;
    // Edits CPHWorld::m_objects, which the step walks. Shell/element wake-ups from other threads are deferred before
    // they get here (PHStepDefer.h); what still arrives is a path left to find.
    PH_STEP_HAZARD("CPHObject::activate");
    if (m_flags.test(st_freezed))
    {
        UnFreeze();
        return;
    }
    if (m_flags.test(st_recently_deactivated))
        remove_from_recently_deactivated();
    ph_world->AddObject(this);
    vis_update_activate();
    m_flags.set(st_activated, TRUE);
}

void CPHObject::EnableObject(CPHObject* obj) { activate(); }
void CPHObject::deactivate()
{
    if (!m_flags.test(st_activated))
        return;
    // The parallel post-integrate part of this object (a character's sleep check in PhDataUpdate): m_objects is edited by
    // PostIntegrateSerial on the stepping thread, in list order. The object stays activated until then; nothing in the
    // rest of its PhDataUpdate reads that, and a repeated call is the same no-op it was before.
    if (m_post_flags & pf_parallel)
    {
        m_post_flags |= pf_deactivate;
        return;
    }
    PH_STEP_HAZARD("CPHObject::deactivate");
    VERIFY2(m_island.IsActive(), "can not do it during processing");

    if (!m_island.IsActive())
        Msg("! CPHObject::deactivate: island is merged (deactivation during physics processing)");
    ph_world->RemoveObject(PH_OBJECT_I(this));
    vis_update_deactivate();
    m_flags.set(st_activated, FALSE);
}

void CPHObject::put_in_recently_deactivated()
{
    VERIFY(!m_flags.test(st_activated) && !m_flags.test(st_freezed));
    if (m_flags.test(st_recently_deactivated))
        return;
    m_check_count = u8(ph_console::ph_tri_clear_disable_count);
    m_flags.set(st_recently_deactivated, TRUE);
    ph_world->AddRecentlyDisabled(this);
}
void CPHObject::remove_from_recently_deactivated()
{
    if (!m_flags.test(st_recently_deactivated))
        return;
    m_check_count = 0;
    m_flags.set(st_recently_deactivated, FALSE);
    ph_world->RemoveFromRecentlyDisabled(PH_OBJECT_I(this));
}
void CPHObject::check_recently_deactivated()
{
    if (m_check_count == 0)
    {
        ClearRecentlyDeactivated();
        remove_from_recently_deactivated();
    }
    else
        m_check_count--;
}
void CPHObject::spatial_move()
{
    SpatialBase::spatial_move();
    m_flags.set(st_dirty, TRUE);
    // Invalidate the broadphase caches here: they capture raw pointers at q_box time and must not
    // outlive the processing pass that filled them. spatial_move() ends per-object processing in
    // CPHWorld::Step()/StepTouch(); without this clear the raw ISpatial* persist across frames and
    // dangle once a cached neighbor is destroyed (reinit_single walks them -> AV in smart_cast).
    m_collide_spatial_overlap.clear();
    m_collide_dyn_candidates.clear();
}

// Parallel post-integrate (plans/optimization_mt/21-physics-step.md, permanent): the island-solve task of this
// object's merge root, after IslandStep and this object's IslandReinit. Touches only this object: its bodies and geoms
// (no joint or body of it is in another island), its own fields and spatial bounds. spatial_move() without the octree
// rebuild: the bounds refresh and the node test are SpatialBase::spatial_move's lock-free part (single writer per
// object, B-1 in xrCDB/ISpatial.cpp); the rebuild is left to PostIntegrateSerial, so that the octree items keep the order
// of the serial loop (q_box result order -> contact order of the next substep).
void CPHObject::PostIntegrateParallel(dReal step)
{
    VERIFY(PhDataUpdateSplit() && !m_post_flags);
    m_post_flags = pf_parallel;
    PhDataUpdateParallel(step);
    if (spatial.node_ptr)
    {
        spatial_refresh_bounds();
        spatial.type |= STYPEFLAG_INVALIDSECTOR;
        if (spatial_inside())
        {
            PERF_INC(spatial_move_calls);
            PERF_INC(spatial_move_idle);
        }
        else
            m_post_flags |= pf_rebuild; // counted as a call (and a rebuild) by SpatialBase::spatial_move
    }
    m_flags.set(st_dirty, TRUE);
    m_collide_spatial_overlap.clear();
    m_collide_dyn_candidates.clear();
    m_post_flags = u8((m_post_flags & ~pf_parallel) | pf_done);
}

// The serial rest on the stepping thread, in m_objects order: the deactivation recorded above, the type's own serial part
// (CPHShell: DisableObject + put_in_recently_deactivated / Disable), the octree rebuild. The same order as in
// PhDataUpdate() + spatial_move().
void CPHObject::PostIntegrateSerial(dReal step)
{
    const u8 flags = m_post_flags;
    m_post_flags = 0;
    if (flags & pf_deactivate)
    {
        PERF_INC(ph_post_mt_deactivate);
        deactivate();
    }
    PhDataUpdateSerial(step);
    if (flags & pf_rebuild)
    {
        PERF_INC(ph_post_mt_rebuild);
        SpatialBase::spatial_move(); // refreshes the same bounds again, then remove + insert under the exclusive lock
    }
}

void CPHObject::CollideRayMotions()
{
    if (!m_flags.test(fl_ray_motions))
        return;
    CPHMoveStorage* tracers = MoveStorage();
    CPHMoveStorage::iterator I = tracers->begin(), E = tracers->end();
    for (; E != I; ++I)
    {
        const Fvector *from = 0, *to = 0;
        Fvector dir;
        I.Positions(from, to);
        if (from->x == -dInfinity)
            continue;
        dir.sub(*to, *from);
        float magnitude = dir.magnitude();
        if (magnitude < EPS)
            continue;
        dir.mul(1.f / magnitude);
        qResultVec& result = PhTlsSpatial();
        result.clear();
        PERF_INC(ph_broadphase_calls);
        SpatialCaller spatial_caller(SpatialQueryCaller::physics);
        g_pGamePersistent->SpatialSpacePhysic.q_ray(
            result, 0, STYPE_PHYSIC, *from, dir, magnitude); //|ISpatial_DB::O_ONLYFIRST
        PERF_ADD(ph_broadphase_results, result.size());
#ifdef DEBUG
        if (debug_output().ph_dbg_draw_mask().test(phDbgDrawRayMotions))
        {
            debug_output().DBG_OpenCashedDraw();
            debug_output().DBG_DrawLine(
                *from, Fvector().add(*from, Fvector().mul(dir, magnitude)), color_xrgb(0, 255, 0));
            debug_output().DBG_ClosedCashedDraw(30000);
        }

#endif
        auto i = result.begin(), e = result.end();
        u32 ray_candidates = 0;
        // The corpse pair rules of CollideDynamicsBroadphase for the ray motions too (a fast corpse after an explosion
        // traces its motion here): per pair, ray motions are rare
        const float corpse_corpse_dist = ph_console::ph_corpse_vs_corpse_dist;
        const float corpse_npc_dist = ph_console::ph_corpse_vs_npc_dist;
        const bool npc_corpse_pass = ph_console::ph_npc_corpse_collide == 0;
        const CorpseRuleSide s1 = GetCorpseRuleSide(this);
        const auto corpse_pair_skipped = [&](CPHObject* obj2) {
            if (s1 == CorpseRuleSide::None)
                return false;
            const CorpseRuleSide s2 = GetCorpseRuleSide(obj2);
            const Fvector& cam = ph_world->StepCameraPosition();
            if (s1 == CorpseRuleSide::Corpse && s2 == CorpseRuleSide::Corpse)
            {
                return corpse_corpse_dist <= 0.f || (FarFromCamera(spatial.sphere.P, cam, corpse_corpse_dist) &&
                    FarFromCamera(obj2->spatial.sphere.P, cam, corpse_corpse_dist));
            }
            // Corpse x live NPC controller: at any distance by default (ph_npc_corpse_collide 0)
            if (npc_corpse_pass && s2 != CorpseRuleSide::None && s1 != s2)
                return true;
            if (corpse_npc_dist > 0.f && s1 != CorpseRuleSide::None && s2 != CorpseRuleSide::None && s1 != s2)
            {
                return FarFromCamera(spatial.sphere.P, cam, corpse_npc_dist) &&
                    FarFromCamera(obj2->spatial.sphere.P, cam, corpse_npc_dist);
            }
            return false;
        };
        for (; i != e; ++i)
        {
            CPHObject* obj2 = smart_cast<CPHObject*>(*i);
            if (!obj2)
                continue;
            if (obj2 == this || !obj2->m_flags.test(st_dirty))
                continue;
            if (corpse_pair_skipped(obj2))
                continue;
            ++ray_candidates;
            dGeomID motion_ray = ph_world->GetMotionRayGeom();
            dGeomRayMotionSetGeom(motion_ray, I.dGeom());
            dGeomRayMotionsSet(motion_ray, (const dReal*)from, (const dReal*)&dir, magnitude);
            NearCallback(this, obj2, motion_ray, obj2->dSpacedGeom());
        }
        PERF_ADD(ph_broadphase_candidates, ray_candidates);
    }
}

void CPHObject::CollideDynamicsBroadphase()
{
    qResultVec& result = PhTlsSpatial();
    result.clear();
    PERF_INC(ph_broadphase_calls);
    SpatialCaller spatial_caller(SpatialQueryCaller::physics);
    g_pGamePersistent->SpatialSpacePhysic.q_box(result, 0, STYPE_PHYSIC, spatial.sphere.P, AABB);
    PERF_ADD(ph_broadphase_results, result.size());
    m_collide_spatial_overlap.assign(result.begin(), result.end());
    m_collide_dyn_candidates.clear();
    m_collide_dyn_candidates.reserve(result.size());
    // Crowd broadphase (plans/optimization_mt/21-physics-step.md, journal 2026-10-15), exact AABB filter: q_box tests
    // the query box (this object's exact AABB) against the CUBE of the other object's bounding sphere (radius = its
    // largest half-extent). For an upright NPC controller that cube is 0.88 m in x/z instead of 0.4 m, so ~60 % of
    // the neighbours it returns in a crowd cannot touch: NearCallback still collides them space x space (dSpaceCollide2
    // does not test the two space AABBs first - every geom of one space against every geom of the other). The exact
    // AABB of both sides drops them first. Both AABBs come from the last spatial_move (end of the previous substep or a
    // teleport), i.e. from the same body positions dCollide sees now; the margin only absorbs float rounding. Only when
    // both AABBs are exact (HasExactPhAABB) and this object is in the spatial DB (an unregistered one keeps a stale AABB).
    const bool aabb_filter = spatial.node_ptr != nullptr && HasExactPhAABB(this);
    constexpr float aabb_margin = 0.02f;
    [[maybe_unused]] const bool count_pairs = xray::perf::g_perf_metrics_active && ph_world->Processing();

    // Corpse pairs (see CorpseRuleSide above), every path: the frame step and the local steps alike - a TryPosition
    // probe of an NPC far away does not see the corpse either, so it walks through it instead of failing the probe and
    // then passing through it in the frame step. A pair is near when either object is within the distance of the
    // camera (StepCameraPosition: the Kick snapshot inside the frame step, the live camera on main). The side and the
    // distances of this object are taken once per query; per candidate it is one class bit test unless a rule applies.
    // ph_npc_corpse_collide 0 (default, plans/npc-corpse-collision.md): corpse x live NPC controller is skipped at any
    // distance - "far" always, no camera needed for it.
    const float corpse_corpse_dist = ph_console::ph_corpse_vs_corpse_dist;
    const float corpse_npc_dist = ph_console::ph_corpse_vs_npc_dist;
    const bool npc_corpse_pass = ph_console::ph_npc_corpse_collide == 0;
    const CorpseRuleSide corpse_side = GetCorpseRuleSide(this);
    const bool corpse_corpse_rule = corpse_side == CorpseRuleSide::Corpse;
    const bool corpse_npc_rule = (npc_corpse_pass || corpse_npc_dist > 0.f) && corpse_side != CorpseRuleSide::None;
    const Fvector* camera = nullptr;
    bool this_far_corpse = true; // for ph_corpse_vs_corpse_dist (0 = everywhere: "far" always)
    bool this_far_npc = npc_corpse_pass;
    if (corpse_corpse_rule || (corpse_npc_rule && !npc_corpse_pass))
    {
        camera = &ph_world->StepCameraPosition();
        if (corpse_corpse_rule && corpse_corpse_dist > 0.f)
            this_far_corpse = FarFromCamera(spatial.sphere.P, *camera, corpse_corpse_dist);
        if (corpse_npc_rule && !npc_corpse_pass)
            this_far_npc = FarFromCamera(spatial.sphere.P, *camera, corpse_npc_dist);
    }

    for (ISpatial* sp : result)
    {
        CPHObject* obj2 = PhObjectFromSpatial(sp);
        if (!obj2)
            continue;
        if (obj2 == this || !obj2->m_flags.test(st_dirty))
            continue;
        if (!CPHCollideValidator::DoCollide(*this, *obj2))
            continue;
        if (aabb_filter && HasExactPhAABB(obj2))
        {
            const Fvector& c1 = spatial.sphere.P;
            const Fvector& c2 = obj2->spatial.sphere.P;
            const Fvector& e2 = obj2->AABB;
            if (_abs(c1.x - c2.x) > AABB.x + e2.x + aabb_margin || _abs(c1.y - c2.y) > AABB.y + e2.y + aabb_margin ||
                _abs(c1.z - c2.z) > AABB.z + e2.z + aabb_margin)
            {
                PERF_INC(ph_broadphase_aabb_reject);
                continue;
            }
        }
        // After the exact AABB filter: the skip counters are the NearCallback calls saved.
        if (corpse_corpse_rule && CPHCollideValidator::IsRagDollObject(*obj2))
        {
            // Corpse x corpse: everywhere or beyond ph_corpse_vs_corpse_dist of the camera.
            if (this_far_corpse &&
                (corpse_corpse_dist <= 0.f || FarFromCamera(obj2->spatial.sphere.P, *camera, corpse_corpse_dist)))
            {
#if XR_PERF_METRICS
                if (count_pairs)
                    xray::perf::Add(xray::perf::Metric::ph_bp_skip_corpse_corpse, 1);
#endif
                continue;
            }
        }
        else if (corpse_npc_rule && this_far_npc &&
            (corpse_side == CorpseRuleSide::Corpse ? IsAiController(obj2) :
                                                     CPHCollideValidator::IsRagDollObject(*obj2)) &&
            (npc_corpse_pass || FarFromCamera(obj2->spatial.sphere.P, *camera, corpse_npc_dist)))
        {
            // Corpse x live NPC controller: everywhere by default (ph_npc_corpse_collide 0) - the NPC walks through the
            // corpse, the corpse is not pushed or woken. ph_npc_corpse_collide 1 (the old behaviour): only with both
            // beyond ph_corpse_vs_npc_dist N m of the camera; within N m NearCallback as before (incl. the one-sided
            // contact of an NPC with a force-slept corpse, where the corpse is a wall for the NPC).
#if XR_PERF_METRICS
            // The same frame-step condition as pair/*; _local = the other paths (TryPosition probes, world steps on main).
            if (count_pairs)
                xray::perf::Add(xray::perf::Metric::ph_bp_skip_corpse_npc, 1);
            else if (xray::perf::g_perf_metrics_active)
                xray::perf::Add(xray::perf::Metric::ph_bp_skip_corpse_npc_local, 1);
#endif
            continue;
        }
#if XR_PERF_METRICS
        if (count_pairs)
            CountBroadphasePair(this, obj2);
#endif
        m_collide_dyn_candidates.push_back(obj2);
    }
    PERF_ADD(ph_broadphase_candidates, m_collide_dyn_candidates.size());
}

void CPHObject::CollideDynamicsNarrow()
{
    // Match legacy per-object q_box loop: skip partner once it already ran CollideClearDirty this step.
    for (CPHObject* obj2 : m_collide_dyn_candidates)
    {
        if (!obj2->m_flags.test(st_dirty))
            continue;
        NearCallback(this, obj2, dSpacedGeom(), obj2->dSpacedGeom());
    }
}

void CPHObject::CollideStepPostBroadphase()
{
    {
        // F0 (plans/optimization_mt/21-physics-step.md): dynamic pairs, the frame step on the physics thread only.
        PERF_SCOPE_IF(ph_world->Processing(), ph_step_narrow);
        CollideRayMotions();
        CollideDynamicsNarrow();
    }
    if (CPHCollideValidator::DoCollideStatic(*this))
    {
        // physics/step/static_ai: the level mesh part of physics/step/static paid by live NPC controllers.
        PERF_SCOPE_IF(xray::perf::g_perf_metrics_active && ph_world->Processing() && IsAiController(this),
            ph_step_static_ai);
        CollideStatic(dSpacedGeom(), this);
    }
    CollideClearDirty();
}

void CPHObject::Collide()
{
    // physics/step/collide_ai: the whole Collide() (q_box + pairs + level mesh) of live NPC controllers, the frame step
    // on the physics thread only - their share of physics/step/collide.
    PERF_SCOPE_IF(xray::perf::g_perf_metrics_active && ph_world->Processing() && IsAiController(this),
        ph_step_collide_ai);
    {
        // F0 (plans/optimization_mt/21-physics-step.md): q_box + filter, the frame step on the physics thread only.
        PERF_SCOPE_IF(ph_world->Processing(), ph_step_broadphase);
        CollideDynamicsBroadphase();
    }
    CollideStepPostBroadphase();
}

void CPHObject::CollideDynamics()
{
    CollideDynamicsBroadphase();
    CollideDynamicsNarrow();
}

void CPHObject::reinit_single()
{
    IslandReinit();
    for (ISpatial* sp : m_collide_spatial_overlap)
    {
        CPHObject* obj = PhObjectFromSpatial(sp);
        if (!obj)
            continue;
        obj->IslandReinit();
    }
    m_collide_spatial_overlap.clear();
    ph_world->ReinitMerged();
    dJointGroupEmpty(ContactGroup);
    ContactFeedBacks.empty();
    ContactEffectors.empty();
}

void CPHObject::step_prediction(float time)
{
    // general idea:
    // perform normal step by time as local as possible for this object then return world to
    // the pervious state
}
bool CPHObject::step_single(dReal step)
{
    // Both collisions below are probes: the island is stepped only when the first one merged nothing.
    PhProbeCollideScope probe;
    CollideDynamics();
    bool ret = !m_island.IsObjGroun();
    if (ret)
    {
        // PhTune							(step);
        IslandStep(step);
        reinit_single();
        // PhDataUpdate					(step);
        spatial_move();
        CollideDynamics();
        ret = !m_island.IsObjGroun();
    }
    reinit_single();
    return ret;
}

void CPHObject::step(float time) // it is still not a true step for object because it collide the object only not
                                 // subsequent collision is doing
{
    PhTlsSpatial().clear();
    // The collide caches may still hold raw ISpatial*/CPHObject* captured by CPHWorld::Step()'s Collide
    // pass (the world step fills them but never consumes them). A neighbor cached there can already be
    // freed by the time a local step runs (VirtualMoveTo is intermittent), and walking the entry state
    // then UAFs inside smart_cast. The world step already unmerged every island - drop the stale set
    // instead (restores pre-cache semantics, where step() cleared the shared query buffer first).
    m_collide_spatial_overlap.clear();
    m_collide_dyn_candidates.clear();
    reinit_single();
    Collide();
    IslandStep(time);
    reinit_single();
}

bool CPHObject::DoCollideObj()
{
    PhProbeCollideScope probe;
    CollideDynamics();
    bool ret = m_island.IsObjGroun();
    reinit_single();
    return ret;
}

void CPHObject::FreezeContent()
{
    R_ASSERT(!m_flags.test(st_freezed));
    m_flags.set(st_freezed, TRUE);
    m_flags.set(st_activated, FALSE);
    vis_update_deactivate();
}
void CPHObject::UnFreezeContent()
{
    R_ASSERT(m_flags.test(st_freezed));
    m_flags.set(st_freezed, FALSE);
    m_flags.set(st_activated, TRUE);
    vis_update_activate();
}

void CPHObject::spatial_refresh_bounds()
{
    get_spatial_params();
}

void CPHObject::spatial_register()
{
    SpatialBase::spatial_register();
    m_flags.set(st_dirty, TRUE);
}

void CPHObject::collision_disable()
{
    // The physics spatial DB is queried by Collide() (q_box) during the step.
    PH_STEP_HAZARD("CPHObject::collision_disable");
    SpatialBase::spatial_unregister();
}
void CPHObject::collision_enable()
{
    PH_STEP_HAZARD("CPHObject::collision_enable");
    SpatialBase::spatial_register();
}
void CPHObject::Freeze()
{
    if (!m_flags.test(st_activated))
        return;
    PH_STEP_HAZARD("CPHObject::Freeze");
    ph_world->RemoveObject(this);
    ph_world->AddFreezedObject(this);
    FreezeContent();
}

void CPHObject::UnFreeze()
{
    if (!m_flags.test(st_freezed))
        return;
    PH_STEP_HAZARD("CPHObject::UnFreeze");
    UnFreezeContent();
    ph_world->RemoveFreezedObject(this);
    ph_world->AddObject(this);
}

CPHUpdateObject::CPHUpdateObject() { b_activated = false; }
// B6 early physics kick: m_update_objects is walked by the step (PhTune / PhDataUpdate of telekinesis, captures,
// cars, shell splitters). From another thread during the step only the activated flag changes right away; the list
// edit waits for the flush (PHStepDefer.h).
void CPHUpdateObject::Activate()
{
    if (b_activated)
        return;
    b_activated = true;
    if (ph_step_defer::DeferUpdateObjectActivate(*this))
        return;
    ph_world->AddUpdateObject(this);
}

void CPHUpdateObject::Deactivate()
{
    if (!b_activated)
        return;
    b_activated = false;
    if (ph_step_defer::DeferUpdateObjectDeactivate(*this))
        return;
    ph_world->RemoveUpdateObject(this);
}

void CPHUpdateObject::DeactivateOnDestroy()
{
    // Deactivated (explicitly, or by a derived destructor such as ~CPHCapture) while the step ran: the removal is still
    // queued and the object is still in the list - unlink it now, it must not stay there freed.
    if (ph_step_defer::CancelUpdateObjectDeactivate(*this))
    {
        PH_STEP_HAZARD("CPHUpdateObject destroyed");
        ph_world->RemoveUpdateObject(this);
        return;
    }
    if (!b_activated)
        return;
    b_activated = false;
    // A queued removal would touch this object after it is freed - only a pending activation can be dropped here.
    if (ph_step_defer::CancelUpdateObjectActivate(*this))
        return;
    PH_STEP_HAZARD("CPHUpdateObject destroyed");
    ph_world->RemoveUpdateObject(this);
}
