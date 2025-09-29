#include "StdAfx.h"
#include "UICellCustomItems.h"
#include "UIInventoryUtilities.h"
#include "Weapon.h"
#include "UIDragDropListEx.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"

#define INV_GRID_WIDTHF 64.0f
#define INV_GRID_HEIGHTF 64.0f

namespace detail
{
static constexpr pcstr ICON_LAYER_FIELD = "icon_layer";

struct is_helper_pred
{
    bool operator()(CUICellItem* child) { return child->IsHelper(); }
}; // struct is_helper_pred
} // namespace detail

CUIInventoryCellItem::CUIInventoryCellItem(CInventoryItem* itm)
{
    m_pData = (void*)itm;

    pcstr iconPath = itm->GetInvIconPath();
    inherited::SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(itm->m_section_id).c_str()));

    m_grid_size.set(itm->GetInvGridRect().rb);
    Frect rect;
    rect.lt.set(INV_GRID_WIDTHF * itm->GetInvGridRect().x1, INV_GRID_HEIGHTF * itm->GetInvGridRect().y1);

    rect.rb.set(rect.lt.x + INV_GRID_WIDTHF * m_grid_size.x, rect.lt.y + INV_GRID_HEIGHTF * m_grid_size.y);

    inherited::SetStretchTexture(true);

    //Alundaio; Layered icon
    for (u8 i = 0; i < 255; ++i)
    {
        string32 layer_str;
        xr_sprintf(layer_str, "%u%s", i, detail::ICON_LAYER_FIELD);
        if (!pSettings->line_exist(itm->m_section_id, layer_str))
            break;

        cpcstr section = pSettings->r_string(itm->m_section_id, layer_str);
        if (!section)
            continue;

        string32 temp;
        const Fvector2 offset
        {
            pSettings->r_float(itm->m_section_id, strconcat(temp, layer_str, "_x")),
            pSettings->r_float(itm->m_section_id, strconcat(temp, layer_str, "_y"))
        };

        cpcstr field_scale = strconcat(temp, layer_str, "_scale");
        const float scale = pSettings->read_if_exists<float>(itm->m_section_id, field_scale, 1.0f);

        //cpcstr field_color = strconcat(temp, layer_str, "_color");
        //const u32 color = READ_IF_EXISTS(pSettings, r_color, itm->m_section_id, field_color, 0);

        CreateLayer(section, offset, scale);
    }
    //-Alundaio
}

