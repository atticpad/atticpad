/* clients/nds/source/camera_nds.c -- see camera_nds.h.
 *
 * MIRRORS references/nds/examples/peripherals/camera/source/main.c:
 *
 *   cameraInit();                                  (main.c:145)
 *   cameraSelect(CAMERA_OUTER);                     (main.c:153, `camera` var)
 *   cameraStartTransfer(buf, MCUREG_APT_SEQ_CMD_PREVIEW, CAMERA_NDMA_CHANNEL);
 *                                                    (main.c:173-175, inside
 *                                                     `if (!ndmaBusy(...) ||
 *                                                     !cameraTransferActive())`)
 *   cameraStopTransfer(); cameraDeinit();            (main.c:186, 247)
 *
 * CAMERA_NDMA_CHANNEL is the sample's own #define (main.c:19), value 1; kept
 * identical here rather than picking a different channel with no reference
 * for it. NOT CROSS-CHECKED against every other NDMA user in this ROM --
 * DSWiFi and the rest of libnds are documented (references/nds/README.md,
 * this project's own platform notes) as classic-DMA (nds/dma.h, channels
 * 0-3) users, a SEPARATE hardware unit from NDMA (nds/ndma.h, also 4
 * channels, DSi-only) that this file uses, so no conflict is expected -- but
 * this was not exhaustively verified against every library linked into the
 * ROM. Flagged in the platform report.
 *
 * See camera_nds.h for why this file does NOT follow camera_3ds.c's threaded
 * capture task, and -- the correction this file was rewritten for -- why the
 * NDMA destination is now the caller's MAIN-ENGINE VRAM (the bottom screen,
 * ui_bottom_camera_lock()) exactly as the sample's own bgGetGfxPtr() target
 * is, rather than the main-RAM buffer this file used to allocate.
 */

#include <malloc.h>   /* memalign() -- see the buffer allocation below */
#include <stdint.h>   /* uintptr_t, for the VRAM-range check in start()     */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "camera_nds.h"

#define CAM_NDMA_CHANNEL 1   /* the sample's CAMERA_NDMA_CHANNEL, verbatim */
#define CAM_BYTES  (APAD_NDS_CAM_W * APAD_NDS_CAM_H * 2)   /* 2 bytes/pixel */
#define LUMA_BYTES (APAD_NDS_CAM_W * APAD_NDS_CAM_H)       /* 1 byte/pixel  */

/* THE LUMA BUFFER IS THE ONLY main-RAM ALLOCATION LEFT, and it is memalign
 * (32)'d rather than malloc()'d out of caution, not necessity: the CPU is the
 * only thing that writes it (apad_nds_cam_luma() fills it by READING the
 * uncached VRAM preview), so no DMA can race a cache line here and no
 * DC_InvalidateRange() is called on it. The alignment is kept because libnds's
 * own cache.h warns that invalidating a range that does not fill whole cache
 * lines also invalidates its heap neighbours -- if a future edit ever does
 * hand this plane to DMA, the buffer is already safe to invalidate. LUMA_BYTES
 * = 256*192 = 49152 = 32*1536, a whole number of lines. The PREVIEW needs none
 * of this at all now: VRAM is UNCACHED on this CPU, which is exactly why the
 * reference sample never calls a cache function around bgGetGfxPtr() either. */
#define CAM_ALIGN 32

static int      s_running;
static int      s_cam_up;          /* cameraInit() succeeded, Deinit owed   */
static int      s_lid_was_closed;  /* edge tracker for apad_nds_cam_poll()  */
static uint16_t *s_preview;        /* the CALLER's main-engine VRAM, NDMA
                                    * target -- borrowed, never freed here  */
static uint8_t  *s_luma;           /* APAD_NDS_CAM_W * APAD_NDS_CAM_H       */
static unsigned  s_frames;
static char      s_status[64];

static void fail(const char *what)
{
    snprintf(s_status, sizeof s_status, "%s failed", what);
}

