////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_location_manager.h
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent location manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "danger_location.h"

class CAI_Stalker;
class CCoverPoint;
class CAgentManager;
class CDangerLocation;
class IGameObject;

// 26-framemove-game-next.md, the cover scan cache (always on since 2026-10-18, was npc_cover_scan_cache 2): suitable() of
// a cover scan answers from a snapshot of the group taken once per scan (CStalkerMovementRestrictor) and
// CSpaceRestriction::accessible(sphere) keeps its answers across frames until a space restrictor moves, appears or goes

class CAgentLocationManager
{
public:
    typedef intrusive_ptr<CDangerLocation> CDangerLocationPtr;
    typedef xr_vector<CDangerLocationPtr> LOCATIONS;

    // what suitable() reads of the group, taken once per cover scan (the scan does not change it)
    struct suitable_snapshot
    {
        struct member
        {
            Fvector position;
            Fvector cover_position;
            float to_cover_sqr;
            bool has_cover;
            bool in_combat;
        };
        xr_vector<member> members;
        xr_vector<Fvector> enemy_positions;
        Fvector self_position;
        bool ready = false;
    };

private:
    CAgentManager* m_object;
    LOCATIONS m_danger_locations;

protected:
    IC CAgentManager& object() const;
    void remove_old_danger_covers();

public:
    IC CAgentLocationManager(CAgentManager* object);
    IC CDangerLocationPtr location(const Fvector& position);
    IC CDangerLocationPtr location(const IGameObject* object);
    IC void clear();
    void update();
    void add(CDangerLocationPtr location);
    float danger(const CCoverPoint* cover, CAI_Stalker* member) const;
    bool suitable(CAI_Stalker* object, const CCoverPoint* location, bool use_enemy_info) const;
    // the same answer as suitable(), from the snapshot (taken on the first call of the scan)
    bool suitable(CAI_Stalker* object, const CCoverPoint* location, bool use_enemy_info,
        suitable_snapshot& snapshot) const;
    void make_suitable(CAI_Stalker* object, const CCoverPoint* location) const;
    void remove_links(IGameObject* object);
    IC const LOCATIONS& locations() const;
};

#include "agent_location_manager_inline.h"
