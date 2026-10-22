#include "pch_script.h"
#include <tracy/Tracy.hpp>
#include "xrServer_Objects_ALife_All.h"
#include "Level.h"
#include "game_cl_base.h"
#include "NET_Queue.h"
#include "ai_space.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrAICore/Navigation/level_graph.h"
#include "client_spawn_manager.h"
#include "xrEngine/xr_object.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrNetServer/NET_Messages.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <typeinfo>

// Spawn stage 0 (plans/spawn_optimization/SPEC.md): what one client spawn costs, by step (PERF_STEP: Tracy zone +
// perf_metrics timer spawn/* + perf_spike site), by category of the server entity and by class of the created object
// ("* [spawn]" report of perf_metrics, log bit). Main thread only (ProcessGameEvents), so the report needs no locks.
namespace
{
enum class ClientSpawnCategory : u8
{
    Stalker,
    Monster,
    Weapon,
    Ammo,
    Item,
    Other,
    Count
};

constexpr pcstr client_spawn_category_names[] = {"stalker", "monster", "weapon", "ammo", "item", "other"};
static_assert(std::size(client_spawn_category_names) == size_t(ClientSpawnCategory::Count));

ClientSpawnCategory client_spawn_category(CSE_Abstract* E)
{
    if (E->cast_human_abstract())
        return ClientSpawnCategory::Stalker;
    if (E->cast_monster_abstract())
        return ClientSpawnCategory::Monster;
    if (E->cast_item_ammo())
        return ClientSpawnCategory::Ammo;
    if (E->cast_item_weapon())
        return ClientSpawnCategory::Weapon;
    if (E->cast_inventory_item())
        return ClientSpawnCategory::Item;
    return ClientSpawnCategory::Other;
}

// The initial spawn of a level / save (loading screen, precache frames) apart from the spawns while the game runs.
bool spawn_while_loading() { return load_screen_renderer.IsActive() || Device.dwPrecacheFrame > 0; }

#if XR_PERF_METRICS
void describe_spawn_spike(const void* ctx, char* buf, size_t size) noexcept
{
    const auto* E = static_cast<const CSE_Abstract*>(ctx);
    xr_sprintf(buf, size, "sect=%s id=%u parent=%u", E->s_name.c_str(), (unsigned)E->ID, (unsigned)E->ID_Parent);
}

struct SpawnClassStat
{
    pcstr name{};
    u64 ticks{};
    u64 max_ticks{};
    u64 create_ticks{};
    u64 net_spawn_ticks{};
    u32 count{};
};

struct SpawnSectionStat
{
    shared_str section;
    u64 ticks{};
    u64 max_ticks{};
    u32 count{};
};

// An "atomic group" of SPEC.md 5.A: a root M_SPAWN and the M_SPAWN of its children (inventory) right behind it in the
// same pass. Its cost is what one indivisible step of a spawn budget would take.
struct SpawnGroup
{
    xr_vector<u16> ids;
    shared_str root_section;
    u64 ticks{};
    u32 frame{};
    bool open{};
};

// Stage 2: the spawn budget in the window (frames of ProcessGameEvents with spawn_budget_us > 0 that had spawns)
struct SpawnBudgetStats
{
    u32 budget_us{};
    u32 min_groups{};
    u32 max_delay_ms{};
    u32 frames{}; // passes that materialized a group or were stopped by the budget
    u32 stopped_frames{}; // ... stopped: groups left for the next frame
    u32 groups{}; // groups materialized
    u32 group_size_max{};
    u32 groups_frame_max{}; // most groups in one pass
    u32 deferred_groups_max{}; // most groups left in the queue by one pass
    u32 deferred_spawns_max{};
    u32 queue_wait_ms_max{};
    u32 forced[size_t(CLevel::SpawnBudgetForce::Count)]{};
};

struct SpawnReport
{
    xr_unordered_map<const std::type_info*, SpawnClassStat> classes;
    xr_unordered_map<const void*, SpawnSectionStat> sections;
    u64 category_ticks[size_t(ClientSpawnCategory::Count)]{};
    u32 category_count[size_t(ClientSpawnCategory::Count)]{};
    xr_vector<u64> group_ticks;
    shared_str max_group_section;
    u64 max_group_ticks{};
    u32 max_group_items{};
    u32 window_start_ms{};
    u32 loading{};
    SpawnGroup group;
    SpawnBudgetStats budget;
};

SpawnReport s_spawn_report;

bool spawn_report_enabled() { return xray::perf::g_perf_metrics_active && (xray::perf::g_perf_metrics & 2) != 0; }

void spawn_group_close()
{
    SpawnReport& r = s_spawn_report;
    if (!r.group.open)
        return;
    r.group.open = false;
    r.group_ticks.push_back(r.group.ticks);
    if (r.group.ticks > r.max_group_ticks)
    {
        r.max_group_ticks = r.group.ticks;
        r.max_group_items = static_cast<u32>(r.group.ids.size()) - 1;
        r.max_group_section = r.group.root_section;
    }
}

void spawn_group_note(const CSE_Abstract* E, u64 ticks)
{
    SpawnGroup& g = s_spawn_report.group;
    const bool joins = g.open && g.frame == Device.dwFrame && E->ID_Parent != u16(-1) &&
        std::find(g.ids.begin(), g.ids.end(), E->ID_Parent) != g.ids.end();
    if (!joins)
    {
        spawn_group_close();
        g.ids.clear();
        g.root_section = E->s_name;
        g.ticks = 0;
        g.frame = Device.dwFrame;
        g.open = true;
    }
    g.ids.push_back(E->ID);
    g.ticks += ticks;
}

void spawn_report_flush(u32 now_ms)
{
    SpawnReport& r = s_spawn_report;
    spawn_group_close();

    u64 total_ticks = 0;
    u32 total_count = 0;
    string512 categories{};
    for (size_t i = 0; i < size_t(ClientSpawnCategory::Count); ++i)
    {
        total_ticks += r.category_ticks[i];
        total_count += r.category_count[i];
        if (!r.category_count[i])
            continue;
        string64 part;
        xr_sprintf(part, " %s=%u/%.2fms", client_spawn_category_names[i], r.category_count[i],
            xray::perf::TicksToMilliseconds(r.category_ticks[i]));
        xr_strcat(categories, part);
    }
    if (total_count)
    {
        std::sort(r.group_ticks.begin(), r.group_ticks.end());
        const size_t groups = r.group_ticks.size();
        const auto group_ms = [&r, groups](size_t index) {
            return groups ? xray::perf::TicksToMilliseconds(r.group_ticks[std::min(index, groups - 1)]) : 0.0;
        };
        Msg("* [spawn] window=%ums spawns=%u (while loading %u) total=%.2fms;%s", now_ms - r.window_start_ms,
            total_count, r.loading, xray::perf::TicksToMilliseconds(total_ticks), categories);
        Msg("*   [spawn] groups (a root + its children in one pass)=%u median=%.2fms p90=%.2fms max=%.2fms [%s + %u "
            "children]",
            static_cast<u32>(groups), group_ms(groups / 2), group_ms((groups * 9) / 10),
            xray::perf::TicksToMilliseconds(r.max_group_ticks),
            r.max_group_section.c_str() ? r.max_group_section.c_str() : "", r.max_group_items);
        const SpawnBudgetStats& b = r.budget;
        if (b.frames)
        {
            using Force = CLevel::SpawnBudgetForce;
            Msg("*   [spawn] budget=%uus (min %u groups, max delay %ums): frames=%u stopped=%u groups=%u (max %u per "
                "frame, largest %u spawns) left in the queue max=%u groups/%u spawns; forced loading=%u pause=%u "
                "delay=%u; queue wait max=%ums",
                b.budget_us, b.min_groups, b.max_delay_ms, b.frames, b.stopped_frames, b.groups, b.groups_frame_max,
                b.group_size_max, b.deferred_groups_max, b.deferred_spawns_max, b.forced[size_t(Force::Loading)],
                b.forced[size_t(Force::Pause)], b.forced[size_t(Force::Delay)], b.queue_wait_ms_max);
        }

        xr_vector<const SpawnClassStat*> classes;
        classes.reserve(r.classes.size());
        for (const auto& [type, entry] : r.classes)
            classes.push_back(&entry);
        std::sort(classes.begin(), classes.end(),
            [](const SpawnClassStat* a, const SpawnClassStat* b) { return a->ticks > b->ticks; });
        Msg("*   [spawn] classes=%u (top 10 by time: count total avg max, avg of create / net_spawn)",
            static_cast<u32>(classes.size()));
        for (size_t i = 0; i < classes.size() && i < 10; ++i)
        {
            const SpawnClassStat& e = *classes[i];
            const double ms = xray::perf::TicksToMilliseconds(e.ticks);
            Msg("*     %s x%u total=%.2fms avg=%.0fus max=%.0fus create=%.0fus net_spawn=%.0fus", e.name, e.count, ms,
                ms * 1000.0 / e.count, xray::perf::TicksToMilliseconds(e.max_ticks) * 1000.0,
                xray::perf::TicksToMilliseconds(e.create_ticks) * 1000.0 / e.count,
                xray::perf::TicksToMilliseconds(e.net_spawn_ticks) * 1000.0 / e.count);
        }

        xr_vector<const SpawnSectionStat*> sections;
        sections.reserve(r.sections.size());
        for (const auto& [key, entry] : r.sections)
            sections.push_back(&entry);
        std::sort(sections.begin(), sections.end(),
            [](const SpawnSectionStat* a, const SpawnSectionStat* b) { return a->max_ticks > b->max_ticks; });
        Msg("*   [spawn] sections=%u (top 5 by the most expensive single spawn)", static_cast<u32>(sections.size()));
        for (size_t i = 0; i < sections.size() && i < 5; ++i)
        {
            const SpawnSectionStat& e = *sections[i];
            const double ms = xray::perf::TicksToMilliseconds(e.ticks);
            Msg("*     %s x%u max=%.0fus avg=%.0fus", e.section.c_str(), e.count,
                xray::perf::TicksToMilliseconds(e.max_ticks) * 1000.0, ms * 1000.0 / e.count);
        }
    }

    r.classes.clear();
    r.sections.clear();
    std::fill(std::begin(r.category_ticks), std::end(r.category_ticks), u64(0));
    std::fill(std::begin(r.category_count), std::end(r.category_count), 0u);
    r.group_ticks.clear();
    r.max_group_section = nullptr;
    r.max_group_ticks = 0;
    r.max_group_items = 0;
    r.loading = 0;
    r.budget = {};
    r.window_start_ms = now_ms;
}

void spawn_report_note(const CSE_Abstract* E, const IGameObject* O, ClientSpawnCategory category, u64 ticks, u64 create_ticks,
    u64 net_spawn_ticks, bool loading)
{
    SpawnReport& r = s_spawn_report;
    const u32 now_ms = CPU::GetTicks();
    if (!r.window_start_ms)
        r.window_start_ms = now_ms;
    else if (now_ms - r.window_start_ms >= 5000)
        spawn_report_flush(now_ms);

    const std::type_info* type = &typeid(*O);
    SpawnClassStat& cls = r.classes[type];
    if (!cls.name)
        cls.name = xray::perf::SpikeClassName(*type);
    cls.ticks += ticks;
    cls.max_ticks = std::max(cls.max_ticks, ticks);
    cls.create_ticks += create_ticks;
    cls.net_spawn_ticks += net_spawn_ticks;
    ++cls.count;

    SpawnSectionStat& sect = r.sections[E->s_name.c_str()];
    if (!sect.section.c_str())
        sect.section = E->s_name;
    sect.ticks += ticks;
    sect.max_ticks = std::max(sect.max_ticks, ticks);
    ++sect.count;

    r.category_ticks[size_t(category)] += ticks;
    ++r.category_count[size_t(category)];
    if (loading)
        ++r.loading;
    spawn_group_note(E, ticks);
}
#endif // XR_PERF_METRICS
} // namespace

