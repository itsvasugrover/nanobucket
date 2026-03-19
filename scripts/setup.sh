#!/usr/bin/env bash
set -euo pipefail

# Robust, modular setup script for conan dependencies.
# Works when invoked from project root or from the scripts directory.
# Adds options for build dir, build type, conan profile and extra conan args,
# and supports dry-run and skipping conan entirely.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults
BUILD_DIR="$PROJECT_ROOT/build"
CMAKE_BUILD_TYPE="Release"
CONAN_BUILD_MISSING="--build=missing"
CONAN_PROFILE=""
CONAN_EXTRA_OPTS_STR=""
CONAN_EXTRA_OPTS=()

DRY_RUN=0
SKIP_CONAN=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -b, --build-dir DIR       Build directory (default: $BUILD_DIR)
  -t, --type TYPE           Build type / config (default: $CMAKE_BUILD_TYPE)
  -p, --profile PROFILE     Conan profile to use (passed as --profile)
      --conan-args STR      Extra conan install args (quoted string; will be split on whitespace)
      --no-conan            Skip running 'conan install' and sourcing conanbuild.sh
  -n, --dry-run             Print commands without executing them
  -h, --help                Show this help
EOF
}

# Helper to run commands (respects dry-run)
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
      BUILD_DIR="$2"
      shift 2
      ;;
    -t|--type)
      CMAKE_BUILD_TYPE="$2"
      shift 2
      ;;
    -p|--profile)
      CONAN_PROFILE="$2"
      shift 2
      ;;
    --conan-args)
      CONAN_EXTRA_OPTS_STR="$2"
      shift 2
      ;;
    --no-conan)
      SKIP_CONAN=1
      shift
      ;;
    -n|--dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1
      ;;
  esac
done

# Split extra conan args string into array (if provided)
if [ -n "${CONAN_EXTRA_OPTS_STR:-}" ]; then
  # shellcheck disable=SC2206
  read -r -a CONAN_EXTRA_OPTS <<< "$CONAN_EXTRA_OPTS_STR"
fi

echo "Project root: $PROJECT_ROOT"
echo "Build dir: $BUILD_DIR"
echo "Build type: $CMAKE_BUILD_TYPE"
if [ -n "$CONAN_PROFILE" ]; then
  echo "Conan profile: $CONAN_PROFILE"
fi
if [ "${#CONAN_EXTRA_OPTS[@]}" -gt 0 ]; then
  echo "Extra conan args: ${CONAN_EXTRA_OPTS[*]}"
fi
if [ "$SKIP_CONAN" -eq 1 ]; then
  echo "Conan install: SKIPPED"
else
  echo "Conan install: ENABLED"
fi
if [ "$DRY_RUN" -eq 1 ]; then
  echo "Dry-run: ENABLED"
fi

if [ "$SKIP_CONAN" -eq 0 ]; then
  # Ensure conan is available
  if ! command -v conan >/dev/null 2>&1; then
    echo "Error: 'conan' not found in PATH. Install conan or use --no-conan to skip." >&2
    exit 2
  fi
fi

# Ensure build dir exists. Stay in project root — Conan v2 uses --output-folder
# with an absolute path, so cd-ing into build/ is not needed and breaks preset paths.
run mkdir -p "$BUILD_DIR"

if [ "$SKIP_CONAN" -eq 0 ]; then
  # Detect Conan major version (if available) and build the conan install command accordingly.
  # Conan 2+ uses a different flow (toolchains / install-folder) and does not generate
  # conanbuild.sh the same way as Conan 1.x. For Conan v2 we will pass an install-folder
  # and avoid attempting to source conanbuild.sh.
  CONAN_MAJOR=0
  if command -v conan >/dev/null 2>&1; then
    ver="$(conan --version 2>/dev/null || true)"
    # Example outputs: "Conan version 1.59.0" or "Conan version 2.24.0"
    if [[ "$ver" =~ ([0-9]+)\. ]]; then
      CONAN_MAJOR="${BASH_REMATCH[1]}"
    fi
  fi

  if [ "$CONAN_MAJOR" -ge 2 ]; then
    echo "Detected Conan v$CONAN_MAJOR; using Conan v2 install options and will not source conanbuild.sh."
    # Conan v2 + cmake_layout: the layout already controls the output folder structure,
    # so --output-folder must NOT be passed (it would create a nested build/build/ tree).
    CONAN_CMD=(conan install "$PROJECT_ROOT")
    if [ "$CONAN_BUILD_MISSING" = "--build=missing" ]; then
      CONAN_CMD+=(--build=missing)
    fi
  else
    # Conan v1 (or unknown): keep previous behavior
    CONAN_CMD=(conan install "$PROJECT_ROOT" "$CONAN_BUILD_MISSING")
  fi

  if [ -n "$CONAN_PROFILE" ]; then
    CONAN_CMD+=(--profile "$CONAN_PROFILE")
  fi

  if [ "${#CONAN_EXTRA_OPTS[@]}" -gt 0 ]; then
    CONAN_CMD+=("${CONAN_EXTRA_OPTS[@]}")
  fi

  # Run conan install
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "+ ${CONAN_CMD[*]}"
  else
    echo "+ ${CONAN_CMD[*]}"
    "${CONAN_CMD[@]}"
  fi

  # For Conan v2 we don't expect conanbuild.sh; for v1 try to source the legacy helper script.
  if [ "$CONAN_MAJOR" -ge 2 ]; then
    echo "Conan v2 detected: skipping sourcing of conanbuild.sh. If you rely on a toolchain file,"
    echo "look for 'conan_toolchain.cmake' or use the install-folder path: $BUILD_DIR"
  else
    # Try to source the generated conanbuild script. Prefer the per-config location (e.g. Release/generators)
    CONAN_SH_PATH="$BUILD_DIR/$CMAKE_BUILD_TYPE/generators/conanbuild.sh"
    if [ -f "$CONAN_SH_PATH" ]; then
      if [ "$DRY_RUN" -eq 1 ]; then
        echo "+ . $CONAN_SH_PATH"
      else
        . "$CONAN_SH_PATH"
      fi
    else
      # Fallback: try to find any conanbuild.sh under the build directory
      FOUND="$(ls -1 "$BUILD_DIR"/*/generators/conanbuild.sh 2>/dev/null | head -n1 || true)"
      if [ -n "$FOUND" ] && [ -f "$FOUND" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
          echo "+ . $FOUND"
        else
          . "$FOUND"
        fi
      else
        echo "Warning: conanbuild.sh not found in expected locations; continuing without sourcing."
      fi
    fi
  fi

fi

echo "Setup complete."
