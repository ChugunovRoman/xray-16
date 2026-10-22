#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Threading/TaskManager.hpp"

#include <vorbis/vorbisfile.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// 46-engine-core-render-audit.md: CSoundRender_Source::m_pcm_state
namespace
{
constexpr u8 snd_pcm_state_none = 0;   // not decided yet: streamed, the starts of play are counted
constexpr u8 snd_pcm_state_queued = 1; // budget reserved, the decode task is queued or running
constexpr u8 snd_pcm_state_done = 2;   // m_cached_pcm published
constexpr u8 snd_pcm_state_off = 3;    // stays streamed (long, over the budget, no task manager, file not opened)

// plans/optimization_mt/50j-slow-disk-async-sound.md: CSoundRender_Source::m_src_state
constexpr u8 snd_src_ready = 0;   // read (the default: a source loaded the usual way)
constexpr u8 snd_src_pending = 1; // deferred, its file not read yet
constexpr u8 snd_src_loading = 2; // being read by the thread that took it (begin_load)
constexpr u8 snd_src_failed = 3;  // the read failed: zero length, nothing plays
} // namespace

// B-1: global accounting of bytes held in per-source PCM caches, bounded by psSoundCacheSizeMB. Touched from
// load/unload and the decode tasks: main, the Game thread and the workers, so the budget is reserved by a
// compare-exchange (try_reserve_pcm) - a check and a later add could let two loads overshoot it.
static std::atomic<size_t> s_cached_pcm_bytes{0};

static bool try_reserve_pcm(size_t bytes, size_t limit)
{
    size_t cur = s_cached_pcm_bytes.load(std::memory_order_relaxed);
    do
    {
        if (cur + bytes > limit)
            return false;
    } while (!s_cached_pcm_bytes.compare_exchange_weak(cur, cur + bytes, std::memory_order_relaxed));
    return true;
}

size_t CSoundRender_Source::cached_pcm_bytes_total() { return s_cached_pcm_bytes.load(std::memory_order_relaxed); }

static void snd_pcm_cached_kb_gauge()
{
    xray::perf::SetGauge(xray::perf::Metric::snd_pcm_cached_kb,
        static_cast<s64>(s_cached_pcm_bytes.load(std::memory_order_relaxed) / 1024));
}

#if XR_PERF_METRICS
// xrSound does not link xrEngine (cycle), so it cannot call xrEngine's IsGameLogicThread() directly - but
// xrCore::perf exports g_main_tid/g_game_tid for exactly this (set by xrEngine's EngineThreading.cpp), so
// xray::perf::CurrentThreadKind() still tells GameThread apart from main and from TaskManager pool workers.
struct SndDecodeTime
{
    bool active{false};
    u64 t0{0};

    SndDecodeTime() noexcept
        : active(xray::perf::g_perf_metrics_active), t0(active ? xray::perf::Ticks() : 0)
    {}

    ~SndDecodeTime() noexcept
    {
        if (!active)
            return;
        const u64 dt = xray::perf::Ticks() - t0;
        using M = xray::perf::Metric;
        switch (xray::perf::CurrentThreadKind())
        {
        case xray::perf::ThreadKind::Game:   xray::perf::AddTimeSlow(M::snd_decode_game, dt); break;
        case xray::perf::ThreadKind::Main:   xray::perf::AddTimeSlow(M::snd_decode_main, dt); break;
        case xray::perf::ThreadKind::Worker: xray::perf::AddTimeSlow(M::snd_decode_worker, dt); break;
        }
    }
};
#endif

CSoundRender_Source::~CSoundRender_Source() { unload(); }

CSoundRender_Source::CSoundRender_Source(CSoundRender_Source&& other) noexcept
    : pname(other.pname), fname(other.fname), fTimeTotal(other.fTimeTotal), dwBytesTotal(other.dwBytesTotal),
      m_data_info(other.m_data_info), m_info(other.m_info),
      m_cached_pcm(other.m_cached_pcm.exchange(nullptr, std::memory_order_relaxed)),
      m_pcm_plays(other.m_pcm_plays.load(std::memory_order_relaxed)),
      m_pcm_state(other.m_pcm_state.exchange(snd_pcm_state_none, std::memory_order_relaxed)),
      m_src_state(other.m_src_state.load(std::memory_order_relaxed)),
      m_src_urgent(other.m_src_urgent.load(std::memory_order_relaxed))
{
    VERIFY(m_pcm_state.load(std::memory_order_relaxed) != snd_pcm_state_queued);
    // the PCM (if any) and its share of the budget belong to this one now: other.unload() gives back nothing
    other.fTimeTotal = 0.0f;
    other.dwBytesTotal = 0;
}

