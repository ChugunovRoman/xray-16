#include "StdAfx.h"
#include "console_vars.h"

#include "PhysicsCommon.h"

BOOL ph_console::g_bDebugDumpPhysicsStep = 0;
int ph_console::ph_mt_frame_write_barrier = 1;
int ph_console::ph_parallel_broadphase_prepass = 0;
int ph_console::ph_parallel_broadphase_prepass_min_objects = 32;
float ph_console::ph_tri_query_ex_aabb_rate = 1.3f;
int ph_console::ph_tri_clear_disable_count = 10;

float ph_console::phBreakCommonFactor = 0.01f;
float ph_console::phRigidBreakWeaponFactor = 1.f;

float ph_console::ph_step_time = fixed_step;
