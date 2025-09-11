#include "pch_script.h"

#include "controller.h"

void CController::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CController, CGameObject>("CController")
            .def(constructor<>())
    ];
}
