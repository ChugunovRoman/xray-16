#pragma once

#include <atomic>
#include <cstdint>

/** Defined in `ai_graph_engine_cvars.cpp`; not exported from xrAICore (same-DLL use only). */
extern std::atomic<std::uint32_t> g_ai_graph_engine_epoch;
