/* seq.c — wrap-safe sequence and tick arithmetic (docs/PROTOCOL.md §9).
 *
 * This is the smallest file in the core and the one most likely to sink the
 * project if it is wrong. A 16-bit sequence at 125 Hz wraps every ~8.7
 * minutes; a naive `a > b` passes every test shorter than nine minutes and
 * then freezes the client forever, on six platforms nobody can test.
 *
 * The spec writes these as `(int16_t)(a - b) > 0`. That is correct on every
 * machine this will ever run on, but conversion of an out-of-range value to a
 * signed type is implementation-defined in C99 (6.3.1.3p3), so the bodies
 * below stay entirely in unsigned arithmetic. The results are identical for
 * all 2^32 input pairs on a two's-complement target; the unsigned form is
 * simply not at the mercy of the compiler.
 */

#include "atticpad/atticpad.h"

#define APAD_SEQ_HALF   0x8000u
#define APAD_TIME_HALF  0x80000000u

int apad_seq_newer(uint16_t a, uint16_t b)
{
    uint16_t d = (uint16_t)((unsigned)a - (unsigned)b);
    /* d == 0 -> equal, not newer. d in 1..0x7FFF -> a is ahead.
     * d >= 0x8000 -> b is ahead (or the distance is ambiguous, in which case
     * the spec's rule is "not newer"). */
    return (d != 0u && d < APAD_SEQ_HALF) ? 1 : 0;
}

int apad_seq_diff(uint16_t a, uint16_t b)
{
    uint16_t d = (uint16_t)((unsigned)a - (unsigned)b);
    if (d < APAD_SEQ_HALF) {
        return (int)d;                        /* 0 .. 32767 */
    }
    return -(int)(uint16_t)(0x10000u - d);    /* -32768 .. -1 */
}

/*
 * §6.20 step 2 — how many trailing slots of an event ring to replay.
 *
 * Implemented ON TOP OF apad_seq_diff and nowhere else: §9 requires exactly
 * one copy of the wrap arithmetic in the whole core, so this function
 * contains no subtraction of its own and `nm` can still prove there is one
 * implementation.
 *
 * IT CANNOT FAIL, AND THAT IS THE DESIGN. §6.20 step 3 makes the caller
 * reconcile held state against the datagram's snapshot UNCONDITIONALLY --
 * including when the gap is zero, and including on overflow -- because that
 * is what limits a divergence to one packet instead of forever. Returning
 * APAD_ERR_STALE for "nothing new" would invite `if (n < 0) return;`, which
 * skips the reconcile and reintroduces exactly the permanent-divergence bug
 * step 3 exists to prevent. So: a gap of zero or less is 0, "replay nothing",
 * and the caller carries on to the reconcile.
 */
int apad_event_ring_new(uint16_t event_seq, uint16_t last_seq, int ring_len,
                        int *out_overflow)
{
    int gap;

    if (out_overflow != NULL) {
        *out_overflow = 0;
    }
    if (ring_len <= 0) {
        return 0;   /* a ring with no slots can replay nothing */
    }

    gap = apad_seq_diff(event_seq, last_seq);
    if (gap <= 0) {
        /* Nothing new, or a duplicate, or a reordered ring. §6.20 step 2. */
        return 0;
    }
    if (gap > ring_len) {
        /* Events were lost that the ring cannot carry. Replay everything it
         * does hold, and SURFACE the overflow rather than swallowing it. */
        if (out_overflow != NULL) {
            *out_overflow = 1;
        }
        return ring_len;
    }
    return gap;
}

uint16_t apad_seq_next(uint16_t s)
{
    return (uint16_t)((unsigned)s + 1u);
}

int apad_time_after(uint32_t a, uint32_t b)
{
    uint32_t d = a - b;
    return (d != 0u && d < APAD_TIME_HALF) ? 1 : 0;
}

uint32_t apad_time_since(uint32_t now, uint32_t then)
{
    return now - then;   /* unsigned wrap is defined; this is the whole trick */
}

int apad_time_reached(uint32_t now, uint32_t deadline)
{
    /* now >= deadline, wrap-safe: true unless the deadline is still ahead. */
    return (now == deadline || apad_time_after(now, deadline)) ? 1 : 0;
}
