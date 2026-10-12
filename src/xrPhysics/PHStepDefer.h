#pragma once

// B6, early physics kick, always (plans/optimization_mt/09-physics-earlier-parallel.md, "Режим 2: доделка").
//
// With the early kick the physics step runs on its own thread parallel to the whole Game thread (non-RT scheduler,
// Lua binders, level_on_frame) and to the main thread's render. Any change to a simulated (active) body, joint or
// contact callback list made from one of those threads lands in the middle of the step: CPHObject::activate() edits
// CPHWorld::m_objects while Step() walks it, a contact callback list is resized while Collide() iterates it, etc.
//
// The rule implemented here: a mutation of an ACTIVE shell / element / joint, called NOT from the step thread while a
// step is in flight (IPHWorld::IsStepRunningOnOtherThread), is queued into CPHWorld's deferred-mutation queue and
// applied on the main thread right after the step is joined (CPHWorld::flush_deferred_mutations). The queued entry
// does not keep a usable raw pointer: it stores the owner object's ID and re-resolves the object, its current shell
// and the element / joint in it at flush time. If any of them is gone (object destroyed, shell rebuilt, ragdoll
// created in between) the mutation is dropped (physics/defer/dropped).
//
// Inactive shells / elements / joints (being built, not in the world yet) are private to their creator and are
// mutated immediately, as before. Reads (positions, velocities, angles) are never deferred: like the render, they may
// see the body a fraction of a step off.
//
// Teleports of bodies in the simulated world (CPHElement::SetTransform / ToBonePos, CPHShell::SetTransform /
// SetGlTransformDynamic) are deferred the same way, but coalesced per element: if one body is teleported several times
// before the flush, only the last pose is applied (DeferElementTransform). A body that is not in the world yet (a
// ragdoll skeleton prebuilt on the scheduler, P16; a shell in the middle of its activation) is set right away.
//
// Everything that cannot be deferred this way (shell activation / deactivation, relative moves of bodies, character
// controllers, ...) is only REPORTED: PH_STEP_HAZARD below (physics/defer/hazard, and a log line with
// overlap_hazard_probe 1; the first distinct call stacks of every site are logged as well).

#include <functional>

#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrPhysics.h"

class CPHShell;
class CPHElement;
class CPHJoint;
class CPHUpdateObject;
class IGameObject;

namespace ph_step_defer
{
// What a deferred mutation does - one physics/defer/<kind> counter each.
enum class Kind : u8
{
    Force, // applyForce / applyImpulse* / applyGravityAccel
    Velocity, // set_LinearVel / set_AngularVel / setForce / setTorque
    Fix, // Fix / ReleaseFixed / set_ApplyByGravity
    Joint, // CPHJoint setters (motor force and velocity, limits, axes, spring/damping)
    Enable, // Enable() of a sleeping shell or element (activates the object: edits CPHWorld::m_objects)
    Contact, // contact callback lists (doors: set_door_ignore_dynamics), collision on/off (physics spatial DB)
    Teleport, // Lua teleports (set_npc_position, set_actor_position, force_set_position)
    Object, // owner-level C++ / Lua operations (monster capture, telekinesis update objects, physics_world)
    Transform, // teleport of a body in the world: CPHElement::SetTransform / ToBonePos, CPHShell::SetGlTransformDynamic
};

// True while a physics step runs on another thread than the caller (see the header comment). Cheap: an atomic
// load on the common path (no step in flight).
XRPHYSICS_API bool StepRunningOnOtherThread();

// Lua exports of the physics API open this scope around the call, so the deferral is counted as physics/defer/lua
// rather than physics/defer/cpp.
struct XRPHYSICS_API ScriptCallScope
{
    ScriptCallScope();
    ~ScriptCallScope();
    ScriptCallScope(const ScriptCallScope&) = delete;
    ScriptCallScope& operator=(const ScriptCallScope&) = delete;
};

// Owner-level deferral (xrGame): queues `op` for the post-step flush. At flush the object is looked up again by
// `object_id`; `op` runs only if it still exists and is not being destroyed. Returns false when no step runs beside
// this thread - the caller then does the work itself, right away.
XRPHYSICS_API bool DeferForObject(Kind kind, u16 object_id, std::function<void(IGameObject&)> op);

// Element / shell / joint level deferral (xrPhysics internals). Return false (caller mutates right away) when no
// step runs beside this thread, or when the target cannot be addressed by an owner ID (no owner object, or a shell
// that is not the owner's main one - e.g. an NPC's animated collision shell); the latter is reported as a hazard.
bool DeferShell(CPHShell& shell, Kind kind, std::function<void(CPHShell&)> op);
bool DeferElement(CPHElement& element, Kind kind, std::function<void(CPHElement&)> op);
bool DeferJoint(CPHShell* shell, CPHJoint& joint, Kind kind, std::function<void(CPHJoint&)> op);

// CPHElement::SetTransform / ToBonePos of a body in the simulated world (CPHElement::IsInSimulatedWorld). Coalesced:
// the first call of the window queues one flush operation for the element, later ones only replace the pose it will
// apply (physics/defer/transform_coalesced). A history state that clears the motion history is not downgraded by a later
// mh_not_clear, and fill_interpolation (ToBonePos) sticks once requested. At the flush the element is found again by
// its owner's ID and CPHElement::ApplyDeferredTransform runs. `history_state` is a motion_history_state (PhysicsShell.h).
// Returns false - the caller sets the pose right away - when no step runs beside this thread, or when the element
// cannot be addressed by an owner ID (reported as a hazard, physics/defer/hazard_transform).
bool DeferElementTransform(CPHElement& element, const Fmatrix& transform, u8 history_state, bool fill_interpolation);

// CPHUpdateObject::Activate / Deactivate while a step walks CPHWorld::m_update_objects: the list edit is postponed to
// the flush (the object's own activated flag changes right away). A pending activation is cancelled by a later
// Deactivate (and so by the destructor). Return false - edit the list now.
bool DeferUpdateObjectActivate(CPHUpdateObject& object);
bool DeferUpdateObjectDeactivate(CPHUpdateObject& object);
// Only cancels a pending activation (true if there was one) - for the destructor, which must not queue anything.
bool CancelUpdateObjectActivate(CPHUpdateObject& object);
// Only cancels a pending removal (true if there was one: the object is still in CPHWorld::m_update_objects) - for the
// destructor, which then unlinks the object itself.
bool CancelUpdateObjectDeactivate(CPHUpdateObject& object);
void ClearPending(); // CPHWorld::Destroy

// Counts physics/defer/hazard and, with overlap_hazard_probe 1, logs `where` (rate-limited per site) and, on Windows,
// the whole call stack of the first few distinct callers of the site (so that the log names the caller).
XRPHYSICS_API void ReportHazard(xray::perf::OverlapHazardSite& site, const char* where) noexcept;
} // namespace ph_step_defer

// A structural physics mutation (activation, deactivation, teleport, body/joint creation or removal in a simulated
// shell) reached from a thread other than the step thread while the step runs. Not deferred - flagged, so that the
// remaining paths can be found in game (overlap_hazard_probe 1).
#define PH_STEP_HAZARD(where)                                                                                    \
    do                                                                                                           \
    {                                                                                                            \
        if (::ph_step_defer::StepRunningOnOtherThread())                                                         \
        {                                                                                                        \
            static ::xray::perf::OverlapHazardSite _ph_step_hazard_site;                                         \
            ::ph_step_defer::ReportHazard(_ph_step_hazard_site, where);                                          \
        }                                                                                                        \
    } while (0)
