#pragma once

#include "RocketLauncher.h"
#include "WeaponShotgun.h"

class CWeaponRG6 : public CRocketLauncher, public CWeaponShotgun
{
    typedef CRocketLauncher inheritedRL;
    typedef CWeaponShotgun inheritedSG;

public:
    virtual ~CWeaponRG6();
    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void Load(LPCSTR section);
    virtual void OnEvent(NET_Packet& P, u16 type);

    virtual void PlayAnimAddOneCartridgeWeapon();

protected:
    virtual void FireStart();
    // The fake grenade is launched together with the cartridge it belongs to:
    // FireTrace is called only for a shot that really happened (no misfire, ammo consumed).
    virtual void FireTrace(const Fvector& P, const Fvector& D);
    virtual u8 AddCartridge(u8 cnt);

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CWeaponShotgun);
    shared_str GetFakeGrenadeName() const;
    void LaunchGrenade(const Fvector& P, const Fvector& D);
};
