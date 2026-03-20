cmake_minimum_required(VERSION 3.23)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PreProjectInit)

calculate_xray_build_id(XRAY_BUILD_ID)
project(OpenXRay
    DESCRIPTION "OpenXRay is an improved version of the X-Ray Engine, \
                the game engine used in the world-famous S.T.A.L.K.E.R. game series by GSC Game World."
    VERSION 1.6.02.${XRAY_BUILD_ID}
    HOMEPAGE_URL "https://github.com/OpenXRay/xray-16"
    LANGUAGES CXX C
)

# Crashpad (Sentry) looks up mach *.defs under ${CMAKE_OSX_SYSROOT}/usr/include/mach/.
# If SYSROOT is empty (some generators/CI), paths become /usr/include/... and configure fails.
if (APPLE AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _xray_osx_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _xray_xcrun_rv)
    if (_xray_xcrun_rv EQUAL 0 AND EXISTS "${_xray_osx_sdk}")
        set(CMAKE_OSX_SYSROOT "${_xray_osx_sdk}" CACHE PATH "macOS SDK path" FORCE)
        message(STATUS "CMAKE_OSX_SYSROOT set to ${CMAKE_OSX_SYSROOT} (xcrun --show-sdk-path)")
    endif()
endif()

option(BUILD_SHARED_LIBS "Build shared libraries"     ${BUILD_SHARED_LIBS_DEFAULT_VALUE})

option(XRAY_USE_LUAJIT   "Use LuaJIT"                 ON)
option(XRAY_USE_ASAN     "Use AddressSanitizer"       OFF)
option(XRAY_ENABLE_TRACY "Enable tracy profiler"      OFF)
option(WITH_SENTRY "Enable Sentry crash reporting (requires Externals/sentry-native with submodules)" OFF)

include(XRay.Build)
include(XRay.Packaging)

add_subdirectory(Externals)
add_subdirectory(src)
add_subdirectory(res)
add_subdirectory(misc)
