#pragma once

// The cost of the addons on the event bus (plans/addon_api_ideas/spec.md P48): the time of every subscriber call of
// the event bus, summed by the owner of the subscriber - the addon of a plugin, the addon ("base" for the game) of the
// script file of a Lua handler - plus zones measured in Lua (addon_perf.add: hooks, addons.profile_zone, ...).
// Off by default: console addon_perf 1 (a diagnostic, not kept in user.ltx), the report - addon_perf_report [N], Lua -
// the global table addon_perf. Off, the bus pays one bool read per dispatch. Main thread only.
// Docs: wiki/doc/addons/howto/performance.md, wiki/doc/addons/lua_api.md (addon_perf).

struct lua_State;

namespace gw::addons::perf
{
extern bool g_enabled;
inline bool Enabled() { return g_enabled; }

void SetEnabled(bool on); // switching on resets the counters
void Reset();

// One call of a native handler of `event` (a batch delivery too) of the plugin of addon `addon_id`
void NoteNative(pcstr addon_id, pcstr event, u64 ticks);
// One call of a Lua handler of `event`. index: the stack index of the handler - a function, or (object = true) the
// object whose method `event` was called; 0 - unknown. The owner: the addon of the script file of the function.
void NoteLua(lua_State* L, int index, bool object, pcstr event, u64 ticks);

// The report into the log: the owners by time, then the `top` slowest (owner, kind, event or zone) rows
void PrintReport(u32 top);
} // namespace gw::addons::perf
