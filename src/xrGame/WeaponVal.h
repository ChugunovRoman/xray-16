#pragma once

#include "WeaponMagazined.h"

class CWeaponVal : public CWeaponMagazined
{
    typedef CWeaponMagazined inherited;

public:
    CWeaponVal(void);
    virtual ~CWeaponVal(void);

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazined);
};
