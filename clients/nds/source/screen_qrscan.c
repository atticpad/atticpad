/* clients/nds/source/screen_qrscan.c
 *
 * clients/3ds/source/screen_qrscan.c, ported. DSi ONLY -- point the outer
 * camera at the QR code the server's web UI shows and the address and the
 * S10.3 pairing secret both arrive in one gesture, exactly as they do on the
 * 3DS. Everything above the camera boundary (quirc, apad_qr.c,
 * apad_pair_uri_parse()) is untouched, shared code; camera_nds.h replaces
 * camera_3ds.h -- see that file's header for why this file's capture side is
 * far simpler than the 3DS's threaded one.
 *
 * THREE DIFFERENCES FROM THE 3DS FILE:
 *
 *   1. GATED ON isDSiMode(), belt and braces. screen_connect.c only offers
 *      the SCAN QR button when ctx->is_dsi, and camera_nds.c's own
 *      apad_nds_cam_start() refuses on a DS (redundantly, since libnds's own
 *      camera functions already refuse too -- see camera_nds.c) -- but this
 *      screen's update() checks a THIRD time, first thing, before touching
 *      anything else, because the task asks for a check that does not depend
 *      on the Makefile or on any other screen's cooperation.
 *   2. THERE IS NO BLIT AND NO TEXTURE: THE CAMERA WRITES THE SCREEN. The
 *      DSi camera's NDMA only completes into MAIN-ENGINE background VRAM
 *      (camera_nds.h has the isolation evidence), and ui.c's main engine is
 *      the BOTTOM screen -- so this screen borrows the bottom screen's VRAM
 *      outright with ui_bottom_camera_lock() (ui.h), hands that address to
 *      apad_nds_cam_start(), and the live preview simply IS the bottom
 *      screen. Native preview resolution is native screen resolution
 *      (256x192 both), so it lands 1:1 with no scaling and no copy at all.
 *      That is why the instructions and CANCEL are on the TOP screen here
 *      and on the bottom on the 3DS: the bottom screen is the viewfinder,
 *      full-bleed, and nothing else may draw on it while the camera runs.
 *   3. NO CAPTURE-MODE TOGGLE, NO TEAR METRIC. camera_3ds.c's Y key and its
 *      whole measurement apparatus exist to compare capture disciplines that
 *      do not apply to this hardware (camera_nds.h's header explains why).
 *      What is kept is a small diagnostic line (frames/decodes/last cost),
 *      because a screen nobody can single-step should always say something
 *      when it is not working -- the M2 lesson this project keeps citing.
 *
 * CANCEL IS A B-BUTTON HINT, NOT A TOUCH TARGET, while the camera runs.
 * It is drawn on the top screen, which has no digitiser -- so B is the only
 * way out of the live scan (plus SELECT for the self-test, unchanged). The
 * box is still drawn as a button rather than as a line of text because it
 * is the same affordance every other screen's footer uses and it flashes on
 * the B press exactly as a touched one would. The QR_FAILED state keeps its
 * BACK button on the BOTTOM screen and keeps it touchable: the camera never
 * started there, so the bottom screen is ordinary again.
 *
 * X ALREADY MEANS "ENTER PIN" ON screen_connect.c (kbd_nds.c can type),
 * unlike the 3DS where X means "scan QR" because there is no keyboard to
 * conflict with. So SCAN QR is reachable ONLY from screen_connect.c's
 * touch button here, not from a key -- a real, named difference from the
 * 3DS's binding, not an oversight.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apad_qr.h"
#include "camera_nds.h"

/* ------------------------------------------------------------------------ */
/* substates                                                                */
/* ------------------------------------------------------------------------ */

enum {
    QR_ARM = 0,   /* one drawn frame before cameraInit() and friends         */
    QR_SCAN,      /* live preview, decoding every so often                  */
    QR_FOUND,     /* decoded; camera already released; brief confirmation   */
    QR_FAILED     /* the camera would not start -- says which call failed   */
};

