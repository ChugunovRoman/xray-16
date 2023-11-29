#pragma once

namespace GameObject
{
enum ECallbackType : u32
{
    eTradeStart = u32(0),
    eTradeStop,
    eTradeSellBuyItem,
    eTradePerformTradeOperation,

    eZoneEnter,
    eZoneExit,
    eExitLevelBorder,
    eEnterLevelBorder,
    eDeath,

    ePatrolPathInPoint,

    eInventoryPda,
    eInventoryInfo,
    eArticleInfo,
    eTaskStateChange,
    eMapLocationAdded,

    eUseObject,

    eHit,

    eSound,

    eActionTypeMovement,
    eActionTypeWatch,
    eActionTypeRemoved,
    eActionTypeAnimation,
    eActionTypeSound,
    eActionTypeParticle,
    eActionTypeObject,

    eActorSleep,

    eHelicopterOnPoint,
    eHelicopterOnHit,

    eOnItemTake,
    eOnItemDrop,

    eScriptAnimation,

    eTraderGlobalAnimationRequest,
    eTraderHeadAnimationRequest,
    eTraderSoundEnd,

    eInvBoxItemTake,
    eWeaponNoAmmoAvailable,

    //AVO: custom callbacks
    // Input
    eActorHudAnimationEnd,
    eKeyPress,
    eKeyRelease,
    eKeyHold,
    eMouseMove,
    eMouseWheel,
    eControllerPress,
    eControllerRelease,
    eControllerHold,
    eControllerAttitudeChange,
    // Inventory
    eItemToBelt,
    eItemToSlot,
    eItemToRuck,
    // Actor
    eActorBeforeDeath,
    //-AVO

    // vehicle
    eAttachVehicle,
    eDetachVehicle,
    eUseVehicle,

    // weapon
    eOnWeaponZoomIn,
    eOnWeaponZoomOut,
    eOnWeaponJammed,
    eOnWeaponFired,
    eOnWeaponMagazineEmpty,

    eDummy = u32(-1),
};
};
