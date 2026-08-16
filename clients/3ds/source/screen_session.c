/* clients/3ds/source/screen_session.c
 *
 * The live session screen. Everything protocol-shaped is one call --
 * apad_client_pump() from clients/common/apad_client.c -- and this file only
 * decides what to draw and which screen to go to next.
 *
 * UNTIL 2026-08-12, physical START disconnected and SELECT opened the
 * self-test, which meant a real session's virtual pad never had a usable
 * Start or Select -- fine for the diagnostics this screen was built to prove
 * out, and wrong for actually playing something. Both UI actions now live as
 * touch buttons in the footer strip (kButtonStrip below), and START/SELECT
 * flow to the wire like every other button (main.c's app_sample_input()).
 * Two behaviours survive from the console-input era, now on the touch
 * buttons instead of the keys that used to carry them:
 *
 *   - DISCONNECT returns to the CONNECT screen. Leaving the app entirely is
 *     a separate, deliberate action (B on the connect screen) -- this pass
 *     changes only which input fires the action, not what the action does.
 *   - SELF-TEST asks first, because running it DOES drop the session:
 *     apad_selftest_run() is one blocking call with no pump inside it, the
 *     server stops hearing INPUT_STATE, and it reaps the session per
 *     docs/PROTOCOL.md S8 -- correctly. The confirmation prompt is drawn as
 *     an overlay while the pump KEEPS RUNNING underneath, so reading the
 *     warning cannot itself cost the session (S11's idle timeout is 3000ms;
 *     a person reading two sentences takes longer). Answering yes then sends
 *     a proper BYE first, which frees the pad slot immediately instead of
 *     leaving the server to time it out.
 *
 * DISCONNECT got the same confirm-overlay treatment for the same reason a
 * touch button always needs it that a physical key mostly doesn't: a stray
 * brush against the bottom screen during play is far likelier than an
 * accidental START press, and there is no un-disconnecting a live session.
 *
 * SELECT ALSO TOGGLES THE HIDDEN DIAGNOSTICS OVERLAY (2026-08-12, "hide the
 * debug surface" pass), on top of flowing to the wire above -- the two do
 * not conflict, because this only flips ctx->show_diag (app.h) for what THIS
 * client draws (app_draw_status_top()'s server/session panel and live input
 * readout, app_draw_gyro_line(), and the raw STATUS/ERROR code below). It
 * never touches ctx->st.buttons, which main.c already filled before
 * update() ran, so a game still sees every SELECT press exactly as before.
 * Off by default: the top screen used to show session id, pad slot, tx/rx
 * packet counts, the caps hex mask and a live button/stick/touch readout to
 * every user on every frame, which is precisely the "debug features showing
 * up everywhere" this pass exists to fix. The big RTT figure is the one
 * exception -- docs/CONVENTIONS.md calls it product, not debug -- and stays up
 * regardless of this flag.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config_3ds.h"

/* How long apad_client_pump() may block waiting for an inbound datagram. It
 * stops waiting as soon as the next INPUT_STATE falls due, so this is a
 * ceiling on IDLE waiting and not on the send cadence.
 *
 * At the default 60 Hz it never binds: a send is already due when this loop
 * reaches pump() one vblank (16.7ms) after the last one, the wait computes to
 * zero, and the drain is non-blocking. It binds only when the SERVER
 * negotiates a slower rate (S6.4), and that is why it is half a frame rather
 * than a whole one: at 30 Hz a 16ms ceiling would leave nothing of the frame
 * for the GPU, the frame would land after vblank, and the redraw -- RTT
 * readout included -- would drop to 30 Hz for no gain. */
#define PUMP_WAIT_MS 8

/* How long the connect screen counts down before reconnecting to the same
 * server after a session ends. Long enough to read the reason, short enough
 * that an unattended console recovers on its own. */
#define RECONNECT_FRAMES 300   /* ~5s */

enum { SS_LIVE = 0, SS_CONFIRM_SELFTEST, SS_CONFIRM_DISCONNECT };
static int s_sub;

/* Latched so a tapped footer button can be drawn held for the one frame it
 * was tapped on -- an unlit button gives no feedback at all, same reason
 * screen_connect.c's s_flash_* exist. Cleared after drawing. */
