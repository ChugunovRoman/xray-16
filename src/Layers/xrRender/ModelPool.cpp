#include "stdafx.h"

#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Threading/Lock.hpp"
#include "xrCore/Threading/ScopeLock.hpp"
#include "xrCore/Threading/ThreadUtil.h"
#include "xrCore/Platform/HwProfile.h"
#include "xrCore/BackgroundIo.h"
#include "xrEngine/device.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#if defined(XR_PLATFORM_WINDOWS)
#include "xrCore/Text/Utf8Utils.hpp"
#endif

#include "ModelPool.h"

#include "xrMaterialSystem/GameMtlLib.h"

#ifndef _EDITOR
#include "xrEngine/IGame_Persistent.h"
#include "xrCore/FMesh.hpp"
#include "FHierrarhyVisual.h"
#include "SkeletonAnimated.h"
#include "FVisual.h"
#include "FProgressive.h"
#include "FSkinned.h"
#include "FLOD.h"
#include "FTreeVisual.h"
#include "ParticleGroup.h"
#include "ParticleEffect.h"
#else
#include "FMesh.h"
#include "FVisual.h"
#include "FProgressive.h"
#include "ParticleEffect.h"
#include "ParticleGroup.h"
#include "FSkinned.h"
#include "FHierrarhyVisual.h"
#include "SkeletonAnimated.h"
#include "IGame_Persistent.h"
#endif

extern ENGINE_API std::atomic<bool> g_bRendering;

namespace
{
// Spawn stage 0 (plans/spawn_optimization/SPEC.md): the level / save is still loading (loading screen, precache frames)
bool modelpool_loading() { return load_screen_renderer.IsActive() || Device.dwPrecacheFrame > 0; }
} // namespace

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// GAME_RENDER_OVERLAP_PLAN P1.6: CModelPool had no lock at all in this fork (not "don't remove the
// existing one" as the plan's wording from the IXray side implies - there was never one here).
// Registry/Pool/Models/ModelsToDelete/bForceDiscard are mutated from GameThread (GameObject.cpp's
// cNameVisual_set, Weapon.cpp, player_hud.cpp, ParticleGroup.cpp and others - see GW_OVERLAP_HAZARD
// hits already recorded on Create/CreateChild/Delete/DeleteInternal/Instance_Duplicate). The call
// graph is recursive (Create -> Instance_Duplicate; Destroy/DeleteQueue/ClearPool -> DeleteInternal
// -> Discard), so this needs the same recursive-lock-with-depth-guard shape as UCalc_mtlock
// (Layers/xrRender/SkeletonCustom.h) - copied here almost verbatim, with its own metric IDs so the
// two don't get confused in a 5s perf_metrics dump.
Lock ModelPool_Mutex;
thread_local int g_modelpool_lock_depth = 0;

struct ModelPool_mtlock
{
    u64 t_hold0{};
    bool outer{};

    ModelPool_mtlock()
    {
        outer = (g_modelpool_lock_depth == 0);
        ++g_modelpool_lock_depth;
        if (!ModelPool_Mutex.TryEnter())
        {
            PERF_INC(modelpool_lock_contend);
            const u64 t0 = xray::perf::Ticks();
            ModelPool_Mutex.Enter();
            const u64 waited = xray::perf::Ticks() - t0;
#if XR_PERF_METRICS
            if (xray::perf::g_perf_metrics_active)
                xray::perf::AddTimeSlow(xray::perf::Metric::modelpool_lock_wait, waited);
#endif
            // 50n: the frame spike report - main / Game waited for the pool held by another thread (a loader)
            if (xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker)
                xray::perf::FrameEventAdd(xray::perf::frame_event::LockWait, waited);
        }
#if XR_PERF_METRICS
        if (outer && xray::perf::g_perf_metrics_active)
            t_hold0 = xray::perf::Ticks();
#endif
    }

    ~ModelPool_mtlock()
    {
        if (outer && xray::perf::g_perf_metrics_active)
            xray::perf::AddTimeSlow(xray::perf::Metric::modelpool_lock_hold, xray::perf::Ticks() - t_hold0);
        ModelPool_Mutex.Leave();
        --g_modelpool_lock_depth;
    }
};

// Live ModelPoolDeferDeletes scopes, see ModelPool.h
std::atomic<int> s_modelpool_defer_deletes{0};

// ---------------------------------------------------------------------------------------------------------------------
// 36-spawn-model-load.md (plans/optimization_mt)

// Depth of the cold model loads (Create loading an ogf) of this thread: MODEL_LOAD_PART is active only inside one
thread_local int g_modelpool_cold_depth = 0;

struct ModelColdLoadScope
{
    ModelColdLoadScope()
    {
        // 50i-stage3-slow-disk-io.md: the time of the outermost load of this thread goes to TlColdModelUs (the
        // spawn_cold_ms limit of the spawn per frame)
        if (g_modelpool_cold_depth++ == 0)
        {
            t0 = std::chrono::steady_clock::now();
            ticks0 = xray::perf::Ticks();
        }
        ++xray::perf::TlColdModelLoads(); // spawn_cold_models (CLevel::ProcessGameEvents) counts these
    }
    ~ModelColdLoadScope()
    {
        if (--g_modelpool_cold_depth == 0)
        {
            xray::perf::TlColdModelUs() += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
            // 50n: the frame spike report - the outermost cold loads of main / Game (not the prefetch threads)
            if (xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker)
                xray::perf::FrameEventAdd(xray::perf::frame_event::ModelCold, xray::perf::Ticks() - ticks0);
        }
    }
    ModelColdLoadScope(const ModelColdLoadScope&) = delete;
    ModelColdLoadScope& operator=(const ModelColdLoadScope&) = delete;

private:
    std::chrono::steady_clock::time_point t0{};
    u64 ticks0{};
};

// Reading the files of a model into the OS file cache: model_PrefetchFiles (the "Model prefetch" thread) and the
// start-up warm threads of Prefetch. Plain C++ file I/O plus the engine's IReader over a private
// buffer: no FS index, no pool, no render resources, so it is safe on any thread and with any render. The engine opens
// the same files later through FS (a file from 16 KB is mapped and read through page faults): their pages are in memory
// by then.
constexpr u64 kWarmMaxModelBytes = 128ull * 1024 * 1024; // all the files of one model
constexpr size_t kWarmMaxOgfBytes = 64 * 1024 * 1024;
constexpr size_t kWarmChunk = 256 * 1024;
// 50a-stage1-prefetch.md (plans/optimization_mt, plan 50 R4): how far the start-up warm threads read ahead of the loads
// is in bytes now - PrefetchWarmLimits::lead_bytes over the size estimates of the list (it was 256 list items)

u64 prefetch_now_us()
{
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// 50a-stage1-prefetch.md (plans/optimization_mt, plan 50 R3): the disk of gamedata measured on the first files of the
// start-up warm-up, once per process - the summed read speed of the warm threads and the median time to the first
// chunk of a file. Slower than kDiskSlowMBps or a median above kDiskSlowMedianUs: an HDD / USB / network disk the
// profile did not recognise (Wine, a USB bridge...) - one warm thread in list order from then on, the loads after it.
constexpr u32 kDiskProbeFiles = 200;
constexpr u32 kDiskProbeMinFiles = 20;
constexpr u64 kDiskProbeUs = 2000000;
constexpr double kDiskSlowMBps = 80.0;
constexpr u64 kDiskSlowMedianUs = 4000;

std::atomic_bool s_disk_slow{ false }; // measured slow (the profile's own Slow is read from the profile)

bool disk_slow_by_profile() { return xray::hw::GetProfile().tiers.disk_class == xray::hw::DiskClass::Slow; }

struct WarmDiskProbe
{
    std::mutex mutex; // all below
    xr_vector<u32> first_us; // per file: the open and the first chunk
    u64 bytes{};
    u64 read_us{}; // the whole reads, summed over the threads
    u64 start_us{};
    u32 threads{};
    bool decided{};
    std::atomic_bool wanted{}; // samples are taken (checked before the lock)

    // A stage starts its warm threads
    void begin(u32 warm_threads)
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (decided)
            return;
        threads = std::max(threads, warm_threads);
        if (!start_us)
            start_us = prefetch_now_us();
        wanted.store(true, std::memory_order_relaxed);
    }

    // One file of a warm thread: the time to its first chunk, the time of the whole read, its bytes
    void add(u64 first, u64 total, u64 n)
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (decided)
            return;
        first_us.push_back(static_cast<u32>(std::min<u64>(first, type_max<u32>)));
        bytes += n;
        read_us += total;
        const size_t files = first_us.size();
        if (files >= kDiskProbeFiles || (files >= kDiskProbeMinFiles && prefetch_now_us() - start_us >= kDiskProbeUs))
            decide();
    }

    // The end of the prefetch: decide on what there is
    void finish()
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (!decided && !first_us.empty())
            decide();
        wanted.store(false, std::memory_order_relaxed);
    }

private:
    void decide() // under `mutex`
    {
        decided = true;
        wanted.store(false, std::memory_order_relaxed);
        const size_t files = first_us.size();
        std::nth_element(first_us.begin(), first_us.begin() + files / 2, first_us.end());
        const u64 median_us = first_us[files / 2];
        // the threads read at the same time: the speed of one of them times their number
        const double mbps = read_us ? double(bytes) / double(1024 * 1024) / (double(read_us) / 1e6) * threads : 0.0;
        const bool slow = mbps < kDiskSlowMBps || median_us > kDiskSlowMedianUs;
        const bool by_profile = disk_slow_by_profile();
        Msg("* [prefetch] disk: %.0f MB/s, median %.2f ms to the first chunk over %u files (%u warm threads) -> %s%s",
            mbps, median_us / 1000.0, static_cast<u32>(files), threads, slow ? "slow" : "fast",
            by_profile ? " (the hw profile says slow)" : "");
        if (slow && !by_profile)
        {
            s_disk_slow.store(true, std::memory_order_relaxed);
            xray::io::BackgroundIoNoteMeasuredSlow(); // 50i: the in-game readers and the spawns go by it too (R6, R7)
            Msg("* [hw] disk measured %.0f MB/s, median %.1f ms -> slow: one warm thread in list order, the loads after "
                "it", mbps, median_us / 1000.0);
        }
        first_us.clear();
        first_us.shrink_to_fit();
    }
};
WarmDiskProbe s_disk_probe;

std::filesystem::path warm_os_path(const xr_string& fs_path)
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

struct WarmRoots
{
    xr_string level; // $level$, with the trailing separator; empty = none
    xr_string meshes; // $game_meshes$
    xr_string textures; // $game_textures$
};

xr_string warm_root(pcstr alias) { return FS.path_exist(alias) ? xr_string(FS.get_path(alias)->m_Path) : xr_string(); }

WarmRoots warm_roots_now()
{
    WarmRoots roots;
    roots.level = warm_root("$level$");
    roots.meshes = warm_root("$game_meshes$");
    roots.textures = warm_root("$game_textures$");
    return roots;
}

struct WarmStats
{
    u32 files{};
    u64 bytes{};
    u32 missing{};
};

// The whole file into `out` (at most kWarmMaxOgfBytes). false = no such file. probe: a sample for s_disk_probe.
bool warm_read_whole(const xr_string& path, xr_vector<u8>& out, bool probe = false)
{
    const u64 t0 = probe ? prefetch_now_us() : 0;
    std::ifstream f(warm_os_path(path), std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamoff size = f.tellg();
    out.clear();
    if (size <= 0)
        return true;
    out.resize(std::min(static_cast<size_t>(size), kWarmMaxOgfBytes));
    f.seekg(0);
    // 50a-stage1-prefetch.md: the first chunk alone first - its time is the latency of the file for the disk probe
    const size_t first = std::min(out.size(), kWarmChunk);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(first));
    size_t got = static_cast<size_t>(std::max<std::streamsize>(f.gcount(), 0));
    const u64 t1 = probe ? prefetch_now_us() : 0;
    if (got == first && out.size() > first)
    {
        f.read(reinterpret_cast<char*>(out.data() + first), static_cast<std::streamsize>(out.size() - first));
        got += static_cast<size_t>(std::max<std::streamsize>(f.gcount(), 0));
    }
    out.resize(got);
    if (probe)
        s_disk_probe.add(t1 - t0, prefetch_now_us() - t0, got);
    return true;
}

// Reads the file (at most `limit` bytes) and throws the bytes away. -1 = no such file. probe: a sample for
// s_disk_probe.
s64 warm_read_discard(const xr_string& path, xr_vector<u8>& buf, u64 limit, const std::atomic_bool& stop,
    bool probe = false)
{
    const u64 t0 = probe ? prefetch_now_us() : 0;
    u64 t1 = t0;
    std::ifstream f(warm_os_path(path), std::ios::binary);
    if (!f)
        return -1;
    u64 total = 0;
    while (total < limit && !stop.load(std::memory_order_relaxed))
    {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (probe && !total)
            t1 = prefetch_now_us();
        if (n <= 0)
            break;
        total += static_cast<u64>(n);
    }
    if (probe)
        s_disk_probe.add(t1 - t0, prefetch_now_us() - t0, total);
    return static_cast<s64>(total);
}

struct OgfRefs
{
    xr_string lod; // OGF_S_LODS
    xr_vector<xr_string> omf; // OGF_S_MOTION_REFS / OGF_S_MOTION_REFS2
    xr_vector<xr_string> textures; // OGF_TEXTURE of the visual and of its children
};

void warm_texture_names(IReader& r, xr_vector<xr_string>& out)
{
    if (!r.find_chunk(OGF_TEXTURE))
        return;
    string256 fnT, fnS;
    r.r_stringZ(fnT, sizeof(fnT));
    r.r_stringZ(fnS, sizeof(fnS));
    const int n = _GetItemCount(fnT);
    for (int i = 0; i < n; ++i)
    {
        string256 t;
        _GetItem(fnT, i, t, sizeof(t));
        if (t[0])
            out.emplace_back(t);
    }
}

void warm_motion_ref(pcstr nm, xr_vector<xr_string>& out)
{
    if (nm[0] && !strchr(nm, '*')) // a "folder\*.omf" set is listed through FS by the load itself
        out.emplace_back(nm);
}

// The names an ogf refers to - the same chunks CKinematics::Load / CKinematicsAnimated::Load / FHierrarhyVisual::Load
// read. The buffer is the ogf as is (a compressed chunk is unpacked by IReader::open_chunk).
void warm_parse_ogf(xr_vector<u8>& data, OgfRefs& refs, bool want_lod)
{
    if (data.empty())
        return;
    IReader R(data.data(), data.size());
    if (want_lod)
    {
        if (IReader* LD = R.open_chunk(OGF_S_LODS))
        {
            string_path lod;
            LD->r_string(lod, sizeof(lod));
            refs.lod = lod;
            LD->close();
        }
    }
    if (R.find_chunk(OGF_S_MOTION_REFS))
    {
        string_path items, nm;
        R.r_stringZ(items, sizeof(items));
        const int n = _GetItemCount(items);
        for (int k = 0; k < n; ++k)
        {
            _GetItem(items, k, nm, sizeof(nm));
            warm_motion_ref(nm, refs.omf);
        }
    }
    else if (R.find_chunk(OGF_S_MOTION_REFS2))
    {
        const u32 n = R.r_u32();
        for (u32 k = 0; k < n; ++k)
        {
            string_path nm;
            R.r_stringZ(nm, sizeof(nm));
            warm_motion_ref(nm, refs.omf);
        }
    }
    warm_texture_names(R, refs.textures);
    if (IReader* OBJ = R.open_chunk(OGF_CHILDREN))
    {
        IReader* O = OBJ->open_chunk(0);
        for (u32 count = 1; O; ++count)
        {
            warm_texture_names(*O, refs.textures);
            O->close();
            O = OBJ->open_chunk(count);
        }
        OBJ->close();
    }
}

xr_string warm_strip_ext(xr_string s)
{
    const size_t dot = s.find_last_of('.');
    const size_t sep = s.find_last_of("\\/");
    if (dot != xr_string::npos && (sep == xr_string::npos || dot > sep))
        s.resize(dot);
    return s;
}

