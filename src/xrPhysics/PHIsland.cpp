#include "StdAfx.h"
#include "PHIsland.h"
#include "Physics.h"
#include "ph_valid_ode.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <atomic>

// Crash in the parallel island solve (21-physics-step.md, 2026-10-15: AV reading 0xB4 in quickstep.cpp
// compute_invM_JT, `body[b1]->invMass`). X-Ray's dxProcessIslands (ode/src/util.cpp) takes ALL bodies and joints of the
// island lists into arrays sized by nb / nj without counting them, dxQuickStepper numbers the bodies (tag = index) and
// reads jb = node[k].body ? body->tag : -1. A joint whose body 1 is null (a contact detached by dJointGroupEmpty - it
// zeroes node[].body, ode.cpp removeJointReferencesFromAttachedBodies - while still linked into the list) gives -1,
// a body of another island gives that island's tag (written by another solve thread meanwhile): body[b1] lies outside
// the array. Such an island is not stepped in this substep (physics/island/bad_lists_skipped + a log line) - its bodies
// stand still for 20 ms instead of the process going down. Read-only and O(nb + nj): the island structures do not
// change during the solve (merges happen in the collide phase).
const char* CPHIsland::CheckSolveLists()
{
    int bodies = 0;
    for (dxBody* body = firstbody; body; body = (dxBody*)body->next)
    {
        if (++bodies > nb)
            return "body list longer than nb";
    }
    int joints = 0;
    for (dxJoint* joint = firstjoint; joint; joint = (dxJoint*)joint->next)
    {
        if (++joints > nj)
            return "joint list longer than nj";
        dxBody* body_1 = joint->node[0].body;
        dxBody* body_2 = joint->node[1].body;
        if (!body_1)
            return "joint without body 1 (detached)";
        if (BodyActiveRoot(body_1) != this || (body_2 && BodyActiveRoot(body_2) != this))
            return "joint body of another island";
    }
    return nullptr;
}

void CPHIsland::Step(dReal step)
{
    if (!m_flags.is_active())
        return;
    if (const char* bad = CheckSolveLists())
    {
        PERF_INC(ph_island_bad_lists_skipped);
        static std::atomic<u32> s_logged{0};
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16)
            Msg("! CPHIsland::Step: island not stepped: %s (nb=%d, nj=%d)", bad, nb, nj);
        return;
    }
    // dWorldStepFast1	(DWorld(),	fixed_step,	phIterations/*+Random.randI(0,phIterationCycle)*/);
    if (m_flags.is_exact_integration_prefeared() && nj < max_joint_allowed_for_exeact_integration)
        dWorldStep(DWorld(), fixed_step);
    else
        dWorldQuickStep(DWorld(), fixed_step);
    // dWorldStep(DWorld(),fixed_step);
}

void CPHIsland::Enable()
{
    if (!m_flags.is_active())
        return;

    for (dxBody* body = DWorld()->firstbody; body; body = (dxBody*)body->next)
        body->flags &= ~dxBodyDisabled;
}
void CPHIsland::Repair()
{
    if (!m_flags.is_active())
        return;
    dBodyID body;
    for (body = firstbody; body; body = (dxBody*)body->next)
    {
        if (!dV_valid(dBodyGetAngularVel(body)))
            dBodySetAngularVel(body, 0.f, 0.f, 0.f);
        if (!dV_valid(dBodyGetLinearVel(body)))
            dBodySetLinearVel(body, 0.f, 0.f, 0.f);
        if (!dV_valid(dBodyGetPosition(body)))
            dBodySetPosition(body, 0.f, 0.f, 0.f);
        if (!dQ_valid(dBodyGetQuaternion(body)))
        {
            dQuaternion q = {1.f, 0.f, 0.f, 0.f}; // dQSetIdentity(q);
            dBodySetQuaternion(body, q);
        }
    }
}
