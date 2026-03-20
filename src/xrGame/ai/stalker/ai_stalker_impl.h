////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_impl.h
//	Created 	: 25.02.2003
//  Modified 	: 25.02.2003
//	Author		: Dmitriy Iassenev
//	Description : AI Behaviour for monster "Stalker" (inline functions implementation)
////////////////////////////////////////////////////////////////////////////

#pragma once
#include "Level.h"
#include "seniority_hierarchy_holder.h"
#include "team_hierarchy_holder.h"
#include "squad_hierarchy_holder.h"
#include "group_hierarchy_holder.h"
#include "EffectorShot.h"
#include "stalker_movement_manager_smart_cover.h"
#include "smart_cover_animation_selector.h"
#include "smart_cover_animation_planner.h"

IC CAgentManager& CAI_Stalker::agent_manager() const
{
    CGroupHierarchyHolder& group = Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(g_Group());
    if (!group.get_agent_manager())
        const_cast<CGroupHierarchyHolder&>(group).lazy_ensure_agent_manager_for_stalker(const_cast<CAI_Stalker*>(this));

    CAgentManager* const am = group.get_agent_manager();
    R_ASSERT2(am, "CAI_Stalker::agent_manager: nullptr after lazy_ensure_agent_manager_for_stalker");
    return (*am);
}

IC Fvector CAI_Stalker::weapon_shot_effector_direction(const Fvector& current) const
{
#if 1
    return current;
#else // #if 1
    VERIFY(weapon_shot_effector().IsActive());
    Fvector result;
    weapon_shot_effector().GetDeltaAngle(result);

    float y, p;
    current.getHP(y, p);

    result.setHP(-result.y + y, -result.x + p);

    return (result);
#endif // #if 1
}
