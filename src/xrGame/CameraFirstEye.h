#pragma once

#include "xrEngine/CameraBase.h"

class CCameraFirstEye : public CCameraBase
{
    using inherited = CCameraBase;

    Fvector lookat_point{};
    bool lookat_active{};

    void UpdateLookat();

public:
    CCameraFirstEye(IGameObject* p, u8 flags = 0) : CCameraBase(p, flags) {}

    void Move(int cmd, float val = 0, float factor = 1.0f) override;

    void OnActivate(CCameraBase* old_cam) override;
    void Update(const Fvector& point, Fvector& noise_angle) override;

    float GetWorldYaw() override { return -yaw; }
    float GetWorldPitch() override { return pitch; }

    void LookAtPoint(const Fvector& p)
    {
        lookat_point = p;
        lookat_active = true;
    }
};
