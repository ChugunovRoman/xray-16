#pragma once

#include "SoundRender_Target.h"
#include "SoundRender_CoreA.h"

class CSoundRender_TargetA : public CSoundRender_Target
{
    using inherited = CSoundRender_Target;

    // OpenAL
    ALuint pSource{};
    ALuint pBuffers[sdef_target_count_submit]{};
    ALuint dataFormat;
    ALsizei sampleRate;

    float cache_gain{};
    float cache_pitch{ 1.0f };

    // 46-engine-core-render-audit.md: the last 3D parameters sent to the AL source - fill_parameters() sets one only when
    // it changes (exact compare, NaN always resent). Invalid after _initialize() (a new AL source) and stop() (it sets
    // AL_SOURCE_RELATIVE itself): the next fill sends all of them, as before.
    bool cache_params_valid{};
    float cache_ref_dist{};
    float cache_max_dist{};
    float cache_rolloff{};
    ALint cache_relative{};
    Fvector cache_pos{};

    void submit_buffer(ALuint BufferID) const;
    void submit_all_buffers() const;

public:
    CSoundRender_TargetA();

    bool _initialize() override;
    void _destroy() override;
    void _restart() override;

    void start(CSoundRender_Emitter* E) override;
    void render() override;
    void rewind() override;
    void stop() override;
    void update() override;
    void fill_parameters() override;
};
