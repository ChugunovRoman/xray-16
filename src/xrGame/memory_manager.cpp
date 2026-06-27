////////////////////////////////////////////////////////////////////////////
//	Module 		: memory_manager.cpp
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Memory manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include <tracy/Tracy.hpp>
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include "enemy_manager.h"
#include "item_manager.h"
#include "danger_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "memory_space_impl.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrEngine/profiler.h"
#include "agent_enemy_manager.h"
#include "script_game_object.h"
#include "Actor.h"
#include "npc_cpp_profile.h"

CMemoryManager::CMemoryManager(CEntityAlive* entity_alive, CSound_UserDataVisitor* visitor)
{
    VERIFY(entity_alive);
    m_object = smart_cast<CCustomMonster*>(entity_alive);
    m_stalker = smart_cast<CAI_Stalker*>(m_object);

    if (m_stalker)
        m_visual = xr_new<CVisualMemoryManager>(m_stalker);
    else
        m_visual = xr_new<CVisualMemoryManager>(m_object);

    m_sound = xr_new<CSoundMemoryManager>(m_object, m_stalker, visitor);
    m_hit = xr_new<CHitMemoryManager>(m_object, m_stalker);
    m_enemy = xr_new<CEnemyManager>(m_object);
    m_item = xr_new<CItemManager>(m_object);
    m_danger = xr_new<CDangerManager>(m_object);
}

CMemoryManager::~CMemoryManager()
{
    xr_delete(m_visual);
    xr_delete(m_sound);
    xr_delete(m_hit);
    xr_delete(m_enemy);
    xr_delete(m_item);
    xr_delete(m_danger);
}

void CMemoryManager::Load(LPCSTR section)
{
    sound().Load(section);
    hit().Load(section);
    enemy().Load(section);
    item().Load(section);
    danger().Load(section);
}

void CMemoryManager::reinit()
{
    visual().reinit();
    sound().reinit();
    hit().reinit();
    enemy().reinit();
    item().reinit();
    danger().reinit();
}

void CMemoryManager::reload(LPCSTR section)
{
    visual().reload(section);
    sound().reload(section);
    hit().reload(section);
    enemy().reload(section);
    item().reload(section);
    danger().reload(section);
}

#ifdef _DEBUG
extern bool g_enemy_manager_second_update;
#endif // _DEBUG

namespace
{
constexpr u32 STALKER_MEMORY_VISUAL_COMBAT_BUDGET = 12;
constexpr u32 STALKER_MEMORY_SOUND_COMBAT_BUDGET = 8;
constexpr u32 STALKER_MEMORY_HIT_COMBAT_BUDGET = 6;
constexpr float STALKER_MEMORY_FULL_COLLECT_NEAR_DIST_SQR = 55.f * 55.f;

template <typename TStage>
IC void add_profile_counter(const TStage stage, const u64 start_qpc)
{
    if (!npc_cpp_profile::enabled())
        return;

    npc_cpp_profile::add(stage, CPU::QPC() - start_qpc);
}

IC u32 effective_memory_collect_budget(
    const bool limited_mode, const u32 object_count, const u32 budget, const float lod_multiplier)
{
    if (!limited_mode || !object_count)
        return object_count;

    const u32 scaled_budget = u32(_max(1.0f, budget * lod_multiplier));
    return _min(object_count, scaled_budget);
}

IC bool should_force_full_memory_collect(const CAI_Stalker* stalker, const bool registered_in_combat)
{
    if (!stalker)
        return false;

    if (!registered_in_combat && !stalker->memory().enemy().selected() && !stalker->memory().danger().selected())
        return false;

    CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!actor)
        return false;

    return stalker->Position().distance_to_sqr(actor->Position()) <= STALKER_MEMORY_FULL_COLLECT_NEAR_DIST_SQR;
}
} // namespace

