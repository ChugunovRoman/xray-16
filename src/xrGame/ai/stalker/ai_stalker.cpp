////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker.cpp
//	Created 	: 25.02.2003
//  Modified 	: 25.02.2003
//	Author		: Dmitriy Iassenev
//	Description : AI Behaviour for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_stalker.h"
#include "ai/ai_monsters_misc.h"
#include "Weapon.h"
#include "Hit.h"
#include "PHDestroyable.h"
#include "CharacterPhysicsSupport.h"
#include "script_entity_action.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrAICore/Navigation/game_graph.h"
#include "Inventory.h"
#include "Artefact.h"
#include "PHMovementControl.h"
#include "xrServerEntities/xrServer_Objects_ALife_Monsters.h"
#include "cover_evaluators.h"
#include "xrServer.h"
#include "xrEngine/xr_level_controller.h"
#include "Include/xrRender/Kinematics.h"
#include "xrServerEntities/character_info.h"
#include "Actor.h"
#include "relation_registry.h"
#include "stalker_animation_manager.h"
#include "stalker_planner.h"
#include "script_game_object.h"
#include "detail_path_manager.h"
#include "agent_manager.h"
#include "agent_corpse_manager.h"
#include "object_handler_planner.h"
#include "object_handler_space.h"
#include "memory_manager.h"
#include "sight_manager.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/ai_object_location_impl.h"
#include "stalker_movement_manager_smart_cover.h"
#include "EntityCondition.h"
#include "xrScriptEngine/script_engine.hpp"
#include "ai_stalker_impl.h"
#include "sound_player.h"
#include "stalker_sound_data.h"
#include "stalker_sound_data_visitor.h"
#include "ai_stalker_space.h"
#include "mt_config.h"
#include <tracy/Tracy.hpp>
#include "EffectorShot.h"
#include "visual_memory_manager.h"
#include "enemy_manager.h"
#include "hit_memory_manager.h"
#include "xrServerEntities/alife_human_brain.h"
#include "xrEngine/profiler.h"
#include "npc_cpp_profile.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "performance_cvars.h"
#include "BoneProtections.h"
#include "stalker_animation_names.h"
#include "stalker_decision_space.h"
#include "agent_member_manager.h"
#include "location_manager.h"
#include "smart_cover_animation_selector.h"
#include "smart_cover_animation_planner.h"
#include "smart_cover_planner_target_selector.h"

#ifdef DEBUG
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "Level.h"
#include "map_location.h"
#include "map_manager.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#endif // DEBUG

using namespace StalkerSpace;

namespace
{
constexpr float STALKER_LOD_NEAR_DIST_SQR = 35.f * 35.f;
constexpr float STALKER_LOD_MEDIUM_DIST_SQR = 80.f * 80.f;
constexpr u32 STALKER_THINK_INTERVAL_NEAR_IDLE_MS = 75;
constexpr u32 STALKER_THINK_INTERVAL_MEDIUM_MS = 120;
constexpr u32 STALKER_THINK_INTERVAL_FAR_MS = 280;
// VIS intervals from npc_perf_stalker_vis_interval_* (user.ltx)
constexpr u32 STALKER_AGENT_MANAGER_INTERVAL_MEDIUM_MS = 150;
constexpr u32 STALKER_AGENT_MANAGER_INTERVAL_FAR_MS = 400;
constexpr u32 STALKER_MEMORY_UPDATE_INTERVAL_MEDIUM_MS = 175;
constexpr u32 STALKER_MEMORY_UPDATE_INTERVAL_FAR_MS = 600;
// P6-opt: feel_touch_update throttle for FAR/MEDIUM (NPC_OPTIMIZATION_TECHNICAL_BRIEF_2026-03-17)
constexpr u32 STALKER_FEEL_TOUCH_INTERVAL_MEDIUM_MS = 200;
constexpr u32 STALKER_FEEL_TOUCH_INTERVAL_FAR_MS = 450;

// P1-opt: separate longer interval for enemy-scan phase of memory update for
// non-combat FAR NPCs. When combat starts (memory().enemy().selected()),
// LOD switches to Near and interval drops to 0 automatically.
constexpr u32 STALKER_ENEMY_SCAN_INTERVAL_FAR_MS   = 900;
constexpr u32 STALKER_ENEMY_SCAN_INTERVAL_MEDIUM_MS = 250;

IC u32 stalker_lod_offset(const u16 object_id, const u32 interval_ms)
{
    if (!interval_ms)
        return 0;

    constexpr u32 bucket_count = 8;
    const u32 buckets = _min(bucket_count, interval_ms);
    return (interval_ms * (object_id % buckets)) / buckets;
}

IC EStalkerUpdateLod get_stalker_update_lod(const CAI_Stalker& stalker)
{
#ifdef DEBUG
    if (stalker.ShouldProcessOnRender())
        return EStalkerUpdateLod::Near;
#endif

    if (stalker.memory().enemy().selected())
        return EStalkerUpdateLod::Near;

    CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!actor)
        return EStalkerUpdateLod::Near;

    const float dist_sqr = stalker.Position().distance_to_sqr(actor->Position());
    if (dist_sqr <= STALKER_LOD_NEAR_DIST_SQR)
        return EStalkerUpdateLod::Near;
    if (dist_sqr <= STALKER_LOD_MEDIUM_DIST_SQR)
        return EStalkerUpdateLod::Medium;

    return EStalkerUpdateLod::Far;
}

IC u32 get_stalker_think_interval(const CAI_Stalker& stalker, const EStalkerUpdateLod lod)
{
    if (lod == EStalkerUpdateLod::Near && stalker.is_nearby_idle_state_optimization_candidate())
        return STALKER_THINK_INTERVAL_NEAR_IDLE_MS;

    switch (lod)
    {
    case EStalkerUpdateLod::Near: return 0;
    case EStalkerUpdateLod::Medium: return STALKER_THINK_INTERVAL_MEDIUM_MS;
    case EStalkerUpdateLod::Far: return STALKER_THINK_INTERVAL_FAR_MS;
    default: return 0;
    }
}

IC u32 get_stalker_visibility_interval(const CAI_Stalker& stalker, const EStalkerUpdateLod lod)
{
    switch (lod)
    {
    case EStalkerUpdateLod::Near:
        if (stalker.memory().enemy().selected())
            return 0;
        return npc_perf_stalker_vis_interval_near_ms;
    case EStalkerUpdateLod::Medium: return npc_perf_stalker_vis_interval_medium_ms;
    case EStalkerUpdateLod::Far: return npc_perf_stalker_vis_interval_far_ms;
    default: return 0;
    }
}

IC u32 get_stalker_agent_manager_interval(const EStalkerUpdateLod lod)
{
    switch (lod)
    {
    case EStalkerUpdateLod::Near: return 0;
    case EStalkerUpdateLod::Medium: return STALKER_AGENT_MANAGER_INTERVAL_MEDIUM_MS;
    case EStalkerUpdateLod::Far: return STALKER_AGENT_MANAGER_INTERVAL_FAR_MS;
    default: return 0;
    }
}

IC u32 get_stalker_memory_update_interval(const EStalkerUpdateLod lod)
{
    switch (lod)
    {
    case EStalkerUpdateLod::Near: return 0;
    case EStalkerUpdateLod::Medium: return STALKER_MEMORY_UPDATE_INTERVAL_MEDIUM_MS;
    case EStalkerUpdateLod::Far: return STALKER_MEMORY_UPDATE_INTERVAL_FAR_MS;
    default: return 0;
    }
}

IC u32 get_stalker_feel_touch_interval(const EStalkerUpdateLod lod)
{
    switch (lod)
    {
    case EStalkerUpdateLod::Near: return 0;
    case EStalkerUpdateLod::Medium: return STALKER_FEEL_TOUCH_INTERVAL_MEDIUM_MS;
    case EStalkerUpdateLod::Far: return STALKER_FEEL_TOUCH_INTERVAL_FAR_MS;
    default: return 0;
    }
}

IC u32 get_stalker_enemy_scan_interval(const CAI_Stalker& stalker, const EStalkerUpdateLod lod)
{
    // Enemy scan is cheap only when there are no enemies; throttle far/medium non-combat NPCs.
    if (stalker.memory().enemy().selected())
        return 0;

    switch (lod)
    {
    case EStalkerUpdateLod::Near: return 0;
    case EStalkerUpdateLod::Medium: return STALKER_ENEMY_SCAN_INTERVAL_MEDIUM_MS;
    case EStalkerUpdateLod::Far: return STALKER_ENEMY_SCAN_INTERVAL_FAR_MS;
    default: return 0;
    }
}

IC bool should_run_stalker_lod_stage(
    const CAI_Stalker& stalker, u32& next_update_time, u32& current_interval, const u32 new_interval, const u32 now_ms)
{
    if (!new_interval)
    {
        current_interval = 0;
        next_update_time = now_ms;
        return true;
    }

    if (current_interval != new_interval)
    {
        current_interval = new_interval;
        next_update_time = now_ms + stalker_lod_offset(stalker.ID(), new_interval);
        return next_update_time <= now_ms;
    }

    if (next_update_time > now_ms)
        return false;

    next_update_time = now_ms + new_interval;
    return true;
}
} // namespace

IC EStalkerUpdateLod CAI_Stalker::get_cached_update_lod() const
{
    if (m_cached_lod_frame != Device.dwFrame)
    {
        m_cached_lod = get_stalker_update_lod(*this);
        m_cached_lod_frame = Device.dwFrame;
    }
    return m_cached_lod;
}

extern int g_AI_inactive_time;

CAI_Stalker::CAI_Stalker()
    : m_sniper_update_rate(false), m_sniper_fire_mode(false), m_take_items_enabled(true), m_death_sound_enabled(true)
{
    m_sound_user_data_visitor = 0;
    m_movement_manager = 0;
    m_group_behaviour = true;
    m_boneHitProtection = NULL;
    m_power_fx_factor = flt_max;
    m_wounded = false;
#ifdef DEBUG
    m_debug_planner = 0;
    m_dbg_hud_draw = false;
#endif // DEBUG
    m_registered_in_combat_on_migration = false;
}

CAI_Stalker::~CAI_Stalker()
{
    xr_delete(m_pPhysics_support);
    xr_delete(m_animation_manager);
    xr_delete(m_brain);
    xr_delete(m_sight_manager);
    xr_delete(m_weapon_shot_effector);
    xr_delete(m_sound_user_data_visitor);
}

