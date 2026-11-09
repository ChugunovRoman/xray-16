# gw_merge: the merge of the replacements of a text file by the addons outside the engine - for the catalog CI and the
# launcher (plans/addons_api_support/15-merge-implementation.md section 7.1). The sources of xrCore/ContentMerge are
# compiled in (CONTENT_MERGE_STANDALONE: the standard library only), the tool does not link xrCore. LuaJIT for
# --check-lua is loaded at run time (dlopen / LoadLibrary), it is not linked either.
#
# Two ways to build it:
# - inside the engine (src/utils/CMakeLists.txt): the options of the engine (cmake/XRay.Build.cmake);
# - alone, without the engine and its dependencies: cmake -S src/utils/gw_merge -B <dir> && cmake --build <dir>.
#   The Linux build of the engine CI for the catalog of the addons (job gw_merge in .github/workflows/cibuild.yml)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(gw_merge LANGUAGES C CXX) # C: miniz (the gzip of the snapshot of plan --base-snapshot)

    # The same standard as the engine (cmake/XRay.Build.cmake)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # libstdc++ and libgcc into the binary: it runs on a machine without the compiler runtime of the build machine
    # (a CI runner of the catalog). glibc stays shared: --check-lua loads LuaJIT with dlopen
    option(GW_MERGE_STATIC_RUNTIME "Link the C++ runtime statically (GCC / Clang, not Apple)" ON)
    set(GW_MERGE_STANDALONE ON)
endif()

add_executable(gw_merge)

get_filename_component(GW_MERGE_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/../../xrCore/ContentMerge" ABSOLUTE)
# miniz of xrCore (vendored, public domain): the inflate of the gzip files of plan --base-snapshot (base_source.cpp)
get_filename_component(GW_MERGE_MINIZ "${CMAKE_CURRENT_SOURCE_DIR}/../../xrCore/Compression/miniz" ABSOLUTE)

target_sources(gw_merge PRIVATE
    main.cpp
    base_source.cpp
    base_source.h
    "${GW_MERGE_MINIZ}/miniz.c"
    "${GW_MERGE_MINIZ}/miniz.h"
    "${GW_MERGE_LIBRARY}/AtomMerge.cpp"
    "${GW_MERGE_LIBRARY}/AtomMerge.h"
    "${GW_MERGE_LIBRARY}/ContentMerge.cpp"
    "${GW_MERGE_LIBRARY}/ContentMerge.h"
    "${GW_MERGE_LIBRARY}/Delta.cpp"
    "${GW_MERGE_LIBRARY}/DeltaJson.cpp"
    "${GW_MERGE_LIBRARY}/DeltaJson.h"
    "${GW_MERGE_LIBRARY}/Diff.cpp"
    "${GW_MERGE_LIBRARY}/Diff.h"
    "${GW_MERGE_LIBRARY}/LtxSegments.cpp"
    "${GW_MERGE_LIBRARY}/LuaSegments.cpp"
    "${GW_MERGE_LIBRARY}/Plan.cpp"
    "${GW_MERGE_LIBRARY}/Plan.h"
    "${GW_MERGE_LIBRARY}/Segments.cpp"
    "${GW_MERGE_LIBRARY}/Segments.h"
    "${GW_MERGE_LIBRARY}/Sha256.cpp"
    "${GW_MERGE_LIBRARY}/TextNorm.cpp"
    "${GW_MERGE_LIBRARY}/TextNorm.h"
    "${GW_MERGE_LIBRARY}/XmlSegments.cpp"
)

target_include_directories(gw_merge
    PRIVATE
    "${GW_MERGE_LIBRARY}"
    "${GW_MERGE_MINIZ}"
)

target_compile_definitions(gw_merge
    PRIVATE
    CONTENT_MERGE_STANDALONE
    # miniz: the inflate only, no stdio / time / archives, no zlib names
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_DEFLATE_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES
)

# A vendored C library: its warnings are not ours to fix (as in xrCore)
set_source_files_properties("${GW_MERGE_MINIZ}/miniz.c" PROPERTIES
    COMPILE_OPTIONS "$<$<C_COMPILER_ID:MSVC>:/W1>$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>"
)

target_link_libraries(gw_merge
    PRIVATE
    ${CMAKE_DL_LIBS}
)

if (GW_MERGE_STANDALONE AND GW_MERGE_STATIC_RUNTIME AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(gw_merge PRIVATE -static-libstdc++ -static-libgcc)
endif()

# --check-lua without a path: dlopen("libluajit-5.1.so.2") searches the RUNPATH of the caller, not the folder of the
# executable - $ORIGIN makes it find the LuaJIT packed next to gw_merge (gw_merge-linux-x86_64.tar.gz). Windows
# (LoadLibrary searches the folder of the exe) and macOS need nothing
if (GW_MERGE_STANDALONE AND NOT WIN32 AND NOT APPLE)
    set_target_properties(gw_merge PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
endif()

if (WIN32)
    # CommandLineToArgvW: the arguments as UTF-16 (the names out of the ANSI code page)
    target_link_libraries(gw_merge PRIVATE shell32)
endif()
