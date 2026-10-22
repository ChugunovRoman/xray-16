#pragma once

// Hardware profile and the policy derived from it (plans/optimization_mt/50-smart-engine-hw-profile.md).
//
// Profile = facts about the machine, Policy = decisions computed from them by one pure function (HwPolicy.cpp, all the
// thresholds live there). Both are filled once, by explicit stages on the main thread, and only read afterwards:
//   A - xrCore::Initialize before the worker pool is spawned: CPU, RAM/commit, OS, Wine, power, -hw_* keys;
//   B - after FS._initialize: volumes of $game_data$, $app_data_root$, the system drive; the "* [hw]" lines are
//       printed here, so they are in the log even if the render device creation crashes;
//   C - the render after its device is created (SubmitGpu): GPU facts, then the "gpu" and "tiers/policy" lines.
// Nothing is collected lazily: a first call from a worker in the middle of a frame must never run an IOCTL or DXGI.
//
// DLL boundary: POD and char arrays only, never std::string / xr_string / containers (xrCore has its own operator new,
// plan 36). New fields go to the END of a struct only; 'size' / 'version' let a reader check what it got.
//
// Stage 1 (plan 50b): the engine reads the policy - the worker pool (R8, at stage A), the parallel main recording and
// the SVP worker (R9, after the real device), the D3D11 internal threading flag (R10). The -hw_* keys change the
// profile and with it the policy (overridden values are marked with '*'); -hw_limit really restricts the process
// (affinity, Job Object commit limit) before stage A collects, so the collectors see the limited values.
//
// Stage 3 (plan 50g): the bounded spin of the waits (R11, TaskManager) follows GetEffectiveSpinClass() - the only
// policy decision that changes during a session: PollPower switches it between AC and battery (R14).
//
// Not included from xrCore.h on purpose: only the consumers include it.

#include "xrCore/xrCore.h"

