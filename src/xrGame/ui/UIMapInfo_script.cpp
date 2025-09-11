#include "pch_script.h"

#include "UIMapInfo.h"

void CUIMapInfo::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUIMapInfo, CUIWindow>("CUIMapInfo")
            .def(constructor<>())
            .def("Init", &CUIMapInfo::InitMapInfo)
            .def("InitMap", &CUIMapInfo::InitMap)
    ];
}
