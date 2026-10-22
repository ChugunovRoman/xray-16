#include "stdafx.h"
#pragma hdrstop

#include "xrCore/Platform/HwProfile_impl.h"
#include "Debug/xrSentry.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

// Stages, -hw_* keys, the "* [hw]" log lines and -hw_selftest of the hardware profile
// (plans/optimization_mt/50-smart-engine-hw-profile.md, section 4.2). Collectors: HwProfile_<platform>.cpp,
// thresholds: HwPolicy.cpp.

namespace xray::hw
{
namespace
{
constexpr double kHwGiB = double(u64(1) << 30);
constexpr double kHwMiB = double(u64(1) << 20);

Profile s_hw_raw{}; // as collected
Profile s_hw_profile{}; // raw + -hw_* overrides, what GetProfile returns
Policy s_hw_policy{};
std::atomic<u32> s_hw_stages{0};

// R11 + R14 (plan 50g): the spin class TaskManager reads on every wait (SpinClass as u8). Written by the main thread
// only (stage A, PollPower).
std::atomic<u8> s_hw_spin_effective{u8(SpinClass::Long)};
// Main thread only
SpinClass s_hw_spin_ac = SpinClass::Long; // the policy's class with the power conditions off
bool s_hw_power_poll = false; // a battery (or an unknown one) and no -hw_battery
s32 s_hw_power_state = -1; // last applied: bit 0 - on battery, bit 1 - saver; -1 - none yet
std::chrono::steady_clock::time_point s_hw_power_next{};
constexpr auto kHwPowerPollPeriod = std::chrono::seconds(5);

struct HwOverrides
{
    u32 mask;
    s32 profile_class; // Tier, -1 = none
    u64 ram;
    u64 commit;
    u16 physical;
    u16 logical;
    DiskKind disk;
    u64 vram;
};
HwOverrides s_hw_ovr{0, -1, 0, 0, 0, 0, DiskKind::Unknown, 0};
bool s_hw_cl_gl_logged = false; // "-hw_cl: no effect on GL" once per session (vid_restart submits again)

// Typical facts of a class for -hw_profile (each one gives exactly that class on every axis)
struct HwClassFacts
{
    u16 physical, logical;
    u32 ram_gb, commit_gb;
    DiskKind disk;
    u32 vram_mb;
    u8 uma, driver_cl;
};
constexpr HwClassFacts kHwClassFacts[] =
{
    { 2,  4,  4,  6, DiskKind::Hdd,     1024, 0, 0 }, // potato
    { 4,  8,  8, 12, DiskKind::Hdd,     2048, 0, 0 }, // low
    { 6, 12, 16, 24, DiskKind::SataSsd, 6144, 0, 1 }, // medium (6c/12t: the CPU tier counts the physical cores)
    { 12, 24, 32, 48, DiskKind::Nvme,  12288, 0, 1 }, // high
};

float HwElapsedMs(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - since).count();
}

// "-key" as a whole word of the command line; the value (optional) is the next word.
// Whole-word match: none of the -hw_* keys may be taken for a prefix of another one.
bool HwFindKey(pcstr key, char* value = nullptr, size_t value_size = 0)
{
    pcstr params = Core.Params;
    if (!params)
        return false;
    const size_t len = xr_strlen(key);
    for (pcstr p = strstr(params, key); p; p = strstr(p + 1, key))
    {
        const bool starts = p == params || p[-1] == ' ' || p[-1] == '\t';
        const char end = p[len];
        if (!starts || (end != 0 && end != ' ' && end != '\t'))
            continue;
        if (value && value_size)
        {
            pcstr v = p + len;
            while (*v == ' ' || *v == '\t')
                ++v;
            size_t n = 0;
            while (v[n] && v[n] != ' ' && v[n] != '\t' && n + 1 < value_size)
            {
                value[n] = v[n];
                ++n;
            }
            value[n] = 0;
        }
        return true;
    }
    return false;
}

struct HwDiskKeyName
{
    pcstr name;
    DiskKind kind;
};
constexpr HwDiskKeyName kHwDiskKeyNames[] = { { "hdd", DiskKind::Hdd }, { "ssd", DiskKind::SataSsd },
    { "nvme", DiskKind::Nvme }, { "usb", DiskKind::Removable }, { "net", DiskKind::Network } };

void HwParseOverrides()
{
    HwOverrides& o = s_hw_ovr;
    string64 value;

    if (HwFindKey("-hw_profile", value, sizeof(value)))
    {
        for (s32 i = 0; i <= s32(Tier::High); ++i)
        {
            if (!xr_stricmp(value, TierName(Tier(i))))
                o.profile_class = i;
        }
        if (o.profile_class >= 0)
            o.mask |= OVR_PROFILE;
        else
            Msg("! [hw] -hw_profile: unknown class '%s' (potato|low|medium|high)", value);
    }
    if (HwFindKey("-hw_ram", value, sizeof(value)))
    {
        const double gb = atof(value);
        if (gb > 0.0)
        {
            o.ram = u64(gb * kHwGiB);
            o.mask |= OVR_RAM;
        }
        else
            Msg("! [hw] -hw_ram: bad value '%s' (GB)", value);
    }
    if (HwFindKey("-hw_commit", value, sizeof(value)))
    {
        const double gb = atof(value);
        if (gb > 0.0)
        {
            o.commit = u64(gb * kHwGiB);
            o.mask |= OVR_COMMIT;
        }
        else
            Msg("! [hw] -hw_commit: bad value '%s' (GB)", value);
    }
    if (HwFindKey("-hw_cpu", value, sizeof(value)))
    {
        unsigned phys = 0, logical = 0;
        const int n = sscanf(value, "%u/%u", &phys, &logical);
        if (n >= 1 && phys > 0 && phys < 1024)
        {
            o.physical = u16(phys);
            o.logical = u16(n == 2 && logical >= phys && logical < 4096 ? logical : phys);
            o.mask |= OVR_CPU;
        }
        else
            Msg("! [hw] -hw_cpu: bad value '%s' (<phys>[/<logical>])", value);
    }
    if (HwFindKey("-hw_disk", value, sizeof(value)))
    {
        for (const HwDiskKeyName& k : kHwDiskKeyNames)
        {
            if (!xr_stricmp(value, k.name))
            {
                o.disk = k.kind;
                o.mask |= OVR_DISK;
            }
        }
        if (!(o.mask & OVR_DISK))
            Msg("! [hw] -hw_disk: unknown kind '%s' (hdd|ssd|nvme|usb|net)", value);
    }
    if (HwFindKey("-hw_vram", value, sizeof(value)))
    {
        const double mb = atof(value);
        if (mb > 0.0)
        {
            o.vram = u64(mb * kHwMiB);
            o.mask |= OVR_VRAM;
        }
        else
            Msg("! [hw] -hw_vram: bad value '%s' (MB)", value);
    }
    if (HwFindKey("-hw_igpu"))
        o.mask |= OVR_IGPU;
    if (HwFindKey("-hw_nocl"))
        o.mask |= OVR_NOCL;
    if (HwFindKey("-hw_cl"))
    {
        if (o.mask & OVR_NOCL)
            Msg("! [hw] -hw_cl: ignored together with -hw_nocl");
        else
            o.mask |= OVR_CL;
    }
    if (HwFindKey("-hw_battery"))
        o.mask |= OVR_BATTERY;
}

void HwSetCpu(Profile& p, u16 physical, u16 logical)
{
    p.cpu.physical = p.cpu.physical_avail = physical;
    p.cpu.logical_total = p.cpu.logical_avail = logical;
    p.cpu.smt = logical > physical ? 1 : 0;
    p.cpu.hybrid = 0;
    p.cpu.perf_cores = p.cpu.eff_cores = 0;
}

void HwSetRam(Profile& p, u64 ram)
{
    // Less RAM, the same pagefile - unless -hw_commit says otherwise
    p.mem.ram_total = ram;
    p.mem.ram_installed = 0;
    p.mem.ram_avail_start = std::min(p.mem.ram_avail_start, ram * 7 / 10);
    p.mem.commit_limit = ram + p.mem.swap_bytes;
    p.mem.commit_avail_start = std::min(p.mem.commit_avail_start, p.mem.commit_limit * 6 / 10);
}

void HwSetCommit(Profile& p, u64 commit)
{
    p.mem.commit_limit = std::max(commit, p.mem.ram_total);
    p.mem.swap_bytes = p.mem.commit_limit - p.mem.ram_total;
    p.mem.commit_avail_start = std::min(p.mem.commit_avail_start, p.mem.commit_limit * 6 / 10);
}

// -hw_limit -hw_commit: the Job Object limit is real and may be below the RAM. The collector already reports it (the
// Windows one reads the job), this keeps it after -hw_ram / -hw_profile changed the RAM.
void HwSetCommitLimited(Profile& p, u64 commit)
{
    p.mem.commit_limit = commit;
    p.mem.swap_bytes = commit > p.mem.ram_total ? commit - p.mem.ram_total : 0;
    p.mem.commit_avail_start = std::min(p.mem.commit_avail_start, commit);
}

void HwSetDisk(Disk& d, DiskKind kind)
{
    d.kind = kind;
    d.seek_penalty = kind == DiskKind::Hdd ? 1 : (kind == DiskKind::SataSsd || kind == DiskKind::Nvme ? 0 : VALUE_UNKNOWN);
    d.bus = VALUE_UNKNOWN;
}

void HwSetVram(Profile& p, u64 vram)
{
    p.gpu.dedicated = vram;
    p.gpu.budget_local_start = vram;
}

void HwApplyOverrides(Profile& p)
{
    const HwOverrides& o = s_hw_ovr;
    // -hw_limit restricted the affinity for real: the collected CPU values are the limited ones, keep them
    const bool cpu_real = (o.mask & OVR_LIMIT_CPU) != 0;
    if (o.mask & OVR_PROFILE)
    {
        const HwClassFacts& f = kHwClassFacts[o.profile_class];
        if (!cpu_real)
            HwSetCpu(p, f.physical, f.logical);
        p.mem.swap_bytes = u64(f.commit_gb - f.ram_gb) << 30;
        HwSetRam(p, u64(f.ram_gb) << 30);
        HwSetDisk(p.data.game, f.disk);
        if (p.data.swap.same_as == 1)
            HwSetDisk(p.data.swap, f.disk);
        HwSetVram(p, u64(f.vram_mb) << 20);
        p.gpu.uma = f.uma;
        p.gpu.software = 0;
        p.gpu.driver_cl = p.gpu.driver_cc = f.driver_cl;
    }
    if ((o.mask & OVR_CPU) && !cpu_real)
        HwSetCpu(p, o.physical, o.logical);
    if (o.mask & OVR_RAM)
        HwSetRam(p, o.ram);
    if (o.mask & OVR_COMMIT)
    {
        if (o.mask & OVR_LIMIT_COMMIT)
            HwSetCommitLimited(p, o.commit);
        else
            HwSetCommit(p, o.commit);
    }
    if (o.mask & OVR_DISK)
    {
        HwSetDisk(p.data.game, o.disk);
        if (p.data.swap.same_as == 1)
            HwSetDisk(p.data.swap, o.disk);
    }
    if (o.mask & OVR_VRAM)
        HwSetVram(p, o.vram);
    if (o.mask & OVR_IGPU)
        p.gpu.uma = 1;
    if (o.mask & OVR_NOCL)
        p.gpu.driver_cl = 0;
    // -hw_cl: D3D11 only - the runtime emulates the deferred contexts on a driver without command lists, so the parallel
    // recording still works there (the A/B test: does it unload main on a 4c/8t CPU). GL has no deferred contexts at
    // all. DriverConcurrentCreates stays real: it is a separate driver capability (R3, the background loading), the
    // recording does not need it - the device is free-threaded for the creates, without it the runtime serializes them.
    if ((o.mask & OVR_CL) && p.gpu.api == GpuApi::D3D11)
        p.gpu.driver_cl = 1;
    if (o.mask & OVR_BATTERY)
        p.env.has_battery = p.env.on_battery = 1;
}

// -hw_limit: the real limits from -hw_cpu (affinity) and -hw_commit (Job Object), applied before stage A collects so
// the collectors and the policy see the limited process. Only the explicit keys count: a -hw_profile class alone does
// not limit anything (its 6 GB commit of potato would just make the game run out of memory on a level load).
void HwApplyLimits()
{
    HwOverrides& o = s_hw_ovr;
    const u32 logical = (o.mask & OVR_CPU) ? o.logical : 0;
    const u32 physical = (o.mask & OVR_CPU) ? o.physical : 0;
    const u64 commit = (o.mask & OVR_COMMIT) ? o.commit : 0;
    if (!logical && !commit)
    {
        Msg("! [hw] -hw_limit: nothing to limit (give -hw_cpu <phys>[/<logical>] and/or -hw_commit <GB>)");
        return;
    }
    o.mask |= detail::PlatformApplyLimits(physical, logical, commit) & (OVR_LIMIT_CPU | OVR_LIMIT_COMMIT);
}

void HwRecompute()
{
    Profile p = s_hw_raw;
    p.size = sizeof(Profile);
    p.version = PROFILE_VERSION;
    p.stages = s_hw_stages.load(std::memory_order_relaxed);
    p.overridden = s_hw_ovr.mask;
    HwApplyOverrides(p);
    p.tiers = detail::ComputeTiers(p);
    s_hw_profile = p;
    s_hw_policy = ComputePolicy(p);
}

void HwInitUnknown(Profile& p)
{
    p = Profile{};
    p.size = sizeof(Profile);
    p.version = PROFILE_VERSION;
    for (Disk* d : { &p.data.game, &p.data.appdata, &p.data.system, &p.data.swap })
    {
        d->seek_penalty = VALUE_UNKNOWN;
        d->bus = VALUE_UNKNOWN;
    }
    p.mem.swap_sys_managed = VALUE_UNKNOWN;
    p.env.has_battery = p.env.on_battery = p.env.battery_saver = VALUE_UNKNOWN;
    p.data.shader_cache_cold = VALUE_UNKNOWN;
#if defined(XR_PLATFORM_WINDOWS)
    p.env.os = Os::Windows;
#elif defined(XR_PLATFORM_LINUX)
    p.env.os = Os::Linux;
#elif defined(XR_PLATFORM_APPLE)
    p.env.os = Os::Apple;
#elif defined(XR_PLATFORM_BSD)
    p.env.os = Os::Bsd;
#else
    p.env.os = Os::Other;
#endif
}

// Cross-platform baseline from SDL (no SDL_Init needed); the platform collectors refine it
void HwCollectSdlBaseline(Profile& p)
{
    const int cpus = SDL_GetCPUCount();
    if (cpus > 0)
        p.cpu.logical_total = p.cpu.logical_avail = u16(cpus);
    const int ram_mb = SDL_GetSystemRAM();
    if (ram_mb > 0)
        p.mem.ram_total = u64(ram_mb) << 20;

#if !defined(XR_PLATFORM_WINDOWS) && !defined(XR_PLATFORM_LINUX)
    // Windows and Linux read the power state themselves: SDL may go through D-Bus (UPower) on Linux, and SDL is not
    // initialized yet at stage A
    int seconds = 0, percent = 0;
    switch (SDL_GetPowerInfo(&seconds, &percent))
    {
    case SDL_POWERSTATE_NO_BATTERY:
        p.env.has_battery = 0;
        p.env.on_battery = 0;
        break;
    case SDL_POWERSTATE_ON_BATTERY:
        p.env.has_battery = 1;
        p.env.on_battery = 1;
        break;
    case SDL_POWERSTATE_CHARGING:
    case SDL_POWERSTATE_CHARGED:
        p.env.has_battery = 1;
        p.env.on_battery = 0;
        break;
    default: break;
    }
#endif
}

//------------------------------------------------------------------------------------------------------------------------
// Log lines

struct HwLine
{
    char text[1024];
    size_t len;

