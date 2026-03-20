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
    add_subdirectory("${_XRAY_SENTRY_ROOT}" "${CMAKE_BINARY_DIR}/externals/sentry-native-build")
endif()
