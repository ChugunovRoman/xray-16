#pragma once

class CUIWindow;

// RS11 (plans/addon_runtime_smarts, the markers of the smart mode): simple shapes drawn in the world from Lua - lines,
// circles, polylines, boxes, spheres, arrows - for the edit modes of addons (the ring of a smart, the points of its
// jobs, their directions) instead of blinking particles. Lua: level.draw_line / draw_circle / draw_polyline /
// draw_box / draw_sphere / draw_arrow / draw_remove / draw_remove_prefix / draw_clear / draw_count (level_script.cpp).
//
// Retained shapes: a shape is set once by its id (a string; an addon prefixes its ids, "gwd_bb.ring") and stays until
// it is set again, removed or the level is unloaded - Lua does not send it every frame. A shape is turned into lines
// when it is set (the thread of Lua), the render (the main thread, CLevel::OnRender before the UI) takes the lines of
// all shapes under a lock, rebuilds its batches only after a change and draws them through IDebugRender (the
// "editor\wire" shader of the debug renderer, the same as the bone points of the HUD tuner; DX11 and GL; Release too).
// Depth tested (hidden by the walls and the ground in front of them, the scene does not see them otherwise), opaque
// (the alpha of the color is ignored), 1 pixel wide. A frame with the Game thread beside the render: the render reads
// only its own batches outside the lock.
namespace gw::world_draw
{
constexpr u32 kMaxShapes = 1024; // shapes at once (all the addons)
constexpr u32 kMaxPoints = 512; // points of one polyline
constexpr u32 kMaxSegments = 128; // segments of a circle / of a circle of a sphere
constexpr u32 kMaxIdLength = 128; // characters of an id

// Set (add or replace) the shape `id`. color - 0xAARRGGBB (GetARGB of Lua; the alpha is ignored). false (one log
// line): no level, a bad id, a value that is not a number, the limit of shapes / points.
bool Line(pcstr id, const Fvector& a, const Fvector& b, u32 color);
// A circle in the horizontal plane (a ring on the ground: the center at its height); segments 0 - by the radius
bool Circle(pcstr id, const Fvector& center, float radius, u32 color, u32 segments);
// A polyline through the points (closed - the last one back to the first)
bool Polyline(pcstr id, const xr_vector<Fvector>& points, bool closed, u32 color);
// A box: the center, the half sizes along its axes, turned by hpb (radians: x - heading, y - pitch, z - bank, as
// Fmatrix::setHPB and level.preview_model_show)
bool Box(pcstr id, const Fvector& center, const Fvector& half, const Fvector& hpb, u32 color);
// A sphere: three circles (horizontal and two vertical ones)
bool Sphere(pcstr id, const Fvector& center, float radius, u32 color);
// An arrow from `from` to `to`: the line and a head of 4 short lines at `to` (head - its length, 0 - a quarter of the
// arrow, at most 0.5 m)
bool Arrow(pcstr id, const Fvector& from, const Fvector& to, u32 color, float head);

// Remove the shape `id`; false - there was none
bool Remove(pcstr id);
// Remove all the shapes whose id starts with prefix (the shapes of one addon); how many were removed
u32 RemovePrefix(pcstr prefix);
// Remove all the shapes (of every addon)
void RemoveAll();
// How many shapes there are
u32 Count();

// RS12: anchors of UI windows to points of the world. The window `wnd` (a direct child of a dialog drawn over the HUD:
// get_hud():AddDialogToRender) is put every frame, right before the UI is drawn, with its top left corner at the
// point `pos` on the screen + (dx, dy) in the UI units (1024 x 768): the camera of the same frame as the 3D world and
// the shapes - no lag behind the camera, whatever the rate of the Lua update. The point behind the camera or out of
// the screen - the window hidden (Show(false)), in the view - shown. An anchor is dropped (the window untouched) when
// its dialog is no longer drawn by the HUD or the window is no longer its child. false: no level, no window, the
// window has no parent, a bad value, the limit of anchors.
constexpr u32 kMaxAnchors = 256;
bool Anchor(CUIWindow* wnd, const Fvector& pos, float dx, float dy);
// Remove the anchor of the window (the window keeps its last position and visibility); false - it had none
bool AnchorRemove(CUIWindow* wnd);
// Remove all the anchors
void AnchorClear();

// CLevel::OnRender (main thread): draws the shapes and puts the anchored windows (before the UI)
void Render();
// CLevel::~CLevel (main thread, no render): drops the shapes and the batches
void Clear();
} // namespace gw::world_draw
