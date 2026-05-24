#include "StdAfx.h"
#include "Level.h"
#include "map_location.h"
#include "map_manager.h"
#include "map_spot.h"
#include "UIMap.h"
#include "UIMapWnd.h"
#include "GamePersistent.h"
#include "xrEngine/xr_input.h" //remove me !!!
#include "xrCore/_fbox2.h"

//const u32 activeLocalMapColor = 0xffffffff; // 0xffc80000;
//const u32 inactiveLocalMapColor = 0xffffffff; // 0xff438cd1;
//const u32 ourLevelMapColor = 0xffffffff;

CUICustomMap::CUICustomMap() : CUIStatic("Custom Map")
{
    m_BoundRect_.set(0, 0, 0, 0);
    m_flags.zero();
    SetPointerDistance(0.0f);
}

void CUICustomMap::Initialize(shared_str name, LPCSTR sh_name)
{
    const CInifile* levelIni{};
    if (name == g_pGameLevel->name())
        levelIni = g_pGameLevel->pLevel;
    else
    {
        string_path map_cfg_fn;
        string_path fname;
        strconcat(sizeof(fname), fname, name.c_str(), DELIMITER "level.ltx");
        FS.update_path(map_cfg_fn, "$game_levels$", fname);
        levelIni = xr_new<CInifile>(map_cfg_fn);
    }

    if (levelIni->section_exist("level_map"))
    {
        Init_internal(name, *levelIni, "level_map", sh_name);
    }
    else
    {
        Msg("! default LevelMap used for level[%s]", name.c_str());
        Init_internal(name, *pGameIni, "def_map", sh_name);
        m_name = name;
    }
    if (levelIni != g_pGameLevel->pLevel)
    {
        xr_delete(const_cast<CInifile*>(levelIni));
    }
}

void CUICustomMap::Update()
{
    SetPointerDistance(0.0f);
    if (!Locked())
        UpdateSpots();

    CUIStatic::Update();
}

void CUICustomMap::Draw()
{
    UI().PushScissor(WorkingArea());
    CUIStatic::Draw();
    UI().PopScissor();
}

void CUICustomMap::Init_internal(const shared_str& name, const CInifile& pLtx, const shared_str& sect_name, LPCSTR sh_name)
{
    m_name = name;

    m_texture = pLtx.read_if_exists<pcstr>(sect_name, "texture", "ui\\ui_nomap2");
    if (pLtx.line_exist(m_name, "texture"))
        m_texture = pLtx.r_string(m_name, "texture"); // Override if needed

    Fvector4 tmp = pLtx.read_if_exists<Fvector4>(sect_name, "bound_rect", {-10000.0f, -10000.0f, 10000.0f, 10000.0f});
    pLtx.read_if_exists(tmp, m_name, "bound_rect"); // Override if needed

    m_shader_name = sh_name;

    if (!Heading())
    {
        tmp.x *= UI().get_current_kx();
        tmp.z *= UI().get_current_kx();
    }

    m_BoundRect_.set(tmp.x, tmp.y, tmp.z, tmp.w);

    Fvector2 sz;
    m_BoundRect_.getsize(sz);
    CUIStatic::SetWndSize(sz);
    CUIStatic::SetWndPos(Fvector2().set(0, 0));
    CUIStatic::InitTextureEx(m_texture.c_str(), m_shader_name.c_str());

    SetStretchTexture(true);
}

void rotation_(float x, float y, const float angle, float& x_, float& y_, float kx)
{
    float _sc = _cos(angle);
    float _sn = _sin(angle);
    x_ = x * _sc + y * _sn;
    y_ = y * _sc - x * _sn;
    x_ *= kx;
}

enum class EMapOrthogonalRotation
{
    None,
    Deg90,
    DegMinus90,
    Deg180,
    Other
};

EMapOrthogonalRotation get_map_rotation_type(float degrees)
{
    if (fsimilar(degrees, 0.0f, 0.1f))
        return EMapOrthogonalRotation::None;
    if (fsimilar(degrees, 90.0f, 0.1f))
        return EMapOrthogonalRotation::Deg90;
    if (fsimilar(degrees, -90.0f, 0.1f))
        return EMapOrthogonalRotation::DegMinus90;
    if (fsimilar(_abs(degrees), 180.0f, 0.1f))
        return EMapOrthogonalRotation::Deg180;
    return EMapOrthogonalRotation::Other;
}

