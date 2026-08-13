/* clients/3ds/source/soc_3ds.c
 *
 * soc:U bring-up ONLY. Everything BSD-sockets-shaped after this point --
 * apad_net_init/apad_udp_open/apad_udp_send/apad_udp_recv/
 * apad_udp_set_broadcast/apad_udp_close -- comes from the SHARED
 * shim/net_bsd.c, unmodified, per docs/DESIGN.md S3 ("net_bsd.c: 3DS, PSP, Vita,
 * Switch, Linux, macOS, Android") and per the coordinator's fix in commit
 * 975d01b (shim/net_bsd.c now includes <arpa/inet.h>, which is what this
 * client originally found missing).
 *
 * Why this file exists at all, separate from net_bsd.c: soc:U needs a
 * one-time, 3DS-specific "turn the network stack on" step that no other
 * platform in the shared shim has an equivalent of (Linux sockets need no
 * such call; that is exactly why apad_net_init() in net_bsd.c is just pool
 * bookkeeping, not a network bring-up routine). Folding socInit() into
 * net_bsd.c would make the "shared" file console-aware; keeping it here
 * keeps net_bsd.c genuinely platform-neutral and puts the one 3DS-specific
 * step where docs/DESIGN.md's architecture implies it belongs: in the client, not
 * the shim. main.c calls apad3ds_soc_bringup() once, BEFORE the first call
 * to apad_net_init()/apad_udp_open() from net_bsd.c.
 *
 * Mirrors references/3ds/sockets/source/sockets.c: memalign(0x1000, ...),
 * socInit() checked against 0, atexit(socExit) registered so it runs before
 * atexit(gfxExit) (main.c registers gfxExit first; atexit runs in reverse
 * order). ONE deliberate divergence from the sample's literal bytes: the
 * sample allocates 0x100000 (1 MB); docs/DESIGN.md S7.1 specifies 128 KB (0x20000),
 * which is what the M0 spike ran successfully on this exact hardware
 * (references/README.md) -- not a transcription slip.
 */

#include <malloc.h>
#include <stdlib.h>

#include <3ds.h>

#define APAD3DS_SOC_ALIGN   0x1000u
#define APAD3DS_SOC_BUFSIZE 0x20000u /* 128 KB -- docs/DESIGN.md S7.1, not the
                                       * sample's 1 MB. See file header. */

static u32 *g_soc_buffer;

static void apad3ds_soc_shutdown(void)
{
    socExit();
}

/* Diagnostic-only: lets main.c print the exact buffer size passed to
 * socInit() without duplicating the constant (coordinator's diagnostic
 * build request). */
size_t apad3ds_soc_bufsize(void)
{
    return (size_t)APAD3DS_SOC_BUFSIZE;
}

/* Call once, before the first shim/net_bsd.c call. Returns the raw socInit()
 * Result in *out_result (0 on success) so main.c can print it on screen per
 * this task's brief: "If socInit returns non-zero, print the code on screen
 * rather than failing silently." Returns 1 on success, 0 on failure. */
int apad3ds_soc_bringup(Result *out_result)
{
    Result rc;

    /* memalign(0x1000, ...): the single most common first-time 3DS
     * networking mistake is skipping this alignment requirement. */
    g_soc_buffer = (u32 *)memalign(APAD3DS_SOC_ALIGN, APAD3DS_SOC_BUFSIZE);
    if (g_soc_buffer == NULL) {
        if (out_result != NULL) {
            *out_result = -1; /* memalign has no Result code of its own */
        }
        return 0;
    }

    rc = socInit(g_soc_buffer, APAD3DS_SOC_BUFSIZE);
    if (out_result != NULL) {
        *out_result = rc;
    }
    if (rc != 0) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        return 0;
    }

    /* atexit functions run in reverse order. main.c registers
     * atexit(gfxExit) before calling apad3ds_soc_bringup(), so this
     * registration -- made after -- runs FIRST at shutdown, exactly
     * mirroring references/3ds/sockets/source/sockets.c's comment: "this
     * runs before gfxExit". */
    atexit(apad3ds_soc_shutdown);
    return 1;
}
