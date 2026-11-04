////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper_script.cpp
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "saved_game_wrapper.h"
#include "ai_space.h"
#include "xr_time.h"
#include "addon_event_bus.h"
#include "level_pack.h"

void CSavedGameWrapper::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CSavedGameWrapper>("CSavedGameWrapper")
            .def(constructor<pcstr>())
            .def("game_time", +[](const CSavedGameWrapper* self)
            {
                return (xrTime(self->game_time()));
            })
            .def("level_id", &CSavedGameWrapper::level_id)
            .def("level_name", &CSavedGameWrapper::level_name)
            .def("actor_health", &CSavedGameWrapper::actor_health)
            // data(key): a persistent value of the data bus in the save (gw_game_state: "gw/game/<name>") -
            // string, number or boolean; nil when the save has no such key (or no data at all, or it is invalid)
            .def("data", +[](const CSavedGameWrapper* self, lua_State* L, pcstr key) -> luabind::object
            {
                // a valid object for nil too: luabind::object() (no state) cannot be pushed back to Lua
                if (const GwpValue* value = self->data(key))
                    gw::addons::events::PushLuaValue(L, *value);
                else
                    lua_pushnil(L);
                luabind::object result(luabind::from_stack(L, -1));
                lua_pop(L, 1);
                return result;
            }),

        // is_level_pack_installed(id): the load dialog marks and refuses saves of a pack that is not installed
        def("is_level_pack_installed", &gw::IsLevelPackInstalled),

        def("valid_saved_game", (bool (*)(pcstr))(&CSavedGameWrapper::valid_saved_game))
    ];

    // Location packs (plans/addons_api_support/10-location-packs.md)
    module(luaState, "level_pack")
    [
        // list(): { {id, version, save_compat, name, description, preview, builtin, active}, ... } - the built-in
        // pack first; name / description are string table ids
        def("list", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            const gw::LevelPackInfo& active = gw::ActiveLevelPack();
            int index = 0;
            for (const gw::LevelPackInfo& pack : gw::LevelPacks())
            {
                luabind::object item = luabind::newtable(L);
                item["id"] = pack.id.c_str();
                item["version"] = pack.version.c_str();
                item["save_compat"] = pack.save_compat;
                item["name"] = pack.name.c_str();
                item["description"] = pack.description.c_str();
                item["preview"] = pack.preview.c_str();
                item["builtin"] = pack.builtin();
                item["active"] = &pack == &active;
                result[++index] = item;
            }
            return result;
        }),
        def("active", +[]() -> pcstr { return gw::ActiveLevelPack().id.c_str(); }),
        // switch(id, action): at the next frame of the main menu; then ui_main_menu.on_level_pack_applied(id, action,
        // ok) in the new Lua state. false - an unknown pack or a game is on
        def("switch", &gw::RequestLevelPackSwitch),
        def("switch_pending", &gw::LevelPackSwitchPending),
        // check_save(save): true - load it now; false - not now (another pack: the switch and the load follow; a pack
        // not installed or incompatible: ui_load_dialog.on_level_pack_missing / on_level_pack_incompatible)
        def("check_save", &gw::CanLoadSaveLevelPack)
    ];
}
