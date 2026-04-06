#include "StdAfx.h"
#include "UICellCustomItems.h"
#include "UIInventoryUtilities.h"
#include "Weapon.h"
#include "weapon_inv_icon.h"
#include "UIDragDropListEx.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include <algorithm>

#define INV_GRID_WIDTHF 64.0f
#define INV_GRID_HEIGHTF 64.0f

extern BOOL debug_ui_item_cell;

namespace detail
{
static constexpr pcstr ICON_LAYER_FIELD = "icon_layer";

struct is_helper_pred
{
    bool operator()(CUICellItem* child) { return child->IsHelper(); }
}; // struct is_helper_pred
} // namespace detail

CUIInventoryCellItem::CUIInventoryCellItem(CInventoryItem* itm) : CUIInventoryCellItem(itm, true, true, true) {}
CUIInventoryCellItem::CUIInventoryCellItem(CInventoryItem* itm, bool needCondBar, bool needFIcon, bool needUgrIcon) : CUICellItem(needCondBar, needFIcon, needUgrIcon)
{
    m_pData = (void*)itm;
    m_section_id = itm->m_section_id;

    pcstr iconPath = itm->GetInvIconPath();
    inherited::SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(itm->m_section_id).c_str()));

    m_grid_size.set(itm->GetInvGridRect().rb);
    Frect rect;
    rect.lt.set(INV_GRID_WIDTHF * itm->GetInvGridRect().x1, INV_GRID_HEIGHTF * itm->GetInvGridRect().y1);

    rect.rb.set(rect.lt.x + INV_GRID_WIDTHF * m_grid_size.x, rect.lt.y + INV_GRID_HEIGHTF * m_grid_size.y);

    inherited::SetStretchTexture(true);

    // Alundaio layered icon (0icon_layer …): skip when GPU dynamic icon already bakes addons into RT.
    if (!weapon_inv_icon::IsEnabledForItem(itm))
    {
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

            CreateLayer(section, offset, scale);
        }
    }

    if (weapon_inv_icon::IsEnabledForItem(itm))
    {
        m_inv_ui_showing_rt = itm->DynamicInvIconPresetReady(m_inv_icon_preset);
        m_inv_seen_rev = itm->DynamicInvIconRevision();
        UpdateIcon();
    }
}

CUIInventoryCellItem::CUIInventoryCellItem(shared_str section_id)
{
    data_is_string = true;
    m_section_attachs_id = section_id;

    if (strstr(section_id.c_str(), "|"))
    {
        int len = (int)strcspn(section_id.c_str(), "|");
        char* result = new char[len + 1];
        strncpy(result, section_id.c_str(), len);
        result[len] = '\0';
        m_section_id = result;
    }
    else
        m_section_id = section_id;

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

    if (!weapon_inv_icon::IsEnabledForSection(m_section_id.c_str()))
    {
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

            CreateLayer(section, offset, scale);
        }
    }

    if (weapon_inv_icon::IsEnabledForSection(m_section_id.c_str()))
        UpdateIcon();
}

CUIInventoryCellItem::~CUIInventoryCellItem()
{
    xr_delete(m_dynamic_overlay);
}

