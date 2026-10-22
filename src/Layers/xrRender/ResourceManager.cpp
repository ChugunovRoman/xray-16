// TextureManager.cpp: implementation of the CResourceManager class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop
#include "xrCore/Profiling/PerfMetrics.hpp"

#include "xrCore/Threading/ParallelForEach.hpp"
#include "xrCore/Threading/ScopeLock.hpp"
#include "xrCore/Threading/ThreadUtil.h"
#include "xrCore/BackgroundIo.h"
#include "xrCore/Platform/HwProfile.h"
#include "xrCommon/xr_deque.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "ResourceManager.h"
#include "tss.h"
#include "Blender.h"
#include "Blender_Recorder.h"

namespace xray::render::RENDER_NAMESPACE
{
//	Already defined in Texture.cpp
void fix_texture_name(pstr fn);
#if defined(USE_DX11)
// 40-parallel-prefetch.md (plans/optimization_mt): dx11Texture.cpp, textures created straight from the file
extern std::atomic<u32> g_texture_direct_loads;
#endif

namespace
{
// 50a-stage1-prefetch.md (plans/optimization_mt, plan 50 R3): DeferredUpload on a slow disk (ModelPoolDiskSlow: an HDD,
// USB or network disk by the hw profile or by the warm-up measurement). One "Texture warm" thread reads the dds into
// the OS cache in the order the loads take the textures (by name - mostly the order the files lie on the disk), at
// most kTexWarmLead textures ahead of them, and a texture is loaded once the reader has passed it: one reader going
// through the disk instead of a seek per task. The textures are still created in parallel (D3D11).
// The textures created already (CTexture::load_reads_no_file: loaded by the prefetch stage, left from the previous
// level) are not read and their loads do not wait: the reader goes through the textures to read only, and the lead
// counts only those (kTexWarmLead textures to read ahead of the loads).
constexpr size_t kTexWarmLead = 64;

struct TextureWarmReader
{
    const xr_vector<CTexture*>* textures{};
    const std::atomic<size_t>* taken{}; // how many textures the loads have taken
    std::unique_ptr<std::atomic<u8>[]> passed; // per texture: the reader is through with it (or it is not to be read)
    xr_vector<u32> to_read; // the textures to read, in load order (indices in textures)
    xr_vector<u32> to_read_before; // per index in textures (and the size): how many textures to read come before it
    u32 loaded{}; // the textures created already: passed from the start
    std::atomic_bool stop{};
    std::atomic_bool finished{};
    std::atomic<u32> files{0};
    std::atomic<u64> waited_us{0};

