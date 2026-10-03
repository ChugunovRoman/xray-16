////////////////////////////////////////////////////////////////////////////
//	Module 		: script_property_evaluator_wrapper.cpp
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script property evaluator wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_property_evaluator_wrapper.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "xrScriptEngine/script_engine.hpp"
#include "npc_cpp_profile.h"
#include "performance_cvars.h"
#include "addon_event_bus.h"
#include "xrAICore/Components/ai_planner_search_limits.h"

// Policies read from configs/npc_perf_evaluator_cache.ltx. The file only overrides the table below, so the
// behaviour without it is exactly the built-in one. Reloaded by the console command ai_evaluator_cache_reload,
// which is what makes tuning a one-session experiment instead of a rebuild per variant.
namespace
{
// A policy plus, for CacheForTTL, the lifetime this evaluator asked for. ttl_ms == 0 means "take the
// ai_evaluator_ttl_ms cvar", which is what the built-in table does.
struct CachePolicySetting
{
    EScriptEvaluatorCachePolicy policy = EScriptEvaluatorCachePolicy::CachePerSolve;
    u32 ttl_ms = 0;
};

using PolicyMap = xr_map<shared_str, CachePolicySetting>;
PolicyMap* g_policy_overrides = nullptr;
bool g_policy_overrides_loaded = false;

// A number is a lifetime in milliseconds for this evaluator alone ("meet_contact = 1500"); 0 turns the cache
// off for it. Words keep their meaning, and "ttl" takes the lifetime from the cvar.
bool parse_cache_policy(pcstr text, CachePolicySetting& out)
{
    if (!text || !text[0])
        return false;
    if (text[0] >= '0' && text[0] <= '9')
    {
        const int ms = atoi(text);
        if (ms < 0)
            return false;
        out.ttl_ms = static_cast<u32>(ms);
        out.policy = ms > 0 ? EScriptEvaluatorCachePolicy::CacheForTTL : EScriptEvaluatorCachePolicy::NeverCache;
        return true;
    }
    out.ttl_ms = 0;
    EScriptEvaluatorCachePolicy& policy = out.policy;
    if (xr_strcmp(text, "never") == 0)
        policy = EScriptEvaluatorCachePolicy::NeverCache;
    else if (xr_strcmp(text, "frame") == 0)
        policy = EScriptEvaluatorCachePolicy::CachePerFrame;
    else if (xr_strcmp(text, "frame10") == 0)
        policy = EScriptEvaluatorCachePolicy::CacheFor10Frames;
    else if (xr_strcmp(text, "frame30") == 0)
        policy = EScriptEvaluatorCachePolicy::CacheFor30Frames;
    else if (xr_strcmp(text, "solve") == 0)
        policy = EScriptEvaluatorCachePolicy::CachePerSolve;
    else if (xr_strcmp(text, "ttl") == 0)
        policy = EScriptEvaluatorCachePolicy::CacheForTTL;
    else
        return false;
    return true;
}

pcstr cache_policy_name(EScriptEvaluatorCachePolicy policy)
{
    switch (policy)
    {
    case EScriptEvaluatorCachePolicy::NeverCache: return "never";
    case EScriptEvaluatorCachePolicy::CachePerFrame: return "frame";
    case EScriptEvaluatorCachePolicy::CacheFor10Frames: return "frame10";
    case EScriptEvaluatorCachePolicy::CacheFor30Frames: return "frame30";
    case EScriptEvaluatorCachePolicy::CachePerSolve: return "solve";
    case EScriptEvaluatorCachePolicy::CacheForTTL: return "ttl";
    default: return "?";
    }
}

void load_policy_overrides()
{
    g_policy_overrides_loaded = true;
    if (!g_policy_overrides)
        g_policy_overrides = xr_new<PolicyMap>();
    g_policy_overrides->clear();

    string_path path;
    FS.update_path(path, "$game_config$", "npc_perf_evaluator_cache.ltx");
    if (!FS.exist(path, FSType::Any))
        return; // no file: the built-in table stands

    CInifile ini(path, true /*read only*/);
    if (!ini.section_exist("evaluator_cache"))
        return;

    const CInifile::Sect& section = ini.r_section("evaluator_cache");
    for (const CInifile::Item& item : section.Data)
    {
        CachePolicySetting setting;
        if (parse_cache_policy(item.second.c_str(), setting))
            g_policy_overrides->emplace(item.first, setting);
        else
            Msg("! [evaluator_cache] '%s = %s': expected a number of milliseconds or "
                "never|frame|frame10|frame30|solve|ttl",
                item.first.c_str(), item.second.c_str());
    }
    Msg("* [evaluator_cache] %u override(s) from %s", static_cast<u32>(g_policy_overrides->size()), path);
}
} // namespace

u32 g_ai_evaluator_cache_generation = 1;

void ai_evaluator_cache_reload()
{
    load_policy_overrides();
    ++g_ai_evaluator_cache_generation; // every evaluator re-reads its policy and drops the value it cached
    // The native evaluators of plugins keep their own cache and read the same file: they re-read it on this event
    namespace events = gw::addons::events;
    static const GwpEventId reload_event = [] {
        events::Declare("ai_evaluator_cache_on_reload", 0, "");
        return events::Intern("ai_evaluator_cache_on_reload");
    }();
    events::Emit(reload_event);
}

