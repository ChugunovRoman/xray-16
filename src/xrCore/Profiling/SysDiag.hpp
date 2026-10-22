#pragma once

#include "xrCore/xrCore.h"

// 50z5-extra-diagnostics.md (plans/optimization_mt): more always-on diagnostics for the test runs on weak laptops, without
// Tracy, a debugger or the metrics build (Release and Master Gold too). Next to the frame spike report and the window of
// 50n (PerfMetrics.hpp):
//   * [frame] <where>: pace ...   - the present-to-present interval, vsync / refresh / fps limit, VRAM (main thread)
//   * [io] <where>: ...           - the latency of the file reads of our readers (main thread)
//   * [sys] <where>: ...          - two lines from the "Diag" thread: the CPU frequency and its limit, the load of the
//                                   system vs our process, other processes eating the CPU, the disks, power, timer;
//                                   then the CPU of our threads and the busy share of the task pool
//   * [load] <stage> ...          - the level / save load timeline (marks like the [startup] ones of 50z1)
// and the frame / phase context in the crash report (memory_stats.txt). DLL boundary: plain functions and PODs only.

namespace xray::perf
{
// --- File reads ([io] line, [load] marks) -----------------------------------------------------------------------------
// CLocatorAPI::r_open / rs_open (LocatorAPI.cpp), once per opened file: the wall time of the open, which for a compressed
// archive entry is the read + decompress (the page faults of the mapped archive are taken there), for a small loose
// file (< 16 KB) the read itself, and for a mapped one (a loose file >= 16 KB, an uncompressed archive entry, a stream)
// only the mapping - its data comes later through page faults (the "pf / hf" of the [frame] lines). Any thread.
XRCORE_API void IoOpenNote(u64 ticks, u64 bytes, bool archive, bool compressed, bool stream) noexcept;
// The background readers of a slow disk (BackgroundIo.cpp): the time a reader held the single I/O slot (one file read
// of the "Model prefetch" / "Sound prefetch" / "Texture async" / warm-up threads). Nothing on a fast disk (no slot).
XRCORE_API void IoSlotHoldNote(u64 ticks) noexcept;

// --- Threads ([sys] threads line) -------------------------------------------------------------------------------------
// Threading::SetCurrentThreadName: the calling thread is kept (by name) for the per-thread CPU of the [sys] line.
XRCORE_API void ThreadDiagRegister(const char* name) noexcept;
// TaskManager's worker loop: one task taken from a queue and run by a pool worker (its wall time; stolen = taken from
// another worker's queue). Accumulated per thread, published in batches.
XRCORE_API void TaskPoolExecNote(u64 ticks, bool stolen) noexcept;

// --- VRAM and frame pacing ([frame] pace line) ------------------------------------------------------------------------
// The render's Present, every 32nd frame: the local (dedicated) video memory used by the process and its budget (DXGI:
// QueryVideoMemoryInfo; GL NVX: the dedicated memory and what is still available), the non-local (system) memory of
// the process (0 when unknown), the evictions of the driver so far (GL NVX; 0 elsewhere). Main thread.
XRCORE_API void VramSubmit(u64 local_usage, u64 local_budget, u64 nonlocal_usage, u32 evictions) noexcept;
// CRenderDevice::ProcessFrame, every frame: the vsync flag (rs_v_sync; D3D11 applies it in exclusive fullscreen only),
// the window mode (rsWindowed... of defines.h), the monitor index, the frame budget of the fps limiter in this frame
// (1000 / rs_fps_limit or the menu limit, whole ms) and the sleep the limiter asked for in this frame (0 = none).
XRCORE_API void FramePacingContext(bool vsync, u8 window_mode, u32 monitor, u32 frame_budget_ms, u32 sleep_ms) noexcept;

// --- Load timeline ----------------------------------------------------------------------------------------------------
// Starts a timeline: "* [load] begin <kind>" (a console load, a level change, a start). Inside an open one it is only a
// mark; an open timeline without marks for 60 s is closed first (an exit to the menu without a load after it).
// Every mark: "* [load] <stage> X ms (since begin T ms) | reads N, M MB, R ms | cpu P ms | pf F hf H", the stage that
// has just ended. The timeline ends at the first game frame after the load end. Any thread, POD arguments.
XRCORE_API void LoadMarkBegin(pcstr kind) noexcept;
XRCORE_API void LoadMark(pcstr stage) noexcept;

// --- Crash context ----------------------------------------------------------------------------------------------------
// The text for memory_stats.txt of the crash report: the frame number, the phase of the main thread and of the calling
// one, the current load stage, then the last [frame] / [frame-spike] / [io] lines. Writes into buf (no allocation, no
// lock), returns the length. Safe in a crash handler on any thread.
XRCORE_API size_t CrashContextFormat(char* buf, size_t size) noexcept;

namespace detail
{
// xrCore-internal (not exported)
// PerfMetrics.cpp -> SysDiag.cpp: a window line of 50n was printed (where = its label): the [frame] pace and [io] lines
// follow on main, the [sys] lines on the Diag thread. print = false: the window had no game frames (no 50n lines) - the
// pace and [io] windows start anew silently, so they cover the same frames as the [frame] lines.
void SysWindowReport(const char* where, bool print) noexcept;
// PerfMetrics.cpp: one game frame of the window: its wall time, its fps sleep phase (ms), the present-to-present
// interval when known (< 0 = not known).
void SysFrameTick(float wall_ms, float sleep_ms, float present_interval_ms) noexcept;
// PerfMetrics.cpp: the session marks of 50n reach the load timeline (Exit = a begin, LoadEnd = a mark) and the first
// game frame after a load end closes it.
void LoadSessionMark(u8 mark) noexcept;
void LoadFirstGameFrame() noexcept;
// The CPU frequency now (Windows: CallNtPowerInformation, the average of the logical CPUs; Linux: cpufreq of CPU 0):
// "cpu 2100/2800MHz" (+ " lim 1600" when the limit is below the maximum); "" when not known. For the spike line.
void CpuFreqNow(char* buf, size_t size) noexcept;
// SysDiag.cpp -> PerfMetrics.cpp: the process I/O and page faults (the 50n IoQuery: read ops / bytes, faults, the
// system hard faults on Windows, the major faults elsewhere) and the thread cycle -> Ticks() scale.
void ProcessIoSnapshot(u64& read_ops, u64& read_bytes, u64& faults, u64& hard) noexcept;
double ThreadCyclesScale() noexcept;
// PerfMetrics.cpp: a report line is kept for the crash context (main thread only)
void CrashRingPush(const char* line) noexcept;
// SysDiag.cpp: the stage of the open load timeline ("" when none), for the crash context
const char* LoadStageNow() noexcept;
} // namespace detail
} // namespace xray::perf
