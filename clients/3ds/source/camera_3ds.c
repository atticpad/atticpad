/* clients/3ds/source/camera_3ds.c -- see camera_3ds.h.
 *
 * ROUND 7: THE CAPTURE LOOP IS A THREAD, BECAUSE THAT IS WHAT EVERY WORKING
 * 3DS CAMERA IMPLEMENTATION DOES.
 *
 * Six rounds of hardware said the viewfinder tears, in every variant of a
 * capture loop driven from inside the citro2d frame loop. Round 6 measured it
 * objectively for the first time and the answer was that ALL THREE variants
 * tear: 796/1639 frames flagged with the arm at the top of the poll, 79/208
 * with a ClearBuffer before every arm, 361/512 re-arming on completion. The
 * seam row wandered in all three, which rules out scene content. Rounds 5 and
 * 6 each suppressed their target mechanism completely and the artefact
 * survived, twice, so round 7 stopped theorising and went to read code that
 * demonstrably does not have this problem.
 *
 * THE TWO REFERENCES, and they agree with each other on everything:
 *
 *   Steveice10/FBI, source/core/task/capturecam.c (MIT). The QR remote-install
 *   scanner in the most-installed piece of 3DS homebrew there is. Its capture
 *   is a dedicated thread created with
 *       threadCreate(task_capture_cam_thread, data, 0x10000, 0x1A, 0, true)
 *   whose body is:
 *       svcWaitSynchronizationN(&index, events, 3, false, U64_MAX)
 *       EVENT_CANCEL(0) -> stop
 *       EVENT_RECV(1)   -> close the recv handle
 *                          lock, memcpy into the published buffer,
 *                          GSPGPU_FlushDataCache, unlock
 *                          CAMU_SetReceiving(...)                 <- re-arm
 *       EVENT_BUFFER_ERROR(2) -> close the recv handle
 *                          CAMU_ClearBuffer -> CAMU_SetReceiving
 *                          -> CAMU_StartCapture
 *   and whose bring-up ends
 *       ... ClearBuffer -> SetReceiving -> StartCapture.
 *
 *   Anemone3DS, source/camera.c (GPLv3 -- READ, NOT COPIED; it is here as an
 *   independent confirmation, not as a source of text). Same three events in
 *   the same order, same U64_MAX wait, same recv body, same error body, same
 *   pre-armed StartCapture, same teardown. It differs from FBI only in using
 *   linearAlloc for the DMA buffer, LightLock instead of svcCreateMutex, and a
 *   joinable thread on core 1 instead of a detached one on core 0.
 *
 * WHAT EVERY WORKING IMPLEMENTATION DOES, AND WHAT THIS FILE USED TO DO:
 *
 *   | question            | FBI / Anemone            | this file, rounds 1-6  |
 *   |---------------------|--------------------------|------------------------|
 *   | who runs the loop   | dedicated thread         | the UI thread          |
 *   | wait timeout        | U64_MAX (infinite)       | 10 ms, once per frame  |
 *   | detection latency   | microseconds             | up to one 16.7 ms frame|
 *   | work before re-arm  | one 192 KB memcpy        | stage + display xfer + |
 *   |                     |                          | 2 screens + sometimes  |
 *   |                     |                          | a 22 ms quirc decode   |
 *   | unarmed window      | ~2 ms  (6% of a frame)   | 16-55 ms (0.5-1.7)     |
 *   | first StartCapture  | PRE-ARMED                | nothing armed          |
 *   | event order         | recv BEFORE error        | error before recv      |
 *   | frame across restart| kept and shown           | withheld (round 5)     |
 *
 * The unarmed-window row is the one that explains the measurements. A
 * SetReceiving issued while the sensor is mid-scanout starts its DMA from
 * wherever the sensor is, so the buffer holds the tail of exposure N above the
 * head of exposure N+1 -- a splice at a row that depends on the PHASE of the
 * gap. FBI's gap is a small fraction of one sensor period and always lands in
 * about the same place; this file's gap was longer than a whole sensor period
 * and landed somewhere different every time. That is exactly "seam row wanders
 * (mv 431)", which round 6 measured and could not explain.
 *
 * The pre-arm row is the other half. A StartCapture with a receive already
 * armed is the one moment the DMA is KNOWN to begin at a frame boundary; the
 * thread then never gives that alignment up. This file did not pre-arm,
 * because the devkitPro sample does not, and hardware had once preferred the
 * sample -- but that comparison was made against a version that also restarted
 * capture in recovery WITHOUT arming first, which is a different bug.
 *
 * =========================================================================
 *
 * SO: ONE LOOP, FBI'S, PLUS ONE CONTROLLED VARIANT.
 *
 *   G (default)  FBI transliterated. Single DMA buffer. On completion:
 *                close -> publish (lock, 192 KB memcpy, unlock) -> re-arm.
 *   H            G with a ping-pong DMA pair, re-arming into the other buffer
 *                BEFORE the publish copy. Unarmed window shrinks from a memcpy
 *                to a single IPC call. NEITHER REFERENCE DOES THIS. It is here
 *                only so that "G is much better but not clean" and "the gap is
 *                irrelevant" can be told apart in ONE netload instead of two.
 *
 * Everything round 5 and round 6 added on top of the polling pump is GONE:
 * the display filter for the frame received across a restart (neither
 * reference has one, and round 5 proved suppressing that frame does not touch
 * the tear), its starvation guard, and modes D/E/F. What is kept is the TEAR
 * METRIC -- see measure_tear() -- because it is the only adjudicator in this
 * file's history that can be photographed and compared, and it is how hardware
 * will say whether round 7 worked.
 *
 * =========================================================================
 *
 * DELIBERATE DEVIATIONS FROM FBI, all of them named, none of them in the loop:
 *
 *   1. ONE camera (SELECT_OUT1 / PORT_CAM1), the outer left. FBI offers an
 *      inner/outer toggle; a QR code on a monitor is only ever in front of the
 *      console. No CAMU_SynchronizeVsyncTiming (that is for a stereo pair) and
 *      no CAMU_PlayShutterSound (this never stores an image).
 *   2. CAMU_SetTrimming(PORT_CAM1, false) instead of FBI's SetTrimming(true) +
 *      SetTrimmingParamsCenter(w, h, 400, 240). FBI's trim is the IDENTITY at
 *      our size -- it crops 400x240 out of 400x240 -- so the two are provably
 *      the same image, and `false` is what this console has already run for
 *      thousands of frames and what Anemone3DS and the devkitPro sample both
 *      use. Round 7 changes the threading; introducing an untested trim path
 *      in the same build would make the result unattributable.
 *   3. The frame goes to a GPU texture, not to gfx's framebuffer. FBI has a
 *      screen_load_texture_untiled() helper this app has no equivalent of; the
 *      path here is a 512x256 staging copy plus C3D_SyncDisplayTransfer, and
 *      it is the one part of this file no sample covers. See stage_frame().
 *   4. apad3ds_cam_start() BLOCKS until the thread reports bring-up done or
 *      failed. FBI returns immediately and reports failure through a flag the
 *      UI polls. The facade keeps screen_qrscan.c unchanged; see camera_3ds.h.
 *   5. A joinable thread (Anemone's choice) rather than FBI's detached one,
 *      joined in apad3ds_cam_stop(). "No dangling thread at app teardown" is
 *      an explicit requirement here, and a join says it in one call.
 *   6. LightLock rather than svcCreateMutex (Anemone's substitution of the
 *      same semantics; it is the libctru-native lock and its uncontended path
 *      is not a syscall).
 *   7. No pause event. FBI's thread does
 *      svcWaitSynchronization(task_get_pause_event(), U64_MAX) at the top of
 *      every iteration, so that an APT suspend parks the capture thread. This
 *      app has no such event; the thread keeps running across a HOME press and
 *      relies on the buffer-error recovery to pick the camera back up. UNTESTED
 *      -- see "still unverified" at the bottom of this file.
 *   8. A retry path for a REFUSED SetReceiving. FBI has none: it would then
 *      wait on handle 0 forever. Hardware here has measured `refused 0` in
 *      every round, so this is defence against something that has never
 *      happened, and it is written so that cancellation still works while it
 *      retries.
 *   9. A five-second cap on bring-up (deviation 4's LightEvent), so a hung
 *      camera service is a reported failure rather than a frozen app.
 */

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atticpad/atticpad.h"

#include "camera_3ds.h"

/* ------------------------------------------------------------------------ */
/* geometry                                                                 */
/* ------------------------------------------------------------------------ */