    static u64 now_us()
    {
        using namespace std::chrono;
        return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // On the main thread, before the reader and the loads start (the async loading is shut down and blocked: no
    // texture of the list changes its state meanwhile)
    void prepare(const xr_vector<CTexture*>& list, const std::atomic<size_t>& counter)
    {
        textures = &list;
        taken = &counter;
        const size_t n = list.size();
        passed.reset(new std::atomic<u8>[n]);
        to_read.reserve(n);
        to_read_before.resize(n + 1);
        for (size_t i = 0; i < n; ++i)
        {
            to_read_before[i] = static_cast<u32>(to_read.size());
            const bool created = list[i]->load_reads_no_file();
            passed[i].store(created ? 1 : 0, std::memory_order_relaxed);
            if (created)
                ++loaded;
            else
                to_read.push_back(static_cast<u32>(i));
        }
        to_read_before[n] = static_cast<u32>(to_read.size());
    }

    // A load: texture i after the reader
    void wait(size_t i)
    {
        if (passed[i].load(std::memory_order_acquire))
            return;
        const u64 t0 = now_us();
        while (!passed[i].load(std::memory_order_acquire) && !finished.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waited_us.fetch_add(now_us() - t0, std::memory_order_relaxed);
    }
};

// The file CRender::texture_load opens for the texture: $level$, $game_saves$, $game_textures$ + the name + .dds
bool texture_warm_name_path(pcstr name, string_path& fn)
{
    if (!name || !name[0] || name[0] == '$') // $null, $user$ render targets
        return false;
    string_path fname;
    xr_strcpy(fname, name);
    fix_texture_name(fname);
    for (cpcstr folder : { "$level$", "$game_saves$", "$game_textures$" })
    {
        if (FS.path_exist(folder) && FS.exist(fn, folder, fname, ".dds"))
            return true;
    }
    return false;
}

bool texture_warm_file_path(const CTexture* T, string_path& fn) { return texture_warm_name_path(T->cName.c_str(), fn); }

void texture_warm_thread(TextureWarmReader* R)
{
    const size_t n = R->textures->size();
    const size_t m = R->to_read.size();
    for (size_t k = 0; k < m && !R->stop.load(std::memory_order_relaxed); ++k)
    {
        // The lead in textures to read: the loads take the created ones without waiting (the counter of the tasks
        // runs past the end by up to a task each)
        const auto taken_to_read = [R, n] {
            return static_cast<size_t>(R->to_read_before[std::min(R->taken->load(std::memory_order_relaxed), n)]);
        };
        while (!R->stop.load(std::memory_order_relaxed) && k > taken_to_read() + kTexWarmLead)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const size_t i = R->to_read[k];
        string_path fn;
        if (texture_warm_file_path((*R->textures)[i], fn) && ModelPoolWarmFile(fn))
            R->files.fetch_add(1, std::memory_order_relaxed);
        R->passed[i].store(1, std::memory_order_release);
    }
    R->finished.store(true, std::memory_order_release);
}

// 50r-slow-disk-async-textures.md (plans/optimization_mt): the async loading of textures on a slow disk (an HDD, USB or
// network disk by the hw profile or by the warm-up measurement - ModelPoolDiskSlow). A texture created in the game, or
// first bound while the load is deferred (the menu), is not read on the calling thread: one "Texture async" thread
// reads its dds into the OS cache through the background I/O slot (xrCore/BackgroundIo.h: after the model jobs, before the
// sound and the menu warm-up), the bound ones first, then the created ones, then the warm-up names of
// IRender::texture_prefetch_files (the file only). D3D11 with DriverConcurrentCreates: the thread creates the texture
// too and the main thread only puts it in place; otherwise (GL, serialized creates) the main thread creates it from the
// OS cache, 2 ms of it per frame. Meanwhile the texture binds no view (zeros: transparent black).
// The queues hold raw pointers: a texture leaving the async loading (Unload - its destructor, a level change) drops its
// entries under the lock (CResourceManager::_AsyncTextureDetach) and waits while the thread or a finisher works on it
// (asReading / asInstalling), so the loading changes no texture's lifetime. Every state change out of asQueued /
// asWarm / asCreated is a compare-exchange; asReading and asInstalling are left by their owner only.
constexpr u64 kTexAsyncMainBudgetUs = 2000; // the main thread's creations of read textures per frame (one at least)
constexpr u64 kTexAsyncUrgentYieldMs = 250; // a bound texture waits this long at most for the model jobs to end
constexpr size_t kTexAsyncWarmMax = 8192; // warm-up names in the queue at most

u64 tex_async_now_ms()
{
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

u64 tex_async_now_us()
{
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void tex_async_max(std::atomic<u32>& to, u32 v)
{
    u32 cur = to.load(std::memory_order_relaxed);
    while (v > cur && !to.compare_exchange_weak(cur, v, std::memory_order_relaxed))
    {
    }
}

// Always on (Release without metrics too): the summary line at the level load and the device destroy
struct TexAsyncStats
{
    std::atomic<u32> queued{0}; // textures given to the thread
    std::atomic<u32> urgent{0}; // of them bound with the placeholder before they were in place
    std::atomic<u32> on_thread{0}; // put in place, created by the thread (D3D11 concurrent creates)
    std::atomic<u32> on_main{0}; // put in place, created by the main thread from the OS cache
    std::atomic<u32> sync{0}; // finished by a caller that needed them now, file not read yet (read on its thread)
    std::atomic<u64> sync_us{0};
    std::atomic<u32> early{0}; // finished by a caller after the thread had read the file (from the OS cache)
    std::atomic<u64> wait_us{0}; // callers waiting for the thread in the middle of a texture
    std::atomic<u32> wait_max_us{0};
    std::atomic<u64> latency_ms{0}; // queued -> in place, summed over the textures put in place
    std::atomic<u32> latency_max_ms{0};
    std::atomic<u64> bytes{0}; // read by the thread (textures and warm-up)
    std::atomic<u32> warm_files{0};
    std::atomic<u32> warm_dropped{0}; // warm-up names over kTexAsyncWarmMax
    std::atomic<u32> frames{0}; // frames begun with textures pending (the placeholder may be on the screen)
};

struct TexAsyncLoader
{
    std::mutex mutex; // the queues, reading, blocked, the thread's start, CTexture::m_async_entries
    std::condition_variable cv;
    xr_deque<CTexture*> urgent; // bound with the placeholder (null = detached)
    xr_deque<CTexture*> normal; // created
    xr_deque<CTexture*> done; // read (asWarm) or created (asCreated): for the main thread
    xr_deque<shared_str> warm; // warm-up names
    std::thread thread;
    std::atomic_bool stop{false};
    bool reading{}; // the thread has an entry (the texture jobs of the slot)
    bool blocked{}; // DeferredUpload: nothing is queued
    bool create_on_thread{};
    std::atomic_bool urgent_any{false}; // the urgent queue has entries (its waits for the slot look at it)
    std::atomic<u32> pending{0}; // textures queued and not in place
    std::atomic_bool used{false}; // anything was ever queued: the pump has work
    std::atomic_bool logged{false};
    TexAsyncStats stats;
};

TexAsyncLoader& tex_async()
{
    // Never destroyed: the thread is joined by _AsyncTexturesShutdown (the device destroy), not at the process exit
    static TexAsyncLoader* loader = xr_new<TexAsyncLoader>();
    return *loader;
}

// Under the lock: the jobs of the slot (BackgroundIo.h), in order
void tex_async_publish(TexAsyncLoader& L)
{
    const size_t jobs = L.urgent.size() + L.normal.size() + L.warm.size() + (L.reading ? 1 : 0);
    xray::io::BackgroundIoSetTextureJobs(static_cast<u32>(jobs));
}

// Under the lock: the next texture the thread can take (asQueued -> asReading); stale and detached entries go
CTexture* tex_async_take(xr_deque<CTexture*>& q)
{
    while (!q.empty())
    {
        CTexture* T = q.front();
        q.pop_front();
        if (!T)
            continue;
        T->m_async_entries.fetch_sub(1, std::memory_order_relaxed);
        u8 expected = CTexture::asQueued;
        if (T->m_async_state.compare_exchange_strong(expected, CTexture::asReading, std::memory_order_acq_rel))
            return T;
    }
    return nullptr;
}

// Under the lock: null the entries of a texture leaving the async loading
void tex_async_forget(xr_deque<CTexture*>& q, const CTexture* T)
{
    for (CTexture*& e : q)
    {
        if (e == T)
            e = nullptr;
    }
}

enum class TexAsyncPlaced : u8
{
    Thread, // created by the thread, put in place by the main thread (or by a caller)
    Main, // created by the main thread from the OS cache
    Early, // created by a caller from the OS cache (the thread had read it)
    Sync, // the file read and created by a caller (the thread had not got to it)
};

// The claimer of asInstalling, after the texture is in place: asReady and the counters
void tex_async_placed(CTexture& T, TexAsyncPlaced how)
{
    TexAsyncLoader& L = tex_async();
    const u32 latency = static_cast<u32>(tex_async_now_ms()) - T.m_async_t0; // u32 arithmetic: wraps right
    // The last touch of T: from asReady on its detach no longer waits (the pump and the shutdown hold no reference)
    T.m_async_state.store(CTexture::asReady, std::memory_order_release);
    L.pending.fetch_sub(1, std::memory_order_relaxed);
    L.stats.latency_ms.fetch_add(latency, std::memory_order_relaxed);
    tex_async_max(L.stats.latency_max_ms, latency);
    PERF_ADD(tex_async_latency_ms, latency);
    switch (how)
    {
    case TexAsyncPlaced::Thread: L.stats.on_thread.fetch_add(1, std::memory_order_relaxed); break;
    case TexAsyncPlaced::Main: L.stats.on_main.fetch_add(1, std::memory_order_relaxed); break;
    case TexAsyncPlaced::Early: L.stats.early.fetch_add(1, std::memory_order_relaxed); break;
    case TexAsyncPlaced::Sync: L.stats.sync.fetch_add(1, std::memory_order_relaxed); break;
    }
}

// The texture can go to the async loading: a dds of its own (not a render target, a video or a sequence - load_now
// takes those before the dds; not a missing file - texture_load falls back to the default texture at once)
bool tex_async_eligible(const CTexture& T, string_path& fn)
{
    pcstr name = T.cName.c_str();
    if (!name || !name[0] || name[0] == '$')
        return false;
    for (cpcstr ext : { ".ogm", ".avi", ".seq" })
    {
        if (FS.exist(fn, "$game_textures$", name, ext))
            return false;
    }
    return texture_warm_name_path(name, fn);
}

void tex_async_thread(TexAsyncLoader* L)
{
    // Below normal CPU priority (main first), but the normal I/O priority: unlike the readers ahead (the model and the
    // sound prefetch, background mode) this thread mostly reads what is on the screen now
    Threading::SetCurrentThreadPriorityLevel(Threading::priority_level::below_normal);
    const auto stopping = [L] { return L->stop.load(std::memory_order_relaxed); };
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lk(L->mutex);
            L->cv.wait(lk, [L] {
                return L->stop.load(std::memory_order_relaxed) || !L->urgent.empty() || !L->normal.empty() ||
                    !L->warm.empty();
            });
            if (stopping())
                break;
        }
        // The loading screen: the level load reads the disk itself (the prefetch, the textures of the level) - the
        // thread waits (and the other readers do not wait for it); DeferredUpload stops it and loads the pending
        // textures with all the others
        if (load_screen_renderer.IsActive())
        {
            {
                std::lock_guard<std::mutex> lk(L->mutex);
                xray::io::BackgroundIoSetTextureJobs(0);
            }
            while (load_screen_renderer.IsActive() && !stopping())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (stopping())
                break;
            std::lock_guard<std::mutex> lk(L->mutex);
            tex_async_publish(*L);
        }
        // The slot (a slow disk) before an entry is taken: while the thread waits for it the texture stays asQueued,
        // and a caller that needs it reads it itself instead of waiting for the slot. A bound texture waits for the
        // model jobs (the spawns, ALife) at most kTexAsyncUrgentYieldMs, then takes the slot after the current read -
        // for a bound texture only.
        xray::io::BackgroundIoScope io;
        const u64 wait0 = tex_async_now_ms();
        bool entered = io.enter(xray::io::BackgroundIoClient::Texture, [&] {
            return stopping() || (L->urgent_any.load(std::memory_order_relaxed) &&
                                     tex_async_now_ms() - wait0 >= kTexAsyncUrgentYieldMs);
        });
        const bool urgent_only = !entered;
        if (!entered && !stopping())
            entered = io.enter(xray::io::BackgroundIoClient::Model, stopping);
        if (!entered)
            break;

        CTexture* T = nullptr;
        shared_str warm_name;
        {
            std::lock_guard<std::mutex> lk(L->mutex);
            T = tex_async_take(L->urgent);
            L->urgent_any.store(!L->urgent.empty(), std::memory_order_relaxed);
            if (!T && !urgent_only)
                T = tex_async_take(L->normal);
            if (!T && !urgent_only && !L->warm.empty())
            {
                warm_name = L->warm.front();
                L->warm.pop_front();
            }
            L->reading = T || warm_name.size();
            tex_async_publish(*L);
        }
        if (!T && !warm_name.size())
            continue;

        // asReading: the texture is alive until the thread leaves this state (its detach waits)
        string_path fn;
        const u64 t_read = xray::perf::Ticks();
        const bool read = T ? texture_warm_file_path(T, fn) && ModelPoolWarmFile(fn) :
                              texture_warm_name_path(warm_name.c_str(), fn) && ModelPoolWarmFile(fn);
        io.leave();
#if XR_PERF_METRICS
        if (xray::perf::g_perf_metrics_active)
            xray::perf::AddTimeSlow(xray::perf::Metric::tex_async_read, xray::perf::Ticks() - t_read);
#else
        (void)t_read;
#endif
        if (read)
        {
            const int len = FS.file_length(fn);
            if (len > 0)
            {
                L->stats.bytes.fetch_add(static_cast<u64>(len), std::memory_order_relaxed);
                PERF_ADD(tex_async_kb, static_cast<u64>(len) / 1024);
            }
            if (!T)
                L->stats.warm_files.fetch_add(1, std::memory_order_relaxed);
        }

        u8 next = CTexture::asWarm;
        if (T && L->create_on_thread)
        {
            PERF_SCOPE(tex_async_create);
            if (T->async_create())
                next = CTexture::asCreated;
        }
        {
            std::lock_guard<std::mutex> lk(L->mutex);
            if (T)
            {
                T->m_async_state.store(next, std::memory_order_release);
                L->done.push_back(T);
                T->m_async_entries.fetch_add(1, std::memory_order_relaxed);
            }
            L->reading = false;
            tex_async_publish(*L);
        }
    }
    std::lock_guard<std::mutex> lk(L->mutex);
    L->reading = false;
    tex_async_publish(*L);
}

// Under the lock: the thread, started at the first entry (and again after a shutdown)
void tex_async_start(TexAsyncLoader& L)
{
    // stop: _AsyncTexturesShutdown is between the join and the drain - no new thread there (it would outlive the
    // shutdown, e.g. the device destroy); the drain takes back what is queued meanwhile
    if (L.thread.joinable() || L.stop.load(std::memory_order_relaxed))
        return;
#if defined(USE_DX11)
    const xray::hw::Gpu& gpu = xray::hw::GetProfile().gpu;
    L.create_on_thread = gpu.api == xray::hw::GpuApi::D3D11 && gpu.driver_cc;
#else
    L.create_on_thread = false;
#endif
    L.thread = Threading::RunThread("Texture async", &tex_async_thread, &L);
    if (!L.logged.exchange(true))
    {
        Msg("* [hw] textures: slow disk -> async loading with placeholder (the \"Texture async\" thread reads the dds "
            "after the model jobs, the bound ones first; %s)",
            L.create_on_thread ? "created on that thread: D3D11 concurrent creates" :
                                 "created on the main thread from the OS cache, 2 ms per frame");
    }
}
} // namespace

// 50r-slow-disk-async-textures.md: see the loader above
bool CTexture::async_bind(CBackend& cmd_list, u32 stage)
{
    const u8 s = m_async_state.load(std::memory_order_acquire);
    if (s == asReady)
        return false; // in place: apply_load goes on as for a loaded texture
    if (s == asNone)
    {
        // Not loaded yet (a deferred load - the menu; or created on a thread that does not queue): to the async
        // loading as a bound one, unless it must be loaded now
        if (flags.bLoaded || !RImplementation.Resources->_AsyncTextureQueue(this, true))
            return false;
    }
    else
        RImplementation.Resources->_AsyncTexturePromote(this);
    async_bind_placeholder(cmd_list, stage);
    return true;
}

bool CTexture::async_finish_now()
{
    u64 wait_t0 = 0; // Ticks
    bool finished = false;
    for (;;)
    {
        u8 s = m_async_state.load(std::memory_order_acquire);
        if (s == asNone || s == asReady)
        {
            finished = s == asReady;
            break;
        }
        if (s == asQueued || s == asWarm || s == asCreated)
        {
            u8 expected = s;
            if (!m_async_state.compare_exchange_strong(expected, asInstalling, std::memory_order_acq_rel))
                continue;
            // The caller needs it now: a synchronous texture creation on its thread (the frame report's "tex")
            GW_FRAME_EVENT_IF(xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker, TexSync);
            if (s == asCreated)
            {
                async_install();
                tex_async_placed(*this, TexAsyncPlaced::Thread);
            }
            else
            {
                const u64 t0 = tex_async_now_us();
                {
                    PERF_SCOPE(tex_async_sync);
                    load_now();
                }
                TexAsyncLoader& L = tex_async();
                L.stats.sync_us.fetch_add(tex_async_now_us() - t0, std::memory_order_relaxed);
                tex_async_placed(*this, s == asQueued ? TexAsyncPlaced::Sync : TexAsyncPlaced::Early);
            }
            finished = true;
            break;
        }
        // asReading / asInstalling: the thread or another caller is on it - one file at most
        if (!wait_t0)
            wait_t0 = xray::perf::Ticks();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (wait_t0)
    {
        const u64 ticks = xray::perf::Ticks() - wait_t0;
        const u64 waited_us = static_cast<u64>(xray::perf::TicksToMilliseconds(ticks) * 1000.0);
        TexAsyncLoader& L = tex_async();
        L.stats.wait_us.fetch_add(waited_us, std::memory_order_relaxed);
        tex_async_max(L.stats.wait_max_us, static_cast<u32>(std::min<u64>(waited_us, type_max<u32>)));
        // 50n: main / Game waited for a background reader (the frame report's "iow")
        if (xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker)
            xray::perf::FrameEventAdd(xray::perf::frame_event::IoWait, ticks);
#if XR_PERF_METRICS
        if (xray::perf::g_perf_metrics_active)
            xray::perf::AddTimeSlow(xray::perf::Metric::tex_async_wait, ticks);
#endif
    }
    return finished;
}

void CTexture::async_detach()
{
    if (m_async_state.load(std::memory_order_acquire) == asNone && !m_async_entries.load(std::memory_order_relaxed))
        return;
    if (RImplementation.Resources)
        RImplementation.Resources->_AsyncTextureDetach(this);
}

bool CResourceManager::_AsyncTextureQueue(CTexture* T, bool bound)
{
    if (!T || !Device.b_is_Ready || !ModelPoolDiskSlow())
        return false;
    // Must be loaded now: the loading screen and the precache (the load waits anyway), a background loader thread
    // creating it (the "Model bg load" thread of a weak CPU, 50l - it is there to absorb the read)
    if (load_screen_renderer.IsActive() || Device.dwPrecacheFrame != 0)
        return false;
    if (!bound && xray::perf::CurrentThreadKind() == xray::perf::ThreadKind::Worker)
        return false;
    string_path fn;
    if (!tex_async_eligible(*T, fn))
        return false;

    TexAsyncLoader& L = tex_async();
    {
        std::lock_guard<std::mutex> lk(L.mutex);
        if (L.blocked)
            return false;
        u8 expected = CTexture::asNone;
        if (!T->m_async_state.compare_exchange_strong(expected, CTexture::asQueued, std::memory_order_acq_rel))
        {
            // Another thread was first: the placeholder if it is still pending
            if (expected == CTexture::asReady)
                return false;
            if (bound && expected == CTexture::asQueued && !T->m_async_urgent.exchange(1))
            {
                L.urgent.push_back(T);
                L.urgent_any.store(true, std::memory_order_relaxed);
                T->m_async_entries.fetch_add(1, std::memory_order_relaxed);
                L.stats.urgent.fetch_add(1, std::memory_order_relaxed);
                tex_async_publish(L);
            }
            return true;
        }
        T->m_async_t0 = static_cast<u32>(tex_async_now_ms());
        T->m_async_urgent.store(bound ? 1 : 0, std::memory_order_relaxed);
        (bound ? L.urgent : L.normal).push_back(T);
        if (bound)
            L.urgent_any.store(true, std::memory_order_relaxed);
        T->m_async_entries.fetch_add(1, std::memory_order_relaxed);
        L.pending.fetch_add(1, std::memory_order_relaxed);
        L.stats.queued.fetch_add(1, std::memory_order_relaxed);
        if (bound)
            L.stats.urgent.fetch_add(1, std::memory_order_relaxed);
        L.used.store(true, std::memory_order_relaxed);
        tex_async_start(L);
        tex_async_publish(L);
    }
    L.cv.notify_one();
    PERF_INC(tex_async_queued);
    if (bound)
        PERF_INC(tex_async_urgent);
    return true;
}

void CResourceManager::_AsyncTexturePromote(CTexture* T)
{
    if (T->m_async_urgent.load(std::memory_order_relaxed))
        return;
    TexAsyncLoader& L = tex_async();
    {
        std::lock_guard<std::mutex> lk(L.mutex);
        // Only while the thread has not taken it: later the urgent queue would not make it sooner
        if (T->m_async_state.load(std::memory_order_acquire) != CTexture::asQueued || T->m_async_urgent.exchange(1))
            return;
        L.urgent.push_back(T);
        L.urgent_any.store(true, std::memory_order_relaxed);
        T->m_async_entries.fetch_add(1, std::memory_order_relaxed);
        L.stats.urgent.fetch_add(1, std::memory_order_relaxed);
        tex_async_publish(L);
    }
    L.cv.notify_one();
    PERF_INC(tex_async_urgent);
}

void CResourceManager::_AsyncTextureDetach(CTexture* T)
{
    TexAsyncLoader& L = tex_async();
    u64 wait_t0 = 0;
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lk(L.mutex);
            u8 s = T->m_async_state.load(std::memory_order_acquire);
            if (s != CTexture::asReading && s != CTexture::asInstalling &&
                T->m_async_state.compare_exchange_strong(s, CTexture::asNone, std::memory_order_acq_rel))
            {
                if (T->m_async_entries.load(std::memory_order_relaxed))
                {
                    tex_async_forget(L.urgent, T);
                    tex_async_forget(L.normal, T);
                    tex_async_forget(L.done, T);
                    T->m_async_entries.store(0, std::memory_order_relaxed);
                }
                if (s == CTexture::asCreated)
                    T->async_release_created();
                if (s != CTexture::asNone && s != CTexture::asReady)
                    L.pending.fetch_sub(1, std::memory_order_relaxed);
                T->m_async_urgent.store(0, std::memory_order_relaxed);
                break;
            }
        }
        // The thread reads it or a caller finishes it - one file at most
        if (!wait_t0)
            wait_t0 = tex_async_now_us();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (wait_t0)
        L.stats.wait_us.fetch_add(tex_async_now_us() - wait_t0, std::memory_order_relaxed);
}

void CResourceManager::_AsyncTexturesPump()
{
    TexAsyncLoader& L = tex_async();
    if (!L.used.load(std::memory_order_relaxed))
        return;
    const u32 pending = L.pending.load(std::memory_order_relaxed);
#if XR_PERF_METRICS
    xray::perf::SetGauge(xray::perf::Metric::tex_async_pending, static_cast<s64>(pending));
#endif
    if (pending)
    {
        L.stats.frames.fetch_add(1, std::memory_order_relaxed);
        PERF_INC(tex_async_frames);
    }

    const u64 ticks0 = xray::perf::Ticks();
    const u64 us0 = tex_async_now_us();
    u32 placed = 0, created = 0;
    for (;;)
    {
        CTexture* T = nullptr;
        u8 s = CTexture::asNone;
        {
            std::lock_guard<std::mutex> lk(L.mutex);
            while (!L.done.empty() && !L.done.front())
                L.done.pop_front();
            if (L.done.empty())
                break;
            T = L.done.front();
            s = T->m_async_state.load(std::memory_order_acquire);
            // The creations on main within the budget, one at least; the rest next frame
            if (s == CTexture::asWarm && created && tex_async_now_us() - us0 >= kTexAsyncMainBudgetUs)
                break;
            L.done.pop_front();
            T->m_async_entries.fetch_sub(1, std::memory_order_relaxed);
            // asReady: a caller finished it already
            if ((s != CTexture::asWarm && s != CTexture::asCreated) ||
                !T->m_async_state.compare_exchange_strong(s, CTexture::asInstalling, std::memory_order_acq_rel))
                continue;
        }
        // asInstalling: the texture is alive until it leaves this state (its detach waits)
        {
            PERF_SCOPE(tex_async_main);
            if (s == CTexture::asCreated)
                T->async_install();
            else
            {
                T->load_now();
                ++created;
            }
        }
        tex_async_placed(*T, s == CTexture::asCreated ? TexAsyncPlaced::Thread : TexAsyncPlaced::Main);
        ++placed;
    }
    // 50n: the frame report's "texa" - textures put in place on main this frame and the time of it
    if (placed)
        xray::perf::FrameEventAdd(xray::perf::frame_event::TexAsync, xray::perf::Ticks() - ticks0, placed);
}

void CResourceManager::_AsyncTexturesShutdown(pcstr reason)
{
    TexAsyncLoader& L = tex_async();
    if (!L.used.load(std::memory_order_relaxed))
        return;
    {
        std::lock_guard<std::mutex> lk(L.mutex);
        L.stop.store(true, std::memory_order_relaxed);
    }
    L.cv.notify_all();
    if (L.thread.joinable())
        L.thread.join();

    // Nothing reads now: the pending textures go back to "not loaded" (the next bind or Load loads them as before -
    // the level load does it for all), the created ones are put in place
    xr_vector<CTexture*> created;
    {
        std::lock_guard<std::mutex> lk(L.mutex);
        const auto drain = [&L, &created](xr_deque<CTexture*>& q) {
            for (CTexture* T : q)
            {
                if (!T)
                    continue;
                T->m_async_entries.fetch_sub(1, std::memory_order_relaxed);
                u8 s = T->m_async_state.load(std::memory_order_acquire);
                if (s == CTexture::asQueued || s == CTexture::asWarm)
                {
                    if (T->m_async_state.compare_exchange_strong(s, CTexture::asNone, std::memory_order_acq_rel))
                    {
                        T->m_async_urgent.store(0, std::memory_order_relaxed);
                        L.pending.fetch_sub(1, std::memory_order_relaxed);
                    }
                }
                else if (s == CTexture::asCreated &&
                    T->m_async_state.compare_exchange_strong(s, CTexture::asInstalling, std::memory_order_acq_rel))
                    created.push_back(T);
            }
            q.clear();
        };
        drain(L.urgent);
        drain(L.normal);
        drain(L.done);
        L.warm.clear();
        L.urgent_any.store(false, std::memory_order_relaxed);
        L.reading = false;
        tex_async_publish(L);
        L.stop.store(false, std::memory_order_relaxed);
    }
    for (CTexture* T : created)
    {
        T->async_install();
        tex_async_placed(*T, TexAsyncPlaced::Thread);
    }

    // One line per period (since the previous one) with anything done
    TexAsyncStats& st = L.stats;
    const u32 queued = st.queued.exchange(0);
    const u32 warm_files = st.warm_files.exchange(0);
    const u32 warm_dropped = st.warm_dropped.exchange(0);
    const u32 urgent = st.urgent.exchange(0), on_thread = st.on_thread.exchange(0), on_main = st.on_main.exchange(0);
    const u32 sync = st.sync.exchange(0), early = st.early.exchange(0), frames = st.frames.exchange(0);
    const u64 sync_us = st.sync_us.exchange(0), wait_us = st.wait_us.exchange(0), bytes = st.bytes.exchange(0);
    const u32 wait_max_us = st.wait_max_us.exchange(0), latency_max = st.latency_max_ms.exchange(0);
    const u64 latency = st.latency_ms.exchange(0);
    const u32 placed = on_thread + on_main + sync + early;
    if (!queued && !warm_files && !placed)
        return;
    Msg("* [textures] async (%s): queued %u (bound %u), in place %u (created on the thread %u, on main %u), sync "
        "fallbacks %u / %u ms, from the cache %u, waits %u ms (max %u ms), placeholder avg %u ms / max %u ms, frames %u; "
        "warm-up %u files (%u dropped); read %u MB",
        reason, queued, urgent, placed, on_thread, on_main, sync, static_cast<u32>(sync_us / 1000), early,
        static_cast<u32>(wait_us / 1000), wait_max_us / 1000, placed ? static_cast<u32>(latency / placed) : 0,
        latency_max, frames, warm_files, warm_dropped, static_cast<u32>(bytes >> 20));
}

void CResourceManager::_AsyncTexturesBlock(bool block)
{
    TexAsyncLoader& L = tex_async();
    std::lock_guard<std::mutex> lk(L.mutex);
    L.blocked = block;
}

u32 CResourceManager::_TexturePrefetchFiles(const char* const* names, u32 count)
{
    if (!names || !count || !Device.b_is_Ready || !ModelPoolDiskSlow())
        return 0;
    // Names of textures registered already are left out: loaded, or in the async loading themselves
    xr_vector<shared_str> list;
    list.reserve(count);
    {
        ScopeLock scope(&m_textures_lock);
        for (u32 i = 0; i < count; ++i)
        {
            pcstr name = names[i];
            if (!name || !name[0] || name[0] == '$')
                continue;
            string_path fixed;
            xr_strcpy(fixed, name);
            fix_texture_name(fixed);
            if (m_textures.find(fixed) == m_textures.end())
                list.emplace_back(fixed);
        }
    }
    if (list.empty())
        return 0;
    TexAsyncLoader& L = tex_async();
    u32 queued = 0;
    {
        std::lock_guard<std::mutex> lk(L.mutex);
        if (L.blocked)
            return 0;
        for (const shared_str& name : list)
        {
            if (L.warm.size() >= kTexAsyncWarmMax)
            {
                L.stats.warm_dropped.fetch_add(static_cast<u32>(list.size() - queued), std::memory_order_relaxed);
                break;
            }
            L.warm.push_back(name);
            ++queued;
        }
        if (queued)
        {
            L.used.store(true, std::memory_order_relaxed);
            tex_async_start(L);
            tex_async_publish(L);
        }
    }
    if (queued)
        L.cv.notify_one();
    return queued;
}
/*
void fix_texture_name(LPSTR fn)
{
    LPSTR _ext = strext(fn);
    if (_ext &&
        (0==xr_stricmp(_ext, ".tga") ||
        0==xr_stricmp(_ext, ".dds") ||
        0==xr_stricmp(_ext, ".bmp") ||
        0==xr_stricmp(_ext, ".ogm")))
        *_ext = 0;
}
*/

//--------------------------------------------------------------------------------------------------------------
IBlender* CResourceManager::_GetBlender(LPCSTR Name)
{
    R_ASSERT(Name && Name[0]);

    pstr N = pstr(Name);
    map_Blender::iterator I = m_blenders.find(N);

    if (I == m_blenders.end())
    {
        Msg("! Shader '%s' not found in library.", Name);
        return nullptr;
    }

    return I->second;
}

IBlender* CResourceManager::_FindBlender(LPCSTR Name)
{
    if (!(Name && Name[0]))
        return nullptr;

    pstr N = pstr(Name);
    map_Blender::iterator I = m_blenders.find(N);
    if (I == m_blenders.end())
        return nullptr;
    else
        return I->second;
}

void CResourceManager::ED_UpdateBlender(LPCSTR Name, IBlender* data)
{
    pstr N = pstr(Name);
    map_Blender::iterator I = m_blenders.find(N);
    if (I != m_blenders.end())
    {
        R_ASSERT(data->getDescription().CLS == I->second->getDescription().CLS);
        xr_delete(I->second);
        I->second = data;
    }
    else
    {
        m_blenders.emplace(xr_strdup(Name), data);
    }
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CResourceManager::_ParseList(sh_list& dest, LPCSTR names)
{
    if (nullptr == names || 0 == names[0])
        names = "$null";

    dest.clear();
    char* P = (char*)names;
    svector<char, 128> N;

    while (*P)
    {
        if (*P == ',')
        {
            // flush
            N.push_back(0);
            xr_strlwr(N.begin());

            fix_texture_name(N.begin());
            //. andy			if (strext(N.begin())) *strext(N.begin())=0;
            dest.push_back(N.begin());
            N.clear();
        }
        else
        {
            N.push_back(*P);
        }
        P++;
    }
    if (N.size())
    {
        // flush
        N.push_back(0);
        xr_strlwr(N.begin());

        fix_texture_name(N.begin());
        //. andy		if (strext(N.begin())) *strext(N.begin())=0;
        dest.push_back(N.begin());
    }
}

ShaderElement* CResourceManager::_CreateElement(ShaderElement&& S)
{
    if (S.passes.empty())
        return nullptr;

    ScopeLock scope(&m_create_lock); // 40-parallel-prefetch.md

    // Search equal in shaders array (40-parallel-prefetch.md: through the hash index, it was a walk over v_elements)
    const size_t hash = S.hash();
    if (ShaderElement* elem = m_elements_index.find(hash, [&](ShaderElement& E) { return !!S.equal(E); }))
        return elem;

    // Create _new_ entry
    ShaderElement* N = v_elements.emplace_back(xr_new<ShaderElement>(std::move(S)));
    N->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    m_elements_index.insert(hash, N);
    return N;
}

void CResourceManager::_DeleteElement(const ShaderElement* S)
{
    if (0 == (S->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    ScopeLock scope(&m_create_lock); // 40-parallel-prefetch.md
    if (reclaim(v_elements, S))
    {
        m_elements_index.erase(S);
        return;
    }
    Msg("! ERROR: Failed to find compiled 'shader-element'");
}

Shader* CResourceManager::_FindOrRegisterShader(Shader& S)
{
    // 40-parallel-prefetch.md (plans/optimization_mt): through the hash index, it was a walk over v_shaders
    ScopeLock scope(&m_create_lock);
    const size_t hash = S.hash();
    if (Shader* found = m_shaders_index.find(hash, [&](Shader& registered) { return !!S.equal(&registered); }))
        return found;

    // Create _new_ entry
    Shader* N = v_shaders.emplace_back(xr_new<Shader>(std::move(S)));
    N->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    m_shaders_index.insert(hash, N);
    return N;
}

Shader* CResourceManager::_cpp_Create(
    IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    CBlender_Compile C;
    Shader S;

    //.
    // if (strstr(s_shader,"transparent"))	__asm int 3;

    // Access to template
    C.BT = B;
    C.bFFP = RImplementation.o.ffp;
    C.bDetail = FALSE;
#ifdef _EDITOR
    if (!C.BT)
    {
        ELog.Msg(mtError, "Can't find shader '%s'", s_shader);
        return 0;
    }
    C.bFFP = true;
#else
    UNUSED(s_shader);
#endif

    // Parse names
    _ParseList(C.L_textures, s_textures);
    _ParseList(C.L_constants, s_constants);
    _ParseList(C.L_matrices, s_matrices);

    // Compile element	(LOD0 - HQ)
    {
        C.iElement = SE_R1_NORMAL_HQ;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_NORMAL_HQ] = _CreateElement(std::move(E));
    }

    // Compile element	(LOD1)
    {
        C.iElement = SE_R1_NORMAL_LQ;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_NORMAL_LQ] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = SE_R1_LPOINT;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_LPOINT] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = SE_R1_LSPOT;
        C.bDetail = m_textures_description.GetDetailTexture(C.L_textures[0], C.detail_texture, C.detail_scaler);
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_LSPOT] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = SE_R1_LMODELS;
        C.bDetail = TRUE; //.$$$ HACK :)
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[SE_R1_LMODELS] = _CreateElement(std::move(E));
    }

