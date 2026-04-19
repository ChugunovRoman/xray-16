////////////////////////////////////////////////////////////////////////////
//	Module 		: UIInventoryUpgradeWnd.cpp
//	Created 	: 06.10.2007
//  Modified 	: 13.03.2009
//	Author		: Evgeniy Sokolov, Prishchepa Sergey
//	Description : inventory upgrade UI window class implementation
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "Common/object_broker.h"
#include "UIInventoryUpgradeWnd.h"

#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "Actor.h"
#include "xrScriptEngine/script_process.hpp"
#include "Inventory.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "inventory_upgrade_manager.h"
#include "inventory_upgrade.h"
#include "inventory_upgrade_property.h"
#include "UIInventoryUtilities.h"
#include "UIActorMenu.h"
#include "UIItemInfo.h"
#include "xrUICore/Windows/UIFrameLineWnd.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "UIHelper.h"
#include "xrUICore/ui_defs.h"
#include "Weapon.h"
#include "weapon_inv_icon.h"
#include "WeaponRPG7.h"
#include "CustomOutfit.h"
#include "ActorHelmet.h"
#include "script_game_object.h" //Alundaio

const LPCSTR g_inventory_upgrade_xml = "inventory_upgrade.xml";

CUIInventoryUpgradeWnd::Scheme::Scheme() {}
CUIInventoryUpgradeWnd::Scheme::~Scheme() { delete_data(cells); }
// =============================================================================================

CUIInventoryUpgradeWnd::CUIInventoryUpgradeWnd() : CUIWindow(CUIInventoryUpgradeWnd::GetDebugType())
{
    // m_WeaponIconsShader = new ui_shader();
    //(*m_WeaponIconsShader)->create("hud" DELIMITER "default", "ui" DELIMITER "ui_actor_weapons");
    // m_OutfitIconsShader = new ui_shader();
    //(*m_OutfitIconsShader)->create("hud" DELIMITER "default", "ui" DELIMITER "ui_actor_armor");
}

CUIInventoryUpgradeWnd::~CUIInventoryUpgradeWnd()
{
    delete_data(m_schemes);
    // xr_delete(m_WeaponIconsShader);
    // xr_delete(m_OutfitIconsShader);
    // m_WeaponIconsShader = 0;
    // m_OutfitIconsShader = 0;
}

bool CUIInventoryUpgradeWnd::Init()
{
    CUIXml uiXml;
    if (!uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, g_inventory_upgrade_xml, false))
        return false;

    CUIXmlInit::InitWindow(uiXml, "main", 0, this);
    m_border_texture = uiXml.ReadAttrib("border", 0, "texture");
    m_ink_texture = uiXml.ReadAttrib("inking", 0, "texture");

    m_background = UIHelper::CreateStatic(uiXml, "background", this, false);
    m_item = UIHelper::CreateStatic(uiXml, "item_static", this, false);
    m_back = UIHelper::CreateNormalWindow(uiXml, "back", this, false);
    m_scheme_wnd = UIHelper::CreateNormalWindow(uiXml, "scheme", this);

    m_item_info = xr_new<CUIItemInfo>();
    if (m_item_info->InitItemInfo("inventory_upgrade_info.xml"))
    {
        m_item_info->SetAutoDelete(true);
        AttachChild(m_item_info);
    }
    else
    {
        xr_delete(m_item_info);
    }

    m_btn_repair = UIHelper::Create3tButton(uiXml, "repair_button", this);
    CUIActorMenu* parent_wnd = smart_cast<CUIActorMenu*>(m_pParentWnd);
    if (parent_wnd)
    {
        // XXX: restore set_hind_wnd?
        //m_btn_repair->set_hint_wnd(parent_wnd->get_hint_wnd());
    }

    LoadCellsBacks(uiXml);
    LoadSchemes(uiXml);
    return true;
}

