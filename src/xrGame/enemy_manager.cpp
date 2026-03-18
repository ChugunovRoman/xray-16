////////////////////////////////////////////////////////////////////////////
//	Module 		: enemy_manager.cpp
//	Created 	: 30.12.2003
//  Modified 	: 30.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Enemy manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "enemy_manager.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "hit_memory_manager.h"
#include "ef_storage.h"
#include "ef_pattern.h"
#include "autosave_manager.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/level_graph.h"
#include "Level.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "xrEngine/profiler.h"
#include "Actor.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "movement_manager.h"
#include "agent_manager.h"
#include "agent_enemy_manager.h"
#include "npc_cpp_profile.h"

static const u32 ENEMY_INERTIA_TIME_TO_SOMEBODY = 3000;
static const u32 ENEMY_INERTIA_TIME_TO_ACTOR = 0;
static const u32 ENEMY_INERTIA_TIME_FROM_ACTOR = 6000;
static const u32 ENEMY_USEFUL_CALLBACK_CACHE_TTL_MS = 250;
static const size_t ENEMY_USEFUL_CALLBACK_CACHE_SIZE = 32;

#ifdef _DEBUG
bool g_enemy_manager_second_update = false;
#endif // _DEBUG

#define USE_EVALUATOR

CEnemyManager::CEnemyManager(CCustomMonster* object)
{
    VERIFY(object);
    m_object = object;
    m_ignore_monster_threshold = 1.f;
    m_max_ignore_distance = 0.f;
    m_ready_to_save = true;
    m_last_enemy_time = 0;
    m_last_enemy_change = 0;
    m_stalker = smart_cast<CAI_Stalker*>(object);
    m_enable_enemy_change = true;
    m_smart_cover_enemy = 0;
    m_useful_callback_cache.reserve(ENEMY_USEFUL_CALLBACK_CACHE_SIZE);
}

bool CEnemyManager::try_get_useful_callback_cache(ALife::_OBJECT_ID enemy_id, bool& result) const
{
    const u32 now = Device.dwTimeGlobal;
    for (const SUsefulCallbackCacheEntry& entry : m_useful_callback_cache)
    {
        if (entry.enemy_id != enemy_id)
            continue;

        if ((now - entry.timestamp) > ENEMY_USEFUL_CALLBACK_CACHE_TTL_MS)
            return false;

        result = entry.result;
        return true;
    }

    return false;
}

void CEnemyManager::store_useful_callback_cache(ALife::_OBJECT_ID enemy_id, bool result) const
{
    const u32 now = Device.dwTimeGlobal;
    for (SUsefulCallbackCacheEntry& entry : m_useful_callback_cache)
    {
        if (entry.enemy_id != enemy_id)
            continue;

        entry.timestamp = now;
        entry.result = result;
        return;
    }

    if (m_useful_callback_cache.size() >= ENEMY_USEFUL_CALLBACK_CACHE_SIZE)
        m_useful_callback_cache.erase(m_useful_callback_cache.begin());

    m_useful_callback_cache.push_back({enemy_id, now, result});
}

void CEnemyManager::clear_useful_callback_cache()
{
    m_useful_callback_cache.clear();
}

void CEnemyManager::set_useful_callback(const luabind::object& functor)
{
    m_useful_callback.set(functor);
    clear_useful_callback_cache();
}

void CEnemyManager::set_useful_callback(const luabind::object& functor, const luabind::object& object)
{
    m_useful_callback.set(functor, object);
    clear_useful_callback_cache();
}

void CEnemyManager::clear_useful_callback()
{
    m_useful_callback.clear();
    clear_useful_callback_cache();
}