CUIInventoryCellItem::CUIInventoryCellItem(shared_str section_id)
{
    data_is_string = true;
    m_section_attachs_id = section_id;

    if (strstr(*section_id, "|"))
    {
        int len = (int)strcspn(*section_id, "|");
        char* result = new char[len + 1];
        strncpy(result, *section_id, len);
        result[len] = '\0';
        m_section_id = result;
    }
    else
        m_section_id = section_id;

    R_ASSERT2(pSettings->line_exist(m_section_id.c_str(), "inv_icon"), make_string("Item '%s' doesn't has property 'inv_icon'", m_section_id.c_str()));

    pcstr iconPath = pSettings->r_string(m_section_id.c_str(), "inv_icon");
    inherited::SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(m_section_id).c_str()));

    if (pSettings->line_exist(m_section_id.c_str(), "box_size"))
    {
        m_iAmmoCount = pSettings->r_u32(m_section_id.c_str(), "box_size");
        b_isAmmo = true;
    }
    if (pSettings->line_exist(m_section_id.c_str(), "ammo_class"))
        b_isWeapon = true;
    if (pSettings->line_exist(m_section_id.c_str(), "use_condition"))
        b_isUseCondition = true;
    if (pSettings->line_exist(m_section_id.c_str(), "class") && xr_strcmp(pSettings->r_string(m_section_id.c_str(), "class"), "E_HLMET") == 0)
        b_isHelmet = true;
    if (strstr(m_section_id.c_str(), "cev_plastin_"))
        b_isPlastin = true;
    if (!b_isOutfit && pSettings->line_exist(m_section_id.c_str(), "class") && xr_strcmp(pSettings->r_string(m_section_id.c_str(), "class"), "EQU_STLK") == 0)
    {
        b_isOutfit = true;
        s_outfitFaction = pSettings->r_string(m_section_id.c_str(), "faction");
    }

    u32 sl = pSettings->read_if_exists<u32>(m_section_id.c_str(), "slot", NO_ACTIVE_SLOT - 1);
    base_slot_id = sl + 1;

    u32 x, y, w, h;

    x = 0;
    y = 0;
    w = pSettings->r_u32(m_section_id.c_str(), "inv_grid_width");
    h = pSettings->r_u32(m_section_id.c_str(), "inv_grid_height");

    Irect rect1 = Irect().set(x, y, w, h);

    m_grid_size.set(rect1.rb);
    Frect rect;
    rect.lt.set(INV_GRID_WIDTHF * rect1.x1, INV_GRID_HEIGHTF * rect1.y1);

    rect.rb.set(rect.lt.x + INV_GRID_WIDTHF * m_grid_size.x, rect.lt.y + INV_GRID_HEIGHTF * m_grid_size.y);

    inherited::SetStretchTexture(true);

    //Alundaio; Layered icon
    for (u8 i = 0; i < 255; ++i)
    {
        string32 layer_str;
        xr_sprintf(layer_str, "%u%s", i, detail::ICON_LAYER_FIELD);
        if (!pSettings->line_exist(m_section_id.c_str(), layer_str))
            break;

        cpcstr section = pSettings->r_string(m_section_id.c_str(), layer_str);
        if (!section)
            continue;

        string32 temp;
        const Fvector2 offset
        {
            pSettings->r_float(m_section_id.c_str(), strconcat(temp, layer_str, "_x")),
            pSettings->r_float(m_section_id.c_str(), strconcat(temp, layer_str, "_y"))
        };

        cpcstr field_scale = strconcat(temp, layer_str, "_scale");
        const float scale = pSettings->read_if_exists<float>(m_section_id.c_str(), field_scale, 1.0f);

        //cpcstr field_color = strconcat(temp, layer_str, "_color");
        //const u32 color = READ_IF_EXISTS(pSettings, r_color, itm->m_section_id, field_color, 0);

        CreateLayer(section, offset, scale);
    }
    //-Alundaio
}

void CUIInventoryCellItem::OnAfterChild(CUIDragDropListEx* parent_list)
{
    for (SIconLayer* layer : m_layers)
    {
        layer->m_icon = InitLayer(layer->m_icon, layer->m_name, layer->offset, parent_list->GetVerticalPlacement(), layer->m_scale);
    }
}
void CUIInventoryCellItem::UpdateIcon()
{
    CInventoryItem* itm = (CInventoryItem*)m_pData;

    if (!itm)
        return;

    pcstr iconPath = itm->GetInvIconPath();
    inherited::SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(itm->m_section_id).c_str()));

    m_grid_size.set(itm->GetInvGridRect().rb);
    Frect rect;
    rect.lt.set(INV_GRID_WIDTHF * itm->GetInvGridRect().x1, INV_GRID_HEIGHTF * itm->GetInvGridRect().y1);
    rect.rb.set(rect.lt.x + INV_GRID_WIDTHF * m_grid_size.x, rect.lt.y + INV_GRID_HEIGHTF * m_grid_size.y);

    inherited::SetStretchTexture(true);
}

shared_str CUIInventoryCellItem::GetIconPath(shared_str section_id)
{
    R_ASSERT2(pSettings->line_exist(*section_id, "inv_icon"), make_string("Item '%s' doesn't has property 'inv_icon'", section_id.c_str()));

    pcstr itemClass = pSettings->read_if_exists<pcstr>(*section_id, "item_class", "NULL");
    if (xr_strcmp(itemClass, "outfit_patch") == 0)
    {
        std::string factionName{section_id.c_str()};
        factionName.erase(factionName.length() - 6);

        pcstr icon = pSettingsFE->read_if_exists<pcstr>(factionName.c_str(), "icon", make_string("icons\\patches\\%s", factionName.c_str()).c_str());
        std::string iconPath{icon};
        if (strstr(iconPath.c_str(), "ui\\"))
            iconPath.erase(0, 3);
        return iconPath.c_str();
    }

    return pSettings->r_string(*section_id, "inv_icon");
}

