#pragma once

// Data bus: shared key-value store of Lua scripts and native plugins.
//  - Keys are "<owner>/<name>". A plugin writes only keys of its own addon ("<addon id>/..."); Lua writes any key
//    (a C function called from Lua cannot tell which script or addon called it, so the owner is not checked there).
//  - Values are GwpValue of any type but GWP_T_LUA_REF (bool, numbers, strings, bytes, vec3, object ids, arrays of
//    them); they are copied deeply into the store (events::OwnedValue).
//  - Persistent values go into the game save (own chunk of the ALife save stream); a new game clears them,
//    a load replaces them. Other values live until the game exits.
//  - A real change of a value sends the event data_on_changed(key, value).
// Lua: global table data_bus. Plugins: data_* functions of GwpEngineApi (xrAddonHost/include/gwp/gwp_api.h).
// Main thread only. Docs: wiki/doc/plugins/api/data.md; plan: plans/lua_to_cpp/04-plugin-api-events-and-data.md

#include "xrAddonHost/include/gwp/gwp_api.h"

class IReader;
class IWriter;

namespace gw::addons::data
{
void FillEngineApi(GwpEngineApi& api);

void ResetPersistent();          // new game: drop the persistent values of the previous one
void WriteSave(IWriter& stream); // own chunk of the ALife save stream
void ReadSave(IReader& stream);  // replaces the persistent values; a save without the chunk gives none

void Shutdown();
void PrintList(pcstr prefix); // console command "data_list [prefix]"
} // namespace gw::addons::data
