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

    // base_scope
    // colim_scope
    shared_str m_addon_type{"base_scope"};

    virtual void Load(LPCSTR section);
};
