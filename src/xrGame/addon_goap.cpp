#include "StdAfx.h"

#include "addon_goap.h"
#include "addon_api_common.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "action_planner.h"
#include "ai/stalker/ai_stalker.h"
#include "Level.h"
#include "npc_cpp_profile.h"
#include "script_action_wrapper.h"
#include "script_game_object.h"
#include "script_property_evaluator_wrapper.h"
#include "stalker_planner.h"

#include <algorithm>
#include <typeinfo>

namespace gw::addons::goap
{
namespace
{
class CNativeEvaluatorProxy;
class CNativeActionProxy;

struct Registration
{
    GwpEvaluatorId id = GWP_INVALID_EVALUATOR_ID;
    const GwpPlugin* plugin = nullptr;
    u32 planner = GWP_PLANNER_MAIN;
    u32 property = 0;
    xr_string lua_name;     // empty: any Lua evaluator
    xr_string profile_name; // "<lua name>#native": bucket of the native calls in the -npc_cpp_profile report
    GwpEvaluatorFn fn = nullptr;
    void* user = nullptr;
    u32 mode = GWP_EVALUATOR_LUA;
    bool dead = false; // unregistered: the proxies call Lua only and are taken out at the next ApplyPending
    u64 native_calls = 0;
    u64 lua_calls = 0;
    u64 mismatches = 0;
    u32 skipped = 0; // stalkers left on the old evaluator: a constant, another Lua evaluator, dead meanwhile
    u32 absent = 0;  // stalkers whose planner has not got the property after the tries (the scheme is not theirs)
    xr_vector<CNativeEvaluatorProxy*> proxies; // live proxies (owned by their planners)
};

// An action registration: the same shape for the steps of a Lua action (action_register)
struct ActionRegistration
{
    GwpActionRegId id = GWP_INVALID_ACTION_REG_ID;
    const GwpPlugin* plugin = nullptr;
    u32 planner = GWP_PLANNER_MAIN;
    u32 action_id = 0;
    xr_string lua_name;     // empty: any Lua action
    xr_string profile_name; // "<lua name>#native": bucket of the native steps in the -npc_cpp_profile report
    GwpActionVTable vtable{};
    void* user = nullptr;
    u32 mode = GWP_ACTION_LUA;
    bool dead = false; // unregistered: the proxies hand the runs over to Lua and are taken out at the next ApplyPending
    u64 native_steps = 0;
    u64 lua_steps = 0;
    u64 handovers = 0;
    u32 skipped = 0;
    u32 absent = 0; // stalkers whose planner has not got the action id after the tries (no bind_manager yet)
    xr_vector<CNativeActionProxy*> proxies; // live proxies (owned by their planners)
};

xr_vector<Registration*>* g_registrations = nullptr;
xr_vector<ActionRegistration*>* g_action_registrations = nullptr;
GwpEvaluatorId g_next_id = 1;
GwpActionRegId g_next_action_id = 1;

// Stalkers whose planners still have to get the registrations. A scheme adds its evaluator when its section is
// activated: at the first update after net_spawn, much later for an NPC whose smart had no job for it at the
// spawn (setup_logic runs when a job is found), or at a switch to a section of a scheme the NPC had not had yet
// (camper, patrol, walker, animpoint add their evaluators only then). So a stalker is queued at its spawn and
// again at every change of its active_section (OnLogicChanged, from the db.storage mirror), and a missing property,
// or one that still holds the engine's evaluator (the scheme replaces it, e.g. xr_danger for property 8), is
// retried every frame for kRetryMs from then.
struct Pending
{
    u16 id;
    u32 until_ms;       // the end of the retries
    bool spawn = true;  // queued at the spawn: counted in `skipped` and logged when it settles; a requeue after a
                        // change of the logic settles silently (a property of a scheme the NPC has not got is normal)
    u32 next_ms = 0;    // the next try: every kRetryStepMs, not every frame (a spawn wave queues hundreds)
    u64 done = 0;       // evaluator registrations (by index, the first 64) already in place: not tried again
    u64 done_actions = 0; // action registrations (by index, the first 64) already in place
    u32 serial = 0;     // g_registrations_serial the `done` bits belong to
    bool alive = true;  // alive when queued: one dead at the settling has died in the queue (counted in `skipped`);
                        // a corpse from the start (a save holds many) is no skip of a living NPC
};
xr_vector<Pending>* g_pending = nullptr;
bool g_restore_pending = false; // a dead registration has live proxies (evaluators or actions)
constexpr u32 kRetryMs = 10000;
constexpr u32 kRetryStepMs = 100;
u32 g_registrations_serial = 1; // bumped when either registry changes: the indices of the `done` bits move

bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [goap] %s called outside the main thread, ignored", function);
    return false;
}

xr_vector<Registration*>& Registrations()
{
    if (!g_registrations)
        g_registrations = xr_new<xr_vector<Registration*>>();
    return *g_registrations;
}

xr_vector<ActionRegistration*>& ActionRegistrations()
{
    if (!g_action_registrations)
        g_action_registrations = xr_new<xr_vector<ActionRegistration*>>();
    return *g_action_registrations;
}

Registration* FindRegistration(GwpEvaluatorId id)
{
    if (!g_registrations || id == GWP_INVALID_EVALUATOR_ID)
        return nullptr;
    for (Registration* registration : *g_registrations)
    {
        if (registration->id == id && !registration->dead)
            return registration;
    }
    return nullptr;
}

ActionRegistration* FindActionRegistration(GwpActionRegId id)
{
    if (!g_action_registrations || id == GWP_INVALID_ACTION_REG_ID)
        return nullptr;
    for (ActionRegistration* registration : *g_action_registrations)
    {
        if (registration->id == id && !registration->dead)
            return registration;
    }
    return nullptr;
}

void DisablePlugin(const GwpPlugin* plugin);

// ---------------------------------------------------------------------------------------------
// The proxy
// ---------------------------------------------------------------------------------------------

struct NativeCall
{
    GwpEvaluatorFn fn;
    void* user;
    u16 npc;
    int result;
};

void NativeThunk(void* context)
{
    NativeCall& call = *static_cast<NativeCall*>(context);
    call.result = call.fn(call.user, call.npc);
}

class CNativeEvaluatorProxy final : public CScriptPropertyEvaluator
{
    using inherited = CScriptPropertyEvaluator;

public:
    CNativeEvaluatorProxy(Registration* registration, CScriptPropertyEvaluator* lua, CScriptActionPlanner* planner, u16 npc)
        : inherited(lua->m_object, lua->m_evaluator_name), m_registration(registration), m_lua(lua),
          m_lua_wrapper(dynamic_cast<CScriptPropertyEvaluatorWrapper*>(lua)), m_planner(planner), m_npc(npc)
    {
        m_storage = lua->m_storage;
        m_registration->proxies.push_back(this);
    }

    ~CNativeEvaluatorProxy() override
    {
        Unlink();
        xr_delete(m_lua); // the planner deleted us: the wrapped Lua evaluator goes with us
    }

    void setup(CScriptGameObject* object, CPropertyStorage* storage) override
    {
        inherited::setup(object, storage);
        if (m_lua)
            m_lua->setup(object, storage);
    }

