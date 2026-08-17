#!/usr/bin/env bash
# tools/server-harness/build.sh
#
# Builds tools/server-harness: libapadserver (server/src/*.c, the sans-IO
# library, docs/DESIGN.md S6.4) linked directly against this tool's own main.c --
# no socket shim, no real clock, no host process. server.c depends on
# core/src/codec.c, hmac_sha256.c, seq.c and session.c (the same set
# scripts/build.sh's build_server() links) but NOT on shim/net_bsd.c or
# shim/time_posix.c: nothing under server/src/ calls apad_udp_*() or
# apad_ticks_ms() (grep confirms it), which is exactly what makes it
# possible to drive this library with no sockets at all.
#
# Usage:
#   tools/server-harness/build.sh              # build + run every scenario
#   tools/server-harness/build.sh build-only    # just build
#   tools/server-harness/build.sh run           # build + run every scenario
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
CORE_SRC="${REPO_ROOT}/core/src"
CORE_INC="${REPO_ROOT}/core/include"
SERVER_SRC="${REPO_ROOT}/server/src"
SERVER_INC="${REPO_ROOT}/server/include"
SERVER_BACKENDS="${REPO_ROOT}/server/backends"
OUT="${HERE}/server-harness"

CC="${CC:-cc}"

echo "== tools/server-harness/build.sh: compiling ${OUT} =="
"${CC}" -std=c11 -g -O0 \
    -fsanitize=address,undefined \
    -Wall -Wextra \
    -I"${CORE_INC}" -I"${SERVER_INC}" -I"${SERVER_BACKENDS}" -I"${SERVER_SRC}" \
    "${HERE}/main.c" \
    "${CORE_SRC}/codec.c" "${CORE_SRC}/hmac_sha256.c" \
    "${CORE_SRC}/seq.c" "${CORE_SRC}/session.c" \
    "${SERVER_SRC}/server.c" "${SERVER_SRC}/mapping.c" \
    "${SERVER_SRC}/jsonc.c" "${SERVER_SRC}/profiles.c" \
    "${SERVER_SRC}/pairing.c" \
    -lm \
    -o "${OUT}"
echo "built ${OUT}"

if [[ "${1:-}" == "build-only" ]]; then
    exit 0
fi

echo "== tools/server-harness/build.sh: running every scenario =="
"${OUT}"
