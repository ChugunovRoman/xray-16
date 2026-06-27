////////////////////////////////////////////////////////////////////////////
//	Module 		: script_property_evaluator_wrapper.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script property evaluator wrapper
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "property_evaluator.h"

class CScriptGameObject;

typedef CPropertyEvaluator<CScriptGameObject> CScriptPropertyEvaluator;

enum class EScriptEvaluatorCachePolicy : u8
{
    NeverCache = 0,
    CachePerFrame,     // для быстро меняющихся (danger, combat)
    CacheFor10Frames,  // для умеренно стабильных (kill_wounded, dont_shoot) — ~330ms @ 30fps
    CacheFor30Frames,  // для редко меняющихся (campfire, npc_vs_box) — ~1sec @ 30fps
    CachePerSolve,     // default: на время одного solve (actual+search); мир заморожен → безопасно
    CacheForTTL        // B-1: time-based cache via ai_evaluator_ttl_ms
};

class CScriptPropertyEvaluatorWrapper : public CScriptPropertyEvaluator, public luabind::wrap_base
{
public:
    IC CScriptPropertyEvaluatorWrapper(CScriptGameObject* object = 0, LPCSTR evaluator_name = "");
    virtual void setup(CScriptGameObject* object, CPropertyStorage* storage);
    static void setup_static(CScriptPropertyEvaluator* evaluator, CScriptGameObject* object, CPropertyStorage* storage);
    virtual bool evaluate();
    static bool evaluate_static(CScriptPropertyEvaluator* evaluator);

private:
    EScriptEvaluatorCachePolicy cache_policy() const;

private:
    mutable u32 m_cached_frame = u32(-1);
    mutable u32 m_cached_epoch = u32(-1); // for CachePerSolve (g_ai_evaluator_solve_epoch)
    mutable u32 m_cached_time_ms = 0;     // for CacheForTTL (Device.dwTimeGlobal)
    mutable bool m_cached_value = false;
    mutable bool m_has_cached_value = false;
    mutable EScriptEvaluatorCachePolicy m_cache_policy = EScriptEvaluatorCachePolicy::NeverCache;
    mutable bool m_cache_policy_initialized = false;
};

#include "script_property_evaluator_wrapper_inline.h"