#define CAM_W       APAD3DS_CAM_W
#define CAM_H       APAD3DS_CAM_H
#define CAM_BPP     2                       /* OUTPUT_RGB_565               */
#define CAM_BYTES   (CAM_W * CAM_H * CAM_BPP)

/* GPU textures must be power-of-two, so a 400x240 frame lives in the corner
 * of a 512x256 one and a Tex3DS_SubTexture crops it back. */
#define TEX_W       512
#define TEX_H       256
#define TEX_BYTES   (TEX_W * TEX_H * CAM_BPP)

/* Linear->tiled, no flip, no scaling, RGB565 in and RGB565 out.
 *
 * IN and OUT formats are deliberately IDENTICAL: with both set to RGB565 the
 * transfer engine reorders no channels, so whatever the sensor calls
 * "RGB_565" reaches a GPU_RGB565 texel unchanged and this file cannot be the
 * thing that swaps red and blue. */
#define CAM_TEX_XFER_FLAGS                                                  \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1)                    \
     | GX_TRANSFER_RAW_COPY(0)                                              \
     | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565)                        \
     | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)                       \
     | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* THE TEAR METRIC's sampling grid and thresholds. EVERY NUMBER HERE WAS
 * MEASURED, on the host, against synthetic frames spliced the way the theory
 * says the port splices them -- because a metric that has to be calibrated on
 * hardware costs the same session it is supposed to save. The harness built
 * 400x240 RGB565 scenes (a keyboard, a window blind, a blank wall, fine
 * texture, and a half-width horizontal object edge), spliced each one at
 * EVERY row 1..238 the way a receive armed mid-scanout would -- tail of
 * exposure N above the seam, head of exposure N+1 below it, displaced
 * sideways because the entry point is not a row boundary -- and swept the
 * design. Two of its findings overturned the obvious implementation:
 *
 * 1. ROWS CANNOT BE SUBSAMPLED. A splice is ONE row pair. Sampling every 8th
 *    pair sees it one time in eight, so a mode that spliced every single
 *    frame would have read `tear 12%` and been called clean. Every adjacent
 *    pair is measured; the cost is paid back by rolling the previous row's
 *    luma forward, so it is still ONE luma pass over the sampled columns.
 * 2. THE STATISTIC MUST HAVE ITS DC REMOVED. Plain mean |a[x] - b[x]| flagged
 *    100% of CLEAN keyboard and blind frames: a full-width horizontal
 *    brightness step is exactly what those scenes are made of, and it is
 *    indistinguishable from a seam by that measure. What a seam actually is,
 *    and what content edges are not, is a DECORRELATION -- the row below is
 *    from a different part of a different exposure and displaced sideways, so
 *    the difference varies wildly ACROSS the row instead of being a constant
 *    offset. Measuring the mean absolute deviation of the difference from its
 *    own mean throws away the brightness step and keeps the decorrelation.
 *
 * With that, at FACTOR 6 and FLOOR 12 (of 255), the harness measured:
 *
 *   keyboard   clean: max 15, median 4 -> x3, NO flag
 *              spliced: flagged 199/238 seam positions (83%)
 *   blank wall / window blind: neither flagged, clean or spliced -- scenes
 *              with no horizontal detail carry no seam signature (and show no
 *              visible tear either). DO NOT AIM AT A WALL.
 *   half-width horizontal object edge (a laptop lid, a desk edge): FALSE
 *              POSITIVE, max 69 -> x11. This is the metric's one real blind
 *              spot and the reason the trace also carries `mv`: a content
 *              edge flags THE SAME ROW every frame, a splice lands somewhere
 *              new every time. Flags with `mv` near zero are furniture.
 *
 * The round-6 hardware run makes those host numbers checkable: mode F read
 * 796/1639 with mv 431, i.e. the flags moved. That is the reading round 7 has
 * to beat, and it is on the trace next to the new one. */
#define TEAR_COL_STEP 8
#define TEAR_COLS     ((CAM_W + TEAR_COL_STEP - 1) / TEAR_COL_STEP)  /* 50 */
#define TEAR_FACTOR   6u
#define TEAR_FLOOR    12u

/* ------------------------------------------------------------------------ */
/* the capture thread's parameters -- FBI's, verbatim                       */
/* ------------------------------------------------------------------------ */

/* threadCreate(task_capture_cam_thread, data, 0x10000, 0x1A, 0, true) in
 * FBI's task_capture_cam(). Anemone3DS uses the same 0x10000 and the same
 * 0x1A on core 1.
 *
 * 64 KB of stack for a function whose largest local is a three-handle array is
 * generous, and it is FBI's number: this thread must never be the thing that
 * runs out.
 *
 * PRIORITY 0x1A IS HIGHER THAN THE MAIN THREAD (a .3dsx main thread is 0x30;
 * lower number = higher priority on this kernel), and that is the point -- the
 * thread must preempt the UI the instant the receive event fires, because the
 * whole round is about shrinking the delay between completion and re-arm. It
 * spends essentially all of its life blocked, so it cannot starve the UI.
 *
 * CORE 0, the appcore. FBI's value, and also the safe one: core 1 is the
 * syscore and running there needs APT_SetAppCpuTimeLimit, which this app does
 * not call. Keeping both threads on core 0 also means the publication handoff
 * never crosses a cache-coherency boundary. */
#define CAM_THREAD_STACK 0x10000
#define CAM_THREAD_PRIO  0x1A
#define CAM_THREAD_CORE  0

/* svcCreateThread refuses a priority outside the range the process descriptor
 * allows, and this client ships BOTH a .cia (its own exheader) and a .3dsx
 * (launched under whatever host the Homebrew Launcher is running in). FBI is
 * a .cia only and can hardcode 0x1A. If 0x1A is refused, fall back to one
 * step above the calling thread, then to the calling thread's own priority --
 * and put whichever one took on the trace, because "the thread exists but is
 * not preempting the UI" and "the thread does not exist" are different
 * failures that would otherwise look the same. */
#define CAM_PRIO_FLOOR   0x18

/* Deviation 9: bring-up runs on the capture thread and the caller waits for
 * it. Five seconds is far past any measured bring-up (camInit plus a dozen
 * IPC calls) and short enough that a hung cam:u is a message on the QR screen
 * rather than a console someone has to power off. */
#define CAM_READY_TIMEOUT_NS 5000000000LL

/* Deviation 8: how long a refused SetReceiving parks before retrying. It is a
 * wait on the CANCEL event, not a sleep, so B still leaves the screen
 * immediately while the port is misbehaving. */
#define CAM_RETRY_NS 2000000LL

/* The wait set, in FBI's order. THE ORDER IS LOAD-BEARING:
 * svcWaitSynchronizationN with waitAll=false returns the LOWEST signalled
 * index, so recv before error means a completed frame is always consumed and
 * the error is dealt with on the next iteration. Rounds 1-6 had error first
 * and discarded the frame; FBI and Anemone both have recv first. */
enum { EV_CANCEL = 0, EV_RECV = 1, EV_ERR = 2, EV_COUNT = 3 };

/* ------------------------------------------------------------------------ */
/* state -- and WHICH THREAD OWNS EACH PIECE                                */
/* ------------------------------------------------------------------------ */

/* THE THREE OWNERSHIP CLASSES, spelled out because getting this wrong is the
 * one new class of bug round 7 introduces:
 *
 *   UI      touched only by the frame-loop thread (start/stop/poll/luma/draw).
 *   CAP     touched only by the capture thread, between the moment
 *           threadCreate() returns and the moment threadJoin() does. The UI
 *           thread may READ these for the trace; a stale integer in a
 *           diagnostic is not a bug, and every one of them is a naturally
 *           aligned 32-bit word.
 *   SHARED  guarded by s_lock. Kept as small as possible: the published frame
 *           and the counters a reader needs to be self-consistent with it. */

/* --- UI --- */
static int       s_running;         /* the thread is up and captured OK      */
static int       s_atexit_armed;
static Thread    s_thread;
static int       s_thread_prio;     /* which priority threadCreate accepted  */
static LightLock s_lock;
static LightEvent s_ready;          /* CAP -> UI: bring-up finished          */
static volatile int s_th_state;

enum { CAM_TH_IDLE = 0, CAM_TH_STARTING, CAM_TH_RUNNING, CAM_TH_FAILED,
       CAM_TH_DONE };

