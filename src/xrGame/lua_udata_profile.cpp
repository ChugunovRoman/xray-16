#include "pch_script.h"
#include "lua_udata_profile.h"

#include "xrCore/Profiling/PerfMetrics.hpp"

// U1 (32-frame-spikes.md): the hook of luabind push_new_instance (the GlobalWar patch of Externals/luabind) and the
// class of the instance (class_rep::name, constructor_dispatcher)
#include <luabind/detail/class_rep.hpp>
#include <luabind/detail/object_rep.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#if defined(_MSC_VER)
#include <intrin.h> // _ReturnAddress (U2)
#endif

namespace gw::lua_udata
{
int g_enabled = 0;
int g_sample = 64;

namespace
{
using luabind::detail::class_rep;
using ull = unsigned long long;

constexpr u32 kClassSlots = 4096; // a power of 2; a game has ~1-2 thousand luabind classes (C++ and Lua)
constexpr u32 kNoClass = kClassSlots; // the instance of a class that did not fit the table
constexpr size_t kMaxSites = 32768;
constexpr size_t kNameLen = 48;
constexpr size_t kFuncLen = 32;
constexpr size_t kSrcLen = 72; // short_src (LUA_IDSIZE 60) + ":" + the line
constexpr int kCbScanSlots = 12; // how deep the stack is searched for the Lua function about to be called (cb)
constexpr int kMaxLevels = 64; // how deep the call stack is searched for two Lua frames

enum EKind : u8
{
    KindNew, // class_rep::constructor_dispatcher: vector(), CTime(), a Lua class called from Lua
    KindC, // pushed by a C function that runs under Lua: its return value or an argument of a nested callback
    KindEngine, // no Lua function running: the engine pushes the arguments of a Lua call (binder, callbacks)
    KindCount
};
constexpr pcstr kKindNames[KindCount] = {"new", "c", "engine"};

// The classes with their own perf_metrics counter
enum EHot : u8
{
    HotNone,
    HotGameObject,
    HotVector,
    HotCTime,
    HotDevice,
    HotAlife,
};

struct ClassSlot
{
    std::atomic<const class_rep*> key{nullptr};
    std::atomic<u32> ready{0}; // the fields below written (release) by the thread that took the slot
    std::atomic<u8> hot{HotNone};
    bool lua_class = false;
    char name[kNameLen]{}; // a copy: the class_rep of a Lua class lives in the Lua heap
    std::atomic<u64> created{0};
    std::atomic<u64> samples{0};
    std::atomic<u64> kind_samples[KindCount]{};
};

ClassSlot g_classes[kClassSlots]; // open addressing by class_rep*, never cleared (only the counters are reset)
std::atomic<u64> g_class_overflow{0};

// One sample: what made the instance and where. Zeroed as a whole before filling (it is hashed byte by byte).
struct SiteKey
{
    u32 cls; // index in g_classes, kNoClass = not in the table
    u8 kind; // EKind
    char cfunc[kFuncLen]; // the C function running at level 0 as its Lua caller names it ("position", "object_by_id")
    char site[kSrcLen]; // the first Lua frame: source:currentline; "<engine>" = none
    char caller[kSrcLen]; // the next Lua frame
    char cb[kSrcLen]; // the Lua function on the stack about to be called: source:linedefined
};

struct SiteEntry
{
    SiteKey key;
    u64 samples = 0;
};

std::mutex g_sites_mtx;
xr_unordered_map<u64, SiteEntry> g_sites; // by the hash of the key; under g_sites_mtx with the 3 counters below
u64 g_samples = 0;
u64 g_site_overflow = 0; // samples dropped: the table is full (kMaxSites)
u64 g_site_collisions = 0; // samples dropped: two keys with one hash

std::atomic<u64> g_window_start{0}; // CPU::QPC() at the start of the window, 0 = never enabled
std::atomic<u64> g_window_end{0}; // CPU::QPC() at lua_udata_profile 0, 0 = still running
bool g_installed = false; // the console thread only

thread_local s32 t_countdown = 0; // instances until the next sample on this thread
thread_local u32 t_rng = 0;

template <size_t N>
void CopyStr(char (&dst)[N], pcstr src) noexcept
{
    size_t i = 0;
    if (src)
    {
        for (; i + 1 < N && src[i]; ++i)
            dst[i] = src[i];
    }
    dst[i] = 0;
}

// Random interval 1..2N-1 (mean N): a fixed period would alias with loops that make a fixed number of instances
u32 NextInterval() noexcept
{
    const u32 n = static_cast<u32>(std::clamp(g_sample, 1, 65536));
    if (n == 1)
        return 1;
    u32 x = t_rng;
    if (!x)
        x = 0x9E3779B9u ^ static_cast<u32>(reinterpret_cast<uintptr_t>(&t_rng));
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t_rng = x;
    return 1 + x % (2 * n - 1);
}

u32 SlotIndex(const class_rep* cls) noexcept
{
    u64 v = static_cast<u64>(reinterpret_cast<uintptr_t>(cls));
    v ^= v >> 17;
    v *= 0x9E3779B97F4A7C15ull;
    return static_cast<u32>(v >> 40) & (kClassSlots - 1);
}

u8 HotOf(pcstr name) noexcept
{
    if (!xr_strcmp(name, "game_object"))
        return HotGameObject;
    if (!xr_strcmp(name, "vector"))
        return HotVector;
    if (!xr_strcmp(name, "CTime"))
        return HotCTime;
    if (!xr_strcmp(name, "render_device"))
        return HotDevice;
    if (!xr_strcmp(name, "alife_simulator"))
        return HotAlife;
    return HotNone;
}

// Called on the thread that runs Lua, from inside luabind: no Lua errors, no Lua calls
ClassSlot* FindSlot(const class_rep* cls) noexcept
{
    u32 i = SlotIndex(cls);
    for (u32 probe = 0; probe < kClassSlots; ++probe, i = (i + 1) & (kClassSlots - 1))
    {
        ClassSlot& s = g_classes[i];
        const class_rep* k = s.key.load(std::memory_order_acquire);
        if (k == cls)
            return &s;
        if (k)
            continue;
        if (s.key.compare_exchange_strong(k, cls, std::memory_order_acq_rel))
        {
            const pcstr name = cls->name();
            CopyStr(s.name, name && name[0] ? name : "?");
            s.lua_class = cls->get_class_type() == class_rep::lua_class;
            s.hot.store(HotOf(s.name), std::memory_order_relaxed);
            s.ready.store(1, std::memory_order_release);
            return &s;
        }
        if (k == cls) // another thread took this slot for the same class
            return &s;
    }
    return nullptr;
}

// FNV-1a over a key zeroed as a whole before filling
u64 HashBytes(const void* data, size_t size) noexcept
{
    const auto* p = static_cast<const u8*>(data);
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

u64 HashKey(const SiteKey& key) noexcept { return HashBytes(&key, sizeof(key)); }

bool SameKey(const SiteKey& a, const SiteKey& b) noexcept
{
    return a.cls == b.cls && a.kind == b.kind && !std::strcmp(a.cfunc, b.cfunc) && !std::strcmp(a.site, b.site) &&
        !std::strcmp(a.caller, b.caller) && !std::strcmp(a.cb, b.cb);
}

void FormatSource(char (&dst)[kSrcLen], const lua_Debug& ar, int line) noexcept
{
    std::snprintf(dst, kSrcLen, "%s:%d", ar.short_src, line);
}

// The sampled path. The new instance is on the top of the stack of L. lua_getstack/lua_getinfo do not allocate in
// the Lua heap; "f" and ">" use one slot of the stack (lua_checkstack first).
void Sample(lua_State* L, ClassSlot* slot) noexcept
{
    PERF_SCOPE(lua_udata_sample_time);
    PERF_INC(lua_udata_sampled);

    SiteKey key;
    std::memset(&key, 0, sizeof(key));
    key.cls = slot ? static_cast<u32>(slot - g_classes) : kNoClass;
    key.kind = KindEngine;

    lua_Debug ar;
    int found = 0;
    if (lua_getstack(L, 0, &ar))
    {
        key.kind = KindC;
        if (lua_checkstack(L, 2) && lua_getinfo(L, "Snf", &ar))
        {
            if (ar.what && !std::strcmp(ar.what, "C"))
            {
                if (lua_tocfunction(L, -1) == &class_rep::constructor_dispatcher)
                    key.kind = KindNew;
                CopyStr(key.cfunc, ar.name ? ar.name : "?");
            }
            lua_pop(L, 1);
        }
        for (int level = 0; level < kMaxLevels && found < 2 && lua_getstack(L, level, &ar); ++level)
        {
            if (!lua_getinfo(L, "Sl", &ar) || ar.currentline < 0) // a C frame
                continue;
            FormatSource(found == 0 ? key.site : key.caller, ar, ar.currentline);
            ++found;
        }
    }
    if (!found)
        CopyStr(key.site, "<engine>");

    // The Lua function about to be called: luabind call_function/call_member push it, then the arguments (this
    // instance is one of them). In a C function called from Lua the slots are its arguments and results instead -
    // a Lua function there is an argument (obj:iterate_inventory(fn) - it is then the callback too).
    const int top = lua_gettop(L);
    for (int i = top - 1; i >= 1 && i >= top - kCbScanSlots; --i)
    {
        if (lua_type(L, i) != LUA_TFUNCTION || lua_iscfunction(L, i))
            continue;
        if (lua_checkstack(L, 1))
        {
            lua_pushvalue(L, i);
            if (lua_getinfo(L, ">S", &ar)) // pops the function
                FormatSource(key.cb, ar, ar.linedefined);
        }
        break;
    }

    if (slot)
    {
        slot->samples.fetch_add(1, std::memory_order_relaxed);
        slot->kind_samples[key.kind].fetch_add(1, std::memory_order_relaxed);
    }

    const u64 hash = HashKey(key);
    std::lock_guard<std::mutex> guard(g_sites_mtx);
    ++g_samples;
    const auto it = g_sites.find(hash);
    if (it != g_sites.end())
    {
        if (SameKey(it->second.key, key))
            ++it->second.samples;
        else
            ++g_site_collisions;
    }
    else if (g_sites.size() < kMaxSites)
        g_sites.emplace(hash, SiteEntry{key, 1});
    else
        ++g_site_overflow;
}

// luabind::detail::instance_create_hook
void OnInstance(lua_State* L, class_rep* cls) noexcept
{
    ClassSlot* slot = FindSlot(cls);
    if (slot)
    {
        slot->created.fetch_add(1, std::memory_order_relaxed);
        switch (slot->hot.load(std::memory_order_relaxed))
        {
        case HotGameObject: PERF_INC(lua_udata_game_object); break;
        case HotVector: PERF_INC(lua_udata_vector); break;
        case HotCTime: PERF_INC(lua_udata_ctime); break;
        case HotDevice: PERF_INC(lua_udata_device); break;
        case HotAlife: PERF_INC(lua_udata_alife); break;
        default: break;
        }
    }
    else
        g_class_overflow.fetch_add(1, std::memory_order_relaxed);
    PERF_INC(lua_udata_created);

    if (--t_countdown > 0)
        return;
    t_countdown = static_cast<s32>(NextInterval());
    Sample(L, slot);
}

void ResetWindow() noexcept
{
    for (ClassSlot& s : g_classes)
    {
        s.created.store(0, std::memory_order_relaxed);
        s.samples.store(0, std::memory_order_relaxed);
        for (auto& k : s.kind_samples)
            k.store(0, std::memory_order_relaxed);
    }
    g_class_overflow.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(g_sites_mtx);
        g_sites.clear();
        g_samples = 0;
        g_site_overflow = 0;
        g_site_collisions = 0;
    }
    g_window_end.store(0, std::memory_order_relaxed);
    g_window_start.store(CPU::QPC(), std::memory_order_relaxed);
}

// --- the report ---

struct ClassRow
{
    u32 index;
    pcstr name;
    bool lua_class;
    u64 created;
    u64 samples;
    u64 kinds[KindCount];
};

struct Group // the entries with one (class, kind, cfunc, site, cb): [first, first + count) of the sorted entries
{
    u32 first;
    u32 count;
    u64 samples;
    u64 estimate;
};

int CompareGroupKey(const SiteKey& a, const SiteKey& b) noexcept
{
    if (a.cls != b.cls)
        return a.cls < b.cls ? -1 : 1;
    if (a.kind != b.kind)
        return a.kind < b.kind ? -1 : 1;
    if (const int c = std::strcmp(a.cfunc, b.cfunc))
        return c;
    if (const int c = std::strcmp(a.site, b.site))
        return c;
    return std::strcmp(a.cb, b.cb);
}

struct ReportData
{
    xr_vector<ClassRow> classes; // sorted by created, descending
    xr_vector<s32> row_of; // g_classes index (and kNoClass) -> index in classes, -1 = none
    xr_vector<SiteEntry> entries; // sorted by the group key, then by samples descending
    xr_vector<Group> groups; // sorted by estimate, descending
    u64 total_created = 0;
    u64 overflow_created = 0;
    double secs = 0;
};

u64 Estimate(const ReportData& d, u32 cls, u64 samples) noexcept
{
    const s32 row = d.row_of[cls];
    if (row >= 0 && d.classes[row].samples)
        return static_cast<u64>(static_cast<double>(d.classes[row].created) * samples / d.classes[row].samples + 0.5);
    return samples * static_cast<u64>(std::max(g_sample, 1)); // the class table overflow
}

pcstr ClassName(const ReportData& d, u32 cls) noexcept
{
    const s32 row = d.row_of[cls];
    return row >= 0 ? d.classes[row].name : "<overflow>";
}

double PerSec(const ReportData& d, u64 n) noexcept { return d.secs > 0 ? n / d.secs : 0.0; }

double Pct(u64 part, u64 whole) noexcept { return whole ? 100.0 * part / whole : 0.0; }

void PrintGroup(const ReportData& d, u32 rank, const Group& g, u32 max_callers, double min_caller_pct) noexcept
{
    const SiteKey& k = d.entries[g.first].key;
    const s32 row = d.row_of[k.cls];
    const u64 class_created = row >= 0 ? d.classes[row].created : d.overflow_created;
    char cb[kSrcLen + 8] = "";
    if (k.cb[0])
        std::snprintf(cb, sizeof(cb), " cb=%s", k.cb);
    Msg("[lua-udata] %3u. %-24s %-6s %s%s @ %s%s  ~%llu (%.0f/s, %.1f%% all, %.1f%% class) n=%llu", rank,
        ClassName(d, k.cls), kKindNames[k.kind], k.cfunc[0] ? k.cfunc : "-", k.cfunc[0] ? "()" : "", k.site, cb,
        static_cast<ull>(g.estimate), PerSec(d, g.estimate), Pct(g.estimate, d.total_created),
        Pct(g.estimate, class_created), static_cast<ull>(g.samples));
    for (u32 j = 0; j < g.count && j < max_callers; ++j)
    {
        const SiteEntry& e = d.entries[g.first + j];
        const double pct = Pct(e.samples, g.samples);
        if (pct < min_caller_pct)
            break;
        Msg("[lua-udata]          <- %s  %.0f%%", e.key.caller[0] ? e.key.caller : "(no Lua caller)", pct);
    }
}

void Collect(ReportData& d) noexcept
{
    const u64 start = g_window_start.load(std::memory_order_relaxed);
    const u64 end = g_window_end.load(std::memory_order_relaxed);
    d.secs = static_cast<double>((end ? end : CPU::QPC()) - start) / static_cast<double>(CPU::qpc_freq);

    d.row_of.assign(kClassSlots + 1, -1);
    for (u32 i = 0; i < kClassSlots; ++i)
    {
        const ClassSlot& s = g_classes[i];
        if (!s.ready.load(std::memory_order_acquire))
            continue;
        ClassRow r{};
        r.index = i;
        r.name = s.name;
        r.lua_class = s.lua_class;
        r.created = s.created.load(std::memory_order_relaxed);
        r.samples = s.samples.load(std::memory_order_relaxed);
        for (u32 k = 0; k < KindCount; ++k)
            r.kinds[k] = s.kind_samples[k].load(std::memory_order_relaxed);
        if (!r.created && !r.samples)
            continue;
        d.total_created += r.created;
        d.classes.push_back(r);
    }
    d.overflow_created = g_class_overflow.load(std::memory_order_relaxed);
    d.total_created += d.overflow_created;
    std::sort(d.classes.begin(), d.classes.end(),
        [](const ClassRow& a, const ClassRow& b) { return a.created > b.created; });
    for (size_t r = 0; r < d.classes.size(); ++r)
        d.row_of[d.classes[r].index] = static_cast<s32>(r);

    {
        std::lock_guard<std::mutex> guard(g_sites_mtx);
        d.entries.reserve(g_sites.size());
        for (const auto& [hash, e] : g_sites)
            d.entries.push_back(e);
    }
    std::sort(d.entries.begin(), d.entries.end(), [](const SiteEntry& a, const SiteEntry& b) {
        const int c = CompareGroupKey(a.key, b.key);
        return c ? c < 0 : a.samples > b.samples;
    });
    for (u32 i = 0; i < d.entries.size();)
    {
        Group g{i, 0, 0, 0};
        for (; i < d.entries.size() && !CompareGroupKey(d.entries[g.first].key, d.entries[i].key); ++i)
        {
            ++g.count;
            g.samples += d.entries[i].samples;
        }
        g.estimate = Estimate(d, d.entries[g.first].key.cls, g.samples);
        d.groups.push_back(g);
    }
    std::sort(d.groups.begin(), d.groups.end(), [](const Group& a, const Group& b) { return a.estimate > b.estimate; });
}

#ifdef LUAJIT_GW_UDATA_HOOK
// --- U2: every userdata (the LuaJIT hook luaJIT_gw_udata_hook in lj_udata_new) ---
//
// The luabind hook sees only push_new_instance. The GC sees every userdata: the luabind iterators of return_stl_iterator
// (a userdata + a metatable + a closure on every call), luabind function objects, engine holders (gw_condlist
// CompiledCondlist*), newproxy, io files. This counts them all (exact: count, bytes, by payload size) and samples the
// creator of one in lua_udata_profile_sample: the C function running at level 0 (its address and its Lua name), the
// first two Lua frames, and on Windows the native callers of lua_newuserdata (the 4 return addresses after the LuaJIT
// frames), named by dbghelp in the report.

constexpr u32 kSizeExact = 512; // payload sizes counted one by one; bigger ones by power of 2
constexpr u32 kSizeBuckets = 32;
constexpr int kNativeFrames = 4; // native callers kept: [0] = the caller of lua_newuserdata (or of lj_udata_new)
constexpr int kNativeCapture = 32; // frames captured to find them

enum EUdKind : u8
{
    UdC, // a C function (or a builtin) runs at level 0: it makes the userdata
    UdLua, // a Lua function at level 0 (a debug hook): unusual
    UdEngine, // no Lua function running: the engine pushes it (the arguments of a call into Lua)
    UdKindCount
};
constexpr pcstr kUdKindNames[UdKindCount] = {"c", "lua", "engine"};

std::atomic<u64> g_ud_count{0};
std::atomic<u64> g_ud_bytes{0};
std::atomic<u64> g_ud_size_exact[kSizeExact]{};
std::atomic<u64> g_ud_size_big[kSizeBuckets]{}; // [i]: payload in [2^i, 2^(i+1)), only i >= 9 are used

// One sample. Zeroed as a whole before filling: hashed and compared byte by byte.
struct UdKey
{
    u32 size; // payload, bytes
    u8 kind; // EUdKind
    u64 cfunc; // the C function at level 0 (luaJIT_gw_cfunc), 0 = none
    u64 native[kNativeFrames]; // return addresses after the LuaJIT frames, 0 = none (off Windows: always)
    char cname[kFuncLen]; // its name as its Lua caller sees it (lua_getinfo "n")
    char site[kSrcLen]; // the first Lua frame: source:currentline; "<engine>" = none
    char caller[kSrcLen]; // the next Lua frame
};

struct UdEntry
{
    UdKey key;
    u64 samples = 0;
};

std::mutex g_ud_mtx;
xr_unordered_map<u64, UdEntry> g_ud_sites; // under g_ud_mtx with the 3 counters below
u64 g_ud_samples = 0;
u64 g_ud_overflow = 0;
u64 g_ud_collisions = 0;

thread_local s32 t_ud_countdown = 0;

#if defined(XR_PLATFORM_WINDOWS)
std::atomic<uintptr_t> g_lj_base{0}; // [base, end) of the LuaJIT module, from the first sample
std::atomic<uintptr_t> g_lj_end{0};

bool ModuleRange(const void* addr, uintptr_t& base, uintptr_t& end) noexcept
{
    HMODULE mod{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(addr), &mod) ||
        !mod)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const u8*>(mod) + dos->e_lfanew);
    base = reinterpret_cast<uintptr_t>(mod);
    end = base + nt->OptionalHeader.SizeOfImage;
    return true;
}
#endif

