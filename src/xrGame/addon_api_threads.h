#pragma once

// Plugin API, group threads (addon_api_threads.cpp): task_parallel_for over the task scheduler of the engine.

#include "xrAddonHost/include/gwp/gwp_api.h"

namespace gw::addons::threads
{
void FillEngineApi(GwpEngineApi& api);
// A task_parallel_for piece runs on this thread now: the main-thread checks refuse it (IsMainThread)
bool InTaskPiece();
} // namespace gw::addons::threads
