#include "StdAfx.h"

// Plugin API, groups feedback and effects: news, HUD messages, translation, PDA spots, sounds, particle effects,
// pp and camera effectors (addon_api_feedback.h has the overview; wiki/doc/plugins/api/feedback.md, effects.md).
// Every call does what its Lua counterpart does (level_script.cpp, script_sound.cpp, script_particles.cpp,
// script_game_object_inventory_owner.cpp), but checks first what Lua lets the engine stop the game on: an unknown
// particle effect, a missing animation file, a node missing in ui_custom_msgs.xml or map_spots.xml.

#include "addon_api_feedback.h"
#include "addon_api_common.h"
#include "addon_host.h"

#include "Actor.h"
#include "ActorEffector.h"
#include "HUDManager.h"
#include "ParticlesPlayer.h"
#include "PostprocessAnimator.h"
#include "UIGameCustom.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "game_news.h"
#include "map_location.h"
#include "map_manager.h"
#include "script_particles.h"
#include "Include/xrRender/Kinematics.h"
#include "xrEngine/StringTable/StringTable.h"
#include "xrUICore/XML/xrUIXmlParser.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace gw::addons::feedback
{
namespace
{
constexpr pcstr kFeedbackGroup = "feedback";
constexpr pcstr kEffectsGroup = "effects";
constexpr pcstr kFeedbackDefaultHudStatic = "not_enough_money_mine"; // the static of SetHudMsg (_g.script)
constexpr pcstr kFeedbackDefaultNewsIcon = "ui_iconsTotal_grouping"; // the icon of news_manager.send_tip
constexpr size_t kFeedbackMaxPathLength = 256; // a sound or animation path: well inside string_path with its root
// A node of an UI xml (a static, a spot type): XMLDocument::NavigateToNode copies the path into string_path and
// verifies it is below 200 characters
constexpr size_t kFeedbackMaxNodeLength = 128;
// delay_ms of news_send: CActor::AddGameNews_deffered adds it to the u32 Device.dwTimeGlobal
constexpr uint32_t kFeedbackMaxNewsDelayMs = 24u * 60u * 60u * 1000u;

// A handle: the kind in bits 28..31, the generation of the slot in bits 12..27, the slot index + 1 in bits 0..11.
// The kind keeps a sound handle from passing for a particle one; the generation changes at every release of the
// slot, so an old handle never reaches the next sound in it. A table first grows to kFeedbackReuseAfter slots and
// then reuses the free ones in turn: a slot is taken again only after about a thousand others, so its 16-bit
// generation comes round after tens of millions of sounds, not after minutes of short ones.
constexpr u32 kFeedbackKindShift = 28;
constexpr u32 kFeedbackGenerationShift = 12;
constexpr u32 kFeedbackGenerationMask = 0xFFFFu;
constexpr u32 kFeedbackIndexMask = 0xFFFu;
constexpr u32 kFeedbackKindSound = 1u;
constexpr u32 kFeedbackKindParticles = 2u;
constexpr size_t kFeedbackMaxSlots = kFeedbackIndexMask; // the index + 1 fits 12 bits: 31 plugins at their limit
constexpr size_t kFeedbackReuseAfter = 1024;

// The engine id of an effector of a plugin: kFeedbackEffectorBase + (range << 16) + the id of the plugin, one range per
// plugin. Lua uses ids up to a few hundred thousand and the engine hands out its own from 10000 up
// (RequestCamEffectorId), so the ranges from 2^30 meet neither; the largest id stays below 2^31, as the engine keeps the
// ids in an enum of int size.
constexpr u32 kFeedbackEffectorBase = 0x40000000u;
constexpr size_t kFeedbackEffectorRanges = 0x4000u;

struct FeedbackSoundSlot
{
    ref_sound* sound = nullptr; // heap: at the exit without the sound system it is left alone, not destroyed
    const GwpPlugin* owner = nullptr;
    const IGame_Level* level = nullptr; // the level it started on; the sound goes with it
    u32 generation = 1;
    bool used = false;
};

struct FeedbackParticlesSlot
{
    CScriptParticles* particles = nullptr; // m_particles turns nullptr when the engine destroys the effect itself
    const GwpPlugin* owner = nullptr;
    const IGame_Level* level = nullptr;
    u32 generation = 1;
    bool used = false;
    GwpObjectId attached = GWP_INVALID_OBJECT_ID; // particles_attach: follows this bone of the object every frame
    const CGameObject* attached_object = nullptr;  // only compared: another object under the same id stops the effect
    u16 bone = BI_NONE;
};

// An effector the plugin added, removed when the plugin stops. It lives in the camera manager of `actor`: the record
// of an actor that is gone is dropped (a load, a level change).
struct FeedbackEffectorRecord
{
    const GwpPlugin* owner = nullptr;
    const CActor* actor = nullptr;
    u32 engine_id = 0;
    bool camera = false;
};

xr_vector<FeedbackSoundSlot>* g_feedback_sounds = nullptr;
xr_vector<FeedbackParticlesSlot>* g_feedback_particles = nullptr;
size_t g_feedback_sound_cursor = 0; // where the search for a free slot to reuse starts
size_t g_feedback_particles_cursor = 0;
xr_vector<FeedbackEffectorRecord>* g_feedback_effectors = nullptr;
xr_vector<const GwpPlugin*>* g_feedback_effector_ranges = nullptr; // index = the id range of the plugin, for the run

bool FeedbackReadAllowed(pcstr group, pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [%s] %s called outside the main thread, ignored", group, function);
    return false;
}

// The name of a node of an UI xml (a static of ui_custom_msgs.xml, a spot type of map_spots.xml): a node, not a path
// (':' would walk into its children), short enough for XMLDocument::NavigateToNode
bool FeedbackNodeNameOk(pcstr name)
{
    return name && *name && xr_strlen(name) < kFeedbackMaxNodeLength && !strchr(name, ':');
}

bool FeedbackFinite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// ---------------------------------------------------------------------------------------------
// Handles and slots
// ---------------------------------------------------------------------------------------------

u32 FeedbackMakeHandle(u32 kind, size_t index, u32 generation)
{
    return (kind << kFeedbackKindShift) | ((generation & kFeedbackGenerationMask) << kFeedbackGenerationShift) |
        static_cast<u32>(index + 1);
}

bool FeedbackSplitHandle(u32 handle, u32 kind, size_t& index, u32& generation)
{
    const u32 low = handle & kFeedbackIndexMask;
    if ((handle >> kFeedbackKindShift) != kind || low == 0)
        return false;
    index = low - 1;
    generation = (handle >> kFeedbackGenerationShift) & kFeedbackGenerationMask;
    return true;
}

u32 FeedbackNextGeneration(u32 generation)
{
    generation = (generation + 1) & kFeedbackGenerationMask;
    return generation ? generation : 1;
}

void FeedbackReleaseSound(FeedbackSoundSlot& slot)
{
    if (slot.sound)
    {
        if (GEnv.Sound)
        {
            slot.sound->stop();
            xr_delete(slot.sound);
        }
        slot.sound = nullptr; // without the sound system (the exit) its destructor would call into nothing
    }
    slot.used = false;
    slot.owner = nullptr;
    slot.level = nullptr;
    slot.generation = FeedbackNextGeneration(slot.generation);
}

void FeedbackReleaseParticles(FeedbackParticlesSlot& slot)
{
    if (slot.particles)
    {
        // The destructor destroys the effect when the engine has not done it already (PSI_destroy needs the game
        // persistent: without it, at the exit, the wrapper is left alone - the effect still points to it)
        if (g_pGamePersistent)
            xr_delete(slot.particles);
        slot.particles = nullptr;
    }
    slot.used = false;
    slot.owner = nullptr;
    slot.level = nullptr;
    slot.attached = GWP_INVALID_OBJECT_ID;
    slot.attached_object = nullptr;
    slot.bone = BI_NONE;
    slot.generation = FeedbackNextGeneration(slot.generation);
}

// Plays, or waits for its delay; a sound of the previous level is gone with it
bool FeedbackSoundAlive(const FeedbackSoundSlot& slot)
{
    return slot.used && slot.sound && slot.sound->_feedback() && slot.level == g_pGameLevel;
}

bool FeedbackParticlesAlive(const FeedbackParticlesSlot& slot)
{
    return slot.used && slot.particles && slot.particles->m_particles && slot.level == g_pGameLevel &&
        slot.particles->m_particles->IsPlaying();
}

FeedbackSoundSlot* FeedbackFindSound(GwpSound handle)
{
    size_t index = 0;
    u32 generation = 0;
    if (!g_feedback_sounds || !FeedbackSplitHandle(handle, kFeedbackKindSound, index, generation) ||
        index >= g_feedback_sounds->size())
        return nullptr;
    FeedbackSoundSlot& slot = (*g_feedback_sounds)[index];
    return slot.used && slot.generation == generation ? &slot : nullptr;
}

FeedbackParticlesSlot* FeedbackFindParticles(GwpParticles handle)
{
    size_t index = 0;
    u32 generation = 0;
    if (!g_feedback_particles || !FeedbackSplitHandle(handle, kFeedbackKindParticles, index, generation) ||
        index >= g_feedback_particles->size())
        return nullptr;
    FeedbackParticlesSlot& slot = (*g_feedback_particles)[index];
    return slot.used && slot.generation == generation ? &slot : nullptr;
}

// A free slot for one more sound or effect of `owner`: the ended ones are released first. -1 at the limit of the
// plugin or of the table. Below kFeedbackReuseAfter slots the table grows; then the free slots are reused in turn,
// from `cursor` on, so one slot does not take every short sound and turn its generation round.
template <typename Slot, typename AliveFn, typename ReleaseFn>
int FeedbackAcquireSlot(
    xr_vector<Slot>& slots, size_t& cursor, const GwpPlugin* owner, u32 limit, AliveFn alive, ReleaseFn release)
{
    u32 count = 0;
    bool has_free = false;
    for (Slot& slot : slots)
    {
        if (slot.used && !alive(slot))
            release(slot);
        if (!slot.used)
            has_free = true;
        else if (slot.owner == owner)
            ++count;
    }
    if (count >= limit)
        return -1;
    if ((slots.size() < kFeedbackReuseAfter || !has_free) && slots.size() < kFeedbackMaxSlots)
    {
        slots.emplace_back();
        return static_cast<int>(slots.size() - 1);
    }
    for (size_t step = 0; step < slots.size(); ++step)
    {
        const size_t index = (cursor + step) % slots.size();
        if (!slots[index].used)
        {
            cursor = index + 1;
            return static_cast<int>(index);
        }
    }
    return -1;
}

int FeedbackAcquireSound(const GwpPlugin* owner)
{
    if (!g_feedback_sounds)
        g_feedback_sounds = xr_new<xr_vector<FeedbackSoundSlot>>();
    return FeedbackAcquireSlot(*g_feedback_sounds, g_feedback_sound_cursor, owner, GWP_SOUND_MAX_PER_PLUGIN,
        &FeedbackSoundAlive, &FeedbackReleaseSound);
}

int FeedbackAcquireParticles(const GwpPlugin* owner)
{
    if (!g_feedback_particles)
        g_feedback_particles = xr_new<xr_vector<FeedbackParticlesSlot>>();
    return FeedbackAcquireSlot(*g_feedback_particles, g_feedback_particles_cursor, owner,
        GWP_PARTICLES_MAX_PER_PLUGIN, &FeedbackParticlesAlive, &FeedbackReleaseParticles);
}

// The world transform of a bone, as CParticlesPlayer places the particles of an object (the effect looks up).
// false when the object has no skeleton now or not this bone (the visual may change).
bool FeedbackBoneTransform(CGameObject* object, u16 bone, Fmatrix& out)
{
    IKinematics* kinematics = object ? smart_cast<IKinematics*>(object->Visual()) : nullptr;
    if (!kinematics || bone == BI_NONE || bone >= kinematics->LL_BoneCount())
        return false;
    CParticlesPlayer::MakeXFORM(object, bone, Fvector().set(0.f, 1.f, 0.f), Fvector().set(0.f, 0.f, 0.f), out);
    return true;
}

// An attached effect follows its bone; when the object is gone it stops as a deferred stop and stays where it was.
// The id alone is not enough: an object destroyed between two frames may leave its id to a new one.
void FeedbackFollowBone(FeedbackParticlesSlot& slot)
{
    CGameObject* object = api::FindOnlineObject(slot.attached);
    if (object != slot.attached_object)
        object = nullptr;
    Fmatrix xform;
    if (FeedbackBoneTransform(object, slot.bone, xform))
    {
        slot.particles->m_particles->UpdateParent(xform, zero_vel);
        return;
    }
    slot.particles->m_particles->Stop(TRUE);
    slot.attached = GWP_INVALID_OBJECT_ID;
}

// ---------------------------------------------------------------------------------------------
// Effectors
// ---------------------------------------------------------------------------------------------

// The engine id of the effector `id` of the plugin; false when the ranges ran out (16384 plugins in one run).
// create == false: false for a plugin that has no range yet (it never added an effector).
bool FeedbackEffectorEngineId(const GwpPlugin* self, u32 id, bool create, u32& out)
{
    if (!g_feedback_effector_ranges)
        g_feedback_effector_ranges = xr_new<xr_vector<const GwpPlugin*>>();
    auto& ranges = *g_feedback_effector_ranges;
    const auto it = std::find(ranges.begin(), ranges.end(), self);
    size_t range = static_cast<size_t>(it - ranges.begin());
    if (it == ranges.end())
    {
        if (!create || ranges.size() >= kFeedbackEffectorRanges)
            return false;
        ranges.push_back(self);
        range = ranges.size() - 1;
    }
    out = kFeedbackEffectorBase + (static_cast<u32>(range) << 16) + id;
    return true;
}

void FeedbackRememberEffector(const GwpPlugin* self, u32 engine_id, bool camera)
{
    if (!g_feedback_effectors)
        g_feedback_effectors = xr_new<xr_vector<FeedbackEffectorRecord>>();
    for (FeedbackEffectorRecord& record : *g_feedback_effectors)
    {
        if (record.engine_id == engine_id && record.camera == camera)
        {
            record.actor = g_actor;
            return;
        }
    }
    FeedbackEffectorRecord record;
    record.owner = self;
    record.actor = g_actor;
    record.engine_id = engine_id;
    record.camera = camera;
    g_feedback_effectors->push_back(record);
}

void FeedbackForgetEffector(u32 engine_id, bool camera)
{
    if (!g_feedback_effectors)
        return;
    auto& records = *g_feedback_effectors;
    records.erase(std::remove_if(records.begin(), records.end(), [engine_id, camera](const FeedbackEffectorRecord& r)
    {
        return r.engine_id == engine_id && r.camera == camera;
    }), records.end());
}

CPostprocessAnimator* FeedbackFindPpEffector(u32 engine_id)
{
    if (!g_actor)
        return nullptr;
    return smart_cast<CPostprocessAnimator*>(
        g_actor->Cameras().GetPPEffector(static_cast<EEffectorPPType>(engine_id)));
}

// CCameraManager::AddCamEffector only queues the effector (m_EffectorsCam_added_deffered, protected): it joins the
// running ones at the next camera update, and GetCamEffector / RemoveCamEffector do not see it before. A pointer to
// the members named through a derived class reaches the queue and the release of the manager (never instantiated).
struct FeedbackCameraAccess : CCameraManager
{
    static EffectorCamVec& Queued(CCameraManager& cameras)
    {
        return cameras.*(&FeedbackCameraAccess::m_EffectorsCam_added_deffered);
    }
    static void Release(CCameraManager& cameras, SBaseEffector* effector)
    {
        (cameras.*(&FeedbackCameraAccess::OnEffectorReleased))(effector);
    }
};

// Removes the camera effector of this type, running or still queued (added in this frame). true when there was one.
bool FeedbackRemoveCamEffector(CCameraManager& cameras, u32 engine_id)
{
    const auto type = static_cast<ECamEffectorType>(engine_id);
    bool removed = false;
    EffectorCamVec& queued = FeedbackCameraAccess::Queued(cameras);
    for (auto it = queued.begin(); it != queued.end();)
    {
        if ((*it)->GetType() != type)
        {
            ++it;
            continue;
        }
        CEffectorCam* effector = *it;
        it = queued.erase(it);
        FeedbackCameraAccess::Release(cameras, effector); // as RemoveCamEffector does: the callback, the delete
        removed = true;
    }
    if (cameras.GetCamEffector(type))
    {
        cameras.RemoveCamEffector(type);
        removed = true;
    }
    return removed;
}

// Removes the effector from the camera manager of the actor it was added to; the actor must still be the current one
void FeedbackStopEffector(const FeedbackEffectorRecord& record)
{
    if (!g_actor || record.actor != g_actor)
        return;
    if (record.camera)
        FeedbackRemoveCamEffector(g_actor->Cameras(), record.engine_id);
    else if (CPostprocessAnimator* pp = FeedbackFindPpEffector(record.engine_id))
        pp->Stop(1.0f); // fades out, as level.remove_pp_effector
}

// The file of an effector: the extension the loader takes and a file it finds ($level$ first, then $game_anims$, as
// CObjectAnimator::LoadMotions and BasicPostProcessAnimator::Load - they stop the game on a missing file)
GwpResult FeedbackCheckEffectorFile(const GwpPlugin* self, pcstr file, bool camera, pcstr function)
{
    if (!file || !*file || xr_strlen(file) >= kFeedbackMaxPathLength)
        return GWP_ERROR_INVALID_ARGUMENT;
    const pcstr ext = strext(file);
    const bool ext_ok = ext &&
        (camera ? (0 == xr_strcmp(ext, ".anm") || 0 == xr_strcmp(ext, ".anms")) :
                  0 == xr_strcmp(ext, POSTPROCESS_FILE_EXTENSION));
    if (!ext_ok)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT,
            camera ? "a camera effector is an .anm or .anms file" : "a pp effector is a .ppe file");
    string_path full;
    const bool found = (FS.path_exist("$level$") && FS.exist(full, "$level$", file)) ||
        FS.exist(full, "$game_anims$", file);
    return found ? GWP_OK : api::PluginRefusal(self, function, GWP_ERROR_NOT_FOUND, "no such animation file");
}

// The checks every effector call shares: the call itself, the id, an actor. GWP_OK to go on.
GwpResult FeedbackEffectorCall(const GwpPlugin* self, u32 id, pcstr function)
{
    if (!api::CheckPluginMutation(self, kEffectsGroup, function))
        return api::PluginCallRefused(self);
    if (id > GWP_EFFECTOR_ID_MAX)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!g_actor)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no actor");
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// HUD and map
// ---------------------------------------------------------------------------------------------

// CUIGameCustom keeps ui_custom_msgs.xml protected; AddCustomStatic stops the game on a node the file has not.
// A pointer to the member named through a derived class is the legal way to read it (never instantiated).
struct FeedbackUiAccess : CUIGameCustom
{
    static CUIXml* Messages(CUIGameCustom& ui) { return ui.*(&FeedbackUiAccess::MsgConfig); }
};

CUIGameCustom* FeedbackGameUi()
{
    if (!g_pGameLevel || !Level().pHUD)
        return nullptr;
    return CurrentGameUI();
}

CMapManager* FeedbackMapManager()
{
    if (!g_pGameLevel || GEnv.isDedicatedServer)
        return nullptr;
    return &Level().MapManager();
}

// A spot is put on a server object (online or not): the map location follows it
bool FeedbackObjectExists(GwpObjectId id)
{
    if (id == GWP_INVALID_OBJECT_ID)
        return false;
    if (ai().get_alife() && ai().alife().objects().object(id, true))
        return true;
    return api::FindOnlineObject(id) != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group feedback)
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiNewsSend(const GwpPlugin* self, const char* caption_id, const char* text_id, const char* icon,
    uint32_t delay_ms, uint32_t show_ms)
{
    if (!api::CheckPluginMutation(self, kFeedbackGroup, "news_send"))
        return api::PluginCallRefused(self);
    if (!text_id || !*text_id || delay_ms > kFeedbackMaxNewsDelayMs)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!g_actor)
        return api::PluginRefusal(self, "news_send", GWP_ERROR_INVALID_STATE, "no actor");

    // The ids are stored: the PDA translates them when it shows the message (SetTextST), as for Lua news
    GAME_NEWS_DATA news;
    news.m_type = GAME_NEWS_DATA::eNews;
    news.news_caption = caption_id ? caption_id : "";
    news.news_text = text_id;
    news.texture_name = icon && *icon ? icon : kFeedbackDefaultNewsIcon;
    if (show_ms != 0)
        news.show_time = static_cast<int>(std::min<uint32_t>(show_ms, INT_MAX));
    if (delay_ms == 0)
        g_actor->AddGameNews(std::move(news));
    else
        g_actor->AddGameNews_deffered(std::move(news), delay_ms);
    return GWP_OK;
}