void CAI_Stalker::reinit()
{
    CObjectHandler::reinit(this);
    sight().reinit();
    CCustomMonster::reinit();
    animation().reinit();
    //	movement().reinit				();

    //загрузка спецевической звуковой схемы для сталкера согласно m_SpecificCharacter
    sound().sound_prefix(SpecificCharacter().sound_voice_prefix());

    LoadSounds(cNameSect().c_str());

    m_pPhysics_support->in_Init();

    m_best_item_to_kill = 0;
    m_best_item_value = 0.f;
    m_best_ammo = 0;
    m_best_found_item_to_kill = 0;
    m_best_found_ammo = 0;
    m_item_actuality = false;
    m_sell_info_actuality = false;

    m_ce_close = xr_new<CCoverEvaluatorCloseToEnemy>(&movement().restrictions());
    m_ce_far = xr_new<CCoverEvaluatorFarFromEnemy>(&movement().restrictions());
    m_ce_best = xr_new<CCoverEvaluatorBest>(&movement().restrictions());
    m_ce_angle = xr_new<CCoverEvaluatorAngle>(&movement().restrictions());
    m_ce_safe = xr_new<CCoverEvaluatorSafe>(&movement().restrictions());
    m_ce_ambush = xr_new<CCoverEvaluatorAmbush>(&movement().restrictions());

    m_ce_close->set_inertia(3000);
    m_ce_far->set_inertia(3000);
    m_ce_best->set_inertia(1000);
    m_ce_angle->set_inertia(5000);
    m_ce_safe->set_inertia(1000);
    m_ce_ambush->set_inertia(3000);

    m_can_kill_enemy = false;
    m_can_kill_member = false;
    m_pick_distance = 0.f;
    m_pick_frame_id = 0;
    m_pick_time = 0;
    m_object_handler_lod_last_time = 0;
    m_object_handler_lod_skips = u32(-1);
    m_dead_shed_lod_applied = false;

    m_weapon_shot_random_seed = s32(Level().timeServer_Async());

    m_best_cover = 0;
    m_best_cover_actual = false;
    m_best_cover_value = flt_max;

    m_throw_actual = false;
    m_computed_object_position = Fvector().set(flt_max, flt_max, flt_max);
    m_computed_object_direction = Fvector().set(flt_max, flt_max, flt_max);

    m_throw_target_position = Fvector().set(flt_max, flt_max, flt_max);
    m_throw_ignore_object = 0;
    m_throw_ignore_object_id = 0;

    m_throw_position = Fvector().set(flt_max, flt_max, flt_max);
    m_throw_velocity = Fvector().set(flt_max, flt_max, flt_max);

    m_throw_collide_position = Fvector().set(flt_max, flt_max, flt_max);
    m_throw_enabled = false;

    m_last_throw_time = 0;

    m_can_throw_grenades = true;
    m_throw_time_interval = 20000;

    brain().CStalkerPlanner::m_storage.set_property(StalkerDecisionSpace::eWorldPropertyCriticallyWounded, false);

    {
        m_critical_wound_weights.clear();
        //		LPCSTR							weights = pSettings->r_string(cNameSect(),"critical_wound_weights");
        LPCSTR weights = SpecificCharacter().critical_wound_weights();
        string16 temp;
        for (int i = 0, n = _GetItemCount(weights); i < n; ++i)
            m_critical_wound_weights.push_back((float)atof(_GetItem(weights, i, temp)));
    }

    m_update_rotation_on_frame = false;
    
    // Initialize LOD throttling timers
    m_next_agent_manager_update_time = 0;
    m_agent_manager_update_interval = 0;
    m_next_memory_update_time = 0;
    m_memory_update_interval = 0;
    m_next_process_enemies_update_time = 0;
    m_process_enemies_update_interval = 0;

}

void CAI_Stalker::LoadSounds(LPCSTR section)
{
    LPCSTR head_bone_name = pSettings->r_string(section, "bone_head");
    sound().add(pSettings->r_string(section, "sound_death"), 100, SOUND_TYPE_MONSTER_DYING, 0,
        u32(eStalkerSoundMaskDie), eStalkerSoundDie, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_anomaly_death"), 100, SOUND_TYPE_MONSTER_DYING, 0,
        u32(eStalkerSoundMaskDieInAnomaly), eStalkerSoundDieInAnomaly, head_bone_name, 0);
    sound().add(pSettings->r_string(section, "sound_hit"), 100, SOUND_TYPE_MONSTER_INJURING, 1,
        u32(eStalkerSoundMaskInjuring), eStalkerSoundInjuring, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_friendly_fire"), 100, SOUND_TYPE_MONSTER_INJURING, 1,
        u32(eStalkerSoundMaskInjuringByFriend), eStalkerSoundInjuringByFriend, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_panic_human"), 100, SOUND_TYPE_MONSTER_TALKING, 2,
        u32(eStalkerSoundMaskPanicHuman), eStalkerSoundPanicHuman, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_panic_monster"), 100, SOUND_TYPE_MONSTER_TALKING, 2,
        u32(eStalkerSoundMaskPanicMonster), eStalkerSoundPanicMonster, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 3,
        u32(eStalkerSoundMaskGrenadeAlarm), eStalkerSoundGrenadeAlarm, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_friendly_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 3,
        u32(eStalkerSoundMaskFriendlyGrenadeAlarm), eStalkerSoundFriendlyGrenadeAlarm, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_tolls"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
        u32(eStalkerSoundMaskTolls), eStalkerSoundTolls, head_bone_name, xr_new<CStalkerSoundData>(this));
    if (pSettings->line_exist(section, "sound_wounded"))
    {
        sound().add(pSettings->r_string(section, "sound_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
            u32(eStalkerSoundMaskWounded), eStalkerSoundWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
    }
    else
    {
        sound().add(pSettings->r_string(section, "sound_tolls"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
            u32(eStalkerSoundMaskWounded), eStalkerSoundWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
    }
    sound().add(pSettings->r_string(section, "sound_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskAlarm), eStalkerSoundAlarm, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_attack_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskAttackNoAllies), eStalkerSoundAttackNoAllies, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_attack_allies_single_enemy"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskAttackAlliesSingleEnemy), eStalkerSoundAttackAlliesSingleEnemy, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_attack_allies_several_enemies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskAttackAlliesSeveralEnemies), eStalkerSoundAttackAlliesSeveralEnemies, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_backup"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskBackup), eStalkerSoundBackup, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_detour"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskDetour), eStalkerSoundDetour, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_search1_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskSearch1NoAllies), eStalkerSoundSearch1NoAllies, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_search1_with_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskSearch1WithAllies), eStalkerSoundSearch1WithAllies, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    if (pSettings->line_exist(section, "sound_enemy_lost_no_allies"))
    {
        sound().add(pSettings->r_string(section, "sound_enemy_lost_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
            u32(eStalkerSoundMaskEnemyLostNoAllies), eStalkerSoundEnemyLostNoAllies, head_bone_name,
            xr_new<CStalkerSoundData>(this));
    }
    else
    {
        sound().add(pSettings->r_string(section, "sound_search1_no_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
            u32(eStalkerSoundMaskEnemyLostNoAllies), eStalkerSoundEnemyLostNoAllies, head_bone_name,
            xr_new<CStalkerSoundData>(this));
    }
    if (pSettings->line_exist(section, "sound_enemy_lost_with_allies"))
    {
        sound().add(pSettings->r_string(section, "sound_enemy_lost_with_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
            u32(eStalkerSoundMaskEnemyLostWithAllies), eStalkerSoundEnemyLostWithAllies, head_bone_name,
            xr_new<CStalkerSoundData>(this));
    }
    else
    {
        sound().add(pSettings->r_string(section, "sound_search1_with_allies"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
            u32(eStalkerSoundMaskEnemyLostWithAllies), eStalkerSoundEnemyLostWithAllies, head_bone_name,
            xr_new<CStalkerSoundData>(this));
    }
    sound().add(pSettings->r_string(section, "sound_humming"), 100, SOUND_TYPE_MONSTER_TALKING, 6,
        u32(eStalkerSoundMaskHumming), eStalkerSoundHumming, head_bone_name, 0);
    sound().add(pSettings->r_string(section, "sound_need_backup"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
        u32(eStalkerSoundMaskNeedBackup), eStalkerSoundNeedBackup, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_running_in_danger"), 100, SOUND_TYPE_MONSTER_TALKING, 6,
        u32(eStalkerSoundMaskMovingInDanger), eStalkerSoundRunningInDanger, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    //	sound().add						(pSettings->r_string(section,"sound_walking_in_danger"),			100,
    // SOUND_TYPE_MONSTER_TALKING,	6, u32(eStalkerSoundMaskMovingInDanger),			eStalkerSoundWalkingInDanger,
    // head_bone_name, new CStalkerSoundData(this));
    sound().add(pSettings->r_string(section, "sound_kill_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
        u32(eStalkerSoundMaskKillWounded), eStalkerSoundKillWounded, head_bone_name, xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_enemy_critically_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
        u32(eStalkerSoundMaskEnemyCriticallyWounded), eStalkerSoundEnemyCriticallyWounded, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    sound().add(pSettings->r_string(section, "sound_enemy_killed_or_wounded"), 100, SOUND_TYPE_MONSTER_TALKING, 4,
        u32(eStalkerSoundMaskEnemyKilledOrWounded), eStalkerSoundEnemyKilledOrWounded, head_bone_name,
        xr_new<CStalkerSoundData>(this));
    if (pSettings->line_exist(section, "sound_throw_grenade"))
    {
        sound().add(pSettings->r_string(section, "sound_throw_grenade"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
            u32(eStalkerSoundMaskKillWounded), eStalkerSoundThrowGrenade, head_bone_name, xr_new<CStalkerSoundData>(this));
    }
    else
    {
        sound().add(pSettings->r_string(section, "sound_grenade_alarm"), 100, SOUND_TYPE_MONSTER_TALKING, 5,
            u32(eStalkerSoundMaskKillWounded), eStalkerSoundThrowGrenade, head_bone_name, xr_new<CStalkerSoundData>(this));
    }
}

void CAI_Stalker::reload(LPCSTR section)
{
    brain().setup(this);

    CCustomMonster::reload(section);
    CStepManager::reload(section);
    CObjectHandler::reload(section);
    sight().reload(section);
    movement().reload(section);

    m_disp_walk_stand = pSettings->r_float(section, "disp_walk_stand");
    m_disp_walk_crouch = pSettings->r_float(section, "disp_walk_crouch");
    m_disp_run_stand = pSettings->r_float(section, "disp_run_stand");
    m_disp_run_crouch = pSettings->r_float(section, "disp_run_crouch");
    m_disp_stand_stand = pSettings->r_float(section, "disp_stand_stand");
    m_disp_stand_crouch = pSettings->r_float(section, "disp_stand_crouch");
    m_disp_stand_stand_zoom = pSettings->r_float(section, "disp_stand_stand_zoom");
    m_disp_stand_crouch_zoom = pSettings->r_float(section, "disp_stand_crouch_zoom");

    m_can_select_weapon = true;

    LPCSTR queue_sect = READ_IF_EXISTS(pSettings, r_string, cNameSect().c_str(), "fire_queue_section", nullptr);

    if (!queue_sect || xr_strcmp(queue_sect, "") != 0 || !pSettings->section_exist(queue_sect))
    {
        queue_sect = cNameSect().c_str();
    }

    const auto tryToRead = [&](pcstr lineToRead, u32 defaultValue) -> u32
    {
        const u32 value = pSettings->read_if_exists<u32>(queue_sect, lineToRead, u32(-1));
        if (value != u32(-1))
            return value;
        return defaultValue;
    };

    {
        m_pstl_min_queue_size_far = tryToRead("pstl_min_queue_size_far", 1);
        m_pstl_max_queue_size_far = tryToRead("pstl_max_queue_size_far", 1);
        m_pstl_min_queue_interval_far = tryToRead("pstl_min_queue_interval_far", 1000);
        m_pstl_max_queue_interval_far = tryToRead("pstl_max_queue_interval_far", 1250);

        m_pstl_min_queue_size_medium = tryToRead("pstl_min_queue_size_medium", 2);
        m_pstl_max_queue_size_medium = tryToRead("pstl_max_queue_size_medium", 4);
        m_pstl_min_queue_interval_medium = tryToRead("pstl_min_queue_interval_medium", 750);
        m_pstl_max_queue_interval_medium = tryToRead("pstl_max_queue_interval_medium", 1000);

        m_pstl_min_queue_size_close = tryToRead("pstl_min_queue_size_close", 3);
        m_pstl_max_queue_size_close = tryToRead("pstl_max_queue_size_close", 5);
        m_pstl_min_queue_interval_close = tryToRead("pstl_min_queue_interval_close", 500);
        m_pstl_max_queue_interval_close = tryToRead("pstl_max_queue_interval_close", 750);
    }
    {
        m_shtg_min_queue_size_far = tryToRead("shtg_min_queue_size_far", 1);
        m_shtg_max_queue_size_far = tryToRead("shtg_max_queue_size_far", 1);
        m_shtg_min_queue_interval_far = tryToRead("shtg_min_queue_interval_far", 1250);
        m_shtg_max_queue_interval_far = tryToRead("shtg_max_queue_interval_far", 1500);

        m_shtg_min_queue_size_medium = tryToRead("shtg_min_queue_size_medium", 1);
        m_shtg_max_queue_size_medium = tryToRead("shtg_max_queue_size_medium", 1);
        m_shtg_min_queue_interval_medium = tryToRead("shtg_min_queue_interval_medium", 750);
        m_shtg_max_queue_interval_medium = tryToRead("shtg_max_queue_interval_medium", 1250);

        m_shtg_min_queue_size_close = tryToRead("shtg_min_queue_size_close", 1);
        m_shtg_max_queue_size_close = tryToRead("shtg_max_queue_size_close", 1);
        m_shtg_min_queue_interval_close = tryToRead("shtg_min_queue_interval_close", 500);
        m_shtg_max_queue_interval_close = tryToRead("shtg_max_queue_interval_close", 1000);
    }
    {
        m_snp_min_queue_size_far = tryToRead("snp_min_queue_size_far", 1);
        m_snp_max_queue_size_far = tryToRead("snp_max_queue_size_far", 1);
        m_snp_min_queue_interval_far = tryToRead("snp_min_queue_interval_far", 3000);
        m_snp_max_queue_interval_far = tryToRead("snp_max_queue_interval_far", 4000);

        m_snp_min_queue_size_medium = tryToRead("snp_min_queue_size_medium", 1);
        m_snp_max_queue_size_medium = tryToRead("snp_max_queue_size_medium", 1);
        m_snp_min_queue_interval_medium = tryToRead("snp_min_queue_interval_medium", 3000);
        m_snp_max_queue_interval_medium = tryToRead("snp_max_queue_interval_medium", 4000);

        m_snp_min_queue_size_close = tryToRead("snp_min_queue_size_close", 1);
        m_snp_max_queue_size_close = tryToRead("snp_max_queue_size_close", 1);
        m_snp_min_queue_interval_close = tryToRead("snp_min_queue_interval_close", 3000);
        m_snp_max_queue_interval_close = tryToRead("snp_max_queue_interval_close", 4000);
    }
    {
        m_mchg_min_queue_size_far = tryToRead("mchg_min_queue_size_far", 1);
        m_mchg_max_queue_size_far = tryToRead("mchg_max_queue_size_far", 6);
        m_mchg_min_queue_interval_far = tryToRead("mchg_min_queue_interval_far", 500);
        m_mchg_max_queue_interval_far = tryToRead("mchg_max_queue_interval_far", 1000);

        m_mchg_min_queue_size_medium = tryToRead("mchg_min_queue_size_medium", 4);
        m_mchg_max_queue_size_medium = tryToRead("mchg_max_queue_size_medium", 6);
        m_mchg_min_queue_interval_medium = tryToRead("mchg_min_queue_interval_medium", 500);
        m_mchg_max_queue_interval_medium = tryToRead("mchg_max_queue_interval_medium", 750);

        m_mchg_min_queue_size_close = tryToRead("mchg_min_queue_size_close", 4);
        m_mchg_max_queue_size_close = tryToRead("mchg_max_queue_size_close", 10);
        m_mchg_min_queue_interval_close = tryToRead("mchg_min_queue_interval_close", 300);
        m_mchg_max_queue_interval_close = tryToRead("mchg_max_queue_interval_close", 500);
    }
    {
        const u32 min_queue_size_far = pSettings->read_if_exists<u32>(queue_sect, "weapon_min_queue_size_far", 1);
        const u32 max_queue_size_far = pSettings->read_if_exists<u32>(queue_sect, "weapon_max_queue_size_far", 6);
        const u32 min_queue_interval_far = pSettings->read_if_exists<u32>(queue_sect, "weapon_min_queue_interval_far", 500);
        const u32 max_queue_interval_far = pSettings->read_if_exists<u32>(queue_sect, "weapon_max_queue_interval_far", 1000);

        const u32 min_queue_size_medium = pSettings->read_if_exists<u32>(queue_sect, "weapon_min_queue_size_medium", 4);
        const u32 max_queue_size_medium = pSettings->read_if_exists<u32>(queue_sect, "weapon_max_queue_size_medium", 6);
        const u32 min_queue_interval_medium = pSettings->read_if_exists<u32>(queue_sect, "weapon_min_queue_interval_medium", 500);
        const u32 max_queue_interval_medium = pSettings->read_if_exists<u32>(queue_sect, "weapon_max_queue_interval_medium", 750);

        const u32 min_queue_size_close = pSettings->read_if_exists<u32>(queue_sect, "weapon_min_queue_size_close", 4);
        const u32 max_queue_size_close = pSettings->read_if_exists<u32>(queue_sect, "weapon_max_queue_size_close", 10);
        const u32 min_queue_interval_close = pSettings->read_if_exists<u32>(queue_sect, "weapon_min_queue_interval_close", 300);
        const u32 max_queue_interval_close = pSettings->read_if_exists<u32>(queue_sect, "weapon_max_queue_interval_close", 500);

        m_auto_min_queue_size_far = tryToRead("auto_min_queue_size_far", min_queue_size_far);
        m_auto_max_queue_size_far = tryToRead("auto_max_queue_size_far", max_queue_size_far);
        m_auto_min_queue_interval_far = tryToRead("auto_min_queue_interval_far", min_queue_interval_far);
        m_auto_max_queue_interval_far = tryToRead("auto_max_queue_interval_far", max_queue_interval_far);

        m_auto_min_queue_size_medium = tryToRead("auto_min_queue_size_medium", min_queue_size_medium);
        m_auto_max_queue_size_medium = tryToRead("auto_max_queue_size_medium", max_queue_size_medium);
        m_auto_min_queue_interval_medium = tryToRead("auto_min_queue_interval_medium", min_queue_interval_medium);
        m_auto_max_queue_interval_medium = tryToRead("auto_max_queue_interval_medium", max_queue_interval_medium);

        m_auto_min_queue_size_close = tryToRead("auto_min_queue_size_close", min_queue_size_close);
        m_auto_max_queue_size_close = tryToRead("auto_max_queue_size_close", max_queue_size_close);
        m_auto_min_queue_interval_close = tryToRead("auto_min_queue_interval_close", min_queue_interval_close);
        m_auto_max_queue_interval_close = tryToRead("auto_max_queue_interval_close", max_queue_interval_close);
    }

    {
        // m_pstl_queue_fire_dist_close = READ_IF_EXISTS(pSettings, r_float, queue_sect, "pstl_queue_fire_dist_close", 15.0f);
        m_pstl_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "pstl_queue_fire_dist_med", 15.0f);
        m_pstl_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "pstl_queue_fire_dist_far", 30.0f);
    }
    {
        // m_shtg_queue_fire_dist_close = READ_IF_EXISTS(pSettings, r_float, queue_sect, "shtg_queue_fire_dist_close", 15.0f);
        m_shtg_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "shtg_queue_fire_dist_med", 15.0f);
        m_shtg_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "shtg_queue_fire_dist_far", 30.0f);
    }
    {
        // m_snp_queue_fire_dist_close = READ_IF_EXISTS(pSettings, r_float, queue_sect, "snp_queue_fire_dist_close", 15.0f);
        m_snp_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "snp_queue_fire_dist_med", 15.0f);
        m_snp_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "snp_queue_fire_dist_far", 30.0f);
    }
    {
        // m_mchg_queue_fire_dist_close = READ_IF_EXISTS(pSettings, r_float, queue_sect, "mchg_queue_fire_dist_close", 15.0f);
        m_mchg_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "mchg_queue_fire_dist_med", 15.0f);
        m_mchg_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "mchg_queue_fire_dist_far", 30.0f);
    }
    {
        // m_auto_queue_fire_dist_close = READ_IF_EXISTS(pSettings,r_float,queue_sect,"auto_queue_fire_dist_close", 15.0f);
        m_auto_queue_fire_dist_med = READ_IF_EXISTS(pSettings, r_float, queue_sect, "auto_queue_fire_dist_med", 15.0f);
        m_auto_queue_fire_dist_far = READ_IF_EXISTS(pSettings, r_float, queue_sect, "auto_queue_fire_dist_far", 30.0f);
    }

    m_power_fx_factor = pSettings->r_float(section, "power_fx_factor");
}

void CAI_Stalker::Die(IGameObject* who)
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerDie);
    movement().on_death();

    notify_on_wounded_or_killed(who);

    SelectAnimation(XFORM().k, movement().detail().direction(), movement().speed());

    bool bDieSoundEnable = !!READ_IF_EXISTS(pSettingsOpenXRay, r_bool, "gameplay", "death_sound", false);

    if (bDieSoundEnable)
        sound().set_sound_mask(0);
    else
        sound().set_sound_mask((u32)eStalkerSoundMaskDie);

    if (is_special_killer(who))
        sound().play(eStalkerSoundDieInAnomaly);
    else
        sound().play(eStalkerSoundDie);

    m_hammer_is_clutched = m_clutched_hammer_enabled &&
        !CObjectHandler::planner().m_storage.property(ObjectHandlerSpace::eWorldPropertyStrapped) &&
        !::Random.randI(0, 2);

    inherited::Die(who);

    //запретить использование слотов в инвенторе
    inventory().SetSlotsUseful(false);

    if (inventory().GetActiveSlot() == NO_ACTIVE_SLOT)
        return;

    CInventoryItem* active_item = inventory().ActiveItem();
    if (!active_item)
        return;

    // CWeapon* weapon = smart_cast<CWeapon*>(active_item);
    // if (!weapon)
    //     return;

    // {
    //     TIItemContainer::iterator I = inventory().m_all.begin();
    //     TIItemContainer::iterator E = inventory().m_all.end();
    //     for (; I != E; ++I)
    //     {
    //         if (std::find(weapon->m_ammoTypes.begin(), weapon->m_ammoTypes.end(), (*I)->object().cNameSect()) ==
    //             weapon->m_ammoTypes.end())
    //             continue;

    //         NET_Packet packet;
    //         u_EventGen(packet, GE_DESTROY, (*I)->object().ID());
    //         u_EventSend(packet);
    //     }
    // }
}