static int s_sub;
static int s_hold;             /* frames left on the confirmation screen    */
static int s_flash_cancel;
static int s_show_diag;        /* X on screen_connect.c has no equivalent
                                 * here -- SELECT is idle in QR_SCAN except
                                 * for the self-test window it already shares
                                 * with every other screen, so this toggle
                                 * borrows the touch-only convention instead:
                                 * tapping the diagnostic line itself. */

static apad_qr *s_qr;

/* The bottom screen's main-engine VRAM while the camera owns it -- the exact
 * pointer handed to apad_nds_cam_start(), kept so the diagnostic line can
 * SHOW it. That is not decoration: "which engine is this address on" is the
 * single fact that separates a working camera from a frozen one on this
 * hardware (0x060xxxxx = main engine = works, 0x062xxxxx = sub engine =
 * partial frame then freeze), and reading it off the screen is the only way
 * to check it on a console with no debugger. */
static uint16_t *s_cam_vram;

static char s_reject[96];
static int  s_reject_frames;
#define REJECT_FRAMES 180      /* ~3s at 60Hz, same as the 3DS               */

static char     s_found_ip[16];
static unsigned s_found_port;
static int      s_found_paired;

/* DECODE CADENCE -- same adaptive shape as camera_3ds.c's
 * decode_interval_ms(), for the same reason: nothing in this repo has ever
 * run quirc_decode() on a 67 MHz ARM9 with no FPU (quirc's perspective-fit
 * math is `quirc_float_t` = double, clients/vendor/quirc/lib/
 * quirc_internal.h -- soft-float on this CPU, a cost the 3DS's ARM11 FPU
 * never pays), so the interval is a measured floor doubled from the last
 * real decode, not a guessed constant. DECODE_MIN_MS matches the task's "at
 * most every ~10 frames" (10 frames at ~60Hz is ~166ms); DECODE_MAX_MS is
 * higher than the 3DS's because "expect hundreds of ms" is the ballpark this
 * task itself gives for a decode on this CPU, and the adaptive doubling
 * needs room to actually reach that before the cap clips it. */
#define DECODE_MIN_MS 166u
#define DECODE_MAX_MS 2500u
static uint32_t s_last_decode_ms;
static uint32_t s_decode_cost_ms;
static unsigned s_decode_count;

static uint32_t decode_interval_ms(void)
{
    uint32_t want = s_decode_cost_ms * 2u;

    if (want < DECODE_MIN_MS) { want = DECODE_MIN_MS; }
    if (want > DECODE_MAX_MS) { want = DECODE_MAX_MS; }
    return want;
}

/* ------------------------------------------------------------------------ */
/* layout -- 3DS coordinates, like every other screen file here. The live    */
/* preview has no layout at all: the camera writes native DS pixels straight */
/* into the whole bottom screen (see this file's header).                    */
/* ------------------------------------------------------------------------ */

/* TWO boxes, because the two states live on different screens. kCancelTop is
 * in TOP-screen (400x240) space -- centred, x = (400-152)/2 = 124 -- and is
 * drawn only while the camera has the bottom screen. kCancelBtn keeps the
 * 3DS file's own bottom-screen (320x240) numbers and is used only by
 * QR_FAILED, where the bottom screen is ordinary and touchable. */
static const ui_box kCancelTop = { 124.0f, 196.0f, 152.0f, 32.0f };
static const ui_box kCancelBtn = { 84.0f, 200.0f, 152.0f, 32.0f };

/* ------------------------------------------------------------------------ */
/* leaving                                                                  */
/* ------------------------------------------------------------------------ */

/* STOP THE CAMERA BEFORE GIVING THE SCREEN BACK, always in this order:
 * apad_nds_cam_stop() drains the in-flight NDMA, and only then may
 * ui_bottom_camera_unlock() let ui.c start drawing into that VRAM again. */
static void release_camera(void)
{
    apad_nds_cam_stop();
    if (s_cam_vram != NULL) {
        ui_bottom_camera_unlock();
        s_cam_vram = NULL;
    }
}

static apad_screen_id leave(app_ctx *ctx, apad_screen_id next)
{
    release_camera();
    if (s_qr != NULL) {
        apad_qr_destroy(s_qr);
        s_qr = NULL;
    }
    app_disarm(ctx);
    return next;
}

