#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"

#include <vorbis/vorbisfile.h>

#include <atomic>
#include <cstring>

// B-1: global accounting of bytes held in per-source PCM caches, bounded by psSoundCacheSizeMB.
// Touched only from load/unload (effectively main thread), atomic for safety.
static std::atomic<size_t> s_cached_pcm_bytes{0};

CSoundRender_Source::~CSoundRender_Source() { unload(); }

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
    ZoneScoped;

    // B-1: short SFX are fully decoded once into m_cached_pcm (shared across all emitters of this source).
    // Serve the requested slice with a plain memcpy instead of re-running the OGG decoder per emitter/chunk.
    if (!m_cached_pcm.empty())
    {
        ZoneScopedN("decompress_cached"); // diagnostic: count = cache hits (vs decompress count = total)
        auto* const d = static_cast<u8*>(dest);
        const u32 total = static_cast<u32>(m_cached_pcm.size());
        u32 copied = 0;
        if (byte_offset < total)
        {
            copied = std::min(size, total - byte_offset);
            std::memcpy(d, m_cached_pcm.data() + byte_offset, copied);
        }
        if (copied < size) // past end of sound -> silence
            std::memset(d + copied, 0, size - copied);
        return;
    }

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

void CSoundRender_Source::i_decompress(OggVorbis_File* ovf, char* _dest, u32 size) const
{
    long TotalRet = 0;

    // Read loop
    while (TotalRet < static_cast<long>(size))
    {
        const auto ret = ov_read(ovf, _dest + TotalRet, size - TotalRet, 0, 2, 1, nullptr);
        if (ret <= 0 && !ov_can_continue_read(ret))
            break;
        TotalRet += ret;
    }
}

void CSoundRender_Source::i_decompress(OggVorbis_File* ovf, float* _dest, u32 size) const
{
    s32 left = s32(size / m_data_info.blockAlign);
    while (left)
    {
        float** pcm;
        long samples = ov_read_float(ovf, &pcm, left, nullptr);

        if (samples <= 0 && !ov_can_continue_read(samples))
            break;

        if (samples > left)
            samples = left;

        for (long j = 0; j < samples; j++)
            for (long i = 0; i < m_data_info.channels; i++)
                *_dest++ = clampr(pcm[i][j], -1.0f, 1.0f);

        left -= samples;
    }
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

void CSoundRender_Source::cache_pcm_if_short()
{
    if (!should_cache_pcm())
        return;

    // Stay within the global PCM cache budget (psSoundCacheSizeMB). Over budget -> keep streaming this one.
    const size_t budget = size_t(psSoundCacheSizeMB) * 1024u * 1024u;
    if (s_cached_pcm_bytes.load(std::memory_order_relaxed) + dwBytesTotal > budget)
        return;

    OggVorbis_File* ovf = open();
    if (!ovf)
        return;

    m_cached_pcm.resize(dwBytesTotal);
    // decode the whole sound from the start (open() positions at 0) into the shared buffer
    if (m_data_info.format == SoundFormat::Float32)
        i_decompress(ovf, reinterpret_cast<float*>(m_cached_pcm.data()), dwBytesTotal);
    else
        i_decompress(ovf, reinterpret_cast<char*>(m_cached_pcm.data()), dwBytesTotal);
    close(ovf);

    s_cached_pcm_bytes.fetch_add(dwBytesTotal, std::memory_order_relaxed);
}

bool CSoundRender_Source::LoadWave(pcstr pName)
{
    ZoneScoped;

    pname = pName;

    // Load file into memory and parse WAV-format
    OggVorbis_File ovf;
    {
        IReader* wave = FS.r_open(pname.c_str());
        R_ASSERT3(wave && wave->length(), "Can't open wave file:", pname.c_str());
        ov_open_callbacks(wave, &ovf, nullptr, 0, g_ov_callbacks);
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
    string_path fn, N;
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

    if (FS.exist(fn))
    {
        if (LoadWave(fn))
        {
            cache_pcm_if_short(); // B-1: decode-once short SFX into shared PCM (main thread, no race)
            return true;
        }
    }

    return false;
}

void CSoundRender_Source::unload()
{
    if (!m_cached_pcm.empty())
    {
        s_cached_pcm_bytes.fetch_sub(m_cached_pcm.size(), std::memory_order_relaxed);
        m_cached_pcm.clear();
        m_cached_pcm.shrink_to_fit();
    }
    fTimeTotal = 0.0f;
    dwBytesTotal = 0;
}
