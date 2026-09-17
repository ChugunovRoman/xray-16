#include "stdafx.h"

#include "StackTrace.h"

#include "Threading/ScopeLock.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef XR_PLATFORM_WINDOWS
#   include <DbgHelp.h>
#   include <psapi.h>
#elif defined(XR_PLATFORM_LINUX) || defined(XR_PLATFORM_APPLE) || defined(XR_PLATFORM_BSD)
#   if __has_include(<execinfo.h>)
#       include <execinfo.h>
#       define BACKTRACE_AVAILABLE

#       if __has_include(<cxxabi.h>)
#           include <cxxabi.h>
#           include <dlfcn.h>
#           define CXXABI_AVAILABLE
#       endif
#   endif
#endif

#ifdef XR_PLATFORM_WINDOWS
namespace
{
#   if defined(XR_ARCHITECTURE_X86)
constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_I386;
#   elif defined(XR_ARCHITECTURE_X64)
constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_AMD64;
#   elif defined(XR_ARCHITECTURE_ARM)
constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_ARM;
#   elif defined(XR_ARCHITECTURE_ARM64)
constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_ARM64;
#   elif defined(XR_ARCHITECTURE_IA64)
constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_IA64;
#   else
#       error CPU architecture is not supported.
#   endif

Lock s_dbghelp_lock;

HMODULE s_dbghelp{};
bool s_dbghelp_load_attempted{};
bool s_sym_initialized{};

// Enough for any sane process; a static buffer keeps EnumProcessModules off the stack,
// which matters when we are called from a crash handler.
constexpr size_t kMaxProcessModules = 512;

decltype(&SymInitialize)          symInitialize{};
decltype(&SymCleanup)             symCleanup{};
decltype(&SymGetOptions)          symGetOptions{};
decltype(&SymSetOptions)          symSetOptions{};
decltype(&StackWalk)              stackWalk{};
decltype(&SymFunctionTableAccess) symFunctionTableAccess{};
decltype(&SymGetModuleBase)       symGetModuleBase{};
decltype(&SymGetSymFromAddr)      symGetSymFromAddr{};
decltype(&SymGetLineFromAddr)     symGetLineFromAddr{};
decltype(&SymRefreshModuleList)   symRefreshModuleList{};

template <typename T>
void load_function(T& func, cpcstr name)
{
    func = reinterpret_cast<T>(GetProcAddress(s_dbghelp, name)); // NOLINT(clang-diagnostic-cast-function-type-strict)

    if (!func)
        Msg("! [StackTraceBuilder] Failed to load %s function", name);
}

#   define STRINGIZE_HELPER(value) #value
#   define STRINGIZE(value) STRINGIZE_HELPER(value)

void init_dbghelp()
{
    if (s_dbghelp_load_attempted)
        return;
    s_dbghelp_load_attempted = true;

    // LoadLibrary, not GetModuleHandle: dbghelp.lib is not linked and the DLL is not shipped
    // with the game, so it is usually not present in the process yet. The system copy is used.
    s_dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!s_dbghelp)
    {
        Log("! [StackTraceBuilder] Failed to load dbghelp.dll");
        return;
    }

    load_function(symGetOptions, "SymGetOptions");
    load_function(symSetOptions, "SymSetOptions");
    load_function(symInitialize, "SymInitialize");
    load_function(symCleanup,    "SymCleanup");

    load_function(stackWalk, STRINGIZE(StackWalk));
    load_function(symGetModuleBase, STRINGIZE(SymGetModuleBase));
    load_function(symGetSymFromAddr, STRINGIZE(SymGetSymFromAddr));
    load_function(symGetLineFromAddr, STRINGIZE(SymGetLineFromAddr));
    load_function(symFunctionTableAccess, STRINGIZE(SymFunctionTableAccess));
    load_function(symRefreshModuleList, "SymRefreshModuleList");
}

#   undef STRINGIZE_HELPER
#   undef STRINGIZE

// Everything needed to walk the stack. Symbol name / source line lookup is optional:
// without it a frame still carries module + RVA, which can be symbolized offline.
bool dbghelp_can_walk()
{
    return s_dbghelp && symGetOptions && symSetOptions && symInitialize && stackWalk &&
        symFunctionTableAccess && symGetModuleBase;
}

pcstr module_file_name(pcstr fullPath)
{
    pcstr name = std::strrchr(fullPath, '\\');
    cpcstr nameFwd = std::strrchr(fullPath, '/');
    if (nameFwd && (!name || nameFwd > name))
        name = nameFwd;
    return name ? name + 1 : fullPath;
}