bool CEnemyManager::is_useful(const CEntityAlive* entity_alive) const { return (m_object->useful(this, entity_alive)); }
bool CEnemyManager::useful(const CEntityAlive* entity_alive) const
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulCheck);

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulAliveCheck);
        if (!entity_alive->g_Alive())
            return (false);
    }

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulSpatialCheck);
        if ((entity_alive->spatial.type & STYPE_VISIBLEFORAI) != STYPE_VISIBLEFORAI)
            return (false);
    }

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulRelationCheck);
        if ((m_object->ID() == entity_alive->ID()) || !m_object->is_relation_enemy(entity_alive))
            return (false);
    }

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulVertexCheck);
        if (!ai().get_level_graph() || !ai().level_graph().valid_vertex_id(entity_alive->ai_location().level_vertex_id()))
            return (false);
    }

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulMonsterFilter);
        if (m_object->human_being() && !entity_alive->human_being() && !expedient(entity_alive) &&
            (evaluate(entity_alive) >= m_ignore_monster_threshold) &&
            (m_object->Position().distance_to(entity_alive->Position()) >= m_max_ignore_distance))
            return (false);
    }

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyUsefulCallback);
        if (!m_useful_callback)
            return true;

        bool cached_result = false;
        if (try_get_useful_callback_cache(entity_alive->ID(), cached_result))
            return cached_result;

        const bool callback_result = m_useful_callback(m_object->lua_game_object(), entity_alive->lua_game_object());
        store_useful_callback_cache(entity_alive->ID(), callback_result);
        return callback_result;
    }
}

float CEnemyManager::do_evaluate(const CEntityAlive* object) const { return (m_object->evaluate(this, object)); }
float CEnemyManager::evaluate(const CEntityAlive* object) const
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyEvaluate);

    //	Msg						("[%6d] enemy manager %s evaluates
    //%s",Device.dwTimeGlobal,*m_object->cName(),*object->cName());

    bool actor = (!!smart_cast<CActor const*>(object));
    if (actor)
        m_ready_to_save = false;

    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(object);
    bool wounded = stalker ? stalker->wounded(&m_object->movement().restrictions()) : false;
    if (wounded)
    {
        if (m_stalker && m_stalker->agent_manager().enemy().assigned_wounded(object, m_stalker))
            return (0.f);

        float distance = m_object->Position().distance_to_sqr(object->Position());
        return (distance);
    }

    float penalty = 10000.f;

    // if we are hit
    if (object->ID() == m_object->memory().hit().last_hit_object_id())
    {
        if (actor)
            penalty -= 1500.f;
        else
            penalty -= 500.f;
    }

    // if we see object
    if (m_object->memory().visual().visible_now(object))
        penalty -= 1000.f;

// if object is actor and he/she sees us
//	if (actor) {
//		if (smart_cast<const CActor*>(object)->memory().visual().visible_now(m_object))
//			penalty			-= 900.f;
//	}
//	else {
//		// if object is npc and it sees us
//		const CCustomMonster	*monster = smart_cast<const CCustomMonster*>(object);
//		if (monster && monster->memory().visual().visible_now(m_object))
//			penalty			-= 300.f;
//	}

#ifdef USE_EVALUATOR
    ai().ef_storage().non_alife().member_item() = 0;
    ai().ef_storage().non_alife().enemy_item() = 0;
    ai().ef_storage().non_alife().member() = m_object;
    ai().ef_storage().non_alife().enemy() = object;

    float distance = m_object->Position().distance_to_sqr(object->Position());
    return (penalty + distance / 100.f + ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f);
#else // USE_EVALUATOR
    float distance = m_object->Position().distance_to_sqr(object->Position());
    return (1000.f * (visible ? 0.f : 1.f) + distance);
#endif // USE_EVALUATOR
}

bool CEnemyManager::expedient(const CEntityAlive* object) const
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyExpedient);

    ai().ef_storage().non_alife().member() = m_object;
    VERIFY(ai().ef_storage().non_alife().member());
    ai().ef_storage().non_alife().enemy() = object;

    if (ai().ef_storage().m_pfExpediency->dwfGetDiscreteValue())
        return (true);

    if (m_object->memory().hit().hit(ai().ef_storage().non_alife().enemy()))
        return (true);
    return (false);
}

void CEnemyManager::reload(LPCSTR section)
{
    m_ignore_monster_threshold = READ_IF_EXISTS(pSettings, r_float, section, "ignore_monster_threshold", 1.f);
    m_max_ignore_distance = READ_IF_EXISTS(pSettings, r_float, section, "max_ignore_distance", 0.f);
    m_last_enemy_time = 0;
    m_last_enemy = 0;
    m_last_enemy_change = 0;
    m_useful_callback.clear();
    clear_useful_callback_cache();
    VERIFY(m_ready_to_save);
}

