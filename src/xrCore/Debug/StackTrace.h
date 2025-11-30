#pragma once

xr_vector<xr_string> BuildStackTrace(u16 maxFramesCount = 512);

#ifdef XR_PLATFORM_WINDOWS
xr_vector<xr_string> BuildStackTrace(PCONTEXT threadCtx, u16 maxFramesCount);
#endif
