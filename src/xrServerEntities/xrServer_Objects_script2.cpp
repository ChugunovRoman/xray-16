#include "pch_script.h"

#include "xrServer_Objects.h"
#include "xrServer_script_macroses.h"

void CSE_PHSkeleton::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CSE_PHSkeleton>("cse_ph_skeleton")
    ];
}

void CSE_AbstractVisual::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        luabind_class_abstract2(CSE_AbstractVisual, "CSE_AbstractVisual", CSE_Visual, CSE_Abstract)
            .def("getStartupAnimation", &CSE_AbstractVisual::getStartupAnimation)
    ];
}
