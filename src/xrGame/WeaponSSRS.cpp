#include "StdAfx.h"
#include "WeaponSSRS.h"
#include "Entity.h"
#include "ExplosiveRocket.h"
#include "Level.h"
#include "xrPhysics/MathUtils.h"
#include "Actor.h"

#ifdef DEBUG
#include "PHDebug.h"
#endif

CWeaponSSRS::~CWeaponSSRS() {}
bool CWeaponSSRS::net_Spawn(CSE_Abstract* DC)
{
    bool l_res = inheritedWM::net_Spawn(DC);
    if (!l_res)
        return l_res;

    if (iAmmoElapsed && !getCurrentRocket())
        SpawnMissingRockets(u32(iAmmoElapsed), GetFakeGrenadeName(), this);

    return l_res;
};

void CWeaponSSRS::Load(LPCSTR section)
{
    inheritedWM::Load(section);
    inheritedRL::Load(section);
}

shared_str CWeaponSSRS::GetFakeGrenadeName() const
{
    return pSettings->r_string(m_ammoTypes[m_ammoType].c_str(), "fake_grenade_name");
}

void CWeaponSSRS::FireStart()
{
    // Fake grenades can get out of sync with the magazine (unjam ejects a cartridge but keeps
    // its rocket, scripts may change the ammo count, etc.). Make sure there is a rocket for
    // every cartridge before a shot is started; the launch itself happens in FireTrace.
    if (GetState() == eIdle && iAmmoElapsed > 0)
        SpawnMissingRockets(u32(iAmmoElapsed), GetFakeGrenadeName(), this);

    inheritedWM::FireStart();
}

void CWeaponSSRS::FireTrace(const Fvector& P, const Fvector& D)
{
    inheritedWM::FireTrace(P, D);
    LaunchGrenade(P, D);
}

void CWeaponSSRS::LaunchGrenade(const Fvector& P, const Fvector& D)
{
    if (!getRocketCount())
    {
        Msg("! CWeaponSSRS::LaunchGrenade: no fake grenade to launch, weapon [%s][%d], ammo elapsed [%d]",
            cNameSect().c_str(), ID(), iAmmoElapsed);
        return;
    }

    if (!H_Parent())
        return;

    Fvector p1, d;
    p1.set(P);
    d.set(D);

    Fmatrix launch_matrix;
    launch_matrix.identity();
    launch_matrix.k.set(d);
    Fvector::generate_orthonormal_basis(launch_matrix.k, launch_matrix.j, launch_matrix.i);
    launch_matrix.c.set(p1);

    if (IsGameTypeSingle() && IsZoomed() && smart_cast<CActor*>(H_Parent()))
    {
        H_Parent()->setEnabled(FALSE);
        setEnabled(FALSE);

        collide::rq_result RQ;
        BOOL HasPick = Level().ObjectSpace.RayPick(p1, d, 300.0f, collide::rqtStatic, RQ, this);

        setEnabled(TRUE);
        H_Parent()->setEnabled(TRUE);

        if (HasPick)
        {
            Fvector Transference;
            Transference.mul(d, RQ.range);
            Fvector res[2];
            u8 canfire0 = TransferenceAndThrowVelToThrowDir(
                Transference, CRocketLauncher::m_fLaunchSpeed, EffectiveGravity(), res);
            if (canfire0 != 0)
                d = res[0];
        }
    };

    d.normalize();
    d.mul(CRocketLauncher::m_fLaunchSpeed);
    VERIFY2(_valid(launch_matrix), "CWeaponSSRS::LaunchGrenade. Invalid launch_matrix");
    CRocketLauncher::LaunchRocket(launch_matrix, d, zero_vel);

    CExplosiveRocket* pGrenade = smart_cast<CExplosiveRocket*>(getCurrentRocket());
    VERIFY(pGrenade);
    if (pGrenade)
        pGrenade->SetInitiator(H_Parent()->ID());

    if (OnServer())
    {
        NET_Packet packet;
        u_EventGen(packet, GE_LAUNCH_ROCKET, ID());
        packet.w_u16(u16(getCurrentRocket()->ID()));
        u_EventSend(packet);
    }
    dropCurrentRocket();
}

void CWeaponSSRS::ReloadRL()
{
    // Spawn fake grenades for the cartridges that are about to be loaded. The count is based on
    // the rockets actually present (not on iAmmoElapsed): after an unjam the ejected cartridge
    // leaves its rocket in place, so the two values may differ.
    if (iMagazineSize > 0)
        SpawnMissingRockets(u32(iMagazineSize), GetFakeGrenadeName(), this);
}

void CWeaponSSRS::OnStateSwitch(u32 S, u32 oldState)
{
    inheritedWM::OnStateSwitch(S, oldState);

    switch (S)
    {
    case eReload:
        ReloadRL();
        break;
    }
}

void CWeaponSSRS::OnEvent(NET_Packet& P, u16 type)
{
    u16 id;
    switch (type)
    {
    case GE_WPN_STATE_CHANGE:
    {
        u8 state;
        P.r_u8(state);
        P.r_u8(m_sub_state);
        P.r_u8();
        u8 AmmoElapsed = P.r_u8();
        u8 NextAmmo = P.r_u8();
        if (NextAmmo == undefined_ammo_type)
            m_set_next_ammoType_on_reload = undefined_ammo_type;
        else
            m_set_next_ammoType_on_reload = NextAmmo;

        if (OnClient())
            SetAmmoElapsed(int(AmmoElapsed));
        OnStateSwitch(u32(state), GetState());
    }
    break;
    case GE_OWNERSHIP_TAKE:
    {
        P.r_u16(id);
        inheritedRL::AttachRocket(id, this);
    }
    break;
    case GE_OWNERSHIP_REJECT:
    case GE_LAUNCH_ROCKET:
    {
        bool bLaunch = (type == GE_LAUNCH_ROCKET);
        P.r_u16(id);
        inheritedRL::DetachRocket(id, bLaunch);
    }
    break;
    default: { inheritedWM::OnEvent(P, type); }
    break;
    }
}