GwpResult GWP_CALL ApiHudMessage(const GwpPlugin* self, const char* static_id, const char* text_id, uint32_t show_ms)
{
    if (!api::CheckPluginMutation(self, kFeedbackGroup, "hud_message"))
        return api::PluginCallRefused(self);
    if (!text_id || !*text_id || show_ms == 0 || (static_id && *static_id && !FeedbackNodeNameOk(static_id)))
        return GWP_ERROR_INVALID_ARGUMENT;
    CUIGameCustom* ui = FeedbackGameUi();
    CUIXml* messages = ui ? FeedbackUiAccess::Messages(*ui) : nullptr;
    if (!messages)
        return api::PluginRefusal(self, "hud_message", GWP_ERROR_INVALID_STATE, "no HUD");
    const pcstr id = static_id && *static_id ? static_id : kFeedbackDefaultHudStatic;
    if (!messages->NavigateToNode(id, 0))
        return api::PluginRefusal(self, "hud_message", GWP_ERROR_NOT_FOUND, "ui_custom_msgs.xml has no such static");

    StaticDrawableWrapper* message = ui->AddCustomStatic(id, true);
    message->SetText(text_id); // SetTextST: translated here
    // The engine removes the static when its time is over (CUIGameCustom::OnFrame); a shown one gets a new time
    message->m_endTime = Device.fTimeGlobal + static_cast<float>(show_ms) / 1000.f;
    return GWP_OK;
}

