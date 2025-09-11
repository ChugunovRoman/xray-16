#pragma once
#include "WeaponPistol.h"

class CWeaponWalther : public CWeaponPistol
{
    using inherited = CWeaponPistol;

public:
    CWeaponWalther() {}
    virtual ~CWeaponWalther() {}

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazined);
};
