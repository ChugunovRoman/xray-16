#pragma once

#include "xrCore/clsid.h"

constexpr CLASS_ID CLSID_OBJECT_ACTOR               = xray::make_clsid("O_ACTOR ");
constexpr CLASS_ID CLSID_OBJECT_HLAMP               = xray::make_clsid("O_HLAMP ");
constexpr CLASS_ID CLSID_ENTITY                     = xray::make_clsid("ENTITY  ");
constexpr CLASS_ID CLSID_SPECTATOR                  = xray::make_clsid("SPECT   ");
constexpr CLASS_ID CLSID_OBJECT_PROJECTOR           = xray::make_clsid("O_SEARCH");

// all the monsters
constexpr CLASS_ID CLSID_LEVEL_POINT                = xray::make_clsid("LVLPOINT");
constexpr CLASS_ID CLSID_SCRIPT_OBJECT              = xray::make_clsid("SCRPTOBJ");
constexpr CLASS_ID CLSID_AI_GRAPH                   = xray::make_clsid("AI_GRAPH");
constexpr CLASS_ID CLSID_AI_CROW                    = xray::make_clsid("AI_CROW ");

constexpr CLASS_ID CLSID_AI_ZOMBIE                  = xray::make_clsid("AI_ZOM  ");
constexpr CLASS_ID CLSID_AI_POLTERGEIST             = xray::make_clsid("AI_POLTR");

constexpr CLASS_ID CLSID_AI_FLESH                   = xray::make_clsid("AI_FLESH");
constexpr CLASS_ID CLSID_AI_FLESH_GROUP             = xray::make_clsid("AI_FLE_G");

constexpr CLASS_ID CLSID_AI_PHANTOM                 = xray::make_clsid("AI_PHANT");
constexpr CLASS_ID CLSID_AI_SPONGER                 = xray::make_clsid("AI_SPONG");
constexpr CLASS_ID CLSID_AI_CONTROLLER              = xray::make_clsid("AI_CONTR");
constexpr CLASS_ID CLSID_AI_BLOODSUCKER             = xray::make_clsid("AI_BLOOD");
constexpr CLASS_ID CLSID_AI_STALKER                 = xray::make_clsid("AI_STL  ");
constexpr CLASS_ID CLSID_AI_BURER                   = xray::make_clsid("AI_BURER");
constexpr CLASS_ID CLSID_AI_GIANT                   = xray::make_clsid("AI_GIANT");
constexpr CLASS_ID CLSID_AI_CHIMERA                 = xray::make_clsid("AI_HIMER");
constexpr CLASS_ID CLSID_AI_FRACTURE                = xray::make_clsid("AI_FRACT");
constexpr CLASS_ID CLSID_AI_DOG_BLACK               = xray::make_clsid("AI_DOG_B");
constexpr CLASS_ID CLSID_AI_DOG_RED                 = xray::make_clsid("AI_DOG_R");
constexpr CLASS_ID CLSID_AI_DOG_PSY                 = xray::make_clsid("AI_DOG_P");
constexpr CLASS_ID CLSID_AI_DOG_PSY_PHANTOM         = xray::make_clsid("AI_DOG_F");
constexpr CLASS_ID CLSID_AI_TRADER                  = xray::make_clsid("AI_TRADE");
constexpr CLASS_ID CLSID_AI_BOAR                    = xray::make_clsid("AI_BOAR ");
constexpr CLASS_ID CLSID_AI_SNORK                   = xray::make_clsid("AI_SNORK");
constexpr CLASS_ID CLSID_AI_CAT                     = xray::make_clsid("AI_CAT  ");
constexpr CLASS_ID CLSID_AI_TUSHKANO                = xray::make_clsid("AI_TUSH ");
constexpr CLASS_ID CLSID_AI_RAT                     = xray::make_clsid("AI_RAT  ");

// vehicles
constexpr CLASS_ID CLSID_CAR                        = xray::make_clsid("C_NIVA  ");
constexpr CLASS_ID CLSID_VEHICLE_HELICOPTER         = xray::make_clsid("C_HLCPTR");