    _value_type evaluate() override
    {
        Registration* registration = m_registration;
        if (!registration || registration->dead || registration->mode == GWP_EVALUATOR_LUA || !registration->fn)
            return EvaluateLua(registration);

        NativeCall call{ registration->fn, registration->user, m_npc, 0 };
        const u64 start = npc_cpp_profile::enabled() ? CPU::QPC() : 0;
        const bool ok = events::CallPluginGuarded(
            reinterpret_cast<const void*>(registration->fn), "native evaluator", &NativeThunk, &call);
        if (start)
            npc_cpp_profile::add_script_evaluator(registration->profile_name.c_str(), CPU::QPC() - start);
        if (!ok)
        {
            const GwpPlugin* const plugin = registration->plugin;
            DisablePlugin(plugin);
            string128 what;
            xr_sprintf(what, "its native evaluator of property %u", registration->property);
            OnPluginCrashed(plugin, what); // the whole plugin stops
            return EvaluateLua(registration);
        }
        ++registration->native_calls;
        const bool native = call.result != 0;
        if (registration->mode == GWP_EVALUATOR_NATIVE)
            return native;
        // shadow: Lua decides, the difference is counted. Both answers are fresh: the cache of the policy would
        // compare an answer up to its TTL old with the native one of this call
        const bool lua = EvaluateLua(registration, true);
        if (lua != native)
            ++registration->mismatches;
        return lua;
    }

    // The save stream of the planner is positional: exactly the bytes of the Lua evaluator.
    void save(NET_Packet& packet) override
    {
        if (m_lua)
            m_lua->save(packet);
    }
    void load(IReader& packet) override
    {
        if (m_lua)
            m_lua->load(packet);
    }

    Registration* registration() const { return m_registration; }
    CScriptActionPlanner* planner() const { return m_planner; }

    // Gives the Lua evaluator back (the caller puts it into the planner) and forgets the registration.
    CScriptPropertyEvaluator* ReleaseLua()
    {
        CScriptPropertyEvaluator* lua = m_lua;
        m_lua = nullptr;
        m_lua_wrapper = nullptr;
        Unlink();
        return lua;
    }

    void ForgetRegistration() { m_registration = nullptr; }

private:
    bool EvaluateLua(Registration* registration, bool uncached = false)
    {
        if (registration)
            ++registration->lua_calls;
        if (!m_lua)
            return false;
        return uncached && m_lua_wrapper ? m_lua_wrapper->evaluate_uncached() : m_lua->evaluate();
    }

    void Unlink()
    {
        if (!m_registration)
            return;
        auto& proxies = m_registration->proxies;
        const auto it = std::find(proxies.begin(), proxies.end(), this);
        if (it != proxies.end())
        {
            *it = proxies.back();
            proxies.pop_back();
        }
        m_registration = nullptr;
    }

    Registration* m_registration;
    CScriptPropertyEvaluator* m_lua;
    CScriptPropertyEvaluatorWrapper* m_lua_wrapper; // m_lua as the Lua wrapper (the only kind a proxy wraps)
    CScriptActionPlanner* m_planner; // alive while we are: it owns us
    u16 m_npc;
};

// ---------------------------------------------------------------------------------------------
// The action proxy (action_register): wraps a Lua action the way CNativeEvaluatorProxy wraps a Lua evaluator.
// The differences from an evaluator: a run (initialize -> execute* -> finalize) carries state, and the steps have
// side effects, so the shadow mode of the evaluators becomes the verify mode: Lua always acts, the plugin only
// decides around the Lua execute step (pre_execute / post_execute of the vtable) and compares its decision with
// what Lua really did (what is comparable is up to the plugin: unlike an evaluator there is no boolean answer).
// The mode of one run is fixed at its initialize; a proxy installed into a run that has begun stays in the Lua
// mode until the next initialize. When the plugin dies or unregisters while a native run is on, the run is handed
// over: the Lua action is initialized at its next execute and takes over, its finalize of the native side never
// comes. Between frames only (ApplyPending), as for the evaluators: never during a solve.
// ---------------------------------------------------------------------------------------------

struct ActionStepCall
{
    void(GWP_CALL* fn)(void*, GwpObjectId);
    void* user;
    u16 npc;
};

void ActionStepThunk(void* context)
{
    const ActionStepCall& call = *static_cast<const ActionStepCall*>(context);
    call.fn(call.user, call.npc);
}

class CNativeActionProxy final : public CScriptActionBase
{
    using inherited = CScriptActionBase;

public:
    CNativeActionProxy(ActionRegistration* registration, CScriptActionWrapper* lua, CScriptActionPlanner* planner,
        u16 npc)
        : inherited(lua->m_object, lua->m_action_name), m_registration(registration), m_lua(lua), m_planner(planner),
          m_npc(npc)
    {
        m_storage = lua->m_storage;
        // The timing of the action: swap_operator does not call setup, so nothing else sets it, while whoever
        // gets the action of the slot (current_action() of the planner, from the engine or Lua) reads it through
        // the non-virtual completed()/first_time()/inertia_time(). Take the state of the Lua action we replace
        // (a proxy installed into a begun run carries its start time on)
        m_start_level_time = lua->start_level_time();
        m_inertia_time = lua->inertia_time();
        m_first_time = lua->first_time();
        // The planner searches our conditions and effects: take the lists of the Lua action (a Lua
        // add_precondition on the action after the install reaches us and is given back at the removal)
        for (const auto& condition : lua->conditions().conditions())
            add_condition(condition);
        for (const auto& effect : lua->effects().conditions())
            add_effect(effect);
        m_registration->proxies.push_back(this);
    }

    ~CNativeActionProxy() override
    {
        Unlink();
        xr_delete(m_lua); // the planner deleted us: the wrapped Lua action goes with us
    }

    void setup(CScriptGameObject* object, CPropertyStorage* storage) override
    {
        inherited::setup(object, storage);
        if (m_lua)
            m_lua->setup(object, storage);
    }

    void initialize() override
    {
        // The mode of this run, dead plugin included (it stays Lua then); a switch acts from the next run
        m_running_mode = ResolvedMode();
        m_lua_initialized = m_running_mode != GWP_ACTION_NATIVE;
        if (m_running_mode == GWP_ACTION_NATIVE)
        {
            const u64 start = ProfileStart();
            // The engine part of the step, as a Lua action that calls action_base.initialize(self) first
            inherited::initialize();
            const bool ok = CallPluginStep(EStep::Initialize, "initialize");
            ProfileAdd(start, "initialize");
            if (ok)
            {
                CountNativeStep();
                return;
            }
            HandOver(); // the plugin crashed: the Lua action takes the run (its initialize is the step)
            CountLuaStep();
            return;
        }
        m_lua->initialize(); // the Lua method calls action_base.initialize on itself
        CountLuaStep();
    }

