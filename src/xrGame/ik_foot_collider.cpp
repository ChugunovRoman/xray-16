#include "StdAfx.h"
#include "ik_foot_collider.h"

#include "xrMaterialSystem/GameMtlLib.h"
#include "xrCDB/Intersect.hpp"
#include "Include/xrRender/Kinematics.h"

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "xrCDB/xr_area.h"
#include "performance_cvars.h"

#include "xrPhysics/MathUtils.h"

#include "ik_collide_data.h"
#include <tracy/Tracy.hpp>

#ifdef DEBUG
#include "PHDebug.h"
// Custom 5-argument overload is defined in PHDebug.cpp but not exposed in the header.
void DBG_DrawTri(const Fvector& v0, const Fvector& v1, const Fvector& v2, u32 ac, bool solid);
#endif

ik_foot_collider::ik_foot_collider() {}
static const Fplane invalide_plane = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};

struct ik_pick_result
{
    ik_pick_result(ik_foot_geom::e_collide_point _point)
        : p(invalide_plane), position(Fvector().set(-FLT_MAX, -FLT_MAX, -FLT_MAX)),
          point(_point), range(0)
    {
        triangle[0] = Fvector().set(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        triangle[1] = Fvector().set(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        triangle[2] = Fvector().set(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    }

    Fplane p;
    Fvector triangle[3];
    Fvector position;
    ik_foot_geom::e_collide_point point;
    float range;
};

bool ignore_tri(CDB::TRI& tri)
{
    SGameMtl* material = GMLib.GetMaterialByIdx(tri.material);

    return (material->Flags.test(SGameMtl::flPassable) && !material->Flags.test(SGameMtl::flActorObstacle)) ||
        material->Flags.test(SGameMtl::flClimable); // ||
    // material->Flags.test( SGameMtl::flActorObstacle );
}

bool ignore_static_tri(int tri)
{
    VERIFY(Level().ObjectSpace.GetStaticModel()->get_tris_count() > tri);
    CDB::TRI* triangle = Level().ObjectSpace.GetStaticTris() + tri;
    return ignore_tri(*triangle);
}

IC bool ignore_object(IGameObject* O)
{
    VERIFY(O);
    if (static_cast<CGameObject*>(O)
            ->cast_entity_alive() /*&& static_cast<CGameObject*>( O )->cast_entity_alive()->g_Alive() */)
        return true;
    return false;
}

IC bool ignore_result(collide::rq_result& R)
{
    if (R.O)
        return ignore_object(R.O);
    else
        return ignore_static_tri(R.element);
}

IC void tri_plane(const Fvector& v0, const Fvector& v1, const Fvector& v2, Fplane& p)
{
    p.n.mknormal(v0, v1, v2);
    VERIFY(!fis_zero(p.n.magnitude()));
    p.n.invert();
    p.d = -p.n.dotproduct(v0);
}

IC void tri_plane(const CDB::TRI& tri, Fplane& p)
{
    Fvector* pVerts = Level().ObjectSpace.GetStaticVerts();
    tri_plane(pVerts[tri.verts[0]], pVerts[tri.verts[1]], pVerts[tri.verts[2]], p);
}

IC bool get_plane_static(ik_pick_result& r, Fvector& next_pos, float& next_range, const collide::rq_result& R,
    float pick_dist, const Fvector& pos, const Fvector& pick_v)
{
    VERIFY(Level().ObjectSpace.GetStaticModel()->get_tris_count() > R.element);
    CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + R.element;
    Fvector* pVerts = Level().ObjectSpace.GetStaticVerts();

    r.triangle[0] = pVerts[tri->verts[0]];
    r.triangle[1] = pVerts[tri->verts[1]];
    r.triangle[2] = pVerts[tri->verts[2]];

    tri_plane(r.triangle[0], r.triangle[1], r.triangle[2], r.p);

    r.position.add(pos, Fvector().mul(pick_v, R.range));
    next_pos.set(r.position);
    next_range = pick_dist - R.range;
    if (ignore_tri(*tri))
    {
        next_pos.add(Fvector().mul(pick_v, EPS_L));
        float dot = pick_v.dotproduct(r.p.n);
        if (0.f < dot)
        {
            next_pos.add(Fvector().mul(r.p.n, EPS_L));
        }
        // next_pos.add( Fvector().mul( r.p.n, EPS_L  ) );
        next_range -= EPS_L;
#ifdef DEBUG
        float u, v, d;
        VERIFY(!(CDB::TestRayTri(next_pos, pick_v, r.triangle, u, v, d, true) && d > 0.f));
#endif // DEBUG
        return false;
    }
    r.range = R.range;
    return true;
}

IC bool get_plane_dynamic(ik_pick_result& r, Fvector& next_pos, float& next_range, const collide::rq_result R,
    float pick_dist, const Fvector& pos, const Fvector& pick_v)
{
    next_pos.add(pos, Fvector().mul(pick_v, R.range + EPS_L));
    next_range = pick_dist - R.range - EPS_L;

    if (ignore_object(R.O))
        return false;

    IRenderVisual* V = R.O->Visual();
    if (V)
    {
        IKinematics* K = V->dcast_PKinematics();
        if (K)
        {
            float dist = pick_dist;
            IKinematics::pick_result res;

            if (K->PickBone(R.O->XFORM(), res, dist, pos, pick_v, (u16)R.element))
            {
                // cld.collided = true;
                r.position.add(pos, Fvector().mul(pick_v, res.dist));
                r.p.n.invert(res.normal);
                r.p.d = -r.p.n.dotproduct(r.position);
                r.triangle[0] = res.tri[0];
                r.triangle[1] = res.tri[1];
                r.triangle[2] = res.tri[2];
                next_pos.set(r.position);
                next_range = pick_dist - res.dist;
                r.range = res.dist;
                return true;
            }
        }
    }
    return false;
}

static const float reach_dist = 1.5f;
IC bool get_plane(ik_pick_result& r, Fvector& next_pos, float& next_range, const collide::rq_result R, float pick_dist,
    const Fvector& pos, const Fvector& pick_v)
{
    if (!R.O)
        return get_plane_static(r, next_pos, next_range, R, pick_dist, pos, pick_v);
    else
        return get_plane_dynamic(r, next_pos, next_range, R, pick_dist, pos, pick_v);
}

IC bool rq_pick_hit(const collide::rq_result& R) { return R.element >= 0; }

static bool PickContinueAfterFirstHit(ik_pick_result& r, const ik_pick_query& q, IGameObject* ignore_object,
    collide::rq_result R, Fvector pos, float range)
{
    if (!rq_pick_hit(R))
        return false;

    bool collided = false;
    for (;;)
    {
        Fvector next_pos = pos;
        float next_range = range;

        collided = get_plane(r, next_pos, next_range, R, range, pos, q.dir());
        if (collided)
            break;

        range = next_range;
        pos = next_pos;
        if (range < EPS)
        {
            collided = false;
            break;
        }
        if (!g_pGameLevel->ObjectSpace.RayPick(pos, q.dir(), range, collide::rqtBoth, R, ignore_object))
        {
            collided = false;
            break;
        }
    }

#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(phDbgDrawIKCollision) && collided && !R.O)
    {
        CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + R.element;
        Fvector p = q.pos();
        p.add(Fvector().mul(q.dir(), range));
        DBG_DrawLine(pos, p, color_xrgb(255, 0, 0));
        if (tri)
        {
            DBG_DrawTri(tri, Level().ObjectSpace.GetStaticVerts(), color_xrgb(255, 0, 0));
        }
    }
#endif
    return collided;
}

bool Pick(ik_pick_result& r, const ik_pick_query& q, IGameObject* ignore_object)
{
    VERIFY(q.is_valid());

    const float range = q.range();
    const Fvector pos = q.pos();
    collide::rq_result R;
    if (!g_pGameLevel->ObjectSpace.RayPick(pos, q.dir(), range, collide::rqtBoth, R, ignore_object))
        return false;
    return PickContinueAfterFirstHit(r, q, ignore_object, R, pos, range);
}

void ik_foot_collider::build_queries(
    const ik_foot_geom& foot_geom, const Fvector& pick_dir, ik_pick_query queries[3]) const
{
    ZoneScopedN("ik_foot_collider::build_queries");
    VERIFY(foot_geom.is_valid());

    const float pick_dist = collide_dist + reach_dist;

    const Fvector pos_toe = Fvector().sub(foot_geom.pos_toe(), Fvector().mul(pick_dir, collide_dist));
    queries[0] = ik_pick_query(ik_foot_geom::toe, pos_toe, pick_dir, pick_dist);

    const Fvector pos_heel = Fvector().sub(foot_geom.pos_heel(), Fvector().mul(pick_dir, collide_dist));
    queries[1] = ik_pick_query(ik_foot_geom::heel, pos_heel, pick_dir, pick_dist);

    const Fvector pos_side = Fvector().sub(foot_geom.pos_side(), Fvector().mul(pick_dir, collide_dist));
    queries[2] = ik_pick_query(ik_foot_geom::side, pos_side, pick_dir, pick_dist);
}

bool ik_foot_collider::try_cache(SIKCollideData& cld, const ik_foot_geom& foot_geom) const
{
    ZoneScopedN("ik_foot_collider::try_cache");
    ik_pick_query q[3];
    build_queries(foot_geom, cld.m_pick_dir, q);

    if (previous_toe_query.is_equal(q[0]) && previous_heel_query.is_equal(q[1]) &&
        previous_side_query.is_equal(q[2]))
    {
        cld = previous_data;
        return true;
    }
    return false;
}

void ik_foot_collider::set_previous_queries(const ik_pick_query queries[3])
{
    previous_toe_query = queries[0];
    previous_heel_query = queries[1];
    previous_side_query = queries[2];
}

void ik_foot_collider::compute_cld(SIKCollideData& cld, const ik_foot_geom& foot_geom,
    const ik_pick_result& r_toe, const ik_pick_result& r_heel, const ik_pick_result& r_side)
{
    const Fvector pos_toe = foot_geom.pos_toe();
    const Fvector pos_heel = foot_geom.pos_heel();
    const float foot_length = Fvector().sub(pos_toe, pos_heel).magnitude() * 1.5f;

#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(phDbgDrawIKCollision))
    {
        DBG_DrawPoint(pos_toe, 0.01, color_xrgb(255, 0, 0));
        if (cld.collided)
            DBG_DrawPoint(r_toe.position, 0.01, color_xrgb(0, 0, 255));
    }
#endif

    const bool toe_heel_compatible =
        cld.collided && r_heel.point != ik_foot_geom::none &&
        Fvector().sub(r_heel.position, r_toe.position).magnitude() < foot_length;
    const bool toe_side_compatible =
        cld.collided && r_side.point != ik_foot_geom::none &&
        Fvector().sub(r_side.position, r_toe.position).magnitude() < foot_length;

    if (toe_heel_compatible && toe_side_compatible)
    {
        Fplane plane;
        tri_plane(r_toe.position, r_heel.position, r_side.position, plane);
        if (plane.n.dotproduct(r_toe.p.n) < 0.f)
        {
            plane.n.invert();
            plane.d = -plane.d;
        }

        cld.m_plane = plane;
#ifdef DEBUG
        if (ph_dbg_draw_mask1.test(phDbgDrawIKCollision))
        {
            DBG_DrawPoint(pos_toe, 0.01, color_xrgb(255, 0, 0));
            if (cld.collided)
            {
                DBG_DrawTri(r_toe.position, r_heel.position, r_side.position, color_xrgb(0, 0, 255), false);
            }
        }
#endif
        previous_data = cld;
        return;
    }

    float hight = -FLT_MAX;
    ik_pick_result r = r_toe;

    if (cld.collided)
    {
        hight = r_toe.position.y;
    }

    if (r_heel.point != ik_foot_geom::none && r_heel.position.y > hight)
    {
        r = r_heel;
        hight = r_heel.position.y;
        cld.collided = true;
    }

    if (r_side.point != ik_foot_geom::none && r_side.position.y > hight)
    {
        r = r_side;
        hight = r_side.position.y;
        cld.collided = true;
    }

    if (cld.collided)
    {
        cld.m_plane = r.p;
        cld.m_collide_point = r.point;
        previous_data = cld;
        return;
    }

    previous_data = cld;
}

void ik_foot_collider::solve(SIKCollideData& cld, const ik_foot_geom& foot_geom, const Fvector& pick_dir,
    CGameObject* O, bool /*foot_step*/, const collide::rq_result results[3])
{
    ZoneScopedN("ik_foot_collider::solve");
    VERIFY(foot_geom.is_valid());
    cld.collided = false;

    ik_pick_query q[3];
    // TODO: accept pre-built queries from GatherRays to avoid redundant rebuild
    build_queries(foot_geom, pick_dir, q);

    ik_pick_result r_toe(ik_foot_geom::toe);
    ik_pick_result r_heel(ik_foot_geom::heel);
    ik_pick_result r_side(ik_foot_geom::side);

    const bool toe_collided =
        rq_pick_hit(results[0]) && PickContinueAfterFirstHit(r_toe, q[0], O, results[0], q[0].pos(), q[0].range());
    cld.collided = toe_collided;
    cld.m_plane = r_toe.p;
    cld.m_collide_point = ik_foot_geom::toe;

    const bool heel_collided =
        rq_pick_hit(results[1]) && PickContinueAfterFirstHit(r_heel, q[1], O, results[1], q[1].pos(), q[1].range());
    const bool side_collided =
        rq_pick_hit(results[2]) && PickContinueAfterFirstHit(r_side, q[2], O, results[2], q[2].pos(), q[2].range());

    r_toe.point = toe_collided ? ik_foot_geom::toe : ik_foot_geom::none;
    r_heel.point = heel_collided ? ik_foot_geom::heel : ik_foot_geom::none;
    r_side.point = side_collided ? ik_foot_geom::side : ik_foot_geom::none;

    compute_cld(cld, foot_geom, r_toe, r_heel, r_side);

    // Cache queries only after results are validated — prevents stale cache on failed solves.
    set_previous_queries(q);
}

void ik_foot_collider::collide(SIKCollideData& cld, const ik_foot_geom& foot_geom, CGameObject* O, bool foot_step)
{
    ZoneScopedN("ik_foot_collider::collide");
    if (try_cache(cld, foot_geom))
        return;

    ik_pick_query q[3];
    build_queries(foot_geom, cld.m_pick_dir, q);

    ik_pick_result r_toe(ik_foot_geom::toe);
    ik_pick_result r_heel(ik_foot_geom::heel);
    ik_pick_result r_side(ik_foot_geom::side);

    bool heel_collided = false;
    bool side_collided = false;
    bool toe_collided = false;

    if (npc_perf_ik_foot_raypick_batch)
    {
        collide::rq_result R[3];
        CObjectSpace::RayPickBatchItem batch[3];
        batch[0] = { q[0].pos(), q[0].dir(), q[0].range(), collide::rqtBoth, O, &R[0] };
        batch[1] = { q[1].pos(), q[1].dir(), q[1].range(), collide::rqtBoth, O, &R[1] };
        batch[2] = { q[2].pos(), q[2].dir(), q[2].range(), collide::rqtBoth, O, &R[2] };
        g_pGameLevel->ObjectSpace.RayPickBatch(batch, 3);

        toe_collided = rq_pick_hit(R[0]) && PickContinueAfterFirstHit(r_toe, q[0], O, R[0], q[0].pos(), q[0].range());
        cld.collided = toe_collided;
        cld.m_plane = r_toe.p;
        cld.m_collide_point = ik_foot_geom::toe;

        heel_collided = rq_pick_hit(R[1]) && PickContinueAfterFirstHit(r_heel, q[1], O, R[1], q[1].pos(), q[1].range());
        side_collided = rq_pick_hit(R[2]) && PickContinueAfterFirstHit(r_side, q[2], O, R[2], q[2].pos(), q[2].range());
    }
    else
    {
        toe_collided = Pick(r_toe, q[0], O);
        cld.collided = toe_collided;
        cld.m_plane = r_toe.p;
        cld.m_collide_point = ik_foot_geom::toe;

        heel_collided = Pick(r_heel, q[1], O);
        side_collided = Pick(r_side, q[2], O);
    }

    r_toe.point = toe_collided ? ik_foot_geom::toe : ik_foot_geom::none;
    r_heel.point = heel_collided ? ik_foot_geom::heel : ik_foot_geom::none;
    r_side.point = side_collided ? ik_foot_geom::side : ik_foot_geom::none;

    compute_cld(cld, foot_geom, r_toe, r_heel, r_side);

    // Cache queries only after results are validated — prevents stale cache on failed solves.
    set_previous_queries(q);
}