// The files the start-up warm threads have taken already, shared by all of them: the textures and omf of the list are
// common to thousands of models, read once each (without it 8 threads read 95 GB of the OS cache over 27 k files and
// slowed the load on main down)
struct WarmSeen
{
    std::mutex mutex;
    std::unordered_set<xr_string> paths;

    bool claim(const xr_string& path)
    {
        std::lock_guard<std::mutex> lk(mutex);
        return paths.insert(path).second;
    }
};

// The files of one model (name: lower case, no extension, as the pool key): its ogf ($level$ first, as Instance_Load
// looks), its LOD ogf, the omf of its motion refs, the partition ltx, the dds of its textures and their bump maps.
// seen (may be null): the files another call has taken already are skipped. false = the ogf was not found.
// 50a-stage1-prefetch.md: with_textures - false: no dds (plan 50 R4); quit (may be null): leave after the current
// file; probe: samples for s_disk_probe.
bool warm_model_files(const xr_string& name, const WarmRoots& roots, xr_vector<u8>& ogf, xr_vector<u8>& buf,
    WarmStats& st, const std::atomic_bool& stop, WarmSeen* seen = nullptr, bool with_textures = true,
    const std::atomic_bool* quit = nullptr, bool probe = false)
{
    const auto halted = [&] {
        return stop.load(std::memory_order_relaxed) || (quit && quit->load(std::memory_order_relaxed));
    };
    const auto read_model = [&](const xr_string& rel) {
        if (!roots.level.empty() && warm_read_whole(roots.level + rel, ogf, probe))
            return true;
        return !roots.meshes.empty() && warm_read_whole(roots.meshes + rel, ogf, probe);
    };
    const auto discard = [&](const xr_string& path) {
        if (halted() || st.bytes >= kWarmMaxModelBytes)
            return true; // nothing more for this model: not counted as missing
        if (seen && !seen->claim(path))
            return true; // read (or found missing) for another model already
        const s64 n = warm_read_discard(path, buf, kWarmMaxModelBytes - st.bytes, stop, probe);
        if (n < 0)
            return false;
        ++st.files;
        st.bytes += static_cast<u64>(n);
        return true;
    };

    if (!read_model(name + ".ogf"))
    {
        ++st.missing;
        return false;
    }
    ++st.files;
    st.bytes += ogf.size();
    OgfRefs refs;
    warm_parse_ogf(ogf, refs, true);
    if (!refs.lod.empty() && !halted())
    {
        if (read_model(warm_strip_ext(refs.lod) + ".ogf"))
        {
            ++st.files;
            st.bytes += ogf.size();
            warm_parse_ogf(ogf, refs, false);
        }
        else
            ++st.missing;
    }
    for (const xr_string& omf : refs.omf)
    {
        const xr_string rel = warm_strip_ext(omf) + ".omf";
        const bool found = (!roots.level.empty() && discard(roots.level + rel)) ||
            (!roots.meshes.empty() && discard(roots.meshes + rel));
        if (!found)
            ++st.missing;
    }
    if (!roots.meshes.empty())
        discard(roots.meshes + name + ".ltx"); // CPartition::load; mostly there is none
    std::sort(refs.textures.begin(), refs.textures.end());
    refs.textures.erase(std::unique(refs.textures.begin(), refs.textures.end()), refs.textures.end());
    if (with_textures && !roots.textures.empty())
    {
        for (const xr_string& t : refs.textures)
        {
            const xr_string base = roots.textures + warm_strip_ext(t);
            if (!discard(base + ".dds"))
                ++st.missing;
            discard(base + "_bump.dds");
            discard(base + "_bump#.dds");
        }
    }
    return true;
}

// model_PrefetchFiles: one thread and a queue of model names (each once per session)
// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R5-R7), a slow disk only (xray::io::BackgroundIoSingle):
// the jobs of spawns (source 1) go ahead of the ALife ones - the first spawn_jobs of the queue, in the order they came;
// the thread reads in the single background I/O slot shared with the sound prefetch, and no dds (R5). The number of
// jobs not read yet goes to xray::io (the sound reader waits for 0).
struct ModelFilePrefetch
{
    struct Job
    {
        xr_string name;
        xr_string level; // $level$ when it was queued
        bool spawn{}; // 50l: a job of a spawn (ahead of the ALife ones) - so is its background load
    };
    std::mutex mutex; // queue, queued, warmed, roots, thread, current, spawn_jobs, bg_names
    std::condition_variable cv;
    std::deque<Job> queue;
    size_t spawn_jobs{}; // 50i: the first ones of `queue` are of spawns (a slow disk only; 0 otherwise)
    xr_string current; // 50i: the job being read now (empty when none)
    std::unordered_set<xr_string> queued; // queued (or skipped as loaded already, see PrefetchFiles) once per session
    std::unordered_set<xr_string> warmed; // the thread has gone through these
    // 50l-weak-cpu-spawn-models.md: the models of creatures (source bit 2) - once read, their base model is loaded on
    // the "Model bg load" thread (a weak CPU, D3D11)
    std::unordered_set<xr_string> bg_names;
    WarmRoots roots; // meshes / textures, taken at the first job
    // 50-smart-engine-hw-profile.md R5 (plans/optimization_mt 49, W5): the omf / ltx / dds read for any model in this
    // session - by this thread and by the start-up warm threads of Prefetch (StartupWarm::seen points here). The engine
    // keeps what it has loaded from them (textures, motions), so a file read once is not read again for another model.
    WarmSeen seen;
    std::thread thread;
    std::atomic_bool stop{};
    std::atomic_bool used{}; // anything queued yet: only then a cold load looks `warmed` up (metrics) and drops its job
};
ModelFilePrefetch* s_file_prefetch = nullptr;

// 50i: the jobs not read yet (queued + the one being read) to the single background I/O queue; under P.mutex, so the
// values arrive in order
void file_prefetch_publish(const ModelFilePrefetch& P)
{
    xray::io::BackgroundIoSetModelJobs(static_cast<u32>(P.queue.size() + (P.current.empty() ? 0 : 1)));
}

// 50i: is the job of this model still to be read (queued or being read)? Under P.mutex.
bool file_prefetch_pending(const ModelFilePrefetch& P, const xr_string& key)
{
    if (P.current == key)
        return true;
    return std::any_of(
        P.queue.begin(), P.queue.end(), [&key](const ModelFilePrefetch::Job& j) { return j.name == key; });
}

// 50l-weak-cpu-spawn-models.md (plans/optimization_mt): the background base loading, defined below (after the parallel
// prefetch, whose deferred motions parts it reuses)
bool bgload_enabled();
void bgload_offer(const xr_string& name, bool spawn);
bool bgload_pending(const xr_string& name);

// 50i: a spawn asks for a model ALife queued - its job moves behind the spawn jobs (a slow disk only). Under P.mutex.
// true = moved.
bool file_prefetch_promote(ModelFilePrefetch& P, const xr_string& key)
{
    const auto first_alife = P.queue.begin() + static_cast<std::ptrdiff_t>(P.spawn_jobs);
    const auto it = std::find_if(
        first_alife, P.queue.end(), [&key](const ModelFilePrefetch::Job& j) { return j.name == key; });
    if (it == P.queue.end())
        return false;
    ModelFilePrefetch::Job job = std::move(*it);
    job.spawn = true; // 50l
    P.queue.erase(it);
    P.queue.insert(P.queue.begin() + static_cast<std::ptrdiff_t>(P.spawn_jobs), std::move(job));
    ++P.spawn_jobs;
    return true;
}

void file_prefetch_thread(ModelFilePrefetch* P)
{
    // 50-smart-engine-hw-profile.md R0 (plans/optimization_mt): below normal CPU priority, and the background mode on
    // top of it - low I/O priority (these reads queue behind the page faults of main and the Game thread) and low memory
    // priority (acceptable: the thread only reads files ahead). Where the mode is not there, below normal alone. Not
    // for the start-up "Model warm" / "Model load" threads: main waits for them on the loading screen.
    Threading::SetCurrentThreadPriorityLevel(Threading::priority_level::below_normal);
    if (!Threading::SetCurrentThreadBackgroundMode())
        Msg("~ [model_prefetch] no background mode for the thread, below normal priority only");
    xr_vector<u8> ogf;
    xr_vector<u8> buf(kWarmChunk);
    for (;;)
    {
        ModelFilePrefetch::Job job;
        WarmRoots roots;
        size_t pending;
        {
            std::unique_lock<std::mutex> lk(P->mutex);
            P->cv.wait(lk, [P] { return P->stop.load(std::memory_order_relaxed) || !P->queue.empty(); });
            if (P->stop.load(std::memory_order_relaxed))
                return;
            job = std::move(P->queue.front());
            P->queue.pop_front();
            if (P->spawn_jobs)
                --P->spawn_jobs;
            P->current = job.name;
            file_prefetch_publish(*P);
            pending = P->queue.size();
            roots = P->roots;
        }
        roots.level = job.level;
        xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, static_cast<s64>(pending));
        {
            // 50i (R7): on a slow disk one background reader at a time - this job waits for the sound file being read
            // (the sound reader waits for all the model jobs). R5: no dds there - ogf, LOD ogf, omf, partition ltx.
            xray::io::BackgroundIoScope io;
#if XR_PERF_METRICS
            const u64 t_io = xray::perf::g_perf_metrics_active ? xray::perf::Ticks() : 0;
#endif
            if (!io.enter(xray::io::BackgroundIoClient::Model, [P] { return P->stop.load(std::memory_order_relaxed); }))
                return; // shutting down (file_prefetch_shutdown reports 0 jobs)
#if XR_PERF_METRICS
            if (io.taken() && t_io)
                xray::perf::AddTimeSlow(xray::perf::Metric::mprefetch_io_wait, xray::perf::Ticks() - t_io);
#endif
            ZoneScopedN("model_prefetch_files");
            ZoneText(job.name.c_str(), job.name.size());
            PERF_SCOPE(mprefetch_time);
            WarmStats st;
            warm_model_files(job.name, roots, ogf, buf, st, P->stop, &P->seen, !io.taken());
            PERF_ADD(mprefetch_files, st.files);
            PERF_ADD(mprefetch_kb, (st.bytes + 1023) / 1024);
            PERF_ADD(mprefetch_missing, st.missing);
        }
        std::lock_guard<std::mutex> lk(P->mutex);
        P->warmed.insert(job.name);
        // 50l-weak-cpu-spawn-models.md: a creature's model - its base model is loaded next, off the main thread. Under
        // P->mutex, so PrefetchPending sees the job either here or there.
        if (P->bg_names.count(job.name))
            bgload_offer(job.name, job.spawn);
        P->current.clear();
        file_prefetch_publish(*P);
    }
}

void file_prefetch_shutdown()
{
    if (!s_file_prefetch)
        return;
    ModelFilePrefetch& P = *s_file_prefetch;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        P.stop.store(true, std::memory_order_relaxed);
        P.queue.clear();
        P.spawn_jobs = 0;
    }
    P.cv.notify_all();
    if (P.thread.joinable())
        P.thread.join(); // waits for one file read at most: the thread checks `stop` between chunks
    xr_delete(s_file_prefetch);
    xray::io::BackgroundIoSetModelJobs(0); // 50i: the sound reader no longer waits for this thread
    xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, 0);
}

// 50-smart-engine-hw-profile.md R5 (plans/optimization_mt): the bases are freed (models_Clear(true), the end of a
// game) - a new session for the "Model prefetch" thread: the names asked and the files read before may be needed again
void file_prefetch_reset()
{
    if (!s_file_prefetch)
        return;
    ModelFilePrefetch& P = *s_file_prefetch;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        P.queue.clear();
        P.spawn_jobs = 0;
        P.queued.clear();
        P.warmed.clear();
        P.bg_names.clear(); // 50l
        file_prefetch_publish(P); // 50i: the job being read, if any
    }
    {
        std::lock_guard<std::mutex> lk(P.seen.mutex);
        P.seen.paths.clear();
    }
    xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, 0);
}

// The start-up warm threads of Prefetch (the number from the hw policy, plan 50 R3): they take the items of the prefetch
// list in order, up to lead_bytes of the list estimates ahead of the loads (R4), and skip the ones the loads have
// already passed. On a slow disk the loads wait for them instead (wait_warmed): one reader, in list order.
struct StartupWarm
{
    xr_vector<xr_string> names;
    WarmRoots roots;
    std::atomic<size_t> next{0};
    std::atomic<size_t> main_pos{0}; // the furthest item the loads (or main) have taken
    std::atomic_bool stop{};
    std::atomic<u32> files{0};
    std::atomic<u32> missing{0};
    std::atomic<u32> behind{0}; // items main had passed before a warm thread got to them
    std::atomic<u64> bytes{0};
    WarmSeen own_seen; // used when there is no ModelFilePrefetch (not expected)
    WarmSeen* seen{ &own_seen }; // the omf / ltx / dds taken by any of the threads; ModelFilePrefetch::seen

    // 50a-stage1-prefetch.md (plans/optimization_mt, plan 50 R3 / R4)
    xr_vector<u64> cum; // cum[i] - the size estimates of the items [0, i)
    u64 lead_bytes{};
    bool with_textures{ true };
    bool slow_start{}; // the hw profile says the disk is slow
    std::unique_ptr<std::atomic<u8>[]> warmed; // per item: a warm thread is through with it (read or skipped)
    xr_vector<u8> loaded_already; // per item: a pool hit or a repeat, nothing to load (read only on a slow disk)
    std::atomic<u32> running{0}; // warm threads not finished
    std::atomic<u32> thread_ids{0};
    std::atomic<u64> waited_us{0}; // the loads (or main) waiting for the warm-up on a slow disk
    // 50k-slow-disk-early-prefetch.md: per item - read at the menu by the early warm-up, `warmed` already (empty: none)
    xr_vector<u8> early;

    bool slow() const { return slow_start || s_disk_slow.load(std::memory_order_relaxed); }

    // The loads (or main): item `pos` is taken now. main_pos only grows: several load threads report
    void note_main(size_t pos)
    {
        size_t cur = main_pos.load(std::memory_order_relaxed);
        while (cur < pos && !main_pos.compare_exchange_weak(cur, pos, std::memory_order_relaxed))
        {
        }
    }

    // Item i is further than lead_bytes ahead of the loads (the next item after theirs is always allowed)
    bool ahead_too_far(size_t i) const
    {
        const size_t m = main_pos.load(std::memory_order_relaxed);
        return i > m + 1 && cum[i + 1] - cum[m + 1] > lead_bytes;
    }

    void mark(size_t i)
    {
        if (warmed)
            warmed[i].store(1, std::memory_order_release);
    }

