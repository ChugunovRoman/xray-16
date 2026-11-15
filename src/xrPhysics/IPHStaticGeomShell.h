#pragma once
#include "PhysicsExternalCommon.h"

class XR_NOVTABLE IPHStaticGeomShell
{
protected:
    virtual ~IPHStaticGeomShell() = 0;

    //	virtual void						set_ObjectContactCallback	(ObjectContactCallbackFun* callback);
};

inline IPHStaticGeomShell::~IPHStaticGeomShell() = default;

class IPhysicsShellHolder;
class IClimableObject;
XRPHYSICS_API IPHStaticGeomShell* P_BuildStaticGeomShell(
    IPhysicsShellHolder* obj, ObjectContactCallbackFun* object_contact_callback);
XRPHYSICS_API IPHStaticGeomShell* P_BuildLeaderGeomShell(
    IClimableObject* obj, ObjectContactCallbackFun* callback, const Fobb& b);
XRPHYSICS_API void DestroyStaticGeomShell(IPHStaticGeomShell*& p);
// Static collision from the bone shapes of the model of obj (box / sphere / cylinder of every bone with a physics
// shape, in the bind pose, at obj->ObjectXFORM()): characters and other bodies collide with it as with the level - no
// body, no "object" contact of the character controller. Section key static_collision of CPhysicObject
// (plans/addon_base_building/plan.md 17.8). nullptr - no visual or no bone with a physics shape.
XRPHYSICS_API IPHStaticGeomShell* P_BuildStaticGeomShellFromBones(IPhysicsShellHolder* obj);
// Adds an object contact callback to every geom of a static geom shell (as CPhysicsShell::add_ObjectContactCallback
// does for a physics shell)
XRPHYSICS_API void P_StaticGeomShellAddContactCallback(IPHStaticGeomShell* shell, ObjectContactCallbackFun* callback);
// The object contact callback of the section key npc_collision = false of CPhysicObject
// (plans/addon_base_building/plan.md 17.19): the contacts with the character controller of anyone but the actor (an NPC,
// a monster) are dropped - they walk through the object, the actor collides with it as before. The same way the doors
// ignore the AI (door_ignore of CPhysicObject::set_door_ignore_dynamics)
XRPHYSICS_API void P_IgnoreNpcCharacterContact(
    bool& do_collide, bool bo1, dContact& c, SGameMtl* material_1, SGameMtl* material_2);

// CPHStaticGeomShell* P_BuildStaticGeomShell(CGameObject* obj,ObjectContactCallbackFun* object_contact_callback,Fobb
// &b);
// void				P_BuildStaticGeomShell(CPHStaticGeomShell* shell,CGameObject* obj,ObjectContactCallbackFun*
// object_contact_callback,Fobb &b);
