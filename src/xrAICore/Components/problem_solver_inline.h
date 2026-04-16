////////////////////////////////////////////////////////////////////////////
//	Module 		: problem_solver_inline.h
//	Created 	: 24.02.2004
//  Modified 	: 24.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Problem solver inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#ifndef AI_COMPILER
#include "xrAICore/Components/ai_planner_search_limits.h"
#include "xrAICore/Navigation/graph_engine.h"
#include "xrAICore/Navigation/graph_engine_space.h"
#include "xrCore/xrDebug_macros.h"
#include <algorithm>
#include <iterator>
#endif

#define TEMPLATE_SPECIALIZATION                                                                                  \
    template <typename _operator_condition, typename _operator, typename _condition_state,                       \
        typename _condition_evaluator, typename _operator_id_type, bool _reverse_search, typename _operator_ptr, \
        typename _condition_evaluator_ptr\
>

#define CProblemSolverAbstract                                                                                \
    CProblemSolver<_operator_condition, _operator, _condition_state, _condition_evaluator, _operator_id_type, \
        _reverse_search, _operator_ptr, _condition_evaluator_ptr>

TEMPLATE_SPECIALIZATION
IC CProblemSolverAbstract::CProblemSolver() { init(); }
TEMPLATE_SPECIALIZATION
CProblemSolverAbstract::~CProblemSolver() { clear(); }
TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::clear()
{
    while (!m_operators.empty())
        remove_operator(m_operators.back().m_operator_id);

    while (!m_evaluators.empty())
        remove_evaluator((*(m_evaluators.end() - 1)).first);
}