    // A slow disk: the load of item `pos` (a load thread or main) reads its files after the warm thread - one reader in
    // list order instead of a seek per load thread. No deadlock: the item is not ahead of the loads (note_main first),
    // so the warm thread that has it, or takes it, does not wait for the lead.
    void wait_warmed(size_t pos)
    {
        if (!warmed || !slow() || loaded_already[pos] || warmed[pos].load(std::memory_order_acquire))
            return;
        ZoneScopedN("model_prefetch_warm_wait");
        const u64 t0 = prefetch_now_us();
        while (!warmed[pos].load(std::memory_order_acquire) && running.load(std::memory_order_acquire) > 0 &&
            !stop.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waited_us.fetch_add(prefetch_now_us() - t0, std::memory_order_relaxed);
    }
};

void startup_warm_thread(StartupWarm* W)
{
    // 50a-stage1-prefetch.md (plan 50 R3): on a disk measured slow the first thread goes on alone, in list order, the
    // others leave after their current file
    const u32 id = W->thread_ids.fetch_add(1, std::memory_order_relaxed);
    const std::atomic_bool* quit = id ? &s_disk_slow : nullptr;
    xr_vector<u8> ogf;
    xr_vector<u8> buf(kWarmChunk);
    for (;;)
    {
        if (quit && quit->load(std::memory_order_relaxed))
            break;
        const size_t i = W->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= W->names.size() || W->stop.load(std::memory_order_relaxed))
            break;
        if (!W->early.empty() && W->early[i]) // 50k-slow-disk-early-prefetch.md: read at the menu
            continue;
        if (!W->slow() && i < W->main_pos.load(std::memory_order_relaxed)) // on a slow disk the load waits for it
        {
            W->behind.fetch_add(1, std::memory_order_relaxed);
            W->mark(i);
            continue;
        }
        if (W->slow() && W->loaded_already[i]) // a slow disk: no read for a model nobody loads
        {
            W->mark(i);
            continue;
        }
        while (!W->stop.load(std::memory_order_relaxed) && !(quit && quit->load(std::memory_order_relaxed)) &&
            W->ahead_too_far(i))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (W->stop.load(std::memory_order_relaxed) || (quit && quit->load(std::memory_order_relaxed)))
        {
            W->mark(i);
            break;
        }
        {
            ZoneScopedN("model_warm");
            ZoneText(W->names[i].c_str(), W->names[i].size());
            WarmStats st;
            warm_model_files(W->names[i], W->roots, ogf, buf, st, W->stop, W->seen, W->with_textures, quit,
                s_disk_probe.wanted.load(std::memory_order_relaxed));
            W->files.fetch_add(st.files, std::memory_order_relaxed);
            W->missing.fetch_add(st.missing, std::memory_order_relaxed);
            W->bytes.fetch_add(st.bytes, std::memory_order_relaxed);
        }
        W->mark(i);
    }
    W->running.fetch_sub(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------------------
// 50k-slow-disk-early-prefetch.md (plans/optimization_mt): the early warm-up of a slow disk (the hw profile: HDD, USB,
// network). The engine starts it at the main menu (armed in IGame_Persistent::OnAppStart, started when the menu has
// settled - 50z3-startup-slow-disk-io.md) with the prefix of both prefetch lists the budget takes, in the order the
// prefetch stages load it - the visuals of the objects, then the models. One thread reads the files of each model
// (warm_model_files, in the slot of the single background I/O queue, after the foreground reads of the menu) into the
// OS cache while the player is in the menu: at most the warm-up lead of the list estimates (R4: nothing
// loads the files before the level load, so the lead counts from the start of the list), and not on once the RAM
// available falls below the limit the lead was taken for. The first level load stops it (KERNEL:start, before
// PreStart); the warm-up of that prefetch skips the models it has read, and the files it took go into the session set
// (ModelFilePrefetch::seen - models_Clear(true) of PreStart empties that one in between, so they are kept apart until
// the prefetch). Main thread, except early_warm_thread.
struct EarlyWarm
{
    xr_vector<xr_string> names; // the pool keys of the list, each once, in its order
    xr_vector<u64> cum; // cum[i] - the estimates of the names [0, i)
    WarmRoots roots;
    PrefetchEarlyWarmParams params{};
    WarmSeen seen; // the omf / ltx / dds taken, until early_warm_merge
    std::thread thread;
    std::atomic_bool stop{};
    // the thread's, read after the join
    xr_vector<u8> done; // per name: read through (a model whose ogf is not there too)
    u32 files{};
    u32 missing{};
    u64 bytes{};
    u64 start_us{};
    u64 end_us{};
    PrefetchEarlyWarmEnd end{ PrefetchEarlyWarmEnd::Stopped };
    // main, from the stop on
    bool stopped{};
    bool merged{}; // `seen` went into the session set
    bool released{}; // the first level load is over: nothing skips the models any more
    std::unordered_set<xr_string> done_set;
    u32 skipped{}; // the prefetch warm-up found these models read
    u64 waited_us{}; // the loads waiting for the prefetch warm-up in the stages since the stop
};
EarlyWarm* s_early_warm = nullptr;

// The RAM available now is below the limit the warm-up lead was taken for (-hw_ram / -hw_profile: capped as the
// prefetch caps it). Any thread.
bool early_warm_ram_low(const PrefetchEarlyWarmParams& params)
{
    u64 avail = 0, commit = 0;
    xray::hw::QueryMemoryNow(avail, commit);
    if (!avail)
        return false; // no live value on this platform: the limit of the start holds
    if (params.ram_cap)
        avail = std::min(avail, params.ram_cap);
    return avail < params.ram_floor;
}

void early_warm_thread(EarlyWarm* E)
{
    // Below normal CPU priority and EcoQoS (plan 50 R0, R15), but - 50z3-startup-slow-disk-io.md - not the background
    // mode: the normal I/O and memory priority. The menu's own reads go first by the slot now (the Warm client waits
    // until no foreground read for kForegroundQuietMs, model by model), so the very low I/O priority only cost speed in
    // the quiet gaps (the laptop on a USB HDD: ~2.8 MB/s against 20). The normal memory priority keeps the pages it
    // reads minutes before the level load in the OS cache longer (the background mode would give them the low one).
    Threading::SetCurrentThreadPriorityLevel(Threading::priority_level::below_normal);
    Threading::SetCurrentThreadEcoQos();
    xr_vector<u8> ogf;
    xr_vector<u8> buf(kWarmChunk);
    const auto halted = [E] { return E->stop.load(std::memory_order_relaxed); };
    E->end = PrefetchEarlyWarmEnd::ListEnd;
    for (size_t i = 0; i < E->names.size(); ++i)
    {
        if (halted())
        {
            E->end = PrefetchEarlyWarmEnd::Stopped;
            break;
        }
        if (i > 0 && E->cum[i + 1] > E->params.lead_bytes) // the first model is always allowed, as with the loads
        {
            E->end = PrefetchEarlyWarmEnd::Lead;
            break;
        }
        if (early_warm_ram_low(E->params))
        {
            E->end = PrefetchEarlyWarmEnd::LowRam;
            break;
        }
        // 50i (R7): one background reader at a time on a slow disk; in the game this thread is gone. In the menu others
        // read too: 50r-slow-disk-async-textures.md - the "Texture async" thread, the textures of a menu window go first
        // (Warm waits while texture jobs are pending); 50z3-startup-slow-disk-io.md - main and the workers (the menu
        // itself, the preview cache, the scripts: Warm waits until none has opened a file for kForegroundQuietMs), the
        // deferred sounds of the start-up and the smart terrain map (the slot, one at a time)
        xray::io::BackgroundIoScope io;
        if (!io.enter(xray::io::BackgroundIoClient::Warm, halted))
        {
            E->end = PrefetchEarlyWarmEnd::Stopped;
            break;
        }
        ZoneScopedN("model_early_warm");
        ZoneText(E->names[i].c_str(), E->names[i].size());
        WarmStats st;
        warm_model_files(E->names[i], E->roots, ogf, buf, st, E->stop, &E->seen, E->params.with_textures, nullptr,
            s_disk_probe.wanted.load(std::memory_order_relaxed));
        E->files += st.files;
        E->missing += st.missing;
        E->bytes += st.bytes;
        if (halted())
        {
            E->end = PrefetchEarlyWarmEnd::Stopped; // this model may be cut short: the prefetch reads it again
            break;
        }
        E->done[i] = 1;
    }
    E->end_us = prefetch_now_us();
    xray::io::BackgroundIoSetMenuWarmPending(false); // 50z3: over (done, stopped) - a menu reader may go on
    if (E->end != PrefetchEarlyWarmEnd::Stopped) // done at the menu: from now on a level load loses nothing
    {
        u32 read = 0;
        for (const u8 d : E->done)
            read += d;
        Msg("* [prefetch] early warm-up: done at the menu in %.1f s, %u files / %u MB, %u of %u models (%s)",
            (E->end_us - E->start_us) / 1e6, E->files, static_cast<u32>(E->bytes / (1024 * 1024)), read,
            static_cast<u32>(E->names.size()),
            E->end == PrefetchEarlyWarmEnd::ListEnd ? "the whole list" :
            E->end == PrefetchEarlyWarmEnd::Lead    ? "the warm-up lead reached" :
                                                      "little RAM available");
    }
}

// Stops the thread (it leaves after the chunk being read) and waits for it; the first time also the set of the models
// read
void early_warm_join(EarlyWarm& E)
{
    if (E.thread.joinable())
    {
        E.stop.store(true, std::memory_order_relaxed);
        E.thread.join();
    }
    if (E.stopped)
        return;
    E.stopped = true;
    for (size_t i = 0; i < E.names.size(); ++i)
    {
        if (E.done[i])
            E.done_set.insert(E.names[i]);
    }
}

// After PreStart of the first level load (its models_Clear(true) empties the session set): the files the early warm-up
// took are the session's - the prefetch warm-up and the "Model prefetch" thread do not read them again
void early_warm_merge(EarlyWarm& E)
{
    if (E.merged || !E.stopped)
        return;
    E.merged = true;
    if (s_file_prefetch)
    {
        std::lock_guard<std::mutex> lk(s_file_prefetch->seen.mutex);
        s_file_prefetch->seen.paths.insert(E.seen.paths.begin(), E.seen.paths.end());
    }
    E.seen.paths.clear(); // the thread is gone: no lock
}

// PrefetchItems (main): the items of W the early warm-up has read are through - the warm threads skip them, the loads
// do not wait for them. Not when this warm-up reads the dds and the early one went without them. The number of them.
u32 early_warm_take(StartupWarm& W)
{
    EarlyWarm* E = s_early_warm;
    if (!E || E->released)
        return 0;
    early_warm_join(*E); // a prefetch with no KERNEL:start before it: the hand-off is here
    early_warm_merge(*E);
    if (E->done_set.empty() || (W.with_textures && !E->params.with_textures))
        return 0;
    W.early.assign(W.names.size(), 0);
    u32 n = 0;
    for (size_t i = 0; i < W.names.size(); ++i)
    {
        if (!E->done_set.count(W.names[i]))
            continue;
        W.early[i] = 1;
        W.warmed[i].store(1, std::memory_order_relaxed); // the threads start after this
        ++n;
    }
    E->skipped += n;
    return n;
}

// PrefetchItems (main), a stage of the first level load after the stop: how long its loads waited for the warm-up
void early_warm_note(u64 waited_us)
{
    if (s_early_warm && s_early_warm->stopped && !s_early_warm->released)
        s_early_warm->waited_us += waited_us;
}

void early_warm_stats(const EarlyWarm& E, PrefetchEarlyWarmStats& st)
{
    st = {};
    st.models = static_cast<u32>(E.names.size());
    for (size_t i = 0; i < E.names.size(); ++i)
    {
        if (!E.done[i])
            continue;
        ++st.models_read;
        st.estimate_read += E.cum[i + 1] - E.cum[i];
    }
    st.estimate = E.cum.back();
    st.files = E.files;
    st.missing = E.missing;
    st.bytes = E.bytes;
    st.read_ms = static_cast<u32>((E.end_us > E.start_us ? E.end_us - E.start_us : 0) / 1000);
    st.end = E.end;
    st.skipped = E.skipped;
    st.waited_ms = static_cast<u32>(E.waited_us / 1000);
}

// ~CModelPool: the exit from the menu or the render going away
void early_warm_shutdown()
{
    if (!s_early_warm)
        return;
    early_warm_join(*s_early_warm); // waits for one chunk (or one file open) at most
    xr_delete(s_early_warm);
}

// ---------------------------------------------------------------------------------------------------------------------
// 40-parallel-prefetch.md (plans/optimization_mt): the parallel model prefetch (D3D11)

// ModelMotionsScope: the motions part of CKinematicsAnimated::Load, one at a time. Recursive (Lock): the main thread
// holds it around Create / Delete of the list and may load a model itself under it. Order: ModelPool_Mutex -> this.
Lock s_motions_lock;

constexpr u32 kPrefetchMaxLoadThreads = 8; // the serialized parts (shader creation, motions) limit the gain beyond it

// How far the loads may run ahead of the motions chain (each deferred motions part keeps its ogf open until it runs)
constexpr size_t kPrefetchMaxMotionsLag = 512;

// One deferred motions part (CKinematicsAnimated::LoadMotions): the model, its reader, its name. The reader is the ogf
// of the model left open (closed with FS.r_close) or, for a model loaded from a chunk of another file, a reader over a
// copy of that chunk (`copy`; the owner closes its chunk right after the load)
struct PrefetchMotionsPart
{
    CKinematicsAnimated* model{};
    IReader* data{};
    xr_string name;
    xr_vector<u8> copy;
    bool copied{};
};

// The load of the parallel prefetch running on this thread: the ogf readers of the Instance_Load(name) calls active on
// it (the model of the list, its LOD, the LOD of the LOD - each opens its own ogf) and the motions parts deferred so
// far, in the order the sequential load ran them (a LOD is loaded inside CKinematics::Load of its model, so its part
// comes before the part of the model)
struct PrefetchTop
{
    struct Reader
    {
        IReader* data{};
        bool deferred{}; // a deferred part reads it later: the motions chain closes it, not Instance_Load
    };
    xr_vector<Reader> readers; // a stack
    xr_vector<PrefetchMotionsPart> parts;
    // 50l-weak-cpu-spawn-models.md: a load of the "Model bg load" thread (in play), not of the parallel prefetch - the
    // parts run on the main thread when it takes the model (bgload_complete)
    bool background{};
};
thread_local PrefetchTop* tl_prefetch_top = nullptr;

// CModelPool::Prefetch / PrefetchList with load threads: the threads load (Instance_Load, not registered) the models of
// the list that are not in the pool yet, each once, taking them in the order of the list; the main thread registers
// them and makes the instances in the same order as the sequential prefetch did.
//
// The motions part (CKinematicsAnimated::LoadMotions: g_pMotionsContainer, the shared motions_value and partition) has
// to go in the order of the list. A load thread does not wait for it: the load defers the part of its own model
// (ModelMotionsDefer) and goes on with the next model; the deferred parts run as one chain in the order of the list -
// the thread that completes a load runs the chain while its next load is ready (motions_pos), one thread at a time.
struct ParallelPrefetch
{
    struct Load
    {
        xr_string key; // the pool key (name + suffix), the name the base model is registered with
        xr_string name; // the model name the load gets (lower case, no extension)
        xr_string suffix;
        size_t item{}; // its position in the list
        // under `mutex`:
        dxRender_Visual* result{}; // the base model (null: the load failed)
        xr_vector<PrefetchMotionsPart> motions; // the deferred motions parts (PrefetchTop), run by the motions chain
    };

    CModelPool* pool{};
    xr_vector<Load> loads; // in the order of the list
    std::atomic<size_t> next{0}; // the next load a thread takes
    StartupWarm* warm{}; // the warm threads read ahead of the loads taken (null: no warm-up)

    std::mutex mutex; // loaded, done, motions_pos, motions_busy, the fields of Load under it
    std::condition_variable cv;
    xr_vector<u8> loaded; // the load returned (its motions part may be deferred)
    xr_vector<u8> done; // the load is complete, the motions part included: main may take it
    size_t motions_pos{0}; // loads [0, motions_pos) have passed their motions part (done)
    bool motions_busy{}; // a thread runs the motions chain

    // statistics (microseconds, summed over the threads)
    std::atomic<u64> load_us{0}; // the loads without the deferred motions parts
    std::atomic<u64> motions_us{0}; // the deferred motions parts (the chain)
    std::atomic<u64> turn_wait_us{0}; // motions parts not deferred (a fallback, none expected) waiting
    std::atomic<u64> lag_wait_us{0}; // load threads waiting for the chain (kPrefetchMaxMotionsLag)
    std::atomic<u32> deferred{0}; // motions parts deferred
    std::atomic<u32> deferred_nested{0}; // ... of them of a LOD (not of the model of the list)

    // ModelMotionsScope on a load thread (a motions part not deferred): the motions parts of the loads before `index`
    // first. No deadlock: the loads before it are taken (the threads take them in order) and run without waiting for
    // it, and the thread that completes the last of them runs the chain up to `index`.
    void wait_turn(size_t index)
    {
        std::unique_lock<std::mutex> lk(mutex);
        if (motions_pos >= index)
            return;
        ZoneScopedN("model_prefetch_turn");
        const u64 t0 = prefetch_now_us();
        cv.wait(lk, [&] { return motions_pos >= index; });
        turn_wait_us.fetch_add(prefetch_now_us() - t0, std::memory_order_relaxed);
    }

    // A load thread before it starts load `index`: not more than kPrefetchMaxMotionsLag loads past the chain. No
    // deadlock: the load at motions_pos is not held here (index - motions_pos < lag for it).
    void wait_lag(size_t index)
    {
        std::unique_lock<std::mutex> lk(mutex);
        if (index < motions_pos + kPrefetchMaxMotionsLag)
            return;
        ZoneScopedN("model_prefetch_lag");
        const u64 t0 = prefetch_now_us();
        cv.wait(lk, [&] { return index < motions_pos + kPrefetchMaxMotionsLag; });
        lag_wait_us.fetch_add(prefetch_now_us() - t0, std::memory_order_relaxed);
    }

    // A load thread: load `index` returned (top: its deferred motions parts, if any). Then the motions chain, unless
    // another thread runs it now (that one gets to this load when the loads before it are in).
    void finish_load(size_t index, dxRender_Visual* V, PrefetchTop& top)
    {
        std::unique_lock<std::mutex> lk(mutex);
        Load& L = loads[index];
        L.result = V;
        L.motions = std::move(top.parts);
        loaded[index] = 1;
        if (motions_busy)
            return;
        motions_busy = true;
        while (motions_pos < loads.size() && loaded[motions_pos])
        {
            Load& M = loads[motions_pos];
            if (!M.motions.empty())
            {
                xr_vector<PrefetchMotionsPart> parts = std::move(M.motions);
                M.motions.clear();
                lk.unlock();
                for (PrefetchMotionsPart& part : parts) // in the order the sequential load ran them
                    run_motions(part);
                lk.lock();
            }
            done[motions_pos] = 1;
            ++motions_pos;
            cv.notify_all();
        }
        motions_busy = false;
    }

    void run_motions(PrefetchMotionsPart& part)
    {
        {
            ZoneScopedN("model_prefetch_motions");
            ZoneText(part.name.c_str(), part.name.size());
            ModelColdLoadScope cold_scope; // modelpool/load/motions and its parts
            const u64 t0 = prefetch_now_us();
            part.model->LoadMotions(part.name.c_str(), part.data); // tl_prefetch_load is -1 here: no wait_turn
            motions_us.fetch_add(prefetch_now_us() - t0, std::memory_order_relaxed);
        }
        if (!part.copied)
            FS.r_close(part.data);
        else
            xr_delete(part.data);
    }

    // The main thread: the result of the load, once it is done (the motions part included)
    dxRender_Visual* wait_result(size_t index)
    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return done[index] != 0; });
        return loads[index].result;
    }
};
ParallelPrefetch* s_parallel_prefetch = nullptr; // set while the load threads run (before they start, after the join)
thread_local s64 tl_prefetch_load = -1; // the load of s_parallel_prefetch this thread is doing, -1 = none