void CAI_Stalker::Load(LPCSTR section)
{
    CCustomMonster::Load(section);
    CObjectHandler::Load(section);
    sight().Load(section);

    if (strstr(NameSection.c_str(), "trader"))
        m_is_trader = true;

    // skeleton physics
    m_pPhysics_support->in_Load(section);

    m_can_select_items = !!pSettings->r_bool(section, "can_select_items");
}

bool CAI_Stalker::net_Spawn(CSE_Abstract* DC)
{
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeHumanStalker* tpHuman = smart_cast<CSE_ALifeHumanStalker*>(e);
    R_ASSERT(tpHuman);

    // static bool first_time			= true;
    // if ( first_time ) {
    //	tpHuman->o_Position.z		-= 3.f;
    //	first_time					= false;
    //}

    m_group_behaviour = !!tpHuman->m_flags.test(CSE_ALifeObject::flGroupBehaviour);

    if (!CObjectHandler::net_Spawn(DC) || !inherited::net_Spawn(DC))
        return (false);

    // Spawn stage 0 (plans/spawn_optimization/SPEC.md): own part (animations, sight, locations, character physics)
    PERF_STEP("cls_spawn/stalker", spawn_cls_stalker);
    set_money(tpHuman->m_dwMoney, false);

    animation().reload();

    movement().m_head.current.yaw = movement().m_head.target.yaw = movement().m_body.current.yaw =
        movement().m_body.target.yaw = angle_normalize_signed(-tpHuman->o_torso.yaw);
    movement().m_body.current.pitch = movement().m_body.target.pitch = 0;

    if (ai().get_game_graph())
    {
        if (ai().game_graph().valid_vertex_id(tpHuman->m_tGraphID))
            ai_location().game_vertex(tpHuman->m_tGraphID);

        if (ai().game_graph().valid_vertex_id(tpHuman->m_tNextGraphID))
            movement().set_game_dest_vertex(tpHuman->m_tNextGraphID);
    }

    R_ASSERT2(ai().get_game_graph() && ai().get_level_graph() && ai().get_cross_table() &&
            (ai().level_graph().level_id() != u32(-1)),
        "There is no AI-Map, level graph, cross table, or graph is not compiled into the game graph!");

    setEnabled(TRUE);

    if (!Level().CurrentViewEntity())
        Level().SetEntity(this);

    if (!g_Alive())
        sound().set_sound_mask(u32(eStalkerSoundMaskDie));

    //загрузить иммунитеты из модельки сталкера
    IKinematics* pKinematics = smart_cast<IKinematics*>(Visual());
    VERIFY(pKinematics);
    CInifile* ini = pKinematics->LL_UserData();
    if (ini)
    {
        if (ini->section_exist("immunities"))
        {
            LPCSTR imm_sect = ini->r_string("immunities", "immunities_sect");
            conditions().LoadImmunities(imm_sect, pSettings);
        }

        if (ini->line_exist("bone_protection", "bones_protection_sect"))
        {
            m_boneHitProtection = xr_new<SBoneProtections>();
            m_boneHitProtection->reload(ini->r_string("bone_protection", "bones_protection_sect"), pKinematics);
        }
    }

    //вычислить иммунета в зависимости от ранга
    static float novice_rank_immunity = pSettings->r_float("ranks_properties", "immunities_novice_k");
    static float expirienced_rank_immunity = pSettings->r_float("ranks_properties", "immunities_experienced_k");

    static float novice_rank_visibility = pSettings->r_float("ranks_properties", "visibility_novice_k");
    static float expirienced_rank_visibility = pSettings->r_float("ranks_properties", "visibility_experienced_k");

    static float novice_rank_dispersion = pSettings->r_float("ranks_properties", "dispersion_novice_k");
    static float expirienced_rank_dispersion = pSettings->r_float("ranks_properties", "dispersion_experienced_k");

    CHARACTER_RANK_VALUE rank = Rank();
    clamp(rank, 0, 100);
    float rank_k = float(rank) / 100.f;
    m_fRankImmunity = novice_rank_immunity + (expirienced_rank_immunity - novice_rank_immunity) * rank_k;
    m_fRankVisibility = novice_rank_visibility + (expirienced_rank_visibility - novice_rank_visibility) * rank_k;
    m_fRankDisperison =
        expirienced_rank_dispersion + (novice_rank_dispersion - expirienced_rank_dispersion) * (1 - rank_k);

    // Faction-editor immunity overrides per (faction, rank) and leader
    if (pSettingsFE)
    {
        LPCSTR fe_faction = CharacterInfo().Community().id().c_str();
        LPCSTR fe_rank    = CharacterInfo().Rank().id().c_str();
        if (fe_faction && xr_strlen(fe_faction) && fe_rank && xr_strlen(fe_rank))
        {
            string_path fe_sec;
            strconcat(sizeof(fe_sec), fe_sec, fe_faction, "_", fe_rank);
            if (pSettingsFE->section_exist(fe_sec))
            {
                if (pSettingsFE->line_exist(fe_sec, "fire_wound_immunity"))
                {
                    float coef = pSettingsFE->r_float(fe_sec, "fire_wound_immunity");
                    conditions().SetHitImmunity(ALife::eHitTypeFireWound, coef);
                }
                if (pSettingsFE->line_exist(fe_sec, "explosion_immunity"))
                {
                    float coef = pSettingsFE->r_float(fe_sec, "explosion_immunity");
                    conditions().SetHitImmunity(ALife::eHitTypeExplosion, coef);
                }
            }
            if (SpecificCharacter().IsLeader() && pSettingsFE->section_exist(fe_faction))
            {
                if (pSettingsFE->line_exist(fe_faction, "fire_wound_immunity_leader"))
                {
                    float coef = pSettingsFE->r_float(fe_faction, "fire_wound_immunity_leader");
                    conditions().SetHitImmunity(ALife::eHitTypeFireWound, coef);
                }
                if (pSettingsFE->line_exist(fe_faction, "explosion_immunity_leader"))
                {
                    float coef = pSettingsFE->r_float(fe_faction, "explosion_immunity_leader");
                    conditions().SetHitImmunity(ALife::eHitTypeExplosion, coef);
                }
            }
        }
    }

    if (!fis_zero(SpecificCharacter().panic_threshold()))
        m_panic_threshold = SpecificCharacter().panic_threshold();

    sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));

