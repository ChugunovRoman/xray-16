#pragma once

// U1 (plans/optimization_mt/32-frame-spikes.md; diagnostics, temporary, not saved): who creates the temporary userdata
// of luabind. The atomic phase of the Lua GC is ~80 % userdata, 75-85 % of them dead (0.5-1.1 million a GC cycle):
// the wrappers game_object / render_device / alife_simulator and the value copies vector / CTime that luabind makes
// on every pass of a C++ object to Lua. This finds the Lua call sites (source:line) that make them.
//
// luabind (Externals/luabind, a local patch of the submodule: object_rep.hpp/.cpp) calls a hook from
// push_new_instance() - the one place where every class instance is born (make_pointer_instance,
// make_value_instance, class_rep::constructor_dispatcher). Without the hook (lua_udata_profile 0) it pays one load
// and a branch per instance.
//
// lua_udata_profile 1: a new window (counters and samples reset), the hook installed.
//  - every instance: +1 to the exact counter of its class (a lock-free table by class_rep*), perf_metrics
//    lua/udata/created and the hot classes (lua/udata/game_object, vector, CTime, render_device, alife_simulator);
//  - one instance in lua_udata_profile_sample (random interval, mean N, per thread) is sampled: the kind
//      new    - a constructor called from Lua (vector(), CTime(), a Lua class),
//      c      - pushed by a C function that runs under Lua (a return value: obj:position(), level.object_by_id();
//               or an argument of a callback the C function called),
//      engine - pushed with no Lua function running (the engine calls Lua: binder methods, object callbacks),
//    the name of that C function as its Lua caller sees it (lua_getinfo "n"), the first Lua frame (site,
//    source:currentline), the next one (caller), and the Lua function the engine is about to call (cb, source:linedefined:
//    the nearest Lua function in the last 12 slots of the stack - luabind pushes the function, then the arguments).
//    Aggregated by (class, kind, C function, site, caller, cb) under a mutex.
// lua_udata_report [reset] [<class>] [top=N]: "[lua-udata]" lines in the log - the window, created by class (exact,
//    per second), the top N sites over all classes and the top N of each big class (or of <class> only), each site with
//    its callers. Estimate of a site = created of its class * its share of the samples of the class. reset - a new
//    window after the report.
//
// U2 (32-frame-spikes.md): the luabind instances turned out to be ~5-10 % of the dead userdata of a GC cycle. So
// lua_udata_profile 1 also installs the LuaJIT hook luaJIT_gw_udata_hook (a local patch of Externals/LuaJIT, luajit.h;
// compiled in by LUAJIT_GW_UDATA_HOOK) that sees every new userdata in lj_udata_new: exact count, bytes and payload
// sizes, and one in lua_udata_profile_sample is sampled - the C function at level 0 (address by luaJIT_gw_cfunc and the
// Lua name), the first two Lua frames, on Windows the 4 native callers after the LuaJIT frames (RtlCaptureStackBackTrace,
// named in the report by xrDebug::SymbolizeAddresses). lua_udata_report adds the "[lua-udata-all]" section: all
// userdata created vs the luabind instances, by payload size (with the creators of each size), by C function, by native
// caller, the top Lua sites with callers. lua_udata_report all - only that section.
//
// Lua runs on the Game thread or on main, not on both at once; the counters are atomic, the sample state is per thread.
// After a game load the class_rep of the new Lua state may reuse an address of the old one: reset the window.

namespace gw::lua_udata
{
extern int g_enabled; // lua_udata_profile 0/1 (not saved)
extern int g_sample; // lua_udata_profile_sample: one sample per this many instances on average (1 = all)

// lua_udata_profile changed: 0 -> 1 starts a new window and installs the luabind hook, 1 -> 0 removes it
void Apply(bool enable) noexcept;
// lua_udata_report [reset] [all | <class>] [top=N]
void Report(pcstr args) noexcept;
} // namespace gw::lua_udata