void CMemoryManager::update_enemies(const bool& registered_in_combat)
{
#ifdef _DEBUG
    g_enemy_manager_second_update = false;
#endif // _DEBUG
    {
        ZoneNamedN(___tracy_ue_pass1, "CMemoryManager::update_enemies/first_pass", true);
        enemy().update();
    }

    if (m_stalker && (!enemy().selected() || (smart_cast<const CAI_Stalker*>(enemy().selected()) &&
                                                 smart_cast<const CAI_Stalker*>(enemy().selected())->wounded())) &&
        registered_in_combat)
    {
        {
            ZoneNamedN(___tracy_ue_dist, "CMemoryManager::update_enemies/distribute_enemies", true);
            m_stalker->agent_manager().enemy().distribute_enemies();
        }

        {
            ZoneNamedN(___tracy_ue_recol, "CMemoryManager::update_enemies/re_collect_combat", true);
            if (visual().enabled())
                update(visual().objects(), true, false, m_visual_update_cursor, STALKER_MEMORY_VISUAL_COMBAT_BUDGET);

            update(sound().objects(), true, false, m_sound_update_cursor, STALKER_MEMORY_SOUND_COMBAT_BUDGET);
            update(hit().objects(), true, false, m_hit_update_cursor, STALKER_MEMORY_HIT_COMBAT_BUDGET);
        }

#ifdef _DEBUG
        g_enemy_manager_second_update = true;
#endif // _DEBUG
        {
            ZoneNamedN(___tracy_ue_pass2, "CMemoryManager::update_enemies/second_pass", true);
            enemy().update();
        }
    }
}

void CMemoryManager::update(float time_delta)
{
    ZoneScopedN("CMemoryManager::update");
    START_PROFILE("Memory Manager")

    {
        ZoneNamedN(___tracy_mm_visual, "CMemoryManager::update/visual", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryVisualUpdate);
        visual().update(time_delta);
    }
    {
        ZoneNamedN(___tracy_mm_sound, "CMemoryManager::update/sound", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemorySoundUpdate);
        sound().update();
    }
    {
        ZoneNamedN(___tracy_mm_hit, "CMemoryManager::update/hit", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryHitUpdate);
        hit().update();
    }

    bool registered_in_combat = false;
    {
        ZoneNamedN(___tracy_mm_combat, "CMemoryManager::update/combat_flags", true);
        if (m_stalker)
            registered_in_combat = m_stalker->agent_manager().member().registered_in_combat(m_stalker);
    }
    const bool process_items = !registered_in_combat && !enemy().selected();
    const bool limited_collect_mode =
        (registered_in_combat || !!enemy().selected()) && !should_force_full_memory_collect(m_stalker, registered_in_combat);

    // Distance-based multiplier for combat memory budgets; near/combat NPCs stay at 1.0.
    const float lod_multiplier = m_stalker ? m_stalker->memory_collect_budget_multiplier() : 1.0f;

    // update enemies and items
    {
        ZoneNamedN(___tracy_mm_reset, "CMemoryManager::update/reset_enemy_item", true);
        enemy().reset();
        item().reset();
    }

    {
        ZoneNamedN(___tracy_mm_collect, "CMemoryManager::update/collect", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryCollectObjects);
        if (visual().enabled())
            update(visual().objects(), true, process_items, m_visual_update_cursor,
                effective_memory_collect_budget(limited_collect_mode, visual().objects().size(), STALKER_MEMORY_VISUAL_COMBAT_BUDGET, lod_multiplier));

        update(sound().objects(), registered_in_combat ? true : false, process_items, m_sound_update_cursor,
            effective_memory_collect_budget(limited_collect_mode, sound().objects().size(), STALKER_MEMORY_SOUND_COMBAT_BUDGET, lod_multiplier));
        update(hit().objects(), registered_in_combat ? true : false, process_items, m_hit_update_cursor,
            effective_memory_collect_budget(limited_collect_mode, hit().objects().size(), STALKER_MEMORY_HIT_COMBAT_BUDGET, lod_multiplier));
    }

    {
        ZoneNamedN(___tracy_mm_upd_en, "CMemoryManager::update/update_enemies", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryUpdateEnemies);
        update_enemies(registered_in_combat);
    }
    if (process_items)
    {
        ZoneNamedN(___tracy_mm_item, "CMemoryManager::update/item", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryItemUpdate);
        item().update();
    }
    {
        ZoneNamedN(___tracy_mm_danger, "CMemoryManager::update/danger", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryDangerUpdate);
        danger().update();
    }

    STOP_PROFILE
}

void CMemoryManager::enable(const IGameObject* object, bool enable)
{
    visual().enable(object, enable);
    sound().enable(object, enable);
    hit().enable(object, enable);
}

