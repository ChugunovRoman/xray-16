#include "StdAfx.h"

#include <limits>

#include "PHDynamicData.h"
#include "Physics.h"
#include "tri-colliderknoopc/dTriList.h"
#include "PHContactBodyEffector.h"

#include "PHCollideValidator.h"
#ifdef DEBUG
#include "debug_output.h"
#endif
///////////////////////////////////////////////////////////////
#include "ode/ode/src/collision_kernel.h"
#include "ode/ode/src/joint.h"
#include "ode/ode/src/objects.h"

extern CPHWorld* ph_world;
///////////////////////////////////////////////////////////////////

#include "ExtendedGeom.h"
#include "IPhysicsShellHolder.h"
#include "PHCharacter.h"
#include "PHShell.h"
#include "PHActivationShape.h"
#include "xrEngine/xr_object.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "console_vars.h"

namespace
{
#if XR_PERF_METRICS
// Mirrors the per-object classification in CPHWorld::OnFrameStep (ph_obj_*), with the shells that are neither
// vehicles nor inventory items (physic objects, doors) split out of "other" as Prop. One object, not a pass over
// m_objects: makes virtual calls into game objects, so callers pay for it only when metrics are on.
enum class PhObjKind : u8
{
    Actor,
    Ai,
    Ragdoll,
    Vehicle,
    Item,
    Prop,
    Static,
    Other
};

PhObjKind ClassifyPhObject(CPHObject* obj) noexcept
{
    const CPHObject::ECastType tp = obj->CastType();
    if (tp == CPHObject::tpCharacter)
    {
        CPHCharacter* ch = static_cast<CPHCharacter*>(obj);
        if (ch->CastActorCharacter())
            return PhObjKind::Actor;
        if (ch->CastAICharacter())
            return PhObjKind::Ai;
        return PhObjKind::Other;
    }
    if (CPHCollideValidator::IsRagDollObject(*obj))
        return PhObjKind::Ragdoll;
    if (tp == CPHObject::tpStaticShell)
        return PhObjKind::Static;
    if (tp == CPHObject::tpShell)
    {
        IPhysicsShellHolder* holder = static_cast<CPHShell*>(obj)->PhysicsRefObject();
        IGameObject* go = holder ? holder->IObject() : nullptr;
        if (go && go->cast_holder_custom())
            return PhObjKind::Vehicle;
        if (holder && holder->IsInventoryItem())
            return PhObjKind::Item;
        return PhObjKind::Prop;
    }
    return PhObjKind::Other;
}

// physics/wake/*: the object a live contact just woke up (obj2 in NearCallback). Props stay in "other" as before.
xray::perf::Metric PhWakeMetric(PhObjKind kind) noexcept
{
    using M = xray::perf::Metric;
    switch (kind)
    {
    case PhObjKind::Actor: return M::ph_wake_actor;
    case PhObjKind::Ai: return M::ph_wake_ai;
    case PhObjKind::Ragdoll: return M::ph_wake_ragdoll;
    case PhObjKind::Vehicle: return M::ph_wake_vehicle;
    case PhObjKind::Item: return M::ph_wake_item;
    case PhObjKind::Static: return M::ph_wake_static;
    default: return M::ph_wake_other;
    }
}

// physics/corpse_wake/by/*: the object whose contact woke a force-slept corpse (obj1 in NearCallback).
xray::perf::Metric PhCorpseWakerMetric(PhObjKind kind) noexcept
{
    using M = xray::perf::Metric;
    switch (kind)
    {
    case PhObjKind::Actor: return M::ph_corpse_wake_by_actor;
    case PhObjKind::Ai: return M::ph_corpse_wake_by_ai;
    case PhObjKind::Ragdoll: return M::ph_corpse_wake_by_ragdoll;
    case PhObjKind::Vehicle: return M::ph_corpse_wake_by_vehicle;
    case PhObjKind::Item: return M::ph_corpse_wake_by_item;
    case PhObjKind::Prop: return M::ph_corpse_wake_by_prop;
    default: return M::ph_corpse_wake_by_other;
    }
}

// physics/corpse_wake/local_by/*: the same outside the frame step on the physics thread (path/contact_local). The
// activation shape (CPHActivationShape: a death, a movement box switch, FlyTo) is split out of "other".
xray::perf::Metric PhCorpseLocalWakerMetric(CPHObject* obj1) noexcept
{
    using M = xray::perf::Metric;
    switch (ClassifyPhObject(obj1))
    {
    case PhObjKind::Actor: return M::ph_corpse_wake_local_by_actor;
    case PhObjKind::Ai: return M::ph_corpse_wake_local_by_ai;
    case PhObjKind::Ragdoll: return M::ph_corpse_wake_local_by_ragdoll;
    case PhObjKind::Item: return M::ph_corpse_wake_local_by_item;
    case PhObjKind::Prop: return M::ph_corpse_wake_local_by_prop;
    case PhObjKind::Other:
        if (dynamic_cast<CPHActivationShape*>(obj1))
            return M::ph_corpse_wake_local_by_activation;
        return M::ph_corpse_wake_local_by_other;
    default: return M::ph_corpse_wake_local_by_other;
    }
}
#endif // XR_PERF_METRICS

// Caller has already checked IsRagDollObject(): cbClassRagDoll is only ever set on a CPHShell.
bool IsForceSleptCorpse(const CPHObject* obj) noexcept
{
    return !obj->is_active() && static_cast<const CPHShell*>(obj)->m_force_slept;
}

// Ф1 (plans/optimization_mt/21-physics-step.md): the bodies that see a force-slept corpse as static geometry - loose
// shells: inventory items (weapons included) and physic props. Not ragdolls (corpse-vs-corpse is skipped whole in
// NearCallback), not characters (live NPCs: IsLiveNpcController below; the actor keeps pushing and waking corpses),
// not vehicles (a car still shoves a corpse aside), not animated shells (a door driven by its animation must not stop
// against a corpse).
bool IsLooseDynamicShell(CPHObject* obj)
{
    if (obj->CastType() != CPHObject::tpShell)
        return false;
    if (CPHCollideValidator::IsRagDollObject(*obj) || CPHCollideValidator::IsAnimatedObject(*obj))
        return false;
    IPhysicsShellHolder* holder = static_cast<CPHShell*>(obj)->PhysicsRefObject();
    IGameObject* go = holder ? holder->IObject() : nullptr;
    return !(go && go->cast_holder_custom());
}

// NPC one-way contacts (plans/optimization_mt/21-physics-step.md): a live NPC's movement controller (CPHAICharacter),
// not the actor's (CPHActorCharacter: the player keeps shoving corpses around). Characters DO collide with corpses here:
// Common/Config.hpp defines DEAD_BODY_COLLISION, so CCharacterPhysicsSupport::CreateShell never sets
// flRemoveCharacterCollisionAfterDisable and a sleeping corpse keeps colliding with every controller.
bool IsLiveNpcController(CPHObject* obj)
{
    return obj->CastType() == CPHObject::tpCharacter && static_cast<CPHCharacter*>(obj)->CastAICharacter() != nullptr;
}

// PhProbeCollideScope (Physics.h): the collision of the current thread is a probe.
thread_local bool tl_probe_collide = false;
} // namespace