static int s_flash_disconnect;
static int s_flash_selftest;

/* S6.10/S6.11 STATUS and ERROR both surface here. The human-readable text
 * (apad_client_message(), fetched fresh at draw time -- it is not copied
 * into a local buffer here) is shown for STATUS_MSG_TTL_FRAMES after it
 * changes and then drops off; the numeric code is diagnostics-overlay only
 * (ctx->show_diag), where it is shown for as long as the overlay is on,
 * "details:"-style raw information rather than a customer-facing line. */
static uint32_t s_last_status_serial;
static int32_t  s_last_error_code;
static int      s_msg_is_error;
static int      s_msg_fresh_frames;
#define STATUS_MSG_TTL_FRAMES 300   /* ~5s at 60Hz                           */

static const ui_box kConfirmBox    = {  24.0f,  52.0f, 272.0f, 132.0f };
static const ui_box kConfirmRun    = {  40.0f, 146.0f, 110.0f,  30.0f };
static const ui_box kConfirmCancel = { 170.0f, 146.0f, 110.0f,  30.0f };

/* THE RESERVED RECTANGLE. The touch-mapping panel above ends at y=216
 * (session_draw_bottom's `bottom`); everything from there to the screen edge
 * is this screen's own UI, not part of the LT/RT touch surface the server
 * maps. A contact anywhere in kButtonStrip is UI-only and must never reach
 * the wire as a touch contact or move the LT/RT halves -- session_update()
 * clears ctx->st's touch fields for any point inside it, checked with the
 * exact same box the two buttons below are drawn and hit-tested with. One
 * set of numbers, not two copies that can drift (the numpad taught this
 * client that lesson once already). */
static const ui_box kButtonStrip   = {   0.0f, 216.0f, UI_BOT_W, UI_BOT_H - 216.0f };
static const ui_box kDisconnectBtn = {   4.0f, 217.0f, 152.0f,  22.0f };
static const ui_box kSelftestBtn   = { 164.0f, 217.0f, 152.0f,  22.0f };

/* ------------------------------------------------------------------------ */

static void session_enter(app_ctx *ctx)
{
    (void)ctx;
    s_sub = SS_LIVE;
    s_flash_disconnect = 0;
    s_flash_selftest = 0;
    s_last_status_serial = 0u;
    s_last_error_code = 0;
    s_msg_is_error = 0;
    s_msg_fresh_frames = 0;
}

/* Leaves the session cleanly and hands the connect screen a reason plus a
 * countdown. Every exit from this screen goes through here so that none of
 * them can forget the countdown -- the whole point of the return path. */
static apad_screen_id leave_to_connect(app_ctx *ctx, int level,
                                       const char *reason)
{
    ctx->connected = 0;
    ctx->reconnect_frames = RECONNECT_FRAMES;
    app_note(ctx, level, "%s", reason);
    return APAD_SCREEN_CONNECT;
}

