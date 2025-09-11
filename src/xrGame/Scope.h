///////////////////////////////////////////////////////////////
// Scope.h
// Scope - апгрейд оружия снайперский прицел
///////////////////////////////////////////////////////////////

#pragma once

#include "inventory_item_object.h"

class CScope final : public CInventoryItemObject
{
protected:
    typedef CInventoryItemObject inherited;

public:
    CScope();
    virtual ~CScope();

    bool HasScopeTexture() const { return m_scope_texture != nullptr; }
    bool IsBaseScope() const { return xr_strcmp(*m_addon_type, "base_scope") == 0; }
    bool IsColimScope() const { return xr_strcmp(*m_addon_type, "colim_scope") == 0; }
    bool IsAttachment() const { return xr_strcmp(*m_addon_type, "attachment") == 0; }
    bool IsLsa() const { return xr_strcmp(*m_addon_type, "lsa") == 0; }

    // base_scope
    // colim_scope
    // attachment
    shared_str m_addon_type;
    CWeapon::EWeaponAddonSlotType m_slot_type;
    CWeapon::EWeaponAddonSlotType m_provided_slot_type;
    bool m_has_ort{false};
    bool m_scope_dynamic_zoom{false};

    shared_str m_scope_texture;

    virtual void Load(LPCSTR section);
private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};
