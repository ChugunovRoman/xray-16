#pragma once

#include <atomic>

struct OggVorbis_File;

enum class SoundFormat : u8
{
    Unknown,
    PCM,
    Float32,
};

struct SoundDataInfo
{
    SoundFormat format{};
    u16         channels{};       // number of channels (i.e. mono, stereo...)
    u32         samplesPerSec{};  // sample rate
    u32         avgBytesPerSec{}; // for buffer estimation
    u16         blockAlign{};     // block size of data
    u16         bitsPerSample{};  // number of bits per sample of mono data
    u32         bytesPerBuffer{}; // target buffer size
};

struct SoundSourceInfo
{
    float baseVolume{ 1.0f };
    float minDist   { 1.0f };
    float maxDist   { 300.0f };
    float maxAIDist { 300.0f };
    u32   gameType  {};
};

class XRSOUND_API CSoundRender_Source final : public CSound_source
{
    shared_str pname;
    shared_str fname;

    float fTimeTotal{};
    u32 dwBytesTotal{};

    SoundDataInfo m_data_info{};
    SoundSourceInfo m_info{};

    // B-1: fully decoded PCM (dwBytesTotal bytes, xr_alloc), shared by every emitter of this source; null => stream /
    // decode per chunk. 46-engine-core-render-audit.md: filled on demand, not at load - a short sound
    // (psSoundCacheShortSec) that starts playing the second time is decoded whole by a TaskScheduler task
    // (request_pcm_cache) and published here (release). Then it stays until unload(), so decompress() reads it
    // lock-free. Before: decoded at load, first loaded - first cached: the budget (snd_cache_size) was spent by the
    // sounds of the startup prefetch, and the sounds heard in a fight were streamed (decoded per emitter, per chunk).
    mutable std::atomic<u8*> m_cached_pcm{nullptr};
    mutable std::atomic<u32> m_pcm_plays{0}; // starts of play (decompress at offset 0) while not cached
    mutable std::atomic<u8> m_pcm_state{0};  // snd_pcm_state_* (SoundRender_Source.cpp)

private:
    // true = all `size` bytes were decoded (false: the stream ended or broke first)
    bool i_decompress(OggVorbis_File* ovf, char* dest, u32 size) const;
    bool i_decompress(OggVorbis_File* ovf, float* dest, u32 size) const; // this overload clamps denormalized sounds

    bool LoadWave(pcstr name);
    bool should_cache_pcm() const; // B-1: short enough to fully cache (and caching enabled)?
    // 46-engine-core-render-audit.md: a start of play of a streamed sound - from the second one on, reserve the budget
    // (snd_cache_size) and queue the whole decode (cache_pcm_step)
    void request_pcm_cache() const;
    struct PcmJob;
    // the whole decode into m_cached_pcm by short TaskScheduler tasks (a thread waiting in TaskManager may run one)
    void cache_pcm_step(PcmJob* job) const;
    void wait_pcm_task() const;  // unload(): no decode task may still write this source

public:
    CSoundRender_Source() noexcept = default;
    ~CSoundRender_Source() override;

    CSoundRender_Source(const CSoundRender_Source&) = delete;
    // moved only before the source is registered (i_create_source): nothing plays it yet
    CSoundRender_Source(CSoundRender_Source&& other) noexcept;

    CSoundRender_Source& operator=(const CSoundRender_Source&) = delete;
    CSoundRender_Source& operator=(CSoundRender_Source&&) = delete;

    bool load(pcstr name);
    void unload();

    OggVorbis_File* open() const;
    void close(OggVorbis_File*& ovf) const;

    void decompress(void* dest, u32 byte_offset, u32 size, OggVorbis_File* ovf) const;

    // B-1: the whole sound is decoded in m_cached_pcm, decompress() never touches the ogg file (ovf may be null).
    // Once true, stays true until unload().
    [[nodiscard]] bool has_cached_pcm() const { return m_cached_pcm.load(std::memory_order_acquire) != nullptr; }

    [[nodiscard]] const auto& data_info() const { return m_data_info; }
    [[nodiscard]] const auto&      info() const { return m_info; }

    [[nodiscard]] pcstr file_name() const override { return fname.c_str(); }
    // the file the source was loaded from (FS path with the extension), 34-sound-cold-load.md
    [[nodiscard]] pcstr path_name() const { return pname.c_str(); }

    [[nodiscard]] float length_sec() const override { return fTimeTotal; }
    [[nodiscard]] u32 bytes_total() const override { return dwBytesTotal; }

    [[nodiscard]] u16 channels_num() const override { return data_info().channels; }
    [[nodiscard]] u32 game_type() const override { return info().gameType; }
};
