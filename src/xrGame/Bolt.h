#pragma once
#include "Missile.h"
#include "xrPhysics/DamageSource.h"
class CBolt : public CMissile, public IDamageSource
{
    typedef CMissile inherited;
    u16 m_thrower_id;
    u16 m_count;
    shared_str m_ammo_icon{"bolt_ammo_icon"};

public:
    CBolt();
    virtual ~CBolt();

    virtual void OnH_A_Chield();

    virtual void Load(LPCSTR section);

    virtual void SetInitiator(u16 id);
    virtual u16 Initiator();

    virtual void Throw();
    virtual bool Action(u16 cmd, u32 flags);
    virtual bool Useful() const;
    virtual void activate_physic_shell();
    virtual void spawn_fake_missile();
    virtual void OnAnimationEnd(u32 state);

    virtual bool UsedAI_Locations() { return false; }
    virtual IDamageSource* cast_IDamageSource() { return this; }

    void SetCount(const u16 count) { m_count = count; };
    void AddCount(const u16 count) { m_count += count; };
    u16 GetCount() { return m_count; };

    virtual bool GetBriefInfo(II_BriefInfo& info);
};
