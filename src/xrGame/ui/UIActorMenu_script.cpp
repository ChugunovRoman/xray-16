////////////////////////////////////////////////////////////////////////////
//	Module 		: UIActorMenu_script.cpp
//	Created 	: 18.04.2008
//	Author		: Evgeniy Sokolov
//	Description : UI ActorMenu script implementation
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "UIActorMenu.h"
#include "UIPdaWnd.h"
#include "Actor.h"
#include "inventory_item.h"
#include "UICellItem.h"
#include "ai_space.h"
#include "UIGameCustom.h"
#include "xrUICore/Windows/UIWindow.h"
#include "UICellItemFactory.h"
#include "UIDragDropListEx.h"
#include "UIDragDropReferenceList.h"
#include "UICellCustomItems.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrScriptEngine/ScriptExporter.hpp"
#include "xrUICore/TabControl/UITabControl.h"
#include "xrGame/ui/UIMainIngameWnd.h"
#include "eatable_item.h"
#include "InventoryBox.h"
#include "UIPdaWnd.h"
#include "UIActorMenu.h"
#include "UIMainIngameWnd.h"
#include "UIZoneMap.h"
#include "UIMotionIcon.h"
#include "UIHudStatesWnd.h"
#include "script_game_object.h"
#include "script_game_object_impl.h"

void CUIActorMenu::TryRepairItem(CUIWindow* w, void* d)
{
    PIItem item = get_upgrade_item();
    if (!item)
    {
        return;
    }
    if (item->GetCondition() > 0.99f)
    {
        return;
    }
    LPCSTR item_name = item->m_section_id.c_str();

    CEatableItem* EItm = smart_cast<CEatableItem*>(item);
    if (EItm)
    {
        bool allow_repair = !!READ_IF_EXISTS(pSettings, r_bool, item_name, "allow_repair", false);
        if (!allow_repair)
            return;
    }

    LPCSTR partner = m_pPartnerInvOwner->CharacterInfo().Profile().c_str();

    luabind::functor<bool> funct;
    R_ASSERT2(GEnv.ScriptEngine->functor("inventory_upgrades.can_repair_item", funct),
        make_string("Failed to get functor <inventory_upgrades.can_repair_item>, item = %s", item_name));
    bool can_repair = funct(item_name, item->GetCondition(), partner);

    luabind::functor<LPCSTR> funct2;
    R_ASSERT2(GEnv.ScriptEngine->functor("inventory_upgrades.question_repair_item", funct2),
        make_string("Failed to get functor <inventory_upgrades.question_repair_item>, item = %s", item_name));
    LPCSTR question = funct2(item_name, item->GetCondition(), can_repair, partner);

    if (can_repair)
    {
        m_repair_mode = true;
        CallMessageBoxYesNo(question);
    }
    else
        CallMessageBoxOK(question);
}

void CUIActorMenu::RepairEffect_CurItem()
{
    PIItem item = CurrentIItem();
    if (!item)
    {
        return;
    }
    LPCSTR item_name = item->m_section_id.c_str();

    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("inventory_upgrades.effect_repair_item", funct))
        funct(item_name, item->GetCondition());

    item->SetCondition(1.0f);
    SeparateUpgradeItem();
    CUICellItem* itm = CurrentItem();
    if (itm)
        itm->UpdateConditionProgressBar();
}

bool CUIActorMenu::CanUpgradeItem(PIItem item)
{
    VERIFY(item && m_pPartnerInvOwner);
    LPCSTR item_name = item->m_section_id.c_str();
    LPCSTR partner = m_pPartnerInvOwner->CharacterInfo().Profile().c_str();

    luabind::functor<bool> funct;
    R_ASSERT2(GEnv.ScriptEngine->functor("inventory_upgrades.can_upgrade_item", funct),
        make_string("Failed to get functor <inventory_upgrades.can_upgrade_item>, item = %s, mechanic = %s", item_name,
            partner));

    return funct(item_name, partner);
}

void CUIActorMenu::CurModeToScript()
{
    int mode = (int)m_currMenuMode;
    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("actor_menu.actor_menu_mode", funct))
        funct(mode);
}

template<class T>
class enum_dummy {};

