#include "pch_script.h"
#include "ai_trader.h"

void CAI_Trader::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CAI_Trader, CGameObject>("CAI_Trader")
            .def(constructor<>())
    ];
}
