////////////////////////////////////////////////////////////////////////////
//	Module 		: abstract_path_manager.h
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Abstract path manager inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <type_traits>

#include "ai_space.h"
#include "xrAICore/Navigation/graph_engine.h"
#include "xrAICore/Navigation/ai_graph_engine_cvars.h"
#include "xrCore/xrDebug_macros.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/PathManagers/path_manager_params.h"
#include "xrAICore/Navigation/PathManagers/path_manager_params_game_vertex.h"

#include "CustomMonster.h"
#include "xrCore/log.h"
#include "xrCore/xr_types.h"

#define TEMPLATE_SPECIALIZATION \
    template <typename _Graph, typename _VertexEvaluator, typename _vertex_id_type, typename _index_type>

#define CPathManagerTemplate CAbstractPathManager<_Graph, _VertexEvaluator, _vertex_id_type, _index_type>

TEMPLATE_SPECIALIZATION
IC CPathManagerTemplate::CAbstractPathManager(CRestrictedObject* object) : m_object(object) {}

TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::reinit(const _Graph* graph)
{
    m_actuality = false;
    m_failed = false;
    m_evaluator = 0;
    m_graph = graph;
    m_current_index = _index_type(-1);
    m_intermediate_index = _index_type(-1);
    m_dest_vertex_id = _index_type(-1);
    m_path.clear();
    m_failed_start_vertex_id = _vertex_id_type(-1);
    m_failed_dest_vertex_id = _vertex_id_type(-1);
}

template <>
IC void CAbstractPathManager<CGameGraph, SGameVertex<float, u32, u32>, u32, u32>::build_path(
    const u32 start_vertex_id, const u32 dest_vertex_id)
{
    VERIFY(m_graph && m_evaluator && m_graph->valid_vertex_id(start_vertex_id) &&
        m_graph->valid_vertex_id(dest_vertex_id));

    if ((m_failed_start_vertex_id == start_vertex_id) && (m_failed_dest_vertex_id == dest_vertex_id))
    {
        before_search(start_vertex_id, dest_vertex_id);
        m_failed = true;
        after_search();
        m_current_index = u32(-1);
        m_intermediate_index = u32(-1);
        m_actuality = !failed();
        return;
    }

    before_search(start_vertex_id, dest_vertex_id);
    if (ps_ai_path_build_use_tls_scratch)
        m_failed = !m_graph->Search(start_vertex_id, dest_vertex_id, m_path, m_evaluator->m_vertex_types,
            m_evaluator->max_range, m_evaluator->max_iteration_count, m_evaluator->max_visited_node_count);
    else
        m_failed = !ai().graph_engine().search(*m_graph, start_vertex_id, dest_vertex_id, &m_path, *m_evaluator);
    after_search();

    m_current_index = u32(-1);
    m_intermediate_index = u32(-1);
    m_actuality = !failed();

    if (!m_failed)
        return;

    m_failed_start_vertex_id = start_vertex_id;
    m_failed_dest_vertex_id = dest_vertex_id;
}

template <>
IC void CAbstractPathManager<CLevelGraph, SBaseParameters<float, u32, u32>, u32, u32>::build_path(
    const u32 start_vertex_id, const u32 dest_vertex_id)
{
    VERIFY(m_graph && m_evaluator && m_graph->valid_vertex_id(start_vertex_id) &&
        m_graph->valid_vertex_id(dest_vertex_id));

    if ((m_failed_start_vertex_id == start_vertex_id) && (m_failed_dest_vertex_id == dest_vertex_id))
    {
        before_search(start_vertex_id, dest_vertex_id);
        m_failed = true;
        after_search();
        m_current_index = u32(-1);
        m_intermediate_index = u32(-1);
        m_actuality = !failed();
        return;
    }

    before_search(start_vertex_id, dest_vertex_id);
    if (ps_ai_path_build_use_tls_scratch)
        m_failed = !m_graph->Search(start_vertex_id, dest_vertex_id, m_path, m_evaluator->max_range,
            m_evaluator->max_iteration_count, m_evaluator->max_visited_node_count);
    else
        m_failed = !ai().graph_engine().search(*m_graph, start_vertex_id, dest_vertex_id, &m_path, *m_evaluator);
    after_search();

    m_current_index = u32(-1);
    m_intermediate_index = u32(-1);
    m_actuality = !failed();

    if (!m_failed)
        return;

    m_failed_start_vertex_id = start_vertex_id;
    m_failed_dest_vertex_id = dest_vertex_id;
}

TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::build_path(const _vertex_id_type start_vertex_id, const _vertex_id_type dest_vertex_id)
{
    VERIFY(m_graph && m_evaluator && m_graph->valid_vertex_id(start_vertex_id) &&
        m_graph->valid_vertex_id(dest_vertex_id));

    if ((m_failed_start_vertex_id == start_vertex_id) && (m_failed_dest_vertex_id == dest_vertex_id))
    {
        before_search(start_vertex_id, dest_vertex_id);
        m_failed = true;
        after_search();
        m_current_index = _index_type(-1);
        m_intermediate_index = _index_type(-1);
        m_actuality = !failed();
        return;
    }

    before_search(start_vertex_id, dest_vertex_id);
    m_failed = !ai().graph_engine().search(*m_graph, start_vertex_id, dest_vertex_id, &m_path, *m_evaluator);
    after_search();
    m_current_index = _index_type(-1);
    m_intermediate_index = _index_type(-1);
    m_actuality = !failed();

    if (!m_failed)
        return;

    m_failed_start_vertex_id = start_vertex_id;
    m_failed_dest_vertex_id = dest_vertex_id;
}

TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::select_intermediate_vertex()
{
    VERIFY(!failed() && !m_path.empty());
    m_intermediate_index = m_path.size() - 1;
}

TEMPLATE_SPECIALIZATION
IC _vertex_id_type CPathManagerTemplate::intermediate_vertex_id() const
{
    VERIFY(m_intermediate_index < m_path.size());
    return (m_path[intermediate_index()]);
}

TEMPLATE_SPECIALIZATION
IC u32 CPathManagerTemplate::intermediate_index() const { return (m_intermediate_index); }
TEMPLATE_SPECIALIZATION
IC bool CPathManagerTemplate::actual(
    const _vertex_id_type /*start_vertex_id*/, const _vertex_id_type /*dest_vertex_id*/) const
{
    return (m_actuality);
}

TEMPLATE_SPECIALIZATION
IC bool CPathManagerTemplate::completed() const { return (m_intermediate_index == m_path.size() - 1); }
TEMPLATE_SPECIALIZATION
IC bool CPathManagerTemplate::failed() const { return (m_failed); }
TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::set_evaluator(_VertexEvaluator* evaluator)
{
    if ((evaluator != m_evaluator) || !m_evaluator->actual())
        m_actuality = false;
    m_evaluator = evaluator;
}

