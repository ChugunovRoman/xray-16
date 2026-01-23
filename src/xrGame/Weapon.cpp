#include "StdAfx.h"
#include "Weapon.h"
#include "ParticlesObject.h"
#include "entity_alive.h"
#include "inventory_item_impl.h"
#include "Inventory.h"
#include "xrServer_Objects_ALife_Items.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "Level.h"
#include "xrEngine/xr_level_controller.h"
#include "game_cl_base.h"
#include "Include/xrRender/Kinematics.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrPhysics/MathUtils.h"
#include "Common/object_broker.h"
#include "player_hud.h"
#include "HUDManager.h"
#include "GamePersistent.h"
#include "EffectorFall.h"
#include "debug_renderer.h"
#include "static_cast_checked.hpp"
#include "clsid_game.h"
#include "WeaponKnife.h"
#include "WeaponBinocularsVision.h"
#include "xrUICore/Windows/UIWindow.h"
#include "ui/UIXmlInit.h"
#include "Torch.h"
#include "xrNetServer/NET_Messages.h"
#include "xrCore/xr_token.h"
#include "GamePersistent.h"

#define WEAPON_REMOVE_TIME 60000
#define ROTATION_TIME 0.25f
#define ADDON_ID_NONE (u32(-1))

ENGINE_API extern float psHUD_FOV_def;
extern float g_aim_z_offset_coff;
extern float g_second_aim_z_offset_coff;

constexpr pcstr WPN_MAIN_SLOT = "slot_1";
constexpr pcstr DOT = "dot";
constexpr pcstr WPN_SCOPE = "wpn_scope";
constexpr pcstr WPN_SCOPE_2 = "wpn_scope_2";
constexpr pcstr WPN_SILENCER = "wpn_silencer";
constexpr pcstr WPN_GRENADE_LAUNCHER = "wpn_launcher";
constexpr pcstr WPN_GRENADE_LAUNCHER_SOC = "wpn_grenade_launcher";

BOOL b_toggle_weapon_aim = FALSE;

static class CUIWpnScopeXmlManager : public pureUIReset, public pureAppEnd
{
    CUIXml m_xml;
    bool m_loaded{};

    void Load()
    {
        m_loaded = m_xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "scopes.xml");
    }

    void Clear()
    {
        m_xml.ClearInternal();
        m_loaded = false;
    }

public:
    void Init()
    {
        if (m_loaded)
            return;

        Load();
        Device.seqUIReset.Add(this);
    }

    void OnAppEnd() override
    {
        Clear();
        Device.seqUIReset.Remove(this);
    }

    void OnUIReset() override
    {
        Clear();
        if (g_pGameLevel)
            Load();
    }

    CUIXml& operator*()
    {
        return m_xml;
    }
} pWpnScopeXml;

CWeapon::CWeapon()
{
    SetState(eHidden);
    SetNextState(eHidden);
    m_sub_state = eSubstateReloadBegin;
    m_bTriStateReload = false;
    SetDefaults();

    m_Offset.identity();
    m_StrapOffset.identity();

    m_iAmmoCurrentTotal = 0;
    m_BriefInfo_CalcFrame = 0;

    iAmmoElapsed = -1;
    iMagazineSize = -1;
    m_ammoType = 0;

    bHasBulletsToHide = false;
    bullet_cnt = 0;
    bClearJamOnly = false;

    eHandDependence = hdNone;

    m_zoom_params.m_fCurrentZoomFactor = g_fov;
    m_zoom_params.m_fZoomRotationFactor = 0.f;
    m_zoom_params.m_pVision = nullptr;
    m_zoom_params.m_pNight_vision = nullptr;

    m_pCurrentAmmo = nullptr;

    m_pFlameParticles2 = nullptr;
    m_sFlameParticles2 = nullptr;

    m_fCurrentCartirdgeDisp = 1.f;

    m_strap_bone0 = nullptr;
    m_strap_bone1 = nullptr;
    m_StrapOffset.identity();
    m_strapped_mode = false;
    m_can_be_strapped = false;
    m_ef_main_weapon_type = u32(-1);
    m_ef_weapon_type = u32(-1);
    m_UIScope = nullptr;
    m_set_next_ammoType_on_reload = undefined_ammo_type;
    m_crosshair_inertion = 0.f;
    m_activation_speed_is_overriden = false;
    m_cur_scope = 0;
    m_bRememberActorNVisnStatus = false;

    // Mortan: new params
    bUseAltScope = false;
    bScopeIsHasTexture = false;
    bNVsecondVPavaible = false;
    bNVsecondVPstatus = false;

    m_nearwall_last_hud_fov = psHUD_FOV_def;
    m_fZoomStepCount = 3.0f;
    m_fZoomMinKoeff = 0.3f;
    m_fLR_MovingFactor = 0.f;
    m_fLR_CameraFactor = 0.f;
    m_fLR_InertiaFactor = 0.f;
    m_fUD_InertiaFactor = 0.f;

    m_zoom_params.m_f3dZoomFactor = 0.0f;
    m_zoom_params.m_fSecondVPFovFactor = 0.0f;
    m_fSecondRTZoomFactor = 0.0f;
}

const shared_str CWeapon::GetScopeName() const
{
    if (bUseAttachmentSystem)
    {
        auto addon = GetAddonMainScope();
        if (!addon.second)
            return "wpn_addon_scope";
        return addon.second->addon_item_name;
    }

    if (bUseAltScope)
        return m_scopes[m_cur_scope];
    else if (m_cur_scope)
        return READ_IF_EXISTS(pSettings, r_string, m_scopes[m_cur_scope], "scope_name", "wpn_addon_scope");
    else
        return READ_IF_EXISTS(pSettings, r_string, m_section_id, "scope_name", "wpn_addon_scope");
}

void CWeapon::UpdateAltScope()
{
    if (m_eScopeStatus != ALife::eAddonAttachable || !bUseAltScope)
        return;

    shared_str sectionNeedLoad;

    sectionNeedLoad = IsScopePermament() ? m_section_id : IsScopeAttached() ? GetNameWithAttachment() : m_section_id;

    if (!pSettings->section_exist(sectionNeedLoad))
        return;

    shared_str vis = pSettings->r_string(sectionNeedLoad, "visual");

    if (vis != cNameVisual())
    {
        cNameVisual_set(vis);
    }

    shared_str new_hud = pSettings->r_string(sectionNeedLoad, "hud");
    if (new_hud != hud_sect)
    {
        hud_sect = new_hud;
    }
}

bool CWeapon::bChangeNVSecondVPStatus()
{
    if (!bNVsecondVPavaible || (!IsZoomed() && !IsSecondZoomed()))
        return false;

    bNVsecondVPstatus = !bNVsecondVPstatus;

    return true;
}

shared_str CWeapon::GetNameWithAttachment()
{
    string64 str;
    if (pSettings->line_exist(m_section_id.c_str(), "parent_section"))
    {
        shared_str parent = pSettings->r_string(m_section_id.c_str(), "parent_section");
        xr_sprintf(str, "%s_%s", parent.c_str(), GetScopeName().c_str());
    }
    else
    {
        xr_sprintf(str, "%s_%s", m_section_id.c_str(), GetScopeName().c_str());
    }
    return (shared_str)str;
}

CWeapon::~CWeapon()
{
    xr_delete(m_UIScope);
    delete_data(m_scopes);
    delete_data(m_addons);
}

void CWeapon::Hit(SHit* pHDS) { inherited::Hit(pHDS); }
void CWeapon::UpdateXForm()
{
    if (Device.dwFrame == dwXF_Frame)
        return;

    dwXF_Frame = Device.dwFrame;

    if (!H_Parent())
        return;

    // Get access to entity and its visual
    CEntityAlive* E = smart_cast<CEntityAlive*>(H_Parent());

    if (!E)
    {
        if (!IsGameTypeSingle())
            UpdatePosition(H_Parent()->XFORM());

        return;
    }

    const CInventoryOwner* parent = smart_cast<const CInventoryOwner*>(E);
    if (!parent || parent->attached(this))
        return;

    IKinematics* V = smart_cast<IKinematics*>(E->Visual());
    VERIFY(V);

    // Get matrices
    int boneL = -1, boneR = -1, boneR2 = -1;

    // this ugly case is possible in case of a CustomMonster, not a Stalker, nor an Actor
    E->g_WeaponBones(boneL, boneR, boneR2);

    if (boneR == -1)
        return;

    if ((HandDependence() == hd1Hand) || (GetState() == eReload) || (!E->g_Alive()))
        boneL = boneR2;

    V->CalculateBones();
    Fmatrix& mL = V->LL_GetTransform(u16(boneL));
    Fmatrix& mR = V->LL_GetTransform(u16(boneR));
    // Calculate
    Fmatrix mRes;
    Fvector R, D, N;
    D.sub(mL.c, mR.c);

    if (fis_zero(D.magnitude()))
    {
        mRes.set(E->XFORM());
        mRes.c.set(mR.c);
    }
    else
    {
        D.normalize();
        R.crossproduct(mR.j, D);

        N.crossproduct(D, R);
        N.normalize();

        mRes.set(R, N, D, mR.c);
        mRes.mulA_43(E->XFORM());
    }

    UpdatePosition(mRes);
}

void CWeapon::UpdateFireDependencies_internal()
{
    if (Device.dwFrame != dwFP_Frame)
    {
        dwFP_Frame = Device.dwFrame;

        UpdateXForm();

        if (GetHUDmode())
        {
            HudItemData()->setup_firedeps(m_current_firedeps);
            VERIFY(_valid(m_current_firedeps.m_FireParticlesXForm));
        }
        else
        {
            // 3rd person or no parent
            Fmatrix& parent = XFORM();
            Fvector& fp = vLoadedFirePoint;
            Fvector& fp2 = vLoadedFirePoint2;
            Fvector& sp = vLoadedShellPoint;

            parent.transform_tiny(m_current_firedeps.vLastFP, fp);
            parent.transform_tiny(m_current_firedeps.vLastFP2, fp2);
            parent.transform_tiny(m_current_firedeps.vLastSP, sp);

            m_current_firedeps.vLastFD.set(0.f, 0.f, 1.f);
            parent.transform_dir(m_current_firedeps.vLastFD);

            m_current_firedeps.m_FireParticlesXForm.set(parent);
            VERIFY(_valid(m_current_firedeps.m_FireParticlesXForm));
        }
    }
}

void CWeapon::ForceUpdateFireParticles()
{
    if (!GetHUDmode())
    { // update particlesXFORM real bullet direction

        if (!H_Parent())
            return;

        Fvector p, d;
        smart_cast<CEntity*>(H_Parent())->g_fireParams(this, p, d);

        Fmatrix _pxf;
        _pxf.k = d;
        _pxf.i.crossproduct(Fvector().set(0.0f, 1.0f, 0.0f), _pxf.k);
        _pxf.j.crossproduct(_pxf.k, _pxf.i);
        _pxf.c = XFORM().c;

        m_current_firedeps.m_FireParticlesXForm.set(_pxf);
    }
}

void CWeapon::Load(LPCSTR section)
{
    inherited::Load(section);
    CShootingObject::Load(section);

    if (pSettings->line_exist(section, "flame_particles_2"))
        m_sFlameParticles2 = pSettings->r_string(section, "flame_particles_2");

    // load ammo classes
    m_ammoTypes.clear();
    LPCSTR S = pSettings->r_string(section, "ammo_class");
    if (S && S[0])
    {
        string128 _ammoItem;
        int count = _GetItemCount(S);
        for (int it = 0; it < count; ++it)
        {
            _GetItem(S, it, _ammoItem);
            m_ammoTypes.push_back(_ammoItem);
        }
    }

    iAmmoElapsed = pSettings->r_s32(section, "ammo_elapsed");
    iMagazineSize = pSettings->r_s32(section, "ammo_mag_size");
    bUseAttachmentSystem = pSettings->read_if_exists<bool>(section, "use_attachment_system", false);

    if (bUseAttachmentSystem)
    {
        Fvector4 w_pos = pSettings->read_if_exists<Fvector4>(m_section_id.c_str(), "attachment_system_offset_on_world_model", Fvector4().set(0.f, 0.f, 0.f, 0.f));
        bAttachmentSystemOffsetOnWorldModel.identity();
        bAttachmentSystemOffsetOnWorldModel.setHPB(0.0f, 0.0f, w_pos.w);
        bAttachmentSystemOffsetOnWorldModel.translate_over(w_pos.x, w_pos.y, w_pos.z);
        bApplyAncorTransform = pSettings->read_if_exists<bool>(section, "apply_anchor_transform", false);
    }

    ////////////////////////////////////////////////////
    // дисперсия стрельбы

    // подбрасывание камеры во время отдачи
    u8 rm = READ_IF_EXISTS(pSettings, r_u8, section, "cam_return", 1);
    cam_recoil.ReturnMode = (rm == 1);

    rm = READ_IF_EXISTS(pSettings, r_u8, section, "cam_return_stop", 0);
    cam_recoil.StopReturn = (rm == 1);

    float temp_f = 0.0f;
    temp_f = pSettings->r_float(section, "cam_relax_speed");
    cam_recoil.RelaxSpeed = _abs(deg2rad(temp_f));
    VERIFY2(!fis_zero(cam_recoil.RelaxSpeed), section);
    if (fis_zero(cam_recoil.RelaxSpeed))
    {
        cam_recoil.RelaxSpeed = EPS_L;
    }

    cam_recoil.RelaxSpeed_AI = cam_recoil.RelaxSpeed;
    if (pSettings->line_exist(section, "cam_relax_speed_ai"))
    {
        temp_f = pSettings->r_float(section, "cam_relax_speed_ai");
        cam_recoil.RelaxSpeed_AI = _abs(deg2rad(temp_f));
        VERIFY2(!fis_zero(cam_recoil.RelaxSpeed_AI), section);
        if (fis_zero(cam_recoil.RelaxSpeed_AI))
        {
            cam_recoil.RelaxSpeed_AI = EPS_L;
        }
    }
    temp_f = pSettings->r_float(section, "cam_max_angle");
    cam_recoil.MaxAngleVert = _abs(deg2rad(temp_f));
    VERIFY2(!fis_zero(cam_recoil.MaxAngleVert), section);
    if (fis_zero(cam_recoil.MaxAngleVert))
    {
        cam_recoil.MaxAngleVert = EPS;
    }

    temp_f = pSettings->r_float(section, "cam_max_angle_horz");
    cam_recoil.MaxAngleHorz = _abs(deg2rad(temp_f));
    VERIFY2(!fis_zero(cam_recoil.MaxAngleHorz), section);
    if (fis_zero(cam_recoil.MaxAngleHorz))
    {
        cam_recoil.MaxAngleHorz = EPS;
    }

    temp_f = pSettings->r_float(section, "cam_step_angle_horz");
    cam_recoil.StepAngleHorz = deg2rad(temp_f);

    cam_recoil.DispersionFrac = _abs(READ_IF_EXISTS(pSettings, r_float, section, "cam_dispersion_frac", 0.7f));

    // подбрасывание камеры во время отдачи в режиме zoom ==> ironsight or scope
    // zoom_cam_recoil.Clone( cam_recoil ); ==== нельзя !!!!!!!!!!
    zoom_cam_recoil.RelaxSpeed = cam_recoil.RelaxSpeed;
    zoom_cam_recoil.RelaxSpeed_AI = cam_recoil.RelaxSpeed_AI;
    zoom_cam_recoil.DispersionFrac = cam_recoil.DispersionFrac;
    zoom_cam_recoil.MaxAngleVert = cam_recoil.MaxAngleVert;
    zoom_cam_recoil.MaxAngleHorz = cam_recoil.MaxAngleHorz;
    zoom_cam_recoil.StepAngleHorz = cam_recoil.StepAngleHorz;

    zoom_cam_recoil.ReturnMode = cam_recoil.ReturnMode;
    zoom_cam_recoil.StopReturn = cam_recoil.StopReturn;

    if (pSettings->line_exist(section, "zoom_cam_relax_speed"))
    {
        zoom_cam_recoil.RelaxSpeed = _abs(deg2rad(pSettings->r_float(section, "zoom_cam_relax_speed")));
        VERIFY2(!fis_zero(zoom_cam_recoil.RelaxSpeed), section);
        if (fis_zero(zoom_cam_recoil.RelaxSpeed))
        {
            zoom_cam_recoil.RelaxSpeed = EPS_L;
        }
    }
    if (pSettings->line_exist(section, "zoom_cam_relax_speed_ai"))
    {
        zoom_cam_recoil.RelaxSpeed_AI = _abs(deg2rad(pSettings->r_float(section, "zoom_cam_relax_speed_ai")));
        VERIFY2(!fis_zero(zoom_cam_recoil.RelaxSpeed_AI), section);
        if (fis_zero(zoom_cam_recoil.RelaxSpeed_AI))
        {
            zoom_cam_recoil.RelaxSpeed_AI = EPS_L;
        }
    }
    if (pSettings->line_exist(section, "zoom_cam_max_angle"))
    {
        zoom_cam_recoil.MaxAngleVert = _abs(deg2rad(pSettings->r_float(section, "zoom_cam_max_angle")));
        VERIFY2(!fis_zero(zoom_cam_recoil.MaxAngleVert), section);
        if (fis_zero(zoom_cam_recoil.MaxAngleVert))
        {
            zoom_cam_recoil.MaxAngleVert = EPS;
        }
    }
    if (pSettings->line_exist(section, "zoom_cam_max_angle_horz"))
    {
        zoom_cam_recoil.MaxAngleHorz = _abs(deg2rad(pSettings->r_float(section, "zoom_cam_max_angle_horz")));
        VERIFY2(!fis_zero(zoom_cam_recoil.MaxAngleHorz), section);
        if (fis_zero(zoom_cam_recoil.MaxAngleHorz))
        {
            zoom_cam_recoil.MaxAngleHorz = EPS;
        }
    }
    if (pSettings->line_exist(section, "zoom_cam_step_angle_horz"))
    {
        zoom_cam_recoil.StepAngleHorz = deg2rad(pSettings->r_float(section, "zoom_cam_step_angle_horz"));
    }
    if (pSettings->line_exist(section, "zoom_cam_dispersion_frac"))
    {
        zoom_cam_recoil.DispersionFrac = _abs(pSettings->r_float(section, "zoom_cam_dispersion_frac"));
    }

    m_pdm.m_fPDM_disp_base = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_base", 1.0f);
    m_pdm.m_fPDM_disp_vel_factor = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_vel_factor", 1.0f);
    m_pdm.m_fPDM_disp_accel_factor = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_accel_factor", 1.0f);
    m_pdm.m_fPDM_disp_crouch = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_crouch", 1.0f);
    m_pdm.m_fPDM_disp_crouch_no_acc = READ_IF_EXISTS(pSettings, r_float, section, "PDM_disp_crouch_no_acc", 1.0f);

    m_crosshair_inertion = READ_IF_EXISTS(pSettings, r_float, section, "crosshair_inertion", 5.91f);
    m_first_bullet_controller.load(section);
    fireDispersionConditionFactor = pSettings->r_float(section, "fire_dispersion_condition_factor");

    // modified by Peacemaker [17.10.08]
    if (pSettings->line_exist(section, "misfire_start_condition") ||
        pSettings->line_exist(section, "misfire_end_condition") ||
        pSettings->line_exist(section, "misfire_start_prob") ||
        pSettings->line_exist(section, "misfire_end_prob"))
    {
        misfireStartCondition   = pSettings->r_float(section, "misfire_start_condition");
        misfireEndCondition     = pSettings->r_float(section, "misfire_end_condition");
        misfireStartProbability = pSettings->r_float(section, "misfire_start_prob");
        misfireEndProbability   = pSettings->r_float(section, "misfire_end_prob");
    }
    else
    {
        misfireUseOldFormula    = true;

        misfireProbability      = pSettings->r_float(section, "misfire_probability");
        misfireConditionK       = pSettings->read_if_exists<float>(section, "misfire_condition_k", 1.0f);

        // For UI indicators to work correctly, rough estimate values
        misfireStartCondition   = 0.95f;
        misfireEndCondition     = 0.0f;
        misfireStartProbability = misfireProbability;
        misfireEndProbability   = (misfireProbability + misfireConditionK) * 0.25f;
    }
    conditionDecreasePerShot = pSettings->r_float(section, "condition_shot_dec");
    conditionDecreasePerQueueShot = pSettings->read_if_exists<float>(section, "condition_queue_shot_dec", conditionDecreasePerShot);

    vLoadedFirePoint = pSettings->r_fvector3(section, "fire_point");
    vLoadedFirePoint2 = pSettings->read_if_exists<Fvector3>(section, "fire_point2", vLoadedFirePoint);

    // hands
    eHandDependence = EHandDependence(pSettings->r_s32(section, "hand_dependence"));

    m_bIsSingleHanded = pSettings->read_if_exists<bool>(section, "single_handed", true);

    // информация о возможных апгрейдах и их визуализации в инвентаре
    m_eScopeStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "scope_status");
    m_eSilencerStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "silencer_status");
    m_eGrenadeLauncherStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "grenade_launcher_status");

    if (!bUseAttachmentSystem && pSettings->line_exist(section, "scopes") && xr_strcmp(pSettings->r_string(section, "scopes"), "") != 0 && xr_strcmp(pSettings->r_string(section, "scopes"), "none") != 0)
        m_eScopeStatus = ALife::EWeaponAddonStatus::eAddonAttachable;

    m_zoom_params.m_bZoomEnabled = !!pSettings->r_bool(section, "zoom_enabled");
    m_zoom_params.m_bZoomSecondEnabled = READ_IF_EXISTS(pSettings, r_bool, section, "use_alt_aim_hud", false);
    m_zoom_params.m_fZoomRotateTime = READ_IF_EXISTS(pSettings, r_float, section, "zoom_rotate_time", ROTATION_TIME);

    m_zoom_params.m_bUseDynamicZoom = FALSE;
    m_zoom_params.m_sUseZoomPostprocess = "";
    m_zoom_params.m_sUseBinocularVision = "";

    LoadAddonSlosts(section);
    LoadModParams(section);
    bUseAltScope = !!bLoadAltScopesParams(section);
    bLoadzCollimatorScopesParams(section);

    if (!bUseAltScope)
        LoadOriginalScopesParams(section);

    if (m_eSilencerStatus == ALife::eAddonAttachable)
    {
        m_sSilencerName = pSettings->r_string(section, "silencer_name");
        m_iSilencerX = pSettings->r_s32(section, "silencer_x");
        m_iSilencerY = pSettings->r_s32(section, "silencer_y");
    }

    if (m_eGrenadeLauncherStatus == ALife::eAddonAttachable)
    {
        m_sGrenadeLauncherName = pSettings->r_string(section, "grenade_launcher_name");
        m_iGrenadeLauncherX = pSettings->r_s32(section, "grenade_launcher_x");
        m_iGrenadeLauncherY = pSettings->r_s32(section, "grenade_launcher_y");
    }

    UpdateAltScope();
    InitAddons();

    const bool is_16x9 = UICore::is_widescreen();

    string64 base_hud_sect;
    string128 val_name;
    string64 _prefix;

    xr_sprintf(_prefix, "%s", is_16x9 ? "_16x9" : "");
    xr_sprintf(base_hud_sect, "%s_hud", section);
    
    if (pSettings->line_exist(section, "hud"))
        xr_sprintf(base_hud_sect, "%s", pSettings->r_string(section, "hud"));

    m_hands_offset[0][0].set(0, 0, 0);
    m_hands_offset[1][0].set(0, 0, 0);

    if (!bUseAttachmentSystem)
    {
        strconcat(sizeof(val_name), val_name, "aim_hud_offset_alt_pos", _prefix);
        if (pSettings->line_exist(hud_sect, val_name))
            m_hands_offset[0][1] = pSettings->r_fvector3(hud_sect, val_name);
        else if (pSettings->line_exist(base_hud_sect, val_name))
            m_hands_offset[0][1] = pSettings->r_fvector3(base_hud_sect, val_name);
        strconcat(sizeof(val_name), val_name, "aim_hud_offset_alt_rot", _prefix);
        if (pSettings->line_exist(hud_sect, val_name))
            m_hands_offset[1][1] = pSettings->r_fvector3(hud_sect, val_name);
        else if (pSettings->line_exist(base_hud_sect, val_name))
            m_hands_offset[1][1] = pSettings->r_fvector3(base_hud_sect, val_name);
    }

    if (pSettings->line_exist(section, "weapon_remove_time"))
        m_dwWeaponRemoveTime = pSettings->r_u32(section, "weapon_remove_time");
    else
        m_dwWeaponRemoveTime = WEAPON_REMOVE_TIME;

    if (pSettings->line_exist(section, "auto_spawn_ammo"))
        m_bAutoSpawnAmmo = pSettings->r_bool(section, "auto_spawn_ammo");
    else
        m_bAutoSpawnAmmo = TRUE;

    m_zoom_params.m_bHideCrosshairInZoom = true;

    if (pSettings->line_exist(hud_sect, "zoom_hide_crosshair"))
        m_zoom_params.m_bHideCrosshairInZoom = !!pSettings->r_bool(hud_sect, "zoom_hide_crosshair");

    Fvector def_dof;
    def_dof.set(-1, -1, -1);
    m_zoom_params.m_ZoomDof = READ_IF_EXISTS(pSettings, r_fvector3, section, "zoom_dof",Fvector().set(-1, -1, -1));
    m_zoom_params.m_bZoomDofEnabled = !def_dof.similar(m_zoom_params.m_ZoomDof);

    m_zoom_params.m_ReloadDof = READ_IF_EXISTS(pSettings, r_fvector4, section, "reload_dof",Fvector4().set(-1, -1, -1, -1));
    m_zoom_params.m_ReloadEmptyDof = READ_IF_EXISTS(pSettings, r_fvector4, section, "reload_empty_dof", Fvector4().set(-1, -1, -1, -1));

    m_bHasTracers = !!READ_IF_EXISTS(pSettings, r_bool, section, "tracers", true);
    m_u8TracerColorID = READ_IF_EXISTS(pSettings, r_u8, section, "tracers_color_ID", u8(-1));

    string256 temp;
    for (u32 i = egdNovice; i < egdCount; ++i)
    {
        strconcat(temp, "hit_probability_", get_token_name(difficulty_type_token, static_cast<int>(i)));
        m_hit_probability[i] = READ_IF_EXISTS(pSettings, r_float, section, temp, 1.f);
    }

    // Added by Axel, to enable optional condition use on any item
    m_flags.set(FUsingCondition, READ_IF_EXISTS(pSettings, r_bool, section, "use_condition", true));
}
void CWeapon::LoadScope(const shared_str& section)
{
    if (ShadowOfChernobylMode) // XXX: temporary check for SOC mode, to be removed
        return;
    pWpnScopeXml.Init();
    R_ASSERT(m_UIScope);
    CUIXmlInit::InitWindow(*pWpnScopeXml, section.c_str(), 0, m_UIScope);
}

