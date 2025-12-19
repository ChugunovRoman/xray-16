#include "StdAfx.h"
#include "UICellItem.h"
#include "xrUICore/Cursor/UICursor.h"
#include "inventory_item.h"
#include "UIDragDropListEx.h"
#include "eatable_item.h"
#include "xrEngine/xr_level_controller.h"
#include "xrEngine/xr_input.h"
#include "Level.h"
#include "Common/object_broker.h"
#include "UIXmlInit.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include "UIInventoryUtilities.h"
#include "Weapon.h"
#include "CustomOutfit.h"
#include "ActorHelmet.h"
#include "UIHelper.h"

CUICellItem* CUICellItem::m_mouse_selected_item = NULL;
CUIXml uiXml;
bool xml_loaded = false;

extern BOOL debug_ui_item_cell;

CUICellItem::CUICellItem()
    : CUIStatic("Cell Item")
{
    m_pParentList = NULL;
    m_pData = NULL;
    m_custom_draw = NULL;
    m_text = NULL;
    //-	m_mark				= NULL;
    m_upgrade = NULL;
    m_faction = NULL;
    m_pConditionState = NULL;
    m_drawn_frame = 0;
    SetAccelerator(0);
    m_b_destroy_childs = true;
    m_selected = false;
    m_select_armament = false;
    m_cur_mark = false;
    m_has_upgrade = false;

    create_flags.set(needConditionBar, true);
    create_flags.set(needFactionIcon, true);
    create_flags.set(needUpgradeIcon, true);

    init();

    UI().Focus().RegisterFocusable(this);
}

CUICellItem::CUICellItem(bool needCondBar, bool needFIcon, bool needUgrIcon)
    : CUIStatic("Cell Item")
{
    m_pParentList = NULL;
    m_pData = NULL;
    m_custom_draw = NULL;
    m_text = NULL;
    //-	m_mark				= NULL;
    m_upgrade = NULL;
    m_faction = NULL;
    m_pConditionState = NULL;
    m_drawn_frame = 0;
    SetAccelerator(0);
    m_b_destroy_childs = true;
    m_selected = false;
    m_select_armament = false;
    m_cur_mark = false;
    m_has_upgrade = false;

    create_flags.set(needConditionBar, needCondBar);
    create_flags.set(needFactionIcon, needFIcon);
    create_flags.set(needUpgradeIcon, needUgrIcon);

    init();

    UI().Focus().RegisterFocusable(this);
}

CUICellItem::~CUICellItem()
{
    if (m_b_destroy_childs)
        delete_data(m_childs);

    delete_data(m_custom_draw);

    UI().Focus().UnregisterFocusable(this);
}

void CUICellItem::init()
{
    if (!xml_loaded && !uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "actor_menu_item.xml", false))
        return;

    xml_loaded = true;

    m_text = xr_new<CUIStatic>("Text");
    m_text->SetAutoDelete(true);
    AttachChild(m_text);
    CUIXmlInit::InitStatic(uiXml, "cell_item_text", 0, m_text);
    m_text->Show(false);

    /*	m_mark					= new CUIStatic("Mark");
        m_mark->SetAutoDelete	( true );
        AttachChild				( m_mark );
        CUIXmlInit::InitStatic	( uiXml, "cell_item_mark", 0, m_mark );
        m_mark->Show			( false );*/

    if (create_flags.test(needUpgradeIcon))
    {
        m_upgrade = xr_new<CUIStatic>("Upgrade");
        m_upgrade->SetAutoDelete(true);
        AttachChild(m_upgrade);
        CUIXmlInit::InitStatic(uiXml, "cell_item_upgrade", 0, m_upgrade);
        m_upgrade_pos = m_upgrade->GetWndPos();
        m_upgrade->Show(false);
    }

    if (create_flags.test(needFactionIcon))
    {
        m_faction = xr_new<CUIStatic>("OutfitFaction");
        m_faction->SetAutoDelete(true);
        AttachChild(m_faction);
        CUIXmlInit::InitStatic(uiXml, "cell_outfit_faction", 0, m_faction);
        m_faction_pos = m_faction->GetWndPos();
        m_faction->Show(false);
    }

    if (create_flags.test(needConditionBar))
    {
        // Try progress first and then progess
        m_pConditionState = UIHelper::CreateProgressBar(uiXml, "condition_progress_bar", this, false);
    
        if (!m_pConditionState)
            m_pConditionState = UIHelper::CreateProgressBar(uiXml, "condition_progess_bar", this, false);
    
        if (m_pConditionState)
            m_pConditionState->Show(true);
    }
}