namespace
{
bool ov_can_continue_read(long res)
{
    switch (res)
    {
    case 0:
        return false;
    // info
    case OV_HOLE:
        Msg("Vorbisfile encoutered missing or corrupt data in the bitstream. Recovery is normally automatic and this "
            "return code is for informational purposes only.");
        return true;
    case OV_EBADLINK:
        Msg("The given link exists in the Vorbis data stream, but is not decipherable due to garbage or corruption.");
        return true;
    // error
    case OV_FALSE: Msg("Not true, or no data available"); return false;
    case OV_EREAD: Msg("Read error while fetching compressed data for decode"); return false;
    case OV_EFAULT: Msg("Internal inconsistency in decode state. Continuing is likely not possible."); return false;
    case OV_EIMPL: Msg("Feature not implemented"); return false;
    case OV_EINVAL:
        Msg("Either an invalid argument, or incompletely initialized argument passed to libvorbisfile call");
        return false;
    case OV_ENOTVORBIS: Msg("The given file/data was not recognized as Ogg Vorbis data."); return false;
    case OV_EBADHEADER:
        Msg("The file/data is apparently an Ogg Vorbis stream, but contains a corrupted or undecipherable header.");
        return false;
    case OV_EVERSION: Msg("The bitstream format revision of the given stream is not supported."); return false;
    case OV_ENOSEEK: Msg("The given stream is not seekable"); return false;
    }
    return false;
}
}

void CSoundRender_Source::decompress(void* dest, u32 byte_offset, u32 size, OggVorbis_File* ovf) const
{
    PERF_HOT_ZONE("decompress");

    // B-1: short SFX are fully decoded once into m_cached_pcm (shared across all emitters of this source).
    // Serve the requested slice with a plain memcpy instead of re-running the OGG decoder per emitter/chunk.
    if (const u8* const pcm = m_cached_pcm.load(std::memory_order_acquire))
    {
        ZoneScopedN("decompress_cached"); // diagnostic: count = cache hits (vs decompress count = total)
        auto* const d = static_cast<u8*>(dest);
        const u32 total = dwBytesTotal; // the size of m_cached_pcm
        u32 copied = 0;
        if (byte_offset < total)
        {
            copied = std::min(size, total - byte_offset);
            std::memcpy(d, pcm + byte_offset, copied);
        }
        if (copied < size) // past end of sound -> silence
            std::memset(d + copied, 0, size - copied);
        PERF_ADD(snd_pcm_cache_bytes, size);
        return;
    }

    // 46-engine-core-render-audit.md: a start of play (looped sounds come back to 0 too) - this one is streamed, a
    // repeated one queues the whole decode for the next ones
    if (byte_offset == 0)
        request_pcm_cache();

#if XR_PERF_METRICS
    SndDecodeTime decode_time;
#endif
    PERF_ADD(snd_pcm_decode_bytes, size);

    // seek
    const auto sample_offset = ogg_int64_t(byte_offset / m_data_info.blockAlign);
    const u32 cur_pos = u32(ov_pcm_tell(ovf));
    if (cur_pos != sample_offset)
        ov_pcm_seek(ovf, sample_offset);

    // decompress
    if (m_data_info.format == SoundFormat::Float32)
        i_decompress(ovf, static_cast<float*>(dest), size);
    else
        i_decompress(ovf, static_cast<char*>(dest), size);
}

bool CSoundRender_Source::i_decompress(OggVorbis_File* ovf, char* _dest, u32 size) const
{
    long TotalRet = 0;

    // Read loop
    while (TotalRet < static_cast<long>(size))
    {
        const auto ret = ov_read(ovf, _dest + TotalRet, size - TotalRet, 0, 2, 1, nullptr);
        if (ret <= 0 && !ov_can_continue_read(ret))
            return false;
        TotalRet += ret;
    }
    return true;
}

