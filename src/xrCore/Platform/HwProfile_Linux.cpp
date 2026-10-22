#include "stdafx.h"
#pragma hdrstop

#if defined(XR_PLATFORM_LINUX)
#include "xrCore/Platform/HwProfile_impl.h"

#include <dirent.h>
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// Linux collectors of the hardware profile (plans/optimization_mt/50-smart-engine-hw-profile.md, sections 3 and 9):
// procfs / sysfs reads only, every failure leaves the field Unknown / 0.

namespace xray::hw::detail
{
namespace
{
// A small procfs / sysfs file into buf, without the trailing newline
bool HwLnxReadFile(pcstr path, char* buf, size_t size)
{
    buf[0] = 0;
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = 0;
    return true;
}

bool HwLnxReadU64(pcstr path, u64& value)
{
    char buf[64];
    if (!HwLnxReadFile(path, buf, sizeof(buf)) || !buf[0])
        return false;
    char* end = nullptr;
    const unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf)
        return false;
    value = u64(v);
    return true;
}

// "0-3,8,10-11" -> set
void HwLnxParseCpuList(pcstr list, cpu_set_t& set)
{
    CPU_ZERO(&set);
    pcstr s = list;
    while (*s)
    {
        char* end = nullptr;
        long a = strtol(s, &end, 10);
        if (end == s)
            break;
        long b = a;
        if (*end == '-')
        {
            s = end + 1;
            b = strtol(s, &end, 10);
            if (end == s)
                break;
        }
        for (long i = std::max(a, 0L); i <= b && i < CPU_SETSIZE; ++i)
            CPU_SET(i, &set);
        s = end;
        if (*s != ',')
            break;
        ++s;
    }
}

// cgroup v2 directory of the process ("/sys/fs/cgroup/<path>"), empty if not found
void HwLnxCgroupDir(char* out, size_t size)
{
    out[0] = 0;
    char buf[1024];
    if (!HwLnxReadFile("/proc/self/cgroup", buf, sizeof(buf)))
        return;
    // the cgroup v2 line is "0::<path>", at the start of the file or of a line
    pcstr line = strstr(buf, "0::");
    while (line && line != buf && line[-1] != '\n')
        line = strstr(line + 1, "0::");
    if (!line)
        return;
    line += 3;
    size_t n = 0;
    while (line[n] && line[n] != '\n')
        ++n;
    snprintf(out, size, "/sys/fs/cgroup%.*s", int(n), line);
}

// Physical core id of a logical CPU (package << 32 | core), false if the topology is not exposed
bool HwLnxCoreKey(int cpu, u64& key)
{
    char path[96];
    u64 core = 0, package = 0;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
    if (!HwLnxReadU64(path, core))
        return false;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
    HwLnxReadU64(path, package);
    key = (package << 32) | (core & 0xFFFFFFFFull);
    return true;
}

u32 HwLnxCountCores(const cpu_set_t& cpus, const cpu_set_t& online)
{
    xr_vector<u64> keys;
    for (int i = 0; i < CPU_SETSIZE; ++i)
    {
        u64 key = 0;
        if (CPU_ISSET(i, &cpus) && CPU_ISSET(i, &online) && HwLnxCoreKey(i, key) &&
            std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    }
    return u32(keys.size());
}

void HwLnxCollectCpu(Profile& p, pcstr cgroup_dir)
{
    const long online_n = sysconf(_SC_NPROCESSORS_ONLN);
    if (online_n > 0)
        p.cpu.logical_total = u16(std::min(online_n, 0xFFFFL));

    cpu_set_t online;
    char buf[1024];
    if (HwLnxReadFile("/sys/devices/system/cpu/online", buf, sizeof(buf)))
        HwLnxParseCpuList(buf, online);
    else
    {
        CPU_ZERO(&online);
        for (long i = 0; i < online_n && i < CPU_SETSIZE; ++i)
            CPU_SET(i, &online);
    }

    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    const bool have_affinity = sched_getaffinity(0, sizeof(affinity), &affinity) == 0;
    if (!have_affinity)
        affinity = online;
    u32 avail = u32(CPU_COUNT(&affinity));

    // cgroup v2 quota (Flatpak, containers): cpu.max = "<quota> <period>" or "max <period>"
    if (cgroup_dir[0])
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/cpu.max", cgroup_dir);
        if (HwLnxReadFile(path, buf, sizeof(buf)) && strncmp(buf, "max", 3) != 0)
        {
            unsigned long long quota = 0, period = 0;
            if (sscanf(buf, "%llu %llu", &quota, &period) == 2 && quota && period)
                avail = std::min<u32>(avail, u32(std::max<unsigned long long>(1, (quota + period - 1) / period)));
        }
    }
    if (avail)
        p.cpu.logical_avail = u16(std::min<u32>(avail, 0xFFFF));

    const u32 physical = HwLnxCountCores(online, online);
    if (physical)
    {
        p.cpu.physical = u16(physical);
        p.cpu.physical_avail = u16(std::max<u32>(1, HwLnxCountCores(affinity, online)));
        p.cpu.smt = p.cpu.logical_total > physical ? 1 : 0;
    }

    // Intel hybrid: the P and E cores are separate PMUs
    char core_list[512], atom_list[512];
    if (HwLnxReadFile("/sys/devices/cpu_core/cpus", core_list, sizeof(core_list)) &&
        HwLnxReadFile("/sys/devices/cpu_atom/cpus", atom_list, sizeof(atom_list)))
    {
        cpu_set_t core_set, atom_set;
        HwLnxParseCpuList(core_list, core_set);
        HwLnxParseCpuList(atom_list, atom_set);
        p.cpu.perf_cores = u16(HwLnxCountCores(core_set, online));
        p.cpu.eff_cores = u16(HwLnxCountCores(atom_set, online));
        p.cpu.hybrid = (p.cpu.perf_cores && p.cpu.eff_cores) ? 1 : 0;
    }

    // Brand: the first "model name" of /proc/cpuinfo (the first 4 KB are enough)
    char info[4096];
    if (HwLnxReadFile("/proc/cpuinfo", info, sizeof(info)))
    {
        if (pcstr line = strstr(info, "model name"))
        {
            if (pcstr colon = strchr(line, ':'))
            {
                ++colon;
                while (*colon == ' ' || *colon == '\t')
                    ++colon;
                size_t n = 0;
                while (colon[n] && colon[n] != '\n' && n + 1 < sizeof(p.cpu.brand))
                    ++n;
                memcpy(p.cpu.brand, colon, n);
                p.cpu.brand[n] = 0;
            }
        }
    }
}

struct HwLnxMemInfo
{
    u64 total, available, swap_total, swap_free, commit_limit, committed;
};

bool HwLnxReadMemInfo(HwLnxMemInfo& m)
{
    m = HwLnxMemInfo{};
    char buf[4096];
    if (!HwLnxReadFile("/proc/meminfo", buf, sizeof(buf)))
        return false;
    const auto field = [&buf](pcstr name) -> u64
    {
        pcstr line = strstr(buf, name);
        if (!line)
            return 0;
        unsigned long long kb = 0;
        sscanf(line + xr_strlen(name), " %llu", &kb);
        return u64(kb) * 1024;
    };
    m.total = field("MemTotal:");
    m.available = field("MemAvailable:");
    m.swap_total = field("SwapTotal:");
    m.swap_free = field("SwapFree:");
    m.commit_limit = field("CommitLimit:");
    m.committed = field("Committed_AS:");
    return m.total != 0;
}

// The commit limit has a meaning only with vm.overcommit_memory = 2; otherwise RAM + swap
bool HwLnxStrictOvercommit()
{
    char buf[16];
    return HwLnxReadFile("/proc/sys/vm/overcommit_memory", buf, sizeof(buf)) && buf[0] == '2';
}

void HwLnxCommit(const HwLnxMemInfo& m, u64& limit, u64& avail)
{
    if (HwLnxStrictOvercommit() && m.commit_limit)
    {
        limit = m.commit_limit;
        avail = m.commit_limit > m.committed ? m.commit_limit - m.committed : 0;
    }
    else
    {
        limit = m.total + m.swap_total;
        avail = m.available + m.swap_free;
    }
}

void HwLnxCollectMemory(Profile& p, pcstr cgroup_dir)
{
    HwLnxMemInfo m;
    if (!HwLnxReadMemInfo(m))
        return;
    p.mem.ram_total = m.total;
    p.mem.ram_avail_start = m.available;
    // cgroup v2 memory limit (Flatpak, containers)
    if (cgroup_dir[0])
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/memory.max", cgroup_dir);
        u64 limit = 0;
        if (HwLnxReadU64(path, limit) && limit && limit < p.mem.ram_total)
        {
            p.mem.ram_total = limit;
            p.mem.ram_avail_start = std::min(p.mem.ram_avail_start, limit);
        }
    }
    HwLnxCommit(m, p.mem.commit_limit, p.mem.commit_avail_start);
    p.mem.swap_bytes = m.swap_total;