    // Compile element
    {
        C.iElement = 5;
        C.bDetail = FALSE;
        ShaderElement E;
        C._cpp_Compile(&E);
        S.E[5] = _CreateElement(std::move(E));
    }

    // Search equal in shaders array, or create _new_ entry
    return _FindOrRegisterShader(S);
}

Shader* CResourceManager::_cpp_Create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    if (!GEnv.isDedicatedServer)
    {
        IBlender* pBlender = _GetBlender(s_shader ? s_shader : "null");
        if (!pBlender)
            return nullptr;
        return _cpp_Create(pBlender, s_shader, s_textures, s_constants, s_matrices);
    }
    return nullptr;
}

IReader* open_shader(pcstr shader)
{
    string_path shaderPath;

    FS.update_path(shaderPath, "$game_shaders$", RImplementation.getShaderPath());
    xr_strcat(shaderPath, shader);

    return FS.r_open(shaderPath);
}

void CResourceManager::CompatibilityCheck()
{
    ZoneScoped;

    // Check Shoker HQ Geometry Fix support
    {
        IReader* skinh = open_shader("skin.h");
        R_ASSERT3(skinh, "Can't open shader", "skin.h");
        xr_string str(static_cast<pcstr>(skinh->pointer()), skinh->length());

        bool hq_skinning = true;
        // search for (12.f / 32768.f)
        const auto check = [&](cpcstr searchBegin, cpcstr searchEnd) -> bool
        {
            cpcstr begin = strstr(str.c_str(), searchBegin);
            if (!begin)
                return false;

            cpcstr end = strstr(begin, searchEnd);
            if (!end)
                return false;

            str.assign(begin, end);
            pcstr ptr = str.data();

            if ((ptr = strstr(ptr, "12.")))     // 12.f or 12.0
            {
                if ((ptr = strstr(ptr, "/")))   // /
                    if (strstr(ptr, "32768."))  // 32768.f or 32768.0
                    {
                        hq_skinning = false;    // found
                        return true;
                    }
            }
            return false;
        };
        if (!check("u_position", "sbones_array"))
        {
            check("skinning_pos", "skinning_0");
        }
        RImplementation.m_hq_skinning = hq_skinning;
        FS.r_close(skinh);
    }
}

Shader* CResourceManager::Create(IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    if (GEnv.isDedicatedServer)
        return nullptr;

    ScopeLock scope(&m_create_lock); // 40-parallel-prefetch.md: one shader creation at a time
    return _cpp_Create(B, s_shader, s_textures, s_constants, s_matrices);
}

Shader* CResourceManager::Create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    if (!GEnv.isDedicatedServer)
    {
        // 40-parallel-prefetch.md (plans/optimization_mt): one shader creation at a time. The models of the prefetch
        // list load on several threads (D3D11), the blender compile and the registries below are not thread-safe.
        ScopeLock scope(&m_create_lock);
#if defined(USE_DX9)
        const bool useCppBlender = RImplementation.o.ffp && _GetBlender(s_shader);
        if (!useCppBlender && _lua_HasShader(s_shader))
            return _lua_Create(s_shader, s_textures);
        return _cpp_Create(s_shader, s_textures, s_constants, s_matrices);
#else // TODO: DX11: When all shaders are ready switch to common path
        if (_lua_HasShader(s_shader))
        {
            return _lua_Create(s_shader, s_textures);
        }
        else
        {
            Shader* pShader = _cpp_Create(s_shader, s_textures, s_constants, s_matrices);
            if (pShader)
            {
                return pShader;
            }
            else
            {
                if (_lua_HasShader("stub_default"))
                    return _lua_Create("stub_default", s_textures);
                else
                {
                    FATAL("Can't find stub_default.s");
                    return 0;
                }
            }
        }
#endif
    }
    return nullptr;
}