    HwLine() : len(0) { text[0] = 0; }

    void add(pcstr fmt, ...)
    {
        if (len + 1 >= sizeof(text))
            return;
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(text + len, sizeof(text) - len, fmt, args);
        va_end(args);
        if (n > 0)
            len = std::min(len + size_t(n), sizeof(text) - 1);
    }
};

pcstr HwMark(const Profile& p, u32 ovr) { return (p.overridden & ovr) ? "*" : ""; }

pcstr HwDiskClassName(DiskClass c)
{
    switch (c)
    {
    case DiskClass::Slow: return "slow";
    case DiskClass::Sata: return "sata";
    case DiskClass::Fast: return "fast";
    default: return "unknown";
    }
}

pcstr HwPrefetchName(PrefetchMode m)
{
    switch (m)
    {
    case PrefetchMode::Full: return "full";
    case PrefetchMode::Core: return "core";
    default: return "level";
    }
}

void HwFormatCpu(HwLine& l, const Profile& p)
{
    const Cpu& c = p.cpu;
    // -hw_limit: real (limited) values, not emulated - no '*'
    const pcstr m = (p.overridden & OVR_LIMIT_CPU) ? "" : HwMark(p, OVR_CPU | OVR_PROFILE);
    l.add("cpu %u/%u thr%s, ", c.logical_avail, c.logical_total, m);
    if (c.physical_avail != c.physical)
        l.add("%u/%u phys%s", c.physical_avail, c.physical, m);
    else
        l.add("%u phys%s", c.physical, m);
    l.add("%s, hybrid ", c.smt ? " SMT" : "");
    if (c.hybrid)
        l.add("%uP+%uE", c.perf_cores, c.eff_cores);
    else
        l.add("0");
    if (c.groups > 1)
        l.add(", %u groups", c.groups);
    if (c.brand[0])
        l.add(" (%s)", c.brand);
    if (p.overridden & OVR_LIMIT_CPU)
        l.add(", -hw_limit affinity");
}

void HwFormatMem(HwLine& l, const Profile& p)
{
    const Mem& m = p.mem;
    const pcstr mr = HwMark(p, OVR_RAM | OVR_PROFILE);
    // -hw_limit -hw_commit: a real (Job Object) limit, not emulated - no '*'
    const pcstr mc = (p.overridden & OVR_LIMIT_COMMIT) ? "" : HwMark(p, OVR_COMMIT | OVR_RAM | OVR_PROFILE);
    l.add("ram %.1f GB%s (avail %.1f", m.ram_total / kHwGiB, mr, m.ram_avail_start / kHwGiB);
    if (m.ram_installed && m.ram_installed > m.ram_total + (u64(256) << 20))
        l.add(", installed %.1f", m.ram_installed / kHwGiB);
    l.add("), commit %.1f GB%s (avail %.1f", m.commit_limit / kHwGiB, mc, m.commit_avail_start / kHwGiB);
    switch (m.swap_kind)
    {
    case SwapKind::None: l.add(", no swap"); break;
    case SwapKind::Zram: l.add(", zram %.1f", m.swap_bytes / kHwGiB); break;
    default:
        l.add(", swap ~%.1f", m.swap_bytes / kHwGiB);
        if (m.swap_volume[0])
            l.add(" %s", m.swap_volume);
        if (m.swap_sys_managed != VALUE_UNKNOWN)
            l.add(m.swap_sys_managed ? " sys-managed" : " fixed");
        break;
    }
    if (p.overridden & OVR_LIMIT_COMMIT)
        l.add(", -hw_limit job");
    l.add(")");
}

void HwFormatEnv(HwLine& l, const Profile& p)
{
    const Env& e = p.env;
    switch (e.os)
    {
    case Os::Windows: l.add("win %u.%u.%u", e.os_major, e.os_minor, e.os_build); break;
    case Os::Linux: l.add("linux %s", e.os_name[0] ? e.os_name : "?"); break;
    case Os::Apple: l.add("macos %s", e.os_name[0] ? e.os_name : "?"); break;
    case Os::Bsd: l.add("bsd %s", e.os_name); break;
    default: l.add("os ?"); break;
    }
    if (e.wine)
        l.add(", wine %s", e.wine_version[0] ? e.wine_version : "?");
    if (e.steam_deck)
        l.add(", steam deck");
    const pcstr mb = HwMark(p, OVR_BATTERY);
    if (e.has_battery == VALUE_UNKNOWN)
        l.add(", power ?");
    else if (!e.has_battery)
        l.add(", AC");
    else if (e.on_battery == 1)
        l.add(", on battery%s", mb);
    else
        l.add(", AC (battery)%s", mb);
    if (e.battery_saver == 1)
        l.add(", battery saver");
}

void HwFormatDisk(HwLine& l, const Profile& p, pcstr label, const Disk& d, bool game)
{
    static constexpr pcstr same_names[] = { "", "game", "appdata", "system" };
    l.add("%s ", label);
    if (d.volume[0])
        l.add("%s ", d.volume);
    if (d.same_as && d.same_as < 4)
    {
        l.add("same as %s", same_names[d.same_as]);
        return;
    }
    l.add("%s%s", DiskKindName(d.kind), game ? HwMark(p, OVR_DISK | OVR_PROFILE) : "");
    if (d.seek_penalty != VALUE_UNKNOWN || d.bus != VALUE_UNKNOWN)
    {
        l.add(" (");
        if (d.seek_penalty != VALUE_UNKNOWN)
            l.add("seek %u", d.seek_penalty);
        else
            l.add("seek ?");
        if (d.bus != VALUE_UNKNOWN)
            l.add(", bus %u", d.bus);
        if (d.removable)
            l.add(", removable");
        l.add(")");
    }
    if (d.total_bytes)
        l.add(", free %.1f of %.1f GB", d.free_bytes / kHwGiB, d.total_bytes / kHwGiB);
}

void HwFormatGpu(HwLine& l, const Profile& p)
{
    const Gpu& g = p.gpu;
    l.add("gpu %s", g.name[0] ? g.name : "?");
    if (g.vendor || g.device)
        l.add(" (%04X:%04X)", g.vendor, g.device);
    switch (g.api)
    {
    case GpuApi::D3D11: l.add(" D3D11 FL%u_%u", (g.feature_level >> 12) & 0xF, (g.feature_level >> 8) & 0xF); break;
    case GpuApi::OpenGL: l.add(" GL %u.%u", (g.feature_level >> 12) & 0xF, (g.feature_level >> 8) & 0xF); break;
    default: break;
    }
    const pcstr mv = HwMark(p, OVR_VRAM | OVR_PROFILE);
    l.add(", VRAM %.0f MB%s", g.dedicated / kHwMiB, mv);
    if (g.budget_local_start)
        l.add(" (budget %.0f%s)", g.budget_local_start / kHwMiB, mv);
    if (g.shared)
        l.add(", shared %.0f MB", g.shared / kHwMiB);
    // -hw_cl changes D3D11 only (HwApplyOverrides): no '*' on a GL line
    const u32 cl_ovr = OVR_NOCL | OVR_PROFILE | (g.api == GpuApi::D3D11 ? OVR_CL : 0);
    l.add(", uma %u%s, CL %u%s/%u", g.uma, HwMark(p, OVR_IGPU | OVR_PROFILE), g.driver_cl, HwMark(p, cl_ovr),
        g.driver_cc);
    if (g.software)
        l.add(", SOFTWARE");
    if (g.adapters > 1)
        l.add(", adapters %u%s", g.adapters, g.better_adapter ? " (a stronger one exists)" : "");
    if (g.width && g.height)
        l.add(", %ux%u@%u", g.width, g.height, g.refresh);
}

void HwFormatTiersPolicy(HwLine& l, const Tiers& t, const Policy& r)
{
    l.add("tiers cpu %s, ram %s, disk %s, gpu %s%s -> %s; ", TierName(t.cpu), TierName(t.ram),
        HwDiskClassName(t.disk_class), TierName(t.gpu), t.gpu_known ? "" : "(?)", TierName(t.overall));
    l.add("policy prefetch %s", HwPrefetchName(r.prefetch_mode));
    if (r.prefetch_mode != PrefetchMode::Full)
        l.add(" %.1f GB", r.prefetch_budget / kHwGiB);
    l.add(", warm %u, load %u, pool %u, spin %s, main_par %u, svp %u%s, texture_lod %u", r.warm_threads,
        r.load_threads, r.pool_workers, r.spin_class == SpinClass::Short ? "short" : "long", r.main_parallel_parts,
        r.svp_pages, r.svp_worker ? "" : " (no worker)", r.texture_lod_auto);
    if (r.smap_size_auto)
        l.add(", smap %u", r.smap_size_auto);
    l.add(", icons %u MB, snd_cache %u, lua_paced %u, shed %u ms, margin %u, spawn_cold %u ms, io %s, "
          "commit_trim < %.1f GB%s",
        r.icons_mb, r.snd_cache_mb_auto, r.lua_paced_mul, r.shed_budget_ms, r.model_prefetch_margin,
        r.spawn_cold_ms, r.io_single_queue ? "single" : "split", r.commit_headroom_min / kHwGiB,
        r.prevent_internal_threads ? ", prevent_internal_threads" : "");
}

void HwLogCpuLine(const Profile& p)
{
    HwLine l;
    HwFormatCpu(l, p);
    l.add(" | ");
    HwFormatMem(l, p);
    l.add(" | ");
    HwFormatEnv(l, p);
    Msg("* [hw] %s", l.text);
}

void HwLogDiskLine(const Profile& p)
{
    HwLine l;
    HwFormatDisk(l, p, "disk game", p.data.game, true);
    l.add("; ");
    HwFormatDisk(l, p, "appdata", p.data.appdata, false);
    l.add("; ");
    HwFormatDisk(l, p, "system", p.data.system, false);
    if (p.data.swap.volume[0])
    {
        l.add("; ");
        HwFormatDisk(l, p, "pagefile", p.data.swap, false);
    }
    // loose_files = the whole FS index (files inside archives included; with 0 archives all of them are loose)
    l.add("; data %u files, %u archives | A %.1f ms, B %.1f ms", p.data.loose_files, p.data.archives,
        p.stage_a_ms, p.stage_b_ms);
    Msg("* [hw] %s", l.text);
}

void HwLogGpuLine(const Profile& p, bool probe)
{
    HwLine l;
    HwFormatGpu(l, p);
    Msg("* [hw] %s%s", l.text, probe ? " (startup probe)" : "");
}

void HwLogTiersPolicyLine(const Profile& p, const Policy& r)
{
    HwLine l;
    HwFormatTiersPolicy(l, p.tiers, r);
    Msg("* [hw] %s", l.text);
}

// Stage B: the commit limit is too small for a heavy save (plan 50, section 18; the threshold - HwPolicy.cpp). The
// suggested pagefile / swap size brings the commit limit to the wanted one. Not under -hw_limit -hw_commit: that limit
// is a deliberate test.
void HwLogCommitWarning(const Profile& p)
{
    const u64 wanted = detail::CommitLimitWanted(p);
    if (!wanted || (p.overridden & OVR_LIMIT_COMMIT))
        return;
    const Mem& m = p.mem;
    const bool pagefile = p.env.os == Os::Windows && !p.env.wine; // Wine reports the RAM + swap of Linux
    const pcstr name = pagefile ? "pagefile" : "swap";
    const u64 need = wanted > m.ram_total ? wanted - m.ram_total : 0;
    const u32 need_gb = std::max<u32>(u32((need + (u64(1) << 30) - 1) >> 30), 1);
    HwLine l;
    l.add("commit limit %.1f GB%s (", m.commit_limit / kHwGiB, HwMark(p, OVR_COMMIT | OVR_RAM | OVR_PROFILE));
    if (m.swap_kind == SwapKind::None)
        l.add("no %s", name);
    else
    {
        l.add("%s ~%.1f GB", name, m.swap_bytes / kHwGiB);
        if (m.swap_volume[0])
            l.add(" %s", m.swap_volume);
        if (m.swap_sys_managed == 1)
            l.add(" sys-managed");
    }
    l.add("): heavy saves can run out of memory - %s %s %s >= %u GB (commit limit >= %.0f GB)",
        m.swap_kind == SwapKind::None ? "create a" : "enlarge the", name, m.swap_kind == SwapKind::None ? "of" : "to",
        need_gb, wanted / kHwGiB);
    if (pagefile && m.swap_sys_managed == 1)
        l.add("; a system-managed one grows only under memory pressure - set the size by hand");
    Msg("! [hw] %s", l.text);
}

// -hw_selftest: the policy over synthetic machines (no unit tests in the repository - this is their replacement)
struct HwSelfTestCase
{
    pcstr name;
    u16 physical, logical;
    float ram_gb, commit_gb;
    DiskKind disk;
    u32 vram_mb;
    u8 uma, software, driver_cl, on_battery;
    float commit_avail_gb; // 0 - 60 % of the limit
};

void HwRunSelfTest()
{
    constexpr HwSelfTestCase cases[] =
    {
        { "4c/8t 8GB HDD iGPU",          4,  8,  7.8f, 12.0f, DiskKind::Hdd,      2048, 1, 0, 0, 0 },
        { "6c/12t 16GB SATA 8GB",        6, 12, 15.4f, 24.8f, DiskKind::SataSsd,  7480, 0, 0, 1, 0 },
        { "12c/24t 32GB NVMe 12GB",     12, 24, 31.9f, 48.0f, DiskKind::Nvme,    11800, 0, 0, 1, 0 },
        { "Steam Deck 4c/8t 16GB UMA",   4,  8, 14.5f, 22.0f, DiskKind::Nvme,     1024, 1, 0, 1, 1 },
        { "potato 2c/2t 4GB HDD WARP",   2,  2,  3.9f,  6.0f, DiskKind::Hdd,       256, 0, 1, 0, 0 },
        { "Nittor 6c/12t 16GB+9 RTX5060", 6, 12, 15.9f, 24.8f, DiskKind::Nvme,    7480, 0, 0, 1, 0 },
        // the laptop of 2026-10-19 (plan 50, section 18): pagefile ~1.2 GB, as started and with less headroom. One disk
        // here, the one of its pagefile (SSD; the game is on USB there)
        { "laptop 4c/8t 16GB+1 Iris Xe", 4,  8, 15.8f, 17.0f, DiskKind::Ssd,      7429, 1, 0, 0, 0, 13.8f },
        { "laptop busy (commit avail 12)", 4, 8, 15.8f, 17.0f, DiskKind::Ssd,     7429, 1, 0, 0, 0, 12.0f },
    };
    for (const HwSelfTestCase& c : cases)
    {
        Profile p;
        HwInitUnknown(p);
        p.stages = STAGE_A | STAGE_B | STAGE_C;
        HwSetCpu(p, c.physical, c.logical);
        p.mem.ram_total = u64(double(c.ram_gb) * kHwGiB);
        p.mem.ram_avail_start = p.mem.ram_total * 7 / 10;
        p.mem.commit_limit = u64(double(c.commit_gb) * kHwGiB);
        p.mem.commit_avail_start = c.commit_avail_gb > 0.0f ? u64(double(c.commit_avail_gb) * kHwGiB)
                                                             : p.mem.commit_limit * 6 / 10;
        p.mem.swap_bytes = p.mem.commit_limit - std::min(p.mem.commit_limit, p.mem.ram_total);
        p.mem.swap_kind = p.mem.swap_bytes ? SwapKind::File : SwapKind::None;
        HwSetDisk(p.data.game, c.disk);
        p.data.swap = p.data.game;
        HwSetVram(p, u64(c.vram_mb) << 20);
        p.gpu.api = GpuApi::D3D11;
        p.gpu.uma = c.uma;
        p.gpu.software = c.software;
        p.gpu.driver_cl = p.gpu.driver_cc = c.driver_cl;
        p.env.has_battery = c.on_battery;
        p.env.on_battery = c.on_battery;
        p.tiers = detail::ComputeTiers(p);
        const Policy r = ComputePolicy(p);
        HwLine l;
        HwFormatTiersPolicy(l, p.tiers, r);
        Msg("* [hw] selftest %-30s %s%s", c.name, l.text,
            detail::CommitLimitWanted(p) ? ", commit limit warning" : "");
    }
}

// R14: the effective spin class from a power state; logs the state when it changed (the first call included)
void HwPowerApply(bool on_battery, bool saver, pcstr note)
{
    const s32 state = (on_battery ? 1 : 0) | (saver ? 2 : 0);
    if (state == s_hw_power_state)
        return;
    s_hw_power_state = state;
    const SpinClass spin = (on_battery || saver) ? SpinClass::Short : s_hw_spin_ac;
    s_hw_spin_effective.store(u8(spin), std::memory_order_relaxed);
    pcstr power = "AC";
    if (on_battery && saver)
        power = "on battery, battery saver";
    else if (on_battery)
        power = "on battery";
    else if (saver)
        power = "battery saver";
    // "(hardware)": on AC the class is the one of the CPU / pool (a 4-core machine is short on AC too), not the power's
    Msg("* [hw] power: %s -> spin %s%s%s", power, spin == SpinClass::Short ? "short" : "long",
        (on_battery || saver) ? "" : " (hardware)", note);
}

// R14, stage A: the class on AC and the start state. The policy's spin_class already counts on_battery at the start;
// the class on AC is computed again without it, so plugging the charger in gives the class of the hardware back.
void HwPowerSetup()
{
    Profile ac = s_hw_profile;
    ac.env.on_battery = 0;
    ac.env.battery_saver = 0;
    s_hw_spin_ac = ComputePolicy(ac).spin_class;

    const Env& e = s_hw_profile.env; // -hw_battery included
    const bool on_battery = e.on_battery == 1;
    const bool saver = e.battery_saver == 1;
    // A desktop without a battery is never polled; the emulated -hw_battery state stays for the whole session
    s_hw_power_poll = !(s_hw_ovr.mask & OVR_BATTERY) && s_hw_raw.env.has_battery != 0;
    s_hw_power_next = std::chrono::steady_clock::now() + kHwPowerPollPeriod;
    if (s_hw_power_poll || on_battery || saver)
        HwPowerApply(on_battery, saver, s_hw_power_poll ? " (polled every 5 s)" : "");
    else
        s_hw_spin_effective.store(u8(s_hw_spin_ac), std::memory_order_relaxed); // as the policy (tester: long)
}

void HwUpdateSentry()
{
    xrSentry_UpdateHwTags();
}
} // namespace