PhProbeCollideScope::PhProbeCollideScope() : m_prev(tl_probe_collide) { tl_probe_collide = true; }
PhProbeCollideScope::~PhProbeCollideScope() { tl_probe_collide = m_prev; }
// PhysicsStepTimeCallback		*physics_step_time_callback				= 0;

const float default_w_limit = 9.8174770f; //(M_PI/16.f/(fixed_step=0.02f));
const float default_l_limit = 150.f; //(3.f/fixed_step=0.02f);
const float default_l_scale = 1.01f;
const float default_w_scale = 1.01f;
const float default_k_l = 0.0002f; // square resistance !!
const float default_k_w = 0.05f;

extern const u16 max_joint_allowed_for_exeact_integration = 30;

// base	params
const float base_fixed_step = 0.02f;
const float base_erp = 0.54545456f;
const float base_cfm = 1.1363636e-006f;
// base params
float fixed_step = 0.02f;
float world_cfm = CFM(SPRING_S(base_cfm, base_erp, base_fixed_step), DAMPING(base_cfm, base_erp));
float world_erp = ERP(SPRING_S(base_cfm, base_erp, base_fixed_step), DAMPING(base_cfm, base_erp));
float world_spring = 1.0f * SPRING(world_cfm, world_erp);
float world_damping = 1.0f * DAMPING(world_cfm, world_erp);

const float default_world_gravity = 2 * 9.81f;

/////////////////////////////////////////////////////

int phIterations = 18;
float phTimefactor = 1.f;
// float		phBreakCommonFactor										= 0.01f;
// float		phRigidBreakWeaponFactor								= 1.f;
Fbox phBoundaries = {1000.f, 1000.f, -1000.f, -1000.f};
// float		ph_tri_query_ex_aabb_rate								= 1.3f;
// int			ph_tri_clear_disable_count								= 10;
dWorldID phWorld;

/////////////////////////////////////
dJointGroupID ContactGroup;
CBlockAllocator<dJointFeedback, 128> ContactFeedBacks;
CBlockAllocator<CPHContactBodyEffector, 128> ContactEffectors;

///////////////////////////////////////////////////////////
class SApplyBodyEffectorPred
{
public:
    SApplyBodyEffectorPred() {}
    void operator()(CPHContactBodyEffector* pointer) const { pointer->Apply(); }
};
/////////////////////////////////////////////////////////////////////////////
IC void add_contact_body_effector(dBodyID body, const dContact& c, SGameMtl* material)
{
    CPHContactBodyEffector* effector = (CPHContactBodyEffector*)dBodyGetData(body);
    if (effector)
        effector->Merge(c, material);
    else
    {
        effector = ContactEffectors.add();
        effector->Init(body, c, material);
        dBodySetData(body, (void*)effector);
    }
}

// Launched NPCs and corpses (21-physics-step.md, 2026-10-15). ODE corrects a contact's penetration with the velocity
// fps * erp * (depth - surface layer) (contactGetInfo2), and its limit contactp.max_vel is dInfinity: ~27 m/s per metre
// of depth at 50 Hz. Bodies that went deep into each other (pairs left without contacts in a full island, NPCs moved
// kinematically through corpses) were shot out at tens of m/s. The cap is applied by cutting the depth of a new contact
// joint (not by dWorldSetContactMaxCorrectingVel: that is one static value for every island and every step, while the
// activation shape, TryPosition and the step-climbing contacts of the characters must keep the old push-out).
namespace
{
struct ContactDepthCap
{
    bool frame = false; // the frame step on its own thread (physics/contact/* are counted only there)
    bool on = false; // the cap applies
    dReal vel_x_step = 0.f; // cap * fixed_step: the depth one step may correct at erp 1
    dReal layer = 0.f; // dWorldGetContactSurfaceLayer: the depth ODE does not correct
};

ContactDepthCap FrameContactDepthCap()
{
    ContactDepthCap cap;
    if (!ph_world || !ph_world->Processing() || ph_world->IsStepRunningOnOtherThread())
        return cap;
    cap.frame = true;
    const float vel = ph_console::ph_contact_max_correct_vel;
    if (vel > 0.f)
    {
        cap.on = true;
        cap.vel_x_step = vel * fixed_step;
        cap.layer = dWorldGetContactSurfaceLayer(phWorld);
    }
    return cap;
}

#if XR_PERF_METRICS
// physics/contact/max_depth_mm: the deepest contact that got a joint in the frame step (geometric depth, before the
// cap), kept by the step thread and handed to the gauge by FlushFrameContactMetrics() at the end of each frame substep.
float s_frame_contact_max_depth = 0.f;
#endif
} // namespace

void FlushFrameContactMetrics()
{
#if XR_PERF_METRICS
    if (s_frame_contact_max_depth > 0.f)
    {
        if (xray::perf::g_perf_metrics_active)
            xray::perf::UpdateMax(xray::perf::Metric::ph_contact_max_depth_mm, s64(s_frame_contact_max_depth * 1000.f));
        s_frame_contact_max_depth = 0.f;
    }
#endif
}

