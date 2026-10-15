#pragma once

#include <atomic>
#include <cstdint>

#include "xrAICore/AISpaceBase.hpp"



/** When 1 (default), `CGraphEngine::search` serializes on the engine lock (safe baseline). When 0, searches may run in parallel — use only after validating callers (see plans/optimization_arch/SHARED_AI_MT_AUDIT.md). */

extern XRAICORE_API int ps_ai_graph_engine_serialize;



/** When 1, logs concurrent in-flight `CGraphEngine::search` calls per backend when serialization is disabled. */
extern XRAICORE_API int ps_ai_graph_engine_detect_concurrent;

/** When 1, logs every patrol point remap (point name, old/new vertex id). The first remap per
    session is logged unconditionally regardless of this switch. */
extern XRAICORE_API int ps_ai_patrol_remap_log;

extern XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_active_index_search;
extern XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_active_solver_search;
extern XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_active_string_search;
extern XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_concurrent_report_budget;

