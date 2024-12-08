#include "StdAfx.h"
#include "Grenade.h"
#include "xrPhysics/PhysicsShell.h"
#include "Entity.h"
#include "ParticlesObject.h"
#include "Actor.h"
#include "Inventory.h"
#include "Level.h"
#include "xrMessages.h"
#include "xrEngine/xr_level_controller.h"
#include "game_cl_base.h"
#include "xrServer_Objects_ALife.h"
#include "player_hud.h"
#include "HUDManager.h"

#define GRENADE_REMOVE_TIME 30000

const float default_grenade_detonation_threshold_hit = 100;
float _lerp(const float& _val_a, const float& _val_b, const float& _factor);

CGrenade::CGrenade(void)
{
    m_destroy_callback.clear();
    m_eSoundCheckout = ESoundTypes(SOUND_TYPE_WEAPON_RECHARGING);
    m_fLR_MovingFactor = 0.f;
    m_fLR_CameraFactor = 0.f;
    m_fLR_InertiaFactor = 0.f;
    m_fUD_InertiaFactor = 0.f;
}

CGrenade::~CGrenade(void) {}
void CGrenade::Load(LPCSTR section)
{
    inherited::Load(section);
    CExplosive::Load(section);

    m_sounds.LoadSound(section, "snd_checkout", "sndCheckout", false, m_eSoundCheckout);

    //////////////////////////////////////
    //время убирания оружия с уровня
    if (pSettings->line_exist(section, "grenade_remove_time"))
        m_dwGrenadeRemoveTime = pSettings->r_u32(section, "grenade_remove_time");
    else
        m_dwGrenadeRemoveTime = GRENADE_REMOVE_TIME;
    m_grenade_detonation_threshold_hit = READ_IF_EXISTS(
        pSettings, r_float, section, "detonation_threshold_hit", default_grenade_detonation_threshold_hit);

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
}

void CGrenade::Hit(SHit* pHDS)
{
    if (ALife::eHitTypeExplosion == pHDS->hit_type && m_grenade_detonation_threshold_hit < pHDS->damage() &&
        CExplosive::Initiator() == u16(-1))
    {
        CExplosive::SetCurrentParentID(pHDS->who->ID());
        Destroy();
    }
    inherited::Hit(pHDS);
}

bool CGrenade::net_Spawn(CSE_Abstract* DC)
{
    m_dwGrenadeIndependencyTime = 0;
    BOOL ret = inherited::net_Spawn(DC);
    Fvector box;
    BoundingBox().getsize(box);
    float max_size = _max(_max(box.x, box.y), box.z);
    box.set(max_size, max_size, max_size);
    box.mul(3.f);
    CExplosive::SetExplosionSize(box);
    m_thrown = false;
    return ret;
}

void CGrenade::net_Destroy()
{
    if (m_destroy_callback)
    {
        m_destroy_callback(this);
        m_destroy_callback = destroy_callback(NULL);
    }

    inherited::net_Destroy();
    CExplosive::net_Destroy();
}

void CGrenade::OnH_B_Independent(bool just_before_destroy) { inherited::OnH_B_Independent(just_before_destroy); }
void CGrenade::OnH_A_Independent()
{
    m_dwGrenadeIndependencyTime = Level().timeServer();
    inherited::OnH_A_Independent();
}

void CGrenade::OnH_A_Chield()
{
    m_dwGrenadeIndependencyTime = 0;
    m_dwDestroyTime = 0xffffffff;
    inherited::OnH_A_Chield();
}

void CGrenade::State(u32 state, u32 old_state)
{
    switch (state)
    {
    case eThrowStart:
    {
        Fvector C;
        Center(C);
        PlaySound("sndCheckout", C);
    }
    break;
    case eThrowEnd:
    {
        if (m_thrown)
        {
            if (m_pPhysicsShell)
                m_pPhysicsShell->Deactivate();
            xr_delete(m_pPhysicsShell);
            m_dwDestroyTime = 0xffffffff;
            PutNextToSlot();
            if (Local())
            {
#ifndef MASTER_GOLD
                Msg("Destroying local grenade[%d][%d]", ID(), Device.dwFrame);
#endif // #ifndef MASTER_GOLD
                DestroyObject();
            }
        };
    }
    break;
    };
    inherited::State(state, old_state);
}

bool CGrenade::DropGrenade()
{
    EMissileStates grenade_state = static_cast<EMissileStates>(GetState());
    if (((grenade_state == eThrowStart) || (grenade_state == eReady) || (grenade_state == eThrow)) && (!m_thrown))
    {
        Throw();
        return true;
    }
    return false;
}