// static_world (Ф1, one-sided contacts): the island of a sleeping object that must act as static geometry here - its
// bodies are left out of every contact joint (attached as body 0, like the level mesh), so the solver neither moves nor
// enables them and the joint does not tie its island to `world`.
// world2 (a pair NearCallback could not merge, see CollideRejectedPair): the bodies are in two islands that stay apart -
// every contact becomes up to two one-sided joints, body 1 against the world into `world` and body 2 against the world
// into world2, each with half of the depth correction (together as much as one joint between the bodies).
// merge_with (the ordinary pair of NearCallback): the island of obj2, merged into `world` right after the contacts - a
// two-body joint in `world` may hold a body of it.
// Body ownership (21-physics-step.md, 2026-10-15, crash AV 0xB4 in quickstep compute_invM_JT): ODE solves an island from
// its own lists, a joint body of another island indexes the solver arrays with that island's tag. Every body a joint
// gets must therefore be in the merge root of `world` or of world2 / merge_with; a one-sided joint goes into the root
// that owns its body. A contact that fails this (or has no body at all) is dropped, counted and logged.
namespace
{
void DropForeignBodyContact(const char* why)
{
    PERF_INC(ph_contact_foreign_body_dropped);
    static u32 s_logged = 0; // the collide phase is serial: one thread at a time
    if (s_logged < 16)
    {
        ++s_logged;
        Msg("! CollideIntoGroup: contact dropped: %s", why);
    }
}
} // namespace