void CUIInventoryUpgradeWnd::RefreshWeaponItemPortrait()
{
    VERIFY(m_item && m_inv_item);
    const Irect item_upgrade_grid_rect = Irect().set(0, 0, 350, 200);
    CWeapon* wpn = smart_cast<CWeapon*>(m_inv_item);
    bool used_rt = false;
    if (wpn && weapon_inv_icon::IsEnabledForItem(wpn))
    {
        if (wpn->DynamicInvIconPresetReady(eWpnInvIcon_Technician))
        {
            shared_str dyn = weapon_inv_icon::TextureResourceName(wpn, eWpnInvIcon_Technician);
            m_item->SetShader(InventoryUtilities::GetInstanceRtIconShader(dyn.c_str()));
            Frect texture_rect;
            Fvector2 ts{};
            if (m_item->GetUIStaticItem().GetShader()->GetBaseTextureResolution(ts) && ts.x > 0.f && ts.y > 0.f)
            {
                texture_rect.lt.set(0.f, 0.f);
                texture_rect.rb.set(ts.x, ts.y);
            }
            else
            {
                u32 tw, th;
                weapon_inv_icon::GetWeaponIconRtTexelSize(wpn, eWpnInvIcon_Technician, tw, th);
                texture_rect.lt.set(0.f, 0.f);
                texture_rect.rb.set(float(tw), float(th));
            }
            m_item->GetUIStaticItem().SetTextureRect(texture_rect);
            used_rt = true;
        }
    }
    if (!used_rt)
    {
        pcstr iconUpgradePath = m_inv_item->GetUpgrIconPath();
        m_item->SetShader(InventoryUtilities::GetEquipmentIconShader(iconUpgradePath));

        // Same as dynamic RT path: rect must match the real texture size (baked DDS can be 640×416 etc.).
        // A fixed 350×200 rect breaks stretch and lets the quad draw at wrong scale vs item_static.
        Frect texture_rect;
        Fvector2 ts{};
        if (m_item->GetUIStaticItem().GetShader()->GetBaseTextureResolution(ts) && ts.x > 0.f && ts.y > 0.f)
        {
            texture_rect.lt.set(0.f, 0.f);
            texture_rect.rb.set(ts.x, ts.y);
        }
        else if (wpn && weapon_inv_icon::IsEnabledForItem(wpn))
        {
            u32 tw, th;
            weapon_inv_icon::GetWeaponIconUiTexelSize(wpn, eWpnInvIcon_Technician, tw, th);
            texture_rect.lt.set(0.f, 0.f);
            texture_rect.rb.set(float(tw), float(th));
        }
        else
        {
            texture_rect.lt.set(0.f, 0.f);
            texture_rect.rb.set(float(item_upgrade_grid_rect.x2 - item_upgrade_grid_rect.x1),
                float(item_upgrade_grid_rect.y2 - item_upgrade_grid_rect.y1));
        }
        m_item->GetUIStaticItem().SetTextureRect(texture_rect);
    }
    m_item->TextureOn();
    m_item->SetStretchTexture(true);
    Fvector2 v_r = Fvector2().set(item_upgrade_grid_rect.x2, item_upgrade_grid_rect.y2);
    if (UI().is_widescreen())
        v_r.x *= 0.8f;

    m_item->GetUIStaticItem().SetSize(v_r);
    m_item->SetWidth(v_r.x);
    m_item->SetHeight(v_r.y);

    m_upgr_wpn_last_rt_bound = used_rt;
    if (wpn)
        m_upgr_wpn_seen_rev = wpn->DynamicInvIconRevision();
    else
        m_upgr_wpn_seen_rev = 0;
}

void CUIInventoryUpgradeWnd::InitInventory(CUICellItem* cellItem, bool can_upgrade)
{
    if (m_item_info)
        m_item_info->InitItem(cellItem);

    m_inv_item = static_cast<PIItem>(cellItem ? cellItem->m_pData : nullptr);
    // Загружаем картинку
    if (m_item && m_inv_item)
    {
        RefreshWeaponItemPortrait();
        m_item->Show(true);
    }
    else if (m_item)
    {
        m_upgr_wpn_last_rt_bound = false;
        m_upgr_wpn_seen_rev = 0;
        m_item->Show(false);
    }

    m_scheme_wnd->DetachAll();
    m_scheme_wnd->Show(false);
    if (m_back)
    {
        m_back->DetachAll();
        m_back->Show(false);
    }
    m_btn_repair->Enable(false);

    if (ai().get_alife() && m_inv_item)
    {
        if (install_item(*m_inv_item, can_upgrade))
        {
            UpdateAllUpgrades();
        }
    }
}

// ------------------------------------------------------------------------------------------