constexpr CLASS_ID CLSID_EVENT                      = xray::make_clsid("EVENT   ");
constexpr CLASS_ID CLSID_OBJECT_FLYER               = xray::make_clsid("O_FLYER ");
constexpr CLASS_ID CLSID_OBJECT_DOOR                = xray::make_clsid("O_DOOR  ");
constexpr CLASS_ID CLSID_OBJECT_LIFT                = xray::make_clsid("O_LIFT  ");

// Artefacts
constexpr CLASS_ID CLSID_AF_MERCURY_BALL            = xray::make_clsid("AF_MBALL");
constexpr CLASS_ID CLSID_AF_GRAVI                   = xray::make_clsid("AF_GRAVI");
constexpr CLASS_ID CLSID_AF_BLACKDROPS              = xray::make_clsid("AF_BDROP");
constexpr CLASS_ID CLSID_AF_NEEDLES                 = xray::make_clsid("AF_NEEDL");
constexpr CLASS_ID CLSID_AF_BAST                    = xray::make_clsid("AF_BAST ");
constexpr CLASS_ID CLSID_AF_BLACK_GRAVI             = xray::make_clsid("AF_BGRAV");
constexpr CLASS_ID CLSID_AF_DUMMY                   = xray::make_clsid("AF_DUMMY");
constexpr CLASS_ID CLSID_AF_ZUDA                    = xray::make_clsid("AF_ZUDA ");
constexpr CLASS_ID CLSID_AF_THORN                   = xray::make_clsid("AF_THORN");
constexpr CLASS_ID CLSID_AF_FADED_BALL              = xray::make_clsid("AF_FBALL");
constexpr CLASS_ID CLSID_AF_ELECTRIC_BALL           = xray::make_clsid("AF_EBALL");
constexpr CLASS_ID CLSID_AF_RUSTY_HAIR              = xray::make_clsid("AF_RHAIR");
constexpr CLASS_ID CLSID_AF_GALANTINE               = xray::make_clsid("AF_GALAN");
constexpr CLASS_ID CLSID_AF_CTA                     = xray::make_clsid("AF_CTA  ");

constexpr CLASS_ID CLSID_ARTEFACT                   = xray::make_clsid("ARTEFACT");

// Weapons
constexpr CLASS_ID CLSID_OBJECT_W_M134              = xray::make_clsid("W_M134  ");
constexpr CLASS_ID CLSID_OBJECT_W_FN2000            = xray::make_clsid("W_FN2000");
constexpr CLASS_ID CLSID_OBJECT_W_AK74              = xray::make_clsid("W_AK74  ");
constexpr CLASS_ID CLSID_OBJECT_W_LR300             = xray::make_clsid("W_LR300 ");
constexpr CLASS_ID CLSID_OBJECT_W_HPSA              = xray::make_clsid("W_HPSA  ");
constexpr CLASS_ID CLSID_OBJECT_W_PM                = xray::make_clsid("W_PM    ");
constexpr CLASS_ID CLSID_OBJECT_W_FORT              = xray::make_clsid("W_FORT  ");
constexpr CLASS_ID CLSID_OBJECT_W_BINOCULAR         = xray::make_clsid("W_BINOC ");
constexpr CLASS_ID CLSID_OBJECT_W_SHOTGUN           = xray::make_clsid("W_SHOTGN");
constexpr CLASS_ID CLSID_OBJECT_W_ASHOTGUN          = xray::make_clsid("W_ASHTGN");
//  [8/15/2006]
constexpr CLASS_ID CLSID_OBJECT_W_MAGAZINED         = xray::make_clsid("W_WMAGAZ");
//  [8/15/2006]
//  [8/17/2006]
constexpr CLASS_ID CLSID_OBJECT_W_MAGAZWGL          = xray::make_clsid("W_WMAGGL");
//  [8/17/2006]
constexpr CLASS_ID CLSID_OBJECT_W_SVD               = xray::make_clsid("W_SVD   ");
constexpr CLASS_ID CLSID_OBJECT_W_SVU               = xray::make_clsid("W_SVU   ");
constexpr CLASS_ID CLSID_OBJECT_W_RPG7              = xray::make_clsid("W_RPG7  ");
constexpr CLASS_ID CLSID_OBJECT_W_VAL               = xray::make_clsid("W_VAL   ");
constexpr CLASS_ID CLSID_OBJECT_W_VINTOREZ          = xray::make_clsid("W_VINT  ");
constexpr CLASS_ID CLSID_OBJECT_W_WALTHER           = xray::make_clsid("W_WALTHR");
constexpr CLASS_ID CLSID_OBJECT_W_USP45             = xray::make_clsid("W_USP45 ");
constexpr CLASS_ID CLSID_OBJECT_W_GROZA             = xray::make_clsid("W_GROZA ");
constexpr CLASS_ID CLSID_OBJECT_W_KNIFE             = xray::make_clsid("W_KNIFE ");
constexpr CLASS_ID CLSID_OBJECT_W_BM16              = xray::make_clsid("W_BM16  ");
constexpr CLASS_ID CLSID_OBJECT_W_RG6               = xray::make_clsid("W_RG6   ");
constexpr CLASS_ID CLSID_OBJECT_W_SSRS              = xray::make_clsid("W_SSRS  ");