bool CUIInventoryCellItem::EqualTo(CUICellItem* itm)
{
    if (data_is_string && itm->data_is_string)
        return xr_strcmp(itm->m_section_attachs_id.c_str(), m_section_attachs_id.c_str()) == 0;

    CUIInventoryCellItem* ci = smart_cast<CUIInventoryCellItem*>(itm);
    if (!itm)
    {
        return false;
    }
    if (object()->object().cNameSect() != ci->object()->object().cNameSect())
    {
        return false;
    }
    if (!fsimilar(object()->GetCondition(), ci->object()->GetCondition(), 0.01f))
    {
        return false;
    }
    if (!object()->equal_upgrades(ci->object()->upgardes()))
    {
        return false;
    }
    return true;
}

bool CUIInventoryCellItem::IsHelperOrHasHelperChild()
{
    return std::count_if(m_childs.begin(), m_childs.end(), ::detail::is_helper_pred()) > 0 || IsHelper();
}

CUIDragItem* CUIInventoryCellItem::CreateDragItem()
{
    if (IsHelperOrHasHelperChild())
        return nullptr;

    CUIDragItem* i = inherited::CreateDragItem();

    for (const SIconLayer* layer : m_layers)
    {
        CUIStatic* s = xr_new<CUIStatic>("Layer");
        s->SetAutoDelete(true);
        s->SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(layer->m_name).c_str()));
        InitLayer(s, layer->m_name, layer->offset, false, layer->m_scale);
        s->SetTextureColor(i->wnd()->GetTextureColor());
        i->wnd()->AttachChild(s);
    }

    return i;
}

void CUIInventoryCellItem::SetTextureColor(u32 color)
{
    inherited::SetTextureColor(color);
    for (const SIconLayer* layer : m_layers)
    {
        if (layer->m_icon)
            layer->m_icon->SetTextureColor(color);
    }
}

bool CUIInventoryCellItem::IsHelper()
{
    if (data_is_string)
        return false;

    return object()->is_helper_item();
}
void CUIInventoryCellItem::SetIsHelper(bool is_helper)
{
    if (data_is_string)
        return;

    object()->set_is_helper(is_helper); 
}

//Alundaio
void CUIInventoryCellItem::RemoveLayer(const SIconLayer* layer)
{
    for (auto it = m_layers.begin(); m_layers.end() != it; ++it)
    {
        if ((*it) == layer)
        {
            DetachChild((*it)->m_icon);
            m_layers.erase(it);
            break;
        }
    }
}

void CUIInventoryCellItem::CreateLayer(pcstr section, Fvector2 offset, float scale)
{
    SIconLayer* layer = xr_new<SIconLayer>();
    layer->m_name = section;
    layer->offset = offset;
    //layer->m_color = color;
    layer->m_scale = scale;
    m_layers.push_back(layer);
}

CUIStatic* CUIInventoryCellItem::InitLayer(CUIStatic* s, pcstr section,
    Fvector2 addon_offset, bool b_rotate, float scale)
{
    if (!s)
    {
        s = xr_new<CUIStatic>("Layer");
        s->SetAutoDelete(true);
        AttachChild(s);
        s->SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(section).c_str()));
        s->SetTextureColor(GetTextureColor());
    }

    Fvector2 base_scale;

    if (Heading())
    {
        base_scale.x = (GetHeight() / (INV_GRID_WIDTHF * m_grid_size.x)) * scale;
        base_scale.y = (GetWidth() / (INV_GRID_HEIGHTF * m_grid_size.y)) * scale;
    }
    else
    {
        base_scale.x = (GetWidth() / (INV_GRID_WIDTHF * m_grid_size.x)) * scale;
        base_scale.y = (GetHeight() / (INV_GRID_HEIGHTF * m_grid_size.y)) * scale;
    }
    Fvector2 cell_size
    {
        pSettings->r_float(section, "inv_grid_width") * INV_GRID_WIDTHF,
        pSettings->r_float(section, "inv_grid_height") * INV_GRID_HEIGHTF
    };

    cell_size.mul(base_scale);

    if (b_rotate)
    {
        s->SetWndSize(Fvector2{ cell_size.y, cell_size.x });

        const Fvector2 new_offset
        {
            addon_offset.y * base_scale.x,
            GetHeight() - addon_offset.x * base_scale.x - cell_size.x
        };
        addon_offset = new_offset;
        addon_offset.x *= UICore::get_current_kx();
    }
    else
    {
        s->SetWndSize(cell_size);
        addon_offset.mul(base_scale);
    }

    s->SetWndPos(addon_offset);
    s->SetStretchTexture(true);

    s->EnableHeading(b_rotate);

    if (b_rotate)
    {
        s->SetHeading(GetHeading());
        const Fvector2 offs{ 0.0f, s->GetWndSize().y };
        s->SetHeadingPivot(Fvector2{ 0.0f, 0.0f }, /*Fvector2{ 0.0f, 0.0f }*/offs, true);
    }

    return s;
}
//-Alundaio

