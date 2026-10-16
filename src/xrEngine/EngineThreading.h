#pragma once

#include "Engine.h"

namespace XRay::Engine
{
void PreRenderThread();
void GameThread();

// True on the thread that runs the game logic right now: the main thread, or the worker that executes the GameThread
// task (Sheduler.Update, Lua binders, ALife) while the main thread waits for it. The task scheduler lets any worker
// steal that task, so "the main thread" alone is the wrong test for code that must run with the game logic.
ENGINE_API bool IsGameLogicThread();
// True while XRay::Engine::GameThread() runs (on whichever thread took the task)
ENGINE_API bool IsGameThreadRunning();
// Records the current thread as the main (game loop) thread of IsGameLogicThread(). CApplication calls it from its
// constructor and from every frame: the loop runs on the "Primary thread", not on the thread that loaded the module.
ENGINE_API void MarkMainThread();

// GAME_RENDER_OVERLAP_PLAN P1, variant B (game/render overlap frames, see CRenderDevice::ProcessFrame).
// Main thread only. The frame's game logic is split like MX f76479197: the real-time scheduler part (the actor)
// runs on the main thread (GameThreadRT), then a dedicated OS thread "Game" (not a TaskManager task: a Wait() on
// the main thread inside the render must not be able to run the whole game tick nested) runs SoundEvent_Dispatch
// -> non-RT scheduler -> vision batch -> seqParallel -> seqFrameMT, in parallel with DoRender() and with the
// physics step the main thread kicked right before the wake (plans/optimization_mt/09-physics-earlier-parallel.md).
// From KickGameLogicThread() until JoinGameLogicThread() that thread is the game logic thread for
// IsGameLogicThread()/IsGameThreadRunning(); the main thread is not.
void GameThreadRT();
void KickGameLogicThread(bool run_scheduler);
// plans/optimization_mt/26-framemove-game-next.md, K3 / K2b (package 2-A): phase 0 of the Game thread. Main thread
// only, in a game/render overlap frame, after the last Lua of main before the kick (phase A of the bones prepass,
// CLevel::BonesPrepass) and before KickGameLogicThread() of the same frame (which must follow). Wakes the dedicated
// thread early: it runs SoundEvent_Dispatch (sound_dispatch) and then work (may be null) while main still runs phase B
// of the prepass, phase C and the physics kick - none of them runs Lua. The thread is NOT the game logic thread for
// IsGameLogicThread()/IsGameThreadRunning() until the kick (main still is: the overlap deferrals keyed on them do not
// apply before the render starts); the overlap hazard detector (GW_OVERLAP_HAZARD) covers it from here. The body
// started by the kick runs after phase 0 on the same thread and skips the dispatch if phase 0 did it.
ENGINE_API void KickGameLogicEarly(bool sound_dispatch, void (*work)());
// K9 (26-framemove-game-next.md): 1 = GEnv.Sound->render() of a game/render overlap frame runs as a TaskManager task
// started with seqParallel on the Game thread and waited for before seqFrameMT (instead of inside seqFrameMT).
// Temporary, not saved (xrGame console_commands.cpp, mt_sound_render_task); 0 = as before.
ENGINE_API extern int g_mt_sound_render_task;
// Engine.cpp, SoundRenderer: true while the Game thread's task renders the sound of this frame (seqFrameMT skips it)
bool SoundRenderTakenByTask();
// Waits for the kicked frame; returns false (no wait) when nothing is in flight. Idempotent.
bool JoinGameLogicThread();
bool IsGameLogicThreadInFlight();
// Joins and stops the dedicated thread (CRenderDevice::Destroy). Safe to call when it was never started.
void ShutdownGameLogicThread();
} // namespace XRay::Engine
