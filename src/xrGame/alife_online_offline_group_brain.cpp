////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_online_offline_group_brain.cpp
//	Created 	: 25.10.2005
//  Modified 	: 25.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife Online Offline Group brain class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_online_offline_group_brain.h"
#include "Common/object_broker.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include "alife_monster_movement_manager.h"
#include "alife_monster_detail_path_manager.h"
#include "alife_monster_patrol_path_manager.h"
#include "ai_space.h"
#include "ef_storage.h"
#include "ef_primary.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "movement_manager_space.h"
#include "alife_smart_terrain_registry.h"
#include "alife_object_registry.h"
#include "alife_time_manager.h"
#include "date_time.h"
#include "npc_cpp_profile.h"
#include "alife_smart_terrain_task.h"
#include "alife_squad_task_native.h"
#include "xrCore/clsid.h"
#include "xrScriptEngine/script_space.hpp"
#include <luabind/wrapper_base.hpp>
#include <optional>
#ifdef DEBUG
#include "Level.h"
#include "map_location.h"
#include "map_manager.h"
#endif

// plans/lua_to_cpp/57-gw-eoc-next-steps.md, E4 (native since wave 3, C2 removed the switch): the task of an offline
// squad - computed in C++ (alife_squad_task_native.h) from the Lua fields and the server objects; the Lua
// sim_squad_scripted:get_current_task for what C++ cannot decide (the actor as the target, another class, odd values)

namespace gw_squad_task
{
namespace
{
constexpr CLASS_ID kSquadTaskClsidSmart = xray::make_clsid("SMRTTRRN");
constexpr CLASS_ID kSquadTaskClsidSquad = xray::make_clsid("ON_OFF_S");
constexpr CLASS_ID kSquadTaskClsidActor = xray::make_clsid("S_ACTOR "); // TEXT2CLSID pads with spaces

// The environment of Decide in the engine (alife_squad_task_native.h)
struct SquadTaskEngineEnv
{
    using Object = CSE_ALifeDynamicObject;
    using Task = CALifeSmartTerrainTask;

    // The Lua instance of a script server object: the reference luabind keeps in the wrapper of the script class (as
    // the engine's own virtual dispatch reads it; the same as the field readers of addon_api_npc.cpp)
    static lua_State* PushSelf(Object& object)
    {
        luabind::wrap_base* base = dynamic_cast<luabind::wrap_base*>(&object);
        if (!base)
            return nullptr;
        const luabind::weak_ref& self = luabind::detail::wrap_access::ref(*base);
        lua_State* L = self.state();
        if (!L)
            return nullptr;
        self.get(L);
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            return nullptr;
        }
        return L;
    }
    // alife():object(id) (alife_simulator_script.cpp): the registry without the fatal error
    static Object* Find(const std::uint16_t id) { return ai().alife().objects().object(id, true); }
    static EClass Class(const Object& object)
    {
        // se_target:clsid() == clsid.smart_terrain: the script class id of "SMRTTRRN" (class_registrator.script)
        if (object.m_tClassID == kSquadTaskClsidSmart)
            return EClass::Smart;
        if (object.m_tClassID == kSquadTaskClsidSquad)
            return EClass::Squad;
        if (object.m_tClassID == kSquadTaskClsidActor)
            return EClass::Actor;
        return EClass::Other;
    }
    // m_game_vertex_id / m_level_vertex_id of Lua (xrServer_Objects_ALife_script.cpp)
    static std::uint16_t GraphId(const Object& object) { return object.m_tGraphID; }
    static std::uint32_t NodeId(const Object& object) { return object.m_tNodeID; }
    static std::uint16_t CommanderId(Object& squad)
    {
        CSE_ALifeOnlineOfflineGroup* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(&squad);
        return group ? group->commander_id() : ALife::_OBJECT_ID(-1);
    }
    static Task* CastTask(lua_State* L, const int index)
    {
        const luabind::object value(luabind::from_stack(L, index));
        return luabind::object_cast_nothrow<Task*>(value, static_cast<Task*>(nullptr));
    }
};

constexpr ENpcCppProfileStage kSquadTaskSourceStages[] = {
    ENpcCppProfileStage::AlifeSquadTaskNativeJob,
    ENpcCppProfileStage::AlifeSquadTaskNativeSmart,
    ENpcCppProfileStage::AlifeSquadTaskNativeSquad,
    ENpcCppProfileStage::AlifeSquadTaskNativeSelf,
    ENpcCppProfileStage::AlifeSquadTaskLuaActor,
    ENpcCppProfileStage::AlifeSquadTaskLuaOtherClass,
    ENpcCppProfileStage::AlifeSquadTaskLuaOdd,
    ENpcCppProfileStage::AlifeSquadTaskLuaNoScript,
};
static_assert(sizeof(kSquadTaskSourceStages) / sizeof(kSquadTaskSourceStages[0]) ==
    static_cast<size_t>(ESource::NoScript) + 1);

} // namespace

