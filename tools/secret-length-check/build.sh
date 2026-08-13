#!/usr/bin/env bash
# tools/secret-length-check/build.sh
#
# Standalone cross-check for the docs/PROTOCOL.md S10.1 secret-length
# vectors (core/testdata/generate.py's Section I, apad_secret_length_vectors
# in core/testdata/vectors.h). Links core/src/*.c directly, same as
# tools/loopback-client/build.sh -- see this tool's main.c header comment
# for why that is not a violation of the vector-independence rule.
#
# Usage:
#   tools/secret-length-check/build.sh              # build + run
#   tools/secret-length-check/build.sh build-only    # just build

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
CORE_SRC="${REPO_ROOT}/core/src"
CORE_INC="${REPO_ROOT}/core/include"
TESTDATA="${REPO_ROOT}/core/testdata"
OUT="${HERE}/secret-length-check"

CC="${CC:-cc}"

echo "== tools/secret-length-check/build.sh: compiling ${OUT} =="
"${CC}" -std=c99 -g -O0 \
    -fsanitize=address,undefined \
    -Wall -Wextra \
    -I"${CORE_INC}" \
    -I"${TESTDATA}" \
    "${HERE}/main.c" \
    "${CORE_SRC}"/*.c \
    -o "${OUT}"
echo "built ${OUT}"

if [[ "${1:-}" == "build-only" ]]; then
    exit 0
fi

echo "== tools/secret-length-check/build.sh: running S10.1 boundary check =="
"${OUT}"
