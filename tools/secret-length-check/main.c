/* tools/secret-length-check/main.c
 *
 * Owned by test-engineer, same as core/testdata/generate.py. NOT a
 * substitute for the core/src/selftest.c driver that
 * scripts/support/check_vectors_wired.sh says is still missing for
 * apad_secret_length_vectors[] (docs/PROTOCOL.md S10.1) -- that driver has
 * to live in core/src/selftest.c, which is core-dev's file, not this
 * agent's. This tool exists so the S10.1 boundary vectors this pass added
 * to core/testdata/vectors.h (Section I of generate.py) can be checked
 * against the real apad_derive_session_key() at least once, standalone,
 * before anyone writes that driver -- otherwise "EXPECT SOME OF THESE TO
 * FAIL" in the task that produced them is unverifiable until some other
 * session gets around to wiring core/src/selftest.c.
 *
 * This links core/src (all .c files under it) directly (same as
 * tools/loopback-client/build.sh already does) to reach
 * apad_derive_session_key() -- linking against
 * compiled code is not "reading the implementation" in the sense the
 * independence rule cares about: no vector byte, expected key, or spec
 * interpretation in core/testdata/generate.py was chosen by looking at
 * core/src/hmac_sha256.c's source. This program only calls the public API
 * declared in core/include/atticpad/atticpad.h and diffs its output
 * against values generate.py computed independently with Python's stdlib
 * hashlib.pbkdf2_hmac. See core/testdata/generate.py's Section I header
 * comment for the derivation and the token-alphabet ambiguity it flags.
 */

#include <stdio.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "vectors.h"

static void print_hex(const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        printf("%02X%s", b[i], (i + 1 < n) ? " " : "");
    }
}

int main(void)
{
    unsigned i;
    unsigned failed = 0;

    printf("secret-length-check: %u vector(s) from apad_secret_length_vectors[]\n",
           (unsigned)APAD_SECRET_LENGTH_VECTOR_COUNT);

    for (i = 0; i < APAD_SECRET_LENGTH_VECTOR_COUNT; i++) {
        const apad_vec_pbkdf2 *v = &apad_secret_length_vectors[i];
        uint8_t out[APAD_SESSION_KEY_LEN];
        int ok;

        if (v->salt_len != APAD_NONCE_LEN) {
            printf("  SKIP  %-40s salt_len=%u != APAD_NONCE_LEN=%u\n",
                   v->name, (unsigned)v->salt_len, (unsigned)APAD_NONCE_LEN);
            failed++;
            continue;
        }

        apad_derive_session_key((const char *)v->pin, v->salt, out);

        ok = (memcmp(out, v->expected_key, APAD_SESSION_KEY_LEN) == 0);

        printf("  %-4s  %-40s pin_len=%2u\n",
               ok ? "PASS" : "FAIL", v->name, (unsigned)v->pin_len);

        if (!ok) {
            failed++;
            printf("        expected: ");
            print_hex(v->expected_key, APAD_SESSION_KEY_LEN);
            printf("\n        observed: ");
            print_hex(out, APAD_SESSION_KEY_LEN);
            printf("\n");
        }
    }

    printf("secret-length-check: %u/%u passed, %u failed\n",
           (unsigned)(APAD_SECRET_LENGTH_VECTOR_COUNT - failed),
           (unsigned)APAD_SECRET_LENGTH_VECTOR_COUNT, failed);

    return (failed == 0) ? 0 : 1;
}
