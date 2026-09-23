#pragma once

class CCustomRocket;
class CGameObject;

class CRocketLauncher
{
public:
    CRocketLauncher();
    ~CRocketLauncher();

    virtual void Load(LPCSTR section);

    void AttachRocket(u16 rocket_id, CGameObject* parent_rocket_launcher);
    void DetachRocket(u16 rocket_id, bool bLaunch);

    void SpawnRocket(const shared_str& rocket_section, CGameObject* parent_rocket_launcher);
    void LaunchRocket(const Fmatrix& xform, const Fvector& vel, const Fvector& angular_vel);

protected:
    using ROCKET_VECTOR = xr_vector<CCustomRocket*>;
    ROCKET_VECTOR m_rockets;
    ROCKET_VECTOR m_launched_rockets;
    // Rockets whose spawn has been sent but which are not attached yet (see AttachRocket).
    u32 m_pending_rockets{};

    CCustomRocket* getCurrentRocket();
    void dropCurrentRocket();
    u32 getRocketCount();
    // Attached rockets plus the ones still waiting to be attached.
    u32 getExpectedRocketCount() const;
    // Spawns rockets until the expected rocket count reaches `required`.
    void SpawnMissingRockets(u32 required, const shared_str& rocket_section, CGameObject* parent_rocket_launcher);
    float m_fLaunchSpeed;
};
