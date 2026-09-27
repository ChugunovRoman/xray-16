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
// Records the current thread as the main (game loop) thread of IsGameLogicThread(). CApplication calls it from its
// constructor and from every frame: the loop runs on the "Primary thread", not on the thread that loaded the module.
ENGINE_API void MarkMainThread();
} // namespace XRay::Engine
