#pragma once

#include "UIGameCustom.h"
#include "script_export_space.h"
#include "game_cl_Base.h"

class UIGame_custom_script : public CUIGameCustom
{
    typedef CUIGameCustom inherited;

public:
    UIGame_custom_script() : inherited(){};
    virtual void SetClGame(game_cl_GameState* g) { inherited::SetClGame(g); };
    virtual void Init(){};

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIGameCustom);
};