static u16 *s_stage;                /* TEX_W x TEX_H staging copy, linear    */
static u8  *s_luma;                 /* CAM_W x CAM_H, tight, for quirc       */
static int  s_luma_valid;
static unsigned s_seen_seq;         /* last publication this thread staged   */
static unsigned s_shown;            /* frames that reached the texture       */
static unsigned s_missed;           /* publications never staged (see poll)  */
static unsigned s_polls;

static C3D_Tex           s_tex;
static int               s_tex_ready;
static Tex3DS_SubTexture s_subtex;
static int               s_have_frame;

/* --- SHARED (s_lock) --- */
static u8      *s_pub;              /* the published frame, CAM_BYTES        */
static unsigned s_pub_seq;          /* bumped by every publication           */
static unsigned s_captures;         /* completed receives                    */
static uint32_t s_last_cap_ms;

/* --- CAP --- */
static Handle s_ev[EV_COUNT];
static u8    *s_dma[2];             /* [1] allocated in mode H only          */
static int    s_dma_cur;
static s16    s_transfer_unit;
static int    s_cam_up;             /* camInit() succeeded, camExit() owed   */

/* THE MODE. Deliberately NOT reset by apad3ds_cam_start(): switching mode
 * restarts the camera, and a restart that reset the mode would switch it
 * straight back. */
static int      s_mode = APAD3DS_CAM_MODE_G;
static unsigned s_mode_switches;

/* The tear metric's tallies -- UI thread, written in measure_tear(). */
static unsigned s_tear_flagged;    /* staged frames whose seam test tripped  */
static unsigned s_tear_measured;   /* staged frames measured                 */
static int      s_tear_row;        /* most recent flagged seam row, -1 none  */
static unsigned s_tear_last_max;   /* last measured frame: worst row diff    */
static unsigned s_tear_last_med;   /* last measured frame: median row diff   */
static unsigned s_tear_hi_ratio;   /* highest max/median of the run          */
static int      s_tear_hi_row;
static unsigned s_tear_moved;      /* flags landing on a NEW row             */

static char     s_status[96];

/* ------------------------------------------------------------------------ */
/* the capture trace                                                        */
/* ------------------------------------------------------------------------ */

/* Instrumentation, not decoration. Nothing in this file can be stepped: no
 * 3DS emulator implements CAMU, so the only place it runs is a console in
 * someone's hands, and every round trip through that costs a person a netload,
 * an aim and a photograph. The numbers below are the ones that tell the
 * failure modes of the receive lifecycle apart -- "SetReceiving is refusing"
 * looks exactly like "the DMA never completes" looks exactly like "the error
 * event is latched" from outside, and all three read as a black screen.
 *
 * A TRACE NOBODY CAN SEE IS NOT A TRACE. An earlier round drew it only while
 * `frames == 0`; that failure stuck at `frames == 1`, so the viewfinder took
 * the screen forever and every number below was invisible for the whole
 * session. Visibility is apad3ds_cam_unhealthy()'s business and it is a
 * property of the PIPELINE, never of a counter the failure can pin. */
static u32      s_max_bytes;        /* what GetMaxBytes(400,240) answered   */
static u32      s_xfer_bytes;       /* what the port kept, read back        */
static Result   s_rc_xfer_bytes;
static Result   s_rc_setrecv;       /* last SetReceiving                    */
static unsigned s_arms;             /* SetReceiving calls made              */
static unsigned s_setrecv_fails;    /* ...of which refused                  */
static Result   s_rc_clear;         /* last ClearBuffer                     */
static unsigned s_clears;
static Result   s_rc_startcap;      /* last StartCapture                    */
static unsigned s_startcap_fails;
static unsigned s_recoveries;
static unsigned s_first_err_cap;    /* capture number of the FIRST error    */
static unsigned s_ev_recv;          /* receive events observed              */
static unsigned s_ev_err;           /* buffer-error events observed         */
static unsigned s_errors;
static unsigned s_wait_fails;       /* svcWaitSynchronizationN said no      */
static int      s_last_ev;          /* -1 nothing, 0 error, 1 frame         */
static char     s_dbg[APAD3DS_CAM_DEBUG_LINES][96];

/* ------------------------------------------------------------------------ */
/* the primitives -- CAPTURE THREAD ONLY                                    */
/* ------------------------------------------------------------------------ */

/* SetReceiving arms a DMA into the current DMA buffer and hands back an event
 * that fires when the whole image has been moved. The byte count is the TIGHT
 * frame -- 400*240*2 -- and not the 512x256 staging size: the staging buffer
 * is downstream of this and the camera knows nothing about it. transferUnit is
 * the chunk size the service itself chose, in GetMaxBytes.
 *
 * Failure is recorded rather than raised (deviation 8): the loop retries,
 * because a screen that keeps drawing and says why is worth more to someone
 * holding the console than one that tears itself down. `s_arms` and
 * `s_setrecv_fails` only mean anything together -- an arm count that keeps
 * climbing with a flat refusal count says every arm was ACCEPTED and the DMA
 * still never completed, which is a different bug from a port that says no,
 * and the two are indistinguishable from outside. */
static void arm_receive(void)
{
    s_arms++;
    s_rc_setrecv = CAMU_SetReceiving(&s_ev[EV_RECV], s_dma[s_dma_cur],
                                     PORT_CAM1, (u32)CAM_BYTES,
                                     s_transfer_unit);
    if (R_FAILED(s_rc_setrecv)) {
        s_ev[EV_RECV] = 0;
        s_setrecv_fails++;
    }
}

/* ClearBuffer, counted. Documented as clearing the port's buffer AND its
 * error flags.
 *
 * ROUND 7 PUTS IT WHERE BOTH REFERENCES PUT IT AND NOWHERE ELSE: once at
 * bring-up, once per buffer error, once at teardown. Round 6's modes D and E
 * called it before every arm and it did not help (D: errs/100 36, the worst
 * measured; E: 70% of frames flagged), which is consistent with FBI never
 * doing it. `clears` should therefore read 1 + errors + 1 for a whole run; a
 * clear count tracking `arms` means something has crept back in. */
static void clear_buffer(void)
{
    s_clears++;
    s_rc_clear = CAMU_ClearBuffer(PORT_CAM1);
}

static void start_capture(void)
{
    s_rc_startcap = CAMU_StartCapture(PORT_CAM1);
    if (R_FAILED(s_rc_startcap)) {
        s_startcap_fails++;
    }
}

/* Copy the completed DMA buffer into the published one and make it visible.
 *
 * THE CRITICAL SECTION IS THE COPY, which is FBI's shape exactly (its
 * EVENT_RECV body holds data->mutex across memcpy + GSPGPU_FlushDataCache).
 * The only thread that can be blocked by it is the UI thread inside
 * apad3ds_cam_poll(), for the ~2 ms the other copy takes -- and vice versa,
 * which is the one cost this design pays for not carrying a third buffer.
 *
 * NO GSPGPU_FlushDataCache HERE, unlike FBI. FBI hands data->buffer straight
 * to a GPU texture upload, so the GPU reads what the CPU just wrote; here the
 * published frame is read by the CPU (into s_stage) and it is s_stage that is
 * flushed, in stage_frame(), immediately before the display transfer. Flushing
 * both would be a second 192 KB cache operation per frame for nothing.
 *
 * Both threads live on core 0 (CAM_THREAD_CORE), so the lock's barriers are
 * the whole of the coherency story. */
static void publish(const u8 *src)
{
    LightLock_Lock(&s_lock);
    memcpy(s_pub, src, (size_t)CAM_BYTES);
    s_pub_seq++;
    s_captures++;
    s_last_cap_ms = apad_ticks_ms();
    LightLock_Unlock(&s_lock);
}

/* ------------------------------------------------------------------------ */
/* bring-up and teardown -- CAPTURE THREAD ONLY                             */
/* ------------------------------------------------------------------------ */

static int fail(const char *what, Result rc)
{
    snprintf(s_status, sizeof s_status, "%s failed: 0x%08lX", what,
             (unsigned long)rc);
    return 0;
}

/* FBI's and Anemone3DS's bring-up order, which is NOT the devkitPro sample's.
 * The two references agree with each other and the sample is the outlier, on
 * two points:
 *
 *   - CAMU_Activate comes BEFORE GetBufferErrorInterruptEvent, GetMaxBytes and
 *     SetTransferBytes. The sample configures the transfer on an inactive
 *     port and activates afterwards.
 *   - THE FIRST RECEIVE IS ARMED BEFORE THE FIRST StartCapture. The sample
 *     starts capture with nothing armed. This is the change that gives the run
 *     a known-good initial alignment; see the file header.
 *
 * Two independent battle-tested implementations against one sample is the
 * whole of the argument, and it is the same rule this project applies
 * everywhere else: the code that runs on hardware wins. */