pcstr TierName(Tier tier)
{
    switch (tier)
    {
    case Tier::Potato: return "potato";
    case Tier::Low: return "low";
    case Tier::Medium: return "medium";
    case Tier::High: return "high";
    }
    return "?";
}

pcstr DiskKindName(DiskKind kind)
{
    switch (kind)
    {
    case DiskKind::Hdd: return "HDD";
    case DiskKind::SataSsd: return "SATA SSD";
    case DiskKind::Nvme: return "NVMe";
    case DiskKind::Removable: return "USB/removable";
    case DiskKind::Network: return "network";
    case DiskKind::Virtual: return "virtual";
    case DiskKind::Ssd: return "SSD";
    default: return "unknown";
    }
}

const Profile& GetProfile() { return s_hw_profile; }
const Policy& GetPolicy() { return s_hw_policy; }

void QueryMemoryNow(u64& avail_phys, u64& commit_avail)
{
    avail_phys = commit_avail = 0;
    if (!detail::PlatformQueryMemory(avail_phys, commit_avail))
        avail_phys = commit_avail = 0;
}

bool QueryPowerNow(bool& on_battery, bool& saver)
{
    on_battery = saver = false;
    if (detail::PlatformQueryPower(on_battery, saver))
        return true;
    on_battery = saver = false;
    return false;
}

