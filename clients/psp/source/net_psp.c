/*
 * clients/psp/source/net_psp.c -- PSP network bring-up.
 *
 * MIRRORED, not remembered. The call sequence below is pspsdk's own
 * src/samples/net/simple/main.c (vendored at references/psp/), as docs/CONVENTIONS.md
 * requires for a platform nobody here can test by hand. Where a sample and a
 * recollection disagree, the sample wins.
 *
 * ONE DELIBERATE DIVERGENCE FROM THE SAMPLE: the sample calls the pspsdk
 * helper pspSdkInetInit(), which is sceNetInit() + sceNetInetInit() inside a
 * single return code. The spike that opened this port got 0x8002013A out of
 * it and learned nothing -- the helper cannot say WHICH call failed. Splitting
 * it is what made the failure legible (it turned out to be a link problem, not
 * an init problem). The calls and their arguments are the sample's; only the
 * error reporting is ours.
 *
 * A SECOND, EARNED ON HARDWARE (2026-09-11): the sample polls
 * sceNetApctlGetState() until it reads GOT_IP and nothing else. On the
 * console the first run climbed to JOINING, was refused by the access point,
 * fell back to DISCONNECTED -- and the sample's loop polls that forever
 * with nothing to say. So a state that goes DOWN after going up, or thirty
 * seconds without GOT_IP, is a failure here, reported as `joinfail`: the
 * network said no, another saved slot may say yes, and the screen offers
 * both. The sample also hardcodes slot 1; the slots are enumerated from
 * the console's settings instead (sceUtilityCheckNetParam, no network
 * needed) so an empty slot 1 is not a dead end either.
 *
 * MEMORY ORDERING on g_st: one writer (the bring-up thread), one reader (the
 * frame loop), single core, word-sized fields, so no mutex. The ordering that
 * matters is that `ready` is written LAST -- after `ip` -- and nothing is
 * written after it. A reader that sees ready==1 therefore sees a complete ip.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <psputility.h>
#include <psputility_netparam.h>

#include <string.h>

#include "devlog.h"
#include "net_psp.h"

#define JOIN_TIMEOUT_MS 30000
/* How long apad_psp_net_restart() waits for an in-flight bring-up thread to
 * notice g_abort. The poll loop sleeps 50 ms, so this is 40 chances. */
#define ABORT_WAIT_MS   2000
/* How long it waits for the apctl to reach DISCONNECTED before terminating
 * it. Bounded because nothing here may block the frame loop forever. */
#define DISCONNECT_WAIT_MS 1000

static volatile struct {
    int  state;
    int  ready;
    int  failed;
    int  joinfail;
    int  err;
    const char *stage;
    char ip[16];
    int  slot;
    char name[32];
    int  power_save;
} g_st = { -1, 0, 0, 0, 0, "", { 0 }, 0, { 0 }, 0 };

static int g_stack_inited;      /* sceNetInit & co. run once per process   */
static volatile int g_thread_busy;  /* a bring-up thread is in its poll loop */
static volatile int g_abort;    /* ask that thread to give up and exit     */

static void fail(const char *stage, int err, int joinfail)
{
    g_st.stage    = stage;
    g_st.err      = err;
    g_st.joinfail = joinfail;
    g_st.failed   = 1;    /* written last: a reader seeing failed sees all */
}

int apad_psp_net_slots(int *slots, int max)
{
    int i, n = 0;

    for (i = 1; i <= APAD_PSP_NET_SLOTS && n < max; i++) {
        if (sceUtilityCheckNetParam(i) == 0) {
            slots[n++] = i;
        }
    }
    return n;
}

void apad_psp_net_slot_name(int slot, char *out, unsigned cap)
{
    netData d;

    out[0] = '\0';
    if (cap == 0) { return; }
    if (sceUtilityGetNetParam(slot, PSP_NETPARAM_NAME, &d) == 0 && d.asString[0] != '\0') {
        strncpy(out, d.asString, cap - 1);
    } else if (sceUtilityGetNetParam(slot, PSP_NETPARAM_SSID, &d) == 0) {
        strncpy(out, d.asString, cap - 1);
    }
    out[cap - 1] = '\0';
}

int apad_psp_net_modules(void)
{
    int rc;

    /* Modules before anything else touches the network -- the sample does
     * this in main(), before the thread exists, and so do we. */
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) {
        fail("sceUtilityLoadNetModule(COMMON)", rc, 0);
        return rc;
    }
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (rc < 0) {
        fail("sceUtilityLoadNetModule(INET)", rc, 0);
        return rc;
    }
    return 0;
}

/* The bring-up proper. Returns when done, whichever way; the thread
 * wrapper below is what frees the stack. */
