#pragma once

// xrCore-internal part of the hardware profile (not exported): stage entry points called from xrCore.cpp and
// LocatorAPI.cpp, and the platform collectors (HwProfile_Windows.cpp, HwProfile_Linux.cpp, HwProfile_Apple.cpp).

#include "xrCore/Platform/HwProfile.h"

namespace xray::hw::detail
{
// xrCore::Initialize: after Memory._initialize, before the worker pool is spawned (the pool size comes from the
// policy computed here, -hw_* keys included).
void RunStageA();
// xrCore::Initialize: after FS._initialize. Prints the "* [hw] cpu / disk" lines, runs -hw_selftest.
void RunStageB();
// LocatorAPI::_initialize: the file index counters (cheap, already known there).
void NoteFsCounters(u32 loose_files, u32 archives);

// Pure helpers of HwPolicy.cpp
Tiers ComputeTiers(const Profile& profile);
DiskClass ClassifyDisk(const Profile& profile);
// 0 - the commit limit is enough for a heavy session (or unknown), else the commit limit it wants (stage B warning)
u64 CommitLimitWanted(const Profile& profile);

// Platform collectors. Every failure leaves the field Unknown / 0, nothing is fatal, no admin rights are needed.
void PlatformCollectStageA(Profile& profile);
// Paths are UTF-8, empty when unknown.
void PlatformCollectStageB(Profile& profile, pcstr game_path, pcstr appdata_path);
bool PlatformQueryMemory(u64& avail_phys, u64& commit_avail);
// R14, live: false = unknown. Windows and Linux in their files, SDL_GetPowerInfo elsewhere (HwProfile.cpp).
bool PlatformQueryPower(bool& on_battery, bool& saver);
// -hw_limit, before PlatformCollectStageA: really restrict the process. logical != 0 - affinity to that many logical
// CPUs on `physical` distinct cores (performance cores first); commit != 0 - process commit limit (Job Object).
// Logs what was applied; returns OVR_LIMIT_CPU / OVR_LIMIT_COMMIT of what succeeded. Windows only, elsewhere it logs
// "not supported" and returns 0.
u32 PlatformApplyLimits(u32 physical, u32 logical, u64 commit);
} // namespace xray::hw::detail
