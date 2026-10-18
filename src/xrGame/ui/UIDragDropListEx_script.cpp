#include "StdAfx.h"
#include "xrAICore/pch.hpp"
#include "UIDragDropListEx.h"
#include "UICellItem.h"
#include "UICellItemFactory.h"
#include "UICellCustomItems.h"
#include "Weapon.h"
#include "xrScriptEngine/ScriptExporter.hpp"

#include "xrCore/Threading/ParallelFor.hpp"
#include <mutex>

// clang-format off
void CUIDragDropListEx::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUIDragDropListEx, CUIWindow>("CUIDragDropListEx")
            .def(constructor<>())
            .def("AddItem", +[](CUIDragDropListEx* self, pcstr item_name)
            {
                CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(item_name);
                self->SetItem(item);
            })
            .def("AddItems", +[](CUIDragDropListEx* self, luabind::object item_tbl)
            {
                std::mutex push_items_mtx;
                xr_vector<CUIInventoryCellItem*> items;
                xr_vector<shared_str> strings;

                for (luabind::iterator I(item_tbl), E; I != E; ++I)
                {
                    luabind::object value = *I;
                    if (luabind::type(value) != LUA_TSTRING)
                        continue;

                    pcstr section_name = luabind::object_cast<pcstr>(value);
                    shared_str m_section_id;

                    if (strstr(section_name, "|"))
                    {
                        int len = (int)strcspn(section_name, "|");
                        char* result = new char[len + 1];
                        strncpy(result, section_name, len);
                        result[len] = '\0';
                        m_section_id = result;
                    }
                    else
                        m_section_id = section_name;

                    if (pSettings->section_exist(m_section_id.c_str()))
                        strings.push_back(section_name);
                }

                for (shared_str itm : strings)
                {
                    CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(itm.c_str());
                    self->SetItem(item);
                }
            })
            .def("AddWeaponAttachments", +[](CUIDragDropListEx* self, pcstr weapon_section_id)
            {
                if (!pSettings->section_exist(weapon_section_id))
                {
                    GEnv.ScriptEngine->script_log(
                        LuaMessageType::Error, "weapon section does not exist: %s", weapon_section_id);
                    return;
                }
                if (
                    pSettings->line_exist(weapon_section_id, "silencer_status") &&
                    pSettings->r_u8(weapon_section_id, "silencer_status") == 2 &&
                    pSettings->line_exist(weapon_section_id, "silencer_name"))
                {
                    pcstr silencer_name = pSettings->r_string(weapon_section_id, "silencer_name");
                    CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(silencer_name);
                    self->SetItem(item);
                }
                if (
                    pSettings->line_exist(weapon_section_id, "grenade_launcher_status") &&
                    pSettings->r_u8(weapon_section_id, "grenade_launcher_status") == 2 &&
                    pSettings->line_exist(weapon_section_id, "grenade_launcher_name")
                )
                {
                    pcstr gl_name = pSettings->r_string(weapon_section_id, "grenade_launcher_name");
                    CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(gl_name);
                    self->SetItem(item);
                }
                bool bUseAttachSystem = pSettings->line_exist(weapon_section_id, "use_attachment_system") && pSettings->r_bool(weapon_section_id, "use_attachment_system");

                if (!bUseAttachSystem)
                {
                    if (!pSettings->line_exist(weapon_section_id, "scopes"))
                        return;
                        
                    pcstr scopesStr = pSettings->r_string(weapon_section_id, "scopes");
                    if (xr_strcmp(scopesStr, "none") == 0 || xr_strcmp(scopesStr, "wpn_scope_name") == 0)
                        return;
                    string128 _scopeItem;
                    int count = _GetItemCount(scopesStr);
                    for (int it = 0; it < count; ++it)
                    {
                        _GetItem(scopesStr, it, _scopeItem);

                        if (!pSettings->section_exist(_scopeItem))
                            continue;

                        CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(_scopeItem);
                        self->SetItem(item);
                    }
                    return;
                }

                // Root slots exactly as CWeapon::LoadAddonSlosts reads them: numbered and named slots,
                // lines from parent_section, <slot>_addons white lists
                struct root_slot
                {
                    u16 type;
                    xr_vector<shared_str> allowed;
                };
                xr_vector<root_slot> root_slots;

                const shared_str slot_sect = CWeapon::SectionForAddonSlotLines(weapon_section_id);
                xr_vector<std::pair<shared_str, shared_str>> slot_lines;
                CWeapon::GetAddonSlotLines(slot_sect.c_str(), slot_lines);

                for (const auto& [line_name, slot_key] : slot_lines)
                {
                    pcstr slot_str = pSettings->r_string(slot_sect.c_str(), line_name.c_str());
                    if (!slot_str)
                        continue;

                    // The same rule as LoadAddonSlosts: a line with less than 7 fields (e.g. "none") makes no slot
                    root_slot slot;
                    Fvector pos, rot;
                    if (sscanf(slot_str, "%hu,%f,%f,%f,%f,%f,%f", &slot.type, &pos.x, &pos.y, &pos.z, &rot.x, &rot.y, &rot.z) < 7)
                        continue;

                    string64 prop_name;
                    xr_sprintf(prop_name, "%s_addons", slot_key.c_str());
                    pcstr list_section = pSettings->line_exist(weapon_section_id, prop_name) ? weapon_section_id :
                        pSettings->line_exist(slot_sect.c_str(), prop_name) ? slot_sect.c_str() : nullptr;
                    pcstr list_str = list_section ? pSettings->r_string(list_section, prop_name) : nullptr;
                    if (list_str)
                    {
                        string256 item_name;
                        const int count = _GetItemCount(list_str);
                        for (int i = 0; i < count; ++i)
                        {
                            _GetItem(list_str, i, item_name);
                            if (item_name[0])
                                slot.allowed.push_back(item_name);
                        }
                    }

                    root_slots.push_back(slot);
                }

                const xr_vector<pcstr> sections = Dbg.GetSections(ESectionTypeName::scopes);
                const u16 no_slot_type = u16(-1);

                auto fits_root_slot = [&](pcstr name, u16 addon_slot_type)
                {
                    for (const root_slot& slot : root_slots)
                    {
                        if (slot.type != addon_slot_type)
                            continue;
                        if (slot.allowed.empty() ||
                            std::find(slot.allowed.begin(), slot.allowed.end(), shared_str(name)) != slot.allowed.end())
                            return true;
                    }
                    return false;
                };

                // Nested slots come from slot_N bones of the addon model, which a section alone cannot show;
                // the types an addon can offer (provided_slot_type and explicit slot_N types) stand in for them.
                // An addon is listed when it fits a root slot or a slot type offered by an already listed addon.
                xr_vector<bool> listed(sections.size(), false);
                xr_set<u16> nested_slot_types;
                bool has_new = true;
                while (has_new)
                {
                    has_new = false;
                    for (size_t i = 0; i < sections.size(); ++i)
                    {
                        if (listed[i])
                            continue;

                        pcstr name = sections[i];
                        const u16 addon_slot_type = pSettings->read_if_exists<u16>(name, "slot_type", no_slot_type);
                        if (addon_slot_type == no_slot_type)
                            continue;
                        if (!fits_root_slot(name, addon_slot_type) && !nested_slot_types.count(addon_slot_type))
                            continue;

                        listed[i] = true;
                        has_new = true;

                        const u16 provided = pSettings->read_if_exists<u16>(name, "provided_slot_type", no_slot_type);
                        if (provided != no_slot_type)
                            nested_slot_types.insert(provided);

                        for (int slot_idx = 1; slot_idx <= 16; ++slot_idx)
                        {
                            string32 slot_key;
                            xr_sprintf(slot_key, "slot_%d", slot_idx);
                            if (pSettings->line_exist(name, slot_key))
                                nested_slot_types.insert(pSettings->r_u16(name, slot_key));
                        }
                    }
                }

                for (size_t i = 0; i < sections.size(); ++i)
                {
                    if (!listed[i])
                        continue;

                    CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(sections[i]);
                    self->SetItem(item);
                }
            })
            .def("SetDraggingEnabled", +[](CUIDragDropListEx* self, bool enabled)
            {
                self->dragging_enabled = enabled;
            })
            .def("ItemCount", +[](CUIDragDropListEx* self) -> u32
            {
                return self->ItemsCount();
            })
            .def("RemoveItem", +[](CUIDragDropListEx* self, pcstr item_name)
            {
                CUICellItem* item = self->FindByKey(item_name);
                if (item)
                    self->RemoveItem(item, false);
            })
            .def("SelectOne", +[](CUIDragDropListEx* self, pcstr item_name)
            {
                CUICellItem* item = self->FindByKey(item_name);
                self->ResetAllSelected();

                if (item)
                    item->m_select_armament = !item->m_select_armament;
            })
            .def("SelectMultiple", +[](CUIDragDropListEx* self, pcstr item_name)
            {
                CUICellItem* item = self->FindByKey(item_name);
                if (item)
                    item->m_select_armament = !item->m_select_armament;
            })
            .def("ResetAllSelected", +[](CUIDragDropListEx* self)
            {
                self->ResetAllSelected();
            })
            .def("ClearList", +[](CUIDragDropListEx* self)
            {
                self->ClearAll(true);
            })
            .def("GetSelectedItem", +[](CUIDragDropListEx* self) -> pcstr
            {
                auto* selected_item = self->GetSelectedItem();
                if (selected_item && selected_item->data_is_string)
                    return selected_item->m_section_id.c_str();
              
                return nullptr;
            })
            .def("GetItem", +[](CUIDragDropListEx* self, pcstr item_name) -> CUICellItem*
            {
                return self->FindByKey(item_name);
            })
            .def("GetItems", +[](CUIDragDropListEx* self, lua_State* L, bool pickSelected = false) -> luabind::object
            {
                luabind::object result = luabind::newtable(L);
                std::size_t index = 1;

                self->IterItems([&](CUICellItem* item)
                {
                    if (pickSelected && !item->m_select_armament)
                        return;

                    result[index++] = item->m_section_id.c_str();
                    if (item->ChildsCount() > 0)
                    {
                        for (std::size_t i = 0; i < item->ChildsCount(); ++i)
                        {
                            result[index++] = item->m_section_id.c_str();
                        }
                    }
                });

                return result;
            })
            .def("GetItemsAttchName", +[](CUIDragDropListEx* self, lua_State* L) -> luabind::object
            {
                luabind::object result = luabind::newtable(L);
                std::size_t index = 1;

                self->IterItems([&](CUICellItem* item)
                {
                    result[index++] = item->m_section_attachs_id.c_str();
                    if (item->ChildsCount() > 0)
                    {
                        for (std::size_t i = 0; i < item->ChildsCount(); ++i)
                        {
                            result[index++] = item->m_section_attachs_id.c_str();
                        }
                    }
                });

                return result;
            })
            // Callbacks
            .def("AddItemDropCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_drop_lua.set(lua_function, context);
            })
            .def("AddItemStartDragCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_start_drag_lua.set(lua_function, context);
            })
            .def("AddItemDbClickCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_db_click_lua.set(lua_function, context);
            })
            .def("AddItemSelectedCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_selected_lua.set(lua_function, context);
            })
            .def("AddItemLbuttonClickCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_lbutton_click_lua.set(lua_function, context);
            })
            .def("AddItemRbuttonClickCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_rbutton_click_lua.set(lua_function, context);
            })
            .def("AddItemFocusReceivedCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_focus_received_lua.set(lua_function, context);
            })
            .def("AddItemFocusLostCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_focus_lost_lua.set(lua_function, context);
            })
            .def("AddItemFocusedUpdateCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_item_focused_update_lua.set(lua_function, context);
            })
            .def("AddDragEventCallback", +[](CUIDragDropListEx* self, const luabind::functor<void> &lua_function, const luabind::object& context)
            {
                self->m_f_drag_event_lua.set(lua_function, context);
            })
    ];
}
// clang-format on
