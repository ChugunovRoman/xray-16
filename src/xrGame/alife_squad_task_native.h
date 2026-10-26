#pragma once

// plans/lua_to_cpp/57-gw-eoc-next-steps.md, E4: the task of an offline squad without the Lua call.
//
// CALifeOnlineOfflineGroupBrain::update asks the virtual get_current_task of the squad every update: a luabind call of
// sim_squad_scripted:get_current_task (sim_squad_scripted.script), which reads only Lua fields and the server objects:
//
//   local se_target = self.assigned_target_id and alife_object(self.assigned_target_id)
//   if se_target then
//     if se_target:clsid() == clsid.smart_terrain then
//       local npc_info = se_target.npc_info and se_target.npc_info[self:commander_id()]
//       if npc_info and npc_info.job then
//         return npc_info.job.alife_task or se_target:get_alife_task() or self:get_alife_task()
//       end
//     end
//     if se_target.get_alife_task then return se_target:get_alife_task() end
//   end
//   return self:get_alife_task()
//
// with se_smart_terrain:get_alife_task() = self.smart_alife_task (smart_terrain.script), sim_squad_scripted:
// get_alife_task() = CALifeSmartTerrainTask(self.m_game_vertex_id, self.m_level_vertex_id) and se_actor:get_alife_task()
// (se_actor.script, db.storage[0].loud_gvid / loud_lvid or the vertices of the actor). Decide below reads the same Lua
// fields with the Lua C API (lua_gettable: the same lookup as the dot of Lua, the class metatables of luabind included)
// and answers the same task:
//   - Job:   the commander has a job at the target smart: job.alife_task (the Lua object itself);
//   - Smart: the target smart without such a job: smart.smart_alife_task (the Lua object itself);
//   - Squad: the target is a squad: a task at its game / level vertex;
//   - Self:  no target (nil, false, a released id), or a job without alife_task and a smart without smart_alife_task:
//            a task at the game / level vertex of the squad itself.
// The rest goes to the Lua function as before (the caller calls the virtual get_current_task):
//   - Actor:      the target is the actor (hunters, companions; se_actor:get_alife_task reads db.storage);
//   - OtherClass: the target is neither a smart terrain, a squad nor the actor (a class whose get_alife_task the
//                 engine does not know);
//   - Odd:        a value the Lua function would not take quietly: assigned_target_id not a number, not an integer
//                 or out of [0, 65535) (alife_object writes an error), npc_info / its entry / job not a table, an
//                 alife_task / smart_alife_task that is no CALifeSmartTerrainTask, a smart answering nil (the Lua
//                 answers nil and the engine stops on it - the same stop through the Lua);
//   - NoScript:   the squad (or its target smart) has no Lua instance.
// The dependency: get_alife_task of se_smart_terrain / sim_squad_scripted and the code above as they are now (the
// stand scratchpad e4_task compares the Lua function text taken from the script with this code on generated worlds).
//
// Pure Lua C API logic: the environment (the engine - alife_online_offline_group_brain.cpp, the stand - a mock with
// luabind classes) gives the objects:
//   typename Env::Object, Env::Task;
//   lua_State* Env::PushSelf(Object&)  - pushes the Lua instance (one value) or pushes nothing and answers nullptr;
//   Object* Env::Find(std::uint16_t id) - alife():object(id) for an id in [0, 65535);
//   EClass Env::Class(Object&); std::uint16_t Env::GraphId(Object&); std::uint32_t Env::NodeId(Object&);
//   std::uint16_t Env::CommanderId(Object& squad);
//   Task* Env::CastTask(lua_State*, int index) - the CALifeSmartTerrainTask of a Lua value or nullptr.

#include <cmath>
#include <cstdint>
#include <luabind/lua_include.hpp>