#ifdef _DEBUG
    if (ai().get_alife() && !Level().MapManager().HasMapLocation("debug_stalker", ID()))
    {
        CMapLocation* map_location = Level().MapManager().AddMapLocation("debug_stalker", ID());

        map_location->SetHint(cName());
    }
#endif // _DEBUG

    if (SpecificCharacter().terrain_sect().size())
    {
        movement().locations().Load(SpecificCharacter().terrain_sect().c_str());
    }

    sight().update();
    Exec_Look(.001f);

    m_pPhysics_support->in_NetSpawn(e);

    return (true);
}

void CAI_Stalker::net_Destroy()
{
    inherited::net_Destroy();
    CInventoryOwner::net_Destroy();
    m_pPhysics_support->in_NetDestroy();

    // XXX: task scheduler
    //TaskScheduler->RemoveTask({ this, &CAI_Stalker::update_object_handler });
    Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CAI_Stalker::update_object_handler));
    Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CCustomMonster::Exec_Visibility));

#ifdef DEBUG
    fastdelegate::FastDelegate0<> f = fastdelegate::FastDelegate0<>(this, &CAI_Stalker::update_object_handler);
    xr_vector<fastdelegate::FastDelegate0<>>::const_iterator I;
    I = std::find(Device.seqParallel.begin(), Device.seqParallel.end(), f);
    VERIFY(I == Device.seqParallel.end());
    fastdelegate::FastDelegate0<> fv = fastdelegate::FastDelegate0<>(this, &CCustomMonster::Exec_Visibility);
    I = std::find(Device.seqParallel.begin(), Device.seqParallel.end(), fv);
    VERIFY(I == Device.seqParallel.end());
#endif

    xr_delete(m_ce_close);
    xr_delete(m_ce_far);
    xr_delete(m_ce_best);
    xr_delete(m_ce_angle);
    xr_delete(m_ce_safe);
    xr_delete(m_ce_ambush);
    xr_delete(m_boneHitProtection);
}

void CAI_Stalker::net_Save(NET_Packet& P)
{
    inherited::net_Save(P);
    m_pPhysics_support->in_NetSave(P);
}

bool CAI_Stalker::net_SaveRelevant() { return (inherited::net_SaveRelevant() || (PPhysicsShell() != NULL)); }
void CAI_Stalker::net_Export(NET_Packet& P)
{
    R_ASSERT(Local());

    // export last known packet
    R_ASSERT(!NET.empty());
    net_update& N = NET.back();
    //	P.w_float						(inventory().TotalWeight());
    //	P.w_u32							(m_dwMoney);

    P.w_float(GetfHealth());

    P.w_u32(N.dwTimeStamp);
    P.w_u8(0);
    P.w_vec3(N.p_pos);
    P.w_float /*w_angle8*/ (N.o_model);
    P.w_float /*w_angle8*/ (N.o_torso.yaw);
    P.w_float /*w_angle8*/ (N.o_torso.pitch);
    P.w_float /*w_angle8*/ (N.o_torso.roll);
    P.w_u8(u8(g_Team()));
    P.w_u8(u8(g_Squad()));
    P.w_u8(u8(g_Group()));

    float f1 = 0;
    GameGraph::_GRAPH_ID l_game_vertex_id = ai_location().game_vertex_id();
    P.w(&l_game_vertex_id, sizeof(l_game_vertex_id));
    P.w(&l_game_vertex_id, sizeof(l_game_vertex_id));
    //	P.w						(&f1,						sizeof(f1));
    //	P.w						(&f1,						sizeof(f1));
    if (ai().get_game_graph() && ai().game_graph().valid_vertex_id(l_game_vertex_id))
    {
        f1 = Position().distance_to(ai().game_graph().vertex(l_game_vertex_id)->level_point());
        P.w(&f1, sizeof(f1));
        f1 = Position().distance_to(ai().game_graph().vertex(l_game_vertex_id)->level_point());
        P.w(&f1, sizeof(f1));
    }
    else
    {
        P.w(&f1, sizeof(f1));
        P.w(&f1, sizeof(f1));
    }

    P.w_stringZ(m_sStartDialog);
}

void CAI_Stalker::net_Import(NET_Packet& P)
{
    R_ASSERT(Remote());
    net_update N;

    u8 flags;

    P.r_float();
    set_money(P.r_u32(), false);

    float health;
    P.r_float(health);
    SetfHealth(health);
    //	fEntityHealth = health;

    P.r_u32(N.dwTimeStamp);
    P.r_u8(flags);
    P.r_vec3(N.p_pos);
    P.r_float /*r_angle8*/ (N.o_model);
    P.r_float /*r_angle8*/ (N.o_torso.yaw);
    P.r_float /*r_angle8*/ (N.o_torso.pitch);
    P.r_float /*r_angle8*/ (N.o_torso.roll);
    id_Team = P.r_u8();
    id_Squad = P.r_u8();
    id_Group = P.r_u8();

    GameGraph::_GRAPH_ID graph_vertex_id = movement().game_dest_vertex_id();
    P.r(&graph_vertex_id, sizeof(GameGraph::_GRAPH_ID));
    graph_vertex_id = ai_location().game_vertex_id();
    P.r(&graph_vertex_id, sizeof(GameGraph::_GRAPH_ID));

    if (NET.empty() || (NET.back().dwTimeStamp < N.dwTimeStamp))
    {
        NET.push_back(N);
        NET_WasInterpolating = TRUE;
    }

    P.r_float();
    P.r_float();

    P.r_stringZ(m_sStartDialog);

    setVisible(TRUE);
    setEnabled(TRUE);
}

void CAI_Stalker::update_object_handler()
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerObjectHandlerUpdate);
    PERF_SCOPE(pseq_object_handler); // 22-game-thread.md: a seqParallel delegate (mt_object_handler)
    PERF_FF_CALLER(xray::perf::ff_caller::ObjectHandler); // 24-critical-path.md, M11: npc/ff/pick_oh
    if (!g_Alive())
        return;

    try
    {
        try
        {
            CObjectHandler::update();
        }
#if defined(DEBUG) && !defined(LUABIND_NO_EXCEPTIONS)
        catch (const luabind::cast_failed& message)
        {
            Msg("! Expression \"%s\" from luabind::object to %s", message.what(), message.info().name());
            throw;
        }
#endif
        catch (const std::exception& message)
        {
            Msg("! Expression \"%s\"", message.what());
            throw;
        }
        catch (...)
        {
            throw;
        }
    }
    catch (...)
    {
        CObjectHandler::set_goal(eObjectActionIdle);
        CObjectHandler::update();
    }
}

bool CAI_Stalker::mt_object_handler_update_allowed() const
{
    return m_client_updated &&
        (g_pGameLevel->WorldRendered() || g_pGamePersistent->IsMainMenuActive())
#ifdef DEBUG
        && !ShouldProcessOnRender()
#endif
    ;
}

void CAI_Stalker::create_anim_mov_ctrl(CBlend* b, Fmatrix* start_pose, bool local_animation)
{
    inherited::create_anim_mov_ctrl(b, start_pose, local_animation);
}

void CAI_Stalker::destroy_anim_mov_ctrl()
{
    inherited::destroy_anim_mov_ctrl();

    if (!g_Alive())
        return;

    if (getDestroy())
        return;

    movement().m_head.current.yaw = movement().m_body.current.yaw;
    movement().m_head.current.pitch = movement().m_body.current.pitch;
    movement().m_head.target.yaw = movement().m_body.current.yaw;
    movement().m_head.target.pitch = movement().m_body.current.pitch;

    movement().cleanup_after_animation_selector();
    {
        ZoneScopedN("CAI_Stalker::movement_update_destroy_anim");
        movement().update(0);
    }
}

// Stage 3, U1' of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md (always on in single player): UpdateCL
// rate LOD by the distance to the camera, on screen too. In a crowd 650-750 stalkers pass UpdateCL every frame (all of
// them within 30 m or seen within 60 m - the crow rules of CGameObject::UpdateCL), 12-29 ms a frame; those off
// screen are mostly crows already. At 30+ m a step of the position every 2nd-3rd frame is hardly seen.
//
// Why here (an early return at the top of the UpdateCL chain) and not in CObjectList::SingleUpdate: no change of the
// engine interface IGameObject, the decision needs the AI state anyway (enemy, hit memory, the weapon, the physics
// movement control), and the children are not skipped blindly: the parent is updated before its children
// (SingleUpdate recurses into H_Parent() first), so the weapon in the hands sees ucl_lod_skipped_this_frame() of
// its owner and skips itself only when idle (CWeapon::TrySkipOwnerLodUpdateCL); a torch or any other child item
// keeps its full UpdateCL.
//
// What a skipped frame leaves out, and why it can wait:
//  - the movement along the path, the look (Exec_Look), the sound player: they run on m_client_update_delta, the
//    time since the last full UpdateCL. The next full one gets all of it (CCustomMonster::UpdateCL lifts its 100 ms
//    clamp after a skip), so the NPC walks the same distance - in steps of 2-3 frames;
//  - the animation tracks: not here at all - a stalker on screen gets them from the render every frame
//    (CKinematics::CalculateBones -> OnCalculateBones -> UpdateTracks); the animation selection (SelectAnimation),
//    the end callbacks checked in UpdateCL and the switch of animations come a skip later;
//  - the step manager: steps by absolute time, a skip delays a step; no sound or particles beyond 20 m anyway;
//  - the object handler (weapon planner, pushed to seqParallel), the sight manager, IK (it has its own interval):
//    a frame or two later;
//  - the bone particles: their life time counts Device.dwTimeDelta per UpdateCL - never skipped while they play.
// The spatial DB and the update list are kept by UpdateCLSkippedRoot(), the DEBUG check of SingleUpdate as well.
namespace
{
// A hit taken within this time counts as a fight
constexpr u32 UCL_LOD_RECENT_HIT_MS = 5000;
// The first frames after the spawn run in full: the net state, the physics movement control and the animation
// get settled there
constexpr u32 UCL_LOD_SPAWN_FRAMES = 30;
} // namespace