void CWeapon::LoadFireParams(LPCSTR section)
{
    cam_recoil.Dispersion = deg2rad(pSettings->r_float(section, "cam_dispersion"));
    cam_recoil.DispersionInc = 0.0f;

    if (pSettings->line_exist(section, "cam_dispersion_inc"))
    {
        cam_recoil.DispersionInc = deg2rad(pSettings->r_float(section, "cam_dispersion_inc"));
    }

    zoom_cam_recoil.Dispersion = cam_recoil.Dispersion;
    zoom_cam_recoil.DispersionInc = cam_recoil.DispersionInc;

    if (pSettings->line_exist(section, "zoom_cam_dispersion"))
    {
        zoom_cam_recoil.Dispersion = deg2rad(pSettings->r_float(section, "zoom_cam_dispersion"));
    }
    if (pSettings->line_exist(section, "zoom_cam_dispersion_inc"))
    {
        zoom_cam_recoil.DispersionInc = deg2rad(pSettings->r_float(section, "zoom_cam_dispersion_inc"));
    }

    CShootingObject::LoadFireParams(section);
};

void CWeapon::LoadModParams(LPCSTR section)
{
    // Modifier for HUD FOV from the hip
    m_hud_fov_add_mod = READ_IF_EXISTS(pSettings, r_float, section, "hud_fov_addition_modifier", 0.f);

    // Parameters for changing the HUD FOV when the player is standing close to the wall
    m_nearwall_dist_min = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_dist_min", 0.5f);
    m_nearwall_dist_max = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_dist_max", 1.f);
    m_nearwall_target_hud_fov = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_target_hud_fov", 0.27f);
    m_nearwall_speed_mod = READ_IF_EXISTS(pSettings, r_float, section, "nearwall_speed_mod", 10.f);

    // Настройки стрейфа (боковая ходьба)
    const Fvector vZero = { 0.f, 0.f, 0.f };
    Fvector vDefStrafeValue;
    vDefStrafeValue.set(vZero);

    //--> Смещение в стрейфе
    m_strafe_offset[0][0] = READ_IF_EXISTS(pSettings, r_fvector3, section, "strafe_hud_offset_pos", vDefStrafeValue);
    m_strafe_offset[1][0] = READ_IF_EXISTS(pSettings, r_fvector3, section, "strafe_hud_offset_rot", vDefStrafeValue);

    //--> Поворот в стрейфе
    m_strafe_offset[0][1] = READ_IF_EXISTS(pSettings, r_fvector3, section, "strafe_aim_hud_offset_pos", vDefStrafeValue);
    m_strafe_offset[1][1] = READ_IF_EXISTS(pSettings, r_fvector3, section, "strafe_aim_hud_offset_rot", vDefStrafeValue);

    // Параметры стрейфа
    bool  bStrafeEnabled = READ_IF_EXISTS(pSettings, r_bool, section, "strafe_enabled", false);
    bool  bStrafeEnabled_aim = READ_IF_EXISTS(pSettings, r_bool, section, "strafe_aim_enabled", false);
    float fFullStrafeTime = READ_IF_EXISTS(pSettings, r_float, section, "strafe_transition_time", 0.01f);
    float fFullStrafeTime_aim = READ_IF_EXISTS(pSettings, r_float, section, "strafe_aim_transition_time", 0.01f);
    float fStrafeCamLFactor = READ_IF_EXISTS(pSettings, r_float, section, "strafe_cam_limit_factor", 0.5f);
    float fStrafeCamLFactor_aim = READ_IF_EXISTS(pSettings, r_float, section, "strafe_cam_limit_aim_factor", 1.0f);
    float fStrafeMinAngle = READ_IF_EXISTS(pSettings, r_float, section, "strafe_cam_min_angle", 0.0f);
    float fStrafeMinAngle_aim = READ_IF_EXISTS(pSettings, r_float, section, "strafe_cam_aim_min_angle", 7.0f);

    //--> (Data 1)
    m_strafe_offset[2][0].set((bStrafeEnabled ? 1.0f : 0.0f), fFullStrafeTime, 0.0f);         // normal
    m_strafe_offset[2][1].set((bStrafeEnabled_aim ? 1.0f : 0.0f), fFullStrafeTime_aim, 0.0f); // aim-GL

    //--> (Data 2)
    m_strafe_offset[3][0].set(fStrafeCamLFactor, fStrafeMinAngle, 0.0f); // normal
    m_strafe_offset[3][1].set(fStrafeCamLFactor_aim, fStrafeMinAngle_aim, 0.0f); // aim-GL
}

bool CWeapon::bLoadAltScopesParams(LPCSTR section)
{
    if (!pSettings->line_exist(section, "scopes"))
        return false;

    if (!xr_strcmp(pSettings->r_string(section, "scopes"), "none"))
        return false;

    if (m_eScopeStatus == ALife::eAddonAttachable && m_scopes.size() == 0)
    {
        LPCSTR str = pSettings->r_string(section, "scopes");
        for (int i = 0, count = _GetItemCount(str); i < count; ++i)
        {
            string128 scope_section;
            _GetItem(str, i, scope_section);
            m_scopes.push_back(scope_section);
            if (pSettings->line_exist(section, "scope_name") && xr_strcmp(pSettings->r_string(section, "scope_name"), scope_section) == 0)
            {
                m_cur_scope = u8(i);
                m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonScope;
                b_forceIconUpdate = true;
                UpdateAddonsOffset();
            }
        }
    }

    LoadCurrentScopeParams(section);

    return true;
}
bool CWeapon::bLoadzCollimatorScopesParams(LPCSTR section)
{
    if (!bUseAttachmentSystem)
        return false;

    if (m_eScopeStatus == ALife::eAddonAttachable && m_addons.size() == 0)
    {
        for (const auto& name : Dbg.GetSections(ESectionTypeName::scopes))
        {
            if (pSettings->line_exist(name, "slot_type"))
            {
                EWeaponAddonSlotType slot_type = (EWeaponAddonSlotType)pSettings->r_u16(name, "slot_type");
                if (slot_type == m_addon_slot_type)
                    m_addons.push_back(name);
            }
        }
    }

    return true;
}

void CWeapon::LoadOriginalScopesParams(LPCSTR section)
{
    if (pSettings->line_exist(section, "scopes_sect"))
    {
        LPCSTR str = pSettings->r_string(section, "scopes_sect");
        for (int i = 0, count = _GetItemCount(str); i < count; ++i)
        {
            string128 scope_section;
            _GetItem(str, i, scope_section);
            m_scopes.push_back(scope_section);
        }
    }
    else
    {
        m_scopes.push_back(section);
    }

    LoadCurrentScopeParams(section);
}

void CWeapon::LoadCurrentScopeParams(LPCSTR section)
{
    shared_str scope_tex_name = "none";
    shared_str scope_name = section;
    bScopeIsHasTexture = false;

    if(bUseAttachmentSystem)
    {
        auto addon = GetAddonMainScope();
        if (addon.second)
            scope_name = addon.second->addon_item_name;
    }
    else if (IsScopeAttached())
        scope_name = GetScopeName();

    if (pSettings->line_exist(scope_name, "scope_texture") && xr_strcmp(pSettings->r_string(scope_name.c_str(), "scope_texture"), "none") != 0)
    {
        scope_tex_name = pSettings->r_string(scope_name.c_str(), "scope_texture");
        bScopeIsHasTexture = true;
    }

    if(bUseAttachmentSystem)
        Load3DScopeParams(scope_name.c_str());
    else
        Load3DScopeParams(section);

    m_zoom_params.m_fScopeZoomFactor = pSettings->read_if_exists<float>(scope_name.c_str(), "scope_zoom_factor", 83.3f);
    m_zoom_params.m_fSecondScopeZoomFactor = pSettings->read_if_exists<float>(scope_name.c_str(), "scope_zoom_factor_alt", 73.0f);

    if (bScopeIsHasTexture || bIsSecondVPZoomPresent())
    {
        if (bIsSecondVPZoomPresent())
            bNVsecondVPavaible = !!pSettings->line_exist(scope_name.c_str(), "scope_nightvision");

        if (!m_zoom_params.m_sUseZoomPostprocess.size() || bUseAttachmentSystem)
        {
            if(bUseAttachmentSystem)
            {
                m_zoom_params.m_sUseZoomPostprocess = READ_IF_EXISTS(pSettings, r_string, scope_name.c_str(), "scope_nightvision", 0);
                m_zoom_params.m_bUseDynamicZoom = READ_IF_EXISTS(pSettings, r_bool, scope_name.c_str(), "scope_dynamic_zoom", FALSE);
            }
            else
            {
                m_zoom_params.m_sUseZoomPostprocess = READ_IF_EXISTS(pSettings, r_string, section, "scope_nightvision", 0);
                m_zoom_params.m_bUseDynamicZoom = READ_IF_EXISTS(pSettings, r_bool, section, "scope_dynamic_zoom", FALSE);
            }
        }


        if (m_zoom_params.m_bUseDynamicZoom)
        {
            if(bUseAttachmentSystem)
            {
                m_fZoomStepCount = READ_IF_EXISTS(pSettings, r_u8, scope_name.c_str(), "scope_zoom_steps", 3.0f);
                m_fZoomMinKoeff = READ_IF_EXISTS(pSettings, r_u8, scope_name.c_str(), "min_zoom_k", 0.3f);
            }
            else
            {
                m_fZoomStepCount = READ_IF_EXISTS(pSettings, r_u8, section, "scope_zoom_steps", 3.0f);
                m_fZoomMinKoeff = READ_IF_EXISTS(pSettings, r_u8, section, "min_zoom_k", 0.3f);
            }
        }

        if (!m_zoom_params.m_sUseBinocularVision.size() || bUseAttachmentSystem)
        {
            if(bUseAttachmentSystem)
                m_zoom_params.m_sUseBinocularVision = READ_IF_EXISTS(pSettings, r_string, scope_name.c_str(), "scope_alive_detector", 0);
            else
                m_zoom_params.m_sUseBinocularVision = READ_IF_EXISTS(pSettings, r_string, section, "scope_alive_detector", 0);
        }
    }
    else
    {
        bNVsecondVPavaible = false;
        bNVsecondVPstatus = false;
    }

    if(bUseAttachmentSystem)
        m_fScopeInertionFactor = READ_IF_EXISTS(pSettings, r_float, scope_name.c_str(), "scope_inertion_factor", m_fControlInertionFactor);
    else
        m_fScopeInertionFactor = READ_IF_EXISTS(pSettings, r_float, section, "scope_inertion_factor", m_fControlInertionFactor);

    m_fRTZoomFactor = m_zoom_params.m_fScopeZoomFactor;

    if (m_UIScope)
        xr_delete(m_UIScope);

    if (bScopeIsHasTexture)
    {
        m_UIScope = xr_new<CUIWindow>("Scope UI");
        LoadScope(scope_tex_name);
        CUIXmlInit::InitWindow(*pWpnScopeXml, scope_tex_name.c_str(), 0, m_UIScope);
    }
}

void CWeapon::Load3DScopeParams(LPCSTR section)
{
    m_zoom_params.m_fSecondVPFovFactor = READ_IF_EXISTS(pSettings, r_float, section, "3d_fov", 0.0f);
    m_zoom_params.m_f3dZoomFactor	   = READ_IF_EXISTS(pSettings, r_float, section, "3d_zoom_factor", 100.0f);

    if (fis_zero(m_fSecondRTZoomFactor))
        m_fSecondRTZoomFactor = m_zoom_params.m_f3dZoomFactor;
}

bool CWeapon::net_Spawn(CSE_Abstract* DC)
{
    const bool bResult = inherited::net_Spawn(DC);
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeItemWeapon* E = smart_cast<CSE_ALifeItemWeapon*>(e);

    iAmmoElapsed = E->a_elapsed;

    if (iAmmoElapsed == (u16)-1)
        iAmmoElapsed = 0;

    m_flagsAddOnState = E->m_addon_flags.get();
    m_ammoType = E->ammo_type;
    if (pSettings->line_exist(m_section_id.c_str(), "scope_zoom_factor"))
        m_zoom_params.m_fScopeZoomFactor = pSettings->r_float(m_section_id.c_str(), "scope_zoom_factor");

    m_fRTZoomFactor = m_zoom_params.m_fScopeZoomFactor;
    SetState(E->wpn_state);
    SetNextState(E->wpn_state);

    m_DefaultCartridge.Load(m_ammoTypes[m_ammoType].c_str(), m_ammoType);
    if (iAmmoElapsed)
    {
        m_fCurrentCartirdgeDisp = m_DefaultCartridge.param_s.kDisp;
        for (int i = 0; i < iAmmoElapsed; ++i)
            m_magazine.push_back(m_DefaultCartridge);
    }

    SpawnDefaultAddons();
    UpdateAltScope();
    UpdateAddonsVisibility();
    InitAddons();

    m_dwWeaponIndependencyTime = 0;

    VERIFY((u32)iAmmoElapsed == m_magazine.size());
    m_bAmmoWasSpawned = false;

    if (m_bLightShotEnabled)
        Light_Create();

    return bResult;
}

void CWeapon::net_Destroy()
{
    inherited::net_Destroy();

    //удалить объекты партиклов
    StopFlameParticles();
    StopFlameParticles2();
    StopLight();
    Light_Destroy();

    while (m_magazine.size())
        m_magazine.pop_back();
}

BOOL CWeapon::IsUpdating()
{
    bool bIsActiveItem = m_pInventory && m_pInventory->ActiveItem() == this;
    return bIsActiveItem || bWorking; // || IsPending() || getVisible();
}

void CWeapon::net_Export(NET_Packet& P)
{
    inherited::net_Export(P);

    P.w_float_q8(GetCondition(), 0.0f, 1.0f);

    u8 need_upd = IsUpdating() ? 1 : 0;
    P.w_u8(need_upd);
    P.w_u16(u16(iAmmoElapsed));
    P.w_u8(m_flagsAddOnState);
    P.w_u8(m_ammoType);
    P.w_u8((u8)GetState());
    P.w_u8((u8)IsZoomed());
    P.w_u8((u8)m_cur_scope);
}

void CWeapon::net_Import(NET_Packet& P)
{
    inherited::net_Import(P);

    float _cond;
    P.r_float_q8(_cond, 0.0f, 1.0f);
    SetCondition(_cond);

    u8 flags = 0;
    P.r_u8(flags);

    u16 ammo_elapsed = 0;
    P.r_u16(ammo_elapsed);

    u8 NewAddonState;
    P.r_u8(NewAddonState);

    m_flagsAddOnState = NewAddonState;
    UpdateAddonsVisibility();

    u8 ammoType, wstate;
    P.r_u8(ammoType);
    P.r_u8(wstate);

    u8 Zoom;
    P.r_u8(Zoom);

    u8 scope;
    P.r_u8(scope);

    m_cur_scope = scope;

    if (H_Parent() && H_Parent()->Remote())
    {
        if (Zoom)
            OnZoomIn();
        else
            OnZoomOut();
    };
    switch (wstate)
    {
    case eFire:
    case eFire2:
    case eSwitch:
    case eReload: {
    }
    break;
    default:
    {
        if (ammoType >= m_ammoTypes.size())
            Msg("!! Weapon [%d], State - [%d]", ID(), wstate);
        else
        {
            m_ammoType = ammoType;
            SetAmmoElapsed((ammo_elapsed));
        }
    }
    break;
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());
}

