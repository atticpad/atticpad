/* scripts/support/core_selftest_main.c
 *
 * Owned by scripts/ (ci-engineer), NOT by core/. This is the local/CI driver
 * for core/src/selftest.c's apad_selftest_run() -- core/ has no main() of
 * its own (docs/CONVENTIONS.md: no stdio in core/), so something outside core/ has to
 * link libapad, call the entry point, and turn the result into a process
 * exit code. This file is that "something".
 *
 * As of core/src/selftest.c commit d22eabb, apad_selftest_run() runs BOTH
 * halves unconditionally, no build flag: the codec's own internal invariant
 * checks, AND all 157 independently-authored conformance vectors from
 * core/testdata/vectors.h (included directly by selftest.c). There used to
 * be a real gap here -- an extern apad_vectors_run() that nothing defined,
 * so this binary only ran the weaker internal-only half. That gap is
 * closed; do not reintroduce a "vectors not exercised" caveat here without
 * checking core/src/selftest.c first. The printed total below is whatever
 * the linked-in suite reports -- deliberately not hardcoded anywhere in
 * scripts/, since core/src/ and core/testdata/ both add cases over time.
 */

#include <stdio.h>

#include "atticpad/atticpad.h"

static void on_case(void *user, const char *name, int passed)
{
    (void)user;
    if (!passed) {
        fprintf(stderr, "  FAIL  %s\n", name);
    }
}

int main(void)
{
    apad_selftest_result r = {0, 0, 0, NULL};
    int rc;

    rc = apad_selftest_run(&r, on_case, NULL);

    printf("apad_selftest_run: %u/%u passed, %u failed\n",
           (unsigned)r.passed, (unsigned)r.total, (unsigned)r.failed);

    if (rc != APAD_OK) {
        fprintf(stderr, "apad_selftest_run: FAILED, first failure: %s\n",
                r.first_failure != NULL ? r.first_failure : "(unknown)");
        return 1;
    }

    return 0;
}