    void execute() override
    {
        // The plugin is gone while a native run is on: hand the run over (the Lua action has never been
        // initialized for it); its first Lua step is the initialize, then this execute goes on
        if (m_running_mode == GWP_ACTION_NATIVE && !NativeAvailable())
            HandOver();
        else if (m_running_mode == GWP_ACTION_NATIVE)
        {
            const u64 start = ProfileStart();
            inherited::execute();
            const bool ok = CallPluginStep(EStep::Execute, "execute");
            ProfileAdd(start, "execute");
            if (ok)
            {
                CountNativeStep();
                return;
            }
            HandOver(); // the plugin crashed in the step: the Lua action takes the rest of the run
        }

        // The verify mode: the plugin decides before the Lua step and compares after it; Lua acts in between
        if (m_running_mode == GWP_ACTION_VERIFY)
            CallPluginStep(EStep::PreExecute, "pre_execute");
        m_lua->execute();
        CountLuaStep();
        if (m_running_mode == GWP_ACTION_VERIFY)
            CallPluginStep(EStep::PostExecute, "post_execute");
    }

    void finalize() override
    {
        if (m_running_mode == GWP_ACTION_NATIVE)
        {
            const u64 start = ProfileStart();
            inherited::finalize();
            CallPluginStep(EStep::Finalize, "finalize"); // the run is over: a crash here only disables the plugin
            CountNativeStep();
            ProfileAdd(start, "finalize");
        }
        else
        {
            m_lua->finalize();
            CountLuaStep();
        }
        m_running_mode = GWP_ACTION_LUA; // between runs: a proxy installed into a begun run stays Lua
        m_lua_initialized = true;
    }

    // weight stays the business of the Lua action: the wrapper already answers without Lua when the script class
    // does not override it (npc_perf_script_action_weight_lookup)
    edge_value_type weight(const CSConditionState& condition0, const CSConditionState& condition1) const override
    {
        return m_lua ? m_lua->weight(condition0, condition1) : inherited::weight(condition0, condition1);
    }

    // The save stream of the planner is positional: exactly the bytes of the Lua action
    void save(NET_Packet& packet) override
    {
        if (m_lua)
            m_lua->save(packet);
    }
    void load(IReader& packet) override
    {
        if (m_lua)
            m_lua->load(packet);
    }

    ActionRegistration* registration() const { return m_registration; }
    CScriptActionPlanner* planner() const { return m_planner; }
    u32 running_mode() const { return m_running_mode; }
    // A native run that the Lua action was never initialized for (Restore hands such a run over)
    bool mid_native_run() const { return m_running_mode == GWP_ACTION_NATIVE && !m_lua_initialized; }

    // Gives the Lua action back (the caller puts it into the planner), the conditions with it, and forgets us
    CScriptActionWrapper* ReleaseLua()
    {
        CScriptActionWrapper* lua = m_lua;
        m_lua = nullptr;
        Unlink();
        return lua;
    }

    void ForgetRegistration() { m_registration = nullptr; }

private:
    // The step counters of the registration; a proxy that outlives the registry (Shutdown with a level alive)
    // has none to count into
    void CountNativeStep()
    {
        if (m_registration)
            ++m_registration->native_steps;
    }
    void CountLuaStep()
    {
        if (m_registration)
            ++m_registration->lua_steps;
    }

    enum class EStep
    {
        Initialize,
        Execute,
        Finalize,
        PreExecute,
        PostExecute,
    };

    // The mode for a run that begins now: what the registration says, Lua when it is dead or has no steps of that
    // mode. The steps differ: a native run calls initialize/execute/finalize, a verify run only pre_/post_execute
    // (a vtable with only those is a plain verify plugin, not an empty one)
    u32 ResolvedMode() const
    {
        const ActionRegistration* registration = m_registration;
        if (!registration || registration->dead || registration->mode == GWP_ACTION_LUA)
            return GWP_ACTION_LUA;
        const GwpActionVTable& vtable = registration->vtable;
        const bool has_steps = registration->mode == GWP_ACTION_VERIFY ?
            vtable.pre_execute || vtable.post_execute :
            vtable.initialize || vtable.execute || vtable.finalize;
        return has_steps ? registration->mode : GWP_ACTION_LUA;
    }

    // The native side can still run this step: the registration lives and the run is native
    bool NativeAvailable() const
    {
        const ActionRegistration* registration = m_registration;
        return registration && !registration->dead && m_running_mode == GWP_ACTION_NATIVE;
    }

    void HandOver()
    {
        if (m_registration)
            ++m_registration->handovers;
        m_running_mode = GWP_ACTION_LUA;
        if (!m_lua_initialized)
        {
            m_lua_initialized = true;
            m_lua->initialize(); // the Lua state of this run was never set up
        }
    }

    bool CallPluginStep(EStep step, pcstr what)
    {
        ActionRegistration* registration = m_registration;
        // Removed (the plugin crashed or was unloaded, the library may be gone): no step of it is called any more -
        // the finalize of a native run in flight and the pre/post steps of a verify run included
        if (!registration || registration->dead)
            return false;
        const auto fn = step == EStep::Initialize     ? registration->vtable.initialize
            : step == EStep::Execute                  ? registration->vtable.execute
            : step == EStep::Finalize                 ? registration->vtable.finalize
            : step == EStep::PreExecute               ? registration->vtable.pre_execute
                                                      : registration->vtable.post_execute;
        if (!fn)
            return true; // an empty step of the vtable is not a failure
        ActionStepCall call{ fn, registration->user, m_npc };
        if (!events::CallPluginGuarded(reinterpret_cast<const void*>(fn), "native action", &ActionStepThunk, &call))
        {
            const GwpPlugin* const plugin = registration->plugin;
            DisablePlugin(plugin);
            string256 where;
            xr_sprintf(where, "the %s of its action '%s' (id %u)", what, registration->lua_name.c_str(),
                registration->action_id);
            OnPluginCrashed(plugin, where); // the whole plugin stops
            return false;
        }
        return true;
    }

    u64 ProfileStart() const { return npc_cpp_profile::enabled() ? CPU::QPC() : 0; }
    void ProfileAdd(u64 start, pcstr step) const
    {
        // m_registration is gone after Unlink (goap::Shutdown in the middle of a native run)
        if (start && m_registration)
            npc_cpp_profile::add_script_action(m_registration->profile_name.c_str(), step, CPU::QPC() - start);
    }

    void Unlink()
    {
        if (!m_registration)
            return;
        auto& proxies = m_registration->proxies;
        const auto it = std::find(proxies.begin(), proxies.end(), this);
        if (it != proxies.end())
        {
            *it = proxies.back();
            proxies.pop_back();
        }
        m_registration = nullptr;
    }

