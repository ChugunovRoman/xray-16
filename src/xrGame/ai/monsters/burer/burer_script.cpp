#include "pch_script.h"

#include "burer.h"

void CBurer::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CBurer, CGameObject>("CBurer")
            .def(constructor<>())
    ];
}
