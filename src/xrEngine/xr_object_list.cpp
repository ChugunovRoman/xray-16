#include "stdafx.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"

#include "xrSheduler.h"
#include "xr_object_list.h"

#include "xr_object.h"
#include "xrCore/net_utils.h"

#include "CustomHUD.h"
#include "GameFont.h"
#include "PerformanceAlert.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include "xrCommon/xr_unordered_map.h"

#include <typeinfo>

#include <xrCore/Threading/ParallelFor.hpp>
#include <xrCore/Threading/TaskManager.hpp>

extern int ps_obj_preupdate_mt;
extern int ps_obj_postupdate_mt;

namespace
{
void dispatch_preupdate_cl(IGameObject** b, IGameObject** e)
{
    if (b == e)
        return;
    if (ps_obj_preupdate_mt != 0 && TaskScheduler)
    {
        xr_parallel_for(TaskRange(b, e), [&](const TaskRange<IGameObject**>& range)
        {
            for (IGameObject** i = range.begin(); i != range.end(); ++i)
                (*i)->PreUpdateCL();
        });
    }
    else
    {
        for (IGameObject** i = b; i != e; ++i)
            (*i)->PreUpdateCL();
    }
}

template <class Vec>
void dispatch_postupdate_vec(Vec& vec, bool sleeping_flag)
{
    const size_t n = vec.size();
    if (!n)
        return;
    if (ps_obj_postupdate_mt != 0 && TaskScheduler)
    {
        xr_parallel_for(TaskRange(size_t(0), n), [&](const TaskRange<size_t>& range)
        {
            for (size_t i = range.begin(); i != range.end(); ++i)
                vec[i]->PostUpdateCL(sleeping_flag);
        });
    }
    else
    {
        for (auto& o : vec)
            o->PostUpdateCL(sleeping_flag);
    }
}

} // namespace

class fClassEQ
{
    CLASS_ID cls;

public:
    fClassEQ(CLASS_ID C) : cls(C){};
    IC bool operator()(IGameObject* O) { return cls == O->GetClassId(); }
};
#ifdef DEBUG
ENGINE_API BOOL debug_destroy = true;
#endif

void CObjectList::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    stats.FrameEnd();
    float engineTotal = Device.GetStats().EngineTotal.result;
    float percentage = 100.0f * stats.Update.result / engineTotal;
    font.OutNext("Objects:      %2.2fms, %2.1f%%", stats.Update.result, percentage);
    font.OutNext("- crow:       %d", stats.Crows);
    font.OutNext("- active:     %d", stats.Active);
    font.OutNext("- total:      %d", stats.Total);
    if (alert && stats.Update.result > 3.0f)
        alert->Print(font, "UpdateCL  > 3ms:  %3.1f", stats.Update.result);
}

CObjectList::CObjectList()
{
    statsFrame = u32(-1);
    ZeroMemory(map_NETID, 0xffff * sizeof(IGameObject*));
    ZeroMemory(m_destroy_origin, sizeof(m_destroy_origin));
    ZeroMemory(m_relcase_id_marks, sizeof(m_relcase_id_marks));
    std::fill(std::begin(m_relcase_visited_recent), std::end(m_relcase_visited_recent), nullptr);
    // +1: reserved slot for threads that are NOT a registered TaskManager worker but can still legitimately
    // reach o_crow() - today that was CPHWorld::m_physics_thread (Threading::RunThread()) before it
    // registered itself as a worker; now it registers on its first step and gets a slot of its own below
    // GetMaxWorkersCount() (09-physics-earlier-parallel.md, B8). get_crows() maps any out-of-range
    // GetCurrentWorkerID() to this slot instead of indexing out of bounds (GetCurrentWorkerID() default-inits to
    // size_t(-1) for an unknown thread - VERIFY below catches it in DEBUG, but silently reads/writes far out of
    // bounds in Release).
    // GetMaxWorkersCount(), not GetWorkersCount(): the game/render overlap "Game" thread registers itself as a
    // worker only on its first frame (after this constructor), and its ID must not land on the reserved slot.
    m_secondary_crows.resize(TaskScheduler->GetMaxWorkersCount() + 1);
}

CObjectList::~CObjectList()
{
    R_ASSERT(objects_active.empty());
    R_ASSERT(objects_sleeping.empty());
    R_ASSERT(destroy_queue.empty());
    //. R_ASSERT ( map_NETID.empty() );
}

IGameObject* CObjectList::FindObjectByName(shared_str name)
{
    for (auto& it : objects_active)
        if (it->cName().equal(name))
            return it;

    for (auto& it : objects_sleeping)
        if (it->cName().equal(name))
            return it;

    return nullptr;
}
IGameObject* CObjectList::FindObjectByName(pcstr name) { return FindObjectByName(shared_str(name)); }
IGameObject* CObjectList::FindObjectByCLS_ID(CLASS_ID cls)
{
    {
        Objects::iterator O = std::find_if(objects_active.begin(), objects_active.end(), fClassEQ(cls));
        if (O != objects_active.end())
            return *O;
    }
    {
        Objects::iterator O = std::find_if(objects_sleeping.begin(), objects_sleeping.end(), fClassEQ(cls));
        if (O != objects_sleeping.end())
            return *O;
    }

    return NULL;
}

