#!/usr/bin/env bash
# Ensure Externals/sentry-native exists with crashpad (for WITH_SENTRY / MSBuild sentry build).
# Use when sentry-native is not a git submodule in .gitmodules (e.g. fork only added Sentry in CMake).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NATIVE="${ROOT}/Externals/sentry-native"
CRASHPAD_CMAKE="${NATIVE}/external/crashpad/CMakeLists.txt"
SENTRY_TAG="${SENTRY_NATIVE_VERSION:-0.7.17}"
URL="https://github.com/getsentry/sentry-native.git"

if [[ -f "${NATIVE}/CMakeLists.txt" && -f "${CRASHPAD_CMAKE}" ]]; then
    echo "[ensure_sentry_native] sentry-native + crashpad already present."
    exit 0
fi

if [[ -d "${NATIVE}" ]]; then
    echo "[ensure_sentry_native] ${NATIVE} exists but incomplete; initializing submodules..."
    git -C "${NATIVE}" submodule update --init --recursive
    if [[ -f "${CRASHPAD_CMAKE}" ]]; then
        exit 0
    fi
    echo "[ensure_sentry_native] submodule init failed; replacing tree with fresh clone..."
    rm -rf "${NATIVE}"
fi

echo "[ensure_sentry_native] Cloning sentry-native @ ${SENTRY_TAG} into ${NATIVE}..."
git clone --depth 1 --branch "${SENTRY_TAG}" "${URL}" "${NATIVE}"
git -C "${NATIVE}" submodule update --init --recursive

if [[ ! -f "${CRASHPAD_CMAKE}" ]]; then
    echo "[ensure_sentry_native] ERROR: crashpad still missing at ${CRASHPAD_CMAKE}" >&2
    exit 1
fi
echo "[ensure_sentry_native] OK."
