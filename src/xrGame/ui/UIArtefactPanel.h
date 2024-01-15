#pragma once
#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Static/UIStaticItem.h"

class CUIXml;
class CArtefact;
class CUIStaticItem;

class CUIArtefactPanel final : public CUIWindow
{
protected:
    bool m_bVert;
    int m_iIndent;
    float m_fScale;
    int m_count_rects{0};
    Fvector2 m_cell_size;
    xr_vector<Frect> m_vRects;
    xr_vector<CUIStaticItem> m_vStaticRects;

public:
    CUIArtefactPanel();

    void InitIcons(const xr_vector<const CArtefact*>& artefacts);
    void InitFromXML(CUIXml& xml, pcstr path, int index);
    void Draw() override;

    pcstr GetDebugType() override { return "CUIArtefactPanel"; }
};