GwpResult GWP_CALL ApiHudMessageHide(const GwpPlugin* self, const char* static_id)
{
    if (!api::CheckPluginMutation(self, kFeedbackGroup, "hud_message_hide"))
        return api::PluginCallRefused(self);
    if (static_id && *static_id && !FeedbackNodeNameOk(static_id))
        return GWP_ERROR_INVALID_ARGUMENT;
    CUIGameCustom* ui = FeedbackGameUi();
    if (!ui)
        return api::PluginRefusal(self, "hud_message_hide", GWP_ERROR_INVALID_STATE, "no HUD");
    const pcstr id = static_id && *static_id ? static_id : kFeedbackDefaultHudStatic;
    if (!ui->GetCustomStatic(id))
        return GWP_ERROR_NOT_FOUND;
    ui->RemoveCustomStatic(id);
    return GWP_OK;
}

uint32_t GWP_CALL ApiTranslate(const char* id, char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = 0;
    if (!FeedbackReadAllowed(kFeedbackGroup, "translate") || !id || !*id)
        return 0;
    const shared_str text = StringTable().translate(id);
    const pcstr value = text.c_str();
    if (!value)
        return 0;
    const u32 length = xr_strlen(value);
    if (out && cap)
    {
        u32 count = std::min(length, cap - 1);
        // Never half a character: step back to the first byte of the UTF-8 sequence the cut falls into
        while (count > 0 && count < length && (static_cast<u8>(value[count]) & 0xC0) == 0x80)
            --count;
        std::memcpy(out, value, count);
        out[count] = 0;
    }
    return length;
}

