#pragma once

#include "xrCore/fastdelegate.h"
#include "xrCore/_vector3d.h"

#include "device.h"

struct SBaseEffector
{
    using CB_ON_B_REMOVE = fastdelegate::FastDelegate0<>;
    CB_ON_B_REMOVE m_on_b_remove_callback;
    virtual ~SBaseEffector() = 0;
};

inline SBaseEffector::~SBaseEffector() = default;

struct SCamEffectorInfo
{
    Fvector p{ 0, 0, 0 };
    Fvector d{ 0, 0, 1 };
    Fvector n{ 0, 1, 0 };
    Fvector r{};
    float fFov{ 90.0f };
    float fNear{ VIEWPORT_NEAR };
    float fFar{ 100.0f };
    float fAspect{ 1.f };
    float offsetX{}; // Required for Nvidia Ansel
    float offsetY{}; // Required for Nvidia Ansel
    bool dont_apply{};
    bool affected_on_hud{ true };
};

enum ECamEffectorType
{
    cefDemo = 0,
    cefAnsel,
    cefNext
};

enum EEffectorPPType
{
    ppeNext = 0,
};

// refs
class ENGINE_API CCameraBase;
class ENGINE_API CEffectorCam;
class ENGINE_API CEffectorPP;
