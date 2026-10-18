////////////////////////////////////////////////////////////////////////////
//	Module 		: squad_hierarchy_holder.cpp
//	Created 	: 12.11.2001
//  Modified 	: 03.09.2004
//	Author		: Dmitriy Iassenev, Oles Shishkovtsov, Aleksandr Maksimchuk
//	Description : Squad hierarchy holder
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "squad_hierarchy_holder.h"
#include "group_hierarchy_holder.h"
#include "Common/object_broker.h"
#include "seniority_hierarchy_space.h"
#include "memory_space.h"

CSquadHierarchyHolder::~CSquadHierarchyHolder()
{
    delete_data(m_groups);
    for (auto& extra : m_extra_groups)
        xr_delete(extra.second);
}

CGroupHierarchyHolder& CSquadHierarchyHolder::group(u32 group_id) const
{
    if (group_id >= max_group_count)
    {
        CGroupHierarchyHolder*& extra = m_extra_groups[group_id];
        if (!extra)
            extra = xr_new<CGroupHierarchyHolder>(const_cast<CSquadHierarchyHolder*>(this));
        return (*extra);
    }

    if (!m_groups[group_id])
        m_groups[group_id] = xr_new<CGroupHierarchyHolder>(const_cast<CSquadHierarchyHolder*>(this));
    return (*m_groups[group_id]);
}

#ifdef SQUAD_HIERARCHY_HOLDER_USE_LEADER
void CSquadHierarchyHolder::update_leader()
{
    m_leader = 0;
    for (const auto& group : m_groups)
    {
        if (group && group->leader())
        {
            leader(group->leader());
            return;
        }
    }
    for (const auto& extra : m_extra_groups)
    {
        if (extra.second && extra.second->leader())
        {
            leader(extra.second->leader());
            return;
        }
    }
}
#endif // SQUAD_HIERARCHY_HOLDER_USE_LEADER
