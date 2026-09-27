////////////////////////////////////////////////////////////////////////////
//	Module 		: script_action_wrapper_inline.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script action wrapper inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CScriptActionWrapper::CScriptActionWrapper(CScriptGameObject* object, LPCSTR action_name)
    : CScriptActionBase(object, "")
{
    if (action_name && action_name[0])
    {
        m_script_action_name = action_name; // interned: the copy lives as long as this action
        m_action_name = m_script_action_name.c_str();
    }
}
