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

    // P1: NPC spawn / level-load optimizations (see plans/optimization_spawn/plan.md). Each is an
    // independent runtime kill-switch (0 = old behavior) so any of them can be disabled without a rebuild.
    static int ph_spawn_char_no_collide_chars; //= 1; activation shape used to position a spawning character does not collide with other characters (avoids the Freeze()->wake-neighbor cascade in CollisionCorrectObjPos)
    static int ph_spawn_activation_shape_early_exit; //= 1; skip the push-out Step() loop in CPHActivationShape::Activate when StepTouch already found no penetration and the shape size isn't changing
    static int ph_spawn_skip_correct_on_load; //= 1; skip CollisionCorrectObjPos for characters spawned in the initial level/save load burst (their positions are already valid)
    static int ph_dbg_spawn_stats; //= 0; log CPHActivationShape::Activate calls that run an unusually large number of physics steps or wake many objects
};
