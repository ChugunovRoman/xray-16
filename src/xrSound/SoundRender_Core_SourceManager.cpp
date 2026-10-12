#include "stdafx.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

CSoundRender_Source* CSoundRender_Core::i_create_source(pcstr name)
{
    // Search
    string256 id;
    xr_strcpy(id, name);
    xr_strlwr(id);
    if (strext(id))
        * strext(id) = 0;

    {
        ScopeLock scope(&s_sources_lock);
        const auto it = s_sources.find(id);
        if (it != s_sources.end())
        {
            return it->second;
        }
    }

    // Load a _new one
    CSoundRender_Source source;
    bool loaded;
    {
        // spawn stage 0 (plans/spawn_optimization/SPEC.md): a new source - LoadWave + PCM cache of a short sound
        ZoneScopedN("snd_source_load");
        ZoneText(id, xr_strlen(id));
        PERF_SCOPE(snd_source_load);
        loaded = source.load(id);
    }
    if (loaded)
    {
        ScopeLock scope(&s_sources_lock);
        CSoundRender_Source* S = xr_new<CSoundRender_Source>(std::move(source));
        s_sources.emplace(id, S);
        return S;
    }

    return nullptr;
}

void CSoundRender_Core::i_destroy_source(CSoundRender_Source* S)
{
    // No actual destroy at all
}