IC static int CollideIntoGroup(dGeomID o1, dGeomID o2, dJointGroupID jointGroup, CPHIsland* world,
    const int& MAX_CONTACTS, const dxWorld* static_world = nullptr, CPHIsland* world2 = nullptr,
    CPHIsland* merge_with = nullptr)
{
    const int RS = 800 + 10;
    const int N = RS;

    static dContact contacts[RS];
    int collided_contacts = 0;
    // get the contacts up to a maximum of N contacts
    int n;

    VERIFY(o1);
    VERIFY(o2);
    VERIFY(&contacts[0].geom);
    n = dCollide(o1, o2, N, &contacts[0].geom, sizeof(dContact));

    if (n > N - 1)
        n = N - 1;
    int i;

    // Contact push-out speed cap (21-physics-step.md, 2026-10-15; ph_contact_max_correct_vel): the frame step only.
    const ContactDepthCap depth_cap = FrameContactDepthCap();
#if XR_PERF_METRICS
    u32 capped_contacts = 0;
#endif

    for (i = 0; i < n; ++i)
    {
        dContact& c = contacts[i];
        dContactGeom& cgeom = c.geom;
        dSurfaceParameters& surface = c.surface;
        // The geometric depth, before the callbacks: CPHSimpleCharacter::InitContact sets its own depth to lift a
        // character onto a step (b_clamb_jump) - such a contact is left out of the push-out cap.
        const dReal depth0 = cgeom.depth;
        dGeomID g1 = cgeom.g1;
        dGeomID g2 = cgeom.g2;
        bool pushing_neg = false;
        bool do_collide = true;
        dxGeomUserData* usr_data_1 = NULL;
        dxGeomUserData* usr_data_2 = NULL;
        u16 material_idx_1 = 0;
        u16 material_idx_2 = 0;

        surface.mu = 1.f; // 5000.f;
        surface.soft_erp = 1.f; // ERP(world_spring,world_damping);
        surface.soft_cfm = 1.f; // CFM(world_spring,world_damping);
        surface.bounce = 0.01f; // 0.1f;
        surface.bounce_vel = 1.5f; // 0.005f;
        usr_data_1 = retrieveGeomUserData(g1);
        usr_data_2 = retrieveGeomUserData(g2);
        ///////////////////////////////////////////////////////////////////////////////////////////////////
        if (usr_data_2)
            material_idx_2 = usr_data_2->material;
        if (usr_data_1)
            material_idx_1 = usr_data_1->material;
        bool is_tri_1 = dTriListClass == dGeomGetClass(g1);
        bool is_tri_2 = dTriListClass == dGeomGetClass(g2);
        if (!is_tri_2 && !is_tri_1)
            surface.mode = 0;
        if (is_tri_1)
            material_idx_1 = (u16)surface.mode;
        if (is_tri_2)
            material_idx_2 = (u16)surface.mode;
        SGameMtl* material_1 = GMLib.GetMaterialByIdx(material_idx_1);
        SGameMtl* material_2 = GMLib.GetMaterialByIdx(material_idx_2);
        ////////////////params can be changed in
        /// callbacks//////////////////////////////////////////////////////////////////////////
        surface.mode = dContactApprox1 | dContactSoftERP | dContactSoftCFM;
        float spring = material_2->fPHSpring * material_1->fPHSpring * world_spring;
        float damping = material_2->fPHDamping * material_1->fPHDamping * world_damping;
        surface.soft_erp = ERP(spring, damping);
        surface.soft_cfm = CFM(spring, damping);
        surface.mu = material_2->fPHFriction * material_1->fPHFriction;
        /////////////////////////////////////////////////////////////////////////////////////////////////

        Flags32& flags_1 = material_1->Flags;
        Flags32& flags_2 = material_2->Flags;

        if (is_tri_1)
        {
#pragma warning(push)
#pragma warning(disable : 4245)
            if (material_1->Flags.test(SGameMtl::flSlowDown) && !(usr_data_2->pushing_neg || usr_data_2->pushing_b_neg))
#pragma warning(pop)
            {
                dBodyID body = dGeomGetBody(g2);
                R_ASSERT2(body, "static - static collision !!!");
                if (material_1->Flags.test(SGameMtl::flLiquid))
                {
                    add_contact_body_effector(body, c, material_1);
                }
                else
                {
                    if (!usr_data_2 || !usr_data_2->ph_object || !usr_data_2->ph_object->IsRayMotion())
                    {
                        add_contact_body_effector(body, c, material_1);
                    }
                }
            }
            if (material_1->Flags.test(SGameMtl::flPassable))
                do_collide = false;
            //	if(material_2->Flags.is(SGameMtl::flClimable))
            //		do_collide=false;
        }
        if (is_tri_2)
        {
#pragma warning(push)
#pragma warning(disable : 4245)
            if (material_2->Flags.test(SGameMtl::flSlowDown) && !(usr_data_1->pushing_neg || usr_data_1->pushing_b_neg))
#pragma warning(pop)
            {
                dBodyID body = dGeomGetBody(g1);
                R_ASSERT2(body, "static - static collision !!!");
                if (material_2->Flags.test(SGameMtl::flLiquid))
                {
                    add_contact_body_effector(body, c, material_2);
                }
                else
                {
                    if (!usr_data_1 || !usr_data_1->ph_object || !usr_data_1->ph_object->IsRayMotion())
                    {
                        add_contact_body_effector(body, c, material_2);
                    }
                }
            }
            if (material_2->Flags.test(SGameMtl::flPassable))
                do_collide = false;
        }

        if (flags_1.test(SGameMtl::flBounceable) && flags_2.test(SGameMtl::flBounceable))
        {
            surface.mode |= dContactBounce;
            surface.bounce_vel = std::max(material_1->fPHBounceStartVelocity, material_2->fPHBounceStartVelocity);
            surface.bounce = std::min(material_1->fPHBouncing, material_2->fPHBouncing);
        }
        /////////////////////////////////////////////////////////////////////////////////////////////////
        if (usr_data_2 && usr_data_2->object_callbacks)
        {
            usr_data_2->object_callbacks->Call(do_collide, false, c, material_1, material_2);
        }

        if (usr_data_1 && usr_data_1->object_callbacks)
        {
            usr_data_1->object_callbacks->Call(do_collide, true, c, material_1, material_2);
        }

        if (usr_data_2)
        {
            usr_data_2->pushing_b_neg = usr_data_2->pushing_b_neg &&
                !GMLib.GetMaterialByIdx(usr_data_2->b_neg_tri->material)->Flags.test(SGameMtl::flPassable);
            usr_data_2->pushing_neg = usr_data_2->pushing_neg &&
                !GMLib.GetMaterialByIdx(usr_data_2->neg_tri->material)->Flags.test(SGameMtl::flPassable);
            pushing_neg = usr_data_2->pushing_b_neg || usr_data_2->pushing_neg;
            if (usr_data_2->ph_object)
            {
                usr_data_2->ph_object->InitContact(&c, do_collide, material_idx_1, material_idx_2);
            }
        }
        ///////////////////////////////////////////////////////////////////////////////////////
        if (usr_data_1)
        {
            usr_data_1->pushing_b_neg = usr_data_1->pushing_b_neg &&
                !GMLib.GetMaterialByIdx(usr_data_1->b_neg_tri->material)->Flags.test(SGameMtl::flPassable);
            usr_data_1->pushing_neg = usr_data_1->pushing_neg &&
                !GMLib.GetMaterialByIdx(usr_data_1->neg_tri->material)->Flags.test(SGameMtl::flPassable);
            pushing_neg = usr_data_1->pushing_b_neg || usr_data_1->pushing_neg;
            if (usr_data_1->ph_object)
            {
                usr_data_1->ph_object->InitContact(&c, do_collide, material_idx_1, material_idx_2);
            }
        }

        if (pushing_neg)
            surface.mu = std::numeric_limits<dReal>::max();

        if (do_collide && collided_contacts < MAX_CONTACTS)
        {
            dBodyID body_1 = dGeomGetBody(g1);
            dBodyID body_2 = dGeomGetBody(g2);
            if (static_world)
            {
                // dxBody::world is the CPHIsland the body was added to (CPHIsland::AddBody); merges never change it.
                if (body_1 && body_1->world == static_world)
                    body_1 = nullptr;
                if (body_2 && body_2->world == static_world)
                    body_2 = nullptr;
                if (!body_1 && !body_2)
                    continue;
            }
            // Body ownership (see above): the roots now - a merge in an earlier contact callback is seen.
            CPHIsland* const root_1 = world->ActiveRootReadOnly();
            CPHIsland* const other = world2 ? world2 : merge_with;
            CPHIsland* const root_2 = other ? other->ActiveRootReadOnly() : nullptr;
            CPHIsland* const owner_1 = body_1 ? CPHIsland::BodyActiveRoot(body_1) : nullptr;
            CPHIsland* const owner_2 = body_2 ? CPHIsland::BodyActiveRoot(body_2) : nullptr;
            if (!body_1 && !body_2)
            {
                DropForeignBodyContact("no body on either side");
                continue;
            }
            if ((body_1 && (!owner_1 || (owner_1 != root_1 && owner_1 != root_2))) ||
                (body_2 && (!owner_2 || (owner_2 != root_1 && owner_2 != root_2))))
            {
                DropForeignBodyContact("a body is outside the islands of the pair");
                continue;
            }
            const bool split = world2 && body_1 && body_2;
            if (split)
                surface.soft_erp *= 0.5f;
#if XR_PERF_METRICS
            if (depth_cap.frame && depth0 > s_frame_contact_max_depth)
                s_frame_contact_max_depth = depth0;
#endif
            if (depth_cap.on && cgeom.depth == depth0)
            {
                // ODE pushes the contact out at fps * erp * (depth - surface layer) per step, without any limit
                // (contactp.max_vel = dInfinity in every fork): the depth is cut so that this stays <= the cap.
                const dReal erp = (surface.mode & dContactSoftERP) ? surface.soft_erp : world_erp;
                if (erp > 0.f)
                {
                    const dReal max_depth = depth_cap.vel_x_step / erp + depth_cap.layer;
                    if (cgeom.depth > max_depth)
                    {
                        cgeom.depth = max_depth;
#if XR_PERF_METRICS
                        ++capped_contacts;
#endif
                    }
                }
            }
            ++collided_contacts;
#ifdef DEBUG
            if (debug_output().ph_dbg_draw_mask().test(phDbgDrawContacts))
                debug_output().DBG_DrawContact(c);
#endif
            if (world2)
            {
                // Two islands that must not be merged: one one-sided joint per body, each into the island that owns
                // that body (checked above). A body missing on one side (static geometry) leaves just the other joint,
                // at the full correction.
                if (body_1)
                {
                    dJointID joint_1 = dJointCreateContact(0, jointGroup, &c);
                    owner_1->ConnectJoint(joint_1);
                    dJointAttach(joint_1, body_1, nullptr);
                }
                if (body_2)
                {
                    dJointID joint_2 = dJointCreateContact(0, jointGroup, &c);
                    owner_2->ConnectJoint(joint_2);
                    dJointAttach(joint_2, nullptr, body_2); // reversed: body 2 is pushed along -normal
                }
                continue;
            }
            dJointID contact_joint = dJointCreateContact(0, jointGroup, &c);
            world->ConnectJoint(contact_joint);
            // A zero body1 is swapped to body2 by dJointAttach (dJOINT_REVERSE flips the normal), as for the mesh.
            dJointAttach(contact_joint, body_1, body_2);
        }
    }
#if XR_PERF_METRICS
    if (capped_contacts != 0)
        PERF_ADD(ph_contact_vel_capped, u64(capped_contacts));
#endif
    return collided_contacts;
}