void CResourceManager::Delete(const Shader* S)
{
    if (0 == (S->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    ScopeLock scope(&m_create_lock); // 40-parallel-prefetch.md
    if (reclaim(v_shaders, S))
    {
        m_shaders_index.erase(S);
        return;
    }
    Msg("! ERROR: Failed to find complete shader");
}

void CResourceManager::DeferredUpload()
{
    if (!Device.b_is_Ready)
        return;

    ZoneScoped;
    PERF_SCOPE(tex_deferred_upload); // spawn stage 0: level load, CTexture::Load of every texture (texture/load/*)

    // 50r-slow-disk-async-textures.md: the async loading stops (its pending textures load below with all the others,
    // after the one reader of a slow disk) and nothing is queued until the upload is over
    _AsyncTexturesShutdown("level load");
    _AsyncTexturesBlock(true);
    struct AsyncUnblock
    {
        CResourceManager& rm;
        ~AsyncUnblock() { rm._AsyncTexturesBlock(false); }
    } async_unblock{ *this };

#if defined(USE_DX11)
    // 40-parallel-prefetch.md (plans/optimization_mt): the textures one at a time from a shared counter. The range
    // split of xr_parallel_for_each gave each worker one block of size / workers textures of the map, in name order,
    // so the big textures of one folder fell to one worker and the rest spun idle at the end (task/steal_attempt).
    // A texture costs 0.1-50 ms, a task per worker (and one for the waiting main thread) takes the next texture.
    CTimer timer;
    timer.Start();
    xr_vector<CTexture*> textures;
    {
        ScopeLock scope(&m_textures_lock);
        textures.reserve(m_textures.size());
        for (const auto& it : m_textures)
            textures.push_back(it.second);
    }
    const u32 direct_before = g_texture_direct_loads.load(std::memory_order_relaxed);
    std::atomic<size_t> next{0};
    const size_t tasks = std::max<size_t>(1, std::min<size_t>(textures.size(),
        (TaskScheduler ? TaskScheduler->GetWorkersCount() : std::thread::hardware_concurrency()) + 1));
    // 50a-stage1-prefetch.md: a slow disk - the loads after one reader
    std::unique_ptr<TextureWarmReader> reader;
    std::thread reader_thread;
    if (ModelPoolDiskSlow() && !textures.empty())
    {
        reader = std::make_unique<TextureWarmReader>();
        reader->prepare(textures, next);
        reader_thread = Threading::RunThread("Texture warm", &texture_warm_thread, reader.get());
    }
    xr_parallel_for(TaskRange<size_t>(0, tasks, 1), [&](const TaskRange<size_t>&)
    {
        for (;;)
        {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= textures.size())
                break;
            if (reader)
                reader->wait(i);
            textures[i]->Load();
        }
    });
    if (reader)
    {
        reader->stop.store(true, std::memory_order_relaxed);
        reader_thread.join();
    }
    Msg("* [textures] deferred upload: %u textures, %u straight from the file, %u ms on %u tasks",
        static_cast<u32>(textures.size()), g_texture_direct_loads.load(std::memory_order_relaxed) - direct_before,
        timer.GetElapsed_ms(), static_cast<u32>(tasks));
    if (reader)
    {
        Msg("* [textures] slow disk: one reader in name order read %u files, the loads waited for it %u ms, %u already "
            "loaded skipped",
            reader->files.load(), static_cast<u32>(reader->waited_us.load() / 1000), reader->loaded);
    }
#elif defined(USE_OGL) // XXX: OGL: Set additional contexts for all worker threads?
    if (ModelPoolDiskSlow() && !m_textures.empty())
    {
        // 50a-stage1-prefetch.md: a slow disk - the reader goes ahead of the loads on main, the conversion and the
        // upload of a texture overlap the reading of the next ones
        xr_vector<CTexture*> textures;
        {
            ScopeLock scope(&m_textures_lock);
            textures.reserve(m_textures.size());
            for (const auto& it : m_textures)
                textures.push_back(it.second);
        }
        std::atomic<size_t> next{0};
        TextureWarmReader reader;
        reader.prepare(textures, next);
        std::thread reader_thread = Threading::RunThread("Texture warm", &texture_warm_thread, &reader);
        for (size_t i = 0; i < textures.size(); ++i)
        {
            next.store(i + 1, std::memory_order_relaxed);
            reader.wait(i);
            textures[i]->Load();
        }
        reader.stop.store(true, std::memory_order_relaxed);
        reader_thread.join();
        Msg("* [textures] slow disk: one reader in name order read %u files, the loads waited for it %u ms, %u already "
            "loaded skipped",
            reader.files.load(), static_cast<u32>(reader.waited_us.load() / 1000), reader.loaded);
        return;
    }
    for (auto& texture : m_textures)
        texture.second->Load();
#else
#   error No graphics API selected or enabled!
#endif
}

void CResourceManager::DeferredUnload()
{
    if (!Device.b_is_Ready)
        return;

    ZoneScoped;

#if defined(USE_DX11)
    xr_parallel_for_each(m_textures, [&](auto m_tex) { m_tex.second->Unload(); });
#elif defined(USE_OGL) // XXX: OGL: Set additional contexts for all worker threads?
    for (auto& texture : m_textures)
        texture.second->Unload();
#else
#   error No graphics API selected or enabled!
#endif
}

#ifdef _EDITOR
void CResourceManager::ED_UpdateTextures(AStringVec* names)
{
    // 1. Unload
    if (names)
    {
        for (u32 nid = 0; nid < names->size(); nid++)
        {
            map_TextureIt I = m_textures.find((*names)[nid].c_str());
            if (I != m_textures.end())
                I->second->Unload();
        }
    }
    else
    {
        for (map_TextureIt t = m_textures.begin(); t != m_textures.end(); t++)
            t->second->Unload();
    }

    // 2. Load
    // DeferredUpload	();
}
#endif

// The sums in u64 (plan 50 R13, 50d-stage2-auto-engine.md): 14 thousand textures of a level pass 4 GB on a big video card
void CResourceManager::_GetMemoryUsage(u64& m_base, u32& c_base, u64& m_lmaps, u32& c_lmaps)
{
    m_base = m_lmaps = 0;
    c_base = c_lmaps = 0;

    map_Texture::iterator I = m_textures.begin();
    map_Texture::iterator E = m_textures.end();
    for (; I != E; ++I)
    {
        u32 m = I->second->flags.MemoryUsage;
        if (strstr(I->first, "lmap"))
        {
            c_lmaps++;
            m_lmaps += m;
        }
        else
        {
            c_base++;
            m_base += m;
        }
    }
}
bool CResourceManager::_TryGetMemoryUsage(u64& m_base, u32& c_base, u64& m_lmaps, u32& c_lmaps)
{
    m_base = m_lmaps = 0;
    c_base = c_lmaps = 0;
    if (!m_textures_lock.TryEnter())
        return false;
    _GetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);
    m_textures_lock.Leave();
    return true;
}