    // Swap kind: zram is not a disk
    p.mem.swap_kind = m.swap_total ? SwapKind::Unknown : SwapKind::None;
    char swaps[2048];
    if (m.swap_total && HwLnxReadFile("/proc/swaps", swaps, sizeof(swaps)))
    {
        pcstr line = strchr(swaps, '\n'); // skip the header
        while (line && *line)
        {
            ++line;
            char name[256]{}, type[32]{};
            if (sscanf(line, "%255s %31s", name, type) == 2)
            {
                if (strstr(name, "/dev/zram"))
                {
                    p.mem.swap_kind = SwapKind::Zram;
                    break;
                }
                if (!strcmp(type, "partition"))
                    p.mem.swap_kind = SwapKind::Partition;
                else if (!strcmp(type, "file") && p.mem.swap_kind == SwapKind::Unknown)
                    p.mem.swap_kind = SwapKind::File;
            }
            line = strchr(line, '\n');
        }
    }
}

// Power: /sys/class/power_supply/*/type = Battery (status Discharging = on battery) or Mains (online). Stage A and the
// live R14 query (PlatformQueryPower); false = no power_supply class.
bool HwLnxReadPower(bool& battery, bool& on_battery)
{
    battery = on_battery = false;
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir)
        return false;
    bool discharging = false, mains_online = false;
    while (dirent* e = readdir(dir))
    {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX], value[64];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name);
        if (!HwLnxReadFile(path, value, sizeof(value)))
            continue;
        if (!strcmp(value, "Battery"))
        {
            // peripherals (mouse, gamepad) report "scope = Device"
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/scope", e->d_name);
            if (HwLnxReadFile(path, value, sizeof(value)) && !strcmp(value, "Device"))
                continue;
            battery = true;
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", e->d_name);
            if (HwLnxReadFile(path, value, sizeof(value)) && !strcmp(value, "Discharging"))
                discharging = true;
        }
        else if (!strcmp(value, "Mains"))
        {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", e->d_name);
            if (HwLnxReadFile(path, value, sizeof(value)) && value[0] == '1')
                mains_online = true;
        }
    }
    closedir(dir);
    on_battery = battery && discharging && !mains_online;
    return true;
}