static apad_screen_id session_update(app_ctx *ctx)
{
    int state;

    /* The hidden diagnostics toggle (see this file's header comment). Read
     * before the confirm-overlay branch below so it works regardless of
     * s_sub -- it only ever touches ctx->show_diag, never the confirm
     * prompt's own A/B handling. */
    if (app_pressed(ctx, KEY_SELECT)) {
        ctx->show_diag = !ctx->show_diag;
    }

    if (s_sub == SS_CONFIRM_SELFTEST || s_sub == SS_CONFIRM_DISCONNECT) {
        int yes = app_pressed(ctx, KEY_A)
               || (ctx->touch_pressed
                   && ui_box_hit(&kConfirmRun, (int)ctx->touch.px,
                                 (int)ctx->touch.py));
        int no  = app_pressed(ctx, KEY_B)
               || (ctx->touch_pressed
                   && ui_box_hit(&kConfirmCancel, (int)ctx->touch.px,
                                 (int)ctx->touch.py));

        if (yes && s_sub == SS_CONFIRM_SELFTEST) {
            /* Send the BYE ourselves rather than letting the server reap a
             * silent client: the pad slot comes back at once, and the
             * ERROR-7 recovery path stops being part of a routine action. */
            apad_client_disconnect(ctx->client);
            ctx->connected = 0;
            ctx->reconnect_frames = RECONNECT_FRAMES;
            ctx->selftest_return = APAD_SCREEN_CONNECT;
            app_note(ctx, 0, "session closed for the self-test");
            return APAD_SCREEN_SELFTEST;
        }
        if (yes && s_sub == SS_CONFIRM_DISCONNECT) {
            apad_client_disconnect(ctx->client);
            return leave_to_connect(ctx, 0, "disconnected (touch)");
        }
        if (no) {
            s_sub = SS_LIVE;
        }
        /* fall through: the pump below keeps running while the prompt is up */
    } else if (ctx->touch_pressed
               && ui_box_hit(&kDisconnectBtn, (int)ctx->touch.px,
                             (int)ctx->touch.py)) {
        s_flash_disconnect = 1;
        s_sub = SS_CONFIRM_DISCONNECT;
    } else if (ctx->touch_pressed
               && ui_box_hit(&kSelftestBtn, (int)ctx->touch.px,
                             (int)ctx->touch.py)) {
        s_flash_selftest = 1;
        s_sub = SS_CONFIRM_SELFTEST;
    }

    /* THE TOUCH EXCLUSION. Any contact still inside kButtonStrip this frame
     * -- tapped, held, or dragged -- is UI-only and must not leak onto the
     * wire, whether as a touch contact or (via the server's LT/RT profile
     * mapping over touch.x) as a phantom shoulder squeeze. main.c's
     * app_sample_input() already filled ctx->st from this frame's
     * touchPosition before update() ran, so this clears it back out rather
     * than never setting it -- the one deliberate exception to "the readout
     * and the wire cannot disagree" below, and it keeps that promise too:
     * the lamps and TX/TY readout use ctx->st, so they go dark for the same
     * contact the wire never sees. */
    if (ctx->touch_held
        && ui_box_hit(&kButtonStrip, (int)ctx->touch.px, (int)ctx->touch.py)) {
        ctx->st.buttons &= ~(uint32_t)APAD_BTN_TOUCH_PRESS;
        ctx->st.touch_count = 0;
    }

    /* -- one turn of the session. ctx->st was filled by main.c from the same
     * hardware scan this frame's on-screen readout draws from, so the display
     * and the wire cannot disagree (touch inside kButtonStrip excepted, just
     * above). client_ticks_ms is deliberately not set here: the engine
     * stamps it when the datagram is actually built, which is what S6.5
     * wants and is closer to the wire than a value taken at the top of a
     * frame that may then wait for a datagram. */
    state = apad_client_pump(ctx->client, &ctx->st, PUMP_WAIT_MS);
    apad_client_get_stats(ctx->client, &ctx->stats);

    /* Which of STATUS/ERROR it was has to be inferred from error_code MOVING
     * -- that field is sticky, so a second ERROR carrying the same code as the
     * first reads as a STATUS. Only the label is affected. */
    if (ctx->stats.status_serial != s_last_status_serial) {
        s_msg_is_error = (ctx->stats.error_code != s_last_error_code);
        s_last_status_serial = ctx->stats.status_serial;
        s_last_error_code = ctx->stats.error_code;
        s_msg_fresh_frames = STATUS_MSG_TTL_FRAMES;
    }
    if (s_msg_fresh_frames > 0) {
        s_msg_fresh_frames--;
    }

    if (state != APAD_CLIENT_ACTIVE) {
        return leave_to_connect(ctx, 2,
                                app_close_reason_text((int)ctx->stats.close_reason));
    }

    if (!ctx->connected) {
        /* THE ctx->connected 0->1 EDGE: the first frame THIS connect reached
         * ACTIVE, docs/PROTOCOL.md S8's handshake just finished. Persist
         * ip_text/port_text (config_3ds.c: sdmc:/3ds/atticpad/atticpad.cfg)
         * so a RETURNING unit's connect screen prefills next launch --
         * main.c's boot-defaults comment is the other half of this. Gated on
         * the edge, not on every ACTIVE frame, so a long session does not
         * touch the SD card on every tick; and gated on ACTIVE, not on
         * anything typed but not yet connected, so a mistyped address is
         * never what gets saved. Never the pairing secret -- it never
         * reaches config_3ds.c at all (config_3ds.h). */
        apad3ds_config_save(ctx->ip_text, ctx->port_text);
    }
    ctx->connected = 1;
    return APAD_SCREEN_SESSION;
}

