#!/usr/bin/env bash
# Fail if server/host/common/profiles_builtin.h no longer matches the .jsonc
# files it is generated from.
#
# The header is committed rather than generated during the build, because
# scripts/build.sh's per-file source lists and the mingw cross-build would both
# need to learn about a generated file, and a header that only exists after a
# successful build is a worse trade than one checked for drift.
#
# The failure this prevents is quiet: edit a shipped profile, forget to
# regenerate, and every build still succeeds while released binaries carry the
# OLD mapping. Nothing else would notice -- the profiles on disk in a checkout
# shadow the embedded copies, so a developer would never see it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HEADER="${ROOT}/server/host/common/profiles_builtin.h"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cp "$HEADER" "$TMP/committed.h"
python3 "${ROOT}/scripts/support/gen_profiles_builtin.py" >/dev/null
if ! diff -q "$TMP/committed.h" "$HEADER" >/dev/null; then
    cp "$TMP/committed.h" "$HEADER"   # leave the tree as we found it
    echo "== check_profiles_builtin: FAIL ==" >&2
    echo "server/host/common/profiles_builtin.h is out of date with server/profiles/*.jsonc." >&2
    echo "Regenerate and commit it:" >&2
    echo "    python3 scripts/support/gen_profiles_builtin.py" >&2
    exit 1
fi
echo "== check_profiles_builtin: OK — embedded profiles match server/profiles/ =="
