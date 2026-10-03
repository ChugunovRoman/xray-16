#include "StdAfx.h"

#include "addon_api_common.h"
#include "addon_host.h"

#include "InventoryOwner.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_registry_container.h"
#include "alife_simulator.h"
#include "xrServerEntities/InfoPortionDefs.h"
#include "xrServerEntities/alife_space.h"

#include <algorithm>

namespace gw::addons
{
namespace
{
bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [info] %s called outside the main thread, ignored", function);
    return false;
}

// Portions of a character live in the ALife registry, not in the online object: they are there for an NPC on the
// other side of the map too. This is the same data CInventoryOwner::HasInfo reads.
const KNOWN_INFO_VECTOR* Portions(GwpObjectId owner)
{
    if (owner == GWP_INVALID_OBJECT_ID || !ai().get_alife() || !ai().alife().initialized())
        return nullptr;
    return ai().alife().registry(info_portions).object(owner, true);
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group info)
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiInfoHas(GwpObjectId owner, const char* info)
{
    if (!CheckCall("info_has") || !info || !*info)
        return 0;
    const KNOWN_INFO_VECTOR* portions = Portions(owner);
    if (!portions)
        return 0;
    return std::find_if(portions->begin(), portions->end(), CFindByIDPred(info)) != portions->end() ? 1 : 0;
}

uint64_t GWP_CALL ApiInfoTime(GwpObjectId owner, const char* info)
{
    if (!CheckCall("info_time") || !info || !*info)
        return 0;
    const KNOWN_INFO_VECTOR* portions = Portions(owner);
    if (!portions)
        return 0;
    const auto it = std::find_if(portions->begin(), portions->end(), CFindByIDPred(info));
    return it != portions->end() ? static_cast<uint64_t>(it->receive_time) : 0;
}

uint32_t GWP_CALL ApiInfoCount(GwpObjectId owner)
{
    if (!CheckCall("info_count"))
        return 0;
    const KNOWN_INFO_VECTOR* portions = Portions(owner);
    return portions ? static_cast<uint32_t>(portions->size()) : 0;
}

const char* GWP_CALL ApiInfoAt(GwpObjectId owner, uint32_t index)
{
    if (!CheckCall("info_at"))
        return nullptr;
    const KNOWN_INFO_VECTOR* portions = Portions(owner);
    if (!portions || index >= portions->size())
        return nullptr;
    return (*portions)[index].info_id.c_str();
}

GwpResult Transfer(const GwpPlugin* self, GwpObjectId owner, const char* info, bool give, pcstr function)
{
    if (!api::CheckPluginMutation(self, "info", function))
        return api::PluginCallRefused(self);
    if (!info || !*info)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "no info portion name");
    // The owner as an online character: giving a portion runs its scripted effects, which needs the object itself.
    // Offline there is nobody to run them for (NOT_FOUND), an online object of another class is no character
    // (INVALID_ARGUMENT)
    CInventoryOwner* character = api::FindOnline<CInventoryOwner>(owner);
    if (!character)
    {
        const GwpResult code = api::MissingObjectCode(owner);
        if (IsDebugLog())
            Msg("~ [plugin:%s] %s: object %u is %s", PluginAddonId(self), function, static_cast<u32>(owner),
                code == GWP_ERROR_INVALID_ARGUMENT ? "not a character" : "not online");
        return code;
    }
    // TransferInfo is the path Lua takes: it sends the network message and runs the effects of the portion.
    character->TransferInfo(info, give);
    return GWP_OK;
}

GwpResult GWP_CALL ApiInfoGive(const GwpPlugin* self, GwpObjectId owner, const char* info)
{
    return Transfer(self, owner, info, true, "info_give");
}

GwpResult GWP_CALL ApiInfoDisable(const GwpPlugin* self, GwpObjectId owner, const char* info)
{
    return Transfer(self, owner, info, false, "info_disable");
}
} // namespace

void FillInfoApi(GwpEngineApi& api)
{
    api.info_has = &ApiInfoHas;
    api.info_give = &ApiInfoGive;
    api.info_disable = &ApiInfoDisable;
    api.info_time = &ApiInfoTime;
    api.info_count = &ApiInfoCount;
    api.info_at = &ApiInfoAt;
}
} // namespace gw::addons
