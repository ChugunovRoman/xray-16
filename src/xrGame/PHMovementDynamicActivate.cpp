
#include "StdAfx.h"

#include "PHMovementControl.h"

// extern	class CPHWorld	*ph_world;
#include "xrPhysics/PHCharacter.h"
#include "xrPhysics/IPhysicsShellHolder.h"
#include "xrPhysics/IPHWorld.h"

void CPHMovementControl::QueueDeferredBoxActivation(u32 id, int num_it, int num_steps, float resolve_depth)
{
    IPHWorld* ph = physics_world();
    if (!ph)
    {
        ActivateBoxDynamic(id, num_it, num_steps, resolve_depth);
        return;
    }

    m_deferred_box_activation_id = id;
    m_deferred_box_activation_num_it = num_it;
    m_deferred_box_activation_num_steps = num_steps;
    m_deferred_box_activation_resolve_depth = resolve_depth;
    if (m_deferred_box_activation_registered)
        return;

    m_deferred_box_activation_registered = true;
    if (ph->defer_scheduler_mutation([this] { ApplyDeferredBoxActivation(); }))
        return;

    m_deferred_box_activation_registered = false;
    ActivateBoxDynamic(id, num_it, num_steps, resolve_depth);
}

void CPHMovementControl::ApplyDeferredBoxActivation()
{
    m_deferred_box_activation_registered = false;
    ActivateBoxDynamic(m_deferred_box_activation_id, m_deferred_box_activation_num_it,
        m_deferred_box_activation_num_steps, m_deferred_box_activation_resolve_depth);
}

bool CPHMovementControl::ActivateBoxDynamic(
    u32 id, int num_it /*=8*/, int num_steps /*5*/, float resolve_depth /*=0.01f*/)
{
    if (id >= 4)
        return false;

    bool character_exist = CharacterExist();
    if (character_exist && trying_times[id] != u32(-1))
    {
        // Fvector dif;dif.sub(trying_poses[id],cast_fv(dBodyGetPosition(m_character->get_body())));
        Fvector character_body_pos;
        m_character->get_body_position(character_body_pos);
        Fvector dif;
        dif.sub(trying_poses[id], character_body_pos);
        if (Device.dwTimeGlobal - trying_times[id] < 500 && dif.magnitude() < 0.05f)
            return false;
    }
    IPhysicsShellHolder* ref_object = m_character ? m_character->PhysicsRefObject() : nullptr;
    if (!m_character || !ref_object || ref_object->ObjectPPhysicsShell())
        return false;
    u32 old_id = BoxID();

    bool character_disabled = character_exist && !m_character->IsEnabled();

    if (character_exist && id == old_id)
        return true;

    // B-1: a real box transition. If the GameThread scheduler runs parallel to the ODE step (overlap),
    // the resize (and possible CreateCharacter) must not mutate physics mid-step — defer it to the
    // post-step flush (main thread, after wait()), which also runs the real collision/fit test there.
    // Return false ("not applied yet") so the actor stance state machine keeps the transition PENDING
    // (e.g. mcCrouch stays set, auto-stand keeps retrying every frame until clear of a ceiling). Once
    // the deferred apply lands the box, next frame hits the id==old_id early-return above -> returns
    // true -> the state machine syncs (one frame late). This mirrors overlap=0 behavior, including the
    // "can't stand under a low ceiling" case. NOTE: the fix for the original stuck-crouch bug is placing
    // this defer AFTER the id==old_id check (the agent's bug was deferring before it, so state never synced).
    IPHWorld* ph = physics_world();
    if (ph && ph->ShouldDeferSchedulerPhysicsMutation())
    {
        QueueDeferredBoxActivation(id, num_it, num_steps, resolve_depth);
        return false;
    }

    if (!character_exist)
    {
        CreateCharacter();
    }

    Fvector vel;
    Fvector pos;

    GetCharacterVelocity(vel);

    GetCharacterPosition(pos);

    bool ret = ::ActivateBoxDynamic(this, character_exist, id, num_it, num_steps, resolve_depth);

    if (!ret)
    {
        if (!character_exist)
            DestroyCharacter();
        else if (character_disabled)
            m_character->Disable();
        ActivateBox(old_id);
        SetVelocity(vel);
        if (m_character)
            m_character->fix_body_rotation();
        // dBodyID b= !m_character ? 0 : m_character->get_body();//GetBody();
        // if(b)
        //{
        //	dMatrix3 R;
        //	dRSetIdentity (R);
        //	dBodySetAngularVel(b,0.f,0.f,0.f);
        //	dBodySetRotation(b,R);
        //}

        SetPosition(pos);

        // Msg("can not activate!");
    }
    else
    {
        ActivateBox(id);
        // Msg("activate!");
    }

    //	SetOjectContactCallback(saved_callback);
    //	saved_callback=0;
    SetVelocity(vel);

    if (!ret && character_exist)
    {
        trying_times[id] = Device.dwTimeGlobal;

        // trying_poses[id].set(cast_fv(dBodyGetPosition(m_character->get_body())));
        m_character->GetBodyPosition(trying_poses[id]); //.set(cast_fv(dBodyGetPosition(m_character->get_body())));
    }
    else
    {
        trying_times[id] = u32(-1);
    }
    return ret;
}
