#include "StdAfx.h"

#include "npc_cpp_profile.h"
#include "addon_api_console.h"

#include <atomic>
#include <algorithm>
#include <cstring>

#include "xrCore/Threading/Lock.hpp"

namespace
{
constexpr u32 NPC_CPP_PROFILE_FLUSH_INTERVAL_MS = 5000;
constexpr size_t NPC_CPP_PROFILE_STAGE_COUNT = static_cast<size_t>(ENpcCppProfileStage::Count);
constexpr size_t NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT = 512;
// How far the search goes from the hashed slot before a name is given up on. The mod registers about 110
// evaluators, so without probing a name that hashes onto a taken slot lost every sample it ever made.
constexpr size_t NPC_CPP_PROFILE_SCRIPT_EVALUATOR_PROBE = 8;
// Every bucket is drained on a flush, so an evaluator that does not make the printed top loses its numbers for
// good: the report is the only place they go. 16 rows hid about half of the calls of a session, and the profiler
// only runs with -npc_cpp_profile anyway, where a longer table is what is wanted.
constexpr size_t NPC_CPP_PROFILE_SCRIPT_EVALUATOR_TOP_COUNT = 64;
constexpr size_t NPC_CPP_PROFILE_TOP_COUNT = 48; // 32 hid the movement/* and script_entity/sound/* sub-stages

struct NpcCppProfileCounters
{
    std::atomic_ullong total_qpc{0};
    std::atomic_ullong calls{0};
    std::atomic_ullong max_qpc{0};
};

struct NpcCppProfileSnapshot
{
    const char* name;
    u64 total_qpc;
    u64 calls;
    u64 max_qpc;
};

struct ScriptEvaluatorProfileBucket
{
    std::atomic_ullong total_qpc{0};
    std::atomic_ullong calls{0};
    std::atomic_ullong max_qpc{0};
    std::atomic_ullong cache_hits{0};
    std::atomic_ullong cache_misses{0};
    // A copy, not the pointer that was passed in: an evaluator of a script dies with its Lua state (a new game
    // or a load restarts the script engine), and the bucket outlives it.
    char name[128]{}; // cut to the size by named_bucket, compared cut the same way
};

struct ScriptEvaluatorProfileSnapshot
{
    const char* name;
    const char* group;
    u64 total_qpc;
    u64 calls;
    u64 max_qpc;
    u64 cache_hits;
    u64 cache_misses;
};

NpcCppProfileCounters g_npc_cpp_profile_counters[NPC_CPP_PROFILE_STAGE_COUNT];
ScriptEvaluatorProfileBucket g_script_evaluator_profile_buckets[NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT];
// The same buckets for the steps of Lua actions ("<action name>/<step>"): script_action_top of the report
ScriptEvaluatorProfileBucket g_script_action_profile_buckets[NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT];
Lock g_npc_cpp_profile_flush_lock;
std::atomic<u32> g_npc_cpp_profile_next_flush_ms{0};
std::atomic<u32> g_npc_cpp_profile_debug_heartbeat_ms{0};
std::atomic_ullong g_npc_cpp_profile_total_add_calls{0};
std::atomic_bool g_npc_cpp_profile_logged_enabled{false};
std::atomic_bool g_npc_cpp_profile_logged_first_add{false};
std::atomic_bool g_npc_cpp_profile_logged_first_schedule{false};
std::atomic_bool g_npc_cpp_profile_logged_lock_busy{false};

constexpr const char* g_npc_cpp_profile_stage_names[NPC_CPP_PROFILE_STAGE_COUNT] = {
    "game_object/schedule_update",
    "script_binder/update",
    "script_binder/luabind_update",
    "script_entity/process_scripts",
    "script_entity/process_sound_callbacks",
    "script_entity/sound_callback_dispatch",
    "stalker/schedule_update",
    "stalker/update_cl",
    "stalker/think",
    "stalker/think/brain",
    "stalker/planner/solve",
    "stalker/planner/actuality_check",
    "stalker/planner/actuality_fast_path",
    "stalker/planner/actuality_skipped",
    "stalker/planner/state_clear",
    "stalker/planner/graph_search",
    "stalker/planner/transition",
    "stalker/planner/execute",
    "stalker/planner/execute_death",
    "stalker/planner/execute_alife",
    "stalker/planner/execute_combat",
    "stalker/planner/execute_danger",
    "stalker/planner/execute_anomaly",
    "stalker/planner/execute_gather_items",
    "stalker/planner/execute_other",
    "stalker/think/movement_update",
    "stalker/memory_update",
    "stalker/memory_update/visual",
    "stalker/memory_update/sound",
    "stalker/memory_update/hit",
    "stalker/memory_update/collect_objects",
    "stalker/memory_update/collect_visual_objects",
    "stalker/memory_update/collect_sound_objects",
    "stalker/memory_update/collect_hit_objects",
    "stalker/memory_update/collect_danger_add",
    "stalker/memory_update/collect_enemy_add",
    "stalker/memory_update/collect_item_add",
    "stalker/danger/add_visible",
    "stalker/danger/add_sound",
    "stalker/danger/add_hit",
    "stalker/danger/add_danger_object",
    "stalker/danger/useful_check",
    "stalker/danger/find_existing",
    "stalker/enemy/useful_check",
    "stalker/enemy/useful_alive_check",
    "stalker/enemy/useful_spatial_check",
    "stalker/enemy/useful_relation_check",
    "stalker/enemy/useful_vertex_check",
    "stalker/enemy/useful_monster_filter",
    "stalker/enemy/useful_callback",
    "stalker/enemy/evaluate",
    "stalker/enemy/expedient",
    "stalker/enemy/try_change",
    "stalker/enemy/process_wounded",
    "stalker/enemy/need_update",
    "stalker/enemy/inherited_update",
    "stalker/memory_update/update_enemies",
    "stalker/memory_update/item",
    "stalker/memory_update/danger",
    "stalker/object_handler_update",
    "stalker/update_cl/object_handler_dispatch",
    "stalker/update_cl/inherited",
    "stalker/update_cl/physics",
    "stalker/update_cl/sight_manager",
    "stalker/update_cl/exec_look",
    "stalker/update_cl/step_manager",
    "stalker/update_cl/weapon_effector",
    "stalker/schedule_update/visibility",
    "stalker/schedule_update/think_apply",
    "character_physics/update_cl",
    "character_physics/animation_collision",
    "character_physics/calc_time_delta",
    "character_physics/shell_set_ragdoll",
    "character_physics/shell_interpolate",
    "character_physics/interactive_motion_update",
    "character_physics/death_anims",
    "character_physics/friction",
    "character_physics/update_interactive_anims",
    "character_physics/ik_update",
    "custom_monster/schedule_update",
    "custom_monster/update_cl",
    "custom_monster/update_cl/inherited",
    "custom_monster/update_cl/process_sound_callbacks",
    "custom_monster/update_cl/network_extrapolation",
    "custom_monster/update_cl/update_position_animation",
    "custom_monster/update_cl/apply_net_state",
    "custom_monster/update_cl/update_camera",
    "custom_monster/update_cl/animation_controller",
    "custom_monster/think",
    "custom_monster/memory_update",
    "custom_monster/exec_visibility",
    "custom_monster/visibility_s0",
    "custom_monster/visibility_s1",
    "custom_monster/visibility_s2",
    "custom_monster/sound_player_update",
    "physics_shell_holder/update_cl",
    "physics_shell_holder/update_particles",
    "game_object/update_cl/spatial",
    "game_object/update_cl/crow",
    "game_object/update_cl/matrix_change",
    "script_evaluator/evaluate",
    "script_action/update",
    "script_action/initialize",
    "inventory_owner/has_info",
    "addon/binder_update",
    "movement/update_path",
    "movement/move_along_path",
    "movement/move_along_path/nearest_query",
    "movement/move_along_path/collision_move",
    "custom_monster/update_cl/select_animation",
    "script_entity/sound/lua_callback",
    "script_entity/sound/plugin_event",
    "step/update",
    "step/update/material_pick",
    "step/update/sound",
    "step/update/particles",
    "step/update/event",
    "agent_enemy/distribute",
    "agent_enemy/distribute/fill",
    "agent_enemy/distribute/assign",
};

static_assert((sizeof(g_npc_cpp_profile_stage_names) / sizeof(g_npc_cpp_profile_stage_names[0])) == NPC_CPP_PROFILE_STAGE_COUNT);

IC size_t to_index(const ENpcCppProfileStage stage)
{
    return static_cast<size_t>(stage);
}

IC void update_max(std::atomic_ullong& target, const u64 value)
{
    std::atomic_ullong::value_type current = target.load(std::memory_order_relaxed);
    const std::atomic_ullong::value_type desired = static_cast<std::atomic_ullong::value_type>(value);
    while (current < desired && !target.compare_exchange_weak(current, desired, std::memory_order_relaxed))
    {
    }
}

IC u32 fnv1a_hash(pcstr value)
{
    if (!value)
        return 0;

    u32 hash = 2166136261u;
    for (const unsigned char* it = reinterpret_cast<const unsigned char*>(value); *it; ++it)
    {
        hash ^= *it;
        hash *= 16777619u;
    }
    return hash;
}

// The bucket of this name: the one that already holds it, or the first free one after the hashed slot.
// Returns nullptr when the probe window is full - then the sample goes to the common stage instead.
ScriptEvaluatorProfileBucket* named_bucket(ScriptEvaluatorProfileBucket* buckets, pcstr evaluator_name)
{
    const size_t start = static_cast<size_t>(fnv1a_hash(evaluator_name) % NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT);
    for (size_t probe = 0; probe < NPC_CPP_PROFILE_SCRIPT_EVALUATOR_PROBE; ++probe)
    {
        const size_t index = (start + probe) % NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT;
        ScriptEvaluatorProfileBucket& bucket = buckets[index];
        // A longer name is cut (xr_strcpy would hit the invalid parameter handler outside MASTER_GOLD): the same name
        // is cut the same way, so it still finds its bucket
        if (!bucket.name[0])
        {
            std::snprintf(bucket.name, sizeof bucket.name, "%s", evaluator_name);
            return &bucket;
        }
        if (std::strncmp(bucket.name, evaluator_name, sizeof bucket.name - 1) == 0)
            return &bucket;
    }
    return nullptr;
}

ScriptEvaluatorProfileBucket* script_evaluator_bucket(pcstr evaluator_name)
{
    return named_bucket(g_script_evaluator_profile_buckets, evaluator_name);
}

// script_action_top: the steps of Lua actions by total time (the Lua part of stalker/planner/execute)
void flush_script_action_snapshots()
{
    xr_vector<NpcCppProfileSnapshot> snapshots;
    for (size_t i = 0; i < NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT; ++i)
    {
        ScriptEvaluatorProfileBucket& bucket = g_script_action_profile_buckets[i];
        const u64 total_qpc = bucket.total_qpc.exchange(0, std::memory_order_relaxed);
        const u64 calls = bucket.calls.exchange(0, std::memory_order_relaxed);
        const u64 max_qpc = bucket.max_qpc.exchange(0, std::memory_order_relaxed);
        if (!bucket.name[0] || !calls)
            continue;
        snapshots.push_back({bucket.name, total_qpc, calls, max_qpc});
    }
    if (snapshots.empty())
        return;
    std::sort(snapshots.begin(), snapshots.end(),
        [](const NpcCppProfileSnapshot& left, const NpcCppProfileSnapshot& right) { return left.total_qpc > right.total_qpc; });
    const size_t shown = std::min<size_t>(snapshots.size(), NPC_CPP_PROFILE_SCRIPT_EVALUATOR_TOP_COUNT);
    Msg("* npc_cpp_profile script_action_top=%u", static_cast<u32>(shown));
    for (size_t i = 0; i < shown; ++i)
    {
        const NpcCppProfileSnapshot& row = snapshots[i];
        const double total_ms = 1000.0 * static_cast<double>(row.total_qpc) / CPU::qpc_freq;
        const double avg_us = 1000000.0 * static_cast<double>(row.total_qpc) / (CPU::qpc_freq * row.calls);
        const double max_us = 1000000.0 * static_cast<double>(row.max_qpc) / CPU::qpc_freq;
        Msg("*   %s total=%.2fms calls=%llu avg=%.2fus max=%.2fus", row.name, total_ms,
            static_cast<unsigned long long>(row.calls), avg_us, max_us);
    }
}

const char* classify_script_evaluator_group(pcstr evaluator_name)
{
    if (!evaluator_name || !evaluator_name[0])
        return "unnamed";

    if (strstr(evaluator_name, "state_mgr") != nullptr)
        return "state_mgr";

    if (strstr(evaluator_name, "xr_logic") != nullptr || strstr(evaluator_name, "logic") != nullptr)
        return "logic";

    if (strstr(evaluator_name, "combat") != nullptr || strstr(evaluator_name, "enemy") != nullptr ||
        strstr(evaluator_name, "danger") != nullptr || strstr(evaluator_name, "grenade") != nullptr)
        return "combat";

    if (strstr(evaluator_name, "smartcover") != nullptr || strstr(evaluator_name, "smart_cover") != nullptr)
        return "smart_cover";

    if (strstr(evaluator_name, "anim") != nullptr || strstr(evaluator_name, "bodystate") != nullptr ||
        strstr(evaluator_name, "mental") != nullptr || strstr(evaluator_name, "movement") != nullptr ||
        strstr(evaluator_name, "weapon") != nullptr || strstr(evaluator_name, "direction") != nullptr)
        return "state_subsystem";

    if (strstr(evaluator_name, "wound") != nullptr || strstr(evaluator_name, "corpse") != nullptr)
        return "post_combat";

    return "other";
}

void flush_script_evaluator_snapshots()
{
    xr_vector<ScriptEvaluatorProfileSnapshot> snapshots;
    snapshots.reserve(NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT);

    for (size_t i = 0; i < NPC_CPP_PROFILE_SCRIPT_EVALUATOR_BUCKET_COUNT; ++i)
    {
        ScriptEvaluatorProfileBucket& bucket = g_script_evaluator_profile_buckets[i];
        const pcstr name = bucket.name[0] ? bucket.name : nullptr;
        const u64 total_qpc = bucket.total_qpc.exchange(0, std::memory_order_relaxed);
        const u64 calls = bucket.calls.exchange(0, std::memory_order_relaxed);
        const u64 max_qpc = bucket.max_qpc.exchange(0, std::memory_order_relaxed);
        const u64 cache_hits = bucket.cache_hits.exchange(0, std::memory_order_relaxed);
        const u64 cache_misses = bucket.cache_misses.exchange(0, std::memory_order_relaxed);

        if (!name || (!calls && !cache_hits && !cache_misses))
            continue;

        snapshots.push_back({name, classify_script_evaluator_group(name), total_qpc, calls, max_qpc, cache_hits, cache_misses});
    }

    if (snapshots.empty())
        return;

    std::sort(snapshots.begin(), snapshots.end(), [](const ScriptEvaluatorProfileSnapshot& left,
                                                      const ScriptEvaluatorProfileSnapshot& right) {
        return left.total_qpc > right.total_qpc;
    });

    const size_t shown = std::min<size_t>(snapshots.size(), NPC_CPP_PROFILE_SCRIPT_EVALUATOR_TOP_COUNT);
    Msg("* npc_cpp_profile script_evaluator_top=%u", static_cast<u32>(shown));

    for (size_t i = 0; i < shown; ++i)
    {
        const ScriptEvaluatorProfileSnapshot& row = snapshots[i];
        const double total_ms = 1000.0 * static_cast<double>(row.total_qpc) / CPU::qpc_freq;
        const double avg_us = row.calls ? 1000000.0 * static_cast<double>(row.total_qpc) / (CPU::qpc_freq * row.calls) : 0.0;
        const double max_us = 1000000.0 * static_cast<double>(row.max_qpc) / CPU::qpc_freq;
        const u64 cache_total = row.cache_hits + row.cache_misses;
        const double hit_rate = cache_total ? (100.0 * static_cast<double>(row.cache_hits) / static_cast<double>(cache_total)) : 0.0;

        Msg("*   [%s] %s total=%.2fms calls=%llu avg=%.2fus max=%.2fus cache_hits=%llu cache_misses=%llu hit_rate=%.1f%%",
            row.group, row.name, total_ms,
            static_cast<unsigned long long>(row.calls), avg_us, max_us,
            static_cast<unsigned long long>(row.cache_hits),
            static_cast<unsigned long long>(row.cache_misses), hit_rate);
    }
}

IC void debug_log_heartbeat(const u32 now_ms, const u32 next_flush_ms)
{
    u32 heartbeat_ms = g_npc_cpp_profile_debug_heartbeat_ms.load(std::memory_order_relaxed);
    if (heartbeat_ms && now_ms < heartbeat_ms)
        return;

    g_npc_cpp_profile_debug_heartbeat_ms.store(now_ms + NPC_CPP_PROFILE_FLUSH_INTERVAL_MS, std::memory_order_relaxed);
    Msg("* npc_cpp_profile debug heartbeat now=%u next=%u add_calls=%llu qpc_freq=%llu",
        now_ms, next_flush_ms,
        static_cast<unsigned long long>(g_npc_cpp_profile_total_add_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(CPU::qpc_freq));
}

void flush_snapshots(const u32 now_ms)
{
    xr_vector<NpcCppProfileSnapshot> snapshots;
    snapshots.reserve(NPC_CPP_PROFILE_STAGE_COUNT);

    for (size_t i = 0; i < NPC_CPP_PROFILE_STAGE_COUNT; ++i)
    {
        NpcCppProfileCounters& counters = g_npc_cpp_profile_counters[i];
        const u64 total_qpc = counters.total_qpc.exchange(0, std::memory_order_relaxed);
        const u64 calls = counters.calls.exchange(0, std::memory_order_relaxed);
        const u64 max_qpc = counters.max_qpc.exchange(0, std::memory_order_relaxed);
        if (!calls || !total_qpc)
            continue;

        snapshots.push_back({g_npc_cpp_profile_stage_names[i], total_qpc, calls, max_qpc});
    }

    g_npc_cpp_profile_next_flush_ms.store(now_ms + NPC_CPP_PROFILE_FLUSH_INTERVAL_MS, std::memory_order_relaxed);

    if (snapshots.empty())
    {
        Msg("* npc_cpp_profile debug flush_empty now=%u next=%u add_calls=%llu",
            now_ms, now_ms + NPC_CPP_PROFILE_FLUSH_INTERVAL_MS,
            static_cast<unsigned long long>(g_npc_cpp_profile_total_add_calls.load(std::memory_order_relaxed)));
        return;
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const NpcCppProfileSnapshot& left, const NpcCppProfileSnapshot& right) {
        return left.total_qpc > right.total_qpc;
    });

    const size_t shown = std::min<size_t>(snapshots.size(), NPC_CPP_PROFILE_TOP_COUNT);
    Msg("* npc_cpp_profile top=%u interval=%ums", static_cast<u32>(shown), NPC_CPP_PROFILE_FLUSH_INTERVAL_MS);

    for (size_t i = 0; i < shown; ++i)
    {
        const NpcCppProfileSnapshot& row = snapshots[i];
        const double total_ms = 1000.0 * static_cast<double>(row.total_qpc) / CPU::qpc_freq;
        const double avg_us = 1000000.0 * static_cast<double>(row.total_qpc) / (CPU::qpc_freq * row.calls);
        const double max_us = 1000000.0 * static_cast<double>(row.max_qpc) / CPU::qpc_freq;

        Msg("*   %s total=%.2fms calls=%llu avg=%.2fus max=%.2fus", row.name, total_ms,
            static_cast<unsigned long long>(row.calls), avg_us, max_us);
    }

    flush_script_evaluator_snapshots();
    flush_script_action_snapshots();
}
} // namespace

