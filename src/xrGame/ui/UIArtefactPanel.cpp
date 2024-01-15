#include "StdAfx.h"
#include "UIArtefactPanel.h"
#include "UIInventoryUtilities.h"
#include "UIXmlInit.h"

#include "../Artefact.h"

CUIArtefactPanel::CUIArtefactPanel() : CUIWindow(CUIArtefactPanel::GetDebugType())
{
    m_cell_size.set(50.0f, 50.0f);
    m_fScale = 0.5f;
}

void CUIArtefactPanel::InitFromXML(CUIXml& xml, pcstr path, int index)
{
    CUIXmlInit::InitWindow(xml, path, index, this);

    m_cell_size.x = xml.ReadAttribFlt(path, index, "cell_width", 50.0f);
    m_cell_size.y = xml.ReadAttribFlt(path, index, "cell_height", 50.0f);
    m_fScale      = xml.ReadAttribFlt(path, index, "scale", 0.5f);
    m_bVert       = xml.ReadAttribInt(path, index, "vert") == 1;
    m_iIndent     = xml.ReadAttribInt(path, index, "indent", 1);
}

void CUIArtefactPanel::InitIcons(const xr_vector<const CArtefact*>& artefacts)
{
    m_vRects.clear();
    m_count_rects = 0;

    for (const CArtefact* artefact : artefacts)
    {
        const shared_str& sectionName = artefact->cNameSect();

        R_ASSERT2(pSettings->line_exist(sectionName, "inv_icon"), make_string("Item '%s' doesn't has property 'inv_icon'", sectionName));

        Frect texture_rect;
        CUIStaticItem staticItem;
        staticItem.SetShader(InventoryUtilities::GetEquipmentIconShader(pSettings->r_string(sectionName, "inv_icon")));
        texture_rect.x1 = float(0);
        texture_rect.y1 = float(0);
        texture_rect.x2 = pSettings->read<float>(sectionName, "inv_grid_width") * ICON_GRID_WIDTH;
        texture_rect.y2 = pSettings->read<float>(sectionName, "inv_grid_height") * ICON_GRID_HEIGHT;
        texture_rect.rb.add(texture_rect.lt);

        m_vRects.push_back(texture_rect);
        m_vStaticRects.push_back(staticItem);
        m_count_rects++;
    }
}

void CUIArtefactPanel::Draw()
{
    float x = 0.0f;
    float y = 0.0f;

    Frect rect;
    GetAbsoluteRect(rect);
    x = rect.left;
    y = rect.top;

    float _s = m_cell_size.x/m_cell_size.y;

    for (auto i = 0; i < m_count_rects; i++)
    {
        Fvector2 size;
        size.x = m_fScale*(m_vRects[i].bottom - m_vRects[i].top);
        size.y = _s*m_fScale*(m_vRects[i].right - m_vRects[i].left);

        m_vStaticRects[i].SetTextureRect(m_vRects[i]);
        m_vStaticRects[i].SetSize(size);
        m_vStaticRects[i].SetPos(x, y);
        if (!m_bVert)
            x = x + m_iIndent + size.x;
        else
            y = y + m_iIndent + size.y;

        m_vStaticRects[i].Render();
    }

    CUIWindow::Draw();
}
