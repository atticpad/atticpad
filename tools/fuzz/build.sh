#!/usr/bin/env bash
# tools/fuzz/build.sh
#
# Builds the libFuzzer harness against core/src/ (the AtticPad codec) and,
# by default, runs it for a while against a corpus seeded from
# core/testdata/generate.py's conformance vectors (docs/DESIGN.md S9.2).
#
# This is a helper local to tools/fuzz/, not scripts/build.sh (that file is
# owned by ci-engineer and is out of scope here -- see the task that
# produced this directory).
#
# Usage:
#   tools/fuzz/build.sh                 # build + seed corpus + fuzz for 60s
#   tools/fuzz/build.sh build-only       # just build the fuzzer binary
#   tools/fuzz/build.sh <seconds>        # build + seed corpus + fuzz for N s
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
CORE_SRC="${REPO_ROOT}/core/src"
CORE_INC="${REPO_ROOT}/core/include"
OUT="${HERE}/fuzz_decoder"
CORPUS="${HERE}/corpus"
SEED_CORPUS="${HERE}/seed_corpus"

CC="${CC:-clang}"

echo "== tools/fuzz/build.sh: compiling ${OUT} =="
"${CC}" -std=c99 -g -O1 \
    -fsanitize=fuzzer,address,undefined \
    -Wall -Wextra \
    -I"${CORE_INC}" \
    "${HERE}/fuzz_decoder.c" \
    "${CORE_SRC}"/*.c \
    -o "${OUT}"
echo "built ${OUT}"

if [[ "${1:-}" == "build-only" ]]; then
    exit 0
fi

DURATION="${1:-60}"

echo "== tools/fuzz/build.sh: seeding corpus from conformance vectors =="
python3 "${HERE}/make_seed_corpus.py"

mkdir -p "${CORPUS}"

echo "== tools/fuzz/build.sh: fuzzing for ${DURATION}s =="
"${OUT}" -max_total_time="${DURATION}" "${CORPUS}" "${SEED_CORPUS}"
