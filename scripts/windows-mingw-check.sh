#!/usr/bin/env bash
# scripts/windows-mingw-check.sh — verify (never install) the mingw-w64
# cross-compiler scripts/build.sh windows and .github/workflows/ci.yml's
# server-windows job both need.
#
# Deliberately the mirror of scripts/android-sdk-bootstrap.sh's *role*, not
# its behaviour: that script installs a JDK/SDK/NDK it fetches itself,
# because none of that needs root. mingw-w64 is a normal apt package
# (gcc-mingw-w64-x86-64 / g++-mingw-w64-x86-64), this dev machine has NO
# passwordless sudo, and CI's server-windows job already runs its own
# `apt-get install` as root. So there is nothing for a local script to
# usefully install here -- this one only CHECKS what's on PATH and reports
# exactly what to run if it is missing, per scripts/windows.env's pinned
# package list.
#
# Usage: scripts/windows-mingw-check.sh          # report; exit 1 if absent
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=windows.env
source "${HERE}/windows.env"

CC="${APAD_MINGW_TRIPLE}-gcc"
CXX="${APAD_MINGW_TRIPLE}-g++"

log()  { printf '== windows-mingw-check: %s ==\n' "$*"; }
warn() { printf 'windows-mingw-check: WARNING: %s\n' "$*" >&2; }

if ! command -v "${CC}" >/dev/null 2>&1 || ! command -v "${CXX}" >/dev/null 2>&1; then
  cat >&2 <<EOF
windows-mingw-check: ${CC} and/or ${CXX} not found on PATH.

This script will not run apt-get for you (it assumes no passwordless sudo). Install with:

  sudo apt-get install -y ${APAD_MINGW_PACKAGES}

(.github/workflows/ci.yml's server-windows job runs that exact package list
as root, from scripts/windows.env, so local and CI stay in step.)
EOF
  exit 1
fi

GOT="$("${CC}" --version | head -1)"
log "found: $(command -v "${CC}"), $(command -v "${CXX}")"
log "version: ${GOT}"

if [[ "${GOT}" != *"${APAD_MINGW_GCC_VERSION}"* ]]; then
  warn "installed mingw-w64 reports '${GOT}'; the version scripts/build.sh windows was last verified against (scripts/windows.env: APAD_MINGW_GCC_VERSION) is '${APAD_MINGW_GCC_VERSION}'. This is a WARNING, not a failure — see windows.env's header for why an apt version isn't hard-pinned the way a Docker digest is. If server/backends/vigem.c or the vendored ViGEmClient.cpp (server/backends/vendor/) start failing to build, this version difference is the first thing to suspect."
fi

log "OK"
