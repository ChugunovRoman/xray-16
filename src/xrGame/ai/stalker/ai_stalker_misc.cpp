////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_misc.cpp
//	Created 	: 27.02.2003
//  Modified 	: 27.02.2003
//	Author		: Dmitriy Iassenev
//	Description : Miscellaneous functions for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include <tracy/Tracy.hpp>
#include "ai_stalker.h"
#include "ai_stalker_impl.h"
#include "ai_stalker_space.h"
#include "Bolt.h"
#include "Inventory.h"
#include "xrServerEntities/character_info.h"
#include "relation_registry.h"
#include "memory_manager.h"
#include "item_manager.h"
#include "stalker_movement_manager_smart_cover.h"
#include "Explosive.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_explosive_manager.h"
#include "agent_location_manager.h"
#include "danger_object_location.h"
#include "member_order.h"
#include "Level.h"
#include "Actor.h"
#include "sound_player.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "visual_memory_manager.h"
#include "agent_enemy_manager.h"
#include "hit_memory_manager.h"
#include "stalker_planner.h"
#include "stalker_decision_space.h"
#include "xrEngine/xrSheduler.h"

const u32 TOLLS_INTERVAL = 2000;
const u32 GRENADE_INTERVAL = 0 * 1000;
const float FRIENDLY_GRENADE_ALARM_DIST = 5.f;
const u32 DANGER_INFINITE_INTERVAL = 60000000;
const float DANGER_EXPLOSIVE_DISTANCE = 10.f;

bool CAI_Stalker::useful(const CItemManager* manager, const CGameObject* object) const
{
    const CExplosive* explosive = smart_cast<const CExplosive*>(object);

    if (explosive && smart_cast<const CInventoryItem*>(object))
        agent_manager().location().add(xr_new<CDangerObjectLocation>(
            object, Device.dwTimeGlobal, DANGER_INFINITE_INTERVAL, DANGER_EXPLOSIVE_DISTANCE));

    if (explosive && (explosive->CurrentParentID() != 0xffff))
    {
        agent_manager().explosive().register_explosive(explosive, object);
        CEntityAlive* entity_alive = smart_cast<CEntityAlive*>(Level().Objects.net_Find(explosive->CurrentParentID()));
        if (entity_alive)
            memory().danger().add(CDangerObject(entity_alive, object->Position(), Device.dwTimeGlobal,
                CDangerObject::eDangerTypeGrenade, CDangerObject::eDangerPerceiveTypeVisual, object));
    }

    if (!memory().item().useful(object))
        return (false);

    const CInventoryItem* inventory_item = smart_cast<const CInventoryItem*>(object);
    if (!inventory_item || !inventory_item->useful_for_NPC())
        return (false);

    const CBolt* bolt = smart_cast<const CBolt*>(object);
    if (bolt)
        return (false);

    CInventory* inventory_non_const = const_cast<CInventory*>(&inventory());
    CInventoryItem* inventory_item_non_const = const_cast<CInventoryItem*>(inventory_item);
    if (!inventory_non_const->CanTakeItem(inventory_item_non_const))
        return (false);

    return (true);
}

float CAI_Stalker::evaluate(const CItemManager* manager, const CGameObject* object) const
{
    float distance = Position().distance_to_sqr(object->Position());
    distance = !fis_zero(distance) ? distance : EPS_L;
    return (distance);
}

bool CAI_Stalker::useful(const CEnemyManager* manager, const CEntityAlive* object) const
{
    // 45-engine-ai-audit.md, F2: CEnemyManager::useful below answers false for a dead object before anything else,
    // and useful_enemy only reads - so a corpse (memory lists are full of them in a fight) skips the group lookup,
    // registered_in_combat and the search in the group enemies. The answer is the same
    if (!object->g_Alive())
        return (false);

    if (!agent_manager().enemy().useful_enemy(object, this))
        return (false);

    return (memory().enemy().useful(object));
}

bool CAI_Stalker::is_nearby_idle_state_optimization_candidate() const
{
    if (GetScriptControl())
        return false;

    if (movement().mental_state() != eMentalStateFree)
        return false;

    const MonsterSpace::EMovementType movement_type = movement().movement_type();
    if ((movement_type != eMovementTypeStand) && (movement_type != eMovementTypeWalk))
        return false;

    if (memory().enemy().selected() || memory().danger().selected() || memory().item().selected())
        return false;

    CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!actor)
        return false;

    return Position().distance_to_sqr(actor->Position()) <= (35.f * 35.f);
}

ALife::ERelationType CAI_Stalker::tfGetRelationType(const CEntityAlive* tpEntityAlive) const
{
    const CInventoryOwner* pOtherIO = smart_cast<const CInventoryOwner*>(tpEntityAlive);

    ALife::ERelationType relation = ALife::eRelationTypeDummy;

    if (pOtherIO && !(const_cast<CEntityAlive*>(tpEntityAlive)->cast_base_monster()))
        relation = RELATION_REGISTRY().GetRelationType(static_cast<const CInventoryOwner*>(this), pOtherIO);

    if (ALife::eRelationTypeDummy != relation)
        return relation;
    else
        return inherited::tfGetRelationType(tpEntityAlive);
}

