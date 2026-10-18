////////////////////////////////////////////////////////////////////////////
//	Module 		: space_restriction.h
//	Created 	: 17.08.2004
//  Modified 	: 27.08.2004
//	Author		: Dmitriy Iassenev
//	Description : Space restriction
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "restriction_space.h"
#include "space_restriction_holder.h"
#include "space_restriction_bridge.h"
#include "space_restriction_abstract.h"
#include "xrCommon/xr_unordered_map.h"

//#define USE_FREE_IN_RESTRICTIONS

class CSpaceRestrictionManager;

// The cover scan cache (space_restriction.cpp): a space restrictor moved, appeared or went - the answers of accessible
// kept by every CSpaceRestriction are dropped
void space_restriction_shapes_changed();

class CSpaceRestriction : public RestrictionSpace::CTimeIntrusiveBase, public CSpaceRestrictionAbstract
{
private:
    typedef CSpaceRestrictionAbstract inherited;

    friend struct CRemoveMergedFreeInRestrictions;
    friend class LevelGraphDebugRender;

private:
    typedef SpaceRestrictionHolder::CBaseRestrictionPtr CBaseRestrictionPtr;

private:
    struct CFreeInRestriction
    {
        CBaseRestrictionPtr m_restriction;
        bool m_enabled;

        CFreeInRestriction(CBaseRestrictionPtr restriction, bool enabled)
            : m_restriction(restriction), m_enabled(enabled) {}
    };

private:
    typedef xr_vector<CBaseRestrictionPtr> RESTRICTIONS;
    typedef xr_vector<CFreeInRestriction> FREE_IN_RESTRICTIONS;

protected:
    bool m_applied;
    shared_str m_out_restrictions;
    shared_str m_in_restrictions;
    xr_vector<u32> m_temp;
    CSpaceRestrictionManager* m_space_restriction_manager;
    CBaseRestrictionPtr m_out_space_restriction;
    CBaseRestrictionPtr m_in_space_restriction;
#ifdef USE_FREE_IN_RESTRICTIONS
    FREE_IN_RESTRICTIONS m_free_in_restrictions;
#endif

private:
    // The cover scan cache: the answers of accessible(sphere) by the exact sphere, valid until the shapes change, for
    // the same bridges; filled and read on the game logic thread only
    struct accessible_key
    {
        u32 x, y, z, r;
        bool operator==(const accessible_key& other) const
        {
            return x == other.x && y == other.y && z == other.z && r == other.r;
        }
    };
    struct accessible_key_hash
    {
        size_t operator()(const accessible_key& key) const
        {
            u64 h = key.x;
            h = h * 0x9E3779B97F4A7C15ull + key.y;
            h = h * 0x9E3779B97F4A7C15ull + key.z;
            h = h * 0x9E3779B97F4A7C15ull + key.r;
            return size_t(h ^ (h >> 29));
        }
    };
    xr_unordered_map<accessible_key, bool, accessible_key_hash> m_accessible_cache;
    u32 m_accessible_cache_gen = u32(-1);
    const void* m_accessible_cache_out = nullptr;
    const void* m_accessible_cache_in = nullptr;

    bool accessible_uncached(const Fsphere& sphere);

private:
    IC bool intersects(CBaseRestrictionPtr bridge);
    IC bool intersects(
        SpaceRestrictionHolder::CBaseRestrictionPtr bridge0, SpaceRestrictionHolder::CBaseRestrictionPtr bridge1);
    CBaseRestrictionPtr merge(CBaseRestrictionPtr bridge, const RESTRICTIONS& temp_restrictions) const;
    void merge_in_out_restrictions();
    void merge_free_in_retrictions();

protected:
    IC bool initialized() const;
    bool affect(CBaseRestrictionPtr bridge, const Fsphere& sphere) const;
    bool affect(CBaseRestrictionPtr bridge, u32 start_vertex_id, float radius) const;
    bool affect(CBaseRestrictionPtr bridge, const Fvector& start_position, const Fvector& dest_position) const;
    bool affect(CBaseRestrictionPtr bridge, u32 start_vertex_id, u32 dest_vertex_id) const;

public:
    CSpaceRestriction(
        CSpaceRestrictionManager* space_restriction_manager, shared_str out_restrictions, shared_str in_restrictions);
    void initialize();
    void remove_border();
    template <typename T1, typename T2>
    IC void add_border(T1 p1, T2 p2);
    u32 accessible_nearest(const Fvector& position, Fvector& result);
    bool accessible(const Fsphere& sphere);
    bool accessible(u32 level_vertex_id, float radius);
    IC shared_str out_restrictions() const;
    IC shared_str in_restrictions() const;
    IC bool applied() const;
    IC bool inside(const Fsphere& sphere);
    IC bool inside(u32 level_vertex_id, bool partially_inside);
    virtual shared_str name() const;
};

#include "space_restriction_inline.h"
