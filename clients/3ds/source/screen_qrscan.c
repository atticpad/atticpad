/* clients/3ds/source/screen_qrscan.c
 *
 * The QR scanner: point the console at the QR code the server's web UI is
 * showing, and the address and the pairing secret both arrive in one gesture
 * (docs/PROTOCOL.md §10.3). It replaces typing a dotted quad on a numpad
 * while someone reads a PIN out across the room, which is what the connect
 * screen otherwise asks for -- and, until this screen existed, the 3DS could
 * not join a server that required pairing AT ALL: screen_connect.c's own
 * comment said "no PIN entry on 3DS yet" and refused before sending HELLO.
 *
 * WHAT THIS FILE DOES NOT DO, deliberately:
 *
 *   - It does not find QR codes. quirc does (clients/vendor/quirc).
 *   - It does not understand `atticpad://`. core/ does. The bytes quirc
 *     recovers go straight into apad_pair_uri_parse() by way of
 *     clients/common/apad_qr.c -- the SAME adapter, compiled from the same
 *     file, that the Android client has been scanning production QR codes
 *     with since M4. Two camera clients, one decoder and one grammar
 *     (docs/PROTOCOL.md §10.3: three implementations "must agree byte for
 *     byte", which is only cheap if there is only one of each to keep).
 *   - It does not drive a handshake. On a successful decode it fills in the
 *     connect screen's address fields, hands the secret to the session
 *     engine, and returns to screen_connect.c with "connect now" set.
 *     screen_connect.c already owns §8 and is the only place that should.
 *
 * The camera itself is camera_3ds.c, mirrored from
 * references/3ds/camera-video/main.c. Everything in this file above that
 * boundary is ordinary citro2d.
 *
 * ROUND 7 RESTRUCTURED THE CAMERA, NOT THIS SCREEN. camera_3ds.c now runs its
 * capture loop on a dedicated thread (Steveice10's FBI, transliterated), so
 * apad3ds_cam_poll() no longer drives the port or blocks on it -- it just
 * takes the newest published frame if there is one. Nothing in this file had
 * to change for that, which is the point: the boundary was in the right place.
 *
 * TWO capture modes remain behind the Y key, not three: G is FBI verbatim and
 * H is G with a ping-pong DMA pair, and they exist only so one netload can
 * tell "the thread fixed it" from "the thread helped and the residual re-arm
 * gap still matters". The key and the mode line come out when hardware picks
 * one.
 *
 * THE CAPTURE TRACE IS PERMANENT. It is drawn over the viewfinder WHENEVER
 * the camera pipeline is unhealthy (apad3ds_cam_unhealthy(), which is a
 * property of the pipeline and not of a frame counter), and X pins it up over
 * a healthy one for reading the numbers off. It earned that place: three
 * candidate capture loops were compared on hardware through it, and an
 * earlier `frames == 0` gate is what made the worst of them unreadable -- it
 * failed at `frames == 1`, so the viewfinder covered every number that would
 * have explained it for the whole session.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apad_qr.h"
#include "camera_3ds.h"

/* ------------------------------------------------------------------------ */
/* substates                                                                */
/* ------------------------------------------------------------------------ */

/* QR_ARM exists for the reason every _ARM in this client exists (app.h,
 * "BLOCKING WORK"): camInit() plus a dozen CAMU service calls plus quirc's
 * allocations is a visible pause, and the frame that says "starting the
 * camera" has to be DRAWN before it, not after. */
enum {
    QR_ARM = 0,
    QR_SCAN,      /* live viewfinder, decoding every so often               */
    QR_FOUND,     /* decoded; camera already released; brief confirmation   */
    QR_FAILED     /* the camera would not start -- says which call failed   */
};

static int s_sub;
static int s_hold;            /* frames left on the confirmation screen     */
static int s_flash_cancel;

/* X: keep the capture trace on screen even when the pipeline looks healthy,
 * so a working camera's numbers can be read off (and photographed) too. */
static int s_pin_trace;

