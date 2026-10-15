#include "StdAfx.h"
#include "PHDisabling.h"
#include "PhysicsCommon.h"
#include "Physics.h"
#include "MathUtilsOde.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#ifdef DEBUG
#include "debug_output.h"
#endif

extern CPHWorld* ph_world;

namespace
{
// Ф8 (plans/optimization_mt/21-physics-step.md): the sleep thresholds (worldDisablingParams,
// the "disable" sections of objects, the controllers' own in CPHCharacter) were tuned for fixed_step 0.01 (100 Hz) and
// are compared with per-step values: the displacement and the velocity change over one step (L1), and the same summed
// over the L2 window and divided by its step count (L2). With another step the same motion gives values scaled by
// fixed_step / 0.01 - at 50 Hz twice the 100 Hz value, so a body had to be twice as slow to fall asleep (and to stay
// asleep: the re-enable check uses the same values). Scaled back here: 0.5 at 50 Hz, 1 at 100 Hz (fixed_step follows
// ph_frequency, so it is read on every call). The L2 window stays L2frames steps (1.28 s at 50 Hz, 0.64 s at 100 Hz).
float SleepValueScale()
{
    constexpr float tuned_step = 0.01f;
    return tuned_step / fixed_step;
}
} // namespace
SDisableVector::SDisableVector() { Init(); }
void SDisableVector::Reset() { sum.set(0.f, 0.f, 0.f); }
void SDisableVector::Init()
{
    previous.set(0.f, 0.f, 0.f);
    Reset();
}

float SDisableVector::Update(const Fvector& new_vector)
{
    Fvector dif;
    dif.sub(new_vector, previous);
    previous.set(new_vector);
    sum.add(dif);
    return dif.magnitude();
}

float SDisableVector::UpdatePrevious(const Fvector& new_vector)
{
    Fvector dif;
    dif.sub(new_vector, previous);
    previous.set(new_vector);
    return dif.magnitude();
}

float SDisableVector::SumMagnitude() { return sum.magnitude(); }
SDisableUpdateState::SDisableUpdateState() { Reset(); }
void SDisableUpdateState::Reset()
{
    disable = false;
    enable = false;
}

SDisableUpdateState& SDisableUpdateState::operator&=(SDisableUpdateState& lstate)
{
    disable = disable && lstate.disable;
    enable = enable || lstate.enable;
    return *this;
}

CBaseDisableData::CBaseDisableData() : m_disabled(false), m_last_frame_updated(u16(-1))
{
    m_frames = worldDisablingParams.objects_params.L2frames;
    Reinit();
}

void CBaseDisableData::Reinit()
{
    m_count = m_frames;
    if (ph_world)
        m_count = m_count + ph_world->disable_count;
    m_stateL1.Reset();
    m_stateL2.Reset();
}
void CBaseDisableData::Disabling()
{
    VERIFY(ph_world);
    if (ph_world->IsFreezed())
        return;
    if (m_last_frame_updated == ph_world->StepsShortCnt())
        return;
    m_last_frame_updated = ph_world->StepsShortCnt();
    dBodyID body = get_body();
    m_count--;

    UpdateL1();

    CheckState(m_stateL1);

    if (m_count == 0) // ph_world->disable_count==dis_frames//m_count==m_frames
    {
        UpdateL2();
        CheckState(m_stateL2);
        m_count = m_frames;
    }
    const dReal* force = dBodyGetForce(body);
    const dReal* torqu = dBodyGetTorque(body);
    if (dDOT(force, force) > 0.f || dDOT(torqu, torqu) > 0.f)
        m_disabled = false;
    if (dBodyIsEnabled(body))
    {
        ReEnable();
        if (!m_disabled && (ph_world->disable_count != m_count % worldDisablingParams.objects_params.L2frames))
        {
            m_count = m_frames + ph_world->disable_count;
        }
    }
    if (m_disabled)
    {
        // Ф8: bodies the natural auto-disable puts to sleep (physics/sleep/auto_disable).
        if (dBodyIsEnabled(body))
            PERF_INC(ph_auto_disable);
        Disable(); // dBodyDisable(body);
    }
}

void CPHDisablingBase::Reinit()
{
    m_mean_velocity.Init();
    m_mean_acceleration.Init();
    CBaseDisableData::Reinit();
    bool disable = !dBodyIsEnabled(get_body());
    m_stateL1.disable = disable;
    m_stateL1.enable = !disable;
    m_stateL2.disable = disable;
    m_stateL2.enable = !disable;
}
void CPHDisablingBase::UpdateValues(const Fvector& new_pos, const Fvector& new_vel)
{
    const float scale = SleepValueScale(); // Ф8: 0.5 at 50 Hz
    if (m_count < m_frames)
    {
        float velocity_param = m_mean_velocity.Update(new_pos);
        float acceleration_param = m_mean_acceleration.Update(new_vel);
        CheckState(m_stateL1, velocity_param * m_frames * scale, acceleration_param * m_frames * scale);
    }
    else
    {
        float velocity_param = m_mean_velocity.UpdatePrevious(new_pos);
        float acceleration_param = m_mean_acceleration.UpdatePrevious(new_vel);
        CheckState(m_stateL1, velocity_param * m_frames * scale, acceleration_param * m_frames * scale);
    }
}
void CPHDisablingBase::UpdateL2()
{
    m_stateL2.Reset();
    const float scale = SleepValueScale(); // Ф8: 0.5 at 50 Hz
    float velocity_param = m_mean_velocity.SumMagnitude() / m_frames * scale;
    float acceleration_param = m_mean_acceleration.SumMagnitude() / m_frames * scale;

    CheckState(m_stateL2, velocity_param, acceleration_param);

    m_mean_velocity.Reset();
    m_mean_acceleration.Reset();
}