static void net_run(void)
{
    union SceNetApctlInfo info;
    int rc, state, last = -1, peak = -1;
    unsigned waited_ms = 0;

    if (!g_stack_inited) {
        rc = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
        if (rc < 0) { fail("sceNetInit", rc, 0); return; }

        rc = sceNetInetInit();
        if (rc < 0) { fail("sceNetInetInit", rc, 0); return; }

        rc = sceNetApctlInit(0x1600, 0x42);
        if (rc < 0) { fail("sceNetApctlInit", rc, 0); return; }
        g_stack_inited = 1;
    }

    rc = sceNetApctlConnect(g_st.slot);
    if (rc < 0) { fail("sceNetApctlConnect", rc, 1); return; }

    for (;;) {
        if (g_abort) {
            /* apad_psp_net_restart() is waiting to terminate the libraries
             * this loop is polling. Leave promptly and leave the apctl
             * disconnected; `joinfail` (not a hard failure) because nothing
             * is actually broken -- the console just suspended. */
            (void)sceNetApctlDisconnect();
            fail("aborted", 0, 1);
            return;
        }
        rc = sceNetApctlGetState(&state);
        if (rc < 0) { fail("sceNetApctlGetState", rc, 0); return; }
        if (state != last) {
            g_st.state = state;
            last = state;
        }
        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            break;
        }
        if (state > peak) {
            peak = state;
        } else if (state == PSP_NET_APCTL_STATE_DISCONNECTED && peak > 0) {
            /* Went up, came back down: the access point refused us. */
            (void)sceNetApctlDisconnect();
            fail("join", peak, 1);
            return;
        }
        if (waited_ms >= JOIN_TIMEOUT_MS) {
            (void)sceNetApctlDisconnect();
            fail("join timeout", state, 1);
            return;
        }
        sceKernelDelayThread(50 * 1000);   /* the sample's own poll interval */
        waited_ms += 50;
    }

    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) {
        /* Not strncpy: "255.255.255.255" is exactly 15 chars, so strncpy with
         * a bound of 15 fills the buffer and writes no NUL. Copy the bytes
         * and terminate by hand -- the compiler warns about precisely this. */
        size_t n = strlen(info.ip);
        if (n > sizeof g_st.ip - 1) {
            n = sizeof g_st.ip - 1;
        }
        memcpy((void *)g_st.ip, info.ip, n);
        g_st.ip[n] = '\0';
    }
    /* Settings > Power Save Settings > WLAN Power Save. On, the radio dozes
     * between beacons and a 60 Hz UDP stream pays for it in lost datagrams
     * and second-long stalls -- the shape of "connects sometimes, drops
     * randomly" reported from the first console session. User code cannot
     * switch it off; it can at least say so on screen. */
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_POWER_SAVE, &info) == 0) {
        g_st.power_save = info.powerSave ? 1 : 0;
    }
    g_st.ready = 1;        /* LAST. See the ordering note at the top. */
}

static int net_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    net_run();
    g_thread_busy = 0;
    /* EXIT AND DELETE, never plain return. A PSP thread that returns stays
     * dormant with its stack allocated until something deletes it, and the
     * first retry on the console found that out: the second 256 KB stack
     * came back 0x80020190 (SCE_KERNEL_ERROR_NO_MEMORY) because the first
     * was still held. The net sample's own thread ends the same way. */
    sceKernelExitDeleteThread(0);
    return 0;
}

