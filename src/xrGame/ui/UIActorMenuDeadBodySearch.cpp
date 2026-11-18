#include "StdAfx.h"
#include "UIActorMenu.h"
#include "UIWeightBar.h"
#include "UIDragDropListEx.h"
#include "UICharacterInfo.h"
#include "UIInventoryUtilities.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "UICellItem.h"
#include "UICellItemFactory.h"
#include "xrUICore/Windows/UIFrameLineWnd.h"
#include "xrMessages.h"
#include "alife_registry_wrappers.h"
#include "GameObject.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryBox.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "UIDragDropReferenceList.h"
#include "Level.h"
#include "script_game_object.h"
#include "xrScriptEngine/script_engine.hpp"

void move_item_from_to(u16 from_id, u16 to_id, u16 what_id)
{
    NET_Packet P;
    CGameObject::u_EventGen(P, GE_TRADE_SELL, from_id);
    P.w_u16(what_id);
    CGameObject::u_EventSend(P);

    //другому инвентарю - взять вещь
    CGameObject::u_EventGen(P, GE_TRADE_BUY, to_id);
    P.w_u16(what_id);
    CGameObject::u_EventSend(P);
}

bool move_item_check(PIItem itm, CInventoryOwner* from, CInventoryOwner* to, bool weight_check)
{
    if (weight_check)
    {
        float invWeight = to->inventory().CalcTotalWeight();
        float maxWeight = to->MaxCarryWeight();
        float itmWeight = itm->Weight();
        if (invWeight + itmWeight >= maxWeight)
        {
            return false;
        }
    }
    move_item_from_to(from->object_id(), to->object_id(), itm->object_id());
    return true;
}

// -------------------------------------------------------------------------------------------------

void CUIActorMenu::InitDeadBodySearchMode()
{
    // npc_equipment.md: the equipment mode, when Lua asked for it (ActorMenu.set_partner_equipment) for this live
    // partner just now; a monster, a box, a corpse - never. The request is used up by this opening
    m_pEquipOwner = nullptr;
    if (m_pPartnerInvOwner && m_partner_equipment_request_id != u16(-1) &&
        m_partner_equipment_request_id == m_pPartnerInvOwner->object_id() &&
        Device.dwTimeGlobal - m_partner_equipment_request_time <= PARTNER_EQUIPMENT_REQUEST_MS &&
        !smart_cast<CBaseMonster*>(m_pPartnerInvOwner) && m_pPartnerInvOwner->is_alive())
    {
        m_pEquipOwner = m_pPartnerInvOwner;
    }
    m_partner_equipment_request_id = u16(-1);

    ShowIfExist(m_pSearchLootWnd, true);
    m_pLists[eSearchLootBagList]->Show(true);
    m_pLists[eSearchLootActorBagList]->Show(true);
    ShowIfExist(m_LeftBackground, true);
    m_PartnerWeightBar->Show(true);
    m_takeall_button->Show(true);
    GetModeSpecificPartnerInfo(mmDeadBodySearch)->Show(nullptr != m_pPartnerInvOwner);

    SetInvGridSize(m_pLists[eSearchLootBagList]);
    SetInvGridSize(m_pLists[eSearchLootActorBagList]);
    SetInvGridSize(m_pLists[eInventoryBagList]);

    auto partnerIcon = m_SearchLootPartnerCharacterInfo->GetParent();
    if (partnerIcon != this)
        partnerIcon->Show(false);

    InitInventoryContents(m_pLists[eSearchLootActorBagList],
        m_pLists[eSearchLootActorBagList] != m_pLists[eInventoryBagList]);

    TIItemContainer items_list;
    if (m_pPartnerInvOwner)
    {
        // npc_equipment.md: in the equipment mode the bag of the partner is filled below (its ruck only)
        if (!m_pEquipOwner)
            m_pPartnerInvOwner->inventory().AddAvailableItems(items_list, false, m_pPartnerInvOwner->is_alive());
        UpdatePartnerBag();
    }
    else
    {
        VERIFY(m_pInvBox);
        m_pInvBox->set_in_use(true);
        m_pInvBox->AddAvailableItems(items_list);
    }

    std::sort(items_list.begin(), items_list.end(), InventoryUtilities::GreaterRoomInRuck);

    TIItemContainer::iterator it = items_list.begin();
    TIItemContainer::iterator it_e = items_list.end();
    for (; it != it_e; ++it)
    {
        CUICellItem* itm = create_cell_item(*it);
        m_pLists[eSearchLootBagList]->SetItem(itm);
    }

    if (m_pEquipOwner)
    {
        FillPartnerEquipmentBag();
        // the quick slots belong to the actor: in their place - the caption of the mode
        if (m_pQuickSlot)
        {
            m_quick_slot_was_shown = m_pQuickSlot->IsShown();
            m_pQuickSlot->Show(false);
        }
        ShowIfExist(m_partner_equipment_caption, true);
        SnapshotPartnerEquipment(false);
    }

    CBaseMonster* monster = smart_cast<CBaseMonster*>(m_pPartnerInvOwner);

    // only for partner, box = no, monster = no
    if (m_pPartnerInvOwner && !monster && !m_pPartnerInvOwner->is_alive())
    {
        CInfoPortionWrapper known_info_registry;
        known_info_registry.registry().init(m_pPartnerInvOwner->object_id());
        KNOWN_INFO_VECTOR& known_infos = known_info_registry.registry().objects();

        auto it = known_infos.begin();
        for (int i = 0; it != known_infos.end(); ++it, ++i)
        {
            NET_Packet P;
            CGameObject::u_EventGen(P, GE_INFO_TRANSFER, m_pActorInvOwner->object_id());
            P.w_u16(0);
            P.w_stringZ(it->info_id.c_str());
            P.w_u8(1);
            CGameObject::u_EventSend(P);
        }
        known_infos.clear();
    }
    UpdateDeadBodyBag();
}

