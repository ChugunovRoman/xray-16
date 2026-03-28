#pragma once
#include "UICellItem.h"
#include "Weapon.h"
#include "../eatable_item.h"
#include "weapon_inv_icon.h"

struct SIconLayer
{
    pcstr m_name;
    CUIStatic* m_icon;
    Fvector2 offset;
    //u32 m_color;
    float m_scale;
};

class CUIInventoryCellItem : public CUICellItem
{
    typedef CUICellItem inherited;

    xr_vector<SIconLayer*> m_layers;

public:
    CUIInventoryCellItem(CInventoryItem* itm);
    CUIInventoryCellItem(CInventoryItem* itm, bool needCondBar, bool needFIcon, bool needUgrIcon);
    CUIInventoryCellItem(shared_str section_id);
    ~CUIInventoryCellItem() override;

    virtual bool EqualTo(CUICellItem* itm);
    virtual void UpdateItemText();
    CUIDragItem* CreateDragItem();
    virtual bool IsHelper();
    virtual void SetIsHelper(bool is_helper);
    bool IsHelperOrHasHelperChild();
    void Update();
    virtual void UpdateIcon();
    void DrawTexture() override;
    shared_str GetIconPath(shared_str section_id);
    CInventoryItem* object() { return (CInventoryItem*)m_pData; }
protected:
    EWeaponInvIconPreset m_inv_icon_preset{eWpnInvIcon_Inventory};
    bool m_inv_ui_showing_rt{};
    u32 m_inv_seen_rev{};
    CUIStatic* m_dynamic_overlay{};
    bool m_hide_base_for_dynamic{};
    bool m_inv_dyn_overlay_layout_valid{};
    Frect m_inv_dyn_overlay_abs_cache{};
    bool m_inv_dyn_overlay_heading_cache{};
    bool m_inv_layer_layout_valid{};
    float m_inv_layer_cache_w{};
    float m_inv_layer_cache_h{};
    bool m_inv_layer_cache_heading{};
    void UpdateDynamicOverlay(bool ready);
    void InitDynamicOverlay(bool b_rotate);
    void RemoveDynamicOverlay();

    //Alundaio
    void OnAfterChild(CUIDragDropListEx* parent_list) override;
    void SetTextureColor(u32 color) override;

    void RemoveLayer(const SIconLayer* layer);
    void CreateLayer(pcstr name, Fvector2 offset, float scale);
    CUIStatic* InitLayer(CUIStatic* s, pcstr section, Fvector2 addon_offset, bool b_rotate, float scale);
    //-Alundaio

    pcstr GetDebugType() override { return "CUIInventoryCellItem"; }
};

class CUIAmmoCellItem final : public CUIInventoryCellItem
{
    typedef CUIInventoryCellItem inherited;

protected:
    virtual void UpdateItemText();

public:
    CUIAmmoCellItem(CWeaponAmmo* itm);

    u32 CalculateAmmoCount();
    virtual bool EqualTo(CUICellItem* itm);
    virtual CUIDragItem* CreateDragItem();
    CWeaponAmmo* object() { return (CWeaponAmmo*)m_pData; }

    pcstr GetDebugType() override { return "CUIAmmoCellItem"; }
};

class CUIWeaponCellItem final : public CUIInventoryCellItem
{
    typedef CUIInventoryCellItem inherited;

public:
    enum eAddonType
    {
        eSilencer = 0,
        eScope,
        eLauncher,
        eMaxAddon
    };

protected:
    CUIStatic* m_addons[eMaxAddon];
    Fvector2 m_addon_offset[eMaxAddon];
    void CreateIcon(eAddonType);
    void DestroyIcon(eAddonType);
    void RefreshOffset();
    CUIStatic* GetIcon(eAddonType);
    void InitAddon(CUIStatic* s, LPCSTR section, Fvector2 offset, bool use_heading);
    bool is_scope();
    bool is_silencer();
    bool is_launcher();

public:
    CUIWeaponCellItem(CWeapon* itm);
    CUIWeaponCellItem(CWeapon* itm, EWeaponInvIconPreset preset);
    virtual ~CUIWeaponCellItem();
    void UpdateIcon() override;
    virtual void Update();
    virtual void Draw();
    virtual void SetTextureColor(u32 color);

    CWeapon* object() { return (CWeapon*)m_pData; }
    virtual void OnAfterChild(CUIDragDropListEx* parent_list);
    virtual CUIDragItem* CreateDragItem();
    virtual bool EqualTo(CUICellItem* itm);
    CUIStatic* get_addon_static(u32 idx) { return m_addons[idx]; }

    pcstr GetDebugType() override { return "CUIWeaponCellItem"; }
};

class CBuyItemCustomDrawCell final : public ICustomDrawCellItem
{
    CGameFont* m_pFont;
    string16 m_string;

public:
    CBuyItemCustomDrawCell(LPCSTR str, CGameFont* pFont);
    virtual void OnDraw(CUICellItem* cell);
};
