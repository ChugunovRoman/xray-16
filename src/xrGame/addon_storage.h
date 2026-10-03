#pragma once

// Plugin API, group "storage": db.storage[id] of the Lua scripts, mirrored for plugins. Wave W0 (W0.4 of the plan).
//  - The Lua entry stays a Lua table: the scripts read and write it as before. npc_storage_bridge.script turns
//    every entry into a proxy whose writes of a fixed list of scalar fields (kKeys below) are copied here, so a
//    plugin reads them without crossing into Lua; a plugin write goes into the copy and into the Lua table.
//  - Every entry has a generation: the engine reuses object ids, and a handle {id, generation} kept across frames
//    tells a stale entry from a new one instead of reading the fields of another object.
//  - Main thread only, like the rest of the Plugin API; no snapshot for workers yet.
// Lua side: global table npc_storage (acquire / release / set / keys / handle), used by npc_storage_bridge.script.
// Docs: wiki/doc/plugins/api/storage.md; plan: plans/lua_to_cpp/08-wave-w0-native-npc-foundation.md (W0.4)

#include "xrAddonHost/include/gwp/gwp_api.h"

class CGameObject;
struct GwpPlugin;

namespace gw::addons::storage
{
void FillEngineApi(GwpEngineApi& api);

// net_Destroy, after the plugin binders and before the Lua binder: the entry of the object ends (db.del_obj does the
// same from Lua; whichever comes first wins, the other is a no-op).
void OnObjectDestroy(const CGameObject* object);

void Shutdown();
void PrintList(pcstr args); // console command "storage_list [id]"
} // namespace gw::addons::storage