void CUIActorMenu::DeInitDeadBodySearchMode()
{
    // npc_equipment.md: the equipment mode ends with the window, a request not used by now is dropped
    if (m_pEquipOwner)
    {
        m_pEquipOwner = nullptr;
        if (m_pQuickSlot)
            m_pQuickSlot->Show(m_quick_slot_was_shown);
        m_partner_equipment_slots.clear();
        m_partner_equipment_belt.clear();
    }
    ShowIfExist(m_partner_equipment_caption, false);
    m_partner_equipment_request_id = u16(-1);

    ShowIfExist(m_pSearchLootWnd, false);
    m_pLists[eSearchLootBagList]->Show(false);
    m_pLists[eSearchLootActorBagList]->Show(false);
    GetModeSpecificPartnerInfo(mmDeadBodySearch)->Show(false);
    ShowIfExist(m_LeftBackground, false);
    m_PartnerWeightBar->Show(false);
    m_takeall_button->Show(false);

    if (m_pInvBox)
    {
        m_pInvBox->set_in_use(false);
    }
}

bool CUIActorMenu::ToDeadBodyBag(CUICellItem* itm, bool b_use_cursor_pos, bool with_all_childs)
{
    if (m_pPartnerInvOwner && !m_pPartnerInvOwner->deadbody_can_take_status())
        return false;
    else if (m_pInvBox && !m_pInvBox->can_take())
        return false;

    // npc_equipment.md: the ruck of the partner takes the items of the actor and its own items (slots, belt)
    if (m_pEquipOwner)
        return ToPartnerRuck(itm, b_use_cursor_pos, with_all_childs, true);

    PIItem quest_item = (PIItem)itm->m_pData;
    if (quest_item->IsQuestItem())
        return false;

    CUIDragDropListEx* old_owner = itm->OwnerList();
    CUIDragDropListEx* new_owner = nullptr;

    if (b_use_cursor_pos)
    {
        new_owner = CUIDragDropListEx::m_drag_item->BackList();
        VERIFY(new_owner == m_pLists[eSearchLootBagList]);
    }
    else
        new_owner = m_pLists[eSearchLootBagList];

    if (with_all_childs)
    {
        auto count = itm->ChildsCount();
        for (u16 k = 0; k < count; k++)
        {
            CUICellItem* inv_cell_item = itm->Child(0);
            CUICellItem* ichld = old_owner->RemoveItem(inv_cell_item, (old_owner == new_owner));

            if (b_use_cursor_pos)
                new_owner->SetItem(ichld, old_owner->GetDragItemPosition());
            else
                new_owner->SetItem(ichld);

            PIItem iitem = (PIItem)ichld->m_pData;

            if (m_pPartnerInvOwner)
                move_item_from_to(m_pActorInvOwner->object_id(), m_pPartnerInvOwner->object_id(), iitem->object_id());
            else // box
                move_item_from_to(m_pActorInvOwner->object_id(), m_pInvBox->ID(), iitem->object_id());
        }
    }

    CUICellItem* i = old_owner->RemoveItem(itm, (old_owner == new_owner));

    if (b_use_cursor_pos)
        new_owner->SetItem(i, old_owner->GetDragItemPosition());
    else
        new_owner->SetItem(i);

    PIItem iitem = (PIItem)i->m_pData;

    if (m_pPartnerInvOwner)
        move_item_from_to(m_pActorInvOwner->object_id(), m_pPartnerInvOwner->object_id(), iitem->object_id());
    else // box
        move_item_from_to(m_pActorInvOwner->object_id(), m_pInvBox->ID(), iitem->object_id());

    UpdateDeadBodyBag();
    return true;
}

