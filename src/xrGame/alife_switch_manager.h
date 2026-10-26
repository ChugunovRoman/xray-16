////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_switch_manager.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator switch manager
////////////////////////////////////////////////////////////////////////////

#pragma once
#include "alife_simulator_base.h"

// XXX: WTF is this? CALifeSwitchManager IS-A CRandom??? I think NOT! CRandom should be aggregated, NOT inherited from!
class CALifeSwitchManager : public virtual CALifeSimulatorBase, CRandom
{
protected:
    typedef CALifeSimulatorBase inherited;
    typedef ALife::OBJECT_VECTOR OBJECT_VECTOR;

protected:
    float m_switch_distance;
    float m_switch_factor;
    float m_online_distance;
    float m_offline_distance;

private:
    OBJECT_VECTOR m_saved_chidren;

protected:
    bool synchronize_location(CSE_ALifeDynamicObject* object);

public:
    void try_switch_online(CSE_ALifeDynamicObject* object);
    void try_switch_offline(CSE_ALifeDynamicObject* object);
    void switch_online(CSE_ALifeDynamicObject* object);
    void switch_offline(CSE_ALifeDynamicObject* object);
    void remove_online(CSE_ALifeDynamicObject* object, bool update_registries = true);
    void add_online(CSE_ALifeDynamicObject* object, bool update_registries = true);

public:
    IC CALifeSwitchManager(IPureServer* server, LPCSTR section);
    virtual ~CALifeSwitchManager();
    void switch_object(CSE_ALifeDynamicObject* object);
    IC float online_distance() const noexcept;
    IC float offline_distance() const noexcept;
    IC float switch_distance() const noexcept;
    IC void set_switch_distance(float switch_distance) noexcept;
    IC void set_switch_factor(float switch_factor) noexcept;

    // Spawn stage 0 (plans/spawn_optimization/SPEC.md): a running total of the M_SPAWN sent by add_online - the object
    // and one per inventory item (add_online_impl). update_switch takes its difference over a pass (alife/switch/pass_spawns).
    std::atomic<u32> m_online_spawns{0};

    // 32-frame-spikes.md (alife_switch_offline_per_frame_max): a running total of the client objects remove_online
    // destroys (the object with all its children, client_objects); the spread switch pass of CALifeUpdateManager takes
    // its difference around each object it switches.
    std::atomic<u32> m_offline_client_objects{0};
    // The object and all its children (recursively): the client objects Perform_destroy of it destroys
    u32 client_objects(const CSE_ALifeDynamicObject* object) const;
    // How many client objects switch_object would destroy if it took this object offline now: 0 if it is offline,
    // attached or within offline_distance() (a squad: one of its members); an estimate only - can_switch_offline and the
    // scripted overrides are not asked (Lua), so the limit it feeds is soft.
    u32 switch_offline_estimate(const CSE_ALifeDynamicObject* object) const;
};

// 36-spawn-model-load.md (plans/optimization_mt), the model prefetch, always on since 2026-10-18 (model_prefetch 7
// removed, see plan 41): load the visuals of the creatures of the level (ALife objects, offline ones too) and of all
// monster sections into the model pool on the loading screen; read the files of the models of objects near the online
// distance (and of spawns the budget deferred) into the OS cache on the "Model prefetch" thread.
// model_prefetch_margin (a tunable, not saved, console_commands.cpp) - that read starts at online distance
// * (1 + margin / 100).
extern int g_model_prefetch_margin;
// ALife (the Game thread): whether an object this far from the actor is within the prefetch margin
bool model_prefetch_in_margin(float distance, float online_distance);
// ALife: queue the models of the object and of its inventory for the "Model prefetch" thread
void model_prefetch_object(CSE_ALifeDynamicObject* object);
// main (CLevel::ProcessGameEvents): the same for the object of an M_SPAWN the spawn budget left in the queue
void model_prefetch_spawn(u16 id);
// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R6), main (CLevel::ProcessGameEvents), a slow disk: the
// same for the object of an M_SPAWN of a group about to materialize (its jobs ahead of the ALife ones); true = the
// "Model prefetch" thread has not read the files of some of its models yet. 50l-weak-cpu-spawn-models.md: a weak CPU with
// the background base loading too - or a creature's base model is still to come from it
bool model_prefetch_spawn_pending(u16 id);
// main, loading screen (CLevel::net_start_client5): the creatures of the level and all monster visuals into the pool
void model_preload_level();