// ret = the return address of OnUdata: inside lj_udata_new, so in the LuaJIT module. The stack is ours (Sample, OnUdata),
// then LuaJIT (lj_udata_new, lua_newuserdata), then the native caller.
void CaptureNative(UdKey& key, const void* ret) noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    uintptr_t base = g_lj_base.load(std::memory_order_acquire);
    uintptr_t end = g_lj_end.load(std::memory_order_relaxed);
    if (!base)
    {
        if (!ModuleRange(ret, base, end))
            return;
        g_lj_end.store(end, std::memory_order_relaxed);
        g_lj_base.store(base, std::memory_order_release);
    }
    const auto inLuaJit = [base, end](void* p) {
        const auto a = reinterpret_cast<uintptr_t>(p);
        return a >= base && a < end;
    };
    void* frames[kNativeCapture];
    const int n = RtlCaptureStackBackTrace(0, kNativeCapture, frames, nullptr);
    int i = 0;
    while (i < n && !inLuaJit(frames[i])) // our frames
        ++i;
    while (i < n && inLuaJit(frames[i])) // lj_udata_new, lua_newuserdata
        ++i;
    for (int k = 0; k < kNativeFrames && i < n; ++k, ++i)
        key.native[k] = static_cast<u64>(reinterpret_cast<uintptr_t>(frames[i]));
