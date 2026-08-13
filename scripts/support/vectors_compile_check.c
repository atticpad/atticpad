/* scripts/support/vectors_compile_check.c
 *
 * Owned by scripts/ (ci-engineer). Proves ONE property of
 * core/testdata/vectors.h that compiling it as part of core/src/selftest.c
 * cannot prove: that vectors.h depends on NOTHING but <stdint.h>.
 *
 * Why that's a separate claim from "selftest.c compiles cleanly": selftest.c
 * #includes "atticpad/atticpad.h" (which pulls in input.h, protocol.h, ...)
 * BEFORE it #includes "../testdata/vectors.h". If vectors.h ever picked up
 * an accidental dependency on a project type -- say it started spelling a
 * field as `apad_input_state` instead of the plain `int16_t axes[8]` its own
 * header comment promises -- that would compile perfectly inside selftest.c,
 * because atticpad.h is already in scope there, and FAIL everywhere else
 * vectors.h gets compiled alone: every blind-platform client's on-device
 * self-test screen, none of which necessarily pull in the same headers in
 * the same order. Same compiler flags as selftest.c's build proves nothing
 * about this -- it's the include *context* that differs, not the flags.
 *
 * So this translation unit deliberately includes vectors.h ALONE, with an
 * include path (-Icore/testdata, see scripts/build.sh and
 * .github/workflows/ci.yml) that does NOT contain core/include -- so a
 * reference to any atticpad.h/input.h/protocol.h symbol fails to compile
 * here even though it would silently succeed inside selftest.c's own TU.
 * self-containedness is load-bearing (vectors.h's own header comment: "safe
 * to compile into a 4 MB, no-MMU target"), not a style preference, so this
 * check stays even though core/src/selftest.c compiles vectors.h too.
 */

#include "vectors.h"

int main(void)
{
    /* Touch a symbol from each generated section so an unused-but-present
     * table can't silently rot into something that fails to parse instead
     * of failing to link. Values aren't asserted -- this is a syntax/type
     * and dependency-isolation check, not a semantic one. */
    (void)apad_vec_hat_lut_ref;
    (void)apad_frame_vectors;
    (void)apad_truncation_vectors;
    (void)apad_auth_truncation_vectors;
    (void)apad_input_state_vectors;
    (void)apad_seq_newer_vectors;
    (void)apad_time_after_vectors;
    (void)apad_pbkdf2_vectors;
    (void)apad_auth_tag_vectors;
    return 0;
}