/* ------------------------------------------------------------------------ */
/* a successful decode -- clients/3ds/source/screen_qrscan.c's accept_uri(), */
/* unchanged in shape: same engine calls, same secret-wipe, same fields.    */
/* ------------------------------------------------------------------------ */

static void accept_uri(app_ctx *ctx, apad_pair_uri *uri)
{
    snprintf(s_found_ip, sizeof s_found_ip, "%u.%u.%u.%u",
             (unsigned)uri->addr.ip[0], (unsigned)uri->addr.ip[1],
             (unsigned)uri->addr.ip[2], (unsigned)uri->addr.ip[3]);
    s_found_port = (unsigned)uri->addr.port;

    snprintf(ctx->ip_text, sizeof ctx->ip_text, "%s", s_found_ip);
    snprintf(ctx->port_text, sizeof ctx->port_text, "%u", s_found_port);

    ctx->from_announce = 0;
    ctx->server_name[0] = '\0';
    ctx->have_secret = 0;
    s_found_paired = 0;

    if (uri->secret[0] != '\0') {
        if (apad_client_set_secret(ctx->client, uri->secret) == APAD_OK) {
            ctx->have_secret = 1;
            s_found_paired = 1;
        } else {
            app_note(ctx, 2, "the scanned key was rejected by the client");
        }
    } else {
        /* UNREACHABLE TODAY -- see clients/3ds/source/screen_qrscan.c's own
         * comment on this branch, which applies verbatim: §10.3 requires a
         * secret and core enforces that at both ends, so this exists only to
         * make sure a secret from a PREVIOUS scan can never be reused. */
        (void)apad_client_set_secret(ctx->client, NULL);
    }

    apad_secure_zero(uri, sizeof *uri);

    app_note(ctx, 0, "scanned %s:%u%s", s_found_ip, s_found_port,
             s_found_paired ? " (with a pairing key)" : " (no pairing key)");
    ctx->want_connect_now = 1;
}

/* ------------------------------------------------------------------------ */
/* screen callbacks                                                         */
/* ------------------------------------------------------------------------ */

static void qrscan_enter(app_ctx *ctx)
{
    (void)ctx;

    if (s_qr != NULL) {
        apad_qr_destroy(s_qr);
        s_qr = NULL;
    }
    s_sub = QR_ARM;
    s_cam_vram = NULL;
    s_hold = 0;
    s_flash_cancel = 0;
    s_show_diag = 0;
    s_reject[0] = '\0';
    s_reject_frames = 0;
    s_last_decode_ms = 0;
    s_decode_cost_ms = 0;
    s_decode_count = 0;
    s_found_ip[0] = '\0';
    s_found_port = 0;
    s_found_paired = 0;
}

/* `touchable` is 0 while the camera owns the bottom screen: the CANCEL box is
 * drawn on the TOP screen then, and there is no digitiser up there, so a hit
 * test against a bottom-screen rectangle would fire on a tap that landed on
 * the viewfinder instead. QR_FAILED passes 1 and keeps the touch route. */
static int cancelled(app_ctx *ctx, int touchable)
{
    if (app_pressed(ctx, KEY_B)) {
        s_flash_cancel = 1;
        return 1;
    }
    if (touchable && ctx->touch_pressed
        && ui_box_hit(&kCancelBtn, (int)ctx->touch.px, (int)ctx->touch.py)) {
        s_flash_cancel = 1;
        return 1;
    }
    return 0;
}