    ActionRegistration* m_registration;
    CScriptActionWrapper* m_lua;
    CScriptActionPlanner* m_planner; // alive while we are: it owns us
    u16 m_npc;
    u32 m_running_mode = GWP_ACTION_LUA; // fixed at initialize; Lua between runs
    bool m_lua_initialized = true;       // the Lua action got the initialize of this run
};

// ---------------------------------------------------------------------------------------------
// Installing and removing
// ---------------------------------------------------------------------------------------------

// The planner the registration addresses in this stalker, or nullptr.
CScriptActionPlanner* PlannerOf(CAI_Stalker& stalker, u32 planner)
{
    CScriptActionPlanner& brain = stalker.brain();
    if (planner == GWP_PLANNER_MAIN)
        return &brain;
    // An exact lookup: get_operator returns another action for a missing id (a defect of the engine).
    for (const auto& entry : brain.operators())
    {
        if (entry.m_operator_id == planner)
            return smart_cast<CScriptActionPlanner*>(entry.m_operator);
    }
    return nullptr;
}

enum class EInstall
{
    Done,     // installed now or earlier
    Missing,  // the property is not in the planner (yet)
    Engine,   // the property holds a non-Lua evaluator (the engine's one from the spawn, or a constant one): a scheme
              // may still put its Lua evaluator there, so it is retried as Missing
    Mismatch, // the property holds another Lua evaluator or another registration's proxy: left as it is
    Ready,    // a dry probe (install == false): the Lua evaluator or action to wrap is there, left unwrapped
};

// install == false: only the probe, nothing in the planner changes (a corpse: what is not in place, and why)
EInstall Install(Registration& registration, CAI_Stalker& stalker, bool install = true)
{
    CScriptActionPlanner* planner = PlannerOf(stalker, registration.planner);
    if (!planner)
        return EInstall::Missing;
    const auto& evaluators = planner->evaluators();
    const auto it = evaluators.find(registration.property);
    if (it == evaluators.end())
        return EInstall::Missing;
    CScriptPropertyEvaluator* current = (*it).second;
    if (const auto* proxy = dynamic_cast<const CNativeEvaluatorProxy*>(current))
        return proxy->registration() == &registration ? EInstall::Done : EInstall::Mismatch;
    // Only a Lua evaluator of the given name: a constant evaluator (the dead, the zombied) or another scheme's
    // one keeps its value. A non-Lua one is not final yet: the planner starts with the engine's evaluators and the
    // scheme may replace them a little later (after the first ApplyPending).
    if (!dynamic_cast<const CScriptPropertyEvaluatorWrapper*>(current))
        return EInstall::Engine;
    if (!registration.lua_name.empty() && xr_strcmp(current->m_evaluator_name, registration.lua_name.c_str()) != 0)
        return EInstall::Mismatch;
    if (!install)
        return EInstall::Ready;

    auto* proxy = xr_new<CNativeEvaluatorProxy>(&registration, current, planner, stalker.ID());
    CScriptPropertyEvaluator* previous = planner->swap_evaluator(registration.property, proxy);
    R_ASSERT(previous == current);
    return EInstall::Done;
}

// The action the registration addresses in this stalker, or nullptr.
CScriptActionBase* ActionOf(CScriptActionPlanner& planner, u32 action_id)
{
    for (const auto& entry : planner.operators())
    {
        if (entry.m_operator_id == action_id)
            return entry.m_operator;
    }
    return nullptr;
}

EInstall InstallAction(ActionRegistration& registration, CAI_Stalker& stalker, bool install = true)
{
    CScriptActionPlanner* planner = PlannerOf(stalker, registration.planner);
    if (!planner)
        return EInstall::Missing;
    CScriptActionBase* current = ActionOf(*planner, registration.action_id);
    if (!current)
        return EInstall::Missing; // bind_manager has not added it yet (net_spawn of the stalker)
    if (const auto* proxy = dynamic_cast<const CNativeActionProxy*>(current))
        return proxy->registration() == &registration ? EInstall::Done : EInstall::Mismatch;
    // Only a Lua action of the given name. A non-Lua one is the engine's own action of an id the scripts have
    // not taken yet: not final, the scripts add theirs at net_spawn (bind_manager), so it is retried as Missing
    if (!dynamic_cast<const CScriptActionWrapper*>(current))
        return EInstall::Engine;
    if (!registration.lua_name.empty() &&
        xr_strcmp(current->m_action_name ? current->m_action_name : "", registration.lua_name.c_str()) != 0)
        return EInstall::Mismatch;
    if (!install)
        return EInstall::Ready;

    auto* proxy = xr_new<CNativeActionProxy>(
        &registration, dynamic_cast<CScriptActionWrapper*>(current), planner, stalker.ID());
    CScriptActionBase* previous = planner->swap_operator(registration.action_id, proxy);
    R_ASSERT(previous == current);
    return EInstall::Done;
}

// For the -addon_debug line of a skipped stalker: why the registration is not in place.
xr_string DescribeSkip(const Registration& registration, CAI_Stalker& stalker, EInstall result)
{
    CScriptActionPlanner* planner = PlannerOf(stalker, registration.planner);
    if (!planner)
        return "the planner is not there";
    const auto& evaluators = planner->evaluators();
    const auto it = evaluators.find(registration.property);
    if (it == evaluators.end())
        return "the property is not in the planner";
    CScriptPropertyEvaluator* current = (*it).second;
    if (const auto* proxy = dynamic_cast<const CNativeEvaluatorProxy*>(current))
    {
        const Registration* other = proxy->registration();
        return xr_string("the proxy of native evaluator #") + std::to_string(other ? other->id : 0).c_str() +
            " holds the property";
    }
    if (result == EInstall::Engine || !dynamic_cast<const CScriptPropertyEvaluatorWrapper*>(current))
    {
        // A constant of a scheme or the evaluator of the engine no scheme replaced
        return xr_string("a non-Lua evaluator is there (") + typeid(*current).name() + ")";
    }
    return xr_string("the Lua evaluator '") + (current->m_evaluator_name ? current->m_evaluator_name : "") +
        "' holds the property, not '" + registration.lua_name + "'";
}

// The same for an action registration.
xr_string DescribeActionSkip(const ActionRegistration& registration, CAI_Stalker& stalker, EInstall result)
{
    CScriptActionPlanner* planner = PlannerOf(stalker, registration.planner);
    if (!planner)
        return "the planner is not there";
    CScriptActionBase* current = ActionOf(*planner, registration.action_id);
    if (!current)
        return "the action id is not in the planner";
    if (const auto* proxy = dynamic_cast<const CNativeActionProxy*>(current))
    {
        const ActionRegistration* other = proxy->registration();
        return xr_string("the proxy of native action #") + std::to_string(other ? other->id : 0).c_str() +
            " holds the action";
    }
    if (result == EInstall::Engine || !dynamic_cast<const CScriptActionWrapper*>(current))
        return xr_string("a non-Lua action is there (") + typeid(*current).name() + ")";
    return xr_string("the Lua action '") + (current->m_action_name ? current->m_action_name : "") +
        "' holds the id, not '" + registration.lua_name + "'";
}

// A dead registration: its proxies leave the planners, the Lua evaluators go back in their places.
void Restore(Registration& registration)
{
    while (!registration.proxies.empty())
    {
        CNativeEvaluatorProxy* proxy = registration.proxies.back();
        CScriptActionPlanner* planner = proxy->planner();
        CScriptPropertyEvaluator* lua = proxy->ReleaseLua(); // unlinks the proxy
        CScriptPropertyEvaluator* previous = planner->swap_evaluator(registration.property, lua);
        R_ASSERT(previous == proxy);
        xr_delete(proxy);
    }
}

// A dead action registration: the proxies give the actions back to Lua, the conditions they collected included.
// A proxy in the middle of a native run hands the run over first: the Lua action gets its initialize, so its next
// execute (which is already addressed to the Lua object from that moment on) works on an initialized action.
void RestoreAction(ActionRegistration& registration)
{
    while (!registration.proxies.empty())
    {
        CNativeActionProxy* proxy = registration.proxies.back();
        CScriptActionPlanner* planner = proxy->planner();
        CScriptActionWrapper* lua = proxy->ReleaseLua(); // unlinks the proxy
        // A run is handed over only while it is the current one of a living NPC: a planner set up again (reinit)
        // or a corpse left the native run without its finalize, and the Lua initialize there would start a scheme
        // that is not running
        const bool mid_run = proxy->mid_native_run() && planner->initialized() &&
            planner->current_action_id() == registration.action_id && planner->object().Alive();
        // The Lua action kept the list it had at the install; ours is the live one (a Lua add_precondition after
        // the install reached the proxy). Drop the stale one, then give it ours.
        while (!lua->conditions().conditions().empty())
            lua->remove_condition(lua->conditions().conditions().front().condition());
        while (!lua->effects().conditions().empty())
            lua->remove_effect(lua->effects().conditions().front().condition());
        for (const auto& condition : proxy->conditions().conditions())
            lua->add_condition(condition);
        for (const auto& effect : proxy->effects().conditions())
            lua->add_effect(effect);
        // The Lua action is back in its slot before its initialize: a script that looks at the planner from it
        // (current_action, action(id)) finds itself, not the proxy about to be deleted
        CScriptActionBase* previous = planner->swap_operator(registration.action_id, lua);
        R_ASSERT(previous == proxy);
        if (mid_run)
        {
            ++registration.handovers;
            lua->initialize(); // the transfer of a run in flight, as the proxy would do at its next execute
        }
        xr_delete(proxy);
    }
}

// A queued stalker found dead at its settling: nothing is installed into a corpse (the schemes of the living never
// come back to it, its planner never runs again). One that died in the queue is counted as the description of
// `skipped` says, for each registration not in place (a missing slot goes to `absent`, as for the living), with one
// line, not one per registration; a corpse from the start (a save holds many) is no skip of a living NPC
void SettleDead(const Pending& pending, CAI_Stalker& stalker)
{
    if (pending.alive && g_registrations)
    {
        for (Registration* registration : *g_registrations)
        {
            if (registration->dead)
                continue;
            const EInstall result = Install(*registration, stalker, false);
            if (result == EInstall::Missing)
                ++registration->absent;
            else if (result != EInstall::Done)
                ++registration->skipped;
        }
    }
    if (pending.alive && g_action_registrations)
    {
        for (ActionRegistration* registration : *g_action_registrations)
        {
            if (registration->dead)
                continue;
            const EInstall result = InstallAction(*registration, stalker, false);
            if (result == EInstall::Missing)
                ++registration->absent;
            else if (result != EInstall::Done)
                ++registration->skipped;
        }
    }
    if (IsDebugLog())
    {
        Msg("~ [goap] '%s' (%u) is dead%s: native evaluators and actions not installed", stalker.cName().c_str(),
            stalker.ID(), pending.alive ? " (died in the queue, counted as skipped)" : "");
    }
}

// The queued entry of the stalker, or nullptr: the queue holds one entry per stalker
Pending* FindPending(u16 id)
{
    if (!g_pending)
        return nullptr;
    for (Pending& pending : *g_pending)
    {
        if (pending.id == id)
            return &pending;
    }
    return nullptr;
}

// Queues the stalker, or brings its entry back to the start: every registration is tried again at the next
// ApplyPending (done bits cleared, no wait for the next step) for kRetryMs from now. `spawn` only adds to an entry:
// a requeue after a change of the logic does not take the counting away from an entry of the spawn.
void Queue(u16 id, bool spawn, bool alive)
{
    if (!g_pending)
        g_pending = xr_new<xr_vector<Pending>>();
    const u32 until = Device.dwTimeGlobal + kRetryMs;
    if (Pending* pending = FindPending(id))
    {
        pending->until_ms = std::max(pending->until_ms, until);
        pending->next_ms = 0;
        pending->done = 0;
        pending->done_actions = 0;
        if (spawn && !pending->spawn)
        {
            pending->spawn = true;
            pending->alive = alive; // a requeue entry knew nothing of it
        }
        return;
    }
    Pending pending{ id, until, spawn };
    pending.alive = alive;
    g_pending->push_back(pending);
}

void QueueOnlineStalkers()
{
    if (!g_pGameLevel)
        return;
    const u32 count = Level().Objects.o_count();
    for (u32 i = 0; i < count; ++i)
    {
        const IGameObject* object = Level().Objects.o_get_by_iterator(i);
        const auto* stalker = object && !object->getDestroy() ? smart_cast<const CAI_Stalker*>(object) : nullptr;
        // A stalker queued already (its spawn, the previous registration) keeps its one entry: R registrations
        // made in a row would otherwise queue each of S stalkers R times
        if (stalker)
            Queue(stalker->ID(), true, !!stalker->g_Alive());
    }
}

void DisablePlugin(const GwpPlugin* plugin)
{
    if (!g_registrations)
        return;
    for (Registration* registration : *g_registrations)
    {
        if (registration->plugin != plugin || registration->dead)
            continue;
        registration->dead = true; // proxies call Lua from now on; the planners are fixed at the next ApplyPending
        registration->fn = nullptr;
        g_restore_pending = true;
    }
    if (g_action_registrations)
    {
        for (ActionRegistration* registration : *g_action_registrations)
        {
            if (registration->plugin != plugin || registration->dead)
                continue;
            // proxies hand their native runs over from now on; the planners are fixed at the next ApplyPending
            registration->dead = true;
            registration->vtable = GwpActionVTable{};
            g_restore_pending = true;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group goap)
// ---------------------------------------------------------------------------------------------

GwpEvaluatorId GWP_CALL ApiEvaluatorRegister(const GwpPlugin* self, uint32_t planner, uint32_t property_id,
    const char* lua_name, GwpEvaluatorFn fn, void* user, uint32_t mode)
{
    if (!api::CheckPluginMutation(self, "goap", "evaluator_register"))
        return GWP_INVALID_EVALUATOR_ID;
    const pcstr addon = PluginAddonId(self);
    // The high 16 bits of `mode` are flags (GWP_MODE_FLAGS_MASK): none is defined yet, so any is refused
    if (!fn || (mode & GWP_MODE_FLAGS_MASK) || (mode & GWP_MODE_MASK) > GWP_EVALUATOR_SHADOW)
    {
        Msg("! [plugin:%s] evaluator_register: %s (mode 0x%08x)", addon,
            !fn ? "no function" : (mode & GWP_MODE_FLAGS_MASK) ? "a flag bit in mode" : "unknown mode", mode);
        return GWP_INVALID_EVALUATOR_ID;
    }
    for (const Registration* other : Registrations())
    {
        if (!other->dead && other->planner == planner && other->property == property_id)
        {
            Msg("! [plugin:%s] evaluator_register: property %u is taken by addon '%s'", addon, property_id,
                PluginAddonId(other->plugin));
            return GWP_INVALID_EVALUATOR_ID;
        }
    }
    auto* registration = xr_new<Registration>();
    registration->id = g_next_id++;
    registration->plugin = self;
    registration->planner = planner;
    registration->property = property_id;
    if (lua_name)
        registration->lua_name = lua_name;
    registration->profile_name = (lua_name && lua_name[0] ? xr_string(lua_name) : xr_string("property")) + "#native";
    registration->fn = fn;
    registration->user = user;
    registration->mode = mode;
    Registrations().push_back(registration);
    ++g_registrations_serial;
    QueueOnlineStalkers();
    if (IsDebugLog())
    {
        Msg("* [plugin:%s] native evaluator #%u registered: property %u, planner %s, lua '%s', mode %u", addon,
            registration->id, property_id, planner == GWP_PLANNER_MAIN ? "main" : "subplanner",
            lua_name ? lua_name : "*", mode);
    }
    return registration->id;
}

GwpResult GWP_CALL ApiEvaluatorUnregister(const GwpPlugin* self, GwpEvaluatorId id)
{
    if (!api::CheckPluginMutation(self, "goap", "evaluator_unregister"))
        return api::PluginCallRefused(self);
    Registration* registration = FindRegistration(id);
    if (!registration || registration->plugin != self) // no such registration of this plugin
        return GWP_ERROR_NOT_FOUND;
    registration->dead = true;
    registration->fn = nullptr;
    g_restore_pending = true;
    return GWP_OK;
}

GwpResult GWP_CALL ApiEvaluatorSetMode(const GwpPlugin* self, GwpEvaluatorId id, uint32_t mode)
{
    if (!api::CheckPluginMutation(self, "goap", "evaluator_set_mode"))
        return api::PluginCallRefused(self);
    Registration* registration = FindRegistration(id);
    if (!registration || registration->plugin != self) // no such registration of this plugin
        return GWP_ERROR_NOT_FOUND;
    // A flag bit (none defined yet) or a mode above the last GWP_EVALUATOR_*: a value of a later version
    if ((mode & GWP_MODE_FLAGS_MASK) || mode > GWP_EVALUATOR_SHADOW)
        return api::PluginRefusal(self, "evaluator_set_mode", GWP_ERROR_NOT_SUPPORTED, "unknown mode or a flag bit");
    if (registration->mode != mode && IsDebugLog())
        Msg("* [plugin:%s] native evaluator #%u: mode %u -> %u", PluginAddonId(self), id, registration->mode, mode);
    registration->mode = mode;
    return GWP_OK;
}

int GWP_CALL ApiEvaluatorStats(GwpEvaluatorId id, GwpEvaluatorStats* out)
{
    if (!CheckCall("evaluator_stats") || !out || out->size < 2 * sizeof(uint32_t))
        return 0;
    const Registration* registration = FindRegistration(id);
    if (!registration)
        return 0;
    GwpEvaluatorStats stats{};
    stats.size = sizeof(stats);
    stats.mode = registration->mode;
    stats.installed = static_cast<uint32_t>(registration->proxies.size());
    stats.skipped = registration->skipped;
    stats.native_calls = registration->native_calls;
    stats.lua_calls = registration->lua_calls;
    stats.mismatches = registration->mismatches;
    const uint32_t size = std::min<uint32_t>(out->size, sizeof(stats));
    memcpy(out, &stats, size);
    out->size = size;
    return 1;
}

GwpActionRegId GWP_CALL ApiActionRegister(const GwpPlugin* self, uint32_t planner, uint32_t action_id,
    const char* lua_name, const GwpActionVTable* vtable, void* user, uint32_t mode)
{
    if (!api::CheckPluginMutation(self, "goap", "action_register"))
        return GWP_INVALID_ACTION_REG_ID;
    const pcstr addon = PluginAddonId(self);
    // The size covers `reserved` here (the header of the table is required), so a non-zero one is refused too.
    // The high 16 bits of `mode` are flags (GWP_MODE_FLAGS_MASK): none is defined yet, so any is refused.
    if (!vtable || vtable->size < 2 * sizeof(uint32_t) || vtable->reserved != 0 || (mode & GWP_MODE_FLAGS_MASK) ||
        (mode & GWP_MODE_MASK) > GWP_ACTION_VERIFY || action_id == GWP_INVALID_ACTION_ID)
    {
        Msg("! [plugin:%s] action_register: %s (mode 0x%08x)", addon,
            !vtable ? "no vtable" :
                vtable->size < 2 * sizeof(uint32_t) ? "vtable size below the header" :
                vtable->reserved != 0 ? "non-zero vtable reserved field" :
                (mode & GWP_MODE_FLAGS_MASK) ? "a flag bit in mode" :
                (mode & GWP_MODE_MASK) > GWP_ACTION_VERIFY ? "unknown mode" : "no action id", mode);
        return GWP_INVALID_ACTION_REG_ID;
    }
    for (const ActionRegistration* other : ActionRegistrations())
    {
        if (!other->dead && other->planner == planner && other->action_id == action_id)
        {
            Msg("! [plugin:%s] action_register: action id %u is taken by addon '%s'", addon, action_id,
                PluginAddonId(other->plugin));
            return GWP_INVALID_ACTION_REG_ID;
        }
    }
    auto* registration = xr_new<ActionRegistration>();
    registration->id = g_next_action_id++;
    registration->plugin = self;
    registration->planner = planner;
    registration->action_id = action_id;
    if (lua_name)
        registration->lua_name = lua_name;
    registration->profile_name =
        (lua_name && lua_name[0] ? xr_string(lua_name) : xr_string("action")) + "#native";
    memcpy(&registration->vtable, vtable, std::min<size_t>(vtable->size, sizeof(registration->vtable)));
    registration->user = user;
    registration->mode = mode;
    ActionRegistrations().push_back(registration);
    ++g_registrations_serial;
    QueueOnlineStalkers();
    if (IsDebugLog())
    {
        Msg("* [plugin:%s] native action #%u registered: id %u, planner %s, lua '%s', mode %u", addon,
            registration->id, action_id, planner == GWP_PLANNER_MAIN ? "main" : "subplanner",
            lua_name ? lua_name : "*", mode);
    }
    return registration->id;
}

GwpResult GWP_CALL ApiActionUnregister(const GwpPlugin* self, GwpActionRegId id)
{
    if (!api::CheckPluginMutation(self, "goap", "action_unregister"))
        return api::PluginCallRefused(self);
    ActionRegistration* registration = FindActionRegistration(id);
    if (!registration || registration->plugin != self) // no such registration of this plugin
        return GWP_ERROR_NOT_FOUND;
    registration->dead = true;
    registration->vtable = GwpActionVTable{};
    g_restore_pending = true;
    return GWP_OK;
}

GwpResult GWP_CALL ApiActionSetMode(const GwpPlugin* self, GwpActionRegId id, uint32_t mode)
{
    if (!api::CheckPluginMutation(self, "goap", "action_set_mode"))
        return api::PluginCallRefused(self);
    ActionRegistration* registration = FindActionRegistration(id);
    if (!registration || registration->plugin != self) // no such registration of this plugin
        return GWP_ERROR_NOT_FOUND;
    // A flag bit (none defined yet) or a mode above the last GWP_ACTION_*: a value of a later version
    if ((mode & GWP_MODE_FLAGS_MASK) || mode > GWP_ACTION_VERIFY)
        return api::PluginRefusal(self, "action_set_mode", GWP_ERROR_NOT_SUPPORTED, "unknown mode or a flag bit");
    if (registration->mode != mode && IsDebugLog())
        Msg("* [plugin:%s] native action #%u: mode %u -> %u (from the next run of each action)", PluginAddonId(self),
            id, registration->mode, mode);
    registration->mode = mode;
    return GWP_OK;
}

int GWP_CALL ApiActionStats(GwpActionRegId id, GwpActionStats* out)
{
    if (!CheckCall("action_stats") || !out || out->size < 2 * sizeof(uint32_t))
        return 0;
    const ActionRegistration* registration = FindActionRegistration(id);
    if (!registration)
        return 0;
    GwpActionStats stats{};
    stats.size = sizeof(stats);
    stats.mode = registration->mode;
    stats.installed = static_cast<uint32_t>(registration->proxies.size());
    stats.skipped = registration->skipped;
    stats.native_steps = registration->native_steps;
    stats.lua_steps = registration->lua_steps;
    stats.handovers = registration->handovers;
    // The runs in flight right now, by their mode fixed at the initialize
    for (const CNativeActionProxy* proxy : registration->proxies)
    {
        if (proxy->running_mode() == GWP_ACTION_NATIVE)
            ++stats.running_native;
        else if (proxy->running_mode() == GWP_ACTION_VERIFY)
            ++stats.running_verify;
    }
    const uint32_t size = std::min<uint32_t>(out->size, sizeof(stats));
    memcpy(out, &stats, size);
    out->size = size;
    return 1;
}

int32_t GWP_CALL ApiNpcActionRunningMode(GwpObjectId npc, uint32_t planner, uint32_t action_id)
{
    if (!CheckCall("npc_action_running_mode"))
        return -1;
    if (!g_pGameLevel)
        return -1;
    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(npc));
    if (!stalker || stalker->getDestroy())
        return -1;
    CScriptActionPlanner* brain = PlannerOf(*stalker, planner);
    if (!brain)
        return -1;
    CScriptActionBase* action = ActionOf(*brain, action_id);
    if (!action)
        return -1;
    if (const auto* proxy = dynamic_cast<const CNativeActionProxy*>(action))
        return static_cast<int32_t>(proxy->running_mode());
    return GWP_ACTION_LUA; // the plain Lua action of this slot runs in Lua
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.evaluator_register = &ApiEvaluatorRegister;
    api.evaluator_unregister = &ApiEvaluatorUnregister;
    api.evaluator_set_mode = &ApiEvaluatorSetMode;
    api.evaluator_stats = &ApiEvaluatorStats;
    api.action_register = &ApiActionRegister;
    api.action_unregister = &ApiActionUnregister;
    api.action_set_mode = &ApiActionSetMode;
    api.action_stats = &ApiActionStats;
    api.npc_action_running_mode = &ApiNpcActionRunningMode;
}

void OnObjectSpawn(const CGameObject* object)
{
    const bool none = (!g_registrations || g_registrations->empty()) && (!g_action_registrations ||
        g_action_registrations->empty());
    const auto* stalker = none || !object ? nullptr : smart_cast<const CAI_Stalker*>(object);
    if (!stalker)
        return;
    Queue(stalker->ID(), true, !!stalker->g_Alive()); // CEntity::net_Spawn has set the health by now
}

void ApplyPending()
{
    if (g_restore_pending && (g_registrations || g_action_registrations))
    {
        g_restore_pending = false;
        // The dead ones leave the lists before any restore: the Lua initialize of a handover may reach a plugin
        // that registers again, and a push_back into a list walked here would invalidate the walk
        xr_vector<Registration*> dead;
        xr_vector<ActionRegistration*> dead_actions;
        if (g_registrations)
        {
            for (auto it = g_registrations->begin(); it != g_registrations->end();)
            {
                if (!(*it)->dead)
                {
                    ++it;
                    continue;
                }
                dead.push_back(*it);
                it = g_registrations->erase(it);
                ++g_registrations_serial;
            }
        }
        if (g_action_registrations)
        {
            for (auto it = g_action_registrations->begin(); it != g_action_registrations->end();)
            {
                if (!(*it)->dead)
                {
                    ++it;
                    continue;
                }
                dead_actions.push_back(*it);
                it = g_action_registrations->erase(it);
                ++g_registrations_serial;
            }
        }
        for (Registration* registration : dead)
        {
            Restore(*registration);
            xr_delete(registration);
        }
        for (ActionRegistration* registration : dead_actions)
        {
            RestoreAction(*registration);
            xr_delete(registration);
        }
    }

    const bool no_evaluators = !g_registrations || g_registrations->empty();
    const bool no_actions = !g_action_registrations || g_action_registrations->empty();
    if (!g_pending || g_pending->empty() || !g_pGameLevel || (no_evaluators && no_actions))
        return;
    ZoneScopedN("goap/ApplyPending"); // Tracy: the queue of stalkers the native evaluators are put into
    ZoneTextF("%u queued", static_cast<u32>(g_pending->size()));
    const u32 now = Device.dwTimeGlobal;
    xr_vector<Pending> retry;
    xr_vector<EInstall> results;
    for (const Pending& queued : *g_pending)
    {
        CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(queued.id));
        if (!stalker || stalker->getDestroy())
            continue;
        if (now < queued.next_ms)
        {
            retry.push_back(queued);
            continue;
        }
        Pending pending = queued;
        if (pending.serial != g_registrations_serial)
        {
            pending.serial = g_registrations_serial;
            pending.done = 0;
            pending.done_actions = 0;
        }
        // A corpse keeps the evaluators it has: no Install for it, only the counting of a spawn entry
        if (!stalker->g_Alive())
        {
            if (pending.spawn)
                SettleDead(pending, *stalker);
            continue;
        }
        bool waiting = false;
        results.clear();
        for (size_t i = 0; i < (no_evaluators ? 0 : g_registrations->size()); ++i)
        {
            Registration* registration = (*g_registrations)[i];
            const u64 bit = i < 64 ? u64(1) << i : 0;
            EInstall result = EInstall::Done;
            if (!registration->dead && !(pending.done & bit))
            {
                result = Install(*registration, *stalker);
                if (result == EInstall::Done)
                    pending.done |= bit;
            }
            results.push_back(result);
            if ((result == EInstall::Missing || result == EInstall::Engine) && now < pending.until_ms)
                waiting = true;
        }
        const size_t evaluator_results = results.size();
        for (size_t i = 0; i < (no_actions ? 0 : g_action_registrations->size()); ++i)
        {
            ActionRegistration* registration = (*g_action_registrations)[i];
            const u64 bit = i < 64 ? u64(1) << i : 0;
            EInstall result = EInstall::Done;
            if (!registration->dead && !(pending.done_actions & bit))
            {
                result = InstallAction(*registration, *stalker);
                if (result == EInstall::Done)
                    pending.done_actions |= bit;
            }
            results.push_back(result);
            if ((result == EInstall::Missing || result == EInstall::Engine) && now < pending.until_ms)
                waiting = true;
        }
        if (waiting)
        {
            pending.next_ms = now + kRetryStepMs;
            retry.push_back(pending);
            continue;
        }
        if (!pending.spawn)
            continue; // a requeue after a change of the logic: what did not come now comes with the next change
        // Settled: a registration not in place now stays on the old evaluator for this stalker until its logic
        // changes. Counted once per stalker here, not on every retry while another property was still awaited.
        // A missing property is the normal case of a scheme the NPC has not got (camper, patrol, walker, ...):
        // counted apart and not logged one by one; only an NPC without any of them is worth a line.
        bool any_present = false;
        for (const EInstall result : results)
            any_present = any_present || result != EInstall::Missing;
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (results[i] == EInstall::Done)
                continue;
            if (i < evaluator_results)
            {
                Registration& registration = *(*g_registrations)[i];
                if (results[i] == EInstall::Missing)
                {
                    ++registration.absent;
                    continue;
                }
                ++registration.skipped;
                if (IsDebugLog())
                {
                    Msg("~ [goap] native evaluator #%u '%s' (property %u) skipped for '%s' (%u): %s",
                        registration.id, registration.lua_name.c_str(), registration.property,
                        stalker->cName().c_str(), stalker->ID(),
                        DescribeSkip(registration, *stalker, results[i]).c_str());
                }
            }
            else
            {
                ActionRegistration& registration = *(*g_action_registrations)[i - evaluator_results];
                if (results[i] == EInstall::Missing)
                {
                    ++registration.absent;
                    continue;
                }
                ++registration.skipped;
                if (IsDebugLog())
                {
                    Msg("~ [goap] native action #%u '%s' (id %u) skipped for '%s' (%u): %s", registration.id,
                        registration.lua_name.c_str(), registration.action_id, stalker->cName().c_str(),
                        stalker->ID(), DescribeActionSkip(registration, *stalker, results[i]).c_str());
                }
            }
        }
        if (!any_present && IsDebugLog())
        {
            Msg("~ [goap] '%s' (%u): none of the native evaluators found its property in 10 s (no logic yet?); "
                "tried again when its active_section changes", stalker->cName().c_str(), stalker->ID());
        }
    }
    // One entry per stalker (Queue merges the spawn, a new registration and a change of the logic into it): a skip
    // is counted once per settling.
    g_pending->swap(retry);
}

