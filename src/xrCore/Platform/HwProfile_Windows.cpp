#include "stdafx.h"
#pragma hdrstop

#if defined(XR_PLATFORM_WINDOWS)
#include "xrCore/Platform/HwProfile_impl.h"
#include "xrCore/Text/Utf8Utils.hpp"

#include <winioctl.h>
#include <Psapi.h>
#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64)
#include <intrin.h>
#endif

#include <algorithm>
#include <cstring>
#include <cwchar>

// Windows collectors of the hardware profile (plans/optimization_mt/50-smart-engine-hw-profile.md, section 3).
// Cheap calls only, no admin rights; every failure leaves the field Unknown / 0. The Windows 10+ functions are
// resolved at run time (_WIN32_WINNT is Windows 7 here).

namespace xray::hw::detail
{
namespace
{
using HwWinGetProcessDefaultCpuSetsFn = BOOL(WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
using HwWinGetSystemCpuSetInformationFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
using HwWinRtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
using HwWinWineGetVersionFn = const char*(__cdecl*)();

constexpr WORD kHwWinMaxGroups = 64;

// -hw_limit -hw_commit: the per-process commit limit of the Job Object, 0 = none. GlobalMemoryStatusEx does not
// report it, PlatformQueryMemory subtracts the process commit from it. Written once at stage A, before any reader.
u64 s_hw_win_job_commit_limit = 0;

u32 HwWinPopCount(KAFFINITY mask)
{
    u32 n = 0;
    for (; mask; mask &= mask - 1)
        ++n;
    return n;
}

// Logical processors the process may run on, per processor group: affinity (start /affinity), then CPU sets
struct HwWinAllowed
{
    KAFFINITY mask[kHwWinMaxGroups];
    WORD groups;
};

void HwWinCollectAllowed(HwWinAllowed& a)
{
    a = HwWinAllowed{};
    a.groups = std::min<WORD>(GetActiveProcessorGroupCount(), kHwWinMaxGroups);
    for (WORD g = 0; g < a.groups; ++g)
    {
        const DWORD n = GetActiveProcessorCount(g);
        a.mask[g] = n >= sizeof(KAFFINITY) * 8 ? ~KAFFINITY(0) : ((KAFFINITY(1) << n) - 1);
    }

    // A process restricted by an affinity mask lives in one group. With several groups (> 64 CPUs) keep all of them:
    // Windows 11 spreads the threads over the groups, and such machines are not the target of the rules.
    if (a.groups == 1)
    {
        DWORD_PTR process_mask = 0, system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask)
            a.mask[0] &= KAFFINITY(process_mask);
    }

    // CPU sets (Windows 10+): an empty default set means "no restriction"
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto get_default_sets =
        reinterpret_cast<HwWinGetProcessDefaultCpuSetsFn>(GetProcAddress(kernel, "GetProcessDefaultCpuSets"));
    const auto get_set_info =
        reinterpret_cast<HwWinGetSystemCpuSetInformationFn>(GetProcAddress(kernel, "GetSystemCpuSetInformation"));
    if (!get_default_sets || !get_set_info)
        return;
    ULONG count = 0;
    if (get_default_sets(GetCurrentProcess(), nullptr, 0, &count) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        !count)
        return;
    xr_vector<ULONG> ids(count);
    if (!get_default_sets(GetCurrentProcess(), ids.data(), count, &count))
        return;
    ULONG len = 0;
    get_set_info(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len)
        return;
    xr_vector<u8> buffer(len);
    if (!get_set_info(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), len, &len, GetCurrentProcess(), 0))
        return;
    KAFFINITY allowed[kHwWinMaxGroups]{};
    for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= len;)
    {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (!info->Size)
            break;
        if (info->Type == CpuSetInformation && info->CpuSet.Group < kHwWinMaxGroups &&
            std::find(ids.begin(), ids.end(), info->CpuSet.Id) != ids.end())
            allowed[info->CpuSet.Group] |= KAFFINITY(1) << info->CpuSet.LogicalProcessorIndex;
        offset += info->Size;
    }
    for (WORD g = 0; g < a.groups; ++g)
        a.mask[g] &= allowed[g];
}