static void try_decode(app_ctx *ctx)
{
    const uint8_t *luma;
    apad_pair_uri uri;
    uint32_t t0;
    int rc;

    if (s_qr == NULL) {
        return;
    }
    luma = apad_nds_cam_luma();
    if (luma == NULL) {
        return;
    }

    memset(&uri, 0, sizeof uri);
    t0 = apad_ticks_ms();
    /* No capture while quirc runs: a long decode with the camera streaming
     * is exactly how its transfer overruns and wedges (camera_nds.c). The
     * luma above is already a copy of the last complete frame. */
    apad_nds_cam_pause();
    rc = apad_qr_decode(s_qr, luma, APAD_NDS_CAM_W, APAD_NDS_CAM_H,
                        APAD_NDS_CAM_W, &uri);
    s_last_decode_ms = apad_ticks_ms();
    s_decode_cost_ms = apad_time_since(s_last_decode_ms, t0);
    s_decode_count++;

    switch (rc) {
        case APAD_OK:
            accept_uri(ctx, &uri);
            release_camera();
            if (s_qr != NULL) {
                apad_qr_destroy(s_qr);
                s_qr = NULL;
            }
            app_disarm(ctx);
            s_sub = QR_FOUND;
            s_hold = 100;
            break;

        case APAD_ERR_VERSION:
            snprintf(s_reject, sizeof s_reject,
                     "that pairing code is newer than this client (v!=1)");
            s_reject_frames = REJECT_FRAMES;
            break;

        case APAD_ERR_ARG:
            snprintf(s_reject, sizeof s_reject,
                     "that is a QR code, but not an AtticPad pairing code");
            s_reject_frames = REJECT_FRAMES;
            break;

        default:
            /* APAD_ERR_STATE: no readable QR symbol yet. Ordinary while
             * aiming; not a message. */
            break;
    }
}