#else
    (void)key;
    (void)ret;
#endif
}

// The sampled path, inside lj_udata_new: the userdata is not on the stack yet. Only lua_getstack/lua_getinfo without
// "f" and ">" (they push nothing), no Lua heap.
void SampleUdata(lua_State* L, size_t size, const void* ret) noexcept
{
    PERF_SCOPE(lua_udata_all_sample_time);
    PERF_INC(lua_udata_all_sampled);

    UdKey key;
    std::memset(&key, 0, sizeof(key));
    key.size = static_cast<u32>(std::min<size_t>(size, type_max<u32>));
    key.kind = UdEngine;

    lua_Debug ar;
    int found = 0;
    if (lua_getstack(L, 0, &ar))
    {
        key.kind = UdC;
        if (lua_getinfo(L, "Sn", &ar))
        {
            if (ar.what && !std::strcmp(ar.what, "C"))
            {
                key.cfunc = static_cast<u64>(reinterpret_cast<uintptr_t>(luaJIT_gw_cfunc(L, &ar)));
                CopyStr(key.cname, ar.name ? ar.name : "?");
            }
            else
                key.kind = UdLua;
        }
        for (int level = 0; level < kMaxLevels && found < 2 && lua_getstack(L, level, &ar); ++level)
        {
            if (!lua_getinfo(L, "Sl", &ar) || ar.currentline < 0) // a C frame
                continue;
            FormatSource(found == 0 ? key.site : key.caller, ar, ar.currentline);
            ++found;
        }
    }
    if (!found)
        CopyStr(key.site, key.kind == UdEngine ? "<engine>" : "<no Lua frame>");
    CaptureNative(key, ret);

    const u64 hash = HashBytes(&key, sizeof(key));
    std::lock_guard<std::mutex> guard(g_ud_mtx);
    ++g_ud_samples;
    const auto it = g_ud_sites.find(hash);
    if (it != g_ud_sites.end())
    {
        if (!std::memcmp(&it->second.key, &key, sizeof(key)))
            ++it->second.samples;
        else
            ++g_ud_collisions;
    }
    else if (g_ud_sites.size() < kMaxSites)
        g_ud_sites.emplace(hash, UdEntry{key, 1});
    else
        ++g_ud_overflow;
}