void CWeapon::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);

    output_packet.w_u16(iAmmoElapsed);
    output_packet.w_u8(m_cur_scope);
    output_packet.w_u8(m_flagsAddOnState);
    output_packet.w_u8(m_ammoType);
    output_packet.w_u8(m_zoom_params.m_bIsZoomModeNow ? 1 : 0);
    output_packet.w_u8(m_zoom_params.m_bIsZoomSecondModeNow ? 1 : 0);
    output_packet.w_u8(m_bRememberActorNVisnStatus ? 1 : 0);
    output_packet.w_u8(bNVsecondVPstatus ? 1 : 0);
    output_packet.w_float(m_fSecondRTZoomFactor);
    output_packet.w_stringZ(m_section_id);
    output_packet.w_u8(default_addons_was_loaded ? 1 : 0);
    output_packet.w_u32(m_addon_id);
    output_packet.w_u16(m_addon_items.size());

    for (auto [addon_id, addon]: m_addon_items)
    {
        output_packet.w_stringZ(addon->addon_item_name);
        output_packet.w_stringZ(addon->addon_type);
        output_packet.w_stringZ(addon->slot);
        output_packet.w_u8((u8)addon->ort);
        output_packet.w_u32(addon_id);
        output_packet.w_u32(addon->parent_id);
        output_packet.w_u8(addon->has_scope_texture ? 1 : 0);
        output_packet.w_u16(addon->provided_slot_type);
        output_packet.w_u8(addon->ort != 0 ? 1 : 0);
        output_packet.w_u8(addon->scope_dynamic_zoom != 0 ? 1 : 0);
        output_packet.w_u8(addon->has_mag_size != 0 ? 1 : 0);
        output_packet.w_u8(addon->was_inited_in_default_slots != 0 ? 1 : 0);
    }
}

void CWeapon::load(IReader& input_packet)
{
    inherited::load(input_packet);
    iAmmoElapsed = input_packet.r_u16();
    m_cur_scope = input_packet.r_u8();
    m_flagsAddOnState = input_packet.r_u8();
    UpdateAddonsVisibility();
    m_ammoType = input_packet.r_u8();
    m_zoom_params.m_bIsZoomModeNow = input_packet.r_u8() == 1 ? true : false;
    m_zoom_params.m_bIsZoomSecondModeNow = input_packet.r_u8() == 1 ? true : false;

    if (m_zoom_params.m_bIsZoomModeNow)
        OnZoomIn();
    else
        OnZoomOut();
    if (m_zoom_params.m_bIsZoomSecondModeNow)
        OnZoomSecondIn();
    else
        OnZoomOut();

    m_bRememberActorNVisnStatus = input_packet.r_u8() == 1 ? true : false;
    bNVsecondVPstatus = input_packet.r_u8() == 1 ? true : false;
    m_fSecondRTZoomFactor = input_packet.r_float();
    input_packet.r_stringZ(m_section_id);
    
    default_addons_was_loaded = input_packet.r_u8() == 1 ? true : false;
    m_addon_id = input_packet.r_u32();
    u16 const addonCount = input_packet.r_u16();

    for (int i = 0; i < addonCount; i++)
    {
        AddAddonData data;

        input_packet.r_stringZ(data.item_section_id);
        input_packet.r_stringZ(data.addon_type);
        input_packet.r_stringZ(data.slot_name);
        data.ort = (CInventoryItem::EIIAddonOrt)input_packet.r_u8();
        data.addon_id = input_packet.r_u32();
        data.parent_id = input_packet.r_u32();
        data.has_scope_texture = input_packet.r_u8() == 1 ? true : false;
        data.provided_slot_type = (EWeaponAddonSlotType)input_packet.r_u16();
        data.has_ort = input_packet.r_u8() == 1 ? true : false;
        data.scope_dynamic_zoom = input_packet.r_u8() == 1 ? true : false;
        data.has_mag_size = input_packet.r_u8() == 1 ? true : false;
        data.was_inited_in_default_slots = input_packet.r_u8() == 1 ? true : false;

        addAddon(data);
    }

    reload(m_section_id.c_str());

    if (iAmmoElapsed == (u16)-1)
        iAmmoElapsed = 0;       
}

void CWeapon::OnEvent(NET_Packet& P, u16 type)
{
    switch (type)
    {
    case GE_ADDON_CHANGE:
    {
        P.r_u8(m_flagsAddOnState);
        InitAddons();
        UpdateAddonsVisibility();
    }
    break;

    case GE_WPN_STATE_CHANGE:
    {
        u8 state;
        P.r_u8(state);
        P.r_u8(m_sub_state);
        //          u8 NewAmmoType =
        P.r_u8();
        u8 AmmoElapsed = P.r_u8();
        u8 NextAmmo = P.r_u8();
        if (NextAmmo == undefined_ammo_type)
            m_set_next_ammoType_on_reload = undefined_ammo_type;
        else
            m_set_next_ammoType_on_reload = NextAmmo;

        if (OnClient())
            SetAmmoElapsed(int(AmmoElapsed));
        OnStateSwitch(u32(state), GetState());
    }
    break;
    default: { inherited::OnEvent(P, type);
    }
    break;
    }
};

void CWeapon::shedule_Update(u32 dT)
{
    // Queue shrink
    //  u32 dwTimeCL        = Level().timeServer()-NET_Latency;
    //  while ((NET.size()>2) && (NET[1].dwTimeStamp<dwTimeCL)) NET.pop_front();

    // Inherited
    inherited::shedule_Update(dT);
}

void CWeapon::OnH_B_Independent(bool just_before_destroy)
{
    RemoveShotEffector();

    inherited::OnH_B_Independent(just_before_destroy);

    FireEnd();
    SetPending(FALSE);
    SwitchState(eHidden);

    m_strapped_mode = false;
    m_zoom_params.m_bIsZoomModeNow = false;
    m_zoom_params.m_bIsZoomSecondModeNow = false;
    UpdateXForm();
    m_nearwall_last_hud_fov = psHUD_FOV_def;
}

void CWeapon::OnH_A_Independent()
{
    m_dwWeaponIndependencyTime = Level().timeServer();
    m_fLR_MovingFactor = 0.f;
    m_fLR_CameraFactor = 0.f;
    m_fLR_InertiaFactor = 0.f;
    m_fUD_InertiaFactor = 0.f;
    inherited::OnH_A_Independent();
    Light_Destroy();
    UpdateAddonsVisibility();
};

void CWeapon::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
    UpdateAddonsVisibility();
};

void CWeapon::OnActiveItem()
{
    //. from Activate
    UpdateAddonsVisibility();
    m_BriefInfo_CalcFrame = 0;

    //. Show
    SwitchState(eShowing);
    //-

    inherited::OnActiveItem();
    //если мы занружаемся и оружие было в руках
    //. SetState                    (eIdle);
    //. SetNextState                (eIdle);
}

void CWeapon::OnHiddenItem()
{
    m_BriefInfo_CalcFrame = 0;

    if (IsGameTypeSingle())
        SwitchState(eHiding);
    else
        SwitchState(eHidden);

    OnZoomOut();
    inherited::OnHiddenItem();

    m_set_next_ammoType_on_reload = undefined_ammo_type;
}

void CWeapon::SendHiddenItem()
{
    if (!CHudItem::object().getDestroy() && m_pInventory)
    {
        // !!! Just single entry for given state !!!
        NET_Packet P;
        CHudItem::object().u_EventGen(P, GE_WPN_STATE_CHANGE, CHudItem::object().ID());
        P.w_u8(u8(eHiding));
        P.w_u8(u8(m_sub_state));
        P.w_u8(m_ammoType);
        P.w_u8(u8(iAmmoElapsed & 0xff));
        P.w_u8(m_set_next_ammoType_on_reload);
        CHudItem::object().u_EventSend(P, net_flags(TRUE, TRUE, FALSE, TRUE));
        SetPending(TRUE);
    }
}

void CWeapon::OnH_B_Chield()
{
    m_dwWeaponIndependencyTime = 0;
    inherited::OnH_B_Chield();

    OnZoomOut();
    m_set_next_ammoType_on_reload = undefined_ammo_type;
    m_nearwall_last_hud_fov = psHUD_FOV_def;
}

bool CWeapon::AllowBore() { return true; }
void CWeapon::UpdateCL()
{
    inherited::UpdateCL();
    UpdateHUDAddonsVisibility();
    //подсветка от выстрела
    UpdateLight();

    //нарисовать партиклы
    UpdateFlameParticles();
    UpdateFlameParticles2();

    if (!IsGameTypeSingle())
        make_Interpolation();

    if ((GetNextState() == GetState()) && IsGameTypeSingle() && H_Parent() == Level().CurrentEntity())
    {
        CActor* pActor = smart_cast<CActor*>(H_Parent());
        if (pActor && !pActor->AnyMove() && this == pActor->inventory().ActiveItem())
        {
            if (!GamePersistent().GetHudTuner().is_active() && GetState() == eIdle && (Device.dwTimeGlobal - m_dw_curr_substate_time > 20000) &&
                (!IsZoomed() && !IsSecondZoomed()) && g_player_hud[1]->attached_item() == nullptr)
            {
                if (AllowBore())
                    SwitchState(eBore);

                ResetSubStateTime();
            }
        }
    }

    if (m_zoom_params.m_pNight_vision && !need_renderable())
    {
        if (!m_zoom_params.m_pNight_vision->IsActive())
        {
            CActor* pA = smart_cast<CActor*>(H_Parent());
            R_ASSERT(pA);
            CTorch* pTorch = smart_cast<CTorch*>(pA->inventory().ItemFromSlot(TORCH_SLOT));
            if (pTorch && pTorch->GetNightVisionStatus())
            {
                m_bRememberActorNVisnStatus = pTorch->GetNightVisionStatus();
                pTorch->SwitchNightVision(false, false);
            }
            m_zoom_params.m_pNight_vision->Start(m_zoom_params.m_sUseZoomPostprocess, pA, false);
        }
    }
    else if (m_bRememberActorNVisnStatus)
    {
        m_bRememberActorNVisnStatus = false;
        EnableActorNVisnAfterZoom();
    }

    if (m_zoom_params.m_pVision)
        m_zoom_params.m_pVision->Update();
}
void CWeapon::EnableActorNVisnAfterZoom()
{
    CActor* pA = smart_cast<CActor*>(H_Parent());
    if (IsGameTypeSingle() && !pA)
        pA = g_actor;

    if (pA)
    {
        CTorch* pTorch = smart_cast<CTorch*>(pA->inventory().ItemFromSlot(TORCH_SLOT));
        if (pTorch)
        {
            pTorch->SwitchNightVision(true, false);
            pTorch->GetNightVision()->PlaySounds(CNightVisionEffector::eIdleSound);
        }
    }
}

bool CWeapon::need_renderable()
{
    return !Device.m_SecondViewport.IsSVPFrame() && !(IsZoomed() && ZoomTexture() && !IsRotatingToZoom());
}
void CWeapon::renderable_Render(u32 context_id, IRenderable* root)
{
    ScopeLock lock{ &render_lock };

    UpdateXForm();

    //нарисовать подсветку

    RenderLight();

    //если мы в режиме снайперки, то сам HUD рисовать не надо
    if (IsZoomed() && !IsRotatingToZoom() && ZoomTexture())
        RenderHud(FALSE);
    else
        RenderHud(TRUE);

    if (bUseAttachmentSystem)
    {
        Fmatrix m_item_transform = XFORM();
        for (auto [addon_id, item]: m_addon_items)
        {
            item->addon_item_transform.mul(m_item_transform, item->addon_item_pos_world);

            GEnv.Render->add_Visual(context_id, root, item->addon_item_model->dcast_RenderVisual(), item->addon_item_transform);
        }
    }

    inherited::renderable_Render(context_id, root);
}

void CWeapon::signal_HideComplete()
{
    if (H_Parent())
        setVisible(FALSE);
    SetPending(FALSE);

    m_fLR_MovingFactor = 0.f;
    m_fLR_CameraFactor = 0.f;
    m_fLR_InertiaFactor = 0.f;
    m_fUD_InertiaFactor = 0.f;
}

void CWeapon::SetDefaults()
{
    SetPending(FALSE);

    m_flags.set(FUsingCondition, TRUE);
    bMisfire = false;
    m_flagsAddOnState = 0;
    m_zoom_params.m_bIsZoomModeNow = false;
    m_zoom_params.m_bIsZoomSecondModeNow = false;
}

void CWeapon::UpdatePosition(const Fmatrix& trans)
{
    Position().set(trans.c);
    XFORM().mul(trans, m_strapped_mode ? m_StrapOffset : m_Offset);
    VERIFY(!fis_zero(DET(renderable.xform)));
}

bool CWeapon::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    switch (cmd)
    {
    case kWPN_NV_CHANGE:
    {
        return bChangeNVSecondVPStatus();
    }
    case kWPN_FIRE:
    {
        //если оружие чем-то занято, то ничего не делать
        {
            if (IsPending())
                return false;

            if (flags & CMD_START)
            {
                if (ParentIsActor() && !smart_cast<CWeaponKnife*>(this)) // for knife it is handled differently
                {
                    const bool left = IsBinded(kWPN_FIRE, XR_CONTROLLER_AXIS_TRIGGER_LEFT);
                    const bool right = IsBinded(kWPN_FIRE, XR_CONTROLLER_AXIS_TRIGGER_RIGHT);
                    pInput->Feedback(CInput::FeedbackTriggers, left ? 0.5f : 0.0f, right ? 0.5f : 0.0f, 0.1f);
                }
                FireStart();
            }
            else
            {
                FireEnd();
            }
            return true;
        };
        return false;
    }

    case kWPN_NEXT: { return SwitchAmmoType(flags); }

    case kWPN_ZOOM_SECOND:
        if (IsZoomSecondEnabled())
        {
            if (b_toggle_weapon_aim)
            {
                if (flags & CMD_START)
                {
                    if (!IsSecondZoomed())
                    {
                        if (!IsPending())
                        {
                            if (GetState() != eAimStart && isHUDAnimationExist("anm_idle_aim_start"))
                                SwitchState(eAimStart);
                            else if (GetState() != eIdle)
                                SwitchState(eIdle);

                            OnZoomSecondIn();
                        }
                    }
                    else
                    {
                        if (GetState() != eAimEnd && isHUDAnimationExist("anm_idle_aim_end"))
                            SwitchState(eAimEnd);

                        OnZoomSecondOut();
                    }
                }
            }
            else
            {
                if (flags & CMD_START)
                {
                    if (!IsSecondZoomed() && !IsPending())
                    {
                        if (GetState() != eAimStart && isHUDAnimationExist("anm_idle_aim_start"))
                            SwitchState(eAimStart);
                        else if (GetState() != eIdle)
                            SwitchState(eIdle);

                        OnZoomSecondIn();
                    }
                }
                else if (IsSecondZoomed())
                {
                    if (GetState() != eAimEnd && isHUDAnimationExist("anm_idle_aim_end"))
                        SwitchState(eAimEnd);

                    OnZoomSecondOut();
                }
            }
            return true;
        }
        else
            return false;
    case kWPN_ZOOM:
        if (IsZoomEnabled())
        {
            if (b_toggle_weapon_aim)
            {
                if (flags & CMD_START)
                {
                    if (!IsZoomed())
                    {
                        if (!IsPending())
                        {
                            if (GetState() != eAimStart && isHUDAnimationExist("anm_idle_aim_start"))
                                SwitchState(eAimStart);
                            else if (GetState() != eIdle)
                                SwitchState(eIdle);

                            OnZoomIn();
                        }
                    }
                    else
                    {
                        if (GetState() != eAimEnd && isHUDAnimationExist("anm_idle_aim_end"))
                            SwitchState(eAimEnd);

                        OnZoomFirstOut();
                    }
                }
            }
            else
            {
                if (flags & CMD_START)
                {
                    if (!IsZoomed() && !IsPending())
                    {
                        if (GetState() != eAimStart && isHUDAnimationExist("anm_idle_aim_start"))
                            SwitchState(eAimStart);
                        else if (GetState() != eIdle)
                            SwitchState(eIdle);

                        OnZoomIn();
                    }
                }
                else if (IsZoomed())
                {
                    if (GetState() != eAimEnd && isHUDAnimationExist("anm_idle_aim_end"))
                        SwitchState(eAimEnd);

                    OnZoomFirstOut();
                }
            }
            return true;
        }
        else
            return false;

    case kWPN_ZOOM_INC:
    case kWPN_ZOOM_DEC:
        if (((IsZoomEnabled() && IsZoomed()) || (IsZoomSecondEnabled() && IsSecondZoomed())) && (flags&CMD_START) )
        {
            if (cmd == kWPN_ZOOM_INC)
                ZoomInc();
            else
                ZoomDec();
            return true;
        }
        else
            return false;
    }
    return false;
}

bool CWeapon::SwitchAmmoType(u32 flags)
{
    if (IsPending() || OnClient())
        return false;

    if (!(flags & CMD_START))
        return false;

    u8 l_newType = m_ammoType;
    bool b1, b2;
    do
    {
        l_newType = u8((u32(l_newType + 1)) % m_ammoTypes.size());
        b1 = (l_newType != m_ammoType);
        b2 = unlimited_ammo() ? false : (!m_pInventory->GetAny(m_ammoTypes[l_newType].c_str()));
    } while (b1 && b2);

    if (l_newType != m_ammoType)
    {
        m_set_next_ammoType_on_reload = l_newType;
        if (OnServer())
        {
            Reload();
        }
    }
    return true;
}

void CWeapon::SpawnAmmo(u32 boxCurr, LPCSTR ammoSect, u32 ParentID)
{
    if (!m_ammoTypes.size())
        return;
    if (OnClient())
        return;
    m_bAmmoWasSpawned = true;

    int l_type = 0;
    l_type %= m_ammoTypes.size();

    if (!ammoSect)
        ammoSect = m_ammoTypes[l_type].c_str();

    ++l_type;
    l_type %= m_ammoTypes.size();

    CSE_Abstract* D = F_entity_Create(ammoSect);

    {
        CSE_ALifeItemAmmo* l_pA = smart_cast<CSE_ALifeItemAmmo*>(D);
        R_ASSERT(l_pA);
        l_pA->m_boxSize = (u16)pSettings->r_s32(ammoSect, "box_size");
        D->s_name = ammoSect;
        D->set_name_replace("");
        //.     D->s_gameid                 = u8(GameID());
        D->s_RP = 0xff;
        D->ID = 0xffff;
        if (ParentID == 0xffffffff)
            D->ID_Parent = (u16)H_Parent()->ID();
        else
            D->ID_Parent = (u16)ParentID;

        D->ID_Phantom = 0xffff;
        D->s_flags.assign(M_SPAWN_OBJECT_LOCAL);
        D->RespawnTime = 0;
        l_pA->m_tNodeID = GEnv.isDedicatedServer ? u32(-1) : ai_location().level_vertex_id();

        if (boxCurr == 0xffffffff)
            boxCurr = l_pA->m_boxSize;

        while (boxCurr)
        {
            l_pA->a_elapsed = (u16)(boxCurr > l_pA->m_boxSize ? l_pA->m_boxSize : boxCurr);
            NET_Packet P;
            D->Spawn_Write(P, TRUE);
            Level().Send(P, net_flags(TRUE));

            if (boxCurr > l_pA->m_boxSize)
                boxCurr -= l_pA->m_boxSize;
            else
                boxCurr = 0;
        }
    }
    F_entity_Destroy(D);
}

int CWeapon::GetSuitableAmmoTotal(bool use_item_to_spawn) const
{
    int ae_count = iAmmoElapsed;
    if (!m_pInventory)
    {
        return ae_count;
    }

    //чтоб не делать лишних пересчетов
    if (m_pInventory->ModifyFrame() <= m_BriefInfo_CalcFrame)
    {
        return ae_count + m_iAmmoCurrentTotal;
    }
    m_BriefInfo_CalcFrame = Device.dwFrame;

    m_iAmmoCurrentTotal = 0;
    for (u8 i = 0; i < u8(m_ammoTypes.size()); ++i)
    {
        m_iAmmoCurrentTotal += GetAmmoCount_forType(m_ammoTypes[i]);

        if (!use_item_to_spawn)
        {
            continue;
        }
        if (!inventory_owner().item_to_spawn())
        {
            continue;
        }
        m_iAmmoCurrentTotal += inventory_owner().ammo_in_box_to_spawn();
    }
    return ae_count + m_iAmmoCurrentTotal;
}

int CWeapon::GetAmmoCount(u8 ammo_type) const
{
    VERIFY(m_pInventory);
    R_ASSERT(ammo_type < m_ammoTypes.size());

    return GetAmmoCount_forType(m_ammoTypes[ammo_type]);
}

int CWeapon::GetAmmoCount_forType(shared_str const& ammo_type) const
{
    int res = 0;

    TIItemContainer::iterator itb = m_pInventory->m_belt.begin();
    TIItemContainer::iterator ite = m_pInventory->m_belt.end();
    for (; itb != ite; ++itb)
    {
        CWeaponAmmo* pAmmo = smart_cast<CWeaponAmmo*>(*itb);
        if (pAmmo && (pAmmo->cNameSect() == ammo_type))
        {
            res += pAmmo->m_boxCurr;
        }
    }

    itb = m_pInventory->m_ruck.begin();
    ite = m_pInventory->m_ruck.end();
    for (; itb != ite; ++itb)
    {
        CWeaponAmmo* pAmmo = smart_cast<CWeaponAmmo*>(*itb);
        if (pAmmo && (pAmmo->cNameSect() == ammo_type))
        {
            res += pAmmo->m_boxCurr;
        }
    }
    return res;
}

