#include "stdafx.h"

#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Threading/Lock.hpp"
#include "xrCore/Threading/ScopeLock.hpp"
#include "xrCore/Threading/ThreadUtil.h"
#include "xrEngine/device.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
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
#if XR_PERF_METRICS
        if (!ModelPool_Mutex.TryEnter())
        {
            PERF_INC(modelpool_lock_contend);
            const bool metrics_active = xray::perf::g_perf_metrics_active;
            const u64 t0 = metrics_active ? xray::perf::Ticks() : 0;
            ModelPool_Mutex.Enter();
            if (metrics_active)
                xray::perf::AddTimeSlow(xray::perf::Metric::modelpool_lock_wait, xray::perf::Ticks() - t0);
        }
#else
        ModelPool_Mutex.Enter();
#endif
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
        ++g_modelpool_cold_depth;
        ++xray::perf::TlColdModelLoads(); // spawn_cold_models (CLevel::ProcessGameEvents) counts these
    }
    ~ModelColdLoadScope() { --g_modelpool_cold_depth; }
    ModelColdLoadScope(const ModelColdLoadScope&) = delete;
    ModelColdLoadScope& operator=(const ModelColdLoadScope&) = delete;
};

// Reading the files of a model into the OS file cache: model_PrefetchFiles (the "Model prefetch" thread) and the
// start-up warm threads of Prefetch. Plain C++ file I/O plus the engine's IReader over a private
// buffer: no FS index, no pool, no render resources, so it is safe on any thread and with any render. The engine opens
// the same files later through FS (a file from 16 KB is mapped and read through page faults): their pages are in memory
// by then.
constexpr u64 kWarmMaxModelBytes = 128ull * 1024 * 1024; // all the files of one model
constexpr size_t kWarmMaxOgfBytes = 64 * 1024 * 1024;
constexpr size_t kWarmChunk = 256 * 1024;
constexpr size_t kStartupWarmLead = 256; // start-up warm threads stay at most this many list items ahead of main

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

// The whole file into `out` (at most kWarmMaxOgfBytes). false = no such file.
bool warm_read_whole(const xr_string& path, xr_vector<u8>& out)
{
    std::ifstream f(warm_os_path(path), std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamoff size = f.tellg();
    out.clear();
    if (size <= 0)
        return true;
    out.resize(std::min(static_cast<size_t>(size), kWarmMaxOgfBytes));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(std::max<std::streamsize>(f.gcount(), 0)));
    return true;
}

// Reads the file (at most `limit` bytes) and throws the bytes away. -1 = no such file.
s64 warm_read_discard(const xr_string& path, xr_vector<u8>& buf, u64 limit, const std::atomic_bool& stop)
{
    std::ifstream f(warm_os_path(path), std::ios::binary);
    if (!f)
        return -1;
    u64 total = 0;
    while (total < limit && !stop.load(std::memory_order_relaxed))
    {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0)
            break;
        total += static_cast<u64>(n);
    }
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
bool warm_model_files(const xr_string& name, const WarmRoots& roots, xr_vector<u8>& ogf, xr_vector<u8>& buf,
    WarmStats& st, const std::atomic_bool& stop, WarmSeen* seen = nullptr)
{
    const auto read_model = [&](const xr_string& rel) {
        if (!roots.level.empty() && warm_read_whole(roots.level + rel, ogf))
            return true;
        return !roots.meshes.empty() && warm_read_whole(roots.meshes + rel, ogf);
    };
    const auto discard = [&](const xr_string& path) {
        if (stop.load(std::memory_order_relaxed) || st.bytes >= kWarmMaxModelBytes)
            return true; // nothing more for this model: not counted as missing
        if (seen && !seen->claim(path))
            return true; // read (or found missing) for another model already
        const s64 n = warm_read_discard(path, buf, kWarmMaxModelBytes - st.bytes, stop);
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
    if (!refs.lod.empty() && !stop.load(std::memory_order_relaxed))
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
    if (!roots.textures.empty())
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
struct ModelFilePrefetch
{
    struct Job
    {
        xr_string name;
        xr_string level; // $level$ when it was queued
    };
    std::mutex mutex; // queue, queued, warmed, roots, thread
    std::condition_variable cv;
    std::deque<Job> queue;
    std::unordered_set<xr_string> queued;
    std::unordered_set<xr_string> warmed; // the thread has gone through these
    WarmRoots roots; // meshes / textures, taken at the first job
    std::thread thread;
    std::atomic_bool stop{};
    std::atomic_bool used{}; // anything queued yet: only then a cold load looks `warmed` up (metrics)
};
ModelFilePrefetch* s_file_prefetch = nullptr;

void file_prefetch_thread(ModelFilePrefetch* P)
{
    Threading::SetCurrentThreadPriorityLevel(Threading::priority_level::below_normal);
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
            pending = P->queue.size();
            roots = P->roots;
        }
        roots.level = job.level;
        xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, static_cast<s64>(pending));
        {
            ZoneScopedN("model_prefetch_files");
            ZoneText(job.name.c_str(), job.name.size());
            PERF_SCOPE(mprefetch_time);
            WarmStats st;
            warm_model_files(job.name, roots, ogf, buf, st, P->stop);
            PERF_ADD(mprefetch_files, st.files);
            PERF_ADD(mprefetch_kb, (st.bytes + 1023) / 1024);
            PERF_ADD(mprefetch_missing, st.missing);
        }
        std::lock_guard<std::mutex> lk(P->mutex);
        P->warmed.insert(job.name);
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
    }
    P.cv.notify_all();
    if (P.thread.joinable())
        P.thread.join(); // waits for one file read at most: the thread checks `stop` between chunks
    xr_delete(s_file_prefetch);
    xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, 0);
}

