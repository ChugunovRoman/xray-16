#pragma once

#include "xrScriptEngine/xrScriptEngine.hpp"

// 32-frame-spikes.md (plans/optimization_mt), the Lua wrapper cache (always on since 2026-10-18, its switch
// lua_wrapper_cache removed, 41-cvar-cleanup.md): one constant Lua wrapper (luabind userdata) per C++ object instead of a new one on every pass to Lua. luabind makes a new userdata each time a pointer goes to Lua
// (pointer_converter::to_lua -> make_pointer_instance): a callback argument, level.object_by_id, npc:best_enemy(),
// device(), alife()... Tens of thousands of them die every GC cycle, and the atomic phase of the LuaJIT GC walks every
// userdata (separateudata + mmudata). With the cache the first pass makes the wrapper as before and keeps it in the
// Lua registry (luaL_ref); the next passes push the same userdata. The owner (CScriptGameObject, CALifeSimulator, the
// device) drops the reference when it dies (Release); a wrapper a script still holds then lives on as before (it
// points to a deleted object, as the old ones did). The wrapper does not own the object: luabind's pointer_holder
// over a raw pointer, its __gc deletes nothing.
//
// Hooked through explicit specializations of luabind::get_back_reference (the check every pointer/reference converter
// makes before it creates a wrapper), declared next to the class (script_game_object.h, alife_simulator.h,
// UIGameCustom.h, script_ini_file.h) or in the only file that passes the type to Lua (device.cpp,
// XR_IOConsole_script.cpp, fs_registrator_script.cpp, game_graph_script.cpp): luabind itself is not changed.
//
// Threads: Push/Store run only inside a luabind push, i.e. while this thread runs Lua (Lua is on one thread at a
// time). Release may run on any thread: it only queues the reference, the next Push (under Lua) unrefs it.
// A Lua state reset (CScriptEngine::reinit, the destructor) moves the epoch: the references of the old state are
// forgotten, never unref'ed in the new one.
namespace xray::script_wrapper_cache
{
struct Slot
{
    static constexpr int NoRef = -2; // LUA_NOREF

    int ref = NoRef; // luaL_ref in LUA_REGISTRYINDEX
    unsigned epoch = 0; // the Lua state it belongs to
};

// The slot holds a wrapper of the current state: push it, true (a hit). Otherwise false: the caller makes the wrapper,
// then Store. Unrefs the released references first.
XRSCRIPTENGINE_API bool Push(lua_State* L, Slot& slot);
// The new wrapper on the top of the stack (stays there) goes to the slot.
XRSCRIPTENGINE_API void Store(lua_State* L, Slot& slot);
// The owner dies: forget the wrapper (any thread; the reference is unref'ed by the next Push).
XRSCRIPTENGINE_API void Release(Slot& slot);
// The Lua state is closed / replaced (CScriptEngine): all references are gone with it.
XRSCRIPTENGINE_API void OnStateReset();

// A slot outside the object, for an engine singleton that may be replaced (system_ini() after reload_system_ini(),
// game_graph() of the next game, get_console(), getFS()): it remembers the object its wrapper was made for. A pass of
// another object releases the old wrapper first. A new object at the old address gets the old wrapper - what a new
// wrapper would hold too (luabind's pointer_holder: the raw pointer and the class of the same type; the owner
// replaces the object with one of the same dynamic type). Push/Store only under Lua, as the Slot ones.
struct ObjectSlot
{
    Slot slot;
    const void* object = nullptr;
};

inline bool Push(lua_State* L, ObjectSlot& slot, const void* object)
{
    if (slot.object != object)
    {
        Release(slot.slot);
        slot.object = object;
    }
    return Push(L, slot.slot);
}

inline void Store(lua_State* L, ObjectSlot& slot) { Store(L, slot.slot); }
} // namespace xray::script_wrapper_cache