namespace
{
// Pairs left without contacts by a full island (21-physics-step.md, 2026-10-15; always on). CPHIsland caps an
// island at BODIES_LIMIT 500 / JOINTS_LIMIT 1500; past that NearCallback refused the merge and the pair got NO contact
// (physics/near/merge_reject 300-7000 per frame while physics/island/max_bodies was 499 for minutes): corpses in a pile
// and NPCs in it went into each other, and when a later substep (another collide order, the island rebuilt) gave them
// contacts again, ODE shot them out at fps * erp * depth. Such a pair now gets at most this many contacts per substep
// over the limit, without merging (quickstep moves its arrays to the heap above 2048 entries, as for the level reserve).
constexpr int reject_pair_contacts = 8;

// The frame step on its own thread only: the local steps (TryPosition -> step_single, DoCollideObj) read the island
// growth as "touches another object" and run with small islands anyway. Pairs of two live NPC controllers are left as
// they were (a dense crowd has thousands of them, the controllers are capped by their own velocity limit and
// ph_ai_max_rise_vel).
bool RejectFixHere(CPHObject* obj1, CPHObject* obj2, bool counting)
{
    if (!counting || ph_world->IsStepRunningOnOtherThread())
        return false;
    return !(IsLiveNpcController(obj1) && IsLiveNpcController(obj2));
}

void CollideRejectedPair(CPHObject* obj1, CPHObject* obj2, dGeomID o1, dGeomID o2, CPHIsland* island1, CPHIsland* island2)
{
    PERF_INC(ph_reject_fix_pairs);
    int contacts;
    if (island1 == island2)
    {
        // One island without joint budget: the ordinary contact between the two bodies, over JOINTS_LIMIT (nothing to
        // merge, the island does not get more bodies).
        PERF_INC(ph_reject_fix_same);
        contacts = CollideIntoGroup(o1, o2, ContactGroup, island1, reject_pair_contacts);
    }
    else if (!obj2->is_active())
    {
        // Asleep: static geometry for obj1, as a force-slept corpse for an item (Ф1) - obj1 rests on it, it is not woken.
        // obj1 is active (the frame step walks it), its island is unmerged by Step().
        PERF_INC(ph_reject_fix_static);
        contacts = CollideIntoGroup(o1, o2, ContactGroup, island1, reject_pair_contacts, obj2->DWorld());
    }
    else
    {
        // Two active islands: one one-sided joint per body into its own island, half of the correction each.
        PERF_INC(ph_reject_fix_split);
        contacts = CollideIntoGroup(o1, o2, ContactGroup, island1, reject_pair_contacts, nullptr, island2);
        // island2 got joints: make sure it is unmerged before ContactGroup is emptied, as for a merged obj2.
        if (contacts != 0)
            ph_world->RegisterMerged(obj2);
    }
    PERF_ADD(ph_reject_fix_contacts, u64(contacts));
}
} // namespace