SpinClass GetEffectiveSpinClass() { return SpinClass(s_hw_spin_effective.load(std::memory_order_relaxed)); }

void PollPower()
{
    if (!s_hw_power_poll)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now < s_hw_power_next)
        return;
    s_hw_power_next = now + kHwPowerPollPeriod;
    bool on_battery = false, saver = false;
    if (QueryPowerNow(on_battery, saver))
        HwPowerApply(on_battery, saver, "");
}

void SubmitGpu(const Gpu& gpu, bool probe)
{
    // A newer render may pass a longer struct, an older one a shorter: take the common part
    Gpu g{};
    const size_t n = std::min<size_t>(gpu.size ? gpu.size : sizeof(Gpu), sizeof(Gpu));
    std::memcpy(&g, &gpu, n);
    g.size = sizeof(Gpu);
    g.name[sizeof(g.name) - 1] = 0;

    const u32 stages = s_hw_stages.load(std::memory_order_relaxed);
    const bool keep = !probe || !(stages & (STAGE_C | STAGE_C_PROBE));
    if (keep)
    {
        s_hw_raw.gpu = g;
        s_hw_stages.store(stages | (probe ? STAGE_C_PROBE : STAGE_C), std::memory_order_release);
        HwRecompute();
        HwLogGpuLine(s_hw_profile, probe);
    }
    else
    {
        // Another probe (the GL one after R4): only logged, the first probe stays until the real device. The -hw_*
        // overrides apply to its line too (-hw_vram, -hw_igpu, -hw_nocl, -hw_cl, -hw_profile), as HwRecompute does for
        // the kept one.
        Profile p = s_hw_raw;
        p.gpu = g;
        p.stages = stages;
        p.overridden = s_hw_ovr.mask;
        HwApplyOverrides(p);
        HwLogGpuLine(p, true);
    }

    if (g.software && !probe)
        Msg("! [hw] the render runs on a software adapter (no GPU driver?) - GPU tier potato");
    if (!probe && g.api == GpuApi::OpenGL && (s_hw_ovr.mask & OVR_CL) && !s_hw_cl_gl_logged)
    {
        s_hw_cl_gl_logged = true;
        Msg("! [hw] -hw_cl: no effect on GL (no deferred contexts), the recording stays sequential");
    }
    if (!probe)
    {
        HwLogTiersPolicyLine(s_hw_profile, s_hw_policy);
        HwUpdateSentry();
    }
}