void prefetch_load_thread(ParallelPrefetch* P)
{
    for (;;)
    {
        const size_t i = P->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= P->loads.size())
            return;
        P->wait_lag(i);
        const ParallelPrefetch::Load& L = P->loads[i]; // key, name, suffix, item: not changed while the threads run
        if (P->warm)
        {
            P->warm->note_main(L.item);
            P->warm->wait_warmed(L.item); // 50a-stage1-prefetch.md: a slow disk - after the warm-up
        }
        dxRender_Visual* V;
        PrefetchTop top;
        {
            ZoneScopedN("model_prefetch_load");
            ZoneText(L.key.c_str(), L.key.size());
            PERF_SCOPE(modelpool_instance_load);
            ModelColdLoadScope cold_scope; // modelpool/load/* of this thread
            const u64 t0 = prefetch_now_us();
            tl_prefetch_load = static_cast<s64>(i);
            tl_prefetch_top = &top;
            V = P->pool->Instance_Load(L.name.c_str(), L.suffix.c_str(), FALSE);
            tl_prefetch_top = nullptr;
            tl_prefetch_load = -1;
            P->load_us.fetch_add(prefetch_now_us() - t0, std::memory_order_relaxed);
        }
        P->deferred.fetch_add(static_cast<u32>(top.parts.size()), std::memory_order_relaxed);
        P->finish_load(i, V, top);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 50l-weak-cpu-spawn-models.md (plans/optimization_mt): the background base loading, a weak CPU (the hw policy's
// model_bg_load: cpu tier low / potato, D3D11 with DriverConcurrentCreates). A cold model load costs 100-350 ms of the
// main thread there, mostly CPU (skin, geometry, shaders, motions). The "Model prefetch" thread reads the files of a
// creature about to come online (ALife near the online distance, a spawn) and hands its name over to the "Model bg
// load" thread, which loads the base model the way a load thread of the parallel prefetch does (plan 40): Instance_Load
// not registered, the motions parts of the model and of its LOD deferred (ModelMotionsDefer).
//
// The motions part never runs on that thread: it works on data the live models share - the reference counts of the
// docked motions (Instance_Duplicate and the destruction of an instance change them under the pool lock, not under the
// motions lock) and the shared partition (CPartition::load rewrites it, the animation of other models reads it). The
// thread only docks the omf the model refers to (PreloadMotions: the costly parse of the keys outside the motions lock,
// the insert under it). The main thread runs the motions part (then cheap) and registers the base at its next
// DeleteQueue (ModelDeferredClear: before PreRender and FrameMove, no game or render thread runs then), or in the Create
// that needs the model first. The spawn finds the base in the pool and makes only a copy.
//
// Locks: the thread holds no lock across a model. Inside the load it takes what a load thread of the parallel prefetch
// takes, each for one resource: the pool lock for the search of a child (CreateChild), m_create_lock per shader,
// m_textures_lock per texture, v_geoms_lock, the FS lock, the motions lock for a lookup / an insert, the smem / LZ /
// stats locks. On a slow disk the dds of the model are read into the OS cache first, outside every lock, so a texture
// load under its lock does not wait for the disk. The deletes of the shader registries take m_create_lock now too
// (ResourceManager). The main thread waits for this thread only in a flush (ClearPool, a device reset, the shutdown): at
// most one model. Normal priority on purpose, not the background mode / EcoQoS of the file reader: a starved holder of
// a lock the main thread takes would make main wait (priority inversion on a busy 4-core CPU).
constexpr u64 kBgLoadFinishUs = 2000; // DeleteQueue: the time for the motions parts and registrations (one at least)

struct BgLoadReady
{
    xr_string key; // the pool key (lower case, no extension, no suffix)
    dxRender_Visual* model{};
    xr_vector<PrefetchMotionsPart> motions; // the deferred motions parts, in the order the load deferred them
};

struct ModelBgLoad
{
    CModelPool* pool{};
    std::mutex mutex; // all below. A leaf: taken under ModelFilePrefetch::mutex and ModelPool_Mutex, never the reverse
    std::condition_variable cv; // the thread: a job or `stop`; a flush / pause: `busy` cleared
    std::deque<xr_string> queue;
    size_t spawn_jobs{}; // the first ones of `queue` are of spawns
    std::unordered_set<xr_string> taken; // queued once (until a flush)
    xr_string current; // being loaded
    bool busy{};
    u32 paused{}; // device resets in progress: no new job
    xr_vector<BgLoadReady> ready; // loaded, for the main thread to finish
    std::atomic_bool any_ready{}; // `ready` is not empty (looked at before the lock)
    std::thread thread;
    std::atomic_bool stop{};
};
ModelBgLoad* s_bgload = nullptr;
// The bases the thread loaded, registered on the main thread and not used yet (modelpool/bgload/used). ModelPool_Mutex.
std::unordered_set<xr_string> s_bgload_bases;

bool bgload_enabled()
{
#if defined(USE_DX11)
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    return xray::hw::PolicyValid(policy) && policy.model_bg_load != 0;
#else
    return false; // GL: the objects of the context are made on its thread only - the files are read ahead, no more
#endif
}

void bgload_publish(const ModelBgLoad& B) // under B.mutex
{
    xray::perf::SetGauge(xray::perf::Metric::mbg_queue, static_cast<s64>(B.queue.size() + (B.busy ? 1 : 0)));
}

void bgload_thread(ModelBgLoad* B);

// The "Model prefetch" thread has read the files of a creature's model (under its mutex), or PrefetchFiles asks for one
// read before. Each name once until a flush; a spawn's job goes ahead of the ALife ones.
void bgload_offer(const xr_string& name, bool spawn)
{
    ModelBgLoad* B = s_bgload;
    if (!B || B->stop.load(std::memory_order_relaxed) || !bgload_enabled())
        return;
    {
        std::lock_guard<std::mutex> lk(B->mutex);
        if (B->stop.load(std::memory_order_relaxed))
            return; // shutting down: no new thread
        if (!B->taken.insert(name).second)
        {
            if (!spawn)
                return;
            // a spawn asks for one ALife queued: ahead of the ALife jobs
            const auto it = std::find(B->queue.begin() + static_cast<std::ptrdiff_t>(B->spawn_jobs), B->queue.end(), name);
            if (it == B->queue.end())
                return; // being loaded, loaded or skipped
            B->queue.erase(it);
            B->queue.insert(B->queue.begin() + static_cast<std::ptrdiff_t>(B->spawn_jobs), name);
            ++B->spawn_jobs;
            return;
        }
        if (spawn)
        {
            B->queue.insert(B->queue.begin() + static_cast<std::ptrdiff_t>(B->spawn_jobs), name);
            ++B->spawn_jobs;
        }
        else
            B->queue.push_back(name);
        bgload_publish(*B);
        if (!B->thread.joinable())
            B->thread = Threading::RunThread("Model bg load", &bgload_thread, static_cast<ModelBgLoad*>(B));
    }
    B->cv.notify_all();
}

// Is the base model of this name still to come from the thread (queued, being loaded, or loaded and not registered)?
bool bgload_pending(const xr_string& name)
{
    ModelBgLoad* B = s_bgload;
    if (!B)
        return false;
    std::lock_guard<std::mutex> lk(B->mutex);
    if (B->current == name || std::find(B->queue.begin(), B->queue.end(), name) != B->queue.end())
        return true;
    return std::any_of(B->ready.begin(), B->ready.end(), [&name](const BgLoadReady& r) { return r.key == name; });
}

// A cold load of the model on another thread (Create): its job is of no use any more. true = the thread had it
// (queued, being loaded or loaded) - the background load came too late
bool bgload_cold(const xr_string& name)
{
    ModelBgLoad* B = s_bgload;
    if (!B)
        return false;
    std::lock_guard<std::mutex> lk(B->mutex);
    const auto it = std::find(B->queue.begin(), B->queue.end(), name);
    if (it != B->queue.end())
    {
        if (static_cast<size_t>(it - B->queue.begin()) < B->spawn_jobs)
            --B->spawn_jobs;
        B->queue.erase(it);
        bgload_publish(*B);
        return true;
    }
    return B->current == name ||
        std::any_of(B->ready.begin(), B->ready.end(), [&name](const BgLoadReady& r) { return r.key == name; });
}

// A deferred motions part that will not run (or has run): its reader closed
void bgload_close_part(PrefetchMotionsPart& part)
{
    if (!part.copied)
        FS.r_close(part.data);
    else
        xr_delete(part.data);
    part.data = nullptr;
}

// The main thread: a loaded model nobody takes - the readers of its motions parts closed, the model released (its
// motions part never ran: it holds no motions)
void bgload_free(BgLoadReady& entry)
{
    for (PrefetchMotionsPart& part : entry.motions)
        bgload_close_part(part);
    entry.motions.clear();
    if (entry.model)
    {
        entry.model->Release();
        xr_delete(entry.model);
    }
}

// The main thread (DeleteQueue) or the thread of the Create that needs the model: its motions parts here (their omf are
// docked already, PreloadMotions), then the base is registered. nullptr = it became a base meanwhile (loaded cold on
// another thread): freed.
dxRender_Visual* bgload_complete(CModelPool& pool, BgLoadReady& entry)
{
    ModelPool_mtlock lock;
    if (pool.Instance_Find(entry.key.c_str()))
    {
        bgload_free(entry);
        PERF_INC(mbg_discarded);
        return nullptr;
    }
    {
        ZoneScopedN("model_bg_finish");
        ZoneText(entry.key.c_str(), entry.key.size());
        PERF_SCOPE(mbg_finish);
        for (PrefetchMotionsPart& part : entry.motions)
        {
            part.model->LoadMotions(part.name.c_str(), part.data); // ModelMotionsScope inside: pool lock -> motions lock
            bgload_close_part(part);
        }
        entry.motions.clear();
        pool.Instance_Register(entry.key.c_str(), entry.model);
    }
    s_bgload_bases.insert(entry.key);
    dxRender_Visual* V = entry.model;
    entry.model = nullptr;
    return V;
}

// Create (under ModelPool_Mutex), a base not in the pool: the thread's model of this key, finished here
dxRender_Visual* bgload_take(CModelPool& pool, pcstr key)
{
    ModelBgLoad* B = s_bgload;
    if (!B || !B->any_ready.load(std::memory_order_acquire))
        return nullptr;
    BgLoadReady entry;
    {
        std::lock_guard<std::mutex> lk(B->mutex);
        const auto it =
            std::find_if(B->ready.begin(), B->ready.end(), [key](const BgLoadReady& r) { return r.key == key; });
        if (it == B->ready.end())
            return nullptr;
        entry = std::move(*it);
        B->ready.erase(it);
        B->any_ready.store(!B->ready.empty(), std::memory_order_release);
    }
    return bgload_complete(pool, entry);
}

// DeleteQueue (the main thread, ModelDeferredClear): the loaded models, in the order they came, for up to
// kBgLoadFinishUs (one at least)
void bgload_finish(CModelPool& pool)
{
    ModelBgLoad* B = s_bgload;
    if (!B || !B->any_ready.load(std::memory_order_acquire))
        return;
    const u64 t0 = prefetch_now_us();
    for (;;)
    {
        BgLoadReady entry;
        {
            std::lock_guard<std::mutex> lk(B->mutex);
            if (B->ready.empty())
                return;
            entry = std::move(B->ready.front());
            B->ready.erase(B->ready.begin());
            B->any_ready.store(!B->ready.empty(), std::memory_order_release);
        }
        bgload_complete(pool, entry);
        if (prefetch_now_us() - t0 >= kBgLoadFinishUs)
            return;
    }
}

// The main thread, no lock of the render held: waits for the load in progress (one model at most), drops the queue and
// frees the loaded models not finished yet. ClearPool: a level unload, the end of a game.
void bgload_flush()
{
    ModelBgLoad* B = s_bgload;
    if (!B)
        return;
    xr_vector<BgLoadReady> ready;
    {
        std::unique_lock<std::mutex> lk(B->mutex);
        B->cv.wait(lk, [B] { return !B->busy; }); // no new job while the lock is held, none queued after it
        B->queue.clear();
        B->spawn_jobs = 0;
        B->taken.clear(); // the next level may want them again
        ready.swap(B->ready);
        B->any_ready.store(false, std::memory_order_release);
        bgload_publish(*B);
    }
    for (BgLoadReady& entry : ready)
    {
        bgload_free(entry);
        PERF_INC(mbg_discarded);
    }
}

// A device reset (CRender::reset_begin / reset_end): no load across it. on: waits for the load in progress.
void bgload_pause(bool on)
{
    ModelBgLoad* B = s_bgload;
    if (!B)
        return;
    std::unique_lock<std::mutex> lk(B->mutex);
    if (on)
    {
        ++B->paused;
        B->cv.wait(lk, [B] { return !B->busy; });
        return;
    }
    if (B->paused)
        --B->paused;
    lk.unlock();
    B->cv.notify_all();
}

// ~CModelPool, in two steps: the thread first (it reads the session set of the file reader), the state after the file
// reader, which hands it the models (bgload_offer ignores them once `stop` is set)
void bgload_shutdown()
{
    ModelBgLoad* B = s_bgload;
    if (!B)
        return;
    {
        std::lock_guard<std::mutex> lk(B->mutex);
        B->stop.store(true, std::memory_order_relaxed);
        B->queue.clear();
        B->spawn_jobs = 0;
        bgload_publish(*B);
    }
    B->cv.notify_all();
    if (B->thread.joinable())
        B->thread.join(); // one model at most
    for (BgLoadReady& entry : B->ready)
        bgload_free(entry);
    B->ready.clear();
    B->any_ready.store(false, std::memory_order_release);
}

void bgload_delete() { xr_delete(s_bgload); }

// A slow disk: the dds of the model and of its LOD not loaded yet, read into the OS cache before the load (the "Model
// prefetch" thread reads no dds there, 50i R5) - outside every lock, in the single background I/O slot taken per file
// (the jobs of the "Model prefetch" thread get in between: a creature's dds are tens of MB, a second and more on a USB
// HDD); the texture loads of the shader creation then do not wait for the disk under m_textures_lock / m_create_lock
void bgload_warm_textures(const ModelBgLoad& B, const xr_string& name)
{
    const WarmRoots roots = warm_roots_now();
    if (roots.textures.empty())
        return;
    xr_vector<u8> ogf;
    const auto read_model = [&](const xr_string& rel) {
        if (!roots.level.empty() && warm_read_whole(roots.level + rel, ogf))
            return true;
        return !roots.meshes.empty() && warm_read_whole(roots.meshes + rel, ogf);
    };
    if (!read_model(name + ".ogf")) // the "Model prefetch" thread has just read it: from the OS cache
        return;
    OgfRefs refs;
    warm_parse_ogf(ogf, refs, true);
    if (!refs.lod.empty() && read_model(warm_strip_ext(refs.lod) + ".ogf"))
        warm_parse_ogf(ogf, refs, false);
    std::sort(refs.textures.begin(), refs.textures.end());
    refs.textures.erase(std::unique(refs.textures.begin(), refs.textures.end()), refs.textures.end());
    WarmSeen* seen = s_file_prefetch ? &s_file_prefetch->seen : nullptr;
    xr_vector<u8> buf(kWarmChunk);
    for (const xr_string& t : refs.textures)
    {
        if (RImplementation.Resources->_TextureRegistered(t.c_str()))
            continue; // loaded already: the load finds it
        const xr_string base = roots.textures + warm_strip_ext(t);
        for (pcstr ext : { ".dds", "_bump.dds", "_bump#.dds" })
        {
            const xr_string path = base + ext;
            if (seen && !seen->claim(path))
                continue; // read for another model in this session
            xray::io::BackgroundIoScope io;
            if (!io.enter(xray::io::BackgroundIoClient::Model, [&B] { return B.stop.load(std::memory_order_relaxed); }))
                return; // shutting down
            warm_read_discard(path, buf, kWarmMaxModelBytes, B.stop);
        }
    }
}

// One job of the thread: the base model of `name` into `out` (out.model stays null when there is nothing to load)
void bgload_one(ModelBgLoad& B, const xr_string& name, BgLoadReady& out)
{
    CModelPool& pool = *B.pool;
    if (pool.InPool(name.c_str()))
        return; // a base or a free instance already (loaded meanwhile)
    // Instance_Load is fatal without the ogf: none - nothing here, the spawn reports it as before
    string_path file, fn;
    strconcat(sizeof(file), file, name.c_str(), ".ogf");
    if (!FS.exist(fn, "$level$", file) && !FS.exist(fn, "$game_meshes$", file))
        return;
    ZoneScopedN("model_bg_load");
    ZoneText(name.c_str(), name.size());
    PERF_SCOPE(mbg_time);
    if (xray::io::BackgroundIoSingle())
        bgload_warm_textures(B, name);
    PrefetchTop top;
    top.background = true;
    tl_prefetch_top = &top;
    dxRender_Visual* V = pool.Instance_Load(name.c_str(), "", FALSE);
    tl_prefetch_top = nullptr;
    out.key = name;
    out.model = V;
    out.motions = std::move(top.parts);
    if (!V)
        return; // the ogf could not be opened (no motions part deferred then)
    for (PrefetchMotionsPart& part : out.motions)
        part.model->PreloadMotions(part.name.c_str(), part.data);
    PERF_INC(mbg_loaded);
}

void bgload_thread(ModelBgLoad* B)
{
    for (;;)
    {
        xr_string name;
        {
            std::unique_lock<std::mutex> lk(B->mutex);
            B->cv.wait(lk, [B] { return B->stop.load(std::memory_order_relaxed) || (!B->paused && !B->queue.empty()); });
            if (B->stop.load(std::memory_order_relaxed))
                return;
            // Not on the loading screen: the prefetch and the preload of the level run then (the main thread, the load
            // threads); the jobs wait for the play
            if (modelpool_loading())
            {
                B->cv.wait_for(lk, std::chrono::milliseconds(50));
                continue;
            }
            name = std::move(B->queue.front());
            B->queue.pop_front();
            if (B->spawn_jobs)
                --B->spawn_jobs;
            B->current = name;
            B->busy = true;
            bgload_publish(*B);
        }
        BgLoadReady entry;
        bgload_one(*B, name, entry);
        {
            std::lock_guard<std::mutex> lk(B->mutex);
            B->busy = false;
            B->current.clear();
            if (entry.model)
            {
                B->ready.push_back(std::move(entry));
                B->any_ready.store(true, std::memory_order_release);
            }
            bgload_publish(*B);
        }
        B->cv.notify_all(); // a flush / pause waits for `busy` cleared
    }
}

// The pool key of a model name: lower case, no extension (as Create builds low_name)
void model_key(pcstr name, string_path& out)
{
    xr_strcpy(out, name);
    xr_strlwr(out);
    if (strext(out))
        *strext(out) = 0;
}

std::string_view model_name_view(const shared_str& name) { return std::string_view(name.c_str(), name.size()); }
} // namespace

bool ModelPoolColdLoad() { return g_modelpool_cold_depth > 0; }

// 50a-stage1-prefetch.md (plans/optimization_mt, plan 50 R3)
bool ModelPoolDiskSlow() { return disk_slow_by_profile() || s_disk_slow.load(std::memory_order_relaxed); }

bool ModelPoolWarmFile(pcstr path)
{
    if (!path || !path[0])
        return false;
    static const std::atomic_bool never_stop{ false };
    xr_vector<u8> buf(kWarmChunk);
    return warm_read_discard(xr_string(path), buf, type_max<u64>, never_stop) >= 0;
}

// 40-parallel-prefetch.md
ModelMotionsScope::ModelMotionsScope()
{
    // A load thread of the parallel prefetch, a motions part not deferred (a fallback: ModelMotionsDefer defers all):
    // the parts of the models before this one in the list first (they may dock the same omf or rewrite the same
    // partition - the first / the last of the list must win, as with the sequential prefetch). See wait_turn.
    if (tl_prefetch_load >= 0 && s_parallel_prefetch)
        s_parallel_prefetch->wait_turn(static_cast<size_t>(tl_prefetch_load));
    s_motions_lock.Enter();
}

bool ModelMotionsDefer(CKinematicsAnimated* K, pcstr N, IReader* data)
{
    // Every motions part of a load on a load thread is deferred: of the model of the list and of the models loaded
    // inside its load (its LOD: CKinematics::Load -> CreateChild by name). The chain runs the parts of a load in the
    // order they were deferred - the order the sequential load ran them - after the parts of the loads before it and
    // before the ones after it. A model loaded from its own ogf (the reader of the innermost Instance_Load(name) on
    // the thread): Instance_Load leaves the ogf open, the chain closes it. A model loaded from a chunk of another file
    // (Instance_Load with a reader, its owner closes the chunk right after): the part reads a copy of the chunk.
    // 50l-weak-cpu-spawn-models.md: also every motions part of a load of the "Model bg load" thread - the main thread
    // runs them when it takes the model (bgload_complete)
    PrefetchTop* top = tl_prefetch_top;
    if (!top || !data || (!top->background && !s_parallel_prefetch))
        return false;
    PrefetchMotionsPart part;
    part.model = K;
    part.name = N;
    if (!top->readers.empty() && top->readers.back().data == data && !top->readers.back().deferred)
    {
        top->readers.back().deferred = true;
        part.data = data;
    }
    else
    {
        const size_t pos = data->tell();
        data->seek(0);
        part.copy.assign(static_cast<const u8*>(data->pointer()), static_cast<const u8*>(data->pointer()) +
            data->length());
        data->seek(pos);
        part.data = xr_new<IReader>(part.copy.data(), part.copy.size(), 0);
        part.copied = true;
    }
    if (!top->background && (top->readers.size() > 1 || part.copied))
        s_parallel_prefetch->deferred_nested.fetch_add(1, std::memory_order_relaxed);
    top->parts.push_back(std::move(part));
    return true;
}

ModelMotionsScope::~ModelMotionsScope() { s_motions_lock.Leave(); }

ModelPoolDeferDeletes::ModelPoolDeferDeletes() { s_modelpool_defer_deletes.fetch_add(1, std::memory_order_relaxed); }
ModelPoolDeferDeletes::~ModelPoolDeferDeletes() { s_modelpool_defer_deletes.fetch_sub(1, std::memory_order_relaxed); }

dxRender_Visual* CModelPool::Instance_Create(u32 type)
{
    dxRender_Visual* V = nullptr;

    // Check types
    switch (type)
    {
    case MT_NORMAL: // our base visual
        V = xr_new<Fvisual>();
        break;
    case MT_HIERRARHY: V = xr_new<FHierrarhyVisual>(); break;
    case MT_PROGRESSIVE: // dynamic-resolution visual
        V = xr_new<FProgressive>();
        break;
    case MT_SKELETON_ANIM: V = xr_new<CKinematicsAnimated>(); break;
    case MT_SKELETON_RIGID: V = xr_new<CKinematics>(); break;
    case MT_SKELETON_GEOMDEF_PM: V = xr_new<CSkeletonX_PM>(); break;
    case MT_SKELETON_GEOMDEF_ST: V = xr_new<CSkeletonX_ST>(); break;
    case MT_PARTICLE_EFFECT: V = xr_new<PS::CParticleEffect>(); break;
    case MT_PARTICLE_GROUP: V = xr_new<PS::CParticleGroup>(); break;
#ifndef _EDITOR
    case MT_LOD: V = xr_new<FLOD>(); break;
    case MT_TREE_ST: V = xr_new<FTreeVisual_ST>(); break;
    case MT_TREE_PM: V = xr_new<FTreeVisual_PM>(); break;
#endif
    default: FATAL("Unknown visual type"); break;
    }
    R_ASSERT(V);
    V->Type = type;
    return V;
}

dxRender_Visual* CModelPool::Instance_Duplicate(dxRender_Visual* V)
{
    GW_OVERLAP_HAZARD("CModelPool::Instance_Duplicate");
    ModelPool_mtlock lock;
    R_ASSERT(V);
    PERF_HOT_ZONE("model_pool/duplicate");
    PERF_SCOPE(modelpool_duplicate);
    dxRender_Visual* N = Instance_Create(V->Type);
    N->Copy(V);
    N->Spawn();
    // inc ref counter
    for (xr_vector<ModelDef>::iterator I = Models.begin(); I != Models.end(); ++I)
        if (I->model == V)
        {
            I->refs++;
            break;
        }
    return N;
}

dxRender_Visual* CModelPool::Instance_Load(const char* N, BOOL allow_register)
{
    return Instance_Load(N, "", allow_register);
}
dxRender_Visual* CModelPool::Instance_Load(const char* N, LPCSTR suffix, BOOL allow_register)
{
    // 40-parallel-prefetch.md: on a load thread of the parallel prefetch the motions part of the model loaded from
    // this ogf may be deferred (ModelMotionsDefer), then the ogf stays open
    PrefetchTop* top = tl_prefetch_top;

    string_path fn;
    string_path name;

    // Add default ext if no ext at all
    if (nullptr == strext(N))
        strconcat(sizeof(name), name, N, ".ogf");
    else
        xr_strcpy(name, sizeof(name), N);

    // Load data from MESHES or LEVEL
    IReader* data;
    {
    MODEL_LOAD_PART("model_load/open", mload_open); // 36-spawn-model-load.md
    if (!FS.exist(N))
    {
        if (!FS.exist(fn, "$level$", name))
            if (!FS.exist(fn, "$game_meshes$", name))
            {
#ifdef _EDITOR
                Msg("! Can't find model file '%s'.", name);
                return 0;
#else
                xrDebug::Fatal(DEBUG_INFO, "Can't find model file '%s'.", name);
#endif
            }
    }
    else
    {
        xr_strcpy(fn, N);
    }

// Actual loading
#ifdef DEBUG
    if (bLogging)
        Msg("- Uncached model loading: %s", fn);
#endif // DEBUG

    data = FS.r_open(fn);
    }
    if (!data)
    {
        Msg("! model Instance_Load: FS.r_open failed for [%s] (resolved path [%s])", name, fn);
#ifdef _EDITOR
        return 0;
#else
        return nullptr;
#endif
    }
    ogf_header H;
    data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
    dxRender_Visual* V = Instance_Create(H.type);
    bool deferred = false;
    if (top)
    {
        top->readers.push_back({ data, false });
        V->Load(N, suffix, data, 0);
        deferred = top->readers.back().deferred;
        top->readers.pop_back();
    }
    else
        V->Load(N, suffix, data, 0);
    if (!deferred)
        FS.r_close(data); // else the deferred motions part reads it later, the motions chain closes it

    // Register material
    switch (H.type)
    {
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        MODEL_LOAD_PART("model_load/materials", mload_materials); // 36-spawn-model-load.md
        const u16 def_idx = GMLib.GetMaterialIdx("default_object");
        R_ASSERT2(GMLib.GetMaterialByIdx(def_idx)->Flags.is(SGameMtl::flDynamic), "'default_object' - must be dynamic");
        auto* K = static_cast<CKinematics*>(V);
        VERIFY(K);
        const u16 cnt = K->LL_BoneCount();
        for (u16 k = 0; k < cnt; k++)
        {
            CBoneData& bd = K->LL_GetData(k);
            if (bd.game_mtl_name.c_str())
            {
                bd.game_mtl_idx = GMLib.GetMaterialIdx(bd.game_mtl_name.c_str());
                R_ASSERT2(GMLib.GetMaterialByIdx(bd.game_mtl_idx)->Flags.is(SGameMtl::flDynamic),
                    "Required dynamic game material");
            }
            else
            {
                bd.game_mtl_idx = def_idx;
            }
        }
    }
    break;
    } // switch (V->getType())

    // Registration
    if (allow_register)
    {
        string_path low_name_suffix;
        xr_strcpy(low_name_suffix, N);
        if (strlen(suffix) > 0)
            xr_strcat(low_name_suffix, suffix);
        Instance_Register(low_name_suffix, V);
    }

    return V;
}

