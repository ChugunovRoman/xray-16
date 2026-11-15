////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager_update.cpp
//	Created 	: 25.02.2003
//  Modified 	: 13.12.2006
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager update cycle
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "stalker_animation_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "game_object_space.h"
#include "xrScriptEngine/script_callback_ex.h"
#include "xrEngine/profiler.h"
#include "stalker_movement_manager_smart_cover.h"
#include "addon_api_npc_control.h"
#include "stalker_animation_data_storage.h"
#include "stalker_animation_data.h"

/*IC*/ void CStalkerAnimationManager::play_delayed_callbacks() // XXX: can't compile Release because of "inline"
{
    if (m_call_script_callback)
    {
        m_call_script_callback = false;
        object().callback(GameObject::eScriptAnimation)();
        // Plugins: npc_on_script_animation_end, after the Lua callback (Plugin API group npc_anim)
        gw::addons::npcctl::ScriptAnimationEnd(object());
        return;
    }

    if (m_call_global_callback)
    {
        m_call_global_callback = false;
        if (m_global_callback)
            m_global_callback();
        return;
    }
}

IC bool CStalkerAnimationManager::script_callback() const
{
    if (script_animations().empty())
        return (false);

    // A plugin waiting for the end of script animations needs the tracks updated as a Lua callback does
    return (object().callback(GameObject::eScriptAnimation) || gw::addons::npcctl::WantsScriptAnimationEnd());
}

IC bool CStalkerAnimationManager::need_update() const
{
    if (script_callback())
        return (true);

    return (non_script_need_update());
}

IC void CStalkerAnimationManager::update_tracks()
{
    if (!need_update())
        return;

    m_skeleton_animated->UpdateTracks();
}

#ifdef USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_script_impl()
{
    clear_unsafe_callbacks();
    global().reset();
    torso().reset();
    legs().reset();

    const CStalkerAnimationScript& selected = assign_script_animation();
    script().animation(selected.animation());
    if (selected.use_movement_controller())
    {
        script().target_matrix(selected.transform(object()));
        if (m_start_new_script_animation)
        {
            m_start_new_script_animation = false;
            if (selected.has_transform() && object().animation_movement())
                object().destroy_anim_mov_ctrl();
        }
    }

    script().play(m_skeleton_animated, script_play_callback, selected.use_movement_controller(),
        selected.local_animation(), false, m_script_bone_part_mask);

    head().animation(assign_head_animation());
    head().play(m_skeleton_animated, head_play_callback, false, false);
}
#else // USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_script_impl()
{
    clear_unsafe_callbacks();
    global().reset();
    head().reset();
    torso().reset();
    legs().reset();

    const CStalkerAnimationScript& selected = assign_script_animation();
    script().animation(selected.animation());
    script().play(m_skeleton_animated, script_play_callback, selected.use_movement_controller(),
        selected.local_animation(), false, m_script_bone_part_mask);
}
#endif // USE_HEAD_BONE_PART_FAKE

bool CStalkerAnimationManager::play_script()
{
    if (script_animations().empty())
    {
        m_start_new_script_animation = false;
        script().reset();
        return (false);
    }

    play_script_impl();

    return (true);
}

#ifdef USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_global_impl(const MotionID& animation, bool const& animation_movement_controller)
{
    torso().reset();
    legs().reset();

    global().animation(animation);
    global().play(m_skeleton_animated, global_play_callback, animation_movement_controller, true, false,
        m_script_bone_part_mask, true);

    if (m_global_modifier)
        m_global_modifier(global().blend());

    head().animation(assign_head_animation());
    head().play(m_skeleton_animated, head_play_callback, false, false);
}
#else // USE_HEAD_BONE_PART_FAKE
IC void CStalkerAnimationManager::play_global_impl(const MotionID& animation, bool const& animation_movement_controller)
{
    head().reset();
    torso().reset();
    legs().reset();

    global().animation(animation);
    global().play(m_skeleton_animated, global_play_callback, false, false, false);
}
#endif // USE_HEAD_BONE_PART_FAKE

bool CStalkerAnimationManager::play_global()
{
    bool animation_movement_controller = false;
    const MotionID& global_animation = assign_global_animation(animation_movement_controller);
    if (!global_animation)
    {
        clear_unsafe_callbacks();
        global().reset();
        return (false);
    }

    play_global_impl(global_animation, animation_movement_controller);

    return (true);
}

IC void CStalkerAnimationManager::play_head()
{
    head().animation(assign_head_animation());
    head().play(m_skeleton_animated, head_play_callback, false, false);
}

IC void CStalkerAnimationManager::play_torso()
{
    torso().animation(assign_torso_animation());
    torso().play(m_skeleton_animated, torso_play_callback, false, false);
}

