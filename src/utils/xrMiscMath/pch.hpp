#pragma once

#include "Common/Common.hpp"

// XXX: Get rid of xrMiscMath
#ifdef XRAY_STATIC_BUILD
#   define XRCORE_API
#else
#   define XRCORE_API XR_IMPORT
#endif

#include "xrCommon/math_funcs_inline.h"
#include "xrCore/_std_extensions.h"
