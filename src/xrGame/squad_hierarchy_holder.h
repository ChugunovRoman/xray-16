////////////////////////////////////////////////////////////////////////////
//	Module 		: squad_hierarchy_holder.h
//	Created 	: 12.11.2001
//  Modified 	: 03.09.2004
//	Author		: Dmitriy Iassenev, Oles Shishkovtsov, Aleksandr Maksimchuk
//	Description : Squad hierarchy holder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "seniority_hierarchy_space.h"
#include "xrCommon/xr_map.h"

class CGroupHierarchyHolder;
class CEntity;
class CTeamHierarchyHolder;

namespace SquadHierarchyHolder
{
typedef xr_vector<CGroupHierarchyHolder*> GROUP_REGISTRY;
}

class CSquadHierarchyHolder
{
private:
    enum
    {
        max_group_count = 32
    };

private:
    typedef SquadHierarchyHolder::GROUP_REGISTRY GROUP_REGISTRY;

private:
    CTeamHierarchyHolder* m_team;
    mutable GROUP_REGISTRY m_groups;
    // Group ids >= max_group_count: the stalker groups keyed by their ALife squad (the group by squad) and the
    // overflow groups of a full one (64 members, the squad masks of the agent manager) - see CEntity in Entity.cpp -
    // and a group id >= 32 from the scripts (change_team), which used to index past m_groups (VERIFY only)
    mutable xr_map<u32, CGroupHierarchyHolder*> m_extra_groups;

#ifdef SQUAD_HIERARCHY_HOLDER_USE_LEADER
private:
    CEntity* m_leader;
#endif // SQUAD_HIERARCHY_HOLDER_USE_LEADER

public:
    IC CSquadHierarchyHolder(CTeamHierarchyHolder* team);
    virtual ~CSquadHierarchyHolder();
    CGroupHierarchyHolder& group(u32 group_id) const;
    IC CTeamHierarchyHolder& team() const;
    IC const GROUP_REGISTRY& groups() const;

#ifdef SQUAD_HIERARCHY_HOLDER_USE_LEADER
public:
    IC void leader(CEntity* leader);
    IC CEntity* leader() const;
    void update_leader();
#endif // SQUAD_HIERARCHY_HOLDER_USE_LEADER
};

#include "squad_hierarchy_holder_inline.h"
