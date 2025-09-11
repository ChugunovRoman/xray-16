#include "pch_script.h"

#include "flesh.h"

void CAI_Flesh::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CAI_Flesh, CGameObject>("CAI_Flesh")
            .def(constructor<>())
    ];
}
