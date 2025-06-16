///////////////////////////////////////////////////////////////
// Scope.h
// Scope - апгрейд оружия снайперский прицел
///////////////////////////////////////////////////////////////

#pragma once

#include "inventory_item_object.h"

class CScope : public CInventoryItemObject
{
private:
    typedef CInventoryItemObject inherited;

public:
    CScope();
    virtual ~CScope();

    bool HasScopeTexture() const { return m_scope_texture != nullptr; }
    bool IsBaseScope() const { return m_addon_type == "base_scope"; }
    bool IsColimScope() const { return m_addon_type == "colim_scope"; }
    bool IsAttachment() const { return m_addon_type == "attachment"; }

    // base_scope
    // colim_scope
    // attachment
    shared_str m_addon_type;
    CWeapon::EWeaponAddonSlotType m_slot_type;
    bool m_has_ort{false};
    bool m_scope_dynamic_zoom{false};

    shared_str m_scope_texture;

    virtual void Load(LPCSTR section);
};