void OnScriptRestart()
{
    // A new Lua state: the objects of the level are gone or about to be, and their ids come back for others.
    // A pending entry of the old level would count a stalker of the new one as skipped
    if (g_pending)
        g_pending->clear();
}

void OnLogicChanged(u16 id)
{
    const bool none = (!g_registrations || g_registrations->empty()) && (!g_action_registrations ||
        g_action_registrations->empty());
    if (none || !g_pGameLevel)
        return;
    // Already queued: not only a longer wait. A registration marked done in the old section may have lost its
    // place with the switch (the new scheme put its own Lua evaluator there), so everything is tried again now
    Queue(id, false, true);
}

void RemovePluginEvaluators(const GwpPlugin* plugin)
{
    DisablePlugin(plugin);
    // The library is about to go: nothing may call it any more, and the fn pointers are cleared above. The planners
    // are fixed at the next ApplyPending, or never when the level is gone already (the proxies call Lua only).
}

void Shutdown()
{
    if (g_registrations)
    {
        for (Registration* registration : *g_registrations)
        {
            // Proxies still in planners (a level alive at exit): they outlive the registry and must not touch it.
            for (CNativeEvaluatorProxy* proxy : registration->proxies)
                proxy->ForgetRegistration();
            registration->proxies.clear();
            xr_delete(registration);
        }
    }
    xr_delete(g_registrations);
    if (g_action_registrations)
    {
        for (ActionRegistration* registration : *g_action_registrations)
        {
            for (CNativeActionProxy* proxy : registration->proxies)
                proxy->ForgetRegistration(); // the same: a proxy left in a planner counts its steps nowhere
            registration->proxies.clear();
            xr_delete(registration);
        }
    }
    xr_delete(g_action_registrations);
    xr_delete(g_pending);
    g_restore_pending = false;
}

