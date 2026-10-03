#pragma once

// Plugin API, groups feedback and effects (addon_api_feedback.cpp): what a plugin shows and plays to the player.
//  - feedback: PDA news, HUD messages, translation of string ids, PDA map spots. Text is always a string id: the
//    engine translates it (the PDA and the map translate when they show it).
//  - effects: sounds and particle effects behind opaque handles with a generation (the engine owns the ref_sound and
//    the particles), pp and camera effectors on the actor in an id range per plugin.
// What a plugin started (sounds, particle effects, effectors) is released when it stops (unload, crash: StopPlugin),
// when a game starts or is loaded (OnGameStart) and when the level goes (OnFrame). PDA spots are game state and stay.
// Docs: wiki/doc/plugins/api/feedback.md, wiki/doc/plugins/api/effects.md;
// plan: plans/lua_to_cpp/26-plugin-api-pre-release.md (section 5).

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::feedback
{
void FillEngineApi(GwpEngineApi& api);

// Start of a frame (gw::addons::OnFrame): sounds and effects that ended or belong to a gone level are released, the
// effects attached to bones follow their objects, the effector records of a gone actor are dropped.
void OnFrame();
// A game starts or a save is loaded (gw::addons::OnGameStart): everything of every plugin is released.
void OnGameStart();
// The plugin stops (StopPlugin: unload, crash, reload): its sounds stop, its effects and effectors are removed.
// Safe to call any number of times.
void RemovePluginResources(const GwpPlugin* plugin);
void Shutdown();
} // namespace gw::addons::feedback