template <typename T>
void CMemoryManager::update(const xr_vector<T>& objects, bool add_enemies, bool add_items, u32& cursor, u32 budget)
{
    const bool profile_enabled = npc_cpp_profile::enabled();
    const u64 total_start_qpc = profile_enabled ? CPU::QPC() : 0;
    ENpcCppProfileStage collect_stage = ENpcCppProfileStage::StalkerMemoryCollectObjects;

    if constexpr (std::is_same_v<T, CVisibleObject>)
        collect_stage = ENpcCppProfileStage::StalkerMemoryCollectVisualObjects;
    else if constexpr (std::is_same_v<T, CSoundObject>)
        collect_stage = ENpcCppProfileStage::StalkerMemoryCollectSoundObjects;
    else if constexpr (std::is_same_v<T, CHitObject>)
        collect_stage = ENpcCppProfileStage::StalkerMemoryCollectHitObjects;

    squad_mask_type mask = m_stalker ? m_stalker->agent_manager().member().mask(m_stalker) : 0;
    const u32 object_count = objects.size();
    if (!object_count || !budget)
    {
        add_profile_counter(collect_stage, total_start_qpc);
        return;
    }

    if (cursor >= object_count)
        cursor %= object_count;

    u32 index = cursor;
    const bool skip_stalker_items = add_items && !!m_stalker;
    for (u32 i = 0; i < budget; ++i)
    {
        const T& object = objects[index];
        if (!object.m_enabled)
            goto advance_cursor;

        if (m_stalker && !object.m_squad_mask.test(mask))
            goto advance_cursor;

        if (profile_enabled)
        {
            const u64 start_qpc = CPU::QPC();
            danger().add(object);
            npc_cpp_profile::add(ENpcCppProfileStage::StalkerMemoryCollectDangerAdd, CPU::QPC() - start_qpc);
        }
        else
        {
            danger().add(object);
        }

        if (add_enemies)
        {
            const CEntityAlive* entity_alive = smart_cast<const CEntityAlive*>(object.m_object);
            if (entity_alive)
            {
                bool added_enemy = false;
                if (profile_enabled)
                {
                    const u64 start_qpc = CPU::QPC();
                    added_enemy = enemy().add(entity_alive);
                    npc_cpp_profile::add(ENpcCppProfileStage::StalkerMemoryCollectEnemyAdd, CPU::QPC() - start_qpc);
                }
                else
                {
                    added_enemy = enemy().add(entity_alive);
                }

                if (added_enemy)
                    goto advance_cursor;
            }
        }

        if (skip_stalker_items)
        {
            const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(object.m_object);
            if (stalker)
                goto advance_cursor;
        }

        if (add_items && object.m_object)
        {
            if (profile_enabled)
            {
                const u64 start_qpc = CPU::QPC();
                item().add(object.m_object);
                npc_cpp_profile::add(ENpcCppProfileStage::StalkerMemoryCollectItemAdd, CPU::QPC() - start_qpc);
            }
            else
            {
                item().add(object.m_object);
            }
        }

advance_cursor:
        ++index;
        if (index == object_count)
            index = 0;
    }

    cursor = index;
    add_profile_counter(collect_stage, total_start_qpc);
}

CMemoryInfo CMemoryManager::memory(const IGameObject* object) const
{
    CMemoryInfo result;
    if (!this->object().g_Alive())
        return (result);

    u32 level_time = 0;
    const CGameObject* game_object = smart_cast<const CGameObject*>(object);
    VERIFY(game_object);
    squad_mask_type mask = m_stalker ? m_stalker->agent_manager().member().mask(m_stalker) : squad_mask_type(-1);

    {
        xr_vector<CVisibleObject>::const_iterator I =
            std::find(visual().objects().begin(), visual().objects().end(), object_id(object));
        if (visual().objects().end() != I)
        {
            (CMemoryObject<CGameObject>&)result = (CMemoryObject<CGameObject>&)(*I);
            [[maybe_unused]] const bool isVisible = result.visible((*I).visible(mask)); // XXX: this may be wrong, maybe code author wanted to SET visibility, not GET???
            result.m_visual_info = true;
            level_time = (*I).m_level_time;
            VERIFY(result.m_object);
        }
    }

    {
        xr_vector<CSoundObject>::const_iterator I =
            std::find(sound().objects().begin(), sound().objects().end(), object_id(object));
        if ((sound().objects().end() != I) && (level_time < (*I).m_level_time))
        {
            (CMemoryObject<CGameObject>&)result = (CMemoryObject<CGameObject>&)(*I);
            result.m_sound_info = true;
            level_time = (*I).m_level_time;
            VERIFY(result.m_object);
        }
    }

    {
        xr_vector<CHitObject>::const_iterator I =
            std::find(hit().objects().begin(), hit().objects().end(), object_id(object));
        if ((hit().objects().end() != I) && (level_time < (*I).m_level_time))
        {
            (CMemoryObject<CGameObject>&)result = (CMemoryObject<CGameObject>&)(*I);
            result.m_object = game_object;
            result.m_hit_info = true;
            VERIFY(result.m_object);
        }
    }

    return (result);
}