void HwWinCollectCpu(Profile& p)
{
    HwWinAllowed allowed;
    HwWinCollectAllowed(allowed);
    u32 logical_avail = 0;
    for (WORD g = 0; g < allowed.groups; ++g)
        logical_avail += HwWinPopCount(allowed.mask[g]);

    const DWORD logical_total = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (logical_total)
        p.cpu.logical_total = u16(std::min<DWORD>(logical_total, 0xFFFF));
    if (logical_avail)
        p.cpu.logical_avail = u16(std::min<u32>(logical_avail, 0xFFFF));
    p.cpu.groups = allowed.groups;

    // One RelationProcessorCore record per physical core: SMT flag, EfficiencyClass (higher = faster; 0 everywhere on
    // non-hybrid CPUs and before Windows 10)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len)
        return;
    xr_vector<u8> buffer(len);
    auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &len))
        return;

    u32 physical = 0, physical_avail = 0, smt = 0;
    BYTE eff_min = 0xFF, eff_max = 0;
    xr_vector<BYTE> core_class;
    for (DWORD offset = 0; offset < len;)
    {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (!info->Size)
            break;
        if (info->Relationship == RelationProcessorCore)
        {
            const PROCESSOR_RELATIONSHIP& core = info->Processor;
            ++physical;
            if (core.Flags & LTP_PC_SMT)
                smt = 1;
            bool available = false;
            for (WORD i = 0; i < core.GroupCount; ++i)
            {
                const GROUP_AFFINITY& ga = core.GroupMask[i];
                if (ga.Group < allowed.groups && (ga.Mask & allowed.mask[ga.Group]))
                    available = true;
            }
            if (available)
                ++physical_avail;
            eff_min = std::min(eff_min, core.EfficiencyClass);
            eff_max = std::max(eff_max, core.EfficiencyClass);
            core_class.push_back(core.EfficiencyClass);
        }
        offset += info->Size;
    }
    p.cpu.physical = u16(physical);
    p.cpu.physical_avail = u16(physical_avail);
    p.cpu.smt = u8(smt);
    if (physical && eff_max > eff_min)
    {
        p.cpu.hybrid = 1;
        p.cpu.perf_cores = u16(std::count(core_class.begin(), core_class.end(), eff_max));
        p.cpu.eff_cores = u16(physical - p.cpu.perf_cores);
    }
}

void HwWinCollectBrand(Profile& p)
{
    char brand[64]{};
#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64)
    int regs[4]{};
    __cpuid(regs, int(0x80000000));
    if (u32(regs[0]) >= 0x80000004u)
    {
        char raw[49]{};
        for (int i = 0; i < 3; ++i)
            __cpuid(reinterpret_cast<int*>(raw + 16 * i), int(0x80000002u + i));
        xr_strcpy(brand, raw);
    }
#endif
    if (!brand[0])
    {
        DWORD size = sizeof(brand);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                "ProcessorNameString", RRF_RT_REG_SZ, nullptr, brand, &size) != ERROR_SUCCESS)
            brand[0] = 0;
    }
    pcstr b = brand;
    while (*b == ' ')
        ++b;
    xr_strcpy(p.cpu.brand, b);
    // trailing spaces of the CPUID string
    for (size_t n = xr_strlen(p.cpu.brand); n && p.cpu.brand[n - 1] == ' '; --n)
        p.cpu.brand[n - 1] = 0;
}

