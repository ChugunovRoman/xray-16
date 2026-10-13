#include "StdAfx.h"

#include "PHStepDefer.h"

#include "PHWorld.h"
#include "PHShell.h"
#include "PHElement.h"
#include "PHJoint.h"
#include "IPhysicsShellHolder.h"

#include "xrEngine/xr_object.h"
#include "xrEngine/xr_object_list.h"

// See PHStepDefer.h for what is deferred, when, and why.

namespace ph_step_defer
{
namespace
{
// > 0 inside a Lua export of the physics API (ScriptCallScope) - only for the lua/cpp split of the counters.
thread_local int s_tl_script_call_depth = 0;

// CPHUpdateObject activations waiting for the flush (DeferUpdateObjectActivate). Only the address is compared until
// the flush finds it still here: a later Deactivate (the destructor too) takes it out, so a queued entry never
// dereferences an object that was destroyed after it was queued.
std::mutex s_pending_update_lock;
xr_vector<CPHUpdateObject*> s_pending_update_activate;
// CPHUpdateObject removals waiting for the flush (DeferUpdateObjectDeactivate): until then the object is still linked
// into CPHWorld::m_update_objects (an intrusive list). A later Activate drops the removal (the object just stays in the
// list); the destructor (DeactivateOnDestroy) drops it and unlinks the object itself - a queued removal must never walk
// the list to an object that was freed after it was queued (e.g. ~CPHCapture -> Deactivate, then the delete).
xr_vector<CPHUpdateObject*> s_pending_update_deactivate;

void CountDeferred(Kind kind)
{
#if XR_PERF_METRICS
    if (s_tl_script_call_depth > 0)
        PERF_INC(ph_defer_lua);
    else
        PERF_INC(ph_defer_cpp);
    switch (kind)
    {
    case Kind::Force: PERF_INC(ph_defer_force); break;
    case Kind::Velocity: PERF_INC(ph_defer_velocity); break;
    case Kind::Fix: PERF_INC(ph_defer_fix); break;
    case Kind::Joint: PERF_INC(ph_defer_joint); break;
    case Kind::Enable: PERF_INC(ph_defer_enable); break;
    case Kind::Contact: PERF_INC(ph_defer_contact); break;
    case Kind::Teleport: PERF_INC(ph_defer_teleport); break;
    case Kind::Object: PERF_INC(ph_defer_object); break;
    case Kind::Transform: PERF_INC(ph_defer_transform); break;
    }
#else
    (void)kind;
#endif
}

// The owner object, its current main shell and the shell's address as of queueing. Addresses are only compared, never
// dereferenced, until the object has been found again by ID.
struct ShellRef
{
    u16 id{u16(-1)};
    const void* holder{};
    const CPhysicsShell* shell{};
};

bool MakeShellRef(CPHShell* shell, ShellRef& ref)
{
    if (!shell || shell->Elements().empty())
        return false;
    IPhysicsShellHolder* holder = shell->PhysicsRefObject();
    if (!holder)
        return false;
    const u16 id = holder->ObjectID();
    if (id == u16(-1))
        return false;
    // Only the owner's main shell can be found again by ID (ObjectPPhysicsShell); e.g. an NPC's animated collision
    // shell (physics_shell_animated) cannot.
    if (holder->ObjectPPhysicsShell() != static_cast<CPhysicsShell*>(shell))
        return false;
    ref.id = id;
    ref.holder = holder;
    ref.shell = shell;
    return true;
}

// Main thread, in the flush: the step is joined, nothing runs beside this.
CPHShell* ResolveShell(const ShellRef& ref)
{
    IGameObject* object = inl_ph_world().LevelObjects().net_Find(ref.id);
    IPhysicsShellHolder* holder = smart_cast<IPhysicsShellHolder*>(object);
    if (!holder || holder != ref.holder || holder->ObjectGetDestroy())
        return nullptr;
    CPhysicsShell* shell = holder->ObjectPPhysicsShell();
    if (!shell || shell != ref.shell)
        return nullptr;
    return static_cast<CPHShell*>(shell);
}

void CountDropped() { PERF_INC(ph_defer_dropped); }

bool Queue(Kind kind, std::function<void()> op)
{
    if (!ph_world->defer_scheduler_mutation(std::move(op)))
        return false;
    CountDeferred(kind);
    return true;
}

// Teleports waiting for the flush (DeferElementTransform), one per element. The queued flush operation only carries the
// element's address as the key and takes the entry out under the lock, so a later teleport of the same element in the
// same window just replaces the pose (the last one wins). The address is compared, never dereferenced, until the
// element has been found again in its owner's shell.
struct PendingTransform
{
    ShellRef ref;
    Fmatrix transform;
    u8 history_state;
    bool fill_interpolation;
};
std::mutex s_pending_transform_lock;
xr_map<const CPHElement*, PendingTransform> s_pending_transform;

// Main thread, in the flush.
void ApplyPendingTransform(const CPHElement* key)
{
    PendingTransform pending;
    {
        std::lock_guard<std::mutex> guard(s_pending_transform_lock);
        const auto it = s_pending_transform.find(key);
        if (it == s_pending_transform.end())
            return; // cleared with the level (ClearPending)
        pending = it->second;
        s_pending_transform.erase(it);
    }
    if (CPHShell* shell = ResolveShell(pending.ref))
    {
        for (CPHElement* candidate : shell->Elements())
        {
            if (candidate != key)
                continue;
            if (!candidate->ApplyDeferredTransform(
                    pending.transform, motion_history_state(pending.history_state), pending.fill_interpolation))
                CountDropped(); // left the world before the flush (shell deactivated)
            return;
        }
    }
    CountDropped();
}
} // namespace

bool StepRunningOnOtherThread() { return ph_world && ph_world->IsStepRunningOnOtherThread(); }

ScriptCallScope::ScriptCallScope() { ++s_tl_script_call_depth; }
ScriptCallScope::~ScriptCallScope() { --s_tl_script_call_depth; }

bool DeferForObject(Kind kind, u16 object_id, std::function<void(IGameObject&)> op)
{
    if (!StepRunningOnOtherThread() || object_id == u16(-1))
        return false;
    return Queue(kind, [object_id, op = std::move(op)]
    {
        IGameObject* object = inl_ph_world().LevelObjects().net_Find(object_id);
        if (!object || object->getDestroy())
        {
            CountDropped();
            return;
        }
        op(*object);
    });
}

bool DeferShell(CPHShell& shell, Kind kind, std::function<void(CPHShell&)> op)
{
    if (!StepRunningOnOtherThread())
        return false;
    ShellRef ref;
    if (!MakeShellRef(&shell, ref))
    {
        static xray::perf::OverlapHazardSite site;
        ReportHazard(site, "CPHShell mutation (no owner ID to defer by)");
        return false;
    }
    return Queue(kind, [ref, op = std::move(op)]
    {
        CPHShell* target = ResolveShell(ref);
        if (!target)
        {
            CountDropped();
            return;
        }
        op(*target);
    });
}

bool DeferElement(CPHElement& element, Kind kind, std::function<void(CPHElement&)> op)
{
    if (!StepRunningOnOtherThread())
        return false;
    ShellRef ref;
    if (!MakeShellRef(element.PHShell(), ref))
    {
        static xray::perf::OverlapHazardSite site;
        ReportHazard(site, "CPHElement mutation (no owner ID to defer by)");
        return false;
    }
    const CPHElement* const element_address = &element;
    return Queue(kind, [ref, element_address, op = std::move(op)]
    {
        if (CPHShell* shell = ResolveShell(ref))
        {
            for (CPHElement* candidate : shell->Elements())
            {
                if (candidate == element_address)
                {
                    op(*candidate);
                    return;
                }
            }
        }
        CountDropped();
    });
}

bool DeferJoint(CPHShell* shell, CPHJoint& joint, Kind kind, std::function<void(CPHJoint&)> op)
{
    if (!StepRunningOnOtherThread())
        return false;
    ShellRef ref;
    if (!MakeShellRef(shell, ref))
    {
        static xray::perf::OverlapHazardSite site;
        ReportHazard(site, "CPHJoint mutation (no owner ID to defer by)");
        return false;
    }
    const CPhysicsJoint* const joint_address = &joint;
    return Queue(kind, [ref, joint_address, op = std::move(op)]
    {
        if (CPHShell* target = ResolveShell(ref))
        {
            const u16 count = target->get_JointsNumber();
            for (u16 i = 0; i < count; ++i)
            {
                CPhysicsJoint* candidate = target->get_JointByStoreOrder(i);
                if (candidate == joint_address)
                {
                    op(*static_cast<CPHJoint*>(candidate));
                    return;
                }
            }
        }
        CountDropped();
    });
}

bool DeferElementTransform(CPHElement& element, const Fmatrix& transform, u8 history_state, bool fill_interpolation)
{
    if (!StepRunningOnOtherThread())
        return false;
    ShellRef ref;
    if (!MakeShellRef(element.PHShell(), ref))
    {
        // E.g. the actor's camera collision shell or an NPC's animated collision shell (not the owner's main shell).
        PERF_INC(ph_defer_hazard_transform);
        static xray::perf::OverlapHazardSite site;
        ReportHazard(site, "CPHElement::SetTransform (body in the world, no owner ID to defer by)");
        return false;
    }
    const CPHElement* const key = &element;
    std::lock_guard<std::mutex> guard(s_pending_transform_lock);
    const auto it = s_pending_transform.find(key);
    if (it != s_pending_transform.end())
    {
        // Already queued in this window: only the pose the flush applies changes.
        PendingTransform& pending = it->second;
        pending.ref = ref;
        pending.transform = transform;
        if (history_state != u8(mh_not_clear))
            pending.history_state = history_state;
        pending.fill_interpolation = pending.fill_interpolation || fill_interpolation;
        PERF_INC(ph_defer_transform_coalesced);
        return true;
    }
    // Queued under s_pending_transform_lock: the flush operation takes the same lock, so it cannot run before the entry
    // below exists. Lock order is always s_pending_transform_lock -> CPHWorld::m_deferred_mutations_lock (the flush
    // runs the operations after releasing the latter).
    if (!Queue(Kind::Transform, [key] { ApplyPendingTransform(key); }))
        return false;
    s_pending_transform.emplace(key, PendingTransform{ref, transform, history_state, fill_interpolation});
    return true;
}

bool DeferUpdateObjectActivate(CPHUpdateObject& object)
{
    // Deactivated earlier in this window and not removed yet: it is still in the list, so the removal is just dropped.
    if (CancelUpdateObjectDeactivate(object))
        return true;
    if (!StepRunningOnOtherThread())
        return false;
    {
        std::lock_guard<std::mutex> guard(s_pending_update_lock);
        s_pending_update_activate.push_back(&object);
    }
    CPHUpdateObject* const address = &object;
    const bool queued = Queue(Kind::Object, [address]
    {
        {
            std::lock_guard<std::mutex> guard(s_pending_update_lock);
            const auto it = std::find(s_pending_update_activate.begin(), s_pending_update_activate.end(), address);
            if (it == s_pending_update_activate.end())
                return; // deactivated (or destroyed) before the flush
            s_pending_update_activate.erase(it);
        }
        ph_world->AddUpdateObject(address);
    });
    if (!queued)
    {
        std::lock_guard<std::mutex> guard(s_pending_update_lock);
        const auto it = std::find(s_pending_update_activate.begin(), s_pending_update_activate.end(), address);
        if (it != s_pending_update_activate.end())
            s_pending_update_activate.erase(it);
    }
    return queued;
}

bool CancelUpdateObjectActivate(CPHUpdateObject& object)
{
    // Activated during the current step and not in the list yet. Checked even without a step in flight (the flush
    // itself, or a deactivation right after the join): harmless, and the list stays consistent whatever the timing.
    std::lock_guard<std::mutex> guard(s_pending_update_lock);
    const auto it = std::find(s_pending_update_activate.begin(), s_pending_update_activate.end(), &object);
    if (it == s_pending_update_activate.end())
        return false;
    s_pending_update_activate.erase(it);
    return true;
}

bool DeferUpdateObjectDeactivate(CPHUpdateObject& object)
{
    if (CancelUpdateObjectActivate(object))
        return true;
    if (!StepRunningOnOtherThread())
        return false;
    CPHUpdateObject* const address = &object;
    {
        std::lock_guard<std::mutex> guard(s_pending_update_lock);
        s_pending_update_deactivate.push_back(address);
    }
    const bool queued = Queue(Kind::Object, [address]
    {
        {
            std::lock_guard<std::mutex> guard(s_pending_update_lock);
            const auto it = std::find(s_pending_update_deactivate.begin(), s_pending_update_deactivate.end(), address);
            if (it == s_pending_update_deactivate.end())
                return; // activated again or destroyed before the flush
            s_pending_update_deactivate.erase(it);
        }
        ph_world->RemoveUpdateObject(address);
    });
    if (!queued)
        CancelUpdateObjectDeactivate(object);
    return queued;
}

bool CancelUpdateObjectDeactivate(CPHUpdateObject& object)
{
    std::lock_guard<std::mutex> guard(s_pending_update_lock);
    const auto it = std::find(s_pending_update_deactivate.begin(), s_pending_update_deactivate.end(), &object);
    if (it == s_pending_update_deactivate.end())
        return false;
    s_pending_update_deactivate.erase(it);
    return true;
}

void ClearPending()
{
    {
        std::lock_guard<std::mutex> guard(s_pending_update_lock);
        s_pending_update_activate.clear();
        s_pending_update_deactivate.clear();
    }
    std::lock_guard<std::mutex> guard(s_pending_transform_lock);
    s_pending_transform.clear();
}

void ReportHazard(xray::perf::OverlapHazardSite& site, const char* where) noexcept
{
    PERF_INC(ph_defer_hazard);
    if (!xray::perf::g_overlap_hazard_probe)
        return;
    const u32 n = site.count.fetch_add(1, std::memory_order_relaxed) + 1;
    // Names the caller: the whole stack of the first few distinct call paths of this site (probe only, symbolized by
    // the same code as the crash log; up to kHazardStacksPerSite per site, so the cost is bounded; Windows only).
    if (xray::perf::IsNewHazardStack(site))
    {
        string512 header;
        xr_sprintf(header, "! [physics-step-hazard] %s: hit #%u, call stack of a new caller:", where, n);
        xrDebug::LogStackTrace(header);
    }
    if (n <= 8)
    {
        Msg("! [physics-step-hazard] %s: called on a non-physics thread while the physics step runs, hit #%u", where, n);
        return;
    }
    const u64 now_ms = CPU::GetTicks();
    u64 last = site.last_report_ms.load(std::memory_order_relaxed);
    if (now_ms - last < 5000)
        return;
    if (!site.last_report_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed))
        return;
    Msg("! [physics-step-hazard] %s: called on a non-physics thread while the physics step runs, %u hits so far", where,
        n);
}
} // namespace ph_step_defer
