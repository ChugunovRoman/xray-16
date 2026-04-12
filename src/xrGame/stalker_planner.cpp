////////////////////////////////////////////////////////////////////////////
//	Module 		: motivation_action_manager_stalker.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Stalker motivation action manager class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_planner.h"
#include "stalker_property_evaluators.h"
#include "stalker_danger_property_evaluators.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_decision_space.h"
#include "script_game_object.h"
#include "script_game_object_impl.h"
#include "stalker_alife_planner.h"
#include "stalker_anomaly_planner.h"
#include "stalker_death_planner.h"
#include "stalker_danger_planner.h"
#include "stalker_alife_actions.h"
#include "stalker_combat_planner.h"
#include "Actor.h"
#include "item_manager.h"
#include "danger_manager.h"
#include "enemy_manager.h"
#include "npc_cpp_profile.h"
#include "performance_cvars.h"
#include "xrEngine/profiler.h"

//#define GOAP_DEBUG

using namespace StalkerDecisionSpace;

namespace
{
IC float stalker_planner_near_dist_sqr() { return npc_perf_planner_near_dist * npc_perf_planner_near_dist; }
IC float stalker_planner_medium_dist_sqr() { return npc_perf_planner_medium_dist * npc_perf_planner_medium_dist; }

IC u32 stalker_planner_solve_offset(const u16 object_id, const u32 interval_ms)
{
    if (!interval_ms)
        return 0;

    constexpr u32 bucket_count = 8;
    const u32 buckets = _min(bucket_count, interval_ms);
    return (interval_ms * (object_id % buckets)) / buckets;
}

IC u32 get_stalker_planner_solve_interval(const CAI_Stalker& stalker)
{
    if (stalker.memory().enemy().selected())
    {
        // В бою решатель нужен часто, но не "каждый тик",
        // иначе пики graph_search/solve каскадно усиливают Lua evaluators.
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (!actor)
            return npc_perf_planner_solve_interval_medium_ms;

        const float dist_sqr = stalker.Position().distance_to_sqr(actor->Position());
        if (dist_sqr <= stalker_planner_near_dist_sqr())
            return npc_perf_planner_solve_interval_combat_near_ms;
        if (dist_sqr <= stalker_planner_medium_dist_sqr())
            return npc_perf_planner_solve_interval_medium_ms;

        return npc_perf_planner_solve_interval_far_ms;
    }

    if (stalker.memory().danger().selected() && !stalker.memory().enemy().selected() &&
        npc_perf_planner_solve_interval_danger_only_ms > 0)
        return npc_perf_planner_solve_interval_danger_only_ms;

    if (stalker.is_nearby_idle_state_optimization_candidate())
        return npc_perf_planner_solve_interval_near_idle_ms;

    CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!actor)
        return 0;

    const float dist_sqr = stalker.Position().distance_to_sqr(actor->Position());
    if (dist_sqr <= stalker_planner_near_dist_sqr())
        return 0;
    if (dist_sqr <= stalker_planner_medium_dist_sqr())
        return npc_perf_planner_solve_interval_medium_ms;

    return npc_perf_planner_solve_interval_far_ms;
}

IC bool should_run_stalker_planner_solve(
    const CAI_Stalker& stalker, u32& next_update_time, u32& current_interval, const u32 now_ms)
{
    const u32 new_interval = get_stalker_planner_solve_interval(stalker);
    if (!new_interval)
    {
        current_interval = 0;
        next_update_time = now_ms;
        return true;
    }

    if (current_interval != new_interval)
    {
        current_interval = new_interval;
        next_update_time = now_ms + stalker_planner_solve_offset(stalker.ID(), new_interval);
        return next_update_time <= now_ms;
    }

    if (next_update_time > now_ms)
        return false;

    next_update_time = now_ms + new_interval;
    return true;
}

IC u32 get_stalker_planner_actuality_interval(const CAI_Stalker& stalker)
{
    if (stalker.memory().enemy().selected())
        return npc_perf_planner_actuality_interval_combat_ms;

    if (stalker.memory().danger().selected())
        return npc_perf_planner_actuality_interval_danger_ms;

    if (stalker.is_nearby_idle_state_optimization_candidate())
        return npc_perf_planner_actuality_interval_near_idle_ms;

    return 0;
}