void HwWinCollectMemory(Profile& p)
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
    {
        p.mem.ram_total = ms.ullTotalPhys;
        p.mem.ram_avail_start = ms.ullAvailPhys;
        // ullTotalPageFile / ullAvailPageFile are the commit limit and its headroom, not the pagefile size
        p.mem.commit_limit = ms.ullTotalPageFile;
        p.mem.commit_avail_start = ms.ullAvailPageFile;
        p.mem.swap_bytes = ms.ullTotalPageFile > ms.ullTotalPhys ? ms.ullTotalPageFile - ms.ullTotalPhys : 0;
        // -hw_limit: the job's limit and headroom (the swap estimate stays the system one)
        u64 job_avail_phys = 0, job_avail_commit = 0;
        if (s_hw_win_job_commit_limit && PlatformQueryMemory(job_avail_phys, job_avail_commit))
        {
            p.mem.commit_limit = std::min<u64>(p.mem.commit_limit, s_hw_win_job_commit_limit);
            p.mem.commit_avail_start = job_avail_commit;
        }
    }
    ULONGLONG installed_kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&installed_kb))
        p.mem.ram_installed = u64(installed_kb) * 1024;

    // Pagefile: ExistingPageFiles has the real files ("\??\C:\pagefile.sys"), PagingFiles the settings
    // ("?:\pagefile.sys" - system managed on all drives, "C:\pagefile.sys 0 0" - system managed, "... 2048 4096" -
    // custom size, empty - none)
    static constexpr pcstr key = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management";
    char existing[512]{};
    DWORD size = sizeof(existing) - 2;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "ExistingPageFiles", RRF_RT_REG_MULTI_SZ, nullptr, existing, &size) ==
        ERROR_SUCCESS)
    {
        pcstr e = existing;
        if (!strncmp(e, "\\??\\", 4))
            e += 4;
        if (e[0] && e[1] == ':')
        {
            p.mem.swap_volume[0] = char(toupper(e[0]));
            p.mem.swap_volume[1] = ':';
            p.mem.swap_volume[2] = 0;
        }
    }
    char paging[512]{};
    size = sizeof(paging) - 2;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "PagingFiles", RRF_RT_REG_MULTI_SZ, nullptr, paging, &size) ==
        ERROR_SUCCESS)
    {
        if (!paging[0])
            p.mem.swap_sys_managed = 0;
        else if (paging[0] == '?')
            p.mem.swap_sys_managed = 1;
        else
        {
            unsigned initial = 0, maximum = 0;
            pcstr sizes = strchr(paging, ' ');
            const int n = sizes ? sscanf(sizes, "%u %u", &initial, &maximum) : 0;
            p.mem.swap_sys_managed = (n < 2 || (!initial && !maximum)) ? 1 : 0;
        }
    }
    if (p.mem.swap_bytes < (u64(64) << 20))
        p.mem.swap_kind = SwapKind::None;
    else
        p.mem.swap_kind = SwapKind::File;
}

void HwWinCollectEnv(Profile& p)
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return;
    // RtlGetVersion is not shimmed by the compatibility manifest, unlike GetVersionEx
    if (const auto rtl_get_version = reinterpret_cast<HwWinRtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")))
    {
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl_get_version(&vi) == 0)
        {
            p.env.os_major = vi.dwMajorVersion;
            p.env.os_minor = vi.dwMinorVersion;
            p.env.os_build = vi.dwBuildNumber;
            xr_sprintf(p.env.os_name, "Windows %u.%u.%u", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        }
    }
    if (const auto wine_get_version = reinterpret_cast<HwWinWineGetVersionFn>(GetProcAddress(ntdll, "wine_get_version")))
    {
        p.env.wine = 1;
        // Truncated: a custom / git build reports a long string, and xr_strcpy (strcpy_s) fails on overflow
        if (const char* version = wine_get_version())
            strncpy_s(p.env.wine_version, version, _TRUNCATE);
    }

    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps))
    {
        // BatteryFlag 128 - no system battery, 255 - unknown; ACLineStatus 0 - offline;
        // SystemStatusFlag 1 - battery saver on (Windows 10+, 0 before)
        if (ps.BatteryFlag != 255)
            p.env.has_battery = (ps.BatteryFlag & 128) ? 0 : 1;
        if (ps.ACLineStatus != 255)
            p.env.on_battery = (ps.ACLineStatus == 0 && p.env.has_battery == 1) ? 1 : 0;
        p.env.battery_saver = ps.SystemStatusFlag == 1 ? 1 : 0;
    }
}