static int cam_bring_up(void)
{
    Result rc;
    u32 max_bytes = 0;

    rc = camInit();
    if (R_FAILED(rc)) return fail("camInit", rc);
    s_cam_up = 1;

    rc = CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    if (R_FAILED(rc)) return fail("CAMU_SetSize", rc);
    rc = CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    if (R_FAILED(rc)) return fail("CAMU_SetOutputFormat", rc);
    /* 30 fps against a 60 Hz frame loop: the fastest the sensor offers, and
     * the fixed rate rather than one of the FRAME_RATE_*_TO_* auto-slowdown
     * modes -- dropping to 5 fps in dim light is right for a video recorder
     * and wrong for someone waving a console at a monitor. FBI: FRAME_RATE_30.
     */
    rc = CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    if (R_FAILED(rc)) return fail("CAMU_SetFrameRate", rc);

    (void)CAMU_SetNoiseFilter(SELECT_OUT1, true);
    (void)CAMU_SetAutoExposure(SELECT_OUT1, true);
    (void)CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);

    rc = CAMU_Activate(SELECT_OUT1);
    if (R_FAILED(rc)) return fail("CAMU_Activate", rc);

    rc = CAMU_GetBufferErrorInterruptEvent(&s_ev[EV_ERR], PORT_CAM1);
    if (R_FAILED(rc)) return fail("CAMU_GetBufferErrorInterruptEvent", rc);

    /* Deviation 2: identity trim vs no trim, and no trim is what has run. */
    (void)CAMU_SetTrimming(PORT_CAM1, false);

    rc = CAMU_GetMaxBytes(&max_bytes, CAM_W, CAM_H);
    if (R_FAILED(rc)) return fail("CAMU_GetMaxBytes", rc);
    s_max_bytes = max_bytes;
    /* FBI casts the same u32 to s16 for SetReceiving's transferUnit without
     * comment; so does the sample. Keep the cast, keep the value: it is a DMA
     * chunk size the service itself just told us, not a size we chose. Both
     * halves of that cast are on the trace, because a value that did not
     * survive it would look exactly like everything else that goes wrong. */
    s_transfer_unit = (s16)max_bytes;
    rc = CAMU_SetTransferBytes(PORT_CAM1, max_bytes, CAM_W, CAM_H);
    if (R_FAILED(rc)) return fail("CAMU_SetTransferBytes", rc);
    /* Read straight back. Every byte count in this file is a number nobody
     * here can compute, and a transferUnit the port rejected or rounded is
     * indistinguishable from a dead sensor once the only symptom is a blank
     * screen. */
    s_rc_xfer_bytes = CAMU_GetTransferBytes(&s_xfer_bytes, PORT_CAM1);

    clear_buffer();

    /* THE PRE-ARM. FBI and Anemone3DS both do this and the devkitPro sample
     * does not; see the file header for why it matters. */
    arm_receive();
    if (s_ev[EV_RECV] == 0) {
        return fail("CAMU_SetReceiving", s_rc_setrecv);
    }

    start_capture();
    if (R_FAILED(s_rc_startcap)) {
        return fail("CAMU_StartCapture", s_rc_startcap);
    }
    return 1;
}

/* FBI's teardown, verbatim, and Anemone3DS's is identical:
 *
 *     CAMU_StopCapture(PORT_CAM1);
 *     bool busy = false;
 *     while(R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy) {
 *         svcSleepThread(1000000);
 *     }
 *     CAMU_ClearBuffer(PORT_CAM1);
 *     CAMU_Activate(SELECT_NONE);
 *     camExit();
 *
 * THE IsBusy DRAIN IS NEW HERE and it is the reason this runs on the capture
 * thread rather than in apad3ds_cam_stop(): it waits for a DMA that may still
 * be in flight into a buffer this function is about to free. Rounds 1-6 had no
 * drain at all -- they closed the receive handle and freed the buffer, which
 * is a use-after-free waiting for a slow enough console.
 *
 * CAMU_Activate(SELECT_NONE) before camExit(), never the other way round: a
 * camera left activated drains the battery and locks out the system camera
 * applet, which is not something a user can diagnose or recover from without
 * a reboot. */
