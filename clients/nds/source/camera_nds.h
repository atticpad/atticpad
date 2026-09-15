/* clients/nds/source/camera_nds.h
 *
 * DSi camera bring-up for the QR pairing screen (screen_qrscan.c). MIRRORS
 * references/nds/examples/peripherals/camera/source/main.c call for call --
 * cameraInit() / cameraSelect(CAMERA_OUTER) / cameraStartTransfer() /
 * cameraStopTransfer() / cameraDeinit() -- and NOT clients/3ds/source/
 * camera_3ds.c's threaded capture task, which this file deliberately does not
 * follow. That is a large, named deviation, not an oversight:
 *
 *   - The 3DS's camera is a shared IPC service (CAMU) driven by the ARM11's
 *     second core; six hardware rounds documented in camera_3ds.c found that
 *     a capture loop driven from inside the UI frame tears, and the fix was a
 *     dedicated thread woken by a kernel event within microseconds of DMA
 *     completion.
 *   - The DSi's camera is a memory-mapped register block (nds/arm9/camera.h)
 *     moved by NDMA hardware, with no IPC, no second core to contend with,
 *     and no per-completion event to miss -- cameraStartTransfer() arms one
 *     NDMA transfer and cameraTransferActive()/ndmaBusy() say when it is
 *     done. The reference sample's own capture loop is four lines inside
 *     `while (true) { swiWaitForVBlank(); if (!ndmaBusy(...) || !cameraTransferActive())
 *     cameraStartTransfer(...); }` -- there is no analogous tearing problem
 *     for this file's poll-and-rearm to solve, because there is no second
 *     thread's timing to race.
 *
 * So this file is deliberately the SIMPLE side of that comparison: one
 * buffer, one NDMA channel, re-armed once per screen frame from the QR
 * screen's update(), following the sample's own shape rather than
 * transplanting the 3DS's threading model onto hardware that does not need
 * it. melonDS 1.1 in DSi mode DOES drive this loop (the frame counter climbs
 * and quirc decodes) now that the destination is VRAM; what is still
 * UNVERIFIED is whether the DS's simpler hardware really avoids the 3DS's
 * tearing failure mode on a real console, or whether it has one of its own
 * that six rounds on real DSi hardware would find.
 *
 * THE NDMA DESTINATION IS THE BOTTOM SCREEN'S VRAM, AND THAT IS NOT A STYLE
 * CHOICE. It is the same target the sample uses (`bgGetGfxPtr(bg3Main)`, i.e.
 * MAIN-ENGINE background VRAM at 0x06000000..) and this file mirrors it now
 * for the reason the sample never had to spell out: on this hardware the
 * camera's NDMA only ever COMPLETES into main-engine VRAM. A main-RAM
 * destination delivers a partial first frame and then never signals
 * completion again -- `ndmaBusy()` and `cameraTransferActive()` both stay
 * true forever, the poll below never re-arms, and on real DSi hardware (a 3DS
 * in TWL mode) the same run ends in a data abort. ESTABLISHED BY ISOLATION,
 * not by reasoning: the stock BlocksDS sample, unmodified except for being
 * cut down to its preview loop, reaches 600+ frames targeting bgGetGfxPtr();
 * the SAME sample changed ONLY to target a memalign(32) main-RAM buffer
 * stalls at frame 1, exactly like this client used to.
 *
 * So the caller supplies the destination: ui.c's main engine is the BOTTOM
 * screen (ui.c's VRAM MAP), screen_qrscan.c takes it with
 * ui_bottom_camera_lock() (ui.h), the live preview IS the bottom screen with
 * no CPU blit anywhere, and the scan instructions and CANCEL move to the top
 * screen. An earlier attempt at this handed over the TOP screen's base
 * instead -- 0x06200000, the SUB engine's VRAM C -- and froze on a partial
 * frame just like main RAM did. Main engine or nothing.
 *
 * CACHE: there is none to worry about for the preview any more. VRAM is
 * UNCACHED on this CPU, so nothing here calls DC_InvalidateRange() on the
 * preview -- which is also why the reference sample never needed to. The one
 * remaining main-RAM buffer is the 8-bit luma plane quirc is fed from, and
 * the CPU is the only thing that ever writes it (it is filled by READING the
 * uncached VRAM preview), so it needs no cache operation either. It is still
 * memalign(32)'d rather than malloc()'d: costs nothing, and keeps the buffer
 * off the shared cache lines of its heap neighbours if a future edit ever
 * does hand it to DMA.
 */
#ifndef ATTICPAD_NDS_CAMERA_H
#define ATTICPAD_NDS_CAMERA_H

#include <stdint.h>

