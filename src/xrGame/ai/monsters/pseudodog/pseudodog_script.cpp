#include "pch_script.h"

#include "pseudodog.h"
#include "psy_dog.h"

void CAI_PseudoDog::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CAI_PseudoDog, CGameObject>("CAI_PseudoDog")
            .def(constructor<>())
    ];
}

void CPsyDog::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CPsyDog, CGameObject>("CPsyDog")
            .def(constructor<>())
    ];
}

void CPsyDogPhantom::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CPsyDogPhantom, CGameObject>("CPsyDogPhantom")
            .def(constructor<>())
    ];
}
