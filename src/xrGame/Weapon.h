#pragma once

#include "xrPhysics/PhysicsShell.h"
#include "WeaponAmmo.h"
#include "PHShellCreator.h"

#include "ShootingObject.h"
#include "hud_item_object.h"
#include "Actor_Flags.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "firedeps.h"
#include "game_cl_single.h"
#include "first_bullet_controller.h"

#include "CameraRecoil.h"
#include "weapon_inv_icon.h"

class CEntity;
class ENGINE_API CMotionDef;
class CSE_ALifeItemWeapon;
class CSE_ALifeItemWeaponAmmo;
class CWeaponMagazined;
class CParticlesObject;
class CUIWindow;
class CBinocularsVision;
class CNightVisionEffector;

struct addon_slot {
    shared_str slot_name;
    shared_str parent_section;
    shared_str parent_addon_section;
    shared_str bone_name;
    shared_str bone_2_name;
    u32 parent;
    Fmatrix transform;
    Fmatrix transform_2;
    Fmatrix transform_world;
    shared_str busy_by;
    u16 slot_type;

    explicit operator bool() const {
        return !slot_name.empty(); // или любое другое условие
    }
};

class addon_item
{
public:
    u32 parent_id;
	shared_str addon_item_name;
	shared_str addon_type;
	shared_str slot;
	shared_str parent;
	shared_str prop_model_name;
    shared_str bone_name;
    shared_str bone_2_name;
	Fmatrix addon_item_transform;
	float addon_aim_z_rot;
	float inherited_aim_z_rot;
	float scale;
	Fmatrix addon_item_pos;
	Fmatrix addon_item_dot_t;
	Fmatrix addon_item_pos_world;
	Fvector calc_aim_offset{};
	Fvector calc_aim_rot{};
	Fvector calc_second_aim_offset{};
	Fvector calc_second_aim_rot{};
	IKinematics* addon_item_model;
	IKinematics* addon_item_model_2;
	IKinematics* addon_item_model_dot;
	BOOL is_dot_pos_initialized{false};
	BOOL has_second_aim_offset{false};
	BOOL has_aim_offset{false};
	BOOL is_latest_zoomed{false};
	BOOL has_scope_texture{false};
	BOOL on_first_line{false};
	BOOL scope_dynamic_zoom{false};
	BOOL has_mag_size{false};
	BOOL was_inited_in_default_slots{false};
	BOOL has_bone_2{false};
    xr_map<shared_str, addon_slot> addon_slots;
    CInventoryItem::EIIAddonOrt ort;
    u16 provided_slot_type;
};

struct AddAddonData {
    shared_str item_section_id;
    shared_str slot_name;
    shared_str addon_type;
    CInventoryItem::EIIAddonOrt ort;
    u32 addon_id; // нужно только для того чтобы корректно востановить аддоны при загрузке
    u32 parent_id;
    u16 provided_slot_type;
    bool has_scope_texture{false};
    bool scope_dynamic_zoom{false};
    bool has_ort{false};
    bool has_mag_size{false};
    bool was_inited_in_default_slots{false};
    // When restoring addons without changing gameplay state (e.g. ini hot reload).
    bool skip_magazine_sync_on_add{};
};

class CWeapon : public CHudItemObject, public CShootingObject
{
    typedef CHudItemObject inherited;

public:
    CWeapon();
    virtual ~CWeapon();

    // аддоны и управление аддонами
    bool bUseAltScope;
    bool bScopeIsHasTexture;
    bool bNVsecondVPavaible;
    bool bNVsecondVPstatus;
    bool bUseAttachmentSystem;
    bool bCollectedAttachmentsForAI;
    bool bApplyAncorTransform{false};

    shared_str sDontDetachableSlots;

    Fmatrix bAttachmentSystemOffsetOnWorldModel;

    xr_map<u32, addon_item*> m_addon_items;
    mutable xr_map<shared_str, addon_slot*> m_addon_slots;