// Creates the symbol handler. Called from PreloadStackTraceLibrary at startup so that neither the
// module enumeration it performs nor the loader lock it takes land on the crash path.
// Caller holds s_dbghelp_lock.
void ensure_sym_initialized()
{
    if (s_sym_initialized || !dbghelp_can_walk())
        return;

    const u32 dwOptions = symGetOptions();
    // NO_PROMPTS and FAIL_CRITICAL_ERRORS keep dbghelp from opening dialogs while we are handling a
    // crash. The search path is the game folder, never nullptr: that would honour _NT_SYMBOL_PATH,
    // and a symbol server in it would send the crash handler to the network for minutes.
    symSetOptions(dwOptions | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME |
        SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);

    string_path searchPath{};
    if (GetModuleFileNameA(nullptr, searchPath, _countof(searchPath)))
    {
        pstr slash = std::strrchr(searchPath, '\\');
        if (slash)
            *slash = '\0';
        else
            searchPath[0] = '\0';
    }

    s_sym_initialized = symInitialize(GetCurrentProcess(), searchPath[0] ? searchPath : nullptr, TRUE) != FALSE;

    if (!s_sym_initialized)
        Msg("! [StackTraceBuilder] SymInitialize failed with error: 0x%x", GetLastError());
}

// Modules loaded after SymInitialize (the render DLLs, GPU driver, overlays) are unknown to dbghelp
// until the list is refreshed, and on x64 a frame in an unknown module also breaks the unwind.
// Caller holds s_dbghelp_lock.
void refresh_sym_modules()
{
    if (s_sym_initialized && symRefreshModuleList)
        symRefreshModuleList(GetCurrentProcess());
}
} // namespace

struct StackTraceBuilder
{
    StackTraceBuilder();

    bool GetNextStackFrameString(LPSTACKFRAME stackFrame, PCONTEXT threadCtx, xr_string& frameStr);

    bool IsInitialized{};
};

StackTraceBuilder::StackTraceBuilder()
{
    // Normally both already happened in PreloadStackTraceLibrary; this covers tools that never
    // call it. SymCleanup is deliberately never called, the handler lives until the process exits.
    init_dbghelp();
    ensure_sym_initialized();

    if (!s_sym_initialized)
        return;

    refresh_sym_modules();
    IsInitialized = true;
}

bool StackTraceBuilder::GetNextStackFrameString(LPSTACKFRAME stackFrame, PCONTEXT threadCtx, xr_string& frameStr)
{
    BOOL result = stackWalk(MACHINE_TYPE, GetCurrentProcess(), GetCurrentThread(), stackFrame, threadCtx, nullptr,
        symFunctionTableAccess, symGetModuleBase, nullptr);

    if (result == FALSE || stackFrame->AddrPC.Offset == 0)
    {
        return false;
    }

    frameStr.clear();
    string512 formatBuff;

    ///
    /// Module + RVA: stays useful without PDBs, offline symbolization needs nothing else
    ///
    FormatModuleRelativeAddress(reinterpret_cast<const void*>(stackFrame->AddrPC.Offset), frameStr);

    ///
    /// Function info (needs symbols for the module, i.e. a PDB next to it)
    ///
    if (symGetSymFromAddr)
    {
        u8 arrSymBuffer[512]{};
        PIMAGEHLP_SYMBOL functionInfo = reinterpret_cast<PIMAGEHLP_SYMBOL>(arrSymBuffer);
        functionInfo->SizeOfStruct  = sizeof(*functionInfo);
        functionInfo->MaxNameLength = sizeof(arrSymBuffer) - sizeof(*functionInfo) + 1;
        DWORD_PTR dwFunctionOffset = 0;

        result = symGetSymFromAddr(GetCurrentProcess(), stackFrame->AddrPC.Offset, &dwFunctionOffset, functionInfo);

        if (result)
        {
            // snprintf, never xr_sprintf: that one is vsprintf_s outside MASTER_GOLD and reports a
            // buffer that is too small through the invalid parameter handler, which calls Fail,
            // which builds a stack trace, which lands right back here. Truncation is the safe answer.
            if (dwFunctionOffset)
            {
                std::snprintf(formatBuff, _countof(formatBuff), " %s()+0x%llX", functionInfo->Name,
                    static_cast<u64>(dwFunctionOffset));
            }
            else
            {
                std::snprintf(formatBuff, _countof(formatBuff), " %s()", functionInfo->Name);
            }
            frameStr.append(formatBuff);
        }
    }

    ///
    /// Source info
    ///
    if (symGetLineFromAddr)
    {
        DWORD dwLineOffset = 0;
        IMAGEHLP_LINE sourceInfo = {};
        sourceInfo.SizeOfStruct = sizeof(sourceInfo);

        result = symGetLineFromAddr(GetCurrentProcess(), stackFrame->AddrPC.Offset, &dwLineOffset, &sourceInfo);

        if (result)
        {
            std::snprintf(formatBuff, _countof(formatBuff), " [%s:%u]", sourceInfo.FileName,
                sourceInfo.LineNumber);
            frameStr.append(formatBuff);
        }
    }

    return true;
}

