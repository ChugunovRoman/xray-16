////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator_base2.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator base class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_simulator_base.h"
#include "relation_registry.h"
#include "alife_registry_wrappers.h"
#include "xrServer_Objects_ALife_Items.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "alife_story_registry.h"
#include "alife_schedule_registry.h"
#include "alife_smart_terrain_registry.h"
#include "alife_group_registry.h"
#include "addon_api_alife_ext.h"
#include "addon_binders.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "npc_cpp_profile.h"

using namespace ALife;

void CALifeSimulatorBase::register_object(CSE_ALifeDynamicObject* object, bool add_object)
{
    object->on_before_register();

    if (add_object)
        objects().add(object);

    graph().update(object);
    scheduled().add(object);
    story_objects().add(object->m_story_id, object);
    smart_terrains().add(object);
    groups().add(object);

    setup_simulator(object);

    CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(object);
    if (item && item->attached())
    {
        CSE_ALifeDynamicObject* II = objects().object(item->base()->ID_Parent);

#ifdef DEBUG
        if (std::find(II->children.begin(), II->children.end(), item->base()->ID) != II->children.end())
        {
            Msg("[LSS] Specified item [%s][%d] is already attached to the specified object [%s][%d]",
                item->base()->name_replace(), item->base()->ID, II->name_replace(), II->ID);
            FATAL("[LSS] Cannot recover from the previous error!");
        }
#endif

        II->children.push_back(item->base()->ID);
        II->attach(item, true, false);
    }

    if (can_register_objects())
    {
        // 53-create-squad-engine.md: alife/on_register/<kind> and alife/on_register/event of the stalkers, the monsters
        // and the squads (only with -npc_cpp_profile; the kind is not looked up without it)
        ENpcCppProfileStage on_register_stage = ENpcCppProfileStage::Count;
        if (npc_cpp_profile::enabled())
        {
            if (smart_cast<CSE_ALifeHumanAbstract*>(object))
                on_register_stage = ENpcCppProfileStage::AlifeOnRegisterStalker;
            else if (smart_cast<CSE_ALifeMonsterAbstract*>(object))
                on_register_stage = ENpcCppProfileStage::AlifeOnRegisterMonster;
            else if (smart_cast<CSE_ALifeOnlineOfflineGroup*>(object))
                on_register_stage = ENpcCppProfileStage::AlifeOnRegisterSquad;
        }
        const bool profile = on_register_stage != ENpcCppProfileStage::Count;

        const u64 on_register_start = profile ? CPU::QPC() : 0;
        object->on_register();
        const u64 event_start = profile ? CPU::QPC() : 0;
        gw::addons::alife_ext::OnServerObjectRegistered(object); // server_object_on_register
        if (profile)
        {
            const u64 event_end = CPU::QPC();
            npc_cpp_profile::add(on_register_stage, event_start - on_register_start);
            npc_cpp_profile::add(ENpcCppProfileStage::AlifeOnRegisterEvent, event_end - event_start);
        }
    }
}

void CALifeSimulatorBase::unregister_object(CSE_ALifeDynamicObject* object, bool alife_query)
{
    gw::addons::alife_ext::OnServerObjectUnregistering(object); // server_object_on_unregister: still in the registries
    gw::addons::binders::OnServerObjectRelease(object->ID); // the kept binder state of this id: the id may be reused
    object->on_unregister();

    // 53-create-squad-engine.md section 9: the object leaves ALife for good (a release: a dead body removed,
    // a squad member removed, any alife():release), its specific character is free for the next selections. Not in
    // on_unregister: ~CALifeObjectRegistry calls that one for every object when ALife is destroyed
    if (CSE_ALifeTraderAbstract* trader = object->cast_trader_abstract())
        trader->gw_release_specific_character();

    CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(object);
    if (item && item->attached())
        graph().detach(*objects().object(item->base()->ID_Parent), item,
            objects().object(item->base()->ID_Parent)->m_tGraphID, alife_query);

    objects().remove(object->ID);
    story_objects().remove(object->m_story_id);
    smart_terrains().remove(object);
    groups().remove(object);

    if (!object->m_bOnline)
    {
        graph().remove(object, object->m_tGraphID);
        scheduled().remove(object);
    }
    else if (object->ID_Parent == 0xffff)
    {
        //			if (object->used_ai_locations())
        graph().level().remove(object, !object->used_ai_locations());
    }
}

void CALifeSimulatorBase::on_death(CSE_Abstract* killed, CSE_Abstract* killer)
{
    typedef CSE_ALifeOnlineOfflineGroup::MEMBER GROUP_MEMBER;

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(killed);
    if (creature)
        creature->on_death(killer);

    GROUP_MEMBER* member = smart_cast<GROUP_MEMBER*>(killed);
    if (!member)
        return;

    if (member->m_group_id == 0xffff)
        return;

    // A member can outlive its squad (the squad is released while the corpse is still around), and
    // the reference-returning groups().object() dereferences end() in release builds.
    CSE_ALifeOnlineOfflineGroup* group = groups().object(member->m_group_id, true);
    if (!group)
    {
        Msg("! [GW] on_death: object [%s] id=%u refers to squad %u which no longer exists, membership cleared",
            member->name_replace(), u32(member->ID), u32(member->m_group_id));
        member->m_group_id = 0xffff;
        return;
    }

    group->notify_on_member_death(member);
}
