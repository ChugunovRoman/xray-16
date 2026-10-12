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
// Waits for the kicked frame; returns false (no wait) when nothing is in flight. Idempotent.
bool JoinGameLogicThread();
bool IsGameLogicThreadInFlight();
// Joins and stops the dedicated thread (CRenderDevice::Destroy). Safe to call when it was never started.
void ShutdownGameLogicThread();
} // namespace XRay::Engine
