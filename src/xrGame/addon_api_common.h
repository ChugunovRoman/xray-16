#pragma once

// Plugin API: helpers shared by the addon_api_*.cpp files (plan 26, 1.6). Inline only: a unity build puts several of
// these files into one translation unit, so a helper defined once here cannot clash there.
//  - a function that changes the game takes `const GwpPlugin* self` first: the log of a refused call names the plugin;
//  - the codes: outside the main thread GWP_ERROR_NOT_MAIN_THREAD; self == NULL, a bad argument, an object of another
//    class GWP_ERROR_INVALID_ARGUMENT; no such object online GWP_ERROR_NOT_FOUND; not possible now (no level, a dead
//    NPC) GWP_ERROR_INVALID_STATE.

#include "addon_host.h"
#include "GameObject.h"
#include "Level.h"
#include "xrCore/clsid.h"

#include <cstring>

namespace gw::addons::api
{
// A call that changes the game: the main thread and a plugin handle. false (logged with the plugin) otherwise.
inline bool CheckPluginMutation(const GwpPlugin* self, pcstr group, pcstr function)
{
    if (!self)
    {
        Msg("! [%s] %s called without the plugin handle (self), ignored", group, function);
        return false;
    }
    if (IsMainThread())
        return true;
    Msg("! [plugin:%s] [%s] %s called outside the main thread, ignored", PluginAddonId(self), group, function);
    return false;
}

// The code of a call CheckPluginMutation refused: NOT_MAIN_THREAD or INVALID_ARGUMENT (no self)
inline GwpResult PluginCallRefused(const GwpPlugin* self)
{
    return self && !IsMainThread() ? GWP_ERROR_NOT_MAIN_THREAD : GWP_ERROR_INVALID_ARGUMENT;
}

// A refused change: logged with the plugin when the debug log is on (-addon_debug), `code` returned
inline GwpResult PluginRefusal(const GwpPlugin* self, pcstr function, GwpResult code, pcstr why)
{
    if (IsDebugLog())
        Msg("~ [plugin:%s] %s: %s", PluginAddonId(self), function, why);
    return code;
}

// An online object of the level that is not being destroyed, or nullptr
inline CGameObject* FindOnlineObject(GwpObjectId id)
{
    if (!g_pGameLevel || id == GWP_INVALID_OBJECT_ID)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object && !object->getDestroy() ? object : nullptr;
}

// The same, of the class T (nullptr for another class)
template <typename T>
T* FindOnline(GwpObjectId id)
{
    return smart_cast<T*>(FindOnlineObject(id));
}

// The code for an object of class T that was not found: NOT_FOUND when nothing with the id is online,
// INVALID_ARGUMENT when it is online but of another class
inline GwpResult MissingObjectCode(GwpObjectId id)
{
    if (!g_pGameLevel)
        return GWP_ERROR_INVALID_STATE;
    return FindOnlineObject(id) ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_FOUND;
}

// object_class_id / alife_object_class_id: the class id as text without the padding spaces CLSID2TEXT adds ("AI_RAT  "
// -> "AI_RAT"), copied into out zero-terminated and cut to cap - 1 (out may be NULL), the length of the full text
// returned
inline uint32_t CopyClassIdText(CLASS_ID class_id, char* out, uint32_t cap)
{
    char text[GWP_CLASS_ID_SIZE];
    static_assert(GWP_CLASS_ID_SIZE == 9, "CLSID2TEXT writes 8 characters and the terminator");
    CLSID2TEXT(class_id, text);
    uint32_t length = GWP_CLASS_ID_SIZE - 1;
    while (length && text[length - 1] == ' ')
        --length;
    if (out && cap)
    {
        const uint32_t copied = length < cap - 1 ? length : cap - 1;
        std::memcpy(out, text, copied);
        out[copied] = '\0';
    }
    return length;
}
} // namespace gw::addons::api