dxRender_Visual* CModelPool::Instance_Load(LPCSTR name, IReader* data, BOOL allow_register)
{
    return Instance_Load(name, "", data, allow_register);
}
dxRender_Visual* CModelPool::Instance_Load(LPCSTR name, LPCSTR suffix, IReader* data, BOOL allow_register)
{
    if (!data)
    {
        Msg("! model Instance_Load: null IReader for [%s]", name);
        return nullptr;
    }

    dxRender_Visual* V;

    ogf_header H;
    data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
    V = Instance_Create(H.type);
    V->Load(name, suffix, data, 0);

    // Registration
    if (allow_register)
    {
        string_path low_name_suffix;
        xr_strcpy(low_name_suffix, name);
        if (strlen(suffix) > 0)
            xr_strcat(low_name_suffix, suffix);
        Instance_Register(low_name_suffix, V);
    }
    return V;
}

void CModelPool::Instance_Register(LPCSTR N, dxRender_Visual* V)
{
    // Registration
    ModelDef M;
    M.name = N;
    M.model = V;
    Models.push_back(M);
    // 36-spawn-model-load.md: the name index (Instance_Find). The view points into the shared_str of the entry, alive
    // while the entry is. A second entry of the same name keeps the first one, as the walk.
    if (Models.back().name.size())
        ModelsIndex.emplace(model_name_view(Models.back().name), V);
}

void CModelPool::IndexErase(const shared_str& name, const dxRender_Visual* model)
{
    // 36-spawn-model-load.md: called before the entry leaves Models
    const auto it = ModelsIndex.find(model_name_view(name));
    if (it == ModelsIndex.end() || it->second != model)
        return;
    ModelsIndex.erase(it);
    for (const ModelDef& other : Models)
    {
        if (other.model != model && other.name == name)
        {
            ModelsIndex.emplace(model_name_view(other.name), other.model);
            break;
        }
    }
}

