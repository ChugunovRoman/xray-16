#pragma once

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

    // B-1: fully decoded PCM, shared by every emitter of this source. Populated once at load for short SFX
    // (see psSoundCacheShortSec); empty => stream/decode per chunk as before. Immutable after load, so
    // decompress() reads it lock-free. Movable (keeps the defaulted move ctor valid).
    xr_vector<u8> m_cached_pcm;

private:
    void i_decompress(OggVorbis_File* ovf, char* dest, u32 size) const;
    void i_decompress(OggVorbis_File* ovf, float* dest, u32 size) const; // this overload clamps denormalized sounds

    bool LoadWave(pcstr name);
    bool should_cache_pcm() const; // B-1: short enough to fully cache (and caching enabled)?
    void cache_pcm_if_short();     // B-1: decode whole sound into m_cached_pcm if short + within budget

public:
    CSoundRender_Source() noexcept = default;
    ~CSoundRender_Source() override;

    CSoundRender_Source(const CSoundRender_Source&) = delete;
    CSoundRender_Source(CSoundRender_Source&&) noexcept = default;

    CSoundRender_Source& operator=(const CSoundRender_Source&) = delete;
    CSoundRender_Source& operator=(CSoundRender_Source&&) noexcept = default;

    bool load(pcstr name);
    void unload();

    OggVorbis_File* open() const;
    void close(OggVorbis_File*& ovf) const;

    void decompress(void* dest, u32 byte_offset, u32 size, OggVorbis_File* ovf) const;

    [[nodiscard]] const auto& data_info() const { return m_data_info; }
    [[nodiscard]] const auto&      info() const { return m_info; }

    [[nodiscard]] pcstr file_name() const override { return fname.c_str(); }

    [[nodiscard]] float length_sec() const override { return fTimeTotal; }
    [[nodiscard]] u32 bytes_total() const override { return dwBytesTotal; }

    [[nodiscard]] u16 channels_num() const override { return data_info().channels; }
    [[nodiscard]] u32 game_type() const override { return info().gameType; }
};