void CResourceManager::_DumpMemoryUsage()
{
    xr_multimap<u32, std::pair<u32, shared_str>> mtex;

    // sort
    {
        map_Texture::iterator I = m_textures.begin();
        map_Texture::iterator E = m_textures.end();
        for (; I != E; ++I)
        {
            u32 m = I->second->flags.MemoryUsage;
            shared_str n = I->second->cName;
            mtex.emplace(m, std::make_pair(I->second->ref_count.load(), n));
        }
    }

    // dump
    {
        xr_multimap<u32, std::pair<u32, shared_str>>::iterator I = mtex.begin();
        xr_multimap<u32, std::pair<u32, shared_str>>::iterator E = mtex.end();
        for (; I != E; ++I)
            Msg("* %4.1f : [%4d] %s", float(I->first) / 1024.f, I->second.first, I->second.second.c_str());
    }
}

void CResourceManager::dbg_texture_stats(u32& total, u32& user_made, u32& inv_icon_rts) const
{
    total = u32(m_textures.size());
    user_made = 0;
    inv_icon_rts = 0;
    static constexpr char k_user[] = "$user$";
    static constexpr char k_icon[] = "$user$itm_inv_";
    for (const auto& pair : m_textures)
    {
        const char* name = pair.first;
        if (!name)
            continue;
        if (0 == strncmp(name, k_user, sizeof(k_user) - 1))
        {
            ++user_made;
            if (0 == strncmp(name, k_icon, sizeof(k_icon) - 1))
                ++inv_icon_rts;
        }
    }
}

void CResourceManager::Evict()
{
    // TODO: DX11: check if we really need this method
}
/*
BOOL	CResourceManager::_GetDetailTexture(LPCSTR Name,LPCSTR& T, R_constant_setup* &CS)
{
    LPSTR N = LPSTR(Name);
    map_TD::iterator I = m_td.find	(N);
    if (I!=m_td.end())
    {
        T	= I->second.T;
        CS	= I->second.cs;
        return TRUE;
    } else {
        return FALSE;
    }
}*/
} // namespace xray::render::RENDER_NAMESPACE
