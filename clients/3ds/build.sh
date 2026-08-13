#!/usr/bin/env bash
# clients/3ds/build.sh
#
# Local build wrapper for the AtticPad 3DS client, following the pattern
# established by tools/fuzz/build.sh and tools/loopback-client/build.sh: a
# helper local to this directory, not scripts/build.sh (that file's '3ds'
# target is owned by ci-engineer and currently exits with "not implemented
# until M2" -- another agent wires this script in from there; this task
# does not touch scripts/).
#
# docs/DESIGN.md S8.5's one deliberate exception applies here: native devkitPro is
# used when present, for 3dslink netload iteration speed (~2s to real
# hardware). Absent a native install, this script falls back to the exact
# pinned devkitARM image CI uses (scripts/toolchains.env, key DEVKITARM),
# via Docker/Podman, so a machine with no local devkitPro can still build.
# CI itself always uses the pinned container -- this fallback is what keeps
# that equivalent, not a way around it.
#
# Usage:
#   clients/3ds/build.sh                     # build .3dsx only
#   clients/3ds/build.sh netload <3ds-ip>    # build, then 3dslink to hardware
#   clients/3ds/build.sh cia                 # build .3dsx AND .cia
#   clients/3ds/build.sh clean
#
# .cia packaging (docs/DESIGN.md S8.3, added after the M2 task that produced the
# rest of this file): `cia` builds the .3dsx/.elf the same way `build`
# does, then fetches the pinned makerom release (scripts/cia-tools.env) if
# it is not already on PATH, and runs `make -C clients/3ds cia`. makerom is
# a host binary, not a 3DS cross-compiler -- it does not need devkitARM, so
# this works the same way whether the .elf above it was built natively or
# via the container. bannertool is deliberately never fetched here -- see
# scripts/cia-tools.env and clients/3ds/meta/app.rsf's header comment
# (docs/DESIGN.md S8.3 / risk 11: archived upstream, confirmed gone entirely as of
# 2026-08-10). clients/3ds/meta/banner.bnr and icon.icn are committed
# prebuilt outputs, fed to makerom directly.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"

log() { printf '== clients/3ds/build.sh: %s ==\n' "$*"; }
warn() { printf 'clients/3ds/build.sh: WARNING: %s\n' "$*" >&2; }

# shellcheck source=/dev/null
source "${REPO_ROOT}/scripts/toolchains.env"

ACTION="${1:-build}"

run_make() {
  local make_target="$1"
  if [[ -n "${DEVKITARM_NATIVE:-}" ]] && command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    log "using native devkitARM at ${DEVKITPRO:-<unset>} (docs/DESIGN.md S8.5 fast path)"
    ( cd "${HERE}" && DEVKITARM="${DEVKITARM_NATIVE}" make "${make_target}" )
  else
    log "no native devkitARM found -- using pinned container ${DEVKITARM}"
    local docker_bin
    if command -v docker >/dev/null 2>&1; then
      docker_bin=docker
    elif command -v podman >/dev/null 2>&1; then
      docker_bin=podman
    else
      echo "clients/3ds/build.sh: need docker or podman to build without native devkitPro" >&2
      exit 1
    fi
    "${docker_bin}" run --rm \
      --user "$(id -u):$(id -g)" \
      -v "${REPO_ROOT}:/repo" \
      -w /repo/clients/3ds \
      "devkitpro/devkitarm@${DEVKITARM#devkitpro/devkitarm@}" \
      bash -c "export DEVKITARM=/opt/devkitpro/devkitARM DEVKITPRO=/opt/devkitpro HOME=/tmp; make ${make_target}"
  fi
}

# fetch_makerom — downloads the pinned makerom release (scripts/cia-tools.env)
# into ${REPO_ROOT}/build/cia-tools/ (gitignored, same as every other build
# output) and prints its directory on stdout, so the caller can add it to
# PATH. Skipped entirely if `makerom` is already reachable on the caller's
# PATH -- e.g. a dev who built it from source themselves.
fetch_makerom() {
  # NOTE: this function's stdout is captured with $(...) by its caller --
  # every diagnostic line MUST go to stderr (>&2), or it corrupts the
  # captured path. Only the final cache/bin directory is written to stdout.
  if command -v makerom >/dev/null 2>&1; then
    log "using makerom already on PATH: $(command -v makerom)" >&2
    dirname "$(command -v makerom)"
    return 0
  fi

  # shellcheck source=/dev/null
  source "${REPO_ROOT}/scripts/cia-tools.env"

  local cache_dir="${REPO_ROOT}/build/cia-tools"
  local bin="${cache_dir}/makerom"
  if [[ -x "${bin}" ]]; then
    log "using cached makerom at ${bin} (${MAKEROM_VERSION})" >&2
    echo "${cache_dir}"
    return 0
  fi

  log "fetching pinned makerom ${MAKEROM_VERSION} (${MAKEROM_LINUX_URL})" >&2
  mkdir -p "${cache_dir}"
  local zip="${cache_dir}/makerom.zip"
  curl -sSL -o "${zip}" "${MAKEROM_LINUX_URL}"
  echo "${MAKEROM_LINUX_SHA256}  ${zip}" | sha256sum -c - >&2
  unzip -q -o "${zip}" -d "${cache_dir}"
  chmod +x "${bin}"
  rm -f "${zip}"
  echo "${cache_dir}"
}

case "${ACTION}" in
  build)
    run_make all
    log "built ${HERE}/atticpad-3ds.3dsx"
    ;;
  cia)
    run_make all
    log "built ${HERE}/atticpad-3ds.3dsx"
    makerom_dir="$(fetch_makerom)"
    ( cd "${HERE}" && PATH="${makerom_dir}:${PATH}" make cia )
    log "built ${HERE}/atticpad-3ds.cia"
    warn "assets in clients/3ds/meta/ (banner.png, icon.png, audio.wav and the .bnr/.icn built from them) are PLACEHOLDERS, not final artwork -- see scripts/cia-tools.env for how to regenerate them."
    warn ".cia format is hardware-proven -- installed via FBI remote install and launched on a real New 3DS (2026-08-10) -- but THIS build is unverified: the script only confirms makerom succeeded and the file exists."
    ;;
  clean)
    run_make clean
    ;;
  netload)
    target_ip="${2:-}"
    if [[ -z "${target_ip}" ]]; then
      echo "usage: clients/3ds/build.sh netload <3ds-ip>" >&2
      exit 1
    fi
    run_make all
    log "built ${HERE}/atticpad-3ds.3dsx"
    log "3dslink -> ${target_ip}:17491 (press Y in the Homebrew Launcher first)"
    if command -v 3dslink >/dev/null 2>&1; then
      3dslink -a "${target_ip}" "${HERE}/atticpad-3ds.3dsx"
    else
      docker_bin=docker
      command -v docker >/dev/null 2>&1 || docker_bin=podman
      "${docker_bin}" run --rm --network host \
        -v "${HERE}:/out" \
        "devkitpro/devkitarm@${DEVKITARM#devkitpro/devkitarm@}" \
        3dslink -a "${target_ip}" "/out/atticpad-3ds.3dsx"
    fi
    ;;
  *)
    echo "clients/3ds/build.sh: unknown action '${ACTION}' (build|cia|clean|netload)" >&2
    exit 1
    ;;
esac
