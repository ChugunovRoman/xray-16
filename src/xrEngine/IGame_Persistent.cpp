#include "stdafx.h"
#pragma hdrstop

#include "IGame_Persistent.h"
#include "GameFont.h"
#include "ILoadingScreen.h"
#include "PerformanceAlert.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "StringTable/StringTable.h"
#include "xrScriptEngine/script_engine.hpp"

#ifndef _EDITOR
#include "Environment.h"
#include "IGame_Level.h"
#include "XR_IOConsole.h"
#include "Render.h"
#include "PS_instance.h"
#include "ParticleWorker.h"
#include "CustomHUD.h"
#include "xrCore/BackgroundIo.h"
#include "xrCore/Platform/HwProfile.h"
#include "xrCore/Profiling/StartupMarks.hpp"

#include <algorithm>
#include <cstdlib>
#endif

ENGINE_API IGame_Persistent* g_pGamePersistent = nullptr;

#ifndef _EDITOR
namespace
{
// 50a-stage1-prefetch.md (plans/optimization_mt): the prefetch budget (plan 50 R1 variant B) and the warm-up limits (R4)
constexpr u64 kPrefetchMiB = 1024ull * 1024;
constexpr u64 kPrefetchGiB = 1024 * kPrefetchMiB;
// A list line without a size: about the mean line of prefetch_single.ltx (7.6 GB of files over 5238 lines)
constexpr u64 kPrefetchDefaultLineBytes = 1536 * 1024;
// Process commit per byte of the list files once they are loaded (vertex / index buffers with their RAM copies,
// motions, textures, the objects). Measured on the tester's machine with the full list, 7.6 GB of files (plan 50,
// section 15): texture_lod 1 - x1.57, texture_lod 2 - x1.29, texture_lod 3 - x1.24. The textures stage also uploads
// the level textures (not in the lists), so the ratios are on the safe side. The "[prefetch] budget" line prints the
// real ratio of each run.
constexpr double kPrefetchMemPerFileByteFull = 1.6; // texture_lod 0..1
constexpr double kPrefetchMemPerFileByteReduced = 1.3; // texture_lod 2+ (a quarter of the texture memory or less)
// psTextureLOD holds the effective value here: an auto texture_lod is resolved at the device creation
// (50d-stage2-auto-engine.md), before the prefetch
double prefetch_mem_per_file_byte() { return psTextureLOD <= 1 ? kPrefetchMemPerFileByteFull : kPrefetchMemPerFileByteReduced; }
// R4: the warm threads read at most min(1/4 of the available RAM, 2 GB) ahead of the loads; below 3 GB available no
// dds (the textures stage comes after all the models, the OS cache would drop them first - a second read), below
// 1.5 GB no warm-up at all. 2 GB is above every 256-item window of the lists (the old lead, kStartupWarmLead):
// objects 1.7 GB at most, models 0.8 GB - the tester's warm-up never runs shorter than before.
constexpr u64 kPrefetchWarmLeadMax = 2 * kPrefetchGiB;
constexpr u64 kPrefetchWarmNoDdsBelow = 3 * kPrefetchGiB;
constexpr u64 kPrefetchWarmNoneBelow = 3 * kPrefetchGiB / 2;

struct PrefetchBudgetState
{
    bool limited{}; // Core / Level
    u64 budget{}; // memory, bytes
    // Core / Level: how many lines of each list (objects, models; in the order of the list) the budget takes, see
    // prefetch_plan; the first line of the merged order that did not fit
    u32 plan[2]{ u32(-1), u32(-1) };
    bool cut{};
    int cut_list{}; // 0 objects, 1 models
    u64 cut_order{};
    u64 cut_bytes{};
    string_path cut_name{}; // not a shared_str: this outlives the string container
    u64 files{}; // the file estimates taken (both lists)
    u32 taken[2]{}; // objects, models
    u32 total[2]{};
    u64 stage_files[2]{};
    PrefetchWarmLimits warm{ kPrefetchWarmLeadMax, true };
};
PrefetchBudgetState s_prefetch_budget;

pcstr prefetch_mode_name(xray::hw::PrefetchMode mode)
{
    switch (mode)
    {
    case xray::hw::PrefetchMode::Full: return "full";
    case xray::hw::PrefetchMode::Core: return "core";
    case xray::hw::PrefetchMode::Level: return "level";
    }
    return "?";
}

u32 prefetch_mb(u64 bytes) { return static_cast<u32>((bytes + kPrefetchMiB / 2) / kPrefetchMiB); }
s32 prefetch_mb_delta(size_t after, size_t before)
{
    return static_cast<s32>((static_cast<s64>(after) - static_cast<s64>(before)) / static_cast<s64>(kPrefetchMiB));
}
// The lines of a list section in the priority order; orders (if not null): the <order> of each of them, u64(-1) for a
// line without one
void prefetch_list_lines(pcstr section, xr_vector<PrefetchListItem>& items, xr_vector<u64>* orders)
{
    items.clear();
    if (orders)
        orders->clear();
    const CInifile::Sect& sect = pSettings->r_section(section);
    xr_vector<std::pair<u64, size_t>> order; // (order, index), the keys come sorted by name
    order.reserve(sect.Data.size());
    xr_vector<PrefetchListItem> lines;
    lines.reserve(sect.Data.size());
    for (const CInifile::Item& item : sect.Data)
    {
        if (!item.first.size())
            continue;
        PrefetchListItem line;
        line.name = item.first;
        line.bytes = kPrefetchDefaultLineBytes;
        u64 place = u64(-1);
        if (pcstr value = item.second.c_str())
        {
            char* end = nullptr;
            const u64 kb = std::strtoull(value, &end, 10);
            if (end != value)
            {
                line.bytes = kb * 1024;
                while (*end == ' ' || *end == '\t' || *end == ',')
                    ++end;
                pcstr rest = end;
                const u64 n = std::strtoull(rest, &end, 10);
                if (end != rest)
                    place = n;
            }
        }
        order.emplace_back(place, lines.size());
        lines.push_back(std::move(line));
    }
    std::stable_sort(order.begin(), order.end(),
        [](const std::pair<u64, size_t>& a, const std::pair<u64, size_t>& b) { return a.first < b.first; });
    items.reserve(lines.size());
    if (orders)
        orders->reserve(lines.size());
    for (const auto& [place, index] : order)
    {
        items.push_back(std::move(lines[index]));
        if (orders)
            orders->push_back(place);
    }
}

// Core / Level (50a-stage1-prefetch.md, "common priority of objects and models"): the generator gives the lines of
// both lists one priority order, so the budget walks the two lists merged by <order> (equal ones - lines without an
// order - objects first) and stops at the first line that does not fit. It does not skip that line for smaller ones
// after it: the size of a line counts only the files no line before it brought, so a line after a skipped one may need
// the files of the skipped one and cost more than its size. A prefix of the merged order is a prefix of each list in
// its own order (PrefetchListRead sorts by the same <order>), so a stage only needs the count of its lines.
// Main thread, before the objects stage (50k-slow-disk-early-prefetch.md: and for the early warm-up at the menu, into
// a state of its own).
void prefetch_plan(PrefetchBudgetState& S, pcstr objects_section, pcstr models_section)
{
    const pcstr sections[2] = { objects_section, models_section };
    xr_vector<PrefetchListItem> lists[2];
    xr_vector<u64> orders[2];
    for (int i = 0; i < 2; ++i)
    {
        if (pSettings->section_exist(sections[i]))
            prefetch_list_lines(sections[i], lists[i], &orders[i]);
    }
    size_t pos[2]{};
    u64 files = 0;
    for (;;)
    {
        int s;
        if (pos[0] < lists[0].size() && (pos[1] >= lists[1].size() || orders[0][pos[0]] <= orders[1][pos[1]]))
            s = 0;
        else if (pos[1] < lists[1].size())
            s = 1;
        else
            break;
        const PrefetchListItem& line = lists[s][pos[s]];
        const double memory = static_cast<double>(files + line.bytes) * prefetch_mem_per_file_byte();
        if (memory > static_cast<double>(S.budget))
        {
            S.cut = true;
            S.cut_list = s;
            S.cut_order = orders[s][pos[s]];
            S.cut_bytes = line.bytes;
            xr_strcpy(S.cut_name, line.name.c_str());
            break;
        }
        files += line.bytes;
        ++pos[s];
    }
    S.plan[0] = static_cast<u32>(pos[0]);
    S.plan[1] = static_cast<u32>(pos[1]);
}

// The RAM available now for the warm-up limits (R4): the live value; under -hw_ram / -hw_profile capped by the
// emulated start value of the profile (emulated = true). real: the live value before the emulation (the "memory by
// stage" line compares it with the live value at the end).
u64 prefetch_avail_phys(u64& real, u64& commit_avail, bool& emulated)
{
    const xray::hw::Profile& profile = xray::hw::GetProfile();
    u64 avail_phys = 0;
    xray::hw::QueryMemoryNow(avail_phys, commit_avail);
    real = avail_phys;
    emulated = false;
    if (profile.overridden & (xray::hw::OVR_RAM | xray::hw::OVR_PROFILE))
    {
        // -hw_ram / -hw_profile emulate less RAM: cap the live value as the profile caps its start value
        if (avail_phys && profile.mem.ram_avail_start)
            avail_phys = std::min(avail_phys, profile.mem.ram_avail_start);
        emulated = true;
    }
    if (!avail_phys)
        avail_phys = profile.mem.ram_avail_start; // no live value on this platform
    return avail_phys;
}

PrefetchWarmLimits prefetch_warm_limits(u64 avail_phys)
{
    PrefetchWarmLimits warm{ kPrefetchWarmLeadMax, true };
    warm.lead_bytes = avail_phys ? std::min(avail_phys / 4, kPrefetchWarmLeadMax) : kPrefetchWarmLeadMax;
    warm.with_textures = !avail_phys || avail_phys >= kPrefetchWarmNoDdsBelow;
    if (avail_phys && avail_phys < kPrefetchWarmNoneBelow)
        warm.lead_bytes = 0;
    return warm;
}

// ---------------------------------------------------------------------------------------------------------------------
// 50k-slow-disk-early-prefetch.md (plans/optimization_mt): the early warm-up of a slow disk. On an HDD / USB / network
// disk the prefetch of the first level load is the warm-up of one reader at ~20 MB/s (minutes: plan 50, section 17),
// and the loads wait for it; the main menu leaves that disk idle meanwhile. So there the file part of the prefetch
// starts at the menu: the render reads the files of the models of the budgeted prefix (IRender::models_EarlyWarmStart,
// one background thread), the first level load stops it and its prefetch warm-up skips what it read. The objects stage
// (game objects: the factory, Lua, sounds - main only) and the textures stage (the D3D textures of the shaders those two
// stages create, unknown before them) stay at the level load. Main thread.
// 50z3-startup-slow-disk-io.md: armed at the start-up, started when the main menu has settled (menu_settle_poll) - not
// at the end of OnAppStart, where it read in the same minutes as the start-up and the building of the menu (the laptop
// on a USB HDD: 649 MB in 231 s, ~2.8 MB/s).
enum class PrefetchEarlyState : u8
{
    None, // not tried yet
    Armed, // a slow disk: waits for the menu to settle
    Running, // started at the menu
    Stopped, // stopped by the first level load: its prefetch skips what it read
    Done, // over, or not to be started in this process
};
PrefetchEarlyState s_prefetch_early = PrefetchEarlyState::None;
CTimer s_prefetch_early_timer; // since the start

pcstr prefetch_early_end_text(PrefetchEarlyWarmEnd end)
{
    switch (end)
    {
    case PrefetchEarlyWarmEnd::Stopped: return "the level load stopped it";
    case PrefetchEarlyWarmEnd::ListEnd: return "the whole list read";
    case PrefetchEarlyWarmEnd::Lead: return "stopped at the warm-up lead";
    case PrefetchEarlyWarmEnd::LowRam: return "stopped: little RAM available";
    }
    return "?";
}

// IGame_Persistent::OnAppStart: the device and the hw policy are there, pSettings holds the lists. Only where the hw
// profile says the disk of gamedata is slow (HDD, USB, network; -hw_disk hdd too). Not on an Unknown disk: the warm-up
// measures it at the level load (normal priority, the real conditions); a measurement here would run next to the reads
// of the menu, and on a fast disk an early read gains nothing but costs the OS cache.
// 50z3-startup-slow-disk-io.md: only armed here; prefetch_early_start runs when the main menu has settled.
void prefetch_early_arm()
{
    if (s_prefetch_early != PrefetchEarlyState::None)
        return;
    s_prefetch_early = PrefetchEarlyState::Done; // one try per process
    // no prefetch, or a level load right away from the command line: no menu time to use
    if (strstr(Core.Params, "-noprefetch") || strstr(Core.Params, "-start ") || strstr(Core.Params, "-load "))
        return;
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    const xray::hw::Profile& profile = xray::hw::GetProfile();
    if (!GEnv.Render || !xray::hw::PolicyValid(policy) || profile.tiers.disk_class != xray::hw::DiskClass::Slow)
        return;
    s_prefetch_early = PrefetchEarlyState::Armed;
    xray::io::BackgroundIoSetMenuWarmPending(true);
}

// menu_settle_poll (main): the main menu is shown and its own reads stopped - the warm-up of the first level load starts
void prefetch_early_start(pcstr game_type)
{
    if (s_prefetch_early != PrefetchEarlyState::Armed)
        return;
    s_prefetch_early = PrefetchEarlyState::Done; // one try per process
    xray::io::BackgroundIoSetMenuWarmPending(false); // set again below once the thread runs
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    const xray::hw::Profile& profile = xray::hw::GetProfile();
    if (!GEnv.Render || !xray::hw::PolicyValid(policy))
        return;
    // The game type comes with the level load (Start). GlobalWar is single player: the game ships the lists of "single"
    // only (configs/prefetch/prefetch_single.ltx). Another type at the load: the prefetch finds nothing of this read in
    // its lists and reads them itself, as without the early warm-up.
    if (!game_type || !game_type[0])
        game_type = "single";
    string256 objects_section, models_section;
    strconcat(sizeof(objects_section), objects_section, "prefetch_objects_", game_type);
    strconcat(sizeof(models_section), models_section, "prefetch_visuals_", game_type);
    const bool have_objects = pSettings->section_exist(objects_section);
    const bool have_models = pSettings->section_exist(models_section);
    if (!have_objects && !have_models)
        return;

    // R4 now, as Prefetch takes it at the level load
    u64 avail_real = 0, commit_avail = 0;
    bool emulated = false;
    const u64 avail_phys = prefetch_avail_phys(avail_real, commit_avail, emulated);
    const PrefetchWarmLimits limits = prefetch_warm_limits(avail_phys);
    if (!limits.lead_bytes)
    {
        Msg("* [prefetch] early warm-up: not started, little RAM available (%u MB%s)", prefetch_mb(avail_phys),
            emulated ? "*" : "");
        return;
    }

    // R1: the prefix the budget takes, as Prefetch computes it (the policy and texture_lod of now)
    PrefetchBudgetState plan;
    plan.limited = policy.prefetch_mode != xray::hw::PrefetchMode::Full;
    plan.budget = policy.prefetch_budget;
    if (plan.limited)
        prefetch_plan(plan, objects_section, models_section);
    xr_vector<PrefetchListItem> lines[2];
    if (have_objects)
        prefetch_list_lines(objects_section, lines[0], nullptr);
    if (have_models)
        prefetch_list_lines(models_section, lines[1], nullptr);
    for (int s = 0; s < 2; ++s)
    {
        if (plan.limited && lines[s].size() > plan.plan[s])
            lines[s].resize(plan.plan[s]);
    }
    // In the order the stages load them: the visuals of the objects (the objects stage; the ones the object Load creates
    // itself too), then the models
    xr_vector<PrefetchModel> models;
    PrefetchObjectVisuals(lines[0], models, true);
    models.reserve(models.size() + lines[1].size());
    for (const PrefetchListItem& line : lines[1])
        models.push_back({ line.name, shared_str(), line.bytes });
    u64 estimate = 0;
    for (const PrefetchModel& model : models)
        estimate += model.bytes;

    PrefetchEarlyWarmParams params;
    params.lead_bytes = limits.lead_bytes;
    params.with_textures = limits.with_textures;
    // the RAM the lead and the dds were taken for: below it the OS cache would drop the files before the load
    params.ram_floor = limits.with_textures ? kPrefetchWarmNoDdsBelow : kPrefetchWarmNoneBelow;
    params.ram_cap = emulated ? profile.mem.ram_avail_start : 0;
    xray::io::BackgroundIoSetMenuWarmPending(true); // the thread clears it when it ends
    if (!GEnv.Render->models_EarlyWarmStart(models, params))
    {
        xray::io::BackgroundIoSetMenuWarmPending(false);
        return;
    }
    s_prefetch_early = PrefetchEarlyState::Running;
    s_prefetch_early_timer.Start();
    Msg("* [prefetch] early warm-up: started at the menu (slow disk), %u objects + %u models of the %s list -> %u "
        "models, %u MB estimated; up to the warm-up lead %u MB, dds %s (ram avail %u MB%s)",
        static_cast<u32>(lines[0].size()), static_cast<u32>(lines[1].size()), prefetch_mode_name(policy.prefetch_mode),
        static_cast<u32>(models.size()), prefetch_mb(estimate), prefetch_mb(limits.lead_bytes),
        limits.with_textures ? "on" : "off", prefetch_mb(avail_phys), emulated ? "*" : "");
}

// KERNEL:start (and the demo start), before PreStart: the level load begins - the hand-off to its prefetch
void prefetch_early_stop()
{
    if (s_prefetch_early == PrefetchEarlyState::Armed) // 50z3: the menu did not settle before the load
    {
        s_prefetch_early = PrefetchEarlyState::Done;
        xray::io::BackgroundIoSetMenuWarmPending(false);
        Msg("* [prefetch] early warm-up: not started, the level load came before the main menu settled");
        return;
    }
    if (s_prefetch_early != PrefetchEarlyState::Running)
        return;
    s_prefetch_early = PrefetchEarlyState::Stopped;
    const u32 menu_ms = s_prefetch_early_timer.GetElapsed_ms();
    PrefetchEarlyWarmStats st;
    if (!GEnv.Render || !GEnv.Render->models_EarlyWarmStop(st, false))
    {
        s_prefetch_early = PrefetchEarlyState::Done;
        Msg("* [prefetch] early warm-up: gone before the level load (the render was recreated)");
        return;
    }
    Msg("* [prefetch] early warm-up: started at the menu, %u files / %u MB read before the level load (%.1f s of "
        "reading, %.1f s at the menu, %u missing), %u of %u models, %u of %u MB estimated; %s",
        st.files, prefetch_mb(st.bytes), st.read_ms / 1000.0, menu_ms / 1000.0, st.missing, st.models_read, st.models,
        prefetch_mb(st.estimate_read), prefetch_mb(st.estimate), prefetch_early_end_text(st.end));
}

// The end of the prefetch of the first level load (prefetch_ms: its time) or of that load without a prefetch (no
// line): the prefetch no longer skips the models read at the menu
void prefetch_early_release(bool report, u32 prefetch_ms)
{
    if (s_prefetch_early != PrefetchEarlyState::Stopped)
        return;
    s_prefetch_early = PrefetchEarlyState::Done;
    PrefetchEarlyWarmStats st;
    if (!GEnv.Render || !GEnv.Render->models_EarlyWarmStop(st, true) || !report)
        return;
    Msg("* [prefetch] early warm-up: the prefetch warm-up found %u models read at the menu, the loads still waited for "
        "the warm-up %u ms; prefetch %u ms",
        st.skipped, st.waited_ms, prefetch_ms);
}

// OnAppEnd: the exit from the menu - the thread stops after the chunk being read
void prefetch_early_quit()
{
    if (s_prefetch_early == PrefetchEarlyState::Running && GEnv.Render)
    {
        PrefetchEarlyWarmStats st;
        if (GEnv.Render->models_EarlyWarmStop(st, true))
        {
            Msg("* [prefetch] early warm-up: stopped at the exit, %u files / %u MB read (%.1f s), %u of %u models",
                st.files, prefetch_mb(st.bytes), st.read_ms / 1000.0, st.models_read, st.models);
        }
    }
    s_prefetch_early = PrefetchEarlyState::Done;
    xray::io::BackgroundIoSetMenuWarmPending(false);
}

// ---------------------------------------------------------------------------------------------------------------------
// 50z3-startup-slow-disk-io.md (plans/optimization_mt): the moment the main menu has settled - shown for
// kMenuSettleFrames frames and no file opened in the foreground (main, the task workers; FS notes them in xray::io) for
// kMenuSettleQuietMs, or shown for kMenuSettleMaxMs at the latest (the speculative readers give way to the foreground
// reads anyway, read by read). Before it the start-up and the menu read alone; at it the deferred work of the start-up
// begins: the early warm-up of the first level load (here) and the smart terrain map of the menu (xrGame, through
// xray::io::BackgroundIoMenuSettled). Once per process, in the menu phase only (before the first level load). Main.
constexpr u32 kMenuSettleFrames = 30;
constexpr u32 kMenuSettleQuietMs = 2000;
constexpr u32 kMenuSettleMaxMs = 30000;

struct MenuSettleState
{
    bool settled{};
    bool counting{};
    u32 frames{};
    CTimer shown; // since the first frame of the menu
};
MenuSettleState s_menu_settle;

void menu_settle_poll(bool menu_active, pcstr game_type)
{
    MenuSettleState& S = s_menu_settle;
    if (S.settled || !xray::io::BackgroundIoMenuPhase())
        return;
    if (!menu_active || g_pGameLevel)
    {
        S.counting = false;
        return;
    }
    if (!S.counting)
    {
        S.counting = true;
        S.frames = 0;
        S.shown.Start();
    }
    if (++S.frames < kMenuSettleFrames)
        return;
    const u32 idle_ms = xray::io::BackgroundIoForegroundIdleMs();
    const u32 shown_ms = S.shown.GetElapsed_ms();
    if (idle_ms < kMenuSettleQuietMs && shown_ms < kMenuSettleMaxMs)
        return;
    S.settled = true;
    xray::io::BackgroundIoNoteMenuSettled();
    xray::StartupMarkOnce("menu_settled");
    if (xray::io::BackgroundIoSingle())
    {
        Msg("* [startup] slow disk: the main menu settled %.1f s after it was shown, %u frames (%s) - the deferred reads "
            "of the start-up begin",
            shown_ms / 1000.0, S.frames, idle_ms >= kMenuSettleQuietMs ? "its reads stopped" : "the time limit");
    }
    prefetch_early_start(game_type);
}

// KERNEL:start / the demo start: the menu phase is over. The sounds the start-up created deferred on a slow disk (the
// material library, the weather ambients: 50z3) and the menu did not read in the background are read here, at the
// start of the load, as the start-up read them before - the load and the game find them all ready.
void menu_phase_end()
{
    if (!xray::io::BackgroundIoMenuPhase())
        return;
    xray::io::BackgroundIoEndMenuPhase();
    if (GEnv.Sound)
        GEnv.Sound->read_pending_sources();
}
} // namespace