bool CSoundRender_Source::i_decompress(OggVorbis_File* ovf, float* _dest, u32 size) const
{
    s32 left = s32(size / m_data_info.blockAlign);
    while (left)
    {
        float** pcm;
        long samples = ov_read_float(ovf, &pcm, left, nullptr);

        if (samples <= 0 && !ov_can_continue_read(samples))
            return false;

        if (samples > left)
            samples = left;

        for (long j = 0; j < samples; j++)
            for (long i = 0; i < m_data_info.channels; i++)
                *_dest++ = clampr(pcm[i][j], -1.0f, 1.0f);

        left -= samples;
    }
    return true;
}

constexpr ov_callbacks g_ov_callbacks =
{
    // read
    [](void* ptr, size_t size, size_t nmemb, void* datasource) -> size_t
    {
        auto* file = static_cast<IReader*>(datasource);
        const size_t exist_block = _max(0ul, iFloor(file->elapsed() / (float)size));
        const size_t read_block = std::min(exist_block, nmemb);
        file->r(ptr, read_block * size);
        return read_block;
    },
    // seek
    [](void* datasource, ogg_int64_t offset, int whence) -> int
    {
        //SEEK_SET 0 File beginning
        //SEEK_CUR 1 Current file pointer position
        //SEEK_END 2 End-of-file
        switch (whence)
        {
        case SEEK_SET: ((IReader*)datasource)->seek((int)offset); break;
        case SEEK_CUR: ((IReader*)datasource)->advance((int)offset); break;
        case SEEK_END: ((IReader*)datasource)->seek((int)offset + ((IReader*)datasource)->length()); break;
        }
        return 0;
    },
    // close
    [](void* datasource) -> int
    {
        auto* file = static_cast<IReader*>(datasource);
        FS.r_close(file);
        return 0;
    },
    // tell
    [](void* datasource) -> long
    {
        const auto file = static_cast<IReader*>(datasource);
        return static_cast<long>(file->tell());
    },
};

OggVorbis_File* CSoundRender_Source::open() const
{
    ZoneScopedN("CSoundRender_Source::open");
    PERF_INC(snd_ogg_open);
    PERF_SCOPE_BY_THREAD(snd_ogg_open_main, snd_ogg_open_game, snd_ogg_open_worker); // stage 1: where it is paid

    const auto file = FS.r_open(pname.c_str());
    R_ASSERT3(file && file->length(), "Can't open wave file:", pname.c_str());

    const auto ovf = xr_new<OggVorbis_File>();
    ov_open_callbacks(file, ovf, nullptr, 0, g_ov_callbacks);

    return ovf;
}

void CSoundRender_Source::close(OggVorbis_File*& ovf) const
{
    if (!ovf)
        return;
    ov_clear(ovf);
    xr_delete(ovf);
}

bool CSoundRender_Source::should_cache_pcm() const
{
    return psSoundCacheShortSec > 0.f && fTimeTotal > 0.f && fTimeTotal <= psSoundCacheShortSec &&
        dwBytesTotal > 0 && m_data_info.blockAlign > 0;
}

struct CSoundRender_Source::PcmJob
{
    OggVorbis_File* ovf{};
    u8* pcm{};
    u32 done{}; // bytes decoded so far
};