void CModelPool::Destroy()
{
    ModelPool_mtlock lock;
    // Pool
    Pool.clear();

    // Registry
    while (!Registry.empty())
    {
        REGISTRY_IT it = Registry.begin();
        dxRender_Visual* V = (dxRender_Visual*)it->first;
#ifdef _DEBUG
        Msg("ModelPool: Destroy object: '%s'", V->dbg_name.c_str());
#endif
        DeleteInternal(V, TRUE);
    }

    // Base/Reference
    for (auto& md : Models)
    {
        md.model->Release();
        xr_delete(md.model);
    }

    Models.clear();
    ModelsIndex.clear(); // 36-spawn-model-load.md
    s_bgload_bases.clear(); // 50l-weak-cpu-spawn-models.md
    InstanceBytes.clear(); // 50c-commit-growth-analysis.md

    // cleanup motions container
    g_pMotionsContainer->clean(false);
}

CModelPool::CModelPool()
{
    bLogging = TRUE;
    bForceDiscard = FALSE;
    g_pMotionsContainer = xr_new<motions_container>();
    if (!s_file_prefetch)
        s_file_prefetch = xr_new<ModelFilePrefetch>(); // 36-spawn-model-load.md: the thread starts at the first model
    if (!s_bgload)
    {
        s_bgload = xr_new<ModelBgLoad>(); // 50l-weak-cpu-spawn-models.md: the thread starts at the first model
        s_bgload->pool = this;
    }
}

CModelPool::~CModelPool()
{
    early_warm_shutdown(); // 50k-slow-disk-early-prefetch.md: before the session set it may merge into goes
    bgload_shutdown(); // 50l-weak-cpu-spawn-models.md: its thread before the file reader (it reads its session set)
    file_prefetch_shutdown(); // 36-spawn-model-load.md
    bgload_delete(); // 50l: after the file reader, which hands it the models
    Destroy();
    xr_delete(g_pMotionsContainer);
}

dxRender_Visual* CModelPool::Instance_Find(LPCSTR N)
{
    // 36-spawn-model-load.md: the hash index instead of the walk over Models (always since 2026-10-18, see plan 41)
    if (!N || !N[0])
        return nullptr;
    const auto it = ModelsIndex.find(std::string_view(N));
    return it != ModelsIndex.end() ? it->second : nullptr;
}

dxRender_Visual* CModelPool::Create(const char* name, IReader* data)
{
    // No GW_OVERLAP_HAZARD here: this just forwards to the 3-arg overload below, which already
    // fires it - tagging both would double-count every call through this path.
    return Create(name, "", data);
}
dxRender_Visual* CModelPool::Create(LPCSTR name, LPCSTR suffix, IReader* data)
{
    GW_OVERLAP_HAZARD("CModelPool::Create");
    // Spawn stage 0 (plans/spawn_optimization/SPEC.md): pool hit / find / cold load / copy, see modelpool/* metrics
    PERF_SCOPE(modelpool_create);
    ModelPool_mtlock lock;
#ifdef _EDITOR
    if (!name || !name[0])
        return 0;
#endif
    string_path low_name;
    string_path low_name_suffix;
    VERIFY(xr_strlen(name) < sizeof(low_name));
    xr_strcpy(low_name, name);
    xr_strlwr(low_name);
    if (strext(low_name))
        *strext(low_name) = 0;
    xr_strcpy(low_name_suffix, low_name);
    if (strlen(suffix) > 0)
        xr_strcat(low_name_suffix, suffix);

    // 0. Search POOL
    POOL_IT it = Pool.find(low_name_suffix);
    if (it != Pool.end())
    {
        // 1. Instance found
        PERF_HOT_ZONE("model_pool/pool_hit");
        PERF_INC(modelpool_pool_hit);
        dxRender_Visual* Model = it->second;
        Model->Spawn();
        Pool.erase(it);
        return Model;
    }
    else
    {
        // 1. Search for already loaded model (reference, base model)
        dxRender_Visual* Base;
        {
            PERF_HOT_ZONE("model_pool/find");
            PERF_SCOPE(modelpool_find);
            Base = Instance_Find(low_name_suffix);
        }

        // 50l-weak-cpu-spawn-models.md: a base the "Model bg load" thread made - the first use (a spawn found it ready)
        if (Base && !s_bgload_bases.empty() && s_bgload_bases.erase(low_name_suffix))
            PERF_INC(mbg_used);
        // ... or one it has loaded and the main thread has not finished yet (DeleteQueue): finished here, no cold load
        if (nullptr == Base && !data && !suffix[0])
        {
            Base = bgload_take(*this, low_name);
            if (Base && s_bgload_bases.erase(low_name_suffix))
                PERF_INC(mbg_used);
        }

        if (nullptr == Base)
        {
            // 2. If not found: a cold load of the ogf (under ModelPool_Mutex), in play / inside a client spawn apart
            ZoneScopedN("model_pool/instance_load");
            ZoneText(low_name_suffix, xr_strlen(low_name_suffix));
            PERF_SCOPE(modelpool_instance_load);
            PERF_SCOPE_IF(!modelpool_loading(), modelpool_instance_load_play);
            PERF_SCOPE_IF(xray::perf::TlSpawn() != 0, modelpool_instance_load_spawn);
            PERF_SCOPE_IF(xray::perf::TlGameEvent() != 0, ge_cold_model_load); // 30-game-events.md
            ModelColdLoadScope cold_scope; // 36-spawn-model-load.md: modelpool/load/*, TlColdModelLoads
            // 50l: the cold loads in play while the background loading is on; late = the thread had the model
            if (s_bgload && bgload_enabled() && !modelpool_loading())
            {
                PERF_INC(mbg_cold);
                if (bgload_cold(xr_string(low_name)))
                    PERF_INC(mbg_late);
            }
            if (s_file_prefetch && s_file_prefetch->used.load(std::memory_order_relaxed))
            {
                // 36-spawn-model-load.md: had the "Model prefetch" thread read the files of this model? (metrics)
                // 50-smart-engine-hw-profile.md R5: the model is loaded now, its job still in the queue is of no use -
                // dropped. Without metrics the lock of the queue is not waited for (the thread holds it for a pop only):
                // when busy the job stays, a needless read but no wait here.
                ModelFilePrefetch& FP = *s_file_prefetch;
                const bool metrics = xray::perf::g_perf_metrics_active;
                std::unique_lock<std::mutex> lk(FP.mutex, std::defer_lock);
                if (metrics)
                    lk.lock();
                else
                    (void)lk.try_lock();
                if (lk.owns_lock())
                {
                    const xr_string key(low_name);
                    const bool warmed = metrics && FP.warmed.count(key) != 0;
                    const auto job = std::find_if(FP.queue.begin(), FP.queue.end(),
                        [&key](const ModelFilePrefetch::Job& j) { return j.name == key; });
                    const bool dropped = job != FP.queue.end();
                    if (dropped)
                    {
                        if (static_cast<size_t>(job - FP.queue.begin()) < FP.spawn_jobs)
                            --FP.spawn_jobs; // 50i: one of the spawn jobs at the head
                        FP.queue.erase(job);
                        file_prefetch_publish(FP);
                    }
                    const size_t pending = FP.queue.size();
                    lk.unlock();
                    PERF_ADD_IF(metrics && warmed, mprefetch_cold_warm, 1);
                    PERF_ADD_IF(metrics && !warmed, mprefetch_cold_miss, 1);
                    PERF_ADD_IF(dropped, mprefetch_dropped, 1);
                    if (dropped)
                        xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, static_cast<s64>(pending));
                }
            }
            if (data)
                Base = Instance_Load(low_name, suffix, data, TRUE);
            else
                Base = Instance_Load(low_name, suffix, TRUE);
            if (!Base)
                return nullptr;
        }
        // 3. If found - return (cloned) reference
        dxRender_Visual* Model = Instance_Duplicate(Base);
        Registry.emplace(Model, low_name_suffix);
        return Model;
    }
}

dxRender_Visual* CModelPool::CreateChild(LPCSTR name, IReader* data)
{
    // No GW_OVERLAP_HAZARD here: forwards to the 3-arg overload below, which already fires it.
    return CreateChild(name, "", data);
}
dxRender_Visual* CModelPool::CreateChild(LPCSTR name, LPCSTR suffix, IReader* data)
{
    GW_OVERLAP_HAZARD("CModelPool::CreateChild");
    string256 low_name;
    string256 low_name_suffix;
    VERIFY(xr_strlen(name) < 256);
    xr_strcpy(low_name, name);
    xr_strlwr(low_name);
    if (strext(low_name))
        *strext(low_name) = 0;
    xr_strcpy(low_name_suffix, low_name);
    if (strlen(suffix) > 0)
        xr_strcat(low_name_suffix, suffix);

    // 1. Search for already loaded model
    // 40-parallel-prefetch.md (plans/optimization_mt): the pool lock only for the search. The child is always loaded
    // privately (below) and its load does not touch the pool; the load threads of the parallel prefetch load children
    // at once (under the lock they would go one at a time).
    bool registered_base;
    {
        ModelPool_mtlock lock;
        MODEL_LOAD_PART("model_load/child_find", mload_child_find); // 36-spawn-model-load.md
        registered_base = nullptr != Instance_Find(low_name_suffix);
    }

    // A registered Models base must never be handed out for ownership: nested owners
    // (skeleton LOD in ~CKinematics, hierrarhy children in ~FHierrarhyVisual) free
    // visuals with xr_delete, which would leave a dangling Models entry and a
    // use-after-free for every other user of that base. A duplicate would not do
    // either: CKinematics::Copy shares bones/pUserData/bone_map with the base, and
    // the owner marks the child m_is_original_lod and calls Release() on it - the
    // shared data of the base would be freed twice. The child gets a private load.
    if (registered_base)
    {
        Msg("! [ModelPool] CreateChild: child '%s' matches a registered base model, "
            "loaded privately to own its data", low_name_suffix);
    }
    if (data)
        return Instance_Load(low_name, suffix, data, FALSE);
    return Instance_Load(low_name, suffix, FALSE);
}

void CModelPool::DeleteInternal(dxRender_Visual*& V, BOOL bDiscard)
{
    GW_OVERLAP_HAZARD("CModelPool::DeleteInternal");
    ModelPool_mtlock lock;
    VERIFY(!g_bRendering);
    if (!V)
        return;
    V->Depart();
    if (bDiscard || bForceDiscard)
    {
        Discard(V, TRUE);
    }
    else
    {
        //
        REGISTRY_IT it = Registry.find(V);
        if (it != Registry.end())
        {
            // Registry entry found - move it to pool
            Pool.emplace(it->second, V);
        }
        else
        {
            // Registry entry not-found - just special type of visual / particles / etc.
            xr_delete(V);
        }
    }
    V = nullptr;
}

void CModelPool::Delete(dxRender_Visual*& V, BOOL bDiscard)
{
    GW_OVERLAP_HAZARD("CModelPool::Delete");
    // The owner gives the model up now, even if it is freed only later: blend callbacks a render thread deferred to
    // after the join must not call into an owner that may be destroyed by then
    if (V)
    {
        if (CKinematicsAnimated* animated = PKinematicsAnimated(V))
            animated->DeferredBlendEvents_OwnerRelease();
    }
    ModelPool_mtlock lock;
    if (nullptr == V)
        return;
    // g_bRendering alone used to gate this (and only ever saw bDiscard=FALSE - bDiscard=TRUE was
    // asserted against). Device.IsFrameOverlapActive() is the broader P1 overlap window (see device.h
    // for why it is a separate flag from g_bRendering); with the overlap in place bDiscard=TRUE calls
    // can happen inside the window same as FALSE ones, so both defer the same way. ModelPoolDeferDeletes: game
    // callbacks run on main after the window (deferred blend events) get the same guarantee.
    if (g_bRendering || Device.IsFrameOverlapActive() ||
        s_modelpool_defer_deletes.load(std::memory_order_relaxed) > 0)
    {
        ModelsToDelete.emplace_back(V, bDiscard);
    }
    else
    {
        DeleteInternal(V, bDiscard);
    }
    V = nullptr;
}

void CModelPool::DeleteQueue()
{
    ModelPool_mtlock lock;
    // A destructor run by DeleteInternal() may call Delete() again and push into ModelsToDelete - iterate a
    // swapped-out copy so the vector we walk never reallocates, and repeat until nothing new was queued.
    while (!ModelsToDelete.empty())
    {
        decltype(ModelsToDelete) local;
        local.swap(ModelsToDelete);
        for (auto& [visual, discard] : local)
            DeleteInternal(visual, discard);
    }
    // 50l-weak-cpu-spawn-models.md: ModelDeferredClear - the main thread, nothing else runs: the base models the "Model
    // bg load" thread has loaded get their motions part and go into the pool
    bgload_finish(*this);
}

void CModelPool::Discard(dxRender_Visual*& V, BOOL b_complete)
{
    ModelPool_mtlock lock;
    //
    REGISTRY_IT it = Registry.find(V);
    if (it != Registry.end())
    {
        // Pool - OK

        // Base
        const shared_str& name = it->second;
        xr_vector<ModelDef>::iterator I = Models.begin();
        xr_vector<ModelDef>::iterator I_e = Models.end();

        for (; I != I_e; ++I)
        {
            if (I->name == name)
            {
                if (b_complete || strchr(name.c_str(), '#'))
                {
                    VERIFY(I->refs > 0);
                    I->refs--;
                    if (0 == I->refs)
                    {
                        IndexErase(I->name, I->model); // 36-spawn-model-load.md
                        bForceDiscard = TRUE;
                        I->model->Release();
                        xr_delete(I->model);
                        Models.erase(I);
                        bForceDiscard = FALSE;
                    }
                    break;
                }
                else
                {
                    if (I->refs > 0)
                        I->refs--;
                    break;
                }
            }
        }
        // Registry
        xr_delete(V);
        //.		xr_free			(name);
        Registry.erase(it);
    }
    else
    {
        // Registry entry not-found - just special type of visual / particles / etc.
        xr_delete(V);
    }
    V = nullptr;
}

void CModelPool::Prefetch()
{
    ZoneScoped;
    Logging(FALSE);
    // prefetch visuals
    string256 section;
    strconcat(sizeof(section), section, "prefetch_visuals_", g_pGamePersistent->m_game_params.m_game_type);
    // 50a-stage1-prefetch.md (plans/optimization_mt): in the global priority order, the models of the prefix that
    // Prefetch() took from both lists by the budget (plan 50 R1; PrefetchMode::Full - all of it)
    xr_vector<PrefetchListItem> list;
    PrefetchListRead(section, list);
    PrefetchListTake("models", list);

    // The pool keys of the list
    xr_vector<ModelPrefetchItem> items;
    items.reserve(list.size());
    for (const PrefetchListItem& item : list)
    {
        string_path key;
        model_key(item.name.c_str(), key);
        items.push_back({ key, "", item.bytes });
    }
    PrefetchItems(items, "models", true);
    Logging(TRUE);
}

// 40-parallel-prefetch.md (plans/optimization_mt): IGame_ObjectPool::prefetch - the visuals the Load of the prefetch
// objects creates, before it, on the load threads. Where the prefetch is sequential (GL) nothing:
// Load creates them on main, as before.
void CModelPool::PrefetchList(const xr_vector<PrefetchModel>& visuals)
{
    if (visuals.empty())
        return;
    xr_vector<ModelPrefetchItem> items;
    items.reserve(visuals.size());
    for (const PrefetchModel& visual : visuals)
    {
        if (!visual.name.size())
            continue;
        string_path key;
        model_key(visual.name.c_str(), key);
        items.push_back({ key, visual.suffix.size() ? visual.suffix.c_str() : "", visual.bytes });
    }
    PrefetchItems(items, "objects", false);
}

