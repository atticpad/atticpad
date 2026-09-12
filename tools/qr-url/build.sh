#!/usr/bin/env bash
# Build tools/qr-url. See main.c for what it is for.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
CC="${CC:-cc}"

# qrcodegen.c is NOT on this line: qr.h #includes the vendored .c body itself
# (see its header comment -- deliberate, so the server stays one translation
# unit per host file). Linking it again is a multiple-definition error.
echo "== tools/qr-url/build.sh: compiling ${HERE}/qr-url =="
# -Wno-unused-function: qr.h and the sockcompat.h it pulls in are header-only
# libraries of `static` functions. The server uses all of them; a tool that
# wants exactly one still compiles the rest, and -Werror would fail on every
# function it did not call. Scoped to this tool rather than relaxed anywhere
# that compiles the server itself.
"${CC}" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O2 \
    -I"${ROOT}/core/include" -I"${ROOT}/server/host/common" \
    -I"${ROOT}/server/vendor/qrcodegen" \
    "${HERE}/main.c" \
    -o "${HERE}/qr-url"
echo "built ${HERE}/qr-url"