void PrefetchListRead(pcstr section, xr_vector<PrefetchListItem>& items)
{
    prefetch_list_lines(section, items, nullptr);
}

void PrefetchListTake(pcstr stage, xr_vector<PrefetchListItem>& items)
{
    PrefetchBudgetState& S = s_prefetch_budget;
    const int s = 0 == xr_strcmp(stage, "objects") ? 0 : 1;
    S.total[s] += static_cast<u32>(items.size());
    if (S.limited && items.size() > S.plan[s])
        items.resize(S.plan[s]);
    for (const PrefetchListItem& item : items)
    {
        S.files += item.bytes;
        S.stage_files[s] += item.bytes;
    }
    S.taken[s] += static_cast<u32>(items.size());
}

PrefetchWarmLimits PrefetchWarmLimitsNow() { return s_prefetch_budget.warm; }
#endif

IGame_Persistent::IGame_Persistent()
{
    ZoneScoped;

    eStart = Engine.Event.Handler_Attach("KERNEL:start", this);
    eStartLoad = Engine.Event.Handler_Attach("KERNEL:load", this);
    eDisconnect = Engine.Event.Handler_Attach("KERNEL:disconnect", this);
    eStartMPDemo = Engine.Event.Handler_Attach("KERNEL:start_mp_demo", this);

    Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 1);
    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);

    pEnvironment = xr_new<CEnvironment>();

    m_pGShaderConstants = xr_new<ShadersExternalData>(); //--#SM+#--

    m_pSound = GEnv.Sound->create_scene();
    DefaultSoundScene = m_pSound;
}

