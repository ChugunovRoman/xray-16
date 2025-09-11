////////////////////////////////////////////////////////////////////////////
//	Module 		: smart_cover_script.cpp
//	Created 	: 21.12.2007
//  Modified 	: 21.12.2007
//	Author		: Dmitriy Iassenev
//	Description : smart cover script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "smart_cover.h"

namespace smart_cover
{
void object::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<smart_cover::object, CGameObject>("smart_cover_object")
            .def(constructor<>())
    ];
}
} // namespace smart_cover