void CUICellItem::Draw()
{
    m_drawn_frame = Device.dwFrame;

    inherited::Draw();
    if (m_custom_draw)
        m_custom_draw->OnDraw(this);
};

void CUICellItem::Update()
{
    EnableHeading(m_pParentList->GetVerticalPlacement());
    if (Heading())
    {
        SetHeading(90.0f * (PI / 180.0f));
        SetHeadingPivot(Fvector2().set(0.0f, 0.0f), Fvector2().set(0.0f, GetWndSize().y), true);
    }
    else
        ResetHeadingPivot();

    inherited::Update();

    if (CursorOverWindow())
    {
        Frect clientArea;
        m_pParentList->GetClientArea(clientArea);
        Fvector2 cp = GetUICursor().GetCursorPosition();
        if (clientArea.in(cp))
            GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_FOCUSED_UPDATE, NULL);
    }

    PIItem item = data_is_string ? nullptr : static_cast<PIItem>(m_pData);
    if (create_flags.test(needUpgradeIcon))
    {
        m_has_upgrade = item ? item->has_any_upgrades() : false;
    
        if (m_upgrade)
        {
            if (item)
            {
                Fvector2 pos;
                pos.set(m_upgrade_pos);
                if (ChildsCount())
                {
                    const float textSize = m_text ? m_text->GetWndSize().x : 0.f;
                    pos.x += textSize + 2.0f;
                }
                m_upgrade->SetWndPos(pos);
            }
            m_upgrade->Show(m_has_upgrade);
        }
    }

    if (create_flags.test(needFactionIcon))
    {
        if ((data_is_string && b_isOutfit) || (item && item->BaseSlot() == OUTFIT_SLOT))
        {
            shared_str faction = "";
            if (item)
            {
                CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(item);
                faction = outfit->m_faction;
            }
            else
                faction = s_outfitFaction;
    
            pcstr icon = pSettingsFE->read_if_exists<pcstr>(faction.c_str(), "icon", make_string("icons\\patches\\%s", faction.c_str()).c_str());
            std::string iconPath{icon};
            if (strstr(iconPath.c_str(), "ui\\"))
                iconPath.erase(0, 3);
            m_faction->SetShader(InventoryUtilities::GetEquipmentIconShader(iconPath.c_str()));
            
            Fvector2 pos;
            pos.set(m_faction_pos);
            pos.x = 0;
            pos.y = 13;
    
            m_faction->SetWndPos(pos);
            m_faction->SetStretchTexture(true);
            m_faction->Show(true);
        }
    }
}

bool CUICellItem::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (mouse_action == WINDOW_LBUTTON_DOWN)
    {
        GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_LBUTTON_CLICK, NULL);
        GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_SELECTED, NULL);
        m_mouse_selected_item = this;
        return false;
    }
    else if (mouse_action == WINDOW_MOUSE_MOVE)
    {
        if (pInput->iGetAsyncKeyState(MOUSE_1) && m_mouse_selected_item && m_mouse_selected_item == this)
        {
            GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_DRAG, NULL);
            return true;
        }
    }
    else if (mouse_action == WINDOW_LBUTTON_DB_CLICK)
    {
        GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_DB_CLICK, NULL);
        return true;
    }
    else if (mouse_action == WINDOW_RBUTTON_DOWN)
    {
        GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_RBUTTON_CLICK, NULL);
        return true;
    }

    m_mouse_selected_item = NULL;
    return false;
};

