#pragma once

// D3 (plans/addon_base_building/plan.md 10.3): "ghost" models - a visual drawn at a transform without a game object:
// no physics, no collision (rays and bullets go through it), no ALife ID, not saved. For placement previews
// (base building) and the like. Lua: level.preview_model_show / preview_model_set_transform / preview_model_hide /
// preview_model_hide_all (level_script.cpp).
//
// A few previews at once, by a slot id 0..kMaxPreviews-1. The calls answer at once (Show checks that the model file
// exists), the change itself is applied on the main thread at the end of the frame (Device.PostJoin): Lua may run on
// the Game thread beside the render, which reads the visual and the transform. The level unload (CLevel::~CLevel)
// destroys every preview; a change queued before it is dropped.
//
// The model is drawn as a translucent ghost (IRender::model_MakeGhost): every mesh gets the forward shader
// "gw_preview_ghost" (gamedata\shaders\r3 and shaders\gl) - its base texture tinted by the color of the slot, in the
// sorted pass after the scene, without depth write and without a shadow; the deferred passes (and the GPU instancing of
// their groups) never see it. Without that shader script (or on a render without model_MakeGhost) the model stays
// opaque with its own shaders, as before. A placement tells "can / cannot" by the color (SetColor).
namespace gw::preview_model
{
constexpr u32 kMaxPreviews = 8;
// The color of a slot until SetColor: a light blue, half transparent
constexpr float kDefaultTint[4] = {0.70f, 0.85f, 1.00f, 0.50f};

// Shows the model `visual` (as the visual key of a section: "dynamics\\box\\box_wood_01", .ogf may be omitted) in the
// slot `id` at `position`, rotated by `hpb` (radians: x - heading, y - pitch, z - bank; XFORM.setHPB). An object spawned
// with se_obj.angle = (p, h, b) stands the same: CGameObject::net_Spawn does XFORM.setXYZ(o_Angle), which is
// setHPB(o_Angle.y, o_Angle.x, o_Angle.z). A slot already shown changes its model. false: no level, bad id, empty name
// or no such model file (one log line).
bool Show(u32 id, pcstr visual, const Fvector& position, const Fvector& hpb);
// Moves the shown preview of the slot. false: no level, bad id or nothing shown in the slot.
bool SetTransform(u32 id, const Fvector& position, const Fvector& hpb);
// Hides and destroys the preview of the slot. false: nothing was shown there.
bool Hide(u32 id);
// Hides all the previews.
void HideAll();
// The tint of the ghost of the slot: rgb - color, a - opacity, each clamped to 0..1. Kept by the slot (through Hide and
// a Show of another model) until the next SetColor or the level unload (Clear: kDefaultTint again). May be set before
// Show. false: bad id or a value is not a number.
bool SetColor(u32 id, const Fvector4& color);
// Whether the slot shows a model (as the Lua calls see it: the last Show / Hide, even before the end of the frame).
bool IsShown(u32 id);

// The bounding box of the model `visual` (the same names as Show) in its own space, as the model file states it (the
// OGF header: the bind pose of a skeleton): object space of an object with this visual. Read from the file, the model
// is not created - any thread; remembered per name until the level unload (Clear). A placement checks the overlap of
// two objects by these boxes turned by their angles (base building). false: empty or too long name, no such model
// file, not an OGF of this version or an empty box (one log line per name).
bool ModelBounds(pcstr visual, Fbox& box);

// Whether the model file of `visual` (the same names as Show) exists - the lookup of the model pool (.ogf appended
// when missing, $level$ then $game_meshes$) - and the name fits its path buffers. For the callers that hand a model
// name from Lua to the model pool, which stops the game on a missing file (level.ui_preview_thumb /
// ui_preview_live_set of level_script.cpp). false: empty or too long name, no such file - one log line per name
// (`function` - the Lua call for the log) until the level unload. Any thread.
bool ModelFileExists(pcstr visual, pcstr function);

// CLevel::~CLevel (main thread, no render): destroys the previews now and drops the changes still queued.
void Clear();
} // namespace gw::preview_model
