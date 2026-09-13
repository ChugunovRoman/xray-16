#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class smapvis : public R_feedback
{
public:
    enum
    {
        state_counting = 0,
        state_working = 1,
        state_usingTC = 3,
    } state{ state_counting };
    xr_vector<dxRender_Visual*> invisible;

    // 'no query issued' marker for testQ_id; R_occlusion treats it as iInvalidHandle (no-op).
    static constexpr u32 no_query = 0xFFFFFFFF;

    u32 frame_sleep{};
    u32 test_count{};
    u32 test_current{};
    dxRender_Visual* testQ_V{};  // visual under test; non-null == a query is outstanding
    u32 testQ_id{ no_query };
    u32 testQ_frame{};
    int id{-1};

public:
    smapvis();
    ~smapvis();

    void invalidate();
    void begin(); // should be called before 'marker++' and before graph-build
    void end();
    void mark();
    void flushoccq(); // should be called when no rendering of light is supposed
    void release();   // abandon an outstanding test: return its query slot without reading it

    void resetoccq();

    IC bool sleep() { return Device.dwFrame > frame_sleep; }
    virtual void rfeedback_static(dxRender_Visual* V) override;
};
} // namespace xray::render::RENDER_NAMESPACE
