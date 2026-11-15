#pragma once

// Plugin API, group threads (addon_api_threads.cpp): task_parallel_for over the task scheduler of the engine.

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::threads
{
void FillEngineApi(GwpEngineApi& api);
// A task_parallel_for piece runs on this thread now: the main-thread checks refuse it (IsMainThread)
bool InTaskPiece();

// task_async (plans/addon_api_ideas, group plugin, P46), host side (addon_host.cpp):
void OnFrame();                                  // the done of the ended works, on the main thread
void CancelPluginTasks(const GwpPlugin* plugin); // StopPlugin: the queued works and the pending done go
void WaitPluginTasks(const GwpPlugin* plugin);   // ReleasePlugin: the running works of the plugin end first
void Shutdown();                                 // the threads stop (after every plugin is unloaded)
} // namespace gw::addons::threads
