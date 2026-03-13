#include "stdafx.h"
#include "Include/xrRender/KinematicsAnimated.h"

CBlend* IKinematicsAnimated::LL_SetInitialPartPose(u16 part, MotionID motion_ID, BOOL bMixing, float blendAccrue,
    float blendFalloff, float Speed, BOOL noloop, PlayCallback Callback, LPVOID CallbackParam, u8 channel)
{
    UNUSED(part, motion_ID, bMixing, blendAccrue, blendFalloff, Speed, noloop, Callback, CallbackParam, channel);
    return nullptr;
}
