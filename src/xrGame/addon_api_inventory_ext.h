#pragma once

// Plugin API, groups "items" (what an inventory holds and does: find, list, move, drop, slots of the actor, condition,
// magazine and upgrades of items) and "object_ext" (zones, physics impulse, model of an object):
// addon_api_inventory_ext.cpp. Docs: wiki/doc/plugins/api/items.md, wiki/doc/plugins/api/object_ext.md

#include "xrAddonHost/include/gwp/gwp_api.h"

namespace gw::addons::items
{
void FillEngineApi(GwpEngineApi& api);
} // namespace gw::addons::items
