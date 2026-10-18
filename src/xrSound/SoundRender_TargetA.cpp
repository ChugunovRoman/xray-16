#include "stdafx.h"

#include "SoundRender_TargetA.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"

CSoundRender_TargetA::CSoundRender_TargetA()
{
}

bool CSoundRender_TargetA::_initialize()
{
    A_CHK(alGenBuffers(sdef_target_count_submit, pBuffers));
    alGenSources(1, &pSource);
    const ALenum error = alGetError();
    if (AL_NO_ERROR == error)
    {
        cache_params_valid = false; // a new AL source: fill_parameters() sends everything
        A_CHK(alSourcei(pSource, AL_LOOPING, AL_FALSE));
        A_CHK(alSourcef(pSource, AL_MIN_GAIN, 0.f));
        A_CHK(alSourcef(pSource, AL_MAX_GAIN, 1.f));
        A_CHK(alSourcef(pSource, AL_GAIN, cache_gain));
        A_CHK(alSourcef(pSource, AL_PITCH, cache_pitch));
        return true;
    }
    Msg("! sound: OpenAL: Can't create source. Error: %s.", static_cast<pcstr>(alGetString(error)));
    return false;
}

void CSoundRender_TargetA::_destroy()
{
    // clean up target
    if (alIsSource(pSource))
        alDeleteSources(1, &pSource);
    A_CHK(alDeleteBuffers(sdef_target_count_submit, pBuffers));
}

void CSoundRender_TargetA::_restart()
{
    _destroy();
    _initialize();
}

void CSoundRender_TargetA::start(CSoundRender_Emitter* E)
{
    inherited::start(E);

    const auto& info = m_pEmitter->source()->data_info();
    const bool mono = info.channels == 1;

    if (info.format == SoundFormat::Float32)
        dataFormat = mono ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
    else
        dataFormat = mono ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

    sampleRate = info.samplesPerSec;
}

void CSoundRender_TargetA::render()
{
    inherited::render();

    submit_all_buffers();

    A_CHK(alSourceQueueBuffers(pSource, sdef_target_count_submit, pBuffers));
    A_CHK(alSourcePlay(pSource));
}

void CSoundRender_TargetA::stop()
{
    if (rendering)
    {
        A_CHK(alSourceStop(pSource));
        A_CHK(alSourcei(pSource, AL_BUFFER, 0));
        A_CHK(alSourcei(pSource, AL_SOURCE_RELATIVE, TRUE));
    }
    cache_params_valid = false; // the next emitter on this voice sends all its 3D parameters
    inherited::stop();
}

void CSoundRender_TargetA::rewind()
{
    inherited::rewind();

    A_CHK(alSourceStop(pSource));
    A_CHK(alSourcei(pSource, AL_BUFFER, 0));

    submit_all_buffers();

    A_CHK(alSourceQueueBuffers(pSource, sdef_target_count_submit, pBuffers));
    A_CHK(alSourcePlay(pSource));
}

void CSoundRender_TargetA::update()
{
    inherited::update();

    ALint processed, state;
    ALenum error;

    /* Get relevant source info */
    alGetSourcei(pSource, AL_SOURCE_STATE, &state);
    alGetSourcei(pSource, AL_BUFFERS_PROCESSED, &processed);
    if ((error = alGetError()) != AL_NO_ERROR)
    {
        Msg("! %s:: source state check failed (0x%d)", __FUNCTION__, error);
        return;
    }

    while (processed > 0)
    {
        ALuint BufferID;
        A_CHK(alSourceUnqueueBuffers(pSource, 1, &BufferID));

        submit_buffer(BufferID);

        A_CHK(alSourceQueueBuffers(pSource, 1, &BufferID));
        processed--;
        if ((error = alGetError()) != AL_NO_ERROR)
        {
            Msg("! %s:: buffering data failed (0x%d)", __FUNCTION__, error);
            return;
        }
    }

    /* Make sure the source hasn't underrun */
    if (state != AL_PLAYING && state != AL_PAUSED)
    {
        ALint queued;

        /* If no buffers are queued, playback is finished */
        alGetSourcei(pSource, AL_BUFFERS_QUEUED, &queued);
        if (queued == 0)
            return;

        alSourcePlay(pSource);
        if ((error = alGetError()) != AL_NO_ERROR)
        {
            Msg("! %s:: restarting playback failed (0x%d)", __FUNCTION__, error);
            return;
        }
    }
}

void CSoundRender_TargetA::fill_parameters()
{
    R_ASSERT1_CURE(m_pEmitter, { return; });
    inherited::fill_parameters();

    // 3D params
    const auto& src = m_pEmitter->p_source;
    const auto& pos = src.position;

    // 46-engine-core-render-audit.md: only what changed, in the same order. Each set of OpenAL Soft takes the context
    // locks and, on a playing source, rebuilds its props for the mixer; ~165 voices x 5 sets every frame on the Game
    // thread (game/seq_frame_mt/sound ~0.48 ms/frame), while mostly only the position of a moving source changes.
    // The AL source ends up in the same state as with the five sets every frame.
    const ALint relative = m_pEmitter->b2D;
    const bool all = !cache_params_valid;
    if (all || cache_ref_dist != src.min_distance)
    {
        cache_ref_dist = src.min_distance;
        A_CHK(alSourcef(pSource, AL_REFERENCE_DISTANCE, src.min_distance));
    }
    if (all || cache_max_dist != src.max_distance)
    {
        cache_max_dist = src.max_distance;
        A_CHK(alSourcef(pSource, AL_MAX_DISTANCE, src.max_distance));
    }
    if (all || cache_pos.x != pos.x || cache_pos.y != pos.y || cache_pos.z != pos.z)
    {
        cache_pos = pos;
        A_CHK(alSource3f(pSource, AL_POSITION, pos.x, pos.y, -pos.z));
    }
    if (all || cache_relative != relative)
    {
        cache_relative = relative;
        A_CHK(alSourcei(pSource, AL_SOURCE_RELATIVE, relative));
    }
    if (all || cache_rolloff != psSoundRolloff)
    {
        cache_rolloff = psSoundRolloff;
        A_CHK(alSourcef(pSource, AL_ROLLOFF_FACTOR, psSoundRolloff));
    }
    cache_params_valid = true;

    float _gain = m_pEmitter->smooth_volume;
    clamp(_gain, EPS_S, 1.f);
    if (!fsimilar(_gain, cache_gain, 0.01f))
    {
        cache_gain = _gain;
        A_CHK(alSourcef(pSource, AL_GAIN, _gain));
    }

    float _pitch = src.freq;
    if (!m_pEmitter->bIgnoringTimeFactor)
        _pitch *= psSoundTimeFactor; //--#SM+#-- Correct sound "speed" by time factor
    clamp(_pitch, EPS_L, 100.f); //--#SM+#-- Increase sound frequency (speed) limit

    if (!fsimilar(_pitch, cache_pitch))
    {
        cache_pitch = _pitch;
        A_CHK(alSourcef(pSource, AL_PITCH, _pitch));
    }
}

void CSoundRender_TargetA::submit_buffer(ALuint BufferID) const
{
    R_ASSERT1_CURE(m_pEmitter, { return; });
    const auto [data, dataSize] = m_pEmitter->obtain_block();
    A_CHK(alBufferData(BufferID, dataFormat, data, static_cast<ALsizei>(dataSize), sampleRate));
}

void CSoundRender_TargetA::submit_all_buffers() const
{
    for (const auto buffer : pBuffers)
        submit_buffer(buffer);
}
