#include "StdAfx.h"
#include "xrAICore/pch.hpp"
#include "UIDragDropListEx.h"
#include "UICellItem.h"
#include "UICellItemFactory.h"
#include "UICellCustomItems.h"
#include "xrScriptEngine/ScriptExporter.hpp"

// clang-format off
SCRIPT_EXPORT(CUIDragDropListEx, (CUIWindow),
{
    using namespace luabind;
    using namespace luabind::policy;

    module(luaState)
    [
        class_<CUIDragDropListEx, CUIWindow>("CUIDragDropListEx")
            .def(constructor<>())
            .def("AddItem", +[](CUIDragDropListEx* self, pcstr item_name)
            {
                CUIInventoryCellItem* item = xr_new<CUIInventoryCellItem>(item_name);
                self->SetItem(item);
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
                    item->m_select_armament = true;
            })
            .def("SelectMultiple", +[](CUIDragDropListEx* self, pcstr item_name)
            {
                CUICellItem* item = self->FindByKey(item_name);
                if (item)
                    item->m_select_armament = true;
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
            .def("GetItems", +[](CUIDragDropListEx* self, lua_State* L) -> luabind::object
            {
                luabind::object result = luabind::newtable(L);
                std::size_t index = 1;

                self->IterItems([&](CUICellItem* item)
                {
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
});
// clang-format on