//------------------------------------------------------------------------------------------------------------------------
// Disks

// IncursSeekPenalty and BusType of a volume or physical drive handle (opened with 0 access: no admin rights needed,
// IOCTL_STORAGE_QUERY_PROPERTY is FILE_ANY_ACCESS)
bool HwWinQueryStorage(HANDLE h, u8& seek, u8& bus, u8& removable)
{
    bool any = false;
    STORAGE_PROPERTY_QUERY query{};
    query.QueryType = PropertyStandardQuery;
    DWORD returned = 0;

    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &penalty, sizeof(penalty), &returned,
            nullptr) &&
        returned >= sizeof(penalty))
    {
        seek = penalty.IncursSeekPenalty ? 1 : 0;
        any = true;
    }

    query.PropertyId = StorageDeviceProperty;
    alignas(8) u8 buffer[1024]{};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned,
            nullptr) &&
        returned >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE))
    {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        bus = u8(desc->BusType);
        removable = desc->RemovableMedia ? 1 : 0;
        any = true;
    }
    return any;
}

DiskKind HwWinKind(u8 seek, u8 bus, u8 removable)
{
    switch (bus)
    {
    case BusTypeNvme: return DiskKind::Nvme;
    case BusTypeUsb:
    case BusTypeSd:
    case BusTypeMmc:
    case BusType1394: return DiskKind::Removable;
    case BusTypeVirtual:
    case BusTypeFileBackedVirtual: return DiskKind::Virtual;
    case BusTypeiScsi:
    case BusTypeNvmeof: return DiskKind::Network;
    default: break;
    }
    if (seek == 1)
        return DiskKind::Hdd;
    if (seek == 0)
        return (bus == BusTypeSata || bus == BusTypeAta) ? DiskKind::SataSsd : DiskKind::Ssd;
    if (removable)
        return DiskKind::Removable;
    return DiskKind::Unknown;
}

// For "take the worst" over the disks of a spanned volume
int HwWinBadness(DiskKind kind)
{
    switch (kind)
    {
    case DiskKind::Network: return 6;
    case DiskKind::Removable: return 5;
    case DiskKind::Hdd: return 4;
    case DiskKind::Unknown:
    case DiskKind::Virtual: return 3;
    case DiskKind::Ssd:
    case DiskKind::SataSsd: return 2;
    default: return 1;
    }
}

void HwWinQueryVolume(const wchar_t* volume_guid, Disk& d)
{
    // "\\?\Volume{...}" without the trailing backslash opens the volume device
    const HANDLE volume = CreateFileW(volume_guid, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
        nullptr);
    if (volume == INVALID_HANDLE_VALUE)
        return;

    u8 seek = VALUE_UNKNOWN, bus = VALUE_UNKNOWN, removable = 0;
    if (HwWinQueryStorage(volume, seek, bus, removable))
    {
        d.seek_penalty = seek;
        d.bus = bus;
        d.removable = removable;
        d.kind = HwWinKind(seek, bus, removable);
    }
    if (d.kind == DiskKind::Unknown)
    {
        // A composite volume (dynamic disk, RAID): the physical drives under it, the worst one counts
        alignas(8) u8 buffer[sizeof(VOLUME_DISK_EXTENTS) + 15 * sizeof(DISK_EXTENT)]{};
        DWORD returned = 0;
        if (DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer, sizeof(buffer),
                &returned, nullptr))
        {
            const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer);
            int worst = 0;
            for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < 16; ++i)
            {
                wchar_t drive_path[48];
                swprintf(drive_path, std::size(drive_path), L"\\\\.\\PhysicalDrive%lu", extents->Extents[i].DiskNumber);
                const HANDLE drive = CreateFileW(drive_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, 0, nullptr);
                if (drive == INVALID_HANDLE_VALUE)
                    continue;
                u8 dseek = VALUE_UNKNOWN, dbus = VALUE_UNKNOWN, dremovable = 0;
                if (HwWinQueryStorage(drive, dseek, dbus, dremovable))
                {
                    const DiskKind kind = HwWinKind(dseek, dbus, dremovable);
                    if (HwWinBadness(kind) > worst)
                    {
                        worst = HwWinBadness(kind);
                        d.kind = kind;
                        d.seek_penalty = dseek;
                        d.bus = dbus;
                        d.removable = dremovable;
                    }
                }
                CloseHandle(drive);
            }
        }
    }
    CloseHandle(volume);
}

