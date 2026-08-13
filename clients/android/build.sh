#!/usr/bin/env bash
# clients/android/build.sh
#
# Local build wrapper for the AtticPad Android client, following the pattern
# clients/3ds/build.sh established: a helper local to this directory, invoked
# by scripts/build.sh's `android` target.
#
# Android is the one platform docs/DESIGN.md §8.5 builds NATIVELY rather than in a
# pinned container ("Android | native SDK/NDK | ~30 s | Yes — adb install"),
# and §8.1 gives its CI job `ubuntu-latest` + SDK/NDK rather than a
# `container:`. So there is no image digest to match; what keeps local and CI
# equivalent instead is scripts/android.env, which both read.
#
# Usage:
#   clients/android/build.sh                 # assembleDebug
#   clients/android/build.sh install <ser>   # build, then adb install -r
#   clients/android/build.sh selftest <ser>  # build, install, run the §13
#                                            # self-test on the device and
#                                            # print the result
#   clients/android/build.sh clean
#
# This script never produces a release artifact (docs/DESIGN.md §8.5). assembleDebug
# signs with the local debug key; a real release comes from tagged CI only.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"

log()  { printf '== clients/android/build.sh: %s ==\n' "$*"; }
die()  { printf 'clients/android/build.sh: FATAL: %s\n' "$*" >&2; exit 1; }

# shellcheck source=../../scripts/android.env
source "${REPO_ROOT}/scripts/android.env"

export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${HOME}/Android/sdk}"
export ANDROID_HOME="${ANDROID_SDK_ROOT}"
export JAVA_HOME="${JAVA_HOME:-${APAD_JDK_HOME:-${HOME}/Android/jdk}}"

if [[ ! -x "${JAVA_HOME}/bin/java" ]] || [[ ! -d "${ANDROID_SDK_ROOT}/platforms" ]]; then
  die "Android toolchain not installed. Run: scripts/android-sdk-bootstrap.sh"
fi

# Drift check. gradle/libs.versions.toml is what Gradle reads;
# scripts/android.env is what CI and the bootstrap script read. Two files
# naming the same version is how they end up naming different ones.
toml_agp="$(sed -n 's/^agp *= *"\(.*\)"$/\1/p' "${HERE}/gradle/libs.versions.toml")"
[[ "${toml_agp}" == "${APAD_AGP_VERSION}" ]] || die \
  "AGP drift: gradle/libs.versions.toml says ${toml_agp}, scripts/android.env says ${APAD_AGP_VERSION}"

gradle_dist="$(sed -n 's/.*gradle-\([0-9.]*\)-bin\.zip.*/\1/p' "${HERE}/gradle/wrapper/gradle-wrapper.properties")"
[[ "${gradle_dist}" == "${APAD_GRADLE_VERSION}" ]] || die \
  "Gradle drift: wrapper says ${gradle_dist}, scripts/android.env says ${APAD_GRADLE_VERSION}"

ACTION="${1:-build}"
SERIAL="${2:-}"
ADB="${ANDROID_SDK_ROOT}/platform-tools/adb"
APK="${HERE}/app/build/outputs/apk/debug/app-debug.apk"
PKG="net.atticpad"

adb_() {
  if [[ -n "${SERIAL}" ]]; then "${ADB}" -s "${SERIAL}" "$@"; else "${ADB}" "$@"; fi
}

do_build() {
  log "assembleDebug (AGP ${APAD_AGP_VERSION}, NDK ${APAD_NDK_VERSION}, JDK ${APAD_JDK_BUILD})"
  ( cd "${HERE}" && ./gradlew --console=plain assembleDebug )
  [[ -f "${APK}" ]] || die "gradle reported success but ${APK} does not exist"
  log "built $(du -h "${APK}" | cut -f1) -> ${APK#"${REPO_ROOT}/"}"
}

case "${ACTION}" in
  clean)
    ( cd "${HERE}" && ./gradlew --console=plain clean )
    ;;

  build)
    do_build
    ;;

  install)
    do_build
    log "adb install -r"
    adb_ install -r "${APK}"
    ;;

  selftest)
    # "It compiles" is not done (docs/CONVENTIONS.md). This runs the §13 conformance
    # vectors on the device's own ARM build of the codec and prints the
    # result, so a build can be reported as RUN rather than merely built.
    do_build
    adb_ install -r "${APK}" >/dev/null
    log "running apad_selftest_run() on the device"
    adb_ shell am force-stop "${PKG}" || true
    adb_ logcat -c || true
    adb_ shell am start -n "${PKG}/.MainActivity" --ez selftest true >/dev/null
    # POLL, do not sleep-and-hope. A freshly installed app can take well over
    # four seconds to reach onCreate on a cold emulator, and a fixed sleep
    # turns that into "the self-test did not run" — the same mistake made
    # repeatedly with udev ACLs on /dev/input.
    result=""
    for _ in $(seq 1 30); do
      result="$(adb_ logcat -d -s AtticPadSelfTest:I 2>/dev/null | grep -o 'abi=.*' | tail -1 || true)"
      [[ -n "${result}" ]] && break
      sleep 1
    done
    [[ -n "${result}" ]] || die "no self-test result in logcat after 30 s -- did the app start?"
    printf '%s\n' "${result}"
    grep -q 'failed=0' <<<"${result}" || die "SELF-TEST FAILED on device"
    log "self-test PASSED on device"
    ;;

  *)
    die "unknown action '${ACTION}' (build | install | selftest | clean)"
    ;;
esac
