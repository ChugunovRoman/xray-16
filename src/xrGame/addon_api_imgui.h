#pragma once

// Plugin API, group "imgui" (plans/addons_api_support/20-imgui-addon-tools.md, 4.3): the ImGui tools of the plugins.
//  - imgui_tool_register / unregister / set_open / is_open: a plugin tool is an entry of the registry of
//    addon_imgui_tools.h (kind Plugin, key - the GwpPlugin handle); its draw / on_open / on_close run under the guards
//    of the host (events::CallPluginGuarded): a crash stops the plugin (OnPluginCrashed), the window goes, the other
//    tools are drawn.
//  - GwpImGuiApi: one static table of the widgets of ImGui for every draw, over the common layer addon_imgui_core.h
//    (only inside draw, the shadow stack, safe formats, limits, the flags of GW translated to ImGui). Outside of draw a
//    function does nothing, returns 0 and warns once per plugin module; an error of a pair or of an argument does
//    nothing and warns once per tool.
// Docs: wiki/doc/plugins/api/imgui.md

#include "xrAddonHost/include/gwp/gwp_api.h"

namespace gw::addons::imgui
{
void FillEngineApi(GwpEngineApi& api);
// StopPlugin (addon_host.cpp): the tools of the plugin go quietly (no on_close: the plugin may have crashed; the open
// flag stays for the next registration). Safe inside its own draw: the entries are only marked then.
void RemovePluginTools(const GwpPlugin* plugin);
} // namespace gw::addons::imgui