void CUIInventoryCellItem::OnAfterChild(CUIDragDropListEx* parent_list)
{
    InitDynamicOverlay(parent_list->GetVerticalPlacement());
    for (SIconLayer* layer : m_layers)
    {
        layer->m_icon = InitLayer(layer->m_icon, layer->m_name, layer->offset, parent_list->GetVerticalPlacement(), layer->m_scale);
    }
}
void CUIInventoryCellItem::UpdateIcon()
{
    m_inv_layer_layout_valid = false;
    m_inv_dyn_overlay_layout_valid = false;

    if (data_is_string)
    {
        if (!weapon_inv_icon::IsEnabledForSection(m_section_id.c_str()))
        {
            m_inv_ui_showing_rt = false;
            m_hide_base_for_dynamic = false;
            RemoveDynamicOverlay();
            return;
        }

        const bool ready =
            weapon_inv_icon::SectionSharedInvIconPresetReady(m_section_id.c_str(), m_inv_icon_preset);
        m_inv_ui_showing_rt = ready;
        m_inv_seen_rev = weapon_inv_icon::InvIconRtEpoch();
        m_hide_base_for_dynamic = ready;

        Frect rect;
        inherited::SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(m_section_id).c_str()));
        const u32 gx = 0, gy = 0;
        const u32 gw = pSettings->r_u32(m_section_id.c_str(), "inv_grid_width");
        const u32 gh = pSettings->r_u32(m_section_id.c_str(), "inv_grid_height");
        m_grid_size.set(float(gw), float(gh));
        rect.lt.set(INV_GRID_WIDTHF * gx, INV_GRID_HEIGHTF * gy);
        rect.rb.set(rect.lt.x + INV_GRID_WIDTHF * m_grid_size.x, rect.lt.y + INV_GRID_HEIGHTF * m_grid_size.y);
        inherited::GetUIStaticItem().SetTextureRect(rect);
        inherited::SetStretchTexture(true);
        UpdateDynamicOverlay(ready);
        return;
    }

    CInventoryItem* itm = (CInventoryItem*)m_pData;
    if (!itm)
        return;

    const bool dyn = weapon_inv_icon::IsEnabledForItem(itm);
    const bool ready = dyn && itm->DynamicInvIconPresetReady(m_inv_icon_preset);
    m_inv_ui_showing_rt = ready;
    m_inv_seen_rev = dyn ? itm->DynamicInvIconRevision() : 0;
    m_hide_base_for_dynamic = ready;

    Frect rect;
    inherited::SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(itm->m_section_id).c_str()));
    m_grid_size.set(itm->GetInvGridRect().rb);
    rect.lt.set(INV_GRID_WIDTHF * itm->GetInvGridRect().x1, INV_GRID_HEIGHTF * itm->GetInvGridRect().y1);
    rect.rb.set(rect.lt.x + INV_GRID_WIDTHF * m_grid_size.x, rect.lt.y + INV_GRID_HEIGHTF * m_grid_size.y);
    inherited::GetUIStaticItem().SetTextureRect(rect);

    inherited::SetStretchTexture(true);
    UpdateDynamicOverlay(ready);
}

void CUIInventoryCellItem::DrawTexture()
{
    inherited::DrawTexture();

    if (!m_dynamic_overlay || !m_inv_ui_showing_rt)
        return;

    m_dynamic_overlay->DrawTexture();
}

shared_str CUIInventoryCellItem::GetIconPath(shared_str section_id)
{
    if (!pSettings->line_exist(section_id.c_str(), "inv_icon"))
        return "";

    pcstr itemClass = pSettings->read_if_exists<pcstr>(section_id.c_str(), "item_class", "NULL");
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

    return pSettings->r_string(section_id.c_str(), "inv_icon");
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
    if (m_inv_ui_showing_rt && m_hide_base_for_dynamic)
        i->wnd()->SetTextureColor(0x00FFFFFF);

    const bool skip_alundaio_layers = data_is_string ? weapon_inv_icon::IsEnabledForSection(m_section_id.c_str())
                                                       : (object() && weapon_inv_icon::IsEnabledForItem(object()));
    if (!skip_alundaio_layers)
    {
        for (const SIconLayer* layer : m_layers)
        {
            CUIStatic* s = xr_new<CUIStatic>("Layer");
            s->SetAutoDelete(true);
            s->SetShader(InventoryUtilities::GetEquipmentIconShader(GetIconPath(layer->m_name).c_str()));
            InitLayer(s, layer->m_name, layer->offset, false, layer->m_scale);
            s->SetTextureColor(i->wnd()->GetTextureColor());
            i->wnd()->AttachChild(s);
        }
    }

    if (m_dynamic_overlay && m_inv_ui_showing_rt)
    {
        CUIStatic* s = xr_new<CUIStatic>("Dynamic overlay");
        s->SetAutoDelete(true);
        s->SetShader(m_dynamic_overlay->GetShader());
        const Frect& src_rect = m_dynamic_overlay->GetTextureRect();
        s->SetTextureRect(src_rect);
        const float src_w = src_rect.width();
        const float src_h = src_rect.height();
        const Fvector2 drag_size = i->wnd()->GetWndSize();
        const float scale = (src_w > 0.f && src_h > 0.f) ? std::min(drag_size.x / src_w, drag_size.y / src_h) : 1.f;
        const Fvector2 draw_size{src_w * scale, src_h * scale};
        const Fvector2 draw_pos{(drag_size.x - draw_size.x) * 0.5f, (drag_size.y - draw_size.y) * 0.5f};
        s->SetWndPos(draw_pos);
        s->SetWndSize(draw_size);
        s->SetStretchTexture(true);
        s->SetTextureColor(GetTextureColor() | 0xFF000000);
        i->wnd()->AttachChild(s);
    }

    return i;
}

