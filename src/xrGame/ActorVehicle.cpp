#include "StdAfx.h"
#pragma hdrstop

#include "Actor.h"
#include "xrEngine/CameraBase.h"

#include "ActorEffector.h"
#include "holder_custom.h"
#ifdef DEBUG
#include "PHDebug.h"
#endif
#include "alife_space.h"
#include "Hit.h"
#include "PHDestroyable.h"
#include "Car.h"
#include "Include/xrRender/Kinematics.h"

#include "actor_anim_defs.h"
#include "game_object_space.h"
#include "CharacterPhysicsSupport.h"
#include "Inventory.h"

#include "game_object_space.h"
#include "xrScriptEngine/script_callback_ex.h"
#include "script_game_object.h"
#include "xrPhysics/IPHWorld.h"

void CActor::attach_Vehicle(CHolderCustom* vehicle)
{
    if(!vehicle || m_holder)
        return;

    CCar* car = smart_cast<CCar*>(vehicle);
    if (!car)
        return;

    //PickupModeOff();
    m_holder=vehicle;

    IRenderVisual* pVis = Visual();
    IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(pVis);
    R_ASSERT(V);
    IKinematics* pK = smart_cast<IKinematics*>(pVis);

    if (!m_holder->attach_Actor(this))
    {
        m_holder = nullptr;
        return;
    }

    // temp play animation
    u16 anim_type = car->DriverAnimationType();
    SVehicleAnimCollection& anims = m_vehicle_anims->m_vehicles_type_collections[anim_type];
    V->PlayCycle(anims.idles[0], false);

    ResetCallbacks();
    u16 head_bone = pK->LL_BoneID("bip01_head");
    pK->LL_GetBoneInstance(u16(head_bone)).set_callback(bctPhysics, VehicleHeadCallback, this);

    // B-1: actor vehicle attach/detach runs on main thread (input/UpdateCL); Request* is a no-op outside overlap.
    character_physics_support()->RequestDestroyCharacter();
    mstate_wishful = 0;
    m_holderID = car->ID();

    SetWeaponHideState(INV_STATE_CAR, true);

    CStepManager::on_animation_start(MotionID(), nullptr);

    // Real Wolf: Колбек на посадку в машину. 01.08.2014.
    this->callback(GameObject::eAttachVehicle)(car->lua_game_object());
}

void CActor::detach_Vehicle()
{
    IPHWorld* ph = physics_world();
    if (ph && ph->ShouldDeferSchedulerPhysicsMutation())
    {
        // B-1: vehicle detach mutates splitter/character state; defer the whole owner-level tail.
        QueueDeferredDetachVehicle();
        return;
    }

    if (!m_holder)
        return;
    CCar* car = smart_cast<CCar*>(m_holder);
    if (!car)
        return;

    // CPHShellSplitterHolder*sh= car->PPhysicsShell()->SplitterHolder();
    // if(sh)
    //	sh->Deactivate();
    car->PPhysicsShell()->SplitterHolderDeactivate();

    if (!character_physics_support()->movement()->ActivateBoxDynamic(0))
    {
        // if(sh)sh->Activate();
        car->PPhysicsShell()->SplitterHolderActivate();
        return;
    }
    // if(sh)
    //	sh->Activate();
    car->PPhysicsShell()->SplitterHolderActivate();
    m_holder->detach_Actor(); //

    // Real Wolf: колбек на высадку из машины. 01.08.2014.
    this->callback(GameObject::eDetachVehicle)(car->lua_game_object());

    character_physics_support()->movement()->SetPosition(m_holder->ExitPosition());
    character_physics_support()->movement()->SetVelocity(m_holder->ExitVelocity());

    r_model_yaw = -m_holder->Camera()->yaw;
    r_torso.yaw = r_model_yaw;
    r_model_yaw_dest = r_model_yaw;
    m_holder = NULL;
    SetCallbacks();
    IKinematicsAnimated* V = smart_cast<IKinematicsAnimated*>(Visual());
    R_ASSERT(V);
    V->PlayCycle(m_anims->m_normal.legs_idle);
    V->PlayCycle(m_anims->m_normal.m_torso_idle);
    m_holderID = u16(-1);

    //.	SetWeaponHideState(whs_CAR, FALSE);
    SetWeaponHideState(INV_STATE_CAR, false);
}

void CActor::QueueDeferredDetachVehicle()
{
    IPHWorld* ph = physics_world();
    if (!ph)
    {
        detach_Vehicle();
        return;
    }

    if (m_deferred_detach_vehicle_registered)
        return;

    m_deferred_detach_vehicle_registered = true;
    if (ph->defer_scheduler_mutation([this] { ApplyDeferredDetachVehicle(); }))
        return;

    m_deferred_detach_vehicle_registered = false;
    detach_Vehicle();
}

void CActor::ApplyDeferredDetachVehicle()
{
    m_deferred_detach_vehicle_registered = false;
    if (getDestroy() || object_removed())
        return;
    detach_Vehicle();
}

bool CActor::use_Vehicle(CHolderCustom* object)
{
    //	CHolderCustom* vehicle=smart_cast<CHolderCustom*>(object);
    CHolderCustom* vehicle = object;
    Fvector center;
    Center(center);
    if (m_holder)
    {
        if (!vehicle && m_holder->Use(Device.vCameraPosition, Device.vCameraDirection, center))
            detach_Vehicle();
        else
        {
            if (m_holder == vehicle)
                if (m_holder->Use(Device.vCameraPosition, Device.vCameraDirection, center))
                    detach_Vehicle();
        }
        return true;
    }
    else
    {
        if (vehicle)
        {
            if (vehicle->Use(Device.vCameraPosition, Device.vCameraDirection, center))
            {
                if (pCamBobbing)
                {
                    Cameras().RemoveCamEffector(eCEBobbing);
                    pCamBobbing = NULL;
                }

                attach_Vehicle(vehicle);
            }
            // Real Wolf: колбек на использование машины (но не посадку) без учета расстояния. 01.08.2014.
            else if (auto car = smart_cast<CCar*>(vehicle))
                this->callback(GameObject::eUseVehicle)(car->lua_game_object());
            return true;
        }
        return false;
    }
}

void CActor::on_requested_spawn(IGameObject* object)
{
    CCar* car = smart_cast<CCar*>(object);
    attach_Vehicle(car);
}
