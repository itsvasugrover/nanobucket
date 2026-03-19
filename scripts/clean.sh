#!/usr/bin/env bash
set -euo pipefail

# Robust, modular clean script for the project.
# Works when invoked from project root or the scripts directory.
#
# Defaults to removing the build directory. Can remove in-source CMake artifacts,
# Conan-generated files, and optionally purge the user's Conan cache (dangerous).
#
# Safety:
# - Refuses to delete paths outside the project root unless --force-foreign is given.
# - Supports dry-run mode to preview destructive commands.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults
BUILD_DIR="$PROJECT_ROOT/build"
REMOVE_BUILD=1
REMOVE_SOURCE_ARTIFACTS=0
REMOVE_CONAN_BUILD_FILES=0
REMOVE_CONAN_CACHE=0
FORCE_FOREIGN=0
DRY_RUN=0
ASSUME_YES=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -b, --build-dir DIR        Build directory to remove (default: $BUILD_DIR)
      --no-build             Do not remove build directory
      --source               Remove in-source CMake artifacts (CMakeFiles, CMakeCache.txt, cmake_install.cmake)
      --conan-build-files    Remove generated conan build helper files (conanbuildinfo*, conanbuild.sh) in build dir or source tree
      --conan-cache          Purge the user's Conan cache (~/.conan) -- this is destructive
      --force-foreign        Allow removing a build dir located outside the project root
  -y, --yes                  Assume yes for destructive operations (no prompts)
  -n, --dry-run              Print commands without executing them
  -h, --help                 Show this help and exit

Examples:
  # Default: remove build directory under project root
  $(basename "$0")

  # Preview what would be removed
  $(basename "$0") --dry-run

  # Remove build dir and in-source artifacts
  $(basename "$0") --source

  # Remove build dir located elsewhere (requires --force-foreign)
  $(basename "$0") -b /tmp/somebuild --force-foreign

  # Purge conan cache (dangerous)
  $(basename "$0") --conan-cache -y
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

