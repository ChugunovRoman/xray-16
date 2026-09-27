////////////////////////////////////////////////////////////////////////////
//	Module 		: script_property_evaluator_wrapper_inline.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script property evaluator wrapper inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CScriptPropertyEvaluatorWrapper::CScriptPropertyEvaluatorWrapper(CScriptGameObject* object, LPCSTR evaluator_name)
    : CScriptPropertyEvaluator(object, "")
{
    if (evaluator_name && evaluator_name[0])
    {
        m_script_evaluator_name = evaluator_name; // interned: the copy lives as long as this evaluator
        m_evaluator_name = m_script_evaluator_name.c_str();
    }
}