    void addAddon(AddAddonData data);
    void addAddon(PIItem item);
    void calc_aim_addon_offset();
    void get_aim_offset_to_center(
        Fmatrix hud_transform,
        Fmatrix hud_cam,
        Fmatrix addon_offset,
        Fmatrix bone_transform,
        Fvector hud_aim_target_pos,
        Fmatrix rotation_matrix,
        const Fvector& correct_offset,
        const Fvector& correct_rot,
        Fvector add_rot,
        bool need_calc_with_rot,
        float coff,
        shared_str bone_name,
        Fvector& out_offset,
        Fvector& out_rot
    );
    void CollectAttachmentsAI(TIItemContainer& l_list);
    bool DeterminateParentSlotForAddon(PIItem& item, PIItem weapon, bool for_ai = false);
    bool HasAddonByName(shared_str name);
    bool HasAddonWithMagSize();
    std::pair<u32, addon_item*> GetAddonFromSlot(u32 parent_id, shared_str slot_name) const;
    std::pair<u32, addon_item*> GetAddonMainScope() const;
    u16 getCountInstalledSecondAimAddons() const;
    void setSecondZoomOnFirstScopeIfHaveIt();
    void HotReloadModelsAfterSystemIni();
    shared_str GetSlotKey(shared_str slot_name, u32 addon_parent_id, u32 addon_id);

    virtual bool bInZoomRightNow() const { return !IsSecondZoomed() && m_zoom_params.m_fZoomRotationFactor > 0.05; }
    IC bool bIsSecondVPZoomPresent() const { return GetSecondVPZoomFactor() > 0.000f; }
    bool bLoadAltScopesParams(LPCSTR section);
    bool bLoadzCollimatorScopesParams(LPCSTR section);
    virtual bool bMarkCanShow() { return IsZoomed() || IsSecondZoomed(); }
    bool bChangeNVSecondVPStatus();

    virtual void UpdateSecondVP(bool bInGrenade = false);
    void LoadModParams(LPCSTR section);
    void Load3DScopeParams(LPCSTR section);
    void LoadOriginalScopesParams(LPCSTR section);
    void LoadCurrentScopeParams(LPCSTR section);
    void GetZoomData(const float scope_factor, float& delta, float& min_zoom_factor) const;
    void ZoomDynamicMod(bool bIncrement, bool bForceLimit);
    void UpdateAltScope();

    virtual float GetControlInertionFactor() const;
    IC float GetZRotatingFactor() const { return m_zoom_params.m_fZoomRotationFactor; }
    IC float GetSecondVPZoomFactor() const { return m_zoom_params.m_fSecondVPFovFactor; }
    float GetHudFov();
    float GetSecondVPFov() const;
    float GetScopeLenseZoom() const;
    /// Обновляет сглаженное значение зума линзы и возвращает его (для плавного уменьшения при выходе из прицела)
    float GetScopeLenseZoomSmoothed(float dt);

    shared_str GetNameWithAttachment();

    float m_fScopeInertionFactor;
    float m_fZoomStepCount;
    float m_fZoomMinKoeff;
    // SWM3.0 hud collision
    float m_hud_fov_add_mod;
    float m_nearwall_dist_max;
    float m_nearwall_dist_min;
    float m_nearwall_last_hud_fov;
    float m_nearwall_target_hud_fov;
    float m_nearwall_speed_mod;
    float m_hud_fov_before_zoom;       // HUD FOV до прицеливания (для g_3d_scope_type == 2)
    float m_hud_fov_main_fov_zoom_smoothed; // сглаженный HUD FOV при зуме main FOV (режим 2)

    float m_weapon_hud_config;
    bool m_weapon_hud_config_valid;
    float m_weapon_hud_adjust_smoothed;

    float m_fLR_MovingFactor;  // Фактор бокового наклона худа при ходьбе [-1; +1]
    float m_fLR_CameraFactor;  // Фактор бокового наклона худа при движении камеры [-1; +1]
    float m_fLR_InertiaFactor; // Фактор горизонтальной инерции худа при движении камеры [-1; +1]
    float m_fUD_InertiaFactor; // Фактор вертикальной инерции худа при движении камеры [-1; +1]