u32 CMemoryManager::memory_time(const IGameObject* object) const
{
    u32 result = 0;
    if (!this->object().g_Alive())
        return (0);

    [[maybe_unused]] auto game_object = smart_cast<const CGameObject*>(object);
    VERIFY(game_object);

    {
        xr_vector<CVisibleObject>::const_iterator I =
            std::find(visual().objects().begin(), visual().objects().end(), object_id(object));
        if (visual().objects().end() != I)
            result = (*I).m_level_time;
    }

    {
        xr_vector<CSoundObject>::const_iterator I =
            std::find(sound().objects().begin(), sound().objects().end(), object_id(object));
        if ((sound().objects().end() != I) && (result < (*I).m_level_time))
            result = (*I).m_level_time;
    }

    {
        xr_vector<CHitObject>::const_iterator I =
            std::find(hit().objects().begin(), hit().objects().end(), object_id(object));
        if ((hit().objects().end() != I) && (result < (*I).m_level_time))
            result = (*I).m_level_time;
    }

    return (result);
}

Fvector CMemoryManager::memory_position(const IGameObject* object) const
{
    u32 time = 0;
    Fvector result = Fvector().set(0.f, 0.f, 0.f);
    if (!this->object().g_Alive())
        return (result);

    [[maybe_unused]] auto game_object = smart_cast<const CGameObject*>(object);
    VERIFY(game_object);

    {
        xr_vector<CVisibleObject>::const_iterator I =
            std::find(visual().objects().begin(), visual().objects().end(), object_id(object));
        if (visual().objects().end() != I)
        {
            time = (*I).m_level_time;
            result = (*I).m_object_params.m_position;
        }
    }

    {
        xr_vector<CSoundObject>::const_iterator I =
            std::find(sound().objects().begin(), sound().objects().end(), object_id(object));
        if ((sound().objects().end() != I) && (time < (*I).m_level_time))
        {
            time = (*I).m_level_time;
            result = (*I).m_object_params.m_position;
        }
    }

    {
        xr_vector<CHitObject>::const_iterator I =
            std::find(hit().objects().begin(), hit().objects().end(), object_id(object));
        if ((hit().objects().end() != I) && (time < (*I).m_level_time))
        {
            time = (*I).m_level_time;
            result = (*I).m_object_params.m_position;
        }
    }

    return (result);
}

void CMemoryManager::remove_links(IGameObject* object)
{
    if (m_object->g_Alive())
    {
        visual().remove_links(object);
        sound().remove_links(object);
        hit().remove_links(object);
    }

    danger().remove_links(object);
    enemy().remove_links(object);
    item().remove_links(object);
}

void CMemoryManager::on_restrictions_change()
{
    if (!m_object->g_Alive())
        return;

    //	danger().on_restrictions_change	();
    //	enemy().on_restrictions_change	();
    item().on_restrictions_change();
}

void CMemoryManager::make_object_visible_somewhen(const CEntityAlive* enemy)
{
    squad_mask_type mask = stalker().agent_manager().member().mask(&stalker());
    MemorySpace::CVisibleObject* obj = visual().visible_object(enemy);
    //	if (obj) {
    //		Msg						("------------------------------------------------------");
    //		Msg						("[%6d] make_object_visible_somewhen [%s] =
    //%x",Device.dwTimeGlobal,*enemy->cName(),obj->m_squad_mask.get());
    //	}
    //	LogStackTrace				("-------------make_object_visible_somewhen-------------");
    bool prev = obj ? obj->visible(mask) : false;
    visual().add_visible_object(enemy, .001f, true);
    MemorySpace::CVisibleObject* obj1 = object().memory().visual().visible_object(enemy);
    VERIFY(obj1);
    //	if (obj1)
    //		Msg						("[%6d] make_object_visible_somewhen [%s] =
    //%x",Device.dwTimeGlobal,*enemy->cName(),obj1->m_squad_mask.get());
    obj1->visible(mask, prev);
}

void CMemoryManager::save(NET_Packet& packet) const
{
    visual().save(packet);
    sound().save(packet);
    hit().save(packet);
    danger().save(packet);
}

void CMemoryManager::load(IReader& packet)
{
    visual().load(packet);
    sound().load(packet);
    hit().load(packet);
    danger().load(packet);
}

// we do this due to the limitation of client spawn manager
// should be revisited from the acrhitectural point of view
void CMemoryManager::on_requested_spawn(IGameObject* object)
{
    visual().on_requested_spawn(object);
    sound().on_requested_spawn(object);
    hit().on_requested_spawn(object);
}
