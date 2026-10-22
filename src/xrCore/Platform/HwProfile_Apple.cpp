#include "stdafx.h"
#pragma hdrstop

#if defined(XR_PLATFORM_APPLE)
#include "xrCore/Platform/HwProfile_impl.h"

#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>

#include <cstdio>
#include <cstring>

// macOS collectors of the hardware profile (plans/optimization_mt/50-smart-engine-hw-profile.md, section 9):
// sysctl only. The disk kind is not queried (IOKit): the system volume counts as an SSD, the rest stays Unknown.

namespace xray::hw::detail
{
namespace
{
bool HwMacSysctlU64(pcstr name, u64& value)
{
    u64 v64 = 0;
    size_t size = sizeof(v64);
    if (sysctlbyname(name, &v64, &size, nullptr, 0) != 0)
        return false;
    if (size == sizeof(u32))
    {
        u32 v32 = 0;
        std::memcpy(&v32, &v64, sizeof(v32));
        value = v32;
    }
    else
        value = v64;
    return true;
}

bool HwMacSysctlString(pcstr name, char* out, size_t out_size)
{
    size_t size = out_size;
    if (sysctlbyname(name, out, &size, nullptr, 0) != 0)
    {
        out[0] = 0;
        return false;
    }
    out[out_size - 1] = 0;
    return true;
}

bool HwMacAvailPhys(u64& avail)
{
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const mach_port_t host = mach_host_self();
    const kern_return_t r = host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
    mach_port_deallocate(mach_task_self(), host);
    if (r != KERN_SUCCESS)
        return false;
    u64 page = 0;
    if (!HwMacSysctlU64("hw.pagesize", page) || !page)
        page = 4096;
    avail = (u64(vm.free_count) + u64(vm.inactive_count) + u64(vm.purgeable_count)) * page;
    return true;
}

u64 HwMacSwapTotal(u64& swap_free)
{
    swap_free = 0;
    xsw_usage swap{};
    size_t size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) != 0)
        return 0;
    swap_free = swap.xsu_avail;
    return swap.xsu_total;
}

void HwMacClassifyPath(pcstr path, Disk& d, u64& fs_id)
{
    fs_id = 0;
    if (!path || !path[0])
        return;
    struct statvfs vfs{};
    if (statvfs(path, &vfs) == 0)
    {
        d.free_bytes = u64(vfs.f_bavail) * vfs.f_frsize;
        d.total_bytes = u64(vfs.f_blocks) * vfs.f_frsize;
        fs_id = u64(vfs.f_fsid);
    }
    struct statfs fs{};
    if (statfs(path, &fs) != 0)
        return;
    xr_strcpy(d.volume, fs.f_mntonname);
    if (!(fs.f_flags & MNT_LOCAL))
        d.kind = DiskKind::Network;
    else if (!strcmp(fs.f_mntonname, "/") || !strncmp(fs.f_mntonname, "/System/Volumes/", 16))
        d.kind = DiskKind::Ssd; // the internal drive of every supported Mac
}
} // namespace

void PlatformCollectStageA(Profile& p)
{
    u64 v = 0;
    if (HwMacSysctlU64("hw.logicalcpu", v) && v)
        p.cpu.logical_total = p.cpu.logical_avail = u16(v);
    if (HwMacSysctlU64("hw.physicalcpu", v) && v)
    {
        p.cpu.physical = p.cpu.physical_avail = u16(v);
        p.cpu.smt = p.cpu.logical_total > p.cpu.physical ? 1 : 0;
    }
    u64 levels = 0, perf = 0, eff = 0;
    if (HwMacSysctlU64("hw.nperflevels", levels) && levels > 1 && HwMacSysctlU64("hw.perflevel0.physicalcpu", perf) &&
        HwMacSysctlU64("hw.perflevel1.physicalcpu", eff))
    {
        p.cpu.hybrid = 1;
        p.cpu.perf_cores = u16(perf);
        p.cpu.eff_cores = u16(eff);
    }
    HwMacSysctlString("machdep.cpu.brand_string", p.cpu.brand, sizeof(p.cpu.brand));

    if (HwMacSysctlU64("hw.memsize", v) && v)
        p.mem.ram_total = v;
    HwMacAvailPhys(p.mem.ram_avail_start);
    u64 swap_free = 0;
    p.mem.swap_bytes = HwMacSwapTotal(swap_free);
    // macOS grows the swap files on demand: RAM + current swap is only a lower estimate of the limit
    p.mem.commit_limit = p.mem.ram_total + p.mem.swap_bytes;
    p.mem.commit_avail_start = p.mem.ram_avail_start + swap_free;
    p.mem.swap_kind = SwapKind::File;

    char version[32]{};
    if (HwMacSysctlString("kern.osproductversion", version, sizeof(version)))
    {
        xr_strcpy(p.env.os_name, version);
        unsigned major = 0, minor = 0;
        if (sscanf(version, "%u.%u", &major, &minor) >= 1)
        {
            p.env.os_major = major;
            p.env.os_minor = minor;
        }
    }
}

void PlatformCollectStageB(Profile& p, pcstr game_path, pcstr appdata_path)
{
    u64 game_fs = 0, appdata_fs = 0, system_fs = 0;
    HwMacClassifyPath(game_path, p.data.game, game_fs);
    HwMacClassifyPath(appdata_path, p.data.appdata, appdata_fs);
    if (appdata_fs && appdata_fs == game_fs)
        p.data.appdata.same_as = 1;
    HwMacClassifyPath("/", p.data.system, system_fs);
    if (system_fs && system_fs == game_fs)
        p.data.system.same_as = 1;
    else if (system_fs && system_fs == appdata_fs)
        p.data.system.same_as = 2;
}

bool PlatformQueryMemory(u64& avail_phys, u64& commit_avail)
{
    if (!HwMacAvailPhys(avail_phys))
        return false;
    u64 swap_free = 0;
    HwMacSwapTotal(swap_free);
    commit_avail = avail_phys + swap_free;
    return true;
}
} // namespace xray::hw::detail
#endif // XR_PLATFORM_APPLE