GwpResult GWP_CALL ApiMapSpotAdd(const GwpPlugin* self, GwpObjectId object, const char* spot_type, const char* hint_id)
{
    if (!api::CheckPluginMutation(self, kFeedbackGroup, "map_spot_add"))
        return api::PluginCallRefused(self);
    if (!FeedbackNodeNameOk(spot_type))
        return GWP_ERROR_INVALID_ARGUMENT;
    CMapManager* map = FeedbackMapManager();
    if (!map)
        return api::PluginRefusal(self, "map_spot_add", GWP_ERROR_INVALID_STATE, "no level");
    if (!FeedbackObjectExists(object))
        return api::PluginRefusal(self, "map_spot_add", GWP_ERROR_NOT_FOUND, "no such object");
    // CMapLocation::LoadSpot asserts on a spot type map_spots.xml does not have
    if (!CMapManager::m_uiSpotXml.NavigateToNode(spot_type, 0))
        return api::PluginRefusal(self, "map_spot_add", GWP_ERROR_NOT_FOUND, "map_spots.xml has no such spot type");

    CMapLocation* location = map->GetMapLocation(spot_type, object);
    if (!location)
        location = map->AddMapLocation(spot_type, object);
    if (hint_id && *hint_id)
        location->SetHint(hint_id); // an id: CMapLocation::GetHint translates it when the map shows it
    return GWP_OK;
}

