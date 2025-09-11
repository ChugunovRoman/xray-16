#include "pch_script.h"

#include "pseudo_gigant.h"

void CPseudoGigant::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CPseudoGigant, CGameObject>("CPseudoGigant")
            .def(constructor<>())
    ];
}
