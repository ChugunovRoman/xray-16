#pragma once
#include "inventory_space.h"
#include "GameObject.h"
#include "xrPhysics/IPhysicsShellHolder.h"

class IPHStaticGeomShell;

// IPhysicsShellHolder (plans/addon_base_building/plan.md 17.37): the box has no physics shell, the actor and the
// bodies pass through it. Section key static_collision gives it a static geom shell of its bone shapes (as
// CPhysicObject has, plan 17.8) - and a static geom shell wants an IPhysicsShellHolder as its ref object (the contact
// code of the character controllers finds it by the ID of the object: validated_contact_ref_object). The box is not
// a CPhysicsShellHolder: that one would change the client data of the boxes in the saves (its save writes a byte)
class CInventoryBox : public CGameObject, public IPhysicsShellHolder
{
    typedef CGameObject inherited;

public:
    xr_vector<u16> m_items;

protected:
    bool m_in_use;
    bool m_can_take;
    bool m_closed;

public:
    CInventoryBox();
    virtual ~CInventoryBox();

    virtual void OnEvent(NET_Packet& P, u16 type);
    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Relcase(IGameObject* O);
    void AddAvailableItems(TIItemContainer& items_container) const;
    IC bool IsEmpty() const { return m_items.empty(); }
    virtual void UpdateCL();
    virtual void Load(LPCSTR section);
    virtual bool is_ai_obstacle() const;
    // CGameObject::net_Spawn puts an object that uses AI locations onto the plane of its level vertex. A box with
    // static_collision is placed like the level geometry (plan 17.39: a stash on a shelf, on a table) - it stays where
    // it was spawned, else it falls through the shelf onto the floor under it
    virtual bool can_validate_position_on_spawn() override { return !m_static_collision; }

    IC void set_in_use(bool status) { m_in_use = status; }
    IC bool in_use() const { return m_in_use; }
    void set_can_take(bool status);
    IC bool can_take() const { return m_can_take; }
    void set_closed(bool status, LPCSTR reason);
    IC bool closed() const { return m_closed; }
protected:
    void SE_update_status();

private:
    // Section keys, read in Load (false / true / static_collision && npc_collision by default): static_collision - the
    // static geom shell of the bone shapes (the actor and the bodies collide with the box as with the level);
    // npc_collision = false - the controllers of NPCs and monsters pass through it (P_IgnoreNpcCharacterContact, as
    // CPhysicObject, plan 17.19); is_ai_obstacle - the box is an obstacle of the NPC paths (moving_objects)
    bool m_static_collision = false;
    bool m_npc_collision = true;
    bool m_is_ai_obstacle = false;
    IPHStaticGeomShell* m_static_shell = nullptr;
    // ObjectPPhysicsShell returns a reference: the box has no physics shell, always nullptr
    CPhysicsShell* m_no_physics_shell = nullptr;
    void CreateStaticCollision();

public:
    // IPhysicsShellHolder: the ref object of the static geom shell only (no physics shell, no character, no capture)
    virtual Fmatrix& ObjectXFORM();
    virtual Fvector& ObjectPosition();
    virtual LPCSTR ObjectName() const;
    virtual LPCSTR ObjectNameVisual() const;
    virtual LPCSTR ObjectNameSect() const;
    virtual bool ObjectGetDestroy() const;
    virtual ICollisionHitCallback* ObjectGetCollisionHitCallback() { return nullptr; }
    virtual u16 ObjectID() const;
    virtual IGameObject* IObject() { return this; }
    virtual ICollisionForm* ObjectCollisionModel();
    virtual IKinematics* ObjectKinematics();
    virtual IDamageSource* ObjectCastIDamageSource() { return nullptr; }
    virtual void ObjectProcessingDeactivate() {}
    virtual void ObjectProcessingActivate() {}
    virtual void ObjectSpatialMove() {}
    virtual CPhysicsShell*& ObjectPPhysicsShell() { return m_no_physics_shell; }
    virtual void enable_notificate() {}
    virtual bool has_parent_object();
    virtual void on_physics_disable() {}
    virtual IPHCapture* PHCapture() { return nullptr; }
    virtual bool IsInventoryItem() { return false; }
    virtual bool IsActor() { return false; }
    virtual bool IsStalker() { return false; }
    virtual bool IsCollideWithBullets() { return true; }
    virtual bool IsCollideWithActorCamera() { return true; }
    virtual void HideAllWeapons(bool /*v*/) {}
    virtual void MovementCollisionEnable(bool /*enable*/) {}
    virtual CPHSoundPlayer* ObjectPhSoundPlayer() { return nullptr; }
    virtual ICollisionDamageReceiver* ObjectPhCollisionDamageReceiver() { return nullptr; }
    virtual void BonceDamagerCallback(float& /*damage_factor*/) {}
#ifdef DEBUG
    virtual std::string dump(EDumpType type) const;
#endif
};
