#!/usr/bin/env bash
#
# clients/psp/build.sh -- build the PSP client.
#
# Mirrors clients/3ds/build.sh: prefer a native toolchain for iteration speed,
# otherwise use the SAME pinned container digest CI uses, so a local pass and a
# CI pass mean the same thing.
#
# Unlike the 3DS script there is no `export PATH=` for the container: the
# pspdev image already has /usr/local/pspdev/bin on PATH and sets PSPDEV
# itself. NEVER pass -e PSPDEV -- outside the container that name holds an
# image digest, and shadowing the SDK root with it breaks psp-config.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"
# shellcheck source=/dev/null
source "${REPO_ROOT}/scripts/toolchains.env"

ACTION="${1:-build}"
log()  { printf '== clients/psp/build.sh: %s ==\n' "$*"; }
die()  { printf 'clients/psp/build.sh: %s\n' "$*" >&2; exit 1; }

make_target="all"
case "${ACTION}" in
  build) make_target="all" ;;
  clean) make_target="clean" ;;
  run)   make_target="all" ;;
  zip)   make_target="all" ;;
  *)     die "unknown action '${ACTION}' (build | clean | run | zip)" ;;
esac

# Dev hooks. Both change CFLAGS, and MAKE CANNOT SEE THAT: object files built
# with a hook look up to date to a build without it, so the next "shipping"
# build silently reuses them. That is not hypothetical -- a build verified as
# clean here still contained the screenshot-tour code, because main.o was left
# over from the previous dev build. So: whenever a hook is involved, or was
# involved last time, force a clean first. A stale object is cheaper to
# recompile than to debug.
make_args=()
hook_stamp="${HERE}/.hooks"
hooks="${APAD_SMOKE_HOST:-}|${APAD_PSP_SHOTS:-}|${APAD_AUTO_HOST:-}|${APAD_AUTO_PORT:-}|${APAD_PSP_DEVLOG:-}|${APAD_PSP_STDOUT:-}|${APAD_PSP_FAKE_RESUME:-}"
if [[ -n "${APAD_SMOKE_HOST:-}" ]]; then
  make_args+=("APAD_SMOKE_HOST=${APAD_SMOKE_HOST}")
fi
if [[ -n "${APAD_PSP_SHOTS:-}" ]]; then
  make_args+=("APAD_PSP_SHOTS=${APAD_PSP_SHOTS}")
fi
if [[ -n "${APAD_AUTO_HOST:-}" ]]; then
  make_args+=("APAD_AUTO_HOST=${APAD_AUTO_HOST}")
fi
if [[ -n "${APAD_AUTO_PORT:-}" ]]; then
  make_args+=("APAD_AUTO_PORT=${APAD_AUTO_PORT}")
fi
if [[ -n "${APAD_PSP_DEVLOG:-}" ]]; then
  make_args+=("APAD_PSP_DEVLOG=${APAD_PSP_DEVLOG}")
fi
if [[ -n "${APAD_PSP_STDOUT:-}" ]]; then
  make_args+=("APAD_PSP_STDOUT=${APAD_PSP_STDOUT}")
fi
if [[ -n "${APAD_PSP_FAKE_RESUME:-}" ]]; then
  make_args+=("APAD_PSP_FAKE_RESUME=${APAD_PSP_FAKE_RESUME}")
fi
if [[ ! -f "${hook_stamp}" ]] || [[ "$(cat "${hook_stamp}" 2>/dev/null)" != "${hooks}" ]]; then
  force_clean=1
fi

if command -v psp-config >/dev/null 2>&1; then
  log "native pspsdk found -- $(psp-config --pspsdk-path)"
  if [[ -n "${force_clean:-}" ]]; then
    log "build flags changed since the last build -- cleaning first"
    ( cd "${HERE}" && make clean >/dev/null 2>&1 || true )
  fi
  ( cd "${HERE}" && make "${make_target}" "${make_args[@]}" )
else
  docker_bin=""
  for candidate in docker podman; do
    if command -v "${candidate}" >/dev/null 2>&1; then docker_bin="${candidate}"; break; fi
  done
  [[ -n "${docker_bin}" ]] || die "no native pspsdk and neither docker nor podman is installed"

  log "no native pspsdk -- using pinned container ${PSPDEV}"
  # --user: without it the artifacts come out root-owned, which the spike hit.
  # sh, not bash: /bin/sh in this image is busybox and nothing here needs more.
  "${docker_bin}" run --rm \
      --user "$(id -u):$(id -g)" \
      -v "${REPO_ROOT}:/repo" \
      -w /repo/clients/psp \
      "pspdev/pspdev@${PSPDEV#pspdev/pspdev@}" \
      sh -c "export HOME=/tmp; ${force_clean:+make clean >/dev/null 2>&1;} make ${make_target} ${make_args[*]}"
fi

printf '%s' "${hooks}" > "${hook_stamp}"

if [[ "${make_target}" == "all" ]]; then
  [[ -f "${HERE}/EBOOT.PBP" ]] || die "make exited 0 but EBOOT.PBP does not exist"
  log "built ${HERE}/EBOOT.PBP ($(du -h "${HERE}/EBOOT.PBP" | cut -f1))"
fi

# `run` is this platform's answer to 3dslink netload: stage the memory-stick
# layout a PSP actually requires and launch the emulator on it. PPSSPP's
# memory stick is a plain host directory, which is also where the client's
# ms0:/ log lands.
if [[ "${ACTION}" == "run" ]]; then
  MEMSTICK="${APAD_PSP_MEMSTICK:-${HOME}/.var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP}"
  dest="${MEMSTICK}/GAME/ATTICPAD"
  mkdir -p "${dest}"
  cp "${HERE}/EBOOT.PBP" "${dest}/EBOOT.PBP"
  log "installed to ${dest}/EBOOT.PBP"
  if command -v flatpak >/dev/null 2>&1 && flatpak info org.ppsspp.PPSSPP >/dev/null 2>&1; then
    log "launching PPSSPP"
    exec flatpak run org.ppsspp.PPSSPP "${dest}/EBOOT.PBP"
  fi
  log "PPSSPP not installed; copy the memory stick to a console instead"
fi

# `zip` stages the layout a PSP actually requires and packs it.
#
# A release asset named atticpad-psp.pbp would not run on a console: it has to
# be EBOOT.PBP inside PSP/GAME/<name>/. An asset the user must rename AND file
# correctly from a README is an asset that generates support requests, so the
# zip extracts onto a memory-stick root and works.
#
# python3 rather than the zip(1) binary: the pinned image does not ship zip,
# and neither do all dev boxes.
if [[ "${ACTION}" == "zip" ]]; then
  stage="${HERE}/.zipstage"
  rm -rf "${stage}"
  mkdir -p "${stage}/PSP/GAME/ATTICPAD"
  cp "${HERE}/EBOOT.PBP" "${stage}/PSP/GAME/ATTICPAD/EBOOT.PBP"
  ( cd "${stage}" && python3 -c "
import zipfile, os, sys
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk('.'):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, '.'))
" "${HERE}/atticpad-psp.zip" )
  rm -rf "${stage}"
  log "packed ${HERE}/atticpad-psp.zip"
  python3 -c "
import zipfile, sys
print('  contains:', zipfile.ZipFile(sys.argv[1]).namelist())
" "${HERE}/atticpad-psp.zip"
fi