void HwLnxCollectEnv(Profile& p)
{
    utsname u{};
    if (uname(&u) == 0)
    {
        xr_strcpy(p.env.os_name, u.release);
        unsigned major = 0, minor = 0;
        if (sscanf(u.release, "%u.%u", &major, &minor) >= 1)
        {
            p.env.os_major = major;
            p.env.os_minor = minor;
        }
    }
    char product[64];
    if (HwLnxReadFile("/sys/class/dmi/id/product_name", product, sizeof(product)))
        p.env.steam_deck = (!strcmp(product, "Jupiter") || !strcmp(product, "Galileo")) ? 1 : 0;

    bool battery = false, on_battery = false;
    if (HwLnxReadPower(battery, on_battery))
    {
        p.env.has_battery = battery ? 1 : 0;
        p.env.on_battery = on_battery ? 1 : 0;
    }
}

// The block device of a path: /sys/dev/block/MAJ:MIN -> the whole disk (partition -> parent, dm/md -> first slave)
void HwLnxClassifyPath(pcstr path, Disk& d, u64& dev_id)
{
    dev_id = 0;
    struct stat st{};
    if (!path || !path[0] || stat(path, &st) != 0)
        return;
    dev_id = u64(st.st_dev);

    struct statvfs vfs{};
    if (statvfs(path, &vfs) == 0)
    {
        d.free_bytes = u64(vfs.f_bavail) * vfs.f_frsize;
        d.total_bytes = u64(vfs.f_blocks) * vfs.f_frsize;
    }
    struct statfs fs{};
    if (statfs(path, &fs) == 0)
    {
        // NFS, SMB, CIFS, SMB2
        const unsigned long type = static_cast<unsigned long>(fs.f_type);
        if (type == 0x6969ul || type == 0x517Bul || type == 0xFF534D42ul || type == 0xFE534D42ul)
        {
            d.kind = DiskKind::Network;
            xr_strcpy(d.volume, "network fs");
            return;
        }
    }

    const unsigned dev_major = major(st.st_dev), dev_minor = minor(st.st_dev);
    xr_sprintf(d.volume, "dev %u:%u", dev_major, dev_minor);
    if (dev_major == 0)
        return; // btrfs, overlayfs...: no block device behind st_dev (plan 50: mountinfo, not done)

    char link[64];
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", dev_major, dev_minor);
    char real[PATH_MAX];
    if (!realpath(link, real))
        return;

    char probe[PATH_MAX + 64];
    for (int depth = 0; depth < 3; ++depth)
    {
        snprintf(probe, sizeof(probe), "%s/partition", real);
        if (access(probe, F_OK) == 0)
        {
            if (char* slash = strrchr(real, '/'))
                *slash = 0;
        }
        // device mapper (LVM, LUKS) / md RAID: the first disk under it
        snprintf(probe, sizeof(probe), "%s/slaves", real);
        DIR* dir = opendir(probe);
        if (!dir)
            break;
        bool descended = false;
        while (dirent* e = readdir(dir))
        {
            if (e->d_name[0] == '.')
                continue;
            char slave[PATH_MAX + 64 + 256];
            snprintf(slave, sizeof(slave), "%s/%s", probe, e->d_name);
            char resolved[PATH_MAX];
            if (realpath(slave, resolved))
            {
                xr_strcpy(real, resolved);
                descended = true;
            }
            break;
        }
        closedir(dir);
        if (!descended)
            break;
    }

    pcstr name = strrchr(real, '/');
    name = name ? name + 1 : real;
    xr_sprintf(d.volume, "%s", name);

    u64 rotational = 2, removable = 0;
    snprintf(probe, sizeof(probe), "%s/queue/rotational", real);
    HwLnxReadU64(probe, rotational);
    snprintf(probe, sizeof(probe), "%s/removable", real);
    HwLnxReadU64(probe, removable);
    d.removable = removable ? 1 : 0;
    if (rotational <= 1)
        d.seek_penalty = u8(rotational);

    if (strstr(real, "/usb") || strstr(real, "/mmc"))
        d.kind = DiskKind::Removable;
    else if (!strncmp(name, "nvme", 4))
        d.kind = DiskKind::Nvme;
    else if (strstr(real, "/virtual/")) // loop, zram, ram disks
        d.kind = DiskKind::Virtual;
    else if (rotational == 1)
        d.kind = DiskKind::Hdd;
    else if (rotational == 0)
        d.kind = strstr(real, "/ata") ? DiskKind::SataSsd : DiskKind::Ssd;
    else if (d.removable)
        d.kind = DiskKind::Removable;
}
} // namespace

