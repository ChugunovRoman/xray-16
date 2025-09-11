#include "pch_script.h"

#include "cat.h"

void CCat::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CCat, CGameObject>("CCat")
            .def(constructor<>())
    ];
}