// Classifies the volume of a path. Works through subst drives (the launcher starts the game from one):
// GetFinalPathNameByHandleW resolves them to the real volume. volume_guid receives "\\?\Volume{...}" for the
// comparison of the volumes (empty if unknown).
void HwWinClassifyPath(const wchar_t* path, Disk& d, wchar_t (&volume_guid)[64])
{
    volume_guid[0] = 0;
    if (!path || !path[0])
        return;

    if (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.')
    {
        d.kind = DiskKind::Network;
        xr_strcpy(d.volume, "UNC");
    }
    char letter = 0;
    if (path[0] && path[1] == L':')
    {
        letter = char(towupper(path[0]));
        const wchar_t root[] = { path[0], L':', L'\\', 0 };
        if (GetDriveTypeW(root) == DRIVE_REMOTE)
            d.kind = DiskKind::Network;
        xr_sprintf(d.volume, "%c:", letter);
    }

    ULARGE_INTEGER free_to_caller{}, total{};
    if (GetDiskFreeSpaceExW(path, &free_to_caller, &total, nullptr))
    {
        d.free_bytes = free_to_caller.QuadPart;
        d.total_bytes = total.QuadPart;
    }
    if (d.kind == DiskKind::Network)
        return;

    const HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    wchar_t final_path[MAX_PATH + 64];
    // The real drive behind a subst letter, or a network share
    DWORD n = GetFinalPathNameByHandleW(h, final_path, DWORD(std::size(final_path)), VOLUME_NAME_DOS);
    if (n && n < std::size(final_path))
    {
        if (!wcsncmp(final_path, L"\\\\?\\UNC\\", 8))
        {
            d.kind = DiskKind::Network;
            if (letter)
                xr_sprintf(d.volume, "%c:=UNC", letter);
            else
                xr_strcpy(d.volume, "UNC");
        }
        else if (!wcsncmp(final_path, L"\\\\?\\", 4) && final_path[5] == L':')
        {
            const char real = char(towupper(final_path[4]));
            if (letter && real != letter)
                xr_sprintf(d.volume, "%c:=%c:", letter, real);
            else if (!letter)
                xr_sprintf(d.volume, "%c:", real);
        }
    }
    if (d.kind != DiskKind::Network)
    {
        n = GetFinalPathNameByHandleW(h, final_path, DWORD(std::size(final_path)), VOLUME_NAME_GUID);
        if (n && n < std::size(final_path) && !wcsncmp(final_path, L"\\\\?\\Volume{", 11))
        {
            const wchar_t* end = wcschr(final_path, L'}');
            if (end && size_t(end - final_path) + 2 <= std::size(volume_guid))
            {
                const size_t len = size_t(end - final_path) + 1;
                std::memcpy(volume_guid, final_path, len * sizeof(wchar_t));
                volume_guid[len] = 0;
            }
        }
    }
    CloseHandle(h);

    if (volume_guid[0])
        HwWinQueryVolume(volume_guid, d);
}

void HwWinMarkSame(Disk& d, const wchar_t* guid, const wchar_t* other_guid, u8 same_as)
{
    if (!d.same_as && guid[0] && !wcscmp(guid, other_guid))
        d.same_as = same_as;
}

//------------------------------------------------------------------------------------------------------------------------
// -hw_limit

struct HwWinCore
{
    KAFFINITY mask; // allowed logical processors of the core
    BYTE eff_class; // higher = faster (P-cores of a hybrid CPU)
};

// Affinity mask of `logical` CPUs of the current processor group, picked by cores: first one logical CPU on each of
// `physical` cores (performance cores first, then in system order), then their SMT siblings, then - when the machine
// has fewer siblings than asked (no SMT) - one CPU of further cores. Simple on purpose: no cache / CCD topology.
KAFFINITY HwWinPickAffinity(u32 physical, u32 logical, KAFFINITY allowed, u32& cores_used)
{
    cores_used = 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len)
        return 0;
    xr_vector<u8> buffer(len);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &len))
        return 0;

    xr_vector<HwWinCore> cores;
    for (DWORD offset = 0; offset < len;)
    {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (!info->Size)
            break;
        if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1 &&
            info->Processor.GroupMask[0].Group == 0)
        {
            const KAFFINITY mask = info->Processor.GroupMask[0].Mask & allowed;
            if (mask)
                cores.push_back({ mask, info->Processor.EfficiencyClass });
        }
        offset += info->Size;
    }
    if (cores.empty())
        return 0; // no core of group 0 within the allowed mask (cores[0] below would be out of range)
    std::stable_sort(cores.begin(), cores.end(),
        [](const HwWinCore& a, const HwWinCore& b) { return a.eff_class > b.eff_class; });

    physical = std::max<u32>(1, std::min<u32>(physical ? physical : logical, u32(cores.size())));
    KAFFINITY result = 0;
    u32 count = 0;
    const auto lowest = [](KAFFINITY m) { return m & (~m + 1); };
    // 1. one CPU on each of the first `physical` cores
    for (u32 i = 0; i < physical && count < logical; ++i)
    {
        result |= lowest(cores[i].mask);
        ++count;
        ++cores_used;
    }
    // 2. their SMT siblings
    for (u32 i = 0; i < physical && count < logical; ++i)
    {
        for (KAFFINITY rest = cores[i].mask & ~result; rest && count < logical; rest &= rest - 1)
        {
            result |= lowest(rest);
            ++count;
        }
    }
    // 3. not enough siblings: further cores
    for (size_t i = physical; i < cores.size() && count < logical; ++i)
    {
        result |= lowest(cores[i].mask);
        ++count;
        ++cores_used;
    }
    return result;
}