u32 FloorLog2(size_t v) noexcept
{
    u32 r = 0;
    while (v >>= 1)
        ++r;
    return r;
}

// luaJIT_gw_udata_hook: every new userdata, on the thread that runs Lua
void OnUdata(lua_State* L, void* /*p*/, size_t size)
{
#if defined(_MSC_VER)
    const void* ret = _ReturnAddress();
#else
    const void* ret = __builtin_return_address(0);
#endif
    g_ud_count.fetch_add(1, std::memory_order_relaxed);
    g_ud_bytes.fetch_add(size, std::memory_order_relaxed);
    if (size < kSizeExact)
        g_ud_size_exact[size].fetch_add(1, std::memory_order_relaxed);
    else
        g_ud_size_big[std::min(FloorLog2(size), kSizeBuckets - 1)].fetch_add(1, std::memory_order_relaxed);
    PERF_INC(lua_udata_all);

    if (--t_ud_countdown > 0)
        return;
    t_ud_countdown = static_cast<s32>(NextInterval());
    SampleUdata(L, size, ret);
}

void ResetUdWindow() noexcept
{
    g_ud_count.store(0, std::memory_order_relaxed);
    g_ud_bytes.store(0, std::memory_order_relaxed);
    for (auto& c : g_ud_size_exact)
        c.store(0, std::memory_order_relaxed);
    for (auto& c : g_ud_size_big)
        c.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(g_ud_mtx);
    g_ud_sites.clear();
    g_ud_samples = 0;
    g_ud_overflow = 0;
    g_ud_collisions = 0;
}

