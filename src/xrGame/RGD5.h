#pragma once

#include "Grenade.h"

class CRGD5 : public CGrenade
{
    typedef CGrenade inherited;

public:
    CRGD5() {}
    virtual ~CRGD5() {}

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject, CExplosive);
};
