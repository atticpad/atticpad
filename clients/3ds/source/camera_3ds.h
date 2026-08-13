/* clients/3ds/source/camera_3ds.h
 *
 * CAMU bring-up and a DEDICATED CAPTURE THREAD, kept out of the screen file
 * for the same reason soc_3ds.c is kept out of main.c: a platform service
 * with a fiddly activation order is its own thing, and the screen above it
 * should read as UI.
 *
 * ROUND 7 REPLACED THE POLLING PUMP WITH A CAPTURE THREAD, AFTER READING
 * BATTLE-TESTED CODE INSTEAD OF ITERATING BLIND.
 *
 * Six hardware rounds established that a capture loop driven from inside the
 * 60 Hz citro2d frame loop tears in EVERY variant that was tried: at the top
 * of the poll (49% of frames flagged by the tear metric), with a ClearBuffer
 * before every arm (worse error rate, still torn), and re-arming immediately
 * on completion (70% flagged). Rounds 5 and 6 each suppressed their target
 * mechanism completely and the artefact survived, which is the pre-registered
 * "the theory is wrong" signature twice over.
 *
 * So round 7 went and read the two most-run camera implementations in 3DS
 * homebrew -- Steveice10's FBI (source/core/task/capturecam.c, MIT) and
 * Anemone3DS (source/camera.c, GPLv3, an independent derivative) -- and they
 * agree with each other on every structural point and disagree with this file
 * on ALL FOUR of them:
 *
 *   1. THE CAPTURE LOOP IS ITS OWN THREAD, blocking on the receive event with
 *      an INFINITE timeout. It is woken by the kernel microseconds after the
 *      DMA completes. This file used to notice a completed receive up to a
 *      whole display frame late, because the only thing that looked was a
 *      10 ms bounded wait once per 16.7 ms UI frame.
 *   2. THE PORT IS RE-ARMED WITHIN ~2 ms OF EVERY COMPLETION -- one 192 KB
 *      memcpy into a published buffer and a SetReceiving. This file re-armed
 *      after a staging copy, a GPU display transfer, two screens of drawing
 *      and (a few times a second) a 22 ms quirc decode: an unarmed window of
 *      16-55 ms, which is one to two whole sensor frames at 30 fps, arriving
 *      at an arbitrary phase every single time.
 *   3. THE FIRST RECEIVE IS ARMED BEFORE THE FIRST StartCapture. Both
 *      references pre-arm; this file deliberately did not, on the strength of
 *      the devkitPro sample. A pre-armed StartCapture is the one moment the
 *      DMA is KNOWN to begin at a frame boundary, and (1) and (2) are what
 *      keep that alignment for the rest of the run.
 *   4. THE RECEIVE EVENT IS CHECKED BEFORE THE BUFFER-ERROR EVENT.
 *      svcWaitSynchronizationN with waitAll=false returns the LOWEST signalled
 *      index, and both references put recv first, so a completed frame is
 *      always consumed and the error is handled on the next iteration. This
 *      file put the error first and threw the completed frame away.
 *
 * The display filter that round 5 added (withhold the frame received across a
 * recovery restart) IS GONE: neither reference has anything like it, and round
 * 5 proved on hardware that suppressing it entirely does not touch the tear.
 * So are the three round-6 capture disciplines. What is left is one loop --
 * FBI's -- plus one controlled variant, and the tear metric that will say
 * which.
 *
 * The whole camera path is UNVERIFIED BY ANY EMULATOR -- no 3DS emulator
 * implements CAMU -- so every call sequence is transcribed from FBI, from
 * Anemone3DS and from references/3ds/camera-video/main.c rather than written
 * from memory, and the deviations are named in camera_3ds.c's header comment.
 */
#ifndef ATTICPAD_3DS_CAMERA_H
#define ATTICPAD_3DS_CAMERA_H

#include <stdint.h>

#include <3ds.h>
#include <citro2d.h>

/* SIZE_CTR_TOP_LCD. The one capture size that is exactly the top screen, so
 * the viewfinder is 1:1 and no scaling is needed to judge framing. */
#define APAD3DS_CAM_W 400
#define APAD3DS_CAM_H 240

