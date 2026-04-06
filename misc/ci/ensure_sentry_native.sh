#!/usr/bin/env bash
# Ensure Externals/sentry-native exists with crashpad (for WITH_SENTRY / MSBuild sentry build).
# Use when sentry-native is not a git submodule in .gitmodules (e.g. fork only added Sentry in CMake).
#
# sentry-native pulls external/third_party/lss from chromium.googlesource.com; CI runners often get
# HTTP 429 (rate limit). Retries + optional GHA cache of Externals/sentry-native mitigate that.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NATIVE="${ROOT}/Externals/sentry-native"
CRASHPAD_CMAKE="${NATIVE}/external/crashpad/CMakeLists.txt"
SENTRY_TAG="${SENTRY_NATIVE_VERSION:-0.7.17}"
URL="https://github.com/getsentry/sentry-native.git"
# How many times to run `git submodule update` after transient network / 429 failures.
SUBMODULE_TRIES="${SENTRY_NATIVE_SUBMODULE_TRIES:-1}"

have_crashpad() {
    [[ -f "${CRASHPAD_CMAKE}" ]]
}

# Clone googlesource submodules (linux-syscall-support) often fails with 429 from shared CI IPs.
submodule_update_with_retry() {
    local dir="$1"
    local attempt=1
    local delay="${SENTRY_NATIVE_SUBMODULE_RETRY_DELAY_SEC:-20}"
    while (( attempt <= SUBMODULE_TRIES )); do
        if git -C "$dir" submodule update --init --recursive; then
            return 0
        fi
        if (( attempt >= SUBMODULE_TRIES )); then
            echo "[ensure_sentry_native] submodule update failed after ${SUBMODULE_TRIES} attempt(s)." >&2
            return 1
        fi
        echo "[ensure_sentry_native] submodule update failed (attempt ${attempt}/${SUBMODULE_TRIES}), retrying in ${delay}s..." >&2
        sleep "$delay"
        attempt=$((attempt + 1))
        # cap backoff to avoid multi-hour jobs
        if (( delay < 120 )); then
            delay=$((delay * 2))
        fi
    done
}

if ! have_crashpad; then
    if [[ -f "${NATIVE}/CMakeLists.txt" ]]; then
        echo "[ensure_sentry_native] crashpad missing; initializing submodules..."
        submodule_update_with_retry "${NATIVE}"
    fi
fi

if ! have_crashpad; then
    if [[ -d "${NATIVE}" ]]; then
        echo "[ensure_sentry_native] replacing incomplete ${NATIVE}..."
        rm -rf "${NATIVE}"
    fi
    echo "[ensure_sentry_native] Cloning sentry-native @ ${SENTRY_TAG} into ${NATIVE}..."
    attempt=1
    delay="${SENTRY_NATIVE_CLONE_RETRY_DELAY_SEC:-15}"
    while (( attempt <= SUBMODULE_TRIES )); do
        if git clone --depth 1 --branch "${SENTRY_TAG}" "${URL}" "${NATIVE}"; then
            break
        fi
        if (( attempt >= SUBMODULE_TRIES )); then
            echo "[ensure_sentry_native] git clone sentry-native failed after ${SUBMODULE_TRIES} attempt(s)." >&2
            exit 1
        fi
        echo "[ensure_sentry_native] clone failed (attempt ${attempt}/${SUBMODULE_TRIES}), retrying in ${delay}s..." >&2
        rm -rf "${NATIVE}"
        sleep "$delay"
        attempt=$((attempt + 1))
        if (( delay < 120 )); then
            delay=$((delay * 2))
        fi
    done
    submodule_update_with_retry "${NATIVE}"
fi

if ! have_crashpad; then
    echo "[ensure_sentry_native] ERROR: crashpad still missing at ${CRASHPAD_CMAKE}" >&2
    exit 1
fi

echo "[ensure_sentry_native] OK."