// 46-engine-core-render-audit.md. The PCM cache is filled by use, not by load order. Before, every short source was
// decoded whole at load while the budget lasted: the sounds created by the startup prefetch took all of it (heavy fight
// log s_al: snd_cache_size 96 full already in the main menu, 2369 sources of the level over the budget), and the sounds
// played in a fight were streamed - ogg decoded per emitter, per chunk (sound/decode/worker ~5.6 ms CPU per frame,
// sound/pcm/decode_bytes ~0.97 MB/frame against cache_bytes ~0.35 MB/frame). Now a short sound is cached when it
// starts playing the second time (a sound played once, as most voice lines, takes no budget), by a TaskScheduler task:
// the start that asks is streamed as before, the later ones read the cache. The samples are the same either way (the
// same decoder over the whole sound or from a seek), so what is heard does not change.
void CSoundRender_Source::request_pcm_cache() const
{
    if (m_pcm_state.load(std::memory_order_relaxed) != snd_pcm_state_none)
        return;

    if (!should_cache_pcm() || !TaskScheduler)
    {
        u8 expected = snd_pcm_state_none;
        if (m_pcm_state.compare_exchange_strong(expected, snd_pcm_state_off, std::memory_order_relaxed))
            PERF_INC(snd_pcm_reject_len);
        return;
    }

    // the first start of play is only counted
    if (m_pcm_plays.fetch_add(1, std::memory_order_relaxed) < 1)
        return;

    u8 expected = snd_pcm_state_none;
    if (!m_pcm_state.compare_exchange_strong(expected, snd_pcm_state_queued, std::memory_order_acq_rel))
        return; // another start took it

    // Stay within the global PCM cache budget (psSoundCacheSizeMB). Over budget -> keep streaming this one.
    // Reserved before the decode (given back by the task if the file does not open), see try_reserve_pcm.
    const size_t budget = size_t(psSoundCacheSizeMB) * 1024u * 1024u;
    if (!try_reserve_pcm(dwBytesTotal, budget))
    {
        m_pcm_state.store(snd_pcm_state_off, std::memory_order_release);
        PERF_INC(snd_pcm_reject_budget);
        return;
    }

    // The source outlives the task: a source is freed only after unload() (CSoundRender_Core::_clear,
    // refresh_sources, the destructor), and unload() waits for the task (wait_pcm_task).
    TaskScheduler->AddTask([this, job = xr_new<PcmJob>()] { cache_pcm_step(job); });
}

// One step decodes at most 128 KB (~0.7 ms of a worker), then queues the next one: a thread that waits in TaskManager
// (Wait, wait_prefill) runs queued tasks itself, so one long task could stall main or the Game thread. The steps read
// the stream in order with the same calls as one decode of the whole sound, so the samples are the same.
void CSoundRender_Source::cache_pcm_step(PcmJob* job) const
{
    ZoneScopedN("snd_cache_pcm");

    if (!job->ovf)
    {
        job->ovf = open();
        PERF_INC(snd_ogg_open_cache);
        if (!job->ovf)
        {
            s_cached_pcm_bytes.fetch_sub(dwBytesTotal, std::memory_order_relaxed);
            xr_delete(job);
            m_pcm_state.store(snd_pcm_state_off, std::memory_order_release);
            return;
        }
        job->pcm = xr_alloc<u8>(dwBytesTotal);
        std::memset(job->pcm, 0, dwBytesTotal); // as the zero-initialized vector before: a broken stream leaves silence
    }

    constexpr u32 step_bytes = 128 * 1024;
    const u32 left = dwBytesTotal - job->done;
    const u32 align = m_data_info.blockAlign; // > 0 (should_cache_pcm); dwBytesTotal is a multiple of it
    const u32 size = left > step_bytes ? step_bytes / align * align : left;
    bool complete;
    {
#if XR_PERF_METRICS
        SndDecodeTime decode_time;
#endif
        PERF_ADD(snd_pcm_decode_bytes, size);
        // decode the sound from the start (open() positions at 0) into the shared buffer
        if (m_data_info.format == SoundFormat::Float32)
            complete = i_decompress(job->ovf, reinterpret_cast<float*>(job->pcm + job->done), size);
        else
            complete = i_decompress(job->ovf, reinterpret_cast<char*>(job->pcm + job->done), size);
    }
    job->done += size;

    if (complete && job->done < dwBytesTotal)
    {
        TaskScheduler->AddTask([this, job] { cache_pcm_step(job); });
        return;
    }

    // the whole sound (or as much as the stream gave, the rest is silence, as before)
    close(job->ovf);
    u8* const pcm = job->pcm;
    xr_delete(job);
    m_cached_pcm.store(pcm, std::memory_order_release);
    m_pcm_state.store(snd_pcm_state_done, std::memory_order_release);

    PERF_INC(snd_pcm_accept);
    snd_pcm_cached_kb_gauge();
}

void CSoundRender_Source::wait_pcm_task() const
{
    // the same help-while-waiting loop as CSoundRender_Emitter::wait_prefill (plans/optimization_mt 50g, R11)
    const auto done = [this] { return m_pcm_state.load(std::memory_order_acquire) != snd_pcm_state_queued; };
    if (TaskScheduler)
        TaskScheduler->HelpUntil(done);
    else
    {
        while (!done())
            std::this_thread::yield();
    }
}