IGame_Persistent::~IGame_Persistent()
{
    ZoneScoped;

    GEnv.Sound->destroy_scene(m_pSound);
    DefaultSoundScene = nullptr;

    Device.seqFrame.Remove(this);
    Device.seqAppActivate.Remove(this);
    Device.seqAppDeactivate.Remove(this);

    Engine.Event.Handler_Detach(eDisconnect, this);
    Engine.Event.Handler_Detach(eStartLoad, this);
    Engine.Event.Handler_Detach(eStart, this);
    Engine.Event.Handler_Detach(eStartMPDemo, this);

#ifndef _EDITOR
    xr_delete(pEnvironment);
#endif
    xr_delete(m_pGShaderConstants); //--#SM+#--
}

void IGame_Persistent::OnAppActivate() {}
void IGame_Persistent::OnAppDeactivate() {}

void IGame_Persistent::OnAppStart()
{
    ZoneScoped;

#ifndef _EDITOR
    Environment().load();
#endif
    xray::StartupMark("app_environment"); // 50z1: weathers, weather effects, their sounds

    Level_Scan();
    xray::StartupMark("app_level_scan"); // 50z1

#ifndef _EDITOR
    // 50k-slow-disk-early-prefetch.md: a slow disk - the file part of the prefetch starts at the menu (50z3: once the
    // menu has settled, menu_settle_poll)
    prefetch_early_arm();
#endif
}

