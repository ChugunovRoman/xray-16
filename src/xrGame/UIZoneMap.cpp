#include "StdAfx.h"
#include "bodycam_motion.h"
#include "UIZoneMap.h"
#include "UIGameCustom.h"

#include "InfoPortion.h"
#include "PDA.h"

#include "Grenade.h"
#include "Level.h"
#include "game_cl_base.h"

#include "Actor.h"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"

#include "ui/UIMap.h"
#include "ui/UIXmlInit.h"
#include "ui/UIHelper.h"
#include "ui/UIInventoryUtilities.h"
//////////////////////////////////////////////////////////////////////////

void CUIZoneMap::Init(bool motionIconAttached)
{
    ZoneScoped;

    CUIXml uiXml;
    uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "zone_map.xml");

    CUIXmlInit::InitStatic(uiXml, "minimap:background", 0, &m_background);
    CUIXmlInit::InitStatic(uiXml, "minimap:level_frame", 0, &m_clipFrame);
    CUIXmlInit::InitStatic(uiXml, "minimap:center", 0, &m_center);

    m_clock_wnd = UIHelper::CreateStatic(uiXml, "minimap:clock_wnd", &m_background, false);

    if (IsGameTypeSingle())
    {
        m_pointerDistanceText = UIHelper::CreateStatic(uiXml, "minimap:background:dist_text", &m_background, false);
    }

    m_activeMap = xr_new<CUIMiniMap>();
    m_clipFrame.AttachChild(m_activeMap);
    m_activeMap->SetAutoDelete(true);
    m_activeMap->EnableHeading(true);

    m_activeMap->SetRounded(motionIconAttached);

    CUIXmlInit::InitStatic(uiXml, "minimap:compass", 0, &m_compass);
    m_background.AttachChild(&m_compass);

    m_clipFrame.AttachChild(&m_center);

    m_Counter_text.SetText("");
    visible = true;

    Fvector2 temp;
    const float k = UI().get_current_kx();

    if (motionIconAttached)
    {
        temp = m_clipFrame.GetWndSize();
        temp.y *= UI_BASE_HEIGHT * k;
        temp.x = temp.y / k;
        m_clipFrame.SetWndSize(temp);

        temp = m_clipFrame.GetWndPos();
        m_clipFrame.SetWndPos(temp.mul(UI_BASE_HEIGHT));
    }

    if (motionIconAttached)
    {
        m_background.SetHeight(m_background.GetHeight() * UI_BASE_HEIGHT);
        m_background.SetWidth(m_background.GetHeight() * k);

        m_clipFrame.GetWndRect().getcenter(temp);
        m_background.SetWndPos(temp);
    }

    temp = m_clipFrame.GetWndSize();
    m_center.SetWndPos(temp.div(2.0f));

    if (motionIconAttached)
    {
        temp = m_compass.GetWndPos();
        temp.mul(m_background.GetWndSize());
        m_compass.SetWndPos(temp);
    }

    if (m_clock_wnd && motionIconAttached)
    {
        temp = m_clock_wnd->GetWndPos();
        temp.mul(m_background.GetWndSize());
        m_clock_wnd->SetWndPos(temp);
    }

    if (IsGameTypeSingle())
    {
        CUIXmlInit::InitStatic(uiXml, "minimap:static_counter", 0, &m_Counter);
        m_background.AttachChild(&m_Counter);
        CUIXmlInit::InitStatic(uiXml, "minimap:static_counter:text_static", 0, &m_Counter_text);
        m_Counter.AttachChild(&m_Counter_text);

        if (motionIconAttached)
        {
            temp = m_Counter.GetWndPos();
            temp.mul(m_background.GetWndSize());
            m_Counter.SetWndPos(temp);
        }
    }
    m_motionAttached=motionIconAttached;
    CUIWindow* windows[]={&m_background,&m_clipFrame,&m_Counter,&m_Counter_text,&m_compass,m_clock_wnd};
    for(int i=0;i<6;++i)if(windows[i])
    {
        const Fvector2 pos=windows[i]->GetWndPos(),size=windows[i]->GetWndSize();
        m_originalRects[i].set(pos.x,pos.y,pos.x+size.x,pos.y+size.y);
        m_originalAlignments[i]=windows[i]->GetAlignment();
    }
    m_originalTexture=uiXml.Read("minimap:background:texture",0,"ui_inGame2_Radar_main_window");
    if(m_clock_wnd)m_clockColor=m_clock_wnd->GetTextColor();
    m_counterColor=m_Counter_text.GetTextColor();
    if(m_clock_wnd){m_clockVAlign=m_clock_wnd->TextItemControl()->GetVTextAlignment();m_clockAlign=m_clock_wnd->TextItemControl()->GetTextAlignment();}
    m_counterVAlign=m_Counter_text.TextItemControl()->GetVTextAlignment();m_counterAlign=m_Counter_text.TextItemControl()->GetTextAlignment();
    m_clockFont=m_clock_wnd?m_clock_wnd->GetFont():nullptr;
    m_counterFont=m_Counter_text.GetFont();
    m_background.AttachChild(&m_bodycamContacts);m_bodycamContacts.Show(false);
    m_bodycamLamp.SetStretchTexture(true);
    m_background.AttachChild(&m_bodycamLamp);
    m_bodycamLamp.Show(false);
}

