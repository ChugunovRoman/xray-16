#include "stdafx.h"
#include "Layers/xrRender/light.h"
#include "Layers/xrRender/FBasicVisual.h"

namespace xray::render::RENDER_NAMESPACE
{
smapvis::smapvis()
{
    invalidate();
    frame_sleep = 0;
}
smapvis::~smapvis()
{
    // No flushoccq() here: the visibility result of a dying light is worthless, and fetching it
    // would block in GetData if the command list carrying the query was never executed.
    release();
    invalidate();
}
void smapvis::release()
{
    // Every path that forgets testQ_V/testQ_id without reading the result used to leak the
    // R_occlusion slot forever (moving shadowed lights alone leaked ~0.06 slots per frame).
    // Pure bookkeeping, no D3D call - callable from the SMAP worker tasks too.
    if (testQ_V)
        RImplementation.occq_free(testQ_id);
    testQ_V = 0;
    testQ_id = no_query;
}
void smapvis::invalidate()
{
    // light::spatial_move() invalidates on every noticeable move; the query issued last frame
    // is not flushed yet at that point (flush runs inside Render()) - return it instead.
    release();
    state = state_counting;
    frame_sleep = Device.dwFrame + ps_r__LightSleepFrames;
    invisible.clear();
}
void smapvis::begin()
{
    auto& dsgraph = RImplementation.get_context(id);
    dsgraph.clear_Counters();
    switch (state)
    {
    case state_counting:
        // do nothing -> we just prepare for testing process
        break;
    case state_working:
        // mark already known to be invisible visuals, set breakpoint.
        // A test still outstanding here was missed by flushoccq (frames without Render():
        // menu, save) - return its slot instead of forgetting it.
        release();
        mark();
        dsgraph.set_Feedback(this, test_current);
        break;
    case state_usingTC:
        // just mark
        mark();
        break;
    }
}
void smapvis::end()
{
    auto& dsgraph = RImplementation.get_context(id);

    // Gather stats
    u32 ts, td;
    dsgraph.get_Counters(ts, td);
    RImplementation.Stats.ic_total += ts;
    dsgraph.set_Feedback(0, 0);

    switch (state)
    {
    case state_counting:
        // switch to 'working'
        if (sleep())
        {
            test_count = ts;
            test_current = 0;
            state = state_working;
        }
        break;
    case state_working:
        // feedback should be called at this time -> clear feedback
        // issue query
        if (testQ_V)
        {
            RImplementation.occq_begin(testQ_id, dsgraph.cmd_list.context_id);
            dsgraph.marker += 1;
            dsgraph.insert_static(testQ_V);
            dsgraph.render_graph(0);
            RImplementation.occq_end(testQ_id, dsgraph.cmd_list.context_id);
            testQ_frame = Device.dwFrame + 1; // get result on next frame
        }
        break;
    case state_usingTC:
        // nothing to do
        break;
    }
}

void smapvis::flushoccq()
{
    // the tough part
    // Not yet: the query was issued this frame (testQ_frame = dwFrame + 1). Older ones (frames
    // skipped without Render()) are fetched, not dropped - the result is long ready by then.
    if (testQ_frame > Device.dwFrame)
        return;
    if ((state != state_working) || (!testQ_V))
        return;
    if (testQ_id == no_query)
    {
        testQ_V = 0; // feedback set a visual but no query was issued - nothing to read
        return;
    }
    const auto fragments = RImplementation.occq_get(testQ_id);
    if (0 == fragments)
    {
        // this is invisible shadow-caster, register it
        // next time we will not get this caster, so 'test_current' remains the same
        invisible.push_back(testQ_V);
        test_count--;
    }
    else
    {
        // this is visible shadow-caster, advance testing
        test_current++;
    }

    // occq_get already recycled the slot (it zeroes the id). Mark it 'none' so a later
    // release() can never hand slot 0 - a valid id belonging to somebody else - to occq_free.
    testQ_V = 0;
    testQ_id = no_query;

    if (test_current == test_count)
    {
        // we are at the end of list
        if (state == state_working)
            state = state_usingTC;
    }
}
void smapvis::resetoccq()
{
    if (testQ_frame == (Device.dwFrame + 1))
        testQ_frame--;
    flushoccq();
}

void smapvis::mark()
{
    auto& dsgraph = RImplementation.get_context(id);
    RImplementation.Stats.ic_culled += invisible.size();
    u32 marker = dsgraph.marker + 1; // we are called befor marker increment
    for (u32 it = 0; it < invisible.size(); it++)
        invisible[it]->vis.marker[id] = marker; // this effectively disables processing
}

void smapvis::rfeedback_static(dxRender_Visual* V)
{
    testQ_V = V;
    auto& dsgraph = RImplementation.get_context(id);
    dsgraph.set_Feedback(0, 0);
}
} // namespace xray::render::RENDER_NAMESPACE