// The start-up warm threads of Prefetch (one per hardware thread but main's): they take the items of the prefetch list in order, up
// to kStartupWarmLead items ahead of main, and skip the ones main has already passed
struct StartupWarm
{
    xr_vector<xr_string> names;
    WarmRoots roots;
    std::atomic<size_t> next{0};
    std::atomic<size_t> main_pos{0};
    std::atomic_bool stop{};
    std::atomic<u32> files{0};
    std::atomic<u32> missing{0};
    std::atomic<u32> behind{0}; // items main had passed before a warm thread got to them
    std::atomic<u64> bytes{0};
    WarmSeen seen; // the omf / ltx / dds taken by any of the threads
};

void startup_warm_thread(StartupWarm* W)
{
    xr_vector<u8> ogf;
    xr_vector<u8> buf(kWarmChunk);
    for (;;)
    {
        const size_t i = W->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= W->names.size() || W->stop.load(std::memory_order_relaxed))
            return;
        if (i < W->main_pos.load(std::memory_order_relaxed))
        {
            W->behind.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        while (!W->stop.load(std::memory_order_relaxed) &&
            i > W->main_pos.load(std::memory_order_relaxed) + kStartupWarmLead)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (W->stop.load(std::memory_order_relaxed))
            return;
        ZoneScopedN("model_warm");
        ZoneText(W->names[i].c_str(), W->names[i].size());
        WarmStats st;
        warm_model_files(W->names[i], W->roots, ogf, buf, st, W->stop, &W->seen);
        W->files.fetch_add(st.files, std::memory_order_relaxed);
        W->missing.fetch_add(st.missing, std::memory_order_relaxed);
        W->bytes.fetch_add(st.bytes, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 40-parallel-prefetch.md (plans/optimization_mt): the parallel model prefetch (D3D11)

// ModelMotionsScope: the motions part of CKinematicsAnimated::Load, one at a time. Recursive (Lock): the main thread
// holds it around Create / Delete of the list and may load a model itself under it. Order: ModelPool_Mutex -> this.
Lock s_motions_lock;

constexpr u32 kPrefetchMaxLoadThreads = 8; // the serialized parts (shader creation, motions) limit the gain beyond it

u64 prefetch_now_us()
{
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

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
    std::atomic<size_t>* warm_pos{}; // StartupWarm::main_pos: the warm threads read ahead of the loads taken

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
        if (P->warm_pos)
            P->warm_pos->store(L.item, std::memory_order_relaxed);
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
    PrefetchTop* top = tl_prefetch_top;
    if (!top || !s_parallel_prefetch || !data)
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
    if (top->readers.size() > 1 || part.copied)
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
}

CModelPool::~CModelPool()
{
    file_prefetch_shutdown(); // 36-spawn-model-load.md
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
            if (xray::perf::g_perf_metrics_active && s_file_prefetch &&
                s_file_prefetch->used.load(std::memory_order_relaxed))
            {
                // 36-spawn-model-load.md: had the "Model prefetch" thread read the files of this model?
                bool warmed;
                {
                    std::lock_guard<std::mutex> lk(s_file_prefetch->mutex);
                    warmed = s_file_prefetch->warmed.count(xr_string(low_name)) != 0;
                }
                PERF_ADD_IF(warmed, mprefetch_cold_warm, 1);
                PERF_ADD_IF(!warmed, mprefetch_cold_miss, 1);
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
    const CInifile::Sect& sect = pSettings->r_section(section);

    // The pool keys of the list
    xr_vector<ModelPrefetchItem> items;
    items.reserve(sect.Data.size());
    for (const CInifile::Item& item : sect.Data)
    {
        string_path key;
        model_key(item.first.c_str(), key);
        items.push_back({ key, "" });
    }
    PrefetchItems(items, "models", true);
    Logging(TRUE);
}

// 40-parallel-prefetch.md (plans/optimization_mt): IGame_ObjectPool::prefetch - the visuals the Load of the prefetch
// objects creates, before it, on the load threads. Where the prefetch is sequential (GL) nothing:
// Load creates them on main, as before.
void CModelPool::PrefetchList(const xr_vector<std::pair<shared_str, shared_str>>& visuals)
{
    if (visuals.empty())
        return;
    xr_vector<ModelPrefetchItem> items;
    items.reserve(visuals.size());
    for (const auto& [name, suffix] : visuals)
    {
        if (!name.size())
            continue;
        string_path key;
        model_key(name.c_str(), key);
        items.push_back({ key, suffix.size() ? suffix.c_str() : "" });
    }
    PrefetchItems(items, "objects", false);
}

// Loads the models of the list into the pool (for each item: Create(name, suffix) + Delete, an instance stays in Pool).
// sequential_too: without load threads (GL, < 3 hardware threads) load them on main, else nothing.
void CModelPool::PrefetchItems(const xr_vector<ModelPrefetchItem>& items, pcstr label, bool sequential_too)
{
    ZoneScoped;
    const int hw_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

    // 40-parallel-prefetch.md (plans/optimization_mt): threads that load the models of the list (D3D11 creates its
    // resources free-threaded; on GL the objects of the context are made on its thread only, the load stays
    // sequential). Always on since 2026-10-18 (r__prefetch_mt removed, see plan 41).
#if defined(USE_DX11)
    constexpr bool parallel_load_supported = true;
#else
    constexpr bool parallel_load_supported = false;
#endif
    u32 load_n = 0;
    if (parallel_load_supported)
        load_n = static_cast<u32>(std::min<int>(hw_threads - 1, static_cast<int>(kPrefetchMaxLoadThreads)));
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

    // 36-spawn-model-load.md: threads that read the files of the list into the OS cache ahead of the load below, one
    // per hardware thread but main's and the load threads'. They touch no engine state (see warm_model_files).
    StartupWarm warm;
    xr_vector<std::thread> warm_threads;
    const int warm_n = std::max(1, hw_threads - 1 - static_cast<int>(load_n));
    if (warm_n > 0)
    {
        warm.roots = warm_roots_now();
        warm.names.reserve(items.size());
        for (const ModelPrefetchItem& item : items)
            warm.names.push_back(item.name);
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
        P.warm_pos = &warm.main_pos;
        P.loaded.assign(P.loads.size(), 0);
        P.done.assign(P.loads.size(), 0);
        s_parallel_prefetch = &P;
        for (u32 t = 0; t < load_n; ++t)
            load_threads.emplace_back(Threading::RunThread("Model load", &prefetch_load_thread, &P));
    }

    u64 main_wait_us = 0;
    for (size_t pos = 0; pos < items.size(); ++pos)
    {
        if (!load_n)
            warm.main_pos.store(pos, std::memory_order_relaxed); // with the load threads they move it

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
    }
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
void CModelPool::PrefetchFiles(LPCSTR name, u32 source)
{
    if (!name || !name[0] || !s_file_prefetch)
        return;
    string_path low_name;
    model_key(name, low_name);
    ModelFilePrefetch& P = *s_file_prefetch;
    size_t pending;
    {
        std::lock_guard<std::mutex> lk(P.mutex);
        if (P.stop.load(std::memory_order_relaxed) || !P.queued.insert(xr_string(low_name)).second)
            return;
        if (P.roots.meshes.empty())
            P.roots = warm_roots_now();
        ModelFilePrefetch::Job job;
        job.name = low_name;
        job.level = warm_root("$level$");
        P.queue.push_back(std::move(job));
        pending = P.queue.size();
        if (!P.thread.joinable())
            P.thread = Threading::RunThread("Model prefetch", &file_prefetch_thread, &P);
        P.used.store(true, std::memory_order_relaxed);
    }
    P.cv.notify_one();
    PERF_INC(mprefetch_queued);
    PERF_ADD_IF(source == 0, mprefetch_alife, 1);
    PERF_ADD_IF(source == 1, mprefetch_deferred, 1);
    xray::perf::SetGauge(xray::perf::Metric::mprefetch_queue, static_cast<s64>(pending));
}

void CModelPool::ClearPool(BOOL b_complete)
{
    ModelPool_mtlock lock;
    for (auto& v : Pool)
        Discard(v.second, b_complete);
    Pool.clear();
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
