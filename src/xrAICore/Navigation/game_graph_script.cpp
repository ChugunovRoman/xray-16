////////////////////////////////////////////////////////////////////////////
//	Module 		: game_graph_script.cpp
//	Created 	: 02.11.2005
//  Modified 	: 02.11.2005
//	Author		: Dmitriy Iassenev
//	Description : Game graph class script export
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include "game_graph.h"
#include "AISpaceBase.hpp"

#include "xrScriptEngine/script_space.hpp"
#include "xrScriptEngine/script_wrapper_cache.hpp"

const CGameGraph* get_game_graph() { return &GEnv.AISpace->game_graph(); }

// The Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp): game_graph() gives one wrapper per Lua state.
// game_graph() returns a pointer to
// const: luabind passes it through const_pointer_converter, which asks get_back_reference with T = CGameGraph const*
// (a const wrapper, as before). Only the current game graph (AISpaceBase::get_game_graph()) is cached: it is created
// with the spawn registry of each game (load / new game) and deleted with it - the slot remembers the graph and
// replaces the wrapper of the old one (a new graph at the old address: the same class and raw pointer, valid). A Lua
// script that keeps the old wrapper across games keeps a dangling one, as before. CGameGraph has no luabind::wrap_base
// class, the generic get_back_reference never finds anything for it. Only this file passes the graph to Lua (xrAICore;
// xrGame has its own instantiation of the generic one and passes no graph): declared and defined here.
static xray::script_wrapper_cache::ObjectSlot s_game_graph_lua_wrapper;

namespace luabind
{
template <>
bool get_back_reference<CGameGraph const*>(lua_State* L, CGameGraph const* const& x)
{
    using namespace xray::script_wrapper_cache;
    if (!GEnv.AISpace || x != GEnv.AISpace->get_game_graph())
        return false;
    if (Push(L, s_game_graph_lua_wrapper, x))
        return true;
    luabind::detail::make_pointer_instance(L, x);
    Store(L, s_game_graph_lua_wrapper);
    return true;
}
} // namespace luabind
const CGameGraph::CHeader* get_header(const CGameGraph* self_) { return (&self_->header()); }
bool get_accessible1(const CGameGraph* self_, const u32& vertex_id) { return (self_->accessible(vertex_id)); }
void get_accessible2(const CGameGraph* self_, const u32& vertex_id, bool value) { self_->accessible(vertex_id, value); }
Fvector CVertex__level_point(const CGameGraph::CGameVertex* vertex)
{
    THROW(vertex);
    return (vertex->level_point());
}

Fvector CVertex__game_point(const CGameGraph::CGameVertex* vertex)
{
    THROW(vertex);
    return (vertex->game_point());
}

GameGraph::LEVEL_MAP const& get_levels(CGameGraph const* graph)
{
    THROW(graph);
    return graph->header().levels();
}

void CGameGraph::script_register(lua_State* luaState)
{
    using namespace luabind;
    using namespace luabind::policy;

    module(luaState)
    [
        class_<GameGraph::LEVEL_MAP::value_type>("GameGraph__LEVEL_MAP__value_type")
            .def_readonly("id", &GameGraph::LEVEL_MAP::value_type::first)
            .def_readonly("level", &GameGraph::LEVEL_MAP::value_type::second),

        def("game_graph", &get_game_graph),

        class_<CGameGraph>("CGameGraph")
            .def("accessible", &get_accessible1)
            .def("accessible", &get_accessible2)
            .def("valid_vertex_id", &CGameGraph::valid_vertex_id)
            .def("vertex", &CGameGraph::vertex)
            .def("vertex_id", &CGameGraph::vertex_id)
            .def("levels", &get_levels, return_stl_iterator()),

        class_<CGameVertex>("GameGraph__CVertex")
            .def("level_point", &CVertex__level_point)
            .def("game_point", &CVertex__game_point)
            .def("level_id", &CGameVertex::level_id)
            .def("level_vertex_id", &CGameVertex::level_vertex_id),

        def("gg_vertex_level_id", +[](u32 vertex_id)
        {
            return GEnv.AISpace->game_graph().vertex(vertex_id)->level_id();
        }),
        def("gg_level_id", +[](_LEVEL_ID idx)
        {
            const GameGraph::LEVEL_MAP& levels = GEnv.AISpace->game_graph().header().levels();
            return (levels.begin() + idx)->second.id();
        }),
        def("gg_levels_count", +[]()
        {
            return GEnv.AISpace->game_graph().header().level_count();
        }),
        def("gg_distance", +[](u32 vid1, u32 vid2)
        {
            const auto game_graph = GEnv.AISpace->game_graph();
            const auto p1 = game_graph.vertex(vid1)->game_point();
            const auto p2 = game_graph.vertex(vid2)->game_point();
            return p1.distance_to(p2);
        })
    ];
}
