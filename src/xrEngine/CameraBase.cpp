// CameraBase.cpp: implementation of the CCameraBase class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "IGame_Level.h"

#include "CameraBase.h"

void CCameraBase::Load(pcstr section)
{
    rot_speed = pSettings->r_fvector3(section, "rot_speed");

    lim_yaw = pSettings->r_fvector2(section, "lim_yaw");
    lim_pitch = pSettings->r_fvector2(section, "lim_pitch");

    bClampPitch = (0 != lim_pitch[0]) || (0 != lim_pitch[1]);
    bClampYaw = (0 != lim_yaw[0]) || (0 != lim_yaw[1]);

    if (bClampPitch)
        pitch = (lim_pitch[0] + lim_pitch[1]) * 0.5f;
    if (bClampYaw)
        yaw = (lim_yaw[0] + lim_yaw[1]) * 0.5f;
}

ICF float AClamp(const Fvector2& l, float v)
{
    return (2 * v - l[0] - l[1]) / (l[1] - l[0]);
}

float CCameraBase::CheckLimYaw()
{
    if (bClampYaw)
        return AClamp(lim_yaw, yaw);
    return 0;
}

float CCameraBase::CheckLimPitch()
{
    if (bClampYaw)
        return AClamp(lim_pitch, pitch);
    return 0;
}

float CCameraBase::CheckLimRoll()
{
    if (bClampYaw)
        return AClamp(lim_roll, roll);
    return 0;
}
