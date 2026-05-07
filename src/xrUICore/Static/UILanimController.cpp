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
    // m_ChildWndList may change during callbacks (e.g. auto-delete/detach), so iterate over a copy.
    const xr_vector<CUIWindow*> children_copy(m_ChildWndList.begin(), m_ChildWndList.end());
    for (CUIWindow* w : children_copy)
    {
        if (!w)
            continue;
        // Child could have been detached since we made the copy.
        if (std::find(m_ChildWndList.begin(), m_ChildWndList.end(), w) == m_ChildWndList.end())
            continue;

        ITextureOwner* TO = smart_cast<ITextureOwner*>(w);
        if (TO)
            TO->SetTextureColor((only_alpha) ? subst_alpha(TO->GetTextureColor(), color) : color);
    }
}

void CUIColorAnimConrollerContainer::ColorAnimationSetTextColor(u32 color, bool only_alpha)
{
    // m_ChildWndList may change during callbacks (e.g. auto-delete/detach), so iterate over a copy.
    const xr_vector<CUIWindow*> children_copy(m_ChildWndList.begin(), m_ChildWndList.end());
    for (CUIWindow* w : children_copy)
    {
        if (!w)
            continue;
        // Child could have been detached since we made the copy.
        if (std::find(m_ChildWndList.begin(), m_ChildWndList.end(), w) == m_ChildWndList.end())
            continue;

        CUILightAnimColorConroller* TO = smart_cast<CUILightAnimColorConroller*>(w);
        if (TO)
            TO->ColorAnimationSetTextColor(color, only_alpha);
    }
}
