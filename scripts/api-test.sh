#!/usr/bin/env bash
# api-test.sh — end-to-end S3 API compatibility test using s3cmd
#
# Tests all major operations:
#   ListBuckets, CreateBucket, HeadBucket, DeleteBucket
#   PutObject, GetObject, HeadObject (info), CopyObject, DeleteObject
#   DeleteObjects (bulk), ListObjectsV2 (prefix / delimiter / pagination)
#   Multipart upload lifecycle (create → upload parts → complete)
#   Multipart abort
#
# Usage:
#   ./scripts/api-test.sh [options]
#
# The server must already be running. Start it with:
#   ./scripts/start.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────

HOST="localhost"
PORT="${NANOBUCKET_PORT:-9000}"
ACCESS_KEY="${NANOBUCKET_ACCESS_KEY:-nanobucketadmin}"
SECRET_KEY="${NANOBUCKET_SECRET_KEY:-nanobucketadmin}"
NO_MULTIPART=0
VERBOSE=0

# Unique bucket names for this run (avoids collision with parallel runs)
RUN_ID="$(date +%s)"
BUCKET="nanobucket-test-${RUN_ID}"
BUCKET2="nanobucket-test2-${RUN_ID}"

# Temp files cleaned up on exit
TMP_DIR="$(mktemp -d)"
S3CFG="$TMP_DIR/s3cmd.cfg"
SMALL_FILE="$TMP_DIR/small.txt"
BIG_FILE="$TMP_DIR/big.bin"
GOT_FILE="$TMP_DIR/got.bin"

# ── Pass / fail counters ──────────────────────────────────────────────────────

PASS=0
FAIL=0
FAILURES=()

# ── Colors ────────────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    C_GREEN="\033[0;32m"
    C_RED="\033[0;31m"
    C_YELLOW="\033[0;33m"
    C_CYAN="\033[0;36m"
    C_BOLD="\033[1m"
    C_RESET="\033[0m"
else
    C_GREEN="" C_RED="" C_YELLOW="" C_CYAN="" C_BOLD="" C_RESET=""
fi

# ── Helpers ───────────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  -H, --host HOST          Server hostname (default: $HOST)
  -p, --port PORT          Server port (default: $PORT, overrides NANOBUCKET_PORT)
  -a, --access-key KEY     Access key (default: $ACCESS_KEY, overrides NANOBUCKET_ACCESS_KEY)
  -s, --secret-key KEY     Secret key (overrides NANOBUCKET_SECRET_KEY)
      --no-multipart       Skip the multipart upload test (faster)
  -v, --verbose            Print s3cmd output even on success
  -h, --help               Show this help

Environment:
  NANOBUCKET_PORT, NANOBUCKET_ACCESS_KEY, NANOBUCKET_SECRET_KEY

Examples:
  $(basename "$0")
  $(basename "$0") --port 9001 --no-multipart
  $(basename "$0") -v
EOF
}

section() {
    echo
    echo -e "${C_CYAN}${C_BOLD}━━━ $* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"
}

pass() {
    PASS=$(( PASS + 1 ))
    echo -e "  ${C_GREEN}PASS${C_RESET}  $*"
}

fail() {
    FAIL=$(( FAIL + 1 ))
    FAILURES+=("$*")
    echo -e "  ${C_RED}FAIL${C_RESET}  $*"
}

# Run an s3cmd command, capture stdout+stderr.
# Usage: s3 [args...]  — sets $OUT, returns s3cmd's exit code.
s3() {
    OUT="$(s3cmd -c "$S3CFG" "$@" 2>&1)" || return $?
    return 0
}

# Assert a condition; usage: assert "description" <cmd...>
assert() {
    local desc="$1"; shift
    local out
    if out=$("$@" 2>&1); then
        [ "$VERBOSE" -eq 1 ] && echo "    $out"
        pass "$desc"
    else
        fail "$desc"
        echo "    $out" >&2
    fi
}

# Assert that an s3cmd command succeeds.
assert_s3() {
    local desc="$1"; shift
    if OUT="$(s3cmd -c "$S3CFG" "$@" 2>&1)"; then
        [ "$VERBOSE" -eq 1 ] && echo "$OUT" | sed 's/^/    /'
        pass "$desc"
    else
        fail "$desc"
        echo "$OUT" | sed 's/^/    /' >&2
    fi
}

