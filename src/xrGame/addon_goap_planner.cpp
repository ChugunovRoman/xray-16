#include "StdAfx.h"

#include "addon_goap_planner.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "action_base.h"
#include "action_planner.h"
#include "GameObject.h"
#include "Level.h"
#include "property_evaluator.h"
#include "script_game_object.h"

namespace gw::addons::goap
{
namespace
{
constexpr u32 kMaxPlanners = 65535; // a handle is (generation of the slot << 16) | (index + 1)

struct Planner;

// The owner of a callback: every native call goes through the guard of the event bus; a crash disables the
// planner (its callbacks are never called again) and marks the plugin: planner_create refuses it from then on, its
// other planners stay disabled until their objects go, the plugin is unloaded or it destroys them itself.
struct Callbacks
{
    Planner* planner = nullptr;
};

class CNativePlannerEvaluator final : public CScriptPropertyEvaluator
{
public:
    CNativePlannerEvaluator(Planner* planner, GwpEvaluatorFn fn, void* user, pcstr name)
        : CScriptPropertyEvaluator(nullptr, ""), m_planner(planner), m_fn(fn), m_user(user), m_name(name)
    {
        m_evaluator_name = m_name.c_str();
    }
    _value_type evaluate() override;

private:
    Planner* m_planner;
    GwpEvaluatorFn m_fn;
    void* m_user;
    shared_str m_name;
};

class CNativePlannerAction final : public CScriptActionBase
{
public:
    CNativePlannerAction(Planner* planner, const GwpActionVTable& vtable, void* user, pcstr name)
        : CScriptActionBase(nullptr, ""), m_planner(planner), m_vtable(vtable), m_user(user), m_name(name)
    {
        m_action_name = m_name.c_str();
    }
    void initialize() override;
    void execute() override;
    void finalize() override;

private:
    enum class EStep
    {
        Initialize,
        Execute,
        Finalize,
    };
    void Call(EStep step);