/* Acquires cam:u, configures the outer-left camera, starts capture and starts
 * the capture thread. Returns 1 on success; on failure returns 0 and
 * apad3ds_cam_status() says which call failed and with what Result.
 * Safe to call when already running (returns 1 immediately).
 *
 * IT BLOCKS UNTIL THE CAPTURE THREAD HAS FINISHED BRING-UP, which is a
 * deliberate deviation from FBI (whose task_capture_cam() returns the moment
 * the thread exists and reports failure asynchronously through a `finished`
 * flag the UI polls). The synchronous facade is worth one LightEvent because
 * it keeps the caller's shape -- screen_qrscan.c's QR_ARM -> QR_SCAN /
 * QR_FAILED decision -- unchanged, and because bring-up is exactly the run of
 * blocking service calls that QR_ARM already exists to draw a frame in front
 * of. Bounded at five seconds; a bring-up that hangs past that is reported as
 * a failure rather than freezing the app. */
int apad3ds_cam_start(void);

/* Signals the capture thread to stop, JOINS IT, then releases every buffer.
 * The thread owns the CAMU teardown (StopCapture, drain IsBusy, ClearBuffer,
 * Activate(SELECT_NONE), camExit) and runs it before it returns, so when this
 * function returns there is no thread and no camera. Idempotent.
 *
 * ALSO registered with atexit() on the first start, because aptMainLoop() can
 * go false under this app (HOME, power) with no screen transition to run
 * teardown -- and a camera left activated drains the battery and locks out the
 * system camera applet, while a capture thread left running is a thread
 * touching cam:u after main() returned. The cancel event is in the thread's
 * wait set, so the join always completes even if the sensor has stopped
 * delivering. */
void apad3ds_cam_stop(void);

int apad3ds_cam_running(void);

/* ------------------------------------------------------------------------ */
/* the capture mode -- ROUND 7 A/B, comes out when hardware picks one        */
/* ------------------------------------------------------------------------ */

/* TWO disciplines, not three, and they differ in ONE line:
 *
 *   G  FBI's loop, transliterated. One DMA buffer; on completion, copy it into
 *      the published buffer and THEN re-arm. Unarmed window ~= one 192 KB
 *      memcpy. THIS IS THE SHIPPING CANDIDATE -- it is what runs on millions
 *      of consoles.
 *   H  G with a ping-pong DMA PAIR: on completion, re-arm into the OTHER
 *      buffer first and copy the completed one afterwards. Unarmed window ~=
 *      one IPC call. Nothing in either reference does this; H exists only to
 *      measure whether the residual gap still contributes, and it is the only
 *      thing worth trying next if G's tear count is low but not zero.
 *
 * Two modes in one build rather than two builds is the pattern round 3 and
 * round 6 both validated: the only instrument this code has is a person
 * holding a console, and a mode that has to be rebuilt and re-netloaded to be
 * compared costs a session per data point. */
enum {
    APAD3DS_CAM_MODE_G = 0,
    APAD3DS_CAM_MODE_H,
    APAD3DS_CAM_MODE_COUNT
};

int         apad3ds_cam_mode(void);

/* Human-readable, and its FIRST CHARACTER IS THE MODE LETTER -- the scan
 * screen prints just that where a full line will not fit. */
const char *apad3ds_cam_mode_name(int mode);

/* Advance to the next mode. THIS STOPS AND RESTARTS THE CAMERA (thread and
 * all): every counter on the trace, including the tear tally, has to belong to
 * the mode named beside it. It is therefore a BLOCKING sequence -- a thread
 * join plus camExit/camInit plus a dozen CAMU calls -- so treat it exactly
 * like any other blocking work on a screen, i.e. app_disarm() after it
 * (app.h). */
void        apad3ds_cam_mode_next(void);

/* Takes the newest published frame, if there is one the caller has not seen.
 * Returns 1 when a new frame was staged -- the viewfinder texture has been
 * refreshed and apad3ds_cam_luma() will return that frame -- and 0 otherwise,
 * which is the ordinary outcome on about half of all display frames since the
 * camera runs at 30 fps against a 60 Hz frame loop.
 *
 * IT NO LONGER DRIVES THE PORT, AND IT NO LONGER BLOCKS ON THE CAMERA. Before
 * round 7 this call armed the DMA, waited up to 10 ms on the receive event and
 * retired the completed receive; skipping one call stalled capture outright.
 * The capture thread does all of that now. What is left here is a sequence
 * check and, when it fires, a 192 KB copy plus a GPU display transfer -- so
 * the only lock it takes is the publication lock, and the only thread it can
 * delay is the capture thread's next memcpy, by about two milliseconds.
 *
 * Still call it once per frame while running: it is what refreshes the
 * viewfinder, and frames that are published while nobody consumes them are
 * counted as `miss` on the trace. */