GwpResult GWP_CALL ApiMapSpotRemove(const GwpPlugin* self, GwpObjectId object, const char* spot_type)
{
    if (!api::CheckPluginMutation(self, kFeedbackGroup, "map_spot_remove"))
        return api::PluginCallRefused(self);
    if (!FeedbackNodeNameOk(spot_type))
        return GWP_ERROR_INVALID_ARGUMENT;
    CMapManager* map = FeedbackMapManager();
    if (!map)
        return api::PluginRefusal(self, "map_spot_remove", GWP_ERROR_INVALID_STATE, "no level");
    if (!map->HasMapLocation(spot_type, object))
        return GWP_ERROR_NOT_FOUND;
    map->RemoveMapLocation(spot_type, object);
    return GWP_OK;
}

int GWP_CALL ApiMapSpotHas(GwpObjectId object, const char* spot_type)
{
    if (!FeedbackReadAllowed(kFeedbackGroup, "map_spot_has") || !FeedbackNodeNameOk(spot_type))
        return 0;
    CMapManager* map = FeedbackMapManager();
    return map && map->HasMapLocation(spot_type, object) ? 1 : 0;
}

GwpResult GWP_CALL ApiMapSpotSetHint(
    const GwpPlugin* self, GwpObjectId object, const char* spot_type, const char* hint_id)
{
    if (!api::CheckPluginMutation(self, kFeedbackGroup, "map_spot_set_hint"))
        return api::PluginCallRefused(self);
    if (!FeedbackNodeNameOk(spot_type))
        return GWP_ERROR_INVALID_ARGUMENT;
    CMapManager* map = FeedbackMapManager();
    if (!map)
        return api::PluginRefusal(self, "map_spot_set_hint", GWP_ERROR_INVALID_STATE, "no level");
    CMapLocation* location = map->GetMapLocation(spot_type, object);
    if (!location)
        return GWP_ERROR_NOT_FOUND;
    // No hint: back to the hint of the spot type, as CMapLocation::LoadSpot reads it
    location->SetHint(hint_id && *hint_id ? hint_id :
                                            CMapManager::m_uiSpotXml.ReadAttrib(spot_type, 0, "hint", "no hint"));
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group effects): sounds
// ---------------------------------------------------------------------------------------------

// pos == nullptr with positional == false: a 2D sound (sound_play_2d)
GwpResult FeedbackPlaySound(const GwpPlugin* self, const char* path, bool positional, const float* pos, float volume,
    uint32_t flags, GwpSound* out_sound, pcstr function)
{
    if (out_sound)
        *out_sound = GWP_INVALID_SOUND;
    if (!api::CheckPluginMutation(self, kEffectsGroup, function))
        return api::PluginCallRefused(self);
    if (!path || !*path || xr_strlen(path) >= kFeedbackMaxPathLength || !std::isfinite(volume) || volume < 0.f ||
        (positional && (!pos || !FeedbackFinite3(pos))))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (flags & ~GWP_SOUND_LOOPED)
        return GWP_ERROR_NOT_SUPPORTED;
    if (!GEnv.Sound)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no sound system");
    string_path file;
    if (!FS.exist(file, "$game_sounds$", path, ".ogg"))
        return api::PluginRefusal(self, function, GWP_ERROR_NOT_FOUND, "no such sound file");
    const int index = FeedbackAcquireSound(self);
    if (index < 0)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "too many sounds of the plugin play");

    FeedbackSoundSlot& slot = (*g_feedback_sounds)[static_cast<size_t>(index)];
    slot.sound = xr_new<ref_sound>();
    // Game type 0 (SOUND_TYPE_NO_SOUND): the AI does not hear it, as a sound_object(path) of Lua
    slot.sound->create(path, st_Effect, 0);
    u32 engine_flags = (flags & GWP_SOUND_LOOPED) ? u32(sm_Looped) : 0u;
    Fvector position = Fvector().set(0.f, 0.f, 0.f); // 2D: relative to the listener, at the head
    if (positional)
        position.set(pos[0], pos[1], pos[2]);
    else
        engine_flags |= sm_2D;
    slot.sound->play_at_pos(nullptr, position, engine_flags, 0.f);
    slot.sound->set_volume(volume);
    slot.used = true;
    slot.owner = self;
    slot.level = g_pGameLevel;
    if (out_sound)
        *out_sound = FeedbackMakeHandle(kFeedbackKindSound, static_cast<size_t>(index), slot.generation);
    return GWP_OK;
}

