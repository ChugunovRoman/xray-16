#pragma once

struct XRPHYSICS_API ph_console
{
    static BOOL g_bDebugDumpPhysicsStep; //= 0;
    static float ph_tri_query_ex_aabb_rate; //= 1.3f;
    static int ph_tri_clear_disable_count; //= 10;
    static float phBreakCommonFactor; //= 0.01f;
    static float phRigidBreakWeaponFactor; //= 1.f;
    static float ph_step_time; //=fixed_step;
    static int ph_mt_island_solve; //= 1; P1 dev kill-switch: parallel island solve (CPHWorld::Step)
    static int ph_mt_island_min; //= 16; min active islands to enable parallel solve
    static int ph_max_substeps; //= 0; cap physics substeps per frame (anti death-spiral); 0 = no cap (default — capping slows the actor under load)
};
