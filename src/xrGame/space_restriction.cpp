////////////////////////////////////////////////////////////////////////////
//	Module 		: space_restriction.cpp
//	Created 	: 17.08.2004
//  Modified 	: 27.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Space restriction
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "space_restriction.h"
#include "space_restriction_manager.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "space_restriction_base.h"
#include "xrEngine/profiler.h"
#include "xrCore/xrDebug_macros.h"
#include "xrCore/buffer_vector.h"
#include "xrEngine/EngineThreading.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <atomic>
#include <cstring>

// 26-framemove-game-next.md, the cover scan cache (mode 2 always on since 2026-10-18, its switch npc_cover_scan_cache
// removed, 41-cvar-cleanup.md); see agent_location_manager.h

namespace
{
// bumped by space_restriction_shapes_changed (CSpaceRestrictor::spatial_move, CSpaceRestrictionHolder register and
// unregister of a restrictor) from whatever thread does it; the caches compare it on the game logic thread
std::atomic<u32> s_space_restriction_shapes_gen{0};
constexpr size_t space_restriction_accessible_cache_max = 8192;

u32 space_restriction_float_bits(float value)
{
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
} // namespace

void space_restriction_shapes_changed() { s_space_restriction_shapes_gen.fetch_add(1, std::memory_order_relaxed); }

//const float dependent_distance = 100.f;

template <bool a>
struct CMergeInOutPredicate
{
    SpaceRestrictionHolder::CBaseRestrictionPtr m_out;
    SpaceRestrictionHolder::CBaseRestrictionPtr m_in;

    IC CMergeInOutPredicate(
        SpaceRestrictionHolder::CBaseRestrictionPtr out, SpaceRestrictionHolder::CBaseRestrictionPtr in)
    {
        m_out = out;
        m_in = in;
    }

    IC bool operator()(u32 level_vertex_id) const
    {
        if (!m_out || !m_in)
            return (false);
        return (a ? m_in->inside(level_vertex_id, false) : !m_out->inside(level_vertex_id, true));
    }
};

struct CRemoveMergedFreeInRestrictions
{
    CSpaceRestriction::RESTRICTIONS* m_restrictions;

    IC CRemoveMergedFreeInRestrictions(CSpaceRestriction::RESTRICTIONS& restrictions)
    {
        m_restrictions = &restrictions;
    }

