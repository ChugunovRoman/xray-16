#!/usr/bin/env bash
# Upload Windows PDBs / PE binaries (or other debug files) to Sentry.
# Prerequisites: sentry-cli in PATH, auth via `sentry-cli login` or SENTRY_AUTH_TOKEN.
#
# Usage:
#   chmod +x upload_debug_files.sh
#   ./upload_debug_files.sh [--org ORG] [--project PROJECT] <pdb_directory>
#
# Org/project can also be set with env: SENTRY_ORG, SENTRY_PROJECT
set -euo pipefail

usage() {
    echo "Usage: $0 [--org ORG] [--project PROJECT] <pdb_directory>" >&2
    echo "  Env: SENTRY_ORG, SENTRY_PROJECT (optional if flags are set)" >&2
    exit 1
}

ORG="${SENTRY_ORG:-}"
PROJECT="${SENTRY_PROJECT:-}"
PDB_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h | --help) usage ;;
        -o | --org)
            ORG="${2:-}"
            shift 2 || usage
            ;;
        -p | --project)
            PROJECT="${2:-}"
            shift 2 || usage
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage
            ;;
        *)
            if [[ -n "$PDB_DIR" ]]; then
                echo "Extra argument: $1" >&2
                usage
            fi
            PDB_DIR="$1"
            shift
            ;;
    esac
done

if [[ -z "$PDB_DIR" ]]; then
    echo "Error: pdb_directory is required." >&2
    usage
fi
if [[ -z "$ORG" ]]; then
    echo "Error: org is required (--org or SENTRY_ORG)." >&2
    usage
fi
if [[ -z "$PROJECT" ]]; then
    echo "Error: project is required (--project or SENTRY_PROJECT)." >&2
    usage
fi

if ! command -v sentry-cli >/dev/null 2>&1; then
    echo "Error: sentry-cli not found. Install from https://docs.sentry.io/product/cli/" >&2
    exit 1
fi

exec sentry-cli debug-files upload --org "$ORG" --project "$PROJECT" "$PDB_DIR"