void IGame_Persistent::OnAppEnd()
{
    ZoneScoped;

#ifndef _EDITOR
    prefetch_early_quit(); // 50k-slow-disk-early-prefetch.md
    Environment().unload();
#endif
    OnGameEnd();

    for (auto& level : Levels)
    {
        xr_free(level.folder);
        xr_free(level.name);
    }
    Levels.clear();
}

void IGame_Persistent::Level_Append(pcstr folder)
{
    ZoneScoped;
    string_path N1, N2, N3, N4;

    strconcat(N1, folder, "level");
    strconcat(N2, folder, "level.ltx");
    strconcat(N3, folder, "level.geom");
    strconcat(N4, folder, "level.cform");

    if (FS.exist("$game_levels$", N1) &&
        FS.exist("$game_levels$", N2) &&
        FS.exist("$game_levels$", N3) &&
        FS.exist("$game_levels$", N4))
    {
        Levels.emplace_back(sLevelInfo{ xr_strdup(folder), nullptr });
    }
}

void IGame_Persistent::Level_Scan()
{
    ZoneScoped;

    for (auto& level : Levels)
    {
        xr_free(level.folder);
        xr_free(level.name);
    }
    Levels.clear();

    xr_vector<char*>* folder = FS.file_list_open("$game_levels$", FS_ListFolders | FS_RootOnly);
    if (!folder)
    {
        Log("! No levels found in game data");
        return;
    }

    for (cpcstr i : *folder)
        Level_Append(i);

    FS.file_list_close(folder);
}

