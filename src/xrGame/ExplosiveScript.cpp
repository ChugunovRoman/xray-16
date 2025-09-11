#include "pch_script.h"

#include "Explosive.h"

void CExplosive::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CExplosive>("explosive")
            .def("explode", &CExplosive::Explode)
    ];
}