bool CAI_Stalker::TrySkipUpdateCLLod()
{
    if (!IsGameTypeSingle())
        return false;
    // The dead have their own path (the ragdoll, the corpse sleep); one on the way out has nothing to save
    if (!g_Alive() || getDestroy() || H_Parent())
        return false;

    // The tier by the distance to the camera. A zoomed camera without a second viewport (a 2D scope, binoculars)
    // shows far NPCs as big as near ones: shrink the distance by the zoom (the ratio of the half-angle tangents)
    float dist = Position().distance_to(Device.vCameraPosition);
    if (Device.fFOV > EPS && g_fov > EPS && Device.fFOV < g_fov)
        dist *= std::tan(deg2rad(Device.fFOV) * 0.5f) / std::tan(deg2rad(g_fov) * 0.5f);
    if (dist < float(npc_perf_ucl_lod_dist))
        return false; // near: every frame, not counted
    const bool far_tier =
        npc_perf_ucl_lod_dist2 > 0 && dist >= float(_max(npc_perf_ucl_lod_dist2, npc_perf_ucl_lod_dist));
    const u32 interval = far_tier ? 3 : 2;

    const bool profile = npc_cpp_profile::enabled();

    // A fight: the target, the hit reaction, the shots and the recoil need every frame (and the fight is what the
    // player looks at, at any distance)
    bool fight = memory().enemy().selected() != nullptr;
    if (!fight)
    {
        const u32 last_hit = memory().hit().last_hit_time();
        fight = last_hit && Device.dwTimeGlobal - last_hit < UCL_LOD_RECENT_HIT_MS;
    }
    if (!fight)
        fight = weapon_shot_effector().IsActive();
    // The weapon in the hands: firing - a fight; switching, reloading - its state machine runs from the UpdateCL of
    // the weapon, keep the owner (and the hands) in step with it
    bool weapon_busy = false;
    if (!fight)
    {
        CInventoryItem* active_item = inventory().ActiveItem();
        CWeapon* weapon = active_item ? active_item->cast_weapon() : nullptr;
        if (weapon)
        {
            const u32 state = weapon->GetState();
            if (state == CWeapon::eFire || state == CWeapon::eFire2 || state == CWeapon::eMisfire ||
                state == CWeapon::eMagEmpty)
                fight = true;
            else if (state != CWeapon::eIdle && state != CWeapon::eHidden)
                weapon_busy = true;
            else if (weapon->GetNextState() != state)
                weapon_busy = true;
        }
    }
    if (fight)
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::UclLodDenyCombat);
        return false;
    }

    // Everything else that needs every frame:
    //  - the current (view) entity: the camera sits in it;
    //  - a second viewport (a 3D scope, binoculars): far NPCs are big in it;
    //  - the first frames after the spawn;
    //  - a jump, a lost footing, an external impulse, a forced physics control: the physics movement control
    //    drives the body there, a 2-3 frame step would snap it (the same rules as should_force_far_kinematic);
    //  - the movement driven by the animation (animation_movement_controlled: smart covers, animpoints, scripted
    //    animations with a root motion): the XFORM follows the animation in update_animation_movement_controller;
    //  - the animated collision body after a hit, an interactive motion or animation;
    //  - bone particles (their life time counts the frame delta per UpdateCL);
    //  - a busy weapon (see above)
    CPHMovementControl* movement_control = character_physics_support()->movement();
    const bool other = weapon_busy || this == Level().CurrentEntity() || this == Level().CurrentViewEntity() ||
        Device.m_SecondViewport.IsSVPActive() || Device.dwFrame - spawn_time() < UCL_LOD_SPAWN_FRAMES ||
        (movement_control &&
            (movement_control->JumpState() || movement_control->ForcedPhysicsControl() ||
                movement_control->bExernalImpulse || movement_control->IsLosingControl())) ||
        animation_movement_controlled() || character_physics_support()->animation_collision() ||
        character_physics_support()->is_interactive_motion() ||
        character_physics_support()->interactive_animation_running() || CParticlesPlayer::IsPlaying();
    if (other)
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::UclLodDenyOther);
        return false;
    }

    // The own frame of the tier: by frames, not by time - at 80 ms a frame any "not before N ms" rule below the frame
    // time would never skip. The ID spreads the NPCs of a tier evenly over the frames
    if ((Device.dwFrame + ID()) % interval == 0)
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::UclLodFull);
        return false;
    }

    // The time cap: the next full UpdateCL comes a frame later at the earliest; do not let the time it gets grow
    // past npc_perf_ucl_lod_max_delta_ms (this frame's delta predicts the next one). At 80 ms a frame both tiers
    // fit into 250 ms; at 130+ ms a frame the cap turns the LOD off by itself
    const u32 elapsed = Device.dwTimeGlobal - last_client_update_time();
    if (elapsed + Device.dwTimeDelta > u32(npc_perf_ucl_lod_max_delta_ms))
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::UclLodDenyTimeCap);
        return false;
    }

    UpdateCLSkippedRoot();
    if (profile)
        npc_cpp_profile::count(far_tier ? ENpcCppProfileStage::UclLodSkipD2 : ENpcCppProfileStage::UclLodSkipD1);
    return true;
}

// Step 1 of plans/optimization_mt/22-game-thread.md: the object handler LOD (always on in single
// player). Session 2026-10-14 18:42 (~1000 NPCs around the player): game/pseq/object_handler 12 ms a frame, ~820
// runs a frame - every living stalker with a full UpdateCL runs its object handler (CObjectHandler::update ->
// CObjectHandlerPlanner::update, a seqParallel delegate) every frame. A run costs ~1.2 us in a quiet scene and 16-25 us
// in a fight: the fire actions ask can_kill_member() every frame, i.e. the friendly-fire pick of update_can_kill_info
// (up to 5 ObjectSpace ray queries against a crowd, spatial/q_ray ~600 a frame with ~47 results a ray).
//
// What a run does: the GOAP planner of the weapon / missile in the hands. solve() checks the evaluators of the current
// world state (the item state, ammo, the queue, aim timers...) and re-plans when one changed or the target changed
// (set_goal from the brain or a scheme, an item taken or dropped); then the current action executes: show / hide /
// strap, aim (inertia timers by absolute time), fire (inventory Action kWPN_FIRE start / stop, the friendly-fire
// check), queue wait, reload. The weapon's own state machine (shots, reload animation, switching) runs from the
// UpdateCL of the weapon, not here. The planner uses no frame delta: its timers are Device.dwTimeGlobal stamps, so a
// later run sees the real elapsed time.
//
// Skipped: an idle stalker farther than npc_perf_object_handler_lod_dist from the camera -
// the planner is initialized, nothing changed since its last solve (solution_actual_flag), the goal is reached (one
// action in the solution) and that action is idle / strapped / aiming-ready; the item in the hands (if any) is idle or
// hidden with no pending or next state; no enemy, no hit in the last 5 s, no recoil. Such a run only re-checks the
// evaluators and executes an idle action; skipping it delays the reaction to a world change the planner does not hear
// of (the item state changed by someone else) by at most npc_perf_object_handler_lod_max_ms. A new goal, a new or
// dropped item reset the actuality and run at once; everything mid-action (fire, queue wait, aim, reload, show, hide,
// strapping, switch, throw) runs every UpdateCL as before. Never skipped: the view entity, the first frames after the
// spawn. On screen (or with a second viewport): every npc_perf_object_handler_lod_every_seen-th UpdateCL; off screen:
// every npc_perf_object_handler_lod_every_unseen-th one (the counter counts UpdateCL passes, so it stacks with the
// UpdateCL LOD; the time cap bounds both). The per-NPC counter starts at ID % N to spread the runs over the frames.
// update_sound_player is not included: 0.1 us a run, 0.06-0.1 ms a frame in all.
namespace
{
// The camera distance of the object handler LOD, shrunk by a zoomed camera the way TrySkipUpdateCLLod does
float ai_stalker_oh_lod_camera_distance(const CAI_Stalker& stalker)
{
    float dist = stalker.Position().distance_to(Device.vCameraPosition);
    if (Device.fFOV > EPS && g_fov > EPS && Device.fFOV < g_fov)
        dist *= std::tan(deg2rad(Device.fFOV) * 0.5f) / std::tan(deg2rad(g_fov) * 0.5f);
    return dist;
}
} // namespace

bool CAI_Stalker::TrySkipObjectHandlerLod()
{
    // Every run stamps its time: the time cap below counts from it
    const auto run = [this]() {
        m_object_handler_lod_last_time = Device.dwTimeGlobal;
        PERF_INC(npc_oh_full);
        return false;
    };
    if (!IsGameTypeSingle())
        return run();

    // Near: the view entity, the first frames after the spawn, within the distance
    if (this == Level().CurrentEntity() || this == Level().CurrentViewEntity() ||
        Device.dwFrame - spawn_time() < UCL_LOD_SPAWN_FRAMES ||
        ai_stalker_oh_lod_camera_distance(*this) < float(npc_perf_object_handler_lod_dist))
    {
        PERF_INC(npc_oh_keep_near);
        m_object_handler_lod_skips = 0;
        return run();
    }

    // A fight: the enemy, a recent hit, the recoil
    bool fight = memory().enemy().selected() != nullptr || weapon_shot_effector().IsActive();
    if (!fight)
    {
        const u32 last_hit = memory().hit().last_hit_time();
        fight = last_hit && Device.dwTimeGlobal - last_hit < UCL_LOD_RECENT_HIT_MS;
    }
    if (fight)
    {
        PERF_INC(npc_oh_keep_fight);
        m_object_handler_lod_skips = 0;
        return run();
    }

    // Busy: the planner is about to re-plan or mid-way to its goal, its action is not an idle one, or the item in
    // the hands is not settled
    CObjectHandlerPlanner& oh_planner = CObjectHandler::planner();
    bool busy = !oh_planner.initialized() || !oh_planner.solution_actual_flag() || oh_planner.solution().size() != 1;
    if (!busy)
    {
        switch (oh_planner.current_action_state_id())
        {
        case ObjectHandlerSpace::eWorldOperatorIdle: // eWorldOperatorNoItemsIdle too (the same state id)
        case ObjectHandlerSpace::eWorldOperatorStrapped: break;
        case ObjectHandlerSpace::eWorldOperatorAimingReady1:
        case ObjectHandlerSpace::eWorldOperatorAimingReady2:
        {
            // Aiming ready (CObjectActionAim): its execute sets the aimed property once the aim time is over - idle
            // only after a run past that moment did it
            const auto& action = oh_planner.current_action();
            const u32 start = action.start_level_time();
            busy = m_object_handler_lod_last_time < start ||
                m_object_handler_lod_last_time - start < action.inertia_time();
            break;
        }
        default: busy = true;
        }
    }
    if (!busy)
    {
        CInventoryItem* active_item = inventory().ActiveItem();
        CHudItem* hud_item = active_item ? active_item->cast_hud_item() : nullptr;
        if (hud_item)
        {
            const u32 state = hud_item->GetState();
            busy = hud_item->IsPending() || state != hud_item->GetNextState() ||
                (state != CHUDState::eIdle && state != CHUDState::eHidden);
        }
    }
    if (busy)
    {
        PERF_INC(npc_oh_keep_busy);
        m_object_handler_lod_skips = 0;
        return run();
    }

    // Idle and far: every N-th UpdateCL, at least once in npc_perf_object_handler_lod_max_ms (this frame's delta
    // predicts the next one)
    const bool seen = Device.m_SecondViewport.IsSVPActive() || rendered_in_main_pass();
    const u32 every =
        u32(_max(1, seen ? npc_perf_object_handler_lod_every_seen : npc_perf_object_handler_lod_every_unseen));
    if (m_object_handler_lod_skips == u32(-1))
        m_object_handler_lod_skips = u32(ID()) % every;
    const u32 elapsed = Device.dwTimeGlobal - m_object_handler_lod_last_time;
    if (m_object_handler_lod_skips + 1 >= every ||
        elapsed + Device.dwTimeDelta > u32(npc_perf_object_handler_lod_max_ms))
    {
        m_object_handler_lod_skips = 0;
        return run();
    }

    ++m_object_handler_lod_skips;
    PERF_INC(npc_oh_skipped);
    return true;
}