Frect calc_rotated_rect_bounds(const Frect& rect, float angle)
{
    if (fis_zero(angle, EPS_L))
        return rect;

    Fvector2 center;
    rect.getcenter(center);

    const float width = rect.width();
    const float height = rect.height();
    const float cosA = _abs(_cos(angle));
    const float sinA = _abs(_sin(angle));

    const float rotatedWidth = width * cosA + height * sinA;
    const float rotatedHeight = width * sinA + height * cosA;

    Frect result;
    result.set(center.x - rotatedWidth * 0.5f, center.y - rotatedHeight * 0.5f, center.x + rotatedWidth * 0.5f,
        center.y + rotatedHeight * 0.5f);
    return result;
}

Fvector2 CUICustomMap::ConvertLocalToReal(const Fvector2& src, Frect const& bound_rect)
{
    Fvector2 res;
    res.x = bound_rect.lt.x + src.x / GetCurrentZoom().x;
    res.y = bound_rect.height() + bound_rect.lt.y - src.y / GetCurrentZoom().x;

    return res;
}

Fvector2 CUICustomMap::ConvertRealToLocal(
    const Fvector2& src, bool for_drawing) // meters->pixels (relatively own left-top pos)
{
    Fvector2 res;
    if (!Heading())
    {
        Frect bound_rect = BoundRect();
        bound_rect.x1 /= UI().get_current_kx();
        bound_rect.x2 /= UI().get_current_kx();
        res = ConvertRealToLocalNoTransform(src, bound_rect);
        res.x *= UI().get_current_kx();
    }
    else
    {
        Fvector2 heading_pivot = GetStaticItem()->GetHeadingPivot();

        res = ConvertRealToLocalNoTransform(src, BoundRect());
        res.sub(heading_pivot);
        rotation_(res.x, res.y, GetHeading(), res.x, res.y, for_drawing ? UI().get_current_kx() : 1.0f);

        res.add(heading_pivot);
    };
    return res;
}

Fvector2 CUICustomMap::ConvertRealToLocalNoTransform(
    const Fvector2& src, Frect const& bound_rect) // meters->pixels (relatively own left-top pos)
{
    Fvector2 res;
    res.x = (src.x - bound_rect.lt.x) * GetCurrentZoom().x;
    res.y = (bound_rect.height() - (src.y - bound_rect.lt.y)) * GetCurrentZoom().x;

    return res;
}

// position and heading for drawing pointer to src pos
bool CUICustomMap::GetPointerTo(const Fvector2& src, float item_radius, Fvector2& pos, float& heading)
{
    Frect clip_rect_abs = WorkingArea(); // absolute rect coords
    Frect map_rect_abs;
    GetAbsoluteRect(map_rect_abs);

    Frect rect;
    BOOL res = rect.intersection(clip_rect_abs, map_rect_abs);
    if (!res)
        return false;

    rect = clip_rect_abs;
    rect.sub(map_rect_abs.lt.x, map_rect_abs.lt.y);

    Fbox2 f_clip_rect_local;
    f_clip_rect_local.set(rect.x1, rect.y1, rect.x2, rect.y2);

    Fvector2 f_center;
    f_clip_rect_local.getcenter(f_center);

    Fvector2 f_dir, f_src;

    f_src.set(src.x, src.y);
    f_dir.sub(f_center, f_src);
    f_dir.normalize_safe();
    Fvector2 f_intersect_point{};
    res = f_clip_rect_local.Pick2(f_src, f_dir, f_intersect_point);
    if (!res)
        return false;

    heading = -f_dir.getH();

    f_intersect_point.mad(f_intersect_point, f_dir, item_radius);

    pos.set(iFloor(f_intersect_point.x), iFloor(f_intersect_point.y));
    return true;
}

void CUICustomMap::FitToWidth(float width)
{
    float k = m_BoundRect_.width() / m_BoundRect_.height();
    float w = width;
    float h = width / k;
    SetWndRect(Frect().set(0.0f, 0.0f, w, h));
}

void CUICustomMap::FitToHeight(float height)
{
    float k = m_BoundRect_.width() / m_BoundRect_.height();
    float h = height;
    float w = k * height;

    SetWndRect(Frect().set(0.0f, 0.0f, w, h));
}

void CUICustomMap::OptimalFit(const Frect& r)
{
    if ((BoundRect().height() / r.height()) < (BoundRect().width() / r.width()))
        FitToHeight(r.height());
    else
        FitToWidth(r.width());
}

// try to positioning clipRect center to vNewPoint
void CUICustomMap::SetActivePoint(const Fvector& vNewPoint)
{
    Fvector2 pos;
    pos.set(vNewPoint.x, vNewPoint.z);
    Frect bound = BoundRect();
    if (FALSE == bound.in(pos))
        return;

    Fvector2 pos_on_map = ConvertRealToLocalNoTransform(pos, BoundRect());
    Frect map_abs_rect;
    GetAbsoluteRect(map_abs_rect);
    Fvector2 pos_abs;

    pos_abs.set(map_abs_rect.lt);
    pos_abs.add(pos_on_map);

    Fvector2 clip_center;
    WorkingArea().getcenter(clip_center);
    clip_center.sub(pos_abs);
    MoveWndDelta(clip_center);
    SetHeadingPivot(pos_on_map, Fvector2().set(0, 0), false);
}