bool CUICellItem::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (WINDOW_KEY_PRESSED == keyboard_action)
    {
        if (GetAccelerator() == dik)
        {
            GetMessageTarget()->SendMessage(this, DRAG_DROP_ITEM_DB_CLICK, NULL);
            return true;
        }
        if (CursorOverWindow())
        {
            const auto [x, y] = UI().GetUICursor().GetCursorPosition();
            switch (GetBindedAction(dik, EKeyContext::UI))
            {
            case kUI_ACCEPT:
                return OnMouseAction(x, y, WINDOW_LBUTTON_DB_CLICK);
            case kUI_ACTION_1:
                return OnMouseAction(x, y, WINDOW_RBUTTON_DOWN);
            } // switch (GetBindedAction(dik, EKeyContext::UI))
        }
    }
    return inherited::OnKeyboardAction(dik, keyboard_action);
}

CUIDragItem* CUICellItem::CreateDragItem()
{
    CUIDragItem* tmp;
    tmp = xr_new<CUIDragItem>(this);
    Frect r;
    GetAbsoluteRect(r);

    if (m_UIStaticItem.GetFixedLTWhileHeading())
    {
        float t1, t2;
        t1 = r.width();
        t2 = r.height() * UI().get_current_kx();

        Fvector2 cp = GetUICursor().GetCursorPosition();

        r.x1 = (cp.x - t2 / 2.0f);
        r.y1 = (cp.y - t1 / 2.0f);
        r.x2 = r.x1 + t2;
        r.y2 = r.y1 + t1;
    }
    tmp->Init(GetShader(), r, GetUIStaticItem().GetTextureRect());
    return tmp;
}

void CUICellItem::SetOwnerList(CUIDragDropListEx* p)
{
    m_pParentList = p;
    //UpdateConditionProgressBar();
}

void CUICellItem::UpdateConditionProgressBar()
{
    if (!m_pConditionState)
        return;

    if (m_pParentList && m_pParentList->GetConditionProgBarVisibility())
    {
        float cond = 1.f;
        bool show_cond{false};

        PIItem itm = data_is_string ? nullptr : static_cast<PIItem>(m_pData);

        if (itm && itm->m_pInventory != nullptr && itm->IsUsingCondition())
        {
            cond = itm->GetCondition();

            CEatableItem* eitm = data_is_string ? nullptr : smart_cast<CEatableItem*>(itm);
            if (eitm)
            {
                const u8 max_uses = eitm->GetMaxUses();
                if (max_uses > 0)
                {
                    const u8 remaining_uses = eitm->GetRemainingUses();
    
                    if (max_uses < 8)
                        m_pConditionState->ShowBackground(false);
    
                    if (remaining_uses < 1)
                        cond = 0.f;
                    else if (max_uses > 8)
                        cond = (float)remaining_uses / (float)max_uses;
                    else
                        cond = (float)remaining_uses * 0.125f - 0.0625f;
    
                    m_pConditionState->UseGradient(false);
                }
            }

            show_cond = true;
        }
        if (data_is_string && (b_isOutfit || b_isWeapon || b_isHelmet || b_isPlastin))
        {
            cond = m_fCondition;
            show_cond = true;
        }

        if (show_cond)
        {
            Ivector2 itm_grid_size = GetGridSize();
            if (m_pParentList->GetVerticalPlacement())
                std::swap(itm_grid_size.x, itm_grid_size.y);
    
            Ivector2 cell_size = m_pParentList->CellSize();
            Ivector2 cell_space = m_pParentList->CellsSpacing();
            float x = 1.f;
            float y = itm_grid_size.y * (cell_size.y + cell_space.y) - m_pConditionState->GetHeight() - 2.f;
    
            m_pConditionState->SetWndPos(Fvector2().set(x, y));
            m_pConditionState->UseColor(true);
            m_pConditionState->ForceSetProgressPos(iCeil(cond * 13.0f) / 13.0f);

            m_pConditionState->Show(true);

            return;
        }
    }

    m_pConditionState->Show(false);
}

bool CUICellItem::EqualTo(CUICellItem* itm)
{
    return (m_grid_size.x == itm->GetGridSize().x) && (m_grid_size.y == itm->GetGridSize().y);
}

u32 CUICellItem::ChildsCount() { return m_childs.size(); }
void CUICellItem::PushChild(CUICellItem* c)
{
    R_ASSERT(c->ChildsCount() == 0);
    VERIFY(this != c);
    m_childs.push_back(c);
    UpdateItemText();
}

