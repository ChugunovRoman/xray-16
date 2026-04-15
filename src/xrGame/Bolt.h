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

    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);

    virtual void Load(LPCSTR section);

    virtual void SetInitiator(u16 id);
    virtual u16 Initiator();

    virtual void Throw();
    virtual bool Action(u16 cmd, u32 flags);
    virtual bool Useful() const;
    virtual void activate_physic_shell();
    virtual void spawn_fake_missile();
    virtual void OnAnimationEnd(u32 state);

    virtual void UpdateHudAdditional(Fmatrix&);

    virtual bool UsedAI_Locations() { return false; }
    virtual IDamageSource* cast_IDamageSource() { return this; }

    void SetCount(const u16 count) { m_count = count; };
    void AddCount(const u16 count) { m_count += count; };
    u16 GetCount() { return m_count; };

    virtual bool GetBriefInfo(II_BriefInfo& info);

    void RebuildDescription() override;

protected:
    float m_fLR_MovingFactor;  // Фактор бокового наклона худа при ходьбе [-1; +1]
    float m_fLR_CameraFactor;  // Фактор бокового наклона худа при движении камеры [-1; +1]
    float m_fLR_InertiaFactor; // Фактор горизонтальной инерции худа при движении камеры [-1; +1]
    float m_fUD_InertiaFactor; // Фактор вертикальной инерции худа при движении камеры [-1; +1]

    Fvector m_strafe_offset[4][2]; //pos,rot,data1,data2/ normal,aim-GL --#SM+#--

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};