// The friendly-fire pick LOD (with the object handler LOD above): update_can_kill_info casts up to 5 ray queries
// from the muzzle every frame for every stalker whose fire action, sight or fire_make_sense asks can_kill_member /
// can_kill_enemy / pick_distance. A stalker nobody sees (off screen, no second viewport, farther than
// npc_perf_object_handler_lod_dist from the camera, not the view entity, not fighting the actor) keeps its last pick
// for up to npc_perf_object_handler_lod_pick_ms: it fires at the same rate, but stops for a friend crossing its line of
// fire (or starts again) up to that much later. The actor's fight is never affected.
bool CAI_Stalker::can_kill_pick_lod_reuse() const
{
    if (npc_perf_object_handler_lod_pick_ms <= 0 || !IsGameTypeSingle())
        return false;
    if (!m_pick_frame_id || Device.dwTimeGlobal - m_pick_time >= u32(npc_perf_object_handler_lod_pick_ms))
        return false;
    if (Device.m_SecondViewport.IsSVPActive() || rendered_in_main_pass())
        return false;
    if (this == Level().CurrentEntity() || this == Level().CurrentViewEntity())
        return false;
    const CEntityAlive* enemy = memory().enemy().selected();
    const CActor* actor = Actor();
    if (enemy && actor && enemy->ID() == actor->ID())
        return false;
    return ai_stalker_oh_lod_camera_distance(*this) >= float(npc_perf_object_handler_lod_dist);
}

// Step 1 of 22-game-thread.md, part B: corpses in the scheduler (always on).
// A dead stalker stays registered with the living interval (100..625 ms, CCustomMonster::net_Spawn). Its update
// (session 2026-10-14 18:42: stalker/shed/dead 2-14 a frame, i.e. a count, not ms; dead_think 2-12 us) does:
// the NET queue, the forget-killer event after FORGET_KILLER_TIME, the corpse render tier and the pose freeze
// (UpdateDeadNpcRenderTier), fire particles and blood drops, KillEntity once, the dead Think (CStalkerActionDead:
// fire on death, then the drop of the items in the slots once the ragdoll allows it), UpdateInventoryOwner (the
// actual drop), the physics support (the collision activation delay, CPHDestroyable, the movement control), and the
// Lua binder only for the first npc_perf_long_dead_skip_binder_ms (5 s). Measured cost of all of it: a few us an update,
// ~0.1-0.2 ms a frame. What the corpses cost is scheduler steps: the scheduler runs a fixed 128 steps a frame and
// every object is ~14 s late, so each corpse update takes a step from a living stalker or an item.
// A corpse that is settled - dead for more than 10 s (the death handling and the binder are done), the ragdoll
// asleep or gone, nothing in the hands (the weapon dropped by the dead action) - gets
// npc_perf_dead_shed_interval_ms instead; the next update after it sees the whole elapsed time (DT is clamped to
// t_max by the scheduler). Late by that much: the forget-killer event, the pose freeze, burn particles and blood
// drops of the corpse. Not affected: a corpse that still holds an item (the drop keeps its pace), searching the
// corpse (the use callback is set by the binder in the first seconds). Note: the steps freed go to other objects,
// living stalkers among them - the AI reacts sooner, but the scheduler time a frame may grow, not shrink.
// A corpse that stops being settled (the ragdoll woken by a hit or a push, an item put in its hands, revived) gets
// the living interval back at its next update.
namespace
{
constexpr u32 AI_STALKER_DEAD_SHED_SETTLE_MS = 10000;
// CCustomMonster::net_Spawn
constexpr u32 AI_STALKER_SHED_T_MIN = 100;
constexpr u32 AI_STALKER_SHED_T_MAX = 250;
} // namespace

void CAI_Stalker::UpdateDeadShedLod()
{
    bool settled = false;
    if (!g_Alive() && !getDestroy())
    {
        const u32 death_time = GetLevelDeathTime();
        const CPhysicsShell* shell = PPhysicsShell();
        settled = death_time && Device.dwTimeGlobal > death_time &&
            Device.dwTimeGlobal - death_time > AI_STALKER_DEAD_SHED_SETTLE_MS && (!shell || !shell->isEnabled()) &&
            !inventory().ActiveItem();
    }

    if (settled)
    {
        // The scheduler takes dwMin = t_min, dwMax = (1000 + t_max) / 2 (xrSheduler.cpp): t_max = 2 * t - 1000 gives
        // dwMax = t. 14-bit fields: t <= 8000 (the cvar range)
        const u32 interval = u32(_max(1000, _min(npc_perf_dead_shed_interval_ms, 8000)));
        shedule.t_min = interval;
        shedule.t_max = 2 * interval - 1000;
        m_dead_shed_lod_applied = true;
        PERF_INC(stk_shed_dead_lod);
        return;
    }

    if (m_dead_shed_lod_applied)
    {
        shedule.t_min = AI_STALKER_SHED_T_MIN;
        shedule.t_max = AI_STALKER_SHED_T_MAX;
        m_dead_shed_lod_applied = false;
    }
}

void CAI_Stalker::UpdateCL()
{
    ZoneScopedN("ucl_CAI_Stalker");
    if (TrySkipUpdateCLLod())
        return;
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCL);
    START_PROFILE("stalker")
    START_PROFILE("stalker/client_update")
    VERIFY2(PPhysicsShell() || getEnabled(), cName().c_str());

    // -npc_cpp_profile only (stage 0 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md): updates of a
    // living NPC off screen, and of those also farther than 25 m from the camera without an enemy - what an
    // update-rate LOD could skip (not seen in the last main pass; never while a second viewport renders)
    if (g_Alive() && npc_cpp_profile::enabled() && !Device.m_SecondViewport.IsSVPActive() && !rendered_in_main_pass())
    {
        npc_cpp_profile::count(ENpcCppProfileStage::UclLodOffscreen);
        if (!memory().enemy().selected() && this != Level().CurrentEntity() &&
            Position().distance_to_sqr(Device.vCameraPosition) > 25.f * 25.f)
            npc_cpp_profile::count(ENpcCppProfileStage::UclLodCandidate);
    }

    if (g_Alive())
    {
        {
            ZoneScopedN("ucl_stalker_object_handler");
            // 22-game-thread.md, step 1: the object handler LOD of idle far stalkers (TrySkipObjectHandlerLod)
            if (TrySkipObjectHandlerLod())
            {
                // not this UpdateCL (npc/object_handler/skipped)
            }
            else if (g_mt_config.test(mtObjectHandler) && CObjectHandler::planner().initialized())
            {
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLObjectHandlerDispatch);
                // XXX: task scheduler
                //TaskScheduler->AddTask("CAI_Stalker::update_object_handler",
                //    { this, &CAI_Stalker::update_object_handler },
                //    { this, &CAI_Stalker::mt_object_handler_update_allowed });

#ifdef DEBUG
                fastdelegate::FastDelegate0<> f = fastdelegate::FastDelegate0<>(this, &CAI_Stalker::update_object_handler);
                xr_vector<fastdelegate::FastDelegate0<>>::const_iterator I;
                I = std::find(Device.seqParallel.begin(), Device.seqParallel.end(), f);
                VERIFY(I == Device.seqParallel.end());
#endif
                Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CAI_Stalker::update_object_handler));
            }
            else
            {
                START_PROFILE("stalker/client_update/object_handler")
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLObjectHandlerDispatch);
                update_object_handler();
                STOP_PROFILE
            }
        }

        if ((movement().speed(character_physics_support()->movement()) > EPS_L) &&
            (eMovementTypeStand != movement().movement_type()) && (eMentalStateDanger == movement().mental_state()))
        {
            ZoneScopedN("ucl_stalker_danger_movement_sound");
            if ((eBodyStateStand == movement().body_state()) && (eMovementTypeRun == movement().movement_type()))
            {
                sound().play(eStalkerSoundRunningInDanger);
            }
            else
            {
                //				sound().play	(eStalkerSoundWalkingInDanger);
            }
        }
    }

    START_PROFILE("stalker/client_update/inherited")
    {
        ZoneScopedN("ucl_stalker_inherited");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLInherited);
        inherited::UpdateCL();
    }
    STOP_PROFILE

    START_PROFILE("stalker/client_update/physics")
    {
        ZoneScopedN("ucl_stalker_physics");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLPhysics);
        m_pPhysics_support->in_UpdateCL();
    }
    STOP_PROFILE

    if (g_Alive())
    {
        VERIFY(!m_pPhysicsShell);

        START_PROFILE("stalker/client_update/sight_manager")
        {
            ZoneScopedN("ucl_stalker_sight_update");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLSightManager);
            try
            {
                sight().update();
            }
            catch (...)
            {
                sight().setup(CSightAction(SightManager::eSightTypeCurrentDirection));
                sight().update();
            }
        }
        STOP_PROFILE

        START_PROFILE("stalker/client_update/exec_look")
        {
            ZoneScopedN("ucl_stalker_exec_look");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLExecLook);
            Exec_Look(client_update_fdelta());
        }
        STOP_PROFILE

        START_PROFILE("stalker/client_update/step_manager")
        {
            ZoneScopedN("ucl_stalker_step_manager");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLStepManager);
            CStepManager::update(false);
        }
        STOP_PROFILE

        START_PROFILE("stalker/client_update/weapon_shot_effector")
        {
            ZoneScopedN("ucl_stalker_weapon_shot_effector");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerUpdateCLWeaponEffector);
            if (weapon_shot_effector().IsActive())
                weapon_shot_effector().Update();
        }
        STOP_PROFILE
    }
#ifdef DEBUG
    debug_text();
#endif
    STOP_PROFILE
    STOP_PROFILE
}

void CAI_Stalker::PHHit(SHit& H) { m_pPhysics_support->in_Hit(H, false); }
CPHDestroyable* CAI_Stalker::ph_destroyable() { return smart_cast<CPHDestroyable*>(character_physics_support()); }

u32 CAI_Stalker::vision_rays_budget() const
{
    const u32 base = static_cast<u32>(npc_perf_vision_rays_per_npc);
    if (base == 0)
        return 0;

    const EStalkerUpdateLod lod = get_cached_update_lod();
    switch (lod)
    {
    case EStalkerUpdateLod::Near: return base;
    case EStalkerUpdateLod::Medium: return _max(1u, (base * 2) / 3);
    case EStalkerUpdateLod::Far: return _max(1u, base / 4);
    default: return base;
    }
}

float CAI_Stalker::memory_collect_budget_multiplier() const
{
    const EStalkerUpdateLod lod = get_cached_update_lod();
    switch (lod)
    {
    case EStalkerUpdateLod::Near: return 1.0f;
    case EStalkerUpdateLod::Medium: return 0.75f;
    case EStalkerUpdateLod::Far: return 0.33f;
    default: return 1.0f;
    }
}