void PlatformCollectStageA(Profile& p)
{
    char cgroup_dir[PATH_MAX];
    HwLnxCgroupDir(cgroup_dir, sizeof(cgroup_dir));
    HwLnxCollectCpu(p, cgroup_dir);
    HwLnxCollectMemory(p, cgroup_dir);
    HwLnxCollectEnv(p);
}

void PlatformCollectStageB(Profile& p, pcstr game_path, pcstr appdata_path)
{
    u64 game_dev = 0, appdata_dev = 0, system_dev = 0;
    HwLnxClassifyPath(game_path, p.data.game, game_dev);
    HwLnxClassifyPath(appdata_path, p.data.appdata, appdata_dev);
    if (appdata_dev && appdata_dev == game_dev)
        p.data.appdata.same_as = 1;
    HwLnxClassifyPath("/", p.data.system, system_dev);
    if (system_dev && system_dev == game_dev)
        p.data.system.same_as = 1;
    else if (system_dev && system_dev == appdata_dev)
        p.data.system.same_as = 2;
}

bool PlatformQueryMemory(u64& avail_phys, u64& commit_avail)
{
    HwLnxMemInfo m;
    if (!HwLnxReadMemInfo(m))
        return false;
    u64 limit = 0;
    avail_phys = m.available;
    HwLnxCommit(m, limit, commit_avail);
    return true;
}

// R14, live: the same sysfs reading as stage A. No battery saver flag on Linux.
bool PlatformQueryPower(bool& on_battery, bool& saver)
{
    saver = false;
    bool battery = false;
    return HwLnxReadPower(battery, on_battery);
}
} // namespace xray::hw::detail
#endif // XR_PLATFORM_LINUX