bool CObjectList::o_remove(Objects& v, IGameObject* O)
{
    Objects::iterator _i = std::find(v.begin(), v.end(), O);
    VERIFY(_i != v.end());
    // Release safety: never erase(end()) — that memmoves a garbage-sized range -> AV. Hit in heavy combat
    // when an object is sleep/activate-requested while already moved out of this list (e.g. double
    // ObjectProcessingDeactivate from a physics shell coming to rest). Returning false here also stops the
    // caller from double-adding O to the other list (which would leave a stale duplicate). Debug VERIFY
    // above still flags the underlying double-move.
    if (_i == v.end())
        return false;
    v.erase(_i);
    return true;
    //. Msg("---o_remove[%s][%d]", O->cName().c_str(), O->ID() );
}

void CObjectList::o_activate(IGameObject* O)
{
    VERIFY(O && O->processing_enabled());
    if (!o_remove(objects_sleeping, O))
        return; // already active (not in sleeping list) -> do not duplicate it in objects_active
    objects_active.push_back(O);
    PERF_CROW_SRC(xray::perf::crow::OlAct);
    O->MakeMeCrow();
}
void CObjectList::o_sleep(IGameObject* O)
{
    VERIFY(O && !O->processing_enabled());
    if (!o_remove(objects_active, O))
        return; // already sleeping (not in active list) -> do not duplicate it in objects_sleeping
    objects_sleeping.push_back(O);
    PERF_CROW_SRC(xray::perf::crow::OlSleep);
    O->MakeMeCrow();
}

void CObjectList::SingleUpdate(IGameObject* O)
{
    if (Device.dwFrame == O->GetUpdateFrame())
    {
#ifdef DEBUG
// if (O->getDestroy())
// Msg ("- !!!processing_enabled ->destroy_queue.push_back %s[%d] frame [%d]",O->cName().c_str(), O->ID(),
// Device.dwFrame);
#endif // #ifdef DEBUG

        return;
    }

    if (!O->processing_enabled())
    {
#ifdef DEBUG
// if (O->getDestroy())
// Msg ("- !!!processing_enabled ->destroy_queue.push_back %s[%d] frame [%d]",O->cName().c_str(), O->ID(),
// Device.dwFrame);
#endif // #ifdef DEBUG

        return;
    }

    if (O->H_Parent())
        SingleUpdate(O->H_Parent());
    stats.Updated++;    
    O->SetUpdateFrame(Device.dwFrame);

    // Msg ("[%d][0x%08x]IAmNotACrowAnyMore (CObjectList::SingleUpdate)", Device.dwFrame, dynamic_cast<void*>(O));

    {
        ZoneScopedN("ol_ObjUpdateCL");
        // P19b: the actor's own items (the weapon in hands, detector...) apart - ucl/actor_item, spikes with the CPU time
        const bool perf_on = xray::perf::g_perf_metrics_active || xray::perf::SpikeActive();
        const bool actor_item = perf_on && O->H_Parent() && O->H_Parent()->cast_actor();
        // P19 spike logger (perf_spike_us): one UpdateCL longer than the threshold; the actor always with the CPU time
        PERF_SPIKE_OBJECT_CPU("ucl", O, xray::perf::SpikeActive() && (actor_item || O->cast_actor()));
        if (xray::perf::g_perf_metrics_active)
        {
            using M = xray::perf::Metric;
            M metric = M::ucl_other;
            if (O->cast_actor())
                metric = M::ucl_actor;
            else if (actor_item)
                metric = M::ucl_actor_item;
            else if (O->cast_stalker())
                metric = M::ucl_stalker;
            else if (O->cast_base_monster())
                metric = M::ucl_monster;
            else if (O->cast_custom_monster())
                metric = M::ucl_custom_monster;
            else if (O->cast_weapon())
                metric = M::ucl_weapon;
            else if (O->cast_artefact())
                metric = M::ucl_artefact;
            else if (O->cast_inventory_item())
                metric = M::ucl_item;
            else if (O->cast_custom_zone())
                metric = M::ucl_zone;
            else if (O->cast_restrictor())
                metric = M::ucl_restrictor;
            else if (O->cast_physics_shell_holder())
                metric = M::ucl_psh;
            xray::perf::ScopedTime ucl_time(metric);
            O->UpdateCL();
        }
        else
            O->UpdateCL();
    }

    VERIFY3(O->GetDbgUpdateFrame() == Device.dwFrame, "Broken sequence of calls to 'UpdateCL'", O->cName().c_str());
#if 0 // ndef DEBUG
    __try
    {
#endif
    if (O->H_Parent() && (O->H_Parent()->getDestroy() || O->H_Root()->getDestroy()))
    {
        // Push to destroy-queue if it isn't here already
        Msg("! ERROR: incorrect destroy sequence for object[%d:%s], section[%s], parent[%d:%s]", O->ID(), O->cName().c_str(),
            O->cNameSect().c_str(), O->H_Parent()->ID(), O->H_Parent()->cName().c_str());
    }
#if 0 // ndef DEBUG
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        IGameObject* parent_obj = O->H_Parent();
        IGameObject* root_obj = O->H_Root();
        Msg ("! ERROR: going to crush: [%d:%s], section[%s], parent_obj_addr[0x%08x], root_obj_addr[0x%08x]",O->ID(),O->cName().c_str(),*O->cNameSect(), *((u32*)&parent_obj), *((u32*)&root_obj));
        if (parent_obj)
        {
            __try
            {
                Msg("! Parent object: [%d:%s], section[%s]",
                    parent_obj->ID(),
                    parent_obj->cName().c_str(),
                    parent_obj->cNameSect().c_str());

            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Msg("! Failed to get parent object info.");
            }
        }
        if (root_obj)
        {
            __try
            {
                Msg("! Root object: [%d:%s], section[%s]",
                    root_obj->ID(),
                    root_obj->cName().c_str(),
                    root_obj->cNameSect().c_str());
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Msg("! Failed to get root object info.");
            }
        }
        R_ASSERT(false);
    } //end of __except
#endif

#ifdef DEBUG
// if (O->getDestroy())
// Msg ("- !!!processing_enabled ->destroy_queue.push_back %s[%d] frame [%d]",O->cName().c_str(), O->ID(),
// Device.dwFrame);
#endif // #ifdef DEBUG
}