IC bool should_run_stalker_planner_actuality_check(
    const CAI_Stalker& stalker, u32& next_update_time, u32& current_interval, const u32 now_ms)
{
    const u32 new_interval = get_stalker_planner_actuality_interval(stalker);
    if (!new_interval)
    {
        current_interval = 0;
        next_update_time = now_ms;
        return true;
    }

    if (current_interval != new_interval)
    {
        current_interval = new_interval;
        next_update_time = now_ms + stalker_planner_solve_offset(stalker.ID(), new_interval);
        return next_update_time <= now_ms;
    }

    if (next_update_time > now_ms)
        return false;

    next_update_time = now_ms + new_interval;
    return true;
}

IC bool evaluate_stalker_planner_property_fast(
    const CStalkerPlanner& planner, CAI_Stalker& stalker, const GraphEngineSpace::_solver_condition_type property_id, bool& value)
{
    switch (property_id)
    {
    case eWorldPropertyAlive:
        value = !!stalker.g_Alive();
        return true;
    case eWorldPropertyPuzzleSolved:
        value = false;
        return true;
    case eWorldPropertyItems:
        value = !!stalker.memory().item().selected();
        return true;
    case eWorldPropertyDanger:
        value = !!stalker.memory().danger().selected();
        return true;
    case eWorldPropertyEnemy:
        value = !!stalker.memory().enemy().selected();
        if (!value)
        {
            const u32 last_enemy_time = stalker.memory().enemy().last_enemy_time();
            value = Device.dwTimeGlobal < (last_enemy_time + CStalkerCombatPlanner::POST_COMBAT_WAIT_INTERVAL);
        }
        return true;
    case eWorldPropertyAnomaly:
    {
        const CPropertyStorage::CConditionStorage& storage = planner.m_storage.m_storage;
        CPropertyStorage::CConditionStorage::const_iterator I = std::find(storage.begin(), storage.end(), eWorldPropertyAnomaly);
        if (I == storage.end())
            return false;

        value = (*I).m_value;
        return true;
    }
    default: return false;
    }
}

IC bool try_actual_fast(const CStalkerPlanner& planner, bool& planner_actual)
{
    if (planner.current_state().conditions().empty())
        return false;

    CAI_Stalker& stalker = planner.object();
    const xr_vector<GraphEngineSpace::CWorldProperty>& conditions = planner.current_state().conditions();
    xr_vector<GraphEngineSpace::CWorldProperty>::const_iterator I = conditions.begin();
    xr_vector<GraphEngineSpace::CWorldProperty>::const_iterator E = conditions.end();
    for (; I != E; ++I)
    {
        bool actual_value = false;
        if (!evaluate_stalker_planner_property_fast(planner, stalker, (*I).condition(), actual_value))
            return false;

        if (actual_value != (*I).value())
        {
            planner_actual = false;
            return true;
        }
    }

    planner_actual = true;
    return true;
}
} // namespace

CStalkerPlanner::CStalkerPlanner() { m_affect_cover = false; }
CStalkerPlanner::~CStalkerPlanner() {}
#ifdef LOG_ACTION
LPCSTR CStalkerPlanner::action2string(const _action_id_type& action_id)
{
    return (inherited::action2string(action_id));
}

LPCSTR CStalkerPlanner::property2string(const _condition_type& property_id)
{
    return (inherited::property2string(property_id));
}

LPCSTR CStalkerPlanner::object_name() const
{
    VERIFY(m_object);
    return (m_object->cName().c_str());
}
#endif

void CStalkerPlanner::setup(CAI_Stalker* object)
{
#ifdef LOG_ACTION
    set_use_log(!!psAI_Flags.test(aiGOAP));
#endif

    inherited::setup(object);

    clear();
    add_evaluators();
    add_actions();

    CWorldState target;
    target.add_condition(CWorldProperty(eWorldPropertyPuzzleSolved, true));
    set_target_state(target);

    m_affect_cover = false;
    m_next_solve_update_time = 0;
    m_solve_update_interval = 0;
    m_next_actuality_check_time = 0;
    m_actuality_check_interval = 0;
}