int apad_psp_net_start(int slot)
{
    SceUID th;
    char   name[32];

    if (g_thread_busy || g_st.ready) {
        return -1;
    }
    apad_psp_net_slot_name(slot, name, sizeof name);
    g_st.slot     = slot;
    memcpy((void *)g_st.name, name, sizeof g_st.name);
    g_st.state    = -1;
    g_st.err      = 0;
    g_st.stage    = "";
    g_st.joinfail = 0;
    g_st.failed   = 0;     /* the retry: a reader sees "in progress" again */

    g_thread_busy = 1;
    th = sceKernelCreateThread("apad_net", net_thread, 0x11,
                               256 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (th < 0) {
        g_thread_busy = 0;
        fail("sceKernelCreateThread", th, 0);
        return th;
    }
    return sceKernelStartThread(th, 0, NULL);
}

/*
 * SUSPEND / RESUME. A PSP suspend takes the radio and the network libraries'
 * state with it, so after a resume the stack has to come down and go up
 * again -- re-running sceNetApctlConnect() alone leaves the console off the
 * network, which is exactly the hardware report this exists for.
 *
 * THE TEARDOWN ORDER IS THE SDK'S OWN, not a recollection: pspsdk's
 * src/sdk/inethelper.c (the implementation behind the pspSdkInetTerm()
 * declared in pspsdk.h, read from pspdev/pspsdk at the pinned SDK's own
 * source) is
 *
 *     sceNetApctlTerm(); sceNetResolverTerm(); sceNetInetTerm(); sceNetTerm();
 *
 * -- i.e. the exact reverse of pspSdkInetInit()'s order, which in turn is
 * the order net_run() above brings the stack up in. sceNetResolverTerm() is
 * the one call omitted here, because net_run() never calls
 * sceNetResolverInit(): this client dials a numeric address and has no use
 * for DNS. The sceNetApctlDisconnect() in front is ours -- the helper
 * terminates an apctl that was never connected, and this one is.
 *
 * WHAT IS NOT DONE HERE: the net modules are not unloaded and reloaded.
 * sceNetTerm() does not unload them, nothing in the SDK's samples or the
 * helper touches sceUtilityUnloadNetModule(), and unloading a module that a
 * torn-down-but-not-quite library still references is a good way to invent a
 * new failure. sceUtilityLoadNetModule() IS called again (return code
 * ignored and logged) purely as a hedge: if a suspend does drop the modules,
 * this reloads them; if it does not, the call fails harmlessly because they
 * are already loaded. Whether a real console keeps them across a suspend was
 * not verifiable here.
 */
int apad_psp_net_restart(void)
{
    int waited, rc, slot;

    slot = (g_st.slot > 0) ? g_st.slot : 1;
    apad_devlog("net restart: begin (slot %d, thread_busy=%d, inited=%d)",
                slot, g_thread_busy, g_stack_inited);

    /* 1. Stop any bring-up thread first. Terminating the libraries under a
     *    thread that is still calling sceNetApctlGetState() on them is the
     *    one way this can crash rather than merely fail. */
    if (g_thread_busy) {
        g_abort = 1;
        for (waited = 0; g_thread_busy && waited < ABORT_WAIT_MS; waited += 20) {
            sceKernelDelayThread(20 * 1000);
        }
        g_abort = 0;
        if (g_thread_busy) {
            /* Give up rather than tear down underneath it. The connect
             * screen keeps its existing "could not join / try again"
             * handling, which is a survivable place to be. */
            apad_devlog("net restart: ABANDONED -- bring-up thread still"
                        " running after %d ms", ABORT_WAIT_MS);
            return -1;
        }
        apad_devlog("net restart: bring-up thread stopped after %d ms", waited);
    }

    /* 2. Down, in the SDK helper's order. Every return code is logged: on a
     *    console with no debugger this log is the only account of which half
     *    of the resume failed. */
    if (g_stack_inited) {
        rc = sceNetApctlDisconnect();
        apad_devlog("net restart: sceNetApctlDisconnect -> 0x%08X", (unsigned)rc);
        for (waited = 0; waited < DISCONNECT_WAIT_MS; waited += 50) {
            int state = 0;
            if (sceNetApctlGetState(&state) < 0
                || state == PSP_NET_APCTL_STATE_DISCONNECTED) {
                break;
            }
            sceKernelDelayThread(50 * 1000);
        }
        rc = sceNetApctlTerm();
        apad_devlog("net restart: sceNetApctlTerm -> 0x%08X", (unsigned)rc);
        rc = sceNetInetTerm();
        apad_devlog("net restart: sceNetInetTerm -> 0x%08X", (unsigned)rc);
        rc = sceNetTerm();
        apad_devlog("net restart: sceNetTerm -> 0x%08X", (unsigned)rc);
        g_stack_inited = 0;
    }

    /* 3. Forget everything the old association published. `ready` is cleared
     *    FIRST here (it is written last on the way up), so no reader can see
     *    a stale "ready" with a dead socket behind it. */
    g_st.ready      = 0;
    g_st.failed     = 0;
    g_st.joinfail   = 0;
    g_st.err        = 0;
    g_st.stage      = "";
    g_st.state      = -1;
    g_st.power_save = 0;
    g_st.ip[0]      = '\0';

    /* 4. The module hedge -- see the comment above. */
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    apad_devlog("net restart: LoadNetModule(COMMON) -> 0x%08X (ignored)", (unsigned)rc);
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    apad_devlog("net restart: LoadNetModule(INET) -> 0x%08X (ignored)", (unsigned)rc);

    /* 5. Up again from the very beginning: the thread runs net_run(), which
     *    with g_stack_inited cleared redoes sceNetInit / sceNetInetInit /
     *    sceNetApctlInit before connecting. Same thread rule as every other
     *    bring-up here: it exits-and-deletes, so repeated resumes cannot
     *    accumulate 256 KB stacks and hit 0x80020190. */
    rc = apad_psp_net_start(slot);
    apad_devlog("net restart: apad_psp_net_start(%d) -> 0x%08X",
                slot, (unsigned)rc);
    return rc;
}

int apad_psp_net_associated(void)
{
    int state = 0;
    /* sceNetApctlGetState() is a query, valid any time after ApctlInit --
     * the bring-up thread has long since exited by the time this is asked. */
    if (sceNetApctlGetState(&state) < 0) {
        return 0;
    }
    return (state == PSP_NET_APCTL_STATE_GOT_IP);
}

void apad_psp_net_get(apad_psp_net_status *out)
{
    out->state    = g_st.state;
    out->ready    = g_st.ready;
    out->failed   = g_st.failed;
    out->joinfail = g_st.joinfail;
    out->err      = g_st.err;
    out->stage    = (const char *)g_st.stage;
    out->slot     = g_st.slot;
    out->power_save = g_st.power_save;
    memcpy(out->ip, (const void *)g_st.ip, sizeof out->ip);
    memcpy(out->name, (const void *)g_st.name, sizeof out->name);
}
