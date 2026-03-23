#include "stdafx.h"
#include "LtxMultilineString.hpp"

namespace ltx_multiline
{
void UnescapeLtxLineToInternal(xr_string& dest, pcstr src)
{
    dest.clear();
    if (!src)
        return;
    for (pcstr p = src; *p; ++p)
    {
        if (*p == '\\' && p[1])
        {
            if (p[1] == 'n')
            {
                dest += '\n';
                ++p;
                continue;
            }
            if (p[1] == '\\')
            {
                dest += '\\';
                ++p;
                continue;
            }
        }
        dest += *p;
    }
}

void EscapeInternalToLtxLine(xr_string& dest, pcstr src)
{
    dest.clear();
    if (!src)
        return;
    for (pcstr p = src; *p; ++p)
    {
        if (*p == '\\')
            dest += "\\\\";
        else if (*p == '\n')
            dest += "\\n";
        else
            dest += *p;
    }
}
} // namespace ltx_multiline
