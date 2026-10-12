#pragma once

#include "xrCore/Profiling/PerfMetrics.hpp"

// P20 (plans/optimization_mt/15-ini-cache.md): who parses ltx while the game runs.
//
// CInifile::Load (xr_ini.cpp) times every outermost parse (a file or a text with all its #include files) and adds it to
// the perf_metrics timers ini/load and ini/load/<layer>, and to a table of sources: layer + file name (or "<text>" for
// a parse from memory) + detail of the caller. Every 5 s window of perf_metrics (bit 2, the log) prints the top 15
// sources by time ("* [ini-load] ..."). Only while perf_metrics is active and only in builds with XR_PERF_METRICS; the
// Tracy zone CInifile::Load gets the file name as its text in every build.
//
// The layer and the detail come from the code that creates the CInifile (INI_SOURCE below): Lua ini_file /
// create_ini_file (detail = the Lua call site), the custom data of an object, a native addon... Untagged = engine.

namespace xray::ini_stats
{
enum class Layer : u8
{
    Engine = 0, // not tagged: engine configs, model user data...
    LuaFile, // Lua ini_file(path) (ini_file_ex too); detail = Lua call site
    LuaString, // Lua create_ini_file(text); detail = Lua call site
    ObjectCustomData, // CGameObject::net_Spawn: custom data of an object going online; detail = section
    ServerCustomData, // CSE_Abstract::spawn_ini: custom data of a server object (once per object); detail = section
    SpawnSupplies, // CSE_ALifeObject::spawn_supplies: custom data of a new object; detail = section
    TraderCustomData, // CSE_ALifeTraderAbstract::set_specific_character: custom data of an NPC; detail = section
    Plugin, // native addon ini_open; detail = addon id
    Count
};

XRCORE_API pcstr LayerName(Layer layer) noexcept;

// Who creates the CInifile on this thread right now. detail must stay valid until the CInifile is constructed.
struct Source
{
    Layer layer{Layer::Engine};
    pcstr detail{nullptr};
};

XRCORE_API Source& TlSource() noexcept;

// Attribution and the table are kept only while this is true (perf_metrics bits 1 or 2, latched once per frame).
inline bool Active() noexcept
{
#if XR_PERF_METRICS
    return xray::perf::g_perf_metrics_active;
#else
    return false;
#endif
}

struct ScopedSource
{
    Source prev;

    ScopedSource(Layer layer, pcstr detail) noexcept : prev(TlSource()) { TlSource() = Source{layer, detail}; }
    ~ScopedSource() noexcept { TlSource() = prev; }

    ScopedSource(const ScopedSource&) = delete;
    ScopedSource& operator=(const ScopedSource&) = delete;
};

// From xray::perf::Flush() at the end of each 5 s window (main thread): print = perf_metrics bit 2. Prints the top 15
// sources of the window and clears the table.
XRCORE_API void WindowReport(bool print) noexcept;
} // namespace xray::ini_stats

// Tags the CInifile created inside the enclosing block. detail: pcstr (may be null), evaluated only in builds with
// XR_PERF_METRICS.
#if XR_PERF_METRICS
#   define INI_SOURCE(layer, detail)                                                                                  \
        ::xray::ini_stats::ScopedSource CONCATENIZE(_ini_source_, __LINE__)(::xray::ini_stats::Layer::layer, (detail))
#else
#   define INI_SOURCE(layer, detail) ((void)0)
#endif
