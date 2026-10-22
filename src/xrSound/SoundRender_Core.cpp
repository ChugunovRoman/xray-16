#include "stdafx.h"

#include "Include/xrAPI/xrAPI.h"
#include "Common/LevelStructure.hpp"
#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "SoundRender_Emitter.h"

// XXX: old SDK functionality
//#if defined(XR_PLATFORM_WINDOWS)
//#define OPENAL
//#include <eax/eax.h>
//#endif

XRSOUND_API Flags32 psSoundFlags =
{
    ss_Hardware | ss_EFX | ss_UseFloat32
};

XRSOUND_API int psSoundTargets = 32;
XRSOUND_API float psSoundOcclusionScale = 0.5f;
XRSOUND_API float psSoundTimeFactor = 1.0f;
XRSOUND_API float psSoundCull = 0.01f;
XRSOUND_API float psSoundRolloff = 0.75f;
XRSOUND_API u32 psSoundModel = 0;
XRSOUND_API float psSoundVEffects = 1.0f;
XRSOUND_API float psSoundVFactor = 1.0f;

XRSOUND_API float psSoundVMusic = 1.f;
XRSOUND_API int psSoundCacheSizeMB = 96;
XRSOUND_API float psSoundCacheShortSec = 8.0f;
// 34-sound-cold-load.md: the sound prefetch (read ahead) is always on since 2026-10-18 (snd_source_prefetch removed,
// see plan 41)

CSoundRender_Core* SoundRender = nullptr;

CSoundRender_Core::CSoundRender_Core(CSoundManager& p)
    : Parent(p)
{
    bPresent = false;
    s_emitters_u = 0;
    e_current.set_identity();
    e_target.set_identity();
    bReady = false;
    fTimer_Value = Timer.GetElapsed_sec();
    fTimer_Delta = 0.0f;
    fTimerPersistent_Value = TimerPersistent.GetElapsed_sec();
    fTimerPersistent_Delta = 0.0f;
    prefetch_create(); // 34-sound-cold-load.md
}

CSoundRender_Core::~CSoundRender_Core()
{
    prefetch_destroy(); // 34-sound-cold-load.md: the thread is joined before the sources go
}

void CSoundRender_Core::_initialize()
{
    Timer.Start();
    TimerPersistent.Start();

    bPresent = true;

    bReady = true;
}

void CSoundRender_Core::_clear()
{
    bReady = false;

    // remove sources
    ScopeLock scope(&s_sources_lock);
    // 34-sound-cold-load.md: the prefetch thread is stopped and its queue dropped (the next level reads its folders
    // anew). 50j-slow-disk-async-sound.md: under s_sources_lock - a deferred source is queued under it too, so no
    // thread reads one of the sources freed below (the order s_sources_lock -> the prefetch mutex, as there)
    prefetch_shutdown();
    for (auto& kv : s_sources)
    {
        xr_delete(kv.second);
    }
    s_sources.clear();
}

ISoundScene* CSoundRender_Core::create_scene()
{
    return m_scenes.emplace_back(xr_new<CSoundRender_Scene>());
}

void CSoundRender_Core::destroy_scene(ISoundScene*& sound_scene)
{
    m_scenes.erase(std::remove(m_scenes.begin(), m_scenes.end(), sound_scene), m_scenes.end());
    xr_delete(sound_scene);
}

void CSoundRender_Core::stop_emitters()
{
    ScopeLock lock(&m_emitters_lock);
    for (const auto& scene : m_scenes)
        scene->stop_emitters();
}

int CSoundRender_Core::pause_emitters(bool pauseState)
{
    ScopeLock lock(&m_emitters_lock);
    int cnt = 0;
    for (const auto& scene : m_scenes)
        cnt += scene->pause_emitters(pauseState);
    return cnt;
}

void CSoundRender_Core::_restart()
{
    env_apply();
}

CSound* CSoundRender_Core::create(pcstr fName, esound_type sound_type, u32 game_type)
{
    if (!bPresent)
        return nullptr;

    string_path fn;
    xr_strcpy(fn, fName);
    if (strext(fn))
        *strext(fn) = 0;

    CSoundRender_Source* handle = i_create_source(fn);
    if (!handle)
        return nullptr;

    auto* snd = xr_new<CSound>(handle);

    snd->g_type = game_type;
    if (game_type == sg_SourceType)
        snd->g_type = snd->handle->game_type();

    snd->s_type = sound_type;

    snd->dwBytesTotal = snd->handle->bytes_total();
    snd->fTimeTotal = snd->handle->length_sec();

    return snd;
}

