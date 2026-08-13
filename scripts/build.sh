#!/usr/bin/env bash
# scripts/build.sh — the local build wrapper (docs/DESIGN.md §8.5, docs/CONVENTIONS.md
# "Build and test").
#
# CI is the gate; local is the loop. The rule that keeps the two equivalent:
# console targets here use the SAME pinned image digest as CI, sourced from
# scripts/toolchains.env by KEY (DEVKITARM, DEVKITA64, PSPDEV, VITASDK) —
# never a hardcoded tag or path. If a provider swaps, one file changes.
#
# This wrapper NEVER produces a release artifact. It builds and, where it is
# safe and fast to do so, RUNS the result (self-test, smoke test). Releases
# come only from tagged CI runs (release.yml — not yet written, M1 has no
# artifacts to release).
#
# Usage:
#   scripts/build.sh core      # native, run constantly (~2s)
#   scripts/build.sh server    # native Linux server (uinput backend)
#   scripts/build.sh tools     # tools/fuzz + tools/loopback-client
#   scripts/build.sh all       # core + server + tools, in that order
#   scripts/build.sh 3ds       # clients/3ds/build.sh (native devkitPro if
#                               # present, else the pinned DEVKITARM digest)
#   scripts/build.sh 3ds-cia   # 3ds, plus a .cia (docs/DESIGN.md S8.3) via the
#                               # pinned makerom release (scripts/cia-tools.env)
#   scripts/build.sh psp|nds|switch   # not implemented yet (docs/DESIGN.md S11: M5)
#   scripts/build.sh vita      # shelved — see docs/DESIGN.md S11 (vitasdk-docker
#                               # can't run in its own image, not "not built yet")
#   scripts/build.sh windows   # cross-compiled Windows server (mingw-w64,
#                               # -static). Compile-only — this is Linux,
#                               # there is nothing here that can run a .exe.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"

# Overridable so two builds running against the tree at once do not race on
# the same object files. They already touch disjoint SOURCE directories, but
# build/ was shared by every one of them, and a raced build can report a green
# 1018/1018 that is not real. This project's whole verification story rests on that number
# meaning something.
#
#   APAD_BUILD_DIR=build-agentname ./scripts/build.sh all
#
# Cheaper than a git worktree per agent and enough for the common case; a
# worktree is still the right answer when agents must touch the SAME files.
BUILD_DIR="${APAD_BUILD_DIR:-${REPO_ROOT}/build}"

CORE_INC="${REPO_ROOT}/core/include"
CORE_SRC="${REPO_ROOT}/core/src"
SHIM="${REPO_ROOT}/shim"

CC_NATIVE="${CC:-cc}"

# Pinned digests. Sourced by KEY so console targets (below, once they exist)
# and CI's container: fields both reference DEVKITARM/DEVKITA64/PSPDEV/VITASDK
# rather than a tag. Never edit toolchains.env by hand — see
# scripts/pin-toolchains.sh.
# shellcheck source=toolchains.env
source "${HERE}/toolchains.env"

log() { printf '== scripts/build.sh: %s ==\n' "$*"; }
warn() { printf 'scripts/build.sh: WARNING: %s\n' "$*" >&2; }