void CWeapon::OnMoveToRuck(const SInvItemPlace& previous_place)
{
    inherited::OnMoveToRuck(previous_place);
    b_forceIconUpdate = false;
}

float CWeapon::GetConditionMisfireProbability() const
{
    float mis;
    if (misfireUseOldFormula)
    {
        if (GetCondition() > 0.95f)
            return 0.0f;
        mis = misfireProbability + powf(1.f - GetCondition(), 3.f) * misfireConditionK;
    }
    else // modified by Peacemaker [17.10.08]
    {
        if (GetCondition() > misfireStartCondition)
            return 0.0f;
        if (GetCondition() < misfireEndCondition)
            return misfireEndProbability;
        mis = misfireStartProbability +
            ((misfireStartCondition - GetCondition()) * // condition goes from 1.f to 0.f
                (misfireEndProbability - misfireStartProbability) / // probability goes from 0.f to 1.f
                ((misfireStartCondition == misfireEndCondition) ? // !!!say "No" to devision by zero
                    misfireStartCondition :
                    (misfireStartCondition - misfireEndCondition)));
    }
    clamp(mis, 0.0f, 0.99f);
    return mis;
}

BOOL CWeapon::CheckForMisfire()
{
    if (OnClient())
        return FALSE;

    float rnd = ::Random.randF(0.f, 1.f);
    float mp = GetConditionMisfireProbability();
    if (rnd < mp)
    {
        FireEnd();

        bMisfire = true;
        SwitchState(eMisfire);

        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

void CWeapon::HUD_VisualBulletUpdate(bool force, int force_idx)
{
    if (!bHasBulletsToHide)
        return;

    if (!GetHUDmode())
        return;

    bool hide = true;

    if (last_hide_bullet == bullet_cnt || force) hide = false;

    for (u8 b = 0; b < bullet_cnt; b++)
    {
        u16 bone_id = HudItemData()->m_model->LL_BoneID(bullets_bones[b]);

        if (bone_id != BI_NONE)
            HudItemData()->set_bone_visible(bullets_bones[b], !hide);

        if (b == last_hide_bullet) hide = false;
    }
}

BOOL CWeapon::IsMisfire() const { return bMisfire; }
void CWeapon::Reload()
{
    if (IsZoomed())
        OnZoomFirstOut();
    else if (IsSecondZoomed())
        OnZoomSecondOut();
}
bool CWeapon::IsGrenadeLauncherAttached() const
{
    return (ALife::eAddonAttachable == m_eGrenadeLauncherStatus &&
               0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher)) ||
        ALife::eAddonPermanent == m_eGrenadeLauncherStatus;
}

ALife::EWeaponAddonStatus CWeapon::GetScopeStatusParent() const
{
    pcstr section = pSettings->read_if_exists<pcstr>(m_section_id.c_str(), "parent_section", m_section_id.c_str());
    return (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "scope_status");
}
bool CWeapon::IsScopePermament() const
{
    return pSettings->r_s32(m_section_id, "scope_status") == ALife::eAddonPermanent;
}
bool CWeapon::IsScopeAttached() const
{
    if (bUseAttachmentSystem && mainScopeSlotIsBusy())
        return true;
    if (ALife::eAddonAttachable == m_eScopeStatus)
    {
        if (0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope))
            return true;
    }
    if (ALife::eAddonPermanent == m_eScopeStatus)
        return true;
    if (IsScopePermament())
        return true;

    return false;
}

bool CWeapon::IsSilencerAttached() const
{
    return (ALife::eAddonAttachable == m_eSilencerStatus &&
               0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer)) ||
        ALife::eAddonPermanent == m_eSilencerStatus;
}

bool CWeapon::mainScopeSlotIsBusy() const
{
    for (auto [addon_id, addon]: m_addon_items)
    {
        if (addon->has_scope_texture)
            return true;

        if (addon->parent_id == 0)
        {
            Fvector hpb;
            m_addon_slots[addon->slot]->transform.getHPB(hpb.x, hpb.y, hpb.z);
            if (fis_zero(hpb.z))
            {
                if (xr_strcmp(addon->addon_type.c_str(), "attachment") == 0)
                {
                    for (auto [addon_id2, addon2]: m_addon_items)
                        if (addon2->parent_id == addon_id && xr_strcmp(addon2->addon_type.c_str(), "attachment") != 0)
                            return true;
                }
                else
                    return true;
            }
        }
    }

    return false;
}

bool CWeapon::GrenadeLauncherAttachable() { return (ALife::eAddonAttachable == m_eGrenadeLauncherStatus); }
bool CWeapon::ScopeAttachable()
{
    if (ALife::eAddonAttachable == m_eScopeStatus)
        return true;
    if (pSettings->line_exist(m_section_id.c_str(), "parent_section"))
    {
        shared_str parent = pSettings->r_string(m_section_id.c_str(), "parent_section");
        if (pSettings->line_exist(parent.c_str(), "scope_status"))
        {
            if (pSettings->r_s32(parent.c_str(), "scope_status") == ALife::EWeaponAddonStatus::eAddonAttachable)
                return true;
        }
    }
    return false;
}
bool CWeapon::SilencerAttachable() { return (ALife::eAddonAttachable == m_eSilencerStatus); }


void CWeapon::UpdateHUDAddonsVisibility()
{
    if (GamePersistent().GetHudTuner().is_active())
        return;
    static shared_str wpn_scope = WPN_SCOPE;
    static shared_str wpn_silencer = WPN_SILENCER;
    static shared_str wpn_grenade_launcher = WPN_GRENADE_LAUNCHER;
    static shared_str wpn_grenade_launcher_soc = WPN_GRENADE_LAUNCHER_SOC;

    // actor only
    if (!GetHUDmode())
        return;

    //. return;

    u16 bone_id = HudItemData()->m_model->LL_BoneID(wpn_scope);

    if (bone_id != BI_NONE)
    {
        if (ScopeAttachable())
        {
            HudItemData()->set_bone_visible(wpn_scope, IsScopeAttached());
        }

        if (m_eScopeStatus == ALife::eAddonDisabled)
        {
            HudItemData()->set_bone_visible(wpn_scope, FALSE, TRUE);
        }
        else if (IsScopePermament())
            HudItemData()->set_bone_visible(wpn_scope, TRUE, TRUE);

        if (bUseAttachmentSystem)
        {
            if (mainScopeSlotIsBusy())
                HudItemData()->set_bone_visible(wpn_scope, FALSE, TRUE);
            else
                HudItemData()->set_bone_visible(wpn_scope, TRUE, TRUE);
        }
    }

    if (SilencerAttachable())
    {
        HudItemData()->set_bone_visible(wpn_silencer, IsSilencerAttached());
    }
    if (m_eSilencerStatus == ALife::eAddonDisabled)
    {
        HudItemData()->set_bone_visible(wpn_silencer, FALSE, TRUE);
    }
    else if (m_eSilencerStatus == ALife::eAddonPermanent)
        HudItemData()->set_bone_visible(wpn_silencer, TRUE, TRUE);

    bool use_soc_name{};
    if (HudItemData()->m_model->LL_BoneID(wpn_grenade_launcher) == BI_NONE)
        use_soc_name = HudItemData()->m_model->LL_BoneID(wpn_grenade_launcher_soc) != BI_NONE;

    if (GrenadeLauncherAttachable())
    {
        HudItemData()->set_bone_visible((use_soc_name ? wpn_grenade_launcher_soc : wpn_grenade_launcher), IsGrenadeLauncherAttached());
    }
    if (m_eGrenadeLauncherStatus == ALife::eAddonDisabled)
    {
        HudItemData()->set_bone_visible((use_soc_name ? wpn_grenade_launcher_soc : wpn_grenade_launcher), FALSE, TRUE);
    }
    else if (m_eGrenadeLauncherStatus == ALife::eAddonPermanent)
        HudItemData()->set_bone_visible((use_soc_name ? wpn_grenade_launcher_soc : wpn_grenade_launcher), TRUE, TRUE);
}

void CWeapon::UpdateAddonsOffset()
{
    if (m_eScopeStatus == ALife::eAddonAttachable || mainScopeSlotIsBusy())
    {
        if (pSettings->line_exist(m_section_id, "scope_name"))
            m_sScopeName = pSettings->r_string(m_section_id, "scope_name");
        if (pSettings->line_exist(m_section_id, "scope_x"))
            m_iScopeX = pSettings->r_s32(m_section_id, "scope_x");
        if (pSettings->line_exist(m_section_id, "scope_y"))
            m_iScopeY = pSettings->r_s32(m_section_id, "scope_y");
    }

    if (m_eSilencerStatus == ALife::eAddonAttachable)
    {
        if (pSettings->line_exist(m_section_id, "silencer_name"))
            m_sSilencerName = pSettings->r_string(m_section_id, "silencer_name");
        if (pSettings->line_exist(m_section_id, "silencer_x"))
            m_iSilencerX = pSettings->r_s32(m_section_id, "silencer_x");
        if (pSettings->line_exist(m_section_id, "silencer_y"))
            m_iSilencerY = pSettings->r_s32(m_section_id, "silencer_y");
    }

    if (m_eGrenadeLauncherStatus == ALife::eAddonAttachable)
    {
        if (pSettings->line_exist(m_section_id, "grenade_launcher_name"))
            m_sGrenadeLauncherName = pSettings->r_string(m_section_id, "grenade_launcher_name");
        if (pSettings->line_exist(m_section_id, "grenade_launcher_x"))
            m_iGrenadeLauncherX = pSettings->r_s32(m_section_id, "grenade_launcher_x");
        if (pSettings->line_exist(m_section_id, "grenade_launcher_y"))
            m_iGrenadeLauncherY = pSettings->r_s32(m_section_id, "grenade_launcher_y");
    }
}
void CWeapon::LoadAltHudAim()
{
    auto sectionNeedLoad = IsScopePermament() ? m_section_id : IsScopeAttached() ? GetNameWithAttachment() : m_section_id;

    if (bUseAttachmentSystem)
        sectionNeedLoad = m_section_id;

    R_ASSERT3(pSettings->section_exist(sectionNeedLoad), "Section doesn't exist", sectionNeedLoad.c_str());

    if (!bUseAttachmentSystem)
        m_zoom_params.m_bZoomSecondEnabled = READ_IF_EXISTS(pSettings, r_bool, sectionNeedLoad, "use_alt_aim_hud", false);

    if (m_zoom_params.m_bZoomSecondEnabled && !bUseAttachmentSystem)
    {
        const bool is_16x9 = UICore::is_widescreen();

        string64 hud_sect;
        string64 base_hud_sect;
        string128 val_name;
        string64 _prefix;

        xr_sprintf(_prefix, "%s", is_16x9 ? "_16x9" : "");
        xr_sprintf(hud_sect, "%s_hud", sectionNeedLoad.c_str());
        xr_sprintf(base_hud_sect, "%s_hud", m_section_id.c_str());

        strconcat(sizeof(val_name), val_name, "aim_hud_offset_alt_pos", _prefix);
        if (pSettings->line_exist(hud_sect, val_name))
            m_hands_offset[0][1] = pSettings->r_fvector3(hud_sect, val_name);
        else if (pSettings->line_exist(base_hud_sect, val_name))
            m_hands_offset[0][1] = pSettings->r_fvector3(base_hud_sect, val_name);
        strconcat(sizeof(val_name), val_name, "aim_hud_offset_alt_rot", _prefix);
        if (pSettings->line_exist(hud_sect, val_name))
            m_hands_offset[1][1] = pSettings->r_fvector3(hud_sect, val_name);
        else if (pSettings->line_exist(base_hud_sect, val_name))
            m_hands_offset[1][1] = pSettings->r_fvector3(base_hud_sect, val_name);
    }
}
void CWeapon::LoadAddonSlosts(LPCSTR section)
{
    if (bUseAttachmentSystem)
    {
        auto load_slot = [&](shared_str line_name, shared_str slot_key) {
            if (!pSettings->line_exist(section, line_name.c_str()))
                return;

            addon_slot* slot = xr_new<addon_slot>();
            shared_str line_name_world = make_string("%s_world", line_name.c_str()).c_str();
            pcstr str = pSettings->r_string(section, line_name.c_str());
            string128 bone_str = "";
            string128 bone_name = "";
            string128 bone_2_name = "";
            u16 slot_type;
            Fvector3 pos = {0.f, 0.f, 0.f};
            Fvector3 rot = {0.f, 0.f, 0.f};
            sscanf(str, "%hu,%f,%f,%f,%f,%f,%f,%s", &slot_type, &pos.x, &pos.y, &pos.z, &rot.x, &rot.y, &rot.z, &bone_str);
            slot->transform_2 = Fmatrix().identity();
            xr_strcpy(bone_name, bone_str);
            if (strstr(bone_str, ","))
            {
                _GetItem(bone_str, 1, bone_2_name);
                _GetItem(bone_str, 0, bone_name);

                shared_str line_name_pos_2 = make_string("%s_2", line_name.c_str()).c_str();
                if (pSettings->line_exist(section, line_name_pos_2.c_str()))
                {
                    Fvector3 pos_2 = {0.f, 0.f, 0.f};
                    Fvector3 rot_2 = {0.f, 0.f, 0.f};
                    str = pSettings->r_string(section, line_name_pos_2.c_str());
                    sscanf(str, "%f,%f,%f,%f,%f,%f", &pos_2.x, &pos_2.y, &pos_2.z, &rot_2.x, &rot_2.y, &rot_2.z);

                    Fmatrix trans_2;
                    trans_2.setHPB(rot_2.x, rot_2.y, rot_2.z);
                    trans_2.translate_over(pos_2.x, pos_2.y, pos_2.z);

                    slot->transform_2 = trans_2;
                }
            }
            Fvector3 pos_w = pos;
            Fvector3 rot_w = rot;
            if (pSettings->line_exist(section, line_name_world.c_str()))
            {
                str = pSettings->r_string(section, line_name_world.c_str());
                sscanf(str, "%f,%f,%f,%f,%f,%f", &pos_w.x, &pos_w.y, &pos_w.z, &rot_w.x, &rot_w.y, &rot_w.z);
            }
            Fmatrix trans;
            trans.setHPB(rot.x, rot.y, rot.z);
            trans.translate_over(pos.x, pos.y, pos.z);

            Fmatrix trans_w;
            trans_w.setHPB(rot_w.x, rot_w.y, rot_w.z);
            trans_w.translate_over(pos_w.x, pos_w.y, pos_w.z);

            slot->slot_name = slot_key;
            slot->transform = trans;
            slot->transform_world = trans_w;
            slot->parent = 0;
            slot->slot_type = slot_type;
            slot->bone_name = bone_name;
            slot->bone_2_name = bone_2_name;
            m_addon_slots[slot_key] = slot;
        };
        auto load_slot_offsets = [&](const char* format) {
            u16 index = 1;
            shared_str line_name;
            shared_str slot_key;
            
            while (true)
            {
                line_name = make_string(format, index).c_str();
                if (!pSettings->line_exist(section, line_name.c_str()))
                    break;

                slot_key = make_string("slot_%d", index).c_str();

                load_slot(line_name, slot_key);

                index++;
            }
        };

        load_slot_offsets("addon_slot_%d_offset");
        load_slot("addon_slot_bh_offset", "slot_bh");
        load_slot("addon_slot_mag_offset", "slot_mag");
        load_slot("addon_slot_dtk_offset", "slot_dtk");
        load_slot("addon_slot_grip_offset", "slot_grip");
        load_slot("addon_slot_sight_offset", "slot_sight");
        load_slot("addon_slot_tac_grip_offset", "slot_tac_grip");
        load_slot("addon_slot_cover_offset", "slot_cover");
        load_slot("addon_slot_cev_up_offset", "slot_cev_up");
        load_slot("addon_slot_cev_down_offset", "slot_cev_down");
    }
}
shared_str CWeapon::GetSlotKey(shared_str slot_name, u32 addon_parent_id, u32 addon_id)
{
    if (m_addon_items[addon_parent_id]->parent_id == 0)
    {
        return make_string("%s.%s.%s", m_addon_items[addon_parent_id]->slot.c_str(), m_addon_items[addon_parent_id]->addon_item_name.c_str(), slot_name.c_str()).c_str();
    }
    else
    {
        shared_str key = make_string("%s.%s.%s", m_addon_items[addon_parent_id]->slot.c_str(), m_addon_items[addon_parent_id]->addon_item_name.c_str(), slot_name.c_str()).c_str();
        return GetSlotKey(key, m_addon_items[addon_parent_id]->parent_id, addon_parent_id).c_str();
    }
}

void CWeapon::SpawnDefaultAddons()
{
    if (default_addons_was_loaded)
        return;
    for (auto [slot_key, slot] : m_addon_slots)
    {
        shared_str default_addon = "";
        if (pSettings->line_exist(m_section_id.c_str(), slot_key.c_str()))
            default_addon = pSettings->r_string(m_section_id.c_str(), slot_key.c_str());
        if (pSettings->section_exist(default_addon.c_str()))
        {
            auto addon = GetAddonFromSlot(0, slot_key);
            if (addon.second)
                continue;

            AddAddonData data;
            data.item_section_id = default_addon;
            data.addon_type = pSettings->r_string(default_addon.c_str(), "addon_type");
            data.slot_name = slot_key;
            data.ort = CInventoryItem::EIIAddonOrt::FOrtNone;
            data.addon_id = ADDON_ID_NONE;
            data.parent_id = 0;
            if (pSettings->line_exist(default_addon.c_str(), "provided_slot_type"))
                data.provided_slot_type = (CWeapon::EWeaponAddonSlotType)pSettings->r_u8(default_addon.c_str(), "provided_slot_type");
            else
                data.provided_slot_type = CWeapon::EWeaponAddonSlotType::eNone;

            if (pSettings->line_exist(default_addon.c_str(), "ammo_mag_size"))
                iAmmoElapsed = pSettings->r_s32(default_addon.c_str(), "ammo_mag_size");

            addAddon(data);
        }
    }

    auto load_attachment = [&](const char* slot_name, u32 parent, const char* key) {
        shared_str addon_str = pSettings->r_string(m_section_id.c_str(), key);
        string128 addon_section;
        string128 addon_ort = "";
        u16 count = _GetItemCount(addon_str.c_str(), '|');
        _GetItem(addon_str.c_str(), 0, addon_section, '|');
        if (count > 1)
            _GetItem(addon_str.c_str(), 1, addon_ort, '|');

        R_ASSERT2(pSettings->section_exist(addon_section), make_string("Section of addon: %s doesn't exist", addon_section).c_str());

        AddAddonData data;
        data.item_section_id = addon_section;
        data.addon_type = pSettings->r_string(addon_section, "addon_type");
        data.slot_name = slot_name;
        data.ort = xr_strcmp(addon_ort, "") == 0 ? CInventoryItem::EIIAddonOrt::FOrtNone : xr_strcmp(addon_ort, "left") == 0 ? CInventoryItem::EIIAddonOrt::FOrtLeft : CInventoryItem::EIIAddonOrt::FOrtRight;
        data.addon_id = ADDON_ID_NONE;
        data.parent_id = parent;
        if (pSettings->line_exist(addon_section, "provided_slot_type"))
        data.provided_slot_type = (CWeapon::EWeaponAddonSlotType)pSettings->r_u8(addon_section, "provided_slot_type");
        else
        data.provided_slot_type = CWeapon::EWeaponAddonSlotType::eNone;
        
        if (data.ort != CInventoryItem::EIIAddonOrt::FOrtNone)
            data.has_ort = true;

        addAddon(data);
    };

    bool addon_with_slot_has_been_added = false;

    while (true)
    {
        addon_with_slot_has_been_added = false;

        for (auto [addon_id, addon] : m_addon_items)
        {
            if (addon->addon_slots.size() == 0)
                continue;
            if (addon->was_inited_in_default_slots)
                continue;
            
            for (auto [slot_name, slot] : addon->addon_slots)
            {
                shared_str key = make_string("%s.%s.%s", addon->slot.c_str(), addon->addon_item_name.c_str(), slot_name.c_str()).c_str();
                if (addon->parent_id != 0)
                    key = GetSlotKey(key.c_str(), addon->parent_id, addon_id);

                if (pSettings->line_exist(m_section_id.c_str(), key.c_str()))
                {
                    shared_str addon_str = pSettings->r_string(m_section_id.c_str(), key.c_str());
                    string128 addon_section;
                    _GetItem(addon_str.c_str(), 0, addon_section, '|');

                    load_attachment(slot.slot_name.c_str(), addon_id, key.c_str());

                    addon_with_slot_has_been_added = true;
                }
            }

            addon->was_inited_in_default_slots = true;
        }

        if (!addon_with_slot_has_been_added)
            break;
    }

    default_addons_was_loaded = true;
}

