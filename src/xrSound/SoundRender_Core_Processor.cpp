#include "stdafx.h"

#include "xrEngine/Engine.h"
#include "xrEngine/GameFont.h"
#include "xrEngine/PerformanceAlert.hpp"
#include "xrCDB/Intersect.hpp"
#include "SoundRender_Core.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Target.h"
#include "SoundRender_Source.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

// Multithreading audit:
// - `CSoundRender_Core::update` stays on the caller thread: Tracy shows many child zones because each
//   emitter runs `CSoundRender_Emitter::update` (occlusion rays, FSM, etc.) serially.
// - Vorbis `fill_block` already runs on TaskScheduler workers; that does not move this coordinator.
// - Parallel per-emitter `update` is unsafe without a redesign: `i_start` can `cancel()` another emitter
//   (cross-emitter mutation), and the scene emitter list is compacted in-place when sounds stop.
// - Scene ray queries use a thread-local CDB::COLLIDER (`SoundRender_Scene.cpp`) so occlusion is not
//   blocked on a single shared collider if parallel simulation is added later (still need deferred voice
//   assignment + deferred deletes + fixing `get_environment` static fallback, etc.).
// - Target emitters loop is small (hardware voices) — keep serial.

namespace
{
// The emitter a target (voice) plays, or nullptr. A target whose emitter does not own it (emitter->target != T) is
// released here instead of being rendered: CSoundRender_Emitter::render() and update() use emitter->target, so such a
// pair crashed in render() ("emitter->target was nullptr", 2026-10-15). With m_emitters_lock held by update()/render()
// and by every play/stop the pairs stay consistent; this is the guard against a pair broken anyway.
CSoundRender_Emitter* owned_emitter(CSoundRender_Target* T)
{
    CSoundRender_Emitter* E = T->get_emitter();
    if (!E || E->target == T)
        return E;
    static u32 s_reported = 0;
    if (s_reported < 16)
    {
        ++s_reported;
        Msg("! sound: voice %p referenced emitter %p which plays on voice %p; voice released", T, E, E->target);
    }
    T->stop();
    return nullptr;
}
} // namespace

void CSoundRender_Core::update(const Fvector& P, const Fvector& D, const Fvector& N, const Fvector& R)
{
    ZoneScoped;

    if (0 == bReady)
        return;
    ScopeLock lock(&m_emitters_lock);
    Stats.Update.Begin();
    m_locked_by.store(std::this_thread::get_id(), std::memory_order_relaxed);

    if (xray::perf::g_perf_metrics_active)
    {
        s64 busy = 0;
        for (CSoundRender_Target* T : s_targets)
        {
            if (T->get_emitter())
                ++busy;
        }
        xray::perf::SetGauge(xray::perf::Metric::snd_targets_busy, busy);

        s64 n_emitters = 0;
        for (CSoundRender_Scene* scene : m_scenes)
            n_emitters += static_cast<s64>(scene->get_emitters().size());
        xray::perf::SetGauge(xray::perf::Metric::snd_emitters, n_emitters);
    }

    Timer.time_factor(psSoundTimeFactor); //--#SM+#--
    {
        const float new_tm = Timer.GetElapsed_sec();
        fTimer_Delta = new_tm - fTimer_Value;
        fTimer_Value = new_tm;

        const float new_tm_p = TimerPersistent.GetElapsed_sec();
        fTimerPersistent_Delta = new_tm_p - fTimerPersistent_Value;
        fTimerPersistent_Value = new_tm_p;
    }
    s_emitters_u++;

    const auto update_emitter = [this](CSoundRender_Emitter* emitter)
    {
        const bool ignore = emitter->bIgnoringTimeFactor;
        const float time = ignore ? fTimerPersistent_Value : fTimer_Value;
        const float delta = ignore ? fTimerPersistent_Delta : fTimer_Delta;
        emitter->update(time, delta);
        emitter->marker = s_emitters_u;
    };

    // Firstly update emitters, which are now being rendered
    for (CSoundRender_Target* T : s_targets)
    {
        if (CSoundRender_Emitter* E = owned_emitter(T))
        {
            update_emitter(E);
        }
    }

    // Update emitters
    for (CSoundRender_Scene* scene : m_scenes)
    {
        auto& emitters = scene->get_emitters();
        for (u32 it = 0; it < emitters.size(); it++)
        {
            CSoundRender_Emitter* pEmitter = emitters[it];
            if (pEmitter->marker != s_emitters_u)
            {
                update_emitter(pEmitter);
            }
            if (!pEmitter->isPlaying())
            {
                // Stopped
                xr_delete(pEmitter);
                emitters.erase(emitters.begin() + it);
                it--;
            }
        }
    }

    // update listener
    update_listener(P, D, N, R, fTimer_Delta);

    // Events
    for (CSoundRender_Scene* scene : m_scenes)
        scene->update();

    m_locked_by.store(std::thread::id{}, std::memory_order_relaxed);
    Stats.Update.End();
}

void CSoundRender_Core::render()
{
    ZoneScoped;

    // Game/render overlap frame: this runs on the Game logic thread (seqFrameMT) while the physics step and the main
    // thread's render may play/stop sounds - they wait for this loop (m_emitters_lock), so no pair changes under it.
    ScopeLock lock(&m_emitters_lock);
    m_locked_by.store(std::this_thread::get_id(), std::memory_order_relaxed);
    Stats.Render.Begin();

    for (CSoundRender_Target* T : s_targets)
    {
        if (CSoundRender_Emitter* emitter = owned_emitter(T))
        {
            emitter->render();
        }
    }

    Stats.Render.End();
    m_locked_by.store(std::thread::id{}, std::memory_order_relaxed);
}

void CSoundRender_Core::statistic(CSound_stats* dest, CSound_stats_ext* ext)
{
    ScopeLock lock(&m_emitters_lock); // main thread (stats, debug UI) beside the physics step's play_at_pos
    if (dest)
    {
        dest->_rendered = 0;
        dest->_simulated = 0;
        dest->_events = 0;

        for (auto T : s_targets)
        {
            if (T->get_emitter() && T->get_Rendering())
                dest->_rendered++;
        }

        for (CSoundRender_Scene* scene : m_scenes)
        {
            dest->_simulated += scene->get_emitters().size();
            dest->_events += scene->get_prev_events_count();
        }
    }
    if (ext)
    {
        for (CSoundRender_Scene* scene : m_scenes)
        {
            auto& emitters = scene->get_emitters();
            for (const auto emitter : emitters)
            {
                CSound_stats_ext::SItem item;
                item._3D = !emitter->b2D;
                item._rendered = !!emitter->target;
                item.params = emitter->p_source;
                item.volume = emitter->smooth_volume;
                if (emitter->owner_data)
                {
                    item.name = emitter->source()->file_name();
                    item.game_object = emitter->owner_data->g_object;
                    item.game_type = emitter->owner_data->g_type;
                    item.type = emitter->owner_data->s_type;
                }
                else
                {
                    item.game_object = nullptr;
                    item.game_type = 0;
                    item.type = st_Effect;
                }
                ext->append(item);
            }
        }
    }
}

void CSoundRender_Core::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    Stats.FrameEnd();
    CSound_stats sndStat;
    statistic(&sndStat, nullptr);
    font.OutNext("*** SOUND:    %2.2fms", Stats.Update.result);
    font.OutNext("    RENDER:   %2.2fms", Stats.Render.result);
    font.OutNext("Rendered:     %d", sndStat._rendered);
    font.OutNext("Simulated:    %d", sndStat._simulated);
    font.OutNext("Events:       %d", sndStat._events);
    Stats.FrameStart();
}
