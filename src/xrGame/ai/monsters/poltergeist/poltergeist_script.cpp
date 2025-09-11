#include "pch_script.h"

#include "poltergeist.h"

void CPoltergeist::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CPoltergeist, CGameObject>("CPoltergeist")
            .def(constructor<>())
    ];
}