void CWeapon::UpdateAddonsVisibility()
{
    if (GamePersistent().GetHudTuner().is_active())
        return;

    static shared_str wpn_scope = WPN_SCOPE;
    static shared_str wpn_silencer = WPN_SILENCER;
    static shared_str wpn_grenade_launcher = WPN_GRENADE_LAUNCHER;
    static shared_str wpn_grenade_launcher_soc = WPN_GRENADE_LAUNCHER_SOC;

    IKinematics* pWeaponVisual = smart_cast<IKinematics*>(Visual());
    R_ASSERT(pWeaponVisual);

    u16 bone_id;
    UpdateHUDAddonsVisibility();

    pWeaponVisual->CalculateBones_Invalidate();

    bone_id = pWeaponVisual->LL_BoneID(wpn_scope);
    if (ScopeAttachable() && bone_id != BI_NONE)
    {
        if (IsScopeAttached() || IsScopePermament())
        {
            if (!pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
        else
        {
            if (pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
        }

        if (bUseAttachmentSystem)
        {
            if (mainScopeSlotIsBusy() && pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
            else
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
    }
    if (m_eScopeStatus == ALife::eAddonDisabled && bone_id != BI_NONE && pWeaponVisual->LL_GetBoneVisible(bone_id))
    {
        pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
        //      Log("scope", pWeaponVisual->LL_GetBoneVisible       (bone_id));
    }
    bone_id = pWeaponVisual->LL_BoneID(wpn_silencer);
    if (SilencerAttachable() && bone_id != BI_NONE)
    {
        if (IsSilencerAttached())
        {
            if (!pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
        else
        {
            if (pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
        }
    }
    if (m_eSilencerStatus == ALife::eAddonDisabled && bone_id != BI_NONE && pWeaponVisual->LL_GetBoneVisible(bone_id))
    {
        pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
        //      Log("silencer", pWeaponVisual->LL_GetBoneVisible    (bone_id));
    }

    bone_id = pWeaponVisual->LL_BoneID(wpn_grenade_launcher);
    if (bone_id == BI_NONE)
        bone_id = pWeaponVisual->LL_BoneID(wpn_grenade_launcher_soc);

    if (GrenadeLauncherAttachable() && bone_id != BI_NONE)
    {
        if (IsGrenadeLauncherAttached())
        {
            if (!pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, TRUE, TRUE);
        }
        else
        {
            if (pWeaponVisual->LL_GetBoneVisible(bone_id))
                pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
        }
    }
    if (m_eGrenadeLauncherStatus == ALife::eAddonDisabled && bone_id != BI_NONE &&
        pWeaponVisual->LL_GetBoneVisible(bone_id))
    {
        pWeaponVisual->LL_SetBoneVisible(bone_id, FALSE, TRUE);
        //      Log("gl", pWeaponVisual->LL_GetBoneVisible          (bone_id));
    }

    pWeaponVisual->CalculateBones_Invalidate();
    pWeaponVisual->CalculateBones(TRUE);
}

void CWeapon::InitAddons() {}
float CWeapon::CurrentZoomFactor()
{
    if (psActorFlags.test(AF_3DSCOPE) && (IsScopeAttached() || IsScopePermament()))
        return m_zoom_params.m_f3dZoomFactor;

    if (IsScopePermament())
        return m_zoom_params.m_fScopeZoomFactor;

    return IsScopeAttached() ? m_zoom_params.m_fScopeZoomFactor : m_zoom_params.m_fIronSightZoomFactor;
};

// Чувствительность мышкии с оружием в руках
float CWeapon::GetControlInertionFactor() const
{
    float fInertionFactor = inherited::GetControlInertionFactor();
    if (IsScopeAttached() && (IsZoomed() || IsSecondZoomed()))
        return m_fScopeInertionFactor;

    return fInertionFactor;
}

void CWeapon::GetZoomData(const float scope_factor, float& delta, float& min_zoom_factor)
{
    float def_fov = bIsSecondVPZoomPresent() ? 75.0f : g_fov;//float(g_fov);
    float delta_factor_total = def_fov - scope_factor;
    VERIFY(delta_factor_total > 0);
    min_zoom_factor = def_fov - delta_factor_total * m_fZoomMinKoeff;
    delta = (delta_factor_total * (1 - m_fZoomMinKoeff)) / m_fZoomStepCount;
}

void CWeapon::OnZoomSecondIn()
{ 
    if (IsZoomed())
        m_zoom_params.m_iLatestZoomType = EWeaponLatestZoom::eMainZoom;
    m_zoom_params.m_bIsZoomModeNow = false;
    m_zoom_params.m_bIsZoomSecondModeNow = true;

    m_zoom_params.m_fSecondZoomRotationFactor = 0.f;
    if (m_zoom_params.m_fZoomRotationFactor == 1.f)
        m_zoom_params.m_fZoomRotationFactor = 0.f;

    SetZoomFactor(m_zoom_params.m_fSecondScopeZoomFactor);

    if (m_zoom_params.m_bZoomDofEnabled && !IsScopeAttached())
        GamePersistent().SetEffectorDOF(m_zoom_params.m_ZoomDof);

    if (GetHUDmode())
        GamePersistent().SetPickableEffectorDOF(true);

    PlayCamAnim("cam_anm_aim_in");
    g_player_hud[1]->set_detector_state(EHudStates::eWpnZoomStart);
}
void CWeapon::OnZoomIn()
{
    if (IsSecondZoomed())
        m_zoom_params.m_iLatestZoomType = EWeaponLatestZoom::eSecondZoom;
    m_zoom_params.m_bIsZoomModeNow = true;
    m_zoom_params.m_bIsZoomSecondModeNow = false;

    attachable_hud_item* hi = HudItemData();
    if (m_zoom_params.m_fZoomRotationFactor == 1.f)
        m_zoom_params.m_fZoomRotationFactor = 0.f;

    if (!IsScopeAttached())
        SetZoomFactor(m_fRTZoomFactor);
    else if (!m_zoom_params.m_bUseDynamicZoom)
        SetZoomFactor(CurrentZoomFactor());
    else
        SetZoomFactor(psActorFlags.test(AF_3DSCOPE) ? m_zoom_params.m_f3dZoomFactor : m_fRTZoomFactor);

    // Отключаем инерцию (Заменено GetInertionFactor())
    // EnableHudInertion(FALSE);

    if (m_zoom_params.m_bZoomDofEnabled && !IsScopeAttached())
        GamePersistent().SetEffectorDOF(m_zoom_params.m_ZoomDof);

    if (GetHUDmode())
        GamePersistent().SetPickableEffectorDOF(true);

    if (m_zoom_params.m_sUseBinocularVision.size() && (IsScopeAttached() || IsScopePermament()) && nullptr == m_zoom_params.m_pVision)
        m_zoom_params.m_pVision = xr_new<CBinocularsVision>(m_zoom_params.m_sUseBinocularVision /*"wpn_binoc"*/);

    CActor* pA = smart_cast<CActor*>(H_Parent());

    if (pA && (IsScopeAttached() || IsScopePermament()))
    {
        if (psActorFlags.test(AF_PNV_W_SCOPE_DIS) && UseScopeTexture())
        {
            CTorch* pTorch = smart_cast<CTorch*>(pA->inventory().ItemFromSlot(TORCH_SLOT));
            if (pTorch && pTorch->GetNightVisionStatus())
                OnZoomOut();
        }
        else if (m_zoom_params.m_sUseZoomPostprocess.size() && !psActorFlags.test(AF_3DSCOPE))
        {
            if (NULL == m_zoom_params.m_pNight_vision)
                m_zoom_params.m_pNight_vision = xr_new<CNightVisionEffector>(m_zoom_params.m_sUseZoomPostprocess/*"device_torch"*/);
        }
    }

    PlayCamAnim("cam_anm_aim_in");
    g_player_hud[1]->set_detector_state(EHudStates::eWpnZoomStart);
}

void CWeapon::OnZoomFirstOut()
{
    m_zoom_params.m_iLatestZoomType = EWeaponLatestZoom::eMainZoom;
    OnZoomOut();
}
void CWeapon::OnZoomSecondOut()
{
    m_zoom_params.m_iLatestZoomType = EWeaponLatestZoom::eSecondZoom;
    OnZoomOut();
}
void CWeapon::OnZoomOut()
{
    if (!IsSecondZoomed() && !psActorFlags.test(AF_3DSCOPE))
        m_fRTZoomFactor = GetZoomFactor(); // Сохраняем текущий динамический зум
    m_zoom_params.m_bIsZoomModeNow = false;
    m_zoom_params.m_bIsZoomSecondModeNow = false;
    m_zoom_params.m_bSwitchBetweenSecondsZooms = false;

    SetZoomFactor(g_fov);
    // Включаем инерцию (также заменено  GetInertionFactor())
    // EnableHudInertion(TRUE);

    GamePersistent().RestoreEffectorDOF();

    if (GetHUDmode())
        GamePersistent().SetPickableEffectorDOF(false);

    ResetSubStateTime();

    xr_delete(m_zoom_params.m_pVision);
    if (m_zoom_params.m_pNight_vision)
    {
        m_zoom_params.m_pNight_vision->Stop(100000.0f, false);
        xr_delete(m_zoom_params.m_pNight_vision);
    }

    PlayCamAnim("cam_anm_aim_out");
    g_player_hud[1]->set_detector_state(EHudStates::eWpnZoomEnd);
}

CUIWindow* CWeapon::ZoomTexture()
{
    if (UseScopeTexture() && !psActorFlags.test(AF_3DSCOPE))
        return m_UIScope;
    else
        return nullptr;
}

void CWeapon::SwitchState(u32 S)
{
    if (OnClient())
        return;

#ifndef MASTER_GOLD
    if (bDebug)
    {
        Msg("---Server is going to send GE_WPN_STATE_CHANGE to [%d], weapon_section[%s], parent[%s]", S,
            cNameSect().c_str(), H_Parent() ? H_Parent()->cName().c_str() : "NULL Parent");
    }
#endif // #ifndef MASTER_GOLD

    SetNextState(S);
    if (CHudItem::object().Local() && !CHudItem::object().getDestroy() && m_pInventory && OnServer())
    {
        // !!! Just single entry for given state !!!
        NET_Packet P;
        CHudItem::object().u_EventGen(P, GE_WPN_STATE_CHANGE, CHudItem::object().ID());
        P.w_u8(u8(S));
        P.w_u8(u8(m_sub_state));
        P.w_u8(m_ammoType);
        P.w_u8(u8(iAmmoElapsed & 0xff));
        P.w_u8(m_set_next_ammoType_on_reload);
        CHudItem::object().u_EventSend(P, net_flags(TRUE, TRUE, FALSE, TRUE));
    }
}

void CWeapon::OnMagazineEmpty() { VERIFY((u32)iAmmoElapsed == m_magazine.size()); }
void CWeapon::reinit()
{
    CShootingObject::reinit();
    CHudItemObject::reinit();
}

void CWeapon::reload(LPCSTR section)
{
    CShootingObject::reload(section);
    CHudItemObject::reload(section);

    m_can_be_strapped = true;
    m_strapped_mode = false;
    b_forceIconUpdate = true;

    if (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope)
        UpdateAddonsOffset();

    if (pSettings->line_exist(section, "strap_bone0"))
        m_strap_bone0 = pSettings->r_string(section, "strap_bone0");
    else
        m_can_be_strapped = false;

    if (pSettings->line_exist(section, "strap_bone1"))
        m_strap_bone1 = pSettings->r_string(section, "strap_bone1");
    else
        m_can_be_strapped = false;

    m_eScopeStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "scope_status");
    m_eSilencerStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "silencer_status");
    m_eGrenadeLauncherStatus = (ALife::EWeaponAddonStatus)pSettings->r_s32(section, "grenade_launcher_status");

    if (pSettings->line_exist(section, "scopes") && xr_strcmp(pSettings->r_string(section, "scopes"), "") != 0 && xr_strcmp(pSettings->r_string(section, "scopes"), "none") != 0)
        m_eScopeStatus = ALife::EWeaponAddonStatus::eAddonAttachable;

    if (!bUseAttachmentSystem)
        m_zoom_params.m_bZoomSecondEnabled = READ_IF_EXISTS(pSettings, r_bool, section, "use_alt_aim_hud", false);
    m_zoom_params.m_fZoomRotateTime = READ_IF_EXISTS(pSettings, r_float, section, "zoom_rotate_time", ROTATION_TIME);

    bUseAltScope = !!bLoadAltScopesParams(section);
    bLoadzCollimatorScopesParams(section);

    if (!bUseAltScope)
        LoadOriginalScopesParams(section);

    if (m_eSilencerStatus == ALife::eAddonAttachable)
    {
        m_sSilencerName = pSettings->r_string(section, "silencer_name");
        m_iSilencerX = pSettings->r_s32(section, "silencer_x");
        m_iSilencerY = pSettings->r_s32(section, "silencer_y");
    }

    if (m_eGrenadeLauncherStatus == ALife::eAddonAttachable)
    {
        m_sGrenadeLauncherName = pSettings->r_string(section, "grenade_launcher_name");
        m_iGrenadeLauncherX = pSettings->r_s32(section, "grenade_launcher_x");
        m_iGrenadeLauncherY = pSettings->r_s32(section, "grenade_launcher_y");
    }

    UpdateAltScope();

    if (m_eScopeStatus == ALife::eAddonAttachable)
    {
        m_addon_holder_range_modifier =
            READ_IF_EXISTS(pSettings, r_float, GetScopeName(), "holder_range_modifier", m_holder_range_modifier);
        m_addon_holder_fov_modifier =
            READ_IF_EXISTS(pSettings, r_float, GetScopeName(), "holder_fov_modifier", m_holder_fov_modifier);
    }
    else
    {
        m_addon_holder_range_modifier = m_holder_range_modifier;
        m_addon_holder_fov_modifier = m_holder_fov_modifier;
    }

    {
        Fvector pos, ypr;
        pos = pSettings->r_fvector3(section, "position");
        ypr = pSettings->r_fvector3(section, "orientation");
        ypr.mul(PI / 180.f);

        m_Offset.setHPB(ypr.x, ypr.y, ypr.z);
        m_Offset.translate_over(pos);
    }

    m_StrapOffset = m_Offset;
    if (pSettings->line_exist(section, "strap_position") && pSettings->line_exist(section, "strap_orientation"))
    {
        Fvector pos, ypr;
        pos = pSettings->r_fvector3(section, "strap_position");
        ypr = pSettings->r_fvector3(section, "strap_orientation");
        ypr.mul(PI / 180.f);

        m_StrapOffset.setHPB(ypr.x, ypr.y, ypr.z);
        m_StrapOffset.translate_over(pos);
    }
    else
        m_can_be_strapped = false;

    m_ef_main_weapon_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_main_weapon_type", u32(-1));
    m_ef_weapon_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_weapon_type", u32(-1));

    string64 base_hud_sect;
    string128 val_name;
    string64 _prefix;

    const bool is_16x9 = UICore::is_widescreen();
    xr_sprintf(_prefix, "%s", is_16x9 ? "_16x9" : "");
    xr_sprintf(base_hud_sect, "%s_hud", section);

    if (pSettings->line_exist(section, "hud"))
        xr_sprintf(base_hud_sect, "%s", pSettings->r_string(section, "hud"));

    m_hands_offset[0][0].set(0, 0, 0);
    m_hands_offset[1][0].set(0, 0, 0);

    if (!bUseAttachmentSystem)
    {
        strconcat(sizeof(val_name), val_name, "aim_hud_offset_alt_pos", _prefix);
        if (pSettings->line_exist(hud_sect, val_name))
            m_hands_offset[0][1] = pSettings->r_fvector3(hud_sect, val_name);
        else if (pSettings->line_exist(base_hud_sect, val_name))
            m_hands_offset[0][1] = pSettings->r_fvector3(base_hud_sect, val_name);
        strconcat(sizeof(val_name), val_name, "aim_hud_offset_alt_rot", _prefix);
        if (pSettings->line_exist(hud_sect, val_name))
            m_hands_offset[1][1] = pSettings->r_fvector3(hud_sect, val_name);
        else if (pSettings->line_exist(base_hud_sect, val_name))
            m_hands_offset[1][1] = pSettings->r_fvector3(base_hud_sect, val_name);
    }

    LoadCurrentScopeParams(m_section_id.c_str());
}

void CWeapon::create_physic_shell() { CPhysicsShellHolder::create_physic_shell(); }
bool CWeapon::ActivationSpeedOverriden(Fvector& dest, bool clear_override)
{
    if (m_activation_speed_is_overriden)
    {
        if (clear_override)
        {
            m_activation_speed_is_overriden = false;
        }

        dest = m_overriden_activation_speed;
        return true;
    }

    return false;
}

void CWeapon::SetActivationSpeedOverride(Fvector const& speed)
{
    m_overriden_activation_speed = speed;
    m_activation_speed_is_overriden = true;
}

void CWeapon::activate_physic_shell()
{
    UpdateXForm();
    CPhysicsShellHolder::activate_physic_shell();
}

void CWeapon::setup_physic_shell() { CPhysicsShellHolder::setup_physic_shell(); }
int g_iWeaponRemove = 1;

bool CWeapon::NeedToDestroyObject() const
{
    if (GameID() == eGameIDSingle)
        return false;
    if (Remote())
        return false;
    if (H_Parent())
        return false;
    if (g_iWeaponRemove == -1)
        return false;
    if (g_iWeaponRemove == 0)
        return true;
    if (TimePassedAfterIndependant() > m_dwWeaponRemoveTime)
        return true;

    return false;
}

ALife::_TIME_ID CWeapon::TimePassedAfterIndependant() const
{
    if (!H_Parent() && m_dwWeaponIndependencyTime != 0)
        return Level().timeServer() - m_dwWeaponIndependencyTime;
    else
        return 0;
}

bool CWeapon::can_kill() const
{
    if (GetSuitableAmmoTotal(true) || m_ammoTypes.empty())
        return (true);

    return (false);
}

CInventoryItem* CWeapon::can_kill(CInventory* inventory) const
{
    if (GetAmmoElapsed() || m_ammoTypes.empty())
        return (const_cast<CWeapon*>(this));

    TIItemContainer::iterator I = inventory->m_all.begin();
    TIItemContainer::iterator E = inventory->m_all.end();
    for (; I != E; ++I)
    {
        CInventoryItem* inventory_item = smart_cast<CInventoryItem*>(*I);
        if (!inventory_item)
            continue;

        xr_vector<shared_str>::const_iterator i =
            std::find(m_ammoTypes.begin(), m_ammoTypes.end(), inventory_item->object().cNameSect());
        if (i != m_ammoTypes.end())
            return (inventory_item);
    }

    return (nullptr);
}

const CInventoryItem* CWeapon::can_kill(const xr_vector<const CGameObject*>& items) const
{
    if (m_ammoTypes.empty())
        return (this);

    xr_vector<const CGameObject*>::const_iterator I = items.begin();
    xr_vector<const CGameObject*>::const_iterator E = items.end();
    for (; I != E; ++I)
    {
        const CInventoryItem* inventory_item = smart_cast<const CInventoryItem*>(*I);
        if (!inventory_item)
            continue;

        xr_vector<shared_str>::const_iterator i =
            std::find(m_ammoTypes.begin(), m_ammoTypes.end(), inventory_item->object().cNameSect());
        if (i != m_ammoTypes.end())
            return (inventory_item);
    }

    return (nullptr);
}

bool CWeapon::ready_to_kill() const
{
	//Alundaio
    const auto io = smart_cast<const CInventoryOwner*>(H_Parent());
	if (!io)
		return false;

	if (!io->inventory().ActiveItem() || io->inventory().ActiveItem()->object().ID() != ID())
		return false;
	//-Alundaio
    return (
        !IsMisfire() && ((GetState() == eIdle) || (GetState() == eFire) || (GetState() == eFire2)) && GetAmmoElapsed());
}

void _inertion(float& _val_cur, const float& _val_trgt, const float& _friction)
{
    float friction_i = 1.f - _friction;
    _val_cur = _val_cur * _friction + _val_trgt * friction_i;
}

float _lerp(const float& _val_a, const float& _val_b, const float& _factor)
{
    return (_val_a * (1.0 - _factor)) + (_val_b * _factor);
}

void CWeapon::UpdateHudAdditional(Fmatrix& trans)
{
    CActor* pActor = smart_cast<CActor*>(H_Parent());
    if (!pActor)
        return;

    attachable_hud_item* hi = HudItemData();
    R_ASSERT(hi);

    u8 idx = GetCurrentHudOffsetIdx();
    float m_fZoomRotateTime = m_zoom_params.m_fZoomRotateTime;
    float t = m_zoom_params.m_fZoomRotationFactor;
    bool not_calc_z_offset = false;

    if (IsZoomSecondEnabled() && IsSecondZoomed())
    {
        t = m_zoom_params.m_fSecondZoomRotationFactor;
        not_calc_z_offset = true;
    }

    //============= Поворот ствола во время аима =============//
    if (((IsZoomed() || IsSecondZoomed()) && t <= 1.f) || ((!IsZoomed() || !IsSecondZoomed()) && t > 0.f))
    {
        Fvector curr_offs;
        Fvector curr_rot;

        float out = 1.f - powf(1.f - t, 0.5f);
        float in = 1.f - powf(1.f - t, 3.0f);
        if (in > 1.f)
            in = 1.f;
        float curve = out;
        if ((IsZoomed() || IsSecondZoomed()))
            curve = in;

        // Из дефолтного состояния в первичный зум
        if ((IsZoomed() || (!IsSecondZoomed() && m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eMainZoom)) && (m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::noZoom || m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eMainZoom))
        {
            curr_offs = hi->m_measures.m_hands_offset[0][idx]; //pos,aim
            curr_rot = hi->m_measures.m_hands_offset[1][idx]; //rot,aim

            curr_offs.mul(curve);
            curr_rot.mul(curve);
        }
        // Из дефолтного состояния во вторичный зум
        else if (IsZoomSecondEnabled() && !IsZoomed() && (IsSecondZoomed() || m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eSecondZoom) && (m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::noZoom || m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eSecondZoom) && !m_zoom_params.m_bSwitchBetweenSecondsZooms)
        {
            curr_offs = m_hands_offset[0][idx]; //pos,aim
            curr_rot = m_hands_offset[1][idx]; //rot,aim

            curr_offs.mul(curve);
            curr_rot.mul(curve);
        }
        // Из первичного зума во вторичный зум
        else if (IsZoomSecondEnabled() && IsSecondZoomed() && (m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eMainZoom || m_zoom_params.m_bSwitchBetweenSecondsZooms))
        {
            Fvector curr_offs1, curr_rot1;
            curr_offs1 = curr_offs = m_tmp_offs;
            curr_rot1 = curr_rot = m_tmp_rot;

            curr_offs1.sub(m_hands_offset[0][idx]);
            curr_rot1.sub(m_hands_offset[1][idx]);

            curr_offs1.mul(curve);
            curr_rot1.mul(curve);

            curr_offs.sub(curr_offs1);
            curr_rot.sub(curr_rot1);
        }
        // Из вторичного зума в первичный зум
        else if (IsZoomed() && m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eSecondZoom)
        {
            Fvector curr_offs1, curr_rot1;
            curr_offs1 = curr_offs = m_tmp_offs;
            curr_rot1 = curr_rot = m_tmp_rot;

            curr_offs1.sub(hi->m_measures.m_hands_offset[0][idx]);
            curr_rot1.sub(hi->m_measures.m_hands_offset[1][idx]);

            curr_offs1.mul(curve);
            curr_rot1.mul(curve);

            curr_offs.sub(curr_offs1);
            curr_rot.sub(curr_rot1);
        }
        // Из зума в дефолтное состояние
        else
        {
            if (m_zoom_params.m_iLatestZoomType == EWeaponLatestZoom::eSecondZoom)
            {
                curr_offs = m_hands_offset[0][idx]; //pos,aim
                curr_rot = m_hands_offset[1][idx]; //rot,aim
            }
            else
            {
                curr_offs = hi->m_measures.m_hands_offset[0][idx];
                curr_rot = hi->m_measures.m_hands_offset[1][idx];
            }

            curr_offs.mul(curve);
            curr_rot.mul(curve);
        }

        // Сохраняем промежуточные значения для плавного перехода из промежуточной позиции худа в другой зум
        m_tmp_offs = curr_offs; //pos,aim
        m_tmp_rot = curr_rot; //rot,aim

        Fmatrix hud_rotation;
        hud_rotation.identity();
        hud_rotation.rotateX(curr_rot.x);

        Fmatrix hud_rotation_y;
        hud_rotation_y.identity();
        hud_rotation_y.rotateY(curr_rot.y);
        hud_rotation.mulA_43(hud_rotation_y);

        hud_rotation_y.identity();
        hud_rotation_y.rotateZ(curr_rot.z);
        hud_rotation.mulA_43(hud_rotation_y);

        hud_rotation.translate_over(curr_offs);
        trans.mulB_43(hud_rotation);

        if (pActor->IsZoomAimingMode())
            m_zoom_params.m_fZoomRotationFactor += Device.fTimeDelta / m_fZoomRotateTime;
        else
            m_zoom_params.m_fZoomRotationFactor -= Device.fTimeDelta / m_fZoomRotateTime;

        if (pActor->IsZoomAimingMode() && IsSecondZoomed())
            m_zoom_params.m_fSecondZoomRotationFactor += Device.fTimeDelta / m_fZoomRotateTime;
        else
            m_zoom_params.m_fSecondZoomRotationFactor -= Device.fTimeDelta / m_fZoomRotateTime;

        clamp(m_zoom_params.m_fZoomRotationFactor, 0.f, 1.f);
        clamp(m_zoom_params.m_fSecondZoomRotationFactor, 0.f, 1.f);
    }
    if (!IsZoomed() && !IsSecondZoomed() && m_zoom_params.m_fZoomRotationFactor == 0.f)
        m_zoom_params.m_iLatestZoomType = EWeaponLatestZoom::noZoom;

    //============= Подготавливаем общие переменные =============//

    clamp(idx, u8(0), u8(1));
    bool bForAim = (idx == 1);

    float fInertiaPower = GetInertionPowerFactor();

    float fYMag = pActor->fFPCamYawMagnitude;
    float fPMag = pActor->fFPCamPitchMagnitude;

    static float fAvgTimeDelta = Device.fTimeDelta;
    _inertion(fAvgTimeDelta, Device.fTimeDelta, 0.8f);

    //======== Проверяем доступность инерции и стрейфа ========//
    if (!g_player_hud[0]->inertion_allowed())
        return;

    //============= Боковой стрейф с оружием =============//
    float fStrafeMaxTime = m_strafe_offset[2][idx].y; // Макс. время в секундах, за которое мы наклонимся из центрального положения
    if (fStrafeMaxTime <= EPS)
        fStrafeMaxTime = 0.01f;

    float fStepPerUpd = fAvgTimeDelta / fStrafeMaxTime; // Величина изменение фактора поворота

    // Добавляем боковой наклон от движения камеры
    float fCamReturnSpeedMod = 1.5f; // Восколько ускоряем нормализацию наклона, полученного от движения камеры (только от бедра)
    // Высчитываем минимальную скорость поворота камеры для начала инерции
    float fStrafeMinAngle = _lerp(
        m_strafe_offset[3][0].y,
        m_strafe_offset[3][1].y,
        m_zoom_params.m_fZoomRotationFactor);

    // Высчитываем мксимальный наклон от поворота камеры
    float fCamLimitBlend = _lerp(
        m_strafe_offset[3][0].x,
        m_strafe_offset[3][1].x,
        m_zoom_params.m_fZoomRotationFactor);

    // Считаем стрейф от поворота камеры
    if (abs(fYMag) > (m_fLR_CameraFactor == 0.0f ? fStrafeMinAngle : 0.0f))
    { //--> Камера крутится по оси Y
        m_fLR_CameraFactor -= (fYMag * 0.025f);

        clamp(m_fLR_CameraFactor, -fCamLimitBlend, fCamLimitBlend);
    }
    else
    { //--> Камера не поворачивается - убираем наклон
        if (m_fLR_CameraFactor < 0.0f)
        {
            m_fLR_CameraFactor += fStepPerUpd * (bForAim ? 1.0f : fCamReturnSpeedMod);
            clamp(m_fLR_CameraFactor, -fCamLimitBlend, 0.0f);
        }
        else
        {
            m_fLR_CameraFactor -= fStepPerUpd * (bForAim ? 1.0f : fCamReturnSpeedMod);
            clamp(m_fLR_CameraFactor, 0.0f, fCamLimitBlend);
        }
    }
    // Добавляем боковой наклон от ходьбы вбок
    float fChangeDirSpeedMod = 3; // Восколько быстро меняем направление направление наклона, если оно в другую сторону от текущего

    u32 iMovingState = pActor->MovingState();
    if ((iMovingState & mcLStrafe) != 0)
    { // Движемся влево
        float fVal = (m_fLR_MovingFactor > 0.f ? fStepPerUpd * fChangeDirSpeedMod : fStepPerUpd);
        m_fLR_MovingFactor -= fVal;
    }
    else if ((iMovingState & mcRStrafe) != 0)
    { // Движемся вправо
        float fVal = (m_fLR_MovingFactor < 0.f ? fStepPerUpd * fChangeDirSpeedMod : fStepPerUpd);
        m_fLR_MovingFactor += fVal;
    }
    else
    { // Двигаемся в любом другом направлении - плавно убираем наклон
        if (m_fLR_MovingFactor < 0.0f)
        {
            m_fLR_MovingFactor += fStepPerUpd;
            clamp(m_fLR_MovingFactor, -1.0f, 0.0f);
        }
        else
        {
            m_fLR_MovingFactor -= fStepPerUpd;
            clamp(m_fLR_MovingFactor, 0.0f, 1.0f);
        }
    }

    clamp(m_fLR_MovingFactor, -1.0f, 1.0f); // Фактор боковой ходьбы не должен превышать эти лимиты

    // Вычисляем и нормализируем итоговый фактор наклона
    float fLR_Factor = m_fLR_MovingFactor + (m_fLR_CameraFactor * fInertiaPower);
    clamp(fLR_Factor, -1.0f, 1.0f); // Фактор боковой ходьбы не должен превышать эти лимиты

    // Производим наклон ствола для нормального режима и аима
    for (int _idx = 0; _idx <= 1; _idx++)//<-- Для плавного перехода
    {
        bool bEnabled = (m_strafe_offset[2][_idx].x != 0.0f);
        if (!bEnabled)
            continue;

        Fvector curr_offs, curr_rot;

        // Смещение позиции худа в стрейфе
        curr_offs = m_strafe_offset[0][_idx]; //pos
        curr_offs.mul(fLR_Factor);                   // Умножаем на фактор стрейфа

        // Поворот худа в стрейфе
        curr_rot = m_strafe_offset[1][_idx]; //rot
        curr_rot.mul(-PI / 180.f);                          // Преобразуем углы в радианы
        curr_rot.mul(fLR_Factor);                   // Умножаем на фактор стрейфа

        // Мягкий переход между бедром \ прицелом
        if (_idx == 0)
        { // От бедра
            curr_offs.mul(1.f - m_zoom_params.m_fZoomRotationFactor);
            curr_rot.mul(1.f - m_zoom_params.m_fZoomRotationFactor);
        }
        else
        { // Во время аима
            curr_offs.mul(m_zoom_params.m_fZoomRotationFactor);
            curr_rot.mul(m_zoom_params.m_fZoomRotationFactor);
        }

        Fmatrix hud_rotation;
        Fmatrix hud_rotation_y;

        hud_rotation.identity();
        hud_rotation.rotateX(curr_rot.x);

        hud_rotation_y.identity();
        hud_rotation_y.rotateY(curr_rot.y);
        hud_rotation.mulA_43(hud_rotation_y);

        hud_rotation_y.identity();
        hud_rotation_y.rotateZ(curr_rot.z);
        hud_rotation.mulA_43(hud_rotation_y);

        hud_rotation.translate_over(curr_offs);
        trans.mulB_43(hud_rotation);
        hi->m_item_dot_transform.mulB_43(hud_rotation);
    }

    //============= Инерция оружия =============//
   // Параметры инерции
    float fInertiaSpeedMod = _lerp(
        hi->m_measures.m_inertion_params.m_tendto_speed,
        hi->m_measures.m_inertion_params.m_tendto_speed_aim,
        m_zoom_params.m_fZoomRotationFactor);

    float fInertiaReturnSpeedMod = _lerp(
        hi->m_measures.m_inertion_params.m_tendto_ret_speed,
        hi->m_measures.m_inertion_params.m_tendto_ret_speed_aim,
        m_zoom_params.m_fZoomRotationFactor);

    float fInertiaMinAngle = _lerp(
        hi->m_measures.m_inertion_params.m_min_angle,
        hi->m_measures.m_inertion_params.m_min_angle_aim,
        m_zoom_params.m_fZoomRotationFactor);

    Fvector4 vIOffsets; // x = L, y = R, z = U, w = D
    vIOffsets.x = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.x,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.x,
        m_zoom_params.m_fZoomRotationFactor) * fInertiaPower;
    vIOffsets.y = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.y,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.y,
        m_zoom_params.m_fZoomRotationFactor) * fInertiaPower;
    vIOffsets.z = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.z,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.z,
        m_zoom_params.m_fZoomRotationFactor) * fInertiaPower;
    vIOffsets.w = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.w,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.w,
        m_zoom_params.m_fZoomRotationFactor) * fInertiaPower;

    // Высчитываем инерцию из поворотов камеры
    bool bIsInertionPresent = m_fLR_InertiaFactor != 0.0f || m_fUD_InertiaFactor != 0.0f;
    if (abs(fYMag) > fInertiaMinAngle || bIsInertionPresent)
    {
        float fSpeed = fInertiaSpeedMod;
        if (fYMag > 0.0f && m_fLR_InertiaFactor > 0.0f ||
            fYMag < 0.0f && m_fLR_InertiaFactor < 0.0f)
        {
            fSpeed *= 2.f; //--> Ускоряем инерцию при движении в противоположную сторону
        }

        m_fLR_InertiaFactor -= (fYMag * fAvgTimeDelta * fSpeed); // Горизонталь (м.б. > |1.0|)
    }

    if (abs(fPMag) > fInertiaMinAngle || bIsInertionPresent)
    {
        float fSpeed = fInertiaSpeedMod;
        if (fPMag > 0.0f && m_fUD_InertiaFactor > 0.0f ||
            fPMag < 0.0f && m_fUD_InertiaFactor < 0.0f)
        {
            fSpeed *= 2.f; //--> Ускоряем инерцию при движении в противоположную сторону
        }

        m_fUD_InertiaFactor -= (fPMag * fAvgTimeDelta * fSpeed); // Вертикаль (м.б. > |1.0|)
    }

    clamp(m_fLR_InertiaFactor, -1.0f, 1.0f);
    clamp(m_fUD_InertiaFactor, -1.0f, 1.0f);

    // Плавное затухание инерции (основное, но без линейной никогда не опустит инерцию до полного 0.0f)
    m_fLR_InertiaFactor *= clampr(1.f - fAvgTimeDelta * fInertiaReturnSpeedMod, 0.0f, 1.0f);
    m_fUD_InertiaFactor *= clampr(1.f - fAvgTimeDelta * fInertiaReturnSpeedMod, 0.0f, 1.0f);

    // Минимальное линейное затухание инерции при покое (горизонталь)
    if (fYMag == 0.0f)
    {
        float fRetSpeedMod = (fYMag == 0.0f ? 1.0f : 0.75f) * (fInertiaReturnSpeedMod * 0.075f);
        if (m_fLR_InertiaFactor < 0.0f)
        {
            m_fLR_InertiaFactor += fAvgTimeDelta * fRetSpeedMod;
            clamp(m_fLR_InertiaFactor, -1.0f, 0.0f);
        }
        else
        {
            m_fLR_InertiaFactor -= fAvgTimeDelta * fRetSpeedMod;
            clamp(m_fLR_InertiaFactor, 0.0f, 1.0f);
        }
    }

    // Минимальное линейное затухание инерции при покое (вертикаль)
    if (fPMag == 0.0f)
    {
        float fRetSpeedMod = (fPMag == 0.0f ? 1.0f : 0.75f) * (fInertiaReturnSpeedMod * 0.075f);
        if (m_fUD_InertiaFactor < 0.0f)
        {
            m_fUD_InertiaFactor += fAvgTimeDelta * fRetSpeedMod;
            clamp(m_fUD_InertiaFactor, -1.0f, 0.0f);
        }
        else
        {
            m_fUD_InertiaFactor -= fAvgTimeDelta * fRetSpeedMod;
            clamp(m_fUD_InertiaFactor, 0.0f, 1.0f);
        }
    }

    // Применяем инерцию к худу
    float fLR_lim = (m_fLR_InertiaFactor < 0.0f ? vIOffsets.x : vIOffsets.y);
    float fUD_lim = (m_fUD_InertiaFactor < 0.0f ? vIOffsets.z : vIOffsets.w);

    Fvector curr_offs;
    curr_offs = { fLR_lim * -1.f * m_fLR_InertiaFactor, fUD_lim * m_fUD_InertiaFactor, 0.0f };

    Fmatrix hud_rotation;
    hud_rotation.identity();
    hud_rotation.translate_over(curr_offs);
    trans.mulB_43(hud_rotation);
    hi->m_item_dot_transform.mulB_43(hud_rotation);
}

void CWeapon::SetAmmoElapsed(int ammo_count)
{
    iAmmoElapsed = ammo_count;

    u32 uAmmo = u32(iAmmoElapsed);

    if (uAmmo != m_magazine.size())
    {
        if (uAmmo > m_magazine.size())
        {
            CCartridge l_cartridge;
            l_cartridge.Load(m_ammoTypes[m_ammoType].c_str(), m_ammoType);
            while (uAmmo > m_magazine.size())
                m_magazine.push_back(l_cartridge);
        }
        else
        {
            while (uAmmo < m_magazine.size())
                m_magazine.pop_back();
        };
    };
}

u32 CWeapon::ef_main_weapon_type() const
{
    VERIFY(m_ef_main_weapon_type != u32(-1));
    return (m_ef_main_weapon_type);
}

u32 CWeapon::ef_weapon_type() const
{
    VERIFY(m_ef_weapon_type != u32(-1));
    return (m_ef_weapon_type);
}

bool CWeapon::IsNecessaryItem(const shared_str& item_sect)
{
    return (std::find(m_ammoTypes.begin(), m_ammoTypes.end(), item_sect) != m_ammoTypes.end());
}

void CWeapon::modify_holder_params(float& range, float& fov) const
{
    if (!IsScopeAttached())
    {
        inherited::modify_holder_params(range, fov);
        return;
    }
    range *= m_addon_holder_range_modifier;
    fov *= m_addon_holder_fov_modifier;
}

bool CWeapon::render_item_ui_query()
{
    bool b_is_active_item = (m_pInventory->ActiveItem() == this);
    bool res = b_is_active_item && IsZoomed() && ZoomHideCrosshair() && ZoomTexture() && !IsRotatingToZoom();
    return res;
}

void CWeapon::render_item_ui()
{
    if (m_zoom_params.m_pVision)
        m_zoom_params.m_pVision->Draw();

    ZoomTexture()->Update();
    ZoomTexture()->Draw();
}

bool CWeapon::unlimited_ammo()
{
    if (IsGameTypeSingle())
    {
        if (m_pInventory)
        {
            return inventory_owner().unlimited_ammo() && m_DefaultCartridge.m_flags.test(CCartridge::cfCanBeUnlimited);
        }
        else
            return false;
    }

    return ((GameID() == eGameIDDeathmatch) && m_DefaultCartridge.m_flags.test(CCartridge::cfCanBeUnlimited));
};

float CWeapon::GetMagazineWeight(const decltype(CWeapon::m_magazine)& mag) const
{
    float res = 0;
    const char* last_type = nullptr;
    float last_ammo_weight = 0;
    for (auto& c : mag)
    {
        // Usually ammos in mag have same type, use this fact to improve performance
        if (last_type != c.m_ammoSect.c_str())
        {
            last_type = c.m_ammoSect.c_str();
            last_ammo_weight = c.Weight();
        }
        res += last_ammo_weight;
    }
    return res;
}

float CWeapon::Weight() const
{
    float res = CInventoryItemObject::Weight();

    if (bUseAttachmentSystem)
    {
        for (auto [addon_id, addon]: m_addon_items)
            res += pSettings->r_float(addon->addon_item_name.c_str(), "inv_weight");
    }

    if (IsGrenadeLauncherAttached() && GetGrenadeLauncherName().size())
    {
        res += pSettings->r_float(GetGrenadeLauncherName(), "inv_weight");
    }
    if (!bUseAttachmentSystem && IsScopeAttached() && m_scopes.size())
    {
        res += pSettings->r_float(GetScopeName(), "inv_weight");
    }
    if (IsSilencerAttached() && GetSilencerName().size())
    {
        res += pSettings->r_float(GetSilencerName(), "inv_weight");
    }
    res += GetMagazineWeight(m_magazine);

    return res;
}

bool CWeapon::show_crosshair() { return !IsPending() && ((!IsZoomed() && !IsSecondZoomed()) || !ZoomHideCrosshair()); }
bool CWeapon::show_indicators() { return !(IsZoomed() && ZoomTexture()); }
float CWeapon::GetConditionToShow() const
{
    return (GetCondition());
}

BOOL CWeapon::ParentMayHaveAimBullet()
{
    IGameObject* O = H_Parent();
    CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
    return EA->cast_actor() != nullptr;
}

BOOL CWeapon::ParentIsActor()
{
    IGameObject* O = H_Parent();
    if (!O)
        return FALSE;

    CEntityAlive* EA = smart_cast<CEntityAlive*>(O);
    if (!EA)
        return FALSE;

    return EA->cast_actor() != nullptr;
}

bool CWeapon::ZoomHideCrosshair()
{
    CActor* pA = smart_cast<CActor*>(H_Parent());
    if (pA && pA->active_cam() == eacLookAt || GamePersistent().GetHudTuner().is_active())
        return false;
    return m_zoom_params.m_bHideCrosshairInZoom || ZoomTexture();
}

const float& CWeapon::hit_probability() const
{
    VERIFY((g_SingleGameDifficulty >= egdNovice) && (g_SingleGameDifficulty <= egdMaster));
    return (m_hit_probability[egdNovice]);
}

void CWeapon::OnStateSwitch(u32 S, u32 oldState)
{
    inherited::OnStateSwitch(S, oldState);
    m_BriefInfo_CalcFrame = 0;

    if (S == eReload)
    {
        CActor* current_actor = smart_cast<CActor*>(H_Parent());
        if (current_actor && H_Parent() == Level().CurrentEntity())
        {
            const Fvector4& reloadDof = iAmmoElapsed == 0 ? m_zoom_params.m_ReloadEmptyDof : m_zoom_params.m_ReloadDof;
            if (!fsimilar(reloadDof.w, -1.0f))
                current_actor->Cameras().AddCamEffector(xr_new<CEffectorDOF>(reloadDof));
        }
    }
}

void CWeapon::OnAnimationEnd(u32 state) { inherited::OnAnimationEnd(state); }
u8 CWeapon::GetCurrentHudOffsetIdx()
{
    CActor* pActor = smart_cast<CActor*>(H_Parent());
    if (!pActor)
        return 0;

    bool b_aiming = (((IsZoomed() || IsSecondZoomed()) && m_zoom_params.m_fZoomRotationFactor <= 1.f) ||
        ((!IsZoomed() && !IsSecondZoomed()) && m_zoom_params.m_fZoomRotationFactor > 0.f));

    if (!b_aiming)
        return 0;
    else
        return 1;
}

void CWeapon::render_hud_mode() { RenderLight(); }
bool CWeapon::MovingAnimAllowedNow() { return !IsZoomed() && !IsSecondZoomed(); }
bool CWeapon::IsHudModeNow() { return (HudItemData() != nullptr); }

void CWeapon::ZoomDynamicMod(bool bIncrement, bool bForceLimit)
{
    if (IsSecondZoomed())
    {
        if (getCountInstalledSecondAimAddons() > 1 && m_zoom_params.m_fSecondZoomRotationFactor == 1.f)
        {
            m_zoom_params.m_bSwitchBetweenSecondsZooms = true;
            m_zoom_params.m_fSecondZoomRotationFactor = 0.f;
            SwitchToNextZoomableAddon();
        }
        return;
    }
    if (!bUseAttachmentSystem && !IsScopeAttached() && !IsScopePermament())
        return;
    if (!m_zoom_params.m_bUseDynamicZoom)
        return;

    float delta, min_zoom_factor, max_zoom_factor;

    bool bIsSecondZOOM = bIsSecondVPZoomPresent() && psActorFlags.test(AF_3DSCOPE);

    max_zoom_factor = (bIsSecondZOOM ? GetSecondVPZoomFactor() * 100.0f : m_zoom_params.m_fScopeZoomFactor);
    GetZoomData(max_zoom_factor, delta, min_zoom_factor);

    if (bForceLimit)
    {
        if (bIsSecondZOOM)
            m_fSecondRTZoomFactor = (bIncrement ? max_zoom_factor : min_zoom_factor);
        else
            m_fRTZoomFactor = (bIncrement ? max_zoom_factor : min_zoom_factor);
    }
    else
    {
        float f = (bIsSecondZOOM ? m_fSecondRTZoomFactor : GetZoomFactor());
        f -= delta * (bIncrement ? 1.f : -1.f);
        clamp(f, max_zoom_factor, min_zoom_factor);

        if (bIsSecondZOOM)
            m_fSecondRTZoomFactor = f;
        else
            SetZoomFactor(f);
    }
}

void CWeapon::ZoomInc()
{
    ZoomDynamicMod(true, false);
}

void CWeapon::ZoomDec()
{
    if (IsSecondZoomed())
    {
        if (getCountInstalledSecondAimAddons() > 1 && m_zoom_params.m_fSecondZoomRotationFactor == 1.f)
        {
            m_zoom_params.m_bSwitchBetweenSecondsZooms = true;
            m_zoom_params.m_fSecondZoomRotationFactor = 0.f;
            SwitchToPrevZoomableAddon();
        }
        return;
    }
    if (!bUseAttachmentSystem && !IsScopeAttached() && !IsScopePermament())
        return;
    if (!m_zoom_params.m_bUseDynamicZoom)
        return;

    bool bIsSecondZOOM = bIsSecondVPZoomPresent() && psActorFlags.test(AF_3DSCOPE);

    float delta, min_zoom_factor;
    GetZoomData(m_zoom_params.m_fScopeZoomFactor, delta, min_zoom_factor);

    float f = GetZoomFactor() + delta;
    clamp(f, m_zoom_params.m_fScopeZoomFactor, min_zoom_factor);
    if (bIsSecondZOOM)
        m_fSecondRTZoomFactor = f;
    else
        SetZoomFactor(f);
}

u32 CWeapon::Cost() const
{
    u32 res = CInventoryItem::Cost();
    if (IsGrenadeLauncherAttached() && GetGrenadeLauncherName().size())
    {
        res += pSettings->r_u32(GetGrenadeLauncherName(), "cost");
    }
    if (IsScopeAttached() && m_scopes.size())
    {
        res += pSettings->r_u32(GetScopeName(), "cost");
    }
    if (IsSilencerAttached() && GetSilencerName().size())
    {
        res += pSettings->r_u32(GetSilencerName(), "cost");
    }

    if (iAmmoElapsed)
    {
        float w = pSettings->r_float(m_ammoTypes[m_ammoType].c_str(), "cost");
        float bs = pSettings->r_float(m_ammoTypes[m_ammoType].c_str(), "box_size");

        res += iFloor(w * (iAmmoElapsed / bs));
    }
    return res;
}

// Get the HUD FOV of the current weapon
float CWeapon::GetHudFov()
{
    // We calculate the HUD FOV from the hip (taking into account the abutment against the walls)
    if (ParentIsActor() && Level().CurrentViewEntity() == H_Parent())
    {
        // Get the distance from the camera to the point in the scope
        collide::rq_result& RQ = HUD().GetCurrentRayQuery();
        float dist = RQ.range;

        // Interpolate the distance in the range from 0 (min) to 1 (max)
        clamp(dist, m_nearwall_dist_min, m_nearwall_dist_max);
        float fDistanceMod =
            ((dist - m_nearwall_dist_min) / (m_nearwall_dist_max - m_nearwall_dist_min)); // 0.f ... 1.f

        // We calculate the basic HUD FOV from the hip
        float fBaseFov = psHUD_FOV_def + m_hud_fov_add_mod;
        clamp(fBaseFov, 0.0f, FLT_MAX);

        // Smoothly calculate the final FOV from the hip
        float src = m_nearwall_speed_mod * Device.fTimeDelta;
        clamp(src, 0.f, 1.f);

        float fTrgFov = m_nearwall_target_hud_fov + fDistanceMod * (fBaseFov - m_nearwall_target_hud_fov);
        m_nearwall_last_hud_fov = m_nearwall_last_hud_fov * (1 - src) + fTrgFov * src;
    }
    return m_nearwall_last_hud_fov;
}

// Получить FOV от текущего оружия игрока для второго рендера
float CWeapon::GetSecondVPFov() const
{
    if (m_zoom_params.m_bUseDynamicZoom && bIsSecondVPZoomPresent())
        return (m_fSecondRTZoomFactor / 100.f) * 75.0f; // g_fov;

    return GetSecondVPZoomFactor() * 75.0f; // g_fov;
}

// Обновление необходимости включения второго вьюпорта +SecondVP+
// Вызывается только для активного оружия игрока
void CWeapon::UpdateSecondVP(bool bInGrenade)
{
    // bool b_is_active_item = (m_pInventory != NULL) && (m_pInventory->ActiveItem() == this);
    // R_ASSERT(ParentIsActor() && b_is_active_item); // Эта функция должна вызываться только для оружия в руках нашего игрока

    // CActor* pActor = smart_cast<CActor*>(H_Parent());

    // bool bCond_1 = bInZoomRightNow(); // Мы должны целиться
    // bool bCond_2 = bIsSecondVPZoomPresent() && psActorFlags.test(AF_3DSCOPE); // В конфиге должен быть прописан фактор зума для линзы (scope_lense_factor, больше чем 0)
    // // bool bCond_2 = psActorFlags.test(AF_3DSCOPE); // В конфиге должен быть прописан фактор зума для линзы (scope_lense_factor, больше чем 0)
    // bool bCond_3 = pActor->cam_Active() == pActor->cam_FirstEye(); // Мы должны быть от 1-го лица

    // Device.m_SecondViewport.SetSVPActive(bCond_1 && bCond_2 && bCond_3 && !bInGrenade);

    Device.m_SecondViewport.SetSVPActive(psActorFlags.test(AF_3DSCOPE) && m_zoom_params.m_fZoomRotationFactor > 0.05);
}

void CWeapon::CollectAttachmentsAI(TIItemContainer& l_list)
{
    if (bCollectedAttachmentsForAI)
        return;

    for (TIItemContainer::iterator l_it = l_list.begin(); l_list.end() != l_it; ++l_it)
    {
        PIItem pIItem = *l_it;

        const CScope* pScope = smart_cast<const CScope*>(pIItem);
        if (pScope)
        {
            const bool result = DeterminateParentSlotForAddon(pIItem, this, true);
            if (result)
                Attach(pIItem, true);
        }

        const CSilencer* pSilencer = smart_cast<const CSilencer*>(pIItem);
        if (pSilencer && CanAttach(pIItem) && !IsSilencerAttached())
            Attach(pIItem, true);

        const CGrenadeLauncher* pGrenadeLauncher = smart_cast<const CGrenadeLauncher*>(pIItem);
        if (pGrenadeLauncher && CanAttach(pIItem) && !IsGrenadeLauncherAttached())
            Attach(pIItem, true);
    }

    bCollectedAttachmentsForAI = true;
}

bool CWeapon::DeterminateParentSlotForAddon(PIItem& item, PIItem weapon, bool for_ai)
{
    CScope* pScope = smart_cast<CScope*>(item);
    CWeapon* wpn = smart_cast<CWeapon*>(weapon);

    if (!wpn || !pScope)
        return false;
    if (!wpn->bUseAttachmentSystem)
        return false;
    if (!weapon->CanAttach(pScope))
        return false;
    
    auto attach_plnk = [&](addon_slot slot)
    {
        item->attach_to_slot_name = slot.slot_name;
        item->parent_addon = slot.parent;
        Fvector hpb;
        slot.transform.getHPB(hpb.x, hpb.y, hpb.z);
        if (pScope->m_has_ort && hpb.z < -1.f)
            item->attach_to_ort = CInventoryItem::EIIAddonOrt::FOrtRight;
        else if (pScope->m_has_ort)
            item->attach_to_ort = CInventoryItem::EIIAddonOrt::FOrtLeft;
        else
            item->attach_to_ort = CInventoryItem::EIIAddonOrt::FOrtNone;
    };
    auto attach_scope = [&](addon_slot slot)
    {
        item->attach_to_slot_name = slot.slot_name;
        item->parent_addon = slot.parent;
        item->attach_to_ort = CInventoryItem::EIIAddonOrt::FOrtNone;
    };

    
    addon_slot plnk_1_slot;
    addon_slot plnk_1_slot_busy_but_compatible;
    addon_slot plnk_2_slot;
    addon_slot wpn_1_slot;
    addon_slot wpn_1_slot_busy_but_compatible;
    addon_slot wpn_2_slot;
    addon_slot wpn_3_slot;
    addon_slot wpn_4_slot;
    addon_slot wpn_5_slot;
    addon_slot wpn_last_free_slot;
    addon_slot free_slot;

    auto slots = wpn->getAvaliableSlots();
    for (auto slot : slots)
    {
        if (slot.parent == 0 && xr_strcmp(slot.slot_name.c_str(), WPN_MAIN_SLOT) == 0 && slot.busy_by.c_str() == nullptr && slot.slot_type == pScope->m_slot_type)
            wpn_1_slot = slot;
        if (slot.parent == 0 && xr_strcmp(slot.slot_name.c_str(), WPN_MAIN_SLOT) == 0 && slot.busy_by.c_str() != nullptr && slot.slot_type == pScope->m_slot_type)
            wpn_1_slot_busy_but_compatible = slot;
        if (slot.parent == 0 && slot.busy_by.c_str() == nullptr && xr_strcmp(slot.slot_name.c_str(), "slot_2") == 0 && slot.slot_type == pScope->m_slot_type)
            wpn_2_slot = slot;
        if (slot.parent == 0 && slot.busy_by.c_str() == nullptr && xr_strcmp(slot.slot_name.c_str(), "slot_3") && slot.slot_type == pScope->m_slot_type)
            wpn_3_slot = slot;
        if (slot.parent == 0 && slot.busy_by.c_str() == nullptr && xr_strcmp(slot.slot_name.c_str(), "slot_4") && slot.slot_type == pScope->m_slot_type)
            wpn_4_slot = slot;
        if (slot.parent == 0 && slot.busy_by.c_str() == nullptr && xr_strcmp(slot.slot_name.c_str(), "slot_5") && slot.slot_type == pScope->m_slot_type)
            wpn_5_slot = slot;
        if (slot.parent == 0 && slot.busy_by.c_str() == nullptr && slot.slot_type == pScope->m_slot_type)
            wpn_last_free_slot = slot;
        if (slot.parent != 0 && slot.busy_by.c_str() == nullptr && slot.slot_type == pScope->m_slot_type)
            free_slot = slot;
        if ((slot.parent_section.c_str() != nullptr && (strstr(slot.parent_section.c_str(), "plnk_") || strstr(slot.parent_section.c_str(), "atch_"))) && xr_strcmp(slot.slot_name.c_str(), WPN_MAIN_SLOT) == 0 && slot.busy_by.c_str() == nullptr && slot.slot_type == pScope->m_slot_type)
            plnk_1_slot = slot;
        if ((slot.parent_section.c_str() != nullptr && (strstr(slot.parent_section.c_str(), "plnk_") || strstr(slot.parent_section.c_str(), "atch_"))) && xr_strcmp(slot.slot_name.c_str(), WPN_MAIN_SLOT) == 0 && slot.busy_by.c_str() != nullptr && slot.slot_type == pScope->m_slot_type)
            plnk_1_slot_busy_but_compatible = slot;
        if ((slot.parent_section.c_str() != nullptr && (strstr(slot.parent_section.c_str(), "plnk_") || strstr(slot.parent_section.c_str(), "atch_"))) && xr_strcmp(slot.slot_name.c_str(), "slot_2") == 0 && slot.busy_by.c_str() == nullptr && slot.slot_type == pScope->m_slot_type)
            plnk_2_slot = slot;
    }

    if (pScope->HasScopeTexture())
    {
        if (wpn_1_slot)
        {
            attach_scope(wpn_1_slot);
            return true;
        }
        if (plnk_1_slot)
        {
            attach_scope(plnk_1_slot);
            return true;
        }
        if (plnk_1_slot_busy_but_compatible)
        {
            attach_scope(plnk_1_slot_busy_but_compatible);
            return true;
        }
        if (wpn_1_slot_busy_but_compatible)
        {
            attach_scope(wpn_1_slot_busy_but_compatible);
            return true;
        }
    }
    if (pScope->IsLsa())
    {
        if (wpn_3_slot)
        {
            attach_scope(wpn_3_slot);
            return true;
        }
        if (wpn_2_slot)
        {
            attach_scope(wpn_2_slot);
            return true;
        }
        if (plnk_1_slot)
        {
            attach_scope(plnk_1_slot);
            return true;
        }
        if (plnk_2_slot)
        {
            attach_scope(plnk_2_slot);
            return true;
        }
        if (wpn_last_free_slot)
        {
            attach_scope(wpn_last_free_slot);
            return true;
        }
        if (free_slot)
        {
            attach_scope(free_slot);
            return true;
        }
    }

    if (strstr(item->m_section_id.c_str(), "plnk_"))
    {
        if (wpn_2_slot)
        {
            attach_plnk(wpn_2_slot);
            return true;
        }
        if (wpn_3_slot)
        {
            attach_plnk(wpn_3_slot);
            return true;
        }
        if (wpn_4_slot)
        {
            attach_plnk(wpn_4_slot);
            return true;
        }
        if (wpn_5_slot)
        {
            attach_plnk(wpn_5_slot);
            return true;
        }
        if (wpn_last_free_slot)
        {
            attach_plnk(wpn_last_free_slot);
            return true;
        }
        if (free_slot)
        {
            attach_plnk(free_slot);
            return true;
        }
    }

    if (wpn_1_slot)
    {
        attach_scope(wpn_1_slot);
        return true;
    }
    if (plnk_1_slot)
    {
        attach_scope(plnk_1_slot);
        return true;
    }
    if (plnk_2_slot)
    {
        attach_scope(plnk_2_slot);
        return true;
    }
    if (wpn_2_slot)
    {
        attach_scope(wpn_2_slot);
        return true;
    }
    if (wpn_3_slot)
    {
        attach_scope(wpn_3_slot);
        return true;
    }
    if (wpn_4_slot)
    {
        attach_scope(wpn_4_slot);
        return true;
    }
    if (wpn_5_slot)
    {
        attach_scope(wpn_5_slot);
        return true;
    }
    if (wpn_last_free_slot)
    {
        attach_scope(wpn_last_free_slot);
        return true;
    }
    if (free_slot)
    {
        attach_scope(free_slot);
        return true;
    }
    
    return false;
}

std::pair<u32, addon_item*> CWeapon::GetAddonMainScope() const
{
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_scope_texture)
            return std::make_pair(addon_id, addon);
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_scope_texture || (addon->on_first_line && (addon->has_aim_offset || addon->has_second_aim_offset)))
            return std::make_pair(addon_id, addon);
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_aim_offset || addon->has_second_aim_offset)
            return std::make_pair(addon_id, addon);

    return std::make_pair(0, nullptr);
}
std::pair<u32, addon_item*> CWeapon::GetAddonFromSlot(u32 parent_id, shared_str slot_name) const
{
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->parent_id == parent_id && addon->slot == slot_name)
            return std::make_pair(addon_id, addon);

    return std::make_pair(0, nullptr);
}
u16 CWeapon::getCountInstalledSecondAimAddons() const
{
    u16 count = 0;
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_second_aim_offset)
            count++;

    return count;
}
void CWeapon::setSecondZoomOnFirstScopeIfHaveIt()
{
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_second_aim_offset)
            addon->is_latest_zoomed = true;
}
void CWeapon::UpdateAvailableSecondZoom()
{
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_second_aim_offset)
        {
            m_zoom_params.m_bZoomSecondEnabled = true;
            return;
        }

    m_zoom_params.m_bZoomSecondEnabled = false;
}
shared_str CWeapon::GetInstalledMagType()
{
    shared_str type = "";
    for (auto [addon_id, addon]: m_addon_items)
        if (xr_strcmp(addon->slot.c_str(), "slot_mag") == 0 && pSettings->line_exist(addon->addon_item_name.c_str(), "mag_type"))
            return make_string("_%s", pSettings->r_string(addon->addon_item_name.c_str(), "mag_type")).c_str();

    return type;
}
shared_str CWeapon::GetInstalledTacGripType()
{
    shared_str type = "";
    for (auto [addon_id, addon]: m_addon_items)
        if (xr_strcmp(addon->slot.c_str(), "tac_grip") == 0 && pSettings->line_exist(addon->addon_item_name.c_str(), "grip_type"))
            return make_string("_%s", pSettings->r_string(addon->addon_item_name.c_str(), "grip_type")).c_str();

    return type;
}
bool CWeapon::HasAddonByName(shared_str name)
{
    for (auto [addon_id, addon]: m_addon_items)
        if (xr_strcmp(addon->addon_item_name.c_str(), name.c_str()) == 0)
            return true;

    return false;
}