    Fvector m_strafe_offset[4][2]; //pos,rot,data1,data2/ normal,aim-GL --#SM+#--

    // End=================================

    // Generic
    virtual void Load(LPCSTR section);

    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Export(NET_Packet& P);
    virtual void net_Import(NET_Packet& P);

    virtual CWeapon* cast_weapon() { return this; }
    virtual CWeaponMagazined* cast_weapon_magazined() { return 0; }
    // serialization
    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);
    virtual bool net_SaveRelevant() { return inherited::net_SaveRelevant(); }
    virtual void UpdateCL();
    virtual void shedule_Update(u32 dt);

    void renderable_Render(u32 context_id, IRenderable* root) override;
    void SetWeaponIconSnapshot(bool v) { m_bWeaponIconSnapshot = v; }
    bool IsWeaponInventoryIconSnapshot() const override { return m_bWeaponIconSnapshot; }
    void render_hud_mode() override;
    bool need_renderable() override;

    virtual void render_item_ui();
    virtual bool render_item_ui_query();

    virtual void OnH_B_Chield();
    virtual void OnH_A_Chield();
    virtual void OnH_B_Independent(bool just_before_destroy);
    virtual void OnH_A_Independent();
    virtual void OnEvent(NET_Packet& P, u16 type); // {inherited::OnEvent(P,type);}

    virtual void Hit(SHit* pHDS);

    virtual void reinit();
    virtual void reload(LPCSTR section);
    virtual void create_physic_shell();
    virtual void activate_physic_shell();
    virtual void setup_physic_shell();

    virtual void SwitchState(u32 S);

    virtual void OnActiveItem();
    virtual void OnHiddenItem();
    virtual void SendHiddenItem(); // same as OnHiddenItem but for client... (sends message to a server)...
    virtual void OnMoveToRuck(const SInvItemPlace& previous_place);

private:
    bool m_bWeaponIconSnapshot{};
    bool default_addons_was_loaded{false};
    u32 m_addon_id{1};
    typedef xr_map<u32, addon_item*>::iterator AddonIter;

    // для переключения вторичного зума между аддонами
    bool IsAddonSuitableForZoom(const std::pair<u32, addon_item*>& addon);
    AddonIter FindCurrentZoomedAddon();
    AddonIter FindNextAddon(AddonIter start, bool forward);
    void UpdateZoomedAddon(AddonIter current, AddonIter found);
    void SwitchZoomableAddon(bool direction);

    void SwitchToNextZoomableAddon() { SwitchZoomableAddon(true); }
    void SwitchToPrevZoomableAddon() { SwitchZoomableAddon(false); }


public:
    virtual bool can_kill() const;
    virtual CInventoryItem* can_kill(CInventory* inventory) const;
    virtual const CInventoryItem* can_kill(const xr_vector<const CGameObject*>& items) const;
    virtual bool ready_to_kill() const;
    virtual bool NeedToDestroyObject() const;
    virtual ALife::_TIME_ID TimePassedAfterIndependant() const;

protected:
    //время удаления оружия
    ALife::_TIME_ID m_dwWeaponRemoveTime;
    ALife::_TIME_ID m_dwWeaponIndependencyTime;

    virtual bool IsHudModeNow();
    void LoadScope(const shared_str& section);

