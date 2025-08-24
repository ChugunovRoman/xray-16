#pragma once

#include "xrEngine/CameraBase.h"
#include "xrCore/_quaternion.h"

class CCameraLook : public CCameraBase
{
    using inherited = CCameraBase;

    Fvector2 lim_zoom{};
    float dist{}, prev_d{};

public:
    CCameraLook(IGameObject* p, u8 flags = 0) : CCameraBase(p, flags) {}

    void Load(pcstr section) override;
    void Move(int cmd, float val = 0, float factor = 1.0f) override;

    void OnActivate(CCameraBase* old_cam) override;
    void Update(const Fvector& point, Fvector& noise_dangle) override;

    float GetWorldYaw() override { return -yaw; }
    float GetWorldPitch() override { return pitch; }

protected:
    void UpdateDistance(const Fvector& point);
};

class CCameraLook2 : public CCameraLook
{
public:
    static Fvector m_cam_offset;

protected:
    IGameObject* m_locked_enemy{};
    Fvector2 m_autoaim_inertion_yaw{};
    Fvector2 m_autoaim_inertion_pitch{};
    void UpdateAutoAim();

public:
    CCameraLook2(IGameObject* p, u8 flags = 0) : CCameraLook(p, flags) {}

    void Update(const Fvector& point, Fvector& noise_dangle) override;
    void Load(pcstr section) override;
};

class CCameraFixedLook : public CCameraLook
{
    typedef CCameraLook inherited;

public:
    CCameraFixedLook(IGameObject* p, u8 flags = 0) : CCameraLook(p, flags) {}

    void Move(int cmd, float val = 0, float factor = 1.0f) override;
    void OnActivate(CCameraBase* old_cam) override;
    void Update(const Fvector& point, Fvector& noise_dangle) override;
    void Set(float Y, float P, float R) override;

private:
    Fquaternion m_final_dir{};
    Fquaternion m_current_dir{};
};
