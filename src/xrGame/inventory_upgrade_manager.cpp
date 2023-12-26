////////////////////////////////////////////////////////////////////////////
//	Module 		: inventory_upgrade_manager.cpp
//	Created 	: 19.10.2007
//	Author		: Dmitriy Iassenev, Evgeniy Sokolov
//	Description : inventory upgrade manager class implementation
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "inventory_upgrade_manager.h"
#include "inventory_upgrade_base.h"
#include "inventory_upgrade.h"
#include "inventory_upgrade_root.h"
#include "inventory_upgrade_group.h"
#include "inventory_upgrade_property.h"

int g_upgrades_log = 0;

namespace inventory
{
namespace upgrade
{
// using inventory::upgrade::Manager;
// using inventory::upgrade::UpgradeBase;
// using inventory::upgrade::Upgrade;
// using inventory::upgrade::Root;
// using inventory::upgrade::Group;

Manager::Manager()
{
    load_all_properties(); // first
    load_all_inventory();
}

Manager::~Manager()
{
    delete_data(m_roots);
    delete_data(m_groups);
    delete_data(m_upgrades);
    delete_data(m_properties);
    m_roots.clear(); // !!!!!!!!!!!!
    m_groups.clear();
    m_upgrades.clear();
    m_properties.clear();
}

Root* Manager::get_root(shared_str const& root_id)
{
    Roots_type::const_iterator i = m_roots.find(root_id);
    if (i != m_roots.end())
    {
        return ((*i).second);
    }
    return (NULL);
}

Upgrade* Manager::get_upgrade(shared_str const& upgrade_id)
{
    Upgrades_type::const_iterator i = m_upgrades.find(upgrade_id);
    if (i != m_upgrades.end())
    {
        return ((*i).second);
    }
    return (NULL);
}

Group* Manager::get_group(shared_str const& group_id)
{
    Groups_type::const_iterator i = m_groups.find(group_id);
    if (i != m_groups.end())
    {
        return ((*i).second);
    }
    return (NULL);
}

Property* Manager::get_property(shared_str const& property_id)
{
    Properties_type::const_iterator i = m_properties.find(property_id);
    if (i != m_properties.end())
    {
        return ((*i).second);
    }
    return (NULL);
}

// -----------------------------------------------------------------------

Root* Manager::add_root(shared_str const& root_id)
{
    if (get_root(root_id))
    {
        VERIFY2(0, make_string("Try add the existent upgrade_root for inventory item <%s>!", root_id.c_str()));
    }
    Root* new_root = xr_new<Root>();
    m_roots.emplace(root_id, new_root);
    new_root->construct(root_id, *this);
    return (new_root);
}

Upgrade* Manager::add_upgrade(shared_str const& upgrade_id, Group& parent_group)
{
    if (get_upgrade(upgrade_id))
    {
        VERIFY2(
            0, make_string("Try add the existent upgrade (%s), in group <%s>. Such upgrade is in group <%s> already!",
                   upgrade_id.c_str(), parent_group.id_str(), get_upgrade(upgrade_id)->parent_group_id().c_str()));
    }
    Upgrade* new_upgrade = xr_new<Upgrade>();
    m_upgrades.emplace(upgrade_id, new_upgrade);
    new_upgrade->construct(upgrade_id, parent_group, *this);
    return (new_upgrade);
}

Group* Manager::add_group(shared_str const& group_id, UpgradeBase& parent_upgrade)
{
    Group* new_group = get_group(group_id);
    if (!new_group)
    {
        new_group = xr_new<Group>();
        m_groups.emplace(group_id, new_group);
        new_group->construct(group_id, parent_upgrade, *this);
        return (new_group);
    }
    new_group->add_parent_upgrade(parent_upgrade);
    return (new_group);
}

Property* Manager::add_property(shared_str const& property_id)
{
    if (get_property(property_id))
    {
        VERIFY2(0, make_string("Try add the existent upgrade property <%s>!", property_id.c_str()));
    }
    Property* new_property = xr_new<Property>();
    m_properties.emplace(property_id, new_property);
    new_property->construct(property_id, *this);
    return (new_property);
}

// -------------------------------------------------------------------------------------------

bool Manager::item_upgrades_exist(shared_str const& item_id)
{
    VERIFY2(pSettings->section_exist(item_id), make_string("Inventory item [%s] does not exist!", item_id.c_str()));
    if (!pSettings->line_exist(item_id, "upgrades") || !pSettings->r_string(item_id, "upgrades"))
    {
        return false;
    }
    if (!pSettings->line_exist(item_id, "upgrade_scheme") || !pSettings->r_string(item_id, "upgrade_scheme"))
    {
        return false;
    }

    return true;
}

class check_upgraded_inventory_section
{
    const CInifile::Sect* inv_section{};

public:
    check_upgraded_inventory_section(pcstr items_section)
    {
        if (pSettings->section_exist(items_section))
            inv_section = &pSettings->r_section(items_section);
    }