public:
    void signal_HideComplete();
    virtual bool Action(u16 cmd, u32 flags);

    enum EWeaponStates
    {
        eFire = eLastBaseState + 1,
        eFire2,
        eReload,
        eMisfire,
        eMagEmpty,
        eSwitch,
        eUnMisfire,
        eAimStart,
        eAimEnd,
    };
    enum EWeaponSubStates
    {
        eSubstateReloadBegin = 0,
        eSubstateReloadInProcess,
        eSubstateReloadEnd,
    };
    enum EWeaponAddonSlotType
    {
        ePicatinny = 0,
        eWeaver,
        eDovetail,
        eG3,

        eNone = u16(-1)
    };
    enum
    {
        undefined_ammo_type = u8(-1)
    };
    enum EWeaponLatestZoom
    {
        eMainZoom = 1, // Обычный зум
        eSecondZoom, // Вторичный зум
        eGLZoom, // На подствол

        noZoom,
    };

    IC BOOL IsValid() const { return iAmmoElapsed; }
    // Does weapon need's update?
    BOOL IsUpdating();

    BOOL IsMisfire() const;
    BOOL CheckForMisfire();

    BOOL AutoSpawnAmmo() const { return m_bAutoSpawnAmmo; };
    bool IsTriStateReload() const { return m_bTriStateReload; }
    EWeaponSubStates GetReloadState() const { return (EWeaponSubStates)m_sub_state; }
protected:
    bool m_bTriStateReload;

    // Inv icon GPU regen: set from attach/detach / GE_ADDON_CHANGE; consumed at end of CWeapon::reload().
    bool m_defer_inv_icon_invalidate_after_reload{};

    // a misfire happens, you'll need to rearm weapon
    bool bMisfire;
    bool bClearJamOnly; //used for "reload" misfire animation

    BOOL m_bAutoSpawnAmmo;
    virtual bool AllowBore();

public:
    u8 m_sub_state; // Alundaio: made public
    bool b_forceIconUpdate = false;

    bool IsGrenadeLauncherAttached() const;
    bool IsScopeAttached() const;
    bool IsScopePermament() const;
    bool IsSilencerAttached() const;
    bool IsAddonCanBeDetached(addon_item* addon) const;

    bool mainScopeSlotIsBusy() const;

    xr_vector<addon_slot> getAvaliableSlots() const;

    ALife::EWeaponAddonStatus GetScopeStatusParent() const;

    virtual bool GrenadeLauncherAttachable();
    virtual bool ScopeAttachable();
    virtual bool SilencerAttachable();

    ALife::EWeaponAddonStatus get_GrenadeLauncherStatus() const { return m_eGrenadeLauncherStatus; }
    ALife::EWeaponAddonStatus get_ScopeStatus() const { return m_eScopeStatus; }
    ALife::EWeaponAddonStatus get_SilencerStatus() const { return m_eSilencerStatus; }
    virtual bool UseScopeTexture() { return bScopeIsHasTexture; };
    //обновление видимости для косточек аддонов
    void SpawnDefaultAddons();
    void UpdateAddonsVisibility();
    void UpdateHUDAddonsVisibility();
    //инициализация свойств присоединенных аддонов
    virtual void InitAddons();
    void LoadAltHudAim();
    void UpdateAddonsOffset();
    void UpdateAvailableSecondZoom();
    void LoadAddonSlosts(LPCSTR section);
    shared_str GetInstalledMagType();
    shared_str GetInstalledTacGripType();

    void SetScopeOffset(Ivector2 pos) { m_iScopeX = pos.x; m_iScopeY = pos.y; }
    void SetSilencerOffset(Ivector2 pos) { m_iSilencerX = pos.x; m_iSilencerY = pos.y; }
    void SetGLOffset(Ivector2 pos) { m_iGrenadeLauncherX = pos.x; m_iGrenadeLauncherY = pos.y; }

    //для отоброажения иконок апгрейдов в интерфейсе
    int GetScopeX() { return m_iScopeX; }
    int GetScopeY() { return m_iScopeY; }
    int GetSilencerX() { return m_iSilencerX; }
    int GetSilencerY() { return m_iSilencerY; }
    int GetGrenadeLauncherX() { return m_iGrenadeLauncherX; }
    int GetGrenadeLauncherY() { return m_iGrenadeLauncherY; }

    const shared_str& GetGrenadeLauncherName() const { return m_sGrenadeLauncherName; }
    const shared_str GetScopeName() const;
    const shared_str& GetSilencerName() const { return m_sSilencerName; }
    IC void ForceUpdateAmmo() { m_BriefInfo_CalcFrame = 0; }
    u8 GetAddonsState() const { return m_flagsAddOnState; };
    void SetAddonsState(u8 st) { m_flagsAddOnState = st; } // dont use!!! for buy menu only!!!