constexpr CLASS_ID CLSID_OBJECT_W_STATMGUN          = xray::make_clsid("W_STMGUN");

// Weapons Ammo
constexpr CLASS_ID CLSID_OBJECT_AMMO                = xray::make_clsid("AMMO    ");
//------------------------------------------        -----------------------------------
constexpr CLASS_ID CLSID_OBJECT_A_VOG25             = xray::make_clsid("A_VOG25 ");
constexpr CLASS_ID CLSID_OBJECT_A_OG7B              = xray::make_clsid("A_OG7B  ");
constexpr CLASS_ID CLSID_OBJECT_A_M209              = xray::make_clsid("A_M209  ");
//------------------------------------------        -----------------------------------
// Weapons Add-ons
constexpr CLASS_ID CLSID_OBJECT_W_SCOPE             = xray::make_clsid("W_SCOPE ");
constexpr CLASS_ID CLSID_OBJECT_W_SILENCER          = xray::make_clsid("W_SILENC");
constexpr CLASS_ID CLSID_OBJECT_W_GLAUNCHER         = xray::make_clsid("W_GLAUNC");

// Modifiers
constexpr CLASS_ID CLSID_OBJECT_M_QDAMAGE           = xray::make_clsid("O_QDMG  ");
constexpr CLASS_ID CLSID_OBJECT_M_IMMORTAL          = xray::make_clsid("O_IMMORT");
constexpr CLASS_ID CLSID_OBJECT_M_INVIS             = xray::make_clsid("O_INVIS ");

constexpr CLASS_ID CLSID_OBJECT_HEALTH              = xray::make_clsid("O_HEALTH");
constexpr CLASS_ID CLSID_OBJECT_ARMOR               = xray::make_clsid("O_ARMOR ");

constexpr CLASS_ID CLSID_OBJECT_TRIGGER             = xray::make_clsid("O_TRIGER");

// Targets
constexpr CLASS_ID CLSID_TARGET                     = xray::make_clsid("T_BASE  ");
constexpr CLASS_ID CLSID_TARGET_ASSAULT             = xray::make_clsid("T_ASS   ");
constexpr CLASS_ID CLSID_TARGET_CS_BASE             = xray::make_clsid("T_CSBASE");
constexpr CLASS_ID CLSID_TARGET_CS                  = xray::make_clsid("T_CS    ");
constexpr CLASS_ID CLSID_TARGET_CS_CASK             = xray::make_clsid("T_CSCASK");

// Standard level object
constexpr CLASS_ID CLSID_OBJECT_ITEM_STD            = xray::make_clsid("O_ITEM  ");
constexpr CLASS_ID CLSID_OBJECT_BREAKABLE           = xray::make_clsid("O_BRKBL ");
constexpr CLASS_ID CLSID_OBJECT_CLIMABLE            = xray::make_clsid("O_CLMBL ");
constexpr CLASS_ID CLSID_OBJECT_HOLDER_ENT          = xray::make_clsid("O_HLDR_E");

//
constexpr CLASS_ID CLSID_PH_SKELETON_OBJECT         = xray::make_clsid("P_SKELET");
constexpr CLASS_ID CLSID_OBJECT_PHYSIC              = xray::make_clsid("O_PHYSIC");
constexpr CLASS_ID CLSID_PHYSICS_DESTROYABLE        = xray::make_clsid("P_DSTRBL");
constexpr CLASS_ID CLSID_INVENTORY_BOX              = xray::make_clsid("O_INVBOX");

