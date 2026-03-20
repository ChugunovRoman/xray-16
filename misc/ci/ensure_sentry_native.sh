#!/usr/bin/env bash
# Ensure Externals/sentry-native exists with crashpad (for WITH_SENTRY / MSBuild sentry build).
# Use when sentry-native is not a git submodule in .gitmodules (e.g. fork only added Sentry in CMake).
# After a fresh clone, applies misc/ci/apply_crashpad_gcc_fix.py (GCC + unity build template parse).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NATIVE="${ROOT}/Externals/sentry-native"
CRASHPAD_CMAKE="${NATIVE}/external/crashpad/CMakeLists.txt"
SENTRY_TAG="${SENTRY_NATIVE_VERSION:-0.7.17}"
URL="https://github.com/getsentry/sentry-native.git"

have_crashpad() {
    [[ -f "${CRASHPAD_CMAKE}" ]]
}

if ! have_crashpad; then
    if [[ -f "${NATIVE}/CMakeLists.txt" ]]; then
        echo "[ensure_sentry_native] crashpad missing; initializing submodules..."
        git -C "${NATIVE}" submodule update --init --recursive
    fi
fi

if ! have_crashpad; then
    if [[ -d "${NATIVE}" ]]; then
        echo "[ensure_sentry_native] replacing incomplete ${NATIVE}..."
        rm -rf "${NATIVE}"
    fi
    echo "[ensure_sentry_native] Cloning sentry-native @ ${SENTRY_TAG} into ${NATIVE}..."
    git clone --depth 1 --branch "${SENTRY_TAG}" "${URL}" "${NATIVE}"
    git -C "${NATIVE}" submodule update --init --recursive
fi

if ! have_crashpad; then
    echo "[ensure_sentry_native] ERROR: crashpad still missing at ${CRASHPAD_CMAKE}" >&2
    exit 1
fi

_fix_py="${ROOT}/misc/ci/apply_crashpad_gcc_fix.py"
if [[ -f "${_fix_py}" ]]; then
    if command -v python3 >/dev/null 2>&1; then
        python3 "${_fix_py}"
    elif command -v python >/dev/null 2>&1; then
        python "${_fix_py}"
    else
        echo "[ensure_sentry_native] WARNING: python3/python not found; skip apply_crashpad_gcc_fix.py" >&2
    fi
fi

echo "[ensure_sentry_native] OK."
