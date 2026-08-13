#!/usr/bin/env bash
# tools/apad-ui-test/build.sh
#
# Standalone check for clients/common/apad_ui.c against the precedence order
# documented in clients/common/apad_ui.h's apad_ui_status_message() comment.
# Links only clients/common/apad_ui.c + apad_ui_strings.c + this tool's own
# main.c -- apad_ui.c never calls into libapad, so there is nothing under
# core/src or shim/ to link, unlike tools/loopback-client or
# tools/secret-length-check.
#
# Usage:
#   tools/apad-ui-test/build.sh              # build + run
#   tools/apad-ui-test/build.sh build-only    # just build

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
COMMON="${REPO_ROOT}/clients/common"
CORE_INC="${REPO_ROOT}/core/include"
OUT="${HERE}/apad-ui-test"

CC="${CC:-cc}"

echo "== tools/apad-ui-test/build.sh: compiling ${OUT} =="
"${CC}" -std=c99 -g -O0 \
    -fsanitize=address,undefined \
    -Wall -Wextra \
    -I"${COMMON}" \
    -I"${CORE_INC}" \
    "${HERE}/main.c" \
    "${COMMON}/apad_ui.c" \
    "${COMMON}/apad_ui_strings.c" \
    -o "${OUT}"
echo "built ${OUT}"

if [[ "${1:-}" == "build-only" ]]; then
    exit 0
fi

echo "== tools/apad-ui-test/build.sh: running precedence check =="
"${OUT}"