SCRIPT_EXPORT(CUIActorMenu, (CUIDialogWnd),
{
    using namespace luabind;

    module(luaState)
    [
        class_<enum_dummy<EDDListType>>("EDDListType")
            .enum_("EDDListType")
            [
                value("iActorBag", int(EDDListType::iActorBag)),
                value("iActorBelt", int(EDDListType::iActorBelt)),
                value("iActorSlot", int(EDDListType::iActorSlot)),
                value("iActorTrade", int(EDDListType::iActorTrade)),
                value("iDeadBodyBag", int(EDDListType::iDeadBodyBag)),
                value("iInvalid", int(EDDListType::iInvalid)),
                value("iPartnerTrade", int(EDDListType::iPartnerTrade)),
                value("iPartnerTradeBag", int(EDDListType::iPartnerTradeBag)),
                value("iQuickSlot", int(EDDListType::iQuickSlot)),
                value("iTrashSlot", int(EDDListType::iTrashSlot))
            ],

        class_<CUIActorMenu, CUIDialogWnd>("CUIActorMenu")
            .def(constructor<>())
            .def("get_drag_item", &CUIActorMenu::GetCurrentItemAsGameObject)
            .def("highlight_section_in_slot", &CUIActorMenu::HighlightSectionInSlot)
            .def("highlight_for_each_in_slot", &CUIActorMenu::HighlightForEachInSlot)
            .def("refresh_current_cell_item", &CUIActorMenu::RefreshCurrentItemCell)
            .def("CallMessageBoxYesNo", &CUIActorMenu::CallMessageBoxYesNo)
            .def("CallMessageBoxOK", &CUIActorMenu::CallMessageBoxOK)
            .def("IsShown", &CUIActorMenu::IsShown)
            .def("ShowDialog", &CUIActorMenu::ShowDialog)
            .def("HideDialog", &CUIActorMenu::HideDialog)
            .def("ToSlot", +[](CUIActorMenu* self, CScriptGameObject* GO,  const bool force_place, u16 slot_id) -> bool {
              CInventoryItem* iitem = smart_cast<CInventoryItem*>(GO->object().dcast_GameObject());

              if (!iitem || !self->m_pActorInvOwner->inventory().InRuck(iitem))
                  return false;

              CUIDragDropListEx* invlist = self->GetListByType(iActorBag);
              CUICellContainer* c = invlist->GetContainer();
              CUIWindow::WINDOW_LIST child_list = c->GetChildWndList();

              for (auto& it : child_list)
              {
                  CUICellItem* i = static_cast<CUICellItem*>(it);
                  const PIItem pitm = static_cast<PIItem>(i->m_pData);
                  if (pitm == iitem)
                  {
                      self->ToSlot(i, force_place, slot_id);
                      return true;
                  }
              }

              return false;
            })
            .def("ToBelt", +[](CUIActorMenu* self, CScriptGameObject* GO, const bool b_use_cursor_pos) -> bool {
              CInventoryItem* iitem = smart_cast<CInventoryItem*>(GO->object().dcast_GameObject());

            if (!iitem || !self->m_pActorInvOwner->inventory().InRuck(iitem))
              return false;

            CUIDragDropListEx* invlist = self->GetListByType(iActorBag);
            CUICellContainer* c = invlist->GetContainer();
            CUIWindow::WINDOW_LIST child_list = c->GetChildWndList();

              for (auto& it : child_list)
              {
                  CUICellItem* i = static_cast<CUICellItem*>(it);
                  const PIItem pitm = static_cast<PIItem>(i->m_pData);
                  if (pitm == iitem)
                  {
                      self->ToBelt(i, b_use_cursor_pos);
                      return true;
                  }
              }

            return false;
            })
            .def("SetMenuMode", &CUIActorMenu::SetMenuMode)
            .def("GetMenuMode", &CUIActorMenu::GetMenuMode)
            .def("GetPartner", +[](CUIActorMenu* self) -> CScriptGameObject* {
              CInventoryOwner* io = self->GetPartner();
              if (io)
              {
                  CGameObject* GO = smart_cast<CGameObject*>(io);
                  return GO->lua_game_object();
              }

              return nullptr;
            })
            .def("GetInvBox", +[](CUIActorMenu* self)  -> CScriptGameObject* {
              CInventoryBox* inv_box = self->GetInvBox();
              if (inv_box)
              {
                  CGameObject* GO = smart_cast<CGameObject*>(inv_box);
                  return GO->lua_game_object();
              }

              return nullptr;
            })
            .def("SetPartner", +[](CUIActorMenu* self, CScriptGameObject* GO) {
              CInventoryOwner* io = GO->object().cast_inventory_owner();
              if (io)
                  self->SetPartner(io);
            })
            .def("SetInvBox", +[](CUIActorMenu* self, CScriptGameObject* GO) {
              CInventoryBox* inv_box = smart_cast<CInventoryBox*>(&GO->object());
              if (inv_box)
                  self->SetInvBox(inv_box);
            })
            .def("SetActor", +[](CUIActorMenu* self) {
              self->SetActor(Actor()->cast_inventory_owner());
            })
    ];

    using namespace luabind;

    module(luaState, "ActorMenu")
    [
        def("get_pda_menu", +[](){ return &CurrentGameUI()->GetPdaMenu(); }),
        def("get_actor_menu", +[](){ return &CurrentGameUI()->GetActorMenu(); }),
        def("get_menu_mode", +[](){ return CurrentGameUI()->GetActorMenu().GetMenuMode(); }),
        def("get_maingame", +[](){ return CurrentGameUI()->UIMainIngameWnd; })
    ];
});

SCRIPT_EXPORT(CUIPdaWnd, (CUIDialogWnd),
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUIPdaWnd, CUIDialogWnd>("CUIPdaWnd")
            .def(constructor<>())
            .def("IsShown", &CUIPdaWnd::IsShown)
            .def("ShowDialog", &CUIPdaWnd::ShowDialog)
            .def("HideDialog", &CUIPdaWnd::HideDialog)
            .def("SetActiveSubdialog", &CUIPdaWnd::SetActiveSubdialog_script)
            .def("GetActiveSection", &CUIPdaWnd::GetActiveSection)
            .def("GetTabControl", &CUIPdaWnd::GetTabControl)
    ];
});
