////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator.h
//	Created 	: 25.12.2002
//  Modified 	: 13.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_interaction_manager.h"
#include "alife_update_manager.h"
#include "xrScriptEngine/script_wrapper_cache.hpp"

#include <luabind/back_reference_fwd.hpp>

#pragma warning(push)
#pragma warning(disable : 4005)

class CALifeSimulator : public CALifeUpdateManager, public CALifeInteractionManager
{
protected:
    virtual void setup_simulator(CSE_ALifeObject* object);
    virtual void reload(LPCSTR section);

public:
    CALifeSimulator(IPureServer* server, shared_str* command_line);
    virtual ~CALifeSimulator();
    virtual void destroy();
    IReader const* get_config(shared_str config) const;

    // The Lua wrapper cache: the one Lua wrapper of alife() for this simulator (luabind::get_back_reference below),
    // released in the destructor
    xray::script_wrapper_cache::Slot m_lua_wrapper;

#if 0 // def DEBUG
			void	validate			();
#endif // DEBUG

private:
    typedef xr_list<std::pair<shared_str, IReader*>> configs_type;
    mutable configs_type m_configs_lru;

    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

// The Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp): alife() and any other pass of the simulator to Lua
// by pointer give the cached wrapper. Defined in
// alife_simulator_script.cpp; declared with the class, so no translation unit instantiates the generic template.
namespace luabind
{
template <>
bool get_back_reference<CALifeSimulator*>(lua_State* L, CALifeSimulator* const& x);
} // namespace luabind

#pragma warning(pop)