void CEnemyManager::set_ready_to_save()
{
    if (m_ready_to_save)
        return;

    //	Msg							("%6d %s DEcreased enemy counter for player (%d ->
    //%d)",Device.dwTimeGlobal,*m_object->cName(),Level().autosave_manager().not_ready_count(),Level().autosave_manager().not_ready_count()-1);
    Level().autosave_manager().dec_not_ready();
    m_ready_to_save = true;
}

void CEnemyManager::remove_links(IGameObject* object)
{
    // since we use no members in CEntityAlive during search,
    // we just use the pinter itself, we can just statically cast object
    OBJECTS::iterator I = std::find(m_objects.begin(), m_objects.end(), (CEntityAlive*)object);
    if (I != m_objects.end())
        m_objects.erase(I);

    if (m_last_enemy == object)
        m_last_enemy = 0;

    if (m_selected == object)
        m_selected = 0;

    clear_useful_callback_cache();
}

void CEnemyManager::ignore_monster_threshold(const float& ignore_monster_threshold)
{
    m_ignore_monster_threshold = ignore_monster_threshold;
}

void CEnemyManager::restore_ignore_monster_threshold()
{
    m_ignore_monster_threshold =
        READ_IF_EXISTS(pSettings, r_float, m_object->cNameSect().c_str(), "ignore_monster_threshold", 1.f);
}

float CEnemyManager::ignore_monster_threshold() const { return (m_ignore_monster_threshold); }
void CEnemyManager::max_ignore_monster_distance(const float& max_ignore_monster_distance)
{
    m_max_ignore_distance = max_ignore_monster_distance;
}

void CEnemyManager::restore_max_ignore_monster_distance()
{
    m_max_ignore_distance = READ_IF_EXISTS(pSettings, r_float, m_object->cNameSect().c_str(), "max_ignore_distance", 0.f);
}

float CEnemyManager::max_ignore_monster_distance() const { return (m_max_ignore_distance); }
bool CEnemyManager::change_from_wounded(const CEntityAlive* current, const CEntityAlive* previous) const
{
    const CAI_Stalker* current_stalker = smart_cast<const CAI_Stalker*>(current);
    if (!current_stalker)
        return (false);

    if (current_stalker->wounded())
        return (false);

    const CAI_Stalker* previous_stalker = smart_cast<const CAI_Stalker*>(previous);
    if (!previous_stalker)
        return (false);

    if (!previous_stalker->wounded())
        return (false);

    return (true);
}

IC bool CEnemyManager::enemy_inertia(const CEntityAlive* previous_enemy) const
{
    if (smart_cast<CActor const*>(m_selected))
        return (Device.dwTimeGlobal <= (m_last_enemy_change + ENEMY_INERTIA_TIME_TO_ACTOR));

    if (previous_enemy && smart_cast<CActor const*>(previous_enemy))
        return (Device.dwTimeGlobal <= (m_last_enemy_change + ENEMY_INERTIA_TIME_FROM_ACTOR));

    return (Device.dwTimeGlobal <= (m_last_enemy_change + ENEMY_INERTIA_TIME_TO_SOMEBODY));
}

void CEnemyManager::on_enemy_change(const CEntityAlive* previous_enemy)
{
    clear_useful_callback_cache();

    VERIFY(previous_enemy);
    VERIFY(selected());

    if (!previous_enemy->g_Alive())
    {
        m_last_enemy_change = Device.dwTimeGlobal;
        return;
    }

    if (change_from_wounded(selected(), previous_enemy))
    {
        m_last_enemy_change = Device.dwTimeGlobal;
        return;
    }

    if (enemy_inertia(previous_enemy))
    {
        m_selected = previous_enemy;
        return;
    }

    if (!m_object->memory().visual().visible_now(previous_enemy) && m_object->memory().visual().visible_now(selected()))
    {
        m_last_enemy_change = Device.dwTimeGlobal;
        return;
    }

    m_last_enemy_change = Device.dwTimeGlobal;
}

