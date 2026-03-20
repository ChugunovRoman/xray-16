#!/usr/bin/env bash
# Builds sentry-native with MSVC and installs into sdk/sentry/<x64|x86>/<Configuration>/
# for MSBuild when UseSentry=true.
#
# Rebuild sentry-native only when: upgrading sentry-native, changing MSVC/toolchain, or adding
# a new VS Configuration/Platform — not on every engine rebuild.
#
# Usage (from repo root or any dir):
#   bash misc/windows/build_sentry_native.sh [x64|Win32] ["Release"|"Debug"|"Mixed"|"Release Master Gold"]
#
# Requires: CMake, Visual Studio 2022 (generator "Visual Studio 17 2022"), Git.
# Run from Git Bash on Windows, or any shell that has cmake + MSVC in PATH.

set -euo pipefail

PLATFORM="${1:-x64}"
CONFIGURATION="${2:-Release}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SENTRY_SRC="${ROOT}/Externals/sentry-native"
CRASHPAD_CMAKE="${SENTRY_SRC}/external/crashpad/CMakeLists.txt"

if [[ ! -f "${SENTRY_SRC}/CMakeLists.txt" ]]; then
  echo "Missing ${SENTRY_SRC}. Clone sentry-native (e.g. tag 0.7.17) into Externals/sentry-native" >&2
  exit 1
fi

if [[ ! -f "${CRASHPAD_CMAKE}" ]]; then
  echo "Initializing sentry-native submodules..."
  (cd "${SENTRY_SRC}" && git submodule update --init --recursive)
  if [[ ! -f "${CRASHPAD_CMAKE}" ]]; then
    echo "crashpad still missing: cd Externals/sentry-native && git submodule update --init --recursive" >&2
    exit 1
  fi
fi

case "${PLATFORM}" in
  Win32) ARCH=x86; PLAT_ARG=Win32 ;;
  x64)   ARCH=x64; PLAT_ARG=x64 ;;
  *) echo "Platform must be x64 or Win32, got: ${PLATFORM}" >&2; exit 1 ;;
esac

if [[ "${CONFIGURATION}" == *Debug* ]]; then
  BUILD_TYPE=Debug
  MSVC_RT=MultiThreadedDebugDLL
else
  BUILD_TYPE=Release
  MSVC_RT=MultiThreadedDLL
fi

# MSBuild $(Configuration) may contain spaces — install path must match exactly.
INSTALL_ROOT="${ROOT}/sdk/sentry/${ARCH}/${CONFIGURATION}"
BUILD_SLUG="${CONFIGURATION// /_}"
BUILD_DIR="${ROOT}/build/sentry-native-${ARCH}-${BUILD_SLUG}"

mkdir -p "${BUILD_DIR}" "${INSTALL_ROOT}"

cmake -S "${SENTRY_SRC}" -B "${BUILD_DIR}" \
  -G "Visual Studio 17 2022" \
  -A "${PLAT_ARG}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_ROOT}" \
  -DSENTRY_BUILD_TESTS=OFF \
  -DSENTRY_BUILD_EXAMPLES=OFF \
  "-DCMAKE_MSVC_RUNTIME_LIBRARY=${MSVC_RT}"

cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" --parallel
cmake --install "${BUILD_DIR}" --config "${BUILD_TYPE}"

HANDLER="${BUILD_DIR}/crashpad_build/handler/${BUILD_TYPE}/crashpad_handler.exe"
if [[ ! -f "${HANDLER}" ]]; then
  HANDLER="${BUILD_DIR}/crashpad_build/handler/Release/crashpad_handler.exe"
fi
if [[ ! -f "${HANDLER}" ]]; then
  HANDLER="$(find "${BUILD_DIR}" -name crashpad_handler.exe 2>/dev/null | head -n1 || true)"
fi

if [[ -n "${HANDLER}" && -f "${HANDLER}" ]]; then
  cp -f "${HANDLER}" "${INSTALL_ROOT}/crashpad_handler.exe"
  echo "Installed crashpad_handler.exe to ${INSTALL_ROOT}"
else
  echo "Warning: crashpad_handler.exe not found under ${BUILD_DIR}" >&2
fi

echo "Sentry install prefix: ${INSTALL_ROOT}"