bool CUICustomMap::IsRectVisible(Frect r)
{
    Fvector2 pos;
    GetAbsolutePos(pos);
    r.add(pos.x, pos.y);

    return !!WorkingArea().intersected(r);
}

bool CUICustomMap::NeedShowPointer(Frect r)
{
    Frect map_visible_rect = WorkingArea();
    map_visible_rect.shrink(5, 5);
    Fvector2 pos;
    GetAbsolutePos(pos);
    r.add(pos.x, pos.y);

    return !map_visible_rect.intersected(r);
}

void CUICustomMap::SendMessage(CUIWindow* pWnd, s16 msg, void* pData) { CUIWndCallback::OnEvent(pWnd, msg, pData); }
bool CUIGlobalMap::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (inherited::OnMouseAction(x, y, mouse_action))
        return true;
    if (mouse_action == WINDOW_MOUSE_MOVE && (FALSE == pInput->iGetAsyncKeyState(MOUSE_1)))
    {
        if (MapWnd())
        {
            MapWnd()->Hint("global_map");
            return true;
        }
    }
    return false;
}

CUIGlobalMap::CUIGlobalMap(CUIMapWnd* pMapWnd)
{
    m_mapWnd = pMapWnd;
    m_minZoom = 1.f;
    Show(false);
}

void CUIGlobalMap::Initialize(pcstr section_name) { Init_internal(section_name, *pGameIni, section_name, "hud" DELIMITER "default"); }
void CUIGlobalMap::Init_internal(const shared_str& name, const CInifile& pLtx, const shared_str& sect_name, LPCSTR sh_name)
{
    inherited::Init_internal(name, pLtx, sect_name, sh_name);
    //	Fvector2 size = CUIStatic::GetWndSize();
    SetMaxZoom(pLtx.r_float(m_name, "max_zoom"));
}

