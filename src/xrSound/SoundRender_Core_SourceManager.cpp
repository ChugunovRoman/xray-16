#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "xrCore/BackgroundIo.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_set>

#if defined(XR_PLATFORM_WINDOWS)
#include "xrCore/Text/Utf8Utils.hpp"
#endif

// ---------------------------------------------------------------------------------------------------------------------
// plans/optimization_mt/34-sound-cold-load.md: the sound prefetch. Always on since 2026-10-18 (snd_source_prefetch
// removed, see plan 41; its mode 2 - the folders a script names loaded as sources on this thread - removed with it).
//
// A script picks a sound at random from a folder (taunts, death cries, weapon jams) and creates it inside a game event
// (CLevel::ProcessGameEvents -> Lua): a source never loaded before is loaded right there, on main
// (i_create_source -> LoadWave). The file of such a sound has usually never been read in this session, and the first
// read of a cold file (FS.r_open maps it; ov_open_callbacks reads its headers and seeks to its end) waits for the disk.
// Here a thread of its own reads such folders ahead: it only reads the .ogg files (into a scratch buffer), their pages
// land in the OS file cache, the later load on main reads them from memory. No engine state is touched by the thread
// (std::filesystem + std::ifstream).
// Which folders: the ones a script names (ISoundManager::prefetch_sources, Lua sound_prefetch_dir) and, while a game
// event runs (not a spawn), the folder of every sound loaded cold: the next random pick of that folder is warm.
// Not a TaskScheduler task: a Wait() of main (or of the Game thread) runs queued tasks itself, so a blocking read put
// there could end up on main - the very wait this removes. The thread sleeps on a condition variable when idle.
//
// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R7, R19), a slow disk only (xray::io::BackgroundIoSingle:
// an HDD / USB / network disk by the hw profile, or measured slow at the start-up warm-up):
// - one background I/O queue: every file is read in the slot shared with the "Model prefetch" thread of the render, and
//   only while it has no model job pending (the spawns wait for those) - one read at a time on the disk;
// - a folder with a "*" component a script names (the death cries of every community, ~290 files at once) is not read
//   ahead in bulk; a cold load of a sound of it in a game event reads its own folder (the auto rule above).
// On an SSD / NVMe nothing of this applies.
//
// 50j-slow-disk-async-sound.md (plans/optimization_mt), a slow disk only: the sources created deferred
// (ref_sound::create_deferred - the NPC sound sets, CSoundPlayer) are read on this thread too, before the folders. A
// stalker of a voice not heard yet loaded ~360 sources inside its spawn, on main: 4.5 s on a USB HDD (12.8 ms per
// file: the open, then the vorbis headers and the seek to the end). Now the spawn only queues them; a play takes a
// source that is ready or waits for its own one (queued as urgent) - main does not read them.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
constexpr size_t kPrefetchMaxFilesPerDir = 256; // a folder with more .ogg is not a "random pick" one: skipped
constexpr size_t kPrefetchMaxSubdirs = 128; // at most this many subfolders matched by a "*" component
constexpr u64 kPrefetchMaxFileBytes = 4u << 20; // read at most the first 4 MB of a file (music and alike)
constexpr u64 kPrefetchMaxDirBytes = 32u << 20; // and at most 32 MB of one folder
constexpr size_t kPrefetchReadChunk = 64u << 10;

void lower_ascii(xr_string& s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
}

// The key of a folder in the queued/warmed sets: lower case, '\' separators
xr_string prefetch_key(const xr_string& fs_dir)
{
    xr_string key = fs_dir;
    for (char& c : key)
    {
        if (c == '/')
            c = '\\';
    }
    lower_ascii(key);
    return key;
}

std::filesystem::path os_path(const xr_string& fs_path)
{
#if defined(XR_PLATFORM_WINDOWS)
    // From UTF-8 here, not through XRay::Utf8::ToWide: its std::wstring is allocated by the operator new of xrCore
    // (mimalloc) and would be freed by the CRT of this module - a heap corruption (c0000374)
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(fs_path.c_str()), fs_path.size()));
#else
    std::string p(fs_path.c_str());
    for (char& c : p)
    {
        if (c == '\\')
            c = '/';
    }
    return std::filesystem::path(p);
#endif
}

xr_string utf8_file_name(const std::filesystem::path& p)
{
#if defined(XR_PLATFORM_WINDOWS)
    return XRay::Utf8::FromWide(p.filename().c_str());
#else
    return xr_string(p.filename().c_str());
#endif
}

