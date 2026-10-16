#pragma once

#include "xrCDB/ISpatial.h"
#include "PHItemList.h"
#include "PHIsland.h"
typedef u32 CLClassBits;
typedef u32 CLBits;
class SpatialBase;
using qResultVec = xr_vector<ISpatial*>;
class CPHObject;
class CPHUpdateObject;
class CPHMoveStorage;
class CPHSynchronize;

typedef void CollideCallback(CPHObject* obj1, CPHObject* obj2, dGeomID o1, dGeomID o2);
#ifdef DEBUG
class IPhysicsShellHolder;
#endif
class CPHObject : public SpatialBase
{
#ifdef DEBUG
    friend struct SPHObjDBGDraw;
#endif
    DECLARE_PHLIST_ITEM(CPHObject)

    Flags8 m_flags;

    enum
    {
        st_activated = (1 << 0),
        st_freezed = (1 << 1),
        st_dirty = (1 << 2),
        st_net_interpolation = (1 << 3),
        fl_ray_motions = (1 << 4),
        st_recently_deactivated = (1 << 5)
    };

    CPHIsland m_island;
    CLBits m_collide_bits;
    u8 m_check_count;
    // Parallel post-integrate (permanent, see PostIntegrateParallel): written by the task that runs this
    // object's parallel part, read and cleared by the serial pass of the same step.
    enum : u8
    {
        pf_parallel = (1 << 0), // the parallel part runs now: deactivate() only records pf_deactivate
        pf_deactivate = (1 << 1), // deactivate() requested by the parallel part, applied by PostIntegrateSerial
        pf_rebuild = (1 << 2), // the object left its octree node: SpatialBase::spatial_move in PostIntegrateSerial
        pf_done = (1 << 3), // the parallel part ran this step
    };
    u8 m_post_flags;
    _flags<CLClassBits> m_collide_class_bits;

public:
    enum ECastType
    {
        tpNotDefinite,
        tpShell,
        tpCharacter,
        tpStaticShell
    };

protected:
    Fvector AABB;
    xr_vector<ISpatial*> m_collide_spatial_overlap;
    xr_vector<CPHObject*> m_collide_dyn_candidates;

protected:
    virtual dGeomID dSpacedGeom() = 0;
    virtual void get_spatial_params() = 0;
    void spatial_refresh_bounds() override;
    virtual void spatial_register();
    void SetRayMotions() { m_flags.set(fl_ray_motions, TRUE); }
    void UnsetRayMotions() { m_flags.set(fl_ray_motions, FALSE); }
    void SetPrefereExactIntegration() { m_island.SetPrefereExactIntegration(); }
    CPHObject* SelfPointer() { return this; }
public:
    IC BOOL IsRayMotion() { return m_flags.test(fl_ray_motions); }
    void IslandReinit() { m_island.Unmerge(); }
    void IslandStep(dReal step) { m_island.Step(step); }
    void MergeIsland(CPHObject* obj) { m_island.Merge(&obj->m_island); }
    CPHIsland& Island() { return m_island; }
    dWorldID DActiveWorld() { return m_island.DActiveWorld(); }
    CPHIsland* DActiveIsland() { return m_island.DActiveIsland(); }
    dWorldID DWorld() { return m_island.DWorld(); }
    virtual void FreezeContent();
    virtual void UnFreezeContent();
    virtual void EnableObject(CPHObject* obj);
    virtual bool DoCollideObj();
    virtual bool step_single(dReal step);
    void reinit_single();
    void step_prediction(float time);
    void step(float time);
    virtual void PhDataUpdate(dReal step) = 0;
    virtual void PhTune(dReal step) = 0;
    virtual void spatial_move();
    // Parallel post-integrate (plans/optimization_mt/21-physics-step.md, permanent; CPHWorld::Step). An
    // object whose PhDataUpdateSplit() is true runs its post-integrate in two parts: PostIntegrateParallel() in the
    // island-solve task of its merge root, right after that island is solved (own bodies, geoms and fields only: velocity
    // limits, sleep checks, interpolation, bounds), and PostIntegrateSerial() on the stepping thread in m_objects order
    // (list edits, game callbacks, octree rebuilds) - the same calls in the same order as PhDataUpdate() + spatial_move().
    virtual bool PhDataUpdateSplit() const { return false; }
    void PostIntegrateParallel(dReal step);
    IC bool PostIntegrateDone() const { return !!(m_post_flags & pf_done); }
    void PostIntegrateSerial(dReal step);

protected:
    // PhDataUpdate() == PhDataUpdateParallel() + PhDataUpdateSerial() for a split-capable object.
    virtual void PhDataUpdateParallel(dReal step) {}
    virtual void PhDataUpdateSerial(dReal step) {}

public:
    virtual void InitContact(dContact* c, bool& do_collide, u16 /*material_idx_1*/, u16 /*material_idx_2*/) = 0;
    virtual void CutVelocity(float l_limit, float a_limit){};

    void Freeze();
    void UnFreeze();
    IC bool IsFreezed() { return !!(m_flags.test(st_freezed)); }
    void NetInterpolationON() { m_flags.set(st_net_interpolation, true); }
    void NetInterpolationOFF() { m_flags.set(st_net_interpolation, false); }
    bool NetInterpolation() { return !!(m_flags.test(st_net_interpolation)); }
    virtual u16 get_elements_number() = 0;
    virtual CPHSynchronize* get_element_sync(u16 element) = 0;
    // virtual void StepFrameUpdate(dReal step)=0;

    CPHObject();
    void activate();
    IC bool is_active() const { return !!m_flags.test(st_activated) /*b_activated*/; }
    void deactivate();
    void put_in_recently_deactivated();
    void remove_from_recently_deactivated();
    void check_recently_deactivated();
    void collision_disable();
    void collision_enable();
    virtual void ClearRecentlyDeactivated() { ; }
    virtual void Collide();
    virtual void near_callback(CPHObject* obj) { ; }
    virtual void RMotionsQuery(qResultVec& res) { ; }
    virtual CPHMoveStorage* MoveStorage() { return NULL; }
    virtual ECastType CastType() { return tpNotDefinite; }
    virtual void vis_update_activate() {}
    virtual void vis_update_deactivate() {}
#ifdef DEBUG
    virtual IPhysicsShellHolder* ref_object() = 0;
#endif

    IC CLBits& collide_bits() { return m_collide_bits; }
    IC _flags<CLClassBits>& collide_class_bits() { return m_collide_class_bits; }
    IC const CLBits& collide_bits() const { return m_collide_bits; }
    IC const _flags<CLClassBits>& collide_class_bits() const { return m_collide_class_bits; }
    void CollideRayMotions();
    virtual void CollideDynamicsBroadphase();
    void CollideDynamicsNarrow();
    virtual void CollideStepPostBroadphase();
    void CollideDynamics();
    IC void CollideClearDirty() { m_flags.set(st_dirty, FALSE); }
};

DEFINE_PHITEM_LIST(CPHObject, PH_OBJECT_STORAGE, PH_OBJECT_I)