/* ------------------------------------------------------------------------ */

static void session_draw_top(app_ctx *ctx)
{
    app_draw_status_top(ctx, 1);
    app_draw_gyro_line(ctx, 8.0f, UI_STATUS_BOTTOM + 1.0f);

    /* The STATUS/ERROR line. Diagnostics overlay ON: the raw code, "details:"
     * style, for as long as the overlay stays on -- the numeric code is
     * exactly the kind of thing that belongs behind ctx->show_diag, not in
     * front of every user. Diagnostics overlay OFF: human text only, no
     * numeric prefix, and only for STATUS_MSG_TTL_FRAMES after it last
     * changed -- a STATUS/ERROR line that never goes away reads as a
     * permanent warning long after it stopped being news. */
    if (ctx->show_diag) {
        ui_textf_fit(8.0f, 212.0f, UI_S_TINY,
                     s_msg_is_error ? ui_c_bad() : ui_c_dim(), UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f, "%s %u: %s",
                     s_msg_is_error ? "ERROR" : "STATUS",
                     (unsigned)ctx->stats.status_code,
                     apad_client_message(ctx->client));
    } else if (s_msg_fresh_frames > 0) {
        ui_textf_fit(8.0f, 212.0f, UI_S_TINY,
                     s_msg_is_error ? ui_c_bad() : ui_c_dim(), UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f, "%s", apad_client_message(ctx->client));
    }
    ui_textf_fit(8.0f, 225.0f, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT,
                 UI_TOP_W - 16.0f,
                 "disconnect / self-test: bottom screen buttons");
}

/* The bottom screen is the touch surface the server maps, so it draws what
 * the mapping does with it: docs/PROTOCOL.md S6.2 / server/profiles/
 * 3ds-default.jsonc splits it in half, left -> LT and right -> RT, both
 * analog.
 *
 * THIS IS A HINT, NOT A FACT, and it is labelled as one on screen. The client
 * has no way to know what profile the server actually loaded: profiles are
 * server-side files matched against the device_name in HELLO, nothing in the
 * v1 wire format reports the resulting mapping back, and a user who edited
 * 3ds-default.jsonc would see this overlay lie to them. Making it truthful
 * would need a new message type, which is a protocol change -- out of scope
 * for a UI pass and not obviously worth a wire change. So: shipped-default
 * only, said out loud.
 */
/* v2 EXPERIMENT (branch experiment/touchmap-v2).
 *
 * Draw the layout the SERVER sent, rather than the compiled-in guess below.
 * The old drawing hardcoded an LT/RT half-split and had to caption itself
 * "shipped DEFAULT profile only -- the server may have an edited one",
 * because the client genuinely could not know: an edited profile, or any
 * profile matched by device name, produced a screen that was confidently
 * wrong. With TOUCHMAP the caption is unnecessary -- the screen is the
 * mapping.
 *
 * Region rects arrive normalised 0..255, +Y down, which is this screen's own
 * coordinate convention, so this is a scale with no flip.
 *
 * Labels are resolved LOCALLY from the §5.7 pad_bit: the wire carries no
 * text, so this console renders "L"/"ZL" from its own table while another
 * platform can render its own names from the identical packet.
 */
static const char *touchmap_label(const apad_touch_region_wire *r)
{
    if (r->target == 1u) { return "LT"; }
    if (r->target == 2u) { return "RT"; }
    /* pad_bit is the OUTPUT pad's button (Xbox naming), not the Nintendo
     * APAD_BTN_* this client sends -- a touch region says what the PC will
     * see, which is the useful thing to show. The constants live in
     * protocol.h precisely so a client can do this by name. */
    switch (r->pad_bit) {
    case APAD_PADBTN_A:      return "A";
    case APAD_PADBTN_B:      return "B";
    case APAD_PADBTN_X:      return "X";
    case APAD_PADBTN_Y:      return "Y";
    case APAD_PADBTN_LB:     return "LB";
    case APAD_PADBTN_RB:     return "RB";
    case APAD_PADBTN_BACK:   return "SELECT";
    case APAD_PADBTN_START:  return "START";
    case APAD_PADBTN_GUIDE:  return "HOME";
    case APAD_PADBTN_LTHUMB: return "L3";
    case APAD_PADBTN_RTHUMB: return "R3";
    default:         return "?";
    }
}

