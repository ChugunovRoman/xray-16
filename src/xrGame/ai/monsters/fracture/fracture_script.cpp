#include "pch_script.h"

#include "fracture.h"

void CFracture::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CFracture, CGameObject>("CFracture")
            .def(constructor<>())
    ];
}