static apad_screen_id qrscan_update(app_ctx *ctx)
{
    /* THE THIRD, MAKEFILE-INDEPENDENT GATE. See this file's header. */
    if (!ctx->is_dsi) {
        app_note(ctx, 2, "the QR scanner needs a DSi camera");
        return APAD_SCREEN_CONNECT;
    }

    if (s_reject_frames > 0) {
        s_reject_frames--;
    }

    switch (s_sub) {
        case QR_ARM:
            if (s_qr != NULL) {
                apad_qr_destroy(s_qr);
                s_qr = NULL;
            }
            s_qr = apad_qr_create();
            if (s_qr == NULL) {
                snprintf(s_reject, sizeof s_reject,
                         "out of memory for the QR decoder");
                s_sub = QR_FAILED;
                app_disarm(ctx);
                return APAD_SCREEN_QRSCAN;
            }
            /* THE BOTTOM SCREEN BECOMES THE VIEWFINDER HERE. Take the lock
             * FIRST -- apad_nds_cam_start() refuses an address that is not
             * main-engine VRAM, so the camera can never be armed against a
             * surface ui.c is still flipping. On any failure the lock goes
             * straight back, so QR_FAILED gets an ordinary bottom screen with
             * a touchable BACK button. */
            s_cam_vram = ui_bottom_camera_lock();
            if (!apad_nds_cam_start(s_cam_vram)) {
                ui_bottom_camera_unlock();
                s_cam_vram = NULL;
                apad_qr_destroy(s_qr);
                s_qr = NULL;
                s_sub = QR_FAILED;
                app_disarm(ctx);
                return APAD_SCREEN_QRSCAN;
            }
            app_disarm(ctx);
            s_sub = QR_SCAN;
            s_last_decode_ms = apad_ticks_ms();
            return APAD_SCREEN_QRSCAN;

        case QR_SCAN:
            if (!apad_nds_cam_running()) {
                s_sub = QR_ARM;
                return APAD_SCREEN_QRSCAN;
            }
            if (cancelled(ctx, 0)) {
                return leave(ctx, APAD_SCREEN_CONNECT);
            }
            if (app_pressed(ctx, KEY_SELECT)) {
                ctx->selftest_return = APAD_SCREEN_CONNECT;
                return leave(ctx, APAD_SCREEN_SELFTEST);
            }
            if (ctx->touch_pressed && (int)ctx->touch.py < 20) {
                /* The top strip of the viewfinder -- an unadvertised tap
                 * target, same spirit as the 3DS's X (s_pin_trace): pulls up
                 * the frame/decode counters, and the camera's VRAM address,
                 * for someone reporting a bug. The counters are drawn on the
                 * TOP screen now (nothing may draw over the live image), so
                 * the tap and its effect are on different screens -- which is
                 * why the strip is at the top of the bottom screen, directly
                 * under where the line appears. */
                s_show_diag = !s_show_diag;
            }

            /* THE LID. camera_nds.c's own poll() already stops/restarts the
             * transfer on this edge (camera_nds.h); this screen only has to
             * pass the reading through and skip a decode attempt while
             * closed -- there is nothing new to read. */
            if (apad_nds_cam_poll(ctx->lid_closed) && !ctx->lid_closed) {
                if (apad_time_since(apad_ticks_ms(), s_last_decode_ms)
                    >= decode_interval_ms()) {
                    try_decode(ctx);
                }
            }
            return APAD_SCREEN_QRSCAN;

        case QR_FOUND:
            if (--s_hold <= 0 || app_pressed(ctx, 0xFFFFFFFFu)
                || ctx->touch_pressed) {
                return APAD_SCREEN_CONNECT;
            }
            return APAD_SCREEN_QRSCAN;

        default: /* QR_FAILED */
            if (cancelled(ctx, 1) || app_pressed(ctx, KEY_A)
                || ctx->touch_pressed) {
                return leave(ctx, APAD_SCREEN_CONNECT);
            }
            return APAD_SCREEN_QRSCAN;
    }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void draw_found_top(void)
{
    ui_header(UI_TOP_W, "AtticPad -- scan QR", "found", ui_c_good());
    /* Fitted, not ui_bignum(): the big face has no width limit and a
     * 13-character address ("192.168.1.103") drew ~1.5 screens wide, clipped
     * at both ends on the 256 px screen (seen scanning a LAN-addressed
     * server, 2026-09-10). ui_textf_fit() steps the face down until it fits. */
    ui_textf_fit(UI_TOP_W * 0.5f, 92.0f, UI_S_HEAD, ui_c_text(), UI_ALIGN_CENTER,
                 UI_TOP_W - 16.0f, "%s", s_found_ip);
    ui_textf(UI_TOP_W * 0.5f, 146.0f, UI_S_HEAD, ui_c_dim(), UI_ALIGN_CENTER,
             "port %u", s_found_port);
}

static void draw_failed_top(app_ctx *ctx)
{
    const char *why = apad_nds_cam_status();

    ui_header(UI_TOP_W, "AtticPad -- scan QR", "camera unavailable",
              ui_c_bad());
    ui_textf(UI_TOP_W * 0.5f, 60.0f, UI_S_HEAD, ui_c_bad(), UI_ALIGN_CENTER,
             "the camera did not start");
    ui_textf_fit(UI_TOP_W * 0.5f, 176.0f, UI_S_SMALL, ui_c_dim(),
                 UI_ALIGN_CENTER, UI_TOP_W - 16.0f,
                 "you can type the address and PIN instead");
    if (ctx->keys_held & KEY_SELECT) {
        ui_textf_fit(UI_TOP_W * 0.5f, 132.0f, UI_S_TINY, ui_c_border(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f, "details: %s",
                     (why != NULL) ? why
                                   : (s_reject[0] != '\0' ? s_reject
                                                          : "reason unknown"));
    }
}

static void draw_scan_top(app_ctx *ctx)
{
    /* Header right-text: the DS's 8x8 face is proportionally WIDER than the
     * 3DS's system font, so a phrase that fits the 3DS header truncates here
     * (the ~256px face cut "aim at the code" to "aim at the cod"). These are
     * all short enough to render whole in the header's right slot on the DS. */
    const char *right = !apad_nds_cam_running() ? "starting..."
                      : ctx->lid_closed          ? "closed"
                                                 : "aim at the QR";
    float y;
    int n;

    ui_header(UI_TOP_W, "AtticPad -- scan QR", right, ui_c_accent());

    /* WRAP, do not fit: on the DS every UI_S_* maps to the one 8x8 face, so
     * ui_textf_fit() cannot shrink to fit and instead TRUNCATES -- which is
     * what dropped "QR code" off the end of this line on hardware. The
     * instruction wraps onto as many lines as it needs; the dim second line
     * follows below it, positioned from the line count actually drawn. */
    /* THE VERTICAL BUDGET, and why the second line is short. Everything on
     * this screen shares one column between the header and the CANCEL box at
     * y=196: two wrapped HEAD lines, one dim line, a transient reject
     * message, and the two-line diagnostic. Measured in melonDS, an earlier
     * three-line dim sentence pushed the diagnostic's second line onto the
     * button's top border. The dropped half of it ("the camera image is on
     * the touch screen") was saying what the screen below already shows. */
    y = 76.0f;
    n = ui_textf_wrap(UI_TOP_W * 0.5f, y, UI_TOP_W - 16.0f, UI_S_HEAD,
                      ui_c_text(), UI_ALIGN_CENTER, UI_LINE(UI_S_HEAD),
                      "point the outer camera at the QR code");
    y += (float)n * UI_LINE(UI_S_HEAD) + 6.0f;
    n = ui_textf_wrap(UI_TOP_W * 0.5f, y, UI_TOP_W - 16.0f, UI_S_SMALL,
                      ui_c_dim(), UI_ALIGN_CENTER, UI_LINE(UI_S_SMALL),
                      "it is on the server's web page");
    y += (float)n * UI_LINE(UI_S_SMALL) + 6.0f;

    if (s_reject_frames > 0 && s_reject[0] != '\0') {
        n = ui_textf_wrap(UI_TOP_W * 0.5f, y, UI_TOP_W - 16.0f, UI_S_SMALL,
                          ui_c_warn(), UI_ALIGN_CENTER, UI_LINE(UI_S_SMALL),
                          "%s", s_reject);
        y += (float)n * UI_LINE(UI_S_SMALL) + 4.0f;
    }
    if (s_show_diag) {
        /* THE ADDRESS IS THE POINT (see s_cam_vram): 0x060xxxxx is the main
         * engine and the camera runs; anything else and it does not. */
        ui_textf_fit(UI_TOP_W * 0.5f, y, UI_S_TINY, ui_c_border(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f,
                     "frames %u  decodes %u  last %lu ms",
                     apad_nds_cam_frames(), s_decode_count,
                     (unsigned long)s_decode_cost_ms);
        ui_textf_fit(UI_TOP_W * 0.5f, y + 14.0f, UI_S_TINY, ui_c_border(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f, "vram %08lX",
                     (unsigned long)(uintptr_t)s_cam_vram);
    }

    /* CANCEL lives up here because the bottom screen is the viewfinder. No
     * digitiser on this screen -- B is the only way to press it. */
    ui_button(&kCancelTop, "CANCEL (B)", s_flash_cancel, 0);
    s_flash_cancel = 0;
}

static void qrscan_draw_top(app_ctx *ctx)
{
    switch (s_sub) {
        case QR_FOUND:  draw_found_top();      break;
        case QR_FAILED: draw_failed_top(ctx);  break;
        default:        draw_scan_top(ctx);    break;
    }
}

static void qrscan_draw_bottom(app_ctx *ctx)
{
    const float W = UI_BOT_W - 20.0f;

    (void)ctx;

    if (s_sub == QR_FOUND) {
        ui_header(UI_BOT_W, "scan QR", "done", ui_c_good());
        ui_textf_fit(UI_BOT_W * 0.5f, 200.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, W, "connecting ...");
        return;
    }
    if (s_sub == QR_FAILED) {
        ui_header(UI_BOT_W, "scan QR", "failed", ui_c_bad());
        ui_button(&kCancelBtn, "BACK (B)", s_flash_cancel, 0);
        s_flash_cancel = 0;
        return;
    }

    /* THE LIVE PREVIEW, AND THERE IS NOTHING TO DO. The camera's NDMA is
     * writing these exact pixels right now (ui_bottom_camera_lock() gave it
     * this surface and ui.c neither clears nor flips it while the lock is
     * held), so drawing ANYTHING here -- including a header or a button --
     * would punch a hole in the live image. Every affordance this screen has
     * is on the top screen. Leaving this function empty is the feature. */
}

const apad_screen apad_screen_qrscan = {
    "scan QR",
    qrscan_enter,
    qrscan_update,
    qrscan_draw_top,
    qrscan_draw_bottom
};
