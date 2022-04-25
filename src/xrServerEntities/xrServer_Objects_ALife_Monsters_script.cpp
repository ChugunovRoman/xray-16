////////////////////////////////////////////////////////////////////////////
//	Module 		: xrServer_Objects_ALife_Monsters_script.cpp
//	Created 	: 19.09.2002
//  Modified 	: 04.06.2003
//	Author		: Dmitriy Iassenev
//	Description : Server monsters for ALife simulator, script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer_script_macroses.h"
#include "xrScriptEngine/ScriptExporter.hpp"

using namespace luabind;

#ifdef XRGAME_EXPORTS
LPCSTR profile_name_script(CSE_ALifeTraderAbstract* ta) { return *ta->character_profile(); }
void profile_name_set_script(CSE_ALifeTraderAbstract* ta, const pcstr str) { ta->set_character_profile(str); }
pcstr character_name_script(CSE_ALifeTraderAbstract* ta) { return ta->m_character_name.c_str(); }
void set_character_name_script(CSE_ALifeTraderAbstract* ta, const pcstr str) { ta->m_character_name = str; }
pcstr icon_name_script(CSE_ALifeTraderAbstract* ta)
{
	ta->specific_character();
	if (!ta->m_icon_name.size())
	{
		CSpecificCharacter selected_char;
		selected_char.Load(ta->m_SpecificCharacter);
		ta->m_icon_name = selected_char.IconName();
	}
	return *ta->m_icon_name;
}

SCRIPT_EXPORT(CSE_ALifeTraderAbstract, (), {
    module(luaState)[class_<CSE_ALifeTraderAbstract>("cse_alife_trader_abstract")
        .def("community", &CSE_ALifeTraderAbstract::CommunityName)
        .def("profile_name", &profile_name_script)
        .def("set_profile_name", &profile_name_set_script)
        .def("character_name", &character_name_script)
        .def("set_character_name", &set_character_name_script)
        .def("rank", &CSE_ALifeTraderAbstract::Rank)
        .def("set_rank", &CSE_ALifeTraderAbstract::SetRank)
        .def("reputation", &CSE_ALifeTraderAbstract::Reputation)
        .def("character_icon", &icon_name_script)
    ];
});
#else

LPCSTR profile_name_script(CSE_ALifeTraderAbstract* ta) { return *ta->character_profile(); }
SCRIPT_EXPORT(CSE_ALifeTraderAbstract, (), {
    module(luaState)[class_<CSE_ALifeTraderAbstract>("cse_alife_trader_abstract")
        .def("profile_name", &profile_name_script)
    ];
});
#endif

SCRIPT_EXPORT(CSE_ALifeTrader, (CSE_ALifeDynamicObjectVisual, CSE_ALifeTraderAbstract), {
    module(luaState)[luabind_class_dynamic_alife2(
        CSE_ALifeTrader, "cse_alife_trader", CSE_ALifeDynamicObjectVisual, CSE_ALifeTraderAbstract)];
});

SCRIPT_EXPORT(CSE_ALifeCustomZone, (CSE_ALifeDynamicObject, CSE_Shape), {
    module(luaState)[luabind_class_dynamic_alife2(
        CSE_ALifeCustomZone, "cse_custom_zone", CSE_ALifeDynamicObject, CSE_Shape)];
});

SCRIPT_EXPORT(CSE_ALifeAnomalousZone, (CSE_ALifeCustomZone), {
    module(luaState)[luabind_class_dynamic_alife1(CSE_ALifeAnomalousZone, "cse_anomalous_zone", CSE_ALifeCustomZone)];
});

SCRIPT_EXPORT(CSE_ALifeMonsterRat, (CSE_ALifeMonsterAbstract, CSE_ALifeInventoryItem), {
    module(luaState)[luabind_class_monster2(
        CSE_ALifeMonsterRat, "cse_alife_monster_rat", CSE_ALifeMonsterAbstract, CSE_ALifeInventoryItem)];
});
