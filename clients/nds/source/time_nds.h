/*
 * clients/nds/source/time_nds.h -- the millisecond tick behind apad_ticks_ms().
 *
 * See time_nds.c for why timer 1 and why ClockDivider_1.
 */
#ifndef ATTICPAD_NDS_TIME_H
#define ATTICPAD_NDS_TIME_H

/* ARM9 hardware timer used for the 1 kHz tick. 3 is DSWiFi's and 2 is
 * libnds's system counter (nds/timers.h); 1 is claimed by nothing. */
#define APAD_NDS_TICK_TIMER 1

/* Start the tick. Call ONCE, before anything asks for apad_ticks_ms() --
 * which means before Wifi_InitDefault() and before apad_client_create(). */
void apad_nds_time_init(void);

#endif /* ATTICPAD_NDS_TIME_H */