CUICellItem* CUICellItem::PopChild(CUICellItem* needed)
{
    CUICellItem* itm = m_childs.back();
    m_childs.pop_back();

    if (needed)
    {
        if (itm != needed)
            std::swap(itm->m_pData, needed->m_pData);
    }
    else
    {
        std::swap(itm->m_pData, m_pData);
    }
    UpdateItemText();
    R_ASSERT(itm->ChildsCount() == 0);
    itm->SetOwnerList(NULL);
    return itm;
}

bool CUICellItem::HasChild(CUICellItem* item)
{
    return (m_childs.end() != std::find(m_childs.begin(), m_childs.end(), item));
}

void CUICellItem::UpdateItemText()
{
    string32 tempStr;
    pcstr finalText = nullptr;
    if (debug_ui_item_cell)
        Msg("CUICellItem::UpdateItemText 1 item: %s childs: %d", m_section_id.c_str(), ChildsCount());
    if (ChildsCount())
    {
        xr_sprintf(tempStr, "x%d", ChildsCount() + 1);
        finalText = tempStr;
    }

    if (m_text)
    {
        if (debug_ui_item_cell)
            Msg("CUICellItem::UpdateItemText 2 item: %s childs: %d finalText: %s", m_section_id.c_str(), ChildsCount(), finalText);
        m_text->Show(nullptr != finalText);
        m_text->SetText(finalText);
    }
    else
    {
        this->SetText(finalText);
    }
}

void CUICellItem::Mark(bool status) { m_cur_mark = status; }
void CUICellItem::SetCustomDraw(ICustomDrawCellItem* c)
{
    if (m_custom_draw)
        xr_delete(m_custom_draw);
    m_custom_draw = c;
}

// -------------------------------------------------------------------------------------------------

CUIDragItem::CUIDragItem(CUICellItem* parent)
    : CUIWindow(CUIDragItem::GetDebugType()), m_static("Static")
{
    m_pParent = parent;
    AttachChild(&m_static);
    Device.seqRender.Add(this, REG_PRIORITY_LOW - 5000);
    Device.seqFrame.Add(this, REG_PRIORITY_LOW - 5000);
    VERIFY(m_pParent->GetMessageTarget());
}

CUIDragItem::~CUIDragItem()
{
    Device.seqRender.Remove(this);
    Device.seqFrame.Remove(this);
    delete_data(m_custom_draw);
}

void CUIDragItem::SetCustomDraw(ICustomDrawDragItem* c)
{
    if (m_custom_draw)
        xr_delete(m_custom_draw);
    m_custom_draw = c;
}

void CUIDragItem::Init(const ui_shader& sh, const Frect& rect, const Frect& text_rect)
{
    SetWndRect(rect);
    m_static.SetShader(sh);
    m_static.SetTextureRect(text_rect);
    m_static.SetWndPos(Fvector2().set(0.0f, 0.0f));
    m_static.SetWndSize(GetWndSize());
    m_static.TextureOn();
    m_static.SetTextureColor(color_rgba(255, 255, 255, 170));
    m_static.SetStretchTexture(true);
    m_pos_offset.sub(rect.lt, GetUICursor().GetCursorPosition());
}

bool CUIDragItem::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (mouse_action == WINDOW_LBUTTON_UP)
    {
        m_pParent->GetMessageTarget()->SendMessage(m_pParent, DRAG_DROP_ITEM_DROP, NULL);
        return true;
    }
    return false;
}

void CUIDragItem::OnRender() { Draw(); }
void CUIDragItem::OnFrame() { Update(); }
void CUIDragItem::Draw()
{
    Fvector2 tmp;
    tmp.sub(GetWndPos(), GetUICursor().GetCursorPosition());
    tmp.sub(m_pos_offset);
    tmp.mul(-1.0f);
    MoveWndDelta(tmp);
    inherited::Draw();
    if (m_custom_draw)
        m_custom_draw->OnDraw(this);
}

void CUIDragItem::SetBackList(CUIDragDropListEx* l)
{
    if (m_back_list)
        m_back_list->OnDragEvent(this, false);

    m_back_list = l;

    if (m_back_list)
        l->OnDragEvent(this, true);
}

Fvector2 CUIDragItem::GetPosition() { return Fvector2().add(m_pos_offset, GetUICursor().GetCursorPosition()); }