void CUIInventoryCellItem::SetTextureColor(u32 color)
{
    const u32 base_color = m_hide_base_for_dynamic ? (color & 0x00FFFFFF) : color;
    inherited::SetTextureColor(base_color);
    for (const SIconLayer* layer : m_layers)
    {
        if (layer->m_icon)
            layer->m_icon->SetTextureColor(color);
    }
    if (m_dynamic_overlay)
        m_dynamic_overlay->SetTextureColor(color | 0xFF000000);
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

void CUIInventoryCellItem::InitDynamicOverlay(bool b_rotate)
{
    if (!m_dynamic_overlay)
        return;

    if (data_is_string)
    {
        if (!weapon_inv_icon::IsEnabledForSection(m_section_id.c_str()))
            return;
    }
    else if (!object())
        return;

    Frect abs_rect;
    GetAbsoluteRect(abs_rect);
    if (m_inv_dyn_overlay_layout_valid && m_inv_dyn_overlay_heading_cache == b_rotate &&
        fsimilar(abs_rect.lt.x, m_inv_dyn_overlay_abs_cache.lt.x) && fsimilar(abs_rect.lt.y, m_inv_dyn_overlay_abs_cache.lt.y) &&
        fsimilar(abs_rect.width(), m_inv_dyn_overlay_abs_cache.width()) &&
        fsimilar(abs_rect.height(), m_inv_dyn_overlay_abs_cache.height()))
        return;

    Fvector2 base_scale;
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

    u32 src_w = (u32)(m_grid_size.x * INV_GRID_WIDTHF);
    u32 src_h = (u32)(m_grid_size.y * INV_GRID_HEIGHTF);
    if (data_is_string)
        weapon_inv_icon::GetWeaponIconUiTexelSizeForSection(m_section_id.c_str(), m_inv_icon_preset, src_w, src_h);
    else if (CInventoryItem* itm = object())
    {
        src_w = (u32)(itm->GetInvGridRect().rb.x * INV_GRID_WIDTHF);
        src_h = (u32)(itm->GetInvGridRect().rb.y * INV_GRID_HEIGHTF);
        if (CWeapon* wpn = itm->cast_weapon())
            weapon_inv_icon::GetWeaponIconUiTexelSize(wpn, m_inv_icon_preset, src_w, src_h);
    }

    Fvector2 ts{};
    auto& sh = m_dynamic_overlay->GetShader();
    if (sh && sh->GetBaseTextureResolution(ts) && ts.x > 0.f && ts.y > 0.f)
    {
        src_w = std::min<u32>(src_w, (u32)ts.x);
        src_h = std::min<u32>(src_h, (u32)ts.y);
    }

    Frect tex_rect;
    tex_rect.set(0.f, 0.f, float(src_w), float(src_h));
    m_dynamic_overlay->SetTextureRect(tex_rect);

    Fvector2 cell_size{float(src_w), float(src_h)};
    cell_size.mul(base_scale);

    if (b_rotate)
        m_dynamic_overlay->SetWndSize(Fvector2{cell_size.y, cell_size.x});
    else
        m_dynamic_overlay->SetWndSize(cell_size);

    Fvector2 addon_offset{0.f, 0.f};
    if (b_rotate)
    {
        const Fvector2 new_offset
        {
            addon_offset.y * base_scale.x,
            GetHeight() - addon_offset.x * base_scale.x - cell_size.x
        };
        addon_offset = new_offset;
        addon_offset.x *= UI().get_current_kx();
        m_dynamic_overlay->SetHeading(GetHeading());
        const Fvector2 offs{0.0f, m_dynamic_overlay->GetWndSize().y};
        m_dynamic_overlay->SetHeadingPivot(Fvector2{0.0f, 0.0f}, offs, true);
    }

    GetAbsoluteRect(abs_rect);
    m_dynamic_overlay->SetWndPos(Fvector2{abs_rect.left + addon_offset.x, abs_rect.top + addon_offset.y});
    m_dynamic_overlay->SetStretchTexture(true);
    m_dynamic_overlay->EnableHeading(b_rotate);

    m_inv_dyn_overlay_abs_cache = abs_rect;
    m_inv_dyn_overlay_heading_cache = b_rotate;
    m_inv_dyn_overlay_layout_valid = true;
}

void CUIInventoryCellItem::RemoveDynamicOverlay()
{
    m_inv_ui_showing_rt = false;
    m_inv_dyn_overlay_layout_valid = false;
}

void CUIInventoryCellItem::UpdateDynamicOverlay(bool ready)
{
    if (!ready)
    {
        RemoveDynamicOverlay();
        return;
    }

    if (data_is_string)
    {
        if (!weapon_inv_icon::IsEnabledForSection(m_section_id.c_str()))
        {
            RemoveDynamicOverlay();
            return;
        }
    }
    else if (!object())
    {
        RemoveDynamicOverlay();
        return;
    }

    if (!m_dynamic_overlay)
    {
        m_dynamic_overlay = xr_new<CUIStatic>("Dynamic overlay");
    }

    const shared_str tex = data_is_string
        ? weapon_inv_icon::TextureResourceName(m_section_id.c_str(), m_inv_icon_preset)
        : weapon_inv_icon::TextureResourceName(object(), m_inv_icon_preset);
    m_dynamic_overlay->SetShader(InventoryUtilities::GetInstanceRtIconShader(tex.c_str()));
    m_dynamic_overlay->SetTextureColor(GetTextureColor() | 0xFF000000);
    InitDynamicOverlay(Heading());
}

void CUIInventoryCellItem::Update()
{
    if (data_is_string)
    {
        if (weapon_inv_icon::IsEnabledForSection(m_section_id.c_str()))
        {
            const u32 rev = weapon_inv_icon::InvIconRtEpoch();
            const bool ready =
                weapon_inv_icon::SectionSharedInvIconPresetReady(m_section_id.c_str(), m_inv_icon_preset);
            if (rev != m_inv_seen_rev || ready != m_inv_ui_showing_rt)
                UpdateIcon();
        }
    }
    else
    {
        CInventoryItem* itm = object();
        if (weapon_inv_icon::IsEnabledForItem(itm))
        {
            const u32 rev = itm->DynamicInvIconRevision();
            const bool ready = itm->DynamicInvIconPresetReady(m_inv_icon_preset);
            if (rev != m_inv_seen_rev || ready != m_inv_ui_showing_rt)
                UpdateIcon();
        }
    }

    inherited::Update();
    UpdateConditionProgressBar(); //Alundaio
    UpdateItemText();

    u32 color = GetTextureColor();
    // Base icon can be hidden by zero alpha; keep logical tint color opaque for child overlays.
    color |= 0xFF000000;
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

    const bool h = Heading();
    const float cw = GetWidth();
    const float ch = GetHeight();
    const bool layers_dirty = !m_inv_layer_layout_valid || !fsimilar(cw, m_inv_layer_cache_w) || !fsimilar(ch, m_inv_layer_cache_h) ||
        h != m_inv_layer_cache_heading;
    if (layers_dirty)
    {
        for (SIconLayer* layer : m_layers)
        {
            layer->m_icon = InitLayer(layer->m_icon, layer->m_name, layer->offset, h, layer->m_scale);
            layer->m_icon->SetTextureColor(color);
        }
        m_inv_layer_cache_w = cw;
        m_inv_layer_cache_h = ch;
        m_inv_layer_cache_heading = h;
        m_inv_layer_layout_valid = true;
    }
    else
    {
        for (SIconLayer* layer : m_layers)
            layer->m_icon->SetTextureColor(color);
    }
    InitDynamicOverlay(h);
}

void CUIInventoryCellItem::UpdateItemText()
{
    const u32 helper_count =
        (u32)std::count_if(m_childs.begin(), m_childs.end(), ::detail::is_helper_pred()) + IsHelper() ? 1 : 0;

    const u32 count = data_is_string ? ChildsCount() + 1 : ChildsCount() + 1 - helper_count;

    string32 tempStr;
    pcstr finalText = nullptr;

    if (debug_ui_item_cell)
        Msg("CUIInventoryCellItem::UpdateItemText 1 data_is_string: %d item: %s childs: %d b_isAmmo: %d", data_is_string, m_section_id.c_str(), ChildsCount(), b_isAmmo);

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
        if (debug_ui_item_cell)
            Msg("CUIInventoryCellItem::UpdateItemText 2 data_is_string: %d item: %s childs: %d finalText: %s", data_is_string, m_section_id.c_str(), ChildsCount(), finalText);
    }

    if (m_text)
    {
        if (debug_ui_item_cell)
            Msg("CUIInventoryCellItem::UpdateItemText 3 data_is_string: %d item: %s childs: %d finalText: %s", data_is_string, m_section_id.c_str(), ChildsCount(), finalText);
        m_text->Show(nullptr != finalText);
        m_text->SetText(finalText);
    }
    else
    {
        this->SetText(finalText);
    }
}

