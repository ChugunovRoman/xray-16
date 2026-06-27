#include "pch_script.h"
#include "VisionBatch.h"
#include "CustomMonster.h"

#include "xrCDB/xr_area.h"
#include "xrCDB/xrXRC.h"
#include "xrCDB/ISpatial.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrMaterialSystem/GameMtlLib.h"

#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCore/Threading/TaskManager.hpp"

#include "performance_cvars.h"

VisionBatch g_vision_batch;

void VisionBatch::Reset()
{
    m_rays.clear();
    m_entries.clear();
    m_current_npc = nullptr;
}

u32 VisionBatch::BeginEntry(CCustomMonster* npc)
{
    m_current_npc = npc;
    VisionBatchEntry e;
    e.npc = npc;
    e.first_ray = static_cast<u32>(m_rays.size());
    e.ray_count = 0;
    m_entries.push_back(e);
    return static_cast<u32>(m_entries.size() - 1);
}

u32 VisionBatch::AddRay(const Fvector& start, const Fvector& dir, float range)
{
    VisionBatchRay r;
    r.start = start;
    r.dir = dir;
    r.range = range;
    r.accumulated_vis = 1.f;
    r.static_hit_opaque = false;
    r.blocking_verts[0].set(0, 0, 0);
    r.blocking_verts[1].set(0, 0, 0);
    r.blocking_verts[2].set(0, 0, 0);
    r.blocking_element = -1;
    m_rays.push_back(r);
    return static_cast<u32>(m_rays.size() - 1);
}

void VisionBatch::EndEntry()
{
    if (m_entries.empty())
        return;
    VisionBatchEntry& e = m_entries.back();
    e.ray_count = static_cast<u32>(m_rays.size()) - e.first_ray;
    if (e.ray_count == 0)
        m_entries.pop_back(); // NPC had no rays — remove entry
    m_current_npc = nullptr;
}

void VisionBatch::AbortEntry()
{
    if (m_entries.empty())
        return;
    VisionBatchEntry& e = m_entries.back();
    m_rays.resize(e.first_ray); // discard rays
    m_entries.pop_back();
    m_current_npc = nullptr;
}

namespace
{

thread_local xrXRC g_vision_batch_xrc("vision batch");

inline float get_static_mtl_transp(const CDB::TRI& T)
{
    return GMLib.GetMaterialByIdx(T.material)->fVisTransparencyFactor;
}

// Cached pointers for the duration of ExecuteParallel — prevents dangling access
// if level unloads or static model rebuilds mid-batch on another thread.
CDB::MODEL* g_cached_static_model = nullptr;
CDB::TRI*   g_cached_static_tris  = nullptr;
Fvector*    g_cached_static_verts  = nullptr;

void execute_one_ray(VisionBatchRay& ray)
{
    CDB::TRI* tris = g_cached_static_tris;
    Fvector* verts = g_cached_static_verts;
    g_vision_batch_xrc.ray_query(CDB::OPT_CULL, g_cached_static_model,
        ray.start, ray.dir, ray.range);

    float vis = 1.f;
    for (int i = 0; i < g_vision_batch_xrc.r_count(); ++i)
    {
        const CDB::RESULT& R = g_vision_batch_xrc.r_begin()[i];
        const float mtl_vis = get_static_mtl_transp(tris[R.id]);
        vis *= mtl_vis;

        if (fis_zero(mtl_vis))
        {
            ray.static_hit_opaque = true;
            ray.blocking_element = static_cast<int>(R.id);
            ray.blocking_verts[0].set(verts[tris[R.id].verts[0]]);
            ray.blocking_verts[1].set(verts[tris[R.id].verts[1]]);
            ray.blocking_verts[2].set(verts[tris[R.id].verts[2]]);
            break;
        }
    }
    ray.accumulated_vis = vis;
}

} // namespace

void VisionBatch::ExecuteParallel()
{
    if (m_rays.empty())
        return;

    // Cache static model pointers before parallel execution to prevent dangling access
    // if level unloads or static model rebuilds on another thread mid-batch.
    g_cached_static_model = g_pGameLevel->ObjectSpace.GetStaticModel();
    g_cached_static_tris  = g_pGameLevel->ObjectSpace.GetStaticTris();
    g_cached_static_verts  = g_pGameLevel->ObjectSpace.GetStaticVerts();

    if (npc_perf_vision_parallel_batch && TaskScheduler
        && static_cast<int>(m_rays.size()) >= npc_perf_vision_parallel_batch_min_rays)
    {
        xr_parallel_for(TaskRange(size_t(0), m_rays.size()),
            [this](const TaskRange<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                    execute_one_ray(m_rays[i]);
            });
    }
    else
    {
        for (size_t i = 0; i < m_rays.size(); ++i)
            execute_one_ray(m_rays[i]);
    }

    g_cached_static_model = nullptr;
    g_cached_static_tris  = nullptr;
    g_cached_static_verts  = nullptr;
}
