/* time_win32.c — monotonic milliseconds on Windows (docs/DESIGN.md §3).
 *
 * Windows' half of the split that shim/time_posix.c is Linux's half of, and
 * clients/3ds/source/time_3ds.c is the 3DS's. The clock is per-platform even
 * where the sockets are shared, which is why it is a separate file.
 *
 * The core has no time.h; every tick arrives through this one function.
 *
 * It returns a uint32_t that wraps at 2^32 (~49.7 days) BY DESIGN, not by
 * accident: the wire carries 32-bit tick fields (§5, §6.3, §6.4, §6.6) and
 * every comparison goes through apad_time_after() / apad_time_since(), which
 * are wrap-safe. Widening this to 64 bits would only hide the wrap from tests
 * while leaving it on the wire.
 */

#if defined(_WIN32)

/* GetTickCount64 is Vista and later; 0x0601 = Windows 7. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "atticpad/atticpad.h"

uint32_t apad_ticks_ms(void)
{
    /* GetTickCount64, not GetTickCount. Both are monotonic milliseconds since
     * boot, but the 32-bit GetTickCount wraps on its own schedule — at
     * whatever point uptime crosses 2^32 ms, which is not under our control
     * and is not reproducible in a test.
     *
     * The truncation below is NOT a bug and NOT a lost-precision accident.
     * Taking the low 32 bits of the 64-bit counter gives exactly the 2^32 ms
     * wrap the protocol specifies and that time_posix.c also produces, and it
     * puts the wrap where the wrap-safe helpers in core/src/seq.c already
     * handle it. Do not "fix" this by widening the return type; the wire
     * field is 32 bits regardless.
     *
     * Resolution is the system timer tick, typically 10-16 ms. That is
     * coarser than clock_gettime(CLOCK_MONOTONIC) and coarse relative to the
     * §5.9 latency budget, so a Windows host should measure latency with
     * QueryPerformanceCounter rather than with this. It is fine for the
     * protocol's timeouts and heartbeats, which are the only thing the core
     * uses ticks for. */
    return (uint32_t)(GetTickCount64() & 0xFFFFFFFFu);
}

#else  /* !_WIN32 */

/* ISO C forbids an empty translation unit; -pedantic enforces it. */
typedef int apad_time_win32_unused_on_this_platform;

#endif /* _WIN32 */