void CUIGlobalMap::Update()
{
    for (auto it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
    {
        CUICustomMap* m = smart_cast<CUICustomMap*>(*it);
        if (!m)
            continue;
        m->DetachAll();
    }
    inherited::Update();
}

void CUIGlobalMap::ClipByVisRect()
{
    Frect r = GetWndRect();
    Frect clip = WorkingArea();
    if (r.x2 < clip.width())
        r.x1 += clip.width() - r.x2;
    if (r.y2 < clip.height())
        r.y1 += clip.height() - r.y2;
    if (r.x1 > 0.0f)
        r.x1 = 0.0f;
    if (r.y1 > 0.0f)
        r.y1 = 0.0f;
    SetWndPos(r.lt);
}

Fvector2 CUIGlobalMap::ConvertRealToLocal(
    const Fvector2& src, bool for_drawing) // pixels->pixels (relatively own left-top pos)
{
    Fvector2 res;
    res.x = (src.x - BoundRect().lt.x) * GetCurrentZoom().x;
    res.y = (src.y - BoundRect().lt.y) * GetCurrentZoom().x;
    return res;
}

void CUIGlobalMap::MoveWndDelta(const Fvector2& d)
{
    inherited::MoveWndDelta(d);
    ClipByVisRect();
    m_mapWnd->UpdateScroll();
}

float CUIGlobalMap::CalcOpenRect(const Fvector2& center_point, Frect& map_desired_rect, float tgt_zoom)
{
    Fvector2 new_center_pt;
    // calculate desired rect in new zoom
    map_desired_rect.set(0.0f, 0.0f, BoundRect().width() * tgt_zoom, BoundRect().height() * tgt_zoom);

    // calculate center point in new zoom (center_point is in identity global map space)
    new_center_pt.set(center_point.x * tgt_zoom, center_point.y * tgt_zoom);
    // get vis width & height
    Frect vis_abs_rect = m_mapWnd->ActiveMapRect();
    float vis_w = vis_abs_rect.width();
    float vis_h = vis_abs_rect.height();
    // calculate center delta from vis rect
    Fvector2 delta_pos;
    delta_pos.set(new_center_pt.x - vis_w * 0.5f, new_center_pt.y - vis_h * 0.5f);

    // correct desired rect
    map_desired_rect.sub(delta_pos.x, delta_pos.y);
    // clamp pos by vis rect
    const Frect& r = map_desired_rect;
    Fvector2 np = r.lt;
    if (r.x2 < vis_w)
        np.x += vis_w - r.x2;
    if (r.y2 < vis_h)
        np.y += vis_h - r.y2;
    if (r.x1 > 0.0f)
        np.x = 0.0f;
    if (r.y1 > 0.0f)
        np.y = 0.0f;
    np.sub(r.lt);
    map_desired_rect.add(np.x, np.y);
    // calculate max way dist
    float dist = 0.f;

    Frect s_rect, t_rect;
    s_rect.div(GetWndRect(), GetCurrentZoom().x, GetCurrentZoom().x);
    t_rect.div(map_desired_rect, tgt_zoom, tgt_zoom);

    Fvector2 cpS, cpT;
    s_rect.getcenter(cpS);
    t_rect.getcenter(cpT);

    dist = cpS.distance_to(cpT);

    return dist;
}

CUILevelMap::CUILevelMap(CUIMapWnd* p)
{
    m_mapWnd = p;
    m_pdaMapHeadingDegrees = 0.0f;
    m_pdaMapTextureOffset.set(0.0f, 0.0f);
    m_pdaMapTextureScale.set(1.0f, 1.0f);
    m_currentSubMapIdx = u8(-1);
    m_UnrotatedWndRect.set(0.0f, 0.0f, 0.0f, 0.0f);
    m_RenderOffset.set(0.0f, 0.0f);
    Show(false);
}

CUIGlobalMap* CUILevelMap::GlobalMap() const { return smart_cast<CUIGlobalMap*>(GetParent()); }

void CUILevelMap::SetGlobalRect(const Frect& rect) { m_GlobalRect = rect; }

void CUILevelMap::ApplyPdaMapHeading()
{
    const float headingRadians = deg2rad(m_pdaMapHeadingDegrees);
    const bool hasHeading = !fis_zero(headingRadians, EPS_L);
    const float renderWidth =
        !fis_zero(m_UnrotatedWndRect.width(), EPS_L) ? m_UnrotatedWndRect.width() * m_pdaMapTextureScale.x : GetWndSize().x * m_pdaMapTextureScale.x;
    const float renderHeight = !fis_zero(m_UnrotatedWndRect.height(), EPS_L) ? m_UnrotatedWndRect.height() * m_pdaMapTextureScale.y :
                                                                               GetWndSize().y * m_pdaMapTextureScale.y;

    EnableHeading(hasHeading);
    SetConstHeading(true);
    SetHeading(headingRadians);

    const auto rotationType = get_map_rotation_type(m_pdaMapHeadingDegrees);
    switch (rotationType)
    {
    case EMapOrthogonalRotation::Deg90:
        SetHeadingPivot(Fvector2().set(0.0f, 0.0f), Fvector2().set(0.0f, renderWidth), true);
        break;
    case EMapOrthogonalRotation::DegMinus90:
        SetHeadingPivot(Fvector2().set(0.0f, 0.0f), Fvector2().set(renderHeight, 0.0f), true);
        break;
    case EMapOrthogonalRotation::Deg180:
        SetHeadingPivot(Fvector2().set(0.0f, 0.0f), Fvector2().set(renderWidth, renderHeight), true);
        break;
    default:
    {
        Fvector2 pivot;
        const float pivotWidth = renderWidth;
        const float pivotHeight = renderHeight;
        pivot.set(pivotWidth * 0.5f, pivotHeight * 0.5f);
        SetHeadingPivot(pivot, Fvector2().set(0.0f, 0.0f), false);
        break;
    }
    }
}

void CUILevelMap::SetPdaMapHeadingDegrees(float degrees)
{
    m_pdaMapHeadingDegrees = degrees;
    ApplyPdaMapHeading();
}

void CUILevelMap::SetPdaMapTextureOffset(const Fvector2& offset) { m_pdaMapTextureOffset = offset; }

void CUILevelMap::SetPdaMapTextureScale(const Fvector2& scale)
{
    m_pdaMapTextureScale.x = _max(scale.x, 0.01f);
    m_pdaMapTextureScale.y = _max(scale.y, 0.01f);
    ApplyPdaMapHeading();
}

void CUILevelMap::UpdateSubLevelMapTexture()
{
    const auto resetToBaseTexture = [this]()
    {
        if (m_currentSubMapIdx != u8(-1))
        {
            m_currentSubMapIdx = u8(-1);
            InitTextureEx(m_texture.c_str(), m_shader_name.c_str());
        }
    };

    if (MapName() != Level().name())
    {
        resetToBaseTexture();
        return;
    }

    if (!g_pGameLevel || !g_pGameLevel->pLevel || !g_pGameLevel->pLevel->section_exist("sub_level_map"))
    {
        resetToBaseTexture();
        return;
    }

    const auto sector = GamePersistent().GetLastSectorId();
    string64 sectorKey;
    xr_sprintf(sectorKey, "%zd", sector);

    if (sector == static_cast<IRender_Sector::sector_id_t>(-1))
    {
        resetToBaseTexture();
        return;
    }

    if (!g_pGameLevel->pLevel->line_exist("sub_level_map", sectorKey))
    {
        resetToBaseTexture();
        return;
    }

    const u8 mapIdx = g_pGameLevel->pLevel->r_u8("sub_level_map", sectorKey);
    if (m_currentSubMapIdx == mapIdx)
        return;

    m_currentSubMapIdx = mapIdx;

    string_path subTexture;
    if (mapIdx == u8(-1))
        xr_sprintf(subTexture, "%s", m_texture.c_str());
    else
        xr_sprintf(subTexture, "%s#%d", m_texture.c_str(), m_currentSubMapIdx);

    InitTextureEx(subTexture, m_shader_name.c_str());
}

Fvector2 CUILevelMap::ConvertRealToLocal(const Fvector2& src, bool for_drawing)
{
    const auto rotationType = get_map_rotation_type(m_pdaMapHeadingDegrees);
    if (rotationType == EMapOrthogonalRotation::None)
        return inherited::ConvertRealToLocal(src, for_drawing);

    if (rotationType == EMapOrthogonalRotation::Other)
    {
        Fvector2 result = inherited::ConvertRealToLocal(src, for_drawing);
        result.add(m_RenderOffset);
        return result;
    }

    const Frect& boundRect = BoundRect();
    const float width = !fis_zero(m_UnrotatedWndRect.width(), EPS_L) ? m_UnrotatedWndRect.width() : GetWndSize().x;
    const float height = !fis_zero(m_UnrotatedWndRect.height(), EPS_L) ? m_UnrotatedWndRect.height() : GetWndSize().y;
    const float zoomX = !fis_zero(boundRect.width(), EPS_L) ? width / boundRect.width() : 0.0f;
    const float zoomY = !fis_zero(boundRect.height(), EPS_L) ? height / boundRect.height() : 0.0f;

    Fvector2 local;
    local.x = (src.x - boundRect.lt.x) * zoomX;
    local.y = (boundRect.height() - (src.y - boundRect.lt.y)) * zoomY;

    const float drawKx = for_drawing ? UI().get_current_kx() : 1.0f;

    switch (rotationType)
    {
    case EMapOrthogonalRotation::Deg90: return Fvector2().set(local.y * drawKx, width - local.x);
    case EMapOrthogonalRotation::DegMinus90: return Fvector2().set((height - local.y) * drawKx, local.x);
    case EMapOrthogonalRotation::Deg180: return Fvector2().set((width - local.x) * drawKx, height - local.y);
    case EMapOrthogonalRotation::None:
    default:
        break;
    }

    return local;
}

void CUILevelMap::Draw()
{
    CUIGlobalMap* globalMap = GlobalMap();
    if (globalMap)
    {
        float gmz = globalMap->GetCurrentZoom().x;
        const bool keepUndergroundSpotsVisible = MapWnd() && MapWnd()->ActiveLayer() == EPdaMapLayer::Underground;
        for (auto it = m_ChildWndList.begin(); m_ChildWndList.end() != it; ++it)
        {
            CMapSpot* sp = smart_cast<CMapSpot*>((*it));
            if (!sp)
                continue;

            if (sp->m_bScale)
            {
                Fvector2 sz = sp->m_originSize;
                // XXX: try to remove if-else branches and use common code path
                if (ShadowOfChernobylMode)
                {
                    sz.mul(gmz);
                    sp->SetWndSize(sz);
                }
                else if (ClearSkyMode)
                {
                    if (gmz > sp->m_scale_bounds.x && gmz < sp->m_scale_bounds.y)
                    {
                        float k = (gmz - sp->m_scale_bounds.x) / (sp->m_scale_bounds.y - sp->m_scale_bounds.x);
                        sz.mul(k);
                        sp->SetWndSize(sz);
                    }
                    else if (gmz > sp->m_scale_bounds.y)
                    {
                        sp->SetWndSize(sz);
                    }
                }
                else // COP
                {
                    float k = gmz;

                    if (gmz > sp->m_scale_bounds.y)
                        k = sp->m_scale_bounds.y;
                    else if (gmz < sp->m_scale_bounds.x)
                        k = sp->m_scale_bounds.x;

                    sz.mul(k);
                    sp->SetWndSize(sz);
                }
            }
            else if (sp->m_scale_bounds.x > 0.0f)
            {
                if (keepUndergroundSpotsVisible)
                    sp->SetVisible(true);
                else
                    sp->SetVisible(sp->m_scale_bounds.x < gmz);
            }
        }
    }
    inherited::Draw();
}

void CUILevelMap::Init_internal(const shared_str& name, const CInifile& pLtx, const shared_str& sect_name, LPCSTR sh_name)
{
    inherited::Init_internal(name, pLtx, sect_name, sh_name);
    Fvector4 tmp = pGameIni->r_fvector4(MapName(), "global_rect");

    tmp.x *= UI().get_current_kx();
    tmp.z *= UI().get_current_kx();
    m_GlobalRect.set(tmp.x, tmp.y, tmp.z, tmp.w);

    m_pdaMapHeadingDegrees = pGameIni->line_exist(MapName(), "pda_map_heading") ? pGameIni->r_float(MapName(), "pda_map_heading") :
                                                                                     0.0f;
    m_pdaMapTextureOffset =
        pGameIni->line_exist(MapName(), "pda_map_texture_offset") ? pGameIni->r_fvector2(MapName(), "pda_map_texture_offset") :
                                                                    Fvector2().set(0.0f, 0.0f);
    m_pdaMapTextureOffset.x *= UI().get_current_kx();
    m_pdaMapTextureScale =
        pGameIni->line_exist(MapName(), "pda_map_texture_scale") ? pGameIni->r_fvector2(MapName(), "pda_map_texture_scale") :
                                                                   Fvector2().set(1.0f, 1.0f);
    m_pdaMapTextureScale.x = _max(m_pdaMapTextureScale.x, 0.01f);
    m_pdaMapTextureScale.y = _max(m_pdaMapTextureScale.y, 0.01f);
    ApplyPdaMapHeading();

#ifdef DEBUG
    float kw = m_GlobalRect.width() / BoundRect().width();
    float kh = m_GlobalRect.height() / BoundRect().height();

    if (FALSE == fsimilar(kw, kh, EPS_L))
    {
        Msg(" --incorrect global rect definition for map [%s]  kw=%f kh=%f", MapName().c_str(), kw, kh);
        Msg(" --try x2=%f or  y2=%f", m_GlobalRect.x1 + kh * BoundRect().width(),
            m_GlobalRect.y1 + kw * BoundRect().height());
    }
#endif
}

void CUILevelMap::UpdateSpots()
{
    DetachAll();

    //.	if( fsimilar(MapWnd()->GlobalMap()->GetCurrentZoom(),MapWnd()->GlobalMap()->GetMinZoom(),EPS_L ) ) return;

    Frect _r;
    GetAbsoluteRect(_r);

    if (FALSE == MapWnd()->ActiveMapRect().intersected(_r))
        return;

    vLocations& ls = Level().MapManager().Locations();
    auto it = ls.begin();
    auto it_e = ls.end();

    for (u32 idx = 0; it != it_e; ++it, ++idx)
        if ((*it).actual && MapName() == (*it).location->GetLevelName())
            (*it).location->UpdateLevelMap(this);
}

Frect CUILevelMap::CalcWndRectOnGlobal()
{
    Frect res;
    CUIGlobalMap* globalMap = GlobalMap();
    if (!globalMap)
        return res;

    res.lt = globalMap->ConvertRealToLocal(GlobalRect().lt, false);
    res.rb = globalMap->ConvertRealToLocal(GlobalRect().rb, false);
    res = calc_rotated_rect_bounds(res, GetHeading());
    res.add(globalMap->GetWndPos().x, globalMap->GetWndPos().y);

    return res;
}

void CUILevelMap::Show(bool status) { inherited::Show(status); }
void CUILevelMap::Update()
{
    CUIGlobalMap* w = GlobalMap();
    if (!w)
        return;

    UpdateSubLevelMapTexture();

    Frect rect;
    Fvector2 tmp;

    tmp = w->ConvertRealToLocal(GlobalRect().lt, false);
    rect.lt = tmp;
    tmp = w->ConvertRealToLocal(GlobalRect().rb, false);
    rect.rb = tmp;
    m_UnrotatedWndRect = rect;
    const auto rotationType = get_map_rotation_type(m_pdaMapHeadingDegrees);
    switch (rotationType)
    {
    case EMapOrthogonalRotation::Deg90:
        rect.set(m_UnrotatedWndRect.x1, m_UnrotatedWndRect.y1, m_UnrotatedWndRect.x1 + m_UnrotatedWndRect.height(),
            m_UnrotatedWndRect.y1 + m_UnrotatedWndRect.width());
        m_RenderOffset.set(0.0f, m_UnrotatedWndRect.width());
        break;
    case EMapOrthogonalRotation::DegMinus90:
        rect.set(m_UnrotatedWndRect.x1, m_UnrotatedWndRect.y1, m_UnrotatedWndRect.x1 + m_UnrotatedWndRect.height(),
            m_UnrotatedWndRect.y1 + m_UnrotatedWndRect.width());
        m_RenderOffset.set(m_UnrotatedWndRect.height(), 0.0f);
        break;
    case EMapOrthogonalRotation::Deg180:
        rect = m_UnrotatedWndRect;
        m_RenderOffset.set(m_UnrotatedWndRect.width(), m_UnrotatedWndRect.height());
        break;
    case EMapOrthogonalRotation::None:
        rect = m_UnrotatedWndRect;
        m_RenderOffset.set(0.0f, 0.0f);
        break;
    case EMapOrthogonalRotation::Other:
        rect = calc_rotated_rect_bounds(m_UnrotatedWndRect, GetHeading());
        m_RenderOffset.set(m_UnrotatedWndRect.x1 - rect.x1, m_UnrotatedWndRect.y1 - rect.y1);
        break;
    }

    SetWndRect(rect);
    ApplyPdaMapHeading();

    inherited::Update();

    if (m_bCursorOverWindow)
    {
        VERIFY(m_dwFocusReceiveTime >= 0);
        if (Device.dwTimeGlobal > (m_dwFocusReceiveTime + 500 * Device.time_factor()))
        {
            if (fsimilar(w->GetCurrentZoom().x, w->GetMinZoom(), EPS_L))
                MapWnd()->ShowHintStr(this, MapName().c_str());
            else
                MapWnd()->HideHint(this);
        }
    }
}

void CUILevelMap::DrawTexture()
{
    if (!m_bTextureEnable || !GetShader() || !GetShader()->inited())
        return;

    Fvector2 absolutePos;
    GetAbsolutePos(absolutePos);
    const float zoom = GlobalMap() ? GlobalMap()->GetCurrentZoom().x : 1.0f;
    const Fvector2 scaledTextureOffset =
        Fvector2().set(m_TextureOffset.x + m_pdaMapTextureOffset.x * zoom, m_TextureOffset.y + m_pdaMapTextureOffset.y * zoom);
    const Fvector2 renderSize =
        Fvector2().set(m_UnrotatedWndRect.width() * m_pdaMapTextureScale.x, m_UnrotatedWndRect.height() * m_pdaMapTextureScale.y);

    const auto rotationType = get_map_rotation_type(m_pdaMapHeadingDegrees);
    Frect renderRect;
    if (rotationType == EMapOrthogonalRotation::Other)
    {
        renderRect.set(absolutePos.x + m_RenderOffset.x, absolutePos.y + m_RenderOffset.y,
            absolutePos.x + m_RenderOffset.x + m_UnrotatedWndRect.width(), absolutePos.y + m_RenderOffset.y + m_UnrotatedWndRect.height());
    }
    else
    {
        renderRect.set(absolutePos.x, absolutePos.y, absolutePos.x + m_UnrotatedWndRect.width(), absolutePos.y + m_UnrotatedWndRect.height());
    }

    m_UIStaticItem.SetPos(renderRect.left + scaledTextureOffset.x, renderRect.top + scaledTextureOffset.y);
    m_UIStaticItem.SetSize(renderSize);

    if (Heading())
        m_UIStaticItem.Render(GetHeading());
    else
        m_UIStaticItem.Render();
}

bool CUILevelMap::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (inherited::OnMouseAction(x, y, mouse_action))
        return true;
    CUIGlobalMap* globalMap = GlobalMap();
    if (!globalMap || globalMap->Locked())
        return true;

    if (mouse_action == WINDOW_MOUSE_MOVE && (FALSE == pInput->iGetAsyncKeyState(MOUSE_1)))
    {
        if (MapWnd())
        {
            MapWnd()->Hint(MapName());
            return true;
        }
    }
    return false;
}