void CAI_Stalker::shedule_Update(u32 DT)
{
    ZoneScopedN("sh_CAI_Stalker");
    ZoneTextF("%s", cName().c_str());
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleUpdate);
    // 22-game-thread.md, step 1: the whole update of a corpse (stalker/shed/dead counts them)
    PERF_SCOPE_IF(!g_Alive(), stk_shed_dead_total);
    START_PROFILE("stalker")
    START_PROFILE("stalker/schedule_update")
    VERIFY2(getEnabled() || PPhysicsShell(), cName().c_str());

    if (!CObjectHandler::planner().initialized())
    {
        ZoneScopedN("sh_stalker_object_handler_planner_gate");
        START_PROFILE("stalker/client_update/object_handler")
        update_object_handler();
        STOP_PROFILE
    }
    //	if (Position().distance_to(Level().CurrentEntity()->Position()) <= 50.f)
    //		Msg				("[%6d][SH][%s]",Device.dwTimeGlobal,*cName());
    // Queue shrink
    VERIFY(_valid(Position()));
    u32 dwTimeCL = Level().timeServer() - NET_Latency;
    VERIFY(!NET.empty());
    {
        ZoneScopedN("sh_stalker_net_queue_shrink");
        while ((NET.size() > 2) && (NET[1].dwTimeStamp < dwTimeCL))
            NET.pop_front();
    }

    Fvector vNewPosition = Position();
    VERIFY(_valid(Position()));
    // *** general stuff
    float dt = float(DT) / 1000.f;
    const u32 now_ms = Device.dwTimeGlobal;
    const EStalkerUpdateLod update_lod = get_stalker_update_lod(*this);
    const bool run_visibility = should_run_stalker_lod_stage(
        *this, m_next_visibility_update_time, m_visibility_update_interval, get_stalker_visibility_interval(*this, update_lod), now_ms);
    const bool run_think = should_run_stalker_lod_stage(
        *this, m_next_think_update_time, m_think_update_interval, get_stalker_think_interval(*this, update_lod), now_ms);
    // 22-game-thread.md (diagnostics only): the LOD tier of this update and the interval it covers (DT, the scheduler
    // lateness included): stalker/shed/dt_ms / (lod_near + lod_medium + lod_far + dead) = the real update period
    PERF_ADD(stk_shed_dt_ms, DT);
    // DT is clamped by the scheduler to max(t_max, 1000) ms (xrSheduler.cpp), so dt_ms tops out at 1 s a stalker under
    // overload; the real gap between two updates of a live stalker comes from its own timestamp:
    // stalker/shed/alive_period_ms / alive_period_n = the real update period of live stalkers
    if (g_Alive())
    {
        if (m_shed_alive_last_ms)
        {
            PERF_ADD(stk_shed_alive_period_ms, now_ms - m_shed_alive_last_ms);
            PERF_INC(stk_shed_alive_period_n);
        }
        m_shed_alive_last_ms = now_ms;
    }
    if (!g_Alive())
        PERF_INC(stk_shed_dead);
    else if (update_lod == EStalkerUpdateLod::Near)
        PERF_INC(stk_shed_lod_near);
    else if (update_lod == EStalkerUpdateLod::Medium)
        PERF_INC(stk_shed_lod_medium);
    else
        PERF_INC(stk_shed_lod_far);

    if (g_Alive())
    {
        ZoneScopedN("sh_stalker_alive_precalc");
        START_PROFILE("stalker/schedule_update/precalc")
        {
            ZoneScopedN("sh_stalker_anim_delayed_callbacks");
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerSchedulePrecalc);
            PERF_SCOPE(stk_shed_anim_callbacks);
            animation().play_delayed_callbacks();
        }

#ifndef USE_SCHEDULER_IN_AGENT_MANAGER
        // Optimization: throttle agent_manager updates based on LOD
        {
            ZoneScopedN("sh_stalker_agent_manager");
            const bool run_agent_manager = should_run_stalker_lod_stage(*this, 
                m_next_agent_manager_update_time, m_agent_manager_update_interval,
                get_stalker_agent_manager_interval(update_lod), now_ms);
        
            if (run_agent_manager)
            {
                // calls = the stalkers whose throttle let it through; CAgentManager::update itself runs the group
                // update (agent_enemy/distribute and the rest) at most once per its update rate
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleAgentManager);
                PERF_SCOPE(stk_shed_agent_manager);
                agent_manager().update();
            }
        }
#endif // USE_SCHEDULER_IN_AGENT_MANAGER

//		bool			check = !!memory().enemy().selected();
#if 0 // def DEBUG
		memory().visual().check_visibles();
#endif
        if (run_visibility)
        {
            ZoneScopedN("sh_stalker_Exec_Visibility");
            START_PROFILE("stalker/schedule_update/vision")
            {
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleVisibility);
                PERF_SCOPE(stk_shed_vision);
                // the shared vision batch, as in CCustomMonster::shedule_Update
                Exec_Visibility_Prepare();
            }
            STOP_PROFILE
        }

        START_PROFILE("stalker/schedule_update/memory")

        {
            ZoneScopedN("sh_stalker_process_enemies");
            START_PROFILE("stalker/schedule_update/memory/process")
            const bool run_process_enemies = should_run_stalker_lod_stage(*this,
                m_next_process_enemies_update_time, m_process_enemies_update_interval,
                get_stalker_enemy_scan_interval(*this, update_lod), now_ms);
            if (run_process_enemies)
            {
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleProcessEnemies);
                PERF_SCOPE(stk_shed_process_enemies);
                process_enemies();
            }
            STOP_PROFILE
        }

        {
            ZoneScopedN("sh_stalker_memory_update");
            START_PROFILE("stalker/schedule_update/memory/update")
            {
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerMemoryUpdate);
                // Optimization: throttle memory updates based on LOD
                const bool run_memory_update = should_run_stalker_lod_stage(*this,
                    m_next_memory_update_time, m_memory_update_interval,
                    get_stalker_memory_update_interval(update_lod), now_ms);
            
                if (run_memory_update)
                {
                    PERF_SCOPE(stk_shed_memory);
                    memory().update(dt);
                }
            }
            STOP_PROFILE
        }

        STOP_PROFILE
        STOP_PROFILE
    }

    {
        ZoneScopedN("sh_stalker_CEntityAlive_shedule_Update");
        START_PROFILE("stalker/schedule_update/inherited")
        // CEntityAlive -> ... -> CGameObject::shedule_Update: the script binder (the Lua update of the NPC:
        // script_binder/update/stalker) and the rest of the base chain
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleInherited);
        PERF_SCOPE(stk_shed_inherited);
        CEntityAlive::shedule_Update(DT);
        STOP_PROFILE
    }

    if (Remote())
    {
    }
    else if (!g_Alive())
    {
        {
            ZoneScopedN("sh_stalker_dead_net_update");
            START_PROFILE("stalker/schedule_update/net_update")
            net_update uNext;
            uNext.dwTimeStamp = Level().timeServer();
            uNext.o_model = movement().m_body.current.yaw;
            uNext.o_torso = movement().m_head.current;
            uNext.p_pos = vNewPosition;
            uNext.fHealth = GetfHealth();
            NET.push_back(uNext);
            STOP_PROFILE
        }

        // BUGFIX: мёртвый сталкер должен вызывать Think(), чтобы
        // CStalkerActionDead::execute() мог вызвать SetDropManual(TRUE) через
        // can_drop_active_weapon(). Без этого оружие навсегда прилипает к руке.
        //
        // ИСПРАВЛЕНО: было "if (!GetScriptControl())" — условие было инвертировано!
        // В CoC/GlobalWar все НПС под скрипт-контролем (xr_motivator вызывает
        // SetScriptControl(true)). С отрицанием Think() никогда не запускался
        // для мёртвых script-controlled НПС. Убрали условие — Think() теперь
        // вызывается для ВСЕХ мёртвых сталкеров независимо от GetScriptControl().
        m_fTimeUpdateDelta = dt;
        Level().AIStats.Think.Begin();
        {
            ZoneScopedN("sh_stalker_dead_Think");
#ifdef DEBUG
            if (Device.dwFrame > (spawn_time() + g_AI_inactive_time))
#endif
            {
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleThinkApply);
                PERF_SCOPE(stk_shed_dead_think);
                Think();
                m_dwLastUpdateTime = Device.dwTimeGlobal;
            }
        }
        Level().AIStats.Think.End();
    }
    else
    {
        ZoneScopedN("sh_stalker_alive_ai_apply");
        // here is monster AI call
        VERIFY(_valid(Position()));
        m_fTimeUpdateDelta = dt;
        Level().AIStats.Think.Begin();
        START_PROFILE("stalker/schedule_update/apply")
        if (GetScriptControl())
        {
            ZoneScopedN("sh_stalker_ProcessScripts");
            START_PROFILE("stalker/schedule_update/script_control")
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleScriptControl);
            PERF_SCOPE(stk_shed_script_control);
            ProcessScripts();
            m_dwLastUpdateTime = Device.dwTimeGlobal;
            STOP_PROFILE
        }
        else
#ifdef DEBUG
            if (Device.dwFrame > (spawn_time() + g_AI_inactive_time))
#endif
            if (run_think)
            {
                ZoneScopedN("sh_stalker_Think_apply");
                START_PROFILE("stalker/schedule_update/think_apply")
                {
                    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleThinkApply);
                    PERF_SCOPE(stk_shed_think);
                    Think();
                }
                m_dwLastUpdateTime = Device.dwTimeGlobal;
                STOP_PROFILE
            }
        STOP_PROFILE
        Level().AIStats.Think.End();
        VERIFY(_valid(Position()));

        // Look and action streams
        float temp = conditions().health();
        if (temp > 0)
        {
            // P6-opt: throttle feel_touch_update for FAR/MEDIUM LOD
            const bool run_feel_touch = should_run_stalker_lod_stage(*this,
                m_next_feel_touch_update_time, m_feel_touch_update_interval,
                get_stalker_feel_touch_interval(update_lod), now_ms);
            if (run_feel_touch)
            {
                ZoneScopedN("sh_stalker_feel_touch_update");
                START_PROFILE("stalker/schedule_update/feel_touch")
                NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleFeelTouch);
                PERF_SCOPE(stk_shed_feel_touch);
                Fvector C;
                float R;
                Center(C);
                R = Radius();
                feel_touch_update(C, R);
                STOP_PROFILE
            }

            {
                ZoneScopedN("sh_stalker_net_update_alive");
                START_PROFILE("stalker/schedule_update/net_update")
                net_update uNext;
                uNext.dwTimeStamp = Level().timeServer();
                uNext.o_model = movement().m_body.current.yaw;
                uNext.o_torso = movement().m_head.current;
                uNext.p_pos = vNewPosition;
                uNext.fHealth = GetfHealth();
                NET.push_back(uNext);
                STOP_PROFILE
            }
        }
        else
        {
            ZoneScopedN("sh_stalker_net_update_low_health");
            START_PROFILE("stalker/schedule_update/net_update")
            net_update uNext;
            uNext.dwTimeStamp = Level().timeServer();
            uNext.o_model = movement().m_body.current.yaw;
            uNext.o_torso = movement().m_head.current;
            uNext.p_pos = vNewPosition;
            uNext.fHealth = GetfHealth();
            NET.push_back(uNext);
            STOP_PROFILE
        }
    }
    VERIFY(_valid(Position()));

    // BUGFIX: UpdateInventoryOwner должен вызываться и для мёртвых сталкеров,
    // чтобы обработать флаг SetDropManual(TRUE), выставленный CStalkerActionDead.
    // Без этого предмет никогда не дропается из инвентаря трупа.
    {
        ZoneScopedN("sh_stalker_UpdateInventoryOwner");
        START_PROFILE("stalker/schedule_update/inventory_owner")
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerScheduleInventoryOwner);
        PERF_SCOPE(stk_shed_inventory_owner);
        UpdateInventoryOwner(DT);
        STOP_PROFILE
    }

    //#ifdef DEBUG
    //	if (psAI_Flags.test(aiALife)) {
    //		smart_cast<CSE_ALifeHumanStalker*>(ai().alife().objects().object(ID()))->check_inventory_consistency();
    //	}
    //#endif

    {
        ZoneScopedN("sh_stalker_physics_in_shedule_Update");
        START_PROFILE("stalker/schedule_update/physics")
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerSchedulePhysics);
        PERF_SCOPE(stk_shed_physics);
        VERIFY(_valid(Position()));
        m_pPhysics_support->in_shedule_Update(DT);
        VERIFY(_valid(Position()));
        STOP_PROFILE
    }

    // 22-game-thread.md, step 1: the scheduler interval of a settled corpse (UpdateDeadShedLod), after the dead Think
    // and UpdateInventoryOwner above have had their say this update; a revived stalker with the corpse interval gets
    // the living one back
    if (!g_Alive() || m_dead_shed_lod_applied)
        UpdateDeadShedLod();
    STOP_PROFILE
    STOP_PROFILE
}