void CObjectList::dispatch_singleupdate_workload(IGameObject** b, IGameObject** e)
{
    const u32 n = u32(e - b);
    if (!n)
        return;

    // SingleUpdate -> UpdateCL must stay on the main thread: game/AI/scripts/physics are not MT-safe.
    constexpr u32 object_profile_batch = 256;
    for (IGameObject** batch_b = b; batch_b < e; batch_b += object_profile_batch)
    {
        ZoneScopedN("ol_Batch256");
        IGameObject** batch_e = batch_b + object_profile_batch;
        if (batch_e > e)
            batch_e = e;
        ZoneTextF("[%u,%u) / %u", u32(batch_b - b), u32(batch_e - b), n);

        {
            ZoneScopedN("ol_SingleUpdate");
            for (IGameObject** i = batch_b; i < batch_e; ++i)
                SingleUpdate(*i);
        }
    }
}

void CObjectList::clear_crow_vec(Objects& o)
{
    for (auto& it : o)
    {
        // Msg ("[%d][0x%08x]IAmNotACrowAnyMore (clear_crow_vec)", Device.dwFrame, dynamic_cast<void*>(it));
        it->IAmNotACrowAnyMore();
    }
    o.clear();
}

void CObjectList::Update(bool bForce)
{
    ZoneScopedN("ol_Update");

    if (statsFrame != Device.dwFrame)
    {
        statsFrame = Device.dwFrame;
        stats.FrameStart();
    }

    {
        ZoneScopedN("ol_MergeCrows");
        PERF_SCOPE(ol_upd_prepare);
        for (auto& crows_list : m_secondary_crows)
        {
            m_primary_crows.insert(m_primary_crows.end(), crows_list.cbegin(), crows_list.cend());
            crows_list.clear();
        }
    }

    if (!Device.Paused() || bForce)
    {
        ZoneScopedN("ol_UpdateCL");

        // Clients
        if (Device.fTimeDelta > EPS_S || bForce)
        {
            // Select Crow-Mode
            stats.Updated = 0;

#if 0
            std::sort (m_own_crows.begin(), m_own_crows.end());
            m_own_crows.erase (
                std::unique(
                    m_own_crows.begin(),
                    m_own_crows.end()
                ),
                m_own_crows.end()
            );
#else
#ifdef DEBUG
            std::sort(m_primary_crows.begin(), m_primary_crows.end());
            VERIFY(std::unique(m_primary_crows.begin(), m_primary_crows.end()) == m_primary_crows.end());
#endif // ifdef DEBUG
#endif

            stats.Crows = m_primary_crows.size();
            xray::perf::SetGauge(xray::perf::Metric::objects_crows, static_cast<s64>(stats.Crows));
            Objects* workload;
            if (!psDeviceFlags.test(rsDisableObjectsAsCrows))
                workload = &m_primary_crows;
            else
            {
                workload = &objects_active;
                clear_crow_vec(m_primary_crows);
            }

            stats.Update.Begin();
            stats.Active = objects_active.size();
            stats.Total = objects_active.size() + objects_sleeping.size();

            u32 const objects_count = workload->size();
            IGameObject** objects = (IGameObject**)xr_alloca(objects_count * sizeof(IGameObject*));
            {
                ZoneScopedN("ol_CopyWorkload");
                PERF_SCOPE(ol_upd_prepare);
                std::copy(workload->begin(), workload->end(), objects);
            }

            m_primary_crows.clear();

            IGameObject** b = objects;
            IGameObject** e = objects + objects_count;
            {
                ZoneScopedN("ol_ResetCrowFlags");
                PERF_SCOPE(ol_upd_prepare);
                for (IGameObject** i = b; i != e; ++i)
                {
                    (*i)->IAmNotACrowAnyMore();
                    (*i)->SetCrowUpdateFrame(u32(-1));
                }
            }

            {
                ZoneScopedN("ol_PreUpdateCL");
                PERF_SCOPE(ol_upd_pre);
                dispatch_preupdate_cl(b, e);
            }
            {
                ZoneScopedN("ol_SingleUpdate");
                PERF_SCOPE(ol_upd_single);
                ZoneValue(objects_count);
                dispatch_singleupdate_workload(b, e);
            }

            //--#SM+#-- PostUpdateCL для всех клиентских объектов [for crowed and non-crowed]
            {
                ZoneScopedN("ol_PostUpdate_active");
                PERF_SCOPE(ol_upd_post);
                dispatch_postupdate_vec(objects_active, false);
            }

            {
                ZoneScopedN("ol_PostUpdate_sleep");
                PERF_SCOPE(ol_upd_post);
                dispatch_postupdate_vec(objects_sleeping, true);
            }

            stats.Update.End();
            xray::perf::SetGauge(xray::perf::Metric::objects_updated, static_cast<s64>(stats.Updated));
        }
    }

    // Destroy
    if (!destroy_queue.empty())
        process_destroy_queue();
}

