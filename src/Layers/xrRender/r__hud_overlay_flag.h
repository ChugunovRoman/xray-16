#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
struct SPass;

/// Set true only during phase_hud_overlay (HUD forward overlay pass). Used by backend to set hud_overlay_state in shaders.
extern bool g_rendering_hud_overlay;

/// Setup m_shadow, Ldynamic_* etc. for HUD overlay. Call after set_Element. Pass is the current pass (e.g. se->passes[0]).
void setup_hud_overlay_constants(CBackend& cmd_list, SPass* pass);
}
