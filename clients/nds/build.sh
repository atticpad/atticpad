#!/usr/bin/env bash
#
# clients/nds/build.sh -- build the Nintendo DS / DSi client.
#
# Mirrors clients/psp/build.sh: prefer a native toolchain for iteration speed,
# otherwise use the SAME pinned container digest CI uses, so a local pass and a
# CI pass mean the same thing.
#
# Two things differ from the PSP script, both forced by the image:
#
#   1. BLOCKSDS lives at /opt/wonderful/thirdparty/blocksds/core, NOT at the
#      SDK makefile's own default of /opt/blocksds/core. The image exports it,
#      so nothing needs passing in -- but a native install that does not is why
#      the Makefile spells the fallback out.
#   2. arm-none-eabi-gcc is NOT on PATH inside the image. The SDK makefile
#      finds it through $WONDERFUL_TOOLCHAIN, which the image also exports.
#      Do not try to "fix" this with an export PATH= line; the makefile is
#      already right and PATH is not how it looks.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
# shellcheck source=/dev/null
source "${REPO_ROOT}/scripts/toolchains.env"

# scripts/toolchains.env names the digest BLOCKSDS, which is ALSO the name the
# SDK uses for its install root. Rename it immediately, before anything can
# confuse the two -- handing an image digest to the makefile as $BLOCKSDS would
# send it looking for headers inside a sha256 string. clients/psp/build.sh
# carries the same warning about PSPDEV, which is where this bit us first.
BLOCKSDS_IMAGE="skylyrac/blocksds@${BLOCKSDS#skylyrac/blocksds@}"
unset BLOCKSDS

ACTION="${1:-build}"
log()  { printf '== clients/nds/build.sh: %s ==\n' "$*"; }
die()  { printf 'clients/nds/build.sh: %s\n' "$*" >&2; exit 1; }

make_target="all"
case "${ACTION}" in
  build) make_target="all" ;;
  clean) make_target="clean" ;;
  run)   make_target="all" ;;
  *)     die "unknown action '${ACTION}' (build | clean | run)" ;;
esac

# Dev hooks. All three change CFLAGS, AND MAKE CANNOT SEE THAT: an object file
# built with a hook looks up to date to a build without it, so the next
# "shipping" build silently reuses it and keeps the baked-in address. That is
# not hypothetical -- clients/psp/build.sh carries the same comment because a
# PSP build verified as clean still contained the previous dev build's code.
# So: whenever a hook is involved, or was involved last time, force a clean.
make_args=()
hook_stamp="${HERE}/.hooks"
hooks="${APAD_AUTO_HOST:-}|${APAD_AUTO_PORT:-}|${APAD_AUTO_AP:-}|${APAD_AUTO_BCAST:-}"
if [[ -n "${APAD_AUTO_HOST:-}" ]]; then
  make_args+=("APAD_AUTO_HOST=${APAD_AUTO_HOST}")
fi
if [[ -n "${APAD_AUTO_PORT:-}" ]]; then
  make_args+=("APAD_AUTO_PORT=${APAD_AUTO_PORT}")
fi
if [[ -n "${APAD_AUTO_AP:-}" ]]; then
  make_args+=("APAD_AUTO_AP=${APAD_AUTO_AP}")
fi
if [[ -n "${APAD_AUTO_BCAST:-}" ]]; then
  make_args+=("APAD_AUTO_BCAST=${APAD_AUTO_BCAST}")
fi
if [[ ! -f "${hook_stamp}" ]] || [[ "$(cat "${hook_stamp}" 2>/dev/null)" != "${hooks}" ]]; then
  force_clean=1
fi

run_make() {
  # A native BlocksDS install must be opted into explicitly with
  # APAD_NDS_BLOCKSDS=/path/to/blocksds/core, because $BLOCKSDS cannot be used
  # to detect one -- toolchains.env has just written an image digest into that
  # name and we have unset it above.
  if [[ -n "${APAD_NDS_BLOCKSDS:-}" ]] && command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    log "native BlocksDS -- ${APAD_NDS_BLOCKSDS}"
    make_args+=("BLOCKSDS=${APAD_NDS_BLOCKSDS}")
    if [[ -n "${force_clean:-}" ]]; then
      log "build flags changed since the last build -- cleaning first"
      ( cd "${HERE}" && make clean >/dev/null 2>&1 || true )
    fi
    ( cd "${HERE}" && make "$@" "${make_args[@]}" )
    return
  fi

  local docker_bin=""
  for candidate in docker podman; do
    if command -v "${candidate}" >/dev/null 2>&1; then docker_bin="${candidate}"; break; fi
  done
  [[ -n "${docker_bin}" ]] || die "no native BlocksDS and neither docker nor podman is installed"

  log "no native BlocksDS -- using pinned container ${BLOCKSDS_IMAGE}"
  # --user: the image runs as root, and without this every artifact and every
  # object under build/ comes out root-owned -- which then makes the NEXT
  # build fail for the ordinary user with a permission error rather than a
  # useful message. HOME=/tmp for the same reason: $HOME is /root in there.
  "${docker_bin}" run --rm \
      --user "$(id -u):$(id -g)" \
      -v "${REPO_ROOT}:/repo" \
      -w /repo/clients/nds \
      "${BLOCKSDS_IMAGE}" \
      sh -c "export HOME=/tmp; ${force_clean:+make clean >/dev/null 2>&1;} make $* ${make_args[*]}"
}

run_make "${make_target}"

# The clean has happened. Leaving force_clean set would make the `size`
# invocation below wipe build/ and compile the whole thing a second time --
# which is what it did until this line existed.
unset force_clean

printf '%s' "${hooks}" > "${hook_stamp}"

if [[ "${make_target}" == "all" ]]; then
  [[ -f "${HERE}/atticpad-nds.nds" ]] || die "make exited 0 but atticpad-nds.nds does not exist"
  log "built ${HERE}/atticpad-nds.nds ($(du -h "${HERE}/atticpad-nds.nds" | cut -f1))"
  run_make size || true
fi

# `run` is this platform's test loop. melonDS has working networking (indirect
# / libslirp mode) and reaches a server on the host by unicast, so this is a
# real session against a real server rather than a boot check.
if [[ "${ACTION}" == "run" ]]; then
  if command -v flatpak >/dev/null 2>&1 && flatpak info net.kuribo64.melonDS >/dev/null 2>&1; then
    log "launching melonDS"
    exec flatpak run net.kuribo64.melonDS "${HERE}/atticpad-nds.nds"
  fi
  log "melonDS not installed; copy atticpad-nds.nds to a flashcart instead"
fi