bool is_ogg(const std::filesystem::path& p)
{
    const std::filesystem::path ext_path = p.extension();
    const auto& ext = ext_path.native();
    if (ext.size() != 4 || ext[0] != '.')
        return false;
    const auto lc = [](auto c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; };
    return lc(ext[1]) == 'o' && lc(ext[2]) == 'g' && lc(ext[3]) == 'g';
}

// Reads the file (at most kPrefetchMaxFileBytes) and throws the bytes away: they stay in the OS file cache.
u64 warm_file(const std::filesystem::path& p, xr_vector<char>& buf)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return 0;
    u64 total = 0;
    while (total < kPrefetchMaxFileBytes)
    {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0)
            break;
        total += static_cast<u64>(n);
    }
    return total;
}
} // namespace

struct CSoundRender_Core::PrefetchJob
{
    xr_string fs_dir; // FS path of the folder with the trailing separator; may contain one "*" component
    bool auto_sibling{}; // the folder of a sound loaded cold inside a game event
};

struct CSoundRender_Core::SourcePrefetch
{
    std::mutex mutex; // queue, queued, warmed, thread, sources, urgent
    std::condition_variable cv;
    std::deque<PrefetchJob> queue;
    // 50j: the deferred sources to read (in s_sources, alive until _clear, which stops the thread first). A source may
    // be in both (urgent later): the second time it is not pending any more and is skipped.
    std::deque<CSoundRender_Source*> sources;
    std::deque<CSoundRender_Source*> urgent;
    std::atomic<u32> sources_waiting{0}; // sources.size() + urgent.size(), read without the mutex
    std::atomic<u32> urgent_waiting{0};  // urgent.size(), read without the mutex
    std::unordered_set<xr_string> queued; // prefetch_key: each folder once per session
    std::unordered_set<xr_string> warmed; // prefetch_key of the folders the thread has gone through
    std::thread thread;
    std::atomic_bool stop{};
    xr_vector<char> buf; // read buffer, the thread's own
};

CSoundRender_Source* CSoundRender_Core::i_create_source(pcstr name, bool deferred)
{
    // Search
    string256 id;
    xr_strcpy(id, name);
    xr_strlwr(id);
    if (strext(id))
        * strext(id) = 0;

    CSoundRender_Source* found = nullptr;
    {
        ScopeLock scope(&s_sources_lock);
        const auto it = s_sources.find(id);
        if (it != s_sources.end())
            found = it->second;
    }
    if (found)
    {
        // 50j: created deferred before and still pending - this caller needs it now (outside s_sources_lock: it may
        // wait for the thread)
        if (!deferred)
            found->ensure_ready();
        return found->load_state() == SoundSourceState::Failed ? nullptr : found;
    }

    if (deferred && i_defer_sources())
        return i_create_deferred_source(id); // 50j-slow-disk-async-sound.md

    // Load a _new one
    CSoundRender_Source source;
    bool loaded;
    {
        // spawn stage 0 (plans/spawn_optimization/SPEC.md): a new source - LoadWave + PCM cache of a short sound
        ZoneScopedN("snd_source_load");
        ZoneText(id, xr_strlen(id));
        PERF_SCOPE(snd_source_load);
        PERF_SCOPE_IF(xray::perf::TlGameEvent() != 0, ge_cold_sound_load); // 30-game-events.md
        GW_FRAME_EVENT_IF(xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker, SoundCold); // 50n: the frame spike report
        loaded = source.load(id);
    }
    if (loaded)
    {
        prefetch_after_cold_load(source); // 34-sound-cold-load.md: counters, read its folder ahead

        ScopeLock scope(&s_sources_lock);
        // Two threads may load the same id at once (main and the Game thread): the first one in keeps its source, a
        // later copy is freed with `source` (its PCM budget given back) instead of being returned unregistered and
        // leaked.
        const auto [it, inserted] = s_sources.try_emplace(id, nullptr);
        if (inserted)
            it->second = xr_new<CSoundRender_Source>(std::move(source));
        else
            PERF_INC(snd_source_load_dup);
        return it->second;
    }

    return nullptr;
}

void CSoundRender_Core::i_destroy_source(CSoundRender_Source* S)
{
    // No actual destroy at all
}

// ---------------------------------------------------------------------------------------------------------------------
// 50j-slow-disk-async-sound.md: deferred sources
// ---------------------------------------------------------------------------------------------------------------------

bool CSoundRender_Core::i_defer_sources() const { return m_prefetch && xray::io::BackgroundIoSingle(); }