void CUIInventoryUpgradeWnd::Show(bool status)
{
    inherited::Show(status);
    UpdateAllUpgrades();
    if (status && m_item && m_inv_item)
        RefreshWeaponItemPortrait();
}

void CUIInventoryUpgradeWnd::Update()
{
    inherited::Update();

    // Inventory lists can rebuild cells during Update(); m_inv_item must stay in sync with the actor menu selection
    // or smart_cast<CWeapon*>(m_inv_item) may dereference a freed PIItem (Sentry crash).
    CUIActorMenu* actor_menu = nullptr;
    for (CUIWindow* w = this; w; w = w->GetParent())
    {
        actor_menu = smart_cast<CUIActorMenu*>(w);
        if (actor_menu)
            break;
    }
    if (actor_menu)
        m_inv_item = actor_menu->get_upgrade_item();

    if (!m_item || !m_inv_item || !m_item->IsShown())
        return;

    CWeapon* wpn = smart_cast<CWeapon*>(m_inv_item);
    if (!wpn || !weapon_inv_icon::IsEnabledForItem(wpn))
        return;

    const u32 rev = wpn->DynamicInvIconRevision();
    const bool now_rt = wpn->DynamicInvIconPresetReady(eWpnInvIcon_Technician);

    if (rev != m_upgr_wpn_seen_rev || now_rt != m_upgr_wpn_last_rt_bound)
        RefreshWeaponItemPortrait();
}
void CUIInventoryUpgradeWnd::Reset()
{
    for (Scheme* scheme : m_schemes)
    {
        for (auto& cell : scheme->cells)
        {
            cell->Reset();
            if (cell->m_point)
                cell->m_point->Reset();
        }
    }

    inherited::Reset();
    inherited::ResetAll();
}

void CUIInventoryUpgradeWnd::UpdateAllUpgrades()
{
    if (!m_current_scheme || !m_inv_item)
    {
        return;
    }

    for (auto& cell : m_current_scheme->cells)
        cell->update_item(m_inv_item);
}

void CUIInventoryUpgradeWnd::SetCurScheme(const shared_str& id)
{
    for (Scheme* scheme : m_schemes)
    {
        if (scheme->name._get() == id._get())
        {
            m_current_scheme = scheme;
            return;
        }
    }
    VERIFY2(0, make_string("Scheme <%s> does not loaded !", id.c_str()));
}

bool CUIInventoryUpgradeWnd::install_item(CInventoryItem& inv_item, bool can_upgrade)
{
    m_scheme_wnd->DetachAll();
    if (m_back)
        m_back->DetachAll();
    m_btn_repair->Enable((inv_item.GetCondition() < 0.99f));

    if (!can_upgrade)
    {
#ifdef DEBUG
        Msg("Inventory item <%s> cannot upgrade - Mechanic say.", inv_item.m_section_id.c_str());
#endif // DEBUG
        m_current_scheme = nullptr;
        return false;
    }

    LPCSTR scheme_name = get_manager().get_item_scheme(inv_item);
    if (!scheme_name)
    {
#ifdef DEBUG
        Msg("Inventory item <%s> does not contain upgrade scheme.", inv_item.m_section_id.c_str());
#endif // DEBUG
        m_current_scheme = nullptr;
        return false;
    }

    SetCurScheme(scheme_name);

    for (UIUpgrade* ui_item : m_current_scheme->cells)
    {
        m_scheme_wnd->AttachChild(ui_item);
        if (m_back && ui_item->m_point)
            m_back->AttachChild(ui_item->m_point);

        LPCSTR upgrade_name = get_manager().get_upgrade_by_index(inv_item, ui_item->get_scheme_index());
        ui_item->init_upgrade(upgrade_name, inv_item);

        Upgrade_type* upgrade_p = get_manager().get_upgrade(upgrade_name);
        VERIFY(upgrade_p);
        for (u8 i = 0; i < inventory::upgrade::max_properties_count; i++)
        {
            shared_str prop_name = upgrade_p->get_property_name(i);
            if (prop_name.size())
            {
                [[maybe_unused]] auto prop_p = get_manager().get_property(prop_name);
                VERIFY(prop_p);
            }
        }

        ui_item->set_texture(UIUpgrade::LAYER_ITEM, upgrade_p->icon_name());
        ui_item->set_texture(UIUpgrade::LAYER_POINT, m_point_textures[UIUpgrade::STATE_ENABLED].c_str()); // default
        ui_item->set_texture(UIUpgrade::LAYER_COLOR, m_cell_textures[UIUpgrade::STATE_ENABLED].c_str()); // default
        ui_item->set_texture(UIUpgrade::LAYER_BORDER, m_border_texture.c_str());
        ui_item->set_texture(UIUpgrade::LAYER_INK, m_ink_texture.c_str());
    }

    m_scheme_wnd->Show(true);
    if (m_item)
        m_item->Show(true);
    if (m_back)
        m_back->Show(true);

    UpdateAllUpgrades();
    return true;
}