void PreloadStackTraceLibrary()
{
    ScopeLock lock(&s_dbghelp_lock);
    init_dbghelp();
    ensure_sym_initialized();
}

void FormatModuleRelativeAddress(const void* address, xr_string& out)
{
    string512 formatBuff;
    HMODULE hModule{};

    // GetModuleHandleEx instead of SymGetModuleBase: plain WinAPI, works even when dbghelp is broken.
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &hModule) &&
        hModule)
    {
        string_path modulePath;
        if (GetModuleFileNameA(hModule, modulePath, _countof(modulePath)))
        {
            const u64 rva = static_cast<u64>(reinterpret_cast<uintptr_t>(address) -
                reinterpret_cast<uintptr_t>(hModule));
            std::snprintf(formatBuff, _countof(formatBuff), "%s+0x%08llX", module_file_name(modulePath), rva);
            out.append(formatBuff);
            return;
        }
    }

    std::snprintf(formatBuff, _countof(formatBuff), "0x%016llX <unknown>",
        static_cast<u64>(reinterpret_cast<uintptr_t>(address)));
    out.append(formatBuff);
}

static void collect_game_modules_impl(xr_vector<xr_string>& out, const void* extraAddress)
{
    // Only modules shipped with the game are interesting: system DLLs are noise and are never
    // symbolized from our PDBs. The module holding extraAddress is kept regardless.
    string_path exeDir{};
    size_t exeDirLen = 0;
    if (GetModuleFileNameA(nullptr, exeDir, _countof(exeDir)))
    {
        pstr slash = std::strrchr(exeDir, '\\');
        if (slash)
        {
            slash[1] = '\0';
            exeDirLen = std::strlen(exeDir);
        }
    }

    HMODULE extraModule{};
    if (extraAddress)
    {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(extraAddress), &extraModule);
    }

    static HMODULE modules[kMaxProcessModules];
    DWORD bytesNeeded = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytesNeeded))
        return;

    const size_t count = std::min<size_t>(bytesNeeded / sizeof(HMODULE), kMaxProcessModules);

    string512 formatBuff;
    string_path modulePath;
    for (size_t i = 0; i < count; ++i)
    {
        if (!GetModuleFileNameA(modules[i], modulePath, _countof(modulePath)))
            continue;

        const bool fromGameFolder = exeDirLen != 0 && _strnicmp(modulePath, exeDir, exeDirLen) == 0;
        if (!fromGameFolder && modules[i] != extraModule)
            continue;

        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info)))
            continue;

        std::snprintf(formatBuff, _countof(formatBuff), "%-20s base=0x%016llX size=0x%08X",
            module_file_name(modulePath), static_cast<u64>(reinterpret_cast<uintptr_t>(info.lpBaseOfDll)),
            static_cast<u32>(info.SizeOfImage));
        out.emplace_back(formatBuff);
    }
}

// The lock is released through __finally, not through a scope guard: the project builds without /EHa,
// so an SEH exception caught by the caller would not run C++ destructors, and dbghelp would stay
// locked for every other thread. A function using __finally must hold no unwindable objects itself.
void CollectGameModules(xr_vector<xr_string>& out, const void* extraAddress)
{
    s_dbghelp_lock.Enter(); // guards the static module buffer inside
    __try
    {
        collect_game_modules_impl(out, extraAddress);
    }
    __finally
    {
        s_dbghelp_lock.Leave();
    }
}

