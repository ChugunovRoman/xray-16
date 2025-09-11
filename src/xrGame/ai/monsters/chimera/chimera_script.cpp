#include "pch_script.h"

#include "chimera.h"

void CChimera::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CChimera, CGameObject>("CChimera")
            .def(constructor<>())
    ];
}
