@echo off
rem Builds GlobalWar plugins (native C++ libraries of addons; CMake + Ninja, MSVC) before the engine.
rem Called by gw_plugins.vcxproj (NMake project in engine.sln), can be run by hand too.
rem Docs: wiki/doc/plugins
rem
rem Usage: build_plugins.cmd <Configuration> <Platform> [build|rebuild|clean] [plugins_source_dir]
rem   Configuration: Debug | Mixed | Release | "Release Master Gold"
rem   Platform:      x64 | Win32 | x86   (other platforms are skipped)
rem   plugins_source_dir: default is %GW_PLUGINS_DIR%, then ..\..\..\GlobalWar\addons (common folder of all addons)
rem
rem Tools: VS 2022 (MSVC v143) developer environment, CMake and Ninja bundled with that Visual Studio.

rem No delayed expansion on purpose: it would eat "!" in user paths.
setlocal EnableExtensions

set "GW_CFG=%~1"
set "GW_PLATFORM=%~2"
set "GW_ACTION=%~3"
set "GW_SRC=%~4"
if "%GW_ACTION%"=="" set "GW_ACTION=build"

rem --- configuration mapping (engine -> CMake) --------------------------------
set "GW_BUILD_TYPE="
if /I "%GW_CFG%"=="Debug" set "GW_BUILD_TYPE=Debug"
if /I "%GW_CFG%"=="Mixed" set "GW_BUILD_TYPE=RelWithDebInfo"
if /I "%GW_CFG%"=="Release" set "GW_BUILD_TYPE=Release"
if /I "%GW_CFG%"=="Release Master Gold" set "GW_BUILD_TYPE=Release"
if not defined GW_BUILD_TYPE (
    echo [gw_plugins] error: unknown configuration "%GW_CFG%"
    exit /b 1
)

rem --- platform mapping --------------------------------------------------------
set "GW_ARCH="
if /I "%GW_PLATFORM%"=="x64" set "GW_ARCH=x64"
if /I "%GW_PLATFORM%"=="Win32" set "GW_ARCH=x86"
if /I "%GW_PLATFORM%"=="x86" set "GW_ARCH=x86"
if not defined GW_ARCH (
    echo [gw_plugins] platform "%GW_PLATFORM%" is not supported, skipping
    exit /b 0
)

rem --- Plugin API reference page (wiki/doc/plugins/api/all.md) ------------------
rem Regenerated from gwp_api.h / gwp.hpp on every build; an untagged API function fails the build.
if /I "%GW_ACTION%"=="clean" goto :after_api_index
call :gen_api_index || exit /b 1
:after_api_index

rem --- source and build directories -------------------------------------------
if "%GW_SRC%"=="" set "GW_SRC=%GW_PLUGINS_DIR%"
if "%GW_SRC%"=="" set "GW_SRC=%~dp0..\..\..\GlobalWar\addons"
for %%I in ("%GW_SRC%") do set "GW_SRC=%%~fI"
if not exist "%GW_SRC%\CMakeLists.txt" (
    echo [gw_plugins] no plugins at "%GW_SRC%", skipping
    exit /b 0
)

for %%I in ("%~dp0..\xrAddonHost\include") do set "GW_SDK=%%~fI"

set "GW_CFG_TAG=%GW_CFG: =_%"
for %%I in ("%~dp0..\..\build\plugins\%GW_ARCH%-%GW_CFG_TAG%") do set "GW_BUILD=%%~fI"

