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

#include "net_psp.h"

#define JOIN_TIMEOUT_MS 30000

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
static int g_thread_busy;       /* a bring-up thread is in its poll loop   */

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
