#!/usr/bin/env bash
# tools/engine-client/build.sh
#
# Builds tools/engine-client as a native Linux binary: core/src/ (libapad)
# + shim/net_bsd.c + shim/time_posix.c (the POSIX platform shim) + this
# tool's own main.c. docs/DESIGN.md D7: stays a separate process from server/,
# talks over a real UDP socket.
#
# Usage:
#   tools/engine-client/build.sh              # build + run the serverless checks
#   tools/engine-client/build.sh build-only    # just build
#   tools/engine-client/build.sh run           # build + run the serverless checks
#
# "run" executes the two modes that need NO SERVER -- --inputcaps-reorder and
# --release-on-clear, which bring their own fake peer up on a scratch port.
# The --kbm mode is not among them: it needs a live server and a human (or a
# script) watching evtest, so it stays a manual/integration step exactly as
# the default --target run does.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
CORE_SRC="${REPO_ROOT}/core/src"
CORE_INC="${REPO_ROOT}/core/include"
CLIENT_COMMON="${REPO_ROOT}/clients/common"
SHIM="${REPO_ROOT}/shim"
OUT="${HERE}/engine-client"

CC="${CC:-cc}"

echo "== tools/engine-client/build.sh: compiling ${OUT} =="
# -pthread: the --inputcaps-reorder mode runs a fake peer in a second thread
# so it can deliver two INPUTCAPS out of order without needing a server binary
# or a second process. Linux-only tool; the engine itself is still
# single-threaded and says so in apad_client.h.
"${CC}" -std=c99 -g -O0 \
    -fsanitize=address,undefined \
    -pthread \
    -Wall -Wextra \
    -I"${CORE_INC}" \
    -I"${CLIENT_COMMON}" \
    "${HERE}/main.c" \
    "${CLIENT_COMMON}/apad_client.c" \
    "${CORE_SRC}"/*.c \
    "${SHIM}/net_bsd.c" \
    "${SHIM}/time_posix.c" \
    -o "${OUT}"
echo "built ${OUT}"

# Two of the three modes need no server at all, so they can run right here in
# the build loop -- and they are the two that pin rules nothing else in the
# tree can reach: §6.20's fourth per-type window (INPUTCAPS reordering) and
# §6.19's release-before-stop. Scratch ports, never 21100.
#
# scripts/build.sh still invokes this with "build-only", so CI's behaviour is
# unchanged until whoever owns that file decides to flip it.
if [ "${1:-run}" != "build-only" ]; then
    echo "== tools/engine-client/build.sh: --inputcaps-reorder (§6.20 fourth window) =="
    ASAN_OPTIONS=detect_leaks=0 "${OUT}" --inputcaps-reorder 21187
    echo "== tools/engine-client/build.sh: --release-on-clear (§6.19 release, §6.20 repeat floor) =="
    ASAN_OPTIONS=detect_leaks=0 "${OUT}" --release-on-clear 21188
fi