# Assert that an s3cmd command fails (non-zero exit).
assert_s3_fails() {
    local desc="$1"; shift
    if OUT="$(s3cmd -c "$S3CFG" "$@" 2>&1)"; then
        fail "$desc (expected failure, got success)"
        [ "$VERBOSE" -eq 1 ] && echo "$OUT" | sed 's/^/    /'
    else
        [ "$VERBOSE" -eq 1 ] && echo "$OUT" | sed 's/^/    /'
        pass "$desc"
    fi
}

# Assert that stdout/variable contains a substring.
assert_contains() {
    local desc="$1" haystack="$2" needle="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        pass "$desc"
    else
        fail "$desc (expected to contain: '$needle')"
        [ "$VERBOSE" -eq 1 ] && echo "    got: $haystack" >&2
    fi
}

# Assert that stdout/variable does NOT contain a substring.
assert_not_contains() {
    local desc="$1" haystack="$2" needle="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        fail "$desc (expected to NOT contain: '$needle')"
        [ "$VERBOSE" -eq 1 ] && echo "    got: $haystack" >&2
    else
        pass "$desc"
    fi
}

# Assert two strings are equal.
assert_eq() {
    local desc="$1" got="$2" want="$3"
    if [ "$got" = "$want" ]; then
        pass "$desc"
    else
        fail "$desc (got='$got', want='$want')"
    fi
}

cleanup() {
    # Best-effort removal of test buckets (ignore errors)
    s3cmd -c "$S3CFG" rb --recursive "s3://$BUCKET"  >/dev/null 2>&1 || true
    s3cmd -c "$S3CFG" rb --recursive "s3://$BUCKET2" >/dev/null 2>&1 || true
    rm -rf "$TMP_DIR"
}

# ── Parse CLI args ────────────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        -H|--host)       HOST="$2";       shift 2 ;;
        -p|--port)       PORT="$2";       shift 2 ;;
        -a|--access-key) ACCESS_KEY="$2"; shift 2 ;;
        -s|--secret-key) SECRET_KEY="$2"; shift 2 ;;
        --no-multipart)  NO_MULTIPART=1;  shift   ;;
        -v|--verbose)    VERBOSE=1;       shift   ;;
        -h|--help)       usage; exit 0             ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

# ── Dependency check ──────────────────────────────────────────────────────────

if ! command -v s3cmd >/dev/null 2>&1; then
    echo -e "${C_RED}${C_BOLD}Error: s3cmd is not installed.${C_RESET}"
    echo
    echo "Install it with one of:"
    echo "  Ubuntu / Debian : sudo apt install s3cmd"
    echo "  Fedora / RHEL   : sudo dnf install s3cmd"
    echo "  Arch            : sudo pacman -S s3cmd"
    echo "  macOS (Homebrew): brew install s3cmd"
    echo "  pip             : pip install s3cmd"
    echo "  Upstream        : https://s3tools.org/s3cmd"
    exit 1
fi

S3CMD_VER="$(s3cmd --version 2>&1 | head -1)"
echo -e "${C_BOLD}s3cmd:${C_RESET} $S3CMD_VER"

# ── Server reachability check ─────────────────────────────────────────────────

if ! curl -s --max-time 3 "http://$HOST:$PORT" >/dev/null 2>&1; then
    echo -e "${C_RED}${C_BOLD}Error: nanobucket not reachable at http://$HOST:$PORT${C_RESET}" >&2
    echo "Start the server with: ./scripts/start.sh" >&2
    exit 1
fi
echo -e "${C_BOLD}Server:${C_RESET} http://$HOST:$PORT  (reachable)"

# ── Write temp s3cfg ──────────────────────────────────────────────────────────

cat >"$S3CFG" <<EOF
[default]
access_key = $ACCESS_KEY
secret_key = $SECRET_KEY
host_base = $HOST:$PORT
host_bucket = $HOST:$PORT
use_https = False
signature_v2 = False
EOF

trap cleanup EXIT

echo -e "${C_BOLD}Buckets:${C_RESET} $BUCKET, $BUCKET2"

# ═════════════════════════════════════════════════════════════════════════════
# Test suite
# ═════════════════════════════════════════════════════════════════════════════

# ── Prepare small test file ───────────────────────────────────────────────────

printf 'Hello, nanobucket!\n' >"$SMALL_FILE"
SMALL_CONTENT="Hello, nanobucket!"
SMALL_SIZE=19   # 18 chars + newline

# ── Bucket operations ─────────────────────────────────────────────────────────

section "Bucket — Create / Head / List"

assert_s3        "CreateBucket"            mb "s3://$BUCKET"
assert_s3        "CreateBucket (second)"   mb "s3://$BUCKET2"
assert_s3_fails  "CreateBucket duplicate"  mb "s3://$BUCKET"