rem --- locate Visual Studio 2022 ---------------------------------------------
set "GW_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%GW_VSWHERE%" (
    echo [gw_plugins] error: vswhere.exe not found, Visual Studio 2022 is required
    exit /b 1
)
set "GW_VS="
for /f "usebackq delims=" %%I in (`"%GW_VSWHERE%" -version [17.0^,18.0^) -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "GW_VS=%%I"
if not defined GW_VS (
    echo [gw_plugins] error: Visual Studio 2022 with C++ tools not found
    exit /b 1
)

set "GW_CMAKE=%GW_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "GW_NINJA=%GW_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%GW_CMAKE%" (
    echo [gw_plugins] error: CMake not found in "%GW_VS%". Install the "C++ CMake tools for Windows" component.
    exit /b 1
)
if not exist "%GW_NINJA%" (
    echo [gw_plugins] error: Ninja not found in "%GW_VS%". Install the "C++ CMake tools for Windows" component.
    exit /b 1
)

rem --- MSVC environment (target arch, x64 host tools) --------------------------
rem VsDevCmd.bat calls vswhere.exe by name.
for %%I in ("%GW_VSWHERE%") do set "PATH=%%~dpI;%PATH%"
call "%GW_VS%\Common7\Tools\VsDevCmd.bat" -arch=%GW_ARCH% -host_arch=x64 -no_logo
if errorlevel 1 (
    echo [gw_plugins] error: VsDevCmd.bat failed
    exit /b 1
)

echo [gw_plugins] %GW_ACTION% %GW_BUILD_TYPE% %GW_ARCH%
echo [gw_plugins]   source: %GW_SRC%
echo [gw_plugins]   build:  %GW_BUILD%

if /I "%GW_ACTION%"=="clean" goto :clean

rem A build dir is bound to one source dir; start clean when the source dir changes.
set "GW_SRC_MARKER=%GW_BUILD%\gw_source_dir.txt"
set "GW_PREV_SRC="
if exist "%GW_SRC_MARKER%" set /p GW_PREV_SRC=<"%GW_SRC_MARKER%"
if exist "%GW_BUILD%\CMakeCache.txt" if /I not "%GW_PREV_SRC%"=="%GW_SRC%" (
    echo [gw_plugins] source dir changed, recreating build dir
    rmdir /s /q "%GW_BUILD%"
)
if not exist "%GW_BUILD%" mkdir "%GW_BUILD%"
> "%GW_SRC_MARKER%" echo %GW_SRC%

if not exist "%GW_BUILD%\CMakeCache.txt" (
    "%GW_CMAKE%" -S "%GW_SRC%" -B "%GW_BUILD%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=%GW_BUILD_TYPE% ^
        -DCMAKE_MAKE_PROGRAM="%GW_NINJA%" ^
        -DCMAKE_CXX_COMPILER=cl.exe ^
        -DGWP_SDK_INCLUDE_DIR="%GW_SDK%"
    if errorlevel 1 (
        echo [gw_plugins] error: CMake configure failed
        exit /b 1
    )
)

set "GW_BUILD_ARGS="
if /I "%GW_ACTION%"=="rebuild" set "GW_BUILD_ARGS=--clean-first"

"%GW_CMAKE%" --build "%GW_BUILD%" %GW_BUILD_ARGS%
if errorlevel 1 (
    echo [gw_plugins] error: build failed
    exit /b 1
)
exit /b 0

:clean
if not exist "%GW_BUILD%\build.ninja" exit /b 0
"%GW_CMAKE%" --build "%GW_BUILD%" --target clean
exit /b %errorlevel%

rem Generates the Plugin API reference page with gen_plugin_api_index.py (this folder). Skipped (not an error)
rem without Python; the script itself skips when the wiki or tools\wiki_plugins_toc.py is not next to xray-16,
rem e.g. on the engine CI where only xray-16 is checked out.
:gen_api_index
set "GW_API_INDEX=%~dp0gen_plugin_api_index.py"
set "GW_PYTHON="
where python >nul 2>nul && set "GW_PYTHON=python"
if not defined GW_PYTHON where py >nul 2>nul && set "GW_PYTHON=py -3"
if not defined GW_PYTHON (
    echo [gw_plugins] warning: Python not found, Plugin API reference page not updated
    exit /b 0
)
%GW_PYTHON% "%GW_API_INDEX%" --headers "%~dp0..\xrAddonHost\include\gwp"
if errorlevel 1 (
    echo [gw_plugins] error: Plugin API reference page generation failed
    exit /b 1
)
exit /b 0
