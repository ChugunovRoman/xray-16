#pragma once

#include <tracy/Tracy.hpp>

#include "xrDebug.h"
//#include "vector.h"

#include "clsid.h"
//#include "Threading/Lock.hpp"
#include "xrMemory.h"

//#include "_stl_extensions.h"
#include "_std_extensions.h"
#include "_rect.h"
#include "_matrix.h"
#include "xrCommon/xr_vector.h"
#include "xrCommon/xr_set.h"
#include "xrsharedmem.h"
#include "xrstring.h"
#include "xr_resource.h"
#include "Compression/rt_compressor.h"
#include "xr_shared.h"
#include "string_concatenations.h"
#include "_flags.h"

#include "xr_shortcut.h"

#include "FS.h"
#include "log.h"
#include "xr_trims.h"
#include "xr_ini.h"
#ifdef NO_FS_SCAN
#include "ELocatorAPI.h"
#else
#include "LocatorAPI.h"
#endif
#include "FileSystem.h"
#include "FTimer.h"
#include "fastdelegate.h"
#ifdef XR_PLATFORM_WINDOWS
#include "intrusive_ptr.h"
#endif

#include "net_utils.h"
#include "Threading/ThreadUtil.h"

enum ESectionTypeName : u32
{
    ammo = u32(0),
    knife,
    pistol,
    auto_pistol,
    shotgun,
    rifle,
    sniper_rifle,
    heavy_rifle,
    explosive,
    scopes,
    silencers,
    launchers,
    outfit,
    artefact,
    item_quest,
    item_misc,
    item_consumable,
    item_medical,
    item_food,
    npc,
    mutant,
    squad_npc,
    squad_mutant,
    vehicle,
    physic,
    models,
    anomaly,
    phantom,
    backpack,

    latest,
};

class XRCORE_API xrDbg
{
    xr_vector<pcstr> sections_map[0xff];

public:
    xrDbg();

    xr_vector<pcstr> GetSections(ESectionTypeName type);

    void ClearAll();
    void InitSectionLists();
    bool IsParentSection(const shared_str section);

};

extern XRCORE_API xrDbg Dbg;
