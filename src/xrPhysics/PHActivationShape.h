#pragma once

#include "PHValideValues.h"
#include "PHObject.h"
class IPhysicsShellHolder;
class CPHActivationShape : public CPHObject
{
    dBodyID m_body;
    dGeomID m_geom;
    Flags16 m_flags;
    CSafeFixedRotationState m_safe_state;

#ifdef DEBUG
    virtual IPhysicsShellHolder* ref_object();
#endif

public:
    enum EType
    {
        etBox,
        etCylinder,
        etSphere
    };

    enum
    {
        flFixedRotation = 1 << 0,
        flFixedPosition = 1 << 1,
        flStaticEnvironment = 1 << 2,
        flGravity = 1 << 3
    };
    CPHActivationShape();
    ~CPHActivationShape();
    void Create(const Fvector start_pos, const Fvector start_size, IPhysicsShellHolder* ref_obj, EType type = etBox,
        u16 flags = 0);
    void Destroy();
    // max_push_steps: P16 (plans/optimization_mt/11-ragdoll-spikes.md) - cap on the world steps of the push-out loop
    // (up to 10 attempts x 15 steps otherwise); 0 = no cap
    bool Activate(const Fvector need_size, u16 steps, float max_displacement, float max_rotation,
        bool un_freeze_later = false, u16 max_push_steps = 0);
    const Fvector& Position();
    void Size(Fvector& size);
    dBodyID ODEBody() { return m_body; }
    void set_rotation(const Fmatrix& rot);

private:
    // F1 (plans/optimization_mt/24-critical-path.md): StepTouch() / Step() of the push-out on the shape alone (see Activate)
    void LocalTouch();
    void LocalStep();
    void LocalStepCleanup();
    virtual void PhDataUpdate(dReal step);
    virtual void PhTune(dReal step);
    virtual void CutVelocity(float l_limit, float a_limit);
    virtual void InitContact(dContact* c, bool& do_collide, u16, u16);
    virtual dGeomID dSpacedGeom();
    virtual void get_spatial_params();
    virtual u16 get_elements_number() { return 0; }
    virtual CPHSynchronize* get_element_sync(u16 element) { return NULL; }
};
