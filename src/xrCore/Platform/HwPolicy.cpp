#include "stdafx.h"
#pragma hdrstop

#include "xrCore/Platform/HwProfile_impl.h"

#include <algorithm>

// All the thresholds of the hardware policy, in one place (plans/optimization_mt/50-smart-engine-hw-profile.md,
// tiers - section 5.1, rules R1..R24 - section 6). The starting values are estimates, stage 4 of the plan refines
// them by measurements. Rule of the plan: on a "high" machine every decision equals the current behaviour, the
// changes happen below the thresholds only.
//
// Pure functions: no globals, no system calls, so -hw_selftest can run them over synthetic profiles.

namespace xray::hw
{
namespace
{
constexpr u64 kHwPolicyMiB = u64(1) << 20;
constexpr u64 kHwPolicyGiB = u64(1) << 30;

// The OS reports less RAM than installed (iGPU reserve, firmware): 15.4 GB of a 16 GB machine must count as 16 (plan 50,
// section 10, border values). Applied to the RAM and the commit limit thresholds.
constexpr u64 kHwPolicyRamSlack = kHwPolicyGiB;

// psTextureLOD default (xrEngine/defines.cpp)
constexpr u8 kHwPolicyTextureLodDefault = 1;
// r__main_parallel_parts range is [2, 12] (xrRender_console.cpp); 1 here = sequential recording
constexpr u8 kHwPolicyMainPartsMax = 12;
// r__svp_smap_pages of the shipped user.ltx
constexpr u8 kHwPolicySvpPagesDefault = 8;
// kPrefetchMaxLoadThreads (ModelPool.cpp): the serialized parts limit the gain beyond it
constexpr u32 kHwPolicyLoadThreadsMax = 8;
// R8: the pool size above was measured up to 24 hardware threads only
constexpr u32 kHwPolicyPoolMax = 23;
// R1: the process commit of a heavy GW session WITHOUT the prefetch: the laptop of 2026-10-19 (i7-1165G7, 15.8 GB RAM,
// commit limit 17.0 GB, a save with ~1000 NPCs) peaked at 11.7 GB of process commit with 3.4 GB of prefetch in it,
// i.e. ~8 GB without it (plan 50, section 18).
constexpr u64 kHwPolicyGameCommitPeakNoPrefetch = 8 * kHwPolicyGiB;
// R1: what the system keeps free on top of that peak (the OS, the driver, the background programs grow during the
// session; the laptop fell to 2.6 GB of system commit headroom and still ran). 3 GB, not 2: the developer's call
// (2026-10-19) - more headroom on laptops with a small pagefile (the laptop's budget 3.5 -> 2.8 GB)
constexpr u64 kHwPolicyCommitReserve = 3 * kHwPolicyGiB;
// Stage B warning (plan 50, section 18): a commit limit below this is too small for a heavy save - the peak above plus
// the core prefetch (~3.5 GB on 16 GB), the OS and the background (3-4 GB), the reserve and the programs a player keeps
// open. An absolute value, not RAM + N: the game needs the same commit on any RAM (32 GB without a pagefile is enough,
// 8 GB with an 8 GB pagefile is not). 24 GB = 16 GB of RAM + an 8 GB pagefile, the "medium" class of -hw_profile.
constexpr u32 kHwPolicyCommitWarnGiB = 24;

bool HwPolicyAtLeastGiB(u64 bytes, u64 gib) { return bytes + kHwPolicyRamSlack >= gib * kHwPolicyGiB; }

u32 HwPolicyLogical(const Profile& p)
{
    const u32 n = p.cpu.logical_avail ? p.cpu.logical_avail : p.cpu.logical_total;
    return n ? n : 4; // unknown: an average machine (plan 50, section 9)
}

u32 HwPolicyPhysical(const Profile& p)
{
    u32 n = p.cpu.physical_avail ? p.cpu.physical_avail : p.cpu.physical;
    if (!n)
        n = HwPolicyLogical(p); // topology unknown: assume no SMT
    return std::min(n, HwPolicyLogical(p));
}

// Video memory the process may use: the DXGI budget when known, else the dedicated memory
u64 HwPolicyVram(const Gpu& g) { return g.budget_local_start ? g.budget_local_start : g.dedicated; }

bool HwPolicyGpuKnown(const Profile& p)
{
    return (p.stages & (STAGE_C | STAGE_C_PROBE)) != 0 || (p.overridden & (OVR_PROFILE | OVR_VRAM)) != 0;
}

// R12/R13: the "little video memory" condition - integrated GPU or a budget of 3 GB or less
bool HwPolicyLowVram(const Profile& p)
{
    if (!HwPolicyGpuKnown(p))
        return false;
    const u64 vram = HwPolicyVram(p.gpu);
    return p.gpu.uma || p.gpu.software || (vram && vram <= 3 * kHwPolicyGiB);
}

bool HwPolicySlowKind(DiskKind kind)
{
    return kind == DiskKind::Hdd || kind == DiskKind::Removable || kind == DiskKind::Network;
}
} // namespace

namespace detail
{
// Disk axis (5.1): Slow - HDD, USB, network; Unknown - Wine, VM disk, detection failed; Sata; Fast - NVMe.
DiskClass ClassifyDisk(const Profile& p)
{
    const DiskKind kind = p.data.game.kind;
    if (kind == DiskKind::Network)
        return DiskClass::Slow;
    // R24: Wine most likely does not implement the seek penalty query - do not trust what it says, stage 1 measures
    if (p.env.wine && !(p.overridden & (OVR_DISK | OVR_PROFILE)))
        return DiskClass::Unknown;
    switch (kind)
    {
    case DiskKind::Hdd:
    case DiskKind::Removable: return DiskClass::Slow;
    case DiskKind::SataSsd:
    case DiskKind::Ssd: return DiskClass::Sata;
    case DiskKind::Nvme: return DiskClass::Fast;
    default: return DiskClass::Unknown;
    }
}

Tiers ComputeTiers(const Profile& p)
{
    Tiers t{};
    t.gpu_known = HwPolicyGpuKnown(p) ? 1 : 0;

    // CPU (5.1): by the physical cores available to the process only (decision of 2026-10-18, plan 50 section 14):
    // the SMT siblings of a 6c/12t share the cores with main and the Game thread. The worker pool (R8) is a
    // separate rule and still counts the logical threads.
    const u32 phys = HwPolicyPhysical(p);
    if (phys >= 8)
        t.cpu = Tier::High;
    else if (phys >= 5)
        t.cpu = Tier::Medium;
    else if (phys >= 3)
        t.cpu = Tier::Low;
    else
        t.cpu = Tier::Potato;

    // RAM (5.1): Low - < 12 GB, or UMA with < 16 GB, or commit limit < 16 GB; Medium - 12..24; High - >= 24 and commit
    // limit >= 32. Potato (< 6 GB, 4 GB machines) is not in the plan table: below the Low prefetch there is nothing.
    const u64 ram = p.mem.ram_total;
    const u64 commit = p.mem.commit_limit;
    const bool uma = t.gpu_known && p.gpu.uma;
    if (!ram)
        t.ram = Tier::Medium;
    else if (!HwPolicyAtLeastGiB(ram, 6))
        t.ram = Tier::Potato;
    else if (!HwPolicyAtLeastGiB(ram, 12) || (uma && !HwPolicyAtLeastGiB(ram, 16)) ||
        (commit && !HwPolicyAtLeastGiB(commit, 16)))
        t.ram = Tier::Low;
    else if (HwPolicyAtLeastGiB(ram, 24) && (!commit || HwPolicyAtLeastGiB(commit, 32)))
        t.ram = Tier::High;
    else
        t.ram = Tier::Medium;

    // Disk: Slow -> low, Unknown -> medium (average policy + measurement, section 10), SATA SSD and NVMe -> high.
    // SATA SSD is not a weakness for the overall class; the I/O rules read disk_class, where SATA and NVMe differ.
    t.disk_class = ClassifyDisk(p);
    switch (t.disk_class)
    {
    case DiskClass::Slow: t.disk = Tier::Low; break;
    case DiskClass::Unknown: t.disk = Tier::Medium; break;
    default: t.disk = Tier::High; break;
    }

    // GPU (5.1): Potato - software adapter or budget <= 1.5 GB; Low - UMA or budget <= 3 GB; Medium - <= 6 GB;
    // High - more. Unknown (no device yet) - Medium. !driver_cl is a separate flag (R9), not a tier.
    // UMA is checked before the 1.5 GB budget: the "dedicated" memory of an iGPU is a small carve-out (128 MB-1 GB)
    // that says nothing about it, the RAM axis covers its memory.
    if (!t.gpu_known)
        t.gpu = Tier::Medium;
    else
    {
        const u64 vram = HwPolicyVram(p.gpu);
        if (p.gpu.software)
            t.gpu = Tier::Potato;
        else if (p.gpu.uma)
            t.gpu = Tier::Low;
        else if (vram && vram * 2 <= 3 * kHwPolicyGiB)
            t.gpu = Tier::Potato;
        else if (vram && vram <= 3 * kHwPolicyGiB)
            t.gpu = Tier::Low;
        else if (!vram || vram <= 6 * kHwPolicyGiB)
            t.gpu = Tier::Medium;
        else
            t.gpu = Tier::High;
    }

    t.overall = std::min({t.cpu, t.ram, t.disk, t.gpu});
    return t;
}

// Stage B warning: Windows and Linux only - macOS grows its swap files on demand, the limit there is a lower estimate
u64 CommitLimitWanted(const Profile& p)
{
    if (p.env.os != Os::Windows && p.env.os != Os::Linux)
        return 0;
    const u64 commit = p.mem.commit_limit;
    if (!commit || HwPolicyAtLeastGiB(commit, kHwPolicyCommitWarnGiB))
        return 0;
    return u64(kHwPolicyCommitWarnGiB) * kHwPolicyGiB;
}
} // namespace detail

Policy ComputePolicy(const Profile& p)
{
    Policy r{};
    r.size = sizeof(Policy);
    r.version = POLICY_VERSION;

    const Tiers t = detail::ComputeTiers(p);
    const bool gpu_known = t.gpu_known != 0;
    const u32 logical = HwPolicyLogical(p);
    const u32 phys = HwPolicyPhysical(p);
    const bool smt = p.cpu.smt || logical > phys;
    const bool on_battery = p.env.on_battery == 1;
    const u64 ram = p.mem.ram_total;
    const u64 commit = p.mem.commit_limit;
    const bool uma = gpu_known && p.gpu.uma;
    const u64 vram = gpu_known ? HwPolicyVram(p.gpu) : 0;
    const bool low_vram = HwPolicyLowVram(p);

    // R8: worker pool. >= 12 available logical threads - as now (logical - 1, checked on the tester's machine), but not
    // above 23 (never measured). Fewer: the physical cores minus main, half of the SMT siblings, and on <= 4 cores two
    // more for the Game and Physics threads; at least 1.
    u32 pool;
    if (logical >= 12)
        pool = std::min(logical - 1, kHwPolicyPoolMax);
    else
    {
        s32 n = s32(phys) - 1 + (logical > phys ? s32(logical - phys) / 2 : 0);
        if (phys <= 4)
            n -= 2;
        pool = u32(std::max(n, 1));
    }
    r.pool_workers = u8(pool);

    // R3: model load threads - min(pool, 8) as the prefetch does now (min(hw - 1, 8), < 2 -> none); without
    // DriverConcurrentCreates the resources are created sequentially (as on GL).
    u32 load = std::min(pool, kHwPolicyLoadThreadsMax);
    if (load < 2 || (gpu_known && !p.gpu.driver_cc))
        load = 0;
    r.load_threads = u8(load);

    // R3: warm-up readers by the disk of gamedata. NVMe - as now (hw - 1 - load), at most 16; SATA - 4; slow - 1 in
    // list order (4-9 readers on an HDD give 20-40 MB/s); unknown - 2 plus the measurement of stage 1.
    switch (t.disk_class)
    {
    case DiskClass::Fast: r.warm_threads = u8(std::clamp(s32(logical) - 1 - s32(load), 1, 16)); break;
    case DiskClass::Sata: r.warm_threads = u8(std::clamp(s32(logical) - 1, 1, 4)); break;
    case DiskClass::Slow: r.warm_threads = 1; break;
    default: r.warm_threads = 2; break;
    }

    // R1: prefetch size by RAM and commit limit. full: RAM >= 24 and commit >= 32 and not (UMA with RAM < 32);
    // level: RAM < 12, or commit < 16, or UMA with RAM < 16; core otherwise. Pagefile on a slow disk - one step lower.
    // Full = +9.5 GB of commit after the prefetch, 18.5 in a fight; OS and background ~3-4 GB.
    if (!ram)
        r.prefetch_mode = PrefetchMode::Core;
    else if (HwPolicyAtLeastGiB(ram, 24) && (!commit || HwPolicyAtLeastGiB(commit, 32)) &&
        !(uma && !HwPolicyAtLeastGiB(ram, 32)))
        r.prefetch_mode = PrefetchMode::Full;
    else if (!HwPolicyAtLeastGiB(ram, 12) || (commit && !HwPolicyAtLeastGiB(commit, 16)) ||
        (uma && !HwPolicyAtLeastGiB(ram, 16)))
        r.prefetch_mode = PrefetchMode::Level;
    else
        r.prefetch_mode = PrefetchMode::Core;
    if (p.mem.swap_bytes && HwPolicySlowKind(p.data.swap.kind) && r.prefetch_mode != PrefetchMode::Level)
        r.prefetch_mode = r.prefetch_mode == PrefetchMode::Full ? PrefetchMode::Core : PrefetchMode::Level;

    // R1 variant B (decision 11.1): the engine takes the prefix of the priority list up to
    // min(0.35 * RAM - 2 GB, commit headroom - game peak without the prefetch (8 GB) - reserve (2 GB)): the prefetch
    // must fit into the system commit headroom at the start together with the game's own peak (plan 50, section 18;
    // was headroom - 6 GB). Ignored in Full. Never below the budget of an 8 GB machine (decision of 2026-10-18, plan 50
    // section 14): machines with less RAM are not targeted, a zero budget is out.
    {
        constexpr s64 by_ram_floor = s64(8 * kHwPolicyGiB * 35 / 100) - s64(2 * kHwPolicyGiB);
        const s64 by_ram = s64(double(ram) * 0.35) - s64(2 * kHwPolicyGiB);
        s64 budget = by_ram;
        if (p.mem.commit_avail_start)
        {
            budget = std::min(budget, s64(p.mem.commit_avail_start) - s64(kHwPolicyGameCommitPeakNoPrefetch) -
                    s64(kHwPolicyCommitReserve));
        }
        r.prefetch_budget = u64(std::max(budget, by_ram_floor));
    }

    // R2: commit headroom insurance at a level load: trim the pool below max(4 GB, 15 % of the limit)
    r.commit_headroom_min = std::max<u64>(4 * kHwPolicyGiB, commit / 100 * 15);

    // R9: parallel recording of the main render and the SVP worker. Without DriverCommandLists (the runtime emulates
    // the command lists) or with a pool < 3 - sequential (1); else parts = min(user, pool + 1).
    if ((gpu_known && !p.gpu.driver_cl) || pool < 3)
    {
        r.main_parallel_parts = 1;
        r.svp_worker = 0;
    }
    else
    {
        r.main_parallel_parts = u8(std::min<u32>(pool + 1, kHwPolicyMainPartsMax));
        r.svp_worker = 1;
    }

    // R10: on <= 4 available threads the driver's internal thread competes with main
    r.prevent_internal_threads = logical <= 4 ? 1 : 0;

    // R11: bounded spin with a small pool, SMT on <= 4 cores, or on battery; long (as now) otherwise
    r.spin_class = (pool <= 6 || (smt && phys <= 4) || on_battery) ? SpinClass::Short : SpinClass::Long;

    // R12: SVP atlas pages by the video memory: <= 3 GB or UMA - 2, <= 6 GB - 4, more - as now (8)
    if (low_vram)
        r.svp_pages = 2;
    else if (vram && vram <= 6 * kHwPolicyGiB)
        r.svp_pages = 4;
    else
        r.svp_pages = kHwPolicySvpPagesDefault;

    // R13: texture_lod auto - one step more (1/4 of the memory) at <= 3 GB or UMA; shadow map 1024 there;
    // weapon icons min(192, budget / 16) MB
    r.texture_lod_auto = u8(kHwPolicyTextureLodDefault + (low_vram ? 1 : 0));
    r.smap_size_auto = low_vram ? 1024 : 0;
    r.icons_mb = u16(vram ? std::min<u64>(192, vram / kHwPolicyMiB / 16) : 192);

    // R18: PCM cache of the sound: RAM < 12 GB - 40 MB, 12..24 - 64, more - 96 (the engine default)
    if (!ram || HwPolicyAtLeastGiB(ram, 24))
        r.snd_cache_mb_auto = 96;
    else if (HwPolicyAtLeastGiB(ram, 12))
        r.snd_cache_mb_auto = 64;
    else
        r.snd_cache_mb_auto = 40;

    // R17: Lua GC pace - x2 on a weak CPU costs 2-3 times more
    r.lua_paced_mul = phys <= 4 ? 100 : 200;
    // R16: scheduler budget - preemption burns the wall-clock budget on <= 4 cores
    r.shed_budget_ms = phys <= 4 ? 5 : 8;
    // R5: model_prefetch_margin - an HDD needs a bigger lead
    r.model_prefetch_margin = t.disk_class == DiskClass::Slow ? 50 : 25;
    // R6: time limit of cold model loads per frame; 0 = the spawn waits for the background warm-up (slow disk)
    switch (t.disk_class)
    {
    case DiskClass::Fast: r.spawn_cold_ms = 8; break;
    case DiskClass::Slow: r.spawn_cold_ms = 0; break;
    default: r.spawn_cold_ms = 6; break;
    }
    // R7: one background I/O queue on a slow disk
    r.io_single_queue = t.disk_class == DiskClass::Slow ? 1 : 0;

    // 50l-weak-cpu-spawn-models.md (plans/optimization_mt): a weak CPU - a cold model load costs 100-350 ms there (the
    // laptop of 2026-10-19, half of them with the files in the OS cache: CPU). The cold loads of the spawns are spaced
    // out (one per 3 frames on 3-4 cores, per 4 on 2), and the base models of the creatures about to come online are
    // loaded on a thread of their own. That thread creates the D3D resources (buffers, shaders, textures) off the main
    // thread: D3D11 with DriverConcurrentCreates only (GL makes the objects of its context on that thread).
    const bool weak_cpu = t.cpu <= Tier::Low;
    r.spawn_cold_frames = t.cpu == Tier::Potato ? 4 : weak_cpu ? 3 : 0;
    r.model_bg_load = weak_cpu && gpu_known && p.gpu.api == GpuApi::D3D11 && p.gpu.driver_cc ? 1 : 0;

    return r;
}
} // namespace xray::hw