void CStalkerPlanner::update(u32 time_delta)
{
    ZoneScopedN("CStalkerPlanner::update");
    ZoneTextF("%s", m_object ? m_object->cName().c_str() : "no_object");
#ifdef LOG_ACTION
    if ((psAI_Flags.test(aiGOAP) && !m_use_log) || (!psAI_Flags.test(aiGOAP) && m_use_log))
        set_use_log(!!psAI_Flags.test(aiGOAP));
#endif

    const u32 now_ms = Device.dwTimeGlobal;
    bool run_solve = should_run_stalker_planner_solve(object(), m_next_solve_update_time, m_solve_update_interval, now_ms);
    {
        ZoneScopedN("brain_update/solve_gate");
        ZoneTextF("run_solve=%d interval=%u", run_solve ? 1 : 0, m_solve_update_interval);
    }
    if (!initialized() || this->solution().empty())
        run_solve = true;
    else if (!m_solve_update_interval && current_action().completed())
        run_solve = true;

    if (run_solve)
    {
        ZoneScopedN("brain_update/solve");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerSolve);

        m_solution_changed = false;

        bool planner_actual = false;
        const bool can_skip_actuality_check = m_actuality && !m_current_state.conditions().empty() &&
            !should_run_stalker_planner_actuality_check(object(), m_next_actuality_check_time, m_actuality_check_interval, now_ms);
        if (can_skip_actuality_check)
        {
            ZoneScopedN("brain_update/actuality_skipped");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerActualitySkipped);
            planner_actual = true;
        }
        else if (m_actuality && try_actual_fast(*this, planner_actual))
        {
            ZoneScopedN("brain_update/actuality_fast_path");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerActualityFastPath);
        }
        else
        {
            ZoneScopedN("brain_update/actuality_full");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerActualityCheck);
            planner_actual = actual();
        }

        if (!planner_actual)
        {
            m_actuality = true;
            m_solution_changed = true;

            {
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerStateClear);
                m_current_state.clear();
            }

            m_solving = true;
            {
                ZoneScopedN("brain_update/graph_search");
                const bool combat_active = object().memory().enemy().selected() != nullptr;
                const bool danger_active = object().memory().danger().selected() != nullptr;
                u32 max_nodes = npc_perf_planner_graph_search_max_nodes;
                if (combat_active && npc_perf_planner_graph_search_max_nodes_combat > 0)
                    max_nodes = npc_perf_planner_graph_search_max_nodes_combat;
                else if (danger_active && npc_perf_planner_graph_search_max_nodes_danger > 0)
                    max_nodes = npc_perf_planner_graph_search_max_nodes_danger;
                ZoneTextF("max_nodes=%u combat=%d danger=%d", max_nodes, combat_active ? 1 : 0, danger_active ? 1 : 0);
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerGraphSearch);
                m_failed = !GEnv.AISpace->graph_engine().search(*this, reverse_search ? target_state() : current_state(),
                    reverse_search ? current_state() : target_state(), &m_solution,
                    GraphEngineSpace::CSolverBaseParameters(
                        GraphEngineSpace::_solver_dist_type(-1), GraphEngineSpace::_solver_condition_type(-1),
                        max_nodes));
            }
            m_solving = false;
        }
    }

#ifdef LOG_ACTION
    if (m_use_log)
    {
        if (this->m_solution_changed)
        {
            show_current_world_state();
            show_target_world_state();
            Msg("%6d : Solution for object %s [%d vertices searched]", Device.dwTimeGlobal, object_name(),
                ai().graph_engine().solver_algorithm().data_storage().get_visited_node_count());
            for (int i = 0; i < (int)this->solution().size(); ++i)
                Msg("%s", action2string(this->solution()[i]));
        }
    }
#endif

    static bool bDbgAct = strstr(Core.Params, "-dbgact") != NULL;

#ifdef LOG_ACTION
    if (this->m_failed)
    {
        show();
        Msg("! ERROR: there is no action sequence, which can transfer current world state to the target one: action[%s] object[%s], time[%6d]",
            object_name(), current_action().m_action_name, Device.dwTimeGlobal);
        show_current_world_state();
        show_target_world_state();
    }
#else
    if (bDbgAct && this->m_failed && current_action().m_action_name)
        GEnv.ScriptEngine->script_log(LuaMessageType::Error,
            "! ERROR: there is no action sequence, which can transfer current world state to the target one: action[%s] object_name[%s]",
            current_action().m_action_name, m_object->cName().c_str());
