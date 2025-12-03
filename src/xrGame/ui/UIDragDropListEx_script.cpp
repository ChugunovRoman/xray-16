#include "StdAfx.h"
#include "xrAICore/pch.hpp"
#include "UIDragDropListEx.h"
#include "UICellItem.h"
#include "UICellItemFactory.h"
#include "UICellCustomItems.h"
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

                    if (pSettings->section_exist(section_name))
                        strings.push_back(section_name);
                }

                xr_parallel_for(TaskRange<u32>(0, strings.size()), [&](const TaskRange<u32>& range)
                {
                    xr_vector<CUIInventoryCellItem*> tmp;

                    for (u32 i = range.begin(); i != range.end(); ++i)
                    {
                        shared_str itm = strings.at(i);
                        CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(itm.c_str());

                        tmp.push_back(item);
                    }
                    
                    std::lock_guard<std::mutex> lock(push_items_mtx);
                    items.insert(items.end(), tmp.begin(), tmp.end());
                });

                for (auto& item : items)
                    self->SetItem(item);
            })
            .def("AddWeaponAttachments", +[](CUIDragDropListEx* self, pcstr weapon_section_id)
            {
                if (!pSettings->section_exist(weapon_section_id))
                {
                    GEnv.ScriptEngine->script_log(
                        LuaMessageType::Error, "weapon section does not exist: %s", weapon_section_id);
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

                auto get_slot_type = [&](pcstr slot_name) -> u32
                {
                    if (!pSettings->line_exist(weapon_section_id, slot_name))
                        return (u32)-1;

                    pcstr slot_str = pSettings->r_string(weapon_section_id, slot_name);
                    string128 _slot_type;
                    _GetItem(slot_str, 0, _slot_type);

                    return (u32)atoi(_slot_type);
                };

                u32 slot_1_type = get_slot_type("addon_slot_1_offset");
                u32 slot_2_type = get_slot_type("addon_slot_2_offset");
                u32 slot_3_type = get_slot_type("addon_slot_3_offset");
                u32 slot_4_type = get_slot_type("addon_slot_4_offset");
                u32 slot_5_type = get_slot_type("addon_slot_5_offset");
                u32 slot_6_type = get_slot_type("addon_slot_6_offset");

                for (const auto& name : Dbg.GetSections(ESectionTypeName::scopes))
                {
                    u32 addon_slot_type = pSettings->read_if_exists<u32>(name, "slot_type", (u32)-1);
                    u32 addon_provided_slot_type = pSettings->read_if_exists<u32>(name, "provided_slot_type", (u32)-1);

                    if (
                        addon_slot_type == slot_1_type || (addon_slot_type == 0 && addon_provided_slot_type == 0) ||
                        addon_slot_type == slot_2_type || (addon_slot_type == 0 && addon_provided_slot_type == 0) ||
                        addon_slot_type == slot_3_type || (addon_slot_type == 0 && addon_provided_slot_type == 0) ||
                        addon_slot_type == slot_4_type || (addon_slot_type == 0 && addon_provided_slot_type == 0) ||
                        addon_slot_type == slot_5_type || (addon_slot_type == 0 && addon_provided_slot_type == 0) ||
                        addon_slot_type == slot_6_type || (addon_slot_type == 0 && addon_provided_slot_type == 0)
                    )
                    {
                        CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(name);
                        self->SetItem(item);
                    }
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
