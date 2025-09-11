////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_monster_movement_manager_script.cpp
//	Created 	: 02.11.2005
//  Modified 	: 22.11.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife monster movement manager class script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "movement_manager_space.h"
#include "alife_monster_movement_manager.h"
#include "alife_monster_detail_path_manager.h"
#include "alife_monster_patrol_path_manager.h"

void CALifeMonsterMovementManager::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CALifeMonsterMovementManager>("CALifeMonsterMovementManager")
            .def("detail", +[](const CALifeMonsterMovementManager* self) { return (&self->detail()); })
            .def("patrol", +[](const CALifeMonsterMovementManager* self) { return (&self->patrol()); })
            .def("path_type", (void (CALifeMonsterMovementManager::*)(const MovementManager::EPathType&))(
                                  &CALifeMonsterMovementManager::path_type))
            .def("path_type", (const MovementManager::EPathType& (CALifeMonsterMovementManager::*)()
                                      const)(&CALifeMonsterMovementManager::path_type))
            .def("actual", &CALifeMonsterMovementManager::actual)
            .def("completed", &CALifeMonsterMovementManager::completed)
    ];
}