void CUILevelMap::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    inherited::SendMessage(pWnd, msg, pData);

    if (msg == MAP_SHOW_HINT)
    {
        CMapSpot* sp = smart_cast<CMapSpot*>(pWnd);
        VERIFY(sp);
        if (sp)
        {
            MapWnd()->ShowHintSpot(sp);
        }
    }
    else if (msg == MAP_HIDE_HINT)
        MapWnd()->HideHint(pWnd);
    else if (msg == MAP_SELECT_SPOT)
        MapWnd()->SpotSelected(pWnd);
    else if (msg == MAP_SELECT_SPOT2)
        MapWnd()->ActivatePropertiesBox(pWnd);
}

void CUILevelMap::OnFocusLost()
{
    inherited::OnFocusLost();
    MapWnd()->HideHint(this);
}

CUIMiniMap::CUIMiniMap()
{
    SetRounded(true);
}

void CUIMiniMap::Init_internal(const shared_str& name, const CInifile& pLtx, const shared_str& sect_name, LPCSTR sh_name)
{
    inherited::Init_internal(name, pLtx, sect_name, sh_name);
    CUIStatic::SetTextureColor(0x7fffffff);
}

void CUIMiniMap::UpdateSpots()
{
    DetachAll();
    vLocations& ls = Level().MapManager().Locations();
    for (auto it = ls.begin(); it != ls.end(); ++it)
        (*it).location->UpdateMiniMap(this);
}

