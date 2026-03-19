#!/usr/bin/env bash
set -euo pipefail

# Run unit tests (and optionally integration tests) for nanobucket.
# Works when invoked from project root or from the scripts directory.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults
BUILD_DIR="$PROJECT_ROOT/build"
CMAKE_BUILD_TYPE="Release"
FILTER=""          # gtest --gtest_filter pattern (unit tests only)
JOBS=0             # 0 = let ctest decide
RUN_INTEGRATION=0
SERVER_URL="http://localhost:9000"
DRY_RUN=0
VERBOSE=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -b, --build-dir DIR        Build directory (default: $BUILD_DIR)
  -t, --type TYPE            Build type: Release | Debug (default: $CMAKE_BUILD_TYPE)
  -f, --filter PATTERN       Run only tests matching gtest filter pattern
                             e.g. --filter "ListBuckets.*"
  -j, --jobs N               Parallel test jobs (default: ctest decides)
      --integration          Also run integration tests (server must be running)
      --server-url URL       Server URL for integration tests (default: $SERVER_URL)
  -v, --verbose              Show individual test output
  -n, --dry-run              Print commands without executing them
  -h, --help                 Show this help

Examples:
  $(basename "$0")                          # run all unit tests
  $(basename "$0") -f "Error.*"             # run only Error suite
  $(basename "$0") -v                       # verbose output
  $(basename "$0") --integration            # unit + integration (server must be up)
  $(basename "$0") -t Debug                 # run Debug build tests
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

# Parse CLI args
while [ $# -gt 0 ]; do
  case "$1" in
    -b|--build-dir)
      BUILD_DIR="$2"; shift 2 ;;
    -t|--type)
      CMAKE_BUILD_TYPE="$2"; shift 2 ;;
    -f|--filter)
      FILTER="$2"; shift 2 ;;
    -j|--jobs)
      JOBS="$2"; shift 2 ;;
    --integration)
      RUN_INTEGRATION=1; shift ;;
    --server-url)
      SERVER_URL="$2"; shift 2 ;;
    -v|--verbose)
      VERBOSE=1; shift ;;
    -n|--dry-run)
      DRY_RUN=1; shift ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage; exit 1 ;;
  esac
done

CTEST_BIN_DIR="$BUILD_DIR/$CMAKE_BUILD_TYPE"

echo "Project root : $PROJECT_ROOT"
echo "Build dir    : $CTEST_BIN_DIR"
echo "Build type   : $CMAKE_BUILD_TYPE"
[ -n "$FILTER" ]          && echo "Filter       : $FILTER"
[ "$RUN_INTEGRATION" -eq 1 ] && echo "Integration  : ENABLED (server: $SERVER_URL)"
[ "$VERBOSE" -eq 1 ]      && echo "Verbose      : ENABLED"
[ "$DRY_RUN" -eq 1 ]      && echo "Dry-run      : ENABLED"

if [ ! -d "$CTEST_BIN_DIR" ]; then
  echo "Error: build directory not found: $CTEST_BIN_DIR" >&2
  echo "Run ./scripts/build.sh -t $CMAKE_BUILD_TYPE first." >&2
  exit 1
fi

# ── Build ctest command ───────────────────────────────────────────────────────

CTEST_CMD=(ctest --test-dir "$CTEST_BIN_DIR" --output-on-failure)

[ "$VERBOSE" -eq 1 ] && CTEST_CMD+=(-V)
[ "$JOBS"    -gt 0 ] && CTEST_CMD+=(-j "$JOBS")

# Translate gtest filter to ctest -R (regex match on test name)
if [ -n "$FILTER" ]; then
  CTEST_CMD+=(-R "$FILTER")
fi

# Exclude integration tests from the default unit-only run
if [ "$RUN_INTEGRATION" -eq 0 ]; then
  CTEST_CMD+=(-E "integration")
fi

# ── Unit tests ────────────────────────────────────────────────────────────────
echo
echo "━━━ Unit tests ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
run "${CTEST_CMD[@]}"

# ── Integration tests ─────────────────────────────────────────────────────────
if [ "$RUN_INTEGRATION" -eq 1 ]; then
  echo
  echo "━━━ Integration tests ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

  # Verify the server is reachable before running
  if [ "$DRY_RUN" -eq 0 ]; then
    if ! curl -sf --max-time 3 "$SERVER_URL" >/dev/null 2>&1; then
      echo "Error: server not reachable at $SERVER_URL" >&2
      echo "Start it with: ./scripts/start.sh" >&2
      exit 1
    fi
    echo "Server reachable at $SERVER_URL"
  fi

  for script in "$PROJECT_ROOT/test/integration"/*.sh; do
    [ -f "$script" ] || continue
    echo
    echo "--- $(basename "$script") ---"
    run bash "$script" "$SERVER_URL"
  done
fi

echo
echo "All tests passed."