void gen_logo_name(string_path& dest, pcstr level_name, int num = -1)
{
    strconcat(sizeof(dest), dest, "intro" DELIMITER "intro_", level_name);

    const auto len = xr_strlen(dest);
    if (dest[len - 1] == _DELIMITER)
        dest[len - 1] = 0;

    if (num < 0)
        return;

    string16 buff;
    xr_strcat(dest, sizeof(dest), "_");
    xr_strcat(dest, sizeof(dest), xr_itoa(num + 1, buff, 10));
}

// Return true if logo exists
// Always sets the path even if logo doesn't exist
bool set_logo_path(string_path& path, pcstr levelName, int count = -1)
{
    gen_logo_name(path, levelName, count);
    string_path temp2;
    return FS.exist(temp2, "$game_textures$", path, ".dds") || FS.exist(temp2, "$level$", path, ".dds");
}

void IGame_Persistent::Level_Set(u32 id)
{
    ZoneScoped;

    if (id >= Levels.size())
        return;
    FS.get_path("$level$")->_set(Levels[id].folder);
    Level_Current = id;

    static string_path path;
    path[0] = 0;

    int count = 0;
    while (true)
    {
        if (set_logo_path(path, Levels[id].folder, count))
            count++;
        else
            break;
    }

    if (count)
    {
        const int num = ::Random.randI(count);
        gen_logo_name(path, Levels[id].folder, num);
    }
    else if (!set_logo_path(path, Levels[id].folder))
    {
        if (!set_logo_path(path, "no_start_picture"))
            path[0] = 0;
    }

    if (path[0])
        m_pLoadingScreen->SetLevelLogo(path);
}

int IGame_Persistent::Level_ID(pcstr name, pcstr ver, bool bSet)
{
    ZoneScoped;

    int result = -1;
    bool arch_res = false;

    for (CLocatorAPI::archive& A : FS.m_archives)
    {
        if (!A.hSrcFile)
        {
            cpcstr ln = A.header->r_string("header", "level_name");
            cpcstr lv = A.header->r_string("header", "level_ver");
            if (0 == xr_stricmp(ln, name) && 0 == xr_stricmp(lv, ver))
            {
                FS.LoadArchive(A);
                arch_res = true;
            }
        }
    }

    if (arch_res)
        Level_Scan();

    string256 buffer;
    strconcat(sizeof(buffer), buffer, name, DELIMITER);
    for (u32 I = 0; I < Levels.size(); ++I)
    {
        if (0 == xr_stricmp(buffer, Levels[I].folder))
        {
            result = int(I);
            break;
        }
    }

    if (bSet && result != -1)
        Level_Set(result);

    if (arch_res)
        g_pGamePersistent->OnAssetsChanged();
    return result;
}

CInifile* IGame_Persistent::GetArchiveHeader(pcstr name, pcstr ver)
{
    for (const CLocatorAPI::archive& A : FS.m_archives)
    {
        if (!A.header)
            continue;

        cpcstr ln = A.header->r_string("header", "level_name");
        cpcstr lv = A.header->r_string("header", "level_ver");
        if (0 == xr_stricmp(ln, name) && 0 == xr_stricmp(lv, ver))
        {
            return A.header;
        }
    }
    return nullptr;
}

