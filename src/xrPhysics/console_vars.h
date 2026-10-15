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

    // P16 (plans/optimization_mt/11-ragdoll-spikes.md): the position correction at the start of a free ragdoll
    // (CCharacterPhysicsSupport::EndActivateFreeShell -> CollisionCorrectObjPos) - a box of the ragdoll is pushed out of
    // everything around in a frozen world, up to 150 world steps when it cannot get out (a crowd, a pile of corpses).
    // The box always ignores characters (live NPCs, actor). Not saved.
    static int ph_death_correct_max_steps; //= 0; >0 = at most this many world steps in the push-out loop

    // Ф2 (PHYSICS_ACTIVE_SET_PLAN): принудительный сон дальних трупов — максимально грубо, без проверки
    // скорости и без ожидания "устаканивания": старше ph_corpse_sleep_ms и дальше ph_corpse_sleep_far_dist
    // от камеры -> Disable() безусловно. Труп может не среагировать на следующий удар — это приемлемая
    // цена против постоянной стоимости его симуляции. 0 = выкл (старое поведение, только естественный сон ODE).
    static int ph_corpse_sleep_ms; //= 3000; минимальный возраст трупа (мс) прежде чем гасить физику
    static float ph_corpse_sleep_far_dist; //= 60.f; дистанция (м) от камеры, дальше которой труп гасится безусловно
    // Защита от "замороженной растяжки": не гасить труп, пока он ещё летит/кувыркается или пока его суставы
    // разошлись (после взрыва ODE стягивает элементы обратно за несколько шагов - если усыпить в этот момент,
    // растянутая поза остаётся навсегда). 0 = проверка выключена.
    static float ph_corpse_sleep_max_vel; //= 1.f; м/с, максимальная линейная скорость любого элемента рэгдолла
    static float ph_corpse_sleep_max_stretch; //= 0.1f; м, максимальное расхождение точек крепления любого сустава

    // plans/optimization_mt/21-physics-step.md: put a corpse to sleep before ph_corpse_sleep_ms (which stays the upper
    // limit) once it has settled - older than ph_corpse_settle_min_age_ms, every element slower than ph_corpse_settle_vel
    // and ph_corpse_settle_ang_vel for ph_corpse_settle_ms of physics time in a row, no joint stretched beyond
    // ph_corpse_sleep_max_stretch, farther than ph_corpse_sleep_far_dist. TEMPORARY A/B switch, not saved.
    static int ph_corpse_sleep_settled; //= 0;
    static int ph_corpse_settle_min_age_ms; //= 2000; lets falls, explosions and the joint recovery after them finish
    static int ph_corpse_settle_ms; //= 700; continuous settled time (frame steps of the physics thread)
    static float ph_corpse_settle_vel; //= 0.1f; m/s, max linear speed of any element
    static float ph_corpse_settle_ang_vel; //= 0.5f; rad/s, max angular speed of any element
};
