#pragma once

#include "xrCore/vector.h"
#include "xrCore/xrstring.h"
#include "xrCommon/xr_vector.h"

enum class EUiMapClick : u8
{
    Left = 0,
    Right
};

struct SMapPointDesc
{
    shared_str level_name;
    Fvector position{};
    shared_str spot_type;
    shared_str hint_text;
    u32 logical_id = 0;
    u32 flags = 0;

    // Optional editor-oriented payload.
    shared_str section_name;
    shared_str smart_name;
    shared_str display_name;
    shared_str smart_type;
    shared_str owner_faction;
    shared_str icon_texture;
    u32 icon_color = 0xFFFFFFFF;
};

class IMapDataSource
{
public:
    virtual ~IMapDataSource() = default;

    virtual void Reload() = 0;
    virtual void EnumeratePoints(xr_vector<SMapPointDesc>& out) const = 0;
    virtual u32 GetDataRevision() const { return 0; }
    virtual bool GetFocusLevel(shared_str& outLevel) const { return false; }
    virtual void OnMapClick(u32 logical_id, EUiMapClick clickType) {}
};