static void cam_tear_down(void)
{
    bool busy = false;
    int i;

    if (s_cam_up) {
        CAMU_StopCapture(PORT_CAM1);
        while (R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy) {
            svcSleepThread(1000000LL);
        }
        CAMU_ClearBuffer(PORT_CAM1);
        CAMU_Activate(SELECT_NONE);
        camExit();
        s_cam_up = 0;
    }

    /* EV_CANCEL is the UI thread's; it closes it after the join. */
    for (i = EV_RECV; i < EV_COUNT; i++) {
        if (s_ev[i] != 0) {
            svcCloseHandle(s_ev[i]);
            s_ev[i] = 0;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* THE CAPTURE THREAD                                                       */
/* ------------------------------------------------------------------------ */

/* FBI's task_capture_cam_thread(), with this file's two deviations (the retry
 * for a refused arm, and the mode H re-arm order) marked where they are.
 *
 * The loop body is:
 *
 *     svcWaitSynchronizationN(&index, events, 3, false, U64_MAX)
 *     CANCEL -> leave
 *     RECV   -> close the handle; publish; re-arm
 *     ERROR  -> close the handle; clear; re-arm; StartCapture
 *
 * and there is nothing else in it. No drawing, no decode, no staging, no
 * texture upload, no vsync -- that is the entire difference from six rounds of
 * polling pump, and it is why the port is armed again about two milliseconds
 * after each completion instead of one to two sensor frames later.
 *
 * MODE H changes exactly one thing: the re-arm moves ahead of the publish
 * copy, into the other buffer of a ping-pong pair. Neither reference does
 * this; it exists to measure whether the residual ~2 ms still matters. */
static void cam_thread(void *arg)
{
    int cancelled = 0;

    (void)arg;

    if (!cam_bring_up()) {
        cam_tear_down();
        s_th_state = CAM_TH_FAILED;
        LightEvent_Signal(&s_ready);
        return;
    }
    s_th_state = CAM_TH_RUNNING;
    LightEvent_Signal(&s_ready);

    while (!cancelled) {
        s32 index = -1;
        Result rc;

        /* DEVIATION 8. FBI cannot reach this state because it never checks:
         * a refused SetReceiving would leave handle 0 in the wait set and the
         * thread would block forever. Park on the cancel event instead, so B
         * still works, and retry. Hardware has never produced a refusal. */
        if (s_ev[EV_RECV] == 0) {
            arm_receive();
            if (s_ev[EV_RECV] == 0) {
                if (R_SUCCEEDED(svcWaitSynchronization(s_ev[EV_CANCEL],
                                                       CAM_RETRY_NS))) {
                    break;
                }
                continue;
            }
        }

        rc = svcWaitSynchronizationN(&index, s_ev, EV_COUNT, false, U64_MAX);
        if (R_FAILED(rc)) {
            /* Cannot happen with a valid handle set and an infinite timeout;
             * counted rather than ignored so that "the thread is spinning" is
             * visible on the trace instead of only in the battery. */
            s_wait_fails++;
            svcSleepThread(1000000LL);
            continue;
        }

        switch (index) {
            case EV_CANCEL:
                cancelled = 1;
                break;

            case EV_RECV: {
                const u8 *done = s_dma[s_dma_cur];

                s_ev_recv++;
                s_last_ev = 1;
                svcCloseHandle(s_ev[EV_RECV]);
                s_ev[EV_RECV] = 0;

                if (s_mode == APAD3DS_CAM_MODE_H) {
                    /* MODE H: re-arm into the OTHER buffer FIRST, so the port
                     * is unarmed for one IPC call rather than one memcpy.
                     * Nothing races: the copy below reads the buffer the DMA
                     * is no longer pointed at. */
                    s_dma_cur ^= 1;
                    arm_receive();
                    publish(done);
                } else {
                    /* MODE G: FBI's order -- publish, then re-arm. */
                    publish(done);
                    arm_receive();
                }
                break;
            }

            case EV_ERR:
                s_ev_err++;
                s_errors++;
                s_last_ev = 0;
                if (s_first_err_cap == 0u) {
                    s_first_err_cap = s_captures + 1u;
                }
                if (s_ev[EV_RECV] != 0) {
                    svcCloseHandle(s_ev[EV_RECV]);
                    s_ev[EV_RECV] = 0;
                }
                clear_buffer();
                arm_receive();
                start_capture();
                if (R_SUCCEEDED(s_rc_startcap)) {
                    s_recoveries++;
                }
                break;

            default:
                break;
        }
    }

    cam_tear_down();
    s_th_state = CAM_TH_DONE;
}

/* ------------------------------------------------------------------------ */
/* start / stop -- UI THREAD                                                */
/* ------------------------------------------------------------------------ */

static void free_buffers(void)
{
    int i;

    if (s_tex_ready) {
        C3D_TexDelete(&s_tex);
        s_tex_ready = 0;
    }
    if (s_stage != NULL) {
        linearFree(s_stage);
        s_stage = NULL;
    }
    if (s_pub != NULL) {
        linearFree(s_pub);
        s_pub = NULL;
    }
    for (i = 0; i < 2; i++) {
        if (s_dma[i] != NULL) {
            linearFree(s_dma[i]);
            s_dma[i] = NULL;
        }
    }
    if (s_luma != NULL) {
        free(s_luma);
        s_luma = NULL;
    }
    s_luma_valid = 0;
    s_have_frame = 0;
}

void apad3ds_cam_stop(void)
{
    if (s_thread != NULL) {
        /* RESET_STICKY, so this cannot be lost if the thread is between the
         * wait and the switch. The cancel event is in the wait set with an
         * infinite timeout, which is what makes the join bounded even when the
         * sensor has stopped delivering entirely. */
        if (s_ev[EV_CANCEL] != 0) {
            svcSignalEvent(s_ev[EV_CANCEL]);
        }
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    if (s_ev[EV_CANCEL] != 0) {
        svcCloseHandle(s_ev[EV_CANCEL]);
        s_ev[EV_CANCEL] = 0;
    }
    s_running = 0;
    s_th_state = CAM_TH_IDLE;
    /* Safe only because the thread has been joined AND its teardown drained
     * the port with CAMU_IsBusy: nothing can be DMA-ing into these. */
    free_buffers();
}

/* atexit-registered on the first start, the same pattern main.c uses for the
 * gyroscope. The frame loop's `while (aptMainLoop())` can end with no screen
 * transition at all -- that is a real, reported behaviour on this app -- and
 * the QR screen's own teardown would then never run, leaving both a powered
 * camera and a live thread behind. */
static void cam_atexit(void)
{
    apad3ds_cam_stop();
}

/* FBI hardcodes 0x1A. This client ships a .3dsx too, where the allowed
 * priority range comes from whatever host process the Homebrew Launcher is
 * running inside, so a hardcoded value can be refused. Try FBI's, then one
 * step above the caller, then the caller's own -- and record which. */
static int spawn_thread(void)
{
    s32 caller = 0x30;
    int prio = CAM_THREAD_PRIO;

    s_thread = threadCreate(cam_thread, NULL, CAM_THREAD_STACK, prio,
                            CAM_THREAD_CORE, false);
    if (s_thread == NULL) {
        svcGetThreadPriority(&caller, CUR_THREAD_HANDLE);
        prio = (int)caller - 1;
        if (prio < CAM_PRIO_FLOOR) {
            prio = CAM_PRIO_FLOOR;
        }
        s_thread = threadCreate(cam_thread, NULL, CAM_THREAD_STACK, prio,
                                CAM_THREAD_CORE, false);
    }
    if (s_thread == NULL) {
        prio = (int)caller;
        s_thread = threadCreate(cam_thread, NULL, CAM_THREAD_STACK, prio,
                                CAM_THREAD_CORE, false);
    }
    s_thread_prio = (s_thread != NULL) ? prio : -1;
    return s_thread != NULL;
}

int apad3ds_cam_start(void)
{
    Result rc;

    if (s_running) {
        return 1;
    }
    s_status[0] = '\0';
    s_captures = 0;
    s_shown = 0;
    s_missed = 0;
    s_polls = 0;
    s_pub_seq = 0;
    s_seen_seq = 0;
    s_errors = 0;
    s_max_bytes = 0;
    s_xfer_bytes = 0;
    s_rc_xfer_bytes = 0;
    s_rc_setrecv = 0;
    s_rc_clear = 0;
    s_rc_startcap = 0;
    s_arms = 0;
    s_clears = 0;
    s_setrecv_fails = 0;
    s_startcap_fails = 0;
    s_recoveries = 0;
    s_first_err_cap = 0;
    s_ev_recv = 0;
    s_ev_err = 0;
    s_wait_fails = 0;
    s_last_ev = -1;
    s_dma_cur = 0;
    s_thread_prio = -1;
    s_tear_flagged = 0;
    s_tear_measured = 0;
    s_tear_row = -1;
    s_tear_last_max = 0;
    s_tear_last_med = 0;
    s_tear_hi_ratio = 0;
    s_tear_hi_row = -1;
    s_tear_moved = 0;
    s_last_cap_ms = apad_ticks_ms();

    /* --- buffers ------------------------------------------------------- */

    /* linearAlloc, not malloc: s_stage is the SOURCE of a GPU display
     * transfer and therefore has to be physically contiguous. The DMA targets
     * are linear too -- FBI uses plain calloc for its and that does work,
     * Anemone3DS uses linearAlloc -- because it keeps the rule ("anything the
     * hardware touches is linear") uniform and the memcpy between them cheap.
     *
     * The SECOND DMA buffer exists only in mode H. Mode G's footprint is
     * therefore identical to what has been running on hardware all along,
     * which is one fewer difference to attribute a result to. */
    s_dma[0] = (u8 *)linearAlloc(CAM_BYTES);
    s_dma[1] = (s_mode == APAD3DS_CAM_MODE_H)
             ? (u8 *)linearAlloc(CAM_BYTES) : NULL;
    s_pub    = (u8 *)linearAlloc(CAM_BYTES);
    s_stage  = (u16 *)linearAlloc(TEX_BYTES);
    s_luma   = (u8 *)malloc((size_t)CAM_W * (size_t)CAM_H);
    if (s_dma[0] == NULL || s_pub == NULL || s_stage == NULL || s_luma == NULL
        || (s_mode == APAD3DS_CAM_MODE_H && s_dma[1] == NULL)) {
        snprintf(s_status, sizeof s_status, "out of memory for camera buffers");
        free_buffers();
        return 0;
    }
    /* The 112 unused columns and 16 unused rows of the staging buffer are
     * transferred into the texture every frame whatever they hold, so clear
     * them once rather than show uninitialised memory beside the image. */
    memset(s_stage, 0, TEX_BYTES);
    memset(s_pub, 0, CAM_BYTES);
    memset(s_dma[0], 0, CAM_BYTES);
    if (s_dma[1] != NULL) {
        memset(s_dma[1], 0, CAM_BYTES);
    }

    if (!C3D_TexInit(&s_tex, TEX_W, TEX_H, GPU_RGB565)) {
        snprintf(s_status, sizeof s_status, "C3D_TexInit(512x256) failed");
        free_buffers();
        return 0;
    }
    s_tex_ready = 1;
    C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* tex3ds's convention, and it is NOT the intuitive one: `top` is the
     * LARGER v (Tex3DS_SubTextureRotated() is literally `top < bottom`), so
     * v runs bottom-up while the image rows run top-down. The frame occupies
     * memory rows 0..239 of a 256-row texture, hence top = 1.0 and
     * bottom = 1 - 240/256. Hardware confirms this pair is the right way
     * round; backwards draws the viewfinder upside down. */
    s_subtex.width  = CAM_W;
    s_subtex.height = CAM_H;
    s_subtex.left   = 0.0f;
    s_subtex.right  = (float)CAM_W / (float)TEX_W;
    s_subtex.top    = 1.0f;
    s_subtex.bottom = 1.0f - (float)CAM_H / (float)TEX_H;

    /* --- synchronisation, then the thread ------------------------------- */

    LightLock_Init(&s_lock);
    LightEvent_Init(&s_ready, RESET_ONESHOT);

    /* RESET_STICKY, exactly as FBI creates its cancelEvent: a signal raised
     * before the thread reaches its wait must not be lost, or apad3ds_cam_stop
     * would block forever on the join. */
    rc = svcCreateEvent(&s_ev[EV_CANCEL], RESET_STICKY);
    if (R_FAILED(rc)) {
        snprintf(s_status, sizeof s_status, "svcCreateEvent failed: 0x%08lX",
                 (unsigned long)rc);
        free_buffers();
        return 0;
    }

    if (!s_atexit_armed) {
        atexit(cam_atexit);
        s_atexit_armed = 1;
    }

    s_th_state = CAM_TH_STARTING;
    if (!spawn_thread()) {
        snprintf(s_status, sizeof s_status,
                 "threadCreate for the capture thread was refused");
        apad3ds_cam_stop();
        return 0;
    }

    /* Deviation 4/9: wait for the thread's bring-up, bounded. */
    if (LightEvent_WaitTimeout(&s_ready, CAM_READY_TIMEOUT_NS) != 0) {
        snprintf(s_status, sizeof s_status,
                 "camera bring-up did not finish within 5s");
        apad3ds_cam_stop();
        return 0;
    }
    if (s_th_state != CAM_TH_RUNNING) {
        /* s_status was set by whichever call in cam_bring_up() failed. */
        apad3ds_cam_stop();
        return 0;
    }

    s_running = 1;
    return 1;
}

int apad3ds_cam_running(void)
{
    return s_running;
}

/* ------------------------------------------------------------------------ */
/* the mode switch                                                          */
/* ------------------------------------------------------------------------ */

int apad3ds_cam_mode(void)
{
    return s_mode;
}

/* The first character is the mode letter; the scan screen prints just that
 * where it has no room for the rest. */
const char *apad3ds_cam_mode_name(int mode)
{
    switch (mode) {
        case APAD3DS_CAM_MODE_H: return "H: thread + ping-pong, arm then copy";
        default:                 return "G: thread, FBI verbatim (copy, arm)";
    }
}

/* A FULL RESTART, not a flag flip: the thread is joined, the camera is torn
 * down and everything is brought back up. Every counter on the trace has to
 * belong to the mode named beside it, and mode H needs a buffer mode G does
 * not allocate. The mode itself is the one piece of state start() does not
 * reset, or this would switch straight back. */
void apad3ds_cam_mode_next(void)
{
    int next = (s_mode + 1) % APAD3DS_CAM_MODE_COUNT;

    apad3ds_cam_stop();
    s_mode = next;
    s_mode_switches++;
    (void)apad3ds_cam_start();
}

/* ------------------------------------------------------------------------ */
/* frame -> texture -- UI THREAD                                            */
/* ------------------------------------------------------------------------ */

/* One RGB565 texel to one BT.601 luma sample, 0..250.
 *
 * IT IS A FUNCTION SO THAT THE TEAR METRIC AND THE DECODER CANNOT DISAGREE.
 * apad3ds_cam_luma() builds quirc's plane out of this; measure_tear() judges
 * seams out of this; both read s_stage. "The metric runs on the luma the
 * decode path already produces" is then a property of the code rather than a
 * claim in a comment. See apad3ds_cam_luma() for why the weights are on
 * expanded 5/6/5 channels and why the channel order does not matter. */
static inline unsigned luma565(unsigned p)
{
    unsigned r = (p >> 11) & 0x1Fu;   /* 5 bits -> <<3 */
    unsigned g = (p >> 5) & 0x3Fu;    /* 6 bits -> <<2 */
    unsigned b = p & 0x1Fu;           /* 5 bits -> <<3 */

    return (77u * (r << 3) + 151u * (g << 2) + 28u * (b << 3)) >> 8;
}

/* THE TEAR METRIC: does this staged frame contain a splice seam?
 *
 * Rounds 1 through 5 were each adjudicated by a person looking at a 3.5 inch
 * screen and saying "still tears", which cannot be photographed, cannot be
 * compared against a run an hour earlier, and cannot tell "much better" from
 * "fixed". This can, and round 6 proved it on hardware: 796/1639 flagged with
 * mv 431 for the shipped loop. It is UNCHANGED in round 7 -- deliberately, so
 * that this round's reading is comparable with that one to the digit.
 *
 * Per adjacent row pair -- ALL of them, see the TEAR_* block above for why
 * subsampling rows was measured and rejected:
 *
 *   d[x]  = luma(row y) - luma(row y+1), for every 8th column
 *   dev   = mean |d[x] - mean(d)|          <- DC removed: a brightness step
 *                                             is content, a decorrelation is
 *                                             a seam
 *
 * The frame is flagged when the LARGEST dev of any pair beats both FLOOR (an
 * absolute floor, because a blank wall has a median near zero and would
 * otherwise flag on sensor noise) and FACTOR x the frame's own MEDIAN dev
 * (scale-free, so bright, dark and busy scenes are each judged against their
 * own texture rather than a constant nobody could pick).
 *
 * A median and not a mean: one anomalous pair out of 239 moves a median not
 * at all, which is the entire reason the frame can be its own reference. It
 * is taken from a 256-bin histogram rather than by sorting -- O(n) with a
 * fixed 512-byte table, against an insertion sort of 239 elements that would
 * have cost more than the measurement.
 *
 * `s_tear_moved` is the metric's own honesty check. Its one known false
 * positive is a horizontal object edge crossing part of the width, which
 * flags at THE SAME ROW in every frame; a splice lands at a different row
 * every time. Flags without movement are furniture, not tearing. */
static void measure_tear(void)
{
    u8 rowa[TEAR_COLS];
    u8 rowb[TEAR_COLS];
    u8 *prev = rowa;
    u8 *cur = rowb;
    u16 hist[256];
    int d[TEAR_COLS];
    unsigned n = 0u;
    unsigned max = 0u;
    unsigned cum;
    unsigned med = 0u;
    unsigned i;
    int max_row = 0;
    int y, x, k;

    memset(hist, 0, sizeof hist);

    k = 0;
    for (x = 0; x < CAM_W; x += TEAR_COL_STEP) {
        prev[k++] = (u8)luma565(s_stage[x]);
    }

    for (y = 0; y + 1 < CAM_H; y++) {
        const u16 *src = s_stage + (size_t)(y + 1) * TEX_W;
        long sum = 0;
        unsigned dev = 0u;
        int mean;
        u8 *swap;

        k = 0;
        for (x = 0; x < CAM_W; x += TEAR_COL_STEP) {
            cur[k++] = (u8)luma565(src[x]);
        }

        for (i = 0u; i < (unsigned)k; i++) {
            d[i] = (int)prev[i] - (int)cur[i];
            sum += d[i];
        }
        mean = (int)(sum / k);
        for (i = 0u; i < (unsigned)k; i++) {
            int e = d[i] - mean;

            dev += (unsigned)((e < 0) ? -e : e);
        }
        dev /= (unsigned)k;
        if (dev > 255u) {
            dev = 255u;                   /* cannot happen; the bin must exist */
        }
        hist[dev]++;
        n++;
        if (dev > max) {
            max = dev;
            max_row = y;
        }

        /* Roll: this row becomes the next pair's upper row, so each row's
         * luma is computed once and used twice. */
        swap = prev;
        prev = cur;
        cur = swap;
    }
    if (n == 0u) {
        return;
    }

    cum = 0u;
    for (i = 0u; i < 256u; i++) {
        cum += hist[i];
        if (cum > n / 2u) {
            med = i;
            break;
        }
    }

    s_tear_measured++;
    s_tear_last_max = max;
    s_tear_last_med = med;

    if (max >= TEAR_FLOOR) {
        unsigned ratio = max / ((med > 0u) ? med : 1u);

        if (ratio > s_tear_hi_ratio) {
            s_tear_hi_ratio = ratio;
            s_tear_hi_row = max_row;
        }
        if (max > TEAR_FACTOR * med) {
            if (s_tear_flagged > 0u && max_row != s_tear_row) {
                s_tear_moved++;
            }
            s_tear_flagged++;
            s_tear_row = max_row;
        }
    }
}

/* WHY A STAGING COPY AND NOT A DIRECT TRANSFER FROM THE PUBLISHED FRAME.
 *
 * The display-transfer engine reads a linear surface and writes a tiled one,
 * and the safe way to drive it is with the input and output dimensions EQUAL.
 * The published frame is 400 pixels per row; the texture is 512. Rather than
 * hand the engine a mismatched pair and hope its stride handling does what the
 * name suggests -- on hardware nobody here can single-step -- the frame is
 * copied row by row into a buffer that genuinely is 512x256, and the transfer
 * is 512x256 -> 512x256. That is 240 memcpys of 800 bytes, once per CAMERA
 * frame (30 Hz), not per display frame.
 *
 * The copy earns its keep twice over: it is also the stable snapshot the
 * grayscale repack reads from, so quirc can take as long as it likes over a
 * frame no other thread will touch. Measured on a New 3DS: 22 ms per decode
 * against a 33 ms sensor period.
 *
 * THE ROW LOOP IS INSIDE THE LOCK and the rest is not. The lock protects the
 * published frame; the tear metric, the cache flush and the display transfer
 * all read s_stage, which is this thread's alone. That is what keeps the
 * capture thread's worst-case wait at one memcpy rather than at a memcpy plus
 * a GPU transfer. */
static int stage_frame(void)
{
    unsigned seq;
    int y;

    LightLock_Lock(&s_lock);
    seq = s_pub_seq;
    if (seq == s_seen_seq) {
        LightLock_Unlock(&s_lock);
        return 0;
    }
    for (y = 0; y < CAM_H; y++) {
        memcpy(s_stage + (size_t)y * TEX_W,
               s_pub + (size_t)y * CAM_W * CAM_BPP,
               (size_t)CAM_W * CAM_BPP);
    }
    /* Publications this thread never looked at. At 30 fps published against a
     * 60 Hz consumer this should stay 0; it climbs when the UI thread is busy,
     * which in this app means a quirc decode ran long. It is a load readout,
     * not a fault. */
    s_missed += (seq - s_seen_seq) - 1u;
    s_seen_seq = seq;
    LightLock_Unlock(&s_lock);

    /* Judge the seam BEFORE handing the buffer to the GPU: the rows are still
     * in cache from the copy that just happened. */
    measure_tear();

    /* The CPU just wrote s_stage; the GPU is about to read it. */
    GSPGPU_FlushDataCache(s_stage, TEX_BYTES);
    C3D_SyncDisplayTransfer((u32 *)s_stage, GX_BUFFER_DIM(TEX_W, TEX_H),
                            (u32 *)s_tex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
                            CAM_TEX_XFER_FLAGS);
    /* Deliberately NO C3D_TexFlush() here. That flushes the CPU data cache
     * over the texture, and the bytes in it were just written by the GPU --
     * pushing a stale CPU line back over them is the one way to corrupt what
     * we just uploaded. The CPU never reads or writes s_tex.data. */

    s_have_frame = 1;
    s_luma_valid = 0;
    s_shown++;
    return 1;
}

int apad3ds_cam_poll(void)
{
    if (!s_running) {
        return 0;
    }
    s_polls++;
    return stage_frame();
}

C2D_Image apad3ds_cam_image(void)
{
    C2D_Image img;

    img.tex = &s_tex;
    img.subtex = &s_subtex;
    return img;
}

/* ------------------------------------------------------------------------ */
/* frame -> grayscale, for quirc                                            */
/* ------------------------------------------------------------------------ */

/* quirc wants one tightly packed 8-bit plane (clients/vendor/quirc/README.md,
 * "No colour conversion needed"), and the sensor is handing us RGB565, so
 * somebody has to do this multiply. It is done HERE, on demand, rather than
 * in the capture thread: the viewfinder must stay fluid at 30 fps and a decode
 * attempt only happens a few times a second, so paying ~96k pixels of luma
 * maths on every captured frame would be most of the cost for none of the
 * benefit -- and it would be paid on the thread whose entire job is to re-arm
 * the port quickly.
 *
 * BT.601 weights on the expanded channels. R and B carry 5 bits, G carries 6,
 * so a full-white pixel lands at 250 rather than 255 -- irrelevant to a
 * recognizer that is looking for the boundary between black and white
 * modules, and not worth a second multiply to correct.
 *
 * If the sensor's "RGB_565" turns out to be ordered the other way round, the
 * only consequence here is that 77 and 28 are on the wrong channels: a QR
 * code is neutral, so its luma is unchanged either way and DECODING CANNOT
 * BE AFFECTED -- 139 decodes on hardware agree. */
const uint8_t *apad3ds_cam_luma(void)
{
    int x, y;

    if (!s_have_frame || s_luma == NULL) {
        return NULL;
    }
    if (s_luma_valid) {
        return s_luma;
    }

    for (y = 0; y < CAM_H; y++) {
        const u16 *src = s_stage + (size_t)y * TEX_W;
        u8 *dst = s_luma + (size_t)y * CAM_W;

        for (x = 0; x < CAM_W; x++) {
            dst[x] = (u8)luma565(src[x]);
        }
    }
    s_luma_valid = 1;
    return s_luma;
}

/* ------------------------------------------------------------------------ */
/* diagnostics                                                              */
/* ------------------------------------------------------------------------ */

const char *apad3ds_cam_status(void)
{
    return (s_status[0] != '\0') ? s_status : NULL;
}

/* DISPLAYED frames -- the ones that reached the texture. Deliberately not the
 * capture count: this is what the scan screen gates its viewfinder draw on,
 * and drawing a texture nothing was ever staged into shows uninitialised GPU
 * memory. Round 7 removed the display filter that used to hold the two
 * counters apart, so they should now track each other closely; the gap that is
 * left is `miss` on the trace. */
unsigned apad3ds_cam_frames(void)
{
    return s_shown;
}

unsigned apad3ds_cam_errors(void)
{
    return s_errors;
}

/* Milliseconds since the last receive that COMPLETED, or since the camera
 * started if none ever has. Capture liveness, not display liveness. Through
 * apad_time_since(), not a bare subtraction: apad_ticks_ms() wraps and
 * core/src/seq.c owns that arithmetic for every client (docs/CONVENTIONS.md). */
/* THE `s_thread == NULL` SHORT CIRCUIT IS NOT AN OPTIMISATION, IT IS THE
 * CORRECTNESS CONDITION. A LightLock is a plain s32 whose UNLOCKED value is 1,
 * so a statically zero-initialised one reads as LOCKED-with-no-waiters and
 * LightLock_Lock() on it never returns. LightLock_Init() runs in
 * apad3ds_cam_start(), but the trace and the health gate are read by the scan
 * screen BEFORE that -- QR_ARM draws a frame first, on purpose -- and after
 * apad3ds_cam_stop() has freed everything. With no capture thread in
 * existence there is no second writer, so reading these unlocked is not merely
 * safe, it is the only thing that can be done. */
static uint32_t idle_ms(void)
{
    uint32_t last;

    if (s_thread == NULL) {
        last = s_last_cap_ms;
    } else {
        LightLock_Lock(&s_lock);
        last = s_last_cap_ms;
        LightLock_Unlock(&s_lock);
    }
    return apad_time_since(apad_ticks_ms(), last);
}

static unsigned captures(void)
{
    unsigned n;

    if (s_thread == NULL) {
        return s_captures;
    }
    LightLock_Lock(&s_lock);
    n = s_captures;
    LightLock_Unlock(&s_lock);
    return n;
}

/* IS THE PIPELINE UNHEALTHY -- i.e. should the trace be on screen.
 *
 *   caps < 30         nothing has settled yet, or nothing ever will. One
 *                     second of capture at 30 fps.
 *   errors > caps/2   THE ERROR RATE, and the threshold comes from
 *                     measurement rather than from hope. The polling pump's
 *                     measured rate was 20 to 24 per 100 captured frames, and
 *                     at that rate the viewfinder was live and quirc decoded
 *                     -- so a HEALTHY run must not trip this. An earlier
 *                     errors > frames/8 term, written when zero was believed
 *                     reachable, would have pinned the trace over a working
 *                     camera forever. Twice the measured rate is the line.
 *                     ROUND 7 MAY MAKE THIS RATE COLLAPSE: a buffer error is
 *                     the port complaining that nothing was armed to receive
 *                     into, and a thread that re-arms in 2 ms gives it far
 *                     less to complain about. A big drop here is corroborating
 *                     evidence for the whole diagnosis, not a separate win.
 *   idle > 1 s        frames HAVE landed and then stopped. The term the dead
 *                     ping-pong shape needed and did not have: it stuck at
 *                     `frames == 1`, which no counter-based gate catches. It
 *                     is also the ONLY term that can catch a capture thread
 *                     that has parked forever on its infinite wait, which is
 *                     the new failure mode round 7 introduces.
 *
 * The round-6 starvation term is gone with the display filter it watched.
 *
 * X on the scan screen pins the trace up regardless, for reading the numbers
 * off a camera that is behaving. */
int apad3ds_cam_unhealthy(void)
{
    unsigned caps = captures();

    if (!s_running) {
        return 1;
    }
    if (caps < 30u) {
        return 1;
    }
    if (s_errors > (caps >> 1)) {
        return 1;
    }
    if (idle_ms() > 1000u) {
        return 1;
    }
    return 0;
}

static const char *state_tag(void)
{
    if (!s_running) {
        return "stopped";
    }
    switch (s_th_state) {
        case CAM_TH_STARTING: return "starting";
        case CAM_TH_FAILED:   return "failed";
        case CAM_TH_DONE:     return "exited";
        default:              break;
    }
    return (s_ev[EV_RECV] == 0) ? "unarmed" : "armed";
}

/* Errors per hundred CAPTURED frames, integer. It is the one number that says
 * whether this console is in the regime the loop was measured in: the polling
 * pump gave 20 to 24 per hundred with a live viewfinder throughout. A reading
 * in the hundreds is a different failure, and the lines above say which. */
static unsigned err_per_100(void)
{
    unsigned caps = captures();

    if (caps == 0u) {
        return 0u;
    }
    return (s_errors * 100u) / caps;
}

/* THE TRACE. Eight lines, each one a different question, so that ONE netload
 * settles which of them is happening:
 *
 *   0  WHICH MODE the numbers below belong to, and how many times the mode
 *      has been switched this session. Every counter is reset by the switch,
 *      so a photograph of this trace is self-describing.
 *   1  is it moving. State tag, CAPTURED frames, how many of those were
 *      SHOWN, errors, and how long since a receive completed. `cap` and
 *      `shown` should now be within a frame or two of each other; if `cap`
 *      climbs and `shown` does not, the UI thread has stopped consuming and
 *      the capture thread is fine, which is a new failure mode and one no
 *      earlier round could even express.
 *   2  IS THE RE-ARM BEING REFUSED. `arms` climbing with `refused 0` means
 *      every SetReceiving was ACCEPTED and the DMA still never completed -- a
 *      different bug from a port that says no, and indistinguishable from
 *      outside. Healthy: arms ~= captures + errors + 1 (the pre-arm), refused
 *      0. `1st err@cap` is which capture the FIRST buffer error landed on:
 *      `@1` means bring-up itself raised it, which round 3 saw correlate with
 *      a ClearBuffer inside the loop -- there is no longer one, so a `@1` here
 *      would be new information.
 *   3  THE CAPTURE THREAD ITSELF, which is round 7's whole change: the
 *      priority threadCreate actually accepted (0x1A is FBI's; anything else
 *      means the first two attempts were refused and the thread may not be
 *      preempting the UI at all), the core, and the count of waits that
 *      returned an error. `prio -1` with a `stopped` state on line 1 means no
 *      thread was created.
 *   4  the calls that repair the port, with their last Result and their
 *      counts. `clears` should read about 1 + errors for a whole run -- once
 *      at bring-up, once per buffer error, once at teardown -- because round 7
 *      took ClearBuffer back out of the steady loop, where neither FBI nor
 *      Anemone3DS has it. `restarts` tracks errors, which is the self-healing
 *      made visible.
 *   5  WHICH EVENT IS FIRING, and the PUBLICATION HANDOFF: `pub` is the
 *      sequence the capture thread has published, `seen` is what the UI has
 *      staged, `miss` counts publications the UI never looked at. `pub`
 *      climbing with `seen` flat is a UI thread that stopped polling; `miss`
 *      climbing steadily means decodes are eating the frame loop.
 *   6  the byte counts, none of which anyone here can compute. A transferUnit
 *      the s16 cast mangled, or a port that kept a different byte count than
 *      it was given, would look exactly like every other failure once the only
 *      symptom is a black screen. Plus the error rate per hundred CAPTURED
 *      frames against the polling pump's measured 20-24.
 *   7  THE TEAR METRIC, which is this round's verdict and the same metric
 *      round 6 measured with, unchanged so the two are comparable to the
 *      digit. `tear FLAGGED/MEASURED` over STAGED frames, the row the most
 *      recent seam was found at, the last measured frame's raw max and median
 *      row difference, and the highest max/median RATIO of the whole run with
 *      its row. THE WIN CONDITION IS `tear 0/M` WITH A LIVE VIEWFINDER (M in
 *      the hundreds and `shown` climbing) -- `0/0` means nothing was ever
 *      staged and is a failure, not a pass. ROUND 6'S MODE F READ 796/1639
 *      (49%) WITH mv 431; that is the number to beat and it is printed beside
 *      the new one. `mv` counts flags that landed on a DIFFERENT row from the
 *      previous flag: seams wander, furniture does not, so `tear 300/300 mv 2`
 *      is a horizontal edge in the scene and not a tear -- move the console
 *      and read it again. */
const char *apad3ds_cam_debug(unsigned line)
{
    const char *ev = (s_last_ev == 1) ? "frame"
                   : (s_last_ev == 0) ? "error"
                                      : "none";

    switch (line) {
        case 0:
            snprintf(s_dbg[0], sizeof s_dbg[0],
                     "MODE %s  (Y cycles, sw %u)",
                     apad3ds_cam_mode_name(s_mode), s_mode_switches);
            break;
        case 1:
            snprintf(s_dbg[1], sizeof s_dbg[1],
                     "%s  cap %u  shown %u  errs %u  idle %lu ms",
                     state_tag(), captures(), s_shown, s_errors,
                     (unsigned long)idle_ms());
            break;
        case 2:
            snprintf(s_dbg[2], sizeof s_dbg[2],
                     "SetRecv %08lX  arms %u  refused %u  1st err@cap %u",
                     (unsigned long)s_rc_setrecv, s_arms, s_setrecv_fails,
                     s_first_err_cap);
            break;
        case 3:
            snprintf(s_dbg[3], sizeof s_dbg[3],
                     "thread prio 0x%02X (FBI 0x1A) core %d  waitfail %u",
                     (unsigned)(s_thread_prio & 0xFF), CAM_THREAD_CORE,
                     s_wait_fails);
            break;
        case 4:
            snprintf(s_dbg[4], sizeof s_dbg[4],
                     "Clear %08lX n%u  StartCap %08lX f%u  restarts %u",
                     (unsigned long)s_rc_clear, s_clears,
                     (unsigned long)s_rc_startcap, s_startcap_fails,
                     s_recoveries);
            break;
        case 5:
            snprintf(s_dbg[5], sizeof s_dbg[5],
                     "ev recv %u err %u last %s  pub %u seen %u miss %u",
                     s_ev_recv, s_ev_err, ev, s_pub_seq, s_seen_seq,
                     s_missed);
            break;
        case 6:
            snprintf(s_dbg[6], sizeof s_dbg[6],
                     "max %lu port %lu unit %d  errs/100 %u (poll ran 20-24)",
                     (unsigned long)s_max_bytes, (unsigned long)s_xfer_bytes,
                     (int)s_transfer_unit, err_per_100());
            break;
        case 7:
            snprintf(s_dbg[7], sizeof s_dbg[7],
                     "tear %u/%u r%d mv %u  max %u med %u  hi x%u (F 49%%)",
                     s_tear_flagged, s_tear_measured, s_tear_row,
                     s_tear_moved, s_tear_last_max, s_tear_last_med,
                     s_tear_hi_ratio);
            break;
        default:
            return NULL;
    }
    return s_dbg[line];
}

/* ------------------------------------------------------------------------ */
/* STILL UNVERIFIED ON HARDWARE (round 7)                                   */
/* ------------------------------------------------------------------------ */

/* - Whether the capture thread fixes the tear at all. That is the whole
 *   point of the netload; mode F's 796/1639 at mv 431 is the number to beat.
 * - Whether threadCreate accepts priority 0x1A under the Homebrew Launcher
 *   (.3dsx path). Line 3 of the trace answers it. The .cia path has its own
 *   exheader and should be FBI's case exactly.
 * - Behaviour across an APT suspend/resume. FBI parks its capture thread on a
 *   pause event; this app has none (deviation 7), so a HOME press leaves the
 *   thread running against a camera the system may take away. The expected
 *   outcome is a burst of buffer errors and self-recovery, which is what the
 *   error branch is for -- but nobody has watched it.
 * - Whether s_rc_xfer_bytes / GetTransferBytes still reads back the same value
 *   now that SetTransferBytes runs on an ACTIVATED port (FBI's order) rather
 *   than before Activate (the sample's). Line 6 shows both numbers.
 * - Whether mode H's second linearAlloc succeeds on an Old 3DS (192 KB more
 *   linear heap). Mode G is unchanged in footprint.
 * - R/B channel order (cosmetic only -- a QR code is neutral, so decoding
 *   cannot be affected).
 */
