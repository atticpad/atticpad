#!/usr/bin/env bash
# scripts/android-sdk-bootstrap.sh — install the Android toolchain for a
# machine that has none, without root.
#
# Android is the one target docs/DESIGN.md §8.5 schedules as a NATIVE local build
# rather than a pinned container ("Android | native SDK/NDK | ~30 s | Yes —
# adb install"), and §8.1 gives its CI job `ubuntu-latest` + SDK/NDK rather
# than a `container:`. So there is no digest to pin here the way
# scripts/toolchains.env pins devkitARM. What replaces it is
# scripts/android.env: exact package versions, sourced by BOTH this script
# and CI, so local and CI resolve the same SDK, build-tools and NDK.
#
# Everything lands under $ANDROID_SDK_ROOT (default ~/Android/sdk) and
# $APAD_JDK_HOME (default ~/Android/jdk). Nothing is installed system-wide
# and no step needs sudo — the dev box for this project has no passwordless
# sudo and the user is frequently away from it.
#
# Usage:  scripts/android-sdk-bootstrap.sh          # install what is missing
#         scripts/android-sdk-bootstrap.sh --check  # report only, exit 1 if incomplete
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"

# shellcheck source=android.env
source "${HERE}/android.env"

ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${HOME}/Android/sdk}"
APAD_JDK_HOME="${APAD_JDK_HOME:-${HOME}/Android/jdk}"

log()  { printf '== android-sdk-bootstrap: %s ==\n' "$*"; }
warn() { printf 'android-sdk-bootstrap: WARNING: %s\n' "$*" >&2; }
die()  { printf 'android-sdk-bootstrap: FATAL: %s\n' "$*" >&2; exit 1; }

CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

have_jdk()  { [[ -x "${APAD_JDK_HOME}/bin/javac" ]]; }
have_sdkm() { [[ -x "${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager" ]]; }
have_ndk()  { [[ -d "${ANDROID_SDK_ROOT}/ndk/${APAD_NDK_VERSION}" ]]; }
have_plat() { [[ -d "${ANDROID_SDK_ROOT}/platforms/android-${APAD_COMPILE_SDK}" ]]; }
have_bt()   { [[ -d "${ANDROID_SDK_ROOT}/build-tools/${APAD_BUILD_TOOLS}" ]]; }

if [[ ${CHECK_ONLY} -eq 1 ]]; then
  rc=0
  for f in have_jdk have_sdkm have_plat have_bt have_ndk; do
    if $f; then printf '  ok      %s\n' "$f"; else printf '  MISSING %s\n' "$f"; rc=1; fi
  done
  printf 'ANDROID_SDK_ROOT=%s\nAPAD_JDK_HOME=%s\n' "${ANDROID_SDK_ROOT}" "${APAD_JDK_HOME}"
  exit $rc
fi

mkdir -p "${ANDROID_SDK_ROOT}" "${APAD_JDK_HOME%/*}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# ---- JDK ---------------------------------------------------------------
# AGP 8.x requires a JDK 17 or newer toolchain to RUN Gradle. This is the
# JVM Gradle itself runs on; the Java/Kotlin bytecode target is set in
# build.gradle.kts and is a separate knob.
if have_jdk; then
  log "JDK already present: $("${APAD_JDK_HOME}/bin/java" -version 2>&1 | head -1)"
else
  log "downloading Temurin JDK ${APAD_JDK_FEATURE} (${APAD_JDK_BUILD})"
  curl -fSL --retry 3 -o "${TMP}/jdk.tar.gz" "${APAD_JDK_URL}"
  echo "${APAD_JDK_SHA256}  ${TMP}/jdk.tar.gz" | sha256sum -c - \
    || die "JDK checksum mismatch — refusing to install"
  mkdir -p "${APAD_JDK_HOME}"
  tar -xzf "${TMP}/jdk.tar.gz" -C "${APAD_JDK_HOME}" --strip-components=1
  have_jdk || die "JDK unpacked but ${APAD_JDK_HOME}/bin/javac is missing"
  log "JDK installed: $("${APAD_JDK_HOME}/bin/java" -version 2>&1 | head -1)"
fi

export JAVA_HOME="${APAD_JDK_HOME}"
export PATH="${JAVA_HOME}/bin:${PATH}"

# ---- command-line tools ------------------------------------------------
if have_sdkm; then
  log "cmdline-tools already present"
else
  log "downloading Android cmdline-tools ${APAD_CMDLINE_TOOLS_VERSION} (build ${APAD_CMDLINE_TOOLS_BUILD})"
  curl -fSL --retry 3 -o "${TMP}/cmdline-tools.zip" \
    "https://dl.google.com/android/repository/commandlinetools-linux-${APAD_CMDLINE_TOOLS_BUILD}_latest.zip"
  echo "${APAD_CMDLINE_TOOLS_SHA1}  ${TMP}/cmdline-tools.zip" | sha1sum -c - \
    || die "cmdline-tools checksum mismatch — refusing to install"
  rm -rf "${TMP}/unz"; mkdir -p "${TMP}/unz"
  unzip -q "${TMP}/cmdline-tools.zip" -d "${TMP}/unz"
  # The zip contains cmdline-tools/; sdkmanager insists on living at
  # cmdline-tools/latest/ or it cannot locate the SDK root.
  mkdir -p "${ANDROID_SDK_ROOT}/cmdline-tools"
  rm -rf "${ANDROID_SDK_ROOT}/cmdline-tools/latest"
  mv "${TMP}/unz/cmdline-tools" "${ANDROID_SDK_ROOT}/cmdline-tools/latest"
  have_sdkm || die "cmdline-tools unpacked but sdkmanager is missing"
fi

SDKMANAGER="${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager"

# ---- SDK packages ------------------------------------------------------
# `yes |` accepts the licences non-interactively. That is the same thing
# android-actions/setup-android does in CI; there is no other way to accept
# them from a script and the user is not at this machine to click through.
PKGS=(
  "platform-tools"
  "platforms;android-${APAD_COMPILE_SDK}"
  "build-tools;${APAD_BUILD_TOOLS}"
  "ndk;${APAD_NDK_VERSION}"
  "cmake;${APAD_CMAKE_VERSION}"
)
log "installing: ${PKGS[*]}"
# Licence prompts are answered from a finite here-doc, NOT `yes |`. Under
# `set -o pipefail` the infinite `yes` is killed by SIGPIPE the moment
# sdkmanager stops reading, and the pipeline then reports 141 for a install
# that fully succeeded. One "y" per package plus slack is enough.
LICENCE_YES="$(printf 'y\n%.0s' $(seq 1 40))"
sdk_rc=0
printf '%s' "${LICENCE_YES}" \
  | "${SDKMANAGER}" --sdk_root="${ANDROID_SDK_ROOT}" "${PKGS[@]}" \
    >"${TMP}/sdkmanager.log" 2>&1 || sdk_rc=$?
if [[ ${sdk_rc} -ne 0 ]]; then
  tail -40 "${TMP}/sdkmanager.log" >&2
  die "sdkmanager exited ${sdk_rc} — full log above"
fi

for f in have_plat have_bt have_ndk; do
  $f || die "${f} still missing after sdkmanager reported success"
done

log "done. Add to your shell, or let clients/android/build.sh export them:"
printf '  export ANDROID_SDK_ROOT=%s\n  export JAVA_HOME=%s\n' \
  "${ANDROID_SDK_ROOT}" "${APAD_JDK_HOME}"
