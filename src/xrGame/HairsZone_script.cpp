#include "pch_script.h"

#include "HairsZone.h"
#include "AmebaZone.h"
#include "NoGravityZone.h"

void CHairsZone::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CHairsZone, CGameObject>("CHairsZone")
            .def(constructor<>()),
        class_<CAmebaZone, CGameObject>("CAmebaZone")
            .def(constructor<>()),
        class_<CNoGravityZone,CGameObject>("CNoGravityZone")
            .def(constructor<>())
    ];
}
