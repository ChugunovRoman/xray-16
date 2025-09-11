////////////////////////////////////////////////////////////////////////////
//	Module 		: property_storage_script.h
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Property storage class script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "property_storage.h"

void CPropertyStorage::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CPropertyStorage>("property_storage")
            .def(constructor<>())
            .def("set_property", &CPropertyStorage::set_property)
            .def("property", &CPropertyStorage::property)
    ];
}