// Zones
constexpr CLASS_ID CLSID_ZONE                       = xray::make_clsid("Z_ZONE  ");
constexpr CLASS_ID CLSID_Z_MBALD                    = xray::make_clsid("Z_MBALD ");
constexpr CLASS_ID CLSID_Z_MINCER                   = xray::make_clsid("Z_MINCER");
constexpr CLASS_ID CLSID_Z_ACIDF                    = xray::make_clsid("Z_ACIDF ");
constexpr CLASS_ID CLSID_Z_GALANT                   = xray::make_clsid("Z_GALANT");
constexpr CLASS_ID CLSID_Z_RADIO                    = xray::make_clsid("Z_RADIO ");
constexpr CLASS_ID CLSID_Z_BFUZZ                    = xray::make_clsid("Z_BFUZZ ");
constexpr CLASS_ID CLSID_Z_RUSTYH                   = xray::make_clsid("Z_RUSTYH");
constexpr CLASS_ID CLSID_Z_AMEBA                    = xray::make_clsid("Z_AMEBA ");
constexpr CLASS_ID CLSID_Z_NOGRAVITY                = xray::make_clsid("Z_NOGRAV");
constexpr CLASS_ID CLSID_Z_FRYUP                    = xray::make_clsid("Z_FRYUP ");
constexpr CLASS_ID CLSID_Z_DEAD                     = xray::make_clsid("Z_DEAD  ");

// Read more about this clsids in object_factory_register.cpp
constexpr CLASS_ID CLSID_LEVEL_CHANGER              = xray::make_clsid("LVLCHNGR");
constexpr CLASS_ID CLSID_LEVEL_CHANGER_S            = xray::make_clsid("LVL_CHNG");

constexpr CLASS_ID CLSID_SCRIPT_ZONE                = xray::make_clsid("SCRIPTZN");
constexpr CLASS_ID CLSID_Z_TEAM_BASE                = xray::make_clsid("Z_TEAMBS");
constexpr CLASS_ID CLSID_Z_TORRID                   = xray::make_clsid("Z_TORRID");
constexpr CLASS_ID CLSID_SPACE_RESTRICTOR           = xray::make_clsid("SPACE_RS");
constexpr CLASS_ID CLSID_SMART_ZONE                 = xray::make_clsid("SMRTZONE");
constexpr CLASS_ID CLSID_Z_CAMPFIRE                 = xray::make_clsid("Z_CFIRE ");

// Detectors
constexpr CLASS_ID CLSID_DETECTOR_SIMPLE            = xray::make_clsid("D_SIMDET");
constexpr CLASS_ID CLSID_DETECTOR_VISUAL            = xray::make_clsid("D_VISDET");
constexpr CLASS_ID CLSID_DETECTOR_ADVANCED          = xray::make_clsid("D_ADVANC");
constexpr CLASS_ID CLSID_DETECTOR_ELITE             = xray::make_clsid("D_ELITE ");
constexpr CLASS_ID CLSID_DETECTOR_SCIENTIFIC        = xray::make_clsid("D_SCIENT");

// PDA
constexpr CLASS_ID CLSID_DEVICE_PDA                 = xray::make_clsid("D_PDA   ");

// Devices
constexpr CLASS_ID CLSID_DEVICE_TORCH               = xray::make_clsid("D_TORCH ");
constexpr CLASS_ID CLSID_DEVICE_AF_MERGER           = xray::make_clsid("D_AFMERG");
constexpr CLASS_ID CLSID_DEVICE_FLARE               = xray::make_clsid("D_FLARE ");

// Inventory items
constexpr CLASS_ID CLSID_IITEM_BOLT                 = xray::make_clsid("II_BOLT ");

constexpr CLASS_ID CLSID_IITEM_MEDKIT               = xray::make_clsid("II_MEDKI");
constexpr CLASS_ID CLSID_IITEM_BANDAGE              = xray::make_clsid("II_BANDG");
constexpr CLASS_ID CLSID_IITEM_FOOD                 = xray::make_clsid("II_FOOD ");
constexpr CLASS_ID CLSID_IITEM_BOTTLE               = xray::make_clsid("II_BOTTL");
constexpr CLASS_ID CLSID_IITEM_ANTIRAD              = xray::make_clsid("II_ANTIR");
constexpr CLASS_ID CLSID_IITEM_EXPLOSIVE            = xray::make_clsid("II_EXPLO");

