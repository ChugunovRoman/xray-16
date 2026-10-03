////////////////////////////////////////////////////////////////////////////
//	Module 		: script_action_wrapper.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script action wrapper
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "action_base.h"

class CScriptActionWrapper : public CScriptActionBase, public luabind::wrap_base
{
public:
    IC CScriptActionWrapper(CScriptGameObject* object = 0, LPCSTR action_name = "");
    virtual void setup(CScriptGameObject* object, CPropertyStorage* storage);
    static void setup_static(CScriptActionBase* action, CScriptGameObject* object, CPropertyStorage* storage);
    virtual void initialize();
    static void initialize_static(CScriptActionBase* action);
    virtual void execute();
    static void execute_static(CScriptActionBase* action);
    virtual void finalize();
    static void finalize_static(CScriptActionBase* action);
    virtual edge_value_type weight(const CSConditionState& condition0, const CSConditionState& condition1) const;
    static  edge_value_type weight_static(CScriptActionBase* action, const CSConditionState& condition0, const CSConditionState& condition1);

private:
    // Does the Lua class of this action define its own weight()? Looked up once: the planner asks the weight of
    // every edge of every search, and without an override the Lua call only comes back to CScriptActionBase::weight.
    // -1 = not looked up yet, 0 = no, 1 = yes (npc_perf_script_action_weight_lookup)
    bool weight_overridden() const;
    mutable s8 m_weight_override = -1;

    // Same reason as in CScriptPropertyEvaluatorWrapper: the name comes from a Lua string that does not outlive
    // the constructor, while m_action_name keeps the pointer for the life of the action (planner logs use it).
    shared_str m_script_action_name;
};

#include "script_action_wrapper_inline.h"