void CUIActorMenu::UpdateDeadBodyBag()
{
    // npc_equipment.md: the whole load of the partner - its slots and belt are not in its bag list
    const float total = m_pEquipOwner ? m_pEquipOwner->inventory().TotalWeight() :
                                        CalcItemsWeight(m_pLists[eSearchLootBagList]);
    m_PartnerWeightBar->UpdateData(total);
}

// -------------------------------------------------------------------------------------------------
// The equipment of the partner (plans/addon_companion_extended/npc_equipment.md)
// -------------------------------------------------------------------------------------------------

void CUIActorMenu::SetPartnerEquipmentRequest(u16 partner_id)
{
    m_partner_equipment_request_id = partner_id;
    m_partner_equipment_request_time = Device.dwTimeGlobal;
}

bool CUIActorMenu::IsPartnerEquipItem(PIItem item) const
{
    return m_pEquipOwner && item && item->parent_id() == m_pEquipOwner->object_id();
}

bool CUIActorMenu::PartnerEquipmentCanMove(PIItem item, bool to_npc, pcstr place, u16 slot_id, u32 count)
{
    if (!m_pEquipOwner || !item)
        return true;

    if (to_npc && !IsPartnerEquipItem(item))
    {
        // the quest items of the actor stay with the actor (as in the usual exchange, ToDeadBodyBag)
        if (item->IsQuestItem())
            return false;
        // an NPC refuses an item it cannot carry and drops it to the ground (CInventory::CanTakeItem): refuse here
        const float weight = m_pEquipOwner->inventory().TotalWeight() + item->Weight() * float(count);
        if (weight > m_pEquipOwner->MaxCarryWeight())
        {
            ShowMessage(StringTable().translate("st_partner_equipment_overweight").c_str());
            return false;
        }
    }

    // actor_menu_inventory.script sends actor_menu_partner_equipment_before_move: a subscriber may refuse
    luabind::functor<bool> funct;
    if (!GEnv.ScriptEngine->functor("actor_menu_inventory.CUIActorMenu_PartnerEquipmentCanMove", funct))
        return true;
    CGameObject* npc = m_pEquipOwner->cast_game_object();
    CGameObject* go = item->cast_game_object();
    if (!npc || !go)
        return true;
    return funct(npc->lua_game_object(), go->lua_game_object(), to_npc, place, int(slot_id));
}