GwpResult GWP_CALL ApiSoundPlayAt(const GwpPlugin* self, const char* path, const float pos[3], float volume,
    uint32_t flags, GwpSound* out_sound)
{
    return FeedbackPlaySound(self, path, true, pos, volume, flags, out_sound, "sound_play_at");
}

GwpResult GWP_CALL ApiSoundPlay2d(
    const GwpPlugin* self, const char* path, float volume, uint32_t flags, GwpSound* out_sound)
{
    return FeedbackPlaySound(self, path, false, nullptr, volume, flags, out_sound, "sound_play_2d");
}

GwpResult GWP_CALL ApiSoundStop(const GwpPlugin* self, GwpSound sound)
{
    if (!api::CheckPluginMutation(self, kEffectsGroup, "sound_stop"))
        return api::PluginCallRefused(self);
    FeedbackSoundSlot* slot = FeedbackFindSound(sound);
    if (!slot || slot->owner != self)
        return GWP_ERROR_NOT_FOUND;
    const bool playing = FeedbackSoundAlive(*slot);
    FeedbackReleaseSound(*slot);
    return playing ? GWP_OK : GWP_ERROR_NOT_FOUND;
}

int GWP_CALL ApiSoundIsPlaying(GwpSound sound)
{
    if (!FeedbackReadAllowed(kEffectsGroup, "sound_is_playing"))
        return 0;
    const FeedbackSoundSlot* slot = FeedbackFindSound(sound);
    return slot && FeedbackSoundAlive(*slot) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group effects): particles
// ---------------------------------------------------------------------------------------------

// The checks of particles_play_at / particles_attach before a slot is taken. GWP_OK to go on.
GwpResult FeedbackParticlesCall(const GwpPlugin* self, const char* name, pcstr function)
{
    if (!api::CheckPluginMutation(self, kEffectsGroup, function))
        return api::PluginCallRefused(self);
    if (!name || !*name)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (GEnv.isDedicatedServer)
        return GWP_ERROR_NOT_SUPPORTED;
    if (!g_pGameLevel || !g_pGamePersistent || !GEnv.Render)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no level");
    // model_CreateParticles stops the game on an unknown name (Lua particles_object does the same)
    if (!GEnv.Render->model_ParticlesExist(name))
        return api::PluginRefusal(self, function, GWP_ERROR_NOT_FOUND, "no such particle effect");
    return GWP_OK;
}

// A slot with a new effect, not started yet; -1 at the limit of the plugin
int FeedbackCreateParticles(const GwpPlugin* self, const char* name)
{
    const int index = FeedbackAcquireParticles(self);
    if (index < 0)
        return -1;
    FeedbackParticlesSlot& slot = (*g_feedback_particles)[static_cast<size_t>(index)];
    slot.particles = xr_new<CScriptParticles>(name);
    slot.used = true;
    slot.owner = self;
    slot.level = g_pGameLevel;
    return index;
}

GwpResult GWP_CALL ApiParticlesPlayAt(const GwpPlugin* self, const char* name, const float pos[3],
    const float dir[3], GwpParticles* out_particles)
{
    if (out_particles)
        *out_particles = GWP_INVALID_PARTICLES;
    const GwpResult check = FeedbackParticlesCall(self, name, "particles_play_at");
    if (check != GWP_OK)
        return check;
    if (!pos || !FeedbackFinite3(pos) || (dir && !FeedbackFinite3(dir)))
        return GWP_ERROR_INVALID_ARGUMENT;
    Fvector direction = Fvector().set(0.f, 0.f, 1.f);
    if (dir)
    {
        direction.set(dir[0], dir[1], dir[2]);
        if (direction.square_magnitude() < EPS_S)
            return GWP_ERROR_INVALID_ARGUMENT;
        direction.normalize();
    }
    const int index = FeedbackCreateParticles(self, name);
    if (index < 0)
        return api::PluginRefusal(self, "particles_play_at", GWP_ERROR_INVALID_STATE, "too many effects of the plugin");

    FeedbackParticlesSlot& slot = (*g_feedback_particles)[static_cast<size_t>(index)];
    if (dir)
        slot.particles->SetDirection(direction);
    slot.particles->PlayAtPos(Fvector().set(pos[0], pos[1], pos[2]));
    if (out_particles)
        *out_particles = FeedbackMakeHandle(kFeedbackKindParticles, static_cast<size_t>(index), slot.generation);
    return GWP_OK;
}

