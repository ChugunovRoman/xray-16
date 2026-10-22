#pragma once

// 50z1 (plans/optimization_mt/50z1-startup-markers-fs-index.md): the startup timeline, in every build (Release
// without XR_PERF_METRICS and without Tracy too). Each mark prints one log line
//   * [startup] <stage> X ms (since start T ms), files N, MB M
// X - time since the previous mark, T - wall time since the process started, N / M - files opened through FS
// (r_open / rs_open) and their size in MB since the previous mark (all threads). The cost is one log line per mark
// and two relaxed atomic adds per opened file, so the marks stay on: a slow PC's log shows where the startup goes.

namespace xray
{
// Prints the line above. stage is not stored (POD argument, safe from any module).
XRCORE_API void StartupMark(pcstr stage);
// The same, only for the first call with this stage (for code that runs every frame or more than once).
XRCORE_API void StartupMarkOnce(pcstr stage);
// Milliseconds since the process started (Windows: since the creation time of the process; other platforms: since
// xrCore was loaded, which is the process start up to the dynamic loader).
XRCORE_API double StartupElapsedMs();

namespace startup_detail
{
// xrCore-internal (not exported): CLocatorAPI counts every opened file here.
void NoteFileOpen(size_t bytes) noexcept;
} // namespace startup_detail
} // namespace xray
