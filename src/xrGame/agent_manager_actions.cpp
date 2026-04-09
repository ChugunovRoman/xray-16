////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_manager_actions.cpp
//	Created 	: 25.05.2004
//  Modified 	: 25.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Agent manager actions
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include <tracy/Tracy.hpp>
#include "agent_manager_actions.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_location_manager.h"
#include "agent_corpse_manager.h"
#include "agent_explosive_manager.h"
#include "agent_enemy_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "sight_action.h"
#include "Inventory.h"

//////////////////////////////////////////////////////////////////////////
// CAgentManagerActionNoOrders
//////////////////////////////////////////////////////////////////////////

CAgentManagerActionNoOrders::CAgentManagerActionNoOrders(CAgentManager* object, LPCSTR action_name)
    : inherited(object, action_name)
{
}

void CAgentManagerActionNoOrders::finalize()
{
    inherited::finalize();
    m_object->corpse().clear();
}

//////////////////////////////////////////////////////////////////////////
// CAgentManagerActionGatherItems
//////////////////////////////////////////////////////////////////////////

CAgentManagerActionGatherItems::CAgentManagerActionGatherItems(CAgentManager* object, LPCSTR action_name)
    : inherited(object, action_name)
{
}

//////////////////////////////////////////////////////////////////////////
// CAgentManagerActionKillEnemy
//////////////////////////////////////////////////////////////////////////

CAgentManagerActionKillEnemy::CAgentManagerActionKillEnemy(CAgentManager* object, LPCSTR action_name)
    : inherited(object, action_name)
{
}

void CAgentManagerActionKillEnemy::initialize()
{
    inherited::initialize();

    m_object->location().clear();
}

void CAgentManagerActionKillEnemy::finalize()
{
    inherited::finalize();

    //	m_object->enemy().distribute_enemies		();
}

void CAgentManagerActionKillEnemy::execute()
{
    ZoneNamedN(___tracy_am_kill, "agent_mgr_act/kill_enemy", true);
    {
        ZoneNamedN(___tracy_am_kill_base, "agent_mgr_act/kill_enemy/action_base", true);
        inherited::execute();
    }
    {
        ZoneNamedN(___tracy_am_kill_dist, "agent_mgr_act/kill_enemy/distribute_enemies", true);
        m_object->enemy().distribute_enemies();
    }
    {
        ZoneNamedN(___tracy_am_kill_expl, "agent_mgr_act/kill_enemy/react_explosives", true);
        m_object->explosive().react_on_explosives();
    }
    {
        ZoneNamedN(___tracy_am_kill_corpse, "agent_mgr_act/kill_enemy/react_member_death", true);
        m_object->corpse().react_on_member_death();
    }
}

//////////////////////////////////////////////////////////////////////////
// CAgentManagerActionReactOnDanger
//////////////////////////////////////////////////////////////////////////

CAgentManagerActionReactOnDanger::CAgentManagerActionReactOnDanger(CAgentManager* object, LPCSTR action_name)
    : inherited(object, action_name)
{
}

void CAgentManagerActionReactOnDanger::initialize()
{
    inherited::initialize();

    m_object->location().clear();
}

void CAgentManagerActionReactOnDanger::execute()
{
    ZoneNamedN(___tracy_am_dang, "agent_mgr_act/react_on_danger", true);
    {
        ZoneNamedN(___tracy_am_dang_base, "agent_mgr_act/react_on_danger/action_base", true);
        inherited::execute();
    }
    {
        ZoneNamedN(___tracy_am_dang_expl, "agent_mgr_act/react_on_danger/react_explosives", true);
        m_object->explosive().react_on_explosives();
    }
    {
        ZoneNamedN(___tracy_am_dang_corpse, "agent_mgr_act/react_on_danger/react_member_death", true);
        m_object->corpse().react_on_member_death();
    }
}