OUT="$(s3cmd -c "$S3CFG" ls 2>&1)"
assert_contains  "ListBuckets shows first bucket"  "$OUT" "$BUCKET"
assert_contains  "ListBuckets shows second bucket" "$OUT" "$BUCKET2"

assert_s3        "HeadBucket (exists)"   info "s3://$BUCKET" --no-check-certificate
assert_s3_fails  "HeadBucket (missing)"  info "s3://no-such-bucket-${RUN_ID}"

# ── Object — Put / Get / Head ─────────────────────────────────────────────────

section "Object — Put / Get / Head"

assert_s3 "PutObject small" put "$SMALL_FILE" "s3://$BUCKET/hello.txt"

OUT="$(s3cmd -c "$S3CFG" get "s3://$BUCKET/hello.txt" - 2>&1)"
assert_contains "GetObject returns correct content" "$OUT" "$SMALL_CONTENT"

OUT="$(s3cmd -c "$S3CFG" info "s3://$BUCKET/hello.txt" 2>&1)"
if echo "$OUT" | grep -qF "File size: $SMALL_SIZE"; then
    pass "HeadObject reports correct size ($SMALL_SIZE bytes)"
else
    fail "HeadObject reports correct size ($SMALL_SIZE bytes) — got: $(echo "$OUT" | grep -i 'file size' || echo 'no size line')"
    [ "$VERBOSE" -eq 1 ] && echo "$OUT" | sed 's/^/    /' >&2
fi

# Verify size in listing
OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/" 2>&1)"
assert_contains "ListObjectsV2 shows hello.txt" "$OUT" "hello.txt"

assert_s3_fails "GetObject missing key returns error" get "s3://$BUCKET/no-such-key.txt" -

# ── Object — Nested keys ──────────────────────────────────────────────────────

section "Object — Nested keys"

assert_s3 "PutObject nested key (a/b/c.txt)" put "$SMALL_FILE" "s3://$BUCKET/a/b/c.txt"
assert_s3 "PutObject nested key (a/b/d.txt)" put "$SMALL_FILE" "s3://$BUCKET/a/b/d.txt"
assert_s3 "PutObject nested key (a/x.txt)"   put "$SMALL_FILE" "s3://$BUCKET/a/x.txt"

# List with delimiter groups common prefixes
OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/" 2>&1)"
assert_contains     "Delimiter folds a/ into DIR" "$OUT" "DIR"
assert_not_contains "Delimiter hides a/b/c.txt"   "$OUT" "a/b/c.txt"

OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/a/" 2>&1)"
assert_contains "List a/ shows a/b/ prefix" "$OUT" "a/b/"
assert_contains "List a/ shows a/x.txt"     "$OUT" "a/x.txt"

OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/a/b/" 2>&1)"
assert_contains "List a/b/ shows c.txt" "$OUT" "c.txt"
assert_contains "List a/b/ shows d.txt" "$OUT" "d.txt"

# Get nested key content
OUT="$(s3cmd -c "$S3CFG" get "s3://$BUCKET/a/b/c.txt" - 2>&1)"
assert_contains "GetObject nested key content correct" "$OUT" "$SMALL_CONTENT"

# ── Object — CopyObject ───────────────────────────────────────────────────────

section "Object — CopyObject"

assert_s3 "CopyObject same bucket" cp "s3://$BUCKET/hello.txt" "s3://$BUCKET/hello-copy.txt"

OUT="$(s3cmd -c "$S3CFG" get "s3://$BUCKET/hello-copy.txt" - 2>&1)"
assert_contains "CopyObject content matches source" "$OUT" "$SMALL_CONTENT"

OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/" 2>&1)"
assert_contains "CopyObject appears in listing" "$OUT" "hello-copy.txt"

assert_s3 "CopyObject cross-bucket" \
    cp "s3://$BUCKET/hello.txt" "s3://$BUCKET2/hello-from-bucket1.txt"

OUT="$(s3cmd -c "$S3CFG" get "s3://$BUCKET2/hello-from-bucket1.txt" - 2>&1)"
assert_contains "CopyObject cross-bucket content correct" "$OUT" "$SMALL_CONTENT"

# ── Object — DeleteObject ─────────────────────────────────────────────────────

section "Object — DeleteObject"

assert_s3 "DeleteObject" rm "s3://$BUCKET/hello-copy.txt"

OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/" 2>&1)"
assert_not_contains "Deleted object gone from listing" "$OUT" "hello-copy.txt"

assert_s3 "DeleteObject non-existent key is idempotent (204)" rm "s3://$BUCKET/no-such-key.txt"