CSoundRender_Source* CSoundRender_Core::i_create_deferred_source(pcstr id)
{
    // The path as load() takes it ($level$ first) is found here, by the creating thread: only the read is left to the
    // prefetch thread
    auto* source = xr_new<CSoundRender_Source>();
    if (!source->prepare_deferred(id))
    {
        xr_delete(source);
        return nullptr; // no such file, as a load
    }

    CSoundRender_Source* result;
    {
        ScopeLock scope(&s_sources_lock);
        const auto [it, inserted] = s_sources.try_emplace(id, source);
        result = it->second;
        // Queued under s_sources_lock: _clear and refresh_sources stop the thread under it, so the thread never gets a
        // source they free or reload
        if (inserted)
            prefetch_enqueue_source(source, false);
    }
    if (result != source)
    {
        xr_delete(source); // another thread inserted it meanwhile
        return result->load_state() == SoundSourceState::Failed ? nullptr : result;
    }

    PERF_INC(snd_async_created);
    static std::atomic_bool s_logged{false};
    if (!s_logged.exchange(true, std::memory_order_relaxed))
    {
        // 50z3-startup-slow-disk-io.md: the first one comes at the start-up now (the material library, the weather
        // ambients), so the line shows the path is on even if no NPC of a new voice appears in the game
        Msg("* [hw] sound: slow disk -> async source loading (the sounds of the start-up and the NPC sound sets in game "
            "are read on the Sound prefetch thread; a play of an NPC waits for its source at most %u ms, then it is "
            "dropped)%s",
            snd_deferred_max_delay_ms,
            xray::io::BackgroundIoMeasuredSlow() ? " (measured at the start-up warm-up)" : "");
    }
    return source;
}

// 50z3-startup-slow-disk-io.md: the start of the first level load (IGame_Persistent, main) - the sources still pending
// are read here. Pending ones then are the sounds of the start-up the menu did not read (the NPC sound sets come only in
// game). The queue of the thread keeps them: it skips a source that is not pending any more.
u32 CSoundRender_Core::read_pending_sources()
{
    xr_vector<CSoundRender_Source*> pending;
    {
        ScopeLock scope(&s_sources_lock);
        for (const auto& kv : s_sources)
        {
            if (kv.second->load_state() == SoundSourceState::Pending)
                pending.push_back(kv.second);
        }
    }
    if (pending.empty())
        return 0;
    // Alive until _clear, which is main's: this thread
    CTimer timer;
    timer.Start();
    u32 failed = 0;
    for (CSoundRender_Source* source : pending)
    {
        source->ensure_ready(); // read here, or wait for the thread in the middle of it
        failed += source->load_state() == SoundSourceState::Failed ? 1 : 0;
    }
    Msg("* [hw] sound: %u sources of the start-up were not read at the menu - read at the start of the level load in "
        "%u ms (%u failed)",
        static_cast<u32>(pending.size()), timer.GetElapsed_ms(), failed);
    return static_cast<u32>(pending.size());
}

SoundSourceState CSoundRender_Core::source_state(const CSound& S, bool urgent)
{
    const auto* source = static_cast<const CSoundRender_Source*>(S.handle);
    if (!source)
        return SoundSourceState::Failed;
    const SoundSourceState state = source->load_state();
    // A play waits for it: in front of the other deferred sources, once (a waiting play asks every frame)
    if (urgent && state == SoundSourceState::Pending && source->mark_urgent())
    {
        PERF_INC(snd_async_urgent);
        prefetch_enqueue_source(const_cast<CSoundRender_Source*>(source), true);
    }
    return state;
}

bool CSoundRender_Core::i_resolve_deferred(CSound& S)
{
    if (!S.deferred)
        return true;
    const auto* source = static_cast<const CSoundRender_Source*>(S.handle);
    source->ensure_ready(); // normally a no-op: CSoundPlayer starts it when ready; otherwise counted (sound/async/sync)
    if (source->load_state() == SoundSourceState::Failed)
        return false;
    S.dwBytesTotal = source->bytes_total();
    S.fTimeTotal = source->length_sec();
    if (S.deferred_g_type)
        S.g_type = source->game_type();
    S.deferred = false;
    S.deferred_g_type = false;
    return true;
}