void CUIInventoryCellItem::Update()
{
    inherited::Update();
    UpdateConditionProgressBar(); //Alundaio
    UpdateItemText();

    u32 color = GetTextureColor();
    if (IsHelper() && !ChildsCount())
    {
        color = 0xbbbbbbbb;
    }
    else if (IsHelperOrHasHelperChild())
    {
        color = 0xffffffff;
    }

    if (data_is_string)
        color = 0xffffffff;

    SetTextureColor(color);

    for (SIconLayer* layer : m_layers)
    {
        layer->m_icon = InitLayer(layer->m_icon, layer->m_name, layer->offset, Heading(), layer->m_scale);
        layer->m_icon->SetTextureColor(color);
    }
}

void CUIInventoryCellItem::UpdateItemText()
{
    const u32 helper_count =
        (u32)std::count_if(m_childs.begin(), m_childs.end(), ::detail::is_helper_pred()) + IsHelper() ? 1 : 0;

    const u32 count = data_is_string ? ChildsCount() + 1 : ChildsCount() + 1 - helper_count;

    string32 tempStr;
    pcstr finalText = nullptr;

    if (data_is_string)
    {
        if (b_isAmmo)
        {
            u32 count = GetAmmoCnt();
            if (ChildsCount() > 0)
                for (std::size_t i = 0; i < ChildsCount(); ++i)
                    count += Child(i)->GetAmmoCnt();

            xr_sprintf(tempStr, "%d", count);
            finalText = tempStr;

            if (m_text)
            {
                m_text->Show(nullptr != finalText);
                m_text->SetText(finalText);
            }

            return;
        }
        else if (count <= 1)
            m_text->Show(false);
    }

    if (count > 1 || helper_count)
    {
        xr_sprintf(tempStr, "x%d", count);
        finalText = tempStr;
    }

    if (m_text)
    {
        m_text->Show(nullptr != finalText);
        m_text->SetText(finalText);
    }
    else
    {
        this->SetText(finalText);
    }
}

CUIAmmoCellItem::CUIAmmoCellItem(CWeaponAmmo* itm) : inherited(itm) {}
bool CUIAmmoCellItem::EqualTo(CUICellItem* itm)
{
    if (!inherited::EqualTo(itm))
        return false;

    if (data_is_string && itm->data_is_string)
        return xr_strcmp(m_section_attachs_id.c_str(), itm->m_section_attachs_id.c_str()) == 0;

    CUIAmmoCellItem* ci = smart_cast<CUIAmmoCellItem*>(itm);
    if (!ci)
        return false;

    return ((object()->cNameSect() == ci->object()->cNameSect()));
}

CUIDragItem* CUIAmmoCellItem::CreateDragItem() { return IsHelper() ? NULL : inherited::CreateDragItem(); }
u32 CUIAmmoCellItem::CalculateAmmoCount()
{
    if (data_is_string)
        return 0;

    xr_vector<CUICellItem*>::iterator it = m_childs.begin();
    xr_vector<CUICellItem*>::iterator it_e = m_childs.end();

    u32 total = IsHelper() ? 0 : object()->m_boxCurr;
    for (; it != it_e; ++it)
    {
        CUICellItem* child = *it;

        if (!child->IsHelper())
        {
            total += ((CUIAmmoCellItem*)(*it))->object()->m_boxCurr;
        }
    }

    return total;
}

