#!/usr/bin/env bash
# tools/loopback-client/build.sh
#
# Builds tools/loopback-client as a native Linux binary: core/src/ (libapad)
# + shim/net_bsd.c + shim/time_posix.c (the POSIX platform shim) + this
# tool's own main.c. docs/DESIGN.md D7: stays a separate process from server/,
# talks over a real UDP socket.
#
# Usage:
#   tools/loopback-client/build.sh              # build + run self-loopback
#   tools/loopback-client/build.sh build-only    # just build
#   tools/loopback-client/build.sh run           # build + run self-loopback
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
CORE_SRC="${REPO_ROOT}/core/src"
CORE_INC="${REPO_ROOT}/core/include"
SHIM="${REPO_ROOT}/shim"
OUT="${HERE}/loopback-client"

CC="${CC:-cc}"

echo "== tools/loopback-client/build.sh: compiling ${OUT} =="
"${CC}" -std=c99 -g -O0 \
    -fsanitize=address,undefined \
    -Wall -Wextra \
    -I"${CORE_INC}" \
    "${HERE}/main.c" \
    "${CORE_SRC}"/*.c \
    "${SHIM}/net_bsd.c" \
    "${SHIM}/time_posix.c" \
    -o "${OUT}"
echo "built ${OUT}"

if [[ "${1:-}" == "build-only" ]]; then
    exit 0
fi

echo "== tools/loopback-client/build.sh: running self-loopback smoke test =="
"${OUT}"