protected:
    //состояние подключенных аддонов
    u8 m_flagsAddOnState;

    //возможность подключения различных аддонов
    ALife::EWeaponAddonStatus m_eScopeStatus;
    ALife::EWeaponAddonStatus m_eSilencerStatus;
    ALife::EWeaponAddonStatus m_eGrenadeLauncherStatus;

    //названия секций подключаемых аддонов
    shared_str m_sScopeName;
    shared_str m_sSilencerName;
    shared_str m_sGrenadeLauncherName;

    //смещение иконов апгрейдов в инвентаре
    int m_iScopeX, m_iScopeY;
    int m_iSilencerX, m_iSilencerY;
    int m_iGrenadeLauncherX, m_iGrenadeLauncherY;

    struct current_addon_t
    {
        union
        {
            u16 data;
            struct
            {
                u16 scope : 6; // 2^6 possible scope sections
                u16 silencer : 5; // 2^5 possible silencer/launcher sections
                u16 launcher : 5;
            };
        };
    };

protected:
    struct SZoomParams
    {
        EWeaponLatestZoom m_iLatestZoomType = EWeaponLatestZoom::noZoom; // какой был последний режим прицеливания

        bool m_bZoomEnabled; //разрешение режима приближения
        bool m_bZoomSecondEnabled; //разрешение режима приближения на коллиматор
        bool m_bHideCrosshairInZoom;
        bool m_bZoomDofEnabled;

        bool m_bIsZoomModeNow; //когда режим приближения включен
        bool m_bIsZoomSecondModeNow; //когда режим приближения включен на коллиматор
        float m_fCurrentZoomFactor; //текущий фактор приближения
        float m_fZoomRotateTime; //время приближения

        float m_fIronSightZoomFactor; //коэффициент увеличения прицеливания
        float m_fSecondScopeZoomFactor; //коэффициент увеличения второго прицела (коллиматора)
        float m_fScopeZoomFactor; //коэффициент увеличения прицела

        float m_f3dZoomFactor; //коэффициент мирового зума при использовании второго вьюпорта

        float m_fZoomRotationFactor;
        float m_fSecondZoomRotationFactor;
        float m_fSecondVPFovFactor;

        Fvector m_ZoomDof;
        Fvector4 m_ReloadDof;
        Fvector4 m_ReloadEmptyDof;

        bool m_bUseDynamicZoom;
        bool m_bSwitchBetweenSecondsZooms{false};
        shared_str m_sUseZoomPostprocess;
        shared_str m_sUseBinocularVision;
        CBinocularsVision* m_pVision;
        CNightVisionEffector* m_pNight_vision;

    } m_zoom_params;

    float m_fRTZoomFactor; // run-time zoom factor
    float m_fSecondRTZoomFactor; //текущий зум для 3д прицела
    float m_fScopeLenseZoomSmoothed; // сглаженное значение зума линзы для шейдера [0.1, 1.0], плавный переход при выходе из прицела
    CUIWindow* m_UIScope;

    xr_vector<shared_str> bullets_bones;
    int bullet_cnt;
    int last_hide_bullet;
    bool bHasBulletsToHide;
    u16 m_bullet_show_frame;

    virtual void HUD_VisualBulletUpdate(bool force = false, int force_idx = -1);

