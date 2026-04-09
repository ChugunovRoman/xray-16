#include "stdafx.h"
#include "ISpatial.h"
#include "xrCore/_fbox.h"
#include "xrCore/FTimer.h"

extern Fvector c_spatial_offset[8];

template <bool b_first>
class box_walker
{
public:
    u32 mask;
    Fvector center;
    Fvector size;
    Fbox box;
    xr_vector<ISpatial*>* out;

public:
    box_walker(xr_vector<ISpatial*>* _out, u32 _mask, const Fvector& _center, const Fvector& _size)
    {
        mask = _mask;
        center = _center;
        size = _size;
        box.setb(center, size);
        out = _out;
    }

    void walk(ISpatial_NODE* N, Fvector& n_C, float n_R)
    {
        // box
        float n_vR = 2 * n_R;
        Fbox BB;
        BB.set(n_C.x - n_vR, n_C.y - n_vR, n_C.z - n_vR, n_C.x + n_vR, n_C.y + n_vR, n_C.z + n_vR);
        if (!BB.intersect(box))
            return;

        // test items
        for (auto& it : N->items)
        {
            ISpatial* S = it;
            if (0 == (S->GetSpatialData().type & mask))
                continue;

            Fvector& sC = S->GetSpatialData().sphere.P;
            float sR = S->GetSpatialData().sphere.R;
            Fbox sB;
            sB.set(sC.x - sR, sC.y - sR, sC.z - sR, sC.x + sR, sC.y + sR, sC.z + sR);
            if (!sB.intersect(box))
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

void ISpatial_DB::q_sphere(xr_vector<ISpatial*>& R, u32 _o, u32 _mask, const Fvector& _center, const float _radius)
{
    ZoneScoped;
    Fvector _size = {_radius, _radius, _radius};
    q_box(R, _o, _mask, _center, _size);
}
