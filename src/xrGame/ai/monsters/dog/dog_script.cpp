#include "pch_script.h"

#include "dog.h"

void CAI_Dog::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CAI_Dog, CGameObject>("CAI_Dog")
            .def(constructor<>())
    ];
}
