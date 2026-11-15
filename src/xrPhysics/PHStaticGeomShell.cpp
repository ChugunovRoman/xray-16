#include "StdAfx.h"
#include "PHStaticGeomShell.h"
#include "SpaceUtils.h"

#include "IPhysicsShellHolder.h"
#include "PHCharacter.h"
#include "IClimableObject.h"

#include "Include/xrRender/Kinematics.h"
#include "PHCollideValidator.h"
#include "ExtendedGeom.h"
#include "xrEngine/xr_object.h"
#include "xrCore/Animation/Bone.hpp"

void CPHStaticGeomShell::get_spatial_params()
{
    Fvector AABB;
    spatialParsFromDGeom(dSpacedGeometry(), spatial.sphere.P, AABB, spatial.sphere.R);
}

void CPHStaticGeomShell::PhDataUpdate(dReal step)
{
    Island().Step(step);
    Island().Unmerge();
    PhysicsRefObject()->enable_notificate();
    CPHUpdateObject::Deactivate();
}
void CPHStaticGeomShell::Activate(const Fmatrix& form)
{
    build();
    setStaticForm(form);
    get_spatial_params();
    spatial_register();
}

void CPHStaticGeomShell::Deactivate()
{
    spatial_unregister();
    CPHUpdateObject::Deactivate();
    destroy();
}

CPHStaticGeomShell::CPHStaticGeomShell() { spatial.type |= STYPE_PHYSIC; }
void cb(CBoneInstance* B) {}
void P_BuildStaticGeomShell(CPHStaticGeomShell* pUnbrokenObject, IPhysicsShellHolder* obj,
    ObjectContactCallbackFun* object_contact_callback, const Fobb& b)
{
    pUnbrokenObject->add_Box(b);
    pUnbrokenObject->Activate(obj->ObjectXFORM());

    pUnbrokenObject->set_PhysicsRefObject(obj);
    // m_pUnbrokenObject->SetPhObjectInGeomData(m_pUnbrokenObject);
    pUnbrokenObject->set_ObjectContactCallback(object_contact_callback);
    CPHCollideValidator::SetNonDynamicObject(*pUnbrokenObject);
}
CPHStaticGeomShell* P_BuildStaticGeomShell(
    IPhysicsShellHolder* obj, ObjectContactCallbackFun* object_contact_callback, const Fobb& b)
{
    CPHStaticGeomShell* pUnbrokenObject = xr_new<CPHStaticGeomShell>();
    P_BuildStaticGeomShell(pUnbrokenObject, obj, object_contact_callback, b);
    return pUnbrokenObject;
}

IPHStaticGeomShell* P_BuildStaticGeomShell(IPhysicsShellHolder* obj, ObjectContactCallbackFun* object_contact_callback)
{
    Fobb b;
    // IRenderVisual* V=obj->ObjectVisual();
    // R_ASSERT2(V,"need visual to build");
    IKinematics* K = obj->ObjectKinematics();
    R_ASSERT2(K, "need visual to build");
    K->CalculateBones(TRUE); //. bForce - was TRUE

    // V->getVisData().box.getradius	(b.m_halfsize);
    K->GetBox().getradius(b.m_halfsize);

    b.xform_set(Fidentity);
    CPHStaticGeomShell* pUnbrokenObject = P_BuildStaticGeomShell(obj, object_contact_callback, b);

    // IKinematics* K=smart_cast<IKinematics*>(V); VERIFY(K);
    K->CalculateBones(TRUE);
    for (u16 k = 0; k < K->LL_BoneCount(); k++)
    {
        K->LL_GetBoneInstance(k).set_callback(bctPhysics, cb, K->LL_GetBoneInstance(k).callback_param(), TRUE);
        // K->LL_GetBoneInstance(k).Callback_overwrite = TRUE;
        // K->LL_GetBoneInstance(k).Callback = cb;
    }
    return pUnbrokenObject;
}

