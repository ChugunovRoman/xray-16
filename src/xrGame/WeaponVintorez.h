#pragma once

#include "WeaponMagazined.h"

class CWeaponVintorez : public CWeaponMagazined
{
    typedef CWeaponMagazined inherited;

public:
    CWeaponVintorez(void);
    virtual ~CWeaponVintorez(void);

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazined);
};