void CUIAmmoCellItem::UpdateItemText()
{
    string32 tempStr;
    pcstr finalText = nullptr;
    if (!m_custom_draw)
    {
        xr_sprintf(tempStr, "%d", CalculateAmmoCount());
        finalText = tempStr;
    }

    if (m_text)
    {
        m_text->Show(nullptr != finalText);
        m_text->SetText(finalText);
    }
    else
    {
        this->SetText(finalText);
    }
}

CUIWeaponCellItem::CUIWeaponCellItem(CWeapon* itm) : inherited(itm)
{
    m_addons[eSilencer] = NULL;
    m_addons[eScope] = NULL;
    m_addons[eLauncher] = NULL;

    if (itm->SilencerAttachable())
        m_addon_offset[eSilencer].set(object()->GetSilencerX(), object()->GetSilencerY());

    if (itm->ScopeAttachable() || itm->mainScopeSlotIsBusy())
        m_addon_offset[eScope].set(object()->GetScopeX(), object()->GetScopeY());

    if (itm->GrenadeLauncherAttachable())
        m_addon_offset[eLauncher].set(object()->GetGrenadeLauncherX(), object()->GetGrenadeLauncherY());
}

#include "Common/object_broker.h"
CUIWeaponCellItem::~CUIWeaponCellItem() {}
bool CUIWeaponCellItem::is_scope() { return object()->ScopeAttachable() && object()->IsScopeAttached(); }
bool CUIWeaponCellItem::is_silencer() { return object()->SilencerAttachable() && object()->IsSilencerAttached(); }
bool CUIWeaponCellItem::is_launcher()
{
    return object()->GrenadeLauncherAttachable() && object()->IsGrenadeLauncherAttached();
}

void CUIWeaponCellItem::CreateIcon(eAddonType t)
{
    if (m_addons[t])
        return;
    m_addons[t] = xr_new<CUIStatic>("Addon icon");
    m_addons[t]->SetAutoDelete(true);
    AttachChild(m_addons[t]);
    CInventoryItem* itm = (CInventoryItem*)m_pData;
    R_ASSERT2(pSettings->line_exist(itm->m_section_id.c_str(), "inv_icon"), make_string("Item '%s' doesn't has property 'inv_icon'", itm->m_section_id.c_str()));
    if (pSettings->line_exist(itm->m_section_id.c_str(), "inv_icon_alt"))
        m_addons[t]->SetShader(InventoryUtilities::GetEquipmentIconShader(pSettings->r_string(itm->m_section_id.c_str(), "inv_icon_alt")));
    else
        m_addons[t]->SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(itm->m_section_id).c_str()));

    u32 color = GetTextureColor();
    m_addons[t]->SetTextureColor(color);
}

void CUIWeaponCellItem::DestroyIcon(eAddonType t)
{
    DetachChild(m_addons[t]);
    m_addons[t] = NULL;
}

CUIStatic* CUIWeaponCellItem::GetIcon(eAddonType t) { return m_addons[t]; }
void CUIWeaponCellItem::RefreshOffset()
{
    if (data_is_string)
        return;

    if (object()->SilencerAttachable())
        m_addon_offset[eSilencer].set(object()->GetSilencerX(), object()->GetSilencerY());

    if (object()->ScopeAttachable())
        m_addon_offset[eScope].set(object()->GetScopeX(), object()->GetScopeY());

    if (object()->GrenadeLauncherAttachable())
        m_addon_offset[eLauncher].set(object()->GetGrenadeLauncherX(), object()->GetGrenadeLauncherY());
}

void CUIWeaponCellItem::Draw()
{
    inherited::Draw();

    if (m_upgrade && m_upgrade->IsShown())
        m_upgrade->Draw();
};