float CAI_Stalker::Radius() const
{
    float R = inherited::Radius();
    CWeapon* W = smart_cast<CWeapon*>(inventory().ActiveItem());
    if (W)
        R += W->Radius();
    return R;
}

void CAI_Stalker::spawn_supplies()
{
    inherited::spawn_supplies();
    CObjectHandler::spawn_supplies();
}

void CAI_Stalker::Think()
{
    ZoneScopedN("CAI_Stalker::Think");
    ZoneTextF("%s", cName().c_str());
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerThink);
    PERF_FF_CALLER(xray::perf::ff_caller::Think); // 24-critical-path.md, M11: npc/ff/pick_think
    START_PROFILE("stalker/schedule_update/think")
    const u32 update_delta = (m_dwLastUpdateTime == u32(-1)) ? 0 : (Device.dwTimeGlobal - m_dwLastUpdateTime);

    {
        ZoneScopedN("sh_stalker_brain_update");
        START_PROFILE("stalker/schedule_update/think/brain")
        {
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerBrainUpdate);
            PERF_SCOPE(stk_think_brain);
            //	try {
            //		try {
            brain().update(update_delta);
        }
//		}
#ifdef DEBUG
//		catch (const luabind::cast_failed &message) {
//			Msg						("! Expression \"%s\" from luabind::object to
//%s",message.what(),message.info().name());
// throw;
//		}
#endif
//		catch (const std::exception &message) {
//			Msg						("! Expression \"%s\"",message.what());
//			throw;
//		}
//		catch (...) {
//			Msg						("! unknown exception occured");
//			throw;
//		}
//	}
//	catch(...) {
#ifdef DEBUG
//		Msg						("! Last action being executed : %s",brain().current_action().m_action_name);
#endif
    //		brain().setup			(this);
    //		brain().update			(update_delta);
    //	}
        STOP_PROFILE
    }

    START_PROFILE("stalker/schedule_update/think/movement")
    if (!g_Alive())
        return;

    //	try {
    {
        ZoneScopedN("sh_stalker_movement_update");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::StalkerThinkMovementUpdate);
        PERF_SCOPE(stk_think_movement);
        movement().update(update_delta);
    }
//	}
#if 0 // def DEBUG
	catch (const luabind::cast_failed& message) {
		Msg						("! Expression \"%s\" from luabind::object to %s",message.what(),message.info().name());
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
	catch (const std::exception& message) {
		Msg						("! Expression \"%s\"",message.what());
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
	catch (...) {
		Msg						("! unknown exception occured");
		movement().initialize	();
		movement().update		(update_delta);
		throw;
	}
#endif // DEBUG

    STOP_PROFILE
    STOP_PROFILE
}

void CAI_Stalker::SelectAnimation(const Fvector& view, const Fvector& move, float speed)
{
    if (!Device.Paused())
        animation().update();
}

bool CAI_Stalker::should_update_animation(u32 now_ms)
{
    // Animation LOD: full-rate for Near/Medium and any combat NPC (get_stalker_update_lod() -> Near when
    // an enemy is selected). Only far, non-combat NPCs are throttled; movement().on_frame still runs every
    // frame, so position/path stay correct -- only animation blending updates at a lower rate (imperceptible
    // at >80m). On Far->Near the interval drops to 0 and animation resumes immediately.
    if (get_stalker_update_lod(*this) != EStalkerUpdateLod::Far)
        return true;
    if (now_ms < m_next_animation_update_time)
        return false;
    m_next_animation_update_time = now_ms + u32(_max(1, npc_anim_lod_far_interval_ms));
    return true;
}

const SRotation CAI_Stalker::Orientation() const { return (movement().m_head.current); }
const MonsterSpace::SBoneRotation& CAI_Stalker::head_orientation() const { return (movement().head_orientation()); }
void CAI_Stalker::net_Relcase(IGameObject* O)
{
    ZoneScopedN("CAI_Stalker::net_Relcase");
    inherited::net_Relcase(O);

    {
        ZoneScopedN("CAI_Stalker::net_Relcase sight_movement");
        sight().remove_links(O);
        movement().remove_links(O);
    }

    if (!g_Alive())
        return;

    {
        ZoneScopedN("CAI_Stalker::net_Relcase agent_physics");
        // P17 (plans/optimization_mt/12-relcase.md): the agent manager belongs to the whole group - inside the batched
        // relcase its first alive member cleans it of every object destroyed this frame (back to front, as the
        // per-object relcase did), the other members skip it. Outside a batch (a direct net_Relcase): per object.
        const RelcaseBatch* batch = Level().Objects.relcase_batch();
        if (batch && batch->contains(O))
        {
            if (Level().Objects.relcase_first_visit(&agent_manager()))
            {
                for (u32 it = batch->count; it-- > 0;)
                    agent_manager().remove_links(batch->objects[it]);
            }
            else
                PERF_INC(relcase_agent_skips);
        }
        else
            agent_manager().remove_links(O);
        m_pPhysics_support->in_NetRelcase(O);
    }
}

CMovementManager* CAI_Stalker::create_movement_manager()
{
    return (m_movement_manager = xr_new<stalker_movement_manager_smart_cover>(this));
}

CSound_UserDataVisitor* CAI_Stalker::create_sound_visitor()
{
    return (m_sound_user_data_visitor = xr_new<CStalkerSoundDataVisitor>(this));
}

CMemoryManager* CAI_Stalker::create_memory_manager() { return (xr_new<CMemoryManager>(this, create_sound_visitor())); }
IFactoryObject* CAI_Stalker::_construct()
{
    m_pPhysics_support = xr_new<CCharacterPhysicsSupport>(CCharacterPhysicsSupport::etStalker, this);
    CCustomMonster::_construct();
    CObjectHandler::_construct();
    CStepManager::_construct();

    m_actor_relation_flags.zero();
    m_animation_manager = xr_new<CStalkerAnimationManager>(this);
    m_brain = xr_new<CStalkerPlanner>();
    m_sight_manager = xr_new<CSightManager>(this);
    m_weapon_shot_effector = xr_new<CWeaponShotEffector>();

    return (this);
}

bool CAI_Stalker::use_center_to_aim() const { return (!wounded() && (movement().body_state() != eBodyStateCrouch)); }
void CAI_Stalker::UpdateCamera()
{
    float new_range = eye_range, new_fov = eye_fov;
    Fvector temp = eye_matrix.k;
    if (g_Alive())
    {
        update_range_fov(
            new_range, new_fov, memory().visual().current_state().m_max_view_distance * eye_range, eye_fov);
        if (weapon_shot_effector().IsActive())
            temp = weapon_shot_effector_direction(temp);
    }

    g_pGameLevel->Cameras().Update(eye_matrix.c, temp, eye_matrix.j, new_fov, .75f, new_range, 0);
}

bool CAI_Stalker::can_attach(const CInventoryItem* inventory_item) const
{
    return (CObjectHandler::can_attach(inventory_item));
}

void CAI_Stalker::save(NET_Packet& packet)
{
    inherited::save(packet);
    CInventoryOwner::save(packet);
    brain().save(packet);
}

void CAI_Stalker::load(IReader& packet)
{
    inherited::load(packet);
    CInventoryOwner::load(packet);
    brain().load(packet);
}

void CAI_Stalker::load_critical_wound_bones()
{
    fill_bones_body_parts("head", critical_wound_type_head);
    fill_bones_body_parts("torso", critical_wound_type_torso);
    fill_bones_body_parts("hand_left", critical_wound_type_hand_left);
    fill_bones_body_parts("hand_right", critical_wound_type_hand_right);
    fill_bones_body_parts("leg_left", critical_wound_type_leg_left);
    fill_bones_body_parts("leg_right", critical_wound_type_leg_right);
}

void CAI_Stalker::fill_bones_body_parts(LPCSTR bone_id, const ECriticalWoundType& wound_type)
{
    LPCSTR body_parts_section_id = pSettings->r_string(cNameSect(), "body_parts_section_id");
    VERIFY(body_parts_section_id);

    LPCSTR body_part_section_id = pSettings->r_string(body_parts_section_id, bone_id);
    VERIFY(body_part_section_id);

    IKinematics* kinematics = smart_cast<IKinematics*>(Visual());
    VERIFY(kinematics);

    CInifile::Sect& body_part_section = pSettings->r_section(body_part_section_id);
    auto I = body_part_section.Data.cbegin();
    auto E = body_part_section.Data.cend();
    for (; I != E; ++I)
        m_bones_body_parts.emplace(kinematics->LL_BoneID((*I).first), u32(wound_type));
}

void CAI_Stalker::on_before_change_team()
{
    m_registered_in_combat_on_migration = agent_manager().member().registered_in_combat(this);
}

void CAI_Stalker::on_after_change_team()
{
    if (!m_registered_in_combat_on_migration)
        return;

    agent_manager().member().register_in_combat(this);
}

float CAI_Stalker::shedule_Scale() const
{
    if (!sniper_update_rate())
        return (inherited::shedule_Scale());

    return (0.f);
}

void CAI_Stalker::aim_bone_id(shared_str const& bone_id)
{
    //	IKinematics				*kinematics = smart_cast<IKinematics*>(Visual());
    //	VERIFY2					(kinematics->LL_BoneID(bone_id) != BI_NONE, make_string("Cannot find bone %s",bone_id));
    m_aim_bone_id = bone_id;
}

shared_str const& CAI_Stalker::aim_bone_id() const { return (m_aim_bone_id); }
void aim_target(shared_str const& aim_bone_id, Fvector& result, const CGameObject* object)
{
    IKinematics* kinematics = smart_cast<IKinematics*>(object->Visual());
    VERIFY(kinematics);

    u16 bone_id = kinematics->LL_BoneID(aim_bone_id);
    VERIFY2(bone_id != BI_NONE, make_string("Cannot find bone %s", bone_id));

    Fmatrix const& bone_matrix = kinematics->LL_GetTransform(bone_id);
    Fmatrix final;
    final.mul_43(object->XFORM(), bone_matrix);
    result = final.c;
}

void CAI_Stalker::aim_target(Fvector& result, const CGameObject* object)
{
    VERIFY(m_aim_bone_id.size());

    ::aim_target(m_aim_bone_id, result, object);
}

bool CAI_Stalker::AlwaysTheCrow()
{
    VERIFY(character_physics_support());
    return (character_physics_support()->is_interactive_motion());
}

smart_cover::cover const* CAI_Stalker::get_current_smart_cover()
{
    if (movement().current_params().cover_id() != movement().target_params().cover_id())
        return 0;

    return movement().current_params().cover();
}

smart_cover::loophole const* CAI_Stalker::get_current_loophole()
{
    if (movement().current_params().cover_id() != movement().target_params().cover_id())
        return 0;

    if (movement().current_params().cover_loophole_id() != movement().target_params().cover_loophole_id())
        return 0;

    return movement().current_params().cover_loophole();
}

bool CAI_Stalker::can_fire_right_now()
{
    if (!ready_to_kill())
        return (false);

    VERIFY(best_weapon());
    CWeapon& best_weapon = smart_cast<CWeapon&>(*this->best_weapon());
    return best_weapon.GetAmmoElapsed() > 0;
}

bool CAI_Stalker::unlimited_ammo() { return infinite_ammo() && CObjectHandler::planner().object().g_Alive(); }

void CAI_Stalker::ResetBoneProtections(pcstr imm_sect, pcstr bone_sect)
{
    IKinematics* pKinematics = smart_cast<IKinematics*>(Visual());
    CInifile* ini = pKinematics->LL_UserData();
    if (ini)
    {
        if (imm_sect || ini->section_exist("immunities"))
        {
            imm_sect = imm_sect ? imm_sect : ini->r_string("immunities", "immunities_sect");
            conditions().LoadImmunities(imm_sect, pSettings);
        }

        if (bone_sect || ini->line_exist("bone_protection", "bones_protection_sect"))
        {
            bone_sect = ini->r_string("bone_protection", "bones_protection_sect");
            m_boneHitProtection->reload(bone_sect, pKinematics);
        }
    }
}
