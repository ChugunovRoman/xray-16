// File:		UILine.cpp
// Description:	Single text line
// Created:		05.04.2005
// Author:		Serge Vynnycheko
// Mail:		narrator@gsc-game.kiev.ua
//
// Copyright 2005 GSC Game World

#include "pch.hpp"
#include "UILine.h"
#include "ui_base.h"
#include "xrEngine/GameFont.h"

namespace
{
// LTX/XML uses the two-character sequence \ + n; editors insert real CR/LF bytes.
void find_next_line_break(const xr_string& t, size_t& out_pos, size_t& out_skip)
{
    out_pos = xr_string::npos;
    out_skip = 0;

    auto consider = [&](size_t p, size_t sk)
    {
        if (p == xr_string::npos)
            return;
        if (out_pos == xr_string::npos || p < out_pos)
        {
            out_pos = p;
            out_skip = sk;
        }
    };

    consider(t.find("\\n"), 2);

    for (size_t k = 0; k < t.size();)
    {
        const unsigned char c = static_cast<unsigned char>(t[k]);
        if (c == '\r' && k + 1 < t.size() && t[k + 1] == '\n')
        {
            consider(k, 2);
            k += 2;
        }
        else if (c == '\r')
        {
            consider(k, 1);
            k += 1;
        }
        else if (c == '\n')
        {
            consider(k, 1);
            k += 1;
        }
        else
            ++k;
    }
}
} // namespace

void CUILine::ProcessNewLines()
{
    for (u32 i = 0; i < m_subLines.size(); i++)
    {
        size_t pos;
        size_t skip;
        find_next_line_break(m_subLines[i].m_text, pos, skip);
        if (pos == xr_string::npos)
            continue;

        CUISubLine sbLine;
        if (pos)
            sbLine = m_subLines[i].Cut2Pos(pos - 1);
        sbLine.m_last_in_line = true;
        m_subLines.insert(m_subLines.begin() + i, sbLine);
        m_subLines[i + 1].m_text.erase(0, skip);
        if (m_subLines[i + 1].m_text.empty())
        {
            m_subLines.erase(m_subLines.begin() + i + 1);
        }
    }
}

void CUILine::Draw(CGameFont* pFont, float x, float y) const
{
    float length = 0;

    for (const auto& subline : m_subLines)
    {
        subline.Draw(pFont, x + length, y);
        float ll = pFont->SizeOf_(subline.m_text.c_str()); //. all ok
        UI().ClientToScreenScaledWidth(ll);
        length += ll;
    }
}