void CAI_Stalker::react_on_grenades()
{
    CMemberOrder* member_order = agent_manager().member().get_member(ID());
    if (!member_order)
        return;

    CMemberOrder::CGrenadeReaction& reaction = member_order->grenade_reaction();
    if (!reaction.m_processing)
        return;

    if (!reaction.m_grenade)
    {
        reaction.clear();
        return;
    }

    if (Device.dwTimeGlobal < reaction.m_time + GRENADE_INTERVAL)
        return;

    //	u32							interval = AFTER_GRENADE_DESTROYED_INTERVAL;
    const CMissile* missile = smart_cast<const CMissile*>(reaction.m_grenade);
    //	if (missile && (missile->destroy_time() > Device.dwTimeGlobal))
    //		interval				= missile->destroy_time() - Device.dwTimeGlobal + AFTER_GRENADE_DESTROYED_INTERVAL;
    //	m_object->agent_manager().add_danger_location(reaction.m_game_object->Position(),Device.dwTimeGlobal,interval,GRENADE_RADIUS);

    if (missile && agent_manager().member().group_behaviour())
    {
        //		Msg						("%6d : Stalker %s : grenade reaction",Device.dwTimeGlobal,*m_object->cName());
        CEntityAlive* initiator =
            smart_cast<CEntityAlive*>(Level().Objects.net_Find(reaction.m_grenade->CurrentParentID()));
        /*		VERIFY2					(
                    initiator,
                    make_string(
                        "grenade[%d][%s], parent[%d]",
                        missile->ID(),
                        missile->cName().c_str(),
                        reaction.m_grenade->CurrentParentID()
                    )
                );
        */
        if (initiator)
        {
            if (is_relation_enemy(initiator))
                sound().play(StalkerSpace::eStalkerSoundGrenadeAlarm);
            else if (missile->Position().distance_to(Position()) < FRIENDLY_GRENADE_ALARM_DIST)
            {
                u32 const time = missile->destroy_time() >= Device.dwTimeGlobal ?
                    u32(missile->destroy_time() - Device.dwTimeGlobal) :
                    0;
                sound().play(StalkerSpace::eStalkerSoundFriendlyGrenadeAlarm, time + 1500, time + 1000);
            }
        }
    }

    reaction.clear();
}

void CAI_Stalker::react_on_member_death()
{
    CMemberOrder* member_order = agent_manager().member().get_member(ID());
    if (!member_order)
        return;

    CMemberOrder::CMemberDeathReaction& reaction = member_order->member_death_reaction();
    if (!reaction.m_processing)
        return;

    if (!reaction.m_member)
    {
        reaction.clear();
        return;
    }

    if (Device.dwTimeGlobal < reaction.m_time + TOLLS_INTERVAL)
        return;

    if (agent_manager().member().group_behaviour())
    {
        if (!reaction.m_member->g_Alive())
            sound().play(StalkerSpace::eStalkerSoundTolls, 3000, 2000);
        else
            sound().play(StalkerSpace::eStalkerSoundWounded, 3000, 2000);
    }

    reaction.clear();
}

void CAI_Stalker::process_enemies()
{
    ZoneScopedN("CAI_Stalker::process_enemies");
    if (memory().enemy().selected())
        return;

    typedef MemorySpace::squad_mask_type squad_mask_type;
    typedef CVisualMemoryManager::VISIBLES VISIBLES;

    squad_mask_type mask = memory().visual().mask();
    VISIBLES::const_iterator I = memory().visual().objects().begin();
    VISIBLES::const_iterator E = memory().visual().objects().end();
    for (; I != E; ++I)
    {
        if (!(*I).visible(mask))
            continue;

        const CAI_Stalker* member = smart_cast<const CAI_Stalker*>((*I).m_object);
        if (!member)
            continue;

        // 45-engine-ai-audit.md, F2: both checks only read and both skip the member, the cheap one first - a corpse
        // no longer goes to the relation registry
        if (!member->g_Alive())
            continue;

        if (is_relation_enemy(member))
            continue;

        if (!member->memory().enemy().selected())
        {
            if (!memory().danger().selected() && member->memory().danger().selected())
                memory().danger().add(*member->memory().danger().selected());
            continue;
        }

        //Alundaio: Only transfer enemy if I can see member at this very moment!
        if (!memory().visual().visible_now(member))
            continue;
        //Alundaio: END

        memory().make_object_visible_somewhen(member->memory().enemy().selected());
        break;
    }
}

// 22-game-thread.md, step 4d (urgent near heap, xrEngine/xrSheduler.cpp): the stalker has to react now - its combat or
// danger planner is active, an enemy is selected (seen, heard or remembered and useful), or it was hit within
// shed_urgent_hit_ms ms. Asked by the scheduler on the thread of its non-RT pass, between the updates (the requeue after
// this stalker's own shedule_Update, the scans of the near heaps), only while alive and near. Everything read here is
// the state of the game logic and is not written meanwhile: the planner and the enemy selection change only in this
// stalker's Think / memory update (that same thread), the health and the hit time in Hit - the events of FrameMove on
// the main thread before the Game thread is woken, or a hit from an update on the Game thread itself (the overlap frame
// runs only the render and the physics step in parallel, both never touch them). A few loads and compares
bool CAI_Stalker::shedule_Urgent() const
{
    if (!g_Alive())
        return false;
    if (brain().initialized())
    {
        const auto action = brain().current_action_id();
        if (action == StalkerDecisionSpace::eWorldOperatorCombatPlanner ||
            action == StalkerDecisionSpace::eWorldOperatorDangerPlanner)
        {
            return true;
        }
    }
    if (memory().enemy().selected())
        return true;
    const u32 hit_time = memory().hit().last_hit_time();
    return hit_time != 0 && Device.dwTimeGlobal - hit_time <= u32(_max(0, ps_shed_urgent_hit_ms));
}