int apad_nds_cam_start(uint16_t *vram_target)
{
    if (s_running) {
        return 1;
    }
    s_status[0] = '\0';
    s_frames = 0;
    s_lid_was_closed = 0;
    s_cam_up = 0;
    s_preview = NULL;

    /* Belt and braces: cameraInit()/cameraSelect()/cameraStartTransfer() all
     * already refuse in DS mode by returning false (libnds source,
     * source/arm9/peripherals/camera.c: each is a one-line `if
     * (!isDSiMode()) return false;` wrapper around its TWL implementation),
     * so this check is redundant with the hardware's own refusal -- but it
     * is also the Makefile-independent runtime gate the task asks for, and
     * it means s_status says something more specific than "cameraInit
     * failed" when the real reason is "not a DSi". */
    if (!isDSiMode()) {
        fail("not running in DSi mode");
        return 0;
    }

    /* THE DESTINATION CHECK. camera_nds.h says why this matters more than an
     * ordinary NULL guard: a destination outside main-engine background VRAM
     * (0x06000000 .. 0x06200000, where the SUB engine's own range starts) is
     * the one mistake that does not fail loudly -- it delivers a partial
     * first frame and then hangs the transfer forever, which is what both a
     * main-RAM target and a SUB-engine (top screen) target did. Cheap to
     * check, and it turns a silent freeze into a message on the screen. */
    if (vram_target == NULL
        || (uintptr_t)vram_target < 0x06000000u
        || (uintptr_t)vram_target + (uintptr_t)CAM_BYTES > 0x06200000u) {
        fail("camera target is not main-engine VRAM");
        return 0;
    }
    s_preview = vram_target;

    /* The luma plane quirc is fed from -- the only main-RAM buffer this file
     * still allocates. Ordinary heap: this screen is DSi-only (16 MB) and
     * enters with no session in flight, so docs/CONVENTIONS.md's "no malloc after init"
     * rule (which is about the wire-path core/shim code) does not apply here.
     * memalign(32) rather than malloc() -- see CAM_ALIGN. */
    s_luma = (uint8_t *)memalign(CAM_ALIGN, (size_t)LUMA_BYTES);
    if (s_luma == NULL) {
        fail("out of memory for the camera luma buffer");
        apad_nds_cam_stop();
        return 0;
    }
    memset(s_luma, 0, (size_t)LUMA_BYTES);

    if (!cameraInit()) {
        fail("cameraInit()");
        apad_nds_cam_stop();
        return 0;
    }
    s_cam_up = 1;

    if (!cameraSelect(CAMERA_OUTER)) {
        fail("cameraSelect(CAMERA_OUTER)");
        apad_nds_cam_stop();
        return 0;
    }

    /* The first transfer is armed by the first apad_nds_cam_poll() call, on
     * the same "not-busy, not-active" check the sample makes every VBlank --
     * see that function. Nothing to arm yet here, matching the sample's own
     * loop shape (it checks before every StartCapture too, including the
     * first). */
    s_running = 1;
    return 1;
}

/* Drain the in-flight camera NDMA, BOUNDED. The sample waits with a plain
 * `while (ndmaBusy(ch)) swiWaitForVBlank();` and that is what this used to
 * be -- but a camera transfer can die permanently: when the camera's
 * ping-pong buffer overruns, the camera block sets REG_CAM_CNT bit 4 and
 * stops feeding, cameraStartTransfer() never clears that bit, and the NDMA
 * channel then reports busy FOREVER. An unbounded wait on it froze the
 * console on the camera screen right after a successful decode, whichever
 * server's QR it was (2026-09-10). So: wait a handful of frames for an
 * honest completion, then give up and kill the transfer from both ends --
 * stop the camera feeding (CAM_CNT transfer enable off) and clear the NDMA
 * channel's enable bit, which halts it whatever state it is in. Nothing
 * downstream needs the last frame; this only ever runs on the way out. */
#define CAM_DRAIN_MAX_FRAMES 8   /* ~130 ms, several whole preview frames */

static void cam_drain_or_kill(void)
{
    int frames = 0;

    while (ndmaBusy(CAM_NDMA_CHANNEL) && frames < CAM_DRAIN_MAX_FRAMES) {
        swiWaitForVBlank();
        frames++;
    }
    cameraStopTransfer();
    if (ndmaBusy(CAM_NDMA_CHANNEL)) {
        /* Wedged. Force the channel off; nds/ndma.h has no helper for this,
         * the control register's bit 31 is both the enable and the busy
         * flag (NDMA_ENABLE == NDMA_BUSY), so clearing it halts the DMA. */
        REG_NDMA_CR(CAM_NDMA_CHANNEL) &= ~NDMA_ENABLE;
    }
}