bool CSoundRender_Source::LoadWave(pcstr pName, bool background)
{
    ZoneScoped;

    // 50j: a deferred source got pname from prepare_deferred, before any other thread could see it; pName is its c_str()
    if (!background)
        pname = pName;

    // Load file into memory and parse WAV-format
    OggVorbis_File ovf;
    {
        // 34-sound-cold-load.md: where a cold load waits. r_open = the FS lookup and the open of the file (a loose file
        // of 16 KB and more is memory-mapped, a smaller one read whole; a file of a .db archive is a view of the
        // archive mapping), ov_open = the vorbis headers and, the stream being seekable, the seek to its end for the
        // length: the first reads of the mapped pages, i.e. the disk reads of a file not yet in the OS cache.
        ZoneScopedN("snd_load_open");
        PERF_SCOPE_IF(!background, snd_load_open);
        PERF_SCOPE_IF(xray::perf::TlGameEvent() != 0, ge_cold_sound_open);
        IReader* wave;
        {
            ZoneScopedN("snd_load_r_open");
            PERF_SCOPE_IF(!background, snd_load_r_open);
            wave = FS.r_open(pname.c_str());
        }
        R_ASSERT3(wave && wave->length(), "Can't open wave file:", pname.c_str());
        {
            ZoneScopedN("snd_load_ov_open");
            ov_open_callbacks(wave, &ovf, nullptr, 0, g_ov_callbacks);
        }
    }

    const vorbis_info* ovi = ov_info(&ovf, -1);

    // verify
    R_ASSERT3_CURE(ovi, "Invalid source info:", pName,
    {
        ov_clear(&ovf);
        return false;
    });

    m_data_info = {};

    m_data_info.samplesPerSec = ovi->rate;
    m_data_info.channels = u16(ovi->channels);

    if (SoundRender->supports_float_pcm)
    {
        m_data_info.format = SoundFormat::Float32;
        m_data_info.bitsPerSample = 32;
    }
    else
    {
        m_data_info.format = SoundFormat::PCM;
        m_data_info.bitsPerSample = 16;
    }

    m_data_info.blockAlign = m_data_info.bitsPerSample / 8 * m_data_info.channels;
    m_data_info.avgBytesPerSec = m_data_info.samplesPerSec * m_data_info.blockAlign;
    m_data_info.bytesPerBuffer = sdef_target_block * m_data_info.avgBytesPerSec / 1000;

    const s64 pcm_total = ov_pcm_total(&ovf, -1);
    dwBytesTotal = u32(pcm_total * m_data_info.blockAlign);
    fTimeTotal = dwBytesTotal / float(m_data_info.avgBytesPerSec);

    m_info = {};

    const vorbis_comment* ovm = ov_comment(&ovf, -1);
    if (ovm->comments)
    {
        IReader F(ovm->user_comments[0], ovm->comment_lengths[0]);
        const u32 vers = F.r_u32();
        if (vers == 0x0001)
        {
            m_info.minDist = F.r_float();
            m_info.maxDist = F.r_float();
            m_info.baseVolume = 1.f;
            m_info.gameType = F.r_u32();
            m_info.maxAIDist = m_info.maxDist;
        }
        else if (vers == 0x0002)
        {
            m_info.minDist = F.r_float();
            m_info.maxDist = F.r_float();
            m_info.baseVolume = F.r_float();
            m_info.gameType = F.r_u32();
            m_info.maxAIDist = m_info.maxDist;
        }
        else if (vers == OGG_COMMENT_VERSION)
        {
            m_info.minDist = F.r_float();
            m_info.maxDist = F.r_float();
            m_info.baseVolume = F.r_float();
            m_info.gameType = F.r_u32();
            m_info.maxAIDist = F.r_float();
        }
        else
        {
#ifndef MASTER_GOLD
            Log("! Invalid ogg-comment version, file: ", pName);
#endif
        }
    }
    else
    {
#ifndef MASTER_GOLD
        Log("! Missing ogg-comment, file: ", pName);
#endif
    }

    R_ASSERT3_CURE(m_info.maxAIDist >= 0.1f && m_info.maxDist >= 0.1f, "Invalid max distance.", pName,
    {
        ov_clear(&ovf);
        return false;
    });

    ov_clear(&ovf);
    return true;
}