/* THE UNWELL GRACE PERIOD (2026-08-12 hardware report: "the camera screen
 * shows the debug capture-trace during its first seconds").
 *
 * apad3ds_cam_unhealthy() is 1 for the pipeline's entire first second by
 * design (camera_3ds.c: fewer than 30 captured frames reads unhealthy) --
 * and QR_ARM's whole existence is a period BEFORE that, where the camera has
 * not started at all, which is unhealthy too (!s_running). So "unwell" is
 * the pipeline's NORMAL state for the first chunk of every visit to this
 * screen, not a fault -- and drawing the eight-line trace over the
 * viewfinder for that whole stretch put a debug wall in front of every user
 * for the first few seconds of every scan, which is exactly the thing
 * today's UX bar (visual feedback over words, no debug surface by default)
 * rules out.
 *
 * Fix: track how long the pipeline has been CONTINUOUSLY unwell and only
 * bring the trace up once that exceeds UNWELL_GRACE_MS, resetting the clock
 * the instant a frame reports healthy. During the grace window the ordinary
 * viewfinder/"starting the camera ..." text still draws (draw_scan_top()'s
 * existing logic, untouched) -- there is just no trace over it. X (s_pin_trace
 * above) is a separate, unconditional override and is NOT touched by this: a
 * user who explicitly asks for the numbers gets them immediately, grace
 * period or not. */
#define UNWELL_GRACE_MS 3000u
static uint32_t s_unwell_since_ms; /* 0 == currently healthy               */

/* Mutates s_unwell_since_ms and returns whether the grace period has expired,
 * i.e. whether the trace is due on its own merits (independent of X).
 * Ordinarily called once per real frame (draw_scan_top() below is main.c's
 * only call site today); if a future stereo pass ever calls this screen's
 * draw_top() twice in the same frame, calling it a second time within the
 * same millisecond is harmless -- apad_ticks_ms() has not moved, so the
 * comparison is unchanged either way. */
static int trace_grace_expired(void)
{
    uint32_t now;

    if (!apad3ds_cam_unhealthy()) {
        s_unwell_since_ms = 0u;
        return 0;
    }
    now = apad_ticks_ms();
    if (s_unwell_since_ms == 0u) {
        s_unwell_since_ms = now;
        return 0;
    }
    return apad_time_since(now, s_unwell_since_ms) > UNWELL_GRACE_MS;
}

static apad_qr *s_qr;

/* What to tell the user about the last decode ATTEMPT that saw something.
 * A frame with no QR code in it at all is the normal case while aiming and
 * is not a message (apad_qr.h says so); a frame with a QR code that is not
 * ours is worth saying out loud, because otherwise a poster on the wall
 * looks identical to a camera that is not working. */
static char s_reject[96];
static int  s_reject_frames;
#define REJECT_FRAMES 180     /* ~3s                                        */

/* The result, kept only long enough to draw the confirmation. The SECRET is
 * never stored here -- it goes into the session engine (which owns the one
 * copy, per clients/common/apad_client.h) and the parsed struct is wiped
 * immediately. §10: the secret is as sensitive as the PIN it replaces. */
static char s_found_ip[16];
static unsigned s_found_port;
static int s_found_paired;

/* DECODE CADENCE, and the reason it measures itself.
 *
 * apad_qr_decode() is one blocking call on the frame-loop thread: while it
 * runs, nothing draws and no button is scanned. quirc thresholds and
 * flood-fills all 96000 pixels before it does any Reed-Solomon, and NOTHING
 * IN THIS REPO HAS EVER RUN IT ON A 268 MHz ARM11 -- there is no 3DS emulator
 * that implements CAMU, so there was no way to find out before hardware.
 *
 * So the interval is not a guess that has to be right. It is a floor of
 * DECODE_MIN_MS, raised to twice whatever the last decode actually cost, and
 * capped at DECODE_MAX_MS. Whatever quirc turns out to cost here, the
 * decoder gets at most about a third of the wall clock and the viewfinder
 * keeps the rest -- an aimable viewfinder is what makes a scan land at all,
 * and a decoder that runs three times a second is plenty for a QR code that
 * is sitting still on a monitor. The measured figure is drawn on the bottom
 * screen, so the first netload answers the question for good. */
#define DECODE_MIN_MS 150
#define DECODE_MAX_MS 1000
static uint32_t s_last_decode_ms;
static uint32_t s_decode_cost_ms;   /* last measured apad_qr_decode() cost  */
static unsigned s_decode_count;

static uint32_t decode_interval_ms(void)
{
    uint32_t want = s_decode_cost_ms * 2u;

    if (want < (uint32_t)DECODE_MIN_MS) {
        want = (uint32_t)DECODE_MIN_MS;
    }
    if (want > (uint32_t)DECODE_MAX_MS) {
        want = (uint32_t)DECODE_MAX_MS;
    }
    return want;
}

/* ------------------------------------------------------------------------ */
/* layout                                                                   */
/* ------------------------------------------------------------------------ */

