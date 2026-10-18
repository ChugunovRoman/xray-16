////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_stalker_events.cpp
//	Created 	: 26.02.2003
//  Modified 	: 26.02.2003
//	Author		: Dmitriy Iassenev
//	Description : Events handling for monster "Stalker"
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ai_stalker.h"
#include "PDA.h"
#include "Inventory.h"
#include "xrServerEntities/xrMessages.h"
#include "ShootingObject.h"
#include "Level.h"
#include "ai_monster_space.h"
#include "CharacterPhysicsSupport.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

using namespace StalkerSpace;
using namespace MonsterSpace;

// 30-game-events.md (plans/optimization_mt), B2 (always on since 2026-10-18, its switch npc_reject_destroy_skip_bones
// removed, 41-cvar-cleanup.md): a reject with just_before_destroy (the server destroys the item right behind it: a
// release of an item in the inventory) does not update the owner's physics and bones first (in_UpdateCL +
// CalculateBones(true)) - they are for the position of an item that stays in the world; the item lives until the
// destroy phase of this frame.

#define SILENCE

void CAI_Stalker::OnEvent(NET_Packet& P, u16 type)
{
    inherited::OnEvent(P, type);
    CInventoryOwner::OnEvent(P, type);

    switch (type)
    {
    case GE_TRADE_BUY:
    case GE_OWNERSHIP_TAKE:
    {
        u16 id;
        P.r_u16(id);
        IGameObject* O = Level().Objects.net_Find(id);

        if (!O)
        {
#ifndef MASTER_GOLD
            Msg("! ERROR: GE_OWNERSHIP_TAKE / GE_TRADE_BUY: object id[%u] not found (stale event or spawn order)",
                (u32)id);
#endif
            break;
        }

#ifndef SILENCE
        Msg("Trying to take - %s (%d)", *O->cName(), O->ID());
#endif
        CGameObject* _O = smart_cast<CGameObject*>(O);
        CInventoryItem* pItem = smart_cast<CInventoryItem*>(_O);
        if (!pItem)
        {
#ifndef MASTER_GOLD
            Msg("! ERROR: GE_OWNERSHIP_TAKE / GE_TRADE_BUY: id[%u] is not CInventoryItem (%s)", (u32)id,
                O->cName().c_str());
#endif
            NET_Packet P2;
            u_EventGen(P2, GE_OWNERSHIP_REJECT, ID());
            P2.w_u16(id);
            u_EventSend(P2);
            break;
        }

        if (inventory().CanTakeItem(pItem))
        {
            O->H_SetParent(this);
            inventory().Take(_O, true, false);
            if (!inventory().ActiveItem() && GetScriptControl() && smart_cast<CShootingObject*>(O))
                CObjectHandler::set_goal(eObjectActionIdle, _O);

            on_after_take(_O);
#ifndef SILENCE
            Msg("TAKE - %s (%d)", *O->cName(), O->ID());
#endif
        }
        else
        {
            //				DropItemSendMessage(O);
            NET_Packet P2;
            u_EventGen(P2, GE_OWNERSHIP_REJECT, ID());
            P2.w_u16(u16(O->ID()));
            u_EventSend(P2);

#ifndef SILENCE
            Msg("TAKE - can't take! - Dropping for valid server information %s (%d)", *O->cName(), O->ID());
#endif
        }
        break;
    }
    case GE_TRADE_SELL:
    case GE_OWNERSHIP_REJECT:
    {
        u16 id;
        P.r_u16(id);

        IGameObject* O = Level().Objects.net_Find(id);

#pragma todo("Dima to Oles : how can this happen?")
        if (!O)
            break;

        bool just_before_destroy = !P.r_eof() && P.r_u8();
        bool dont_create_shell = (type == GE_TRADE_SELL) || just_before_destroy;

        O->SetTmpPreDestroy(just_before_destroy);
        on_ownership_reject(O, dont_create_shell, just_before_destroy);

        break;
    }
    }
}

void CAI_Stalker::on_ownership_reject(IGameObject* O, bool just_before_destroy, bool skip_bones)
{
    if (skip_bones)
        PERF_INC(reject_bones_skipped); // B2: the item is destroyed in this frame, its position does not matter
    else
    {
        PERF_SCOPE(reject_bones); // 30-game-events.md
        m_pPhysics_support->in_UpdateCL();
        IKinematics* const kinematics = smart_cast<IKinematics*>(Visual());
        kinematics->CalculateBones_Invalidate();
        kinematics->CalculateBones(true);
    }

    CGameObject* const game_object = smart_cast<CGameObject*>(O);
    VERIFY(game_object);

    bool dropped;
    {
        PERF_SCOPE(reject_drop); // 30-game-events.md: DropItem with the eOnItemDrop callback and the bus inside
        dropped = inventory().DropItem(game_object, just_before_destroy, just_before_destroy);
    }
    if (!dropped)
        return;

    if (O->getDestroy())
        return;

    feel_touch_deny(O, 2000);
}

void CAI_Stalker::generate_take_event(IGameObject const* const object) const
{
    NET_Packet packet;
    u_EventGen(packet, GE_OWNERSHIP_TAKE, ID());
    packet.w_u16(object->ID());
    u_EventSend(packet);
}

void CAI_Stalker::DropItemSendMessage(IGameObject* O)
{
    if (!O || !O->H_Parent() || (this != O->H_Parent()))
        return;

#ifndef SILENCE
    Msg("Dropping item!");
#endif
    // We doesn't have similar weapon - pick up it
    NET_Packet P;
    u_EventGen(P, GE_OWNERSHIP_REJECT, ID());
    P.w_u16(u16(O->ID()));
    u_EventSend(P);
}

void CAI_Stalker::UpdateAvailableDialogs(CPhraseDialogManager* partner)
{
    CAI_PhraseDialogManager::UpdateAvailableDialogs(partner);
}

void CAI_Stalker::feel_touch_new(IGameObject* O)
{
    //	Msg					("FEEL_TOUCH::NEW : %s",*O->cName());
    if (!g_Alive())
        return;
    if (Remote())
        return;
    if ((O->GetSpatialData().type | STYPE_VISIBLEFORAI) != O->GetSpatialData().type)
        return;

    // Now, test for game specific logical objects to minimize traffic
    CInventoryItem* I = smart_cast<CInventoryItem*>(O);

    if (!wounded() && !critically_wounded() && I && I->useful_for_NPC() && can_take(I))
    {
#ifndef SILENCE
        Msg("Taking item %s (%d)!", I->object().cName().c_str(), I->object().ID());
#endif
        generate_take_event(O);
        return;
    }

    VERIFY2(std::find(m_ignored_touched_objects.begin(), m_ignored_touched_objects.end(), O) ==
            m_ignored_touched_objects.end(),
        make_string("object %s is already in ignroed touched objects list", O->cName().c_str()));
    m_ignored_touched_objects.push_back(O);
}
