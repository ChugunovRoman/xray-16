#include "StdAfx.h"

// Plugin API: the functions of plans/addon_api_ideas (group plugin) without a file of their own group. The functions of
// a group that has its state in another file live there: cvar_register_string / console_command_* in
// addon_api_console.cpp, log_set_level in addon_host.cpp, task_async in addon_api_threads.cpp, sound_play_attached in
// addon_api_feedback.cpp. Docs: wiki/doc/plugins/api/*.md (the group of every function is in gwp_api.h).

#include "addon_api_extra.h"
#include "addon_api_common.h"
#include "addon_api_console.h"
#include "addon_event_bus.h"

#include "Actor.h"
#include "ActorBackpack.h"
#include "ActorHelmet.h"
#include "Artefact.h"
#include "CustomMonster.h"
#include "CustomOutfit.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "inventory_item.h"
#include "level_world_draw.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/xr_level_controller.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// console_commands.cpp: the check of the name of a save (no \ / : * ? " < > |)
bool valid_saved_game_name(LPCSTR file_name);

namespace gw::addons::extra
{
namespace
{
constexpr pcstr kIniGroup = "ini";
constexpr pcstr kLevelGroup = "level";

bool ReadAllowed(pcstr group, pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [%s] %s called outside the main thread, ignored", group, function);
    return false;
}

// ---------------------------------------------------------------------------------------------
// P18: the files of the game data (group ini)
// ---------------------------------------------------------------------------------------------

constexpr size_t kMaxFsPath = 400; // a string_path holds $game_data$ + this

// A path relative to gamedata, as the index of the file system spells it: '\' between the folders, lower case (the
// index keeps the names in lower case). false: empty, too long, absolute, a drive, "..", an alias ($...$)
bool FsRelativePath(pcstr path, bool allow_empty, xr_string& out)
{
    out.clear();
    if (!path || !*path)
        return allow_empty;
    const size_t length = xr_strlen(path);
    if (length > kMaxFsPath || path[0] == '\\' || path[0] == '/' || strchr(path, ':') || strchr(path, '$'))
        return false;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i)
    {
        char c = path[i];
        if (c == '/')
            c = '\\';
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
        // the ASCII letters only: a UTF-8 byte stays as it is
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        out += c;
    }
    // ".." as a whole component, anywhere
    for (size_t begin = 0; begin <= out.size();)
    {
        size_t end = out.find('\\', begin);
        if (end == xr_string::npos)
            end = out.size();
        if (out.compare(begin, end - begin, "..") == 0)
            return false;
        begin = end + 1;
    }
    return true;
}

// The absolute name of a file of $game_data$ in the index; false for a bad path
bool FsFullName(pcstr path, string_path& full)
{
    xr_string relative;
    if (!FsRelativePath(path, false, relative) || relative.back() == '\\')
        return false;
    FS.update_path(full, "$game_data$", relative.c_str());
    return true;
}

GwpResult GWP_CALL ApiFsStat(const char* path, uint32_t* out_size, int64_t* out_mtime)
{
    if (out_size)
        *out_size = 0;
    if (out_mtime)
        *out_mtime = 0;
    if (!ReadAllowed(kIniGroup, "fs_stat"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    string_path full;
    if (!FsFullName(path, full))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!FS.exist(full))
        return GWP_ERROR_NOT_FOUND;
    const int length = FS.file_length(full);
    if (length < 0)
        return GWP_ERROR_NOT_FOUND;
    if (out_size)
        *out_size = static_cast<uint32_t>(length);
    if (out_mtime)
    {
        // modif of the index: the time of a loose file; an archive keeps its own (or 0)
        const u32 age = FS.get_file_age(full);
        *out_mtime = age == u32(-1) ? 0 : static_cast<int64_t>(age);
    }
    return GWP_OK;
}

GwpResult GWP_CALL ApiFsRead(const char* path, void* out, uint32_t cap, uint32_t* size)
{
    if (size)
        *size = 0;
    if (!ReadAllowed(kIniGroup, "fs_read"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    string_path full;
    if (!size || (!out && cap > 0) || !FsFullName(path, full))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!FS.exist(full))
        return GWP_ERROR_NOT_FOUND;
    const int length = FS.file_length(full);
    if (length < 0)
        return GWP_ERROR_NOT_FOUND;
    *size = static_cast<uint32_t>(length);
    if (static_cast<u64>(length) > GWP_FS_MAX_FILE_SIZE)
        return GWP_ERROR_NOT_SUPPORTED;
    if (cap < *size)
        return GWP_ERROR_INVALID_ARGUMENT; // cap 0 asks the size
    IReader* reader = FS.r_open(full);
    if (!reader)
        return GWP_ERROR_NOT_FOUND;
    // The size of what was opened: the file may have changed since file_length (a loose file being written)
    const size_t opened = static_cast<size_t>(reader->length());
    *size = static_cast<uint32_t>(std::min<size_t>(opened, GWP_FS_MAX_FILE_SIZE));
    const bool fits = opened <= cap;
    if (fits && opened)
        std::memcpy(out, reader->pointer(), opened);
    FS.r_close(reader);
    return fits ? GWP_OK : GWP_ERROR_INVALID_ARGUMENT;
}

uint32_t GWP_CALL ApiFsList(const char* folder, const char* mask, uint32_t flags, char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = 0;
    if (!ReadAllowed(kIniGroup, "fs_list"))
        return 0;
    constexpr uint32_t kKnown = GWP_FS_LIST_FILES | GWP_FS_LIST_FOLDERS | GWP_FS_LIST_RECURSIVE;
    xr_string relative;
    if ((flags & ~kKnown) || !FsRelativePath(folder, true, relative))
        return 0;
    if (!relative.empty() && relative.back() != '\\')
        relative += '\\';
    xr_string pattern = mask && *mask ? mask : "*";
    std::transform(pattern.begin(), pattern.end(), pattern.begin(),
        [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });

    u32 engine_flags = 0;
    if ((flags & GWP_FS_LIST_FILES) || !(flags & GWP_FS_LIST_FOLDERS))
        engine_flags |= FS_ListFiles;
    if (flags & GWP_FS_LIST_FOLDERS)
        engine_flags |= FS_ListFolders;
    if (!(flags & GWP_FS_LIST_RECURSIVE))
        engine_flags |= FS_RootOnly;
    string_path full;
    FS.update_path(full, "$game_data$", relative.c_str());
    FS_FileSet files;
    FS.file_list(files, full, engine_flags);

    // The set is sorted by name; the mask goes to the last component of the name (a folder without its '\')
    xr_string text;
    for (const FS_File& file : files)
    {
        const xr_string& name = file.name;
        xr_string last = name;
        if (!last.empty() && last.back() == '\\')
            last.pop_back();
        const size_t slash = last.rfind('\\');
        if (slash != xr_string::npos)
            last.erase(0, slash + 1);
        if (!PatternMatch(last.c_str(), pattern.c_str()))
            continue;
        if (!text.empty())
            text += '\n';
        text += name;
    }
    const uint32_t length = static_cast<uint32_t>(text.size());
    if (out && cap)
    {
        const uint32_t copied = std::min(length, cap - 1);
        std::memcpy(out, text.data(), copied);
        out[copied] = 0;
    }
    return length;
}

// ---------------------------------------------------------------------------------------------
// P21: game_save (group save)
// ---------------------------------------------------------------------------------------------

constexpr size_t kMaxSaveName = 200;

GwpResult GWP_CALL ApiGameSave(const GwpPlugin* self, const char* name)
{
    if (!api::CheckPluginMutation(self, "save", "game_save"))
        return api::PluginCallRefused(self);
    if (!g_pGameLevel || !IsGameTypeSingle() || !g_actor || !Actor()->g_Alive())
        return api::PluginRefusal(self, "game_save", GWP_ERROR_INVALID_STATE, "no game, or the actor is dead");
    xr_string command = "save";
    if (name && *name)
    {
        if (xr_strlen(name) > kMaxSaveName || strchr(name, '\n') || strchr(name, '\r') ||
            !valid_saved_game_name(name))
            return api::PluginRefusal(self, "game_save", GWP_ERROR_INVALID_ARGUMENT, "the engine refuses this name");
        command += ' ';
        command += name;
    }
    if (IsDebugLog())
        Msg("  [plugin:%s] game_save: %s (the next frame)", PluginAddonId(self), command.c_str());
    console::QueueCommand(command.c_str());
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// P24: item_params (group items)
// ---------------------------------------------------------------------------------------------

constexpr ALife::EHitType HitTypeAt(u32 index) { return static_cast<ALife::EHitType>(index); }
static_assert(GWP_HIT_TYPE_COUNT == ALife::eHitTypeMax, "GWP_HIT_TYPE_COUNT: the real hit types of the engine");

GwpResult GWP_CALL ApiItemParams(GwpObjectId item_id, GwpItemParams* out)
{
    if (!ReadAllowed("items", "item_params"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    constexpr uint32_t kMinSize = offsetof(GwpItemParams, artefact_count) + sizeof(uint32_t);
    if (!out || out->size < kMinSize)
        return GWP_ERROR_INVALID_ARGUMENT;
    CGameObject* object = api::FindOnlineObject(item_id);
    if (!object)
        return api::MissingObjectCode(item_id);
    CInventoryItem* item = smart_cast<CInventoryItem*>(object);
    if (!item)
        return GWP_ERROR_INVALID_ARGUMENT;

    // Filled in a full local copy, then the part the plugin knows is copied (its size)
    GwpItemParams params{};
    params.size = sizeof(GwpItemParams);
    params.kind = GWP_ITEM_KIND_OTHER;
    params.weight = item->Weight();
    params.cost = item->Cost();
    params.condition = item->GetCondition();
    for (u32 i = 0; i < GWP_HIT_TYPE_COUNT; ++i)
        params.immunity[i] = item->GetHitImmunity(HitTypeAt(i));

    const auto restore = [&params](float health, float radiation, float satiety, float power, float bleeding) {
        params.restore[GWP_RESTORE_HEALTH] = health;
        params.restore[GWP_RESTORE_RADIATION] = radiation;
        params.restore[GWP_RESTORE_SATIETY] = satiety;
        params.restore[GWP_RESTORE_POWER] = power;
        params.restore[GWP_RESTORE_BLEEDING] = bleeding;
    };
    if (const CCustomOutfit* outfit = smart_cast<const CCustomOutfit*>(object))
    {
        params.kind = GWP_ITEM_KIND_OUTFIT;
        params.additional_weight = outfit->m_additional_weight;
        params.additional_weight_max = outfit->m_additional_weight2;
        params.power_loss = outfit->GetPowerLoss();
        params.artefact_count = outfit->get_artefact_count();
        restore(outfit->m_fHealthRestoreSpeed, outfit->m_fRadiationRestoreSpeed, outfit->m_fSatietyRestoreSpeed,
            outfit->m_fPowerRestoreSpeed, outfit->m_fBleedingRestoreSpeed);
        for (u32 i = 0; i < GWP_HIT_TYPE_COUNT; ++i)
            params.protection[i] = outfit->GetDefHitTypeProtection(HitTypeAt(i));
    }
    else if (const CHelmet* helmet = smart_cast<const CHelmet*>(object))
    {
        params.kind = GWP_ITEM_KIND_HELMET;
        params.power_loss = helmet->m_fPowerLoss;
        restore(helmet->m_fHealthRestoreSpeed, helmet->m_fRadiationRestoreSpeed, helmet->m_fSatietyRestoreSpeed,
            helmet->m_fPowerRestoreSpeed, helmet->m_fBleedingRestoreSpeed);
        for (u32 i = 0; i < GWP_HIT_TYPE_COUNT; ++i)
            params.protection[i] = helmet->GetDefHitTypeProtection(HitTypeAt(i));
    }
    else if (const CArtefact* artefact = smart_cast<const CArtefact*>(object))
    {
        params.kind = GWP_ITEM_KIND_ARTEFACT;
        params.additional_weight = artefact->AdditionalInventoryWeight();
        restore(artefact->GetHealthPower(), artefact->GetRadiationPower(), artefact->GetSatietyPower(),
            artefact->GetPowerPower(), artefact->GetBleedingPower());
        for (u32 i = 0; i < GWP_HIT_TYPE_COUNT; ++i)
            params.protection[i] = artefact->m_ArtefactHitImmunities.GetHitImmunity(HitTypeAt(i));
    }
    else if (const CBackpack* backpack = smart_cast<const CBackpack*>(object))
    {
        params.kind = GWP_ITEM_KIND_BACKPACK;
        params.additional_weight = backpack->m_additional_weight;
        params.additional_weight_max = backpack->m_additional_weight2;
        params.power_loss = backpack->m_fPowerLoss;
    }

    const uint32_t plugin_size = out->size;
    std::memcpy(out, &params, std::min<size_t>(plugin_size, sizeof(params)));
    out->size = plugin_size; // the size field stays the plugin's own
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// P25: debug shapes (group level)
// ---------------------------------------------------------------------------------------------

// The id of a shape of the plugin in world_draw: "plugin:<addon id>:<id>" (the shapes of Lua have no such prefix)
bool DrawId(const GwpPlugin* self, pcstr id, xr_string& out)
{
    if (!id || !*id || xr_strlen(id) > GWP_DRAW_MAX_ID)
        return false;
    for (pcstr p = id; *p; ++p)
    {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    }
    out = "plugin:";
    out += PluginAddonId(self);
    out += ':';
    out += id;
    return true;
}

bool Finite3(const float* v) { return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
Fvector Vec(const float* v) { return Fvector().set(v[0], v[1], v[2]); }

// The checks of every draw_*: GWP_OK and the id of world_draw, or the code
GwpResult DrawCall(const GwpPlugin* self, pcstr function, pcstr id, bool values_ok, xr_string& shape_id)
{
    if (!api::CheckPluginMutation(self, kLevelGroup, function))
        return api::PluginCallRefused(self);
    if (!DrawId(self, id, shape_id) || !values_ok)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!g_pGameLevel)
        return GWP_ERROR_INVALID_STATE;
    return GWP_OK;
}

// world_draw answers false for the limit of shapes (and logs it): the values were checked before
GwpResult DrawResult(bool stored) { return stored ? GWP_OK : GWP_ERROR_INVALID_STATE; }

GwpResult GWP_CALL ApiDrawLine(const GwpPlugin* self, const char* id, const float a[3], const float b[3], uint32_t color)
{
    xr_string shape;
    const GwpResult check = DrawCall(self, "draw_line", id, Finite3(a) && Finite3(b), shape);
    return check != GWP_OK ? check : DrawResult(world_draw::Line(shape.c_str(), Vec(a), Vec(b), color));
}

GwpResult GWP_CALL ApiDrawCircle(const GwpPlugin* self, const char* id, const float center[3], float radius,
    uint32_t color, uint32_t segments)
{
    xr_string shape;
    const GwpResult check = DrawCall(self, "draw_circle", id,
        Finite3(center) && std::isfinite(radius) && radius > 0.f, shape);
    return check != GWP_OK ? check :
                             DrawResult(world_draw::Circle(shape.c_str(), Vec(center), radius, color, segments));
}

GwpResult GWP_CALL ApiDrawSphere(const GwpPlugin* self, const char* id, const float center[3], float radius,
    uint32_t color)
{
    xr_string shape;
    const GwpResult check = DrawCall(self, "draw_sphere", id,
        Finite3(center) && std::isfinite(radius) && radius > 0.f, shape);
    return check != GWP_OK ? check : DrawResult(world_draw::Sphere(shape.c_str(), Vec(center), radius, color));
}

GwpResult GWP_CALL ApiDrawBox(const GwpPlugin* self, const char* id, const float center[3], const float half[3],
    const float hpb[3], uint32_t color)
{
    xr_string shape;
    const GwpResult check =
        DrawCall(self, "draw_box", id, Finite3(center) && Finite3(half) && (!hpb || Finite3(hpb)), shape);
    if (check != GWP_OK)
        return check;
    const Fvector turn = hpb ? Vec(hpb) : Fvector().set(0.f, 0.f, 0.f);
    return DrawResult(world_draw::Box(shape.c_str(), Vec(center), Vec(half), turn, color));
}

GwpResult GWP_CALL ApiDrawArrow(const GwpPlugin* self, const char* id, const float from[3], const float to[3],
    uint32_t color, float head)
{
    xr_string shape;
    const GwpResult check =
        DrawCall(self, "draw_arrow", id, Finite3(from) && Finite3(to) && std::isfinite(head) && head >= 0.f, shape);
    return check != GWP_OK ? check :
                             DrawResult(world_draw::Arrow(shape.c_str(), Vec(from), Vec(to), color, head));
}

GwpResult GWP_CALL ApiDrawRemove(const GwpPlugin* self, const char* id)
{
    if (!api::CheckPluginMutation(self, kLevelGroup, "draw_remove"))
        return api::PluginCallRefused(self);
    if (!id)
    {
        const xr_string prefix = xr_string("plugin:") + PluginAddonId(self) + ":";
        world_draw::RemovePrefix(prefix.c_str());
        return GWP_OK;
    }
    xr_string shape;
    if (!DrawId(self, id, shape))
        return GWP_ERROR_INVALID_ARGUMENT;
    return world_draw::Remove(shape.c_str()) ? GWP_OK : GWP_ERROR_NOT_FOUND;
}

// ---------------------------------------------------------------------------------------------
// P26: input (group level)
// ---------------------------------------------------------------------------------------------

int KeyDown(int dik)
{
    // 0 is SDL_SCANCODE_UNKNOWN; a negative code would index the key array of CInput below its start
    return dik > 0 && pInput && pInput->iGetAsyncKeyState(dik) ? 1 : 0;
}

int GWP_CALL ApiInputKeyDown(int dik)
{
    if (!ReadAllowed(kLevelGroup, "input_key_down"))
        return 0;
    return KeyDown(dik);
}

// The key of an action: a game action (bind) or an action of an addon key group; -1 none
int ActionKey(pcstr action, u32 index)
{
    if (!action || !*action || index >= bindtypes_count)
        return -1;
    const EGameActions id = ActionNameToId(action, true);
    if (id < kLASTACTION)
    {
        const int dik = GetActionDik(id, static_cast<int>(index));
        return dik > 0 ? dik : -1;
    }
    const keyboard_key* key = AddonKeyGet(action, static_cast<int>(index));
    return key && key->dik > 0 ? key->dik : -1;
}

int GWP_CALL ApiInputActionDown(const char* action)
{
    if (!ReadAllowed(kLevelGroup, "input_action_down"))
        return 0;
    for (u32 index = 0; index < bindtypes_count; ++index)
    {
        const int dik = ActionKey(action, index);
        if (dik > 0 && KeyDown(dik))
            return 1;
    }
    return 0;
}

int GWP_CALL ApiInputActionKey(const char* action, uint32_t index)
{
    if (!ReadAllowed(kLevelGroup, "input_action_key"))
        return -1;
    return ActionKey(action, index);
}

// ---------------------------------------------------------------------------------------------
// P47: level_objects_in_radius (group level)
// ---------------------------------------------------------------------------------------------

uint32_t GWP_CALL ApiLevelObjectsInRadius(const float center[3], float radius, const char* class_id, GwpObjectId* out,
    uint32_t cap)
{
    if (!ReadAllowed(kLevelGroup, "level_objects_in_radius") || !g_pGameLevel || !Finite3(center) ||
        !std::isfinite(radius) || radius <= 0.f)
        return 0;
    // The class as object_class_id spells it: up to 8 characters (CLASS_ID pads them with blanks)
    CLASS_ID clsid = 0;
    if (class_id && *class_id)
    {
        if (xr_strlen(class_id) > 8)
            return 0;
        clsid = TEXT2CLSID(class_id);
    }
    // The search of the Lua level.objects_in_radius (engine gaps P47, addon_api_level.cpp): nearest first, the items
    // inside an inventory left out (their position is the owner's)
    xr_vector<u16> found;
    LevelObjectsInRadius(Vec(center), radius, clsid, -1, false, found);
    if (out && cap)
    {
        const size_t copied = std::min<size_t>(found.size(), cap);
        for (size_t i = 0; i < copied; ++i)
            out[i] = static_cast<GwpObjectId>(found[i]);
    }
    return static_cast<uint32_t>(found.size());
}

// ---------------------------------------------------------------------------------------------
// P51: character_start_dialog (group character)
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiCharacterStartDialog(const GwpPlugin* self, GwpObjectId npc, int disable_break)
{
    constexpr pcstr function = "character_start_dialog";
    if (!api::CheckPluginMutation(self, "character", function))
        return api::PluginCallRefused(self);
    CActor* actor = smart_cast<CActor*>(g_actor);
    if (!g_pGameLevel || !actor || !actor->g_Alive())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no living actor");
    CGameObject* object = api::FindOnlineObject(npc);
    if (!object)
        return api::MissingObjectCode(npc);
    CInventoryOwner* partner = smart_cast<CInventoryOwner*>(object);
    if (!partner || object == actor)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (actor->IsTalking())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "the actor talks already");
    // CActor::RunTalkDialog takes the flag of the partner (its disable_break is not used): set for this start only,
    // the talk window copies it when it opens
    const bool old_disable_break = partner->bDisableBreakDialog;
    if (disable_break)
        partner->bDisableBreakDialog = true;
    actor->RunTalkDialog(partner, disable_break != 0);
    partner->bDisableBreakDialog = old_disable_break;
    return actor->IsTalking() ? GWP_OK :
                                api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "the character refused");
}

// ---------------------------------------------------------------------------------------------
// P54: light and visibility (group npc)
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiObjectLuminosity(GwpObjectId id, float* out)
{
    if (out)
        *out = 0.f;
    if (!ReadAllowed("npc", "object_luminosity"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!out)
        return GWP_ERROR_INVALID_ARGUMENT;
    CGameObject* object = api::FindOnlineObject(id);
    if (!object)
        return api::MissingObjectCode(id) == GWP_ERROR_INVALID_STATE ? GWP_ERROR_INVALID_STATE : GWP_ERROR_NOT_FOUND;
    IRender_ObjectSpecific* ros = object->renderable_ROS();
    if (!ros)
        return GWP_ERROR_INVALID_STATE;
    *out = ros->get_luminocity();
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcVisibilityOf(GwpObjectId npc, GwpObjectId target, float* out)
{
    if (out)
        *out = 0.f;
    if (!ReadAllowed("npc", "npc_visibility_of"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!out)
        return GWP_ERROR_INVALID_ARGUMENT;
    CGameObject* npc_object = api::FindOnlineObject(npc);
    CGameObject* target_object = api::FindOnlineObject(target);
    if (!npc_object || !target_object)
        return g_pGameLevel ? GWP_ERROR_NOT_FOUND : GWP_ERROR_INVALID_STATE;
    CCustomMonster* creature = smart_cast<CCustomMonster*>(npc_object);
    if (!creature)
        return GWP_ERROR_INVALID_ARGUMENT;
    const CVisualMemoryManager& visual = creature->memory().visual();
    if (visual.visible_now(target_object))
    {
        *out = 1.f;
        return GWP_OK;
    }
    const float threshold = visual.visibility_threshold();
    for (const CNotYetVisibleObject& entry : visual.not_yet_visible_objects())
    {
        if (entry.m_object == target_object)
        {
            // The ratio of the stealth HUD (visual_memory_manager.cpp, SetActorVisibility)
            *out = threshold > 0.f ? clampr(entry.m_value / threshold, 0.f, 1.f) : 0.f;
            break;
        }
    }
    return GWP_OK;
}
} // namespace

bool BeforeGameSave(pcstr name, pcstr kind)
{
    if (!events::HasSubscribers(events::EBuiltin::GameOnBeforeSave))
        return true;
    const GwpValue args[] = { events::String(name ? name : ""), events::String(kind ? kind : "save") };
    GwpValue result = events::Bool(true);
    events::Emit(events::EBuiltin::GameOnBeforeSave, args, static_cast<u32>(std::size(args)), &result);
    // false cancels; anything else (true, nil, a number) lets the save go
    const bool allowed = !(result.type == GWP_T_BOOL && result.u.b == 0);
    if (!allowed)
        Msg("~ [events] save '%s' (%s) cancelled by a subscriber of game_on_before_save", name ? name : "",
            kind ? kind : "save");
    return allowed;
}

void RemovePluginResources(const GwpPlugin* plugin)
{
    if (!plugin)
        return;
    const xr_string prefix = xr_string("plugin:") + PluginAddonId(plugin) + ":";
    world_draw::RemovePrefix(prefix.c_str());
}

void FillEngineApi(GwpEngineApi& api)
{
    api.fs_stat = &ApiFsStat;
    api.fs_read = &ApiFsRead;
    api.fs_list = &ApiFsList;
    api.game_save = &ApiGameSave;
    api.item_params = &ApiItemParams;
    api.draw_line = &ApiDrawLine;
    api.draw_circle = &ApiDrawCircle;
    api.draw_sphere = &ApiDrawSphere;
    api.draw_box = &ApiDrawBox;
    api.draw_arrow = &ApiDrawArrow;
    api.draw_remove = &ApiDrawRemove;
    api.input_key_down = &ApiInputKeyDown;
    api.input_action_down = &ApiInputActionDown;
    api.input_action_key = &ApiInputActionKey;
    api.level_objects_in_radius = &ApiLevelObjectsInRadius;
    api.character_start_dialog = &ApiCharacterStartDialog;
    api.object_luminosity = &ApiObjectLuminosity;
    api.npc_visibility_of = &ApiNpcVisibilityOf;
}
} // namespace gw::addons::extra
