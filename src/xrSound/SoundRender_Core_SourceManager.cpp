#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
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
    std::mutex mutex; // queue, queued, warmed, thread
    std::condition_variable cv;
    std::deque<PrefetchJob> queue;
    std::unordered_set<xr_string> queued; // prefetch_key: each folder once per session
    std::unordered_set<xr_string> warmed; // prefetch_key of the folders the thread has gone through
    std::thread thread;
    std::atomic_bool stop{};
    xr_vector<char> buf; // read buffer, the thread's own
};

CSoundRender_Source* CSoundRender_Core::i_create_source(pcstr name)
{
    // Search
    string256 id;
    xr_strcpy(id, name);
    xr_strlwr(id);
    if (strext(id))
        * strext(id) = 0;

    {
        ScopeLock scope(&s_sources_lock);
        const auto it = s_sources.find(id);
        if (it != s_sources.end())
            return it->second;
    }

    // Load a _new one
    CSoundRender_Source source;
    bool loaded;
    {
        // spawn stage 0 (plans/spawn_optimization/SPEC.md): a new source - LoadWave + PCM cache of a short sound
        ZoneScopedN("snd_source_load");
        ZoneText(id, xr_strlen(id));
        PERF_SCOPE(snd_source_load);
        PERF_SCOPE_IF(xray::perf::TlGameEvent() != 0, ge_cold_sound_load); // 30-game-events.md
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
    }
    P.cv.notify_all();
    // The thread checks `stop` between files: the join waits at most for one file read
    if (P.thread.joinable())
        P.thread.join();
    P.stop.store(false, std::memory_order_relaxed); // a later folder starts the thread again
    xray::perf::SetGauge(xray::perf::Metric::snd_prefetch_queue, 0);
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
    Threading::SetCurrentThreadPriorityLevel(Threading::priority_level::below_normal);
    SourcePrefetch& P = *m_prefetch;
    P.buf.resize(kPrefetchReadChunk);
    for (;;)
    {
        PrefetchJob job;
        size_t pending;
        {
            std::unique_lock<std::mutex> lk(P.mutex);
            P.cv.wait(lk, [&P] { return P.stop.load(std::memory_order_relaxed) || !P.queue.empty(); });
            if (P.stop.load(std::memory_order_relaxed))
                return;
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