void CSoundRender_Core::prefetch_enqueue_source(CSoundRender_Source* source, bool urgent)
{
    if (!m_prefetch)
        return;
    SourcePrefetch& P = *m_prefetch;
    u32 waiting;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        if (P.stop.load(std::memory_order_relaxed))
        {
            source->clear_urgent();
            return; // shutting down: a caller that needs it reads it itself (ensure_ready)
        }
        if (urgent)
        {
            P.urgent.push_back(source);
            P.urgent_waiting.fetch_add(1, std::memory_order_relaxed);
        }
        else
            P.sources.push_back(source);
        waiting = P.sources_waiting.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!P.thread.joinable())
            P.thread = Threading::RunThread("Sound prefetch", &CSoundRender_Core::prefetch_thread_proc, this);
    }
    P.cv.notify_one();
    xray::perf::SetGauge(xray::perf::Metric::snd_async_queue, static_cast<s64>(waiting));
}

bool CSoundRender_Core::prefetch_sources_waiting() const
{
    return m_prefetch && m_prefetch->sources_waiting.load(std::memory_order_relaxed) != 0;
}

void CSoundRender_Core::prefetch_serve_sources()
{
    SourcePrefetch& P = *m_prefetch;
    const auto stopping = [&P] { return P.stop.load(std::memory_order_relaxed); };
    for (;;)
    {
        CSoundRender_Source* source;
        bool urgent;
        u32 waiting;
        {
            std::lock_guard<std::mutex> lk(P.mutex);
            if (stopping())
                return;
            urgent = !P.urgent.empty();
            std::deque<CSoundRender_Source*>& from = urgent ? P.urgent : P.sources;
            if (from.empty())
                return;
            source = from.front();
            from.pop_front();
            if (urgent)
                P.urgent_waiting.fetch_sub(1, std::memory_order_relaxed);
            waiting = P.sources_waiting.fetch_sub(1, std::memory_order_relaxed) - 1;
        }
        xray::perf::SetGauge(xray::perf::Metric::snd_async_queue, static_cast<s64>(waiting));
        if (source->load_state() != SoundSourceState::Pending)
            continue; // read by a caller meanwhile (ensure_ready), or a second entry of it

        // 50i (R7) slot: an urgent source only waits for the read in progress (as a model job); the others also wait
        // for the model jobs, and give way to an urgent one that comes meanwhile
        xray::io::BackgroundIoScope io;
#if XR_PERF_METRICS
        const u64 t_io = xray::perf::g_perf_metrics_active ? xray::perf::Ticks() : 0;
#endif
        const bool entered = urgent ?
            io.enter(xray::io::BackgroundIoClient::Model, stopping) :
            io.enter(xray::io::BackgroundIoClient::Sound,
                [&P, &stopping] { return stopping() || P.urgent_waiting.load(std::memory_order_relaxed) != 0; });
        if (!entered)
        {
            if (stopping())
                return;
            // an urgent one came: this one goes back to the head of its queue
            std::lock_guard<std::mutex> lk(P.mutex);
            P.sources.push_front(source);
            P.sources_waiting.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
#if XR_PERF_METRICS
        if (io.taken() && t_io)
            xray::perf::AddTimeSlow(xray::perf::Metric::snd_prefetch_io_wait, xray::perf::Ticks() - t_io);
#endif
        if (!source->begin_load())
            continue; // a caller took it meanwhile
        {
            ZoneScopedN("snd_async_load");
            ZoneText(source->file_name(), xr_strlen(source->file_name()));
            PERF_SCOPE(snd_async_load);
            source->finish_load(true);
        }
        if (source->load_state() == SoundSourceState::Ready)
            PERF_INC(snd_async_loaded);
        else
            PERF_INC(snd_async_failed);
    }
}

void CSoundRender_Core::prefetch_create()
{
    m_prefetch = xr_new<SourcePrefetch>();
}

void CSoundRender_Core::prefetch_shutdown()
{
    if (!m_prefetch)
        return;
    SourcePrefetch& P = *m_prefetch;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        P.stop.store(true, std::memory_order_relaxed);
        P.queue.clear();
        P.queued.clear();
        // 50j: the deferred sources not read stay pending - _clear frees them, refresh_sources reads them itself, a
        // later caller that needs one reads it (ensure_ready) or queues it as urgent again
        for (CSoundRender_Source* source : P.urgent)
            source->clear_urgent();
        P.urgent.clear();
        P.sources.clear();
        P.urgent_waiting.store(0, std::memory_order_relaxed);
        P.sources_waiting.store(0, std::memory_order_relaxed);
    }
    P.cv.notify_all();
    // The thread checks `stop` between files: the join waits at most for one file read
    if (P.thread.joinable())
        P.thread.join();
    P.stop.store(false, std::memory_order_relaxed); // a later folder starts the thread again
    xray::perf::SetGauge(xray::perf::Metric::snd_prefetch_queue, 0);
    xray::perf::SetGauge(xray::perf::Metric::snd_async_queue, 0);
}

