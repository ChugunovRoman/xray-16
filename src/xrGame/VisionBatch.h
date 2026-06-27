#pragma once

#include "xrCore/_vector3d.h"
#include "xrCommon/xr_vector.h"

class CCustomMonster;

struct VisionBatchRay
{
    Fvector start, dir;
    float range;
    float accumulated_vis;      // [0..1] accumulated transparency from static CDB
    bool static_hit_opaque;     // completely blocked by opaque static geometry
    Fvector blocking_verts[3];  // vertices of the blocking static triangle
    int blocking_element;       // triangle index in static CDB (-1 = none)
};

struct VisionBatchEntry
{
    CCustomMonster* npc;
    u32 first_ray;
    u32 ray_count;
};

class VisionBatch
{
    xr_vector<VisionBatchRay> m_rays;
    xr_vector<VisionBatchEntry> m_entries;
    CCustomMonster* m_current_npc{};

public:
    void Reset();
    u32 BeginEntry(CCustomMonster* npc);
    u32  AddRay(const Fvector& start, const Fvector& dir, float range);
    void EndEntry();
    void AbortEntry(); // NPC added no rays — discard the entry

    void ExecuteParallel();

    u32  GetEntryCount() const { return static_cast<u32>(m_entries.size()); }
    const VisionBatchEntry& GetEntry(u32 i) const { return m_entries[i]; }
    u32  GetRayCount() const { return static_cast<u32>(m_rays.size()); }
    const VisionBatchRay& GetRay(u32 i) const { return m_rays[i]; }
    VisionBatchRay& GetRay(u32 i) { return m_rays[i]; }
};

extern VisionBatch g_vision_batch;