void PrintList()
{
    const u32 count = g_registrations ? static_cast<u32>(g_registrations->size()) : 0;
    Msg("- [goap] %u native evaluator(s):", count);
    if (!g_registrations)
        return;
    static constexpr pcstr kModes[] = { "lua", "native", "shadow" };
    for (const Registration* registration : *g_registrations)
    {
        Msg("-   #%-4u %-24s property=%-6u lua=%-24s mode=%-6s installed=%-4u skipped=%-4u absent=%-4u native=%llu "
            "lua_calls=%llu mismatches=%llu%s",
            registration->id, PluginAddonId(registration->plugin), registration->property,
            registration->lua_name.empty() ? "*" : registration->lua_name.c_str(), kModes[registration->mode],
            static_cast<u32>(registration->proxies.size()), registration->skipped, registration->absent,
            static_cast<unsigned long long>(registration->native_calls),
            static_cast<unsigned long long>(registration->lua_calls),
            static_cast<unsigned long long>(registration->mismatches), registration->dead ? " (removed)" : "");
    }
    const u32 actions = g_action_registrations ? static_cast<u32>(g_action_registrations->size()) : 0;
    Msg("- [goap] %u native action(s):", actions);
    if (!g_action_registrations)
        return;
    static constexpr pcstr kActionModes[] = { "lua", "native", "verify" };
    for (const ActionRegistration* registration : *g_action_registrations)
    {
        u32 running_native = 0, running_verify = 0;
        for (const CNativeActionProxy* proxy : registration->proxies)
        {
            if (proxy->running_mode() == GWP_ACTION_NATIVE)
                ++running_native;
            else if (proxy->running_mode() == GWP_ACTION_VERIFY)
                ++running_verify;
        }
        Msg("-   #%-4u %-24s planner=%-4u action=%-6u lua=%-28s mode=%-6s installed=%-4u skipped=%-4u absent=%-4u "
            "native=%llu lua_steps=%llu handovers=%llu running=%u/%u%s",
            registration->id, PluginAddonId(registration->plugin), registration->planner, registration->action_id,
            registration->lua_name.empty() ? "*" : registration->lua_name.c_str(),
            kActionModes[registration->mode], static_cast<u32>(registration->proxies.size()), registration->skipped,
            registration->absent, static_cast<unsigned long long>(registration->native_steps),
            static_cast<unsigned long long>(registration->lua_steps),
            static_cast<unsigned long long>(registration->handovers), running_native, running_verify,
            registration->dead ? " (removed)" : "");
    }
}
} // namespace gw::addons::goap
