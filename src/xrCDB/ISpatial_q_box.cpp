#include "stdafx.h"
#include "ISpatial.h"
#include "xrCore/_fbox.h"
#include "xrCore/FTimer.h"

#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_E2K) || defined(XR_ARCHITECTURE_PPC64)
#include <xmmintrin.h>
#elif defined(XR_ARCHITECTURE_ARM) || defined(XR_ARCHITECTURE_ARM64)
#include "sse2neon/sse2neon.h"
#elif defined(XR_ARCHITECTURE_RISCV)
#include "sse2rvv/sse2rvv.h"
#else
#error Add your platform here
#endif

extern Fvector c_spatial_offset[8];

namespace
{
// Strict AABB-AABB test matching Fbox::intersect semantics.
// Returns true if boxes intersect or touch.
ICF bool aabb_intersect_sse(
    const Fvector& aMin, const Fvector& aMax, const Fvector& bMin, const Fvector& bMax)
{
    const __m128 amin = _mm_set_ps(0.f, aMin.z, aMin.y, aMin.x);
    const __m128 amax = _mm_set_ps(0.f, aMax.z, aMax.y, aMax.x);
    const __m128 bmin = _mm_set_ps(0.f, bMin.z, bMin.y, bMin.x);
    const __m128 bmax = _mm_set_ps(0.f, bMax.z, bMax.y, bMax.x);

    // No intersection if amax < bmin or bmax < amin on any axis.
    const __m128 c1 = _mm_cmplt_ps(amax, bmin);
    const __m128 c2 = _mm_cmplt_ps(bmax, amin);
    const __m128 c = _mm_or_ps(c1, c2);
    return _mm_movemask_ps(c) == 0;
}
} // namespace

template <bool b_first>
class box_walker
{
public:
    u32 mask;
    Fvector center;
    Fvector size;
    Fvector bmin;
    Fvector bmax;
    Fbox box;
    xr_vector<ISpatial*>* out;

public:
    box_walker(xr_vector<ISpatial*>* _out, u32 _mask, const Fvector& _center, const Fvector& _size)
    {
        mask = _mask;
        center = _center;
        size = _size;
        box.setb(center, size);
        box.getcenter(center);
        bmin.set(center.x - size.x, center.y - size.y, center.z - size.z);
        bmax.set(center.x + size.x, center.y + size.y, center.z + size.z);
        out = _out;
    }

    void walk(ISpatial_NODE* N, Fvector& n_C, float n_R)
    {
        // node AABB
        const float n_vR = 2 * n_R;
        Fvector node_min, node_max;
        node_min.set(n_C.x - n_vR, n_C.y - n_vR, n_C.z - n_vR);
        node_max.set(n_C.x + n_vR, n_C.y + n_vR, n_C.z + n_vR);
        if (!aabb_intersect_sse(bmin, bmax, node_min, node_max))
            return;

        // test items
        for (auto& it : N->items)
        {
            ISpatial* S = it;
            if (0 == (S->GetSpatialData().type & mask))
                continue;

            Fvector& sC = S->GetSpatialData().sphere.P;
            float sR = S->GetSpatialData().sphere.R;
            Fvector obj_min, obj_max;
            obj_min.set(sC.x - sR, sC.y - sR, sC.z - sR);
            obj_max.set(sC.x + sR, sC.y + sR, sC.z + sR);
            if (!aabb_intersect_sse(bmin, bmax, obj_min, obj_max))
                continue;

            out->push_back(S);
            if constexpr (b_first)
                return;
        }

        // recurse
        float c_R = n_R / 2;
        for (u32 octant = 0; octant < 8; octant++)
        {
            if (0 == N->children[octant])
                continue;
            Fvector c_C;
            c_C.mad(n_C, c_spatial_offset[octant], c_R);
            walk(N->children[octant], c_C, c_R);
            if constexpr (b_first)
            {
                if (!out->empty())
                    return;
            }
        }
    }
};

template <bool b_first>
class sphere_walker
{
public:
    u32 mask;
    Fvector center;
    float radius;
    xr_vector<ISpatial*>* out;

public:
    sphere_walker(xr_vector<ISpatial*>* _out, u32 _mask, const Fvector& _center, const float _radius)
    {
        mask = _mask;
        center = _center;
        radius = _radius;
        out = _out;
    }

    void walk(ISpatial_NODE* N, Fvector& n_C, float n_R)
    {
        // Conservative node cull using the sphere's AABB.
        const float n_vR = 2 * n_R;
        Fvector node_min, node_max;
        node_min.set(n_C.x - n_vR, n_C.y - n_vR, n_C.z - n_vR);
        node_max.set(n_C.x + n_vR, n_C.y + n_vR, n_C.z + n_vR);
        Fvector sphere_min, sphere_max;
        sphere_min.set(center.x - radius, center.y - radius, center.z - radius);
        sphere_max.set(center.x + radius, center.y + radius, center.z + radius);
        if (!aabb_intersect_sse(node_min, node_max, sphere_min, sphere_max))
            return;

        // test items using exact sphere-sphere test
        for (auto& it : N->items)
        {
            ISpatial* S = it;
            if (0 == (S->GetSpatialData().type & mask))
                continue;

            Fvector& sC = S->GetSpatialData().sphere.P;
            float sR = S->GetSpatialData().sphere.R;
            const float radius_sum = radius + sR;
            if (center.distance_to_sqr(sC) > radius_sum * radius_sum)
                continue;

            out->push_back(S);
            if constexpr (b_first)
                return;
        }

        // recurse
        float c_R = n_R / 2;
        for (u32 octant = 0; octant < 8; octant++)
        {
            if (0 == N->children[octant])
                continue;
            Fvector c_C;
            c_C.mad(n_C, c_spatial_offset[octant], c_R);
            walk(N->children[octant], c_C, c_R);
            if constexpr (b_first)
            {
                if (!out->empty())
                    return;
            }
        }
    }
};

void ISpatial_DB::q_box(xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center, const Fvector& _size)
{
    ZoneScoped;
    std::shared_lock<std::shared_mutex> shlock(rw);
    {
        ScopeStatTimer scope(Stats.Query, query_stats_lock);
        R.clear();
        if (_o & O_ONLYFIRST)
        {
            box_walker<true> W(&R, _mask, _center, _size);
            W.walk(m_root, m_center, m_bounds);
        }
        else
        {
            box_walker<false> W(&R, _mask, _center, _size);
            W.walk(m_root, m_center, m_bounds);
        }
    }
}

void ISpatial_DB::q_sphere(
    xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center, const float _radius)
{
    ZoneScoped;
    std::shared_lock<std::shared_mutex> shlock(rw);
    {
        ScopeStatTimer scope(Stats.Query, query_stats_lock);
        R.clear();
        if (_o & O_ONLYFIRST)
        {
            sphere_walker<true> W(&R, _mask, _center, _radius);
            W.walk(m_root, m_center, m_bounds);
        }
        else
        {
            sphere_walker<false> W(&R, _mask, _center, _radius);
            W.walk(m_root, m_center, m_bounds);
        }
    }
}