public:
    IC bool IsZoomEnabled() const { return m_zoom_params.m_bZoomEnabled; }
    IC bool IsZoomSecondEnabled() const { return m_zoom_params.m_bZoomSecondEnabled; }
    virtual void ZoomInc();
    virtual void ZoomDec();
    virtual void OnZoomIn();
    virtual void OnZoomOut();
    virtual void OnZoomFirstOut();
    virtual void OnZoomSecondOut();
    virtual void OnZoomSecondIn();
    // Ручная подстройка базового HUD FOV в прицеливании (используется при зажатом LCTRL и прокрутке колеса)
    void AdjustScopeHudFov(float wheel_delta);
    // Per-weapon HUD FOV (от бедра): консоль weapon_hud / секция weapon_hud
    void AdjustWeaponHudFov(float wheel_delta);
    IC bool IsZoomed() const { return m_zoom_params.m_bIsZoomModeNow; };
    IC bool IsSecondZoomed() const { return m_zoom_params.m_bIsZoomSecondModeNow; };
    CUIWindow* ZoomTexture();

    bool ZoomHideCrosshair();
    IC float GetZoomFactor() const { return m_zoom_params.m_fCurrentZoomFactor; }
    IC void SetZoomFactor(float f) { m_zoom_params.m_fCurrentZoomFactor = f; }
    virtual float CurrentZoomFactor();
    //показывает, что оружие находится в соостоянии поворота для приближенного прицеливания
    bool IsRotatingToZoom() const { return (m_zoom_params.m_fZoomRotationFactor < 1.f); }
    virtual u8 GetCurrentHudOffsetIdx();

    virtual float Weight() const;
    virtual u32 Cost() const;

    Fvector m_hands_offset[2][3]; // pos,rot/ normal,aim,GL
    Fvector m_tmp_offs;
    Fvector m_tmp_rot;

public:
    virtual EHandDependence HandDependence() const { return eHandDependence; }
    bool IsSingleHanded() const { return m_bIsSingleHanded; }
public:
    IC LPCSTR strap_bone0() const { return m_strap_bone0; }
    IC LPCSTR strap_bone1() const { return m_strap_bone1; }
    IC void strapped_mode(bool value) { m_strapped_mode = value; }
    IC bool strapped_mode() const { return m_strapped_mode; }
protected:
    LPCSTR m_strap_bone0;
    LPCSTR m_strap_bone1;
    Fmatrix m_StrapOffset;
    bool m_strapped_mode;
    bool m_can_be_strapped;

    Fmatrix m_Offset;
    // 0-используется без участия рук, 1-одна рука, 2-две руки
    EHandDependence eHandDependence;
    bool m_bIsSingleHanded;

public:
    current_addon_t m_cur_addon;

    //загружаемые параметры
    Fvector vLoadedFirePoint;
    Fvector vLoadedFirePoint2;

private:
    // Ключ и операции для пресетов HUD FOV в прицеливании (per-weapon + per-scope)
    shared_str GetScopeHudFovKey() const;
    bool GetScopeHudFovPreset(float& outValue) const;
    void SetScopeHudFovPreset(float value);

    bool GetWeaponHudFovPreset(float& outValue) const;
    void SetWeaponHudFovPreset(float value);
    float GetWeaponHudBase() const;

    firedeps m_current_firedeps;

protected:
    virtual void UpdateFireDependencies_internal();
    virtual void UpdatePosition(const Fmatrix& transform); //.
    virtual void UpdateXForm();
    virtual void UpdateHudAdditional(Fmatrix&);
    IC void UpdateFireDependencies()
    {
        if (dwFP_Frame == Device.dwFrame)
            return;
        UpdateFireDependencies_internal();
    };

    virtual void LoadFireParams(LPCSTR section);

public:
    IC const Fvector& get_LastFP()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastFP;
    }
    IC const Fvector& get_LastFP2()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastFP2;
    }
    IC const Fvector& get_LastFD()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastFD;
    }
    IC const Fvector& get_LastSP()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastSP;
    }

    virtual const Fvector& get_CurrentFirePoint() { return get_LastFP(); }
    virtual const Fvector& get_CurrentFirePoint2() { return get_LastFP2(); }
    virtual const Fmatrix& get_ParticlesXFORM()
    {
        UpdateFireDependencies();
        return m_current_firedeps.m_FireParticlesXForm;
    }
    virtual void ForceUpdateFireParticles();

