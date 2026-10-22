#include "stdafx.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "IGame_ObjectPool.h"
#include "xr_object.h"
#include "xrServerEntities/smart_cast.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <unordered_map>

namespace
{
// 40-parallel-prefetch.md (plans/optimization_mt): the visuals the Load of the prefetch objects creates
// (CGameObject::Load -> cNameVisual_set: `visual` without extension in lower case, `model_cache_suffix` of the section),
// each once, in the order of its first section - the order the loads in Load would go. models_PrefetchList loads them
// on the load threads of the model prefetch (R4) and Load then takes them from the pool. A visual whose first section
// has world_material_N lines is left to Load: cNameVisual_set registers the texture replacements of the visual
// (keys "<visual>:<child><suffix>") just before it creates it. Only the sections of the visual register them, so for
// the other visuals nothing is registered before the load either way. A visual Load does not create (another class)
// is just loaded into the pool earlier - the model prefetch list loads every mesh anyway.
// 50a-stage1-prefetch.md: the sections taken by the prefetch budget, in the priority order; a visual gets the size
// estimates of its sections (the warm-up lead of the load threads counts them).
// 50k-slow-disk-early-prefetch.md: with_material_visuals - the visuals left to Load as well (a file warm-up only).
void collect_object_visuals(
    const xr_vector<PrefetchListItem>& sections, xr_vector<PrefetchModel>& visuals, bool with_material_visuals)
{
    std::unordered_map<xr_string, size_t> seen; // key -> index into visuals, or size_t(-1) (left to Load)
    for (const PrefetchListItem& item : sections)
    {
        pcstr section = item.name.c_str();
        if (!pSettings->section_exist(section) || !pSettings->line_exist(section, "visual"))
            continue;
        pcstr visual = pSettings->r_string(section, "visual");
        if (!visual || !visual[0] || xr_strlen(visual) >= sizeof(string_path))
            continue;
        string_path name;
        xr_strcpy(name, visual);
        if (strext(name))
            *strext(name) = 0;
        xr_strlwr(name);
        if (!name[0])
            continue;
        pcstr suffix = "";
        if (pSettings->line_exist(section, "model_cache_suffix")) // as read_if_exists<pcstr> in cNameVisual_set
            suffix = pSettings->r_string(section, "model_cache_suffix");
        if (!suffix)
            suffix = "";
        xr_string key(name);
        key += '|';
        key += suffix;
        if (const auto it = seen.find(key); it != seen.end())
        {
            if (it->second != size_t(-1))
                visuals[it->second].bytes += item.bytes;
            continue;
        }
        bool materials = false;
        for (const auto& line : pSettings->r_section(section).Data)
        {
            if (line.first.size() && 0 == strncmp(line.first.c_str(), "world_material_", 15))
            {
                materials = true;
                break;
            }
        }
        const bool left_to_load = materials && !with_material_visuals;
        seen.emplace(key, left_to_load ? size_t(-1) : visuals.size());
        if (!left_to_load)
            visuals.push_back({ shared_str(name), shared_str(suffix), item.bytes });
    }
}
} // namespace

void PrefetchObjectVisuals(
    const xr_vector<PrefetchListItem>& sections, xr_vector<PrefetchModel>& visuals, bool with_material_visuals)
{
    collect_object_visuals(sections, visuals, with_material_visuals);
}

IGame_ObjectPool::IGame_ObjectPool(void) {}
IGame_ObjectPool::~IGame_ObjectPool(void) { R_ASSERT(m_PrefetchObjects.empty()); }
void IGame_ObjectPool::prefetch()
{
    R_ASSERT(m_PrefetchObjects.empty());

    int p_count = 0;
    GEnv.Render->model_Logging(false);

    string256 section;
    // prefetch objects
    strconcat(sizeof(section), section, "prefetch_objects_", g_pGamePersistent->m_game_params.m_game_type);
    // 50a-stage1-prefetch.md (plans/optimization_mt): in the priority order of the list, the objects of the prefix of
    // the priority order over both lists the prefetch budget allows (plan 50 R1; PrefetchMode::Full - all of them)
    xr_vector<PrefetchListItem> sections;
    PrefetchListRead(section, sections);
    PrefetchListTake("objects", sections);

    // 40-parallel-prefetch.md (plans/optimization_mt): their visuals first, on the load threads (R4)
    {
        xr_vector<PrefetchModel> visuals;
        collect_object_visuals(sections, visuals, false);
        GEnv.Render->models_PrefetchList(visuals);
    }

    for (const PrefetchListItem& item : sections)
    {
        CLASS_ID CLS = pSettings->r_clsid(item.name.c_str(), "class");
        p_count++;
        IGameObject* pObject = smart_cast<IGameObject*>(NEW_INSTANCE(CLS));
        pObject->Load(item.name.c_str());
        VERIFY2(pObject->cNameSect().c_str(), item.name.c_str());
        m_PrefetchObjects.push_back(pObject);
    }

    // out statistic
    GEnv.Render->model_Logging(true);
}