#endif

    if (this->solution().empty() || this->solution().size() == 0)
    {
        ZoneScopedN("brain_update/empty_solution");
        static u32 s_last_empty_solution_log = 0;
        if (Device.dwTimeGlobal - s_last_empty_solution_log >= 1000)
        {
            s_last_empty_solution_log = Device.dwTimeGlobal;
            Msg("Solution array is empty! m_object=[%s] solution().empty()=[%d], size=[%d] initialized=[%d] m_current_action_id=[%d]",
                m_object->cName().c_str(), this->solution().empty(), this->solution().size(), initialized(), m_current_action_id);
        }
        return;
    }

    {
        ZoneScopedN("brain_update/transition");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerTransition);
        if (initialized())
        {
            if (!this->solution().empty() && this->solution().size() > 0 && current_action_id() != this->solution().front())
            {
                current_action().finalize();
                m_current_action_id = this->solution().front();
                if (bDbgAct)
                    Msg("DEBUG: Action [%s] initializing", current_action().m_action_name);
                current_action().initialize();
            }
        }
        else
        {
            m_initialized = true;
            m_current_action_id = this->solution().front();
            if (bDbgAct)
                Msg("DEBUG: Action [%s] initializing", current_action().m_action_name);
            current_action().initialize();
        }
    }

    if (bDbgAct)
        Msg("DEBUG: Action [%s] executing", current_action().m_action_name);

    {
        ZoneScopedN("brain_update/execute");
        ZoneTextF("action_id=%d", int(current_action_id()));
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecute);
        switch (current_action_id())
        {
        case eWorldOperatorDeathPlanner:
        {
            ZoneScopedN("brain_update/exec_death");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteDeath);
            current_action().execute();
            break;
        }
        case eWorldOperatorALifePlanner:
        {
            ZoneScopedN("brain_update/exec_alife");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteALife);
            current_action().execute();
            break;
        }
        case eWorldOperatorCombatPlanner:
        {
            ZoneScopedN("brain_update/exec_combat");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteCombat);
            current_action().execute();
            break;
        }
        case eWorldOperatorDangerPlanner:
        {
            ZoneScopedN("brain_update/exec_danger");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteDanger);
            current_action().execute();
            break;
        }
        case eWorldOperatorAnomalyPlanner:
        {
            ZoneScopedN("brain_update/exec_anomaly");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteAnomaly);
            current_action().execute();
            break;
        }
        case eWorldOperatorGatherItems:
        {
            ZoneScopedN("brain_update/exec_gather_items");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteGatherItems);
            current_action().execute();
            break;
        }
        default:
        {
            ZoneScopedN("brain_update/exec_other");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerPlannerExecuteOther);
            current_action().execute();
            break;
        }
        }
    }

#ifdef GOAP_DEBUG
    if (m_failed)
    {
        {
            Msg("%d", evaluators().size());
            EVALUATORS::const_iterator I = evaluators().begin();
            EVALUATORS::const_iterator E = evaluators().end();
            for (; I != E; ++I)
                Msg("%d,%d", (*I).first, (*I).second->evaluate() ? 1 : 0);
        }
        {
            Msg("%d", target_state().conditions().size());
            xr_vector<COperatorCondition>::const_iterator I = target_state().conditions().begin();
            xr_vector<COperatorCondition>::const_iterator E = target_state().conditions().end();
            for (; I != E; ++I)
                Msg("%d,%d", (*I).condition(), (*I).value() ? 1 : 0);
        }
        {
            Msg("%d", operators().size());
            const_iterator I = operators().begin();
            const_iterator E = operators().end();
            for (; I != E; ++I)
            {
                Msg("%d,%d", (*I).m_operator_id, (*I).m_operator->weight(target_state(), target_state()));
                {
                    Msg("%d", (*I).m_operator->conditions().conditions().size());
                    xr_vector<COperatorCondition>::const_iterator i =
                        (*I).m_operator->conditions().conditions().begin();
                    xr_vector<COperatorCondition>::const_iterator e = (*I).m_operator->conditions().conditions().end();
                    for (; i != e; ++i)
                        Msg("%d,%d", (*i).condition(), (*i).value() ? 1 : 0);
                }
                {
                    Msg("%d", (*I).m_operator->effects().conditions().size());
                    xr_vector<COperatorCondition>::const_iterator i = (*I).m_operator->effects().conditions().begin();
                    xr_vector<COperatorCondition>::const_iterator e = (*I).m_operator->effects().conditions().end();
                    for (; i != e; ++i)
                        Msg("%d,%d", (*i).condition(), (*i).value() ? 1 : 0);
                }
            }
        }
    }
