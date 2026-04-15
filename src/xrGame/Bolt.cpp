#include "StdAfx.h"
#include "Bolt.h"
#include "ParticlesObject.h"
#include "xrPhysics/PhysicsShell.h"
#include "xrEngine/xr_level_controller.h"
#include "player_hud.h"
#include "HUDManager.h"

float _lerp(const float& _val_a, const float& _val_b, const float& _factor);

CBolt::CBolt(void) {
    m_count = 0;
    m_thrower_id = u16(-1);
    m_fLR_MovingFactor = 0.f;
    m_fLR_CameraFactor = 0.f;
    m_fLR_InertiaFactor = 0.f;
    m_fUD_InertiaFactor = 0.f;
}
CBolt::~CBolt(void) {}
void CBolt::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
    IGameObject* o = H_Parent()->H_Parent();
    if (o)
        SetInitiator(o->ID());
}

void CBolt::Load(LPCSTR section)
{
    m_count = pSettings->r_u16(section, "count");
    inherited::Load(section);

    if (pSettings->line_exist(section, "tip_text"))
        set_tip_text(pSettings->r_string(section, "tip_text"));

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

void CBolt::RebuildDescription()
{
    const auto section = CInventoryItem::object().cNameSect();
    if (!pSettings->line_exist(section, "description"))
    {
        m_Description = "";
        return;
    }
    string16 count;
    xr_sprintf(count, "%d", GetCount());
    string2048 tmp_descr;
    pcstr descr{StringTable().translate(pSettings->r_string(section, "description")).c_str()};
    xr_sprintf(tmp_descr, descr, count);
    m_Description = tmp_descr;
}

void CBolt::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    CInventoryItemObject::save(output_packet);

    output_packet.w_u16(m_count);
}
void CBolt::load(IReader& input_packet)
{
    inherited::load(input_packet);
    CInventoryItemObject::load(input_packet);

    m_count = input_packet.r_u16();
    RefreshInventoryDescription();
}

bool CBolt::GetBriefInfo(II_BriefInfo& info)
{
    VERIFY(m_pInventory);
    info.clear();

    info.name._set(m_nameShort);
    info.icon._set(m_ammo_icon);

    string16 stmp;
    xr_sprintf(stmp, "%d", m_count);
    info.cur_ammo._set(stmp);
    return true;
}

void CBolt::Throw()
{
    CMissile* l_pBolt = smart_cast<CMissile*>(m_fake_missile);
    if (!l_pBolt)
        return;
    l_pBolt->set_destroy_time(u32(m_dwDestroyTimeMax / phTimefactor));
    inherited::Throw();
    spawn_fake_missile();
    AddCount(-1);
    RefreshInventoryDescription();

    NET_Packet P;
    P.w_begin(M_EVENT);
    P.w_u32(Device.dwTimeGlobal);
    P.w_u16(GE_WPN_AMMO_ADD);
    P.w_u16(ID());
    P.w_u16(m_count);
    Level().Send(P, net_flags(TRUE, TRUE));

    if (m_count == 0)
        DestroyObject();
}

void CBolt::spawn_fake_missile()
{
    if (OnClient())
        return;

    if (!getDestroy())
    {
        CSE_Abstract* object = Level().spawn_item(
            "bolt", Position(), (GEnv.isDedicatedServer) ? u32(-1) : ai_location().level_vertex_id(), ID(), true);

        CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
        VERIFY(alife_object);
        alife_object->m_flags.set(CSE_ALifeObject::flCanSave, FALSE);

        NET_Packet P;
        object->Spawn_Write(P, TRUE);
        Level().Send(P, net_flags(TRUE));
        F_entity_Destroy(object);
    }
}

void CBolt::OnAnimationEnd(u32 state)
{
    switch (state)
    {
    case eHiding:
    {
        setVisible(FALSE);
        SwitchState(eHidden);
    }
    break;
    case eShowing:
    {
        setVisible(TRUE);
        SwitchState(eIdle);
    }
    break;
    case eThrowStart:
    {
        if (!m_fake_missile && !smart_cast<CMissile*>(H_Parent()))
            spawn_fake_missile();

        if (m_throw)
            SwitchState(eThrow);
        else
            SwitchState(eReady);
    }
    break;
    case eThrow:
    {
        SwitchState(eThrowEnd);
    }
    break;
    case eThrowEnd:
    {
        SwitchState(eShowing);
    }
    break;
    default: inherited::OnAnimationEnd(state);
    }
}

bool CBolt::Useful() const { return true; }
bool CBolt::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    return false;
}

void CBolt::activate_physic_shell()
{
    inherited::activate_physic_shell();
    m_pPhysicsShell->SetAirResistance(.0001f);
}

void CBolt::UpdateHudAdditional(Fmatrix& trans)
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

void CBolt::SetInitiator(u16 id) { m_thrower_id = id; }
u16 CBolt::Initiator() { return m_thrower_id; }