void LogInfo()
{
    const Profile& p = s_hw_profile;
    const u32 st = p.stages;
    Msg("* [hw] profile v%u, stages%s%s%s%s, overrides 0x%x", p.version, st & STAGE_A ? " A" : "",
        st & STAGE_B ? " B" : "", st & STAGE_C ? " C" : "", st & STAGE_C_PROBE ? " C(probe)" : "", p.overridden);
    HwLogCpuLine(p);
    HwLogDiskLine(p);
    if (st & (STAGE_C | STAGE_C_PROBE))
        HwLogGpuLine(p, !(st & STAGE_C));
    HwLogTiersPolicyLine(p, s_hw_policy);
    if (p.overridden)
    {
        const Profile& r = s_hw_raw;
        Msg("* [hw] real values: cpu %u/%u thr, %u phys | ram %.1f GB, commit %.1f GB | disk game %s | VRAM %.0f MB "
            "(budget %.0f), uma %u, CL %u",
            r.cpu.logical_avail, r.cpu.logical_total, r.cpu.physical_avail, r.mem.ram_total / kHwGiB,
            r.mem.commit_limit / kHwGiB, DiskKindName(r.data.game.kind), r.gpu.dedicated / kHwMiB,
            r.gpu.budget_local_start / kHwMiB, r.gpu.uma, r.gpu.driver_cl);
    }
    u64 avail = 0, commit_avail = 0;
    QueryMemoryNow(avail, commit_avail);
    Msg("* [hw] now: ram avail %.1f GB, commit avail %.1f GB", avail / kHwGiB, commit_avail / kHwGiB);
    bool on_battery = false, saver = false;
    const bool power_known = QueryPowerNow(on_battery, saver);
    Msg("* [hw] spin %s now (on AC %s), power %s%s%s", GetEffectiveSpinClass() == SpinClass::Short ? "short" : "long",
        s_hw_spin_ac == SpinClass::Short ? "short" : "long",
        !power_known ? "?" : on_battery ? "on battery" : "AC", saver ? ", battery saver" : "",
        s_hw_power_poll ? " (polled every 5 s)" : "");
}