void IGame_ObjectPool::clear()
{
    // Clear POOL
    for (auto& it : m_PrefetchObjects)
        xr_delete(it);
    m_PrefetchObjects.clear();
}

IGameObject* IGame_ObjectPool::create(pcstr name)
{
    // Spawn stage 0 (plans/spawn_optimization/SPEC.md): CObjectList::Create of a client spawn by step, spawn/create/*
    CLASS_ID CLS = pSettings->r_clsid(name, "class");
    IGameObject* O = nullptr;
    {
        PERF_STEP("object_pool/new_instance", spawn_create_new);
        O = smart_cast<IGameObject*>(NEW_INSTANCE(CLS));
    }
    O->cNameSect_set(name);
    {
        PERF_STEP("object_pool/load", spawn_create_load);
        ZoneText(name, xr_strlen(name));
        O->Load(name);
    }
    {
        PERF_STEP("object_pool/post_load", spawn_create_post_load);
        O->PostLoad(name); //--#SM+#--
    }
    return O;
}

void IGame_ObjectPool::destroy(IGameObject* O) { xr_delete(O); }
/*
void IGame_ObjectPool::prefetch ()
{
R_ASSERT (map_POOL.empty());

u32 mem_0 = Memory.mem_usage();
float p_time = 1000.f*Device.GetTimerGlobal()->GetElapsed_sec();
int p_count = 0;
GlobalEnv.Render->model_Logging (false);

string256 section;
// prefetch objects
strconcat (section,"prefetch_objects_",g_pGamePersistent->m_game_params.m_game_type);
CInifile::Sect& sect = pSettings->r_section(section);
for (CInifile::SectIt I=sect.begin(); I!=sect.end(); I++) {
CInifile::Item& item= *I;
CLASS_ID CLS = pSettings->r_clsid(item.first.c_str(),"class");
int count = atoi(item.second.c_str());
count += (count==0)?1:0;
R_ASSERT2 ((count>0) && (count<=128), "Too many objects for prefetching");
p_count += count;
for (int c=0; c<count; c++){
IGameObject* pObject= (IGameObject*) NEW_INSTANCE(CLS);
pObject->Load (item.first.c_str());
VERIFY2 (pObject->cNameSect().c_str(),item.first.c_str());
map_POOL.insert (std::make_pair(pObject->cNameSect(),pObject));
}
}

// out statistic
GlobalEnv.Render->model_Logging (true);
p_time = 1000.f*Device.GetTimerGlobal()->GetElapsed_sec() - p_time;
u32 p_mem = Memory.mem_usage() - mem_0;
if (p_count){
float a_time = float(p_time)/float(p_count);
Msg ("* [Object-prefetch] objects: %d", p_count);
Msg ("* [Object-prefetch] time: %d ms", iFloor(p_time));
Msg ("* [Object-prefetch] memory: %dKb", p_mem/1024);
Msg ("* [Object-prefetch] average: %2.2f ms, %d bytes", a_time, p_mem/p_count);
}
}

void IGame_ObjectPool::clear ( )
{
// Clear POOL
for (POOL_IT it=map_POOL.begin(); it!=map_POOL.end(); it++)
xr_delete (it->second);
map_POOL.clear();
}

IGameObject* IGame_ObjectPool::create ( pcstr name )
{
string256 l_name;
POOL_IT it = map_POOL.find (shared_str(xr_strlwr(xr_strcpy(l_name,name))));
if (it!=map_POOL.end())
{
// Instance found
IGameObject* O = it->second;
map_POOL.erase (it);
return O;
} else {
// Create and load _new instance
// Msg ("! Uncached loading '%s'...",name);
CLASS_ID CLS = pSettings->r_clsid (name,"class");
IGameObject* O = (IGameObject*) NEW_INSTANCE (CLS);
O->Load (name);
return O;
}
}

void IGame_ObjectPool::destroy ( IGameObject* O )
{
map_POOL.insert (std::make_pair(O->cNameSect(),O));
}
*/