// --- the report of U2 ---

// What a payload size usually is (a hint: other userdata may have the same size)
pcstr SizeHint(u32 size) noexcept
{
    if (size == sizeof(luabind::detail::object_rep))
        return "luabind class instance (object_rep)";
    // The luabind instances without __gc (object_rep.hpp push_new_pointer_instance): the holder of a raw pointer inside
    // the userdata
    if (size == sizeof(luabind::detail::object_rep) + sizeof(luabind::detail::pointer_holder<int*>))
        return "luabind class instance over a raw pointer, without __gc (object_rep + pointer_holder)";
    if (size == sizeof(void*))
        return "a pointer: luabind function_object*, gw_condlist CompiledCondlist*";
    if (size == 2 * sizeof(void*))
        return "two pointers: luabind return_stl_iterator over a map/vector?";
    if (size == 0)
        return "newproxy() / empty";
    return "";
}

// Symbols of the addresses, by address (one dbghelp batch)
using SymbolMap = xr_unordered_map<u64, xr_string>;

void Symbolize(const xr_vector<u64>& addresses, SymbolMap& out) noexcept
{
    xr_vector<const void*> list;
    for (const u64 a : addresses)
    {
        if (a && !out.count(a))
        {
            out.emplace(a, xr_string());
            list.push_back(reinterpret_cast<const void*>(static_cast<uintptr_t>(a)));
        }
    }
    if (list.empty())
        return;
    const xr_vector<xr_string> names = xrDebug::SymbolizeAddresses(list);
    for (size_t i = 0; i < list.size() && i < names.size(); ++i)
        out[static_cast<u64>(reinterpret_cast<uintptr_t>(list[i]))] = names[i];
}

pcstr SymbolOf(const SymbolMap& symbols, u64 a) noexcept
{
    if (!a)
        return "-";
    const auto it = symbols.find(a);
    return it != symbols.end() && !it->second.empty() ? it->second.c_str() : "?";
}

// Up to 3 sizes of a group of entries with their share: "8 B 97%, 72 B 3%"
template <typename Pred>
void FormatSizes(const xr_vector<UdEntry>& entries, Pred&& in, u64 total, char* out, size_t outSize) noexcept
{
    xr_vector<std::pair<u32, u64>> sizes;
    for (const UdEntry& e : entries)
    {
        if (!in(e))
            continue;
        auto it = std::find_if(sizes.begin(), sizes.end(), [&](const auto& s) { return s.first == e.key.size; });
        if (it == sizes.end())
            sizes.emplace_back(e.key.size, e.samples);
        else
            it->second += e.samples;
    }
    std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    out[0] = 0;
    size_t used = 0;
    for (size_t i = 0; i < sizes.size() && i < 3 && used < outSize; ++i)
    {
        const int w = std::snprintf(out + used, outSize - used, "%s%u B %.0f%%", i ? ", " : "", sizes[i].first,
            Pct(sizes[i].second, total));
        if (w < 0)
            break;
        used += static_cast<size_t>(w);
    }
}

