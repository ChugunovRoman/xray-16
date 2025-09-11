////////////////////////////////////////////////////////////////////////////
//	Module 		: base_client_classes_script.cpp
//	Created 	: 20.12.2004
//  Modified 	: 20.12.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay base client classes script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "base_client_classes_wrappers.h"

#include "xrEngine/Feel_Sound.h"
#include "Include/xrRender/RenderVisual.h"
#include "Include/xrRender/Kinematics.h"
#include "ai/stalker/ai_stalker.h"

// Include files below to force the linker to include the symbols
#include "script_effector.h"
#include "script_particles.h"

void CGameObject::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CBlend>("CBlend"),

        class_<IKinematicsAnimated>("IKinematicsAnimated")
            .def("PlayCycle", +[](IKinematicsAnimated* sa, pcstr anim)
            {
                sa->PlayCycle(anim);
            }),

        class_<IRenderVisual>("IRender_Visual")
            .def("dcast_PKinematicsAnimated", &IRenderVisual::dcast_PKinematicsAnimated),

        // 'DLL_Pure' is preserved to maintain backward compatibility with mod scripts
        class_<IFactoryObject, no_bases, default_holder, FactoryObjectWrapper>("DLL_Pure")
            .def(constructor<>())
            .def("_construct", &IFactoryObject::_construct, &FactoryObjectWrapper::_construct_static),

        class_<ISheduled, no_bases, default_holder, CISheduledWrapper>("ISheduled"),

        class_<IRenderable, no_bases, default_holder, CIRenderableWrapper>("IRenderable"),

        class_<ICollidable>("ICollidable"),

        class_<CGameObject, bases<IFactoryObject, ISheduled, ICollidable, IRenderable>, default_holder,
            CGameObjectWrapper>("CGameObject")
            .def(constructor<>())
            .def("_construct", &CGameObject::_construct, &CGameObjectWrapper::_construct_static)
            .def("Visual", &CGameObject::Visual)

            .def("net_Export", &CGameObject::net_Export, &CGameObjectWrapper::net_Export_static)
            .def("net_Import", &CGameObject::net_Import, &CGameObjectWrapper::net_Import_static)
            .def("net_Spawn", &CGameObject::net_Spawn, &CGameObjectWrapper::net_Spawn_static)

            .def("use", &CGameObject::use, &CGameObjectWrapper::use_static)

            .def("getVisible", &CGameObject::getVisible)
            .def("getEnabled", &CGameObject::getEnabled)
    ];
}