usage() {
  cat <<EOF
Usage: scripts/build.sh <target>

Targets that exist today:
  core      native libapad build + run the on-device self-test (ASan+UBSan)
  server    native Linux server, uinput backend (brief bind/init smoke run)
  tools     tools/fuzz (build only) + tools/server-harness (build + run,
            fake clock, no sockets) + tools/loopback-client (build + run)
  all       core + server + tools, in that order
  3ds       clients/3ds/build.sh — .3dsx only. Uses native devkitPro if
            present, else the pinned DEVKITARM digest via Docker/Podman —
            DEVKITARM=${DEVKITARM:-<unset>}
  android   clients/android/build.sh — debug .apk (arm64-v8a, armeabi-v7a,
            x86_64). NATIVE SDK/NDK, not a container: docs/DESIGN.md §8.5 schedules
            Android that way and §8.1's CI job matches. Versions are pinned in
            scripts/android.env; scripts/android-sdk-bootstrap.sh installs
            them without root. Extra args pass through:
              scripts/build.sh android install <serial>
              scripts/build.sh android selftest <serial>   # RUNS the §13
                                                           # vectors on-device
  3ds-cia   3ds, plus a .cia (docs/DESIGN.md §8.3) — fetches the pinned makerom
            release (scripts/cia-tools.env) if not already on PATH.
            bannertool is never fetched: clients/3ds/meta/banner.bnr and
            icon.icn are committed prebuilt outputs (archived upstream,
            see scripts/cia-tools.env). Never produces a signed/published
            release artifact — this is the local dev-loop equivalent of
            what a tagged CI run does, not a substitute for it.
  windows   server/host/windows/ — atticpad-server.exe, cross-compiled from
            Linux with mingw-w64 (x86_64-w64-mingw32-gcc/g++, -static —
            server/backends/vendor/README.md, docs/DESIGN.md §2.1: the Windows test
            machine has no C toolchain at all). Versions are pinned in
            scripts/windows.env, the same role scripts/android.env plays for
            Android (a host package, not a container digest); run
            scripts/windows-mingw-check.sh if it is missing — that script
            only checks and reports, it does not apt-get anything, this
            machine has no passwordless sudo. Build-only: this wrapper never
            runs the result (Linux cannot run a PE32+ binary) and never
            signs anything — see the "Build and test" note above for what
            that means for reporting.

Console targets with no client code yet (docs/DESIGN.md §11 roadmap: M5). These
print a clear message and exit nonzero rather than failing on a missing
toolchain or a confusing compiler error:
  switch    devkitA64   — DEVKITA64=${DEVKITA64:-<unset>}
  psp       PSPDEV      — PSPDEV=${PSPDEV:-<unset>}
  nds       BlocksDS    — not yet in scripts/toolchains.env (docs/DESIGN.md D8: no
                          pinned BlocksDS image chosen yet)

Shelved, not merely unbuilt (docs/DESIGN.md §11):
  vita      VitaSDK     — VITASDK=${VITASDK:-<unset>} — gnuton/vitasdk-docker's
            toolchain binaries need glibc 2.36/2.38 against an image shipping
            2.35; the compiler cannot execute at all, on :latest or the
            pinned digest. Revisit when a working image exists — this is not
            a "not implemented yet" gap to fill by writing client code.

Every OTHER console target, once client code exists, builds against the
pinned digest above via a container (Docker/Podman) — the exact same digest
CI's workflow uses, referenced by key from scripts/toolchains.env, never a
hardcoded tag or local devkitPro install path. See docs/DESIGN.md §8.5 on the one
deliberate exception (native devkitPro for 3ds, for netload iteration speed)
and why CI still catches drift there.

This wrapper never signs anything and never produces a release artifact.
EOF
}

# ---------------------------------------------------------------------------
# core
# ---------------------------------------------------------------------------
build_core() {
  log "core: compiling libapad + self-test runner (-std=c99 -Wall -Wextra -Werror -pedantic, ASan+UBSan)"
  mkdir -p "${BUILD_DIR}/core"
  "${CC_NATIVE}" -std=c99 -Wall -Wextra -Werror -pedantic -g -O1 \
      -fsanitize=address,undefined \
      -I"${CORE_INC}" \
      "${CORE_SRC}"/*.c \
      "${HERE}/support/core_selftest_main.c" \
      -o "${BUILD_DIR}/core/core-selftest"

  log "core: running apad_selftest_run() (codec invariants + core/testdata/vectors.h, unconditionally — see core/src/selftest.c)"
  # The printed "N/M passed" line below IS the signal that catches a vectors.h
  # section landing with no driver (see scripts/support/check_vectors_wired.sh's
  # header for the incident this is about): a dead vector doesn't fail, it
  # just never increments M. core_selftest_main.c prints the total on every
  # run, unconditionally, for exactly that reason — capture it to a file here
  # only so this step can also echo it as its own clearly-labelled line,
  # since scrolling CI/terminal output is where a case-count regression is
  # easiest to miss.
  "${BUILD_DIR}/core/core-selftest" | tee "${BUILD_DIR}/core/selftest-output.txt"
  log "core: case count — $(grep -o '[0-9]*/[0-9]* passed, [0-9]* failed' "${BUILD_DIR}/core/selftest-output.txt")"

  log "core: vectors.h standalone compile check (depends on nothing but stdint.h — see scripts/support/vectors_compile_check.c)"
  "${CC_NATIVE}" -std=c99 -Wall -Wextra -Werror -pedantic \
      -I"${REPO_ROOT}/core/testdata" \
      "${HERE}/support/vectors_compile_check.c" \
      -o "${BUILD_DIR}/core/vectors-compile-check"
  "${BUILD_DIR}/core/vectors-compile-check"

  log "core: dead-vectors guard — every *_vectors[] table in core/testdata/vectors.h must have a driver in core/src/selftest.c (scripts/support/check_vectors_wired.sh)"
  "${HERE}/support/check_vectors_wired.sh"
}

