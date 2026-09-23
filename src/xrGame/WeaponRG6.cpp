#include "StdAfx.h"
#include "WeaponRG6.h"
#include "Entity.h"
#include "ExplosiveRocket.h"
#include "Level.h"
#include "xrPhysics/MathUtils.h"
#include "Actor.h"

#ifdef DEBUG
#include "PHDebug.h"
#endif

CWeaponRG6::~CWeaponRG6() {}
bool CWeaponRG6::net_Spawn(CSE_Abstract* DC)
{
    bool l_res = inheritedSG::net_Spawn(DC);
    if (!l_res)
        return l_res;

    if (iAmmoElapsed && !getCurrentRocket())
        SpawnMissingRockets(u32(iAmmoElapsed), GetFakeGrenadeName(), this);

    return l_res;
};

void CWeaponRG6::Load(LPCSTR section)
{
    inheritedRL::Load(section);
    inheritedSG::Load(section);
}

shared_str CWeaponRG6::GetFakeGrenadeName() const
{
    return pSettings->r_string(m_ammoTypes[m_ammoType].c_str(), "fake_grenade_name");
}

void CWeaponRG6::FireStart()
{
    // Fake grenades can get out of sync with the drum (unjam ejects a cartridge but keeps its
    // rocket, scripts may change the ammo count, etc.). Make sure there is a rocket for every
    // cartridge before a shot is started; the launch itself happens in FireTrace.
    if (GetState() == eIdle && iAmmoElapsed > 0)
        SpawnMissingRockets(u32(iAmmoElapsed), GetFakeGrenadeName(), this);

    inheritedSG::FireStart();
}

void CWeaponRG6::FireTrace(const Fvector& P, const Fvector& D)
{
    inheritedSG::FireTrace(P, D);
    LaunchGrenade(P, D);
}

void CWeaponRG6::LaunchGrenade(const Fvector& P, const Fvector& D)
{
    if (!getRocketCount())
    {
        Msg("! CWeaponRG6::LaunchGrenade: no fake grenade to launch, weapon [%s][%d], ammo elapsed [%d]",
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
    VERIFY2(_valid(launch_matrix), "CWeaponRG6::LaunchGrenade. Invalid launch_matrix");
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

void CWeaponRG6::PlayAnimAddOneCartridgeWeapon()
{
    VERIFY(GetState() == eReload);

    // Index by the cartridges already in the drum: fake rockets are attached asynchronously
    // and their count may temporarily differ from the real one.
    const std::string animName = make_string("anm_add_cartridge_%d_%d", iAmmoElapsed, iAmmoElapsed + 1);

    if (isHUDAnimationExist(animName.c_str()))
        PlayHUDMotion(animName.c_str(), "anim_add_cartridge", FALSE, this, GetState());
    else if (isHUDAnimationExist("anm_add_cartridge"))
        PlayHUDMotion("anm_add_cartridge", "anim_add_cartridge", FALSE, this, GetState());
}

u8 CWeaponRG6::AddCartridge(u8 cnt)
{
    const u8 rest = inheritedSG::AddCartridge(cnt);

    // One fake grenade per cartridge that is really in the drum. The count is derived from
    // iAmmoElapsed rather than from cnt, so that leftover rockets (unjam, quick unload) are
    // reused instead of piling up, and nothing is spawned when no cartridge was added.
    if (iAmmoElapsed > 0)
        SpawnMissingRockets(u32(iAmmoElapsed), GetFakeGrenadeName(), this);

    return rest;
}

void CWeaponRG6::OnEvent(NET_Packet& P, u16 type)
{
    inheritedSG::OnEvent(P, type);

    u16 id;
    switch (type)
    {
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
    }
}