bool CUIActorMenu::ToPartnerRuck(CUICellItem* itm, bool b_use_cursor_pos, bool with_all_childs, bool ask)
{
    if (!m_pEquipOwner || !itm)
        return false;
    PIItem iitem = static_cast<PIItem>(itm->m_pData);
    CUIDragDropListEx* old_owner = itm->OwnerList();
    if (!iitem || !old_owner)
        return false;

    const u32 count = with_all_childs ? itm->ChildsCount() + 1 : 1;
    if (ask && !PartnerEquipmentCanMove(iitem, true, "ruck", 0, count))
        return false;

    CUIDragDropListEx* new_owner = m_pLists[eSearchLootBagList];
    if (b_use_cursor_pos)
    {
        new_owner = CUIDragDropListEx::m_drag_item->BackList();
        VERIFY(new_owner == m_pLists[eSearchLootBagList]);
    }

    bool own_moved = false;
    const auto place_cell = [&](CUICellItem* cell)
    {
        if (b_use_cursor_pos)
            new_owner->SetItem(cell, old_owner->GetDragItemPosition());
        else
            new_owner->SetItem(cell);

        PIItem cell_item = static_cast<PIItem>(cell->m_pData);
        if (IsPartnerEquipItem(cell_item))
        {
            // the partner's own item (its slot, belt): to its ruck at once, as ToBag does for the actor
            m_pEquipOwner->inventory().Ruck(cell_item);
            own_moved = true;
        }
        else // an item of the actor: moved to the partner, which keeps it in the ruck (not in a free slot)
            SendEvent_PartnerItem2Place(cell_item, GEG_PLAYER_ITEM2RUCK, 0);
    };

    if (with_all_childs)
    {
        const u32 cnt = itm->ChildsCount();
        for (u32 k = 0; k < cnt; ++k)
        {
            CUICellItem* child = old_owner->RemoveItem(itm->Child(0), old_owner == new_owner);
            if (child)
                place_cell(child);
        }
    }

    CUICellItem* i = old_owner->RemoveItem(itm, old_owner == new_owner);
    if (!i)
        return false;
    place_cell(i);

    if (own_moved)
    {
        PlaySnd(eItemToRuck);
        clear_highlight_lists();
    }
    UpdateDeadBodyBag();
    return true;
}

void CUIActorMenu::SendEvent_PartnerItem2Place(PIItem pItem, u16 event_type, u16 slot_id)
{
    VERIFY(m_pEquipOwner);
    const u16 npc_id = m_pEquipOwner->object_id();
    if (pItem->parent_id() != npc_id)
        move_item_from_to(pItem->parent_id(), npc_id, pItem->object_id());

    // to the partner itself (not to the owner the item has now, as SendEvent_Item2Slot does): the events are processed
    // in order, the partner has the item by then. CInventoryOwner::OnEquipmentEvent checks that it is its item
    NET_Packet P;
    CGameObject::u_EventGen(P, event_type, npc_id);
    P.w_u16(pItem->object_id());
    if (event_type == GEG_PLAYER_ITEM2SLOT)
        P.w_u16(slot_id);
    CGameObject::u_EventSend(P);

    switch (event_type)
    {
    case GEG_PLAYER_ITEM2SLOT: PlaySnd(eItemToSlot); break;
    case GEG_PLAYER_ITEM2BELT: PlaySnd(eItemToBelt); break;
    default: PlaySnd(eItemToRuck); break;
    }
    clear_highlight_lists();
}

void CUIActorMenu::SendEvent_PartnerItem2Actor(PIItem pItem)
{
    const u16 actor_id = m_pActorInvOwner->object_id();
    move_item_from_to(pItem->parent_id(), actor_id, pItem->object_id());

    // to the ruck of the actor: sent to the actor, which has the item by then (its take goes first). Otherwise the actor
    // puts a weapon / an outfit into a free slot of its own, and the slots in the window are the partner's now
    NET_Packet P;
    CGameObject::u_EventGen(P, GEG_PLAYER_ITEM2RUCK, actor_id);
    P.w_u16(pItem->object_id());
    CGameObject::u_EventSend(P);

    PlaySnd(eItemToRuck);
    clear_highlight_lists();
}

void CUIActorMenu::ReadPartnerEquipment(xr_vector<u16>& slots, xr_vector<u16>& belt) const
{
    slots.clear();
    belt.clear();
    if (!m_pEquipOwner)
        return;

    const CInventory& inv = m_pEquipOwner->inventory();
    const u16 last = inv.LastSlot();
    slots.assign(last + 1, u16(-1));
    for (u16 i = inv.FirstSlot(); i <= last; ++i)
    {
        if (const PIItem item = inv.ItemFromSlot(i))
            slots[i] = item->object_id();
    }
    for (const PIItem item : inv.m_belt)
        belt.push_back(item->object_id());
}

