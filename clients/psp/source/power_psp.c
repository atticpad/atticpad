/*
 * clients/psp/source/power_psp.c -- the power callback.
 *
 * MIRRORED, not remembered. The shape below -- sceKernelCreateCallback() +
 * scePowerRegisterCallback(0, cbid) on the same callback thread that
 * registers the exit callback, a callback whose signature is the KERNEL
 * callback signature `int (int unknown, int pwrflags, void *common)` and not
 * psppower.h's powerCallback_t, and the flag ladder
 * POWER_SWITCH|SUSPENDING -> RESUMING -> RESUME_COMPLETE -> STANDBY -- is
 * pspsdk's own samples/power/main.c, vendored at references/psp/power/main.c.
 * Where a sample and a recollection disagree, the sample wins (docs/CONVENTIONS.md).
 *
 * TWO DELIBERATE DIVERGENCES FROM THE SAMPLE, both noted here because a
 * later reader will otherwise "fix" them back:
 *
 * 1. The sample's callback ends with sceDisplayWaitVblankStart(). That is
 *    how it paces its own debug print; it is not part of the callback
 *    contract, and blocking a kernel callback for up to 16 ms while the
 *    console is trying to suspend is the last thing this one should do.
 *    Ours sets counters and returns.
 *
 * 2. The sample only prints. This one has to hand the event to the frame
 *    loop, and psppower.h's own comment on the flag it would naturally wait
 *    for is a warning:
 *
 *      /-*indicates the resume process has been completed (only seems to be
 *         triggered when another event happens)*-/
 *      #define PSP_POWER_CB_RESUME_COMPLETE 0x00040000
 *
 *    "only seems to be triggered when another event happens" means a client
 *    that waits for RESUME_COMPLETE and nothing else can sit with a dead
 *    radio indefinitely on a console where nothing else happens. So
 *    PSP_POWER_CB_RESUMING arms a fallback: if RESUME_COMPLETE has not
 *    arrived RESUME_FALLBACK_FRAMES later, the resume is treated as complete
 *    anyway. Re-running the bring-up a little early costs one failed join
 *    and a retry; never running it costs the session.
 *
 * THREADING. The callback runs on the callback thread (priority 0x11, above
 * main's 0x30) and writes nothing but three volatile counters. The frame
 * loop reads them. Single core, word-sized, one writer per counter, so no
 * lock -- the same rule net_psp.c's g_st publishes under.
 */
#include <pspkernel.h>
#include <psppower.h>

#include "devlog.h"
#include "power_psp.h"

/* ~2 s at 60 Hz. Long enough that a RESUME_COMPLETE which is merely late
 * still wins the race and the fallback never fires; short enough that a
 * console whose RESUME_COMPLETE never comes is back on the network before
 * anybody reaches for the power switch again. */
#define RESUME_FALLBACK_FRAMES 120

static volatile unsigned g_suspend_seq;    /* POWER_SWITCH | SUSPENDING  */
static volatile unsigned g_resuming_seq;   /* RESUMING                   */
static volatile unsigned g_complete_seq;   /* RESUME_COMPLETE            */

static unsigned s_suspend_seen;
static unsigned s_resuming_seen;
static unsigned s_complete_seen;
static int      s_awaiting;       /* a resume is expected/under way      */
static int      s_await_frames;

static int power_cb(int unknown, int pwrflags, void *common)
{
    (void)unknown; (void)common;

    /* The sample's ladder, in the sample's order. The two suspend flags are
     * one case there ("one is manual and the other automatic") and one case
     * here: the consequence for a UDP session is identical. */
    if ((pwrflags & PSP_POWER_CB_POWER_SWITCH) != 0
        || (pwrflags & PSP_POWER_CB_SUSPENDING) != 0) {
        g_suspend_seq++;
    } else if ((pwrflags & PSP_POWER_CB_RESUMING) != 0) {
        g_resuming_seq++;
    } else if ((pwrflags & PSP_POWER_CB_RESUME_COMPLETE) != 0) {
        g_complete_seq++;
    }
    /* STANDBY and the battery/AC/hold status bits are ignored: nothing here
     * changes what the network has to do. */
    return 0;
}

void apad_psp_power_register(void)
{
    SceUID cbid = sceKernelCreateCallback("apad_power", power_cb, NULL);

    if (cbid >= 0) {
        /* Slot 0, as the sample registers it. -1 would auto-assign; the
         * sample does not, and this client registers exactly one. */
        int rc = scePowerRegisterCallback(0, cbid);
        (void)rc;   /* apad_devlog() compiles away in a shipping build */
        apad_devlog("power callback registered: cbid=0x%08X rc=0x%08X",
                    (unsigned)cbid, (unsigned)rc);
    } else {
        apad_devlog("power callback NOT registered: sceKernelCreateCallback"
                    " returned 0x%08X", (unsigned)cbid);
    }
}

int apad_psp_power_awaiting_resume(void)
{
    return s_awaiting;
}

#ifdef APAD_PSP_FAKE_RESUME
void apad_psp_power_inject(void)
{
    /* A real suspend delivers the suspend flags and then, after the console
     * has been away, RESUME_COMPLETE -- and the frame loop, frozen in
     * between, sees both in one frame. That is what this reproduces. */
    g_suspend_seq++;
    g_complete_seq++;
}
#endif

int apad_psp_power_poll(apad_psp_power_event *out)
{
    unsigned suspend = g_suspend_seq;
    unsigned resuming = g_resuming_seq;
    unsigned complete = g_complete_seq;

    out->suspended = 0;
    out->resumed   = 0;

    if (suspend != s_suspend_seen) {
        s_suspend_seen = suspend;
        out->suspended = 1;
        /* A suspend is always followed by a resume, and the frame loop is
         * frozen in between -- so in practice both arrive in the same frame
         * once the console wakes. Arming here is what makes the fallback
         * work even if RESUMING is never delivered either. */
        s_awaiting     = 1;
        s_await_frames = 0;
    }
    if (resuming != s_resuming_seen) {
        s_resuming_seen = resuming;
        s_awaiting      = 1;
        s_await_frames  = 0;
    }
    if (complete != s_complete_seen) {
        s_complete_seen = complete;
        out->resumed    = 1;
        s_awaiting      = 0;
        s_await_frames  = 0;
    } else if (s_awaiting) {
        s_await_frames++;
        if (s_await_frames >= RESUME_FALLBACK_FRAMES) {
            /* See the header comment: RESUME_COMPLETE is documented as
             * unreliable, so this is the timeout that keeps the radio from
             * staying down forever. */
            apad_devlog("power: no RESUME_COMPLETE after %d frames --"
                        " treating the resume as finished anyway",
                        RESUME_FALLBACK_FRAMES);
            out->resumed   = 1;
            s_awaiting     = 0;
            s_await_frames = 0;
        }
    }
    return (out->suspended != 0 || out->resumed != 0);
}