bool npc_cpp_profile::enabled()
{
    static const bool value = strstr(Core.Params, "-npc_cpp_profile") != nullptr;
    if (value && !g_npc_cpp_profile_logged_enabled.exchange(true, std::memory_order_relaxed))
    {
        Msg("* npc_cpp_profile debug enabled params=%s", Core.Params ? Core.Params : "<null>");
    }
    return value;
}

void npc_cpp_profile::flush_if_needed()
{
    if (!enabled())
        return;

    const u32 now_ms = Device.dwTimeGlobal;
    u32 next_flush_ms = g_npc_cpp_profile_next_flush_ms.load(std::memory_order_relaxed);

    if (!next_flush_ms)
    {
        const u32 scheduled_flush_ms = now_ms + NPC_CPP_PROFILE_FLUSH_INTERVAL_MS;
        if (g_npc_cpp_profile_next_flush_ms.compare_exchange_strong(next_flush_ms, scheduled_flush_ms, std::memory_order_relaxed))
        {
            if (!g_npc_cpp_profile_logged_first_schedule.exchange(true, std::memory_order_relaxed))
            {
                Msg("* npc_cpp_profile debug schedule_first now=%u next=%u", now_ms, scheduled_flush_ms);
            }
        }
        return;
    }

    debug_log_heartbeat(now_ms, next_flush_ms);

    if (now_ms < next_flush_ms)
        return;

    if (!g_npc_cpp_profile_flush_lock.TryEnter())
    {
        if (!g_npc_cpp_profile_logged_lock_busy.exchange(true, std::memory_order_relaxed))
        {
            Msg("* npc_cpp_profile debug lock_busy now=%u next=%u", now_ms, next_flush_ms);
        }
        return;
    }

    next_flush_ms = g_npc_cpp_profile_next_flush_ms.load(std::memory_order_relaxed);
    if (now_ms >= next_flush_ms)
    {
        Msg("* npc_cpp_profile debug flush_begin now=%u next=%u add_calls=%llu",
            now_ms, next_flush_ms,
            static_cast<unsigned long long>(g_npc_cpp_profile_total_add_calls.load(std::memory_order_relaxed)));
        flush_snapshots(now_ms);
        gw::addons::console::FlushProfile(); // stages of plugins (profile_stage), printed with ours
    }

    g_npc_cpp_profile_flush_lock.Leave();
}

