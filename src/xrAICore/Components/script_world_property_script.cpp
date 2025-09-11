////////////////////////////////////////////////////////////////////////////
//	Module 		: script_world_property_script.cpp
//	Created 	: 19.03.2004
//  Modified 	: 19.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script world property script export
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include "operator_abstract.h"
#include "Navigation/graph_engine_space.h"

#include "xrScriptEngine/script_space.hpp"

namespace GraphEngineSpace
{
void CScriptWorldProperty::script_register(lua_State* luaState)
{
    using namespace luabind;
    using GraphEngineSpace::CWorldProperty;

    module(luaState)
    [
        class_<CWorldProperty>("world_property")
            .def(constructor<CWorldProperty::condition_type, CWorldProperty::value_type>())
            .def("condition", &CWorldProperty::condition)
            .def("value", &CWorldProperty::value)
            .def(const_self < other<CWorldProperty>())
            .def(const_self == other<CWorldProperty>())
    ];
}
}  // namespace GraphEngineSpace