void CUIWeaponCellItem::Update()
{
    if (data_is_string)
        return;

    bool b = Heading();
    inherited::Update();

    bool bForceReInitAddons = (b != Heading());

    if (object()->SilencerAttachable())
    {
        if (object()->IsSilencerAttached())
        {
            if (!GetIcon(eSilencer) || bForceReInitAddons || object()->b_forceIconUpdate)
            {
                CreateIcon(eSilencer);
                RefreshOffset();
                InitAddon(GetIcon(eSilencer), *object()->GetSilencerName(), m_addon_offset[eSilencer], Heading());
            }
        }
        else
        {
            if (m_addons[eSilencer])
                DestroyIcon(eSilencer);
        }
    }

    if (object()->ScopeAttachable())
    {
        if (object()->IsScopeAttached())
        {
            if (!GetIcon(eScope) || bForceReInitAddons || object()->b_forceIconUpdate)
            {
                CreateIcon(eScope);
                RefreshOffset();
                InitAddon(GetIcon(eScope), *object()->GetScopeName(), m_addon_offset[eScope], Heading());
                UpdateIcon();
            }
        }
        else
        {
            if (m_addons[eScope])
            {
                DestroyIcon(eScope);
                UpdateIcon();
            }
        }
    }

    if (object()->GrenadeLauncherAttachable())
    {
        if (object()->IsGrenadeLauncherAttached())
        {
            if (!GetIcon(eLauncher) || bForceReInitAddons || object()->b_forceIconUpdate)
            {
                CreateIcon(eLauncher);
                RefreshOffset();
                InitAddon(
                    GetIcon(eLauncher), *object()->GetGrenadeLauncherName(), m_addon_offset[eLauncher], Heading());
            }
        }
        else
        {
            if (m_addons[eLauncher])
                DestroyIcon(eLauncher);
        }
    }
}

void CUIWeaponCellItem::SetTextureColor(u32 color)
{
    inherited::SetTextureColor(color);
    if (m_addons[eSilencer])
    {
        m_addons[eSilencer]->SetTextureColor(color);
    }
    if (m_addons[eScope])
    {
        m_addons[eScope]->SetTextureColor(color);
    }
    if (m_addons[eLauncher])
    {
        m_addons[eLauncher]->SetTextureColor(color);
    }
}

void CUIWeaponCellItem::OnAfterChild(CUIDragDropListEx* parent_list)
{
    if (data_is_string)
        return;

    if (is_silencer() && GetIcon(eSilencer))
        InitAddon(GetIcon(eSilencer), *object()->GetSilencerName(), m_addon_offset[eSilencer], parent_list->GetVerticalPlacement());

    if (is_scope() && GetIcon(eScope))
        InitAddon(GetIcon(eScope), *object()->GetScopeName(), m_addon_offset[eScope], parent_list->GetVerticalPlacement());

    if (is_launcher() && GetIcon(eLauncher))
        InitAddon(GetIcon(eLauncher), *object()->GetGrenadeLauncherName(), m_addon_offset[eLauncher],
            parent_list->GetVerticalPlacement());
}