void ai_evaluator_cache_print()
{
    if (!g_policy_overrides_loaded)
        load_policy_overrides();
    Msg("- [evaluator_cache] ttl %u ms, solve cache %s, %u override(s):", ai_evaluator_ttl_ms,
        ai_evaluator_solve_cache ? "on" : "off",
        g_policy_overrides ? static_cast<u32>(g_policy_overrides->size()) : 0u);
    if (g_policy_overrides)
    {
        for (const auto& [name, setting] : *g_policy_overrides)
        {
            if (setting.ttl_ms)
                Msg("-   %-40s %s %u ms", name.c_str(), cache_policy_name(setting.policy), setting.ttl_ms);
            else
                Msg("-   %-40s %s", name.c_str(), cache_policy_name(setting.policy));
        }
    }
    Msg("- [evaluator_cache] the rest keeps the built-in policy; reload the file with ai_evaluator_cache_reload");
}

static CachePolicySetting get_script_evaluator_cache_policy(pcstr evaluator_name)
{
    CachePolicySetting result;
    if (!evaluator_name || !evaluator_name[0])
    {
        result.policy = EScriptEvaluatorCachePolicy::NeverCache;
        return result;
    }

    if (!g_policy_overrides_loaded)
        load_policy_overrides();
    if (g_policy_overrides)
    {
        const auto it = g_policy_overrides->find(evaluator_name);
        if (it != g_policy_overrides->end())
            return it->second;
    }

    // --- CachePerFrame: быстро меняющиеся состояния (danger, combat, weapon state) ---
    // state_mgr weapon evaluators
    if (xr_strcmp(evaluator_name, "state_mgr_logic_active") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_in_smartcover") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_locked") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_locked") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_none_now") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_strapped_now") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_unstrapped_now") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_strapped") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_unstrapped") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_none") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_drop") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_fire") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    // combat/danger evaluators: меняются per-frame (выстрел, попадание)
    // но safe для per-frame cache: плановщик вычисляет их много раз за одну итерацию solverа
    if (xr_strcmp(evaluator_name, "danger") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "script_danger") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "evaluator_combat_enemy") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_mental_danger_now") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_end") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_direction") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };
    if (xr_strcmp(evaluator_name, "state_mgr_weapon") == 0)
        return { EScriptEvaluatorCachePolicy::CachePerFrame, 0 };



    // --- CacheForTTL: stable / slow-changing evaluators cached by real time (Device.dwTimeGlobal).
    // Controlled by ai_evaluator_ttl_ms. These are heavy Lua scans (~40-75us) called every solve.
    // Combat preempts most of them, so a small delay is imperceptible for non-combat behavior.
    if (xr_strcmp(evaluator_name, "corpse_exist") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eva_gather_itm") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "meet_contact") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eva_kill_wounded") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eva_dont_shoot") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "evaluator_abuse") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eval_turn_on_campfire") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eva_npc_vs_box") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eva_npc_vs_heli") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };
    if (xr_strcmp(evaluator_name, "eva_radio_in_heli") == 0)
        return { EScriptEvaluatorCachePolicy::CacheForTTL, 0 };

    // Default: cache for the duration of one GOAP solve. The world is frozen during a synchronous
    // solve (actions execute after planning, not during), so reusing an evaluator's value across the
    // solve cannot go stale. Collapses redundant Lua calls (actual() + Search re-evaluate the same
    // conditions). Disable via dev cvar ai_evaluator_solve_cache (reverts these to NeverCache).
    return { EScriptEvaluatorCachePolicy::CachePerSolve, 0 };
}

EScriptEvaluatorCachePolicy CScriptPropertyEvaluatorWrapper::cache_policy() const
{
    if (!m_cache_policy_initialized || m_cache_policy_generation != g_ai_evaluator_cache_generation)
    {
        const CachePolicySetting setting = get_script_evaluator_cache_policy(m_evaluator_name);
        m_cache_policy = setting.policy;
        m_cache_ttl_ms = setting.ttl_ms;
        m_cache_policy_generation = g_ai_evaluator_cache_generation;
        m_cache_policy_initialized = true;
        m_has_cached_value = false; // the value was cached under the previous policy
    }

    return m_cache_policy;
}

void CScriptPropertyEvaluatorWrapper::setup(CScriptGameObject* object, CPropertyStorage* storage)
{
    luabind::call_member<void>(this, "setup", object, storage);
}

void CScriptPropertyEvaluatorWrapper::setup_static(
    CScriptPropertyEvaluator* evaluator, CScriptGameObject* object, CPropertyStorage* storage)
{
    evaluator->CScriptPropertyEvaluator::setup(object, storage);
}