/* Returns 1 if it drew a server-supplied layout, 0 if there is none to draw
 * and the caller should fall back. */
static int session_draw_bottom_from_touchmap(app_ctx *ctx)
{
    const apad_touchmap *tm = &ctx->stats.touchmap;
    const float top = 24.0f, bottom = 216.0f;
    const float usable_h = bottom - top;
    unsigned i;

    if (ctx->stats.touchmap_serial == 0u || tm->region_count == 0u) {
        return 0;
    }

    ui_header(UI_BOT_W, "touch mapping", "from server", ui_c_dim());

    for (i = 0; i < tm->region_count; i++) {
        const apad_touch_region_wire *r = &tm->regions[i];
        float x0 = (float)r->x0 / 255.0f * UI_BOT_W;
        float x1 = (float)r->x1 / 255.0f * UI_BOT_W;
        float y0 = top + (float)r->y0 / 255.0f * usable_h;
        float y1 = top + (float)r->y1 / 255.0f * usable_h;
        float w  = x1 - x0, h = y1 - y0;
        int   hit;

        if (w <= 0.0f || h <= 0.0f) {
            continue;   /* a degenerate rect is the server's bug, not a crash */
        }
        hit = (ctx->st.touch_count > 0)
              && ((float)ctx->touch.px >= x0) && ((float)ctx->touch.px < x1)
              && ((float)ctx->touch.py >= y0) && ((float)ctx->touch.py < y1);

        ui_rect(x0, y0, w, h, hit ? ui_c_panel_hi() : ui_c_panel());
        ui_outline(x0, y0, w, h, 1.0f, ui_c_border());
        ui_textf(x0 + w * 0.5f, y0 + h * 0.5f - 12.0f, 1.30f,
                 hit ? ui_c_accent() : ui_c_text(), UI_ALIGN_CENTER,
                 "%s", touchmap_label(r));
        if (r->analog) {
            ui_textf_fit(x0 + w * 0.5f, y0 + h * 0.5f + 26.0f, UI_S_TINY,
                         ui_c_dim(), UI_ALIGN_CENTER, w - 8.0f,
                         "analog: slide to squeeze");
        }
    }

    if (ctx->st.touch_count > 0) {
        C2D_DrawCircleSolid((float)ctx->touch.px, (float)ctx->touch.py, 0.0f,
                            6.0f, ui_c_good());
    }
    /* Deliberately does NOT draw the footer buttons or the confirm overlay:
     * this function owns the region area only, and its caller owns everything
     * below it. Drawing them here and returning early is what broke
     * DISCONNECT and SELF-TEST -- the tap registered and set s_sub, but the
     * confirmation panel lives past the early return, so nothing appeared and
     * the YES/NO targets were invisible. The button was never unresponsive;
     * it was answering a prompt nobody could see. */
    return 1;
}

