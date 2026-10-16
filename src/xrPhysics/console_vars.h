#pragma once

struct XRPHYSICS_API ph_console
{
    static BOOL g_bDebugDumpPhysicsStep; //= 0;
    static float ph_tri_query_ex_aabb_rate; //= 1.3f;
    static int ph_tri_clear_disable_count; //= 10;
    static float phBreakCommonFactor; //= 0.01f;
    static float phRigidBreakWeaponFactor; //= 1.f;
    static float ph_step_time; //=fixed_step;
    static int ph_mt_island_min; //= 16; min active islands for the parallel island solve (CPHWorld::Step; permanent)
    static int ph_max_substeps; //= 0; cap physics substeps per frame (anti death-spiral); 0 = no cap (default — capping slows the actor under load)

    // P1: NPC spawn / level-load optimizations (see plans/optimization_spawn/plan.md) are permanent: the activation
    // shape of a spawning character does not collide with other characters, CPHActivationShape::Activate skips the
    // push-out Step() loop when StepTouch found no penetration, characters of the load burst skip CollisionCorrectObjPos.
    static int ph_dbg_spawn_stats; //= 0; log CPHActivationShape::Activate calls that run an unusually large number of physics steps or wake many objects

    // P16 (plans/optimization_mt/11-ragdoll-spikes.md): the position correction at the start of a free ragdoll
    // (CCharacterPhysicsSupport::EndActivateFreeShell -> CollisionCorrectObjPos) - a box of the ragdoll is pushed out of
    // everything around in a frozen world, up to 150 world steps when it cannot get out (a crowd, a pile of corpses).
    // The box always ignores characters (live NPCs, actor). Saved tunable.
    static int ph_death_correct_max_steps; //= 15; >0 = at most this many world steps in the push-out loop, 0 = no cap

    // Ф2 (PHYSICS_ACTIVE_SET_PLAN): принудительный сон дальних трупов — максимально грубо, без проверки
    // скорости и без ожидания "устаканивания": старше ph_corpse_sleep_ms и дальше ph_corpse_sleep_far_dist
    // от камеры -> Disable() безусловно. Труп может не среагировать на следующий удар — это приемлемая
    // цена против постоянной стоимости его симуляции. 0 = выкл (старое поведение, только естественный сон ODE).
    static int ph_corpse_sleep_ms; //= 3000; минимальный возраст трупа (мс) прежде чем гасить физику
    static float ph_corpse_sleep_far_dist; //= 10.f; дистанция (м) от камеры, дальше которой труп гасится безусловно
    // Защита от "замороженной растяжки": не гасить труп, пока он ещё летит/кувыркается или пока его суставы
    // разошлись (после взрыва ODE стягивает элементы обратно за несколько шагов - если усыпить в этот момент,
    // растянутая поза остаётся навсегда). 0 = проверка выключена.
    static float ph_corpse_sleep_max_vel; //= 1.f; м/с, максимальная линейная скорость любого элемента рэгдолла
    static float ph_corpse_sleep_max_stretch; //= 0.1f; м, максимальное расхождение точек крепления любого сустава

    // Launched NPCs and corpses (plans/optimization_mt/21-physics-step.md, 2026-10-15/16). The fixes themselves are
    // permanent (pairs refused by a full island get contacts, the rise cap takes back the rise of the substep, no
    // step-climb onto another character - see Physics.cpp, PHSimpleCharacter.cpp); the limits below are saved tunables.
    // The push-out speed of a contact in the frame step, m/s: the depth of a new contact joint is cut so that ODE's
    // correction fps * erp * depth stays under it (ODE: unlimited). Depths a game callback set (step climbing) are kept.
    static float ph_contact_max_correct_vel; //= 5.f; 0 = unlimited
    // The rise speed of a live NPC controller, m/s, outside a jump, an external impulse and climbing; the rise of the
    // substep over it is taken back too.
    static float ph_ai_max_rise_vel; //= 5.f; 0 = no cap
    // Watchdog of CPHAICharacter::TryPosition, m: a result higher / lower than both the start and the destination by more
    // than this is refused (the NPC stays, TryPosition fails, the frame step takes over). 0 = off.
    static float ph_ai_try_max_dy; //= 1.5f
    // Watchdog of the frame step, m: a live NPC controller that rose more than this in one substep is put back where it
    // was before the substep, its velocity zeroed. 0 = off.
    static float ph_ai_step_max_rise; //= 2.f

    // Corpse pairs dropped in the broadphase (plans/optimization_mt/21-physics-step.md, journal 2026-10-16;
    // CPHObject::CollideDynamicsBroadphase). Permanent after the A/B test; these are its saved limits. A corpse = a
    // ragdoll-class shell (the death shell of a character). Corpse x corpse pairs are never collided (corpses pass through
    // each other) - beyond ph_corpse_vs_corpse_dist of the camera, m: > 0 = keep the pair when either corpse is within it.
    static float ph_corpse_vs_corpse_dist; //= 0.f; 0 = skip everywhere
    // m: > 0 = corpse x live NPC controller pairs with both beyond this of the camera are never collided (the NPC walks
    // through, the corpse is not pushed or woken). The actor always collides with corpses.
    static float ph_corpse_vs_npc_dist; //= 30.f; 0 = off
};
