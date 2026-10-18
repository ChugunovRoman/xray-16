#pragma once

#ifdef XRAY_STATIC_BUILD
#    define ENGINE_API
#else
#    ifdef ENGINE_BUILD
#        define ENGINE_API XR_EXPORT
#    else
#        define ENGINE_API XR_IMPORT
#    endif
#endif

#include "pure.h"
#include "EngineAPI.h"
#include "EventAPI.h"
#include "xrSheduler.h"
#include "xrSound/Sound.h"

// TODO: this should be in render configuration
#define R__NUM_SUN_CASCADES         (3u) // csm/s.ligts
// 27-render-main.md, R4: 5 -> 9 - the parallel main submit (r__main_parallel_parts, up to 11 deferred units) and the
// spot light shadow batches (as many lights per batch as free contexts) share the pool with the cascades, rain and the
// scope pass. Changes vis_data (marker[R__NUM_CONTEXTS]): every module including this header must be rebuilt.
#define R__NUM_AUX_CONTEXTS         (9u) // rain/s.lights + svp scope pass + parallel main submit
#define R__NUM_PARALLEL_CONTEXTS    (R__NUM_SUN_CASCADES + R__NUM_AUX_CONTEXTS)
#define R__NUM_CONTEXTS             (R__NUM_PARALLEL_CONTEXTS + 1/* imm */)

class ENGINE_API CEngine final : public pureFrame, public IEventReceiver
{
    EVENT eQuit;

public:
    // DLL api stuff
    CEngineAPI External;
    CEventAPI Event;
    CSheduler Sheduler;
    CSoundManager Sound;

    void Initialize(GameModule* game, const std::array<RendererModule*, 2>& modules);
    void Destroy();

    void OnEvent(EVENT E, u64 P1, u64 P2) override;
    void OnFrame() override;

    CEngine();
    ~CEngine();
};

ENGINE_API extern CEngine Engine;