void IGame_Persistent::OnEvent(EVENT E, u64 P1, u64 P2)
{
    ZoneScoped;

    if (E == eStart)
    {
#ifndef _EDITOR
        // 50k-slow-disk-early-prefetch.md: the early warm-up of the menu hands over to the prefetch of this load (its
        // PreStart empties the file set of the session, Prefetch skips what was read)
        prefetch_early_stop();
        menu_phase_end(); // 50z3-startup-slow-disk-io.md
#endif
        pstr op_server = pstr(P1);
        pstr op_client = pstr(P2);
        Level_Current = u32(-1);
        R_ASSERT(nullptr == g_pGameLevel);
        Console->Execute("main_menu off");
        Console->Hide();
        //-----------------------------------------------------------
        PreStart(op_server);
        //-----------------------------------------------------------
        g_pGameLevel = CreateLevel();
        R_ASSERT(g_pGameLevel);
        LoadBegin();
        Start(op_server);
        g_pGameLevel->net_Start(op_server, op_client);
        LoadEnd();
#ifndef _EDITOR
        prefetch_early_release(false, 0); // 50k: a load without a prefetch (Prefetch released it otherwise)
#endif
        xr_free(op_server);
        xr_free(op_client);
    }
    else if (E == eDisconnect)
    {
        if (pInput != nullptr && true == Engine.Event.Peek("KERNEL:quit"))
            pInput->GrabInput(false);

        if (g_pGameLevel)
        {
            const bool show = Console->bVisible;
            Console->Hide();
            g_pGameLevel->net_Stop();
            DestroyLevel(g_pGameLevel);
            if (show)
                Console->Show();

            if ((false == Engine.Event.Peek("KERNEL:quit")) && (false == Engine.Event.Peek("KERNEL:start")))
            {
                Console->Execute("main_menu off");
                Console->Execute("main_menu on");
            }
        }
        Disconnect();
    }
    else if (E == eStartMPDemo)
    {
        pstr demo_file = pstr(P1);

#ifndef _EDITOR
        prefetch_early_stop(); // 50k-slow-disk-early-prefetch.md: no prefetch for a demo
        prefetch_early_release(false, 0);
        menu_phase_end(); // 50z3-startup-slow-disk-io.md
#endif
        R_ASSERT(nullptr == g_pGameLevel);

        Console->Execute("main_menu off");
        Console->Hide();
        Device.Reset(false);

        g_pGameLevel = CreateLevel();
        VERIFY(g_pGameLevel);
        const shared_str server_options = g_pGameLevel->OpenDemoFile(demo_file);

        //-----------------------------------------------------------
        PreStart(server_options.c_str());
        //-----------------------------------------------------------

        LoadBegin();
        Start(""); // server_options.c_str()); - no prefetch !
        g_pGameLevel->net_StartPlayDemo();
        LoadEnd();

        xr_free(demo_file);
    }
}

void IGame_Persistent::PreStart(pcstr op)
{
    ZoneScoped;

    string256 prev_type;
    params new_game_params;
    xr_strcpy(prev_type, m_game_params.m_game_type);
    new_game_params.parse_cmd_line(op);

    // change game type
    if (0 != xr_strcmp(prev_type, new_game_params.m_game_type))
    {
        OnGameEnd();
    }
}
void IGame_Persistent::Start(pcstr op)
{
    ZoneScoped;

    string256 prev_type;
    xr_strcpy(prev_type, m_game_params.m_game_type);
    m_game_params.parse_cmd_line(op);
    // change game type
    if ((0 != xr_strcmp(prev_type, m_game_params.m_game_type)))
    {
        if (*m_game_params.m_game_type)
            OnGameStart();
    }
    else
        UpdateGameType();

    VERIFY(ps_destroy.empty());
}

void IGame_Persistent::Disconnect()
{
    ZoneScoped;
#ifndef _EDITOR
    // clear "need to play" particles
    destroy_particles(true);
#endif
}

void IGame_Persistent::OnGameStart()
{
    ZoneScoped;
#ifndef _EDITOR
    LoadTitle("st_prefetching_objects");
    if (!strstr(Core.Params, "-noprefetch"))
        Prefetch();
#endif
}