bool CSoundRender_Source::load(pcstr name)
{
    string_path fn;
    const bool loaded = resolve_path(name, fn) && LoadWave(fn); // B-1: a short SFX is cached later, by use
    // 50j: a pending source loaded here (refresh_sources) is read now
    m_src_state.store(loaded ? snd_src_ready : snd_src_failed, std::memory_order_release);
    return loaded;
}

bool CSoundRender_Source::resolve_path(pcstr name, string_path& fn)
{
    string_path N;
    xr_strcpy(N, name);
#ifdef XR_PLATFORM_WINDOWS
    xr_strlwr(N);
#endif

    if (strext(N))
        *strext(N) = 0;

    fname = N;

    strconcat(fn, N, ".ogg");
    if (!FS.exist("$level$", fn))
        FS.update_path(fn, "$game_sounds$", fn);

#ifndef MASTER_GOLD
    if (!FS.exist(fn))
    {
        Msg("~ %s: Can't find sound '%s'", __FUNCTION__, name);
#   ifdef _EDITOR
        FS.update_path(fn, "$game_sounds$", "$no_sound.ogg");
#   endif
    }
#endif

    return FS.exist(fn);
}

// plans/optimization_mt/50j-slow-disk-async-sound.md. The source is not in s_sources yet: the thread that inserts it
// publishes these writes with the map (s_sources_lock).
bool CSoundRender_Source::prepare_deferred(pcstr name)
{
    string_path fn;
    if (!resolve_path(name, fn))
        return false;
    pname = fn;
    m_src_state.store(snd_src_pending, std::memory_order_relaxed);
    return true;
}

bool CSoundRender_Source::begin_load()
{
    u8 expected = snd_src_pending;
    return m_src_state.compare_exchange_strong(expected, snd_src_loading, std::memory_order_acq_rel);
}

void CSoundRender_Source::finish_load(bool background)
{
    VERIFY(m_src_state.load(std::memory_order_relaxed) == snd_src_loading);
    const bool loaded = LoadWave(pname.c_str(), background);
    if (!loaded)
    {
        fTimeTotal = 0.0f;
        dwBytesTotal = 0;
    }
    m_src_state.store(loaded ? snd_src_ready : snd_src_failed, std::memory_order_release);
}

SoundSourceState CSoundRender_Source::load_state() const
{
    switch (m_src_state.load(std::memory_order_acquire))
    {
    case snd_src_ready: return SoundSourceState::Ready;
    case snd_src_failed: return SoundSourceState::Failed;
    default: return SoundSourceState::Pending;
    }
}

// 50j: the source is needed now - its length (a sound created the usual way, a script's length()), its format or info
// (the start of a play) - while it is pending or being read. The callers that can wait (CSoundPlayer) ask
// load_state first, so this is the fallback: counted, the time is the wait of the caller.
void CSoundRender_Source::ensure_ready_slow() const
{
    auto* self = const_cast<CSoundRender_Source*>(this); // the fields are written by the one that takes the read
    if (self->begin_load())
    {
        ZoneScopedN("snd_async_sync");
        PERF_SCOPE(snd_async_sync);
        GW_FRAME_EVENT_IF(xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker, SoundCold); // 50n: the frame spike report
        self->finish_load(false);
        return;
    }
    if (m_src_state.load(std::memory_order_acquire) != snd_src_loading)
        return; // ready or failed
    // The "Sound prefetch" thread reads it now (one file). It runs at a low priority: sleep, do not spin, so it gets
    // the CPU.
    ZoneScopedN("snd_async_wait");
    PERF_SCOPE(snd_async_wait);
    GW_FRAME_EVENT_IF(xray::perf::CurrentThreadKind() != xray::perf::ThreadKind::Worker, IoWait); // 50n: the frame spike report
    while (m_src_state.load(std::memory_order_acquire) == snd_src_loading)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void CSoundRender_Source::unload()
{
    wait_pcm_task();
    if (u8* pcm = m_cached_pcm.exchange(nullptr, std::memory_order_acq_rel))
    {
        s_cached_pcm_bytes.fetch_sub(dwBytesTotal, std::memory_order_relaxed);
        xr_free(pcm);
        snd_pcm_cached_kb_gauge();
    }
    m_pcm_plays.store(0, std::memory_order_relaxed);
    m_pcm_state.store(snd_pcm_state_none, std::memory_order_relaxed);
    fTimeTotal = 0.0f;
    dwBytesTotal = 0;
}
