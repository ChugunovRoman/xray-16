#pragma once

#ifdef XRAY_STATIC_BUILD
#   define XRSCRIPTENGINE_API
#else
#   ifdef XRSCRIPTENGINE_EXPORTS
#      define XRSCRIPTENGINE_API XR_EXPORT
#   else
#      define XRSCRIPTENGINE_API XR_IMPORT
#   endif
#endif

extern "C"
{
typedef struct lua_State lua_State;
typedef struct lua_Debug lua_Debug;
}