# ---------------------------------------------------------------------------
# server
# ---------------------------------------------------------------------------
# libapadserver (server/src/*.c + the backend) is sans-IO: no socket, no
# clock, no stdout. The host (server/host/linux/main.c) supplies all three.
# Compiled as one command rather than an archive because there is exactly one
# host per platform and a .a would buy nothing but a step -- the split that
# matters is the API in server/include/apadserver.h, not the link unit.
build_server() {
  log "server: compiling libapadserver + linux host -> atticpad-server (-std=c11 -Wall -Wextra -Werror, uinput backend)"
  mkdir -p "${BUILD_DIR}/server"
  "${CC_NATIVE}" -std=c11 -Wall -Wextra -Werror -g -O1 \
      -I"${CORE_INC}" -I"${REPO_ROOT}/server/backends" \
      -I"${REPO_ROOT}/server/include" -I"${REPO_ROOT}/server/src" \
      "${CORE_SRC}/codec.c" "${CORE_SRC}/hmac_sha256.c" \
      "${CORE_SRC}/seq.c" "${CORE_SRC}/session.c" \
      "${SHIM}/net_bsd.c" "${SHIM}/time_posix.c" \
      "${REPO_ROOT}/server/src/server.c" "${REPO_ROOT}/server/src/mapping.c" \
      "${REPO_ROOT}/server/src/jsonc.c" "${REPO_ROOT}/server/src/profiles.c" \
      "${REPO_ROOT}/server/src/pairing.c" \
      "${REPO_ROOT}/server/backends/uinput.c" \
      "${REPO_ROOT}/server/host/linux/main.c" \
      -o "${BUILD_DIR}/server/atticpad-server"

  log "server: smoke run — bind a scratch port, init uinput backend, SIGTERM, expect clean shutdown"
  if [[ -e /dev/uinput ]] && [[ -r /dev/uinput && -w /dev/uinput ]]; then
    local port=21199
    "${BUILD_DIR}/server/atticpad-server" "${port}" &
    local pid=$!
    sleep 1
    if ! kill -0 "${pid}" 2>/dev/null; then
      wait "${pid}" || true
      warn "server: process exited on its own before the smoke test finished — see output above (already-running server on ${port}? no /dev/uinput access?)"
      exit 1
    fi
    kill -TERM "${pid}"
    wait "${pid}"
    log "server: clean shutdown (exit 0), backend initialized and socket bound"
  else
    warn "server: /dev/uinput not present or not read/write-able for this user — skipping the smoke run, binary is built but unexercised. See docs/CONVENTIONS.md 'Build and test': build AND run before reporting, so this is a real gap, not a pass."
  fi
}