int apad3ds_cam_poll(void);

/* The viewfinder, as a C2D_Image (a 512x256 GPU_RGB565 texture with a
 * 400x240 sub-rectangle). Valid only while running. */
C2D_Image apad3ds_cam_image(void);

/* The last staged frame repacked as a TIGHT 8-bit grayscale plane,
 * APAD3DS_CAM_W bytes per row, APAD3DS_CAM_H rows -- exactly what
 * clients/common/apad_qr.c hands to quirc. Repacked on demand, so calling
 * this is the expensive part and it is deliberately not done on every frame.
 * NULL before the first frame.
 *
 * It reads the STAGING buffer, which only apad3ds_cam_poll() writes and only
 * this thread reads, so a decode that takes 22 ms is looking at a stable
 * snapshot the capture thread stopped owning the moment it was staged. */
const uint8_t *apad3ds_cam_luma(void);

/* ------------------------------------------------------------------------ */
/* diagnostics                                                              */
/* ------------------------------------------------------------------------ */

/* A status/error string (NULL when healthy), frames staged, and buffer-error
 * interrupts recovered from.
 *
 * apad3ds_cam_frames() is the DISPLAYED count -- frames that reached the
 * texture. The capture count is on the trace beside it; the two should now
 * track each other closely, because round 7 removed the display filter that
 * used to hold them apart.
 *
 * apad3ds_cam_errors() CLIMBING SLOWLY IS NORMAL AND IS NOT A FAULT. Measured
 * on a New 3DS with the polling pump (2026-08-11): 20 to 24 per 100 captured
 * frames, while the viewfinder stayed live and quirc decoded 139 codes at
 * 22 ms each. Each error means "capture stopped" and each one is repaired in
 * the capture thread by ClearBuffer -> SetReceiving -> StartCapture, which is
 * FBI's recovery verbatim. THE RATE IS EXPECTED TO CHANGE IN ROUND 7 and it is
 * one of the two numbers to read first: an error is the port complaining that
 * nothing was armed to receive into, and the whole point of the thread is that
 * something almost always is. A count that outruns captures, or a capture
 * counter that stops, is the real fault -- apad3ds_cam_unhealthy() encodes
 * exactly that distinction. */
const char *apad3ds_cam_status(void);
unsigned    apad3ds_cam_frames(void);
unsigned    apad3ds_cam_errors(void);

/* IS THE PIPELINE UNHEALTHY. 1 while the camera is stopped, while fewer than
 * 30 frames have been captured, while errors exceed half the capture count
 * (twice the rate the polling pump measured), or when more than a second has
 * passed with no receive completing.
 *
 * THIS IS THE TRACE'S VISIBILITY GATE, and it exists because an earlier gate
 * was `frames == 0`: that failure stuck at `frames == 1`, so the viewfinder
 * owned the screen for the whole session and every diagnostic number was
 * invisible while the thing was visibly broken. Health is a property of the
 * pipeline, not of the frame counter. See camera_3ds.c for the exact terms
 * and where each threshold's number came from. */
int apad3ds_cam_unhealthy(void);

/* THE CAPTURE TRACE. `line` in [0, APAD3DS_CAM_DEBUG_LINES); NULL past the
 * end. The loop's identity and the capture thread's real priority/core, the
 * byte counts, the last Result and call count of every call in the receive
 * lifecycle that can fail without stopping anything, per-event counters, the
 * publication handoff's own counters, and the tear metric -- see the comment
 * on the definition in camera_3ds.c for what each line answers and what
 * signature indicts what.
 *
 * This exists because a 3DS is the slowest debugger in the project: no
 * emulator implements CAMU, so the only instrument is a person holding the
 * console reading the screen back. A build that answers six questions beats
 * six builds that answer one (the M2 lesson). Cheap
 * enough to keep permanently: formatted only when drawn. */
#define APAD3DS_CAM_DEBUG_LINES 8
const char *apad3ds_cam_debug(unsigned line);

#endif /* ATTICPAD_3DS_CAMERA_H */
