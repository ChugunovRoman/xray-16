#pragma once

#include "xrCore/xrCore.h"
#include "xrCore/xrstring.h"

namespace ltx_multiline
{
// One physical line from LTX/INI: "\\n" (two chars) -> '\n', "\\\\" -> '\\'
XRCORE_API void UnescapeLtxLineToInternal(xr_string& dest, pcstr src);
// Internal text with real newlines -> single line safe for LTX value
XRCORE_API void EscapeInternalToLtxLine(xr_string& dest, pcstr src);
} // namespace ltx_multiline