#if XR_PERF_METRICS
namespace
{
// P17 (plans/optimization_mt/12-relcase.md): relcase report of perf_metrics (log bit), printed by the first frame with a
// destroy after 5 s: what the relcase pass costs per class of the receiving object and what was destroyed (section,
// class, who asked for it). CObjectList::Update only (main thread), so no locks.
struct RelcaseClassStat
{
    pcstr name{};
    u64 ticks{};
    u64 objects{};
    u64 pairs{};
};

struct RelcaseSectionStat
{
    shared_str section;
    pcstr class_name{};
    u32 count{};
    u32 children{};
    u32 by_origin[CObjectList::destroy_origin_count]{};
};

constexpr pcstr relcase_origin_names[CObjectList::destroy_origin_count] = {
    "unknown", "engine", "lua", "plugin", "switch", "server"};

xr_unordered_map<const std::type_info*, RelcaseClassStat> s_relcase_classes;
xr_unordered_map<const void*, RelcaseSectionStat> s_relcase_sections;
u32 s_relcase_window_start_ms = 0;
u32 s_relcase_frames = 0;
u32 s_relcase_destroyed = 0;
u32 s_relcase_children = 0;
u32 s_relcase_origins[CObjectList::destroy_origin_count]{};

bool relcase_report_enabled()
{
    return xray::perf::g_perf_metrics_active && (xray::perf::g_perf_metrics & 2) != 0;
}

pcstr relcase_class_name(const std::type_info& type)
{
    pcstr name = type.name();
    if (0 == strncmp(name, "class ", 6))
        return name + 6;
    if (0 == strncmp(name, "struct ", 7))
        return name + 7;
    return name;
}

// Time of one receiving object's relcase (all destroyed objects), by its most derived class.
void relcase_profile_object(const IGameObject* object, u64 ticks, u32 pairs)
{
    const std::type_info* type = &typeid(*object);
    RelcaseClassStat& entry = s_relcase_classes[type];
    if (!entry.name)
        entry.name = relcase_class_name(*type);
    entry.ticks += ticks;
    ++entry.objects;
    entry.pairs += pairs;
}

void relcase_note_destroyed(const IGameObject* object, u8 origin)
{
    const shared_str& section = object->cNameSect();
    RelcaseSectionStat& entry = s_relcase_sections[section.c_str()];
    if (!entry.class_name)
    {
        entry.section = section;
        entry.class_name = relcase_class_name(typeid(*object));
    }
    const u8 kind = origin & ~CObjectList::destroy_origin_child;
    const u8 index = kind < CObjectList::destroy_origin_count ? kind : u8(CObjectList::destroy_origin_unknown);
    ++entry.count;
    ++entry.by_origin[index];
    ++s_relcase_origins[index];
    ++s_relcase_destroyed;
    if (origin & CObjectList::destroy_origin_child)
    {
        ++entry.children;
        ++s_relcase_children;
    }
}

void relcase_report_flush(u32 now_ms)
{
    const u32 elapsed_ms = now_ms - s_relcase_window_start_ms;
    Msg("* relcase window=%ums destroy_frames=%u destroyed=%u (engine=%u lua=%u plugin=%u switch=%u server=%u "
        "unknown=%u; destroyed with the parent=%u)",
        elapsed_ms, s_relcase_frames, s_relcase_destroyed, s_relcase_origins[CObjectList::destroy_origin_engine],
        s_relcase_origins[CObjectList::destroy_origin_lua], s_relcase_origins[CObjectList::destroy_origin_plugin],
        s_relcase_origins[CObjectList::destroy_origin_switch], s_relcase_origins[CObjectList::destroy_origin_server],
        s_relcase_origins[CObjectList::destroy_origin_unknown], s_relcase_children);

    xr_vector<const RelcaseClassStat*> classes;
    classes.reserve(s_relcase_classes.size());
    u64 total_ticks = 0;
    for (const auto& [type, entry] : s_relcase_classes)
    {
        classes.push_back(&entry);
        total_ticks += entry.ticks;
    }
    std::sort(classes.begin(), classes.end(),
        [](const RelcaseClassStat* a, const RelcaseClassStat* b) { return a->ticks > b->ticks; });
    Msg("*   relcase/receivers total=%.2fms classes=%u (top 10 by time; avg per receiving object and per object x "
        "destroyed pair)",
        xray::perf::TicksToMilliseconds(total_ticks), static_cast<u32>(classes.size()));
    for (size_t i = 0; i < classes.size() && i < 10; ++i)
    {
        const RelcaseClassStat& entry = *classes[i];
        const double ms = xray::perf::TicksToMilliseconds(entry.ticks);
        Msg("*     %s total=%.2fms objects=%llu pairs=%llu avg=%.2fus/object %.3fus/pair", entry.name, ms,
            entry.objects, entry.pairs, entry.objects ? ms * 1000.0 / double(entry.objects) : 0.0,
            entry.pairs ? ms * 1000.0 / double(entry.pairs) : 0.0);
    }

    xr_vector<const RelcaseSectionStat*> sections;
    sections.reserve(s_relcase_sections.size());
    for (const auto& [key, entry] : s_relcase_sections)
        sections.push_back(&entry);
    std::sort(sections.begin(), sections.end(),
        [](const RelcaseSectionStat* a, const RelcaseSectionStat* b) { return a->count > b->count; });
    Msg("*   relcase/destroyed sections=%u (top 10)", static_cast<u32>(sections.size()));
    for (size_t i = 0; i < sections.size() && i < 10; ++i)
    {
        const RelcaseSectionStat& entry = *sections[i];
        string256 origins{};
        for (u32 o = 0; o < CObjectList::destroy_origin_count; ++o)
        {
            if (!entry.by_origin[o])
                continue;
            string32 part;
            xr_sprintf(part, " %s=%u", relcase_origin_names[o], entry.by_origin[o]);
            xr_strcat(origins, part);
        }
        Msg("*     %s [%s] x%u%s with_parent=%u", entry.section.c_str(), entry.class_name, entry.count, origins,
            entry.children);
    }

    s_relcase_classes.clear();
    s_relcase_sections.clear();
    s_relcase_window_start_ms = now_ms;
    s_relcase_frames = 0;
    s_relcase_destroyed = 0;
    s_relcase_children = 0;
    std::fill(std::begin(s_relcase_origins), std::end(s_relcase_origins), 0u);
}
} // namespace
#endif // XR_PERF_METRICS