namespace xray::hw
{
constexpr u32 PROFILE_VERSION = 1;
constexpr u32 POLICY_VERSION = 1;

enum class DiskKind : u8
{
    Unknown,
    Hdd,
    SataSsd,
    Nvme,
    Removable, // USB, SD, MMC
    Network, // UNC, mapped network drive, iSCSI, NFS/SMB
    Virtual, // VHD, VM disk, loop device, RAM disk
    Ssd, // solid state on another bus (SAS, RAID, Storage Spaces)
};

// Disk axis for the rules (plan 50, 5.1): what the I/O-related rules read.
enum class DiskClass : u8
{
    Unknown,
    Slow, // HDD, USB, network
    Sata,
    Fast, // NVMe
};

enum class Tier : u8
{
    Potato,
    Low,
    Medium,
    High,
};

enum class Os : u8
{
    Unknown,
    Windows,
    Linux,
    Apple,
    Bsd,
    Other,
};

enum class GpuApi : u8
{
    Unknown,
    D3D11,
    OpenGL,
};

enum class SwapKind : u8
{
    Unknown,
    None, // no pagefile / swap: the commit limit is the RAM
    File,
    Partition,
    Zram,
};

enum class PrefetchMode : u8
{
    Full, // the whole list, as now
    Core, // prefix of the priority list up to prefetch_budget
    Level, // only the level stage
};

enum class SpinClass : u8
{
    Long, // current behaviour
    Short, // bounded spin -> yield -> block
};

// Profile::stages
enum : u32
{
    STAGE_A = 1u << 0,
    STAGE_B = 1u << 1,
    STAGE_C = 1u << 2, // GPU of the real render device
    STAGE_C_PROBE = 1u << 3, // GPU of a startup HW probe only (until STAGE_C)
};

// Profile::overridden - which facts come from -hw_* keys
enum : u32
{
    OVR_PROFILE = 1u << 0,
    OVR_RAM = 1u << 1,
    OVR_COMMIT = 1u << 2,
    OVR_CPU = 1u << 3,
    OVR_DISK = 1u << 4,
    OVR_VRAM = 1u << 5,
    OVR_IGPU = 1u << 6,
    OVR_NOCL = 1u << 7,
    OVR_BATTERY = 1u << 8,
    // -hw_limit applied for real (not emulation): the collected CPU / commit values are the limited ones
    OVR_LIMIT_CPU = 1u << 9,
    OVR_LIMIT_COMMIT = 1u << 10,
    // -hw_cl (A/B test, D3D11 only): DriverCommandLists taken as 1 - the policy (R9) allows the parallel main recording
    // and the SVP worker on a driver whose deferred contexts the runtime emulates
    OVR_CL = 1u << 11,
};

constexpr u8 VALUE_UNKNOWN = 0xFF; // for the u8 tri-state fields (0 / 1 / unknown)

struct Disk
{
    DiskKind kind;
    u8 seek_penalty; // 0, 1, VALUE_UNKNOWN
    u8 bus; // STORAGE_BUS_TYPE on Windows, VALUE_UNKNOWN elsewhere
    u8 removable;
    u8 same_as; // 0 - own volume, 1 - same volume as game, 2 - as appdata, 3 - as system
    u8 reserved[3];
    u64 free_bytes;
    u64 total_bytes;
    char volume[64]; // "K:", "K:=D:" for a subst drive, "UNC", mount point...
};

struct Cpu
{
    u16 logical_total;
    u16 logical_avail; // affinity / CPU sets / cgroup quota
    u16 physical;
    u16 physical_avail;
    u16 perf_cores; // hybrid CPUs only, physical cores
    u16 eff_cores;
    u8 smt;
    u8 hybrid;
    u16 groups;
    char brand[64];
};

struct Mem
{
    u64 ram_total;
    u64 ram_installed; // 0 = unknown
    u64 ram_avail_start;
    u64 commit_limit;
    u64 commit_avail_start;
    u64 swap_bytes; // commit_limit - ram_total on Windows
    SwapKind swap_kind;
    u8 swap_sys_managed; // 0, 1, VALUE_UNKNOWN
    char swap_volume[8]; // "C:" - volume of the pagefile (Windows)
};

struct Gpu
{
    u32 size; // sizeof(Gpu) of the submitter
    u32 vendor;
    u32 device;
    char name[128];
    GpuApi api;
    u8 software; // WARP / Microsoft Basic Render / llvmpipe
    u8 uma; // integrated: VRAM is the same RAM
    u8 driver_cl; // D3D11 DriverCommandLists
    u8 driver_cc; // D3D11 DriverConcurrentCreates
    u8 better_adapter; // a stronger adapter than the selected one exists (log only)
    u8 adapters; // hardware adapters seen
    u8 reserved;
    u16 feature_level; // D3D_FEATURE_LEVEL; GL: major << 12 | minor << 8
    u16 reserved2;
    u64 dedicated;
    u64 shared;
    u64 budget_local_start; // DXGI local budget / GL available video memory, 0 = unknown
    u32 width;
    u32 height;
    u32 refresh;
};

struct Env
{
    Os os;
    u8 wine;
    u8 has_battery; // 0, 1, VALUE_UNKNOWN
    u8 on_battery;
    u8 battery_saver;
    u8 steam_deck;
    u8 reserved[2];
    u32 os_major;
    u32 os_minor;
    u32 os_build;
    char os_name[48]; // kernel release / product version
    char wine_version[32];
};

struct Data
{
    Disk game;
    Disk appdata;
    Disk system;
    Disk swap; // volume of the pagefile when it is none of the above
    u32 loose_files;
    u32 archives;
    u64 bytes_meshes; // not collected yet (0)
    u64 bytes_textures;
    u64 bytes_sounds;
    u8 shader_cache_cold; // not collected yet (VALUE_UNKNOWN)
    u8 reserved[7];
};

struct Tiers
{
    Tier cpu;
    Tier ram;
    Tier disk;
    Tier gpu;
    Tier overall; // min of the axes: log, -hw_profile, first-run preset
    DiskClass disk_class;
    u8 gpu_known; // 0 until a GPU was submitted (the GPU axis is Medium then)
    u8 reserved;
};

struct Profile
{
    u32 size;
    u32 version;
    u32 stages; // STAGE_*
    u32 overridden; // OVR_*
    Cpu cpu;
    Mem mem;
    Data data;
    Gpu gpu;
    Env env;
    Tiers tiers;
    float stage_a_ms;
    float stage_b_ms;
};

struct Policy
{
    u32 size;
    u32 version;
    PrefetchMode prefetch_mode; // R1
    u8 warm_threads; // R3
    u8 load_threads; // R3
    u8 pool_workers; // R8
    SpinClass spin_class; // R11
    u8 main_parallel_parts; // R9: cap, effective = min(user, policy)
    u8 svp_worker; // R9
    u8 svp_pages; // R12: cap
    u64 prefetch_budget; // R1 variant B, bytes; ignored in PrefetchMode::Full
    u64 commit_headroom_min; // R2: trim the pool below this commit headroom at a level load
    u8 texture_lod_auto; // R13
    u8 prevent_internal_threads; // R10
    u8 io_single_queue; // R7
    u8 reserved;
    u16 smap_size_auto; // R13: 0 = keep
    u16 icons_mb; // R13
    u16 snd_cache_mb_auto; // R18
    u16 lua_paced_mul; // R17
    u16 shed_budget_ms; // R16
    u16 model_prefetch_margin; // R5
    u16 spawn_cold_ms; // R6: cold model loads per frame, 0 = wait for the background warm-up
    u16 reserved2;
    // 50l-weak-cpu-spawn-models.md (plans/optimization_mt), a weak CPU (cpu tier low / potato):
    u8 spawn_cold_frames; // in play at most one cold model load per this many frames (low 3, potato 4; 0 = no spacing)
    u8 model_bg_load; // the base models of the creatures about to come online are loaded off the main thread (D3D11 with
                      // DriverConcurrentCreates only)
    u16 reserved3;
};

XRCORE_API const Profile& GetProfile();
XRCORE_API const Policy& GetPolicy();

// From the render: probe = a startup HW probe instance (r2_test_hw.cpp), false = the real device. The first probe is
// kept until the real device reports; the real device always replaces it (vid_restart reports again).
XRCORE_API void SubmitGpu(const Gpu& gpu, bool probe);

// Live values, not cached (the profile keeps only the start values).
XRCORE_API void QueryMemoryNow(u64& avail_phys, u64& commit_avail);

// R14: live power state, not cached. false = unknown (no API on this platform, an error): the outputs are false then.
// Cheap: one system call on Windows (GetSystemPowerStatus), a few sysfs reads on Linux, SDL_GetPowerInfo elsewhere.
// saver: Windows battery saver / energy saver only (false elsewhere).
XRCORE_API bool QueryPowerNow(bool& on_battery, bool& saver);

// R11 + R14: the spin class the waits use now (TaskManager). The policy's class on AC; Short while on battery or in
// battery saver. Long until stage A ran. Any thread, one relaxed atomic load.
XRCORE_API SpinClass GetEffectiveSpinClass();

// R14: main thread, every frame (CRenderDevice::ProcessFrame). Re-reads the power state every ~5 s on a machine with
// a battery (or an unknown one) and switches the effective spin class, logging "* [hw] power: ..." on a change.
// Returns at once on a desktop without a battery and under -hw_battery (the emulated state stays).
XRCORE_API void PollPower();

// The pure policy function (HwPolicy.cpp), exported for tests.
XRCORE_API Policy ComputePolicy(const Profile& profile);

// Prints the profile and the policy to the log (console command hw_info).
XRCORE_API void LogInfo();

XRCORE_API pcstr TierName(Tier tier);
XRCORE_API pcstr DiskKindName(DiskKind kind);

// A zeroed Policy (size 0) means "no policy": stage A never ran in this process. The readers keep the old behaviour then.
inline bool PolicyValid(const Policy& policy) { return policy.size != 0; }

// R9: parts of the parallel main recording = min(user r__main_parallel_parts, policy cap); < 2 = sequential recording.
inline u32 EffectiveMainParallelParts(u32 user_parts)
{
    const Policy& policy = GetPolicy();
    if (!PolicyValid(policy) || !policy.main_parallel_parts)
        return user_parts;
    return user_parts < policy.main_parallel_parts ? user_parts : policy.main_parallel_parts;
}

// R9: the persistent SVP worker (parallel scope pass) is allowed
inline bool SvpWorkerAllowed()
{
    const Policy& policy = GetPolicy();
    return !PolicyValid(policy) || policy.svp_worker != 0;
}
} // namespace xray::hw