TEMPLATE_SPECIALIZATION
void CProblemSolverAbstract::init() {}
TEMPLATE_SPECIALIZATION
void CProblemSolverAbstract::setup()
{
    m_target_state.clear();
    m_current_state.clear();
    m_temp.clear();
    m_solution.clear();
    m_applied = false;
    m_solution_changed = false;
    m_actuality = true;
    m_failed = false;
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::actual() const
{
    if (!m_actuality)
        return (false);

    typename xr_vector<_operator_condition>::const_iterator I = current_state().conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator E = current_state().conditions().end();
    typename EVALUATORS::const_iterator i = evaluators().begin();
    typename EVALUATORS::const_iterator e = evaluators().end();
    for (; I != E; ++I)
    {
        if ((*i).first < (*I).condition())
            i = std::lower_bound(i, e, (*I).condition(), evaluators().value_comp());
        VERIFY(i != e);
        VERIFY((*i).first == (*I).condition());
        if ((*i).second->evaluate() != (*I).value())
            return (false);
    }
    return (true);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::add_operator(const _operator_id_type& operator_id, _operator_ptr _op)
{
    typename OPERATOR_VECTOR::iterator I = std::lower_bound(m_operators.begin(), m_operators.end(), operator_id);
    THROW((I == m_operators.end()) || ((*I).m_operator_id != operator_id));
#ifdef DEBUG
    validate_properties(_op->conditions());
    validate_properties(_op->effects());
#endif
    m_actuality = false;
    m_operators.emplace(I, operator_id, _op);
}

#ifdef DEBUG
TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::validate_properties(const CState& conditions) const
{
    for (const auto& cond : conditions.conditions())
    {
        if (evaluators().find(cond.condition()) == evaluators().end())
        {
            Msg("! cannot find corresponding evaluator to the property with id %d", cond.condition());
            VERIFY(evaluators().find(cond.condition()) != evaluators().end());
        }
    }
}
#endif

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::remove_operator(const _operator_id_type& operator_id)
{
    typename OPERATOR_VECTOR::iterator I = std::lower_bound(m_operators.begin(), m_operators.end(), operator_id);
    THROW(m_operators.end() != I);
    try
    {
        delete_data((*I).m_operator);
    }
    catch (...)
    {
        (*I).m_operator = 0;
    }
    m_actuality = false;
    m_operators.erase(I);
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::OPERATOR_VECTOR& CProblemSolverAbstract::operators() const
{
    return (m_operators);
}

// states
TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::set_target_state(const CState& state)
{
    m_actuality = m_actuality && (m_target_state == state);
    m_target_state = state;
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::CState& CProblemSolverAbstract::current_state() const
{
    return (m_current_state);
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::CState& CProblemSolverAbstract::target_state() const
{
    return (m_target_state);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::add_evaluator(const condition_type& condition_id, _condition_evaluator_ptr evaluator)
{
    THROW(evaluators().end() == evaluators().find(condition_id));
    m_evaluators.insert(std::make_pair(condition_id, evaluator));
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::remove_evaluator(const condition_type& condition_id)
{
    typename EVALUATORS::iterator I = m_evaluators.find(condition_id);
    THROW(I != m_evaluators.end());
    try
    {
        delete_data((*I).second);
    }
    catch (...)
    {
        (*I).second = 0;
    }
    m_evaluators.erase(I);
    m_actuality = false;
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::condition_evaluator_ptr_type CProblemSolverAbstract::evaluator(
    const condition_type& condition_id) const
{
    typename EVALUATORS::const_iterator I = evaluators().find(condition_id);
    THROW(evaluators().end() != I);
    return ((*I).second);
}

TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::EVALUATORS& CProblemSolverAbstract::evaluators() const
{
    return (m_evaluators);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::evaluate_condition(typename xr_vector<_operator_condition>::const_iterator& I,
    typename xr_vector<_operator_condition>::const_iterator& E, const condition_type& condition_id) const
{
    size_t index = I - m_current_state.conditions().begin();
    m_current_state.add_condition(I, _operator_condition(condition_id, evaluator(condition_id)->evaluate()));
    I = m_current_state.conditions().begin() + index;
    E = m_current_state.conditions().end();
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::edge_value_type CProblemSolverAbstract::get_edge_weight(
    const _index_type& vertex_index0, const _index_type& vertex_index1, const const_iterator& i) const
{
    edge_value_type current, min;
    current = (*i).m_operator->weight(vertex_index1, vertex_index0);
    min = (*i).m_operator->min_weight();
    THROW(current >= min);
    return (current);
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_accessible(const _index_type& vertex_index) const { return (m_applied); }
TEMPLATE_SPECIALIZATION
IC const typename CProblemSolverAbstract::_index_type& CProblemSolverAbstract::value(
    const _index_type& vertex_index, const_iterator& i, bool reverse_search) const
{
    if (reverse_search)
    {
        if ((*i).m_operator->applicable_reverse(
                (*i).m_operator->effects(), (*i).m_operator->conditions(), vertex_index))
            m_applied = (*i).m_operator->apply_reverse(
                vertex_index, (*i).m_operator->effects(), m_temp, (*i).m_operator->conditions());
        else
            m_applied = false;
    }
    else
    {
        if ((*i).m_operator->applicable(vertex_index, current_state(), (*i).m_operator->conditions(), *this))
        {
            (*i).m_operator->apply(vertex_index, (*i).m_operator->effects(), m_temp, m_current_state, *this);
            m_applied = true;
        }
        else
            m_applied = false;
    }
    return (m_temp);
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::begin(const _index_type& vertex_index, const_iterator& b, const_iterator& e) const
{
    b = m_operators.begin();
    e = m_operators.end();
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_goal_reached(const _index_type& vertex_index) const
{
    return (is_goal_reached_impl<reverse_search>(vertex_index));
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_goal_reached_impl(const _index_type& vertex_index) const
{
    static_assert(!reverse_search, "This function cannot be used in the REVERSE search.");
    typename xr_vector<_operator_condition>::const_iterator I = vertex_index.conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator E = vertex_index.conditions().end();
    typename xr_vector<_operator_condition>::const_iterator i = target_state().conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator e = target_state().conditions().end();

    {
        typename xr_vector<_operator_condition>::const_iterator II = current_state().conditions().begin();
        typename xr_vector<_operator_condition>::const_iterator EE = current_state().conditions().end();
        for (; (i != e) && (I != E);)
        {
            if ((*I).condition() < (*i).condition())
            {
                ++I;
            }
            else if ((*I).condition() > (*i).condition())
            {
                for (; (II != EE) && ((*II).condition() < (*i).condition());)
                    ++II;
                if ((II == EE) || ((*II).condition() > (*i).condition()))
                    evaluate_condition(II, EE, (*i).condition());
                if ((*II).value() != (*i).value())
                    return (false);
                ++II;
                ++i;
            }
            else
            {
                if ((*I).value() != (*i).value())
                    return (false);
                ++I;
                ++i;
            }
        }

        if (I == E)
        {
            I = std::move(II);
            E = std::move(EE);
        }
        else
            return (true);
    }

    for (; i != e;)
    {
        if ((I == E) || ((*I).condition() > (*i).condition()))
            evaluate_condition(I, E, (*i).condition());

        if ((*I).condition() < (*i).condition())
            ++I;
        else
        {
            VERIFY((*I).condition() == (*i).condition());
            if ((*I).value() != (*i).value())
                return (false);
            ++I;
            ++i;
        }
    }
    return (true);
}

TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::is_goal_reached_impl(const _index_type& vertex_index, bool) const
{
    static_assert(reverse_search, "This function cannot be used in the STRAIGHT search.");
    typename xr_vector<_operator_condition>::const_iterator I = m_current_state.conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator E = m_current_state.conditions().end();
    typename xr_vector<_operator_condition>::const_iterator i = vertex_index.conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator e = vertex_index.conditions().end();
    for (; i != e;)
    {
        if ((I == E) || ((*I).condition() > (*i).condition()))
            evaluate_condition(I, E, (*i).condition());

        if ((*I).condition() < (*i).condition())
            ++I;
        else
        {
            if ((*I).value() != (*i).value())
                return (false);
            ++I;
            ++i;
        }
    }
    return (true);
}

TEMPLATE_SPECIALIZATION
IC const xr_vector<_operator_id_type>& CProblemSolverAbstract::solution() const
{
    return (m_solution);
}

TEMPLATE_SPECIALIZATION
IC _operator_ptr CProblemSolverAbstract::get_operator(const _operator_id_type& operator_id)
{
    typename OPERATOR_VECTOR::iterator I = std::lower_bound(m_operators.begin(), m_operators.end(), operator_id);
    if (m_operators.end() == I)
        return m_operators.at(0).get_operator();
    R_ASSERT2(m_operators.end() != I, make_string("m_operators.size=[%d], operator_id=[%d]", m_operators.size(), operator_id));
    return ((*I).get_operator());
}

TEMPLATE_SPECIALIZATION
IC void CProblemSolverAbstract::solve()
{
#ifndef AI_COMPILER
    m_solution_changed = false;

    if (actual())
        return;

    m_actuality = true;
    m_solution_changed = true;
    m_current_state.clear();

    ZoneScopedN("ai/problem_solver/search");
    m_failed = !Search(reverse_search ? target_state() : current_state(),
        reverse_search ? current_state() : target_state(), m_solution, GraphEngineSpace::_solver_dist_type(-1), u32(-1),
        g_ai_nested_planner_graph_search_max_nodes);
#endif
}

#ifndef AI_COMPILER
TEMPLATE_SPECIALIZATION
IC bool CProblemSolverAbstract::Search(const CState& FromID, const CState& dest_vertex_id,
    xr_vector<_operator_id_type>& OutPath, GraphEngineSpace::_solver_dist_type max_range, u32 max_iteration_count,
    u32 max_visited_node_count) const
{
    auto IsAccessible = [this](const CState& VertexID)
    {
        if (!is_accessible(VertexID))
            return false;
        return true;
    };

    auto CalcCost = [this](const CState& Node1, const CState& Node2, const_iterator i)
    {
        return get_edge_weight(Node1, Node2, i);
    };

    auto DistanceNode = [this](const CState& Node1) { return estimate_edge_weight(Node1); };

    // Stack-local search state: thread_local + clear() at entry breaks nested solve()->Search() on the same
    // thread (sub-planner re-enters while parent Search is running), corrupting the outer walk and emptying plans.
    xr_vector<std::pair<GraphEngineSpace::_solver_dist_type, CState>> TempPriorityNode;
    xr_map<CState, CState> TempCameFrom;
    xr_map<CState, GraphEngineSpace::_solver_dist_type> TempCostSoFar;
    xr_map<CState, _operator_id_type> TempEdges;
    xr_vector<_operator_id_type> TempRebuildForward;
    xr_vector<CState> TempRebuildSeen;

    OutPath.clear();

    // `FromID` is commonly `current_state()` — mutable. Evaluators/actions refill it during Search, so keeping a
    // const ref to the caller's state makes rebuild checks (`while (NextNode != FromID)`, self_parent vs start)
    // compare against the *live* world state instead of the search start snapshot → bogus self_parent / empty plans.
    const CState start_snapshot = FromID;

    TempPriorityNode.push_back({0, start_snapshot});
    TempCameFrom.insert({start_snapshot, start_snapshot});
    TempCostSoFar.insert({start_snapshot, 0});
    TempEdges.insert({start_snapshot, _operator_id_type()});

    while (!TempPriorityNode.empty())
    {
        const CState CurrentNodeID = TempPriorityNode.back().second;
        TempPriorityNode.pop_back();

        const bool goal = is_goal_reached(CurrentNodeID);
        if (goal)
        {
            const size_t max_hops = std::max<size_t>(size_t(256), TempCameFrom.size() * 2 + 32);

            CState NextNode = CurrentNodeID;
            u64 rb = 0;
            TempRebuildSeen.clear();
            const auto is_state_equivalent = [](const CState& lhs, const CState& rhs)
            {
                return !(lhs < rhs) && !(rhs < lhs);
            };

            // Never use map[key] here: std::map::operator[] inserts a default entry on miss and corrupts the search
            // graph, which previously produced an infinite parent walk; with a hop cap that becomes constant failure
            // and NPCs lose all plans.
            if (reverse_search)
            {
                while (!is_state_equivalent(NextNode, start_snapshot))
                {
                    ++rb;
                    if (rb > max_hops)
                    {
                        OutPath.clear();
                        return false;
                    }
                    bool seen = false;
                    for (const CState& s : TempRebuildSeen)
                    {
                        if (s == NextNode)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (seen)
                    {
                        OutPath.clear();
                        return false;
                    }
                    TempRebuildSeen.push_back(NextNode);

                    const auto ie = TempEdges.find(NextNode);
                    const auto ic = TempCameFrom.find(NextNode);
                    if (ie == TempEdges.end() || ic == TempCameFrom.end())
                    {
                        OutPath.clear();
                        return false;
                    }
                    const CState parent = ic->second;
                    if (is_state_equivalent(parent, NextNode) &&
                        !is_state_equivalent(NextNode, start_snapshot))
                    {
                        OutPath.clear();
                        return false;
                    }
                    OutPath.push_back(ie->second);
                    NextNode = parent;
                }
            }
            else
            {
                TempRebuildForward.clear();
                if (max_hops < size_t(16384))
                    TempRebuildForward.reserve(max_hops);
                while (!is_state_equivalent(NextNode, start_snapshot))
                {
                    ++rb;
                    if (rb > max_hops)
                    {
                        OutPath.clear();
                        return false;
                    }
                    bool seen = false;
                    for (const CState& s : TempRebuildSeen)
                    {
                        if (s == NextNode)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (seen)
                    {
                        OutPath.clear();
                        return false;
                    }
                    TempRebuildSeen.push_back(NextNode);

                    const auto ie = TempEdges.find(NextNode);
                    const auto ic = TempCameFrom.find(NextNode);
                    if (ie == TempEdges.end() || ic == TempCameFrom.end())
                    {
                        OutPath.clear();
                        return false;
                    }
                    const CState parent = ic->second;
                    if (is_state_equivalent(parent, NextNode) &&
                        !is_state_equivalent(NextNode, start_snapshot))
                    {
                        OutPath.clear();
                        return false;
                    }
                    TempRebuildForward.push_back(ie->second);
                    NextNode = parent;
                }
                std::reverse(TempRebuildForward.begin(), TempRebuildForward.end());
                OutPath.assign(TempRebuildForward.begin(), TempRebuildForward.end());
            }

            return true;
        }

        const_iterator i, e;
        begin(CurrentNodeID, i, e);

        for (; i != e; i++)
        {
            const CState NeighborID = value(CurrentNodeID, i, reverse_search);
            if (!IsAccessible(NeighborID))
                continue;
            // Same abstract vertex for std::map: strict-weak equivalence is !(a<b)&&!(b<a). For CWorldState
            // (CConditionState) that can differ from operator== (hash vs lexicographic <), so find(NeighborID)
            // may alias CurrentNodeID's slot and set came_from to a self-parent (goal_self_parent_r0).
            if (!(NeighborID < CurrentNodeID) && !(CurrentNodeID < NeighborID))
                continue;
            // Never change the start key's parent away from itself (neighbor may map-alias start_snapshot).
            const bool neighbor_equiv_from = (!(NeighborID < start_snapshot) && !(start_snapshot < NeighborID));
            const bool current_equiv_from = (!(CurrentNodeID < start_snapshot) && !(start_snapshot < CurrentNodeID));
            if (neighbor_equiv_from && !current_equiv_from)
                continue;

            if (max_iteration_count == 0)
                continue;
            max_iteration_count--;

            const auto cost_here = TempCostSoFar.find(CurrentNodeID);
            VERIFY(cost_here != TempCostSoFar.end());
            const GraphEngineSpace::_solver_dist_type NewCost =
                cost_here->second + CalcCost(CurrentNodeID, NeighborID, i);
            auto TempCostSoFarIterator = TempCostSoFar.find(NeighborID);
            if ((TempCostSoFarIterator != TempCostSoFar.end() && TempCostSoFarIterator->second > NewCost) ||
                (TempCostSoFarIterator == TempCostSoFar.end() && max_visited_node_count > TempCostSoFar.size()))
            {
                const GraphEngineSpace::_solver_dist_type Distance = DistanceNode(NeighborID);
                if (Distance > max_range)
                    continue;

                if (TempCostSoFarIterator != TempCostSoFar.end())
                    TempCostSoFarIterator->second = NewCost;
                else
                    TempCostSoFar.insert({NeighborID, NewCost});

                const GraphEngineSpace::_solver_dist_type priority = NewCost + Distance;
                TempPriorityNode.insert(
                    std::upper_bound(TempPriorityNode.begin(), TempPriorityNode.end(),
                        std::pair<GraphEngineSpace::_solver_dist_type, CState>{priority, NeighborID},
                        [](const std::pair<GraphEngineSpace::_solver_dist_type, CState>& Left,
                            const std::pair<GraphEngineSpace::_solver_dist_type, CState>& Right)
                        { return Left.first > Right.first; }),
                    {priority, NeighborID});

                auto TempCameFromIterator = TempCameFrom.find(NeighborID);
                auto TempEdgesIterator = TempEdges.find(NeighborID);
                if (TempCameFromIterator != TempCameFrom.end())
                {
                    TempCameFromIterator->second = CurrentNodeID;
                    if (TempEdgesIterator != TempEdges.end())
                        TempEdgesIterator->second = i->m_operator_id;
                    else
                        TempEdges.insert({NeighborID, i->m_operator_id});
                }
                else
                {
                    TempCameFrom.insert({NeighborID, CurrentNodeID});
                    TempEdges.insert({NeighborID, i->m_operator_id});
                }
            }
        }
    }
    return false;
}
#endif // !AI_COMPILER

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::edge_value_type CProblemSolverAbstract::estimate_edge_weight(
    const _index_type& condition) const
{
    return (helper::template estimate_edge_weight_impl<reverse_search>(*this, condition));
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::edge_value_type CProblemSolverAbstract::estimate_edge_weight_impl(
    const _index_type& condition) const
{
    static_assert(!reverse_search, "This function cannot be used in the REVERSE search.");
    edge_value_type result = 0;
    typename xr_vector<_operator_condition>::const_iterator I = target_state().conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator E = target_state().conditions().end();
    typename xr_vector<_operator_condition>::const_iterator i = condition.conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator e = condition.conditions().end();
    for (; (I != E) && (i != e);)
        if ((*I).condition() < (*i).condition())
        {
            ++result;
            ++I;
        }
        else if ((*I).condition() > (*i).condition())
            ++i;
        else
        {
            if ((*I).value() != (*i).value())
                ++result;
            ++I;
            ++i;
        }
    return (result + edge_value_type(E - I));
}

TEMPLATE_SPECIALIZATION
IC typename CProblemSolverAbstract::edge_value_type CProblemSolverAbstract::estimate_edge_weight_impl(
    const _index_type& condition, bool) const
{
    static_assert(reverse_search, "This function cannot be used in the STRAIGHT search.");
    edge_value_type result = 0;
    typename xr_vector<_operator_condition>::const_iterator I = current_state().conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator E = current_state().conditions().end();
    typename xr_vector<_operator_condition>::const_iterator i = condition.conditions().begin();
    typename xr_vector<_operator_condition>::const_iterator e = condition.conditions().end();
    for (; (i != e);)
    {
        if ((I == E) || ((*I).condition() > (*i).condition()))
            evaluate_condition(I, E, (*i).condition());

        if ((*I).condition() < (*i).condition())
            ++I;
        else
        {
            VERIFY((*I).condition() == (*i).condition());
            if ((*I).value() != (*i).value())
                ++result;
            ++I;
            ++i;
        }
    }
    return (result);
}

#undef TEMPLATE_SPECIALIZATION
#undef CProblemSolverAbstract
