#!/usr/bin/env bash
# Gate 5 verification loop: build the P3P host + I/O unit tests, run the
# tests, then boot the AOT host against the extracted game tree and report
# how far init progresses with file I/O live.
#
# Usage: profiles/p3p/scripts/verify_io.sh
# Env:   P3P_ELF  (default: ~/recompile/p3p_sys/EBOOT_DECRYPTED.ELF)
#        P3P_MAX_DISPATCHES (default: 50000000)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/out/p3p-build"
ELF="${P3P_ELF:-${HOME}/recompile/p3p_sys/EBOOT_DECRYPTED.ELF}"
GAME_ROOT="${REPO_ROOT}/profiles/p3p/game"
LOG="$(mktemp /tmp/p3p_io_boot.XXXXXX.log)"

cd "${REPO_ROOT}"

echo "== build =="
cmake --build "${BUILD_DIR}" --target p3p test_p3p_io test_p3p_threading -j2

echo "== unit tests =="
"${BUILD_DIR}/profiles/p3p/test_p3p_io"
"${BUILD_DIR}/profiles/p3p/test_p3p_threading"

echo "== boot (game root: ${GAME_ROOT}) =="
set +e
timeout 240 "${BUILD_DIR}/profiles/p3p/p3p" "${ELF}" \
    --game-root "${GAME_ROOT}" \
    --max-dispatches "${P3P_MAX_DISPATCHES:-50000000}" 2> "${LOG}"
set -e

echo "-- first file opens --"
grep "\[p3p-io\] open" "${LOG}" | head -20 || true
echo "-- stop reason --"
grep "\[p3p\] stopped:" "${LOG}" || tail -3 "${LOG}"
grep "\[p3p\] kernel:" "${LOG}" || true
echo "full boot log: ${LOG}"