void CUIWeaponCellItem::InitAddon(CUIStatic* s, LPCSTR section, Fvector2 addon_offset, bool b_rotate)
{
    Frect tex_rect;
    Fvector2 base_scale;

    R_ASSERT2(pSettings->line_exist(section, "inv_icon"), make_string("Item '%s' doesn't has property 'inv_icon'", section));
    if (pSettings->line_exist(section, "inv_icon_alt"))
        s->SetShader(InventoryUtilities::GetEquipmentIconShader(pSettings->r_string(section, "inv_icon_alt")));
    else
        s->SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(section).c_str()));

    if (Heading())
    {
        base_scale.x = GetHeight() / (INV_GRID_WIDTHF * m_grid_size.x);
        base_scale.y = GetWidth() / (INV_GRID_HEIGHTF * m_grid_size.y);
    }
    else
    {
        base_scale.x = GetWidth() / (INV_GRID_WIDTHF * m_grid_size.x);
        base_scale.y = GetHeight() / (INV_GRID_HEIGHTF * m_grid_size.y);
    }
    Fvector2 cell_size;

    if (pSettings->line_exist(section, "inv_grid_alt_width") && pSettings->line_exist(section, "inv_grid_alt_height"))
    {
        cell_size.x = pSettings->r_u32(section, "inv_grid_alt_width") * INV_GRID_WIDTHF;
        cell_size.y = pSettings->r_u32(section, "inv_grid_alt_height") * INV_GRID_HEIGHTF;
    }
    else
    {
        cell_size.x = pSettings->r_u32(section, "inv_grid_width") * INV_GRID_WIDTHF;
        cell_size.y = pSettings->r_u32(section, "inv_grid_height") * INV_GRID_HEIGHTF;
    }

    tex_rect.x1 = 0;
    tex_rect.y1 = 0;

    tex_rect.rb.add(tex_rect.lt, cell_size);

    cell_size.mul(base_scale);

    if (b_rotate)
    {
        s->SetWndSize(Fvector2().set(cell_size.y, cell_size.x));
        Fvector2 new_offset;
        new_offset.x = addon_offset.y * base_scale.x;
        new_offset.y = GetHeight() - addon_offset.x * base_scale.x - cell_size.x;
        addon_offset = new_offset;
        addon_offset.x *= UI().get_current_kx();
    }
    else
    {
        s->SetWndSize(cell_size);
        addon_offset.mul(base_scale);
    }

    s->SetWndPos(addon_offset);
    s->SetTextureRect(tex_rect);
    s->SetStretchTexture(true);

    s->EnableHeading(b_rotate);

    if (b_rotate)
    {
        s->SetHeading(GetHeading());
        Fvector2 offs;
        offs.set(0.0f, s->GetWndSize().y);
        s->SetHeadingPivot(Fvector2().set(0.0f, 0.0f), /*Fvector2().set(0.0f,0.0f)*/ offs, true);
    }
}

CUIDragItem* CUIWeaponCellItem::CreateDragItem()
{
    CUIDragItem* i = inherited::CreateDragItem();
    CUIStatic* s = NULL;

    if (GetIcon(eSilencer))
    {
        s = xr_new<CUIStatic>("Silencer");
        s->SetAutoDelete(true);
        InitAddon(s, *object()->GetSilencerName(), m_addon_offset[eSilencer], false);
        s->SetTextureColor(i->wnd()->GetTextureColor());
        i->wnd()->AttachChild(s);
    }

    if (GetIcon(eScope))
    {
        s = xr_new<CUIStatic>("Scope");
        s->SetAutoDelete(true);
        InitAddon(s, *object()->GetScopeName(), m_addon_offset[eScope], false);
        s->SetTextureColor(i->wnd()->GetTextureColor());
        i->wnd()->AttachChild(s);
    }

    if (GetIcon(eLauncher))
    {
        s = xr_new<CUIStatic>("Grenade launcher");
        s->SetAutoDelete(true);
        InitAddon(s, *object()->GetGrenadeLauncherName(), m_addon_offset[eLauncher], false);
        s->SetTextureColor(i->wnd()->GetTextureColor());
        i->wnd()->AttachChild(s);
    }
    return i;
}

bool CUIWeaponCellItem::EqualTo(CUICellItem* itm)
{
    if (!inherited::EqualTo(itm))
        return false;

    CUIWeaponCellItem* ci = smart_cast<CUIWeaponCellItem*>(itm);
    if (!ci)
        return false;

    //	bool b_addons					= ( (object()->GetAddonsState() == ci->object()->GetAddonsState()) );
    if (object()->GetAddonsState() != ci->object()->GetAddonsState())
    {
        return false;
    }
    if (this->is_scope() && ci->is_scope())
    {
        if (object()->GetScopeName() != ci->object()->GetScopeName())
        {
            return false;
        }
    }
    //	bool b_place					= ( (object()->m_eItemCurrPlace == ci->object()->m_eItemCurrPlace) );

    return true;
}

CBuyItemCustomDrawCell::CBuyItemCustomDrawCell(LPCSTR str, CGameFont* pFont)
{
    m_pFont = pFont;
    VERIFY(xr_strlen(str) < 16);
    xr_strcpy(m_string, str);
}

void CBuyItemCustomDrawCell::OnDraw(CUICellItem* cell)
{
    Fvector2 pos;
    cell->GetAbsolutePos(pos);
    UI().ClientToScreenScaled(pos, pos.x, pos.y);
    m_pFont->Out(pos.x, pos.y, m_string);
    m_pFont->OnRender();
}
