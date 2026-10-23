#!/usr/bin/env bash
# Gate 4B verification loop for the P3P profile:
#   1. incremental build of the executable host + threading unit tests
#   2. unit tests (Gate 4A vectors, allocator invariants, failure matrix)
#   3. AOT boot smoke run (bounded dispatches)
#   4. interpreter-mode oracle trace + Gate 4B window comparison
#      (0x0880421C..0x08804260 vs the PPSSPP reference, window_diff.py)
#
# Usage: profiles/p3p/scripts/verify_boot.sh [build-dir]
# Env:   P3P_ELF, P3P_REF_TRACE, P3P_OUT_TRACE, P3P_MAX_DISPATCHES
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/out/p3p-build}"
ELF="${P3P_ELF:-$HOME/recompile/p3p_sys/EBOOT_DECRYPTED.ELF}"
REF_TRACE="${P3P_REF_TRACE:-$HOME/recompile/traces/ppsspp_p3p_trace.jsonl}"
OUT_TRACE="${P3P_OUT_TRACE:-/tmp/p3p_recomp_trace.jsonl}"
MAX_DISPATCHES="${P3P_MAX_DISPATCHES:-1000}"
BIN_DIR="$BUILD_DIR/profiles/p3p"

echo "== build =="
cmake --build "$BUILD_DIR" --target p3p test_p3p_threading

echo "== unit tests =="
"$BIN_DIR/test_p3p_threading"

echo "== AOT boot smoke (max $MAX_DISPATCHES dispatches) =="
"$BIN_DIR/p3p" "$ELF" --max-dispatches "$MAX_DISPATCHES" 2>&1 | tail -4 || true

echo "== interpreter oracle trace =="
# The boot run is expected to end at the post-Gate-4B init wall (a guest
# null dereference caused by a return-0 stub deep in game init); the Gate 4B
# window sits at the start of the trace, so a non-zero exit here is normal.
YAKUMO_ORACLE_TRACE="$OUT_TRACE" YAKUMO_ORACLE_MODE=instruction \
    "$BIN_DIR/p3p" "$ELF" --interpret --max-dispatches "$MAX_DISPATCHES" || true

echo "== Gate 4B window diff (0x0880421C..0x08804260) =="
python3 "$(dirname "$0")/window_diff.py" "$REF_TRACE" "$OUT_TRACE"

echo "== verify_boot: OK =="