    IC bool operator()(const CSpaceRestriction::CFreeInRestriction& free_in_restriction) const
    {
        return (std::find(m_restrictions->begin(), m_restrictions->end(), free_in_restriction.m_restriction) !=
            m_restrictions->end());
    }
};

bool CSpaceRestriction::accessible(const Fsphere& sphere)
{
    if (!initialized())
    {
        initialize();
        if (!initialized())
            return (true);
    }

    if (!XRay::Engine::IsGameLogicThread())
        return (accessible_uncached(sphere));

    // The answer depends on the sphere, the level graph (static) and the shapes behind the two bridges: kept until a
    // restrictor moves, appears or goes, and only for the same bridges
    const u32 gen = s_space_restriction_shapes_gen.load(std::memory_order_relaxed);
    const void* out = m_out_space_restriction.get();
    const void* in = m_in_space_restriction.get();
    if (m_accessible_cache_gen != gen || m_accessible_cache_out != out || m_accessible_cache_in != in ||
        m_accessible_cache.size() >= space_restriction_accessible_cache_max)
    {
        m_accessible_cache.clear();
        m_accessible_cache_gen = gen;
        m_accessible_cache_out = out;
        m_accessible_cache_in = in;
    }

    const accessible_key key{space_restriction_float_bits(sphere.P.x), space_restriction_float_bits(sphere.P.y),
        space_restriction_float_bits(sphere.P.z), space_restriction_float_bits(sphere.R)};
    const auto found = m_accessible_cache.find(key);
    if (found != m_accessible_cache.end())
    {
        PERF_INC(ai_cover_accessible_hit);
        return (found->second);
    }

    PERF_INC(ai_cover_accessible_miss);
    const bool result = accessible_uncached(sphere);
    m_accessible_cache.emplace(key, result);
    return (result);
}

bool CSpaceRestriction::accessible_uncached(const Fsphere& sphere)
{
    return (ai().level_graph().valid_vertex_position(sphere.P) &&
        (m_out_space_restriction ?
                (m_out_space_restriction->inside(sphere) && !m_out_space_restriction->on_border(sphere.P) &&
                    !m_out_space_restriction->out_of_border(sphere.P)) :
                true) &&
        (m_in_space_restriction ?
                (!m_in_space_restriction->inside(sphere) && !m_in_space_restriction->on_border(sphere.P)) :
                true));
}

bool CSpaceRestriction::accessible(u32 level_vertex_id, float radius)
{
    if (!initialized())
    {
        initialize();
        if (!initialized())
            return (true);
    }

    return ((m_out_space_restriction ? m_out_space_restriction->inside(level_vertex_id, false, radius) : true) &&
        (m_in_space_restriction ? !m_in_space_restriction->inside(level_vertex_id, true, radius) : true));
}

IC bool CSpaceRestriction::intersects(
    SpaceRestrictionHolder::CBaseRestrictionPtr bridge0, SpaceRestrictionHolder::CBaseRestrictionPtr bridge1)
{
    xr_vector<u32>::const_iterator I = bridge1->border().begin();
    xr_vector<u32>::const_iterator E = bridge1->border().end();
    for (; I != E; ++I)
        if (bridge0->inside(*I, true))
            return (true);

    if (!bridge0->border().empty() && bridge1->inside(bridge0->border().front(), true))
        return (true);

    m_temp.resize(bridge0->border().size() + bridge1->border().size());
    xr_vector<u32>::iterator J = std::set_intersection(bridge0->border().begin(), bridge0->border().end(),
        bridge1->border().begin(), bridge1->border().end(), m_temp.begin());
    return (J != m_temp.begin());
}

IC bool CSpaceRestriction::intersects(SpaceRestrictionHolder::CBaseRestrictionPtr bridge)
{
    if (!m_out_space_restriction)
        return (false);

    return (intersects(m_out_space_restriction, bridge));
}

void CSpaceRestriction::merge_in_out_restrictions()
{
    START_PROFILE("Restricted Object/Merge In-Out");
    m_border = m_out_space_restriction->border();
    m_border.erase(std::remove_if(m_border.begin(), m_border.end(),
                       CMergeInOutPredicate<true>(m_out_space_restriction, m_in_space_restriction)),
        m_border.end());

    if (m_in_space_restriction)
    {
        buffer_vector<u32> temp_border(xr_alloca(m_in_space_restriction->border().size() * sizeof(u32)),
            m_in_space_restriction->border().size(), m_in_space_restriction->border().begin(),
            m_in_space_restriction->border().end());
        temp_border.erase(std::remove_if(temp_border.begin(), temp_border.end(),
                              CMergeInOutPredicate<false>(m_out_space_restriction, m_in_space_restriction)),
            temp_border.end());
        m_border.insert(m_border.end(), temp_border.begin(), temp_border.end());
    }

    std::sort(m_border.begin(), m_border.end());
    m_border.erase(std::unique(m_border.begin(), m_border.end()), m_border.end());
    STOP_PROFILE;
}

CSpaceRestriction::CBaseRestrictionPtr CSpaceRestriction::merge(
    CBaseRestrictionPtr bridge, const RESTRICTIONS& temp_restrictions) const
{
    u32 acc_length = xr_strlen(bridge->name().c_str()) + 1;
    {
        RESTRICTIONS::const_iterator I = temp_restrictions.begin();
        RESTRICTIONS::const_iterator E = temp_restrictions.end();
        for (; I != E; ++I)
            acc_length += xr_strlen((*I)->name().c_str()) + 1;
    }

    pstr S = xr_alloc<char>(acc_length);
    S[0] = 0;
    shared_str temp = bridge->name();
    RESTRICTIONS::const_iterator I = temp_restrictions.begin();
    RESTRICTIONS::const_iterator E = temp_restrictions.end();
    for (; I != E; ++I)
        temp = strconcat(acc_length, S, temp.c_str(), ",", (*I)->name().c_str());

    xr_free(S);

    return (m_space_restriction_manager->restriction(temp));
}

#ifdef USE_FREE_IN_RESTRICTIONS
void CSpaceRestriction::merge_free_in_retrictions()
{
    START_PROFILE("Restricted Object/Merge Free In");
    string256 temp;
    for (u32 i = 0, n = _GetItemCount(*m_in_restrictions); i < n; ++i)
    {
        SpaceRestrictionHolder::CBaseRestrictionPtr bridge =
            m_space_restriction_manager->restriction(shared_str(_GetItem(*m_in_restrictions, i, temp)));
        m_free_in_restrictions.push_back(CFreeInRestriction(bridge, false));
    }

    RESTRICTIONS temp_restrictions;
    for (bool ok = false; !ok;)
    {
        ok = true;
        temp_restrictions.clear();

        FREE_IN_RESTRICTIONS::iterator I = m_free_in_restrictions.begin(), J;
        FREE_IN_RESTRICTIONS::iterator E = m_free_in_restrictions.end();
        for (; I != E; ++I)
        {
            for (J = I + 1; J != E; ++J)
                if (intersects((*I).m_restriction, (*J).m_restriction))
                    temp_restrictions.push_back((*J).m_restriction);

            if (!temp_restrictions.empty())
            {
                J = remove_if(m_free_in_restrictions.begin(), m_free_in_restrictions.end(),
                    CRemoveMergedFreeInRestrictions(temp_restrictions));
                m_free_in_restrictions.erase(J, m_free_in_restrictions.end());
                (*I).m_restriction = merge((*I).m_restriction, temp_restrictions);
                ok = false;
                break;
            }
        }
    }
    STOP_PROFILE;
}
#endif

void CSpaceRestriction::initialize()
{
    VERIFY(!m_initialized);
    m_out_space_restriction = m_space_restriction_manager->restriction(m_out_restrictions);
    m_in_space_restriction = m_space_restriction_manager->restriction(m_in_restrictions);

    if (!m_out_space_restriction && !m_in_space_restriction)
    {
        m_initialized = true;
        return;
    }

    if (m_out_space_restriction && !m_out_space_restriction->initialized())
        m_out_space_restriction->initialize();

#ifdef DEBUG
    if (m_out_space_restriction)
    {
        if (!m_out_space_restriction->object().correct())
        {
            Msg("~ BAD out restrictions combination :");
            Msg("~ %s", m_out_space_restriction->name().c_str());
        }
    }
#endif

    if (m_in_space_restriction && !m_in_space_restriction->initialized())
        m_in_space_restriction->initialize();

    if ((m_out_space_restriction && !m_out_space_restriction->initialized()) ||
        (m_in_space_restriction && !m_in_space_restriction->initialized()))
        return;

    if (m_out_space_restriction)
        merge_in_out_restrictions();
#ifdef USE_FREE_IN_RESTRICTIONS
    else
        merge_free_in_retrictions();
#endif

#ifdef DEBUG
    if (!m_out_space_restriction)
        m_border = m_in_space_restriction->border();
#endif

    m_initialized = true;
}

void CSpaceRestriction::remove_border()
{
    if (!initialized())
        return;

    VERIFY(m_applied);

    m_applied = false;

    if (m_out_space_restriction)
    {
        ai().level_graph().clear_mask(border());
        return;
    }

#ifdef USE_FREE_IN_RESTRICTIONS
    FREE_IN_RESTRICTIONS::iterator I = m_free_in_restrictions.begin();
    FREE_IN_RESTRICTIONS::iterator E = m_free_in_restrictions.end();
    for (; I != E; ++I)
        if ((*I).m_enabled)
        {
            VERIFY((*I).m_restriction);
            (*I).m_enabled = false;
            ai().level_graph().clear_mask((*I).m_restriction->border());
        }
#else
    ai().level_graph().clear_mask(m_in_space_restriction->border());
#endif
}

u32 CSpaceRestriction::accessible_nearest(const Fvector& position, Fvector& result)
{
    if (m_out_space_restriction)
    {
#pragma TODO("Xottab_DUTY: investigate temporary fix!")
        CSpaceRestriction* mutable_this = const_cast<CSpaceRestriction*> (this); // Xottab_DUTY: temporary fix to allow compilation. Thanks to Giperion
        return (m_out_space_restriction->accessible_nearest(mutable_this, position, result, true));
    }

    VERIFY(m_in_space_restriction);
    return (m_in_space_restriction->accessible_nearest(m_in_space_restriction, position, result, false));
}

bool CSpaceRestriction::affect(SpaceRestrictionHolder::CBaseRestrictionPtr bridge, const Fsphere& sphere) const
{
    if (bridge->inside(sphere))
        return (false);

    return (true);

    // if (bridge->inside(start_position))
    //	return						(false);
    // Fvector							position;
    // bridge->accessible_nearest		(start_position,position,false);
    // return							(start_position.distance_to(position) <= radius + dependent_distance);
}

bool CSpaceRestriction::affect(
    SpaceRestrictionHolder::CBaseRestrictionPtr bridge, u32 start_vertex_id, float radius) const
{
    Fsphere sphere;
    sphere.P = ai().level_graph().vertex_position(start_vertex_id);
    sphere.R = radius;
    return (affect(bridge, sphere));
}

bool CSpaceRestriction::affect(SpaceRestrictionHolder::CBaseRestrictionPtr bridge, const Fvector& start_position,
    const Fvector& dest_position) const
{
    Fsphere sphere0, sphere1;
    sphere0.P = start_position;
    sphere0.R = 0.f;
    sphere1.P = dest_position;
    sphere1.R = 0.f;
    return (affect(bridge, sphere0) || affect(bridge, sphere1));
}

bool CSpaceRestriction::affect(
    SpaceRestrictionHolder::CBaseRestrictionPtr bridge, u32 start_vertex_id, u32 dest_vertex_id) const
{
    return (affect(bridge, start_vertex_id, 0.f) || affect(bridge, dest_vertex_id, 0.f));
}

shared_str CSpaceRestriction::name() const { return (m_out_restrictions); }
