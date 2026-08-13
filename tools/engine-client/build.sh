#!/usr/bin/env bash
# tools/engine-client/build.sh
#
# Builds tools/engine-client as a native Linux binary: core/src/ (libapad)
# + shim/net_bsd.c + shim/time_posix.c (the POSIX platform shim) + this
# tool's own main.c. docs/DESIGN.md D7: stays a separate process from server/,
# talks over a real UDP socket.
#
# Usage:
#   tools/engine-client/build.sh              # build + run self-loopback
#   tools/engine-client/build.sh build-only    # just build
#   tools/engine-client/build.sh run           # build + run self-loopback
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
"${CC}" -std=c99 -g -O0 \
    -fsanitize=address,undefined \
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

# No self-loopback mode: engine-client needs a live server (see
# scripts/build.sh's integration loop). build.sh only builds.