// The destroy phase of Update(): every object drops its links to the objects destroyed this frame (net_Relcase), then
// the sound scene, the relcase callbacks (Feel::Touch / Feel::Vision ...) and the HUD, then the objects are destroyed.
// Always batched: one net_RelcaseBatch per object instead of vanilla's net_Relcase per object x destroyed object
// (P17, plans/optimization_mt/12-relcase.md).
void CObjectList::process_destroy_queue()
{
    ZoneScopedN("ol_Destroy_relcase");
#if XR_PERF_METRICS
    // P19 spike logger: the whole destroy phase (relcase + net_Destroy) of a frame, detail = its size
    struct RelcaseSpikeInfo
    {
        u32 destroyed, active, sleeping, callbacks;
    };
    const RelcaseSpikeInfo spike_info{static_cast<u32>(destroy_queue.size()), static_cast<u32>(objects_active.size()),
        static_cast<u32>(objects_sleeping.size()), static_cast<u32>(m_relcase_callbacks.size())};
    const xray::perf::SpikeDescribeFn describe_spike = [](const void* ctx, char* buf, size_t size) {
        const auto* info = static_cast<const RelcaseSpikeInfo*>(ctx);
        xr_sprintf(buf, size, "destroyed=%u active=%u sleeping=%u callbacks=%u", info->destroyed,
            info->active, info->sleeping, info->callbacks);
    };
    PERF_SPIKE_NAMED(relcase_spike, "objects/relcase", &spike_info, describe_spike, true);
#endif
    ZoneTextF("%u destroyed x %u active + %u sleeping, %u callbacks",
        static_cast<u32>(destroy_queue.size()), static_cast<u32>(objects_active.size()),
        static_cast<u32>(objects_sleeping.size()), static_cast<u32>(m_relcase_callbacks.size()));
    PERF_ADD(ol_destroy, destroy_queue.size());
    PERF_ADD(ol_destroy_active, objects_active.size());
    PERF_ADD(ol_destroy_sleeping, objects_sleeping.size());
    PERF_ADD(ol_destroy_cb, m_relcase_callbacks.size());
    PERF_INC(ol_relcase_frames);

    // Who asked for each destruction (origin table, see destroy_origin_note): read once here and forgotten, so that
    // a later object with the same ID starts clean.
#if XR_PERF_METRICS
    const bool report = relcase_report_enabled();
    if (report)
        ++s_relcase_frames;
#endif
#ifdef TRACY_ENABLE
    string1024 tracy_sections{};
    const bool tracy_text = TracyIsConnected;
#endif
    for (IGameObject* O : destroy_queue)
    {
        const u16 id = O->ID();
        const u8 origin = destroy_origin(id);
        if (id < 0xffff)
            m_destroy_origin[id] = destroy_origin_unknown;
#if XR_PERF_METRICS
        if (report)
            relcase_note_destroyed(O, origin);
#endif
#ifdef TRACY_ENABLE
        if (tracy_text && xr_strlen(tracy_sections) + 80 < sizeof(tracy_sections))
        {
            string128 part;
            xr_sprintf(part, "%s%s(%u%s)", tracy_sections[0] ? " " : "", O->cNameSect().c_str(),
                u32(origin & ~destroy_origin_child), (origin & destroy_origin_child) ? "c" : "");
            xr_strcat(tracy_sections, part);
        }
#endif
    }
#ifdef TRACY_ENABLE
    // section(origin) per destroyed object: 0 unknown, 1 engine, 2 lua, 3 plugin, 4 switch, 5 server; "c" = with parent
    if (tracy_text && tracy_sections[0])
        ZoneText(tracy_sections, xr_strlen(tracy_sections));
#endif

    // destroy_queue may grow while its objects are relcased (a callback destroying something else): relcase the
    // new part as a batch of its own instead of leaving it out
    for (size_t done = 0; done < destroy_queue.size();)
    {
        m_relcase_objects.assign(destroy_queue.begin() + done, destroy_queue.end());
        done = destroy_queue.size();

        m_relcase_sorted.assign(m_relcase_objects.begin(), m_relcase_objects.end());
        std::sort(m_relcase_sorted.begin(), m_relcase_sorted.end());
        for (IGameObject* O : m_relcase_objects)
            m_relcase_id_marks[O->ID()] = 1;

        RelcaseBatch batch;
        batch.objects = m_relcase_objects.data();
        batch.count = static_cast<u32>(m_relcase_objects.size());
        batch.sorted = m_relcase_sorted.data();
        batch.id_marks = m_relcase_id_marks;

        relcase_objects_batch(batch);
        {
            ZoneScopedN("ol_Destroy_sound");
            PERF_SCOPE(ol_relcase_sound);
            for (u32 it = batch.count; it-- > 0;)
                g_pGameLevel->Sound->object_relcase(batch.objects[it]);
        }
        relcase_callbacks_batch(batch);

        for (IGameObject* O : m_relcase_objects)
            m_relcase_id_marks[O->ID()] = 0;
    }
    m_relcase_objects.clear();
    m_relcase_sorted.clear();

    {
        ZoneScopedN("ol_Destroy_net");
        PERF_SCOPE(ol_relcase_net);
        for (int it = destroy_queue.size() - 1; it >= 0; it--)
        {
            IGameObject* O = destroy_queue[it];
// Msg ("Object [%x]", O);
#ifdef DEBUG
            if (debug_destroy)
                Msg("Destroying object[%x][%x] [%d][%s] frame[%d]", dynamic_cast<void*>(O), O, O->ID(), O->cName().c_str(),
                    Device.dwFrame);
#endif // DEBUG
            O->net_Destroy();
            Destroy(O);
        }
    }
    destroy_queue.clear();

#if XR_PERF_METRICS
    if (report)
    {
        const u32 now_ms = CPU::GetTicks();
        if (!s_relcase_window_start_ms)
            s_relcase_window_start_ms = now_ms;
        else if (now_ms - s_relcase_window_start_ms >= 5000)
            relcase_report_flush(now_ms);
    }
#endif
}

