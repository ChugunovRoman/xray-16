@echo off
rem Game-layout zips of the engine for the launcher (plans/launcher/engine-distribution-plan.md, sec. 4):
rem   res\engine-windows-<arch>-<edition>.zip   exe and dll of bin\<platform>\<configuration>, files in the root
rem   res\symbols-windows-<arch>-<edition>.zip  their pdb
rem The names follow the launcher (launcher-new/src-tauri/src/utils/engine.rs: normalize_arch, normalize_edition).
rem
rem   misc\windows\xr_pack_engine.cmd <configuration> <platform>

if [%1]==[] (
  echo Please, specify configuration
  exit /b 1
)
if [%2]==[] (
  echo Please, specify platform
  exit /b 1
)

set CONFIGURATION=%~1
set PLATFORM=%~2

if /i "%CONFIGURATION%"=="Debug" (
  set EDITION=debug
) else if /i "%CONFIGURATION%"=="Mixed" (
  set EDITION=mixed
) else if /i "%CONFIGURATION%"=="Release" (
  set EDITION=release
) else if /i "%CONFIGURATION%"=="Release Master Gold" (
  set EDITION=mastergold
) else if /i "%CONFIGURATION%"=="ReleaseMasterGold" (
  set EDITION=mastergold
) else (
  echo ::error::xr_pack_engine: unknown configuration "%CONFIGURATION%"
  exit /b 1
)

if /i "%PLATFORM%"=="x64" (
  set ARCH=amd64
) else if /i "%PLATFORM%"=="x86" (
  set ARCH=x86
) else if /i "%PLATFORM%"=="arm64" (
  set ARCH=arm64
) else (
  set ARCH=%PLATFORM%
)

set BIN_DIR=bin\%PLATFORM%\%CONFIGURATION%
if not exist "%BIN_DIR%\" (
  echo ::error::xr_pack_engine: build output "%BIN_DIR%" not found
  exit /b 1
)

set STAGE=res\game_layout
if exist "%STAGE%" rmdir /s /q "%STAGE%"
md "%STAGE%\engine" || exit /b 1
md "%STAGE%\symbols" || exit /b 1

copy /y "%BIN_DIR%\*.exe" "%STAGE%\engine\" >nul || exit /b 1
copy /y "%BIN_DIR%\*.dll" "%STAGE%\engine\" >nul || exit /b 1
rem MFC libraries that Visual Studio copies to the output are not the engine's
del /q /f "%STAGE%\engine\mfc*.dll" 2>nul
copy /y "%BIN_DIR%\*.pdb" "%STAGE%\symbols\" >nul

set ENGINE_ZIP=engine-windows-%ARCH%-%EDITION%.zip
set SYMBOLS_ZIP=symbols-windows-%ARCH%-%EDITION%.zip
if exist "res\%ENGINE_ZIP%" del /q "res\%ENGINE_ZIP%"
if exist "res\%SYMBOLS_ZIP%" del /q "res\%SYMBOLS_ZIP%"

rem 7z adds the contents of the folder (\*), the files land in the root of the zip
7z a -tzip -mx=9 "res\%ENGINE_ZIP%" ".\%STAGE%\engine\*" >nul || exit /b 1
echo xr_pack_engine: res\%ENGINE_ZIP%
if exist "%STAGE%\symbols\*.pdb" (
  7z a -tzip -mx=9 "res\%SYMBOLS_ZIP%" ".\%STAGE%\symbols\*" >nul || exit /b 1
  echo xr_pack_engine: res\%SYMBOLS_ZIP%
) else (
  echo ::warning::xr_pack_engine: no pdb in "%BIN_DIR%", no symbols zip
)

rmdir /s /q "%STAGE%"
exit /b 0
