#include "stdafx.h"

#include "xrEngine/x_ray.h"
#include "xrGame/xrGame.h"
#include "Include/xrRender/xrRender.h"
#include "xrCore/Debug/xrSentry.hpp"

#if defined(XR_PLATFORM_WINDOWS)
#include <Windows.h>
#endif

#if !defined(XR_PLATFORM_WINDOWS)
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#endif

// Always request high performance GPU
extern "C"
{
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
XR_EXPORT u32 NvOptimusEnablement = 0x00000001; // NVIDIA Optimus

// https://gpuopen.com/amdpowerxpressrequesthighperformance/
XR_EXPORT u32 AmdPowerXpressRequestHighPerformance = 0x00000001; // PowerXpress or Hybrid Graphics
}

std::array<RendererModule*, 2> s_render_modules =
{
#ifdef XR_PLATFORM_WINDOWS
    xray::render::render_r4::GetRendererModule(),
#endif
    xray::render::render_gl::GetRendererModule(),
};

struct tracy_raii
{
    ~tracy_raii()
    {
#ifdef TRACY_ENABLE
        tracy::GetProfiler().RequestShutdown();
        while (!tracy::GetProfiler().HasShutdownFinished())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
#endif
    }
};

static int RunGameImpl(pcstr commandLine)
{
    tracy_raii raii;
    auto* game = strstr(commandLine, "-nogame") ? nullptr : &xrGame;
    CApplication app{ commandLine, game, s_render_modules };
    return app.Run();
}

#if defined(XR_PLATFORM_WINDOWS)
namespace
{
// Captured by the filter, which runs before the stack is unwound, and used from the handler once
// _resetstkoflw has made the stack usable again. Static storage: at filter time only the sliver of
// stack past the guard page is left, so nothing bigger than a memcpy is safe there.
EXCEPTION_RECORD s_stack_overflow_record{};
CONTEXT s_stack_overflow_context{};
EXCEPTION_POINTERS s_stack_overflow_pointers{ &s_stack_overflow_record, &s_stack_overflow_context };
bool s_stack_overflow_captured = false;
} // namespace

int StackoverflowFilter(EXCEPTION_POINTERS* exPtrs)
{
    if (!exPtrs || !exPtrs->ExceptionRecord ||
        exPtrs->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;

    s_stack_overflow_record = *exPtrs->ExceptionRecord;
    // The nested-record pointer refers to the dispatcher's stack frame and is dangling afterwards.
    s_stack_overflow_record.ExceptionRecord = nullptr;
    s_stack_overflow_captured = true; // the record alone already carries the code and fault address

    if (exPtrs->ContextRecord)
    {
        s_stack_overflow_context = *exPtrs->ContextRecord;
        s_stack_overflow_pointers.ContextRecord = &s_stack_overflow_context; // may have been cleared
    }
    else
        s_stack_overflow_pointers.ContextRecord = nullptr; // logging then walks the current stack

    return EXCEPTION_EXECUTE_HANDLER;
}

// The deep frames that caused the overflow still sit below the current stack pointer: unwinding
// moved the pointer but did not erase them, and logging only reuses the topmost few kilobytes.
void LogStackOverflow()
{
    EXCEPTION_POINTERS* const exPtrs = s_stack_overflow_captured ? &s_stack_overflow_pointers : nullptr;

    // This handler swallows the exception, so no unhandled exception filter runs for it and the
    // crash report has to be saved from here. Done first: it carries the faulting context, while
    // logging below allocates and could fault again.
    if (exPtrs)
        xrSentry_SaveLocalCrashReport(exPtrs);

    // Guarded: the stack is usable again after _resetstkoflw, but logging still allocates, and a
    // second fault here would cost us the FATAL dialog that follows.
    xrDebug::LogCrashInfoGuarded(exPtrs, "stack overflow");
}

// Main thread stack from the PE header can be ignored or capped in some setups; the render/D3D path
// needs a large reserve. A dedicated thread with an explicit stack size is reliable.
namespace
{
constexpr DWORD kGameThreadStackBytes = 128u * 1024u * 1024u;

struct GameThreadCtx
{
    pcstr command_line{};
    int exit_code{};
};

DWORD WINAPI GameThreadEntry(void* param)
{
    auto* ctx = static_cast<GameThreadCtx*>(param);
    __try
    {
        ctx->exit_code = RunGameImpl(ctx->command_line);
    }
    __except (StackoverflowFilter(GetExceptionInformation()))
    {
        _resetstkoflw();
        LogStackOverflow();
        FATAL("stack overflow");
        ctx->exit_code = 0;
    }
    return 0;
}
} // namespace
#endif

int entry_point(pcstr commandLine)
{
#if defined(XR_PLATFORM_WINDOWS)
    GameThreadCtx ctx{ commandLine, 0 };
    if (HANDLE h = ::CreateThread(nullptr, kGameThreadStackBytes, GameThreadEntry, &ctx,
            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr))
    {
        ::WaitForSingleObject(h, INFINITE);
        ::CloseHandle(h);
        return ctx.exit_code;
    }
#endif
    return RunGameImpl(commandLine);
}

#if defined(XR_PLATFORM_WINDOWS)
int APIENTRY WinMain(HINSTANCE inst, HINSTANCE prevInst, char* commandLine, int cmdShow)
{
    int result = 0;
    // Stack overflow on the game thread is handled inside GameThreadEntry; this covers the fallback
    // path when CreateThread fails and RunGameImpl runs on the main thread.
    __try
    {
        result = entry_point(commandLine);
    }
    __except (StackoverflowFilter(GetExceptionInformation()))
    {
        _resetstkoflw();
        LogStackOverflow();
        FATAL("stack overflow");
    }

    return result;
}
#else
int main(int argc, char *argv[])
{
    int result = EXIT_FAILURE;

    try
    {
        char* commandLine = nullptr;
        int i;
        if(argc > 1)
        {
            size_t sum = 1;
            for(i = 1; i < argc; ++i)
                sum += strlen(argv[i]) + 1;

            commandLine = (char*)xr_malloc(sum);
            ZeroMemory(commandLine, sum);

            for(i = 1; i < argc; ++i)
            {
                strcat(commandLine, argv[i]);
                strcat(commandLine, " ");
            }

            result = entry_point(commandLine);

            xr_free(commandLine);
        }
        else
            result = entry_point("");
    }
    catch (const std::overflow_error& e)
    {
        _resetstkoflw();
        FATAL_F("stack overflow: %s", e.what());
    }
    catch (const std::runtime_error& e)
    {
        FATAL_F("runtime error: %s", e.what());
    }
    catch (const std::exception& e)
    {
        FATAL_F("exception: %s", e.what());
    }
    catch (...)
    {
    // this executes if f() throws std::string or int or any other unrelated type
    }

    return result;
}
#endif
