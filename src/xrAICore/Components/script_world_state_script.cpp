////////////////////////////////////////////////////////////////////////////
//	Module 		: script_world_state_script.cpp
//	Created 	: 19.03.2004
//  Modified 	: 19.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script world state script export
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include "condition_state.h"
#include "Navigation/graph_engine_space.h"

#include "xrScriptEngine/script_space.hpp"

namespace GraphEngineSpace
{
void CScriptWorldState::script_register(lua_State* luaState)
{
    using namespace luabind;
    using GraphEngineSpace::CWorldState;

    module(luaState)
    [
        class_<CWorldState>("world_state")
            .def(constructor<>())
            .def(constructor<CWorldState>())
            .def("add_property", (void (CWorldState::*)(const CWorldState::COperatorCondition&))&CWorldState::add_condition)
            .def("remove_property", &CWorldState::remove_condition)
            .def("clear", &CWorldState::clear)
            .def("includes", &CWorldState::includes)
            .def("property", &CWorldState::property)
            .def(const_self < CWorldState())
            .def(const_self == CWorldState())
    ];
}
} // namespace GraphEngineSpace
