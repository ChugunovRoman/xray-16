////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_enemy_manager.h
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent enemy manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "member_enemy.h"

class CAgentManager;
class CMemberOrder;
class CEntityAlive;
class CAI_Stalker;

// agent_enemy_manager.cpp: the minimum interval of distribute_enemies_from_memory, ms (0 = once a frame, as before)
extern int npc_agent_distribute_min_ms;

class CAgentEnemyManager
{
public:
    typedef xr_vector<CMemberEnemy> ENEMIES;
    typedef MemorySpace::squad_mask_type squad_mask_type;
    typedef std::pair<ALife::_OBJECT_ID, bool> WOUNDED;
    typedef std::pair<const CEntityAlive*, WOUNDED> WOUNDED_ENEMY;
    typedef xr_vector<WOUNDED_ENEMY> WOUNDED_ENEMIES;

private:
    CAgentManager* m_object;
    ENEMIES m_enemies;
    WOUNDED_ENEMIES m_wounded;
    bool m_only_wounded_left;
    bool m_is_any_wounded;
    // Device.dwFrame of the last distribute_enemies: every member without an enemy asks for it on its memory
    // update, and the whole group is walked each time, so it runs once per frame
    u32 m_distributed_frame;
    // npc_agent_distribute_min_ms: Device.dwTimeGlobal of the last distribution, the IDs it knew (its enemies and the
    // enemies the combat members had selected, sorted) and whether a member selected one it did not know since
    u32 m_distributed_time;
    bool m_unknown_enemy_selected;
    xr_vector<ALife::_OBJECT_ID> m_known_enemies;
    u32 m_known_enemies_check_frame;
    bool m_known_enemy_gone;

protected:
    template <typename T>
    IC void setup_mask(xr_vector<T>& objects, CMemberEnemy& enemy, const squad_mask_type& non_combat_members);
    IC void setup_mask(CMemberEnemy& enemy, const squad_mask_type& non_combat_members);
    void fill_enemies();
    void compute_enemy_danger();
    void assign_enemies();
    void permutate_enemies();
    void assign_wounded();
    void assign_enemy_masks();
    void assign_enemy_masks_batched();
    void distribute_enemies_impl();
    void remember_known_enemies();
    bool known_enemy_gone();
    float evaluate(const CEntityAlive* object0, const CEntityAlive* object1) const;
    void exchange_enemies(CMemberOrder& member0, CMemberOrder& member1);
    IC CAgentManager& object() const;

public:
    IC CAgentEnemyManager(CAgentManager* object);
    void update();
    void distribute_enemies();
    // CMemoryManager::update_enemies (a combat member without an enemy): distribute_enemies, at most once in
    // npc_agent_distribute_min_ms unless an enemy it knew died or left, or a member selected one it did not know
    void distribute_enemies_from_memory();
    // CMemoryManager::update_enemies (a combat member with an enemy, npc_agent_distribute_min_ms > 0)
    void on_member_enemy_selected(const CEntityAlive* enemy);
    IC ENEMIES& enemies();
    void remove_links(IGameObject* object);

private:
    void wounded_processor(const CEntityAlive* object, const ALife::_OBJECT_ID& wounded_processor_id);

public:
    ALife::_OBJECT_ID wounded_processor(const CEntityAlive* object);
    void wounded_processed(const CEntityAlive* object, bool value);
    bool wounded_processed(const CEntityAlive* object) const;
    bool assigned_wounded(const CEntityAlive* wounded, const CAI_Stalker* member);
    bool useful_enemy(const CEntityAlive* enemy, const CAI_Stalker* member) const;
};

#include "agent_enemy_manager_inline.h"