// The task of the brain's update: the native computation (alife_squad_task_native.h); the virtual get_current_task
// (the Lua sim_squad_scripted:get_current_task) for the cases C++ leaves to it (actor, other_class, odd, no_script).
// `storage` holds a task made here (the vertices of a squad); the Lua tasks (job.alife_task, smart_alife_task) are the
// Lua objects themselves, alive in their tables while the brain copies them (no Lua runs in between)
CALifeSmartTerrainTask* SquadTask(CSE_ALifeOnlineOfflineGroup& squad, std::optional<CALifeSmartTerrainTask>& storage)
{
    SquadTaskEngineEnv env;
    Answer<CALifeSmartTerrainTask> answer;
    const ESource source = Decide(env, static_cast<CSE_ALifeDynamicObject&>(squad), answer);
    if (npc_cpp_profile::enabled())
        npc_cpp_profile::count(kSquadTaskSourceStages[static_cast<size_t>(source)]);
    if (!IsNative(source))
        return squad.get_current_task();
    CALifeSmartTerrainTask* task = answer.task;
    if (!task)
    {
        storage.emplace(answer.game_vertex_id, answer.level_vertex_id);
        task = &*storage;
    }
    return task;
}
} // namespace gw_squad_task

CALifeOnlineOfflineGroupBrain::CALifeOnlineOfflineGroupBrain(object_type* object)
{
    VERIFY(object);
    m_object = object;

    m_movement_manager = xr_new<CALifeMonsterMovementManager>(object);
}

CALifeOnlineOfflineGroupBrain::~CALifeOnlineOfflineGroupBrain()
{
    xr_delete(m_movement_manager);
}

void CALifeOnlineOfflineGroupBrain::on_state_write(NET_Packet& packet) {}
void CALifeOnlineOfflineGroupBrain::on_state_read(NET_Packet& packet) {}

void CALifeOnlineOfflineGroupBrain::on_register() {}
void CALifeOnlineOfflineGroupBrain::on_unregister() {}
void CALifeOnlineOfflineGroupBrain::on_location_change() {}
void CALifeOnlineOfflineGroupBrain::update()
{
    // 35-gw-eoc-remaining.md, E11-v (diagnostics only, -npc_cpp_profile): the whole update and the task (since 57 E4
    // the native get_current_task with the Lua fallbacks for the rare cases) by the class of the commander.
    // Without the profiler: one check. The parts of the movement: alife/offline_path/* (alife_monster_detail_path_manager)
    const bool profile = npc_cpp_profile::enabled();
    const u64 profile_start = profile ? CPU::QPC() : 0;

    std::optional<CALifeSmartTerrainTask> task_storage;
    CALifeSmartTerrainTask* const task = gw_squad_task::SquadTask(object(), task_storage);

    if (profile)
    {
        const u64 task_delta = CPU::QPC() - profile_start;
        // the commander after the call, by its ID from the registry (not the stored pointer: a Lua call could have
        // changed the squad): the class is that of the squad's members, stalkers or monsters; a squad left empty is
        // counted with the monsters
        const auto& members = object().squad_members();
        CSE_ALifeDynamicObject* commander =
            members.empty() ? nullptr : ai().alife().objects().object(members.begin()->first, true);
        const bool stalkers = commander && commander->cast_human_abstract();
        npc_cpp_profile::add(stalkers ? ENpcCppProfileStage::AlifeSquadTaskStalker :
                                        ENpcCppProfileStage::AlifeSquadTaskMonster, task_delta);
    }

    THROW2(task, "CALifeOnlineOfflineGroupBrain returned nil task, while npc is registered in it");
    movement().path_type(MovementManager::ePathTypeGamePath);
    movement().detail().target(*task);
    movement().update();

    if (profile)
        npc_cpp_profile::add(ENpcCppProfileStage::AlifeSquadBrainUpdate, CPU::QPC() - profile_start);
}

void CALifeOnlineOfflineGroupBrain::update_position() { movement().update_position(); }

void CALifeOnlineOfflineGroupBrain::on_switch_online() { movement().on_switch_online(); }
void CALifeOnlineOfflineGroupBrain::on_switch_offline() { movement().on_switch_offline(); }