// 50k-slow-disk-early-prefetch.md (plans/optimization_mt): the early warm-up of a slow disk, see EarlyWarm. Main thread.
bool CModelPool::EarlyWarmStart(const xr_vector<PrefetchModel>& models, const PrefetchEarlyWarmParams& params)
{
    if (s_early_warm || models.empty() || !params.lead_bytes)
        return false; // once per process
    EarlyWarm* E = xr_new<EarlyWarm>();
    E->params = params;
    E->cum.push_back(0);
    std::unordered_set<xr_string> taken; // a visual of an object can be a line of the model list too
    for (const PrefetchModel& model : models)
    {
        if (!model.name.size())
            continue;
        string_path key;
        model_key(model.name.c_str(), key);
        if (!key[0] || !taken.insert(xr_string(key)).second)
            continue;
        E->names.emplace_back(key);
        E->cum.push_back(E->cum.back() + model.bytes);
    }
    if (E->names.empty())
    {
        xr_delete(E);
        return false;
    }
    EarlyWarm& early = *E;
    early.done.assign(early.names.size(), 0);
    early.roots = warm_roots_now(); // the prefetch of the first level load takes the same ($level$ is set after it)
    early.start_us = prefetch_now_us();
    s_disk_probe.begin(1); // the disk measured by the early reads: the prefetch would find them in the OS cache
    s_early_warm = E;
    early.thread = Threading::RunThread("Model early warm", &early_warm_thread, &early);
    return true;
}

bool CModelPool::EarlyWarmStop(PrefetchEarlyWarmStats& stats, bool release)
{
    stats = {};
    EarlyWarm* E = s_early_warm;
    if (!E)
        return false;
    early_warm_join(*E);
    if (release && !E->released)
    {
        early_warm_merge(*E); // no prefetch took them (or it did already): the files are the session's all the same
        E->released = true;
    }
    early_warm_stats(*E, stats);
    return true;
}

// Loads the models of the list into the pool (for each item: Create(name, suffix) + Delete, an instance stays in Pool).
// sequential_too: without load threads (GL, < 3 hardware threads) load them on main, else nothing.
void CModelPool::PrefetchItems(const xr_vector<ModelPrefetchItem>& items, pcstr label, bool sequential_too)
{
    ZoneScoped;
    const xray::hw::Policy& policy = xray::hw::GetPolicy();

    // 40-parallel-prefetch.md (plans/optimization_mt): threads that load the models of the list (D3D11 creates its
    // resources free-threaded; on GL the objects of the context are made on its thread only, the load stays
    // sequential). Always on since 2026-10-18 (r__prefetch_mt removed, see plan 41).
    // 50a-stage1-prefetch.md (plan 50 R3): their number from the hw policy - min(worker pool, 8), none below 2 or
    // without D3D11 DriverConcurrentCreates (the tester: 8, as min(hw - 1, 8) gave before)
#if defined(USE_DX11)
    constexpr bool parallel_load_supported = true;
#else
    constexpr bool parallel_load_supported = false;
#endif
    u32 load_n = 0;
    if (parallel_load_supported)
        load_n = std::min<u32>(policy.load_threads, kPrefetchMaxLoadThreads);
    if (load_n < 2)
        load_n = 0;
    if (!load_n && !sequential_too)
        return;

    // 40-parallel-prefetch.md: what the load threads load - the first item of each key that is neither a free instance
    // nor a base model yet (the rest are pool hits or copies of a base, as before)
    ParallelPrefetch P;
    xr_vector<s32> item_load(items.size(), -1);
    xr_vector<xr_string> keys(items.size());
    for (size_t i = 0; i < items.size(); ++i)
        keys[i] = items[i].name + items[i].suffix;
    if (load_n)
    {
        ModelPool_mtlock lock;
        std::unordered_set<xr_string> taken;
        for (size_t i = 0; i < items.size(); ++i)
        {
            pcstr key = keys[i].c_str();
            if (!items[i].name.size() || !taken.insert(keys[i]).second || Pool.find(key) != Pool.end() ||
                Instance_Find(key))
                continue;
            item_load[i] = static_cast<s32>(P.loads.size());
            ParallelPrefetch::Load L;
            L.key = keys[i];
            L.name = items[i].name;
            L.suffix = items[i].suffix;
            L.item = i;
            P.loads.push_back(std::move(L));
        }
        if (P.loads.empty())
        {
            if (!sequential_too)
                return; // all of them are in the pool already
            load_n = 0;
        }
    }

    // 36-spawn-model-load.md: threads that read the files of the list into the OS cache ahead of the load below. They
    // touch no engine state (see warm_model_files).
    // 50a-stage1-prefetch.md (plans/optimization_mt): their number from the hw policy (plan 50 R3) - by the disk of
    // gamedata: NVMe as before (hw - 1 - load threads, at most 16), SATA 4, HDD / USB / network 1 in list order with
    // the loads after it; one after the warm-up measured the disk slow. How far ahead they read and whether they read
    // the dds - from the RAM available at the start of the prefetch (R4); no warm-up at all when it is short.
    const PrefetchWarmLimits limits = PrefetchWarmLimitsNow();
    const bool slow_profile = disk_slow_by_profile();
    int warm_n = limits.lead_bytes ? std::max(1, static_cast<int>(policy.warm_threads)) : 0;
    if (warm_n > 1 && (slow_profile || s_disk_slow.load(std::memory_order_relaxed)))
        warm_n = 1;
    StartupWarm warm;
    u32 early_n = 0; // 50k-slow-disk-early-prefetch.md: items the early warm-up read at the menu
    xr_vector<std::thread> warm_threads;
    if (warm_n > 0)
    {
        if (s_file_prefetch)
            warm.seen = &s_file_prefetch->seen; // 50-smart-engine-hw-profile.md R5: one set for the whole session
        warm.roots = warm_roots_now();
        warm.names.reserve(items.size());
        warm.cum.resize(items.size() + 1);
        warm.cum[0] = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            warm.names.push_back(items[i].name);
            warm.cum[i + 1] = warm.cum[i] + items[i].bytes;
        }
        warm.lead_bytes = limits.lead_bytes;
        warm.with_textures = limits.with_textures;
        warm.slow_start = slow_profile;
        warm.warmed.reset(new std::atomic<u8>[items.size() + 1]);
        for (size_t i = 0; i <= items.size(); ++i)
            warm.warmed[i].store(0, std::memory_order_relaxed);
        // what nobody loads: with the load threads - no load of theirs; sequential - a repeat or already in the pool
        warm.loaded_already.assign(items.size() + 1, 0);
        if (load_n)
        {
            for (size_t i = 0; i < items.size(); ++i)
                warm.loaded_already[i] = item_load[i] < 0 ? 1 : 0;
        }
        else
        {
            ModelPool_mtlock lock;
            std::unordered_set<xr_string> taken;
            for (size_t i = 0; i < items.size(); ++i)
            {
                pcstr key = keys[i].c_str();
                if (!items[i].name.size() || !taken.insert(keys[i]).second || Pool.find(key) != Pool.end() ||
                    Instance_Find(key))
                    warm.loaded_already[i] = 1;
            }
        }
        early_n = early_warm_take(warm); // 50k-slow-disk-early-prefetch.md: read at the menu
        warm.running.store(static_cast<u32>(warm_n), std::memory_order_release);
        s_disk_probe.begin(static_cast<u32>(warm_n));
        for (int t = 0; t < warm_n; ++t)
            warm_threads.emplace_back(Threading::RunThread("Model warm", &startup_warm_thread, &warm));
    }

    CTimer timer;
    timer.Start();
    const u32 models_before = static_cast<u32>(Models.size());

    xr_vector<std::thread> load_threads;
    if (load_n)
    {
        P.pool = this;
        P.warm = warm_n > 0 ? &warm : nullptr;
        P.loaded.assign(P.loads.size(), 0);
        P.done.assign(P.loads.size(), 0);
        s_parallel_prefetch = &P;
        for (u32 t = 0; t < load_n; ++t)
            load_threads.emplace_back(Threading::RunThread("Model load", &prefetch_load_thread, &P));
    }

    u64 main_wait_us = 0;
    for (size_t pos = 0; pos < items.size(); ++pos)
    {
        if (!load_n && warm_n > 0)
        {
            warm.note_main(pos); // with the load threads they move it
            warm.wait_warmed(pos); // 50a-stage1-prefetch.md: a slow disk - after the warm-up
        }

        // 40-parallel-prefetch.md: the model a load thread loaded for this item becomes the base now, in the order of
        // the list, and Create below makes the instance from it - as the sequential load did after its own load
        dxRender_Visual* loaded = nullptr;
        const bool from_thread = item_load[pos] >= 0;
        if (from_thread)
        {
            ZoneScopedN("model_prefetch_wait");
            const u64 t0 = prefetch_now_us();
            loaded = P.wait_result(static_cast<size_t>(item_load[pos]));
            main_wait_us += prefetch_now_us() - t0;
        }

        ModelPool_mtlock lock;
        // The load threads may be in the motions part of a model now; Create / Delete below copy and release shared
        // motions (CKinematicsAnimated::Copy, reference counts)
        ScopeLock motions(&s_motions_lock);
        if (loaded)
        {
            pcstr key = keys[pos].c_str();
            if (!Instance_Find(key))
                Instance_Register(key, loaded);
            else
            {
                // Became a base meanwhile on another thread (none loads models during the prefetch, just in case)
                loaded->Release();
                xr_delete(loaded);
            }
        }
        dxRender_Visual* V = Create(items[pos].name.c_str(), items[pos].suffix.c_str());
        Delete(V, FALSE);
    }
    const u32 load_ms = timer.GetElapsed_ms();

    for (std::thread& t : load_threads)
        t.join();
    s_parallel_prefetch = nullptr;
    warm.stop.store(true, std::memory_order_relaxed);
    for (std::thread& t : warm_threads)
        t.join();
    // 36-spawn-model-load.md: the model stage of the prefetch alone ([prefetch] time of IGame_Persistent is all three)
    Msg("* [prefetch] %s: %u items, %u base models loaded, %u ms (load threads %u)", label,
        static_cast<u32>(items.size()), static_cast<u32>(Models.size()) - models_before, load_ms, load_n);
    if (load_n)
    {
        // 40-parallel-prefetch.md: load - the loads summed over the threads without the deferred motions parts (load /
        // wall time = the parallelism); motions chain - the deferred motions parts, one after another in the order of
        // the list; waits - motions parts not deferred waiting for their turn and loads waiting for the chain;
        // main waited - for the results
        Msg("* [prefetch] %s: %u load threads loaded %u models: load %u ms over the threads, motions chain %u ms "
            "(%u models, %u of them nested), waits %u + %u ms, main waited %u ms",
            label, load_n, static_cast<u32>(P.loads.size()), static_cast<u32>(P.load_us.load() / 1000),
            static_cast<u32>(P.motions_us.load() / 1000), P.deferred.load(), P.deferred_nested.load(),
            static_cast<u32>(P.turn_wait_us.load() / 1000), static_cast<u32>(P.lag_wait_us.load() / 1000),
            static_cast<u32>(main_wait_us / 1000));
    }
    if (warm_n > 0)
    {
        Msg("* [prefetch] %s: %d warm threads read %u files, %u MB (%u missing); %u items %s got to first", label,
            warm_n, warm.files.load(), static_cast<u32>(warm.bytes.load() / (1024 * 1024)), warm.missing.load(),
            warm.behind.load(), load_n ? "the load threads" : "main");
        // 50a-stage1-prefetch.md: the warm-up limits (plan 50 R3 / R4) and, on a slow disk, how long the loads waited
        Msg("* [prefetch] %s: warm lead %u MB of %u MB listed, dds %s, disk %s; loads waited for the warm-up %u ms",
            label, static_cast<u32>(warm.lead_bytes / (1024 * 1024)), static_cast<u32>(warm.cum.back() / (1024 * 1024)),
            warm.with_textures ? "on" : "off",
            !warm.slow() ? "fast" : warm.slow_start ? "slow (hw profile), 1 warm thread" :
                                                      "measured slow, 1 warm thread from then on",
            static_cast<u32>(warm.waited_us.load() / 1000));
        // 50k-slow-disk-early-prefetch.md: the part of the stage the early warm-up read at the menu
        early_warm_note(warm.waited_us.load());
        if (early_n)
        {
            Msg("* [prefetch] %s: %u of %u items were read at the menu by the early warm-up", label, early_n,
                static_cast<u32>(items.size()));
        }
    }
    else
        Msg("* [prefetch] %s: no warm-up (little RAM available, see the [prefetch] mode line)", label);
    if (sequential_too)
        s_disk_probe.finish(); // the last stage of the prefetch: decide on the files read so far
}

// 36-spawn-model-load.md: main thread. Loads the model into the pool unless it is there (a base or a free instance) or
// its ogf does not exist; an instance is left in Pool, as Prefetch does, so the next Create is a pool hit.
bool CModelPool::Preload(LPCSTR name)
{
    if (!name || !name[0])
        return false;
    string_path low_name;
    model_key(name, low_name);
    {
        ModelPool_mtlock lock;
        if (Pool.find(low_name) != Pool.end() || Instance_Find(low_name))
            return false;
    }
    string_path file, fn;
    strconcat(sizeof(file), file, low_name, ".ogf");
    if (!FS.exist(fn, "$level$", file) && !FS.exist(fn, "$game_meshes$", file))
    {
        Msg("! [model_prefetch] no model file '%s', not preloaded", file);
        return false;
    }
    dxRender_Visual* V = Create(low_name);
    if (!V)
        return false;
    Delete(V, FALSE);
    return true;
}

// 36-spawn-model-load.md: any thread. Queues the files of the model for the "Model prefetch" thread, once per session.
// 50-smart-engine-hw-profile.md R5 (plans/optimization_mt 49, W5): not a model with a base or a free instance in the
// pool - its files were read when it was loaded (the thread used to read 1.7-2.8 GB of such models per session).
// 50i-stage3-slow-disk-io.md (R6, R7): on a slow disk the job of a spawn (source 1) goes ahead of the ALife jobs, and a
// spawn asking for a model ALife queued moves its job there. false = put off, the pool lock was held (asked again).
bool CModelPool::PrefetchFiles(LPCSTR name, u32 source)
{
    if (!name || !name[0] || !s_file_prefetch)
        return true;
    string_path low_name;
    model_key(name, low_name);
    ModelFilePrefetch& P = *s_file_prefetch;
    // 50l-weak-cpu-spawn-models.md: source bit 2 - a creature's model, its base model may be loaded in the background
    // (a weak CPU, D3D11; not a '#' name - ClearPool(FALSE) frees those bases); the spawn jobs go first there too
    const bool from_spawn = (source & 1) != 0;
    const bool bg_load = (source & 2) != 0 && bgload_enabled() && !strchr(low_name, '#');
    const bool spawn_first = from_spawn && (xray::io::BackgroundIoSingle() || bgload_enabled());
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        if (P.stop.load(std::memory_order_relaxed))
            return true;
        if (P.queued.count(xr_string(low_name)))
        {
            if (spawn_first && file_prefetch_promote(P, xr_string(low_name)))
                PERF_INC(mprefetch_spawn_first);
            if (bg_load)
            {
                // 50l: read before (as an item, or before a level change dropped its background load): now
                P.bg_names.insert(xr_string(low_name));
                if (P.warmed.count(xr_string(low_name)))
                    bgload_offer(xr_string(low_name), from_spawn);
            }
            return true; // asked before: queued, read or skipped
        }
    }

    // In the pool? Its lock is not waited for: it is held over a whole cold load (tens of ms) on main or the Game
    // thread, and this runs in the ALife update and in ProcessGameEvents. When busy nothing now - the callers ask
    // again (ALife on every update of an object within the margin, ProcessGameEvents on every frame a spawn stays
    // deferred). A thread holding it already gets it (recursive).
    const shared_str pool_key(low_name); // made outside the lock
    if (!ModelPool_Mutex.TryEnter())
    {
        PERF_INC(mprefetch_busy);
        return false;
    }
    const bool loaded = Pool.find(pool_key) != Pool.end() || Instance_Find(low_name);
    ModelPool_Mutex.Leave();

    size_t pending;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        // A loaded model goes to `queued` too: a base stays in the pool for the session as a rule (ClearPool(FALSE)
        // frees the bases of the '#' names only), it is not looked up in the pool on every ask
        if (P.stop.load(std::memory_order_relaxed) || !P.queued.insert(xr_string(low_name)).second)
            return true;
        if (loaded)
        {
            PERF_INC(mprefetch_skipped);
            return true;
        }
        if (P.roots.meshes.empty())
            P.roots = warm_roots_now();
        ModelFilePrefetch::Job job;
        job.name = low_name;
        job.level = warm_root("$level$");
        job.spawn = spawn_first;
        if (bg_load)
            P.bg_names.insert(job.name); // 50l: loaded in the background once read
        if (spawn_first)
        {
            // 50i: behind the spawn jobs queued before, ahead of the ALife ones
            P.queue.insert(P.queue.begin() + static_cast<std::ptrdiff_t>(P.spawn_jobs), std::move(job));
            ++P.spawn_jobs;
            PERF_INC(mprefetch_spawn_first);
        }
        else
            P.queue.push_back(std::move(job));
        file_prefetch_publish(P);
        pending = P.queue.size();
        if (!P.thread.joinable())
            P.thread = Threading::RunThread("Model prefetch", &file_prefetch_thread, &P);
        P.used.store(true, std::memory_order_relaxed);
    }
    P.cv.notify_one();
    PERF_INC(mprefetch_queued);
    PERF_ADD_IF(!from_spawn, mprefetch_alife, 1);
    PERF_ADD_IF(from_spawn, mprefetch_deferred, 1);
    xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, static_cast<s64>(pending));
    return true;
}

// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R6): main, a slow disk. Asks as a spawn does (source 1:
// ahead of the ALife jobs) and tells whether the files of the model are still to be read by the "Model prefetch"
// thread. true = queued or being read, or the pool lock was held and it is not known yet (the caller asks again next
// frame); false = in the pool, read already, its job dropped, or no prefetch thread.
bool CModelPool::PrefetchPending(LPCSTR name)
{
    if (!name || !name[0] || !s_file_prefetch)
        return false;
    if (!PrefetchFiles(name, 1))
        return true;
    string_path low_name;
    model_key(name, low_name);
    const xr_string key(low_name);
    ModelFilePrefetch& P = *s_file_prefetch;
    std::lock_guard<std::mutex> lk(P.mutex);
    if (P.stop.load(std::memory_order_relaxed))
        return false;
    if (file_prefetch_pending(P, key))
        return true;
    // 50l-weak-cpu-spawn-models.md: a creature's model - and until its base model is in the pool (the background
    // loading; ModelFilePrefetch::mutex -> the loader's mutex, the order the reader hands models over in)
    return P.bg_names.count(key) != 0 && bgload_pending(key);
}

// 50l-weak-cpu-spawn-models.md: any thread; a base or a free instance of the key (lower case, no extension + suffix)
bool CModelPool::InPool(LPCSTR key)
{
    if (!key || !key[0])
        return false;
    ModelPool_mtlock lock;
    return Pool.find(key) != Pool.end() || Instance_Find(key);
}

// 50l: the main thread, a device reset (CRender::reset_begin / reset_end) - no background model load across it
void CModelPool::BackgroundLoadPause(bool on) { bgload_pause(on); }

void CModelPool::ClearPool(BOOL b_complete)
{
    // 50l-weak-cpu-spawn-models.md: the background loading stops for the unload (the load in progress ends first,
    // outside the pool lock - it may need it); what it has loaded and the main thread has not finished is freed
    bgload_flush();
    {
        ModelPool_mtlock lock;
        for (auto& v : Pool)
            Discard(v.second, b_complete);
        Pool.clear();
        if (b_complete)
            s_bgload_bases.clear();
    }
    if (b_complete)
        file_prefetch_reset(); // 50-smart-engine-hw-profile.md R5
}

dxRender_Visual* CModelPool::CreatePE(PS::CPEDef* source)
{
    PS::CParticleEffect* V = (PS::CParticleEffect*)Instance_Create(MT_PARTICLE_EFFECT);
    V->Compile(source);
    return V;
}

dxRender_Visual* CModelPool::CreatePG(PS::CPGDef* source)
{
    PS::CParticleGroup* V = (PS::CParticleGroup*)Instance_Create(MT_PARTICLE_GROUP);
    V->Compile(source);
    return V;
}

void CModelPool::dump()
{
    Log("--- model pool --- begin:");
    u32 sz = 0;
    u32 k = 0;
    for (xr_vector<ModelDef>::iterator I = Models.begin(); I != Models.end(); ++I)
    {
        CKinematics* K = PCKinematics(I->model);
        if (K)
        {
            u32 cur = K->mem_usage(false);
            sz += cur;
            Msg("#%3d: [%3d/%5d Kb] - %s", k++, I->refs, cur / 1024, I->name.c_str());
        }
    }
    Msg("--- models: %d, mem usage: %d Kb ", k, sz / 1024);
    sz = 0;
    k = 0;
    int free_cnt = 0;
    for (REGISTRY_IT it = Registry.begin(); it != Registry.end(); ++it)
    {
        CKinematics* K = PCKinematics((dxRender_Visual*)it->first);
        VERIFY(K);
        if (K)
        {
            u32 cur = K->mem_usage(true);
            sz += cur;
            bool b_free = (Pool.find(it->second) != Pool.end());
            if (b_free)
                ++free_cnt;
            Msg("#%3d: [%s] [%5d Kb] - %s", k++, (b_free) ? "free" : "used", cur / 1024, it->second.c_str());
        }
    }
    Msg("--- instances: %d, free %d, mem usage: %d Kb ", k, free_cnt, sz / 1024);
    Log("--- model pool --- end.");
}

void CModelPool::memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system)
{
    vb_mem_video = 0;
    vb_mem_system = 0;
    ib_mem_video = 0;
    ib_mem_system = 0;

    for (auto& md : Models)
    {
        dxRender_Visual* ptr = md.model;
        Fvisual* vis_ptr = dynamic_cast<Fvisual*>(ptr);

        if (vis_ptr == nullptr)
            continue;

        ib_mem_video += vis_ptr->m_fast->p_rm_Indices->GetVideoMemoryUsage();
        ib_mem_system += vis_ptr->m_fast->p_rm_Indices->GetSystemMemoryUsage();
        vb_mem_video += vis_ptr->m_fast->p_rm_Vertices->GetVideoMemoryUsage();
        vb_mem_system += vis_ptr->m_fast->p_rm_Vertices->GetSystemMemoryUsage();
    }
}

namespace
{
// 50c-commit-growth-analysis.md (plans/optimization_mt): the estimated bytes one instance of the visual holds - what
// Instance_Duplicate allocates for it (the same for a base model and for each of its duplicates):
// - the object: sizeof of the class Instance_Create makes for its type;
// - CKinematics: the bone instances (bones x sizeof(CBoneInstance)); CKinematicsAnimated also the blend instances
//   (bones x sizeof(CBlendInstance)) and its copy of the motion slots (slots x (the slot + bones x a MotionVec
//   pointer));
// - a hierarchy: the child pointers and the children, duplicated as well; a skeleton: its LOD, duplicated too.
// Not counted: the shared data (geometry, VB / IB, bones, motions, user data), the wallmarks and additional bone
// transforms of a live instance, the allocator rounding of each block, the Registry / Pool nodes. CKinematics::mem_usage
// is no help: sizeof twice for an animated one, no children, no blend instances.
size_t modelpool_instance_bytes(dxRender_Visual* V, u32 depth)
{
    if (!V || depth > 8)
        return 0;
    size_t bytes;
    bool hierarchy = false;
    switch (V->Type)
    {
    case MT_NORMAL: bytes = sizeof(Fvisual); break;
    case MT_HIERRARHY:
        bytes = sizeof(FHierrarhyVisual);
        hierarchy = true;
        break;
    case MT_PROGRESSIVE: bytes = sizeof(FProgressive); break;
    case MT_SKELETON_ANIM:
        bytes = sizeof(CKinematicsAnimated);
        hierarchy = true;
        break;
    case MT_SKELETON_RIGID:
        bytes = sizeof(CKinematics);
        hierarchy = true;
        break;
    case MT_SKELETON_GEOMDEF_PM: bytes = sizeof(CSkeletonX_PM); break;
    case MT_SKELETON_GEOMDEF_ST: bytes = sizeof(CSkeletonX_ST); break;
    case MT_PARTICLE_EFFECT: bytes = sizeof(PS::CParticleEffect); break;
    case MT_PARTICLE_GROUP: bytes = sizeof(PS::CParticleGroup); break;
#ifndef _EDITOR
    case MT_LOD:
        bytes = sizeof(FLOD);
        hierarchy = true;
        break;
    case MT_TREE_ST: bytes = sizeof(FTreeVisual_ST); break;
    case MT_TREE_PM: bytes = sizeof(FTreeVisual_PM); break;
#endif
    default: bytes = sizeof(dxRender_Visual); break;
    }
    if (hierarchy)
    {
        const FHierrarhyVisual* H = static_cast<const FHierrarhyVisual*>(V); // Instance_Create made it by the type
        bytes += H->children.size() * sizeof(dxRender_Visual*);
        for (dxRender_Visual* child : H->children)
            bytes += modelpool_instance_bytes(child, depth + 1);
    }
    if (CKinematics* K = PCKinematics(V))
    {
        const size_t bone_count = K->LL_BoneCount();
        bytes += bone_count * sizeof(CBoneInstance);
        if (CKinematicsAnimated* A = PKinematicsAnimated(V))
        {
            bytes += bone_count * sizeof(CBlendInstance);
            bytes += A->LL_MotionsSlotCount() *
                (sizeof(shared_motions) + sizeof(BoneMotionsVec) + bone_count * sizeof(MotionVec*));
        }
        bytes += modelpool_instance_bytes(K->m_lod, depth + 1);
    }
    return bytes;
}
} // namespace

void CModelPool::memory_counts(RenderMemoryStats& stats, bool no_wait)
{
    CTimer timer;
    timer.Start();
    // Under ModelPool_Mutex. The bytes of an instance per model name, cached (InstanceBytes): the first count of a
    // model walks one instance (its base or a free one), later counts only look it up - per call a pass over Models
    // (a vector), a lookup per base with instances and per run of one name in Pool (a multimap sorted by name)
    const auto count = [&]
    {
        const auto instance_bytes = [this](const shared_str& name, dxRender_Visual* V) -> u64
        {
            const auto [it, inserted] = InstanceBytes.try_emplace(name, 0);
            if (inserted)
                it->second = static_cast<u32>(modelpool_instance_bytes(V, 0));
            return it->second;
        };
        stats.models_valid = true;
        stats.model_bases = static_cast<u32>(Models.size());
        stats.model_bases_unref = 0;
        stats.model_instance_refs = 0;
        stats.model_instance_bytes = 0;
        for (const ModelDef& md : Models)
        {
            if (0 == md.refs)
            {
                ++stats.model_bases_unref;
                continue;
            }
            // refs: the duplicates of this base still in Registry (Instance_Duplicate adds one, Discard takes it)
            stats.model_instance_refs += md.refs;
            stats.model_instance_bytes += md.refs * instance_bytes(md.name, md.model);
        }
        stats.model_pool = static_cast<u32>(Pool.size());
        stats.model_instances = static_cast<u32>(Registry.size());
        stats.model_pool_bytes = 0;
        const str_value* run_name = nullptr;
        u64 run_bytes = 0;
        for (const auto& [name, visual] : Pool)
        {
            if (name._get() != run_name)
            {
                run_name = name._get();
                run_bytes = instance_bytes(name, visual);
            }
            stats.model_pool_bytes += run_bytes;
        }

        size_t motions = 0, motions_unref = 0;
        if (g_pMotionsContainer)
        {
            // dock() of a prefetch load thread
            if (!no_wait)
                s_motions_lock.Enter();
            else if (!s_motions_lock.TryEnter())
                return;
            g_pMotionsContainer->stats(motions, motions_unref);
            s_motions_lock.Leave();
        }
        stats.motions_valid = true;
        stats.motions = static_cast<u32>(motions);
        stats.motions_unref = static_cast<u32>(motions_unref);
    };
    if (no_wait)
    {
        // The [mem] line in play: the pool lock is held over a whole cold load (tens of ms, main or the Game thread) -
        // when busy no model fields this time rather than a wait on main. Recursive: a thread holding it gets it.
        if (ModelPool_Mutex.TryEnter())
        {
            count();
            ModelPool_Mutex.Leave();
        }
    }
    else
    {
        ModelPool_mtlock lock;
        count();
    }
    stats.models_ms = timer.GetElapsed_sec() * 1000.f;
}

#ifdef _EDITOR
IC bool _IsBoxVisible(dxRender_Visual* visual, const Fmatrix& transform)
{
    Fbox bb;
    bb.xform(visual->vis.box, transform);
    return RImplementation.occ_visible(bb);
}
IC bool _IsValidShader(dxRender_Visual* visual, u32 priority, bool strictB2F)
{
    if (visual->shader)
        return (priority == visual->shader->E[0]->flags.iPriority) &&
            (strictB2F == visual->shader->E[0]->flags.bStrictB2F);
    return false;
}

void CModelPool::Render(
    dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD)
{
    // render visual
    xr_vector<dxRender_Visual *>::iterator I, E;
    switch (m_pVisual->Type)
    {
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        if (_IsBoxVisible(m_pVisual, mTransform))
        {
            CKinematics* pV = dynamic_cast<CKinematics*>(m_pVisual);
            VERIFY(pV);
            if (fis_zero(m_fLOD, EPS) && pV->m_lod)
            {
                if (_IsValidShader(pV->m_lod, priority, strictB2F))
                {
                    RCache.set_Shader(pV->m_lod->shader ? pV->m_lod->shader : EDevice.m_WireShader);
                    RCache.set_xform_world(mTransform);
                    pV->m_lod->Render(1.f);
                }
            }
            else
            {
                I = pV->children.begin();
                E = pV->children.end();
                for (; I != E; I++)
                {
                    if (_IsValidShader(*I, priority, strictB2F))
                    {
                        RCache.set_Shader((*I)->shader ? (*I)->shader : EDevice.m_WireShader);
                        RCache.set_xform_world(mTransform);
                        (*I)->Render(m_fLOD);
                    }
                }
            }
        }
    }
    break;
    case MT_HIERRARHY:
    {
        if (_IsBoxVisible(m_pVisual, mTransform))
        {
            FHierrarhyVisual* pV = dynamic_cast<FHierrarhyVisual*>(m_pVisual);
            VERIFY(pV);
            I = pV->children.begin();
            E = pV->children.end();
            for (; I != E; I++)
            {
                if (_IsValidShader(*I, priority, strictB2F))
                {
                    RCache.set_Shader((*I)->shader ? (*I)->shader : EDevice.m_WireShader);
                    RCache.set_xform_world(mTransform);
                    (*I)->Render(m_fLOD);
                }
            }
        }
    }
    break;
    case MT_PARTICLE_GROUP:
    {
        PS::CParticleGroup* pG = dynamic_cast<PS::CParticleGroup*>(m_pVisual);
        VERIFY(pG);
        //		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            RCache.set_xform_world(mTransform);
            for (PS::CParticleGroup::SItemVecIt i_it = pG->items.begin(); i_it != pG->items.end(); i_it++)
            {
                xr_vector<dxRender_Visual*> visuals;
                i_it->GetVisuals(visuals);
                for (xr_vector<dxRender_Visual*>::iterator it = visuals.begin(); it != visuals.end(); it++)
                    Render(*it, Fidentity, priority, strictB2F, m_fLOD);
            }
        }
    }
    break;
    case MT_PARTICLE_EFFECT:
    {
        //		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            if (_IsValidShader(m_pVisual, priority, strictB2F))
            {
                RCache.set_Shader(m_pVisual->shader ? m_pVisual->shader : EDevice.m_WireShader);
                RCache.set_xform_world(mTransform);
                m_pVisual->Render(m_fLOD);
            }
        }
    }
    break;
    default:
        if (_IsBoxVisible(m_pVisual, mTransform))
        {
            if (_IsValidShader(m_pVisual, priority, strictB2F))
            {
                RCache.set_Shader(m_pVisual->shader ? m_pVisual->shader : EDevice.m_WireShader);
                RCache.set_xform_world(mTransform);
                m_pVisual->Render(m_fLOD);
            }
        }
        break;
    }
}

void CModelPool::RenderSingle(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD)
{
    for (int p = 0; p < 4; p++)
    {
        Render(m_pVisual, mTransform, p, false, m_fLOD);
        Render(m_pVisual, mTransform, p, true, m_fLOD);
    }
}
void CModelPool::OnDeviceDestroy() { Destroy(); }
#endif
} // namespace xray::render::RENDER_NAMESPACE