u32 HwWinLimitAffinity(u32 physical, u32 logical)
{
    // SetProcessAffinityMask works inside one processor group; such machines (> 64 CPUs) are not the target of a test
    if (GetActiveProcessorGroupCount() > 1)
    {
        Msg("! [hw] -hw_limit: affinity is not supported with several processor groups (> 64 CPUs)");
        return 0;
    }
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) || !process_mask)
    {
        Msg("! [hw] -hw_limit: GetProcessAffinityMask failed (error %u)", GetLastError());
        return 0;
    }
    u32 cores = 0;
    const KAFFINITY mask = HwWinPickAffinity(physical, logical, KAFFINITY(process_mask), cores);
    if (!mask)
    {
        Msg("! [hw] -hw_limit: no CPU topology, affinity unchanged");
        return 0;
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask)))
    {
        Msg("! [hw] -hw_limit: SetProcessAffinityMask(0x%llx) failed (error %u)", u64(mask), GetLastError());
        return 0;
    }
    Msg("* [hw] -hw_limit: affinity 0x%llx - %u logical CPUs on %u cores (asked %u/%u, process had %u)", u64(mask),
        HwWinPopCount(mask), cores, physical, logical, HwWinPopCount(KAFFINITY(process_mask)));
    return OVR_LIMIT_CPU;
}

u32 HwWinLimitCommit(u64 commit)
{
    // A Job Object with a per-process commit limit: allocations beyond it fail as on a machine with that commit limit.
    // Nested jobs need Windows 8+ (a launcher may already have put the process into a job). The handle is closed: the
    // job lives while the process is in it, and without KILL_ON_JOB_CLOSE closing it changes nothing.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
    {
        Msg("! [hw] -hw_limit: CreateJobObject failed (error %u)", GetLastError());
        return 0;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    info.ProcessMemoryLimit = SIZE_T(commit);
    u32 applied = 0;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
        Msg("! [hw] -hw_limit: SetInformationJobObject failed (error %u)", GetLastError());
    else if (!AssignProcessToJobObject(job, GetCurrentProcess()))
        Msg("! [hw] -hw_limit: AssignProcessToJobObject failed (error %u)", GetLastError());
    else
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        const bool have = GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE;
        Msg("* [hw] -hw_limit: job commit limit %.2f GB (committed now %.2f GB)", double(commit) / double(u64(1) << 30),
            have ? double(counters.PrivateUsage) / double(u64(1) << 30) : 0.0);
        applied = OVR_LIMIT_COMMIT;
        s_hw_win_job_commit_limit = commit;
    }
    CloseHandle(job);
    return applied;
}
} // namespace

