#include "pch_script.h"

#include "Mincer.h"
#include "RadioactiveZone.h"

void CMincer::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CMincer, CGameObject>("CMincer")
            .def(constructor<>()),
        class_<CRadioactiveZone, CGameObject>("CRadioactiveZone")
            .def(constructor<>())
    ];
}