void CUIMiniMap::Draw()
{
    if (!IsRounded())
    {
        inherited::Draw();
        return;
    }

    u32 segments_count = 20;

    GEnv.UIRender->SetShader(*m_UIStaticItem.GetShader());
    GEnv.UIRender->StartPrimitive(segments_count * 3, IUIRender::ptTriList, UI().m_currentPointType);

    u32 color = m_UIStaticItem.GetTextureColor();
    float angle = GetHeading();

    float kx = UI().get_current_kx();

    // clip poly
    sPoly2D S;
    S.resize(segments_count);
    float segment_ang = PI_MUL_2 / segments_count;
    float pt_radius = WorkingArea().width() / 2.0f;
    Fvector2 center;
    WorkingArea().getcenter(center);

    float tt_radius = pt_radius / GetWidth();
    float k_tt_height = GetWidth() / GetHeight();

    Fvector2 tt_offset;
    tt_offset.set(m_UIStaticItem.vHeadingPivot);
    tt_offset.x /= GetWidth();
    tt_offset.y /= GetHeight();

    Fvector2 m_scale_;
    m_scale_.set(float(Device.dwWidth) / UI_BASE_WIDTH, float(Device.dwHeight) / UI_BASE_HEIGHT);

    for (u32 idx = 0; idx < segments_count; ++idx)
    {
        float cosPT = _cos(segment_ang * idx + angle);
        float sinPT = _sin(segment_ang * idx + angle);

        float cosTX = _cos(segment_ang * idx);
        float sinTX = _sin(segment_ang * idx);

        S[idx].pt.set(pt_radius * cosPT * kx, -pt_radius * sinPT);
        S[idx].uv.set(tt_radius * cosTX, -tt_radius * sinTX * k_tt_height);
        S[idx].uv.add(tt_offset);
        S[idx].pt.add(center);

        S[idx].pt.x *= m_scale_.x;
        S[idx].pt.y *= m_scale_.y;
    }

    for (u32 idx = 0; idx < segments_count - 2; ++idx)
    {
        GEnv.UIRender->PushPoint(S[0 + 0].pt.x, S[0 + 0].pt.y, 0, color, S[0 + 0].uv.x, S[0 + 0].uv.y);
        GEnv.UIRender->PushPoint(S[idx + 2].pt.x, S[idx + 2].pt.y, 0, color, S[idx + 2].uv.x, S[idx + 2].uv.y);
        GEnv.UIRender->PushPoint(S[idx + 1].pt.x, S[idx + 1].pt.y, 0, color, S[idx + 1].uv.x, S[idx + 1].uv.y);
    }

    GEnv.UIRender->FlushPrimitive();

    //------------
    CUIWindow::Draw(); // draw childs
}

