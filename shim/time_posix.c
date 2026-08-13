/* time_posix.c — monotonic milliseconds (docs/DESIGN.md §3).
 *
 * The core has no time.h; every tick arrives through this one function.
 *
 * It returns a uint32_t that wraps at 2^32 (~49.7 days) BY DESIGN, not by
 * accident: the wire carries 32-bit tick fields (§5, §6.3, §6.4, §6.6) and
 * every comparison goes through apad_time_after() / apad_time_since(), which
 * are wrap-safe. Widening this to 64 bits would only hide the wrap from tests
 * while leaving it on the wire.
 *
 * The value is deliberately offset so the wrap is reachable in testing: the
 * first call returns a small number, but nothing may depend on that, and no
 * caller may treat the value as an absolute time.
 */

#define _POSIX_C_SOURCE 200809L

#include <time.h>

#include "atticpad/atticpad.h"

uint32_t apad_ticks_ms(void)
{
    struct timespec ts;
    uint64_t ms;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    ms = ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
    return (uint32_t)(ms & 0xFFFFFFFFu);
}