static const ui_box kCancelBtn = { 84.0f, 176.0f, 152.0f, 40.0f };

/* ------------------------------------------------------------------------ */
/* leaving                                                                  */
/* ------------------------------------------------------------------------ */

/* The screen table has no `leave` callback, so every path out of this screen
 * goes through here. A camera left activated keeps the sensor powered and
 * locks the system camera applet out, which is not a thing a user can
 * diagnose or recover from without a reboot. (camera_3ds.c also registers an
 * atexit() for the paths that never reach a screen transition at all.) */
static apad_screen_id leave(app_ctx *ctx, apad_screen_id next)
{
    apad3ds_cam_stop();
    if (s_qr != NULL) {
        apad_qr_destroy(s_qr);
        s_qr = NULL;
    }
    /* Teardown is a run of blocking service calls with no hidScanInput() in
     * it, so anything held would read as a fresh press on the next scan. */
    app_disarm(ctx);
    return next;
}

/* ------------------------------------------------------------------------ */
/* a successful decode                                                      */
/* ------------------------------------------------------------------------ */

static void accept_uri(app_ctx *ctx, apad_pair_uri *uri)
{
    snprintf(s_found_ip, sizeof s_found_ip, "%u.%u.%u.%u",
             (unsigned)uri->addr.ip[0], (unsigned)uri->addr.ip[1],
             (unsigned)uri->addr.ip[2], (unsigned)uri->addr.ip[3]);
    s_found_port = (unsigned)uri->addr.port;

    snprintf(ctx->ip_text, sizeof ctx->ip_text, "%s", s_found_ip);
    snprintf(ctx->port_text, sizeof ctx->port_text, "%u", s_found_port);

    /* Nothing announced itself from this address, so the connect screen must
     * still probe §6.2 before it sends a HELLO. */
    ctx->from_announce = 0;
    ctx->server_name[0] = '\0';
    ctx->have_secret = 0;
    s_found_paired = 0;

    if (uri->secret[0] != '\0') {
        if (apad_client_set_secret(ctx->client, uri->secret) == APAD_OK) {
            ctx->have_secret = 1;
            s_found_paired = 1;
        } else {
            /* Only reachable if the secret is longer than the engine accepts,
             * which apad_pair_uri_parse() should already have rejected. Say
             * so rather than connect as if unpaired and fail at AUTH. */
            app_note(ctx, 2, "the scanned key was rejected by the client");
        }
    } else {
        /* UNREACHABLE TODAY, and kept anyway. docs/PROTOCOL.md §10.3 lists
         * `s` in the grammar itself, and core enforces that at both ends:
         * apad_pair_uri_build() refuses to emit a URI without a secret and
         * apad_pair_uri_parse() returns APAD_ERR_ARG for one that lacks it
         * or carries it empty (both confirmed against core on the host,
         * 2026-08-11 -- a scanned code that reaches APAD_OK always has a
         * secret). So this branch exists for the day §10.3 gains an
         * unpaired form, and until then its only job is to make sure a
         * secret from a PREVIOUS scan can never be silently reused for a
         * different server. */
        (void)apad_client_set_secret(ctx->client, NULL);
    }

    /* The parsed copy has done its job. §10 keeps the secret out of logs; it
     * has no business outliving this function on the stack either. */
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

    /* Belt and braces: every exit path already releases the recognizer, but
     * QR_ARM below assigns s_qr unconditionally and a leak here would be one
     * quirc image buffer per visit to this screen. */
    if (s_qr != NULL) {
        apad_qr_destroy(s_qr);
        s_qr = NULL;
    }

    s_sub = QR_ARM;
    s_hold = 0;
    s_flash_cancel = 0;
    s_pin_trace = 0;
    s_unwell_since_ms = 0u;
    s_reject[0] = '\0';
    s_reject_frames = 0;
    s_last_decode_ms = 0;
    s_decode_cost_ms = 0;
    s_decode_count = 0;
    s_found_ip[0] = '\0';
    s_found_port = 0;
    s_found_paired = 0;
}