// P17: one net_RelcaseBatch per active and sleeping object. Containers shared by several objects
// (group memory lists, agent managers) are cleaned of the whole batch by the first object that reaches them
// (relcase_first_visit), see CVisualMemoryManager::remove_links and others in xrGame.
void CObjectList::relcase_objects_batch(const RelcaseBatch& batch)
{
    m_relcase_visited.clear();
    std::fill(std::begin(m_relcase_visited_recent), std::end(m_relcase_visited_recent), nullptr);
    m_relcase_visited_recent_pos = 0;
    m_relcase_batch = &batch;
#if XR_PERF_METRICS
    const bool report = relcase_report_enabled();
#endif
    // by index: nothing here is meant to change the lists, but an index never dangles if something does
    {
        ZoneScopedN("ol_Destroy_relcase_act");
        PERF_SCOPE(ol_relcase_act);
        for (size_t i = 0; i < objects_active.size(); ++i)
        {
            IGameObject* O = objects_active[i];
#if XR_PERF_METRICS
            const u64 t0 = report ? xray::perf::Ticks() : 0;
            O->net_RelcaseBatch(batch);
            if (report)
                relcase_profile_object(O, xray::perf::Ticks() - t0, batch.count);
#else
            O->net_RelcaseBatch(batch);
#endif
        }
    }
    {
        ZoneScopedN("ol_Destroy_relcase_sleep");
        PERF_SCOPE(ol_relcase_sleep);
        for (size_t i = 0; i < objects_sleeping.size(); ++i)
        {
            IGameObject* O = objects_sleeping[i];
#if XR_PERF_METRICS
            const u64 t0 = report ? xray::perf::Ticks() : 0;
            O->net_RelcaseBatch(batch);
            if (report)
                relcase_profile_object(O, xray::perf::Ticks() - t0, batch.count);
#else
            O->net_RelcaseBatch(batch);
#endif
        }
    }
    m_relcase_batch = nullptr;
}

// P17: the relcase callbacks get the batch at once when they can take it (Feel::Vision), the others
// one call per destroyed object as before; the HUD once per destroyed object - vanilla called it once per callback
// and destroyed object (thousands of identical calls), the result is the same.
void CObjectList::relcase_callbacks_batch(const RelcaseBatch& batch)
{
    ZoneScopedN("ol_Destroy_callbacks");
    PERF_SCOPE(ol_relcase_cb);
    // Snapshot: a callback may destroy another object whose ~pure_relcase calls relcase_unregister() (see above).
    // A member reused from batch to batch (~3500 entries, P17b): no allocation per frame with a destroy; this function
    // is not re-entered (only process_destroy_queue calls it, callbacks do not run the object list update).
    m_relcase_callbacks_snapshot.assign(m_relcase_callbacks.begin(), m_relcase_callbacks.end());
    for (const SRelcasePair& pair : m_relcase_callbacks_snapshot)
    {
        if (pair.m_BatchCallback)
        {
            pair.m_BatchCallback(batch);
            PERF_INC(ol_relcase_cb_calls);
            continue;
        }
        for (u32 it = 0; it < batch.count; ++it)
            pair.m_Callback(batch.objects[it]);
        PERF_ADD(ol_relcase_cb_calls, batch.count);
    }
    m_relcase_callbacks_snapshot.clear();
    for (u32 it = 0; it < batch.count; ++it)
        g_pGameLevel->pHUD->net_Relcase(batch.objects[it]);
    PERF_ADD(ol_relcase_hud_calls, batch.count);
}

bool CObjectList::relcase_first_visit(const void* container)
{
    // the members of one group come one after another for every destroyed object: most calls hit the last few keys
    for (const void* recent : m_relcase_visited_recent)
    {
        if (recent == container)
            return false;
    }
    m_relcase_visited_recent[m_relcase_visited_recent_pos++ % std::size(m_relcase_visited_recent)] = container;
    if (std::find(m_relcase_visited.begin(), m_relcase_visited.end(), container) != m_relcase_visited.end())
        return false;
    m_relcase_visited.push_back(container);
    return true;
}

