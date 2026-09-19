#pragma once
#include "firedeps.h"

#include "Include/xrRender/Kinematics.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "actor_defs.h"

class player_hud;
class CHudItem;
class CMotionDef;

struct motion_descr
{
    MotionID mid;
    shared_str name;
};

struct player_hud_motion
{
    shared_str m_base_name;
    shared_str m_additional_name;
    xr_vector<motion_descr> m_animations;
    float m_anim_speed;
};

struct player_hud_motion_container
{
    xr_unordered_map<shared_str, player_hud_motion> m_anims;

    [[nodiscard]]
    const player_hud_motion* find_motion(const shared_str& name) const;

    void load(IKinematicsAnimated* model, const shared_str& sect);
};

struct hud_item_measures
{
    enum
    {
        e_fire_point = (1 << 0),
        e_fire_point2 = (1 << 1),
        e_shell_point = (1 << 2),
        e_16x9_mode_now = (1 << 3)
    };

    Fvector m_hands_offset[2][5]{}; // pos,rot/ normal,aim,GL,correct,alt_correct
    Fvector m_hands_attach[2]{}; // pos,rot
    Fvector m_item_attach[2]{}; // pos,rot

    Fvector m_fire_point_offset{};
    Fvector m_fire_point2_offset{};
    Fvector m_shell_point_offset{};

    u16 m_fire_bone;
    u16 m_fire_bone2;
    u16 m_shell_bone;
    Flags8 m_prop_flags;

    Fmatrix load(const shared_str& sect_name, IKinematics* K);
    Fmatrix load_monolithic(const shared_str& sect_name, IKinematics* K, CHudItem* owner);
    void load_inertion_params(const shared_str& sect_name);
    void update(Fmatrix& attach_offset);

    struct inertion_params
    {
        float m_pitch_offset_r;
        float m_pitch_offset_n;
        float m_pitch_offset_d;
        float m_pitch_low_limit;
        float m_origin_offset;
        float m_origin_offset_aim;
        float m_tendto_speed;
        float m_tendto_speed_aim;
        float m_tendto_ret_speed;
        float m_tendto_ret_speed_aim;

        float m_min_angle;
        float m_min_angle_aim;

        Fvector4 m_offset_LRUD;
        Fvector4 m_offset_LRUD_aim;
    };
    inertion_params m_inertion_params; //--#SM+#--
};

struct attachable_hud_item
{
    player_hud* m_parent{};
    CHudItem* m_parent_hud_item{};
    shared_str m_sect_name;
    shared_str m_visual_name;
    IKinematics* m_model{};
    IKinematics* m_model_2{}; // static first frame of "anm_idle_aim" (aim rest pose)
    IKinematics* m_model_3{}; // static first frame of "anm_idle" (hip rest pose)
    u16 m_attach_place_idx{};
    bool m_monolithic{};
    hud_item_measures m_measures;

    // runtime positioning
    Fmatrix m_attach_offset{};
    // Animated (real) weapon transform of the current frame.
    Fmatrix m_item_transform{};
    // Rest transform built on the static "anm_idle_aim" hands pose, without the zoom offset.
    // Used by the addon aim-offset solver.
    Fmatrix hud_transform{};
    // Rest transform: zoom (aim) offset applied, hands anchor blended between the static hip
    // and aim idle poses, without any animation-driven motion (sway, inertion, strafe).
    // Reference pose for the laser dot solver.
    Fmatrix m_item_rest_transform{};

    player_hud_motion_container m_hand_motions;

    attachable_hud_item(player_hud* parent, const shared_str& sect_name, IKinematicsAnimated* model);
    ~attachable_hud_item();

    // bDiscard=true drops pool/base refs so the next model_Create can reload mesh from disk (ini hot reload).
    void destroy_render_models(bool bDiscard);

    void reload_measures();
    void calc_addon_aim_offset();
    // Re-primes both static rest poses (aim and hip). Must be called whenever the item
    // animations or the attached addons change, otherwise the laser dot reference goes stale.
    void set_rest_poses();
    void set_idle_anm_for_second_model();
    void set_idle_anm_for_third_model();
    // Freezes handsModel/itemModel on the first frame of preferred_anim (fallback: anm_idle,
    // anm_idle_0) so they can be sampled as a rest pose.
    void set_static_idle_pose(IKinematics* itemModel, IKinematicsAnimated* handsModel, pcstr preferred_anim);
    // Weapon bone transform of the rest pose: static hip and aim idle poses blended by
    // zoom_factor. Falls back to the animated model when the rest models are missing.
    void rest_bone_transform(u16 bone_id, float zoom_factor, Fmatrix& dst) const;