    Planner* m_planner;
    GwpActionVTable m_vtable;
    void* m_user;
    shared_str m_name;
};

struct Planner
{
    GwpPlannerId id = GWP_INVALID_PLANNER_ID;
    const GwpPlugin* plugin = nullptr;
    u16 npc = u16(-1);
    CScriptActionPlanner* planner = nullptr;
    bool disabled = false; // a callback crashed: nothing is called any more
    bool updating = false; // inside planner_update: a nested update or destroy is refused
    bool doomed = false;   // its object went offline during its own update: destroyed when the update returns
};

xr_vector<Planner*>* g_planners = nullptr; // index = (id & 0xFFFF) - 1; nullptr = free
// The generation of each slot (parallel to g_planners), the high half of the id of its planner: bumped at every
// planner_create in the slot, so a stale id of a destroyed planner does not name the next one of the same slot.
// One counter for all slots would wrap after 65535 creations anywhere, and an old id of a slot reused just then
// would name the new planner; per slot it takes 65535 creations in that very slot.
xr_vector<u16>* g_generations = nullptr;

u32 IndexOf(GwpPlannerId id) { return (id & 0xFFFFu) - 1; }
xr_vector<const GwpPlugin*>* g_dead_plugins = nullptr;

bool CheckPlannerCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [goap] %s called outside the main thread, ignored", function);
    return false;
}

Planner* Find(GwpPlannerId id, const GwpPlugin* self)
{
    if (!g_planners || id == GWP_INVALID_PLANNER_ID || (id & 0xFFFFu) == 0 || IndexOf(id) >= g_planners->size())
        return nullptr;
    Planner* planner = (*g_planners)[IndexOf(id)];
    return planner && planner->id == id && (!self || planner->plugin == self) ? planner : nullptr;
}

void StopPlannersOf(const GwpPlugin* plugin)
{
    if (!g_dead_plugins)
        g_dead_plugins = xr_new<xr_vector<const GwpPlugin*>>();
    if (std::find(g_dead_plugins->begin(), g_dead_plugins->end(), plugin) == g_dead_plugins->end())
        g_dead_plugins->push_back(plugin);
    if (!g_planners)
        return;
    for (Planner* other : *g_planners)
    {
        if (other && other->plugin == plugin)
            other->disabled = true;
    }
}

void Disable(Planner& planner, pcstr what)
{
    if (planner.disabled)
        return;
    const GwpPlugin* const plugin = planner.plugin;
    string128 where;
    xr_sprintf(where, "the %s of its planner #%u", what, planner.id);
    StopPlannersOf(plugin);
    OnPluginCrashed(plugin, where); // the whole plugin stops
}

void Destroy(u32 index)
{
    Planner* planner = (*g_planners)[index];
    if (!planner)
        return;
    (*g_planners)[index] = nullptr;
    // clear() finalizes nothing and deletes the evaluators and actions (they are ours)
    xr_delete(planner->planner);
    xr_delete(planner);
}

struct EvaluatorCall
{
    GwpEvaluatorFn fn;
    void* user;
    u16 npc;
    int result;
};

void EvaluatorThunk(void* context)
{
    EvaluatorCall& call = *static_cast<EvaluatorCall*>(context);
    call.result = call.fn(call.user, call.npc);
}

CNativePlannerEvaluator::_value_type CNativePlannerEvaluator::evaluate()
{
    if (m_planner->disabled)
        return false;
    EvaluatorCall call{ m_fn, m_user, m_planner->npc, 0 };
    if (!events::CallPluginGuarded(reinterpret_cast<const void*>(m_fn), "planner evaluator", &EvaluatorThunk, &call))
    {
        Disable(*m_planner, "evaluator");
        return false;
    }
    return call.result != 0;
}

struct ActionCall
{
    void(GWP_CALL* fn)(void*, GwpObjectId);
    void* user;
    u16 npc;
};

void ActionThunk(void* context)
{
    const ActionCall& call = *static_cast<const ActionCall*>(context);
    call.fn(call.user, call.npc);
}

void CNativePlannerAction::Call(EStep step)
{
    if (m_planner->disabled)
        return;
    void(GWP_CALL* fn)(void*, GwpObjectId) = step == EStep::Initialize ? m_vtable.initialize
        : step == EStep::Execute                                         ? m_vtable.execute
                                                                         : m_vtable.finalize;
    if (!fn)
        return;
    ActionCall call{ fn, m_user, m_planner->npc };
    if (!events::CallPluginGuarded(reinterpret_cast<const void*>(fn), "planner action", &ActionThunk, &call))
        Disable(*m_planner, "action");
}

// The base class keeps the timing of the action (start time, first_time): it runs first, as in a Lua action that
// calls action_base.initialize(self) first.
void CNativePlannerAction::initialize()
{
    CScriptActionBase::initialize();
    Call(EStep::Initialize);
}

void CNativePlannerAction::execute()
{
    CScriptActionBase::execute();
    Call(EStep::Execute);
}

void CNativePlannerAction::finalize()
{
    CScriptActionBase::finalize();
    Call(EStep::Finalize);
}

// ---------------------------------------------------------------------------------------------
// Plugin API
// ---------------------------------------------------------------------------------------------

GwpPlannerId GWP_CALL ApiPlannerCreate(const GwpPlugin* self, GwpObjectId npc)
{
    if (!CheckPlannerCall("planner_create") || !self || !g_pGameLevel)
        return GWP_INVALID_PLANNER_ID;
    if (g_dead_plugins && std::find(g_dead_plugins->begin(), g_dead_plugins->end(), self) != g_dead_plugins->end())
        return GWP_INVALID_PLANNER_ID;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(npc));
    if (!object || object->getDestroy())
        return GWP_INVALID_PLANNER_ID;
    if (!g_planners)
        g_planners = xr_new<xr_vector<Planner*>>();
    if (!g_generations)
        g_generations = xr_new<xr_vector<u16>>();

    u32 index = 0;
    while (index < g_planners->size() && (*g_planners)[index])
        ++index;
    if (index >= kMaxPlanners)
    {
        Msg("! [plugin:%s] planner_create: more than %u planners", PluginAddonId(self), kMaxPlanners);
        return GWP_INVALID_PLANNER_ID;
    }
    if (index == g_planners->size())
        g_planners->push_back(nullptr);
    if (index >= g_generations->size())
        g_generations->resize(index + 1, 0);