void CSoundRender_Core::prefetch_destroy()
{
    prefetch_shutdown();
    xr_delete(m_prefetch);
}

void CSoundRender_Core::prefetch_sources(pcstr folder)
{
    if (!bPresent || !m_prefetch || !folder || !folder[0])
        return;

    string_path rel;
    xr_strcpy(rel, folder);
    const size_t len = xr_strlen(rel);
    if (rel[len - 1] != '\\' && rel[len - 1] != '/')
        xr_strcat(rel, "\\");

    // 50i (R19): no bulk "*" folder on a slow disk - the auto rule reads the subfolders that are really used
    if (strchr(rel, '*') && xray::io::BackgroundIoSingle())
    {
        PERF_INC(snd_prefetch_bulk_skipped);
        Msg("* [snd_prefetch] slow disk: '%s' is not read ahead in bulk, its folders are read when a sound of them is "
            "loaded cold", rel);
        return;
    }

    string_path fs_dir;
    FS.update_path(fs_dir, "$game_sounds$", rel);
    prefetch_enqueue(fs_dir, false);
}

void CSoundRender_Core::prefetch_enqueue(xr_string fs_dir, bool auto_sibling)
{
    if (!m_prefetch)
        return;
    SourcePrefetch& P = *m_prefetch;
    xr_string key = prefetch_key(fs_dir);
    size_t pending;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        if (P.stop.load(std::memory_order_relaxed))
            return;
        if (!P.queued.insert(std::move(key)).second)
            return; // queued or done already
        P.queue.push_back(PrefetchJob{ std::move(fs_dir), auto_sibling });
        pending = P.queue.size();
        if (!P.thread.joinable())
            P.thread = Threading::RunThread("Sound prefetch", &CSoundRender_Core::prefetch_thread_proc, this);
    }
    P.cv.notify_one();
    PERF_INC(snd_prefetch_queued);
    PERF_ADD_IF(auto_sibling, snd_prefetch_auto, 1);
    xray::perf::SetGauge(xray::perf::Metric::snd_prefetch_queue, static_cast<s64>(pending));
}

void CSoundRender_Core::prefetch_after_cold_load(const CSoundRender_Source& source)
{
    if (!m_prefetch)
        return;
    const xr_string path = source.path_name();
    const size_t sep = path.find_last_of("\\/");
    if (sep == xr_string::npos)
        return;
    xr_string dir = path.substr(0, sep + 1);

    bool warmed;
    {
        std::lock_guard<std::mutex> lk(m_prefetch->mutex);
        warmed = m_prefetch->warmed.count(prefetch_key(dir)) != 0;
    }
    const bool in_event = xray::perf::TlGameEvent() != 0;
    // hit = this cold load found its folder already read ahead (its file in the OS cache), miss = not
    if (warmed)
        PERF_INC(snd_prefetch_warm_hit);
    else
        PERF_INC(snd_prefetch_warm_miss);
    PERF_ADD_IF(in_event && warmed, ge_cold_sound_warm_hit, 1);
    PERF_ADD_IF(in_event && !warmed, ge_cold_sound_warm_miss, 1);

    // A sound picked at random from a folder by a script: the next pick of that folder is read ahead. A spawn (NPC
    // sound collections, item sounds) loads whole sets at once - its folders are not taken.
    if (!warmed && in_event && xray::perf::TlSpawn() == 0)
        prefetch_enqueue(std::move(dir), true);
}

void CSoundRender_Core::prefetch_thread_proc()
{
    // 50-smart-engine-hw-profile.md R0 (plans/optimization_mt): below normal CPU priority, and the background mode on
    // top of it - low I/O priority (these reads queue behind the page faults of main and the Game thread) and low memory
    // priority (acceptable: the thread only reads files ahead). Where the mode is not there, below normal alone.
    Threading::SetCurrentThreadPriorityLevel(Threading::priority_level::below_normal);
    if (!Threading::SetCurrentThreadBackgroundMode())
        Msg("~ [snd_prefetch] no background mode for the thread, below normal priority only");
    SourcePrefetch& P = *m_prefetch;
    P.buf.resize(kPrefetchReadChunk);
    for (;;)
    {
        PrefetchJob job;
        size_t pending;
        {
            std::unique_lock<std::mutex> lk(P.mutex);
            P.cv.wait(lk, [&P] {
                return P.stop.load(std::memory_order_relaxed) || !P.queue.empty() || !P.urgent.empty() ||
                    !P.sources.empty();
            });
            if (P.stop.load(std::memory_order_relaxed))
                return;
            if (!P.urgent.empty() || !P.sources.empty())
            {
                lk.unlock();
                prefetch_serve_sources(); // 50j: the deferred sources go before the folders
                continue;
            }
            job = std::move(P.queue.front());
            P.queue.pop_front();
            pending = P.queue.size();
        }
        xray::perf::SetGauge(xray::perf::Metric::snd_prefetch_queue, static_cast<s64>(pending));
        prefetch_run(job);
    }
}

