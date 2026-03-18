////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_danger_planner.cpp
//	Created 	: 11.02.2005
//  Modified 	: 11.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker danger planner
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "stalker_danger_planner.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_space.h"
#include "sound_player.h"
#include "script_game_object.h"
#include "script_game_object_impl.h"
#include "stalker_decision_space.h"
#include "stalker_danger_property_evaluators.h"
#include "memory_manager.h"
#include "danger_manager.h"
#include "enemy_manager.h"
#include "cover_evaluators.h"
#include "cover_manager.h"
#include "cover_point.h"
#include "stalker_movement_restriction.h"
#include "agent_member_manager.h"
#include "member_order.h"
#include "stalker_base_action.h"
#include "stalker_danger_unknown_planner.h"
#include "stalker_danger_in_direction_planner.h"
#include "stalker_danger_grenade_planner.h"
#include "stalker_danger_by_sound_planner.h"

using namespace StalkerSpace;
using namespace StalkerDecisionSpace;

CStalkerDangerPlanner::CStalkerDangerPlanner(CAI_Stalker* object, LPCSTR action_name) : inherited(object, action_name)
{
}

void CStalkerDangerPlanner::setup(CAI_Stalker* object, CPropertyStorage* storage)
{
    inherited::setup(object, storage);

    clear();
    add_evaluators();
    add_actions();
}

void CStalkerDangerPlanner::finalize()
{
    inherited::finalize();

    if (!object().g_Alive())
        return;

    if (object().memory().enemy().selected())
        object().memory().danger().time_line(Device.dwTimeGlobal);

    //	object().sound().remove_active_sounds		(u32(-1));
}

void CStalkerDangerPlanner::update()
{
    inherited::update();

    if (this->solution().empty())
    {
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyInCover, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyLookedOut, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyPositionHolded, false);
        CScriptActionPlanner::m_storage.set_property(eWorldPropertyEnemyDetoured, false);

        CMemberOrder* member_order = object().agent_manager().member().get_member(object().ID());
        if (member_order)
            member_order->cover(0);

        if (object().movement().in_smart_cover() || object().movement().entering_smart_cover_with_animation())
        {
            object().movement().target_default(true);
            object().movement().target_idle();
            object().movement().cleanup_after_animation_selector();
        }

        object().movement().clear_path();
        return;
    }

    object().react_on_grenades();
    object().react_on_member_death();
}

void CStalkerDangerPlanner::initialize()
{
    inherited::initialize();
    object().sound().remove_active_sounds(u32(eStalkerSoundMaskNoHumming));
    if (CMemberOrder* member_order = object().agent_manager().member().get_member(object().ID()))
        member_order->cover(0);
}

void CStalkerDangerPlanner::add_evaluators()
{
    add_evaluator(eWorldPropertyDanger, xr_new<CStalkerPropertyEvaluatorDangers>(m_object, "danger"));
    add_evaluator(eWorldPropertyDangerUnknown, xr_new<CStalkerPropertyEvaluatorDangerUnknown>(m_object, "danger unknown"));
    add_evaluator(eWorldPropertyDangerInDirection,
        xr_new<CStalkerPropertyEvaluatorDangerInDirection>(m_object, "danger in direction"));
    add_evaluator(
        eWorldPropertyDangerGrenade, xr_new<CStalkerPropertyEvaluatorDangerWithGrenade>(m_object, "danger with grenade"));
    add_evaluator(eWorldPropertyDangerBySound, xr_new<CStalkerPropertyEvaluatorDangerBySound>(m_object, "danger by sound"));
}

void CStalkerDangerPlanner::add_actions()
{
    CActionPlannerActionScript<CAI_Stalker>* action;

    action = xr_new<CStalkerDangerUnknownPlanner>(m_object, "danger unknown planner");
    add_condition(action, eWorldPropertyDangerUnknown, true);
    add_effect(action, eWorldPropertyDanger, false);
    add_operator(eWorldOperatorDangerUnknownPlanner, action);

    action = xr_new<CStalkerDangerInDirectionPlanner>(m_object, "danger in direction planner");
    add_condition(action, eWorldPropertyDangerInDirection, true);
    add_effect(action, eWorldPropertyDanger, false);
    add_operator(eWorldOperatorDangerInDirectionPlanner, action);

    action = xr_new<CStalkerDangerGrenadePlanner>(m_object, "danger grenade planner");
    add_condition(action, eWorldPropertyDangerGrenade, true);
    add_effect(action, eWorldPropertyDanger, false);
    add_operator(eWorldOperatorDangerGrenadePlanner, action);

    action = xr_new<CStalkerDangerBySoundPlanner>(m_object, "danger by sound planner");
    add_condition(action, eWorldPropertyDangerBySound, true);
    add_effect(action, eWorldPropertyDanger, false);
    add_operator(eWorldOperatorDangerBySoundPlanner, action);
}