void CGrenade::DiscardState()
{
    if (IsGameTypeSingle())
    {
        u32 state = GetState();
        if (state == eReady || state == eThrow)
            OnStateSwitch(eIdle, state);
    }
}

void CGrenade::SendHiddenItem()
{
    if (GetState() == eThrow)
    {
        //		Msg("MotionMarks !!![%d][%d]", ID(), Device.dwFrame);
        Throw();
    }
    CActor* pActor = smart_cast<CActor*>(m_pInventory->GetOwner());
    if (pActor && (GetState() == eReady || GetState() == eThrow))
    {
        return;
    }

    inherited::SendHiddenItem();
}

void CGrenade::Throw()
{
    if (m_thrown)
        return;

    if (!m_fake_missile)
        return;

    CGrenade* pGrenade = smart_cast<CGrenade*>(m_fake_missile);
    VERIFY(pGrenade);

    if (pGrenade)
    {
        pGrenade->set_destroy_time(m_dwDestroyTimeMax);
        //установить ID того кто кинул гранату
        pGrenade->SetInitiator(H_Parent()->ID());
    }
    inherited::Throw();
    m_fake_missile->processing_activate(); //@sliph
    m_thrown = true;
}

void CGrenade::Destroy()
{
    // Generate Expode event
    Fvector normal;

    if (m_destroy_callback)
    {
        m_destroy_callback(this);
        m_destroy_callback = destroy_callback(NULL);
    }

    FindNormal(normal);
    CExplosive::GenExplodeEvent(Position(), normal);
}

bool CGrenade::Useful() const
{
    bool res = (/* !m_throw && */ m_dwDestroyTime == 0xffffffff && CExplosive::Useful() &&
        TestServerFlag(CSE_ALifeObject::flCanSave));

    return res;
}

void CGrenade::OnEvent(NET_Packet& P, u16 type)
{
    inherited::OnEvent(P, type);
    CExplosive::OnEvent(P, type);
}

void CGrenade::PutNextToSlot()
{
    if (OnClient())
        return;

    VERIFY(!getDestroy());
    //выкинуть гранату из инвентаря
    NET_Packet P;
    if (m_pInventory)
    {
        m_pInventory->Ruck(this);

        this->u_EventGen(P, GEG_PLAYER_ITEM2RUCK, this->H_Parent()->ID());
        P.w_u16(this->ID());
        this->u_EventSend(P);
    }
    else
        Msg("! PutNextToSlot : m_pInventory = NULL [%d][%d]", ID(), Device.dwFrame);

    if (smart_cast<CInventoryOwner*>(H_Parent()) && m_pInventory)
    {
        CGrenade* pNext = smart_cast<CGrenade*>(m_pInventory->Same(this, true));
        if (!pNext)
            pNext = smart_cast<CGrenade*>(m_pInventory->SameSlot(GRENADE_SLOT, this, true));

        VERIFY(pNext != this);

        if (pNext && m_pInventory->Slot(pNext->BaseSlot(), pNext))
        {
            pNext->u_EventGen(P, GEG_PLAYER_ITEM2SLOT, pNext->H_Parent()->ID());
            P.w_u16(pNext->ID());
            P.w_u16(pNext->BaseSlot());
            pNext->u_EventSend(P);
            m_pInventory->SetActiveSlot(pNext->BaseSlot());
        }
        else
        {
            CActor* pActor = smart_cast<CActor*>(m_pInventory->GetOwner());

            if (pActor)
                pActor->OnPrevWeaponSlot();
        }

        m_thrown = false;
    }
}

void CGrenade::OnAnimationEnd(u32 state)
{
    switch (state)
    {
    case eThrowEnd: SwitchState(eHidden); break;
    default: inherited::OnAnimationEnd(state);
    }
}

void CGrenade::UpdateCL()
{
    inherited::UpdateCL();
    CExplosive::UpdateCL();

    if (!IsGameTypeSingle())
        make_Interpolation();
}

bool CGrenade::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    switch (cmd)
    {
    //переключение типа гранаты
    case kWPN_NEXT:
    {
        if (flags & CMD_START)
        {
            if (m_pInventory)
                m_pInventory->ActivateNextGrenadeDeffered();
        }
        return true;
    };
    }
    return false;
}

bool CGrenade::NeedToDestroyObject() const
{
    if (IsGameTypeSingle())
        return false;
    if (Remote())
        return false;
    if (TimePassedAfterIndependant() > m_dwGrenadeRemoveTime)
        return true;

    return false;
}

