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

static EScriptEvaluatorCachePolicy get_script_evaluator_cache_policy(pcstr evaluator_name)
{
    if (!evaluator_name || !evaluator_name[0])
        return EScriptEvaluatorCachePolicy::NeverCache;

    // --- CachePerFrame: быстро меняющиеся состояния (danger, combat, weapon state) ---
    // state_mgr weapon evaluators
    if (xr_strcmp(evaluator_name, "state_mgr_logic_active") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_in_smartcover") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_locked") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_locked") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_none_now") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_strapped_now") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_unstrapped_now") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_strapped") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_unstrapped") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_none") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_drop") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon_fire") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    // combat/danger evaluators: меняются per-frame (выстрел, попадание)
    // но safe для per-frame cache: плановщик вычисляет их много раз за одну итерацию solverа
    if (xr_strcmp(evaluator_name, "danger") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "script_danger") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "evaluator_combat_enemy") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_mental_danger_now") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_end") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_direction") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;
    if (xr_strcmp(evaluator_name, "state_mgr_weapon") == 0)
        return EScriptEvaluatorCachePolicy::CachePerFrame;

    // --- CacheFor10Frames: умеренно стабильные (~330ms @ 30fps) ---
    // kill_wounded проверяет живых раненых рядом, меняется при смерти/убегании
    if (xr_strcmp(evaluator_name, "eva_kill_wounded") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor10Frames;
    // dont_shoot проверяется часто, обычно стабильно
    if (xr_strcmp(evaluator_name, "eva_dont_shoot") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor10Frames;
    // abuse проверка тоже редко меняется
    if (xr_strcmp(evaluator_name, "evaluator_abuse") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor10Frames;

    // --- CacheFor30Frames: редко меняющиеся состояния (~1sec @ 30fps) ---
    // campfire state меняется очень редко
    if (xr_strcmp(evaluator_name, "eval_turn_on_campfire") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor30Frames;
    // npc_vs_box, npc_vs_heli, radio_in_heli — ситуации которые устанавливаются скриптом
    if (xr_strcmp(evaluator_name, "eva_npc_vs_box") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor30Frames;
    if (xr_strcmp(evaluator_name, "eva_npc_vs_heli") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor30Frames;
    if (xr_strcmp(evaluator_name, "eva_radio_in_heli") == 0)
        return EScriptEvaluatorCachePolicy::CacheFor30Frames;

    return EScriptEvaluatorCachePolicy::NeverCache;
}

EScriptEvaluatorCachePolicy CScriptPropertyEvaluatorWrapper::cache_policy() const
{
    if (!m_cache_policy_initialized)
    {
        m_cache_policy = get_script_evaluator_cache_policy(m_evaluator_name);
        m_cache_policy_initialized = true;
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
    const EScriptEvaluatorCachePolicy policy = cache_policy();
    const u32 current_frame = Device.dwFrame;

    // Вычисляем допустимую "полосу" кеша в зависимости от политики
    u32 cache_frame_tolerance = 0;
    if (policy == EScriptEvaluatorCachePolicy::CachePerFrame)
        cache_frame_tolerance = 0; // только текущий кадр
    else if (policy == EScriptEvaluatorCachePolicy::CacheFor10Frames)
        cache_frame_tolerance = 10;
    else if (policy == EScriptEvaluatorCachePolicy::CacheFor30Frames)
        cache_frame_tolerance = 30;

    const bool use_cache = policy != EScriptEvaluatorCachePolicy::NeverCache;

    if (use_cache && m_has_cached_value && (current_frame - m_cached_frame) <= cache_frame_tolerance)
    {
        npc_cpp_profile::add_script_evaluator_cache_hit(m_evaluator_name);
        if (evaluator_start_qpc)
            npc_cpp_profile::add_script_evaluator(m_evaluator_name, CPU::QPC() - evaluator_start_qpc);
        return m_cached_value;
    }

    if (use_cache)
        npc_cpp_profile::add_script_evaluator_cache_miss(m_evaluator_name);

    try
    {
        const bool result = (luabind::call_member<bool>(this, "evaluate"));

        if (use_cache)
        {
            m_cached_frame = current_frame;
            m_cached_value = result;
            m_has_cached_value = true;
        }

        if (evaluator_start_qpc)
            npc_cpp_profile::add_script_evaluator(m_evaluator_name, CPU::QPC() - evaluator_start_qpc);
        return result;
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

    if (evaluator_start_qpc)
        npc_cpp_profile::add_script_evaluator(m_evaluator_name, CPU::QPC() - evaluator_start_qpc);

    return (false);
}

bool CScriptPropertyEvaluatorWrapper::evaluate_static(CScriptPropertyEvaluator* evaluator)
{
    return (evaluator->CScriptPropertyEvaluator::evaluate());
}
