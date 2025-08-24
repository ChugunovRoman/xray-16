#include "pch.hpp"
#include "UILanimController.h"
#include "xrEngine/LightAnimLibrary.h"

void CUIColorAnimConrollerContainer::Update()
{
    inherited::Update();
    UpdateColorAnimation();
}

void CUIColorAnimConrollerContainer::ColorAnimationSetTextureColor(u32 color, bool only_alpha)
{
    WINDOW_LIST::iterator it = m_ChildWndList.begin();
    WINDOW_LIST::iterator it_e = m_ChildWndList.end();
    for (; it != it_e; ++it)
    {
        ITextureOwner* TO = smart_cast<ITextureOwner*>(*it);
        if (TO)
            TO->SetTextureColor((only_alpha) ? subst_alpha(TO->GetTextureColor(), color) : color);
    }
}

void CUIColorAnimConrollerContainer::ColorAnimationSetTextColor(u32 color, bool only_alpha)
{
    WINDOW_LIST::iterator it = m_ChildWndList.begin();
    WINDOW_LIST::iterator it_e = m_ChildWndList.end();
    for (; it != it_e; ++it)
    {
        CUILightAnimColorConroller* TO = smart_cast<CUILightAnimColorConroller*>(*it);
        if (TO)
            TO->ColorAnimationSetTextColor(color, only_alpha);
    }
}