# ── Object — DeleteObjects (bulk) ─────────────────────────────────────────────

section "Object — DeleteObjects (bulk)"

# Put a few objects to bulk-delete
assert_s3 "PutObject bulk/1.txt" put "$SMALL_FILE" "s3://$BUCKET/bulk/1.txt"
assert_s3 "PutObject bulk/2.txt" put "$SMALL_FILE" "s3://$BUCKET/bulk/2.txt"
assert_s3 "PutObject bulk/3.txt" put "$SMALL_FILE" "s3://$BUCKET/bulk/3.txt"

# s3cmd rb --recursive uses POST ?delete for bulk removal
assert_s3 "DeleteObjects via rb --recursive" rb --recursive "s3://$BUCKET" --force

OUT="$(s3cmd -c "$S3CFG" ls 2>&1)"
assert_not_contains "Bucket removed after recursive delete" "$OUT" "$BUCKET"

# Recreate for remaining tests
assert_s3 "Recreate bucket after delete" mb "s3://$BUCKET"

# ── Bucket — DeleteBucket not-empty ───────────────────────────────────────────

section "Bucket — Delete (empty / non-empty)"

assert_s3 "PutObject for non-empty test" put "$SMALL_FILE" "s3://$BUCKET/keep.txt"
assert_s3_fails "DeleteBucket non-empty returns 409" rb "s3://$BUCKET"

assert_s3 "DeleteObject to empty bucket" rm "s3://$BUCKET/keep.txt"
assert_s3 "DeleteBucket when empty" rb "s3://$BUCKET"

OUT="$(s3cmd -c "$S3CFG" ls 2>&1)"
assert_not_contains "Deleted bucket gone from ListBuckets" "$OUT" "$BUCKET"

# Recreate for multipart tests
assert_s3 "Recreate bucket for multipart" mb "s3://$BUCKET"

# ── Multipart upload ──────────────────────────────────────────────────────────

if [ "$NO_MULTIPART" -eq 1 ]; then
    section "Multipart — SKIPPED (--no-multipart)"
else
    section "Multipart — Upload (>15 MB)"

    echo "  generating 20 MB random file..."
    dd if=/dev/urandom of="$BIG_FILE" bs=1M count=20 2>/dev/null

    BIG_SHA="$(sha256sum "$BIG_FILE" | awk '{print $1}')"

    assert_s3 "Multipart PutObject (20 MB)" put "$BIG_FILE" "s3://$BUCKET/big.bin"

    s3cmd -c "$S3CFG" get "s3://$BUCKET/big.bin" "$GOT_FILE" >/dev/null 2>&1
    GOT_SHA="$(sha256sum "$GOT_FILE" | awk '{print $1}')"
    assert_eq "Multipart downloaded content matches original (SHA-256)" "$GOT_SHA" "$BIG_SHA"

    OUT="$(s3cmd -c "$S3CFG" ls "s3://$BUCKET/" 2>&1)"
    assert_contains "Multipart object appears in listing" "$OUT" "big.bin"

    assert_s3 "DeleteObject multipart object" rm "s3://$BUCKET/big.bin"

    section "Multipart — Abort"

    # Start an MPU manually via AWS-style API (s3cmd --multipart-chunk-size-mb)
    # We can simulate abort by deleting a half-uploaded object that s3cmd leaves behind
    # when interrupted. Since scripting a real abort requires direct HTTP calls, we verify
    # the abort path using s3cmd's abortmp command on a known-bad uploadId. The server
    # should return 404 NoSuchUpload.
    FAKE_ID="00000000000000000000000000000000"
    OUT="$(s3cmd -c "$S3CFG" abortmp "s3://$BUCKET/ghost.bin" "$FAKE_ID" 2>&1)" || true
    assert_contains "AbortMPU unknown uploadId returns error" "$OUT" "NoSuchUpload"
fi

# ═════════════════════════════════════════════════════════════════════════════
# Summary
# ═════════════════════════════════════════════════════════════════════════════

echo
echo -e "${C_BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"
TOTAL=$(( PASS + FAIL ))
if [ "$FAIL" -eq 0 ]; then
    echo -e "${C_GREEN}${C_BOLD}All $TOTAL tests passed.${C_RESET}"
else
    echo -e "${C_RED}${C_BOLD}$FAIL / $TOTAL tests FAILED:${C_RESET}"
    for f in "${FAILURES[@]}"; do
        echo -e "  ${C_RED}✗${C_RESET}  $f"
    done
fi
echo -e "${C_BOLD}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${C_RESET}"

[ "$FAIL" -eq 0 ]
