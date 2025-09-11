#include "pch_script.h"

#include "snork.h"

void CSnork::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CSnork, CGameObject>("CSnork")
        .def(constructor<>())
    ];
}
