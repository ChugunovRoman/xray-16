#pragma once

#include "WeaponCustomPistol.h"

class CWeaponSVU : public CWeaponCustomPistol
{
    typedef CWeaponCustomPistol inherited;

public:
    CWeaponSVU() {}
    virtual ~CWeaponSVU() {}

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazined);
};
