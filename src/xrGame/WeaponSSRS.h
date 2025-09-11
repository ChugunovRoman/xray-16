#pragma once

#include "RocketLauncher.h"
#include "WeaponMagazined.h"

class CWeaponSSRS : public CRocketLauncher, public CWeaponMagazined
{
    typedef CRocketLauncher inheritedRL;
    typedef CWeaponMagazined inheritedWM;

public:
    virtual ~CWeaponSSRS();
    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void Load(LPCSTR section);
    virtual void OnEvent(NET_Packet& P, u16 type);

protected:
    virtual void FireStart();
    virtual void OnStateSwitch(u32 S, u32 oldState);
private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponMagazined);
    void ReloadRL();
};
