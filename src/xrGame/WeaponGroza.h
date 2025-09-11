#pragma once

#include "WeaponMagazinedWGrenade.h"

class CWeaponGroza : public CWeaponMagazinedWGrenade
{
    typedef CWeaponMagazinedWGrenade inherited;

public:
    CWeaponGroza();
    virtual ~CWeaponGroza();

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazinedWGrenade);
};
