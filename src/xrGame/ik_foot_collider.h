#pragma once
#include "ik_collide_data.h"
#include "xrCDB/xr_collide_defs.h"
class CGameObject;

class ik_pick_query
{
public:
    ik_pick_query()
        : _pos({ -FLT_MAX, -FLT_MAX, -FLT_MAX }),
          _dir({ -FLT_MAX, -FLT_MAX, -FLT_MAX }),
          _range(-FLT_MAX), _point(ik_foot_geom::none) {}

    ik_pick_query(ik_foot_geom::e_collide_point point, const Fvector& pos, const Fvector& dir, float range)
        : _pos(pos), _dir(dir), _range(range), _point(point)
    {
        VERIFY(is_valid());
    }

    bool is_valid() const
    {
#ifdef DEBUG
        if (point() != ik_foot_geom::none)
        {
            VERIFY(range() >= 0.f);
            VERIFY(fsimilar(dir().magnitude(), 1.f));
            return true;
        }
        return false;
#else
        return point() != ik_foot_geom::none;
#endif
    }

    IC bool is_equal(const ik_pick_query& q) const
    {
        VERIFY(q.is_valid());
        // VERIFY( is_valid() );
        return is_valid() && q.point() == point() && fsimilar(q.range(), range()) && q.pos().similar(pos()) &&
            q.dir().similar(dir());
    }

    IC const Fvector& pos() const { return _pos; }
    IC const Fvector& dir() const { return _dir; }
    IC float range() const { return _range; }
    IC ik_foot_geom::e_collide_point point() const { return _point; }
private:
    Fvector _pos;
    Fvector _dir;
    float _range;
    ik_foot_geom::e_collide_point _point;
};

class ik_foot_collider
{
    ik_pick_query previous_toe_query;
    ik_pick_query previous_heel_query;
    ik_pick_query previous_side_query;

    SIKCollideData previous_data;

public:
    ik_foot_collider();

    /** Legacy serial/per-limb path. */
    void collide(SIKCollideData& cld, const ik_foot_geom& foot_geom, CGameObject* O, bool foot_step);

    /** Two-phase path for per-NPC batching.
     *  Phase 1: check cache; returns true if cld is already valid.
     *  Phase 1b: build_queries fills 3 primary ray queries for the foot.
     *  Phase 2: solve processes 3 rq_results from a batch execution.
     */
    bool try_cache(SIKCollideData& cld, const ik_foot_geom& foot_geom) const;
    void build_queries(const ik_foot_geom& foot_geom, const Fvector& pick_dir, ik_pick_query queries[3]) const;
    void solve(SIKCollideData& cld, const ik_foot_geom& foot_geom, const Fvector& pick_dir, CGameObject* O,
        bool /*foot_step*/, const collide::rq_result results[3]);

private:
    void set_previous_queries(const ik_pick_query queries[3]);
    void compute_cld(SIKCollideData& cld, const ik_foot_geom& foot_geom,
        const struct ik_pick_result& r_toe, const struct ik_pick_result& r_heel, const struct ik_pick_result& r_side);
};

static const float collide_dist = 0.5f;
