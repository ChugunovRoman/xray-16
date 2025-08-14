#include "stdafx.h"
#pragma hdrstop

#include "xrDbg.h"

#include <SDL.h>

XRCORE_API xrDbg Dbg;

xrDbg::xrDbg()
{}

void xrDbg::ClearAll()
{
    for (int i = 0; i < ESectionTypeName::latest; ++i)
        sections_map[i].clear();
}
xr_vector<pcstr> xrDbg::GetSections(ESectionTypeName type)
{
    return sections_map[type];
}
void xrDbg::InitSectionLists()
{
    ClearAll();

    for (const auto& section : pSettings->sections())
    {
        pcstr value{};
        pcstr weaponClass{};
        pcstr itemClass{};
        const auto& name = section->Name;
        const bool exist = section->line_exist("class", &value);
        const bool weaponClassExist = section->line_exist("weapon_class", &weaponClass);
        const bool itemClassExist = section->line_exist("debugger_category", &itemClass);

        if (
            strstr(name.c_str(), "mp_") ||
            strstr(name.c_str(), "animation_hit_") ||
            strstr(name.c_str(), "test") ||
            strstr(name.c_str(), "_hud") ||
            strstr(name.c_str(), "debugger_")
        )
            continue;

        // Ammo
        if (itemClassExist && strstr(itemClass, "ammo"))
            sections_map[ESectionTypeName::ammo].push_back(name.c_str());
        // Scopes
        if (exist && itemClassExist && strstr(itemClass, "scopes"))
            sections_map[ESectionTypeName::scopes].push_back(name.c_str());
        // Silencers
        if (exist && itemClassExist && strstr(itemClass, "silencers"))
            sections_map[ESectionTypeName::silencers].push_back(name.c_str());
        // Launchers
        if (exist && itemClassExist && strstr(itemClass, "grenade_launchers"))
            sections_map[ESectionTypeName::launchers].push_back(name.c_str());
        // wpn_knives
        if (itemClassExist && strstr(itemClass, "knife") && IsParentSection(name))
            sections_map[ESectionTypeName::knife].push_back(name.c_str());
        // wpn_pistols
        if (itemClassExist && strstr(itemClass, "pistols") && IsParentSection(name))
            sections_map[ESectionTypeName::pistol].push_back(name.c_str());
        // wpn_auto_pistols
        if (itemClassExist && strstr(itemClass, "pps") && IsParentSection(name))
            sections_map[ESectionTypeName::auto_pistol].push_back(name.c_str());
        // wpn_shotguns
        if (itemClassExist && strstr(itemClass, "shotguns") && IsParentSection(name))
            sections_map[ESectionTypeName::shotgun].push_back(name.c_str());
        // wpn_rifles
        if (itemClassExist && strstr(itemClass, "assault_rifle") && IsParentSection(name))
            sections_map[ESectionTypeName::rifle].push_back(name.c_str());
        // wpn_sniper_rifles
        if (itemClassExist && strstr(itemClass, "sniper_rifle") && IsParentSection(name))
            sections_map[ESectionTypeName::sniper_rifle].push_back(name.c_str());
        // wpn_heavy_rifle
        if (itemClassExist && strstr(itemClass, "heavy_rifle") && IsParentSection(name))
            sections_map[ESectionTypeName::heavy_rifle].push_back(name.c_str());
        // explosive
        if (itemClassExist && strstr(itemClass, "explosive") && IsParentSection(name))
            sections_map[ESectionTypeName::explosive].push_back(name.c_str());
        // consumable items
        if (itemClassExist && strstr(itemClass, "consumable") && IsParentSection(name))
            sections_map[ESectionTypeName::item_consumable].push_back(name.c_str());
        // medical items
        if (itemClassExist && strstr(itemClass, "medical") && IsParentSection(name))
            sections_map[ESectionTypeName::item_medical].push_back(name.c_str());
        // food items
        if (itemClassExist && strstr(itemClass, "food") && IsParentSection(name))
            sections_map[ESectionTypeName::item_food].push_back(name.c_str());
        // food misc
        if (itemClassExist && strstr(itemClass, "misc") && IsParentSection(name))
            sections_map[ESectionTypeName::item_misc].push_back(name.c_str());
        // food quest
        if (itemClassExist && strstr(itemClass, "quest") && IsParentSection(name))
            sections_map[ESectionTypeName::item_quest].push_back(name.c_str());
        // outfits
        if (exist && xr_strcmp(value, "EQU_STLK") == 0)
            sections_map[ESectionTypeName::outfit].push_back(name.c_str());
        // artefacts
        if (exist && xr_strcmp(value, "ARTEFACT") == 0)
            sections_map[ESectionTypeName::artefact].push_back(name.c_str());
        // squad_npc
        if (exist && xr_strcmp(value, "ON_OFF_S") == 0 && xr_strcmp(value, "monster_sim_squad") != 0)
            sections_map[ESectionTypeName::squad_npc].push_back(name.c_str());
        // squad_npc
        if (exist && xr_strcmp(value, "ON_OFF_S") == 0 && xr_strcmp(value, "monster_sim_squad") == 0)
            sections_map[ESectionTypeName::squad_npc].push_back(name.c_str());
        // npc
        if (exist && xr_strcmp(value, "AI_STL_S") == 0)
            sections_map[ESectionTypeName::npc].push_back(name.c_str());
        // mutant
        if (exist && strstr(value, "SM_"))
            sections_map[ESectionTypeName::mutant].push_back(name.c_str());
        // anomaly
        if (exist && strstr(value, "ZS_"))
            sections_map[ESectionTypeName::anomaly].push_back(name.c_str());
        // phantom
        if (exist && xr_strcmp(value, "AI_PHANT") == 0)
            sections_map[ESectionTypeName::phantom].push_back(name.c_str());
        // backpack
        if (exist && xr_strcmp(value, "EQ_BAKPK") == 0)
            sections_map[ESectionTypeName::backpack].push_back(name.c_str());
    }
}

bool xrDbg::IsParentSection(const shared_str section)
{
    if (!pSettings->line_exist(section, "parent_section"))
        return true;
        
    const shared_str parent_section = pSettings->r_string(section, "parent_section");

    if (xr_strcmp(parent_section.c_str(), section.c_str()) == 0)
        return true;

    return false;
}