void CSoundRender_Core::prefetch_run(const PrefetchJob& job)
{
    ZoneScopedN("snd_prefetch_folder");
    ZoneText(job.fs_dir.c_str(), job.fs_dir.size());
    PERF_SCOPE(snd_prefetch_time);

    SourcePrefetch& P = *m_prefetch;

    // The folders of the job: one, or every subfolder matched by its "*" component
    xr_vector<xr_string> folders;
    const size_t star = job.fs_dir.find('*');
    if (star == xr_string::npos)
        folders.push_back(job.fs_dir);
    else
    {
        const xr_string fs_pre = job.fs_dir.substr(0, star);
        const xr_string fs_post = job.fs_dir.substr(star + 1);
        std::error_code ec;
        for (std::filesystem::directory_iterator it(os_path(fs_pre), ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code ec_entry;
            if (!it->is_directory(ec_entry))
                continue;
            if (folders.size() >= kPrefetchMaxSubdirs)
                break;
            folders.push_back(fs_pre + utf8_file_name(it->path()) + fs_post);
        }
    }

    xr_vector<std::filesystem::path> files;
    for (const xr_string& folder : folders)
    {
        if (P.stop.load(std::memory_order_relaxed))
            return;

        files.clear();
        bool too_many = false;
        std::error_code ec;
        std::filesystem::directory_iterator it(os_path(folder), ec);
        if (ec)
        {
            PERF_INC(snd_prefetch_dir_missing); // no such folder (a "*" match without it, a sound in a .db archive)
            continue;
        }
        for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec))
        {
            std::error_code ec_entry;
            if (!it->is_regular_file(ec_entry) || !is_ogg(it->path()))
                continue;
            if (files.size() >= kPrefetchMaxFilesPerDir)
            {
                too_many = true;
                break;
            }
            if (it->file_size(ec_entry) == 0 || ec_entry)
                continue;
            files.push_back(it->path());
        }
        if (too_many)
        {
            PERF_INC(snd_prefetch_dir_skipped);
            continue;
        }

        u64 dir_bytes = 0;
        for (const std::filesystem::path& file : files)
        {
            if (P.stop.load(std::memory_order_relaxed))
                return;
            if (dir_bytes >= kPrefetchMaxDirBytes)
                break;
            // 50j: the deferred sources queued meanwhile go first (a play may wait for one)
            prefetch_serve_sources();
            // 50i (R7): on a slow disk the file waits for the single background I/O slot - after the model jobs
            xray::io::BackgroundIoScope io;
#if XR_PERF_METRICS
            const u64 t_io = xray::perf::g_perf_metrics_active ? xray::perf::Ticks() : 0;
#endif
            const auto quit = [this, &P] {
                return P.stop.load(std::memory_order_relaxed) || prefetch_sources_waiting();
            };
            bool entered;
            for (;;)
            {
                entered = io.enter(xray::io::BackgroundIoClient::Sound, quit);
                if (entered || P.stop.load(std::memory_order_relaxed))
                    break;
                prefetch_serve_sources(); // 50j: they came while this file waited for the slot
            }
            if (!entered)
                return;
#if XR_PERF_METRICS
            if (io.taken() && t_io)
                xray::perf::AddTimeSlow(xray::perf::Metric::snd_prefetch_io_wait, xray::perf::Ticks() - t_io);
#endif
            ZoneScopedN("snd_prefetch_read");
            const u64 n = warm_file(file, P.buf);
            dir_bytes += n;
            if (n != 0)
            {
                PERF_INC(snd_prefetch_files);
                PERF_ADD(snd_prefetch_kb, (n + 1023) / 1024);
            }
        }

        std::lock_guard<std::mutex> lk(P.mutex);
        P.warmed.insert(prefetch_key(folder));
    }
}
