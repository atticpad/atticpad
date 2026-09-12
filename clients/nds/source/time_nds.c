/*
 * clients/nds/source/time_nds.c -- apad_ticks_ms() for the DS / DSi.
 *
 * WHICH TIMER, AND WHY NOT THE OBVIOUS ONES.
 *
 * The DS has four hardware timers per CPU and libnds names the ones it has
 * already spoken for, in $BLOCKSDS/libs/libnds/include/nds/timers.h:
 *
 *     #ifdef ARM7
 *     #define LIBNDS_DEFAULT_TIMER_MUSIC  0   // Maxmod / LibXM7
 *     #define LIBNDS_DEFAULT_TIMER_WIFI   2   // DSWiFi
 *     #define LIBNDS_DEFAULT_TIMER_RTC    3
 *     #endif
 *     #ifdef ARM9
 *     #define LIBNDS_DEFAULT_TIMER_WIFI   3   // DSWiFi
 *     #endif
 *     #define LIBNDS_TIMER_SYSTEM_COUNTER 2
 *
 * Note that WIFI is timer 2 on the ARM7 and timer 3 on the ARM9 -- they are
 * different hardware and the number differs. This file runs on the ARM9, so
 * timer 3 is DSWiFi's and timer 2 is libnds's system counter. Timer 0 is free
 * on the ARM9 but is MUSIC on the ARM7, and a future maxmod ARM7 core would
 * make sharing that number confusing to read even though it would work.
 *
 * TIMER 1 is the one number nothing in libnds claims on either CPU.
 *
 * WHY A CALLBACK AND NOT A FREE-RUNNING COUNTER. A cascaded 32-bit counter
 * read on demand would avoid 1000 interrupts a second, but converting its
 * ~33.5 MHz ticks to milliseconds needs a division per call and
 * apad_ticks_ms() is called several times per pump. The counter approach is
 * also read-torn across the two 16-bit halves unless read twice. The SDK's own
 * example (references/nds/examples/time/timers/source/main.c) uses the
 * callback form for exactly this shape of problem, including a 1500 Hz timer,
 * so the cost is known to be affordable at 67 MHz: the handler below is an
 * increment, well under 0.5% of the CPU at 1 kHz.
 *
 * WHY ClockDivider_1. timerFreqToTicks_1(1000) is 33513982/1000 = 33514
 * ticks, which fits the 16-bit reload register (max 65536) and gives
 * 33513982/33514 = 999.9994 Hz -- 0.6 ppm. The ClockDivider_1024 form the
 * brief suggested is 32728/1000 = 32 ticks, i.e. 1022.8 Hz, which runs 2.3%
 * FAST: a 60-second session would drift 1.4 s and every rtt_ms would read
 * ~2% low. Cheap to get right, so it is got right here.
 *
 * WRAP. The counter is a uint32_t of milliseconds and wraps at 2^32 ms
 * (~49.7 days). That is the shim contract -- every comparison in the codebase
 * goes through apad_time_after()/apad_time_since() in core/src/seq.c, which
 * are wrap-safe by construction. What must NOT happen is a clock that steps
 * BACKWARDS (see clients/psp/source/time_psp.c for what that costs); a
 * monotonically incrementing counter cannot.
 */
#include <nds.h>

#include "atticpad/atticpad.h"

#include "time_nds.h"

/* volatile: written by the timer IRQ, read by the main thread and by DSWiFi's
 * lwIP cothread. A 32-bit aligned load/store is atomic on the ARM9, so no
 * critical section is needed for a single-word counter with one writer. */
static volatile uint32_t g_ms;

static void tick_isr(void)
{
    g_ms++;
}

void apad_nds_time_init(void)
{
    timerStart(APAD_NDS_TICK_TIMER, ClockDivider_1, timerFreqToTicks_1(1000),
               tick_isr);
}

uint32_t apad_ticks_ms(void)
{
    return g_ms;
}