TEMPLATE_SPECIALIZATION
IC const typename CPathManagerTemplate::PATH& CPathManagerTemplate::path() const { return (m_path); }
TEMPLATE_SPECIALIZATION
IC _vertex_id_type CPathManagerTemplate::dest_vertex_id() const { return (m_dest_vertex_id); }
TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::set_dest_vertex(const _vertex_id_type vertex_id)
{
    if (!m_graph)
    {
        m_actuality = false;
        return;
    }
    if (!check_vertex(vertex_id))
    {
        const CLevelGraph* ai_level = ai().get_level_graph();
        const CGameGraph* ai_game = ai().get_game_graph();

        const void* self_graph = static_cast<const void*>(m_graph);
        const bool ptr_is_level = (self_graph == static_cast<const void*>(ai_level));
        const bool ptr_is_game = (self_graph == static_cast<const void*>(ai_game));
        const bool in_range = m_graph->valid_vertex_id(vertex_id);

        int accessible_info = -1;
        if (ptr_is_level && m_object)
            accessible_info = m_object->accessible(static_cast<u32>(vertex_id)) ? 1 : 0;

        string256 object_info{};
        if (m_object)
        {
            const CCustomMonster& M = m_object->object();
            const Fvector& pos = M.Position();
            xr_sprintf(object_info,
                "object=\"%s\" pos=(%.2f,%.2f,%.2f) actor_level_vertex=%u actor_game_vertex=%u",
                M.cName().c_str(), pos.x, pos.y, pos.z, M.ai_location().level_vertex_id(),
                static_cast<u32>(M.ai_location().game_vertex_id()));
        }
        else
            xr_strcpy(object_info, "object=<null>");

        string512 graph_info{};
        if (ptr_is_level && ai_level)
        {
            xr_sprintf(graph_info,
                "LEVEL graph: level_id=%u vertex_count=%u requested_vertex=%llu graph_valid_vertex_id=%d "
                "restriction_accessible=%d",
                ai_level->level_id(), ai_level->header().vertex_count(),
                static_cast<unsigned long long>(vertex_id), in_range ? 1 : 0, accessible_info);
        }
        else if (ptr_is_game && ai_game)
        {
            xr_sprintf(graph_info,
                "GAME graph: vertex_count=%u requested_vertex=%llu graph_valid_vertex_id=%d",
                ai_game->header().vertex_count(), static_cast<unsigned long long>(vertex_id), in_range ? 1 : 0);
        }
        else
        {
            xr_sprintf(graph_info,
                "Graph instance is not ai().get_level_graph() / get_game_graph(); graph_valid_vertex_id=%d",
                in_range ? 1 : 0);
        }

        string256 nearest_info{};
        nearest_info[0] = 0;
        if (ai_level && m_object)
        {
            const u32 vid = ai_level->vertex_id(m_object->object().Position());
            xr_sprintf(nearest_info, "nearest_vertex_from_actor_pos=%u nearest_valid=%d", vid,
                ai_level->valid_vertex_id(vid) ? 1 : 0);
        }

        string512 cause{};
        cause[0] = 0;
        if (ptr_is_level && in_range && accessible_info == 0)
            xr_strcpy(cause,
                "\nLikely cause: vertex is in range for this level graph but blocked by restriction volumes "
                "(smart cover / borders).");
        else if (!in_range)
            xr_strcpy(cause,
                "\nLikely cause: stale or wrong vertex id for this graph (wrong level, reloaded AI data, "
                "cross-table mismatch, script passing bad node id).");

        string256 path_scope{};
        if (std::is_same<_Graph, CLevelGraph>::value)
            xr_strcpy(path_scope,
                "path_scope=level_path (movement().level_path(); CAbstractPathManager<CLevelGraph,...>)");
        else if (std::is_same<_Graph, CGameGraph>::value)
            xr_strcpy(path_scope,
                "path_scope=game_path (movement().game_path(); CAbstractPathManager<CGameGraph,...>)");
        else
            xr_strcpy(path_scope, "path_scope=unknown (nonstandard _Graph in CAbstractPathManager)");

        string512 path_cache{};
        if (m_path.empty())
        {
            xr_strcpy(path_cache,
                "cached_route(m_path): empty (no stored vertex sequence from last successful build_path)");
        }
        else
        {
            size_t idx_found = size_t(-1);
            for (size_t i = 0; i < m_path.size(); ++i)
            {
                if (m_path[i] == vertex_id)
                {
                    idx_found = i;
                    break;
                }
            }
            const unsigned long long first_v = static_cast<unsigned long long>(m_path.front());
            const unsigned long long last_v = static_cast<unsigned long long>(m_path.back());
            if (idx_found != size_t(-1))
            {
                xr_sprintf(path_cache,
                    "cached_route(m_path): requested_vertex FOUND at route_index %zu (path_len=%zu) "
                    "(last built path; first_vertex=%llu last_vertex=%llu)",
                    idx_found, m_path.size(), first_v, last_v);
            }
            else
            {
                xr_sprintf(path_cache,
                    "cached_route(m_path): requested_vertex NOT in stored path len=%zu "
                    "(first_vertex=%llu last_vertex=%llu; dest being set is outside last route cache)",
                    m_path.size(), first_v, last_v);
            }
        }

        string2048 diag{};
        xr_sprintf(diag,
            "AbstractPathManager::set_dest_vertex: check_vertex failed.\n"
            "%s\n"
            "%s\n"
            "%s\n"
            "%s\n"
            "previous_dest_vertex=%llu stored_path_vertices=%zu m_failed=%d cached_failed_start=%llu "
            "cached_failed_dest=%llu\n"
            "%s%s",
            path_scope, graph_info, path_cache, object_info,
            static_cast<unsigned long long>(m_dest_vertex_id), m_path.size(),
            m_failed ? 1 : 0, static_cast<unsigned long long>(m_failed_start_vertex_id),
            static_cast<unsigned long long>(m_failed_dest_vertex_id), nearest_info, cause);

        Msg("! %s", diag);
        R_ASSERT2(false, diag);
    }
    m_actuality = m_actuality && (dest_vertex_id() == vertex_id);
    m_dest_vertex_id = vertex_id;
}

TEMPLATE_SPECIALIZATION
IC const _VertexEvaluator* CPathManagerTemplate::evaluator() const { return (m_evaluator); }
TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::make_inactual() { m_actuality = false; }
TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::before_search(const _vertex_id_type start_vertex_id, const _vertex_id_type dest_vertex_id)
{
}

TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::after_search() {}
TEMPLATE_SPECIALIZATION
IC bool CPathManagerTemplate::check_vertex(const _vertex_id_type vertex_id) const
{
    return (m_graph && m_graph->valid_vertex_id(vertex_id));
}

TEMPLATE_SPECIALIZATION
IC CRestrictedObject& CPathManagerTemplate::object() const
{
    VERIFY(m_object);
    return (*m_object);
}

TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::reset() { m_failed = false; }
TEMPLATE_SPECIALIZATION
IC void CPathManagerTemplate::invalidate_failed_info()
{
    reset();
    m_failed_start_vertex_id = u32(-1);
    m_failed_dest_vertex_id = u32(-1);
}

#undef CPathManagerTemplate
#undef TEMPLATE_SPECIALIZATION
