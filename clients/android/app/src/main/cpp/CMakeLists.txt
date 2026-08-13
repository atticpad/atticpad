# clients/android/app/src/main/cpp/CMakeLists.txt
#
# Builds libapad (core/ + shim/) and the JNI boundary into one .so.
#
# The sources are referenced OUT OF THE REPO, not vendored into the Android
# project. clients/3ds does the same thing. A copy of codec.c under clients/
# would be a second codec that the conformance vectors do not protect, which
# is the entire failure mode docs/DESIGN.md §9.1 exists to prevent.
cmake_minimum_required(VERSION 3.22)
project(atticpad_android C)

get_filename_component(APAD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

if(NOT EXISTS "${APAD_ROOT}/core/include/atticpad/atticpad.h")
    message(FATAL_ERROR
        "Cannot find libapad at ${APAD_ROOT}. This Gradle project only builds "
        "from inside a full atticpad checkout — it is not standalone.")
endif()

file(GLOB APAD_CORE_SRC "${APAD_ROOT}/core/src/*.c")

if(NOT EXISTS "${APAD_ROOT}/clients/vendor/quirc/lib/quirc.h")
    message(FATAL_ERROR
        "Cannot find vendored quirc at ${APAD_ROOT}/clients/vendor/quirc. "
        "See clients/vendor/quirc/README.md.")
endif()

# quirc (docs/PROTOCOL.md §10.3 QR scanning) is THIRD-PARTY vendored source,
# not ours, and clients/vendor/quirc/README.md's "Build notes" section
# documents exactly why it gets its OWN target with its OWN (permissive)
# flags rather than joining apadjni's list: lib/*.c does not compile clean
# under -Wall -Wextra -Werror -pedantic (confirmed: -Wsign-compare twice,
# -Wunused-parameter once, all pre-existing upstream code), and "not one byte
# of vendored source has been modified" rules out patching it quiet. An
# OBJECT library is the CMake analogue of server/backends/vendor's -isystem
# trick: it silences warnings on code we do not own without weakening
# -Werror on a single line apadjni does.
add_library(quirc OBJECT
    "${APAD_ROOT}/clients/vendor/quirc/lib/quirc.c"
    "${APAD_ROOT}/clients/vendor/quirc/lib/identify.c"
    "${APAD_ROOT}/clients/vendor/quirc/lib/decode.c"
    "${APAD_ROOT}/clients/vendor/quirc/lib/version_db.c"
)
target_compile_options(quirc PRIVATE -std=c99)

add_library(apadjni SHARED
    ${APAD_CORE_SRC}
    # shim/: the SAME BSD-sockets shim the Linux server and the 3DS client
    # use. docs/DESIGN.md §7.2 — "Route sockets through shim/, not Kotlin." Bionic is
    # close enough to POSIX that net_bsd.c needs no Android #ifdef, which is
    # the load-bearing assumption in docs/DESIGN.md §3 paying off.
    "${APAD_ROOT}/shim/net_bsd.c"
    "${APAD_ROOT}/shim/time_posix.c"
    # clients/common/: the shared client session engine (hoisted out of this
    # app 2026-08-11 so the 3DS client and loopback-client drive the same §8/§9
    # logic instead of re-implementing it — same rationale as core/ above).
    "${APAD_ROOT}/clients/common/apad_client.c"
    "${APAD_ROOT}/clients/common/apad_qr.c"
    # clients/common/apad_ui*: the shared customer-copy catalog. apad_jni.c
    # only calls apad_ui_msg()/apad_ui_msg_count() (apad_ui_strings.c) today —
    # apad_ui.c's precedence function is re-derived by hand in Kotlin instead,
    # since it takes an apad_client_stats* and the session lives in C via
    # apad_client, not through a struct Kotlin holds — but it is built in here
    # too rather than left half-adopted, so a future caller does not have to
    # touch CMakeLists.txt to reach it.
    "${APAD_ROOT}/clients/common/apad_ui.c"
    "${APAD_ROOT}/clients/common/apad_ui_strings.c"
    apad_jni.c
    $<TARGET_OBJECTS:quirc>
)

target_include_directories(apadjni PRIVATE
    "${APAD_ROOT}/core/include"
    # core/src/selftest.c includes "vectors.h" from core/testdata/.
    "${APAD_ROOT}/core/testdata"
    "${APAD_ROOT}/clients/common"
)
target_include_directories(quirc PRIVATE
    "${APAD_ROOT}/clients/vendor/quirc/lib"
)
# apad_qr.c's `#include "quirc.h"` — the only file in this app that reaches
# into the vendored tree, per apad_qr.c's own header comment. -isystem, not
# -I: it is a third-party header, not one this project owns, and this keeps
# whatever it drags in off apadjni's own -Werror.
target_include_directories(apadjni SYSTEM PRIVATE
    "${APAD_ROOT}/clients/vendor/quirc/lib"
)

# Same standard and same warning gate scripts/build.sh applies to the native
# core build. A warning that only appears under the NDK's clang is exactly the
# kind of thing worth failing on, since this is an ARM target and the vectors
# were generated on x86.
target_compile_options(apadjni PRIVATE
    -std=c99 -Wall -Wextra -Werror -pedantic
)

# log for __android_log_print; the codec itself never logs (no stdio in core/).
# m: quirc's lib/identify.c uses <math.h> (sqrt, atan2, floor) unconditionally
# — see clients/vendor/quirc/README.md's build notes.
find_library(ANDROID_LOG_LIB log)
find_library(ANDROID_MATH_LIB m)
target_link_libraries(apadjni ${ANDROID_LOG_LIB} ${ANDROID_MATH_LIB})