// plans/lua_to_cpp/33-gw-eoc-alife-switch.md (diagnostics only): can_switch_online / can_switch_offline of the switch
// decisions (CSE_ALifeDynamicObject / CSE_ALifeOnlineOfflineGroup::try_switch_*) - a Lua call for an object of a script
// class. A plain call; with -npc_cpp_profile inside switch_object also timed by the kind of the object
// (alife/switch_pass/can_online/*, can_offline/*)
bool alife_switch_profiled_can_online(const CSE_ALifeObject* object);
bool alife_switch_profiled_can_offline(const CSE_ALifeObject* object);
// The same for keep_saved_data_anyway (clear_client_data, CALifeSwitchManager::try_switch_online):
// alife/switch_pass/keep_saved, and the counter keep_saved_empty of the calls on an object with no client data
bool alife_switch_profiled_keep_saved(const CSE_ALifeDynamicObject* object);

// 33-gw-eoc-alife-switch.md, N1/N2 (always on since 2026-10-25, their switch gw_alife_switch_skip_lua removed after
// the runs 3-4): the switch pass asks the virtual can_switch_* / keep_saved_data_anyway (a Lua call for a script class)
// only when the answer can change the result.
// section 9: CALifeSwitchManager::synchronize_location without the Lua used_ai_locations and the graph search when the
// object stays where it is, and a squad checks its members against the registry once per switch_object instead of
// twice.
// section 10 (N3): used_ai_locations / can_switch_* / keep_saved_data_anyway of a Lua class that does not override
// them - the C++ default without luabind::call_member (xrServer_script_macroses.h); in synchronize_location the checks
// that read only fields go before used_ai_locations.
// N1: whether clear_client_data keeps the client data - keep_saved_data_anyway, not asked when the data is empty
// (clear() of an empty vector changes nothing): alife/switch_pass/keep_saved_empty counts such calls in the pass
bool alife_switch_keep_client_data(const CSE_ALifeDynamicObject* object);
// N2: a can_switch_* the full sequence would have asked and N2 did not (alife/switch_pass/can_skipped, in the pass with
// the profiler): counted after the answer that settles it. Not netted: far from the actor, out of the prefetch band,
// try_switch_online asks can_switch_offline first and with both answers false asks one more question than before
void alife_switch_count_skipped();

// 33-gw-eoc-alife-switch.md section 9 (-npc_cpp_profile): one part of the switch pass - a frame's part of update_spread or a
// whole update_switch, declared before their NPC_CPP_PROFILE_SCOPE (alife/update/switch) so that it brackets the same
// work. The switch_pass/* stages of the part are gathered on this thread without atomics and flushed when it ends;
// the timers inside switch_object run in every 8th part only (alife/switch_pass/part_sampled), the counters in every
// part, the other parts are timed whole (alife/switch_pass/part_clean). Without the profiler: one check
class CAlifeSwitchPassProfile
{
public:
    CAlifeSwitchPassProfile();
    ~CAlifeSwitchPassProfile();

    CAlifeSwitchPassProfile(const CAlifeSwitchPassProfile&) = delete;
    CAlifeSwitchPassProfile& operator=(const CAlifeSwitchPassProfile&) = delete;

private:
    u64 m_start{0};
    u8 m_saved_mode{0};
    bool m_active{false};
    bool m_sampled{false};
};

#include "alife_switch_manager_inline.h"
