#pragma once

#ifndef MASTER_GOLD
#include "clsid_game.h"

namespace xray
{
enum class SpawnCategory : u8
{
    Artefacts,
    ArtefactContainers,

    ItemsFood,
    ItemsDrink,
    ItemsMedicine,
    ItemsDevices,
    ItemsTools,
    ItemsRepair,
    ItemsParts,
    ItemsMiscellaneous,
    ItemsNotes,
    ItemsQuest,
    ItemsUpgrades,

    Helmets,
    OutfitsAttachments,
    OutfitsLight,
    OutfitsMedium,
    OutfitsHeavy,

    WeaponsAmmo,
    WeaponsMelee,
    WeaponsPistols,
    WeaponsShotguns,
    WeaponsSMG,
    WeaponsRifles,
    WeaponsSniperRifles,
    WeaponsExplosives,
    WeaponsMiscellaneous,

    Vehicles,
    Physics,

    CreaturesStalkers,
    CreaturesMutants,
    CreaturesPhantoms,
    SquadsStalkers,
    SquadsMutants,

    Zones,

    CategoriesCount,
    Unknown = u8(-1)
};

constexpr pcstr spawn_category_to_text(SpawnCategory category)
{
    switch (category)
    {
    case SpawnCategory::Artefacts:            return "Artefacts";
    case SpawnCategory::ArtefactContainers:   return "Artefact containers";

    case SpawnCategory::ItemsFood:            return "Items (Food)";
    case SpawnCategory::ItemsDrink:           return "Items (Drink)";
    case SpawnCategory::ItemsMedicine:        return "Items (Medicine)";
    case SpawnCategory::ItemsDevices:         return "Items (Devices)";
    case SpawnCategory::ItemsTools:           return "Items (Tools)";
    case SpawnCategory::ItemsRepair:          return "Items (Repair)";
    case SpawnCategory::ItemsParts:           return "Items (Parts)";
    case SpawnCategory::ItemsMiscellaneous:   return "Items (Misc.)";
    case SpawnCategory::ItemsNotes:           return "Items (Notes)";
    case SpawnCategory::ItemsQuest:           return "Items (Quest)";
    case SpawnCategory::ItemsUpgrades:        return "Items (Upgrades)";

    case SpawnCategory::Helmets:              return "Helmets";
    case SpawnCategory::OutfitsAttachments:   return "Outfits (Attachments)";
    case SpawnCategory::OutfitsLight:         return "Outfits (Light)";
    case SpawnCategory::OutfitsMedium:        return "Outfits (Medium)";
    case SpawnCategory::OutfitsHeavy:         return "Outfits (Heavy)";

    case SpawnCategory::WeaponsAmmo:          return "Weapons (Ammo)";
    case SpawnCategory::WeaponsMelee:         return "Weapons (Melee)";
    case SpawnCategory::WeaponsPistols:       return "Weapons (Pistols)";
    case SpawnCategory::WeaponsShotguns:      return "Weapons (Shotguns)";
    case SpawnCategory::WeaponsSMG:           return "Weapons (SMG)";
    case SpawnCategory::WeaponsRifles:        return "Weapons (Rifles)";
    case SpawnCategory::WeaponsSniperRifles:  return "Weapons (Sniper)";
    case SpawnCategory::WeaponsExplosives:    return "Weapons (Explosives)";
    case SpawnCategory::WeaponsMiscellaneous: return "Weapons (Misc.)";

    case SpawnCategory::Vehicles:             return "Vehicles";
    case SpawnCategory::Physics:              return "Physic (Misc.)";

    case SpawnCategory::CreaturesStalkers:    return "NPC (Stalkers)";
    case SpawnCategory::CreaturesMutants:     return "NPC (Mutants)";
    case SpawnCategory::CreaturesPhantoms:    return "Phantoms";
    case SpawnCategory::SquadsStalkers:       return "Squads (Stalkers)";
    case SpawnCategory::SquadsMutants:        return "Squads (Mutants)";

    case SpawnCategory::Zones:                return "Anomalies";

    default:
    case SpawnCategory::Unknown:              return "Unknown";
    } // switch (category)
}

constexpr SpawnCategory kind_to_spawn_category(pcstr kind)
{
    switch (strhash(kind))
    {
    case "i_arty"_hash:             return SpawnCategory::Artefacts;
    case "i_arty_junk"_hash:        return SpawnCategory::Artefacts;
    case "i_arty_cont"_hash:        return SpawnCategory::ArtefactContainers;

    case "i_food"_hash:             return SpawnCategory::ItemsFood;
    case "i_mutant_raw"_hash:       return SpawnCategory::ItemsFood;
    case "i_mutant_cooked"_hash:    return SpawnCategory::ItemsFood;
    case "i_drink"_hash:            return SpawnCategory::ItemsDrink;
    case "i_medical"_hash:          return SpawnCategory::ItemsMedicine;
    case "i_device"_hash:           return SpawnCategory::ItemsDevices;
    case "i_kit"_hash:              return SpawnCategory::ItemsTools;
    case "i_tool"_hash:             return SpawnCategory::ItemsTools;
    case "i_repair"_hash:           return SpawnCategory::ItemsRepair;
    case "i_part"_hash:             return SpawnCategory::ItemsParts;
    case "i_mutant_part"_hash:      return SpawnCategory::ItemsMiscellaneous;
    case "i_misc"_hash:             return SpawnCategory::ItemsMiscellaneous;
    case "i_letter"_hash:           return SpawnCategory::ItemsNotes;
    case "i_quest"_hash:            return SpawnCategory::ItemsQuest;
    case "i_upgrade"_hash:          return SpawnCategory::ItemsUpgrades;

    case "o_helmet"_hash:           return SpawnCategory::Helmets;
    case "i_mutant_belt"_hash:      return SpawnCategory::OutfitsAttachments;
    case "i_attach"_hash:           return SpawnCategory::OutfitsAttachments;
    case "i_backpack"_hash:         return SpawnCategory::OutfitsAttachments;
    case "o_light"_hash:            return SpawnCategory::OutfitsLight;
    case "o_medium"_hash:           return SpawnCategory::OutfitsMedium;
    case "o_sci"_hash:              return SpawnCategory::OutfitsMedium;
    case "o_heavy"_hash:            return SpawnCategory::OutfitsHeavy;

    case "w_ammo"_hash:             return SpawnCategory::WeaponsAmmo;
    case "w_melee"_hash:            return SpawnCategory::WeaponsMelee;
    case "w_pistol"_hash:           return SpawnCategory::WeaponsPistols;
    case "w_shotgun"_hash:          return SpawnCategory::WeaponsShotguns;
    case "w_smg"_hash:              return SpawnCategory::WeaponsSMG;
    case "w_rifle"_hash:            return SpawnCategory::WeaponsRifles;
    case "w_sniper"_hash:           return SpawnCategory::WeaponsSniperRifles;
    case "w_explosive"_hash:        return SpawnCategory::WeaponsExplosives;
    case "w_misc"_hash:             return SpawnCategory::WeaponsMiscellaneous;

    case "S_WPN_MISC"_hash:         return SpawnCategory::WeaponsMiscellaneous;
    case "SM_KARLIK"_hash:          return SpawnCategory::CreaturesMutants;
    case "SM_LURKER"_hash:          return SpawnCategory::CreaturesMutants;
    case "SM_PSYSUCKER"_hash:       return SpawnCategory::CreaturesMutants;
    } // switch (strhash(kind))

    return SpawnCategory::Unknown;
}

constexpr SpawnCategory clsid_to_spawn_category(CLASS_ID clsid)
{
    switch (clsid)
    {
    case CLSID_ARTEFACT:
    case CLSID_AF_MERCURY_BALL:
    case CLSID_AF_GRAVI:
    case CLSID_AF_BLACKDROPS:
    case CLSID_AF_NEEDLES:
    case CLSID_AF_BAST:
    case CLSID_AF_BLACK_GRAVI:
    case CLSID_AF_DUMMY:
    case CLSID_AF_ZUDA:
    case CLSID_AF_THORN:
    case CLSID_AF_FADED_BALL:
    case CLSID_AF_ELECTRIC_BALL:
    case CLSID_AF_RUSTY_HAIR:
    case CLSID_AF_GALANTINE:
    case CLSID_AF_CTA:
    case make_clsid("SCRPTART"):
        return SpawnCategory::Artefacts;

    case CLSID_IITEM_FOOD:
    case make_clsid("S_FOOD  "):
        return SpawnCategory::ItemsFood;

    case CLSID_IITEM_BOTTLE:
        return SpawnCategory::ItemsDrink;

    case CLSID_IITEM_MEDKIT:
    case CLSID_IITEM_BANDAGE:
    case CLSID_IITEM_ANTIRAD:
        return SpawnCategory::ItemsMedicine;

    case CLSID_DETECTOR_SIMPLE:
    case CLSID_DETECTOR_VISUAL:
    case CLSID_DETECTOR_ADVANCED:
    case CLSID_DETECTOR_ELITE:
    case CLSID_DETECTOR_SCIENTIFIC:
    case make_clsid("DET_SIMP"):
    case make_clsid("DET_ADVA"):
    case make_clsid("DET_ELIT"):
    case make_clsid("DET_SCIE"):
        return SpawnCategory::ItemsDevices;

    case CLSID_IITEM_ATTACH:
    case CLSID_DEVICE_PDA:
    case CLSID_DEVICE_TORCH:
    case CLSID_DEVICE_FLARE:
    case make_clsid("EQ_PATCH"):
    case make_clsid("II_BTTCH"):
    case make_clsid("TORCH_S "):
    case make_clsid("D_FLALIT"):
    case make_clsid("D_DSMETR"):
    case make_clsid("S_PDA   "):
        return SpawnCategory::ItemsMiscellaneous;

    case CLSID_IITEM_DOCUMENT:
        return SpawnCategory::ItemsNotes;

    case CLSID_EQUIPMENT_HELMET:
    case make_clsid("E_HLMET "):
        return SpawnCategory::Helmets;

    case CLSID_EQUIPMENT_BACKPACK:
        return SpawnCategory::OutfitsAttachments;

    case CLSID_EQUIPMENT_SIMPLE:
        return SpawnCategory::OutfitsLight;

    case CLSID_EQUIPMENT_SCIENTIFIC:
    case CLSID_EQUIPMENT_STALKER:
    case make_clsid("E_STLK  "):
        return SpawnCategory::OutfitsMedium;

    case CLSID_EQUIPMENT_MILITARY:
    case CLSID_EQUIPMENT_EXO:
        return SpawnCategory::OutfitsHeavy;

    case CLSID_OBJECT_AMMO:
    case CLSID_OBJECT_A_VOG25:
    case CLSID_OBJECT_A_OG7B:
    case CLSID_OBJECT_A_M209:
    case make_clsid("AMMO_S  "):
    case make_clsid("S_VOG25 "):
    case make_clsid("S_OG7B  "):
    case make_clsid("S_M209  "):
        return SpawnCategory::WeaponsAmmo;

    case CLSID_OBJECT_W_KNIFE:
    case make_clsid("WP_KNIFE"):
        return SpawnCategory::WeaponsMelee;

    case CLSID_OBJECT_W_HPSA:
    case CLSID_OBJECT_W_PM:
    case CLSID_OBJECT_W_FORT:
    case CLSID_OBJECT_W_WALTHER:
    case CLSID_OBJECT_W_USP45:
    case make_clsid("WP_HPSA "):
    case make_clsid("WP_PM   "):
    case make_clsid("WP_USP45"):
    case make_clsid("WP_WALTH"):
        return SpawnCategory::WeaponsPistols;

    case CLSID_OBJECT_W_SHOTGUN:
    case CLSID_OBJECT_W_ASHOTGUN:
    case CLSID_OBJECT_W_BM16:
    case make_clsid("WP_SHOTG"):
    case make_clsid("WP_ASHTG"):
    case make_clsid("WP_BM16 "):
        return SpawnCategory::WeaponsShotguns;

    case CLSID_OBJECT_W_FN2000:
    case CLSID_OBJECT_W_AK74:
    case CLSID_OBJECT_W_LR300:
    case CLSID_OBJECT_W_MAGAZINED:
    case CLSID_OBJECT_W_MAGAZWGL:
    case CLSID_OBJECT_W_VAL:
    case CLSID_OBJECT_W_VINTOREZ:
    case CLSID_OBJECT_W_GROZA:
    case make_clsid("WP_GROZA"):
    case make_clsid("WP_AK74 "):
    case make_clsid("WP_LR300"):
    case make_clsid("WP_MAGAZ"):
    case make_clsid("WP_VAL  "):
    case make_clsid("WP_VINT "):
        return SpawnCategory::WeaponsRifles;

    case CLSID_OBJECT_W_SVD:
    case CLSID_OBJECT_W_SVU:
    case make_clsid("WP_SVD  "):
    case make_clsid("WP_SVU  "):
        return SpawnCategory::WeaponsSniperRifles;

    case CLSID_OBJECT_W_RPG7:
    case CLSID_OBJECT_W_RG6:
    case CLSID_GRENADE_F1:
    case CLSID_GRENADE_RGD5:
    case make_clsid("WP_RG6  "):
    case make_clsid("WP_RPG7 "):
    case make_clsid("G_F1_S  "):
    case make_clsid("G_RGD5_S"):
        return SpawnCategory::WeaponsExplosives;

    case CLSID_OBJECT_W_BINOCULAR:
    case CLSID_OBJECT_W_SCOPE:
    case CLSID_OBJECT_W_SILENCER:
    case CLSID_OBJECT_W_GLAUNCHER:
    case CLSID_IITEM_BOLT:
    case CLSID_OBJECT_W_STATMGUN:
    case make_clsid("WP_SCOPE"):
    case make_clsid("WP_SILEN"):
    case make_clsid("WP_GLAUN"):
    case make_clsid("WP_BINOC"):
        return SpawnCategory::WeaponsMiscellaneous;

    case CLSID_CAR:
    case CLSID_VEHICLE_HELICOPTER:
    case make_clsid("C_HLCP_S"):
    case make_clsid("SCRPTCAR"):
        return SpawnCategory::Vehicles;

    case CLSID_OBJECT_HLAMP:
    case CLSID_OBJECT_PROJECTOR:
    case CLSID_OBJECT_BREAKABLE:
    case CLSID_OBJECT_CLIMABLE:
    case CLSID_OBJECT_HOLDER_ENT:
    case CLSID_PH_SKELETON_OBJECT:
    case CLSID_OBJECT_PHYSIC:
    case CLSID_PHYSICS_DESTROYABLE:
    case CLSID_INVENTORY_BOX:
    case CLSID_IITEM_EXPLOSIVE:
    case make_clsid("SO_HLAMP"):
    case make_clsid("O_PHYS_S"):
    case make_clsid("O_DSTR_S"):
    case make_clsid("S_INVBOX"):
    case make_clsid("S_EXPLO "):
        return SpawnCategory::Physics;

    case CLSID_AI_CROW:
    case CLSID_AI_ZOMBIE:
    case CLSID_AI_POLTERGEIST:
    case CLSID_AI_FLESH:
    case CLSID_AI_CONTROLLER:
    case CLSID_AI_BLOODSUCKER:
    case CLSID_AI_BURER:
    case CLSID_AI_GIANT:
    case CLSID_AI_CHIMERA:
    case CLSID_AI_FRACTURE:
    case CLSID_AI_DOG_BLACK:
    case CLSID_AI_DOG_RED:
    case CLSID_AI_DOG_PSY:
    case CLSID_AI_BOAR:
    case CLSID_AI_SNORK:
    case CLSID_AI_CAT:
    case CLSID_AI_TUSHKANO:
    case CLSID_AI_RAT:
    case make_clsid("SM_BLOOD"):
    case make_clsid("SM_BOARW"):
    case make_clsid("SM_DOG_S"):
    case make_clsid("SM_FLESH"):
    case make_clsid("SM_P_DOG"):
    case make_clsid("SM_BURER"):
    case make_clsid("SM_CAT_S"):
    case make_clsid("SM_CHIMS"):
    case make_clsid("SM_CONTR"):
    case make_clsid("SM_IZLOM"):
    case make_clsid("SM_POLTR"):
    case make_clsid("SM_GIANT"):
    case make_clsid("SM_ZOMBI"):
    case make_clsid("SM_SNORK"):
    case make_clsid("SM_TUSHK"):
    case make_clsid("SM_RAT  "):
    case make_clsid("SM_DOG_P"):
        return SpawnCategory::CreaturesMutants;

    case CLSID_AI_STALKER:
    case CLSID_AI_TRADER:
    case make_clsid("AI_STL_S"):
    case make_clsid("AI_TRD_S"):
        return SpawnCategory::CreaturesStalkers;

    case CLSID_AI_PHANTOM:
    case CLSID_AI_DOG_PSY_PHANTOM:
    case make_clsid("SM_DOG_F"):
        return SpawnCategory::CreaturesPhantoms;

    case CLSID_ONLINE_OFFLINE_GROUP:
    case make_clsid("ON_OFF_S"):
        return SpawnCategory::SquadsStalkers;

    case CLSID_ZONE:
    case CLSID_Z_MBALD:
    case CLSID_Z_MINCER:
    case CLSID_Z_ACIDF:
    case CLSID_Z_GALANT:
    case CLSID_Z_RADIO:
    case CLSID_Z_BFUZZ:
    case CLSID_Z_RUSTYH:
    case CLSID_Z_AMEBA:
    case CLSID_Z_NOGRAVITY:
    case CLSID_Z_FRYUP:
    case CLSID_Z_DEAD:
    case CLSID_Z_TORRID:
    case CLSID_Z_CAMPFIRE:
    case make_clsid("ZS_BFUZZ"):
    case make_clsid("ZS_MBALD"):
    case make_clsid("ZS_GALAN"):
    case make_clsid("ZS_MINCE"):
    case make_clsid("ZS_RADIO"):
    case make_clsid("ZS_TORRD"):
    case make_clsid("ZS_AMEBA"):
    case make_clsid("ZS_NGRAV"):
        return SpawnCategory::Zones;
    } // switch (clsid)

    return SpawnCategory::Unknown;
}

constexpr SpawnCategory weapon_class_to_spawn_category(pcstr wpclass)
{
    switch (strhash(wpclass))
    {
    case "shotgun"_hash:        return SpawnCategory::WeaponsShotguns;
    case "assault_rifle"_hash:  return SpawnCategory::WeaponsRifles;
    case "heavy_weapon"_hash:   return SpawnCategory::WeaponsRifles;
    case "sniper_rifle"_hash:   return SpawnCategory::WeaponsSniperRifles;
    }
    return SpawnCategory::Unknown;
}

constexpr SpawnCategory try_detect_spawn_category(pcstr kind, pcstr wpclass, CLASS_ID clsid)
{
    SpawnCategory category{ SpawnCategory::Unknown };

    if (kind)
        category = kind_to_spawn_category(kind);

    if (wpclass && category == SpawnCategory::Unknown)
        category = weapon_class_to_spawn_category(wpclass);

    if (clsid && category == SpawnCategory::Unknown)
        category = clsid_to_spawn_category(clsid);

    return category;
}
} // namespace xray
#endif // MASTER_GOLD
