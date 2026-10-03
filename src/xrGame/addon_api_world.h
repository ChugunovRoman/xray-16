#pragma once

// Plugin API: the world (group world, addon_api_world.cpp): the game graph and the levels, moving online objects,
// the direction of the actor, explosions, weather, writing the game time.
// Docs: wiki/doc/plugins/api/world.md

#include "xrAddonHost/include/gwp/gwp_api.h"

namespace gw::addons
{
void FillWorldApi(GwpEngineApi& api);
} // namespace gw::addons