u32 PlatformApplyLimits(u32 physical, u32 logical, u64 commit)
{
    u32 applied = 0;
    if (logical)
        applied |= HwWinLimitAffinity(physical, logical);
    if (commit)
        applied |= HwWinLimitCommit(commit);
    return applied;
}

void PlatformCollectStageA(Profile& p)
{
    HwWinCollectCpu(p);
    HwWinCollectBrand(p);
    HwWinCollectMemory(p);
    HwWinCollectEnv(p);
}

void PlatformCollectStageB(Profile& p, pcstr game_path, pcstr appdata_path)
{
    wchar_t game_guid[64]{}, appdata_guid[64]{}, system_guid[64]{}, swap_guid[64]{};
    if (game_path && game_path[0])
        HwWinClassifyPath(XRay::Utf8::ToWide(game_path).c_str(), p.data.game, game_guid);
    if (appdata_path && appdata_path[0])
    {
        HwWinClassifyPath(XRay::Utf8::ToWide(appdata_path).c_str(), p.data.appdata, appdata_guid);
        HwWinMarkSame(p.data.appdata, appdata_guid, game_guid, 1);
    }

    wchar_t system_root[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"SystemDrive", system_root, MAX_PATH - 2);
    if (n && n < MAX_PATH - 2)
    {
        system_root[n] = L'\\';
        system_root[n + 1] = 0;
        HwWinClassifyPath(system_root, p.data.system, system_guid);
        HwWinMarkSame(p.data.system, system_guid, game_guid, 1);
        HwWinMarkSame(p.data.system, system_guid, appdata_guid, 2);
    }

    if (p.mem.swap_volume[0])
    {
        const wchar_t swap_root[] = { wchar_t(p.mem.swap_volume[0]), L':', L'\\', 0 };
        HwWinClassifyPath(swap_root, p.data.swap, swap_guid);
        HwWinMarkSame(p.data.swap, swap_guid, game_guid, 1);
        HwWinMarkSame(p.data.swap, swap_guid, appdata_guid, 2);
        HwWinMarkSame(p.data.swap, swap_guid, system_guid, 3);
    }
}

bool PlatformQueryMemory(u64& avail_phys, u64& commit_avail)
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms))
        return false;
    avail_phys = ms.ullAvailPhys;
    commit_avail = ms.ullAvailPageFile;
    if (s_hw_win_job_commit_limit)
    {
        // -hw_limit: the job's headroom = its limit - the commit of the process
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(counters)))
        {
            const u64 used = counters.PrivateUsage;
            const u64 job_avail = s_hw_win_job_commit_limit > used ? s_hw_win_job_commit_limit - used : 0;
            commit_avail = std::min<u64>(commit_avail, job_avail);
        }
    }
    return true;
}

// R14, live: the same reading as stage A (PlatformCollectStageA). GetSystemPowerStatus is one cheap system call.
bool PlatformQueryPower(bool& on_battery, bool& saver)
{
    SYSTEM_POWER_STATUS ps{};
    if (!GetSystemPowerStatus(&ps))
        return false;
    const bool has_battery = ps.BatteryFlag != 255 && !(ps.BatteryFlag & 128);
    on_battery = ps.ACLineStatus == 0 && has_battery;
    saver = ps.SystemStatusFlag == 1;
    return true;
}
} // namespace xray::hw::detail
#endif // XR_PLATFORM_WINDOWS