IPHStaticGeomShell* P_BuildStaticGeomShellFromBones(IPhysicsShellHolder* obj)
{
    IKinematics* K = obj ? obj->ObjectKinematics() : nullptr;
    if (!K)
        return nullptr;
    // The bind pose in the object space (the same matrices CPHShell::AddElementRecursive puts the shapes with), not
    // LL_GetTransform: the bones of a physic object follow its shell, the shell may not be in the bind pose
    xr_vector<Fmatrix> bind;
    K->LL_GetBindTransform(bind);
    CPHStaticGeomShell* shell = xr_new<CPHStaticGeomShell>();
    const u16 no_material = u16(GAMEMTL_NONE_IDX);
    u16 material = no_material;
    bool any = false;
    for (u16 id = 0, count = K->LL_BoneCount(); id < count; ++id)
    {
        // A hidden bone has no shape in the physics shell either (CPHShell::AddElementRecursive): the parts of a broken
        // object (the bones mask of its save, CPHSkeleton) must not stay as invisible walls
        if (!K->LL_GetBoneVisible(id))
            continue;
        const IBoneData& bone = K->GetBoneData(id);
        const SBoneShape& shape = bone.get_shape();
        if (shape.type == SBoneShape::stNone || shape.flags.test(SBoneShape::sfNoPhysics))
            continue;
        shell->add_Shape(shape, bind[id]);
        if (material == no_material)
        {
            material = bone.get_game_mtl_idx();
            if (material == no_material)
                material = GMLib.GetMaterialIdx(bone.GetMaterialName().c_str());
        }
        any = true;
    }
    if (!any)
    {
        xr_delete(shell);
        return nullptr;
    }
    shell->Activate(obj->ObjectXFORM());
    shell->set_PhysicsRefObject(obj);
    // One material for all the shapes (CPHGeometryOwner keeps one): the first bone with a shape - the root as a rule
    if (material != no_material)
        shell->SetMaterial(material);
    CPHCollideValidator::SetNonDynamicObject(*shell);
    return shell;
}

void P_StaticGeomShellAddContactCallback(IPHStaticGeomShell* shell, ObjectContactCallbackFun* callback)
{
    if (!shell || !callback)
        return;
    static_cast<CPHStaticGeomShell*>(shell)->add_ObjectContactCallback(callback);
}

// bo1 - the geom of the object is g1 of the contact, the other one is g2 (dxGeomUserData::object_callbacks, Physics.cpp)
void P_IgnoreNpcCharacterContact(bool& do_collide, bool bo1, dContact& c, SGameMtl* /*material_1*/,
    SGameMtl* /*material_2*/)
{
    dxGeomUserData* ud = PHRetrieveGeomUserData(bo1 ? c.geom.g2 : c.geom.g1);
    // only a character controller (its wheel, shell, hat and cap geoms): a ragdoll, an item, a bullet ray collide
    if (!ud || !ud->ph_object || ud->ph_object->CastType() != CPHObject::tpCharacter)
        return;
    IPhysicsShellHolder* holder = ud->ph_ref_object;
    if (holder && holder->IsActor())
        return;
    do_collide = false;
}

void DestroyStaticGeomShell(IPHStaticGeomShell*& UnbrokenObject)
{
    if (!UnbrokenObject)
        return;
    CPHStaticGeomShell* gs = static_cast<CPHStaticGeomShell*>(UnbrokenObject);
    gs->Deactivate();
    xr_delete(gs);
    UnbrokenObject = 0;
}

class IClimableObject;
class CPHLeaderGeomShell : public CPHStaticGeomShell
{
    IClimableObject* m_pClimable;

public:
    CPHLeaderGeomShell(IClimableObject* climable);
    void near_callback(CPHObject* obj);
};

IPHStaticGeomShell* P_BuildLeaderGeomShell(IClimableObject* obj, ObjectContactCallbackFun* callback, const Fobb& b)
{
    CPHLeaderGeomShell* pStaticShell = xr_new<CPHLeaderGeomShell>(obj);
    P_BuildStaticGeomShell(smart_cast<CPHStaticGeomShell*>(pStaticShell), smart_cast<IPhysicsShellHolder*>(obj), 0, b);
    pStaticShell->SetMaterial(obj->Material());
    pStaticShell->set_ObjectContactCallback(callback);
    return pStaticShell;
}

CPHLeaderGeomShell::CPHLeaderGeomShell(IClimableObject* climable) { m_pClimable = climable; }
void CPHLeaderGeomShell::near_callback(CPHObject* obj)
{
    if (obj && obj->CastType() == CPHObject::tpCharacter)
    {
        CPHCharacter* ch = static_cast<CPHCharacter*>(obj);
        ch->SetElevator(m_pClimable);
    }
}
