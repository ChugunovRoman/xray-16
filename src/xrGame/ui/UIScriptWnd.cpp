#include "pch_script.h"
#include "UIScriptWnd.h"
#include "Common/object_broker.h"
#include "xrUICore/Callbacks/callback_info.h"

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
namespace
{
// C2712: __try cannot share a function with STL/callback unwinding (e.g. std::find_if in SendMessage).
IC bool ui_dialog_script_callback_seh(CScriptCallbackEx<void>* fn)
{
    if (!fn)
        return false;
    __try
    {
        (*fn)();
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return true;
    }
}
} // namespace
#endif

CUIDialogWndEx::CUIDialogWndEx() : CUIDialogWnd("CUIDialogWndEx") {}
CUIDialogWndEx::~CUIDialogWndEx() { delete_data(m_callbacks); }

void CUIDialogWndEx::Register(CUIWindow* pChild) { pChild->SetMessageTarget(this); }
void CUIDialogWndEx::Register(CUIWindow* pChild, pcstr name)
{
    pChild->SetWindowName(name);
    pChild->SetMessageTarget(this);
}

void CUIDialogWndEx::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    const auto it = std::find_if(m_callbacks.begin(), m_callbacks.end(), event_comparer{ pWnd, msg });
    if (it == m_callbacks.end())
        return inherited::SendMessage(pWnd, msg, pData);

    SCallbackInfo* cb = *it;
    // CScriptCallbackEx catches C++ / luabind errors; ACCESS_VIOLATION_EXEC (JIT/stale functor) is SEH only.
#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
    if (ui_dialog_script_callback_seh(&cb->m_callback))
    {
#ifndef MASTER_GOLD
        Msg("! CUIDialogWndEx::SendMessage: SEH in script callback [%s] msg=%d", cb->m_control_name.c_str(), (int)msg);
#endif
        cb->m_callback.clear();
    }
#else
    cb->m_callback();
#endif

    //	if ( cb->m_cpp_callback )
    //		cb->m_cpp_callback(pData);
}

bool CUIDialogWndEx::Load(pcstr /*xml_name*/) { return true; }

SCallbackInfo* CUIDialogWndEx::NewCallback()
{
    m_callbacks.push_back(xr_new<SCallbackInfo>());
    return m_callbacks.back();
}

void CUIDialogWndEx::AddCallback(LPCSTR control_id, s16 evt, const luabind::functor<void> &lua_function)
{
    SCallbackInfo* c = NewCallback ();
    c->m_callback.set(lua_function);
    c->m_control_name = control_id;
    c->m_event = evt;
}

void CUIDialogWndEx::AddCallback(pcstr control_id, s16 evt, const luabind::functor<void>& functor, const luabind::object& object)
{
    SCallbackInfo* c = NewCallback();
    c->m_callback.set(functor, object);
    c->m_control_name = control_id;
    c->m_event = evt;
}