GwpResult GWP_CALL ApiParticlesAttach(const GwpPlugin* self, const char* name, GwpObjectId object, const char* bone,
    GwpParticles* out_particles)
{
    if (out_particles)
        *out_particles = GWP_INVALID_PARTICLES;
    const GwpResult check = FeedbackParticlesCall(self, name, "particles_attach");
    if (check != GWP_OK)
        return check;
    CGameObject* target = api::FindOnlineObject(object);
    if (!target)
        return api::PluginRefusal(self, "particles_attach", GWP_ERROR_NOT_FOUND, "no such online object");
    IKinematics* kinematics = smart_cast<IKinematics*>(target->Visual());
    if (!kinematics)
        return api::PluginRefusal(self, "particles_attach", GWP_ERROR_INVALID_ARGUMENT, "the object has no skeleton");
    const u16 bone_id = bone && *bone ? kinematics->LL_BoneID(bone) : kinematics->LL_GetBoneRoot();
    Fmatrix xform;
    if (!FeedbackBoneTransform(target, bone_id, xform))
        return api::PluginRefusal(self, "particles_attach", GWP_ERROR_NOT_FOUND, "the object has no such bone");
    const int index = FeedbackCreateParticles(self, name);
    if (index < 0)
        return api::PluginRefusal(self, "particles_attach", GWP_ERROR_INVALID_STATE, "too many effects of the plugin");

    // As CScriptParticles::PlayAtPos: the place before the start (the first particles are born there) and after it
    FeedbackParticlesSlot& slot = (*g_feedback_particles)[static_cast<size_t>(index)];
    slot.attached = object;
    slot.attached_object = target;
    slot.bone = bone_id;
    CParticlesObject* effect = slot.particles->m_particles;
    effect->UpdateParent(xform, zero_vel);
    effect->Play(false);
    effect->UpdateParent(xform, zero_vel);
    if (out_particles)
        *out_particles = FeedbackMakeHandle(kFeedbackKindParticles, static_cast<size_t>(index), slot.generation);
    return GWP_OK;
}

GwpResult GWP_CALL ApiParticlesStop(const GwpPlugin* self, GwpParticles particles, uint32_t flags)
{
    if (!api::CheckPluginMutation(self, kEffectsGroup, "particles_stop"))
        return api::PluginCallRefused(self);
    if (flags & ~GWP_PARTICLES_STOP_DEFERRED)
        return GWP_ERROR_NOT_SUPPORTED;
    FeedbackParticlesSlot* slot = FeedbackFindParticles(particles);
    if (!slot || slot->owner != self)
        return GWP_ERROR_NOT_FOUND;
    if (!FeedbackParticlesAlive(*slot))
    {
        FeedbackReleaseParticles(*slot);
        return GWP_ERROR_NOT_FOUND;
    }
    if (flags & GWP_PARTICLES_STOP_DEFERRED)
        slot->particles->m_particles->Stop(TRUE); // the slot goes when the last particles are gone (OnFrame)
    else
        FeedbackReleaseParticles(*slot);
    return GWP_OK;
}

int GWP_CALL ApiParticlesIsPlaying(GwpParticles particles)
{
    if (!FeedbackReadAllowed(kEffectsGroup, "particles_is_playing"))
        return 0;
    const FeedbackParticlesSlot* slot = FeedbackFindParticles(particles);
    return slot && FeedbackParticlesAlive(*slot) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group effects): effectors
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiPpEffectorAdd(const GwpPlugin* self, const char* file, uint32_t id, int cyclic)
{
    const GwpResult check = FeedbackEffectorCall(self, id, "pp_effector_add");
    if (check != GWP_OK)
        return check;
    const GwpResult file_check = FeedbackCheckEffectorFile(self, file, false, "pp_effector_add");
    if (file_check != GWP_OK)
        return file_check;
    u32 engine_id = 0;
    if (!FeedbackEffectorEngineId(self, id, true, engine_id))
        return api::PluginRefusal(self, "pp_effector_add", GWP_ERROR_INVALID_STATE, "no effector id range left");

    // level.add_pp_effector: the same type again replaces the effector (CCameraManager::AddPPEffector)
    CPostprocessAnimator* pp = xr_new<CPostprocessAnimator>(static_cast<int>(engine_id), cyclic != 0);
    pp->Load(file);
    g_actor->Cameras().AddPPEffector(pp);
    FeedbackRememberEffector(self, engine_id, false);
    return GWP_OK;
}

GwpResult GWP_CALL ApiPpEffectorRemove(const GwpPlugin* self, uint32_t id)
{
    const GwpResult check = FeedbackEffectorCall(self, id, "pp_effector_remove");
    if (check != GWP_OK)
        return check;
    u32 engine_id = 0;
    if (!FeedbackEffectorEngineId(self, id, false, engine_id))
        return GWP_ERROR_NOT_FOUND;
    CPostprocessAnimator* pp = FeedbackFindPpEffector(engine_id);
    FeedbackForgetEffector(engine_id, false);
    if (!pp)
        return GWP_ERROR_NOT_FOUND;
    pp->Stop(1.0f); // fades out, as level.remove_pp_effector
    return GWP_OK;
}

GwpResult GWP_CALL ApiPpEffectorSetFactor(const GwpPlugin* self, uint32_t id, float factor, float speed)
{
    const GwpResult check = FeedbackEffectorCall(self, id, "pp_effector_set_factor");
    if (check != GWP_OK)
        return check;
    if (!std::isfinite(factor) || !std::isfinite(speed) || speed < 0.f)
        return GWP_ERROR_INVALID_ARGUMENT;
    u32 engine_id = 0;
    CPostprocessAnimator* pp =
        FeedbackEffectorEngineId(self, id, false, engine_id) ? FeedbackFindPpEffector(engine_id) : nullptr;
    if (!pp)
        return GWP_ERROR_NOT_FOUND;
    pp->SetDesiredFactor(factor, speed); // level.set_pp_effector_factor
    return GWP_OK;
}

