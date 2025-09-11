#include "pch_script.h"

#include "zombie.h"

void CZombie::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CZombie, CGameObject>("CZombie")
            .def(constructor<>())
    ];
}
