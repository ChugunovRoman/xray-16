////////////////////////////////////////////////////////////////////////////
//	Module 		: seniority_hierarchy_holder.h
//	Created 	: 12.11.2001
//  Modified 	: 03.09.2004
//	Author		: Dmitriy Iassenev, Oles Shishkovtsov, Aleksandr Maksimchuk
//	Description : Seniority hierarchy holder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "seniority_hierarchy_space.h"

class CTeamHierarchyHolder;

class CSeniorityHierarchyHolder
{
private:
    enum
    {
        // Every value of the u8 team (game_relations.ltx: communities and [monster_communities]; change_team of the
        // scripts): team() indexes m_teams with only a VERIFY, so in release a team of 128..255 wrote past the array
        // (was 128)
        max_team_count = 256
    };

private:
    typedef svector<CTeamHierarchyHolder*, max_team_count> TEAM_REGISTRY;

private:
    TEAM_REGISTRY m_teams;

public:
    IC CSeniorityHierarchyHolder();
    virtual ~CSeniorityHierarchyHolder();
    CTeamHierarchyHolder& team(u32 team_id);
    IC const TEAM_REGISTRY& teams() const;
};

#include "seniority_hierarchy_holder_inline.h"