// Info Document
constexpr CLASS_ID CLSID_IITEM_DOCUMENT             = xray::make_clsid("II_DOC  ");

constexpr CLASS_ID CLSID_IITEM_ATTACH               = xray::make_clsid("II_ATTCH");

// Grenades
constexpr CLASS_ID CLSID_GRENADE_F1                 = xray::make_clsid("G_F1    ");
constexpr CLASS_ID CLSID_OBJECT_G_RPG7              = xray::make_clsid("G_RPG7  ");
constexpr CLASS_ID CLSID_GRENADE_RGD5               = xray::make_clsid("G_RGD5  ");
constexpr CLASS_ID CLSID_OBJECT_G_FAKE              = xray::make_clsid("G_FAKE  ");

//---------------------------------------------------------------------------------
constexpr CLASS_ID CLSID_OBJECT_PLAYERS_BAG         = xray::make_clsid("MP_PLBAG");
//---------------------------------------------------------------------------------

// Equipment
constexpr CLASS_ID CLSID_EQUIPMENT_SIMPLE           = xray::make_clsid("EQU_SMPL");
constexpr CLASS_ID CLSID_EQUIPMENT_SCIENTIFIC       = xray::make_clsid("EQU_SCIE");
constexpr CLASS_ID CLSID_EQUIPMENT_STALKER          = xray::make_clsid("EQU_STLK");
constexpr CLASS_ID CLSID_EQUIPMENT_MILITARY         = xray::make_clsid("EQU_MLTR");
constexpr CLASS_ID CLSID_EQUIPMENT_EXO              = xray::make_clsid("EQU_EXO ");
constexpr CLASS_ID CLSID_EQUIPMENT_HELMET           = xray::make_clsid("EQ_HLMET");
constexpr CLASS_ID CLSID_EQUIPMENT_BACKPACK         = xray::make_clsid("EQ_BAKPK");

// Game types
constexpr CLASS_ID CLSID_SV_GAME_SINGLE             = xray::make_clsid("SV_SINGL");
constexpr CLASS_ID CLSID_SV_GAME_DEATHMATCH         = xray::make_clsid("SV_DM   ");
constexpr CLASS_ID CLSID_SV_GAME_TEAMDEATHMATCH     = xray::make_clsid("SV_TDM  ");
constexpr CLASS_ID CLSID_SV_GAME_ARTEFACTHUNT       = xray::make_clsid("SV_AHUNT");
constexpr CLASS_ID CLSID_SV_GAME_CAPTURETHEARTEFACT = xray::make_clsid("SV_CTA  ");
//constexpr CLASS_ID CLSID_GAME_CS                    = xray::make_clsid("SV_CS   ");
constexpr CLASS_ID CLSID_CL_GAME_SINGLE             = xray::make_clsid("CL_SINGL");
constexpr CLASS_ID CLSID_CL_GAME_DEATHMATCH         = xray::make_clsid("CL_DM   ");
constexpr CLASS_ID CLSID_CL_GAME_TEAMDEATHMATCH     = xray::make_clsid("CL_TDM  ");
constexpr CLASS_ID CLSID_CL_GAME_ARTEFACTHUNT       = xray::make_clsid("CL_AHUNT");
constexpr CLASS_ID CLSID_CL_GAME_CAPTURETHEARTEFACT = xray::make_clsid("CL_CTA  ");

// Game UI types
constexpr CLASS_ID CLSID_GAME_UI_SINGLE             = xray::make_clsid("UI_SINGL");
constexpr CLASS_ID CLSID_GAME_UI_DEATHMATCH         = xray::make_clsid("UI_DM   ");
constexpr CLASS_ID CLSID_GAME_UI_TEAMDEATHMATCH     = xray::make_clsid("UI_TDM  ");
constexpr CLASS_ID CLSID_GAME_UI_ARTEFACTHUNT       = xray::make_clsid("UI_AHUNT");
constexpr CLASS_ID CLSID_GAME_UI_CAPTURETHEARTEFACT = xray::make_clsid("UI_CTA  ");

constexpr CLASS_ID CLSID_ONLINE_OFFLINE_GROUP       = xray::make_clsid("ON_OFF_G");