protected:
    virtual void SetDefaults();

    virtual bool MovingAnimAllowedNow();
    virtual void OnStateSwitch(u32 S, u32 oldState);
    virtual void OnAnimationEnd(u32 state);

    //трассирование полета пули
    virtual void FireTrace(const Fvector& P, const Fvector& D);
    virtual float GetWeaponDeterioration();

    virtual void FireStart() { CShootingObject::FireStart(); }
    virtual void FireEnd();

    virtual void Reload();
    void StopShooting();

    // обработка визуализации выстрела
    virtual void OnShot(){};
    virtual void AddShotEffector();
    virtual void RemoveShotEffector();
    virtual void ClearShotEffector();
    virtual void StopShotEffector();

public:
    float GetBaseDispersion(float cartridge_k);
    float GetFireDispersion(bool with_cartridge, bool for_crosshair = false);
    virtual float GetFireDispersion(float cartridge_k, bool for_crosshair = false);
    virtual int ShotsFired() { return 0; }
    virtual int GetCurrentFireMode() { return 1; }
    //параметы оружия в зависимоти от его состояния исправности
    float GetConditionDispersionFactor() const;
    float GetConditionMisfireProbability() const;
    virtual float GetConditionToShow() const;

public:
    CameraRecoil cam_recoil; // simple mode (walk, run)
    CameraRecoil zoom_cam_recoil; // using zoom =(ironsight or scope)

protected:
    //фактор увеличения дисперсии при максимальной изношености
    //(на сколько процентов увеличится дисперсия)
    float fireDispersionConditionFactor;

    //вероятность осечки при максимальной изношености
    float misfireProbability;
    float misfireConditionK;
    // modified by Peacemaker [17.10.08]
    bool  misfireUseOldFormula{};
    float misfireStartCondition; //изношенность, при которой появляется шанс осечки
    float misfireEndCondition; //изношеность при которой шанс осечки становится константным
    float misfireStartProbability; //шанс осечки при изношености больше чем misfireStartCondition
    float misfireEndProbability; //шанс осечки при изношености больше чем misfireEndCondition
    float conditionDecreasePerQueueShot; //увеличение изношености при выстреле очередью
    float conditionDecreasePerShot; //увеличение изношености при одиночном выстреле

public:
    float GetMisfireStartCondition() const { return misfireStartCondition; }
    float GetMisfireEndCondition() const { return misfireEndCondition; }

protected:
    struct SPDM
    {
        float m_fPDM_disp_base;
        float m_fPDM_disp_vel_factor;
        float m_fPDM_disp_accel_factor;
        float m_fPDM_disp_crouch;
        float m_fPDM_disp_crouch_no_acc;
    };
    SPDM m_pdm;

    float m_crosshair_inertion;
    first_bullet_controller m_first_bullet_controller;

protected:
    //для отдачи оружия
    Fvector m_vRecoilDeltaAngle;

protected:
    //для второго ствола
    void StartFlameParticles2();
    void StopFlameParticles2();
    void UpdateFlameParticles2();

protected:
    shared_str m_sFlameParticles2;
    //объект партиклов для стрельбы из 2-го ствола
    CParticlesObject* m_pFlameParticles2;

protected:
    int GetAmmoCount(u8 ammo_type) const;

public:
    IC int GetAmmoElapsed() const { return /*int(m_magazine.size())*/ iAmmoElapsed; }
    IC int GetAmmoMagSize() const { return iMagazineSize; }
    int GetSuitableAmmoTotal(bool use_item_to_spawn = false) const;

    void SetAmmoElapsed(int ammo_count);

    virtual void OnMagazineEmpty();
    void SpawnAmmo(u32 boxCurr = 0xffffffff, LPCSTR ammoSect = NULL, u32 ParentID = 0xffffffff);
    bool SwitchAmmoType(u32 flags);

    virtual float Get_PDM_Base() const { return m_pdm.m_fPDM_disp_base; };
    virtual float Get_PDM_Vel_F() const { return m_pdm.m_fPDM_disp_vel_factor; };
    virtual float Get_PDM_Accel_F() const { return m_pdm.m_fPDM_disp_accel_factor; };
    virtual float Get_PDM_Crouch() const { return m_pdm.m_fPDM_disp_crouch; };
    virtual float Get_PDM_Crouch_NA() const { return m_pdm.m_fPDM_disp_crouch_no_acc; };
    virtual float GetCrosshairInertion() const { return m_crosshair_inertion; };
    float GetFirstBulletDisp() const { return m_first_bullet_controller.get_fire_dispertion(); };