void CEnemyManager::remove_wounded()
{
    struct no_wounded
    {
        IC static bool predicate(const CEntityAlive* enemy)
        {
            const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(enemy);
            if (!stalker)
                return (false);

            if (!stalker->wounded())
                return (false);

            return (true);
        }
    };

    m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), &no_wounded::predicate), m_objects.end());
}

void CEnemyManager::process_wounded(bool& only_wounded)
{
    only_wounded = true;
    ENEMIES::const_iterator I = m_objects.begin();
    ENEMIES::const_iterator E = m_objects.end();
    for (; I != E; ++I)
    {
        const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(*I);
        if (stalker && stalker->wounded())
            continue;

        only_wounded = false;
        break;
    }

    if (only_wounded)
    {
#if 0 // def _DEBUG
		if (g_enemy_manager_second_update)
			Msg					("%6d ONLY WOUNDED LEFT %s",Device.dwTimeGlobal,*m_object->cName());
#endif // _DEBUG
        return;
    }

    remove_wounded();
}

bool CEnemyManager::need_update(const bool& only_wounded) const
{
    if (!selected())
        return (true);

    if (!selected()->g_Alive())
        return (true);

    if (!m_object->is_relation_enemy(selected()))
        return (true);

    if (enable_enemy_change() && !m_object->memory().visual().visible_now(selected()))
        return (true);

    if (only_wounded)
        return (false);

    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(selected());
    if (stalker && stalker->wounded())
        return (true);

    u32 last_hit_time = m_object->memory().hit().last_hit_time();
    if (last_hit_time && (last_hit_time > m_last_enemy_change))
    {
        ALife::_OBJECT_ID enemy_id = m_object->memory().hit().last_hit_object_id();
        VERIFY(enemy_id != ALife::_OBJECT_ID(-1));
        IGameObject const* enemy = Level().Objects.net_Find(enemy_id);
        VERIFY(enemy);
        CEntityAlive const* alive_enemy = smart_cast<CEntityAlive const*>(enemy);
        if (alive_enemy && m_object->is_relation_enemy(alive_enemy))
            return (true);
    }

    //	if (Actor() && m_object->memory().visual().visible_now(Actor()))
    //		return					(true);

    return (false);
}

void CEnemyManager::try_change_enemy()
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyTryChange);

    const CEntityAlive* previous_selected = selected();

    bool only_wounded;
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyProcessWounded);
        process_wounded(only_wounded);
    }

    bool should_update = false;
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyNeedUpdate);
        should_update = need_update(only_wounded);
    }

    if (!should_update)
        return;

    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerEnemyInertedUpdate);
        inherited::update();
    }

    if (selected() != previous_selected)
    {
        if (selected() && previous_selected)
            on_enemy_change(previous_selected);
        else
            m_last_enemy_change = Device.dwTimeGlobal;
    }

    if (selected() != previous_selected)
        m_object->on_enemy_change(previous_selected);
}

void CEnemyManager::update()
{
    START_PROFILE("Memory Manager/enemies::update")

    if (!m_ready_to_save)
    {
        //		Msg						("%6d %s DEcreased enemy counter for player (%d ->
        //%d)",Device.dwTimeGlobal,*m_object->cName(),Level().autosave_manager().not_ready_count(),Level().autosave_manager().not_ready_count()-1);
        Level().autosave_manager().dec_not_ready();
    }

    m_ready_to_save = true;

    try_change_enemy();

    if (selected())
    {
        m_last_enemy_time = Device.dwTimeGlobal;
        m_last_enemy = selected();
    }

    if (!m_ready_to_save)
    {
        //		Msg						("%6d %s INcreased enemy counter for player (%d ->
        //%d)",Device.dwTimeGlobal,*m_object->cName(),Level().autosave_manager().not_ready_count(),Level().autosave_manager().not_ready_count()+1);
        Level().autosave_manager().inc_not_ready();
    }

#if 0 // def _DEBUG
	if (g_enemy_manager_second_update && selected() && smart_cast<const CAI_Stalker*>(selected()) && smart_cast<const CAI_Stalker*>(selected())->wounded())
		Msg						("%6d WOUNDED CHOOSED %s",Device.dwTimeGlobal,*m_object->cName());
#endif // _DEBUG

    STOP_PROFILE
}