namespace detail
{
void RunStageA()
{
    ZoneScoped;
    const auto t0 = std::chrono::steady_clock::now();
    HwInitUnknown(s_hw_raw);
    // The keys first: -hw_limit restricts the process before the collectors look at it
    HwParseOverrides();
    if (HwFindKey("-hw_limit"))
        HwApplyLimits();
    HwCollectSdlBaseline(s_hw_raw);
    PlatformCollectStageA(s_hw_raw);
    s_hw_raw.stage_a_ms = HwElapsedMs(t0);
    s_hw_stages.store(s_hw_stages.load(std::memory_order_relaxed) | STAGE_A, std::memory_order_release);
    HwRecompute();
    HwPowerSetup(); // R11/R14: the spin class depends on the CPU and the power only, both known now
}

void NoteFsCounters(u32 loose_files, u32 archives)
{
    s_hw_raw.data.loose_files = loose_files;
    s_hw_raw.data.archives = archives;
}

void RunStageB()
{
    ZoneScoped;
    const u32 stages = s_hw_stages.load(std::memory_order_relaxed);
    if (!(stages & STAGE_A) || (stages & STAGE_B))
        return;
    const auto t0 = std::chrono::steady_clock::now();

    string_path game{}, appdata{};
    FS_Path* path = nullptr;
    if (FS.get_path("$game_data$", &path) && path)
        xr_strcpy(game, path->m_Path);
    path = nullptr;
    if (FS.get_path("$app_data_root$", &path) && path)
        xr_strcpy(appdata, path->m_Path);
    PlatformCollectStageB(s_hw_raw, game, appdata);

    s_hw_raw.stage_b_ms = HwElapsedMs(t0);
    s_hw_stages.store(stages | STAGE_B, std::memory_order_release);
    HwRecompute();

    HwLogCpuLine(s_hw_profile);
    HwLogDiskLine(s_hw_profile);
    // The values limited by -hw_limit are real, not emulated
    u32 emulated = s_hw_ovr.mask & ~(OVR_LIMIT_CPU | OVR_LIMIT_COMMIT);
    if (s_hw_ovr.mask & OVR_LIMIT_CPU)
        emulated &= ~OVR_CPU;
    if (s_hw_ovr.mask & OVR_LIMIT_COMMIT)
        emulated &= ~OVR_COMMIT;
    if (emulated)
    {
        Msg("* [hw] -hw_* keys: the values marked '*' are emulated; the engine follows the policy computed from them "
            "(worker pool, render recording, SVP worker, D3D11 internal threads)");
    }
    if (emulated & OVR_CL)
    {
        Msg("* [hw] -hw_cl: A/B test - D3D11 DriverCommandLists taken as 1 ('CL 1*' in the gpu line): parallel main "
            "recording and the SVP worker as on a driver with command lists, the runtime emulates them if the driver "
            "has none; no effect on GL");
    }
    HwLogCommitWarning(s_hw_profile);
    if (HwFindKey("-hw_selftest"))
        HwRunSelfTest();
    HwUpdateSentry();
}

#if !defined(XR_PLATFORM_WINDOWS)
// -hw_limit is implemented on Windows only (affinity + Job Object)
u32 PlatformApplyLimits(u32 /*physical*/, u32 /*logical*/, u64 /*commit*/)
{
    Msg("! [hw] -hw_limit: not supported on this platform, the -hw_* values stay emulated only");
    return 0;
}
#endif

#if !defined(XR_PLATFORM_WINDOWS) && !defined(XR_PLATFORM_LINUX)
// R14, macOS and the rest: SDL, as the start state (HwCollectSdlBaseline). No battery saver flag there.
bool PlatformQueryPower(bool& on_battery, bool& saver)
{
    saver = false;
    int seconds = 0, percent = 0;
    switch (SDL_GetPowerInfo(&seconds, &percent))
    {
    case SDL_POWERSTATE_NO_BATTERY:
    case SDL_POWERSTATE_CHARGING:
    case SDL_POWERSTATE_CHARGED:
        on_battery = false;
        return true;
    case SDL_POWERSTATE_ON_BATTERY:
        on_battery = true;
        return true;
    default:
        return false;
    }
}
#endif

#if !defined(XR_PLATFORM_WINDOWS) && !defined(XR_PLATFORM_LINUX) && !defined(XR_PLATFORM_APPLE)
// Other platforms: the SDL baseline only, the rest stays Unknown (the policy behaves as on an average machine)
void PlatformCollectStageA(Profile& /*profile*/) {}
void PlatformCollectStageB(Profile& /*profile*/, pcstr /*game_path*/, pcstr /*appdata_path*/) {}
bool PlatformQueryMemory(u64& /*avail_phys*/, u64& /*commit_avail*/) { return false; }
#endif
} // namespace detail
} // namespace xray::hw
