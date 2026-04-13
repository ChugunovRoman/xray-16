#pragma once

struct XRPHYSICS_API ph_console
{
    static BOOL g_bDebugDumpPhysicsStep; //= 0;
    /** 1: while physics FrameStep runs, defer cross-thread mutations to m_objects / m_update_objects / m_recently_disabled_objects until drain (mtPhysics). Same-thread (physics owner) applies immediately. */
    static int ph_mt_frame_write_barrier;
    /** 1: run CollideDynamicsBroadphase for all frozen objects via xr_parallel_for before serial CollideStepPostBroadphase (default 0). Requires TaskScheduler; see docs — concurrent SpatialSpacePhysic q_box is experimental. */
    static int ph_parallel_broadphase_prepass;
    /** Minimum frozen object count to use parallel broadphase prepass when ph_parallel_broadphase_prepass is on. */
    static int ph_parallel_broadphase_prepass_min_objects;
    static float ph_tri_query_ex_aabb_rate; //= 1.3f;
    static int ph_tri_clear_disable_count; //= 10;
    static float phBreakCommonFactor; //= 0.01f;
    static float phRigidBreakWeaponFactor; //= 1.f;
    static float ph_step_time; //=fixed_step;
};