    void update(bool bForce);
    void update_hud_additional(Fmatrix& trans) const;

    void setup_firedeps(firedeps& fd);

    void render(u32 context_id, IRenderable* root);
    void render_item_ui() const;
    bool render_item_ui_query() const;
    bool need_renderable() const;
    void set_bone_visible(const shared_str& bone_name, BOOL bVisibility, BOOL bSilent = FALSE);

    // hands bind position
    Fvector& hands_attach_pos();
    Fvector& hands_attach_rot();

    // hands runtime offset
    Fvector& hands_offset_pos();
    Fvector& hands_offset_rot();

    // props
    u32 m_upd_firedeps_frame{ u32(-1) };
    void tune(Ivector values);
    u32 anim_play(const shared_str& anim_name, BOOL bMixIn, const CMotionDef*& md, u8& rnd);
    // handsModel/itemModel receive the motion; bStaticPose freezes it on the first frame
    // instead of playing it (used to build the rest poses).
    u32 anim_play(const shared_str& anim_name, BOOL bMixIn, const CMotionDef*& md, u8& rnd, IKinematics* itemModel,
        IKinematicsAnimated* handsModel, bool bStaticPose);
};

class player_hud
{
public:
    player_hud() = default;
    ~player_hud();
    void load(const shared_str& model_name);
    void load_default() { load("actor_hud_05"); };
    void update(const Fmatrix& trans);
    void render_hud(u32 context_id, IRenderable* root);
    void render_item_ui() const;
    bool render_item_ui_query() const;
    u32 anim_play(u16 part, const MotionID& M, BOOL bMixIn, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel);
    u32 anim_play(u16 part, const MotionID& M, BOOL bMixIn, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel, IKinematicsAnimated* model, bool bStaticPose);
    const shared_str& section_name() const { return m_sect_name; }
    attachable_hud_item* create_hud_item(const shared_str& sect);

    void hide_detector();
    void set_detector_state(const u32 state);
    void attach_item(CHudItem* item);
    bool allow_activation(CHudItem* item) const;
    attachable_hud_item* attached_item() { return m_attached_item; };
    void after_detach_item_idx(u16 idx);
    void after_detach_item_idx(CHudItem* item);
    void detach_item_idx();
    void detach_item(CHudItem* item);
    void hot_reload_attached_weapon_hud(CHudItem* item);
    void detach_all_items()
    {
        m_attached_item = NULL;
    };

    // result  - animated weapon transform, result2 - aim idle rest transform,
    // result3 - blended rest transform used as the laser dot reference.
    void calc_transform(u16 attach_slot_idx, const Fmatrix& offset, Fmatrix& result, Fmatrix& result2, Fmatrix& result3) const;
    void tune(Ivector values);
    u32 motion_length(const MotionID& M, const CMotionDef*& md, float speed, IKinematicsAnimated* itemModel) const;
    u32 motion_length(const shared_str& anim_name, const shared_str& hud_name, const CMotionDef*& md);
    void OnMovementChanged(ACTOR_DEFS::EMoveCommand cmd) const;

    bool CheckCompatibility(CHudItem* item);
    void set_bone_visible(const shared_str& bone_name, BOOL bVisibility, BOOL bSilent = FALSE);

private:
    void load_ancors();
    void update_inertion(Fmatrix& trans) const;
    void update_additional(Fmatrix& trans) const;
public:
    IKinematicsAnimated* m_model{};
    IKinematicsAnimated* m_model_2{}; // static first frame of "anm_idle_aim": aim rest pose
    IKinematicsAnimated* m_model_3{}; // static first frame of "anm_idle": hip rest pose

    bool inertion_allowed() const;

    Fmatrix get_transform() const { return m_transform; }
    xr_vector<u16> m_ancors;
    // Cached anchor bone transform of the static aim rest pose; a change of it re-runs the
    // addon aim-offset solver.
    mutable Fmatrix tmp;

private:
    shared_str m_sect_name;
    shared_str m_visual_name;

    Fmatrix m_attach_offset{};

    Fmatrix m_transform{ Fidentity };
    // Hands transform built on the raw HUD camera (no inertion/strafe), base of hud_transform.
    Fmatrix m_second_transform{ Fidentity };
    // Hands transform built on the raw HUD camera plus the zoom (aim) offset. Base of the
    // laser dot rest pose.
    Fmatrix m_rest_transform{ Fidentity };
    attachable_hud_item* m_attached_item = NULL;
    xr_unordered_map<shared_str, attachable_hud_item*> m_pool;

    mutable u16 hud_aim_offset_update_interval = 0;
};

extern player_hud* g_player_hud[2]; // 0 - right hand | 1 - left hand 
