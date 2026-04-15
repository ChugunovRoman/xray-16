#include "pch.hpp"

#include "xrAICore/Navigation/ai_graph_engine_cvars.h"

#include "xrAICore/Navigation/ai_graph_engine_epoch.h"



XRAICORE_API int ps_ai_graph_engine_serialize = 0;

XRAICORE_API int ps_ai_path_build_use_tls_scratch = 1;
XRAICORE_API int ps_ai_graph_engine_detect_concurrent = 0;

XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_active_index_search{};
XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_active_solver_search{};
XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_active_string_search{};
XRAICORE_API std::atomic<std::uint32_t> g_ai_graph_engine_concurrent_report_budget{64};

std::atomic<std::uint32_t> g_ai_graph_engine_epoch{};