// Stage 2: a pass of ProcessGameEvents with the spawn budget on, into the "*   [spawn] budget" line of the report
void CLevel::SpawnBudgetReport(const SpawnBudgetFrame& frame)
{
#if XR_PERF_METRICS
    if (!spawn_report_enabled())
        return;
    SpawnBudgetStats& b = s_spawn_report.budget;
    b.budget_us = frame.budget_us;
    b.min_groups = frame.min_groups;
    b.max_delay_ms = frame.max_delay_ms;
    ++b.frames;
    if (frame.stopped)
        ++b.stopped_frames;
    b.groups += frame.groups;
    b.group_size_max = std::max(b.group_size_max, frame.group_size_max);
    b.groups_frame_max = std::max(b.groups_frame_max, frame.groups);
    b.deferred_groups_max = std::max(b.deferred_groups_max, frame.deferred_groups);
    b.deferred_spawns_max = std::max(b.deferred_spawns_max, frame.deferred_spawns);
    b.queue_wait_ms_max = std::max(b.queue_wait_ms_max, frame.queue_wait_ms);
    for (size_t i = 0; i < std::size(b.forced); ++i)
        b.forced[i] += frame.forced[i];
#else
    (void)frame;
#endif
}

void CLevel::cl_Process_Spawn(NET_Packet& P)
{
    // Begin analysis
    shared_str s_name;
    P.r_stringZ(s_name);

    // Create DC (xrSE)
    CSE_Abstract* E = nullptr;
    {
        PERF_STEP("cl_Process_Spawn/client_cse", spawn_client_cse);
        E = F_entity_Create(s_name.c_str());
        R_ASSERT2(E, s_name.c_str());

        E->Spawn_Read(P);
        if (E->s_flags.is(M_SPAWN_UPDATE))
            E->UPDATE_Read(P);
    }

    if (!E->match_configuration())
    {
        F_entity_Destroy(E);
        return;
    }
    //-------------------------------------------------
    //.	Msg ("M_SPAWN - %s[%d][%x] - %d %d", *s_name,  E->ID, E,E->ID_Parent, Device.dwFrame);
    //-------------------------------------------------
    // force object to be local for server client
    if (OnServer())
    {
        E->s_flags.set(M_SPAWN_OBJECT_LOCAL, TRUE);
    };

    /*
    game_spawn_queue.push_back(E);
    if (g_bDebugEvents)		ProcessGameSpawns();
    /*/
    g_sv_Spawn(E);

    F_entity_Destroy(E);
    //*/
};