    auto* planner = xr_new<Planner>();
    u16& generation = (*g_generations)[index];
    if (++generation == 0)
        generation = 1;
    planner->id = (static_cast<u32>(generation) << 16) | (index + 1);
    planner->plugin = self;
    planner->npc = npc;
    planner->planner = xr_new<CScriptActionPlanner>();
    planner->planner->setup(object->lua_game_object());
    (*g_planners)[index] = planner;
    return planner->id;
}

GwpResult GWP_CALL ApiPlannerDestroy(const GwpPlugin* self, GwpPlannerId id)
{
    if (!CheckPlannerCall("planner_destroy"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Planner* planner = Find(id, self);
    if (!planner || planner->updating)
        return GWP_ERROR_INVALID_ARGUMENT;
    Destroy(IndexOf(id));
    return GWP_OK;
}

GwpResult GWP_CALL ApiPlannerAddEvaluator(
    const GwpPlugin* self, GwpPlannerId id, uint32_t property, const char* name, GwpEvaluatorFn fn, void* user)
{
    if (!CheckPlannerCall("planner_add_evaluator"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Planner* planner = Find(id, self);
    if (!planner || planner->updating || !fn)
        return GWP_ERROR_INVALID_ARGUMENT;
    const auto& evaluators = planner->planner->evaluators();
    if (evaluators.find(property) != evaluators.end()) // THROW in the engine: a no-op in Release Master Gold
        return GWP_ERROR_INVALID_ARGUMENT;
    planner->planner->add_evaluator(property, xr_new<CNativePlannerEvaluator>(planner, fn, user, name ? name : ""));
    return GWP_OK;
}

GwpResult GWP_CALL ApiPlannerAddAction(const GwpPlugin* self, GwpPlannerId id, uint32_t action_id, const char* name,
    const GwpActionVTable* vtable, void* user, const GwpWorldProperty* conditions, uint32_t condition_count,
    const GwpWorldProperty* effects, uint32_t effect_count)
{
    if (!CheckPlannerCall("planner_add_action"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Planner* planner = Find(id, self);
    if (!planner || planner->updating || !vtable || vtable->size < 2 * sizeof(uint32_t) ||
        (condition_count && !conditions) || (effect_count && !effects))
        return GWP_ERROR_INVALID_ARGUMENT;
    for (const auto& entry : planner->planner->operators())
    {
        if (entry.m_operator_id == action_id)
            return GWP_ERROR_INVALID_ARGUMENT;
    }
    // Every condition and effect must name an evaluator of this planner (the engine checks it in DEBUG only)
    const auto& evaluators = planner->planner->evaluators();
    for (uint32_t i = 0; i < condition_count + effect_count; ++i)
    {
        const GwpWorldProperty& property = i < condition_count ? conditions[i] : effects[i - condition_count];
        if (evaluators.find(property.id) == evaluators.end())
        {
            Msg("! [plugin:%s] planner_add_action '%s': property %u has no evaluator", PluginAddonId(self),
                name ? name : "", property.id);
            return GWP_ERROR_INVALID_ARGUMENT;
        }
    }
    GwpActionVTable copy{};
    memcpy(&copy, vtable, std::min<size_t>(vtable->size, sizeof(copy)));
    auto* action = xr_new<CNativePlannerAction>(planner, copy, user, name ? name : "");
    for (uint32_t i = 0; i < condition_count; ++i)
        action->add_condition(GraphEngineSpace::CWorldProperty(conditions[i].id, conditions[i].value != 0));
    for (uint32_t i = 0; i < effect_count; ++i)
        action->add_effect(GraphEngineSpace::CWorldProperty(effects[i].id, effects[i].value != 0));
    planner->planner->add_operator(action_id, action);
    return GWP_OK;
}

GwpResult GWP_CALL ApiPlannerSetGoal(
    const GwpPlugin* self, GwpPlannerId id, const GwpWorldProperty* goal, uint32_t count)
{
    if (!CheckPlannerCall("planner_set_goal"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Planner* planner = Find(id, self);
    if (!planner || planner->updating || (count && !goal))
        return GWP_ERROR_INVALID_ARGUMENT;
    // A goal property without an evaluator would make the solver read past the evaluators (THROW is a no-op in
    // a release build)
    const auto& evaluators = planner->planner->evaluators();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (evaluators.find(goal[i].id) == evaluators.end())
        {
            Msg("! [plugin:%s] planner_set_goal: property %u has no evaluator", PluginAddonId(self), goal[i].id);
            return GWP_ERROR_INVALID_ARGUMENT;
        }
    }
    GraphEngineSpace::CWorldState state;
    for (uint32_t i = 0; i < count; ++i)
        state.add_condition(GraphEngineSpace::CWorldProperty(goal[i].id, goal[i].value != 0));
    planner->planner->set_target_state(state);
    return GWP_OK;
}

GwpResult GWP_CALL ApiPlannerUpdate(const GwpPlugin* self, GwpPlannerId id)
{
    if (!CheckPlannerCall("planner_update"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    Planner* planner = Find(id, self);
    if (!planner || planner->updating)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (planner->disabled)
        return GWP_ERROR;
    planner->updating = true;
    planner->planner->update();
    planner->updating = false;
    if (planner->doomed)
        Destroy(IndexOf(id)); // the object was destroyed from inside the update: its Lua object is gone
    return GWP_OK;
}

uint32_t GWP_CALL ApiPlannerCurrentAction(GwpPlannerId id)
{
    if (!CheckPlannerCall("planner_current_action"))
        return GWP_INVALID_ACTION_ID;
    const Planner* planner = Find(id, nullptr);
    if (!planner || !planner->planner->initialized())
        return GWP_INVALID_ACTION_ID;
    return planner->planner->current_action_id();
}

// planner:evaluator(id):evaluate() of Lua: the evaluator is called again (for a native one, the plugin function),
// -1 when the property is unknown.
int GWP_CALL ApiPlannerEvaluate(GwpPlannerId id, uint32_t property)
{
    if (!CheckPlannerCall("planner_evaluate"))
        return -1;
    Planner* planner = Find(id, nullptr);
    if (!planner)
        return -1;
    const auto& evaluators = planner->planner->evaluators();
    const auto it = evaluators.find(property);
    if (it == evaluators.end())
        return -1;
    return (*it).second->evaluate() ? 1 : 0;
}
} // namespace

void FillPlannerApi(GwpEngineApi& api)
{
    api.planner_create = &ApiPlannerCreate;
    api.planner_destroy = &ApiPlannerDestroy;
    api.planner_add_evaluator = &ApiPlannerAddEvaluator;
    api.planner_add_action = &ApiPlannerAddAction;
    api.planner_set_goal = &ApiPlannerSetGoal;
    api.planner_update = &ApiPlannerUpdate;
    api.planner_current_action = &ApiPlannerCurrentAction;
    api.planner_evaluate = &ApiPlannerEvaluate;
}

void OnObjectDestroyPlanners(const CGameObject* object)
{
    if (!g_planners || !object)
        return;
    const u16 npc = object->ID();
    for (u32 i = 0; i < g_planners->size(); ++i)
    {
        Planner* planner = (*g_planners)[i];
        if (!planner || planner->npc != npc)
            continue;
        if (planner->updating)
        {
            planner->disabled = true; // no more callbacks; the update in progress destroys it when it returns
            planner->doomed = true;
        }
        else
            Destroy(i);
    }
}

void RemovePluginPlanners(const GwpPlugin* plugin)
{
    if (!g_planners)
        return;
    for (u32 i = 0; i < g_planners->size(); ++i)
    {
        Planner* planner = (*g_planners)[i];
        if (planner && planner->plugin == plugin)
        {
            if (planner->updating)
                planner->disabled = true; // unloading from inside a callback: leaks the planner rather than crash
            else
                Destroy(i);
        }
    }
}

void StopPluginPlanners(const GwpPlugin* plugin) { StopPlannersOf(plugin); }

void ForgetPluginCrash(const GwpPlugin* plugin)
{
    if (g_dead_plugins)
    {
        g_dead_plugins->erase(std::remove(g_dead_plugins->begin(), g_dead_plugins->end(), plugin),
            g_dead_plugins->end());
    }
}

void ShutdownPlanners()
{
    if (g_planners)
    {
        for (u32 i = 0; i < g_planners->size(); ++i)
            Destroy(i);
    }
    xr_delete(g_planners);
    xr_delete(g_generations);
    xr_delete(g_dead_plugins);
}
} // namespace gw::addons::goap
