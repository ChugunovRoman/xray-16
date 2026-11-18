#pragma once

// The Lua side of the ImGui tools of the addons (plans/addons_api_support/20-imgui-addon-tools.md, 4.1 and 4.2):
//  - addon_imgui_lua.cpp: _gw_internal.imgui_tool_register / unregister / set_open / is_open / list / editor_state /
//    available (the facade is addons.register_imgui_tool & co., gw_imgui_tools.script), the callbacks of a Lua tool
//    (draw / on_open / on_close under lua_pcall), the table `tool`, tool.defer;
//  - addon_imgui_lua_widgets.cpp: the table `ui` - the widgets, through addon_imgui_core.
// Internal to xrGame.

struct lua_State;

namespace gw::addons::imgui_lua
{
// imgui_tools::OnFrame (CGamePersistent::OnFrame, the main thread, outside of any ImGui window): the functions the tools
// gave to tool.defer since the previous frame, one protected call each
void RunDeferred();
// addon_reload <owner>: its deferred functions go (their modules are unloaded)
void DropDeferred(pcstr owner);

// addon_imgui_lua_widgets.cpp: the functions of `ui` into the table on the top of the stack (script_register, once per
// Lua state). Stage 3 adds the widgets there and the constant tables (ui.WindowFlags, ui.TableFlags, ...)
void RegisterUiFunctions(lua_State* L);
} // namespace gw::addons::imgui_lua
