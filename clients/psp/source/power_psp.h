/*
 * clients/psp/source/power_psp.h -- suspend / resume, seen from the frame loop.
 *
 * The console's power switch tears the WLAN and the network libraries down
 * underneath a running app. Without a power callback the client never learns
 * that happened: it keeps a socket that no longer works, and re-running
 * sceNetApctlConnect() against the pre-suspend state does not bring the radio
 * back ("rejoining wifi doesn't work after a power switch", hardware QA
 * 2026-09-15).
 *
 * The callback itself does nothing but count events -- the work happens on
 * the main thread, where the engine and the screens already live, because
 * every apad_client_* call has to come from one thread (apad_client.h) and a
 * kernel callback runs on somebody else's.
 */
#ifndef ATTICPAD_PSP_POWER_H
#define ATTICPAD_PSP_POWER_H

typedef struct {
    int suspended;   /* the console suspended (power switch, or idle)     */
    int resumed;     /* a resume finished: the network must be rebuilt    */
} apad_psp_power_event;

/* Register the power callback. MUST be called from a thread that then waits
 * in sceKernelSleepThreadCB() -- pspsdk's power sample registers the exit and
 * power callbacks on the same such thread, and so does main.c. */
void apad_psp_power_register(void);

/* Consume what the callback has seen since the last call. Returns 1 if
 * either field is set. Call once per frame from the main thread; it also
 * drives the RESUME_COMPLETE fallback timer, so it must be called every
 * frame and not only when something is expected. */
int  apad_psp_power_poll(apad_psp_power_event *out);

/* 1 while a resume has been signalled and no RESUME_COMPLETE has arrived
 * yet; diagnostic only. */
int  apad_psp_power_awaiting_resume(void);

#ifdef APAD_PSP_FAKE_RESUME
/* DEV ONLY (`make APAD_PSP_FAKE_RESUME=<seconds>`). Raises the same counters
 * the kernel callback raises, so everything downstream -- the socket
 * teardown, the network restart, the rejoin, the banner -- runs exactly as
 * it would after a real power switch. It exists because no emulator can
 * suspend a PSP: without it the resume path could only ever be reviewed, not
 * run. It does NOT test callback DELIVERY, which remains the one piece only
 * hardware can confirm. */
void apad_psp_power_inject(void);
#endif

#endif /* ATTICPAD_PSP_POWER_H */
