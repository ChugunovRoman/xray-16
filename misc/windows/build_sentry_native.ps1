# Builds sentry-native for MSVC and installs into sdk/sentry/<x64|x86>/<Configuration>/
# for use with MSBuild when UseSentry=true. Requires CMake and a VS developer environment.
#
# Run once per (Platform, Configuration) you build in VS — not before every engine rebuild.
# Rebuild sentry-native when you upgrade the sentry-native tree, change MSVC, or add a new matrix combo.
# Bash equivalent: misc/windows/build_sentry_native.sh
param(
    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",
    [ValidateSet("Debug", "Release", "Mixed", "Release Master Gold")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sentrySrc = Join-Path $root "Externals\sentry-native"
if (-not (Test-Path (Join-Path $sentrySrc "CMakeLists.txt"))) {
    Write-Error "Missing $sentrySrc. Clone with: git clone --depth 1 --branch 0.7.17 https://github.com/getsentry/sentry-native.git Externals/sentry-native"
}
$crashpad = Join-Path $sentrySrc "external\crashpad\CMakeLists.txt"
if (-not (Test-Path $crashpad)) {
    Write-Host "Initializing sentry-native submodules (crashpad, etc.)..."
    Push-Location $sentrySrc
    try {
        git submodule update --init --recursive
    } finally {
        Pop-Location
    }
    if (-not (Test-Path $crashpad)) {
        Write-Error "crashpad still missing. Run: cd Externals\sentry-native && git submodule update --init --recursive"
    }
}

$arch = if ($Platform -eq "Win32") { "x86" } else { "x64" }
$buildType = if ($Configuration -like "*Debug*") { "Debug" } else { "Release" }
$installRoot = Join-Path $root "sdk\sentry\$arch\$Configuration"
$buildDir = Join-Path $root "build\sentry-native-$arch-$Configuration"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $installRoot | Out-Null

$platArg = if ($Platform -eq "Win32") { "Win32" } else { "x64" }

# Locate VS and pick the matching CMake generator by major version.
# windows-latest on GitHub Actions may have VS 17 (2022) or VS 18 (2025).
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstance = $null
$generator = "Visual Studio 17 2022"  # fallback
if (Test-Path $vsWhere) {
    $vsInstance = & $vsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if (-not $vsInstance) {
        $vsInstance = & $vsWhere -latest -products * -property installationPath 2>$null
    }
    $vsVersion = & $vsWhere -latest -products * -property installationVersion 2>$null
    $vsMajor = if ($vsVersion) { [int]($vsVersion -split '\.')[0] } else { 17 }
    $generator = switch ($vsMajor) {
        18 { "Visual Studio 18 2026" }
        17 { "Visual Studio 17 2022" }
        16 { "Visual Studio 16 2019" }
        default { "Visual Studio 17 2022" }
    }
}
Write-Host "Generator: $generator  Instance: $vsInstance"

$msvcRt = if ($buildType -eq "Debug") { "MultiThreadedDebugDLL" } else { "MultiThreadedDLL" }
$cmakeConfigArgs = @(
    "-S", $sentrySrc, "-B", $buildDir,
    "-G", $generator, "-A", $platArg,
    "-DCMAKE_INSTALL_PREFIX=$installRoot",
    "-DSENTRY_BUILD_TESTS=OFF",
    "-DSENTRY_BUILD_EXAMPLES=OFF",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$msvcRt"
)
cmake @cmakeConfigArgs

cmake --build $buildDir --config $buildType --parallel
cmake --install $buildDir --config $buildType

# crashpad_handler is next to libs in the build tree; copy if install omitted it
$handler = Join-Path $buildDir "crashpad_build\handler\$buildType\crashpad_handler.exe"
if (-not (Test-Path $handler)) {
    $handler = Join-Path $buildDir "crashpad_build\handler\Release\crashpad_handler.exe"
}
if (-not (Test-Path $handler)) {
    $handler = Get-ChildItem -Path $buildDir -Recurse -Filter crashpad_handler.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($handler) { $handler = $handler.FullName }
}
if (Test-Path $handler) {
    Copy-Item -Force $handler (Join-Path $installRoot "crashpad_handler.exe")
    Write-Host "Installed crashpad_handler.exe to $installRoot"
} else {
    Write-Warning "crashpad_handler.exe not found under $buildDir; search build output manually."
}

Write-Host "Sentry install prefix: $installRoot"