bool CWeapon::HasAddonWithMagSize()
{
    for (auto [addon_id, addon]: m_addon_items)
        if (addon->has_mag_size)
            return true;

    return false;
}

xr_vector<addon_slot> CWeapon::getAvaliableSlots() const
{
    xr_vector<addon_slot> slots;

    for (auto slot: m_addon_slots)
        if (slot.second)
            slots.push_back(*slot.second);
    for (auto [addon_id, addon]: m_addon_items)
    {
        for (auto slot: addon->addon_slots)
        {
            addon_slot item;
            item.slot_name = slot.first;
            item.parent_section = addon->addon_item_name;
            item.parent_addon_section = addon->parent;
            item.transform = slot.second.transform;
            item.parent = addon_id;
            item.slot_type = addon->provided_slot_type;

            slots.push_back(item);
        }
    }
    for (auto [addon_id, addon]: m_addon_items)
    {
        if (addon->slot.c_str() == nullptr || xr_strcmp(addon->slot.c_str(), "") == 0)
            continue;

        std::for_each(slots.begin(), slots.end(), [&addon](addon_slot& slot) {
            if (addon->parent_id == slot.parent && slot.slot_name == addon->slot)
                slot.busy_by = addon->addon_item_name;
        });
    }

    return slots;
}

void CWeapon::addAddon(PIItem item)
{
    CScope* scope = smart_cast<CScope*>(item);
    R_ASSERT3(scope != nullptr, "Can't add addon to weapon: addon is not WP_SCOPE class", item->m_section_id.c_str());

    AddAddonData data;
    data.item_section_id = item->m_section_id;
    data.addon_type = scope->m_addon_type;
    data.slot_name = item->attach_to_slot_name;
    data.ort = item->attach_to_ort;
    data.addon_id = ADDON_ID_NONE;
    data.parent_id = item->parent_addon;
    data.has_scope_texture = scope->HasScopeTexture();
    data.provided_slot_type = scope->m_provided_slot_type;
    data.has_ort = scope->m_has_ort;
    data.scope_dynamic_zoom = scope->m_scope_dynamic_zoom;

    addAddon(data);
}
void CWeapon::addAddon(AddAddonData data)
{
    addon_item* new_addon = xr_new<addon_item>();

    new_addon->addon_item_name = data.item_section_id;
    new_addon->addon_type = data.addon_type;
    new_addon->slot = data.slot_name;
    new_addon->ort = data.ort;
    new_addon->parent_id = data.parent_id;
    new_addon->addon_item_pos.identity();
    new_addon->has_scope_texture = data.has_scope_texture;
    new_addon->provided_slot_type = data.provided_slot_type;
    new_addon->scope_dynamic_zoom = data.scope_dynamic_zoom;
    new_addon->has_mag_size = data.has_mag_size;
    new_addon->was_inited_in_default_slots = data.was_inited_in_default_slots;
    
    if (pSettings->line_exist(new_addon->addon_item_name.c_str(), "ammo_mag_size"))
    {
        iMagazineSize = pSettings->r_u32(new_addon->addon_item_name.c_str(), "ammo_mag_size");
        new_addon->has_mag_size = true;

        NET_Packet P;
        CHudItem::object().u_EventGen(P, GE_WPN_STATE_CHANGE, CHudItem::object().ID());
        P.w_u8(u8(eReload));
        P.w_u8(u8(m_sub_state));
        P.w_u8(m_ammoType);
        P.w_u8(u8(iAmmoElapsed & 0xff));
        P.w_u8(m_set_next_ammoType_on_reload);
        CHudItem::object().u_EventSend(P, net_flags(TRUE, TRUE, FALSE, TRUE));
    }

    if (data.has_ort)
    {
        shared_str prop = new_addon->ort == CInventoryItem::EIIAddonOrt::FOrtRight ? "visual_right" : "visual";
        new_addon->addon_item_model = smart_cast<IKinematics*>(GEnv.Render->model_Create(pSettings->r_string(new_addon->addon_item_name.c_str(), prop.c_str())));
        new_addon->prop_model_name = prop;
    }
    else
    {
        new_addon->addon_item_model = smart_cast<IKinematics*>(GEnv.Render->model_Create(pSettings->r_string(new_addon->addon_item_name.c_str(), "visual")));
        new_addon->prop_model_name = "visual";
    }

    new_addon->addon_item_model->CalculateBones_Invalidate();
    new_addon->addon_item_model->CalculateBones(TRUE);

    if (pSettings->line_exist(m_section_id.c_str(), make_string("%s_scale", new_addon->addon_item_name.c_str()).c_str()))
    {
        new_addon->scale = pSettings->r_float(m_section_id.c_str(), make_string("%s_scale", new_addon->addon_item_name.c_str()).c_str());
    }

    u32 addon_id = m_addon_id++;
    if (data.addon_id != ADDON_ID_NONE)
        addon_id = data.addon_id;
    R_ASSERT2(m_addon_items[addon_id] == nullptr, make_string("Addon with id: %d already exist", addon_id).c_str());

    addon_item* parent_item = new_addon->parent_id == 0 ? new_addon : m_addon_items[new_addon->parent_id];
    R_ASSERT3(parent_item, "Parent addon not found by id", make_string("%d", new_addon->parent_id).c_str());
    new_addon->parent = parent_item->addon_item_name;
    
    if((new_addon->parent_id == 0 && xr_strcmp(new_addon->slot.c_str(), WPN_MAIN_SLOT) == 0) || parent_item->on_first_line)
        new_addon->on_first_line = true;

    addon_slot target_slot = new_addon->parent_id == 0 ? *m_addon_slots[new_addon->slot] : parent_item->addon_slots[new_addon->slot];
    Fmatrix slot_transform = target_slot.transform;
    
    bool has_bone = target_slot.bone_name.c_str() != nullptr && xr_strcmp(target_slot.bone_name.c_str(), "") != 0;
    bool has_bone_2 = target_slot.bone_2_name.c_str() != nullptr && xr_strcmp(target_slot.bone_2_name.c_str(), "") != 0;
    new_addon->bone_name = has_bone ? target_slot.bone_name : parent_item->bone_name;
    new_addon->bone_2_name = has_bone_2 ? target_slot.bone_2_name : parent_item->bone_2_name;

    new_addon->has_bone_2 = has_bone_2;

    if (target_slot.bone_2_name.c_str() != nullptr)
        new_addon->addon_item_model_2 = smart_cast<IKinematics*>(GEnv.Render->model_Create(pSettings->r_string(new_addon->addon_item_name.c_str(), "visual")));
    
    Fvector slot_rot;
    slot_transform.getHPB(slot_rot.x, slot_rot.y, slot_rot.z);
    
    if (has_bone)
        slot_rot.z = 0.0f;

    float ortY = new_addon->ort == CInventoryItem::EIIAddonOrt::FOrtNone ? 0.0f
        : new_addon->ort == CInventoryItem::EIIAddonOrt::FOrtRight ? deg2rad(-90.0f) : deg2rad(90.0f);
    float ortZ = parent_item->ort == CInventoryItem::EIIAddonOrt::FOrtNone ? slot_rot.z
        : parent_item->ort == CInventoryItem::EIIAddonOrt::FOrtLeft ? slot_rot.z : abs(slot_rot.z);
    float hudRotZ = ortZ;

    if (new_addon->parent_id == 0)
        parent_item->inherited_aim_z_rot = slot_rot.z;

    u16 bi = new_addon->addon_item_model->LL_BoneID(DOT);
    if (bi != BI_NONE)
        hudRotZ = deg2rad(-50.0f);

    clamp(hudRotZ, -0.8f, 0.8f);
    clamp(parent_item->inherited_aim_z_rot, -0.8f, 0.8f);

    if (parent_item->inherited_aim_z_rot == 0.0f)
        new_addon->inherited_aim_z_rot = hudRotZ;
    else
        new_addon->inherited_aim_z_rot = parent_item->inherited_aim_z_rot;

    new_addon->addon_aim_z_rot = new_addon->inherited_aim_z_rot;

    Fmatrix trans, world_trans;

    trans.mul(parent_item->addon_item_pos, slot_transform);
    world_trans.mul(parent_item->addon_item_pos, bAttachmentSystemOffsetOnWorldModel);
    world_trans.mulB_43(target_slot.transform_world);

    u16 index = 1;
    shared_str slot_name;
    
    while (true)
    {
        slot_name = make_string("slot_%d", index).c_str();
        u16 bone_id = new_addon->addon_item_model->LL_BoneID(slot_name);
        if (bone_id == BI_NONE)
            break;

        Fmatrix bone_transform = new_addon->addon_item_model->LL_GetTransform(bone_id);
        addon_slot slot;
        slot.slot_name = slot_name;
        slot.transform.mul(new_addon->addon_item_pos, bone_transform);

        new_addon->addon_slots.insert(std::make_pair(slot_name, slot));

        index++;
    }

    new_addon->addon_item_pos.set(trans);

    u16 bone_id = new_addon->addon_item_model->LL_BoneID(DOT);
    if (bone_id != BI_NONE)
    {
        // Скрываем все кости у второй модели ЛЦУ кроме рутовой и dot
        new_addon->addon_item_model_dot = smart_cast<IKinematics*>(GEnv.Render->model_Create(pSettings->r_string(data.item_section_id.c_str(), "visual")));
        new_addon->addon_item_model_dot->CalculateBones_Invalidate();
        new_addon->addon_item_model_dot->CalculateBones(TRUE);
        for (const auto& [bone_name, bi] : *new_addon->addon_item_model_dot->LL_Bones())
        {
            if (bi == new_addon->addon_item_model_dot->LL_GetBoneRoot())
                continue;
            if (xr_strcmp(bone_name.c_str(), DOT) == 0)
                continue;

            new_addon->addon_item_model_dot->LL_SetBoneVisible(bi, FALSE, FALSE);
        }

        // У основной модели ЛЦУ скрываем только ксть dot (точку)
        new_addon->addon_item_model->LL_SetBoneVisible(bone_id, FALSE, FALSE);
        m_zoom_params.m_bZoomSecondEnabled = true;
        new_addon->has_second_aim_offset = true;
    }
    bone_id = new_addon->addon_item_model->LL_BoneID(WPN_SCOPE_2);
    if (bone_id != BI_NONE)
    {
        m_zoom_params.m_bZoomSecondEnabled = true;
        new_addon->has_second_aim_offset = true;
    }
    m_zoom_params.m_bUseDynamicZoom = new_addon->scope_dynamic_zoom;

    new_addon->addon_item_pos_world = world_trans;

    m_addon_items[addon_id] = new_addon;

    calc_aim_addon_offset();
}

