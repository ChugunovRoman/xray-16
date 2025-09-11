#include "pch_script.h"

#include "bloodsucker.h"

void CAI_Bloodsucker::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CAI_Bloodsucker, CGameObject>("CAI_Bloodsucker")
            .def(constructor<>())
            .def("force_visibility_state", &CAI_Bloodsucker::force_visibility_state)
    ];
}