void CObjectList::net_Register(IGameObject* O)
{
    R_ASSERT(O);
    R_ASSERT(O->ID() < 0xffff);

    map_NETID[O->ID()] = O;
    m_destroy_origin[O->ID()] = destroy_origin_unknown; // P17: forget an origin noted for an earlier object with this ID
    //. map_NETID.insert(std::make_pair(O->ID(),O));
    // Msg ("-------------------------------- Register: %s",O->cName());
}

void CObjectList::net_Unregister(IGameObject* O)
{
    // R_ASSERT (O->ID() < 0xffff);
    if (O->ID() < 0xffff) // demo_spectator can have 0xffff
        map_NETID[O->ID()] = NULL;
    /*
     xr_map<u32,IGameObject*>::iterator it = map_NETID.find(O->ID());
     if ((it!=map_NETID.end()) && (it->second == O)) {
     // Msg ("-------------------------------- Unregster: %s",O->cName());
     map_NETID.erase(it);
     }
     */
}

int g_Dump_Export_Obj = 0;

u32 CObjectList::net_Export(NET_Packet* _Packet, u32 start, u32 max_object_size)
{
    if (g_Dump_Export_Obj)
        Msg("---- net_export --- ");

    NET_Packet& Packet = *_Packet;
    u32 position;
    for (; start < objects_active.size() + objects_sleeping.size(); start++)
    {
        IGameObject* P =
            (start < objects_active.size()) ? objects_active[start] : objects_sleeping[start - objects_active.size()];
        if (P->net_Relevant() && !P->getDestroy())
        {
            Packet.w_u16(u16(P->ID()));
            Packet.w_chunk_open8(position);
            // Msg ("cl_export: %d '%s'",P->ID(),*P->cName());
            P->net_Export(Packet);

#ifdef DEBUG
            {
                u32 size = u32(Packet.w_tell() - position) - sizeof(u8);
                if (size >= 256)
                {
                    xrDebug::Fatal(DEBUG_INFO, "Object [%s][%d] exceed network-data limit\n size=%d, Pend=%d, Pstart=%d",
                    P->cName().c_str(), P->ID(), size, Packet.w_tell(), position);
                }
            }
#endif
            if (g_Dump_Export_Obj)
            {
                u32 size = u32(Packet.w_tell() - position) - sizeof(u8);
                Msg("* %s : %d", P->cNameSect().c_str(), size);
            }
            Packet.w_chunk_close8(position);
            // if (0==(--count))
            // break;
            if (max_object_size >= (NET_PacketSizeLimit - Packet.w_tell()))
                break;
        }
    }
    if (g_Dump_Export_Obj)
        Msg("------------------- ");
    return start + 1;
}

int g_Dump_Import_Obj = 0;

void CObjectList::net_Import(NET_Packet* Packet)
{
    if (g_Dump_Import_Obj)
        Msg("---- net_import --- ");

    while (!Packet->r_eof())
    {
        u16 ID;
        Packet->r_u16(ID);
        u8 size;
        Packet->r_u8(size);
        IGameObject* P = net_Find(ID);
        if (P)
        {
            u32 rsize = Packet->r_tell();

            P->net_Import(*Packet);

            if (g_Dump_Import_Obj)
                Msg("* %s : %d - %d", P->cNameSect().c_str(), size, Packet->r_tell() - rsize);
        }
        else
            Packet->r_advance(size);
    }

    if (g_Dump_Import_Obj)
        Msg("------------------- ");
}

/*
IGameObject* CObjectList::net_Find(u16 ID)
{

xr_map<u32,IGameObject*>::iterator it = map_NETID.find(ID);
return (it==map_NETID.end())?0:it->second;
}
*/
void CObjectList::Load()
{
    R_ASSERT(/*map_NETID.empty() &&*/ objects_active.empty() && destroy_queue.empty() && objects_sleeping.empty());
}

void CObjectList::Unload()
{
    ZoneScoped;

    if (objects_sleeping.size() || objects_active.size())
        Msg("! objects-leaked: %d", objects_sleeping.size() + objects_active.size());

    // Destroy objects
    while (objects_sleeping.size())
    {
        IGameObject* O = objects_sleeping.back();
        Msg("! [%x] s[%4d]-[%s]-[%s]", O, O->ID(), O->cNameSect().c_str(), O->cName().c_str());
        O->setDestroy(true);

#ifdef DEBUG
        if (debug_destroy)
            Msg("Destroying object [%d][%s]", O->ID(), O->cName().c_str());
#endif
        O->net_Destroy();
        Destroy(O);
    }
    while (objects_active.size())
    {
        IGameObject* O = objects_active.back();
        Msg("! [%x] a[%4d]-[%s]-[%s]", O, O->ID(), O->cNameSect().c_str(), O->cName().c_str());
        O->setDestroy(true);

#ifdef DEBUG
        if (debug_destroy)
            Msg("Destroying object [%d][%s]", O->ID(), O->cName().c_str());
#endif
        O->net_Destroy();
        Destroy(O);
    }

    // Unload() destroys objects directly; setDestroy(true) above pushed them into destroy_queue.
    // Clear the queue so ~CObjectList() assertion (destroy_queue.empty()) does not fail.
    destroy_queue.clear();
}

IGameObject* CObjectList::Create(pcstr name)
{
    IGameObject* O = g_pGamePersistent->ObjectPool.create(name);
    // Msg("CObjectList::Create [%x]%s", O, name);
    objects_sleeping.push_back(O);
    return O;
}