void CPHDisablingBase::set_DisableParams(const SOneDDOParams& params) { m_params = params; }
CPHDisablingTranslational::CPHDisablingTranslational() { m_params = worldDisablingParams.objects_params.translational; }
void CPHDisablingTranslational::Reinit()
{
    CPHDisablingBase::Reinit();
    dBodyID body = get_body();
    const dReal* position = dBodyGetPosition(body);
    const dReal* velocity = dBodyGetLinearVel(body);
    m_mean_velocity.UpdatePrevious(*(Fvector*)position);
    m_mean_acceleration.UpdatePrevious(*(Fvector*)velocity);
}
void CPHDisablingTranslational::UpdateL1()
{
    m_stateL1.Reset();
    dBodyID body = get_body();
    const dReal* position = dBodyGetPosition(body);
    const dReal* velocity = dBodyGetLinearVel(body);
#if 0
	DBG_DrawLine( cast_fv( position ), Fvector().add(cast_fv( position ),m_mean_velocity.sum), color_xrgb( 255, 0, 0 )   );
	DBG_DrawLine( cast_fv( position ), Fvector().add(cast_fv( position ),m_mean_acceleration.sum), color_xrgb( 0, 0, 255 )  );
#endif
    CPHDisablingBase::UpdateValues(*(Fvector*)position, *(Fvector*)velocity);
    // float			velocity_param		=	m_mean_velocity		.Update(* (Fvector*) position)		;
    // float			acceleration_param	=	m_mean_acceleration	.Update(* (Fvector*) velocity)		;
    // CheckState						(m_stateL1,velocity_param*m_frames,acceleration_param*m_frames) ;
}

void CPHDisablingTranslational::set_DisableParams(const SAllDDOParams& params)
{
    CPHDisablingBase::set_DisableParams(params.translational);
    m_frames = params.L2frames;
}

CPHDisablingRotational::CPHDisablingRotational() { m_params = worldDisablingParams.objects_params.rotational; }
void CPHDisablingRotational::Reinit()
{
    CPHDisablingBase::Reinit();
    dBodyID body = get_body();
    const dReal* rotation = dBodyGetRotation(body);
    const dReal* velocity = dBodyGetAngularVel(body);
    Fvector vrotation;
    vrotation.set(rotation[9], rotation[2], rotation[4]);
    m_mean_velocity.UpdatePrevious(vrotation);
    m_mean_acceleration.UpdatePrevious(*(Fvector*)velocity);
}
void CPHDisablingRotational::UpdateL1()
{
    m_stateL1.Reset();
    dBodyID body = get_body();
    const dReal* rotation = dBodyGetRotation(body);
    const dReal* velocity = dBodyGetAngularVel(body);
    Fvector vrotation;
    vrotation.set(rotation[9], rotation[2], rotation[4]);

    CPHDisablingBase::UpdateValues(vrotation, *(Fvector*)velocity);
    // float			velocity_param		=	m_mean_velocity		.Update	(			 vrotation	)	;
    // float			acceleration_param	=	m_mean_acceleration	.Update	(* (Fvector*) velocity	)	;

    // CheckState									(m_stateL1,velocity_param,acceleration_param)		;
}

void CPHDisablingRotational::set_DisableParams(const SAllDDOParams& params)
{
    CPHDisablingBase::set_DisableParams(params.rotational);
    m_frames = params.L2frames;
}

void CPHDisablingFull::Reinit()
{
    CPHDisablingRotational::Reinit();
    CPHDisablingTranslational::Reinit();
}
void CPHDisablingFull::UpdateL1()
{
    SDisableUpdateState state;
    CPHDisablingRotational::UpdateL1();
    state = m_stateL1;
    CPHDisablingTranslational::UpdateL1();
    m_stateL1 &= state;
}

void CPHDisablingFull::UpdateL2()
{
    SDisableUpdateState state;
    CPHDisablingRotational::UpdateL2();
    state = m_stateL2;
    CPHDisablingTranslational::UpdateL2();
    m_stateL2 &= state;
}

void CPHDisablingFull::set_DisableParams(const SAllDDOParams& params)
{
    CPHDisablingRotational::set_DisableParams(params);
    CPHDisablingTranslational::set_DisableParams(params);
}