UIUpgrade* CUIInventoryUpgradeWnd::FindUIUpgrade(Upgrade_type const* upgr)
{
    if (!m_current_scheme)
        return nullptr;

    for (UIUpgrade* cell : m_current_scheme->cells)
    {
        Upgrade_type* i_upgr = cell->get_upgrade();
        if (upgr == i_upgr)
        {
            return cell;
        }
    }
    return nullptr;
}

bool CUIInventoryUpgradeWnd::DBClickOnUIUpgrade(Upgrade_type const* upgr)
{
    UpdateAllUpgrades();
    UIUpgrade* uiupgr = FindUIUpgrade(upgr);
    if (uiupgr)
    {
        uiupgr->OnClick();
        return true;
    }
    return false;
}

void CUIInventoryUpgradeWnd::AskUsing(LPCSTR text, LPCSTR upgrade_name)
{
    VERIFY(m_inv_item);
    VERIFY(upgrade_name);
    VERIFY(m_pParentWnd);

    UpdateAllUpgrades();

    m_cur_upgrade_id = upgrade_name;

    CUIActorMenu* parent_wnd = smart_cast<CUIActorMenu*>(m_pParentWnd);
    if (parent_wnd)
    {
        parent_wnd->CallMessageBoxYesNo(text);
    }
}

void CUIInventoryUpgradeWnd::OnMesBoxYes()
{
    if (get_manager().upgrade_install(*m_inv_item, m_cur_upgrade_id, false))
    {
        VERIFY(m_pParentWnd);
        CUIActorMenu* parent_wnd = smart_cast<CUIActorMenu*>(m_pParentWnd);
        if (parent_wnd)
        {
            //Alundaio: tell script that item has been upgraded
            luabind::functor<void> funct;
            GEnv.ScriptEngine->functor("inventory_upgrades.effect_upgrade_item", funct);
            if (funct)
            {
                CGameObject* GO = m_inv_item->cast_game_object();
                funct(GO->lua_game_object(), m_cur_upgrade_id);
            }
            //-Alundaio
            parent_wnd->UpdateActor();
            parent_wnd->SeparateUpgradeItem();
        }
    }
    UpdateAllUpgrades();
}

void CUIInventoryUpgradeWnd::HighlightHierarchy(shared_str const& upgrade_id)
{
    UpdateAllUpgrades();
    get_manager().highlight_hierarchy(*m_inv_item, upgrade_id);
}

void CUIInventoryUpgradeWnd::ResetHighlight()
{
    UpdateAllUpgrades();
    get_manager().reset_highlight(*m_inv_item);
}

void CUIInventoryUpgradeWnd::set_info_cur_upgrade(Upgrade_type* upgrade)
{
    UIUpgrade* uiu = FindUIUpgrade(upgrade);
    if (uiu)
    {
        if (Device.dwTimeGlobal < uiu->FocusReceiveTime() + (m_item_info ? m_item_info->delay : 0))
        {
            upgrade = nullptr; // visible = false
        }
    }
    else
    {
        upgrade = nullptr;
    }

    CUIActorMenu* parent_wnd = smart_cast<CUIActorMenu*>(m_pParentWnd);
    if (parent_wnd)
    {
        if (parent_wnd->SetInfoCurUpgrade(upgrade, m_inv_item))
        {
            UpdateAllUpgrades();
        }
    }
}

CUIInventoryUpgradeWnd::Manager_type& CUIInventoryUpgradeWnd::get_manager()
{
    return ai().alife().inventory_upgrade_manager();
}
