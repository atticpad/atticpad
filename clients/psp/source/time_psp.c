/*
 * clients/psp/source/time_psp.c -- apad_ticks_ms() for the PSP.
 *
 * WHY THIS FILE EXISTS, when shim/time_posix.c would have compiled:
 *
 * pspdev's libcglue DOES provide clock_gettime(), so time_posix.c links
 * cleanly on this target -- and is silently wrong. Disassembling
 * libcglue.a(clock_gettime.o) in the pinned image shows its second
 * instruction is `move a0,sp`, which overwrites a0 -- the register holding
 * the clockid argument in the MIPS o32 ABI. The clockid is discarded and the
 * call returns wall-clock time from the RTC, so asking for CLOCK_MONOTONIC
 * gets you a clock that steps backwards whenever the RTC is adjusted or the
 * user changes the system time.
 *
 * That is precisely the failure core/src/seq.c's wrap-safe helpers exist to
 * prevent: apad_time_after() assumes a monotonically increasing counter, and
 * one backwards step wedges a session's timers until the app restarts. A
 * clock that is right in the emulator and wrong on a console that has had its
 * date set is the worst shape a bug can have on a platform nobody can test.
 *
 * sceKernelGetSystemTimeWide() is microseconds since boot as a 64-bit value:
 * genuinely monotonic, unaffected by the RTC.
 *
 * NOT sceKernelGetSystemTimeLow(): that is the low 32 bits of the same
 * MICROSECOND counter, so a millisecond value derived from it jumps backwards
 * every ~71 minutes (2^32 us). The wrap this file is allowed to have is the
 * 2^32 MILLISECOND one (~49.7 days) that apad_time_after() is built for, and
 * the Wide form is what gives it.
 */
#include <pspkernel.h>
#include <pspthreadman.h>

#include "atticpad/atticpad.h"

uint32_t apad_ticks_ms(void)
{
    /* Truncating to 32 bits is deliberate: every comparison in the codebase
     * goes through apad_time_after()/apad_time_since(), which are wrap-safe
     * by construction (core/src/seq.c). A wider type here would not help and
     * would diverge from the shim contract in core/include/atticpad. */
    return (uint32_t)((uint64_t)sceKernelGetSystemTimeWide() / 1000ull);
}