void ReportUdata(u32 top, u64 luabind_created, double secs) noexcept
{
    xr_vector<UdEntry> entries;
    u64 samples, overflow, collisions;
    {
        std::lock_guard<std::mutex> guard(g_ud_mtx);
        entries.reserve(g_ud_sites.size());
        for (const auto& [hash, e] : g_ud_sites)
            entries.push_back(e);
        samples = g_ud_samples;
        overflow = g_ud_overflow;
        collisions = g_ud_collisions;
    }
    const u64 total = g_ud_count.load(std::memory_order_relaxed);
    const u64 bytes = g_ud_bytes.load(std::memory_order_relaxed);
    const auto perSec = [secs](u64 n) { return secs > 0 ? n / secs : 0.0; };
    // Uniform sampling over all userdata: a group of s samples stands for s / samples of all
    const auto estimate = [&](u64 s) {
        return samples ? static_cast<u64>(static_cast<double>(total) * s / samples + 0.5) : 0;
    };
    static_assert(sizeof(luabind::detail::object_rep) < kSizeExact, "object_rep is counted by its exact size");
    const u32 repSize = sizeof(luabind::detail::object_rep);
    const u64 repCount = g_ud_size_exact[repSize].load(std::memory_order_relaxed);

    u64 kinds[UdKindCount]{};
    for (const UdEntry& e : entries)
        kinds[e.key.kind] += e.samples;

    Msg("[lua-udata-all] ===== all userdata (LuaJIT hook), window %.1f s: created %llu (%.0f/s), payload %.1f MB "
        "(avg %.1f B); samples %llu: c %.1f%% lua %.1f%% engine %.1f%%; dropped: site overflow %llu, collisions %llu; "
        "keys %u",
        secs, static_cast<ull>(total), perSec(total), bytes / (1024.0 * 1024.0), total ? double(bytes) / total : 0.0,
        static_cast<ull>(samples), Pct(kinds[UdC], samples), Pct(kinds[UdLua], samples), Pct(kinds[UdEngine], samples),
        static_cast<ull>(overflow), static_cast<ull>(collisions), static_cast<u32>(entries.size()));
    Msg("[lua-udata-all] luabind instances (push_new_instance hook) %llu (%.0f/s) = %.1f%% of all userdata; userdata of "
        "the object_rep size (%u B) %llu = %.1f%%; the rest %llu (%.0f/s) is not luabind instances",
        static_cast<ull>(luabind_created), perSec(luabind_created), Pct(luabind_created, total), repSize,
        static_cast<ull>(repCount), Pct(repCount, total),
        static_cast<ull>(total > luabind_created ? total - luabind_created : 0),
        perSec(total > luabind_created ? total - luabind_created : 0));
    Msg("[lua-udata-all] (to compare with [lua-gc-atomic] white=: a userdata with __gc is white in two cycles - found "
        "dead, finalized, then swept in the next one)");

    // Sizes (exact), each with its top creators from the samples
    {
        xr_vector<std::pair<u32, u64>> sizes;
        for (u32 s = 0; s < kSizeExact; ++s)
        {
            if (const u64 c = g_ud_size_exact[s].load(std::memory_order_relaxed))
                sizes.emplace_back(s, c);
        }
        std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        Msg("[lua-udata-all] by payload size (exact; top %u; <- C function or engine site from the samples):", top);
        for (u32 i = 0; i < sizes.size() && i < top; ++i)
        {
            const u32 size = sizes[i].first;
            Msg("[lua-udata-all] %3u. %5u B %10llu %9.0f/s %5.1f%%  %s", i + 1, size, static_cast<ull>(sizes[i].second),
                perSec(sizes[i].second), Pct(sizes[i].second, total), SizeHint(size));
            // The creators of this size: by C function name, or by the Lua site when no C function runs
            xr_vector<std::pair<pcstr, u64>> who;
            u64 sizeSamples = 0;
            for (const UdEntry& e : entries)
            {
                if (e.key.size != size)
                    continue;
                sizeSamples += e.samples;
                const pcstr name = e.key.cname[0] ? e.key.cname : e.key.site;
                auto it = std::find_if(who.begin(), who.end(), [&](const auto& w) { return !std::strcmp(w.first, name); });
                if (it == who.end())
                    who.emplace_back(name, e.samples);
                else
                    it->second += e.samples;
            }
            std::sort(who.begin(), who.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t j = 0; j < who.size() && j < 3; ++j)
            {
                if (Pct(who[j].second, sizeSamples) < 5.0)
                    break;
                Msg("[lua-udata-all]          <- %s  %.0f%% n=%llu", who[j].first, Pct(who[j].second, sizeSamples),
                    static_cast<ull>(who[j].second));
            }
        }
        for (u32 b = 0; b < kSizeBuckets; ++b)
        {
            if (const u64 c = g_ud_size_big[b].load(std::memory_order_relaxed))
            {
                Msg("[lua-udata-all]      [%llu, %llu) B %10llu %9.0f/s %5.1f%%", 1ull << b, 2ull << b,
                    static_cast<ull>(c), perSec(c), Pct(c, total));
            }
        }
    }

    // By C function (address + Lua name) over all sites
    struct Func
    {
        u8 kind;
        u64 cfunc;
        pcstr cname;
        u64 samples;
    };
    xr_vector<Func> funcs;
    for (const UdEntry& e : entries)
    {
        auto it = std::find_if(funcs.begin(), funcs.end(), [&](const Func& f) {
            return f.kind == e.key.kind && f.cfunc == e.key.cfunc && !std::strcmp(f.cname, e.key.cname);
        });
        if (it == funcs.end())
            funcs.push_back(Func{e.key.kind, e.key.cfunc, e.key.cname, e.samples});
        else
            it->samples += e.samples;
    }
    std::sort(funcs.begin(), funcs.end(), [](const Func& a, const Func& b) { return a.samples > b.samples; });

    // By native caller chain
    struct Chain
    {
        const u64* native;
        u64 samples;
    };
    xr_vector<Chain> chains;
    for (const UdEntry& e : entries)
    {
        auto it = std::find_if(chains.begin(), chains.end(), [&](const Chain& c) {
            return !std::memcmp(c.native, e.key.native, sizeof(e.key.native));
        });
        if (it == chains.end())
            chains.push_back(Chain{e.key.native, e.samples});
        else
            it->samples += e.samples;
    }
    std::sort(chains.begin(), chains.end(), [](const Chain& a, const Chain& b) { return a.samples > b.samples; });

    // One dbghelp batch for every address printed below
    SymbolMap symbols;
    {
        xr_vector<u64> addresses;
        for (u32 i = 0; i < funcs.size() && i < top; ++i)
            addresses.push_back(funcs[i].cfunc);
        for (u32 i = 0; i < chains.size() && i < top; ++i)
            addresses.insert(addresses.end(), chains[i].native, chains[i].native + kNativeFrames);
        Symbolize(addresses, symbols);
    }

    Msg("[lua-udata-all] by C function at level 0 (top %u; ~ = all userdata * share of samples):", top);
    for (u32 i = 0; i < funcs.size() && i < top; ++i)
    {
        const Func& f = funcs[i];
        char sizes[96];
        FormatSizes(entries,
            [&](const UdEntry& e) {
                return e.key.kind == f.kind && e.key.cfunc == f.cfunc && !std::strcmp(e.key.cname, f.cname);
            },
            f.samples, sizes, sizeof(sizes));
        const u64 est = estimate(f.samples);
        Msg("[lua-udata-all] %3u. %-6s %s%s ~%llu (%.0f/s, %.1f%%) n=%llu sizes: %s  @ %.220s", i + 1,
            kUdKindNames[f.kind], f.cname[0] ? f.cname : "-", f.cname[0] ? "()" : "", static_cast<ull>(est),
            perSec(est), Pct(f.samples, samples), static_cast<ull>(f.samples), sizes, SymbolOf(symbols, f.cfunc));
    }

#if defined(XR_PLATFORM_WINDOWS)
    Msg("[lua-udata-all] by native caller of lua_newuserdata (top %u; #0 = the caller, then its callers):", top);
    for (u32 i = 0; i < chains.size() && i < top; ++i)
    {
        const Chain& c = chains[i];
        char sizes[96];
        FormatSizes(entries, [&](const UdEntry& e) { return !std::memcmp(c.native, e.key.native, sizeof(e.key.native)); },
            c.samples, sizes, sizeof(sizes));
        const u64 est = estimate(c.samples);
        Msg("[lua-udata-all] %3u. ~%llu (%.0f/s, %.1f%%) n=%llu sizes: %s", i + 1, static_cast<ull>(est), perSec(est),
            Pct(c.samples, samples), static_cast<ull>(c.samples), sizes);
        for (int k = 0; k < kNativeFrames && c.native[k]; ++k)
            Msg("[lua-udata-all]        #%d %.220s", k, SymbolOf(symbols, c.native[k]));
    }
#endif

    // By Lua site: (kind, C function name, size, site), with the callers
    xr_vector<const UdEntry*> sorted;
    sorted.reserve(entries.size());
    for (const UdEntry& e : entries)
        sorted.push_back(&e);
    const auto groupCmp = [](const UdEntry* a, const UdEntry* b) {
        if (a->key.kind != b->key.kind)
            return a->key.kind < b->key.kind ? -1 : 1;
        if (const int c = std::strcmp(a->key.cname, b->key.cname))
            return c;
        if (a->key.size != b->key.size)
            return a->key.size < b->key.size ? -1 : 1;
        return std::strcmp(a->key.site, b->key.site);
    };
    std::sort(sorted.begin(), sorted.end(), [&](const UdEntry* a, const UdEntry* b) {
        const int c = groupCmp(a, b);
        return c ? c < 0 : a->samples > b->samples;
    });
    struct SiteGroup
    {
        u32 first;
        u32 count;
        u64 samples;
    };
    xr_vector<SiteGroup> groups;
    for (u32 i = 0; i < sorted.size();)
    {
        SiteGroup g{i, 0, 0};
        for (; i < sorted.size() && !groupCmp(sorted[g.first], sorted[i]); ++i)
        {
            ++g.count;
            g.samples += sorted[i]->samples;
        }
        groups.push_back(g);
    }
    std::sort(groups.begin(), groups.end(), [](const SiteGroup& a, const SiteGroup& b) { return a.samples > b.samples; });
    Msg("[lua-udata-all] top %u Lua sites (kind, C function, size @ site; <- callers):", top);
    for (u32 i = 0; i < groups.size() && i < top; ++i)
    {
        const SiteGroup& g = groups[i];
        const UdKey& k = sorted[g.first]->key;
        const u64 est = estimate(g.samples);
        Msg("[lua-udata-all] %3u. %-6s %s%s %u B @ %s  ~%llu (%.0f/s, %.1f%%) n=%llu", i + 1, kUdKindNames[k.kind],
            k.cname[0] ? k.cname : "-", k.cname[0] ? "()" : "", k.size, k.site, static_cast<ull>(est), perSec(est),
            Pct(g.samples, samples), static_cast<ull>(g.samples));
        // The callers: entries of the group differ by caller and by the native frames; merge by caller
        xr_vector<std::pair<pcstr, u64>> callers;
        for (u32 j = 0; j < g.count; ++j)
        {
            const UdEntry& e = *sorted[g.first + j];
            const pcstr name = e.key.caller[0] ? e.key.caller : "(no Lua caller)";
            auto it = std::find_if(callers.begin(), callers.end(), [&](const auto& c) { return !std::strcmp(c.first, name); });
            if (it == callers.end())
                callers.emplace_back(name, e.samples);
            else
                it->second += e.samples;
        }
        std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t j = 0; j < callers.size() && j < 3; ++j)
        {
            if (Pct(callers[j].second, g.samples) < 5.0)
                break;
            Msg("[lua-udata-all]          <- %s  %.0f%%", callers[j].first, Pct(callers[j].second, g.samples));
        }
    }
    Msg("[lua-udata-all] ===== end");
}
#endif // LUAJIT_GW_UDATA_HOOK