# Helper: safe absolute path resolution (prefers readlink -f, falls back to python)
abs_path() {
  local p="$1"
  if command -v readlink >/dev/null 2>&1; then
    readlink -f -- "$p"
  else
    # Use python3 or python to resolve; fallback to returning input if neither available
    if command -v python3 >/dev/null 2>&1; then
      python3 -c "import os,sys; print(os.path.abspath(sys.argv[1]))" -- "$p"
    elif command -v python >/dev/null 2>&1; then
      python -c "import os,sys; print(os.path.abspath(sys.argv[1]))" -- "$p"
    else
      # Best-effort: handle relative paths only
      if [ -d "$p" ]; then
        (cd "$p" && pwd)
      else
        case "$p" in
          /*) echo "$p" ;;
          *) echo "$(pwd)/$p" ;;
        esac
      fi
    fi
  fi
}

# Parse args
while [ $# -gt 0 ]; do
  case "$1" in
    -b|--build-dir)
      # Ensure an argument is provided and it is not another option
      if [ -z "${2:-}" ] || printf '%s\n' "${2:-}" | grep -qE '^-' ; then
        echo "Error: --build-dir requires a non-empty argument." >&2
        usage
        exit 1
      fi
      BUILD_DIR="$2"
      shift 2
      ;;
    --no-build)
      REMOVE_BUILD=0
      shift
      ;;
    --source)
      REMOVE_SOURCE_ARTIFACTS=1
      shift
      ;;
    --conan-build-files)
      REMOVE_CONAN_BUILD_FILES=1
      shift
      ;;
    --conan-cache)
      REMOVE_CONAN_CACHE=1
      shift
      ;;
    --force-foreign)
      FORCE_FOREIGN=1
      shift
      ;;
    -y|--yes)
      ASSUME_YES=1
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
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

echo "Project root: $PROJECT_ROOT"
echo "Build dir: $BUILD_DIR"
if [ "$REMOVE_BUILD" -eq 1 ]; then
  echo "Will remove build dir: ENABLED"
else
  echo "Will remove build dir: DISABLED"
fi
if [ "$REMOVE_SOURCE_ARTIFACTS" -eq 1 ]; then
  echo "Will remove in-source CMake artifacts: ENABLED"
fi
if [ "$REMOVE_CONAN_BUILD_FILES" -eq 1 ]; then
  echo "Will remove conan-generated build helper files: ENABLED"
fi
if [ "$REMOVE_CONAN_CACHE" -eq 1 ]; then
  echo "Will purge conan cache (~/.conan): ENABLED"
fi
if [ "$FORCE_FOREIGN" -eq 1 ]; then
  echo "Force deleting outside project root: ENABLED"
fi
if [ "$DRY_RUN" -eq 1 ]; then
  echo "Dry-run: ENABLED"
fi
if [ "$ASSUME_YES" -eq 1 ]; then
  echo "Assume-yes for prompts: ENABLED"
fi

# Resolve absolute paths
ABS_BUILD_DIR="$(abs_path "$BUILD_DIR")"
ABS_PROJECT_ROOT="$(abs_path "$PROJECT_ROOT")"

# Safety: ensure build dir is within project root unless forced
if [ "$REMOVE_BUILD" -eq 1 ]; then
  case "$ABS_BUILD_DIR" in
    "$ABS_PROJECT_ROOT"/*|"$ABS_PROJECT_ROOT")
      # ok
      ;;
    *)
      if [ "$FORCE_FOREIGN" -ne 1 ]; then
        echo "Refusing to remove build dir outside project root: $ABS_BUILD_DIR" >&2
        echo "If you really want to remove it, pass --force-foreign" >&2
        exit 2
      else
        echo "Warning: removing build dir outside project root (user requested)" >&2
      fi
      ;;
  esac
fi

# Collect commands to run (so we can show a plan before executing)
COMMANDS=()

if [ "$REMOVE_BUILD" -eq 1 ]; then
  # Remove whole build directory
  COMMANDS+=("rm -rf \"$ABS_BUILD_DIR\"")
fi

# In-source artifacts (dangerous, only remove common CMake files in project root)
if [ "$REMOVE_SOURCE_ARTIFACTS" -eq 1 ]; then
  # Only target the project root (not anywhere else)
  COMMANDS+=("rm -rf \"$ABS_PROJECT_ROOT/CMakeFiles\"")
  COMMANDS+=("rm -f \"$ABS_PROJECT_ROOT/CMakeCache.txt\"")
  COMMANDS+=("rm -f \"$ABS_PROJECT_ROOT/cmake_install.cmake\"")
  COMMANDS+=("rm -f \"$ABS_PROJECT_ROOT/compile_commands.json\"")
  COMMANDS+=("rm -f \"$ABS_PROJECT_ROOT/conanbuildinfo.txt\"")
  COMMANDS+=("rm -f \"$ABS_PROJECT_ROOT/conanbuild.sh\"")
fi

if [ "$REMOVE_CONAN_BUILD_FILES" -eq 1 ]; then
  # Remove conan helper files both in build dir and project root if present
  COMMANDS+=("find \"$ABS_PROJECT_ROOT\" -maxdepth 2 -type f -name 'conanbuild*.sh' -o -name 'conanbuildinfo*' -print0 | xargs -0 -r rm -f")
  COMMANDS+=("find \"$ABS_BUILD_DIR\" -maxdepth 3 -type f -name 'conanbuild*.sh' -o -name 'conanbuildinfo*' -print0 | xargs -0 -r rm -f")
fi

if [ "$REMOVE_CONAN_CACHE" -eq 1 ]; then
  # Purge conan cache directory in user's home (~/.conan2)
  CONAN_CACHE_DIR="${CONAN_CACHE_DIR:-$HOME/.conan2}"
  COMMANDS+=("rm -rf \"$CONAN_CACHE_DIR\"")
fi

if [ "${#COMMANDS[@]}" -eq 0 ]; then
  echo "Nothing to do. Use --help for options." >&2
  exit 0
fi

# Show plan
echo
echo "Planned destructive operations:"
for c in "${COMMANDS[@]}"; do
  echo "  $c"
done
echo

# Confirm if necessary
if [ "$ASSUME_YES" -ne 1 ]; then
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "Dry-run enabled; no prompt required."
  else
    printf "Proceed? [y/N]: "
    read -r reply || true
    case "$reply" in
      [yY]|[yY][eE][sS])
        ;;
      *)
        echo "Aborted by user."
        exit 0
        ;;
    esac
  fi
fi

# Execute commands
for c in "${COMMANDS[@]}"; do
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "+ $c"
  else
    echo "+ $c"
    # Use eval to allow compound commands / find + xargs expressions
    eval "$c"
  fi
done

echo "Clean complete."
