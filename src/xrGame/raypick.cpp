#include "StdAfx.h"
#include "raypick.h"
#include "Level.h"
#include "xrEngine/xr_collide_form.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Include/xrRender/Kinematics.h"

CRayPick::CRayPick()
{
    start_position.set(0, 0, 0);
    direction.set(0, 0, 0);
    range = 0;
    flags = collide::rq_target::rqtNone;
    ignore = nullptr;
};

CRayPick::CRayPick(const Fvector& P, const Fvector& D, const float R, const collide::rq_target F, CScriptGameObject* I)
{
    start_position.set(P);
    direction.set(D);
    range = R;
    flags = F;
    ignore = nullptr;
    if (I)
        ignore = smart_cast<IGameObject*>(&(I->object()));
};

bool CRayPick::query()
{
    collide::rq_result R;
    if (Level().ObjectSpace.RayPick(start_position, direction, range, flags, R, ignore))
    {
        result.set(R, start_position, direction); // D2: with the surface
        return true;
    }

    return false;
}

void ray_pick_surface(const collide::rq_result& R, const Fvector& start, const Fvector& dir, Fvector& normal,
    u16& material_idx)
{
    // The fallback: facing the ray, material unknown
    normal.set(dir).invert();
    if (normal.square_magnitude() > EPS_S)
        normal.normalize();
    else
        normal.set(0.f, 1.f, 0.f);
    material_idx = u16(-1);

    if (!g_pGameLevel)
        return;
    const Fvector hit = Fvector().mad(start, dir, R.range);
    if (!R.O)
    {
        // Static geometry: element is the triangle of the static CDB of the level
        CDB::MODEL* model = Level().ObjectSpace.GetStaticModel();
        if (R.element < 0 || size_t(R.element) >= size_t(model->get_tris_count()))
            return;
        const Fvector* verts = Level().ObjectSpace.GetStaticVerts();
        const CDB::TRI& tri = *(Level().ObjectSpace.GetStaticTris() + R.element);
        Fvector n;
        n.mknormal(verts[tri.verts[0]], verts[tri.verts[1]], verts[tri.verts[2]]);
        if (!_valid(n) || n.square_magnitude() < EPS_S)
            return;
        // The side the ray came from: a ray from below a floor gets the normal pointing down
        if (n.dotproduct(dir) > 0.f)
            n.invert();
        normal.set(n);
        if (tri.material < GMLib.CountMaterial())
            material_idx = u16(tri.material);
        return;
    }

    // An object: element is the bone for a skeleton collision form (the bullets read it the same way)
    CCF_Skeleton* skeleton = smart_cast<CCF_Skeleton*>(R.O->GetCForm());
    IKinematics* kinematics = R.O->Visual() ? smart_cast<IKinematics*>(R.O->Visual()) : nullptr;
    if (!skeleton || !kinematics || R.element < 0 || R.element >= int(kinematics->LL_BoneCount()))
        return;
    // The face of the hit bone shape (a box - its side, a cylinder - its cap or side, a sphere - from its centre):
    // a ray onto the top of a table or a crate gives "up" over the whole top, not only at its middle
    Fvector shape_normal;
    if (skeleton->_ElementNormal(u16(R.element), hit, shape_normal))
    {
        if (shape_normal.dotproduct(dir) > 0.f) // a ray from inside the shape: face the ray
            shape_normal.invert();
        normal.set(shape_normal);
    }
    else if (Fvector center; skeleton->_ElementCenter(u16(R.element), center))
    {
        Fvector n = Fvector().sub(hit, center);
        if (n.square_magnitude() > EPS_S)
        {
            n.normalize();
            if (n.dotproduct(dir) > 0.f) // the centre is past the hit point (a thin bone): face the ray
                n.invert();
            normal.set(n);
        }
    }
    const u16 bone_material = kinematics->LL_GetData(u16(R.element)).game_mtl_idx;
    if (bone_material < GMLib.CountMaterial())
        material_idx = bone_material;
}

pcstr script_rq_result::get_material() const
{
    if (material_id < 0 || size_t(material_id) >= GMLib.CountMaterial())
        return "";
    return GMLib.GetMaterialByIdx(u16(material_id))->m_Name.c_str();
}
