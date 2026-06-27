////////////////////////////////////////////////////////////////////////////
//	Module 		: cover_manager_inline.h
//	Created 	: 24.03.2004
//  Modified 	: 24.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Cover manager class inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <algorithm>

#include "performance_cvars.h"
#include <tracy/Tracy.hpp>

IC CCoverManager::CPointQuadTree& CCoverManager::covers() const
{
    VERIFY(m_covers);
    return (*m_covers);
}

IC CCoverManager::CPointQuadTree* CCoverManager::get_covers() { return (m_covers); }
IC smart_cover::storage* CCoverManager::smart_covers_storage() const { return m_smart_covers_storage; }
template <typename _evaluator_type, typename _restrictor_type>
IC bool CCoverManager::inertia(
    Fvector const& position, float radius, _evaluator_type& evaluator, const _restrictor_type& restrictor) const
{
    // check if evaluator has no inertion or it's time to reevaluate
    if (!evaluator.inertia(position, radius))
        return (false);

    // so, evaluator has inertion and it's not time to search
    // check if we didn't select cover last time
    if (!evaluator.selected())
        return (true);

    // so, evaluator has inertion and it's not time to search
    // so, evaluator did select cover last time
    // check if this cover is still accessible
    if (!evaluator.accessible(evaluator.selected()->position()))
        return (false);

    // so, evaluator has inertion and it's not time to search
    // so, evaluator did select cover last time
    // so, cover is still accessible
    // check if restrictor still allows this cover
    if (!restrictor(evaluator.selected()))
        return (false);

    // so, evaluator has inertion and it's not time to search
    // so, evaluator did select cover last time
    // so, cover is still accessible
    // so, restrictor still allows this cover
    // therefore inertion is played
    return (true);
}

template <typename _evaluator_type, typename _restrictor_type>
IC const CCoverPoint* CCoverManager::best_cover(
    const Fvector& position, float radius, _evaluator_type& evaluator, const _restrictor_type& restrictor) const
{
    ZoneNamedN(___tracy_cover_bc, "cover_mgr/best_cover", true);
    ZoneTextVF(___tracy_cover_bc, "r=%.0f", double(radius));
    START_PROFILE("Covers/best_cover")

    if (inertia(position, radius, evaluator, restrictor))
    {
        ZoneNamedN(___tracy_cover_bc_inert, "cover_mgr/best_cover/inertia", true);
        return (evaluator.selected());
    }

    const CCoverPoint* last = evaluator.selected();

    evaluator.initialize(position);

    if (last)
    {
        if (position.distance_to_sqr(last->position()) < _sqr(3 * radius))
        {
            if (evaluator.accessible(last->position()))
                if (restrictor(last))
                    evaluator.evaluate(last, restrictor.weight(last));
        }
    }

    {
        ZoneNamedN(___tracy_cover_bc_near, "cover_mgr/best_cover/nearest", true);
        covers().nearest(position, radius, m_nearest);
    }

    const int nearest_cap = npc_perf_cover_nearest_max_points;
    if (nearest_cap > 0 && m_nearest.size() > static_cast<size_t>(nearest_cap))
    {
        std::partial_sort(m_nearest.begin(), m_nearest.begin() + nearest_cap, m_nearest.end(),
            [&position](CCoverPoint* a, CCoverPoint* b)
            { return position.distance_to_sqr(a->position()) < position.distance_to_sqr(b->position()); });
        m_nearest.resize(nearest_cap);
    }

    const int eval_cap = npc_perf_cover_best_max_evaluate;
    if (eval_cap > 0 && m_nearest.size() > 1)
    {
        std::sort(m_nearest.begin(), m_nearest.end(),
            [&position](CCoverPoint* a, CCoverPoint* b)
            { return position.distance_to_sqr(a->position()) < position.distance_to_sqr(b->position()); });
    }

    float radius_sqr = _sqr(radius);

    xr_vector<CCoverPoint*>::const_iterator I = m_nearest.begin();
    xr_vector<CCoverPoint*>::const_iterator E = m_nearest.end();
    {
        ZoneNamedN(___tracy_cover_bc_scan, "cover_mgr/best_cover/scan_eval", true);
        int eval_done = 0;
        const int acc_cap = npc_perf_cover_best_max_accessible;
        int acc_done = 0;
        for (; I != E; ++I)
        {
            if (radius_sqr < position.distance_to_sqr((*I)->position()))
                continue;

            if (_abs(position.y - (*I)->position().y) > 3.f)
                continue;

            if (acc_cap > 0)
            {
                if (acc_done >= acc_cap)
                    break;
                ++acc_done;
            }
            if (!evaluator.accessible((*I)->position()))
                continue;

            if (!restrictor(*I))
                continue;

            evaluator.evaluate(*I, restrictor.weight(*I));
            ++eval_done;
            if (eval_cap > 0 && eval_done >= eval_cap)
                break;
            if (g_npc_perf_cover_find_eval_budget_remaining != u32(-1))
            {
                --g_npc_perf_cover_find_eval_budget_remaining;
                if (g_npc_perf_cover_find_eval_budget_remaining == 0)
                    break;
            }
        }
        const int find_left = (g_npc_perf_cover_find_eval_budget_remaining == u32(-1))
            ? -1
            : (int)g_npc_perf_cover_find_eval_budget_remaining;
        ZoneTextVF(___tracy_cover_bc_scan, "n=%u acc=%u lim=%d ev_cap=%u eval=%d find_left=%d", (unsigned)m_nearest.size(),
            (unsigned)acc_done, acc_cap > 0 ? (int)acc_cap : -1, eval_cap, (int)eval_done, find_left);
    }

    evaluator.finalize();

    restrictor.finalize(evaluator.selected());

    return (evaluator.selected());

    STOP_PROFILE
}

template <typename _evaluator_type>
IC const CCoverPoint* CCoverManager::best_cover(const Fvector& position, float radius, _evaluator_type& evaluator) const
{
    return (best_cover<_evaluator_type, CCoverManager>(position, radius, evaluator, *this));
}

IC bool CCoverManager::operator()(const CCoverPoint*) const { return (true); }
IC float CCoverManager::weight(const CCoverPoint*) const { return (1.f); }
IC void CCoverManager::finalize(const CCoverPoint*) const {}