CUIAmmoCellItem::CUIAmmoCellItem(CWeaponAmmo* itm) : inherited(itm, false, false, false) {}
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

CUIWeaponCellItem::CUIWeaponCellItem(CWeapon* itm) : CUIWeaponCellItem(itm, eWpnInvIcon_Inventory) {}

CUIWeaponCellItem::CUIWeaponCellItem(CWeapon* itm, EWeaponInvIconPreset preset)
    : inherited(itm, true, false, true)
{
    m_inv_icon_preset = preset;

    weapon_inv_icon::DbgTraceWeaponCellShaderDecision(itm->cNameSect().c_str());

    m_addons[eSilencer] = NULL;
    m_addons[eScope] = NULL;
    m_addons[eLauncher] = NULL;

    if (itm->SilencerAttachable())
        m_addon_offset[eSilencer].set(object()->GetSilencerX(), object()->GetSilencerY());

    if (itm->ScopeAttachable() || itm->mainScopeSlotIsBusy())
        m_addon_offset[eScope].set(object()->GetScopeX(), object()->GetScopeY());

    if (itm->GrenadeLauncherAttachable())
        m_addon_offset[eLauncher].set(object()->GetGrenadeLauncherX(), object()->GetGrenadeLauncherY());

    UpdateIcon();
}

