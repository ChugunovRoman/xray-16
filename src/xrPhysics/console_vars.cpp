#include "StdAfx.h"
#include "console_vars.h"

#include "PhysicsCommon.h"

BOOL ph_console::g_bDebugDumpPhysicsStep = 0;
float ph_console::ph_tri_query_ex_aabb_rate = 1.3f;
int ph_console::ph_tri_clear_disable_count = 10;

float ph_console::phBreakCommonFactor = 0.01f;
float ph_console::phRigidBreakWeaponFactor = 1.f;

float ph_console::ph_step_time = fixed_step; // keep in sync with the global physics timestep

int ph_console::ph_mt_island_min = 16;
int ph_console::ph_max_substeps = 0; // 0 = uncapped (default); >0 caps substeps but slows actor under load

// P1: NPC spawn / level-load optimizations (see plans/optimization_spawn/plan.md; permanent, only the log is left).
int ph_console::ph_dbg_spawn_stats = 0;

// P16 (plans/optimization_mt/11-ragdoll-spikes.md): the position correction at the start of a free ragdoll.
int ph_console::ph_death_correct_max_steps = 15;

// Ф2 (PHYSICS_ACTIVE_SET_PLAN): force-sleep far corpses, distance-based.
int ph_console::ph_corpse_sleep_ms = 3000;
float ph_console::ph_corpse_sleep_far_dist = 10.f; // 60 -> 10 (2026-10-16): a corpse pile near the camera never slept, after-fight FPS ~10 -> ~20
float ph_console::ph_corpse_sleep_max_vel = 1.f;
float ph_console::ph_corpse_sleep_max_stretch = 0.1f;

// Launched NPCs and corpses (plans/optimization_mt/21-physics-step.md, 2026-10-15/16): the limits of the permanent fix.
float ph_console::ph_contact_max_correct_vel = 5.f;
float ph_console::ph_ai_max_rise_vel = 5.f;
float ph_console::ph_ai_try_max_dy = 1.5f;
float ph_console::ph_ai_step_max_rise = 2.f;

// Corpse pairs dropped in the broadphase (plans/optimization_mt/21-physics-step.md, 2026-10-16): the limits of the
// permanent rules (corpse x corpse always skipped).
float ph_console::ph_corpse_vs_corpse_dist = 0.f;
float ph_console::ph_corpse_vs_npc_dist = 30.f;