void CUIZoneMap::Render()
{
    ApplyBodycamStyle();
    if(gw_bodycam_motion::hud_active())
    {
        auto* actor=smart_cast<CActor*>(Level().CurrentViewEntity());
        if(!actor || !actor->g_Alive() || CurrentGameUI()->TopInputReceiver() || Device.Paused())return;
    }
    if (!visible)
        return;

    m_clipFrame.Draw();
    m_background.Draw();
}

void CUIZoneMap::Update()
{
    ApplyBodycamStyle();
    const auto* pActor = smart_cast<CActor*>(Level().CurrentViewEntity());
    if (!pActor)
        return;

    if ((m_bodycamStyle || !(Device.dwFrame % 20)) && IsGameTypeSingle())
    {
        string16 text_str;
        xr_strcpy(text_str, sizeof(text_str), "");

        {
            CPda* pda=pActor->GetPDA();
            const u32 cn = pda ? pda->ActiveContactsNum() : 0;
            if(m_bodycamStyle)
            {
                if(m_lastContacts!=~0u && m_lastContacts!=cn)m_blinkUntil=Device.fTimeGlobal+.8f;
                m_lastContacts=cn;
                xr_sprintf(text_str,sizeof(text_str),"%u",cn);
            }
            else if (cn > 0)
            {
                xr_sprintf(text_str, sizeof(text_str), "%d", cn);
            }
        }
        m_Counter_text.SetText(text_str);
    }

    if(m_bodycamStyle)
    {
        const bool flash=Device.fTimeGlobal<m_blinkUntil && fmodf(Device.fTimeGlobal*8.f,1.f)<.5f;
        const u32 light=flash?255:28;
        m_bodycamLamp.SetTextureColor(color_rgba(light,0,0,255));
    }
    UpdateRadar(Device.vCameraPosition);
    float h, p;
    Device.vCameraDirection.getHP(h, p);
    SetHeading(-h);

    if (m_clock_wnd)
    {
        m_clock_wnd->SetText(GetGameTimeAsString(InventoryUtilities::etpTimeToMinutes).c_str());
    }
}

void CUIZoneMap::SetHeading(float angle)
{
    m_activeMap->SetHeading(angle);
    m_compass.SetHeading(angle);
};

void CUIZoneMap::UpdateRadar(Fvector pos)
{
    m_clipFrame.Update();
    m_background.Update();
    m_activeMap->SetActivePoint(pos);

    if (m_pointerDistanceText)
    {
        if (m_activeMap->GetPointerDistance() > 0.5f)
        {
            string64 str;
            xr_sprintf(str, "%.0f m", m_activeMap->GetPointerDistance());
            m_pointerDistanceText->SetText(str);
        }
        else
        {
            m_pointerDistanceText->SetText("");
        }
    }
}

bool CUIZoneMap::ZoomIn() { return true; }
bool CUIZoneMap::ZoomOut() { return true; }

void CUIZoneMap::SetupCurrentMap()
{
    m_activeMap->Initialize(Level().name(), "hud" DELIMITER "default");

    Frect r;
    m_clipFrame.GetAbsoluteRect(r);
    m_activeMap->WorkingArea().set(r);

    Fvector2 wnd_size;
    float zoom_factor = float(m_clipFrame.GetWidth()) / 100.0f;

    cpcstr ln = Level().name().c_str();
    if (pGameIni->section_exist(ln))
    {
        if (pGameIni->line_exist(ln, "minimap_zoom"))
            zoom_factor *= pGameIni->r_float(ln, "minimap_zoom");
    }
    else if (g_pGameLevel->pLevel->section_exist("minimap_zoom"))
    {
        zoom_factor *= g_pGameLevel->pLevel->r_float("minimap_zoom", "value");
    }
    wnd_size.x = m_activeMap->BoundRect().width() * zoom_factor;
    wnd_size.y = m_activeMap->BoundRect().height() * zoom_factor;
    m_activeMap->SetWndSize(wnd_size);
}

void CUIZoneMap::OnSectorChanged(IRender_Sector::sector_id_t sector)
{
    if (!g_pGameLevel->pLevel->section_exist("sub_level_map"))
        return;
    u8 map_idx = u8(-1);
    string64 s_sector;
    xr_sprintf(s_sector, "%zd", sector);

    if (!g_pGameLevel->pLevel->line_exist("sub_level_map", s_sector))
        return;

    map_idx = g_pGameLevel->pLevel->r_u8("sub_level_map", s_sector);
    if (m_current_map_idx == map_idx)
        return;

    m_current_map_idx = map_idx;

    string_path sub_texture;
    xr_sprintf(sub_texture, "%s#%d", m_activeMap->m_texture.c_str(), m_current_map_idx);

    if (map_idx == u8(-1))
        xr_sprintf(sub_texture, "%s", m_activeMap->m_texture.c_str());

    m_activeMap->InitTextureEx(sub_texture, m_activeMap->m_shader_name.c_str());
}