void CStalkerAnimationManager::play_legs()
{
    float speed = 0.f;
    bool first_time = !legs().animation();
    bool result = legs().animation(assign_legs_animation());

    if (!first_time && !result && legs().blend())
    {
        float amount = legs().blend()->blendAmount;
        m_previous_speed = (m_target_speed - m_previous_speed) * amount + m_previous_speed;
    }

    legs().play(m_skeleton_animated, legs_play_callback, false, false, !fis_zero(m_target_speed));

    if (result && legs().blend())
    {
        float amount = legs().blend()->blendAmount;
        speed = (m_target_speed - m_previous_speed) * amount + m_previous_speed;
    }

    if (fis_zero(speed))
        return;

    if (!legs().blend())
        return;

    object().movement().setup_speed_from_animation(speed);
}

void CStalkerAnimationManager::update_impl()
{
    if (!object().g_Alive())
        return;

    update_tracks();
    play_delayed_callbacks();

    if (play_script())
        return;

    if (play_global())
        return;

    play_head();
    play_torso();
    play_legs();

    torso().synchronize(m_skeleton_animated, m_legs);
}

void CStalkerAnimationManager::update()
{
    START_PROFILE("stalker/client_update/animations")
    try
    {
        update_impl();
    }
    catch (...)
    {
        Msg("! error in stalker with visual %s and ID %u", object().cNameVisual().c_str(), object().ID());
        /* avo: prevent game from crashing */
        head().reset();
        torso().reset();
        legs().reset();
        global().reset();
        return;
        // throw;
        /* avo: end */
    }
    STOP_PROFILE
}

// The model of a live stalker was replaced (CAI_Stalker::ChangeVisual, plans/addon_companion_extended/npc_visual.md).
// Why the model "rolled into a ball" before: the pairs keep the blends of the old model (deleted with it) and stay
// "actual" - the next update sees the same animation ids and plays nothing, the new model has no blends at all and its
// bones are not posed. Here the pairs drop the old blends, the model, its animation data and the bone callbacks are
// taken again, and the current animations start on the new model at once, so it is never drawn without blends.
void CStalkerAnimationManager::on_visual_changed()
{
    const CStalkerAnimationData* const previous_storage = m_data_storage;
    // The kind of the head / shoulder / spine callbacks (the blend kind is set by smart cover transitions and the
    // sight manager): put back on the new bones as it was
    const bool forward_blend = forward_blend_callbacks();
    const bool backward_blend = backward_blend_callbacks();

    // Not reinit(): the callbacks of the pairs (smart cover, script waits), the crouch state and the script queue stay
    global().reset();
    head().reset();
    torso().reset();
    legs().reset();
    script().reset();

    m_visual = object().Visual();
    m_skeleton_animated = smart_cast<IKinematicsAnimated*>(m_visual);
    VERIFY(m_skeleton_animated);
    m_data_storage = stalker_animation_data_storage().object(m_skeleton_animated);
    VERIFY(m_data_storage);

    // The ids of the queued script animations index the motion slots of the old model: still valid only with the same
    // animation sets (the same storage entry). Otherwise dropped - the Lua state manager queues them again
    if (m_data_storage != previous_storage && !m_script_animations.empty())
    {
        Msg("! [visual] stalker [%s]: the model [%s] has other animation sets, %u queued script animations dropped",
            object().cName().c_str(), object().cNameVisual().c_str(), u32(m_script_animations.size()));
        m_script_animations.clear();
    }

    if (!object().g_Alive())
        return;

#ifdef USE_HEAD_BONE_PART_FAKE
    VERIFY(!m_data_storage->m_head_animations.A.empty());
    const u16 bone_part =
        m_skeleton_animated->LL_GetMotionDef(m_data_storage->m_head_animations.A.front())->bone_or_part;
    VERIFY(bone_part != BI_NONE);
    m_script_bone_part_mask = CStalkerAnimationPair::all_bone_parts ^ (1 << bone_part);
#endif

    if (forward_blend)
        assign_bone_blend_callbacks(true);
    else if (backward_blend)
        assign_bone_blend_callbacks(false);
    else
        assign_bone_callbacks();

    // As update_impl, without update_tracks and play_delayed_callbacks: those call Lua and stay in the update of the
    // object (this may run inside a Lua call)
    try
    {
        if (play_script())
            return;

        if (play_global())
            return;

        play_head();
        play_torso();
        play_legs();

        torso().synchronize(m_skeleton_animated, m_legs);
    }
    catch (...)
    {
        // As update(): the next update starts the animations again
        Msg("! [visual] stalker [%s]: cannot start the animations on the model [%s]", object().cName().c_str(),
            object().cNameVisual().c_str());
        head().reset();
        torso().reset();
        legs().reset();
        global().reset();
    }
}