void CLevel::g_cl_Spawn(LPCSTR name, u8 rp, u16 flags, Fvector pos)
{
    ZoneScoped;

    // Create
    CSE_Abstract* E = F_entity_Create(name);
    VERIFY(E);

    // Fill
    E->s_name = name;
    E->set_name_replace("");
    //.	E->s_gameid			=	u8(GameID());
    E->s_RP = rp;
    E->ID = 0xffff;
    E->ID_Parent = 0xffff;
    E->ID_Phantom = 0xffff;
    E->s_flags.assign(flags);
    E->RespawnTime = 0;
    E->o_Position = pos;

    // Send
    NET_Packet P;
    E->Spawn_Write(P, TRUE);
    Send(P, net_flags(TRUE));

    // Destroy
    F_entity_Destroy(E);
}

#ifdef DEBUG
extern Flags32 psAI_Flags;
extern float debug_on_frame_gather_stats_frequency;
#include "ai_debug.h"
#endif // DEBUG

void CLevel::g_sv_Spawn(CSE_Abstract* E)
{
    ZoneNamedN(___tracy_g_sv_spawn, "CLevel::g_sv_Spawn", true);
    ZoneTextVF(___tracy_g_sv_spawn, "%s", E->s_name.c_str());

    // Spawn stage 0: a zone per category (Tracy statistics by category), timers spawn/total and spawn/cat/*, the spike
    // site "spawn" (detail: section, ID, parent), the thread tag for cold loads inside a spawn (modelpool/*_spawn...)
    const ClientSpawnCategory category = client_spawn_category(E);
    ZoneNamedN(___tracy_spawn_stalker, "g_sv_Spawn[stalker]", category == ClientSpawnCategory::Stalker);
    ZoneNamedN(___tracy_spawn_monster, "g_sv_Spawn[monster]", category == ClientSpawnCategory::Monster);
    ZoneNamedN(___tracy_spawn_weapon, "g_sv_Spawn[weapon]", category == ClientSpawnCategory::Weapon);
    ZoneNamedN(___tracy_spawn_ammo, "g_sv_Spawn[ammo]", category == ClientSpawnCategory::Ammo);
    ZoneNamedN(___tracy_spawn_item, "g_sv_Spawn[item]", category == ClientSpawnCategory::Item);
    ZoneNamedN(___tracy_spawn_other, "g_sv_Spawn[other]", category == ClientSpawnCategory::Other);
    PERF_SCOPE(spawn_total);
    GW_FRAME_EVENT(Spawn); // 50n: the frame spike report
    PERF_SPIKE_NAMED(spawn_spike, "spawn", E, &describe_spawn_spike, false);
    PERF_SPAWN_TAG();
    const bool loading = spawn_while_loading();
    PERF_ADD_IF(loading, spawn_loading, 1);
#if XR_PERF_METRICS
    using xray::perf::Metric;
    static constexpr Metric category_metrics[] = {Metric::spawn_cat_stalker, Metric::spawn_cat_monster,
        Metric::spawn_cat_weapon, Metric::spawn_cat_ammo, Metric::spawn_cat_item, Metric::spawn_cat_other};
    xray::perf::ScopedTime category_time(category_metrics[size_t(category)]);
    const bool report = spawn_report_enabled();
    const u64 t_spawn0 = report ? xray::perf::Ticks() : 0;
    u64 create_ticks = 0;
    u64 net_spawn_ticks = 0;
#endif

//	CTimer		T(false);

#ifdef DEBUG
//	Msg					("* CLIENT: Spawn: %s, ID=%d", *E->s_name, E->ID);
#endif

    // Optimization for single-player only	- minimize traffic between client and server
    if (GameID() == eGameIDSingle)
        psNET_Flags.set(NETFLAG_MINIMIZEUPDATES, TRUE);
    else
        psNET_Flags.set(NETFLAG_MINIMIZEUPDATES, FALSE);

    // Same net ID already on the client: the spawn is skipped (creating a second object with this ID would break
    // map_NETID). An ID held by an object that is being destroyed normally never gets here: ProcessGameEvents waits for
    // its destruction (CLevel::SpawnMustWait, SPEC.md 8.6) - only if it gave up waiting. Both cases lose the object on
    // the client (the server has it), so they are errors.
    if (GameID() == eGameIDSingle && E->ID != 0xffff)
    {
        if (IGameObject* existing = Objects.net_Find(E->ID))
        {
            // 30-game-events.md, B1: a child of an object destroyed in this pass is marked by the batched walk only
            if (!existing->getDestroy())
                Objects.flush_destroy_child_scan();
            if (existing->getDestroy())
            {
                PERF_INC(spawn_id_reuse_lost);
                Msg("! ERROR: g_sv_Spawn: skip duplicate client spawn id[%u] [%s]: the ID is still held by [%s][%s] being "
                    "destroyed, the object is LOST on the client", (unsigned)E->ID, E->s_name.c_str(),
                    existing->cNameSect().c_str(), existing->cName().c_str());
            }
            else
            {
                PERF_INC(spawn_duplicate_skipped);
                Msg("! ERROR: g_sv_Spawn: skip duplicate client spawn id[%u] [%s] parent[%u]: the ID is held by the live "
                    "object [%s][%s] parent[%u]", (unsigned)E->ID, E->s_name.c_str(), (unsigned)E->ID_Parent,
                    existing->cNameSect().c_str(), existing->cName().c_str(),
                    existing->H_Parent() ? (unsigned)existing->H_Parent()->ID() : 0xffffu);
            }
            return;
        }
    }

    // Client spawn
    //	T.Start		();
    IGameObject* O = nullptr;
    {
        ZoneNamedN(___tracy_spawn_create, "CLevel::g_sv_Spawn/ObjectsCreate", true);
        PERF_SCOPE(spawn_create);
#if XR_PERF_METRICS
        const u64 t0 = report ? xray::perf::Ticks() : 0;
#endif
        O = Objects.Create(E->s_name.c_str());
#if XR_PERF_METRICS
        if (report)
            create_ticks = xray::perf::Ticks() - t0;
#endif
    }
// Msg				("--spawn--CREATE: %f ms",1000.f*T.GetAsync());

//	T.Start		();
    bool net_spawn_ok = false;
    if (O)
    {
        ZoneNamedN(___tracy_spawn_net, "CLevel::g_sv_Spawn/net_Spawn", true);
        PERF_SCOPE(spawn_net_spawn);
        PERF_SPIKE("spawn/net_spawn");
#if XR_PERF_METRICS
        const u64 t0 = report ? xray::perf::Ticks() : 0;
#endif
        net_spawn_ok = O->net_Spawn(E);
#if XR_PERF_METRICS
        if (report)
            net_spawn_ticks = xray::perf::Ticks() - t0;
#endif
    }
    if (!O || !net_spawn_ok)
    {
        ZoneNamedN(___tracy_spawn_fail, "CLevel::g_sv_Spawn/spawn_failed", true);
        PERF_INC(spawn_failed);
        if (O)
        {
            O->net_Destroy();
            if (!GEnv.isDedicatedServer)
                client_spawn_manager().clear(O->ID());
            Objects.Destroy(O);
        }
        Msg("! Failed to spawn entity '%s'", E->s_name.c_str());
        return;
    }

    {
        ZoneNamedN(___tracy_spawn_cb, "CLevel::g_sv_Spawn/client_spawn_callback", true);
        PERF_SCOPE(spawn_client_cb);
        if (!GEnv.isDedicatedServer)
            client_spawn_manager().callback(O);
    }
    // Msg			("--spawn--SPAWN: %f ms",1000.f*T.GetAsync());

    if ((E->s_flags.is(M_SPAWN_OBJECT_LOCAL)) && (E->s_flags.is(M_SPAWN_OBJECT_ASPLAYER)))
    {
        ZoneNamedN(___tracy_spawn_local, "CLevel::g_sv_Spawn/local_as_player", true);
        if (IsDemoPlayStarted())
        {
            if (E->s_flags.is(M_SPAWN_OBJECT_PHANTOM))
            {
                SetControlEntity(O);
                SetEntity(O); // do not switch !!!
                SetDemoSpectator(O);
            }
        }
        else
        {
            if (CurrentEntity() != NULL)
            {
                CGameObject* pGO = smart_cast<CGameObject*>(CurrentEntity());
                if (pGO)
                    pGO->On_B_NotCurrentEntity();
            }
            SetControlEntity(O);
            SetEntity(O); // do not switch !!!
        }
    }

    if (0xffff != E->ID_Parent)
    {
        ZoneNamedN(___tracy_spawn_parent, "CLevel::g_sv_Spawn/parent_ownership", true);
        PERF_SCOPE(spawn_parent_ownership);
        PERF_SPIKE("spawn/parent_ownership");
        // The parent is not on the client: cl_Process_Event drops the event and logs it (GE_OWNERSHIP_TAKE).
        PERF_ADD_IF(!Objects.net_Find(E->ID_Parent), spawn_parent_missing, 1);
        /*
        // Generate ownership-event
        NET_Packet			GEN;
        GEN.w_begin			(M_EVENT);
        GEN.w_u32			(E->m_dwSpawnTime);//-NET_Latency);
        GEN.w_u16			(GE_OWNERSHIP_TAKE);
        GEN.w_u16			(E->ID_Parent);
        GEN.w_u16			(u16(O->ID()));
        game_events->insert	(GEN);
        /*/
        NET_Packet GEN;
        GEN.write_start();
        GEN.read_start();
        GEN.w_u16(u16(O->ID()));
        cl_Process_Event(E->ID_Parent, GE_OWNERSHIP_TAKE, GEN);
        //*/
    }

    /*if (E->s_flags.is(M_SPAWN_UPDATE)) {
		NET_Packet				temp;
		temp.B.count			= 0;
		E->UPDATE_Write			(temp);
		if (temp.B.count > 0)
		{
			temp.r_seek				(0);
			O->net_Import			(temp);
		}
		}*/ //:(

    //---------------------------------------------------------
    {
        ZoneNamedN(___tracy_spawn_ons, "CLevel::g_sv_Spawn/Game_OnSpawn", true);
        PERF_SCOPE(spawn_game_on_spawn);
        Game().OnSpawn(O);
    }

#if XR_PERF_METRICS
    if (report)
        spawn_report_note(E, O, category, xray::perf::Ticks() - t_spawn0, create_ticks, net_spawn_ticks, loading);
#endif
}

