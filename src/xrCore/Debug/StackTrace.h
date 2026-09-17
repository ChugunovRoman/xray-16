#pragma once

xr_vector<xr_string> BuildStackTrace(u16 maxFramesCount = 512);

#ifdef XR_PLATFORM_WINDOWS
xr_vector<xr_string> BuildStackTrace(PCONTEXT threadCtx, u16 maxFramesCount);

// Loads dbghelp.dll and creates the symbol handler up front, at startup. Both take the loader lock
// and enumerate modules, which must not happen while a crash is being handled. Other loader-lock
// users remain on the crash path (module lookups for RVAs), so this reduces the risk, not removes it.
void PreloadStackTraceLibrary();

// Appends "xrCore.dll+0x0004C3D4" for an address inside a loaded module, "0x... <unknown>" otherwise.
// Module + RVA survives without PDBs: tools/symbolize_crash.py resolves it offline.
void FormatModuleRelativeAddress(const void* address, xr_string& out);

// Appends "name base=0x... size=0x..." for every module shipped with the game,
// plus the module holding extraAddress (used when a crash lands in a system DLL).
void CollectGameModules(xr_vector<xr_string>& out, const void* extraAddress = nullptr);
#endif