#ifndef _EDITOR
void IGame_Persistent::Prefetch()
{
    ZoneScoped;

    // prefetch game objects & models
    CTimer timer;
    timer.Start();
    const auto memoryBefore = Memory.mem_usage();

    // 50a-stage1-prefetch.md (plans/optimization_mt): the mode and the byte budget of the hw policy (plan 50 R1), the
    // warm-up limits from the RAM available now (R4)
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    u64 avail_phys_real = 0, commit_avail = 0;
    bool emulated = false;
    const u64 avail_phys = prefetch_avail_phys(avail_phys_real, commit_avail, emulated);
    s_prefetch_budget = {};
    s_prefetch_budget.limited = policy.prefetch_mode != xray::hw::PrefetchMode::Full;
    s_prefetch_budget.budget = policy.prefetch_budget;
    s_prefetch_budget.warm = prefetch_warm_limits(avail_phys);
    const PrefetchWarmLimits& warm = s_prefetch_budget.warm;
    string128 budget_text = "";
    if (s_prefetch_budget.limited)
    {
        xr_sprintf(budget_text, ", budget %u MB", prefetch_mb(policy.prefetch_budget));
        // one priority over both lists: what each stage takes is known before the objects stage (the same sections
        // IGame_ObjectPool::prefetch and CModelPool::Prefetch read)
        string256 objects_section, models_section;
        strconcat(sizeof(objects_section), objects_section, "prefetch_objects_", m_game_params.m_game_type);
        strconcat(sizeof(models_section), models_section, "prefetch_visuals_", m_game_params.m_game_type);
        prefetch_plan(s_prefetch_budget, objects_section, models_section);
    }
    Msg("* [prefetch] mode %s%s, warm-up lead %u MB, dds %s (ram avail %u MB%s)",
        prefetch_mode_name(policy.prefetch_mode), budget_text, prefetch_mb(warm.lead_bytes),
        warm.with_textures ? "on" : "off", prefetch_mb(avail_phys), emulated ? "*" : "");

    // 36-spawn-model-load.md (plans/optimization_mt): the time of each stage (the model stage logs its own details);
    // 50a-stage1-prefetch.md: and the process commit each one adds
    CTimer stage;
    stage.Start();
    Log("Loading objects...");
    ObjectPool.prefetch();
    const u32 objects_ms = stage.GetElapsed_ms();
    const auto memoryObjects = Memory.mem_usage();

    stage.Start();
    Log("Loading models...");
    GEnv.Render->models_Prefetch();
    const u32 models_ms = stage.GetElapsed_ms();
    const auto memoryModels = Memory.mem_usage();

    stage.Start();
    Log("Loading textures...");
    GEnv.Render->ResourcesDeferredUpload();
    const u32 textures_ms = stage.GetElapsed_ms();

    const auto memoryEnd = Memory.mem_usage();
    const auto memoryAfter = memoryEnd - memoryBefore;

    Msg("* [prefetch] time:   %d ms", timer.GetElapsed_ms());
    Msg("* [prefetch] stages: objects %u ms, models %u ms, textures %u ms", objects_ms, models_ms, textures_ms);
    Msg("* [prefetch] memory: %d Kb", memoryAfter / 1024);

    // 50a-stage1-prefetch.md: the memory of each stage and the estimate of the budget, to calibrate
    // prefetch_mem_per_file_byte() and the budget of plan 50 R1
    u64 avail_end = 0, commit_end = 0;
    xray::hw::QueryMemoryNow(avail_end, commit_end);
    // Both ram avail values are real (50d-stage2-auto-engine.md): under -hw_ram / -hw_profile the start value the warm-up
    // limits used is emulated and was printed as "before -> after" with a real "after"; it goes separately now, with '*'
    string64 emulated_text = "";
    if (emulated)
        xr_sprintf(emulated_text, " (emulated %u MB*)", prefetch_mb(avail_phys));
    Msg("* [prefetch] memory by stage: objects %+d MB, models %+d MB, textures %+d MB; process commit %u MB; "
        "ram avail %u -> %u MB%s, commit avail %u -> %u MB",
        prefetch_mb_delta(memoryObjects, memoryBefore), prefetch_mb_delta(memoryModels, memoryObjects),
        prefetch_mb_delta(memoryEnd, memoryModels), prefetch_mb(memoryEnd), prefetch_mb(avail_phys_real),
        prefetch_mb(avail_end), emulated_text, prefetch_mb(commit_avail), prefetch_mb(commit_end));
    const PrefetchBudgetState& B = s_prefetch_budget;
    const s32 real_mb = prefetch_mb_delta(memoryEnd, memoryBefore);
    // where the walk over the merged order stopped: the order and the line that did not fit
    string512 cut_text = "";
    if (B.limited && B.cut)
    {
        string32 order_text = "none";
        if (B.cut_order != u64(-1))
            xr_sprintf(order_text, "%llu", static_cast<unsigned long long>(B.cut_order));
        xr_sprintf(cut_text, "; cut at order %s: %s line %s (%u KB) did not fit", order_text,
            B.cut_list ? "models" : "objects", B.cut_name, static_cast<u32>(B.cut_bytes / 1024));
    }
    else if (B.limited)
        xr_strcpy(cut_text, "; nothing cut");
    Msg("* [prefetch] budget: objects %u of %u taken (%u skipped), models %u of %u taken (%u skipped)%s; "
        "files %u + %u MB estimated, x%.2f = %u MB of memory%s; real %+d MB (%.2f per file byte)",
        B.taken[0], B.total[0], B.total[0] - B.taken[0], B.taken[1], B.total[1], B.total[1] - B.taken[1], cut_text,
        prefetch_mb(B.stage_files[0]), prefetch_mb(B.stage_files[1]), prefetch_mem_per_file_byte(),
        prefetch_mb(static_cast<u64>(static_cast<double>(B.files) * prefetch_mem_per_file_byte())), budget_text, real_mb,
        B.files ? static_cast<double>(real_mb) * kPrefetchMiB / static_cast<double>(B.files) : 0.0);
    s_prefetch_budget.limited = false; // outside the prefetch the lists are taken whole
    // 50k-slow-disk-early-prefetch.md: what the files read at the menu saved this prefetch
    prefetch_early_release(true, timer.GetElapsed_ms());
}
#endif

void IGame_Persistent::OnGameEnd()
{
    ZoneScoped;

#ifndef _EDITOR
    ObjectPool.clear();
    GEnv.Render->models_Clear(true);
#endif
}

void IGame_Persistent::LoadBegin()
{
    ll_dwReference++;
    if (1 == ll_dwReference)
    {
        loaded = false;
        phase_timer.Start();
        load_stage = 0;
        // 50n-frame-spike-report.md: no frame spike report until the level is in play and warmed up
        xray::perf::FrameSessionMark(xray::perf::frame_session::Exit);
    }
}

void IGame_Persistent::LoadEnd()
{
    ll_dwReference--;
    if (0 == ll_dwReference)
    {
        Msg("* phase time: %d ms", phase_timer.GetElapsed_ms());
        Msg("* phase cmem: %d K", Memory.mem_usage() / 1024);
        Console->Execute("stat_memory");
        loaded = true;
        xray::perf::FrameSessionMark(xray::perf::frame_session::LoadEnd); // 50n: the warmup of the frame spike report
    }
}

