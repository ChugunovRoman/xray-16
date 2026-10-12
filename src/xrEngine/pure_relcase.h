#pragma once
#ifndef pure_relcaseH
#define pure_relcaseH
#include "xrCore/xrDebug_macros.h"
#include "IGame_Level.h"

class IGameObject;
struct RelcaseBatch;

class ENGINE_API pure_relcase
{
private:
    int m_ID;

public:
    template <typename class_type>
    pure_relcase(void (class_type::*function_to_bind)(IGameObject*))
    {
        R_ASSERT(g_pGameLevel);
        class_type* self = static_cast<class_type*>(this);
        g_pGameLevel->Objects.relcase_register(CObjectList::RELCASE_CALLBACK(self, function_to_bind), &m_ID);
    }
    // P17 (plans/optimization_mt/12-relcase.md): with a batch form for the batched relcase - only for callbacks that just
    // drop pointers, so that one call for the whole batch gives the same result as one call per destroyed object.
    template <typename class_type>
    pure_relcase(void (class_type::*function_to_bind)(IGameObject*),
        void (class_type::*batch_function_to_bind)(const RelcaseBatch&))
    {
        R_ASSERT(g_pGameLevel);
        class_type* self = static_cast<class_type*>(this);
        g_pGameLevel->Objects.relcase_register(CObjectList::RELCASE_CALLBACK(self, function_to_bind),
            CObjectList::RELCASE_BATCH_CALLBACK(self, batch_function_to_bind), &m_ID);
    }
    virtual ~pure_relcase();
};

#endif // pure_relcaseH
