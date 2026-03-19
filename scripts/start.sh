#!/usr/bin/env bash
set -euo pipefail

# Run the built nanobucket binary.
# Works when invoked from project root or from the scripts directory.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults
BUILD_DIR="$PROJECT_ROOT/build"
CMAKE_BUILD_TYPE="Release"
EXTRA_ARGS=()

DRY_RUN=0

PORT="${NANOBUCKET_PORT:-9000}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- <binary args>]

Options:
  -b, --build-dir DIR    Build directory (default: $BUILD_DIR)
  -t, --type TYPE        Build type used when building: Release or Debug (default: $CMAKE_BUILD_TYPE)
  -p, --port PORT        Port to listen on (default: $PORT, overrides NANOBUCKET_PORT env var)
  -n, --dry-run          Print command without executing it
  -h, --help             Show this help

Any arguments after -- are forwarded directly to the nanobucket binary.

Examples:
  $(basename "$0")                   # run Release build on default port
  $(basename "$0") -t Debug -p 9001  # Debug build on port 9001
  NANOBUCKET_PORT=7480 $(basename "$0")  # set port via env var
EOF
}

run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "+ $*"
  else
    echo "+ $*"
    "$@"
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    -b|--build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    -t|--type)
      CMAKE_BUILD_TYPE="$2"
      shift 2
      ;;
    -p|--port)
      PORT="$2"
      shift 2
      ;;
    -n|--dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      EXTRA_ARGS=("$@")
      break
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

BINARY="$BUILD_DIR/$CMAKE_BUILD_TYPE/src/nanobucket"

if [ ! -f "$BINARY" ]; then
  echo "Error: binary not found at $BINARY" >&2
  echo "Run ./scripts/build.sh -t $CMAKE_BUILD_TYPE first." >&2
  exit 1
fi

echo "Project root : $PROJECT_ROOT"
echo "Binary       : $BINARY"
echo "Build type   : $CMAKE_BUILD_TYPE"
echo "Port         : $PORT"

# Always run from project root so relative paths in the binary (logs/, data/) resolve correctly.
mkdir -p "$PROJECT_ROOT/logs"
cd "$PROJECT_ROOT"

export NANOBUCKET_PORT="$PORT"
run "$BINARY" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