GwpResult GWP_CALL ApiCamEffectorAdd(const GwpPlugin* self, const char* file, uint32_t id, int cyclic,
    float* out_length)
{
    if (out_length)
        *out_length = 0.f;
    const GwpResult check = FeedbackEffectorCall(self, id, "cam_effector_add");
    if (check != GWP_OK)
        return check;
    const GwpResult file_check = FeedbackCheckEffectorFile(self, file, true, "cam_effector_add");
    if (file_check != GWP_OK)
        return file_check;
    u32 engine_id = 0;
    if (!FeedbackEffectorEngineId(self, id, true, engine_id))
        return api::PluginRefusal(self, "cam_effector_add", GWP_ERROR_INVALID_STATE, "no effector id range left");

    // level.add_cam_effector without the Lua callback at the end: the camera manager takes it in the next frame
    // and drops an effector of the same type then (CCameraManager::UpdateDeffered)
    CAnimatorCamEffector* effector = xr_new<CAnimatorCamEffector>();
    effector->SetType(static_cast<ECamEffectorType>(engine_id));
    effector->SetCyclic(cyclic != 0);
    effector->Start(file);
    g_actor->Cameras().AddCamEffector(effector);
    if (out_length)
        *out_length = effector->GetAnimatorLength();
    FeedbackRememberEffector(self, engine_id, true);
    return GWP_OK;
}

GwpResult GWP_CALL ApiCamEffectorRemove(const GwpPlugin* self, uint32_t id)
{
    const GwpResult check = FeedbackEffectorCall(self, id, "cam_effector_remove");
    if (check != GWP_OK)
        return check;
    u32 engine_id = 0;
    if (!FeedbackEffectorEngineId(self, id, false, engine_id))
        return GWP_ERROR_NOT_FOUND;
    // A running one or one added in this frame (still queued in the camera manager)
    const bool removed = FeedbackRemoveCamEffector(g_actor->Cameras(), engine_id);
    FeedbackForgetEffector(engine_id, true);
    return removed ? GWP_OK : GWP_ERROR_NOT_FOUND;
}

// Everything of every plugin goes (a game starts, the exit)
void FeedbackReleaseAll()
{
    if (g_feedback_sounds)
    {
        for (FeedbackSoundSlot& slot : *g_feedback_sounds)
        {
            if (slot.used)
                FeedbackReleaseSound(slot);
        }
    }
    if (g_feedback_particles)
    {
        for (FeedbackParticlesSlot& slot : *g_feedback_particles)
        {
            if (slot.used)
                FeedbackReleaseParticles(slot);
        }
    }
    if (g_feedback_effectors)
        g_feedback_effectors->clear(); // the effectors live in the camera manager of the actor that goes with the game
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Host interface
// ---------------------------------------------------------------------------------------------

void OnFrame()
{
    if (g_feedback_sounds)
    {
        for (FeedbackSoundSlot& slot : *g_feedback_sounds)
        {
            if (slot.used && !FeedbackSoundAlive(slot))
                FeedbackReleaseSound(slot);
        }
    }
    if (g_feedback_particles)
    {
        for (FeedbackParticlesSlot& slot : *g_feedback_particles)
        {
            if (!slot.used)
                continue;
            if (slot.particles && slot.particles->m_particles && slot.level == g_pGameLevel &&
                slot.attached != GWP_INVALID_OBJECT_ID)
                FeedbackFollowBone(slot);
            if (!FeedbackParticlesAlive(slot))
                FeedbackReleaseParticles(slot);
        }
    }
    if (g_feedback_effectors)
    {
        auto& records = *g_feedback_effectors;
        records.erase(std::remove_if(records.begin(), records.end(), [](const FeedbackEffectorRecord& r)
        {
            return r.actor != g_actor;
        }), records.end());
    }
}

void OnGameStart() { FeedbackReleaseAll(); }

void RemovePluginResources(const GwpPlugin* plugin)
{
    if (!plugin)
        return;
    if (g_feedback_sounds)
    {
        for (FeedbackSoundSlot& slot : *g_feedback_sounds)
        {
            if (slot.used && slot.owner == plugin)
                FeedbackReleaseSound(slot);
        }
    }
    if (g_feedback_particles)
    {
        for (FeedbackParticlesSlot& slot : *g_feedback_particles)
        {
            if (slot.used && slot.owner == plugin)
                FeedbackReleaseParticles(slot);
        }
    }
    if (g_feedback_effectors)
    {
        auto& records = *g_feedback_effectors;
        for (const FeedbackEffectorRecord& record : records)
        {
            if (record.owner == plugin)
                FeedbackStopEffector(record);
        }
        records.erase(std::remove_if(records.begin(), records.end(), [plugin](const FeedbackEffectorRecord& r)
        {
            return r.owner == plugin;
        }), records.end());
    }
}

void Shutdown()
{
    FeedbackReleaseAll();
    xr_delete(g_feedback_sounds);
    xr_delete(g_feedback_particles);
    xr_delete(g_feedback_effectors);
    xr_delete(g_feedback_effector_ranges);
    g_feedback_sound_cursor = 0;
    g_feedback_particles_cursor = 0;
}

void FillEngineApi(GwpEngineApi& api)
{
    api.news_send = &ApiNewsSend;
    api.hud_message = &ApiHudMessage;
    api.hud_message_hide = &ApiHudMessageHide;
    api.translate = &ApiTranslate;
    api.map_spot_add = &ApiMapSpotAdd;
    api.map_spot_remove = &ApiMapSpotRemove;
    api.map_spot_has = &ApiMapSpotHas;
    api.map_spot_set_hint = &ApiMapSpotSetHint;
    api.sound_play_at = &ApiSoundPlayAt;
    api.sound_play_2d = &ApiSoundPlay2d;
    api.sound_stop = &ApiSoundStop;
    api.sound_is_playing = &ApiSoundIsPlaying;
    api.particles_play_at = &ApiParticlesPlayAt;
    api.particles_attach = &ApiParticlesAttach;
    api.particles_stop = &ApiParticlesStop;
    api.particles_is_playing = &ApiParticlesIsPlaying;
    api.pp_effector_add = &ApiPpEffectorAdd;
    api.pp_effector_remove = &ApiPpEffectorRemove;
    api.pp_effector_set_factor = &ApiPpEffectorSetFactor;
    api.cam_effector_add = &ApiCamEffectorAdd;
    api.cam_effector_remove = &ApiCamEffectorRemove;
}
} // namespace gw::addons::feedback
