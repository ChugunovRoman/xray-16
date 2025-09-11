#include "pch_script.h"

#include "tushkano.h"

void CTushkano::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CTushkano, CGameObject>("CTushkano")
            .def(constructor<>())
    ];
}