void apad_nds_cam_stop(void)
{
    if (s_cam_up) {
        /* Drain any in-flight NDMA before the caller takes its VRAM back --
         * ui_bottom_camera_unlock() puts the bottom screen straight back into
         * ordinary double-buffered drawing, and a transfer still in flight
         * would then be writing pixels underneath it. Same call the sample --
         * the sample itself does this before every StartCapture and before
         * exit (main.c:184, :221, :241): `while (ndmaBusy(CAMERA_NDMA_CHANNEL))
         * swiWaitForVBlank();`. swiWaitForVBlank() is fine here (unlike
         * main.c's frame loop): this bounded drain runs during screen
         * teardown, not the DSWiFi-cothread-bearing per-frame loop that
         * forces cothread_yield_irq(IRQ_VBLANK) everywhere else in this
         * client -- and camera teardown has nothing to do with the network. */
        cam_drain_or_kill();
        cameraDeinit();
        s_cam_up = 0;
    }
    /* s_preview is the CALLER's VRAM (ui_bottom_camera_lock()). Forget it;
     * never free it -- it is not heap memory and this file does not own it.
     * The caller releases it with ui_bottom_camera_unlock(). */
    s_preview = NULL;
    if (s_luma != NULL) {
        free(s_luma);
        s_luma = NULL;
    }
    s_running = 0;
}

/* Pause the capture around a decode. quirc on a version-4 pairing symbol
 * (a 20-character token) runs for tens of ms on this ARM9 with no frame
 * loop between -- long enough for the camera's ping-pong buffer to overrun
 * and wedge the transfer (see cam_drain_or_kill). Nothing is streaming
 * while the decoder reads the last completed frame out of VRAM, so stop the
 * feed first; apad_nds_cam_poll()'s "not busy, not active" test re-arms it
 * on the next frame exactly as after any completed transfer. */
void apad_nds_cam_pause(void)
{
    if (s_running && s_cam_up) {
        cam_drain_or_kill();
    }
}

int apad_nds_cam_running(void)
{
    return s_running;
}

int apad_nds_cam_poll(int lid_closed)
{
    int completed = 0;

    if (!s_running) {
        return 0;
    }

    if (lid_closed) {
        if (!s_lid_was_closed) {
            /* Falling edge: stop the transfer without tearing the camera
             * down, exactly as apad_nds_cam_stop()'s own drain does, so nothing
             * is DMA-ing into s_preview while the lid is shut. */
            cam_drain_or_kill();
            s_lid_was_closed = 1;
        }
        return 0;
    }
    s_lid_was_closed = 0;

    /* THE SAMPLE'S OWN LOOP, verbatim in shape (main.c:170-176): if nothing
     * is in flight, a completed frame is sitting in the buffer and a new
     * transfer is armed over the top of it. There is no separate "done"
     * flag to read first -- the DS's own camera driver header
     * (nds/arm9/camera.h) documents ndmaBusy()+cameraTransferActive()
     * together as the way to tell, and offers nothing else. */
    if (!ndmaBusy(CAM_NDMA_CHANNEL) || !cameraTransferActive()) {
        completed = (s_frames > 0u) ? 1 : 0;   /* nothing to read on the very
                                                 * first arm */
        s_frames++;
        /* No cache operation: s_preview is VRAM, which is uncached on this
         * CPU -- the same reason the reference sample arms its own transfer
         * straight onto bgGetGfxPtr() with nothing in between. */
        cameraStartTransfer(s_preview, MCUREG_APT_SEQ_CMD_PREVIEW,
                            CAM_NDMA_CHANNEL);
    }
    return completed;
}

const uint16_t *apad_nds_cam_preview(void)
{
    if (!s_running || s_preview == NULL || s_frames == 0u) {
        return NULL;
    }
    /* Straight through: VRAM is uncached, so there is nothing to invalidate
     * and nothing stale to read. */
    return s_preview;
}

/* BGR555 -> BT.601-style luma, same shape as camera_3ds.c's luma565() (see
 * that file's comment for why this is a function rather than inlined at each
 * call site: the decoder and any future debug view must read the exact same
 * weights). Channel positions per ui.c's rgb15(): bits 0-4 R, 5-9 G, 10-14 B,
 * bit 15 alpha/opaque (ignored -- this camera output is always opaque). */
static inline uint8_t luma_bgr555(uint16_t p)
{
    unsigned r = (unsigned)(p & 0x1Fu);
    unsigned g = (unsigned)((p >> 5) & 0x1Fu);
    unsigned b = (unsigned)((p >> 10) & 0x1Fu);

    return (uint8_t)((77u * (r << 3) + 151u * (g << 3) + 28u * (b << 3)) >> 8);
}

const uint8_t *apad_nds_cam_luma(void)
{
    const uint16_t *src = apad_nds_cam_preview();
    int i, n;

    if (src == NULL || s_luma == NULL) {
        return NULL;
    }
    n = APAD_NDS_CAM_W * APAD_NDS_CAM_H;
    for (i = 0; i < n; i++) {
        s_luma[i] = luma_bgr555(src[i]);
    }
    return s_luma;
}

const char *apad_nds_cam_status(void)
{
    return (s_status[0] != '\0') ? s_status : NULL;
}

unsigned apad_nds_cam_frames(void)
{
    return s_frames;
}