void IGame_Persistent::LoadTitle(pcstr ls_title, bool change_tip, shared_str map_name)
{
    ZoneScoped;

    if (ls_title)
    {
        string256 buff;
        xr_sprintf(buff, "%s%s", StringTable().translate(ls_title).c_str(), "...");
        m_pLoadingScreen->SetStageTitle(buff);
    }
    else if (!change_tip)
        m_pLoadingScreen->SetStageTitle("");

    if (change_tip)
    {
        ZoneScopedN("Change tip");

        bool noTips = false;
        string512 buff;
        u8 tip_num;
        luabind::functor<u8> m_functor;
        const bool is_single = !xr_strcmp(m_game_params.m_game_type, "single");
        if (is_single)
        {
            if (GEnv.ScriptEngine->functor("loadscreen.get_tip_number", m_functor))
                tip_num = m_functor(map_name.c_str());
            else
                noTips = true;
        }
        else
        {
            if (GEnv.ScriptEngine->functor("loadscreen.get_mp_tip_number", m_functor))
                tip_num = m_functor(map_name.c_str());
            else
                noTips = true;
        }
        if (noTips)
            return;

        xr_sprintf(buff, "%s%d:", StringTable().translate("ls_tip_number").c_str(), tip_num);
        const shared_str tmp = buff;

        if (is_single)
            xr_sprintf(buff, "ls_tip_%d", tip_num);
        else
            xr_sprintf(buff, "ls_mp_tip_%d", tip_num);

        m_pLoadingScreen->SetStageTip(StringTable().translate("ls_header").c_str(),
            tmp.c_str(),
            StringTable().translate(buff).c_str());
    }

    LoadStage();
}

void IGame_Persistent::LoadStage(bool draw /*= true*/)
{
    VERIFY(ll_dwReference);
    if (!load_screen_renderer.IsActive())
    {
        Msg("* phase time: %d ms", phase_timer.GetElapsed_ms());
        Msg("* phase cmem: %d K", Memory.mem_usage() / 1024);
        phase_timer.Start();
    }

    if (GameType() == 1 && !xr_strcmp(m_game_params.m_alife, "alife"))
        max_load_stage = 18;
    else
        max_load_stage = 14;

    m_pLoadingScreen->Show(true);
    m_pLoadingScreen->Update(load_stage, max_load_stage);

    if (draw)
        LoadDraw();
    ++load_stage;
}

void IGame_Persistent::LoadDraw() const
{
    if (loaded)
        return;

    Device.dwFrame += 1;

    if (!Device.RenderBegin())
        return;

    // XXX: fix dedicated server
    //if (GEnv.isDedicatedServer)
    //    Console->OnRender();
    //else
        load_draw_internal();

    Device.RenderEnd();
}

void IGame_Persistent::load_draw_internal() const
{
    m_pLoadingScreen->Draw();
}

void IGame_Persistent::ShowLoadingScreen(bool show) const
{
    m_pLoadingScreen->Show(show);
}

void IGame_Persistent::OnFrameBeforePreRender()
{
    ZoneScopedN("IGame_Persistent::OnFrameBeforePreRender");

#ifndef _EDITOR
    // 50z3-startup-slow-disk-io.md: every frame, in the menu too (CGamePersistent::OnFrame leaves early without a level)
    menu_settle_poll(IsMainMenuActive(), m_game_params.m_game_type);
#endif

    SpatialSpace.update();
    SpatialSpacePhysic.update();

#ifndef _EDITOR
    stats.Starting = ps_needtoplay.size();
    stats.Active = ps_active.size();
    stats.Destroying = ps_destroy.size();
    while (ps_needtoplay.size())
    {
        CPS_Instance* psi = ps_needtoplay.back();
        ps_needtoplay.pop_back();
        psi->Play(false);
    }
    while (ps_destroy.size())
    {
        CPS_Instance* psi = ps_destroy.back();
        VERIFY(psi);
        if (psi->Locked())
        {
            Log("--locked");
            break;
        }
        ps_destroy.pop_back();
        psi->PSI_internal_delete();
    }
#endif

    ParticleWorker_BeginFrameCollect();
}

void IGame_Persistent::OnFrameEnvironment()
{
    ZoneScopedN("IGame_Persistent::OnFrameEnvironment");

#ifndef _EDITOR
    if (!Device.Paused() || Device.dwPrecacheFrame)
        Environment().OnFrame();
#endif
}

void IGame_Persistent::OnFrame()
{
    ZoneScoped;
    // `Environment().OnFrame()` runs from `CRenderDevice::ProcessFrame` via `OnFrameEnvironment` before PreRender.
}

void IGame_Persistent::destroy_particles(const bool& all_particles)
{
    ZoneScoped;

#ifndef _EDITOR
    // Called from net_Stop / Disconnect, which can run in FrameMove while the PreRender worker is
    // still walking the particle batch. Drain the batch and drop the pending flags first, so the
    // instances deleted below are not referenced any more (this is what Locked() guards in
    // OnFrameBeforePreRender, and what the VERIFY in ~CParticlesObject checks in debug builds).
    ParticleWorker_ShutdownBeforeNullCallback();

    ps_needtoplay.clear();

    while (ps_destroy.size())
    {
        CPS_Instance* psi = ps_destroy.back();
        VERIFY(psi);
        VERIFY(!psi->Locked());
        ps_destroy.pop_back();
        psi->PSI_internal_delete();
    }

    // delete active particles
    if (all_particles)
    {
        for (; !ps_active.empty();)
            (*ps_active.begin())->PSI_internal_delete();
    }
    else
    {
        u32 active_size = ps_active.size();
        CPS_Instance** I = (CPS_Instance**)xr_alloca(active_size * sizeof(CPS_Instance*));
        std::copy(ps_active.begin(), ps_active.end(), I);

        CPS_Instance** E = std::remove_if(I, I + active_size, [](CPS_Instance* const& object)
        {
            return (!object->destroy_on_game_load());
        });
        for (; I != E; ++I)
            (*I)->PSI_internal_delete();
    }

    VERIFY(ps_needtoplay.empty() && ps_destroy.empty() && (!all_particles || ps_active.empty()));
#endif
}

//ECO_RENDER add
bool IGame_Persistent::IsMainMenuActive() const
{
    return m_pMainMenu && m_pMainMenu->IsActive();
}

bool IGame_Persistent::MainMenuActiveOrLevelNotExist() const
{
    return !g_pGameLevel || IsMainMenuActive();
}

void IGame_Persistent::OnAssetsChanged()
{
#ifndef _EDITOR
    GEnv.Render->OnAssetsChanged();
#endif
}

void IGame_Persistent::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    // XXX: move to particle engine
    stats.FrameEnd();
    font.OutNext("Particles:");
    font.OutNext("- starting:   %u", stats.Starting);
    font.OutNext("- active:     %u", stats.Active);
    font.OutNext("- destroying: %u", stats.Destroying);
    stats.FrameStart();
}