void NearCallback(CPHObject* obj1, CPHObject* obj2, dGeomID o1, dGeomID o2)
{
    CPHIsland* island1 = obj1->DActiveIsland();
    CPHIsland* island2 = obj2->DActiveIsland();
    obj2->near_callback(obj1);

    // Ф2 (distance-based corpse sleep, PHYSICS_ISLAND_CORPSE_SLEEP_2026-10-06.md): a force-slept corpse must
    // not be reactivated by touching another corpse. Without this guard any pile of 2+ overlapping corpses
    // cycles endlessly - CPHWorld::SleepCorpses() re-disables every one of them every physics step, and the
    // very next corpse-vs-corpse contact calls EnableObject() again via the code below. Measured on a
    // 587-NPC grenade-kill scene: corpse_force_slept/wake_ragdoll climbed into the thousands *per frame*
    // while physics/island_batch (actually active objects per solve) never dropped below ~80 - the loop
    // just burns CPU reactivating/deactivating the same bodies every step without ever shrinking the active
    // set. A live NPC, the player, a bullet or an explosion still wakes a sleeping corpse normally - this
    // only skips corpse-vs-corpse contact, and only against a corpse that SleepCorpses() itself put to sleep
    // (CPHShell::m_force_slept, i.e. far from the camera). Naturally-slept corpses near the player keep the
    // original behaviour (a fresh ragdoll landing on them wakes them and collides instead of passing
    // through). The whole contact is skipped, not just the wake: a contact joint against a disabled body
    // would get it re-enabled by ODE's island step anyway, behind the CPHObject's back.
    if (CPHCollideValidator::IsRagDollObject(*obj1) && CPHCollideValidator::IsRagDollObject(*obj2) &&
        (IsForceSleptCorpse(obj1) || IsForceSleptCorpse(obj2)))
        return;

    const bool counting = ph_world->Processing();

    // Ф1 (plans/optimization_mt/21-physics-step.md): a loose body (item, weapon, prop) against a force-slept corpse.
    // Measured: corpse_force_slept ~ wake/ragdoll ~ 1.3 x active ragdolls per frame - every corpse SleepCorpses() put to
    // sleep was woken by a contact on the very next substep, so the far corpses (and the loot lying on them) stayed
    // active for good. Here the corpse acts as static geometry for the loose body: its contacts are attached to the
    // loose body only, the islands are not merged and the corpse is not enabled - the body still rests on the corpse
    // (no falling through), the corpse stays asleep. Hits, explosions, grabs and scripts still wake it (impulses and
    // CPHShell::EnableObject, not contacts); the actor and vehicles are not loose bodies and collide as before.
    // The same for a live NPC's controller - measured (session 14:10:43) as about half of the wakes left after the
    // items were taken out (physics/corpse_wake/by/ai). The NPC then meets the corpse as it meets the level mesh (stands
    // on it, steps over it, stops at it) instead of pushing it away; its own contact logic
    // (CPHSimpleCharacter::InitContact) still sees an object contact, only the joint has no second body.
    {
        CPHObject* loose = nullptr;
        CPHObject* corpse = nullptr;
        // Not both: a corpse-vs-corpse pair with a force-slept one has returned above.
        if (CPHCollideValidator::IsRagDollObject(*obj2) && IsForceSleptCorpse(obj2))
        {
            loose = obj1;
            corpse = obj2;
        }
        else if (CPHCollideValidator::IsRagDollObject(*obj1) && IsForceSleptCorpse(obj1))
        {
            loose = obj2;
            corpse = obj1;
        }
        bool npc = false;
        if (loose)
        {
            // An active loose body only: its island is the one Step() unmerges (IslandReinit), so the joints connected
            // to it here never outlive the substep. A sleeping one keeps the old path (merge + RegisterMerged). An NPC
            // controller may also be obj1 of a local step (CPHAICharacter::TryPosition -> DoCollideObj / step_single):
            // reinit_single() unmerges obj1 and empties ContactGroup right after, active or not.
            if ((loose == obj1 || loose->is_active()) && IsLiveNpcController(loose))
                npc = true;
            else if (!(loose->is_active() && IsLooseDynamicShell(loose)))
                loose = nullptr;
        }
        if (loose)
        {
            if (counting)
                PERF_INC(ph_near_calls);
            // NPC pairs are counted on every path: most of them come from TryPosition on main, outside the frame step.
            if (npc)
                PERF_INC(ph_oneway_npc_pairs);
            else if (counting)
                PERF_INC(ph_oneway_pairs);
            // The contacts go into the loose body's island only: its own joint budget, as for the level mesh.
            CPHIsland* island = loose->DActiveIsland();
            int max_contacts = island->MaxJoints();
            if (max_contacts <= 0)
            {
                // A full island: the NPC / item would sink into the corpse (the reject fix, see RejectFixHere).
                if (!RejectFixHere(obj1, obj2, counting))
                    return;
                PERF_INC(ph_reject_fix_oneway_full);
                max_contacts = reject_pair_contacts;
            }
            const int contacts = CollideIntoGroup(o1, o2, ContactGroup, island, max_contacts, corpse->DWorld());
            if (npc)
                PERF_ADD(ph_oneway_npc_contacts, u64(contacts));
            else if (counting)
                PERF_ADD(ph_oneway_contacts, u64(contacts));
            return;
        }
    }

    int MAX_CONTACTS = -1;
    if (counting)
        PERF_INC(ph_near_calls);
    bool can_merge;
    if (island1 == island2)
    {
        // Both already in one island (merged earlier in this substep - every pair inside a corpse pile or a crowd after
        // the first object's pass). CPHIsland::CanMerge would count that island twice (MaxJoints(island) =
        // JOINTS_LIMIT - 2 nj, nb + nb < BODIES_LIMIT): from 250 bodies / 750 joints on, no pair inside it got a contact
        // at all, though nothing is merged here. Its own budget instead.
        MAX_CONTACTS = island1->MaxJoints();
        can_merge = MAX_CONTACTS > 0;
#if XR_PERF_METRICS
        int vanilla_budget;
        if (counting && can_merge && !island1->CanMerge(island2, vanilla_budget))
            PERF_INC(ph_near_same_island_fixed);
#endif
    }
    else
        can_merge = island1->CanMerge(island2, MAX_CONTACTS);
    if (!can_merge)
    {
        if (counting)
        {
            PERF_INC(ph_near_merge_reject);
            if (island1 == island2)
                PERF_INC(ph_near_merge_reject_same);
        }
        if (RejectFixHere(obj1, obj2, counting))
            CollideRejectedPair(obj1, obj2, o1, o2, island1, island2);
        return;
    }
    const int contacts = CollideIntoGroup(o1, o2, ContactGroup, island1, MAX_CONTACTS, nullptr, nullptr, island2);
    if (contacts != 0)
    {
        if (counting)
            PERF_ADD(ph_near_dyn_contacts, u64(contacts));
        obj1->MergeIsland(obj2);
        // obj2 may never reach the IslandReinit loops of Step/StepTouch (frozen world, CPHStaticGeomShell
        // whose EnableObject does not activate it, deactivation mid-step) -> guarantee its Unmerge.
        ph_world->RegisterMerged(obj2);
        if (!obj2->is_active())
        {
#if XR_PERF_METRICS
            // physics/wake/ai_by_ai (the frame step, a part of physics/wake/ai) and wake/ai_by_ai_probe (the TryPosition
            // probes, not in physics/wake/*): a live NPC woken by another live NPC's contact - the crowd churn of a
            // dense crowd (21-physics-step.md, 2026-10-16). Diagnostics only.
            if (xray::perf::g_perf_metrics_active && IsLiveNpcController(obj2) && IsLiveNpcController(obj1))
            {
                if (counting && !ph_world->IsStepRunningOnOtherThread())
                    PERF_INC(ph_wake_ai_by_ai);
                else if (tl_probe_collide)
                    PERF_INC(ph_wake_ai_by_ai_probe);
            }
#endif
            if (counting)
            {
                PERF_INC(ph_wake);
#if XR_PERF_METRICS
                // Classification makes virtual calls into game objects - pay for it only when metrics are on.
                if (xray::perf::g_perf_metrics_active)
                {
                    const PhObjKind woken = ClassifyPhObject(obj2);
                    xray::perf::Add(PhWakeMetric(woken));
                    // Ф0: who wakes force-slept corpses (read before EnableObject clears m_force_slept).
                    if (woken == PhObjKind::Ragdoll && static_cast<CPHShell*>(obj2)->m_force_slept)
                        xray::perf::Add(PhCorpseWakerMetric(ClassifyPhObject(obj1)));
                }
#endif
            }
            // physics/corpse_wake/path/contact (the frame step on its own thread) or contact_local (TryPosition, the
            // activation shape and other local steps, or a frame step on another thread than the caller).
            PERF_PH_WAKE(counting && !ph_world->IsStepRunningOnOtherThread() ? xray::perf::ph_wake::Contact :
                                                                              xray::perf::ph_wake::ContactLocal);
#if XR_PERF_METRICS
            // physics/corpse_wake/local_by/*: who wakes force-slept corpses in the local steps (the same condition as
            // ContactLocal above; read before EnableObject clears m_force_slept).
            if (xray::perf::g_perf_metrics_active && !(counting && !ph_world->IsStepRunningOnOtherThread()) &&
                CPHCollideValidator::IsRagDollObject(*obj2) && static_cast<CPHShell*>(obj2)->m_force_slept)
            {
                xray::perf::Add(PhCorpseLocalWakerMetric(obj1));
            }
#endif
            obj2->EnableObject(obj1);
        }
    }
}