# ---------------------------------------------------------------------------
# windows — cross-compiled from Linux with mingw-w64 (docs/DESIGN.md §2.1: the
# Windows test machine has no C toolchain at all, only git). Same
# libapadserver split as build_server above, different host and different
# backend: server/host/windows/main.c + server/backends/vigem.c (ViGEmBus)
# instead of server/host/linux/main.c + server/backends/uinput.c, and
# shim/net_winsock.c + shim/time_win32.c instead of net_bsd.c/time_posix.c.
#
# Two things this build does differently from every other C target in this
# file, both load-bearing (server/backends/vendor/README.md has the full
# story):
#
#   - The vendored ViGEmClient.cpp (server/backends/vendor/vigemclient/) is
#     C++, so the FINAL LINK goes through x86_64-w64-mingw32-g++, not -gcc,
#     and MUST pass -static: the target machine has no C/C++ runtime DLLs
#     of its own, and a non-static link pulls in libgcc_s_seh-1.dll and
#     libstdc++-6.dll (confirmed with objdump -p while verifying this
#     target) that would simply be missing on it.
#   - server/backends/vendor/mingw-compat/ (case-shim headers) and
#     server/backends/vendor/vigemclient/include/ go on the include path
#     with -isystem, not -I: ViGEm/Common.h's `VOID FORCEINLINE ...` trips
#     -Wold-style-declaration under our -Werror, and -isystem is what
#     silences THAT header without weakening -Werror on anything this
#     project owns. The vendored .cpp itself is compiled WITHOUT -Werror
#     (still with -Wall -Wextra, so real breakage is visible) — it is not
#     "our own code" and -isystem does not reach warnings a .cpp emits
#     about its own body, only about what it #includes.
#
# mingw-w64 is a host package, not a container digest (scripts/windows.env,
# the same role scripts/android.env plays for the Android SDK/NDK) — see
# that file for the exact verified version and scripts/windows-mingw-check.sh
# for how it's checked (report-only: this machine has no passwordless sudo).
#
# Compile-only, always: this is Linux, nothing here can run a PE32+ binary,
# and a local build must never produce a release artifact (docs/DESIGN.md §8.5) —
# this is the exact cross-compile CI's server-windows job runs, just without
# the artifact upload.
build_windows() {
  "${HERE}/windows-mingw-check.sh"
  # shellcheck source=windows.env
  source "${HERE}/windows.env"

  local CC_WIN="${APAD_MINGW_TRIPLE}-gcc"
  local CXX_WIN="${APAD_MINGW_TRIPLE}-g++"
  local VENDOR="${REPO_ROOT}/server/backends/vendor"
  local OUT="${BUILD_DIR}/windows"
  mkdir -p "${OUT}"

  # -Wno-overlength-strings: server/host/common/assets.h embeds the UI's
  # HTML/CSS/JS bundle as one ~9 KB string literal, and -pedantic enforces
  # ISO C99's guaranteed-supported MINIMUM of 4095 bytes. That minimum is a
  # floor every compiler must accept, not a ceiling any real one imposes --
  # gcc, clang and MSVC all handle megabyte literals. This is the single
  # narrowest way to keep -pedantic switched on for everything else; the
  # alternative (rewriting the bundle as a byte-array initializer) would
  # make assets.h unreadable and unmaintainable to satisfy a limit nothing
  # actually enforces. The Linux server build never hit this because it does
  # not pass -pedantic at all -- this target is the stricter of the two.
  local CFLAGS=(-std=c11 -Wall -Wextra -Werror -pedantic -Wno-overlength-strings -g -O1
      -I"${CORE_INC}" -I"${REPO_ROOT}/server/backends"
      -I"${REPO_ROOT}/server/include" -I"${REPO_ROOT}/server/src"
      -isystem "${VENDOR}/vigemclient/include")

  log "windows: compiling libapadserver + core + shim (mingw-w64 ${APAD_MINGW_TRIPLE}, -std=c11 -Wall -Wextra -Werror -pedantic)"
  local objs=()
  local c_srcs=(
    "${CORE_SRC}/codec.c" "${CORE_SRC}/hmac_sha256.c"
    "${CORE_SRC}/seq.c" "${CORE_SRC}/session.c"
    "${SHIM}/net_winsock.c" "${SHIM}/time_win32.c"
    "${REPO_ROOT}/server/src/server.c" "${REPO_ROOT}/server/src/mapping.c"
    "${REPO_ROOT}/server/src/jsonc.c" "${REPO_ROOT}/server/src/profiles.c"
    "${REPO_ROOT}/server/src/pairing.c"
    "${REPO_ROOT}/server/backends/vigem.c"
  )
  for src in "${c_srcs[@]}"; do
    local obj="${OUT}/$(basename "${src}").o"
    "${CC_WIN}" "${CFLAGS[@]}" -c "${src}" -o "${obj}"
    objs+=("${obj}")
  done
  log "windows: ${#c_srcs[@]}/${#c_srcs[@]} translation units compiled clean (shim, libapadserver, vigem.c backend)"

  log "windows: compiling vendored ViGEmClient.cpp (server/backends/vendor/vigemclient/, pinned commit — see vendor/README.md; -isystem, not -Werror — third-party source, see this function's header comment)"
  local vigemclient_obj="${OUT}/ViGEmClient.cpp.o"
  "${CXX_WIN}" -std=c++17 -Wall -Wextra -g -O1 \
      -isystem "${VENDOR}/mingw-compat" \
      -isystem "${VENDOR}/vigemclient/include" \
      -isystem "${VENDOR}/vigemclient/src" \
      -c "${VENDOR}/vigemclient/src/ViGEmClient.cpp" -o "${vigemclient_obj}"
  objs+=("${vigemclient_obj}")
  log "windows: vendored ViGEmClient.cpp compiled clean"

  local host_main="${REPO_ROOT}/server/host/windows/main.c"
  if [[ ! -f "${host_main}" ]]; then
    cat >&2 <<EOF
scripts/build.sh: windows: ${host_main} does not exist yet.

Everything that DOES exist was just verified to compile clean above: the
core sources, shim/net_winsock.c + shim/time_win32.c, server/src/*.c,
server/backends/vigem.c, and the vendored ViGEmClient.cpp translation unit
(server/backends/vendor/vigemclient/). None of that proves the host, once
it lands, links or runs correctly — only that everything under it is sound.

Not treating this as success: no atticpad-server.exe was produced, and this
wrapper never fabricates one. See scripts/build.sh's build_windows() for
the exact link command to run once the host file exists.
EOF
    exit 1
  fi

  log "windows: compiling server/host/windows/main.c"
  local main_obj="${OUT}/main.c.o"
  "${CC_WIN}" "${CFLAGS[@]}" -c "${host_main}" -o "${main_obj}"
  objs+=("${main_obj}")

  # Win32 resources: the .exe/tray icon and VERSIONINFO. The .ico itself is
  # GENERATED by clients/3ds/meta/make-assets.py --windows (the one place
  # the AtticPad mark is drawn) -- never hand-edited. windres needs -I for
  # both the .rc's own directory (atticpad.ico, resource.h) and core's
  # include path (version.h, so the version string cannot drift from what
  # the server reports).
  local host_rc="${REPO_ROOT}/server/host/windows/atticpad.rc"
  if [[ -f "${host_rc}" ]]; then
    log "windows: compiling resources (icon + VERSIONINFO) with windres"
    local res_obj="${OUT}/atticpad.rc.o"
    "${APAD_MINGW_TRIPLE}-windres" \
        -I "${REPO_ROOT}/server/host/windows" -I "${CORE_INC}" \
        "${host_rc}" -o "${res_obj}"
    objs+=("${res_obj}")
  fi

  log "windows: linking atticpad-server.exe (${CXX_WIN} -static — C++ link because of the vendored ViGEmClient TU; -static so the target machine, which has no C/C++ runtime DLLs of its own, needs none)"
  # shellcheck disable=SC2086
  "${CXX_WIN}" -static -o "${OUT}/atticpad-server.exe" "${objs[@]}" ${APAD_MINGW_LIBS}

  if [[ ! -f "${OUT}/atticpad-server.exe" ]]; then
    warn "windows: link reported success but ${OUT}/atticpad-server.exe does not exist."
    exit 1
  fi

  local filetype
  filetype="$(file -b "${OUT}/atticpad-server.exe")"
  log "windows: built ${OUT}/atticpad-server.exe ($(du -h "${OUT}/atticpad-server.exe" | cut -f1)) — ${filetype}"
  case "${filetype}" in
    *"PE32+ executable"*) ;;
    *)
      warn "windows: ${OUT}/atticpad-server.exe does not report as a PE32+ executable (got: ${filetype}) — the mingw-w64 toolchain or link flags may have changed."
      exit 1
      ;;
  esac

  warn "windows: build-only, as documented — this is Linux, nothing here can execute a PE32+ binary. NOT run, NOT smoke-tested. See the ci-engineer report for what was verified on real Windows hardware (server/backends/vigem.c's header) versus what was only compiled here."
}

# ---------------------------------------------------------------------------
# tools
# ---------------------------------------------------------------------------
build_tools() {
  log "tools: fuzz harness (build only — 'tools/fuzz/build.sh <seconds>' to actually fuzz; that binary is owned by tools/, not this script)"
  "${REPO_ROOT}/tools/fuzz/build.sh" build-only

  # server-harness drives libapadserver with a FAKE CLOCK and NO SOCKETS --
  # the thing docs/DESIGN.md §6.4 said the split was for. It reaches what
  # loopback-client structurally cannot: a send that fails on demand, a
  # spoofed source address, and §9's retransmit schedule observed at exact
  # millisecond boundaries.
  log "tools: server-harness (build + run libapadserver scenarios, no sockets)"
  "${REPO_ROOT}/tools/server-harness/build.sh" run

  log "tools: loopback-client (build + run its own self-loopback smoke test)"
  "${REPO_ROOT}/tools/loopback-client/build.sh" run

  # engine-client drives clients/common/apad_client.c -- the SHARED client
  # engine every real client (Android, 3DS, ...) links -- through a real
  # session. It tests THE ENGINE; loopback-client tests THE SERVER (it
  # hand-rolls adversarial datagrams of its own). Neither replaces the
  # other -- see tools/engine-client/main.c's header. Build only here: it
  # has no self-loopback mode, it needs a live server on the far end (see
  # .github/workflows/ci.yml's integration job for where it actually runs).
  log "tools: engine-client (build only -- needs a live server to run; see ci.yml's integration job)"
  "${REPO_ROOT}/tools/engine-client/build.sh" build-only
}

# ---------------------------------------------------------------------------
# 3ds — clients/3ds/ landed at M2 (commit 453b8e3). This wrapper does not
# duplicate the container invocation: clients/3ds/build.sh is the one place
# that knows how to build a 3DS binary (native devkitPro if present, else
# the pinned DEVKITARM digest from toolchains.env, sourced above, via
# Docker/Podman — see that script's header). scripts/build.sh just calls it
# and confirms the artifact landed. Build only — this wrapper never signs
# or runs on hardware; that is clients/3ds/build.sh netload's job, by hand.
# .3dsx only, no .cia — see docs/DESIGN.md §8.3 and clients/3ds/build.sh's header
# comment for why that is out of scope here.
# ---------------------------------------------------------------------------
build_3ds() {
  log "3ds: delegating to clients/3ds/build.sh (DEVKITARM=${DEVKITARM})"
  "${REPO_ROOT}/clients/3ds/build.sh"

  local artifact="${REPO_ROOT}/clients/3ds/atticpad-3ds.3dsx"
  if [[ ! -f "${artifact}" ]]; then
    warn "3ds: clients/3ds/build.sh exited 0 but ${artifact} does not exist — build.sh's own success message does not guarantee this path; treating as a failure."
    exit 1
  fi
  log "3ds: confirmed ${artifact} ($(du -h "${artifact}" | cut -f1))"
}

# ---------------------------------------------------------------------------
# 3ds-cia — .3dsx + .cia (docs/DESIGN.md §8.3). Delegates to
# clients/3ds/build.sh cia, which fetches the pinned makerom release
# (scripts/cia-tools.env) only if `makerom` is not already on PATH, and
# never fetches bannertool (clients/3ds/meta/banner.bnr and icon.icn are
# committed prebuilt outputs — see scripts/cia-tools.env for why). Like
# build_3ds above, this wrapper never signs anything beyond makerom's own
# `-target t` test-key signing (no secret involved, forks can run this) and
# never uploads or publishes — it is the local dev-loop check that .cia
# packaging still works, not a release path.
# ---------------------------------------------------------------------------
build_3ds_cia() {
  log "3ds-cia: delegating to clients/3ds/build.sh cia (DEVKITARM=${DEVKITARM})"
  "${REPO_ROOT}/clients/3ds/build.sh" cia

  local dsx="${REPO_ROOT}/clients/3ds/atticpad-3ds.3dsx"
  local cia="${REPO_ROOT}/clients/3ds/atticpad-3ds.cia"
  local missing=0
  for artifact in "${dsx}" "${cia}"; do
    if [[ ! -f "${artifact}" ]]; then
      warn "3ds-cia: clients/3ds/build.sh cia exited 0 but ${artifact} does not exist."
      missing=1
    fi
  done
  [[ "${missing}" -eq 0 ]] || exit 1
  log "3ds-cia: confirmed ${dsx} ($(du -h "${dsx}" | cut -f1)) and ${cia} ($(du -h "${cia}" | cut -f1))"
}

# ---------------------------------------------------------------------------
# console stubs — no client code exists yet (docs/DESIGN.md §11)
# ---------------------------------------------------------------------------
console_stub() {
  local target="$1" toolchain_key="$2" milestone="$3"
  cat >&2 <<EOF
scripts/build.sh: '${target}' is not implemented until ${milestone}.

There is no clients/${target}/ yet (docs/DESIGN.md §4, §11 roadmap) — this is not a
missing toolchain or a broken container, there is simply nothing to compile.
The pinned digest for this toolchain IS already reserved in
scripts/toolchains.env (key ${toolchain_key}), so when client code lands,
wiring this target in is: add the container invocation here using
\$${toolchain_key}, and the matching job in .github/workflows/ci.yml using
the same key. No new digest pinning needed at that point.
EOF
  exit 1
}

# ---------------------------------------------------------------------------
# vita — shelved, not merely unbuilt (docs/DESIGN.md §11). Distinct message from
# console_stub above on purpose: this is not "no client code yet", it is
# "the pinned toolchain image cannot execute at all". See
# .github/workflows/canary.yml's vitasdk-latest job, which is left failing
# on purpose (not weakened) until upstream fixes the image.
# ---------------------------------------------------------------------------
vita_shelved() {
  cat >&2 <<EOF
scripts/build.sh: 'vita' is SHELVED (docs/DESIGN.md §11), not "not implemented yet".

The pinned VITASDK image (gnuton/vitasdk-docker, key VITASDK=${VITASDK:-<unset>})
ships toolchain binaries (arm-vita-eabi-gcc and friends) linked against a
newer glibc (2.36/2.38) than its own base OS provides (Ubuntu 22.04, glibc
2.35). The compiler cannot execute inside its own container — confirmed
against :latest, the currently pinned digest, and an earlier dated tag, so
this is not a one-day fluke. There is no clients/vita/ to build even if the
toolchain worked.

This is left failing on purpose in .github/workflows/canary.yml
(vitasdk-latest job) rather than silently skipped or worked around. Revisit
when a working vitasdk-docker image exists upstream, or a different pinned
image is found that actually runs — not by adding a workaround here.
EOF
  exit 1
}

build_android() {
  # Android is the ONE target docs/DESIGN.md §8.5 builds with a native toolchain
  # rather than a pinned container ("Android | native SDK/NDK | ~30 s"), and
  # §8.1 gives its CI job `ubuntu-latest` + SDK/NDK rather than a
  # `container:`. So there is no digest in scripts/toolchains.env for it —
  # scripts/android.env pins the package versions instead, and CI and
  # scripts/android-sdk-bootstrap.sh both read that same file.
  if [[ ! -x "${HOME}/Android/jdk/bin/java" && -z "${JAVA_HOME:-}" ]]; then
    cat >&2 <<'EOF'
scripts/build.sh: the Android toolchain is not installed on this machine.

  scripts/android-sdk-bootstrap.sh

installs a pinned JDK, cmdline-tools, platform, build-tools, NDK and CMake
under ~/Android. It needs no root — this project's dev box has no passwordless
sudo and the machine is often unattended.
EOF
    exit 1
  fi
  log "android: clients/android/build.sh ${*}"
  "${REPO_ROOT}/clients/android/build.sh" "$@"
}

# ---------------------------------------------------------------------------
# dispatch
# ---------------------------------------------------------------------------
target="${1:-}"

case "${target}" in
  core)
    build_core
    ;;
  server)
    build_server
    ;;
  windows)
    build_windows
    ;;
  tools)
    build_tools
    ;;
  all)
    build_core
    build_server
    build_tools
    ;;
  3ds)
    build_3ds
    ;;
  3ds-cia)
    build_3ds_cia
    ;;
  android)
    shift || true
    build_android "$@"
    ;;
  switch)
    console_stub "switch" "DEVKITA64" "M5"
    ;;
  psp)
    console_stub "psp" "PSPDEV" "M5"
    ;;
  vita)
    vita_shelved
    ;;
  nds)
    console_stub "nds" "(BlocksDS — not yet pinned, see docs/DESIGN.md D8)" "M5"
    ;;
  ""|-h|--help|help)
    usage
    exit 0
    ;;
  *)
    echo "scripts/build.sh: unknown target '${target}'" >&2
    usage
    exit 1
    ;;
esac