void npc_cpp_profile::add(const ENpcCppProfileStage stage, const u64 qpc_delta)
{
    if (!enabled())
        return;

    NpcCppProfileCounters& counters = g_npc_cpp_profile_counters[to_index(stage)];
    counters.total_qpc.fetch_add(qpc_delta, std::memory_order_relaxed);
    counters.calls.fetch_add(1, std::memory_order_relaxed);
    update_max(counters.max_qpc, qpc_delta);
    const unsigned long long add_calls = g_npc_cpp_profile_total_add_calls.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!g_npc_cpp_profile_logged_first_add.exchange(true, std::memory_order_relaxed))
    {
        Msg("* npc_cpp_profile debug first_add stage=%s qpc=%llu now=%u",
            g_npc_cpp_profile_stage_names[to_index(stage)],
            static_cast<unsigned long long>(qpc_delta), Device.dwTimeGlobal);
    }

    if ((add_calls % 50000ull) == 0)
    {
        Msg("* npc_cpp_profile debug add_milestone add_calls=%llu stage=%s now=%u",
            add_calls, g_npc_cpp_profile_stage_names[to_index(stage)], Device.dwTimeGlobal);
    }

    flush_if_needed();
}

void npc_cpp_profile::add_script_evaluator(pcstr evaluator_name, const u64 qpc_delta)
{
    if (!enabled())
        return;

    const pcstr resolved_name = (evaluator_name && evaluator_name[0]) ? evaluator_name : "<unnamed>";
    ScriptEvaluatorProfileBucket* bucket = script_evaluator_bucket(resolved_name);

    if (!bucket)
    {
        add(ENpcCppProfileStage::ScriptEvaluatorEvaluate, qpc_delta);
        return;
    }

    bucket->total_qpc.fetch_add(qpc_delta, std::memory_order_relaxed);
    bucket->calls.fetch_add(1, std::memory_order_relaxed);
    update_max(bucket->max_qpc, qpc_delta);
    flush_if_needed();
}