static void build_stack_trace_impl(PCONTEXT threadCtx, u16 maxFramesCount, xr_vector<xr_string>& traceResult)
{
    StackTraceBuilder builder;
    if (!builder.IsInitialized)
        return;

    xr_string frameStr;

    traceResult.reserve(maxFramesCount);

    STACKFRAME stackFrame{};
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrBStore.Mode = AddrModeFlat;

    // https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/ns-dbghelp-stackframe
    // https://github.com/reactos/reactos/blob/master/base/applications/drwtsn32/stacktrace.cpp
#if defined XR_ARCHITECTURE_X86
    stackFrame.AddrPC.Offset = threadCtx->Eip;
    stackFrame.AddrStack.Offset = threadCtx->Esp;
    stackFrame.AddrFrame.Offset = threadCtx->Ebp;
#elif defined XR_ARCHITECTURE_X64
    stackFrame.AddrPC.Offset = threadCtx->Rip;
    stackFrame.AddrStack.Offset = threadCtx->Rsp;
    stackFrame.AddrFrame.Offset = threadCtx->Rbp;
#elif defined XR_ARCHITECTURE_ARM
    stackFrame.AddrPC.Offset = threadCtx->Pc;
    stackFrame.AddrStack.Offset = threadCtx->Sp;
    stackFrame.AddrFrame.Offset = threadCtx->R11;
#elif defined XR_ARCHITECTURE_ARM64
    stackFrame.AddrPC.Offset = threadCtx->Pc;
    stackFrame.AddrStack.Offset = threadCtx->Sp;
    stackFrame.AddrFrame.Offset = threadCtx->Fp;
#elif defined XR_ARCHITECTURE_IA64
    stackFrame.AddrPC.Offset = threadCtx->StIIP;
    stackFrame.AddrStack.Offset = threadCtx->IntSp;
    stackFrame.AddrBStore.Offset = threadCtx->RsBSP;
#else
#   error CPU architecture is not supported.
#endif

    while (traceResult.size() < maxFramesCount && builder.GetNextStackFrameString(&stackFrame, threadCtx, frameStr))
    {
        traceResult.emplace_back(std::move(frameStr));
    }
}

// See CollectGameModules: the lock must survive an SEH exception thrown out of dbghelp.
static void build_stack_trace_locked(PCONTEXT threadCtx, u16 maxFramesCount, xr_vector<xr_string>* traceResult)
{
    s_dbghelp_lock.Enter();
    __try
    {
        build_stack_trace_impl(threadCtx, maxFramesCount, *traceResult);
    }
    __finally
    {
        s_dbghelp_lock.Leave();
    }
}

xr_vector<xr_string> BuildStackTrace(PCONTEXT threadCtx, u16 maxFramesCount)
{
    xr_vector<xr_string> traceResult;
    build_stack_trace_locked(threadCtx, maxFramesCount, &traceResult);
    return traceResult;
}

xr_vector<xr_string> BuildStackTrace(u16 maxFramesCount)
{
    CONTEXT currentThreadCtx = {};

    RtlCaptureContext(&currentThreadCtx); /// GetThreadContext can't be used on the current thread
    currentThreadCtx.ContextFlags = CONTEXT_FULL;

    return BuildStackTrace(&currentThreadCtx, maxFramesCount);
}
#elif defined(BACKTRACE_AVAILABLE)
xr_vector<xr_string> BuildStackTrace(u16 maxFramesCount)
{
    xr_vector<xr_string> result;

    void** array = reinterpret_cast<void**>(xr_alloca(sizeof(void*) * maxFramesCount));
    int nptrs = backtrace(array, maxFramesCount); // get void*'s for all entries on the stack
    char** strings = backtrace_symbols(array, nptrs);

    if (strings)
    {
        size_t demangledBufSize = 0;
        char* demangledName = nullptr;
        for (int i = 1; i < nptrs; i++) // skip this function
        {
            char* functionName = strings[i];

#   ifdef CXXABI_AVAILABLE
            Dl_info info;

            if (dladdr(array[i], &info))
            {
                if (info.dli_sname)
                {
                    int status = -1;
                    demangledName = abi::__cxa_demangle(info.dli_sname, demangledName, &demangledBufSize, &status);
                    if (status == 0)
                    {
                        functionName = demangledName;
                    }
                }
            }
#   endif
            result.emplace_back(functionName);
        }
        ::free(demangledName);
    }

    return result;
}
#else
xr_vector<xr_string> BuildStackTrace(u16 maxFramesCount)
{
#   pragma todo("Implement stack trace for this platform")
    return { "Stack trace is not implemented for this platform." };
}
#endif
