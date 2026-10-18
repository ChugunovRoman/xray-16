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
// main, loading screen (CLevel::net_start_client5): the creatures of the level and all monster visuals into the pool
void model_preload_level();

#include "alife_switch_manager_inline.h"