void npc_cpp_profile::add_script_action(pcstr action_name, pcstr step, const u64 qpc_delta)
{
    if (!enabled())
        return;
    string128 name; // a long action name is cut, not a crash (xr_sprintf checks the size outside MASTER_GOLD)
    std::snprintf(name, sizeof name, "%s/%s", action_name && action_name[0] ? action_name : "<unnamed>", step ? step : "?");
    ScriptEvaluatorProfileBucket* bucket = named_bucket(g_script_action_profile_buckets, name);
    if (!bucket)
        return; // the probe window is full: the stage totals still have it
    bucket->total_qpc.fetch_add(qpc_delta, std::memory_order_relaxed);
    bucket->calls.fetch_add(1, std::memory_order_relaxed);
    update_max(bucket->max_qpc, qpc_delta);
    flush_if_needed();
}

void npc_cpp_profile::add_script_evaluator_cache_hit(pcstr evaluator_name)
{
    if (!enabled())
        return;

    const pcstr resolved_name = (evaluator_name && evaluator_name[0]) ? evaluator_name : "<unnamed>";
    ScriptEvaluatorProfileBucket* bucket = script_evaluator_bucket(resolved_name);

    if (!bucket)
        return;

    bucket->cache_hits.fetch_add(1, std::memory_order_relaxed);
    flush_if_needed();
}

void npc_cpp_profile::add_script_evaluator_cache_miss(pcstr evaluator_name)
{
    if (!enabled())
        return;

    const pcstr resolved_name = (evaluator_name && evaluator_name[0]) ? evaluator_name : "<unnamed>";
    ScriptEvaluatorProfileBucket* bucket = script_evaluator_bucket(resolved_name);

    if (!bucket)
        return;

    bucket->cache_misses.fetch_add(1, std::memory_order_relaxed);
    flush_if_needed();
}

ScopedNpcCppProfile::ScopedNpcCppProfile(const ENpcCppProfileStage stage)
    : m_stage(stage), m_start_qpc(0), m_enabled(npc_cpp_profile::enabled())
{
    if (m_enabled)
        m_start_qpc = CPU::QPC();
}

ScopedNpcCppProfile::~ScopedNpcCppProfile()
{
    if (!m_enabled)
        return;

    npc_cpp_profile::add(m_stage, CPU::QPC() - m_start_qpc);
}
