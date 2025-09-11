#pragma once

#include "WeaponPistol.h"

class CWeaponFORT : public CWeaponPistol
{
private:
    typedef CWeaponPistol inherited;

protected:
public:
    CWeaponFORT() {}
    virtual ~CWeaponFORT() {}

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazined);
};
