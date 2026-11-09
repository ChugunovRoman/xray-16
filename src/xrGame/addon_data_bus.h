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
#include "addon_event_bus.h"

class IReader;
class IWriter;

namespace gw::addons::data
{
void FillEngineApi(GwpEngineApi& api);

// A persistent value the engine owns (gw/game/level_pack*, gw/slots/*: level_pack.cpp WriteSaveContentKeys), set
// with no owner check; GWP_T_NIL erases. As data_bus.set of Lua: data_on_changed on a real change
void SetPersistentFromEngine(pcstr key, const GwpValue& value);
void ResetPersistent();          // new game: drop the persistent values of the previous one
void WriteSave(IWriter& stream); // own chunk of the ALife save stream
void ReadSave(IReader& stream);  // replaces the persistent values; a save without the chunk gives none

// Persistent values of a save without loading it (CSavedGameWrapper: the load dialog, the load command): the chunk
// of ReadSave in the uncompressed ALife save stream `stream`, read the same way, into `out` (cleared first); the
// store is not touched. A save without the chunk gives none. open_chunk moves the position of `stream`: seek(0)
// before reading it further.
using SaveValues = xr_map<xr_string, events::OwnedValue>;
void ReadSaveValues(IReader& stream, SaveValues& out);

void Shutdown();
void PrintList(pcstr prefix); // console command "data_list [prefix]"
} // namespace gw::addons::data