bool CUIMiniMap::GetPointerTo(const Fvector2& src, float item_radius, Fvector2& pos, float& heading)
{
    if (!IsRounded())
    {
        return inherited::GetPointerTo(src, item_radius, pos, heading);
    }
    Fvector2 clip_center = GetStaticItem()->GetHeadingPivot();
    float map_radius = WorkingArea().width() / 2.0f;
    Fvector2 direction;

    direction.sub(clip_center, src);
    heading = -direction.getH();

    float kx = UI().get_current_kx();
    float cosPT = _cos(heading);
    float sinPT = _sin(heading);
    pos.set(-map_radius * sinPT * kx, -map_radius * cosPT);
    pos.add(clip_center);

    return true;
}

bool CUIMiniMap::NeedShowPointer(Frect r)
{
    if (!IsRounded())
    {
        return inherited::NeedShowPointer(r);
    }
    Fvector2 clip_center = GetStaticItem()->GetHeadingPivot();

    Fvector2 spot_pos;
    r.getcenter(spot_pos);
    float dist = clip_center.distance_to(spot_pos);
    float spot_radius = r.width() / 2.0f;
    return (dist + spot_radius > WorkingArea().width() / 2.0f);
}

bool CUIMiniMap::IsRectVisible(Frect r)
{
    if (!IsRounded())
    {
        return inherited::IsRectVisible(r);
    }
    Fvector2 clip_center = GetStaticItem()->GetHeadingPivot();
    float vis_radius = WorkingArea().width() / 2.0f;
    Fvector2 rect_center;
    r.getcenter(rect_center);
    float spot_radius = r.width() / 2.0f;
    return clip_center.distance_to(rect_center) + spot_radius < vis_radius; // assume that all minimap spots are
    // circular
}