static void session_draw_bottom(app_ctx *ctx)
{
    const float split = UI_BOT_W * 0.5f;
    const float top = 24.0f, bottom = 216.0f;
    int touch_left  = (ctx->st.touch_count > 0) && (ctx->touch.px < (u16)split);
    int touch_right = (ctx->st.touch_count > 0) && (ctx->touch.px >= (u16)split);

    /* Server-supplied layout when there is one, the compiled-in guess when
     * there is not. Either way the footer below runs. */
    if (!session_draw_bottom_from_touchmap(ctx)) {

    ui_header(UI_BOT_W, "touch mapping", "3ds-default", ui_c_dim());

    ui_rect(0.0f, top, split, bottom - top,
            touch_left ? ui_c_panel_hi() : ui_c_panel());
    ui_rect(split, top, UI_BOT_W - split, bottom - top,
            touch_right ? ui_c_panel_hi() : ui_c_panel());
    ui_rect(split - 1.0f, top, 2.0f, bottom - top, ui_c_border());
    ui_outline(0.0f, top, UI_BOT_W, bottom - top, 1.0f, ui_c_border());

    ui_textf(split * 0.5f, 88.0f, 1.30f,
             touch_left ? ui_c_accent() : ui_c_text(), UI_ALIGN_CENTER, "LT");
    ui_textf(split * 1.5f, 88.0f, 1.30f,
             touch_right ? ui_c_accent() : ui_c_text(), UI_ALIGN_CENTER, "RT");
    ui_textf_fit(split * 0.5f, 136.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_CENTER,
                 split - 8.0f, "analog: slide down to squeeze");
    ui_textf_fit(split * 1.5f, 136.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_CENTER,
                 split - 8.0f, "analog: slide down to squeeze");

    /* Live contact point: the one part of this screen that is a measurement
     * rather than a hint. */
    if (ctx->st.touch_count > 0) {
        C2D_DrawCircleSolid((float)ctx->touch.px, (float)ctx->touch.py, 0.0f,
                            6.0f, ui_c_good());
    }

    ui_textf_fit(UI_BOT_W * 0.5f, 32.0f, UI_S_TINY, ui_c_dim(),
                 UI_ALIGN_CENTER, UI_BOT_W - 12.0f,
                 "shipped DEFAULT profile only -- the server may have an "
                 "edited one");

    }

    /* Shared footer, reached by BOTH layouts.
     *
     * kButtonStrip: the reserved rectangle a touch inside never reaches the
     * wire (see session_update()'s exclusion check, which hit-tests the same
     * box). These two ui_button() calls are its only content, styled like
     * the connect screen's buttons (reuse, not a new look). */
    ui_button(&kDisconnectBtn, "DISCONNECT", s_flash_disconnect, 0);
    ui_button(&kSelftestBtn, "SELF-TEST", s_flash_selftest, 0);
    s_flash_disconnect = 0;
    s_flash_selftest = 0;

    if (s_sub == SS_CONFIRM_SELFTEST || s_sub == SS_CONFIRM_DISCONNECT) {
        int is_disconnect = (s_sub == SS_CONFIRM_DISCONNECT);
        ui_panel(&kConfirmBox, ui_c_panel(), ui_c_warn());
        const float cw = kConfirmBox.w - 20.0f;

        if (is_disconnect) {
            ui_textf_fit(kConfirmBox.x + kConfirmBox.w * 0.5f,
                         kConfirmBox.y + 8.0f, UI_S_BODY, ui_c_warn(),
                         UI_ALIGN_CENTER, cw, "Disconnect now?");
            ui_textf_fit(kConfirmBox.x + 10.0f, kConfirmBox.y + 34.0f,
                         UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, cw,
                         "The session ends immediately (BYE) and");
            ui_textf_fit(kConfirmBox.x + 10.0f, kConfirmBox.y + 50.0f,
                         UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, cw,
                         "the connect screen counts down before");
            ui_textf_fit(kConfirmBox.x + 10.0f, kConfirmBox.y + 66.0f,
                         UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, cw,
                         "reconnecting to the same server.");
            ui_button(&kConfirmRun, "DISCONNECT (A)", 0, 1);
        } else {
            ui_textf_fit(kConfirmBox.x + kConfirmBox.w * 0.5f,
                         kConfirmBox.y + 8.0f, UI_S_BODY, ui_c_warn(),
                         UI_ALIGN_CENTER, cw, "This drops the session");
            ui_textf_fit(kConfirmBox.x + 10.0f, kConfirmBox.y + 34.0f,
                         UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, cw,
                         "The self-test blocks for its whole run,");
            ui_textf_fit(kConfirmBox.x + 10.0f, kConfirmBox.y + 50.0f,
                         UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, cw,
                         "so this session is closed first (BYE)");
            ui_textf_fit(kConfirmBox.x + 10.0f, kConfirmBox.y + 66.0f,
                         UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, cw,
                         "and reconnected automatically after.");
            ui_button(&kConfirmRun, "RUN (A)", 0, 1);
        }
        ui_button(&kConfirmCancel, "CANCEL (B)", 0, 0);
    }
}

const apad_screen apad_screen_session = {
    "session",
    session_enter,
    session_update,
    session_draw_top,
    session_draw_bottom
};
