#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"
#include "SoundRender_Target.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

void CSoundRender_Emitter::start(const ref_sound& _owner, u32 flags, float delay)
{
    const bool _loop = flags & sm_Looped;
    bIgnoringTimeFactor = flags & sm_IgnoreTimeFactor;
    starting_delay = delay;

    VERIFY(_owner);
    owner_data = _owner;
    VERIFY(owner_data);
    p_source.position.set(0, 0, 0);

    const auto info = source()->info();
    p_source.min_distance = info.minDist;
    p_source.max_distance = info.maxDist;
    p_source.base_volume = info.baseVolume;
    p_source.volume = 1.f;
    p_source.freq = 1.f;
    p_source.max_ai_distance = info.maxAIDist;

    if (fis_zero(delay, EPS_L))
    {
        m_current_state = _loop ? stStartingLooped : stStarting;
    }
    else
    {
        m_current_state = _loop ? stStartingLoopedDelayed : stStartingDelayed;
        fTimeToPropagade = bIgnoringTimeFactor ? SoundRender->TimerPersistent.GetElapsed_sec() : SoundRender->Timer.GetElapsed_sec();
    }
    bStopping = FALSE;
    bRewind = FALSE;

    // Storage (temp_buf): allocated by alloc_blocks() when the emitter first gets a voice (46-engine-core-render-audit.md).
    // An emitter that only simulates (no voice: ~500 of ~680 in a heavy fight) never needs the 10 prefill blocks
    // (100 ms each, value-initialized as before). The size is the one start() always used.
    m_block_size = source()->data_info().bytesPerBuffer;

    // Lazy ogg open (plans/spawn_optimization/SPEC.md, stage 1), always: do not open the ogg on the thread that starts
    // the sound (main for GO->OnEvent: hits, shots, items). A source with cached PCM never needs it (decompress() reads
    // the cache); a streamed one is opened by its first fill_data(), on the TaskScheduler worker of the prefill - the
    // same lazy open a sound coming back from stSimulating already does. This emitter is new
    // (CSoundRender_Scene::i_play), not yet seen by update()/render(), so ovf is null here.
    if (source()->has_cached_pcm())
        PERF_INC(snd_ogg_open_skipped);
}

void CSoundRender_Emitter::i_stop()
{
    bRewind = FALSE;
    if (target)
        stop_target();

    wait_prefill();
    if (owner_data)
    {
        source()->close(ovf);
        Event_ReleaseOwner();
        VERIFY(this == owner_data->feedback);
        owner_data->feedback = NULL;
        owner_data = NULL;
    }
    m_current_state = stStopped;
}

void CSoundRender_Emitter::stop(bool isDeffered)
{
    if (isDeffered)
        bStopping = TRUE;
    else
    {
        // Any thread (ref_sound::stop, ~CSound): not in the middle of CSoundRender_Core::update()/render() on another
        // one - i_stop() breaks the emitter<->target pair and closes ovf that render()'s prefill reads.
        ScopeLock lock(&SoundRender->m_emitters_lock);
        i_stop();
    }
}

void CSoundRender_Emitter::rewind()
{
    bStopping = FALSE;
    bRewind = TRUE;
}

void CSoundRender_Emitter::pause(bool bVal, int id)
{
    if (bVal)
    {
        if (0 == iPaused)
            iPaused = id;
    }
    else
    {
        if (id == iPaused)
            iPaused = 0;
    }
}

void CSoundRender_Emitter::cancel()
{
    // Msg		("- %10s : %3d[%1.4f] : %s","cancel",dbg_ID,priority(),source->fname);
    switch (m_current_state)
    {
    case stPlaying:
        stop_target();
        m_current_state = stSimulating;
        break;
    case stPlayingLooped:
        stop_target();
        m_current_state = stSimulatingLooped;
        break;
    default: VERIFY2(false, "Non playing ref_sound forced out of render queue"); break;
    }
}

void CSoundRender_Emitter::stop_target()
{
    wait_prefill();
    R_ASSERT1_CURE(target, { return; });
    target->stop();
    target = nullptr;
    source()->close(ovf);
}