protected:
    int iAmmoElapsed; // ammo in magazine, currently
    int iMagazineSize; // size (in bullets) of magazine

    //для подсчета в GetSuitableAmmoTotal
    mutable int m_iAmmoCurrentTotal;
    mutable u32 m_BriefInfo_CalcFrame; //кадр на котором просчитали кол-во патронов
    bool m_bAmmoWasSpawned;

    virtual bool IsNecessaryItem(const shared_str& item_sect);

public:
    xr_vector<shared_str> m_ammoTypes;

    using SCOPES_VECTOR = xr_vector<shared_str>;
    SCOPES_VECTOR m_scopes;
    SCOPES_VECTOR m_addons;
    EWeaponAddonSlotType m_addon_slot_type;
    u8 m_cur_scope;

    CWeaponAmmo* m_pCurrentAmmo;
    u8 m_ammoType;
    //-	shared_str				m_ammoName; <== deleted
    bool m_bHasTracers;
    u8 m_u8TracerColorID;
    u8 m_set_next_ammoType_on_reload;
    // Multitype ammo support
    xr_vector<CCartridge> m_magazine;
    CCartridge m_DefaultCartridge;
    float m_fCurrentCartirdgeDisp;

    bool unlimited_ammo();
    IC bool can_be_strapped() const { return m_can_be_strapped; };

    float GetMagazineWeight(const decltype(m_magazine)& mag) const;

protected:
    u32 m_ef_main_weapon_type;
    u32 m_ef_weapon_type;

public:
    virtual u32 ef_main_weapon_type() const;
    virtual u32 ef_weapon_type() const;

    //Alundaio
    int GetAmmoCount_forType(shared_str const& ammo_type) const;
    virtual void set_ef_main_weapon_type(u32 type) { m_ef_main_weapon_type = type; };
    virtual void set_ef_weapon_type(u32 type) { m_ef_weapon_type = type; };
    virtual void SetAmmoType(u8 type) { m_ammoType = type; };
    u8 GetAmmoType() { return m_ammoType; }
    //-Alundaio

protected:
    // This is because when scope is attached we can't ask scope for these params
    // therefore we should hold them by ourself :-((
    float m_addon_holder_range_modifier;
    float m_addon_holder_fov_modifier;

public:
    virtual void modify_holder_params(float& range, float& fov) const;
    virtual bool use_crosshair() const { return true; }
    bool show_crosshair();
    bool show_indicators();
    virtual BOOL ParentMayHaveAimBullet();
    virtual BOOL ParentIsActor();

private:
    virtual bool install_upgrade_ammo_class(LPCSTR section, bool test);
    bool install_upgrade_disp(LPCSTR section, bool test);
    bool install_upgrade_hit(LPCSTR section, bool test);
    bool install_upgrade_addon(LPCSTR section, bool test);

protected:
    virtual bool install_upgrade_impl(LPCSTR section, bool test);

private:
    float m_hit_probability[egdCount];

public:
    const float& hit_probability() const;

private:
    Fvector m_overriden_activation_speed;
    bool m_activation_speed_is_overriden;
    virtual bool ActivationSpeedOverriden(Fvector& dest, bool clear_override);

    bool m_bRememberActorNVisnStatus;

    Lock render_lock{};

public:
    virtual void SetActivationSpeedOverride(Fvector const& speed);
    bool GetRememberActorNVisnStatus() { return m_bRememberActorNVisnStatus; };
    virtual void EnableActorNVisnAfterZoom();

    virtual void DumpActiveParams(shared_str const& section_name, CInifile& dst_ini) const;
    virtual shared_str const GetAnticheatSectionName() const { return cNameSect(); };

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};