// Метод для расчета оффсета кости прицела в режиме прицеливания
// ВХодные параметры:
// - hud_transform - трансформация худа в мировых координатах
// - addon_offset - смещение и поворот аддона прицела в локальных координатах
// - bone_transform - трансформация кости (wpn_scope или wpn_scope_2) прицела внутри модели
// - hud_aim_target_pos - центр экрана в мировых координатах куда хотим сместить худ в режиме прицеливания
// - rotation_matrix - матрица поворота прицела, учитываем ее если нужно повернуть худ при прицеливании
// - add_rot - поворот худа в режиме прицеливания (радианы), не участует в расчете, только записываем в конечный результат поворота
// - need_calc_with_rot - нужно ли учитывать поворот прицела при расчете оффсета
// - сoff - коэффициент для корректировки глубины худа (ближе к камере или дальше)
// - bone_name - имя кости, трансформацию которой копирует аддон (ее так же нужно учитывать)
// Возвращаемые параметры:
// - out_offset - оффсет худа в локальных координатах в режиме прицеливания
// - out_rot - поворот худа в режиме прицеливания (радианы), записываем в конечный результат поворота
void CWeapon::get_aim_offset_to_center(
    Fmatrix hud_transform,
    Fmatrix addon_offset,
    Fmatrix bone_transform,
    Fvector hud_aim_target_pos,
    Fmatrix rotation_matrix,
    Fvector add_rot,
    bool need_calc_with_rot,
    float coff,
    shared_str bone_name,
    Fvector& out_offset,
    Fvector& out_rot
)
{
    attachable_hud_item* hi = HudItemData();
    if (!hi)
        return;

    // 1. Получаем базовую трансформацию худа (обычно это позиция оружия в первом кадре анимации idle)
    Fmatrix m_item_transform = hud_transform;
    Fmatrix bone_transform_2;
    bone_transform_2.identity();

    Fmatrix hand_ancor_transform = hi->m_parent->tmp;

    // 2. Получаем трансформацию кости оружия к которой прикреплен аддон если такая кость была указана в конфиге
    bool has_bone = bone_name.c_str() != nullptr && xr_strcmp(bone_name.c_str(), "") != 0;
    if (has_bone)
    {
        const u16 bone_id = hi->m_model_2->LL_BoneID(bone_name.c_str());
        if (bone_id != BI_NONE)
            bone_transform_2.set(hi->m_model_2->LL_GetTransform(bone_id));
    }

    // 3. Создаем матрицу с дополнительным поворотом
    Fmatrix m_item_with_rot = m_item_transform;
    if (need_calc_with_rot)
        m_item_with_rot.mulB_43(rotation_matrix);

    // 4. Позиция кости с кастомным поворотом
    Fmatrix scope_global_with_rot;
    scope_global_with_rot.set(m_item_with_rot);
    if (has_bone)
        scope_global_with_rot.mulB_43(bone_transform_2);
    scope_global_with_rot.mulB_43(addon_offset);
    scope_global_with_rot.mulB_43(bone_transform);

    // 5. Вычисляем где должна быть кость БЕЗ кастомного поворота
    Fmatrix scope_global_no_rot;
    scope_global_no_rot.set(m_item_transform);
    if (has_bone)
        scope_global_no_rot.mulB_43(bone_transform_2);
    scope_global_no_rot.mulB_43(addon_offset);
    scope_global_no_rot.mulB_43(bone_transform);

    // 6. Вычисляем коррекцию между этими позициями
    Fvector world_correction;
    world_correction.sub(scope_global_no_rot.c, scope_global_with_rot.c);

    // 7. Вычисляем оффсет для смещения кости в центре экрана
    Fvector world_center_correction;
    world_center_correction.sub(hud_aim_target_pos, scope_global_with_rot.c);

    // 8. Преобразуем коррекции в локальные координаты
    Fmatrix inv_item_transform;
    inv_item_transform.invert(m_item_with_rot);
    Fvector local_correction;
    inv_item_transform.transform_dir(local_correction, world_correction);
    Fvector local_center_correction;
    inv_item_transform.transform_dir(local_center_correction, world_center_correction);
    Fvector local_correction_w_rot;
    local_correction_w_rot.set(local_center_correction);

    // 9. Компенсируем разницу если есть поворот
    if (need_calc_with_rot)
        local_correction_w_rot.sub(local_correction);

    // 10. Записываем результаты
    out_offset.set(
        local_correction_w_rot.x,
        local_correction_w_rot.y,
        local_correction_w_rot.z * 0.2f * coff
    );
    out_rot.set(add_rot);

}
void CWeapon::calc_aim_addon_offset()
{
    if (!bUseAttachmentSystem)
        return;

    CDebugRenderer& render = Level().debug_renderer();

    bool latest_was_inited = false;
    Fvector hud_aim_pos, hud_aim_rot;
    float depth = 0.5f;
    for (auto [addon_id, item] : m_addon_items)
    {
        attachable_hud_item* hi = HudItemData();
        if (!hi)
            continue;

        Fmatrix bone_transform;
        Fmatrix m_item_transform = hi->hud_transform;
        Fmatrix hand_ancor_transform = hi->m_parent->tmp;
        Fmatrix ancor_rot_mtx;
        Fvector ancor_rot;
        hand_ancor_transform.getHPB(ancor_rot.x, ancor_rot.y, ancor_rot.z);
        ancor_rot_mtx.identity();
        ancor_rot_mtx.setHPB(ancor_rot.x, ancor_rot.y, ancor_rot.z);

        if (!bApplyAncorTransform)
            ancor_rot.set(0.0f, 0.0f, 0.0f);

        // 3. Вычисляем центр экрана
        Fmatrix hud_cam;
        Actor()->Cameras().hud_camera_Matrix(hud_cam);
        // центр экрана в мировых координатах куда хотим сместить худ в режиме прицеливания
        Fvector hud_aim_target_pos;
        hud_aim_target_pos.mad(hud_cam.c, hud_cam.k, depth);
    
        Fmatrix addon_offset = item->addon_item_pos;
        u8 idx = item->on_first_line ? 3 : 4;

        u16 bone_id = item->addon_item_model->LL_BoneID(WPN_SCOPE);
        if (bone_id != BI_NONE)
        {
            // 1. Получаем трансформацию кости прицела
            bone_transform = item->addon_item_model->LL_GetTransform(bone_id);
            bone_transform.c.z = 0.0f;

            Fmatrix offset_trans, updated_transform;
            updated_transform.set(m_item_transform);
            Fvector correct_offset = hi->m_measures.m_hands_offset[0][idx];
            offset_trans.identity();
            offset_trans.translate_over(correct_offset);
            updated_transform.mulB_43(offset_trans);

            // берем попорот худа в режиме прицеливания (радианы)
            Fvector rot = ancor_rot;
            Fmatrix rotation_matrix;
            rotation_matrix.identity();
            Fmatrix R;
            Fvector correct_rot = hi->m_measures.m_hands_offset[1][idx];
            rot.add(correct_rot);
            R.rotateZ(-item->addon_aim_z_rot); rotation_matrix.mulA_43(R);
            R.rotateX(correct_rot.x); rotation_matrix.mulA_43(R);
            R.rotateY(correct_rot.y); rotation_matrix.mulA_43(R);
            R.rotateZ(correct_rot.z); rotation_matrix.mulA_43(R);

            rot.z += item->addon_aim_z_rot;

            get_aim_offset_to_center(
                updated_transform,
                addon_offset,
                bone_transform,
                hud_aim_target_pos,
                rotation_matrix,
                rot,
                // false,
                !fis_zero(rot.magnitude()),
                g_aim_z_offset_coff,
                item->bone_name,
                item->calc_aim_offset,
                item->calc_aim_rot
            );

            item->has_aim_offset = true;
        }

        bone_id = item->addon_item_model->LL_BoneID(WPN_SCOPE_2);
        if (bone_id != BI_NONE)
        {
            // Получаем трансформацию кости
            bone_transform = item->addon_item_model->LL_GetTransform(bone_id);
            bone_transform.c.z = 0.0f;

            Fmatrix offset_trans, updated_transform;
            updated_transform.set(m_item_transform);
            Fvector correct_offset = hi->m_measures.m_hands_offset[0][idx];
            offset_trans.identity();
            offset_trans.translate_over(correct_offset);
            updated_transform.mulB_43(offset_trans);

            // берем попорот худа в режиме прицеливания (радианы)
            Fvector rot = ancor_rot;
            Fmatrix rotation_matrix;
            rotation_matrix.identity();
            Fmatrix R;
            Fvector correct_rot = hi->m_measures.m_hands_offset[1][idx];
            rot.add(correct_rot);
            R.rotateZ(-item->addon_aim_z_rot); rotation_matrix.mulA_43(R);
            R.rotateY(correct_rot.y); rotation_matrix.mulA_43(R);
            R.rotateX(correct_rot.x); rotation_matrix.mulA_43(R);
            R.rotateZ(correct_rot.z); rotation_matrix.mulA_43(R);

            rot.z += item->addon_aim_z_rot;

            get_aim_offset_to_center(
                updated_transform,
                addon_offset,
                bone_transform,
                hud_aim_target_pos,
                rotation_matrix,
                rot,
                !fis_zero(rot.magnitude()),
                g_second_aim_z_offset_coff,
                item->bone_name,
                item->calc_second_aim_offset,
                item->calc_second_aim_rot
            );

            item->has_second_aim_offset = true;
        }

        bone_id = item->addon_item_model->LL_BoneID(DOT);
        if (bone_id != BI_NONE)
        {
            float depth = 5.0f;
            Fmatrix bone_dot_world;
            bone_dot_world.set(m_item_transform);

            Fvector hud_dot_target_pos;
            hud_dot_target_pos.mad(hud_cam.c, hud_cam.k, depth);

            // 4. Вычисляем полную коррекцию
            Fvector world_correction;
            world_correction.sub(hud_dot_target_pos, bone_dot_world.c);

            // 5. Преобразуем в локальные координаты HUD
            Fmatrix inv_trans;
            inv_trans.invert(m_item_transform);
            Fvector local_correction;
            inv_trans.transform_dir(local_correction, world_correction);
            // local_correction.y = local_correction.y * 2.4f;

            item->addon_item_pos_dot.set(local_correction);

            item->calc_aim_offset.set(-0.04f, -0.04f, 0);
            item->calc_aim_rot.set(0, 0, -0.2f);
            item->calc_second_aim_offset.set(-0.04f, -0.04f, 0);
            item->calc_second_aim_rot.set(
                0.f,
                0.f,
                -0.2f
            );
            item->has_second_aim_offset = true;
        }

        if (item->has_second_aim_offset && !latest_was_inited)
        {
            hud_aim_pos = item->calc_second_aim_offset;
            hud_aim_rot = item->calc_second_aim_rot;
        }
        if (item->is_latest_zoomed)
            latest_was_inited = true;
    }

    m_hands_offset[0][1].set(hud_aim_pos);
    m_hands_offset[1][1].set(hud_aim_rot);

    UpdateAltScope();
    UpdateAddonsVisibility();
    InitAddons();
    LoadAltHudAim();
    UpdateAddonsOffset();
}