void CUIActorMenu::SnapshotPartnerEquipment(bool notify)
{
    xr_vector<u16> slots, belt;
    ReadPartnerEquipment(slots, belt);
    const xr_vector<u16> old_slots = std::move(m_partner_equipment_slots);
    const xr_vector<u16> old_belt = std::move(m_partner_equipment_belt);
    m_partner_equipment_slots = slots;
    m_partner_equipment_belt = belt;
    m_partner_equipment_frame = m_pEquipOwner ? m_pEquipOwner->inventory().ModifyFrame() : 0;

    if (!notify || !m_pEquipOwner)
        return;

    // actor_menu_inventory.script sends actor_menu_partner_equipment_changed(npc, place, slot, item, old_id)
    luabind::functor<void> funct;
    if (!GEnv.ScriptEngine->functor("actor_menu_inventory.CUIActorMenu_OnPartnerEquipmentChanged", funct))
        return;

    const u16 npc_id = m_pEquipOwner->object_id();
    const auto lua_object = [](u16 id) -> CScriptGameObject*
    {
        if (id == u16(-1))
            return nullptr;
        CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
        return go ? go->lua_game_object() : nullptr;
    };
    // a handler may close the window or move items: the partner is looked up for every call, the vectors are copies
    const auto send = [&](pcstr place, u16 slot_id, u16 item_id, u16 old_id) -> bool
    {
        CScriptGameObject* npc = lua_object(npc_id);
        if (!npc)
            return false;
        funct(npc, place, int(slot_id), lua_object(item_id), old_id == u16(-1) ? -1 : int(old_id));
        return true;
    };

    for (u16 i = 0; i < slots.size(); ++i)
    {
        const u16 old_id = i < old_slots.size() ? old_slots[i] : u16(-1);
        if (old_id != slots[i] && !send("slot", i, slots[i], old_id))
            return;
    }
    for (const u16 old_id : old_belt)
    {
        if (std::find(belt.begin(), belt.end(), old_id) == belt.end() && !send("belt", 0, u16(-1), old_id))
            return;
    }
    for (const u16 id : belt)
    {
        if (std::find(old_belt.begin(), old_belt.end(), id) == old_belt.end() && !send("belt", 0, id, u16(-1)))
            return;
    }
}

void CUIActorMenu::FillPartnerEquipmentBag()
{
    // the ruck only: the slots and the belt of the partner are in their lists. The filter of the base / addons
    // (actor_menu_inventory.CInventory_ItemAvailableToTrade) still hides what they hide
    TIItemContainer items_list;
    CInventory& inv = m_pEquipOwner->inventory();
    inv.AddAvailableItems(items_list, false, true);
    items_list.erase(std::remove_if(items_list.begin(), items_list.end(),
                         [&inv](PIItem item) { return !inv.InRuck(item); }),
        items_list.end());
    std::sort(items_list.begin(), items_list.end(), InventoryUtilities::GreaterRoomInRuck);

    for (PIItem item : items_list)
        m_pLists[eSearchLootBagList]->SetItem(create_cell_item(item));
}

void CUIActorMenu::InitPartnerEquipmentContents()
{
    CUIDragDropListEx* lists[] =
    {
        m_pLists[eInventoryKnifeList], m_pLists[eInventoryPistolList], m_pLists[eInventoryAutomaticList],
        m_pLists[eInventoryBackpackList], m_pLists[eInventoryOutfitList], m_pLists[eInventoryHelmetList],
        m_pLists[eInventoryBinocularList], m_pLists[eInventoryGrenadeList], m_pLists[eInventoryBoltList],
        m_pLists[eInventoryPdaList], m_pLists[eInventoryDetectorList], m_pLists[eInventoryBeltList],
        m_pLists[eSearchLootBagList]
    };

    // the current cell goes away with its list
    if (m_pCurrentCellItem)
    {
        for (CUIDragDropListEx* lst : lists)
        {
            if (lst && lst->IsOwner(m_pCurrentCellItem))
            {
                SetCurrentItem(nullptr);
                break;
            }
        }
    }

    for (CUIDragDropListEx* lst : lists)
    {
        if (lst)
            lst->ClearAll(true);
    }

    InitEquipmentCells();
    FillPartnerEquipmentBag();
    UpdateOutfit();
    UpdateDeadBodyBag();
    SnapshotPartnerEquipment(true);
}

