////////////////////////////////////////////////////////////////////////////
//	Tracy zones for property evaluators (GOAP / state_mgr): native C++ vs Lua script.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <tracy/Tracy.hpp>

// Use at the start of evaluate() in native C++ evaluators (m_evaluator_name from CPropertyEvaluator).
// this-> required: members live on the dependent base in templates (e.g. CPropertyEvaluatorConst).
#define PROPERTY_EVALUATOR_TRACY_ZONE_CPP() \
    ZoneScopedN("property_evaluator/cpp"); \
    if (this->m_evaluator_name && this->m_evaluator_name[0]) \
        ZoneTextF("[cpp] %s", this->m_evaluator_name); \
    else \
        ZoneTextF("[cpp] (unnamed)")

// Use at the start of CScriptPropertyEvaluatorWrapper::evaluate() (Lua luabind::call_member).
#define PROPERTY_EVALUATOR_TRACY_ZONE_SCRIPT() \
    ZoneScopedN("property_evaluator/script"); \
    if (this->m_evaluator_name && this->m_evaluator_name[0]) \
        ZoneTextF("[script] %s", this->m_evaluator_name); \
    else \
        ZoneTextF("[script] (unnamed)")