namespace gw_squad_task
{
enum class EClass : std::uint8_t
{
    Smart, // "SMRTTRRN", se_smart_terrain (clsid.smart_terrain)
    Squad, // "ON_OFF_S", sim_squad_scripted
    Actor, // "S_ACTOR", se_actor
    Other,
};

enum class ESource : std::uint8_t
{
    Job,
    Smart,
    Squad,
    Self,
    Actor,
    OtherClass,
    Odd,
    NoScript,
};

inline bool IsNative(const ESource source) { return source <= ESource::Self; }

template <class Task>
struct Answer
{
    Task* task = nullptr; // Job, Smart: the Lua object
    std::uint16_t game_vertex_id = 0; // Squad, Self: the vertices of the new task
    std::uint32_t level_vertex_id = 0;
};

namespace detail
{
// The value on the top is a Lua table (npc_info, its entry, job): the Lua function indexes it with the dot
inline bool SquadTaskIsTable(lua_State* L) { return lua_type(L, -1) == LUA_TTABLE; }

// The task of the target smart when the job has none: smart.smart_alife_task (se_smart_terrain:get_alife_task).
// `or_self`: the "or self:get_alife_task()" of the job branch (a smart without the task gives the task of the squad);
// without it a smart answering nil is the stop of the engine - left to the Lua
template <class Env>
ESource SquadTaskOfSmart(Env& env, lua_State* L, const int smart_index, typename Env::Object& squad,
    Answer<typename Env::Task>& out, const bool or_self)
{
    lua_pushstring(L, "smart_alife_task");
    lua_gettable(L, smart_index);
    if (!lua_toboolean(L, -1))
    {
        if (!or_self)
            return ESource::Odd;
        out.game_vertex_id = env.GraphId(squad);
        out.level_vertex_id = env.NodeId(squad);
        return ESource::Self;
    }
    out.task = env.CastTask(L, -1);
    return out.task ? ESource::Smart : ESource::Odd;
}

// The squad instance is on the top of the stack (index `self`)
template <class Env>
ESource SquadTaskWithSelf(Env& env, lua_State* L, typename Env::Object& squad, Answer<typename Env::Task>& out)
{
    const int self = lua_gettop(L);
    lua_pushstring(L, "assigned_target_id");
    lua_gettable(L, self);
    if (!lua_toboolean(L, -1))
    {
        out.game_vertex_id = env.GraphId(squad);
        out.level_vertex_id = env.NodeId(squad);
        return ESource::Self;
    }
    // alife_object (_g.script): an error line for anything but a number in [0, MAX_COUNT_ID_SIZE); a fraction is cut by
    // luabind - all left to the Lua
    if (lua_type(L, -1) != LUA_TNUMBER)
        return ESource::Odd;
    const double id = lua_tonumber(L, -1);
    if (!(id >= 0.0 && id < 65535.0) || std::floor(id) != id)
        return ESource::Odd;

    typename Env::Object* target = env.Find(static_cast<std::uint16_t>(id));
    if (!target)
    {
        out.game_vertex_id = env.GraphId(squad); // alife():object of a released id: nil - the task of the squad
        out.level_vertex_id = env.NodeId(squad);
        return ESource::Self;
    }
    switch (env.Class(*target))
    {
    case EClass::Squad:
        out.game_vertex_id = env.GraphId(*target); // sim_squad_scripted:get_alife_task of the target
        out.level_vertex_id = env.NodeId(*target);
        return ESource::Squad;
    case EClass::Actor: return ESource::Actor;
    case EClass::Other: return ESource::OtherClass;
    case EClass::Smart: break;
    }

    lua_State* const target_state = env.PushSelf(*target);
    if (target_state != L)
    {
        if (target_state)
            lua_pop(target_state, 1); // another state (one script engine: cannot happen) - the Lua decides
        return ESource::NoScript;
    }
    const int smart = lua_gettop(L);
    lua_pushstring(L, "npc_info");
    lua_gettable(L, smart);
    if (lua_toboolean(L, -1))
    {
        if (!SquadTaskIsTable(L))
            return ESource::Odd;
        lua_pushnumber(L, static_cast<lua_Number>(env.CommanderId(squad))); // 65535 for a squad without members
        lua_gettable(L, -2);
        if (lua_toboolean(L, -1))
        {
            if (!SquadTaskIsTable(L))
                return ESource::Odd;
            lua_pushstring(L, "job");
            lua_gettable(L, -2);
            if (lua_toboolean(L, -1))
            {
                if (!SquadTaskIsTable(L))
                    return ESource::Odd;
                lua_pushstring(L, "alife_task");
                lua_gettable(L, -2);
                if (lua_toboolean(L, -1))
                {
                    out.task = env.CastTask(L, -1);
                    return out.task ? ESource::Job : ESource::Odd;
                }
                // job.alife_task or se_target:get_alife_task() or self:get_alife_task()
                return SquadTaskOfSmart(env, L, smart, squad, out, true);
            }
        }
    }
    // if se_target.get_alife_task then return se_target:get_alife_task() end
    return SquadTaskOfSmart(env, L, smart, squad, out, false);
}
} // namespace detail

// The task of the squad as sim_squad_scripted:get_current_task answers it, without calling it. A native source
// (IsNative) fills `out`: `task` (Job, Smart) or the vertices (Squad, Self); any other source - ask the Lua. The Lua stack
// is as it was after the call
template <class Env>
ESource Decide(Env& env, typename Env::Object& squad, Answer<typename Env::Task>& out)
{
    out = Answer<typename Env::Task>{};
    lua_State* L = env.PushSelf(squad);
    if (!L)
        return ESource::NoScript;
    const int top = lua_gettop(L) - 1; // below the instance
    const ESource source = detail::SquadTaskWithSelf(env, L, squad, out);
    lua_settop(L, top);
    if (!IsNative(source))
        out = Answer<typename Env::Task>{};
    return source;
}
} // namespace gw_squad_task