/* Native capture size == native DS screen size, so the preview needs no
 * scaling to fill the bottom screen -- see references/nds/examples/
 * peripherals/camera/'s Peripherals doc ("Preview: 256x192, 16-bit RGBA"),
 * confirmed against the vendored driver source
 * (source/arm9/peripherals/camera.twl.c: `REG_NDMA_LENGTH = (256*192)>>1`
 * words for MCUREG_APT_SEQ_CMD_PREVIEW). This is NOT a 3DS-coordinate
 * constant like UI_TOP_W/UI_BOT_W -- it is the DS's own native resolution,
 * used for raw pixel work only. */
#define APAD_NDS_CAM_W 256
#define APAD_NDS_CAM_H 192

/* `vram_target` is the MAIN-ENGINE background VRAM the camera's NDMA will own
 * outright for as long as the camera runs -- ui_bottom_camera_lock()'s return
 * value, nothing else. It must be in the 0x06000000.. main-engine range and
 * must have room for APAD_NDS_CAM_W * APAD_NDS_CAM_H * 2 bytes written
 * contiguously (libnds camera.twl.c: `REG_NDMA_LENGTH = (256*192) >> 1`
 * words from `buffer`, so a 256-pixel-stride bitmap gets its first 192 rows
 * filled, which is exactly the visible screen). See the header comment above
 * for why a main-RAM or SUB-engine destination does not work.
 *
 * Gated on isDSiMode() INSIDE this file as well as by every caller (app.h's
 * "BLOCKING WORK" _ARM convention still applies -- this makes a run of CAMU-
 * equivalent service calls, a visible pause). Returns 1 on success; on
 * failure returns 0 and apad_nds_cam_status() says which call failed.
 * Idempotent: a second call while already running returns 1 immediately. */
int apad_nds_cam_start(uint16_t *vram_target);

/* cameraStopTransfer() (draining any in-flight NDMA first) then
 * cameraDeinit(), then frees the luma buffer and forgets the caller's VRAM
 * (which this file never owned and must not free -- the caller releases it
 * with ui_bottom_camera_unlock()). Idempotent, and safe to call whether or
 * not apad_nds_cam_start() ever succeeded. */
void apad_nds_cam_stop(void);

int apad_nds_cam_running(void);

/* Stop the feed (bounded) before a decode; the next poll re-arms it. */
void apad_nds_cam_pause(void);

/* Call once per display frame while running, mirroring the sample's own
 * per-VBlank re-arm check. `lid_closed` is app_ctx's KEY_LID reading --
 * passing 1 stops the transfer (cameraStopTransfer()) without tearing the
 * camera down, and the FIRST call with `lid_closed` false again re-arms a
 * fresh transfer, exactly like a normal start.
 *
 * Returns 1 the first poll after a previously in-flight transfer is seen to
 * have completed (!ndmaBusy() && !cameraTransferActive()), which is when
 * apad_nds_cam_preview()/apad_nds_cam_luma() have something new to read; 0
 * otherwise (the ordinary outcome on most polls, since the sensor's own
 * frame period need not match the display's). */
int apad_nds_cam_poll(int lid_closed);

/* The live preview surface -- the caller's own VRAM, handed back so the
 * decode path can read it: APAD_NDS_CAM_W x APAD_NDS_CAM_H, tightly packed,
 * native DS BGR555 (bit15 set/opaque, bits 0-4 R, bits 5-9 G, bits 10-14 B --
 * ui.c's rgb15() packs the same way, which is why cameraStartTransfer()'s
 * output can be blitted straight into a bitmap background with no channel
 * reordering, exactly as the reference sample does -- here it IS the bitmap
 * background, so nothing blits it at all). NULL before the first completed
 * transfer. NOT cache-invalidated: VRAM is uncached -- see this file's
 * header. */
const uint16_t *apad_nds_cam_preview(void);

/* The last preview frame repacked as a TIGHT 8-bit grayscale plane, read out
 * of the VRAM preview one halfword at a time into main RAM,
 * APAD_NDS_CAM_W bytes per row, APAD_NDS_CAM_H rows -- exactly what
 * clients/common/apad_qr.c wants. Repacked on demand (this is the expensive
 * call), so screen_qrscan.c does not do this on every frame -- see its own
 * decode-cadence comment. NULL before the first completed transfer. */
const uint8_t *apad_nds_cam_luma(void);

/* NULL when healthy; otherwise which bring-up call failed and with what this
 * platform's camera.h calls returned. */
const char *apad_nds_cam_status(void);

unsigned apad_nds_cam_frames(void);   /* completed transfers seen by poll() */

#endif /* ATTICPAD_NDS_CAMERA_H */