// The luabind section of lua_udata_report: instances by class, by class + C function, the sites
void ReportLuabind(const ReportData& d, u32 top, pcstr only) noexcept
{
    u64 samples, site_overflow, collisions;
    {
        std::lock_guard<std::mutex> guard(g_sites_mtx);
        samples = g_samples;
        site_overflow = g_site_overflow;
        collisions = g_site_collisions;
    }
    u64 kinds[KindCount]{};
    for (const ClassRow& r : d.classes)
    {
        for (u32 k = 0; k < KindCount; ++k)
            kinds[k] += r.kinds[k];
    }

    Msg("[lua-udata] ===== window %.1f s (%s), sample 1/%d: created %llu luabind instances (%.0f/s) in %u classes; "
        "samples %llu: new %.1f%% c %.1f%% engine %.1f%%; dropped: class overflow %llu, site overflow %llu, "
        "collisions %llu; sites %u",
        d.secs, g_installed ? "running" : "stopped", std::max(g_sample, 1), static_cast<ull>(d.total_created),
        PerSec(d, d.total_created), static_cast<u32>(d.classes.size()), static_cast<ull>(samples),
        Pct(kinds[KindNew], samples), Pct(kinds[KindC], samples), Pct(kinds[KindEngine], samples),
        static_cast<ull>(d.overflow_created), static_cast<ull>(site_overflow), static_cast<ull>(collisions),
        static_cast<u32>(d.entries.size()));

    Msg("[lua-udata] by class (top %u; * = Lua class; kinds from the samples):", top);
    for (u32 r = 0; r < d.classes.size() && r < top; ++r)
    {
        const ClassRow& c = d.classes[r];
        Msg("[lua-udata] %3u. %-24s%s %10llu %9.0f/s %5.1f%%  n=%llu new %.0f%% c %.0f%% engine %.0f%%", r + 1, c.name,
            c.lua_class ? "*" : " ", static_cast<ull>(c.created), PerSec(d, c.created), Pct(c.created, d.total_created),
            static_cast<ull>(c.samples), Pct(c.kinds[KindNew], c.samples), Pct(c.kinds[KindC], c.samples),
            Pct(c.kinds[KindEngine], c.samples));
    }

    if (!only[0])
    {
        // What API makes them: (class, kind, C function) over all sites
        struct Api
        {
            u32 cls;
            u8 kind;
            pcstr cfunc;
            u64 samples;
            u64 estimate;
        };
        xr_vector<Api> apis;
        for (const Group& g : d.groups)
        {
            const SiteKey& k = d.entries[g.first].key;
            auto it = std::find_if(apis.begin(), apis.end(), [&](const Api& a) {
                return a.cls == k.cls && a.kind == k.kind && !std::strcmp(a.cfunc, k.cfunc);
            });
            if (it == apis.end())
                apis.push_back(Api{k.cls, k.kind, k.cfunc, g.samples, 0});
            else
                it->samples += g.samples;
        }
        for (Api& a : apis)
            a.estimate = Estimate(d, a.cls, a.samples);
        std::sort(apis.begin(), apis.end(), [](const Api& a, const Api& b) { return a.estimate > b.estimate; });
        Msg("[lua-udata] by class + C function (top %u):", top);
        for (u32 i = 0; i < apis.size() && i < top; ++i)
        {
            const Api& a = apis[i];
            Msg("[lua-udata] %3u. %-24s %-6s %s%s  ~%llu (%.0f/s, %.1f%% all) n=%llu", i + 1, ClassName(d, a.cls),
                kKindNames[a.kind], a.cfunc[0] ? a.cfunc : "-", a.cfunc[0] ? "()" : "", static_cast<ull>(a.estimate),
                PerSec(d, a.estimate), Pct(a.estimate, d.total_created), static_cast<ull>(a.samples));
        }

        Msg("[lua-udata] top %u sites, all classes (~ = created of the class * share of its samples; <- callers):", top);
        for (u32 i = 0; i < d.groups.size() && i < top; ++i)
            PrintGroup(d, i + 1, d.groups[i], 3, 5.0);
    }

    // The sites of each big class (>= 1 % of all, up to 8 classes), or of the class named in the arguments
    u32 shown = 0;
    for (const ClassRow& c : d.classes)
    {
        if (only[0] ? xr_strcmp(c.name, only) != 0 : (shown >= 8 || Pct(c.created, d.total_created) < 1.0))
            continue;
        if (!c.samples)
            continue;
        ++shown;
        Msg("[lua-udata] --- %s: created %llu (%.0f/s), top %u sites:", c.name, static_cast<ull>(c.created),
            PerSec(d, c.created), top);
        u32 rank = 0;
        for (const Group& g : d.groups)
        {
            if (rank >= top)
                break;
            if (d.entries[g.first].key.cls != c.index)
                continue;
            PrintGroup(d, ++rank, g, only[0] ? 5 : 2, only[0] ? 3.0 : 10.0);
        }
    }
    if (only[0] && !shown)
        Msg("[lua-udata] class '%s': no samples in the window", only);
    Msg("[lua-udata] ===== end");
}

} // namespace