void CUIWeaponCellItem::UpdateIcon()
{
    CUIInventoryCellItem::UpdateIcon();
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
    if (pSettings->line_exist(itm->m_section_id.c_str(), "inv_icon_alt"))
        m_addons[t]->SetShader(
            InventoryUtilities::GetEquipmentIconShaderForItemSection(itm->m_section_id.c_str(), "inv_icon_alt"));
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

    // Динамическая иконка уже содержит модель с аддонами; декали прицел/глушитель/ПГ дублируют картинку
    // и раньше отключались только при bUseAttachmentSystem — для старого режима без attachment system их тоже убираем.
    if (weapon_inv_icon::IsEnabledForSection(object()->cNameSect().c_str()))
    {
        for (u32 ai = 0; ai < eMaxAddon; ++ai)
        {
            if (m_addons[ai])
                DestroyIcon((eAddonType)ai);
        }
        return;
    }

    bool bForceReInitAddons = (b != Heading());

    if (object()->SilencerAttachable())
    {
        if (object()->IsSilencerAttached())
        {
            if (!GetIcon(eSilencer) || bForceReInitAddons || object()->b_forceIconUpdate)
            {
                CreateIcon(eSilencer);
                RefreshOffset();
                InitAddon(GetIcon(eSilencer), object()->GetSilencerName().c_str(), m_addon_offset[eSilencer], Heading());
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
                InitAddon(GetIcon(eScope), object()->GetScopeName().c_str(), m_addon_offset[eScope], Heading());
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
                    GetIcon(eLauncher), object()->GetGrenadeLauncherName().c_str(), m_addon_offset[eLauncher], Heading());
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

    if (weapon_inv_icon::IsEnabledForSection(object()->cNameSect().c_str()))
        return;

    if (is_silencer() && GetIcon(eSilencer))
        InitAddon(GetIcon(eSilencer), object()->GetSilencerName().c_str(), m_addon_offset[eSilencer], parent_list->GetVerticalPlacement());

    if (is_scope() && GetIcon(eScope))
        InitAddon(GetIcon(eScope), object()->GetScopeName().c_str(), m_addon_offset[eScope], parent_list->GetVerticalPlacement());

    if (is_launcher() && GetIcon(eLauncher))
        InitAddon(GetIcon(eLauncher), object()->GetGrenadeLauncherName().c_str(), m_addon_offset[eLauncher],
            parent_list->GetVerticalPlacement());
}

void CUIWeaponCellItem::InitAddon(CUIStatic* s, LPCSTR section, Fvector2 addon_offset, bool b_rotate)
{
    Frect tex_rect;
    Fvector2 base_scale;

    if (pSettings->line_exist(section, "inv_icon_alt"))
        s->SetShader(InventoryUtilities::GetEquipmentIconShaderForItemSection(section, "inv_icon_alt"));
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
    if (weapon_inv_icon::IsEnabledForSection(object()->cNameSect().c_str()))
        return inherited::CreateDragItem();

    CUIDragItem* i = inherited::CreateDragItem();
    CUIStatic* s = NULL;

    if (GetIcon(eSilencer))
    {
        s = xr_new<CUIStatic>("Silencer");
        s->SetAutoDelete(true);
        InitAddon(s, object()->GetSilencerName().c_str(), m_addon_offset[eSilencer], false);
        s->SetTextureColor(i->wnd()->GetTextureColor());
        i->wnd()->AttachChild(s);
    }

    if (GetIcon(eScope))
    {
        s = xr_new<CUIStatic>("Scope");
        s->SetAutoDelete(true);
        InitAddon(s, object()->GetScopeName().c_str(), m_addon_offset[eScope], false);
        s->SetTextureColor(i->wnd()->GetTextureColor());
        i->wnd()->AttachChild(s);
    }

    if (GetIcon(eLauncher))
    {
        s = xr_new<CUIStatic>("Grenade launcher");
        s->SetAutoDelete(true);
        InitAddon(s, object()->GetGrenadeLauncherName().c_str(), m_addon_offset[eLauncher], false);
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