void CUIActorMenu::UpdatePartnerEquipment()
{
    // the partner died or went away: the window closes (its slots would show a corpse or nothing)
    if (!EnsureTradePartnerValid() || !m_pPartnerInvOwner->is_alive())
    {
        HideDialog();
        return;
    }
    m_pEquipOwner = m_pPartnerInvOwner;

    // not under a dragged cell: the next frame
    if (CUIDragDropListEx::m_drag_item)
        return;

    // the partner changed its inventory (the window, the events of a move, its planner taking another weapon)
    xr_vector<u16> slots, belt;
    ReadPartnerEquipment(slots, belt);
    if (m_pEquipOwner->inventory().ModifyFrame() != m_partner_equipment_frame || slots != m_partner_equipment_slots ||
        belt != m_partner_equipment_belt)
    {
        InitPartnerEquipmentContents();
    }
}

void CUIActorMenu::TakeAllFromPartner(CUIWindow* w, void* d)
{
    VERIFY(m_pActorInvOwner);
    if (!m_pPartnerInvOwner)
    {
        if (m_pInvBox)
        {
            TakeAllFromInventoryBox();
        }
        return;
    }

    u32 const cnt = m_pLists[eSearchLootBagList]->ItemsCount();
    for (u32 i = 0; i < cnt; ++i)
    {
        CUICellItem* ci = m_pLists[eSearchLootBagList]->GetItemIdx(i);
        for (u32 j = 0; j < ci->ChildsCount(); ++j)
        {
            PIItem j_item = (PIItem)(ci->Child(j)->m_pData);
            move_item_check(j_item, m_pPartnerInvOwner, m_pActorInvOwner, false);
        }
        PIItem item = (PIItem)(ci->m_pData);
        move_item_check(item, m_pPartnerInvOwner, m_pActorInvOwner, false);
    } // for i
    m_pLists[eSearchLootBagList]->ClearAll(true); // false
}

void CUIActorMenu::StoreAllToPartner(CUIWindow* w, void* d)
{
    VERIFY(m_pActorInvOwner);
    if (!m_pPartnerInvOwner)
    {
        if (m_pInvBox)
        {
            StoreAllToInventoryBox();
        }
        return;
    }

    u32 const cnt = m_pLists[eInventoryBagList]->ItemsCount();
    for (u32 i = 0; i < cnt; ++i)
    {
        CUICellItem* ci = m_pLists[eInventoryBagList]->GetItemIdx(i);
        for (u32 j = 0; j < ci->ChildsCount(); ++j)
        {
            PIItem j_item = (PIItem)(ci->Child(j)->m_pData);
            move_item_check(j_item, m_pActorInvOwner, m_pPartnerInvOwner, false);
        }
        PIItem item = (PIItem)(ci->m_pData);
        move_item_check(item, m_pActorInvOwner, m_pPartnerInvOwner, false);
    } // for i
    m_pLists[eInventoryBagList]->ClearAll(true); // false
}

void CUIActorMenu::TakeAllFromInventoryBox()
{
    u16 actor_id = m_pActorInvOwner->object_id();

    u32 const cnt = m_pLists[eSearchLootBagList]->ItemsCount();
    for (u32 i = 0; i < cnt; ++i)
    {
        CUICellItem* ci = m_pLists[eSearchLootBagList]->GetItemIdx(i);
        for (u32 j = 0; j < ci->ChildsCount(); ++j)
        {
            PIItem j_item = (PIItem)(ci->Child(j)->m_pData);
            move_item_from_to(m_pInvBox->ID(), actor_id, j_item->object_id());
        }

        PIItem item = (PIItem)(ci->m_pData);
        move_item_from_to(m_pInvBox->ID(), actor_id, item->object_id());
    } // for i
    m_pLists[eSearchLootBagList]->ClearAll(true); // false
}

void CUIActorMenu::StoreAllToInventoryBox()
{
    u16 actor_id = m_pActorInvOwner->object_id();

    u32 const cnt = m_pLists[eInventoryBagList]->ItemsCount();
    for (u32 i = 0; i < cnt; ++i)
    {
        CUICellItem* ci = m_pLists[eInventoryBagList]->GetItemIdx(i);
        for (u32 j = 0; j < ci->ChildsCount(); ++j)
        {
            PIItem j_item = (PIItem)(ci->Child(j)->m_pData);
            move_item_from_to(actor_id, m_pInvBox->ID(), j_item->object_id());
        }
        PIItem item = (PIItem)(ci->m_pData);
        move_item_from_to(actor_id, m_pInvBox->ID(), item->object_id());
    }
    m_pLists[eInventoryBagList]->ClearAll(true); // false
}