ALife::_TIME_ID CGrenade::TimePassedAfterIndependant() const
{
    if (!H_Parent() && m_dwGrenadeIndependencyTime != 0)
        return Level().timeServer() - m_dwGrenadeIndependencyTime;
    else
        return 0;
}

bool CGrenade::UsedAI_Locations()
{
#pragma todo( \
    \
"Dima to Yura : It crashes, because on net_Spawn object doesn't use AI locations, but on net_Destroy it does use them")
    return inherited::UsedAI_Locations(); // m_dwDestroyTime == 0xffffffff;
}

void CGrenade::net_Relcase(IGameObject* O)
{
    CExplosive::net_Relcase(O);
    inherited::net_Relcase(O);
}

void CGrenade::DeactivateItem()
{
    // Drop grenade if primed
    StopCurrentAnimWithoutCallback();
    if (!GetTmpPreDestroy() && Local() && (GetState() == eThrowStart || GetState() == eReady || GetState() == eThrow))
    {
        if (m_fake_missile)
        {
            CGrenade* pGrenade = smart_cast<CGrenade*>(m_fake_missile);
            if (pGrenade)
            {
                if (m_pInventory->GetOwner())
                {
                    CActor* pActor = smart_cast<CActor*>(m_pInventory->GetOwner());
                    if (pActor)
                    {
                        if (!pActor->g_Alive())
                        {
                            m_constpower = false;
                            m_fThrowForce = 0;
                        }
                    }
                }
                Throw();
            };
        };
    };

    inherited::DeactivateItem();
}

bool CGrenade::GetBriefInfo(II_BriefInfo& info)
{
    VERIFY(m_pInventory);
    info.clear();

    info.name._set(m_nameShort);
    info.icon._set(cNameSect());

    u32 ThisGrenadeCount = m_pInventory->dwfGetSameItemCount(cNameSect().c_str(), true);

    string16 stmp;
    xr_sprintf(stmp, "%d", ThisGrenadeCount);
    info.cur_ammo._set(stmp);
    return true;
}


void CGrenade::UpdateHudAdditional(Fmatrix& trans)
{
    CActor* pActor = smart_cast<CActor*>(H_Parent());
    if (!pActor)
        return;

    attachable_hud_item* hi = HudItemData();
    if (!hi)
        return;

    //============= Подготавливаем общие переменные =============//

    u8 idx = GetCurrentHudOffsetIdx();
    clamp(idx, u8(0), u8(1));
    bool bForAim = (idx == 1);

    float fInertiaPower = GetInertionPowerFactor();

    float fYMag = pActor->fFPCamYawMagnitude;
    float fPMag = pActor->fFPCamPitchMagnitude;

    static float fAvgTimeDelta = Device.fTimeDelta;
    float friction_i = 1.f - 0.8f;
    fAvgTimeDelta = fAvgTimeDelta * 0.8f + Device.fTimeDelta * friction_i;

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
        0.2);

    // Высчитываем мксимальный наклон от поворота камеры
    float fCamLimitBlend = _lerp(
        m_strafe_offset[3][0].x,
        m_strafe_offset[3][1].x,
        0.2);

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
            curr_offs.mul(1.f - 0.2);
            curr_rot.mul(1.f - 0.2);
        }
        else
        { // Во время аима
            curr_offs.mul(0.2);
            curr_rot.mul(0.2);
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
    }

    //============= Инерция оружия =============//
   // Параметры инерции
    float fInertiaSpeedMod = _lerp(
        hi->m_measures.m_inertion_params.m_tendto_speed,
        hi->m_measures.m_inertion_params.m_tendto_speed_aim,
        0.2);

    float fInertiaReturnSpeedMod = _lerp(
        hi->m_measures.m_inertion_params.m_tendto_ret_speed,
        hi->m_measures.m_inertion_params.m_tendto_ret_speed_aim,
        0.2);

    float fInertiaMinAngle = _lerp(
        hi->m_measures.m_inertion_params.m_min_angle,
        hi->m_measures.m_inertion_params.m_min_angle_aim,
        0.2);

    Fvector4 vIOffsets; // x = L, y = R, z = U, w = D
    vIOffsets.x = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.x,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.x,
        0.2) * fInertiaPower;
    vIOffsets.y = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.y,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.y,
        0.2) * fInertiaPower;
    vIOffsets.z = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.z,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.z,
        0.2) * fInertiaPower;
    vIOffsets.w = _lerp(
        hi->m_measures.m_inertion_params.m_offset_LRUD.w,
        hi->m_measures.m_inertion_params.m_offset_LRUD_aim.w,
        0.2) * fInertiaPower;

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
}