static int cancelled(app_ctx *ctx)
{
    if (app_pressed(ctx, KEY_B)) {
        return 1;
    }
    if (ctx->touch_pressed && ui_box_hit(&kCancelBtn, (int)ctx->touch.px,
                                         (int)ctx->touch.py)) {
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
    luma = apad3ds_cam_luma();
    if (luma == NULL) {
        return;
    }

    memset(&uri, 0, sizeof uri);
    t0 = apad_ticks_ms();
    rc = apad_qr_decode(s_qr, luma, APAD3DS_CAM_W, APAD3DS_CAM_H,
                        APAD3DS_CAM_W, &uri);
    /* Through apad_time_since(), not a bare subtraction: apad_ticks_ms()
     * wraps and core/src/seq.c owns that arithmetic for every client
     * (docs/CONVENTIONS.md). A naive `-` is correct here today and is exactly the
     * habit that breaks somewhere else in nine minutes. */
    s_last_decode_ms = apad_ticks_ms();
    s_decode_cost_ms = apad_time_since(s_last_decode_ms, t0);
    s_decode_count++;

    switch (rc) {
        case APAD_OK:
            accept_uri(ctx, &uri);
            /* Release the camera the instant it has done its job rather than
             * hold it through the confirmation. */
            apad3ds_cam_stop();
            if (s_qr != NULL) {
                apad_qr_destroy(s_qr);
                s_qr = NULL;
            }
            app_disarm(ctx);
            s_sub = QR_FOUND;
            s_hold = 100;   /* ~1.7s to read the address before connecting */
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
            /* APAD_ERR_STATE: no readable QR symbol in this frame. The
             * ordinary outcome while someone is still lining the camera up
             * (apad_qr.h) -- not a message. */
            break;
    }
}

static apad_screen_id qrscan_update(app_ctx *ctx)
{
    if (s_reject_frames > 0) {
        s_reject_frames--;
    }

    switch (s_sub) {
        case QR_ARM:
            /* The "starting the camera" frame has now been drawn. */
            if (s_qr != NULL) {
                /* Reached with a live s_qr after QR_SCAN bounced back here
                 * because APTHOOK_ONSUSPEND stopped the camera out from
                 * under it (main.c) -- the decoder is untouched by that, but
                 * it is re-created fresh alongside the new camera bring-up
                 * anyway, the same "belt and braces" every other entry to
                 * this screen already applies, rather than leak it. */
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
            if (!apad3ds_cam_start()) {
                apad_qr_destroy(s_qr);
                s_qr = NULL;
                s_sub = QR_FAILED;
                app_disarm(ctx);
                return APAD_SCREEN_QRSCAN;
            }
            app_disarm(ctx);
            s_sub = QR_SCAN;
            /* Start the interval fresh so the first decode does not fire on
             * the very first frame, before auto-exposure has settled. */
            s_last_decode_ms = apad_ticks_ms();
            return APAD_SCREEN_QRSCAN;

        case QR_SCAN:
            /* apad3ds_cam_stop() runs on APTHOOK_ONSUSPEND now (main.c,
             * 2026-08-12 crash fix) -- a HOME press that the user resumes
             * FROM stops the capture thread out from under this substate
             * without this screen ever hearing about it, since ONSUSPEND
             * fires on the main thread inside aptMainLoop() itself, not
             * through anything this screen's update() calls. Notice it here,
             * the same "the camera isn't running -- go rearm it" check the
             * rest of this file already uses at bring-up, rather than sit
             * forever on "starting the camera ..." with nothing actually
             * starting it. s_qr (the quirc recognizer) is left alone: it has
             * nothing to do with cam:u and does not need re-creating. */
            if (!apad3ds_cam_running()) {
                s_sub = QR_ARM;
                return APAD_SCREEN_QRSCAN;
            }
            if (cancelled(ctx)) {
                return leave(ctx, APAD_SCREEN_CONNECT);
            }
            if (app_pressed(ctx, KEY_SELECT)) {
                ctx->selftest_return = APAD_SCREEN_CONNECT;
                return leave(ctx, APAD_SCREEN_SELFTEST);
            }
            /* ROUND 7 A/B. Y cycles the capture loop. The camera is torn
             * down -- capture THREAD joined and all -- and brought back up
             * inside that call, so this update ends here: the key gate has to
             * be re-armed after a run of blocking calls with no
             * hidScanInput() in it (app.h). The decode clock restarts too, so
             * the new mode is not judged on a frame the old one captured. */
            if (app_pressed(ctx, KEY_Y)) {
                apad3ds_cam_mode_next();
                app_disarm(ctx);
                s_reject[0] = '\0';
                s_reject_frames = 0;
                s_decode_count = 0;
                s_decode_cost_ms = 0;
                s_last_decode_ms = apad_ticks_ms();
                /* A restarted pipeline is unhealthy again for its own first
                 * second (camera_3ds.c) -- give it the same grace period a
                 * fresh QR_ARM gets rather than showing the trace the instant
                 * the switch lands. */
                s_unwell_since_ms = 0u;
                return APAD_SCREEN_QRSCAN;
            }
            if (app_pressed(ctx, KEY_X)) {
                s_pin_trace = !s_pin_trace;
            }
            /* One camera frame per display frame at most; 0 means "nothing
             * new published yet", which at 30 fps is half of them. Since
             * round 7 this no longer drives the port -- the capture thread
             * does -- so a frame where this is not called costs a viewfinder
             * refresh and nothing else. */
            if (apad3ds_cam_poll()) {
                if (apad_time_since(apad_ticks_ms(), s_last_decode_ms)
                    >= decode_interval_ms()) {
                    try_decode(ctx);
                }
            }
            return APAD_SCREEN_QRSCAN;

        case QR_FOUND:
            /* The camera is already released. Any button skips the wait. */
            if (--s_hold <= 0 || app_pressed(ctx, 0xFFFFFFFFu)
                || ctx->touch_pressed) {
                return APAD_SCREEN_CONNECT;
            }
            return APAD_SCREEN_QRSCAN;

        default: /* QR_FAILED */
            if (cancelled(ctx) || app_pressed(ctx, KEY_A)
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

    ui_textf(UI_TOP_W * 0.5f, 92.0f, UI_S_HUGE, ui_c_text(), UI_ALIGN_CENTER,
             "%s", s_found_ip);
    ui_textf(UI_TOP_W * 0.5f, 146.0f, UI_S_HEAD, ui_c_dim(), UI_ALIGN_CENTER,
             "port %u", s_found_port);
}

static void draw_failed_top(app_ctx *ctx)
{
    const char *why = apad3ds_cam_status();

    ui_header(UI_TOP_W, "AtticPad -- scan QR", "camera unavailable",
              ui_c_bad());

    ui_textf(UI_TOP_W * 0.5f, 60.0f, UI_S_HEAD, ui_c_bad(), UI_ALIGN_CENTER,
             "the camera did not start");
    ui_textf_fit(UI_TOP_W * 0.5f, 96.0f, UI_S_SMALL, ui_c_text(),
                 UI_ALIGN_CENTER, UI_TOP_W - 16.0f,
                 "Another app may be using the camera - close it and "
                 "relaunch");
    ui_textf_fit(UI_TOP_W * 0.5f, 176.0f, UI_S_SMALL, ui_c_dim(),
                 UI_ALIGN_CENTER, UI_TOP_W - 16.0f,
                 "you can type the address instead");

    /* Hidden-diagnostics convention (2026-08-12, "hide the debug surface"
     * pass): this "details:" line is raw camera-error text, no different in
     * kind from screen_fatal.c's, but SELECT is idle on this substate (it is
     * only read in QR_SCAN, to reach self-test) so it is free to gate this
     * one line instead of removing it -- hold it to read the raw reason. */
    if (ctx->keys_held & KEY_SELECT) {
        ui_textf_fit(UI_TOP_W * 0.5f, 132.0f, UI_S_TINY, ui_c_border(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f, "details: %s",
                     (why != NULL) ? why
                                   : (s_reject[0] != '\0' ? s_reject
                                                          : "reason unknown"));
    }
}

/* THE CAPTURE TRACE, over the viewfinder rather than instead of it.
 *
 * A scrim, not an opaque panel: at 0xD0 alpha the image underneath is still
 * legible enough to aim by while the numbers are read off, which matters
 * because the trace is up for the whole of any unhealthy run and an unhealthy
 * run can still be one that decodes.
 *
 * The colour is built here with C2D_Color32() rather than taken from ui.h's
 * palette because every palette entry is opaque (ui.c: alpha 0xFF) -- a
 * translucent overlay is a thing only this diagnostic screen wants. */
static void draw_capture_trace(void)
{
    unsigned i;

    ui_rect(0.0f, 26.0f, UI_TOP_W, 126.0f,
            C2D_Color32(0x0A, 0x0E, 0x14, 0xD0));

    for (i = 0u; i < (unsigned)APAD3DS_CAM_DEBUG_LINES; i++) {
        const char *l = apad3ds_cam_debug(i);

        if (l == NULL) {
            break;
        }
        ui_textf_fit(UI_TOP_W * 0.5f, 30.0f + 15.0f * (float)i, UI_S_TINY,
                     (i == 0u) ? ui_c_accent() : ui_c_text(),
                     UI_ALIGN_CENTER, UI_TOP_W - 12.0f, "%s", l);
    }
}

static void draw_scan_top(void)
{
    /* The trace is shown whenever the pipeline has been CONTINUOUSLY unwell
     * for more than UNWELL_GRACE_MS, not the instant it goes unwell: the
     * pipeline is unwell by definition for its first second or so on every
     * single visit to this screen (camera_3ds.h, apad3ds_cam_unhealthy), and
     * showing the eight-line debug wall for that whole ordinary startup was
     * the 2026-08-12 hardware report ("shows the debug capture-trace during
     * its first seconds"). trace_grace_expired() is the timer; a dead
     * capture loop that never recovers still surfaces the trace exactly like
     * before, just three seconds later than a healthy startup does. */
    const int due = trace_grace_expired();
    const char *right = !apad3ds_cam_running() ? "starting the camera..."
                      : due                    ? "capture trace"
                                               : "aim at the code";

    if (apad3ds_cam_running() && apad3ds_cam_frames() > 0u) {
        /* Full-bleed: the decoder reads the WHOLE frame, so anything visible
         * here is inside the search area and there is no reticle to draw
         * that would not be a lie about where the code has to sit. */
        C2D_DrawImageAt(apad3ds_cam_image(), 0.0f, 0.0f, 0.0f, NULL, 1.0f,
                        1.0f);
    } else {
        ui_textf_fit(UI_TOP_W * 0.5f, 168.0f, UI_S_HEAD, ui_c_dim(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f,
                     "starting the camera ...");
    }

    if (due || s_pin_trace) {
        /* Healthy scans, and the first few seconds of any scan, show only
         * the viewfinder. X (unadvertised, same spirit as SELECT elsewhere)
         * pins this trace up immediately, grace period or not, to bring
         * every counter back for a bug report. */
        draw_capture_trace();
    }

    /* Bands rather than bare text: unreadable white-on-anything is the
     * default outcome of putting a label over a live camera image. */
    ui_header(UI_TOP_W, "AtticPad -- scan QR", right, ui_c_accent());

    ui_rect(0.0f, 204.0f, UI_TOP_W, 36.0f, ui_c_panel());
    ui_rect(0.0f, 204.0f, UI_TOP_W, 1.0f, ui_c_border());
    if (s_reject_frames > 0 && s_reject[0] != '\0') {
        ui_textf_fit(UI_TOP_W * 0.5f, 208.0f, UI_S_SMALL, ui_c_warn(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f, "%s", s_reject);
    } else {
        ui_textf_fit(UI_TOP_W * 0.5f, 208.0f, UI_S_SMALL, ui_c_text(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f,
                     "point the outer camera at the QR code on the server");
    }
}

static void qrscan_draw_top(app_ctx *ctx)
{
    switch (s_sub) {
        case QR_FOUND:  draw_found_top();      break;
        case QR_FAILED: draw_failed_top(ctx);  break;
        default:        draw_scan_top();       break;
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

    ui_header(UI_BOT_W, "scan QR", (s_sub == QR_FAILED) ? "failed" : NULL,
              (s_sub == QR_FAILED) ? ui_c_bad() : ui_c_dim());

    if (s_pin_trace) {
        /* The diagnostic lines, only while X has the trace pinned. Every
         * number answers a question someone standing in front of a console
         * that will not scan actually asks: frames delivered, decoder runs,
         * decode cost, and WHICH capture mode produced them (Y rotates the
         * mode). */
        ui_textf_fit(10.0f, 140.0f, UI_S_TINY, ui_c_border(), UI_ALIGN_LEFT, W,
                     "frames %u   buf-errors %u   decodes %u   last %lu ms",
                     apad3ds_cam_frames(), apad3ds_cam_errors(), s_decode_count,
                     (unsigned long)s_decode_cost_ms);
        ui_textf_fit(10.0f, 153.0f, UI_S_TINY, ui_c_accent(), UI_ALIGN_LEFT, W,
                     "mode %s", apad3ds_cam_mode_name(apad3ds_cam_mode()));
    }

    ui_button(&kCancelBtn, "CANCEL", s_flash_cancel, 0);
    s_flash_cancel = 0;

    ui_textf_fit(UI_BOT_W * 0.5f, 224.0f, UI_S_TINY, ui_c_dim(),
                 UI_ALIGN_CENTER, W,
                 "B: back");
}

const apad_screen apad_screen_qrscan = {
    "scan QR",
    qrscan_enter,
    qrscan_update,
    qrscan_draw_top,
    qrscan_draw_bottom
};