void CollideStatic(dGeomID o2, CPHObject* obj2)
{
    PERF_SCOPE(ph_collide_static);
    PERF_SCOPE_IF(ph_world->Processing(), ph_step_static); // Ф0: the frame step on the physics thread only
    CPHIsland* island2 = obj2->DActiveIsland();
    // Stretched ragdolls (21-physics-step.md, 2026-10-15): the dynamic pairs of every object are collided before its
    // level mesh (CPHObject::CollideStepPostBroadphase), so in an island grown to JOINTS_LIMIT (a pile of corpses with
    // a crowd around it: physics/island/max_joints 1500 for minutes) the objects collided later got NO contact with
    // the level at all - their bodies sank into the ground / a boat, and once a later substep gave them contacts again
    // ODE pushed them out at fps * erp * depth (no max correcting velocity), cut to default_l_limit = 150 m/s, the
    // joints torn apart. Up to static_contact_reserve contacts with the level are allowed over the limit (quickstep
    // moves its arrays to the heap above 2048 entries, the island only gets bigger). A constant: in the tests a starved
    // call got ~4 contacts over the limit on average (physics/collide_static/reserve_contacts / starved), so 32 is not the binding cap,
    // and a lower one only brings the sinking bodies back.
    constexpr int static_contact_reserve = 32;
    const int budget = island2->MaxJoints();
    if (budget <= 0)
        PERF_INC(ph_static_starved);
    const int contacts = CollideIntoGroup(ph_world->GetMeshGeom(), o2, ContactGroup, island2,
        budget < static_contact_reserve ? static_contact_reserve : budget);
    PERF_ADD(ph_collide_static_contacts, u64(contacts));
    if (contacts > 0 && contacts > budget)
        PERF_ADD(ph_static_reserve_contacts, u64(contacts - (budget > 0 ? budget : 0)));
}

// half square time

