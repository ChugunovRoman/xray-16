#include "pch_script.h"

#include "boar.h"

void CAI_Boar::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CAI_Boar, CGameObject>("CAI_Boar")
            .def(constructor<>())
    ];
}