void CObjectList::Destroy(IGameObject* game_obj)
{
    if (nullptr == game_obj)
        return;
    net_Unregister(game_obj);

    {
        // Always remove the object from list of own crows. The object may be not a crow.
        auto iter = std::find(m_primary_crows.begin(), m_primary_crows.end(), game_obj);
        if (iter != m_primary_crows.end())
            m_primary_crows.erase(iter);

        for (auto& crows_list : m_secondary_crows)
        {
            auto iter = std::find(crows_list.begin(), crows_list.end(), game_obj);
            if (iter != crows_list.end())
                crows_list.erase(iter);
        }
    }

    // Remove the object from list of active objects if the object is active,
    // either remove it from list of sleeping objects if it is sleeping
    // and throw fatal error if the object is neither active nor sleeping
    auto iter = std::find(objects_active.begin(), objects_active.end(), game_obj);
    if (iter != objects_active.end())
    {
        // this object is active, so remove it from the list
        objects_active.erase(iter);
        // check that the object does not belong to list of sleeping object too
        VERIFY(std::find(objects_sleeping.begin(), objects_sleeping.end(), game_obj) == objects_sleeping.end());
    }
    else
    {
        iter = std::find(objects_sleeping.begin(), objects_sleeping.end(), game_obj);
        if (iter == objects_sleeping.end())
            FATAL("! Unregistered object being destroyed");
        objects_sleeping.erase(iter);

    }

    g_pGamePersistent->ObjectPool.destroy(game_obj);
}

void CObjectList::relcase_register(RELCASE_CALLBACK cb, int* ID)
{
#ifdef DEBUG
    RELCASE_CALLBACK_VEC::iterator It = std::find(m_relcase_callbacks.begin(), m_relcase_callbacks.end(), cb);
    VERIFY(It == m_relcase_callbacks.end());
#endif
    *ID = m_relcase_callbacks.size();
    m_relcase_callbacks.push_back(SRelcasePair(ID, cb));
}

void CObjectList::relcase_register(RELCASE_CALLBACK cb, RELCASE_BATCH_CALLBACK batch_cb, int* ID)
{
#ifdef DEBUG
    RELCASE_CALLBACK_VEC::iterator It = std::find(m_relcase_callbacks.begin(), m_relcase_callbacks.end(), cb);
    VERIFY(It == m_relcase_callbacks.end());
#endif
    *ID = m_relcase_callbacks.size();
    m_relcase_callbacks.push_back(SRelcasePair(ID, cb, batch_cb));
}

void CObjectList::relcase_unregister(int* ID)
{
    VERIFY(m_relcase_callbacks[*ID].m_ID == ID);
    m_relcase_callbacks[*ID] = m_relcase_callbacks.back();
    *m_relcase_callbacks.back().m_ID = *ID;
    m_relcase_callbacks.pop_back();
}

void CObjectList::dump_list(Objects& v, pcstr reason)
{
#ifdef DEBUG
    for (auto& it : v)
        Msg("%x - name [%s] ID[%d] parent[%s] getDestroy()=[%s]", it, it->cName().c_str(), it->ID(),
        it->H_Parent() ? it->H_Parent()->cName().c_str() : "", it->getDestroy() ? "yes" : "no");
#endif // #ifdef DEBUG
}

bool CObjectList::dump_all_objects()
{
    dump_list(destroy_queue, "destroy_queue");
    dump_list(objects_active, "objects_active");
    dump_list(objects_sleeping, "objects_sleeping");
    dump_list(m_primary_crows, "m_own_crows");
    for (auto& crows_list : m_secondary_crows)
    {
        dump_list(crows_list, "m_other_crows");
    }
    return false;
}

void CObjectList::register_object_to_destroy(IGameObject* object_to_destroy)
{
    VERIFY(!registered_object_to_destroy(object_to_destroy));
    // Msg("CObjectList::register_object_to_destroy [%x]", object_to_destroy);
    destroy_queue.push_back(object_to_destroy);

    for (auto& it : objects_active)
    {
        IGameObject* O = it;
        if (!O->getDestroy() && O->H_Parent() == object_to_destroy)
        {
            Msg("setDestroy called, but not-destroyed child found parent[%d] child[%d]", object_to_destroy->ID(),
                O->ID(), Device.dwFrame);
            O->setDestroy(true);
        }
    }

    for (auto& it : objects_sleeping)
    {
        IGameObject* O = it;
        if (!O->getDestroy() && O->H_Parent() == object_to_destroy)
        {
            Msg("setDestroy called, but not-destroyed child found parent[%d] child[%d]", object_to_destroy->ID(),
                O->ID(), Device.dwFrame);
            O->setDestroy(true);
        }
    }
}

IC CObjectList::Objects& CObjectList::get_crows()
{
    const size_t list_id = TaskScheduler->GetCurrentWorkerID();
    // Not a registered TaskManager worker (today: CPHWorld's dedicated physics thread, reachable via
    // NearCallback -> EnableObject -> activate -> o_activate -> MakeMeCrow during Collide()) - route to the
    // one reserved extra slot (see the +1 in CObjectList::CObjectList()) instead of indexing out of bounds.
    if (list_id >= m_secondary_crows.size() - 1)
        return m_secondary_crows.back();
    return m_secondary_crows[list_id];
}

void CObjectList::o_crow(IGameObject* O)
{
    Objects& crows = get_crows();
    VERIFY(std::find(crows.begin(), crows.end(), O) == crows.end());
    crows.push_back(O);

    O->SetCrowUpdateFrame(Device.dwFrame);
}

#ifdef DEBUG
bool CObjectList::registered_object_to_destroy(const IGameObject* object_to_destroy) const
{
    return (std::find(destroy_queue.begin(), destroy_queue.end(), object_to_destroy) != destroy_queue.end());
}
#endif // DEBUG