void CUIZoneMap::Counter_ResetClrAnimation() { m_Counter_text.ResetColorAnimation(); }

void CUIZoneMap::ApplyBodycamStyle()
{
    const bool active=gw_bodycam_motion::hud_active();
    const float k=UI().get_current_kx();
    const int layout=gw_bodycam_motion::sms_visible();
    if(active==m_bodycamStyle && (!active || (layout==m_bodycamLayout && fsimilar(k,m_bodycamK))))return;
    if(active!=m_bodycamStyle){m_lastContacts=~0u;m_blinkUntil=0;}
    m_bodycamStyle=active;m_bodycamLayout=layout;m_bodycamK=k;
    if(active)
    {
        CUIWindow* windows[]={&m_background,&m_clipFrame,&m_Counter,&m_Counter_text,&m_compass,m_clock_wnd};
        for(auto* window:windows)if(window)window->SetAlignment(waNone);
        const float w=206.f*k,h=206.f,x=12.f*k,y=gw_bodycam_motion::gps_top();
        // Load the lamp shader (shaders\r3 and shaders\gl only) only when the supported mode is active.
        m_bodycamLamp.InitTextureEx("ui\\gw_bodycam\\gps","hud\\bodycam_lamp");
        m_background.InitTexture("ui\\gw_bodycam\\gps");m_background.SetStretchTexture(true);
        m_background.SetWndRect(Frect().set(x,y,x+w,y+h));
        m_clipFrame.SetWndRect(Frect().set(x+w*.122f,y+h*.155f,x+w*.88f,y+h*.785f));
        m_compass.SetWndPos(Fvector2().set(w*.115f,h*.115f));
        const float footerTop=h*.80f,footerBottom=h*.94f;
        m_Counter.SetWndRect(Frect().set(w*.74f,footerTop,w*.88f,footerBottom));
        m_Counter_text.SetWndRect(Frect().set(0,0,w*.14f,footerBottom-footerTop));
        m_Counter_text.SetFont(UI().Font().pFontLetterica18Russian);
        m_Counter_text.SetVTextAlignment(valCenter);m_Counter_text.SetTextAlignment(CGameFont::alRight);
        m_Counter_text.SetTextColor(color_rgba(225,235,225,255));
        if(m_clock_wnd){m_clock_wnd->SetWndRect(Frect().set(w*.135f,footerTop,w*.53f,footerBottom));m_clock_wnd->SetFont(UI().Font().pFontLetterica18Russian);m_clock_wnd->SetVTextAlignment(valCenter);m_clock_wnd->SetTextAlignment(CGameFont::alLeft);m_clock_wnd->SetTextColor(color_rgba(225,235,225,255));}
        m_bodycamContacts.InitTextureEx("ui\\gw_bodycam\\gps","hud\\bodycam_lamp");
        m_bodycamContacts.SetStretchTexture(true);
        m_bodycamContacts.SetTextureColor(color_rgba(60,230,75,255));
        m_bodycamContacts.SetWndRect(Frect().set(w*.78f-3*k,h*.868f-3,w*.78f+3*k,h*.868f+3));
        m_bodycamLamp.SetWndRect(Frect().set(w*.868f-4*k,h*.077f-4,w*.868f+4*k,h*.077f+4));
    }
    else
    {
        m_background.InitTexture(m_originalTexture.c_str());m_background.SetStretchTexture(true);
        CUIWindow* windows[]={&m_background,&m_clipFrame,&m_Counter,&m_Counter_text,&m_compass,m_clock_wnd};
        for(int i=0;i<6;++i)if(windows[i]){windows[i]->SetWndRect(m_originalRects[i]);windows[i]->SetAlignment(m_originalAlignments[i]);}
        if(m_clock_wnd){m_clock_wnd->SetTextColor(m_clockColor);m_clock_wnd->SetFont(m_clockFont);m_clock_wnd->SetVTextAlignment(m_clockVAlign);m_clock_wnd->SetTextAlignment(m_clockAlign);}
        m_Counter_text.SetTextColor(m_counterColor);m_Counter_text.SetFont(m_counterFont);m_Counter_text.SetVTextAlignment(m_counterVAlign);m_Counter_text.SetTextAlignment(m_counterAlign);
    }
    m_bodycamLamp.Show(active);m_bodycamContacts.Show(active);
    m_activeMap->SetRounded(active?false:m_motionAttached);
    const Fvector2 size=m_clipFrame.GetWndSize();m_center.SetWndPos(Fvector2().set(size.x*.5f,size.y*.5f));
    if(m_activeMap->BoundRect().width()>0)SetupCurrentMap();
}