void Apply(bool enable) noexcept
{
    if (enable == g_installed)
        return;
    if (enable)
    {
        ResetWindow();
#ifdef LUAJIT_GW_UDATA_HOOK
        ResetUdWindow();
        luaJIT_gw_udata_hook(&OnUdata);
#endif
        luabind::detail::set_instance_create_hook(&OnInstance);
#ifdef LUAJIT_GW_UDATA_HOOK
        Msg("[lua-udata] profile on: a new window, sample 1/%d (luabind instances and all userdata)",
            std::max(g_sample, 1));
#else
        Msg("[lua-udata] profile on: a new window, sample 1/%d (luabind instances only: LuaJIT without the GlobalWar "
            "patch luaJIT_gw_udata_hook)",
            std::max(g_sample, 1));
#endif
    }
    else
    {
        luabind::detail::set_instance_create_hook(nullptr);
#ifdef LUAJIT_GW_UDATA_HOOK
        luaJIT_gw_udata_hook(nullptr);
#endif
        g_window_end.store(CPU::QPC(), std::memory_order_relaxed);
        Msg("[lua-udata] profile off: the window kept for lua_udata_report");
    }
    g_installed = enable;
}

void Report(pcstr args) noexcept
{
    bool reset = false;
    bool allOnly = false; // "all": only the section of all userdata
    u32 top = 40;
    string128 only{};
    if (args)
    {
        string512 buf;
        CopyStr(buf, args); // truncates (xr_strcpy would assert on a long line)
        for (pstr p = buf; *p;)
        {
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            const pstr tok = p;
            while (*p && *p != ' ' && *p != '\t')
                ++p;
            if (*p)
                *p++ = 0;
            if (!xr_strcmp(tok, "reset"))
                reset = true;
            else if (!xr_strcmp(tok, "all"))
                allOnly = true;
            else if (!strncmp(tok, "top=", 4))
                top = static_cast<u32>(std::clamp(atoi(tok + 4), 1, 500));
            else
                CopyStr(only, tok);
        }
    }

    if (!g_window_start.load(std::memory_order_relaxed))
    {
        Msg("[lua-udata] no window: set lua_udata_profile 1 first (needs luabind with the GlobalWar hook)");
        return;
    }

    ReportData d;
    Collect(d);

    if (!allOnly)
        ReportLuabind(d, top, only);
#ifdef LUAJIT_GW_UDATA_HOOK
    if (!only[0])
        ReportUdata(top, d.total_created, d.secs);
#else
    Msg("[lua-udata-all] unavailable: LuaJIT without the GlobalWar patch (luajit.h LUAJIT_GW_UDATA_HOOK)");
#endif

    if (reset && g_installed)
    {
        ResetWindow();
#ifdef LUAJIT_GW_UDATA_HOOK
        ResetUdWindow();
#endif
        Msg("[lua-udata] a new window");
    }
}
} // namespace gw::lua_udata