CSE_Abstract* CLevel::spawn_item(
    LPCSTR section, const Fvector& position, u32 level_vertex_id, u16 parent_id, bool return_item)
{
    ZoneScoped;

    CSE_Abstract* abstract = F_entity_Create(section);
    R_ASSERT3(abstract, "Cannot find item with section", section);
    CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(abstract);
    if (dynamic_object && ai().get_level_graph())
    {
        dynamic_object->m_tNodeID = level_vertex_id;
        if (ai().level_graph().valid_vertex_id(level_vertex_id) && ai().get_game_graph() && ai().get_cross_table())
            dynamic_object->m_tGraphID = ai().cross_table().vertex(level_vertex_id).game_vertex_id();
    }

    //оружие спавним с полным магазинои
    CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(abstract);
    if (weapon)
        weapon->a_elapsed = weapon->get_ammo_magsize();

    // Fill
    abstract->s_name = section;
    abstract->set_name_replace(section);
    //.	abstract->s_gameid		= u8(GameID());
    abstract->o_Position = position;
    abstract->s_RP = 0xff;
    abstract->ID = 0xffff;
    abstract->ID_Parent = parent_id;
    abstract->ID_Phantom = 0xffff;
    abstract->s_flags.assign(M_SPAWN_OBJECT_LOCAL);
    abstract->RespawnTime = 0;

    if (!return_item)
    {
        NET_Packet P;
        abstract->Spawn_Write(P, TRUE);
        Send(P, net_flags(TRUE));
        F_entity_Destroy(abstract);
        return (0);
    }
    else
        return (abstract);
}

void CLevel::ProcessGameSpawns()
{
    ZoneScoped;

    while (!game_spawn_queue.empty())
    {
        CSE_Abstract* E = game_spawn_queue.front();

        g_sv_Spawn(E);

        F_entity_Destroy(E);

        game_spawn_queue.pop_front();
    }
}
