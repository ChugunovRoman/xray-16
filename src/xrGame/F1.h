#pragma once

#include "Grenade.h"

class CF1 : public CGrenade
{
    typedef CGrenade inherited;

public:
    CF1() {}
    virtual ~CF1() {}

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject, CExplosive);
};