#endif
}

void CStalkerPlanner::add_evaluators()
{
    add_evaluator(eWorldPropertyAlreadyDead, xr_new<CStalkerPropertyEvaluatorConst>(false, "is_already_dead"));
    add_evaluator(eWorldPropertyPuzzleSolved, xr_new<CStalkerPropertyEvaluatorConst>(false, "is_zone_puzzle_solved"));
    add_evaluator(eWorldPropertyAlive, xr_new<CStalkerPropertyEvaluatorAlive>(m_object, "is_alive"));
    add_evaluator(eWorldPropertyEnemy, xr_new<CStalkerPropertyEvaluatorEnemies>(m_object, "is_there_enemies",
                                           CStalkerCombatPlanner::POST_COMBAT_WAIT_INTERVAL));
    add_evaluator(eWorldPropertyDanger, xr_new<CStalkerPropertyEvaluatorDangers>(m_object, "is_there_danger"));
    add_evaluator(eWorldPropertyAnomaly, xr_new<CStalkerPropertyEvaluatorAnomaly>(m_object, "is_there_anomalies"));
    add_evaluator(eWorldPropertyItems, xr_new<CStalkerPropertyEvaluatorItems>(m_object, "is_there_items_to_pick_up"));
}

void CStalkerPlanner::add_actions()
{
    CActionPlannerAction* planner;

    planner = xr_new<CStalkerDeathPlanner>(m_object, "death_planner");
    add_condition(planner, eWorldPropertyAlive, false);
    add_condition(planner, eWorldPropertyPuzzleSolved, false);
    add_effect(planner, eWorldPropertyPuzzleSolved, true);
    add_operator(eWorldOperatorDeathPlanner, planner);

    planner = xr_new<CStalkerALifePlanner>(m_object, "alife_planner");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyEnemy, false);
    add_condition(planner, eWorldPropertyAnomaly, false);
    add_condition(planner, eWorldPropertyDanger, false);
    add_condition(planner, eWorldPropertyItems, false);
    add_condition(planner, eWorldPropertyPuzzleSolved, false);
    add_effect(planner, eWorldPropertyPuzzleSolved, true);
    add_operator(eWorldOperatorALifePlanner, planner);

    planner = xr_new<CStalkerCombatPlanner>(m_object, "combat_planner");
    //	planner					= xr_new<CStalkerCombatPlannerNew>(m_object,"combat_planner_new");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyAnomaly, false);
    add_condition(planner, eWorldPropertyEnemy, true);
    add_effect(planner, eWorldPropertyEnemy, false);
    add_operator(eWorldOperatorCombatPlanner, planner);

    planner = xr_new<CStalkerDangerPlanner>(m_object, "danger_planner");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyEnemy, false);
    add_condition(planner, eWorldPropertyAnomaly, false);
    add_condition(planner, eWorldPropertyDanger, true);
    add_effect(planner, eWorldPropertyDanger, false);
    add_operator(eWorldOperatorDangerPlanner, planner);

    planner = xr_new<CStalkerAnomalyPlanner>(m_object, "anomaly_planner");
    add_condition(planner, eWorldPropertyAlive, true);
    add_condition(planner, eWorldPropertyAnomaly, true);
    add_effect(planner, eWorldPropertyAnomaly, false);
    add_operator(eWorldOperatorAnomalyPlanner, planner);

    CStalkerActionBase* action;

    action = xr_new<CStalkerActionGatherItems>(m_object, "gather_items");
    add_condition(action, eWorldPropertyAlive, true);
    add_condition(action, eWorldPropertyEnemy, false);
    add_condition(action, eWorldPropertyAnomaly, false);
    add_condition(action, eWorldPropertyDanger, false);
    add_condition(action, eWorldPropertyItems, true);
    add_effect(action, eWorldPropertyItems, false);
    add_operator(eWorldOperatorGatherItems, action);
}