// plans/optimization_mt/50j-slow-disk-async-sound.md
CSound* CSoundRender_Core::create_deferred(pcstr fName, esound_type sound_type, u32 game_type)
{
    if (!bPresent)
        return nullptr;

    string_path fn;
    xr_strcpy(fn, fName);
    if (strext(fn))
        *strext(fn) = 0;

    CSoundRender_Source* handle = i_create_source(fn, true);
    if (!handle)
        return nullptr;

    auto* snd = xr_new<CSound>(handle);
    snd->s_type = sound_type;
    if (handle->load_state() == SoundSourceState::Ready)
    {
        snd->g_type = game_type == sg_SourceType ? handle->game_type() : game_type;
        snd->dwBytesTotal = handle->bytes_total();
        snd->fTimeTotal = handle->length_sec();
    }
    else
    {
        // filled by clone() / the start of a play / resolve_deferred; 50z3-startup-slow-disk-io.md: the source's own
        // type (sg_SourceType, the material library and the weather ambients) comes with them
        snd->deferred = true;
        snd->deferred_g_type = game_type == sg_SourceType;
        snd->g_type = snd->deferred_g_type ? sg_Undefined : game_type;
    }

    return snd;
}

void CSoundRender_Core::attach_tail(CSound& snd, pcstr fName)
{
    if (!bPresent)
        return;
    string_path fn;
    xr_strcpy(fn, fName);
    if (strext(fn))
        *strext(fn) = 0;
    if (!snd.fn_attached[0].empty() && !snd.fn_attached[1].empty())
    {
#ifndef MASTER_GOLD
        Msg("! 2 file already in queue [%s][%s]", snd.fn_attached[0].c_str(), snd.fn_attached[1].c_str());
#endif
        return;
    }

    if (!i_resolve_deferred(snd)) // 50j: the lengths below add to those of the sound's own source
        return;

    const u32 idx = snd.fn_attached[0].empty() ? 0 : 1;

    snd.fn_attached[idx] = fn;

    CSoundRender_Source* s = i_create_source(fn);
    snd.dwBytesTotal += s->bytes_total();
    snd.fTimeTotal += s->length_sec();
    ScopeLock lock(&m_emitters_lock); // snd.feedback: an emitter update() may stop and delete meanwhile
    if (snd.feedback)
        ((CSoundRender_Emitter*)snd.feedback)->fTimeToStop += s->length_sec();

    i_destroy_source(s);
}

void CSoundRender_Core::destroy(CSound& S)
{
    if (auto* emitter = (CSoundRender_Emitter*)S.feedback)
    {
        emitter->stop(false);
        VERIFY(S.feedback == nullptr);
    }
    i_destroy_source((CSoundRender_Source*)S.handle);
    S.handle = nullptr;
}

void CSoundRender_Core::env_apply()
{
    /*
    // Force all sounds to change their environment
    // (set their positions to signal changes in environment)
    for (u32 it = 0; it < s_emitters.size(); it++)
    {
        CSoundRender_Emitter* pEmitter = s_emitters[it];
        const CSound_params* pParams = pEmitter->get_params();
        pEmitter->set_position(pParams->position);
    }
    */
    bListenerMoved = true;
}

void CSoundRender_Core::update_listener(const Fvector& P, const Fvector& D, const Fvector& N, const Fvector& R, float dt)
{
    if (!Listener.position.similar(P))
    {
        Listener.position = P;
        bListenerMoved = true;
    }
    Listener.orientation[0] = D;
    Listener.orientation[1] = N;
    Listener.orientation[2] = R;

    if (!psSoundFlags.test(ss_EFX) || !m_effects)
        return;

    // Update effects
    if (bListenerMoved)
    {
        bListenerMoved = false;
        e_target = *(CSoundRender_Environment*)DefaultSoundScene->get_environment(P);
    }

    e_current.lerp(e_current, e_target, fTimer_Delta);

    m_effects->set_listener(e_current);
    m_effects->commit();
}

void CSoundRender_Core::memory_stats(size_t& sources, size_t& pcm_cached_bytes)
{
    {
        ScopeLock sources_lock(&s_sources_lock); // i_create_source of another thread may insert meanwhile
        sources = s_sources.size();
    }
    pcm_cached_bytes = CSoundRender_Source::cached_pcm_bytes_total();
}

void CSoundRender_Core::refresh_sources()
{
    ScopeLock lock(&m_emitters_lock); // no play() between the stop and the reload
    stop_emitters();

    ScopeLock sources_lock(&s_sources_lock); // i_create_source of another thread may insert meanwhile
    // 50j-slow-disk-async-sound.md: no deferred source is read in the background meanwhile; the pending ones are read
    // below with the others
    prefetch_shutdown();
    for (const auto& kv : s_sources)
    {
        CSoundRender_Source* s = kv.second;
        s->unload();
        s->load(s->file_name());
    }
}
