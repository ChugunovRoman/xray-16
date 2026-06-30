if (XRAY_USE_LUAJIT)
    add_subdirectory(LuaJIT-proj)
else()
    find_package(Lua51 REQUIRED)
endif()

add_subdirectory(luabind)
add_subdirectory(xrLuaFix)
add_subdirectory(OPCODE)
add_subdirectory(ode)

# Silence all warnings for external libraries
add_compile_options(${XRAY_DISABLE_WARNINGS})

add_subdirectory(GameSpy)
add_subdirectory(imgui-proj)

# FreeType: disable optional dependencies to minimize build complexity
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

# Disable PFR font driver (C99 compound literal incompatible with MSVC 17.4+,
# and `local` macro conflict with zutil.h on Linux/macOS). Not needed for the game.
# Patches ftmodule.h and CMakeLists.txt at configure time to exclude PFR.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/freetype/include/freetype/config/ftmodule.h" _ftmodule_content)
string(REPLACE "FT_USE_MODULE( FT_Driver_ClassRec, pfr_driver_class )"
                "/* FT_USE_MODULE( FT_Driver_ClassRec, pfr_driver_class ) -- disabled by xray-16 */"
                _ftmodule_content "${_ftmodule_content}")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/freetype/include/freetype/config/ftmodule.h" "${_ftmodule_content}")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/freetype/CMakeLists.txt" _ftcmake_content)
string(REPLACE "  src/pfr/pfr.c\n"
                "  # src/pfr/pfr.c -- disabled by xray-16\n"
                _ftcmake_content "${_ftcmake_content}")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/freetype/CMakeLists.txt" "${_ftcmake_content}")

add_subdirectory(freetype)

if (NOT TARGET xrLuabind)
    message(FATAL_ERROR
        "You probably have downloaded OpenXRay sources as ZIP archive, you can't do that. Use git to clone the repository.\n"
        "Read the build instructions: https://github.com/OpenXRay/xray-16/wiki"
    )
endif()

if (WITH_SENTRY)
    set(_XRAY_SENTRY_ROOT "${CMAKE_SOURCE_DIR}/Externals/sentry-native")
    if (NOT EXISTS "${_XRAY_SENTRY_ROOT}/external/crashpad/CMakeLists.txt")
        message(FATAL_ERROR
            "WITH_SENTRY needs sentry-native with initialized submodules (crashpad).\n"
            "  git clone --branch 0.7.17 --recursive https://github.com/getsentry/sentry-native.git \"${_XRAY_SENTRY_ROOT}\"\n"
            "Or: cd \"${_XRAY_SENTRY_ROOT}\" && git submodule update --init --recursive")
    endif()
    set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # OpenXRay enables CMAKE_UNITY_BUILD globally; Crashpad Linux snapshot sources then
    # fail to parse under GCC in unity TU (template-id vs <). Build sentry-native without unity.
    set(_xray_saved_unity_build "${CMAKE_UNITY_BUILD}")
    set(CMAKE_UNITY_BUILD OFF)
    add_subdirectory("${_XRAY_SENTRY_ROOT}" "${CMAKE_BINARY_DIR}/externals/sentry-native-build")
    set(CMAKE_UNITY_BUILD "${_xray_saved_unity_build}")
endif()
