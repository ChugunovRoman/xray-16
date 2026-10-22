#pragma once

// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R6, R7, R19): one background I/O queue on a slow disk.
//
// The in-game readers ahead - the "Model prefetch" thread of the render (ModelPool.cpp, model files of objects near the
// online distance and of the spawns the budget left in the queue) and the "Sound prefetch" thread (xrSound, the .ogg
// folders scripts pick sounds from) - each have a queue and a thread of their own. On an SSD / NVMe they read at the
// same time, as before. On a slow disk (an HDD, USB, a network drive) two readers at once make the head seek between
// them and both get slower; so there they share one slot: one file read at a time, the model jobs first (the spawns
// wait for them, R6), a sound file only when no model job is pending.
//
// Slow = the hw profile's policy (io_single_queue: the disk of gamedata is an HDD / USB / network one) or the
// measurement of the start-up warm-up of the model prefetch (ModelPool.cpp, WarmDiskProbe: the profile did not
// recognise the disk, e.g. under Wine or behind a USB bridge), reported here by the render.
//
// DLL boundary: plain functions and PODs only; the mutex and the condition variable stay inside xrCore.

#include "xrCore/xrCore.h"

namespace xray::io
{
enum class BackgroundIoClient : u8
{
    Model, // the "Model prefetch" thread: takes the slot as soon as the current read is over
    Sound, // the "Sound prefetch" thread: also waits while model jobs or texture jobs are pending
    // 50r-slow-disk-async-textures.md: the "Texture async" thread of the render (textures created in the game or bound
    // with a placeholder, the warm-up of IRender::texture_prefetch_files) - waits while model jobs are pending
    Texture,
    // 50r: a speculative warm-up (the "Model early warm" thread in the menu) - waits while texture jobs are pending,
    // the textures are on the screen now. 50z3: and while the start-up / the menu read in the foreground
    Warm,
};

// One queue for the background readers (a slow disk). Any thread, cheap (two atomic loads).
XRCORE_API bool BackgroundIoSingle();
// The start-up warm-up measured the disk of gamedata as slow (the render, once per process)
XRCORE_API void BackgroundIoNoteMeasuredSlow();
XRCORE_API bool BackgroundIoMeasuredSlow();

// The model jobs not read yet: queued plus the one being read. The "Model prefetch" side reports every change (under
// its own queue lock, so the values arrive in order); 0 lets the sound reader go on.
XRCORE_API void BackgroundIoSetModelJobs(u32 jobs);
// 50r-slow-disk-async-textures.md: the same for the "Texture async" thread (queued textures and warm-up files plus the
// one being read); 0 lets the sound reader and the menu warm-up go on.
XRCORE_API void BackgroundIoSetTextureJobs(u32 jobs);

// Takes the slot, waiting at most wait_ms for it (Model: until the current read is over; Sound: also until no model
// job and no texture job is pending; Texture: until no model job is pending; Warm: until no texture job is pending).
// 50z3-startup-slow-disk-io.md: Warm always, and Sound in the menu phase, also wait until no foreground read was noted
// for kForegroundQuietMs (they give way to the reads of the start-up and of the menu itself).
// false = not taken in time, the caller checks its own stop flag and asks again. A taken slot is given
// back with BackgroundIoRelease (on the same thread). Only meant for BackgroundIoSingle() - takes the slot regardless.
XRCORE_API bool BackgroundIoAcquire(BackgroundIoClient who, u32 wait_ms);
XRCORE_API void BackgroundIoRelease();

// ---------------------------------------------------------------------------------------------------------------------
// 50z3-startup-slow-disk-io.md (plans/optimization_mt): the start-up and the main menu on a slow disk. The readers ahead
// of the menu (the "Model early warm" thread, the deferred sounds of the start-up, the smart terrain map) only read while
// the start-up and the menu do not: the slot above serialises the background readers only, the foreground ones (main,
// the task workers) are noted here by FS and the speculative clients wait for them to stop.

// How long no foreground read must have been noted before a Warm (and, in the menu phase, a Sound) reader goes on
constexpr u32 kForegroundQuietMs = 500;

// The menu phase: from the process start to the first level load (IGame_Persistent ends it at KERNEL:start). Any
// thread.
XRCORE_API bool BackgroundIoMenuPhase();
XRCORE_API void BackgroundIoEndMenuPhase();

// FS calls this for every file it opens. A thread holding the slot is a background reader - not noted; otherwise, on a
// slow disk in the menu phase, the time is kept as the last foreground read. Cheap (a thread-local and atomics).
XRCORE_API void BackgroundIoNoteForegroundRead();
// Milliseconds since the last foreground read; u32(-1) when none was noted (a fast disk, out of the menu phase)
XRCORE_API u32 BackgroundIoForegroundIdleMs();

// The main menu has settled (IGame_Persistent: it is shown, its own reads stopped - or the time limit), once per
// process. The deferred work of the start-up (the early warm-up, the smart terrain map) starts at it. Any thread.
XRCORE_API void BackgroundIoNoteMenuSettled();
XRCORE_API bool BackgroundIoMenuSettled();

// The early warm-up of the first level load (50k) is still to come or reading: armed at the start-up, cleared when
// its thread ends or the level load / the exit stops it. For a menu reader that would rather go after it (e.g. a
// warm-up of the menu's own cache). Any thread.
XRCORE_API void BackgroundIoSetMenuWarmPending(bool pending);
XRCORE_API bool BackgroundIoMenuWarmPending();

// The slot for one read on a reader thread: enter() takes it in steps of kStepMs while quit() is false and the queue is
// single; on a fast disk it takes nothing and returns true at once. The destructor gives a taken slot back.
class BackgroundIoScope
{
public:
    static constexpr u32 kStepMs = 50;

    BackgroundIoScope() = default;
    ~BackgroundIoScope() { leave(); }
    BackgroundIoScope(const BackgroundIoScope&) = delete;
    BackgroundIoScope& operator=(const BackgroundIoScope&) = delete;

    // false = quit() became true while waiting (nothing taken)
    template <typename Quit>
    bool enter(BackgroundIoClient who, Quit&& quit)
    {
        leave();
        if (!BackgroundIoSingle())
            return true;
        while (!BackgroundIoAcquire(who, kStepMs))
        {
            if (quit())
                return false;
        }
        m_taken = true;
        return true;
    }

    void leave()
    {
        if (!m_taken)
            return;
        m_taken = false;
        BackgroundIoRelease();
    }

    [[nodiscard]] bool taken() const { return m_taken; }

private:
    bool m_taken{};
};
} // namespace xray::io
