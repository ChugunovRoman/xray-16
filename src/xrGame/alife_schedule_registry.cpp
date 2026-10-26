////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry.сзз
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_schedule_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"

CALifeScheduleRegistry::~CALifeScheduleRegistry() {}

// Only with -npc_cpp_profile (CUpdatePredicate checks it). The kind is taken before update(): an update can release
// the object (the delete is postponed until the pass ends, CALifeUpdateManager::update_scheduled), so it is not
// asked afterwards. Squads before monsters; offline stalkers are monsters too (CSE_ALifeHumanAbstract).
void CALifeScheduleRegistry::update_profiled(CSE_ALifeSchedulable& object)
{
    CSE_Abstract* base = object.base();
    ENpcCppProfileStage stage = ENpcCppProfileStage::AlifeScheduledOther;
    if (smart_cast<CSE_ALifeOnlineOfflineGroup*>(base))
        stage = ENpcCppProfileStage::AlifeScheduledSquad;
    else if (smart_cast<CSE_ALifeSmartZone*>(base))
        stage = ENpcCppProfileStage::AlifeScheduledSmartTerrain;
    else if (smart_cast<CSE_ALifeMonsterAbstract*>(base))
        stage = ENpcCppProfileStage::AlifeScheduledMonster;

    const u64 start = CPU::QPC();
    update_object(object); // a plugin update hook included (addon_alife_hooks.cpp)
    const u64 delta = CPU::QPC() - start;
    npc_cpp_profile::add(stage, delta);
    // 57-gw-eoc-next-steps.md, E2, "alife peak": the object for the line of a long alife/update. Read after update(),
    // as describe_spike does: a released object is deleted only when the pass ends.
    if (base && npc_cpp_profile::peak_active())
        npc_cpp_profile::peak_object(base->ID, base->name_replace(), base->s_name.c_str(), stage, delta);
}
void CALifeScheduleRegistry::add(CSE_ALifeDynamicObject* object)
{
    CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(object);
    if (!schedulable)
        return;

    if (!schedulable->need_update(object))
        return;

    inherited::add(object->ID, schedulable);
}

void CALifeScheduleRegistry::remove(CSE_ALifeDynamicObject* object, bool no_assert)
{
    CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(object);
    if (!schedulable)
        return;

    inherited::remove(object->ID, no_assert || !schedulable->need_update(object));
}

// 32-frame-spikes.md, A1: "[perf-spike] alife/object" (CUpdateSpreadPredicate). The Game thread, inside the pass
void CALifeScheduleRegistry::describe_spike(const void* ctx, char* buf, size_t size) noexcept
{
    const CSE_Abstract* base = static_cast<const CSE_ALifeSchedulable*>(ctx)->base();
    if (!base)
    {
        xr_sprintf(buf, size, "obj=?");
        return;
    }
    LPCSTR name = base->name_replace();
    xr_sprintf(buf, size, "obj=%s sect=%s id=%u", name ? name : "", base->s_name.c_str() ? base->s_name.c_str() : "",
        static_cast<u32>(base->ID));
}