    [[nodiscard]]
    bool add_anyway(shared_str const& item_id) const
    {
        if (!inv_section)
            return false;

        const auto ib = inv_section->Data.begin();
        const auto ie = inv_section->Data.end();

        const auto it = std::find_if(ib, ie, [&](const CInifile::Item& item)
        {
            return item.first == item_id;
        });

        return it != ie;
    }
};

void Manager::add_section_by_class(CInifile::Sect *const &section)
{
  pcstr value{};
  pcstr weaponClass{};
  const auto& name = section->Name;
  const bool exist = section->line_exist("class", &value);
  const bool weaponClassExist = section->line_exist("weapon_class", &weaponClass);

  if (strstr(name.c_str(), "mp_") || strstr(name.c_str(), "animation_hit_"))
  {
    return;
  }

  // Ammo
  if (exist && xr_strcmp(value, "AMMO") == 0)
  {
    Level().sections_map[ESectionTypeName::ammo].push_back(name.c_str());
  }
  // wpn_knives
  if (weaponClassExist && xr_strcmp(weaponClass, "knife") == 0)
  {
    Level().sections_map[ESectionTypeName::knife].push_back(name.c_str());
  }
  // wpn_pistols
  if (weaponClassExist && xr_strcmp(weaponClass, "pistol") == 0)
  {
    Level().sections_map[ESectionTypeName::pistol].push_back(name.c_str());
  }
  // wpn_auto_pistols
  if (weaponClassExist && xr_strcmp(weaponClass, "auto_pistol") == 0)
  {
    Level().sections_map[ESectionTypeName::auto_pistol].push_back(name.c_str());
  }
  // wpn_shotguns
  if (weaponClassExist && xr_strcmp(weaponClass, "shotgun") == 0)
  {
    Level().sections_map[ESectionTypeName::shotgun].push_back(name.c_str());
  }
  // wpn_rifles
  if (weaponClassExist && xr_strcmp(weaponClass, "assault_rifle") == 0)
  {
    Level().sections_map[ESectionTypeName::rifle].push_back(name.c_str());
  }
  // wpn_sniper_rifles
  if (weaponClassExist && xr_strcmp(weaponClass, "sniper_rifle") == 0)
  {
    Level().sections_map[ESectionTypeName::sniper_rifle].push_back(name.c_str());
  }
  // wpn_heavy_rifle
  if (weaponClassExist && xr_strcmp(weaponClass, "heavy_rifle") == 0)
  {
    Level().sections_map[ESectionTypeName::heavy_rifle].push_back(name.c_str());
  }
  // outfits
  if (exist && xr_strcmp(value, "EQU_STLK") == 0)
  {
    Level().sections_map[ESectionTypeName::outfit].push_back(name.c_str());
  }
  // artefacts
  if (exist && xr_strcmp(value, "ARTEFACT") == 0)
  {
    Level().sections_map[ESectionTypeName::artefact].push_back(name.c_str());
  }
  // squad_npc
  if (exist && xr_strcmp(value, "ON_OFF_S") == 0 && xr_strcmp(value, "monster_sim_squad") != 0)
  {
    Level().sections_map[ESectionTypeName::squad_npc].push_back(name.c_str());
  }
  // squad_npc
  if (exist && xr_strcmp(value, "ON_OFF_S") == 0 && xr_strcmp(value, "monster_sim_squad") == 0)
  {
    Level().sections_map[ESectionTypeName::squad_npc].push_back(name.c_str());
  }
  // npc
  if (exist && xr_strcmp(value, "AI_STL_S") == 0)
  {
    Level().sections_map[ESectionTypeName::npc].push_back(name.c_str());
  }
  // mutant
  if (exist && strstr(value, "SM_"))
  {
    Level().sections_map[ESectionTypeName::mutant].push_back(name.c_str());
  }
  // anomaly
  if (exist && strstr(value, "ZS_"))
  {
    Level().sections_map[ESectionTypeName::anomaly].push_back(name.c_str());
  }
  // phantom
  if (exist && xr_strcmp(value, "AI_PHANT") == 0)
  {
    Level().sections_map[ESectionTypeName::phantom].push_back(name.c_str());
  }
}

void Manager::load_all_inventory()
{
    static constexpr pcstr items_section = "upgraded_inventory";

    if (g_upgrades_log == 1)
    {
        Msg("# Inventory upgrade manager is loaded.");
    }

    const check_upgraded_inventory_section inv_section{ items_section };

    // Alundaio: No longer the need to define upgradeable sections in [upgraded_inventory]
    // Xottab_DUTY: But still follow original COP behaviour, i.e. add section anyway if it is defined in [upgraded_inventory]
    for (const auto& section : pSettings->sections())
    {
        const auto& name = section->Name;

        if (item_upgrades_exist(name) || inv_section.add_anyway(name))
        {
            add_root(name);
        }

        add_section_by_class(section);
    }
    //-Alundaio

    if (g_upgrades_log == 1)
    {
        Msg("# Upgrades of inventory items loaded.");
    }
}

void Manager::load_all_properties()
{
    LPCSTR properties_section = "upgrades_properties";

    if (!pSettings->section_exist(properties_section) && ShadowOfChernobylMode)
        return;


    VERIFY2(
        pSettings->section_exist(properties_section), make_string("Section [%s] does not exist !", properties_section));
    VERIFY2(pSettings->line_count(properties_section), make_string("Section [%s] is empty !", properties_section));

    CInifile::Sect& inv_section = pSettings->r_section(properties_section);
    auto ib = inv_section.Data.begin();
    auto ie = inv_section.Data.end();
    for (; ib != ie; ++ib)
    {
        shared_str property_id((*ib).first);
        add_property(property_id);
    }

    if (g_upgrades_log == 1)
    {
        Msg("# Upgrades properties of inventory itmes loaded.");
    }
}

//---------------------------------------------------------------------------------------

#ifdef DEBUG

void Manager::log_hierarchy()
{
    { // roots
        Msg("# inventory upgrades roots: [%d] ", m_roots.size());
        Roots_type::iterator ib = m_roots.begin();
        Roots_type::iterator ie = m_roots.end();
        for (; ib != ie; ++ib)
        {
            Msg("   %s", (*ib).first.c_str());
        }
    }

    { // groups
        Msg("# inventory upgrades groups: [%d] ", m_groups.size());
        Groups_type::iterator ib = m_groups.begin();
        Groups_type::iterator ie = m_groups.end();
        for (; ib != ie; ++ib)
        {
            Msg("   %s", (*ib).first.c_str());
        }
    }

    { // upgrades
        Msg("# inventory upgrades: [%d] ", m_upgrades.size());
        Upgrades_type::iterator ib = m_upgrades.begin();
        Upgrades_type::iterator ie = m_upgrades.end();
        for (; ib != ie; ++ib)
        {
            Msg("   %s", (*ib).first.c_str());
        }
    }

    { // properties
        Msg("# inventory upgrade properties: [%d] ", m_properties.size());
        Properties_type::iterator ib = m_properties.begin();
        Properties_type::iterator ie = m_properties.end();
        for (; ib != ie; ++ib)
        {
            Msg("   %s", (*ib).first.c_str());
        }
    }

    Msg("- ----- ----- ----- inventory upgrades hierarchy: begin ----- ----- -----");

    Roots_type::iterator ib = m_roots.begin();
    Roots_type::iterator ie = m_roots.end();
    for (; ib != ie; ++ib)
    {
        ((*ib).second)->log_hierarchy("");
    }

    Msg("- ----- ----- ----- inventory upgrades hierarchy: end   ----- ----- -----");
}

void Manager::test_all_upgrades(CInventoryItem& item)
{
    Root* root_p = get_root(item.m_section_id);
    VERIFY2(root_p,
        make_string("Upgrades for item <%s> (id = %d) does not exist!", item.m_section_id.c_str(), item.object_id()));
    root_p->test_all_upgrades(item);

    if (g_upgrades_log == 1)
    {
        Msg("- Checking all upgrades of item <%s> (id = %d) is successful.", root_p->id_str(), item.object_id());
    }
}

#endif // DEBUG

Upgrade* Manager::upgrade_verify(shared_str const& item_section, shared_str const& upgrade_id)
{
    [[maybe_unused]] auto root_p = get_root(item_section);
    VERIFY2(root_p, make_string("Upgrades of item <%s> don`t exist!", item_section.c_str()));

    Upgrade* upgrade_p = get_upgrade(upgrade_id);
    VERIFY2(
        upgrade_p, make_string("Upgrade <%s> in item <%s> does not exist!", upgrade_id.c_str(), item_section.c_str()));

    VERIFY2(root_p->contain_upgrade(upgrade_id),
        make_string("Inventory item <%s> not contain upgrade <%s> !", item_section.c_str(), upgrade_id.c_str()));

    return upgrade_p;
}

bool Manager::make_known_upgrade(CInventoryItem& item, shared_str const& upgrade_id)
{
    return (upgrade_verify(item.m_section_id, upgrade_id)->make_known());
}

bool Manager::make_known_upgrade(const shared_str& upgrade_id)
{
    Upgrade* upgrade_p = get_upgrade(upgrade_id);
    VERIFY2(upgrade_p, make_string("Upgrade <%s> does not exist!", upgrade_id.c_str()));
    return (upgrade_p->make_known());
}

bool Manager::is_known_upgrade(CInventoryItem& item, shared_str const& upgrade_id)
{
    return (upgrade_verify(item.m_section_id, upgrade_id)->is_known());
}

bool Manager::is_known_upgrade(shared_str const& upgrade_id)
{
    Upgrade* upgrade_p = get_upgrade(upgrade_id);
    VERIFY2(upgrade_p, make_string("Upgrade <%s> does not exist!", upgrade_id.c_str()));
    return (upgrade_p->is_known());
}

/*
bool Manager::is_disabled_upgrade( CInventoryItem& item, shared_str const& upgrade_id )
{
    Upgrade* upgrade_p = upgrade_verify( item.m_section_id, upgrade_id );
    return upgrade_p->can_install( item );
}
*/

bool Manager::can_install_upgrade(CInventoryItem& item, shared_str const& upgrade_id)
{
    return upgrade_verify(item.m_section_id, upgrade_id)->can_install(item, false) == result_ok;
}

bool Manager::can_add_upgrade(CInventoryItem& item, shared_str const& upgrade_id)
{
    return upgrade_verify(item.m_section_id, upgrade_id)->can_add(item) == result_ok;
}

bool Manager::upgrade_add(CInventoryItem& item, shared_str const& upgrade_id)
{
    Upgrade* upgrade = upgrade_verify(item.m_section_id, upgrade_id);
    UpgradeStateResult res = upgrade->can_add(item);

    if (res == result_ok)
    {
        if (item.install_upgrade(upgrade->section()))
        {
            item.add_upgrade(upgrade_id, false);

            return true;
        }
        else
        {
            FATAL(make_string("! Upgrade <%s> of item [%s] (id = %d) is EMPTY or FAILED !", upgrade_id.c_str(),
                item.m_section_id.c_str(), item.object_id())
                      .c_str());
        }
    }

    return false;
}

bool Manager::upgrade_install(CInventoryItem& item, shared_str const& upgrade_id, bool loading)
{
    Upgrade* upgrade = upgrade_verify(item.m_section_id, upgrade_id);
    UpgradeStateResult res = upgrade->can_install(item, loading);

    if (res == result_ok)
    {
        if (!loading)
        {
            item.pre_install_upgrade();
        }

        if (item.install_upgrade(upgrade->section()))
        {
            upgrade->run_effects(loading);
            item.add_upgrade(upgrade_id, loading);

            if (g_upgrades_log == 1)
            {
                Msg("# Upgrade <%s> of inventory item [%s] (id = %d) is installed.", upgrade_id.c_str(),
                    item.m_section_id.c_str(), item.object_id());
            }
            return true;
        }
        else
        {
            FATAL(make_string("! Upgrade <%s> of item [%s] (id = %d) is EMPTY or FAILED !", upgrade_id.c_str(),
                item.m_section_id.c_str(), item.object_id())
                      .c_str());
        }
    }

    if (g_upgrades_log == 1)
    {
        Msg("- Upgrade <%s> of inventory item [%s] (id = %d) can`t be installed. Error = %d", upgrade_id.c_str(),
            item.m_section_id.c_str(), item.object_id(), res);
    }
    return false;
}

void Manager::init_install(CInventoryItem& item)
{
    if (!get_root(item.m_section_id))
        return;

#ifdef DEBUG
    test_all_upgrades(item);
#endif // DEBUG

    if (pSettings->line_exist(item.m_section_id, "installed_upgrades"))
    {
        // installed_upgrades by default
        LPCSTR installed_upgrades_str = pSettings->r_string(item.m_section_id, "installed_upgrades");
        if (installed_upgrades_str)
        {
            u32 const buffer_size = (xr_strlen(installed_upgrades_str) + 1) * sizeof(char);
            PSTR temp = (PSTR)xr_alloca(buffer_size);

            for (int n = _GetItemCount(installed_upgrades_str), i = 0; i < n; ++i)
            {
                upgrade_install(item, _GetItem(installed_upgrades_str, i, temp, buffer_size), true);
            }
        }
    } // if exist
}

LPCSTR Manager::get_item_scheme(CInventoryItem& item)
{
    Root* root_p = get_root(item.m_section_id);
    if (!root_p)
        return NULL;
    return root_p->scheme();
}

LPCSTR Manager::get_upgrade_by_index(CInventoryItem& item, Ivector2 const& index)
{
    Upgrade* upgrade = NULL;

    Root* root_p = get_root(item.m_section_id);
    if (root_p)
    {
        upgrade = root_p->get_upgrade_by_index(index);
        if (upgrade)
        {
            return upgrade->id_str();
        }
    }

    VERIFY2(upgrade, make_string("! Upgrade with index <%d,%d> in inventory item [%s] does not exist!", index.x,
                         index.y, item.m_section_id.c_str()));
    return NULL;
}

// -------------------------------------------------------------------------------------------------

bool Manager::compute_range(LPCSTR parameter, float& low, float& high)
{
    low = flt_max;
    high = flt_min;

    Roots_type::iterator ib = m_roots.begin();
    Roots_type::iterator ie = m_roots.end();
    for (; ib != ie; ++ib)
    {
        compute_range_section(((*ib).second)->id_str(), parameter, low, high);
    }

    Upgrades_type::iterator uib = m_upgrades.begin();
    Upgrades_type::iterator uie = m_upgrades.end();
    for (; uib != uie; ++uib)
    {
        compute_range_section(((*uib).second)->section(), parameter, low, high);
    }

    return (low != flt_max) && (high != flt_min);
}

void Manager::compute_range_section(LPCSTR section, LPCSTR parameter, float& low, float& high)
{
    if (!pSettings->line_exist(section, parameter) || !*pSettings->r_string(section, parameter))
    {
        return;
    }

    float cur = pSettings->r_float(section, parameter);
    if (cur < low)
    {
        low = cur;
    }
    if (cur > high)
    {
        high = cur;
    }
}

// -----------------------------------------------------------------------------

void Manager::highlight_hierarchy(CInventoryItem& item, shared_str const& upgrade_id)
{
    Root* root_p = get_root(item.m_section_id);
    if (root_p)
    {
        root_p->highlight_hierarchy(upgrade_id);
    }
}

void Manager::reset_highlight(CInventoryItem& item)
{
    Root* root_p = get_root(item.m_section_id);
    if (root_p)
    {
        root_p->reset_highlight();
        return;
    }
}

} // namespace upgrade
} // namespace inventory