/*
void BodyCutForce(dBodyID body)
{
dReal linear_limit=l_limit;
//applyed force
const dReal* force=	dBodyGetForce(body);

//body mass
dMass m;
dBodyGetMass(body,&m);

//accceleration correspondent to the force
dVector3 a={force[0]/m.mass,force[1]/m.mass,force[2]/m.mass};

//current velocity
const dReal* start_vel=dBodyGetLinearVel(body);

//velocity adding during one step
dVector3 add_vel={a[0]*fixed_step,a[1]*fixed_step,a[2]*fixed_step};

//result velocity
dVector3 vel={start_vel[0]+add_vel[0],start_vel[1]+add_vel[1],start_vel[2]+add_vel[2]};

//result velocity magnitude
dReal speed=dSqrt(dDOT(vel,vel));

if(speed>linear_limit) //then we need to cut applied force
{
//solve the triangle - cutted velocity - current veocity - add velocity
//to find cutted adding velocity

//add_vell magnitude
dReal add_speed=dSqrt(dDOT(add_vel,add_vel));

//current velocity magnitude
dReal start_speed=dSqrt(dDOT(start_vel,start_vel));

//cosinus of the angle between vel ang add_vell
dReal cosinus1=dFabs(dDOT(add_vel,start_vel)/start_speed/add_speed);

dReal cosinus1_2=cosinus1*cosinus1;
if(cosinus1_2>1.f)
cosinus1_2=1.f;
dReal cutted_add_speed;
if(cosinus1_2==1.f)
cutted_add_speed=linear_limit-start_speed;
else
{
//sinus
dReal sinus1_2=1.f-cosinus1_2;


dReal sinus1=dSqrt(sinus1_2);


//sinus of the angle between cutted velocity and adding velociity (sinus theorem)
dReal sinus2=sinus1/linear_limit*start_speed;
dReal sinus2_2=sinus2*sinus2;
if(sinus2_2>1.f)sinus2_2=1.f;

dReal cosinus2_2=1.f-sinus2_2;
dReal cosinus2=dSqrt(cosinus2_2);

//sinus of 180-ang1-ang2
dReal sinus3=sinus1*cosinus2+cosinus1*sinus2;

//cutted adding velocity magnitude (sinus theorem)

cutted_add_speed=linear_limit/sinus1*sinus3;
}
//substitude force

dBodyAddForce(body,
cutted_add_speed/add_speed/fixed_step*m.mass*add_vel[0]-force[0],
cutted_add_speed/add_speed/fixed_step*m.mass*add_vel[1]-force[1],
cutted_add_speed/add_speed/fixed_step*m.mass*add_vel[2]-force[2]
);
}

}
*/
// limit for angular accel
void dBodyAngAccelFromTorqu(const dBodyID body, dReal* ang_accel, const dReal* torque)
{
    dMass m;
    dMatrix3 invI;
    dBodyGetMass(body, &m);
    dInvertPDMatrix(m.I, invI, 3);
    dMULTIPLY1_333(ang_accel, invI, torque);
}
void FixBody(dBodyID body, float ext_param, float mass_param)
{
    dMass m;
    dMassSetSphere(&m, 1.f, ext_param);
    dMassAdjust(&m, mass_param);
    dBodySetMass(body, &m);
    dBodySetGravityMode(body, 0);
    dBodySetLinearVel(body, 0, 0, 0);
    dBodySetAngularVel(body, 0, 0, 0);
    dBodySetForce(body, 0, 0, 0);
    dBodySetTorque(body, 0, 0, 0);
}
void FixBody(dBodyID body) { FixBody(body, fix_ext_param, fix_mass_param); }
void BodyCutForce(dBodyID body, float l_limit, float w_limit)
{
    const dReal wa_limit = w_limit / fixed_step;
    const dReal* force = dBodyGetForce(body);
    dReal force_mag = dSqrt(dDOT(force, force));

    // body mass
    dMass m;
    dBodyGetMass(body, &m);

    dReal force_limit = l_limit / fixed_step * m.mass;

    if (force_mag > force_limit)
    {
        dBodySetForce(body, force[0] / force_mag * force_limit, force[1] / force_mag * force_limit,
            force[2] / force_mag * force_limit);
    }

    const dReal* torque = dBodyGetTorque(body);
    dReal torque_mag = dSqrt(dDOT(torque, torque));

    if (torque_mag < 0.001f)
        return;

    dMatrix3 tmp, invI, I;

    // compute inertia tensor in global frame
    dMULTIPLY2_333(tmp, m.I, body->posr.R);
    dMULTIPLY0_333(I, body->posr.R, tmp);

    // compute inverse inertia tensor in global frame
    dMULTIPLY2_333(tmp, body->invI, body->posr.R);
    dMULTIPLY0_333(invI, body->posr.R, tmp);

    // angular accel
    dVector3 wa;
    dMULTIPLY0_331(wa, invI, torque);
    dReal wa_mag = dSqrt(dDOT(wa, wa));

    if (wa_mag > wa_limit)
    {
        // scale w
        for (int i = 0; i < 3; ++i)
            wa[i] *= wa_limit / wa_mag;
        dVector3 new_torqu;

        dMULTIPLY0_331(new_torqu, I, wa);

        dBodySetTorque(body, new_torqu[0], new_torqu[1], new_torqu[2]);
    }
}

void dMassSub(dMass* a, const dMass* b)
{
    int i;
    VERIFY(a && b);
    dReal denom = dRecip(a->mass - b->mass);
    for (i = 0; i < 3; ++i)
        a->c[i] = (a->c[i] * a->mass - b->c[i] * b->mass) * denom;
    a->mass -= b->mass;
    for (i = 0; i < 12; ++i)
        a->I[i] -= b->I[i];
}

////Energy of non Elastic collision;
// body - static case
float E_NlS(dBodyID body, const dReal* norm, float norm_sign) // if body c.geom.g1 norm_sign + else -
{ // norm*norm_sign - to body
    const dReal* vel = dBodyGetLinearVel(body);
    dReal prg = -dDOT(vel, norm) * norm_sign;
    prg = prg < 0.f ? prg = 0.f : prg;
    dMass mass;
    dBodyGetMass(body, &mass);
    return mass.mass * prg * prg / 2;
}

// body - body case
float E_NLD(dBodyID b1, dBodyID b2, const dReal* norm) // norm - from 2 to 1
{
    dMass m1, m2;
    dBodyGetMass(b1, &m1);
    dBodyGetMass(b2, &m2);
    const dReal* vel1 = dBodyGetLinearVel(b1);
    const dReal* vel2 = dBodyGetLinearVel(b2);

    dReal vel_pr1 = dDOT(vel1, norm);
    dReal vel_pr2 = dDOT(vel2, norm);

    if (vel_pr1 > vel_pr2)
        return 0.f; // exit if the bodies are departing

    dVector3 impuls1 = {vel1[0] * m1.mass, vel1[1] * m1.mass, vel1[2] * m1.mass};
    dVector3 impuls2 = {vel2[0] * m2.mass, vel2[1] * m2.mass, vel2[2] * m2.mass};

    dVector3 c_mas_impuls = {impuls1[0] + impuls2[0], impuls1[1] + impuls2[1], impuls1[2] + impuls2[2]};
    dReal cmass = m1.mass + m2.mass;
    dVector3 c_mass_vel = {c_mas_impuls[0] / cmass, c_mas_impuls[1] / cmass, c_mas_impuls[2] / cmass};

    dReal c_mass_vel_prg = dDOT(c_mass_vel, norm);

    dReal kin_energy_start = vel_pr1 * vel_pr1 * m1.mass / 2.f + vel_pr2 * vel_pr2 * m2.mass / 2.f;
    dReal kin_energy_end = c_mass_vel_prg * c_mass_vel_prg * cmass / 2.f;

    return (kin_energy_start - kin_energy_end);
}
float E_NL(dBodyID b1, dBodyID b2, const dReal* norm)
{
    VERIFY(b1 || b2);
    if (b1)
    {
        if (b2)
            return E_NLD(b1, b2, norm);
        else
            return E_NlS(b1, norm, 1);
    }
    else
        return E_NlS(b2, norm, -1);
}
void ApplyGravityAccel(dBodyID body, const dReal* accel)
{
    dMass m;
    dBodyGetMass(body, &m);
    dBodyAddForce(body, accel[0] * m.mass, accel[1] * m.mass, accel[2] * m.mass);
}

const dReal* dJointGetPositionContact(dJointID joint)
{
    VERIFY2(dJointGetType(joint) == dJointTypeContact, "not a contact!");
    dxJointContact* c_joint = (dxJointContact*)joint;
    return c_joint->contact.geom.pos;
}
