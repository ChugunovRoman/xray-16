#pragma once

#include "xrCommon/xr_unordered_map.h"

#include <atomic>
#include <thread>

#include "SoundRender.h"
#include "SoundRender_Environment.h"
#include "SoundRender_Effects.h"
#include "SoundRender_Scene.h"

class CSoundRender_Core : public ISoundManager
{
protected:
    struct SoundStatistics
    {
        CStatTimer Update;
        CStatTimer Render;

        SoundStatistics() { FrameStart(); }
        void FrameStart() { Update.FrameStart(); Render.FrameStart(); }
        void FrameEnd() { Update.FrameEnd(); Render.FrameEnd(); }
    };

private:
    // The thread inside update()/render() (under m_emitters_lock), or the empty id. i_locked() (VerSndUnlocked)
    // flags only a call made from inside them on that same thread; other threads just wait for m_emitters_lock.
    std::atomic<std::thread::id> m_locked_by{};

public:
    struct SListener
    {
        Fvector position;
        Fvector orientation[3];

        [[nodiscard]]
        SListener ToRHS() const
        {
            return
            {
                { position.x, position.y, -position.z },
                {
                    { orientation[0].x, orientation[0].y, -orientation[0].z },
                    { orientation[1].x, orientation[1].y, -orientation[1].z },
                    { orientation[2].x, orientation[2].y, -orientation[2].z },
                },
            };
        }
    };

protected:
    SListener Listener;

    bool bListenerMoved{};

    CSoundRender_Environment e_current;
    CSoundRender_Environment e_target;
    SoundStatistics Stats;

public:
    CSoundManager& Parent;

    bool bPresent;
    bool bReady;

    // Emitters, their scene lists and the emitter<->target pairs (plans/optimization_mt/16-b2-overlap-races.md,
    // "CSoundRender_Emitter::render: target == nullptr"). In a game/render overlap frame update() runs on main, but
    // render() runs on the Game logic thread, beside the physics step (ODE contact callbacks -> CPHSoundPlayer ->
    // play_at_pos) and the main thread's render: play/stop come from several threads at once. Recursive: a sound
    // call nested in update()/render() on the same thread (wait_prefill() runs queued tasks) does not deadlock.
    Lock m_emitters_lock;

    CTimer Timer;
    CTimer TimerPersistent; // time-factor is always 1.0f
    float fTimer_Value;
    float fTimer_Delta;
    float fTimerPersistent_Value;
    float fTimerPersistent_Delta;

protected:
    // Containers
    xr_vector<CSoundRender_Scene*> m_scenes;

    Lock s_sources_lock;
    xr_unordered_map<xr_string, CSoundRender_Source*> s_sources;

    // 34-sound-cold-load.md (always on since 2026-10-18, see plan 41): the queue and the thread of the sound prefetch,
    // SoundRender_Core_SourceManager.cpp. Created in the constructor, the thread on the first folder.
    struct SourcePrefetch;
    struct PrefetchJob;
    SourcePrefetch* m_prefetch{};
    void prefetch_create();
    void prefetch_shutdown(); // stops and joins the thread (before the sources are freed); the queue is dropped
    void prefetch_destroy();
    void prefetch_enqueue(xr_string os_dir, bool auto_sibling);
    void prefetch_after_cold_load(const CSoundRender_Source& source);
    void prefetch_thread_proc();
    void prefetch_run(const PrefetchJob& job);

    // plans/optimization_mt/50j-slow-disk-async-sound.md: the deferred sources (a slow disk) are read on the same
    // thread, before the folders: the urgent ones (a play waits) first, then the others in order of creation
    bool i_defer_sources() const; // a slow disk and the prefetch is there
    CSoundRender_Source* i_create_deferred_source(pcstr id);
    void prefetch_enqueue_source(CSoundRender_Source* source, bool urgent);
    bool prefetch_sources_waiting() const;
    void prefetch_serve_sources(); // the thread: reads the deferred sources queued, returns when none is left (or stop)

    u32 s_emitters_u; // emitter update marker
    xr_vector<CSoundRender_Target*> s_targets;

    CSoundRender_Effects* m_effects{};

public:
    bool supports_float_pcm{};

public:
    CSoundRender_Core(CSoundManager& p);
    ~CSoundRender_Core() override;

    // General
    virtual void _initialize_devices_list() = 0;
    virtual void _initialize() = 0;
    virtual void _clear() = 0;

    ISoundScene* create_scene() override;
    void destroy_scene(ISoundScene*&) override;

    void _restart() override;

    // Sound interface
    CSound* create(pcstr fName, esound_type sound_type, u32 game_type) override;
    CSound* create_deferred(pcstr fName, esound_type sound_type, u32 game_type) override; // 50j
    void attach_tail(CSound& S, pcstr fName) override;

    void destroy(CSound& S) override;

    void stop_emitters() override;
    int pause_emitters(bool pauseState) override;

    void set_master_volume(float f) override = 0;

    void update(const Fvector& P, const Fvector& D, const Fvector& N, const Fvector& R) override;
    void render() override;
    void statistic(CSound_stats* dest, CSound_stats_ext* ext) override;
    void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert) override;

    // listener
    const auto& listener_params() const { return Listener; }
    const Fvector& listener_position() override { return Listener.position; }
    virtual void update_listener(const Fvector& P, const Fvector& D, const Fvector& N, const Fvector& R, float dt);

    void refresh_sources() override;
    void prefetch_sources(pcstr folder) override;
    void memory_stats(size_t& sources, size_t& pcm_cached_bytes) override; // 50c-commit-growth-analysis.md
    SoundSourceState source_state(const CSound& S, bool urgent) override; // 50j-slow-disk-async-sound.md
    u32 read_pending_sources() override; // 50z3-startup-slow-disk-io.md
    bool resolve_deferred(CSound& S) override { return i_resolve_deferred(S); } // 50z3

public:
    // deferred (50j): on a slow disk a source not loaded yet is created pending and read in the background
    CSoundRender_Source* i_create_source(pcstr name, bool deferred = false);
    // 50j: before a play / a tail of S - a deferred sound gets its numbers from its source (read here if it still
    // waits). false = the source could not be read
    bool i_resolve_deferred(CSound& S);

    void i_destroy_source(CSoundRender_Source* S);
    void i_start(CSoundRender_Emitter* E) const;
    bool i_allow_play(const CSoundRender_Emitter* E);
    bool i_locked() override { return m_locked_by.load(std::memory_order_relaxed) == std::this_thread::get_id(); }

    void env_apply();
};

extern CSoundRender_Core* SoundRender;