bool CScriptPropertyEvaluatorWrapper::evaluate()
{
    PROPERTY_EVALUATOR_TRACY_ZONE_SCRIPT();
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptEvaluatorEvaluate);
    const u64 evaluator_start_qpc = npc_cpp_profile::enabled() ? CPU::QPC() : 0;
    EScriptEvaluatorCachePolicy policy = cache_policy();
    // Dev kill-switch: revert the per-solve default to NeverCache for A/B. Whitelisted per-frame
    // policies are pre-existing and not gated by this switch.
    if (policy == EScriptEvaluatorCachePolicy::CachePerSolve && ai_evaluator_solve_cache == 0)
        policy = EScriptEvaluatorCachePolicy::NeverCache;
    // Dev kill-switch for TTL: if ai_evaluator_ttl_ms == 0, fall back to per-solve caching.
    if (policy == EScriptEvaluatorCachePolicy::CacheForTTL && ai_evaluator_ttl_ms == 0)
        policy = EScriptEvaluatorCachePolicy::CachePerSolve;

    const u32 current_frame = Device.dwFrame;
    const u32 current_time_ms = Device.dwTimeGlobal;
    const bool use_cache = policy != EScriptEvaluatorCachePolicy::NeverCache;

    // Cache validity: CachePerSolve compares the solve epoch (world frozen during one solve);
    // CachePerFrame/10/30 compare the frame number with a tolerance;
    // CacheForTTL compares real time via Device.dwTimeGlobal.
    bool cache_valid = false;
    if (use_cache && m_has_cached_value)
    {
        if (policy == EScriptEvaluatorCachePolicy::CachePerSolve)
            cache_valid = (m_cached_epoch == g_ai_evaluator_solve_epoch);
        else if (policy == EScriptEvaluatorCachePolicy::CacheForTTL)
        {
            // Own lifetime from the config, or the shared cvar when the config did not name one.
            const u32 ttl_ms = m_cache_ttl_ms ? m_cache_ttl_ms : static_cast<u32>(ai_evaluator_ttl_ms);
            cache_valid = ((current_time_ms - m_cached_time_ms) <= ttl_ms);
        }
        else
        {
            u32 cache_frame_tolerance = 0; // CachePerFrame
            if (policy == EScriptEvaluatorCachePolicy::CacheFor10Frames)
                cache_frame_tolerance = 10;
            else if (policy == EScriptEvaluatorCachePolicy::CacheFor30Frames)
                cache_frame_tolerance = 30;
            cache_valid = (current_frame - m_cached_frame) <= cache_frame_tolerance;
        }
    }

    if (cache_valid)
    {
        npc_cpp_profile::add_script_evaluator_cache_hit(m_evaluator_name);
        if (evaluator_start_qpc)
            npc_cpp_profile::add_script_evaluator(m_evaluator_name, CPU::QPC() - evaluator_start_qpc);
        return m_cached_value;
    }

    if (use_cache)
        npc_cpp_profile::add_script_evaluator_cache_miss(m_evaluator_name);

    bool result = false;
    if (call_lua_evaluate(result) && use_cache)
    {
        m_cached_frame = current_frame;
        m_cached_epoch = g_ai_evaluator_solve_epoch;
        m_cached_time_ms = current_time_ms;
        m_cached_value = result;
        m_has_cached_value = true;
    }

    if (evaluator_start_qpc)
        npc_cpp_profile::add_script_evaluator(m_evaluator_name, CPU::QPC() - evaluator_start_qpc);
    return result;
}

bool CScriptPropertyEvaluatorWrapper::evaluate_uncached()
{
    PROPERTY_EVALUATOR_TRACY_ZONE_SCRIPT();
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptEvaluatorEvaluate);
    const u64 evaluator_start_qpc = npc_cpp_profile::enabled() ? CPU::QPC() : 0;
    bool result = false;
    call_lua_evaluate(result);
    if (evaluator_start_qpc)
        npc_cpp_profile::add_script_evaluator(m_evaluator_name, CPU::QPC() - evaluator_start_qpc);
    return result;
}

// False: the Lua method failed (logged), result stays false and is not cached
bool CScriptPropertyEvaluatorWrapper::call_lua_evaluate(bool& result)
{
    try
    {
        result = luabind::call_member<bool>(this, "evaluate");
        return true;
    }
#if defined(DEBUG) && !defined(LUABIND_NO_EXCEPTIONS)
    catch (const luabind::cast_failed& exception)
    {
#ifdef LOG_ACTION
        GEnv.ScriptEngine->script_log(LuaMessageType::Error,
            "SCRIPT RUNTIME ERROR : evaluator [%s] returns value with not a %s type!", m_evaluator_name,
            exception.info().name());
#else
        GEnv.ScriptEngine->script_log(LuaMessageType::Error,
            "SCRIPT RUNTIME ERROR : evaluator [%s] returns value with not a %s type!", exception.info().name());
#endif
    }
#endif
    catch (...)
    {
        //Alundaio: m_evaluator_name
        GEnv.ScriptEngine->script_log(
            LuaMessageType::Error, "SCRIPT RUNTIME ERROR : evaluator [%s] returns value with not a bool type!", m_evaluator_name);
    }

    result = false;
    return false;
}

bool CScriptPropertyEvaluatorWrapper::evaluate_static(CScriptPropertyEvaluator* evaluator)
{
    return (evaluator->CScriptPropertyEvaluator::evaluate());
}
