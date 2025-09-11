#include "pch_script.h"

#include "space_restrictor.h"

void CSpaceRestrictor::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CSpaceRestrictor, CGameObject>("CSpaceRestrictor")
            .def(constructor<>())
    ];
}