void CWeapon::SwitchZoomableAddon(bool forward)
{
    auto current = FindCurrentZoomedAddon();
    auto found = m_addon_items.end();
    
    if (current != m_addon_items.end())
    {
        auto next = forward ? std::next(current) : current == m_addon_items.begin() 
                          ? m_addon_items.end() : std::prev(current);
        found = FindNextAddon(next, forward);
    }
    
    if (found == m_addon_items.end())
        found = FindNextAddon(forward ? m_addon_items.begin() : std::prev(m_addon_items.end()), forward);

    UpdateZoomedAddon(current, found);
}

CWeapon::AddonIter CWeapon::FindNextAddon(CWeapon::AddonIter start, bool forward)
{
    if (m_addon_items.empty() || start == m_addon_items.end())
        return m_addon_items.end();

    auto it = start;
    const auto& end = forward ? m_addon_items.end() : m_addon_items.begin();

    while (it != end) 
    {
        if (it->second && IsAddonSuitableForZoom(*it))
            return it;

        forward ? ++it : --it;

        // Защита от выхода за границы контейнера
        if (it == m_addon_items.end())
            break;
    }

    // Дополнительная проверка для reverse-итерации (если дошли до begin)
    if (!forward && it == end && end != m_addon_items.end() && IsAddonSuitableForZoom(*end))
        return end;

    return m_addon_items.end();
}

bool CWeapon::IsAddonSuitableForZoom(const std::pair<u32, addon_item*>& addon)
{
    return addon.second && addon.second->has_second_aim_offset;
}

CWeapon::AddonIter CWeapon::FindCurrentZoomedAddon()
{
    return std::find_if(m_addon_items.begin(), m_addon_items.end(), [](std::pair<u32, addon_item*> item) {
        return item.second->is_latest_zoomed;
    });
}

void CWeapon::UpdateZoomedAddon(AddonIter current, AddonIter found)
{
    if (found == m_addon_items.end()) return;

    if (current != m_addon_items.end())
        current->second->is_latest_zoomed = false;
    
    found->second->is_latest_zoomed = true;
    m_hands_offset[0][1].set(found->second->calc_second_aim_offset);
    m_hands_offset[1][1].set(found->second->calc_second_aim_rot);
}

