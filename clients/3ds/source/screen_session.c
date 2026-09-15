/* clients/3ds/source/screen_session.c
 *
 * The live session screen. Everything protocol-shaped is one call --
 * apad_client_pump_ex() from clients/common/apad_client.c -- and this file
 * only decides what to draw and which screen to go to next.
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
 *
 * ---------------------------------------------------------------------- *
 * KEYBOARD / MOUSE / MEDIA MODES (this pass). docs/PROTOCOL.md S6.15-S6.19.
 *
 * A MODE within this screen, not four new screens: the session is one live
 * thing (one enter/update pair, one pump call) and the bottom screen's
 * header band (y 0..24, kModeStrip) becomes a 4-way tab strip -- PAD / MOUSE
 * / KEYS / MEDIA -- switching what the REST of the bottom screen (y 24..216)
 * shows. Nothing shrinks: the touch-mapping panel already started at y=24
 * and drew its own "touch mapping" header in that same 0..24 band, so the
 * tab strip simply replaces that per-mode header with one shared one. The
 * footer (kButtonStrip, y 216..240 -- DISCONNECT / SELF-TEST) and the top
 * screen are unaffected by the mode and drawn exactly as before in every one.
 *
 * WHO OWNS WHAT, restated for the three new modes: this file NEVER encodes a
 * KEYBOARD/MOUSE/MEDIA packet, computes an event ring ordinal, or decides
 * §6.19's gate -- clients/common/apad_client.c does every one of those
 * (kbm_gate(), kbm_pump(), kbm_ring_push() and friends in that file). This
 * screen also does not run the touch state machines any more (2026-09-09):
 * the resistive-panel mouse settle/anchor/tap/drag-lock machinery and the
 * sticky SHIFT/CTRL latch moved to clients/common/apad_kbm_touch.c
 * (apad_kbm_touch_build_mouse/_keyboard/_media, driven off the s_touch
 * struct below) so the NDS client can link the SAME tested logic instead of
 * a second copy -- see that file's own header comment for the mechanism.
 * This screen's only job is building one apad_kbm_touch_input from
 * ctx->touch/ctx->keys_held, handing it to those three builders to get one
 * apad_client_kbm_in snapshot per frame -- "what is held right now, given
 * which mode is on screen" -- and handing THAT to apad_client_pump_ex().
 * Three consequences of the engine already owning the hard parts, worth
 * stating up front:
 *
 *   1. THE GATE IS ALREADY THE ENGINE'S JOB, so this file re-derives
 *      `have_kb`/`have_mo`/`have_me` from ctx->stats.inputcaps only to decide
 *      what to DRAW (a dim, non-hit-testable tab) and what to BUILD (skip the
 *      work for a facility nobody will send) -- never to decide whether a
 *      packet goes out. Passing kbm.have=1 for a facility the server never
 *      advertised costs nothing: kbm_gate() in the engine drops it.
 *   2. EVERY BUILDER RUNS EVERY FRAME, REGARDLESS OF THE ACTIVE MODE, with an
 *      `active` flag that is 0 whenever its mode is not the one on screen.
 *      This is what makes leaving a mode safe without any special "clean up
 *      on tab switch" code: an inactive builder sees synthetic "nothing
 *      touched" input, which drives whatever it was holding through its own
 *      ordinary release path on the very next frame -- a stuck key from
 *      switching away from KEYS mid-press cannot happen because there is no
 *      code path that skips calling the builder at all.
 *   3. NO BUILDER HERE HAND-RESEMBLES §6.20's EVENT RING for a held state.
 *      `keys[]`/`buttons`/`held` are recomputed FRESH every frame from
 *      current touch/pad state and handed to the engine as a snapshot; the
 *      engine's own diff against its last-sent copy synthesises the
 *      down/up transitions (kbm.h's own header comment says a snapshot-only
 *      caller is correct for everything except a tap SHORTER than one pump
 *      interval). Since this client's touch sampling and its pump both run
 *      once per rendered frame (session_update() calls
 *      apad_client_pump_ex() exactly once per app_sample_input()), nothing
 *      here can ever observe a transition faster than its own pump interval
 *      -- so plain snapshotting is not just sufficient, it is the only thing
 *      that could ever be exercised by real touch on this client. The one
 *      deliberate exception is MOUSE mode's tap-to-click gesture
 *      (apad_kbm_touch_build_mouse, clients/common/apad_kbm_touch.c): a
 *      "tap" is a UI decision the builder makes AFTER the fact (short
 *      contact, little movement), not a raw hardware state, so there is no
 *      `buttons` bit that was ever "held" for the engine to diff -- the
 *      down+up pair is submitted explicitly into events[] in the one pump
 *      call that recognises the tap, which is exactly the
 *      events[]-is-a-submission-queue case apad_client.h documents. (The
 *      queue's OTHER purpose -- surviving a tap shorter than the pump
 *      interval itself -- is proven independently by tools/engine-client
 *      --kbm against the same engine; see this task's report for the evtest
 *      capture.)
 *
 * STICKY MODIFIERS (KEYS mode). The touchscreen is resistive and reports one
 * contact, so there is no chording two fingers the way a real keyboard's
 * Shift+letter works. Tapping the on-screen SHIFT/CTRL cell toggles a LATCH
 * (apad_kbm_touch_build_keyboard's key_shift_armed/key_ctrl_armed, in the
 * shared s_touch struct below) that ORs the modifier's HID usage into
 * keys[] starting the moment it is armed, and
 * disarms itself the instant the NEXT ordinary key cell is released --
 * "armed for the next key", exactly one. Physical L and R are the other,
 * LIVE half: held exactly as long as the shoulder button is, on top of
 * whatever the latch is doing, so a real chord (Shift+letter, Ctrl+C) is one
 * finger on a letter plus a shoulder button, not two taps -- the one thing
 * the 3DS's extra buttons buy over a phone's flat glass that a phone client
 * cannot get without a whole second modifier row.
 *
 * L/R DOUBLE DUTY, and why the pad wire never sees it. In MOUSE mode L/R are
 * left/right click; in KEYS mode they are live Shift/Ctrl. Either way
 * main.c's app_sample_input() already set APAD_BTN_L/APAD_BTN_R in ctx->st
 * before this screen's update() ran (same struct every screen samples so the
 * connect screen's readout proves the buttons work before the network is
 * involved), so session_update() clears those two bits back out of
 * ctx->st.buttons for the frame whenever a mode has repurposed them -- the
 * exact "clear it back out rather than never set it" pattern kButtonStrip
 * already uses for touch, and for the identical reason: pressing Shift must
 * not also squeeze a phantom L shoulder button on whatever game the server
 * profile is driving. The RAW sample (ctx->keys_held & KEY_L/KEY_R, not the
 * cleared ctx->st.buttons) is what feeds apad_kbm_touch_input.l_held/r_held
 * below, so this clearing step and the shared builders' live-modifier
 * reading are independent of each other's order.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apad_kbm_touch.h"
#include "apad_glyph_mouse.h"
#include "config_3ds.h"

/* How long apad_client_pump_ex() may block waiting for an inbound datagram.
 * It stops waiting as soon as the next INPUT_STATE falls due, so this is a
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
 * (session_draw_pad's `bottom`); everything from there to the screen edge
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
/* mode strip (PAD / MOUSE / KEYS / MEDIA)                                  */
/* ------------------------------------------------------------------------ */

enum { MODE_PAD = 0, MODE_MOUSE, MODE_KEYS, MODE_MEDIA, MODE_COUNT };
static int s_mode;

/* The shared touch/mouse/keyboard/media builder state (clients/common/
 * apad_kbm_touch.h) for this screen's single live session -- one instance,
 * because this screen only ever drives one session at a time. Boxes and
 * runtime state are (re)computed by apad_kbm_touch_init() in
 * session_enter(); MOUSE/KEYS/MEDIA below read and mutate it through
 * apad_kbm_touch_build_*() and read it directly for drawing (the latch/
 * drag-lock highlight state has no other home now that it moved out of this
 * file's own statics). */
static apad_kbm_touch s_touch;

static const ui_box kModeStrip = { 0.0f, 0.0f, UI_BOT_W, 24.0f };
static const ui_box kModeTab[MODE_COUNT] = {
    {   0.0f, 0.0f, 80.0f, 22.0f },
    {  80.0f, 0.0f, 80.0f, 22.0f },
    { 160.0f, 0.0f, 80.0f, 22.0f },
    { 240.0f, 0.0f, 80.0f, 22.0f }
};
static const char *const kModeLabel[MODE_COUNT] = { "PAD", "MOUSE", "KEYS", "MEDIA" };
/* 0 for PAD: it needs no INPUTCAPS feature bit, it is always available. */
static const uint32_t kModeFeature[MODE_COUNT] = {
    0u, APAD_KBM_FEATURE_MOUSE, APAD_KBM_FEATURE_KEYBOARD, APAD_KBM_FEATURE_MEDIA
};

/* §6.19: nothing may be sent before an INPUTCAPS is accepted, and after that
 * only the bits `features` sets. Same rule, asked here purely to decide what
 * to DRAW and BUILD -- see this file's header comment, point 1. */
static int mode_available(const app_ctx *ctx, int mode)
{
    if (mode == MODE_PAD) {
        return 1;
    }
    return ctx->stats.inputcaps_serial != 0u
        && (ctx->stats.inputcaps.features & kModeFeature[mode]) != 0u;
}

static void draw_mode_strip(const app_ctx *ctx)
{
    int i;

    ui_rect(kModeStrip.x, kModeStrip.y, kModeStrip.w, kModeStrip.h, ui_c_panel());
    ui_rect(0.0f, kModeStrip.h - 1.0f, UI_BOT_W, 1.0f, ui_c_border());

    for (i = 0; i < MODE_COUNT; i++) {
        const ui_box *b = &kModeTab[i];
        int available = mode_available(ctx, i);
        int active = (s_mode == i);
        uint32_t fill, ink;

        if (!available) {
            fill = ui_c_bg();
            ink  = ui_c_border();
        } else if (active) {
            fill = ui_c_panel_hi();
            ink  = ui_c_accent();
        } else {
            fill = ui_c_panel();
            ink  = ui_c_dim();
        }
        ui_rect(b->x, b->y, b->w, b->h, fill);
        if (active && available) {
            ui_outline(b->x, b->y, b->w, b->h, 1.0f, ui_c_accent());
        }
        if (available) {
            ui_textf(b->x + b->w * 0.5f, b->y + 4.0f, UI_S_SMALL, ink,
                     UI_ALIGN_CENTER, "%s", kModeLabel[i]);
        } else {
            /* Drawn dim, not hidden, and not hit-testable (the tab-switch
             * check in session_update() calls mode_available() too) -- same
             * "this console has no ZL" treatment ui_widgets.c's draw_lamps()
             * gives an unusable lamp. The "why" line, per this task's brief:
             * squeezed to two UI_S_TINY-ish lines inside the same 80x22 box,
             * because there is nowhere else on a resistive 320px screen for
             * per-tab prose to live. */
            ui_textf_fit(b->x + b->w * 0.5f, b->y + 1.0f, 0.34f, ink,
                        UI_ALIGN_CENTER, b->w - 4.0f, "%s", kModeLabel[i]);
            ui_textf_fit(b->x + b->w * 0.5f, b->y + 11.0f, 0.34f, ink,
                        UI_ALIGN_CENTER, b->w - 4.0f, "%s",
                        (ctx->stats.inputcaps_serial == 0u)
                            ? "no caps yet" : "not offered");
        }
    }
}

/* ------------------------------------------------------------------------ */
/* MOUSE mode                                                               */
/* ------------------------------------------------------------------------ *
 * §6.16. Bottom screen y 24..190 is a relative touchpad (drag -> dx/dy,
 * short-tap-with-little-movement -> a left click); a 50px gutter on the
 * right (x 270..320) is the wheel; y 190..216 is a LEFT/MIDDLE/RIGHT footer
 * for touch users. Physical L/R double as left/right click on top of that
 * (session_update() clears the pad-wire bits back out).
 *
 * THE STATE MACHINE -- settle/anchor/tap-vs-drag/drag-lock/footer-latch, the
 * resistive-panel hardware finding behind it (found on real 3DS hardware,
 * 2026-08-25), and the MOUSE_* tuning constants -- moved (2026-09-09) to
 * clients/common/apad_kbm_touch.c (apad_kbm_touch_build_mouse), shared with
 * every other touch-driven client; see that file's own comment for the full
 * design record. This section keeps only what belongs to THIS screen: the
 * boxes used to DRAW the pad/wheel/footer (kMousePad etc. below --
 * numerically the same boxes apad_kbm_touch_init() computed into s_touch,
 * kept here as a separate float ui_box copy purely because ui_rect()/
 * ui_button() want that type) and the highlight/readout drawing itself.
 * Every HIT TEST below goes through apad_kbm_box_hit() against s_touch's
 * copy, never through these floats, so the drawn hit-box and the wire
 * builder's hit-box cannot silently drift apart.
 */
static const ui_box kMousePad   = {   0.0f,  24.0f, 270.0f, 166.0f };
static const ui_box kMouseWheel = { 270.0f,  24.0f,  50.0f, 166.0f };
static const ui_box kMouseBtnL  = {   0.0f, 190.0f, 106.0f,  26.0f };
static const ui_box kMouseBtnM  = { 106.0f, 190.0f, 107.0f,  26.0f };
static const ui_box kMouseBtnR  = { 213.0f, 190.0f, 107.0f,  26.0f };

/* A tiny 5px-wide chevron built from four 1px rows, `up` picking the
 * direction -- the wheel gutter's scroll hint (copy declutter pass, item 2)
 * and the PAD mode's "this side squeezes" hint (item 3) are both glyphs, not
 * text, and this is the one shape both need. ui_rect() only, no new asset. */
static void draw_chevron(float cx, float top_y, int up, uint32_t colour)
{
    int row;

    for (row = 0; row < 4; row++) {
        float half = (float)(up ? row : 3 - row) + 1.0f;

        ui_rect(cx - half, top_y + (float)row, half * 2.0f, 1.0f, colour);
    }
}

/* The mouse in the middle of the trackpad: clients/common/apad_glyph_mouse.h,
 * drawn as FAINT LINEWORK -- outline, button seam and wheel in the border
 * tone the crosshair and grid lines use, no fill, no highlight, no drop
 * shadow. A background cue, not a foreground icon. One screen pixel per cell,
 * runs of adjacent drawn cells coalesced into single rects. */
static void draw_mouse_glyph(float fcx, float fcy)
{
    int x0 = (int)fcx - APAD_GLYPH_MOUSE_W / 2;
    int y0 = (int)fcy - APAD_GLYPH_MOUSE_H / 2;
    uint32_t line = ui_c_border();
    int x, y;

    for (y = 0; y < APAD_GLYPH_MOUSE_H; y++) {
        const char *row = apad_glyph_mouse[y];
        for (x = 0; x < APAD_GLYPH_MOUSE_W; ) {
            int run;

            /* Only the linework marks are drawn; body/highlight/shadow cells
             * are left transparent so the panel shows through. */
            if (row[x] != 'o' && row[x] != 'g' && row[x] != 'w') {
                x++;
                continue;
            }
            run = 1;
            while (x + run < APAD_GLYPH_MOUSE_W
                   && (row[x + run] == 'o' || row[x + run] == 'g'
                       || row[x + run] == 'w')) {
                run++;
            }
            ui_rect((float)(x0 + x), (float)(y0 + y), (float)run, 1.0f, line);
            x += run;
        }
    }
}

static void session_draw_mouse(app_ctx *ctx)
{
    int touching = ctx->touch_held;
    int px = (int)ctx->touch.px, py = (int)ctx->touch.py;
    int in_pad   = touching && apad_kbm_box_hit(&s_touch.mouse_pad, px, py);
    int in_wheel = touching && apad_kbm_box_hit(&s_touch.mouse_wheel, px, py);
    /* Reflects the SAME state apad_kbm_touch_build_mouse() actually put on
     * the wire, not a fresh spatial hit-test -- so a footer LEFT press that
     * has since slid up into the pad (the click-and-drag fix, see
     * apad_kbm_touch.c) still shows LEFT held here instead of going dark
     * the moment the finger leaves the footer's rectangle. */
    int l_down   = (ctx->keys_held & KEY_L) != 0u
                 || (s_touch.mouse_btn_latch & APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT)) != 0u
                 || s_touch.mouse_tapdrag_active;
    int r_down   = (ctx->keys_held & KEY_R) != 0u
                 || (s_touch.mouse_btn_latch & APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_RIGHT)) != 0u;
    int m_down   = (s_touch.mouse_btn_latch & APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_MIDDLE)) != 0u;

    ui_rect(kMousePad.x, kMousePad.y, kMousePad.w, kMousePad.h,
            in_pad ? ui_c_panel_hi() : ui_c_panel());
    ui_outline(kMousePad.x, kMousePad.y, kMousePad.w, kMousePad.h, 1.0f,
              ui_c_border());
    if (!touching) {
        /* Glyph only while the pad is idle -- once a finger is down the live
         * dot below is the feedback, and drawing both would be clutter, not
         * an affordance. */
        draw_mouse_glyph(kMousePad.x + kMousePad.w * 0.5f,
                     kMousePad.y + kMousePad.h * 0.5f);
    }
    if (touching && (in_pad || s_touch.mouse_dragging)) {
        C2D_DrawCircleSolid((float)px, (float)py, 0.0f, 5.0f, ui_c_good());
    }

    ui_rect(kMouseWheel.x, kMouseWheel.y, kMouseWheel.w, kMouseWheel.h,
            in_wheel ? ui_c_panel_hi() : ui_c_panel());
    ui_outline(kMouseWheel.x, kMouseWheel.y, kMouseWheel.w, kMouseWheel.h,
              1.0f, ui_c_border());
    draw_chevron(kMouseWheel.x + kMouseWheel.w * 0.5f, kMouseWheel.y + 10.0f,
                1, ui_c_dim());
    draw_chevron(kMouseWheel.x + kMouseWheel.w * 0.5f,
                kMouseWheel.y + kMouseWheel.h - 14.0f, 0, ui_c_dim());

    ui_button(&kMouseBtnL, "LEFT",   l_down, 0);
    ui_button(&kMouseBtnM, "MIDDLE", m_down, 0);
    ui_button(&kMouseBtnR, "RIGHT",  r_down, 0);
}

/* ------------------------------------------------------------------------ */
/* KEYS mode                                                                */
/* ------------------------------------------------------------------------ *
 * §6.15. A APAD_KBM_KEY_COLS x APAD_KBM_KEY_ROWS grid. Hit-testing, the grid
 * contents (kKeyGrid) and the sticky-SHIFT/CTRL latch mechanism moved
 * (2026-09-09) to clients/common/apad_kbm_touch.c
 * (apad_kbm_touch_build_keyboard / _key_cell / _key_def) -- see this file's
 * header comment for the sticky-latch design and why physical L/R are live
 * modifiers on top of it, and apad_kbm_touch.c's own comment for the full
 * mechanism. This section keeps only the DRAWING geometry (still float,
 * still 320x240-only -- this screen's own layout, no scaling needed since
 * the 3DS bottom screen IS the 320x240 reference space apad_kbm_touch.h
 * scales from) and the grid highlight/label drawing, which reads
 * s_touch.key_shift_armed/key_ctrl_armed for the latch highlight and
 * apad_kbm_touch_key_cell()/apad_kbm_touch_key_def() for hit-testing and
 * cell content, so this drawing and the wire builder cannot disagree.
 */
#define KEY_GRID_TOP  24.0f
#define KEY_GRID_BOT 216.0f
#define KEY_CELL_W  (UI_BOT_W / (float)APAD_KBM_KEY_COLS)
#define KEY_CELL_H  ((KEY_GRID_BOT - KEY_GRID_TOP) / (float)APAD_KBM_KEY_ROWS)

static void session_draw_keys(app_ctx *ctx)
{
    int touch_cell = ctx->touch_held
                    ? apad_kbm_touch_key_cell(&s_touch, (int)ctx->touch.px,
                                              (int)ctx->touch.py) : -1;
    int row, col;

    for (row = 0; row < APAD_KBM_KEY_ROWS; row++) {
        for (col = 0; col < APAD_KBM_KEY_COLS; col++) {
            const apad_kbm_key_cell *def = apad_kbm_touch_key_def(row, col);
            int   cell = row * APAD_KBM_KEY_COLS + col;
            float x = (float)col * KEY_CELL_W;
            float y = KEY_GRID_TOP + (float)row * KEY_CELL_H;
            float w = KEY_CELL_W - 1.0f;
            int   is_touch, armed;
            uint32_t fill, ink, border;

            /* The four SPACE cells (row 4, col 2..5) merge visually into one
             * wide bar -- same usage in every one of them, so hit-testing is
             * still the plain per-cell arithmetic above; only the DRAWING
             * merges. */
            if (row == 4 && col > 2 && col < 6
                && def->usage == apad_kbm_touch_key_def(4, 2)->usage) {
                continue;
            }
            if (row == 4 && col == 2) {
                w = KEY_CELL_W * 4.0f - 1.0f;
            }

            is_touch = (cell == touch_cell);
            armed = (def->kind == APAD_KBM_KC_SHIFT && s_touch.key_shift_armed)
                 || (def->kind == APAD_KBM_KC_CTRL  && s_touch.key_ctrl_armed);

            if (is_touch) {
                fill = ui_c_accent(); ink = ui_c_bg(); border = ui_c_accent();
            } else if (armed) {
                fill = ui_c_panel_hi(); ink = ui_c_accent(); border = ui_c_accent();
            } else {
                fill = ui_c_panel(); ink = ui_c_text(); border = ui_c_border();
            }
            ui_rect(x, y, w, KEY_CELL_H - 1.0f, fill);
            ui_outline(x, y, w, KEY_CELL_H - 1.0f, 1.0f, border);
            ui_textf_fit(x + w * 0.5f, y + (KEY_CELL_H - UI_LINE(UI_S_SMALL)) * 0.5f,
                        UI_S_SMALL, ink, UI_ALIGN_CENTER, w - 4.0f, "%s",
                        def->label);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* MEDIA mode                                                               */
/* ------------------------------------------------------------------------ *
 * §6.17/§6.18 for transport and volume; the D-pad+OK navigation cross is
 * §6.15 KEYBOARD arrow keys/Enter instead (the vocabulary a real media-centre
 * remote's nav cross drives, and something this session may not have --
 * gated on FEATURE_KEYBOARD independently of the FEATURE_MEDIA gate the
 * whole tab is already under, same "drawn dim, not hidden" as an unavailable
 * mode tab).
 *
 * The builder (apad_kbm_touch_build_media, clients/common/apad_kbm_touch.c)
 * moved 2026-09-09 along with MOUSE and KEYS -- see its own comment for the
 * §6.17 held+ring rationale. This section keeps only the DRAWING boxes
 * (kMediaPrev etc. below -- numerically the same as the scaled boxes in
 * s_touch, kept here as float purely for ui_button()) and the button
 * highlight/label drawing, whose hit tests go through apad_kbm_box_hit()
 * against s_touch so the drawn highlight and the wire builder cannot
 * disagree.
 */
static const ui_box kMediaPrev  = {   4.0f,  28.0f,  96.0f,  36.0f };
static const ui_box kMediaStop  = { 112.0f,  28.0f,  96.0f,  36.0f };
static const ui_box kMediaNext  = { 220.0f,  28.0f,  96.0f,  36.0f };
static const ui_box kMediaPlay  = {   4.0f,  68.0f, 312.0f,  44.0f };
static const ui_box kMediaVolD  = {   4.0f, 116.0f,  96.0f,  36.0f };
static const ui_box kMediaMute  = { 112.0f, 116.0f,  96.0f,  36.0f };
static const ui_box kMediaVolU  = { 220.0f, 116.0f,  96.0f,  36.0f };
static const ui_box kMediaUp    = { 132.0f, 156.0f,  56.0f,  20.0f };
static const ui_box kMediaLeft  = {  76.0f, 176.0f,  56.0f,  24.0f };
static const ui_box kMediaOk    = { 132.0f, 176.0f,  56.0f,  24.0f };
static const ui_box kMediaRight = { 188.0f, 176.0f,  56.0f,  24.0f };
static const ui_box kMediaDown  = { 132.0f, 200.0f,  56.0f,  20.0f };

static void session_draw_media(app_ctx *ctx)
{
    int have_kb = ctx->stats.inputcaps_serial != 0u
               && (ctx->stats.inputcaps.features & APAD_KBM_FEATURE_KEYBOARD) != 0u;
    int touching = ctx->touch_held;
    int px = (int)ctx->touch.px, py = (int)ctx->touch.py;

    ui_button(&kMediaPrev, "PREV", touching && apad_kbm_box_hit(&s_touch.media_prev, px, py), 0);
    ui_button(&kMediaStop, "STOP", touching && apad_kbm_box_hit(&s_touch.media_stop, px, py), 0);
    ui_button(&kMediaNext, "NEXT", touching && apad_kbm_box_hit(&s_touch.media_next, px, py), 0);
    ui_button(&kMediaPlay, "PLAY / PAUSE",
             touching && apad_kbm_box_hit(&s_touch.media_play, px, py), 1);
    ui_button(&kMediaVolD, "VOL -", touching && apad_kbm_box_hit(&s_touch.media_vol_d, px, py), 0);
    ui_button(&kMediaMute, "MUTE",  touching && apad_kbm_box_hit(&s_touch.media_mute, px, py), 0);
    ui_button(&kMediaVolU, "VOL +", touching && apad_kbm_box_hit(&s_touch.media_vol_u, px, py), 0);

    if (!have_kb) {
        /* Drawn dim, not hidden, one line saying why -- same treatment as an
         * unavailable mode tab (draw_mode_strip above), applied to this
         * sub-feature of an otherwise-available MEDIA tab. */
        ui_outline(kMediaLeft.x, kMediaUp.y, kMediaRight.x + kMediaRight.w - kMediaLeft.x,
                  kMediaDown.y + kMediaDown.h - kMediaUp.y, 1.0f, ui_c_border());
        ui_textf_fit(UI_BOT_W * 0.5f, kMediaOk.y + 4.0f, UI_S_TINY, ui_c_border(),
                    UI_ALIGN_CENTER, 300.0f,
                    "nav cross needs keyboard capability (not offered)");
        return;
    }
    ui_button(&kMediaUp,    "^",  touching && apad_kbm_box_hit(&s_touch.media_up, px, py),    0);
    ui_button(&kMediaLeft,  "<",  touching && apad_kbm_box_hit(&s_touch.media_left, px, py),  0);
    ui_button(&kMediaOk,    "OK", touching && apad_kbm_box_hit(&s_touch.media_ok, px, py),    1);
    ui_button(&kMediaRight, ">",  touching && apad_kbm_box_hit(&s_touch.media_right, px, py), 0);
    ui_button(&kMediaDown,  "v",  touching && apad_kbm_box_hit(&s_touch.media_down, px, py),  0);
}

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

    /* A NEW session: every kbm mode starts clean. apad_kbm_touch_init()
     * recomputes the (unchanged, since the bottom screen's size never
     * changes) scaled boxes AND zeroes every builder's runtime state in one
     * call -- mouse accumulators reset to 0 is harmless (§6.16's first
     * accepted packet establishes the baseline regardless of the starting
     * value; this is just hygiene, not a protocol requirement). */
    s_mode = MODE_PAD;
    apad_kbm_touch_init(&s_touch, (int)UI_BOT_W, (int)UI_BOT_H);
}

/* Leaves the session cleanly and hands the connect screen a reason plus a
 * countdown. Every exit from this screen goes through here so that none of
 * them can forget the countdown -- the whole point of the return path. */
static apad_screen_id leave_to_connect(app_ctx *ctx, int level,
                                       const char *reason, int voluntary)
{
    ctx->connected = 0;
    /* The countdown exists for a session that DROPPED: the server is still
     * there and reconnecting is what the user wants. A session the user
     * ENDED by pressing DISCONNECT is the opposite case -- counting down to
     * reconnect them to the server they just left is the one thing they
     * asked not to happen. So: involuntary exits arm the countdown, a
     * voluntary one lands on the connect screen idle, address still filled,
     * CONNECT one tap away (user feedback 2026-09-10, applied on every
     * client). */
    ctx->reconnect_frames = voluntary ? 0 : RECONNECT_FRAMES;
    if (voluntary) {
        ctx->want_discovery = 0;   /* no auto-discovery either: they chose to stop */
    }
    app_note(ctx, level, "%s", reason);
    return APAD_SCREEN_CONNECT;
}

static apad_screen_id session_update(app_ctx *ctx)
{
    int state;
    apad_client_kbm_in kbm;
    apad_kbm_touch_input kin;
    uint32_t feats;
    int have_kb, have_mo, have_me;

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
            return leave_to_connect(ctx, 0, "disconnected (touch)", 1);
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
    } else if (ctx->touch_pressed) {
        /* Mode tab switch. A tap (touch_pressed), not a drag/hold, matching
         * DISCONNECT/SELF-TEST above -- and only onto a tab mode_available()
         * says is real; an unavailable tab is not hit-testable at all. */
        int i;

        for (i = 0; i < MODE_COUNT; i++) {
            if (mode_available(ctx, i)
                && ui_box_hit(&kModeTab[i], (int)ctx->touch.px,
                             (int)ctx->touch.py)) {
                s_mode = i;
                break;
            }
        }
    }

    /* Safety net: if INPUTCAPS just changed (or has not arrived yet) and the
     * currently-displayed mode is no longer available, fall back to PAD
     * rather than keep drawing/hit-testing a tab the gate would silently
     * drop everything from. */
    if (s_mode != MODE_PAD && !mode_available(ctx, s_mode)) {
        s_mode = MODE_PAD;
    }

    /* THE TOUCH EXCLUSION, extended to the mode strip and (whenever a kbm
     * mode owns the screen) the whole bottom screen -- "one set of numbers,"
     * the same box(es) drawn/hit-tested above, so the readout and the wire
     * cannot disagree. kButtonStrip and kModeStrip are UI in every mode;
     * everything else is UI too whenever s_mode != MODE_PAD, because MOUSE /
     * KEYS / MEDIA have repurposed that whole area for their own surface and
     * a stray contact there must not ALSO arrive as a §5 pad touch (and,
     * through the server's own profile, a phantom LT/RT squeeze) on top of
     * whatever §6.16/§6.15/§6.17 packets this frame's kbm builders produce
     * from the exact same coordinates. */
    if (ctx->touch_held
        && (s_mode != MODE_PAD
            || ui_box_hit(&kModeStrip, (int)ctx->touch.px, (int)ctx->touch.py)
            || ui_box_hit(&kButtonStrip, (int)ctx->touch.px, (int)ctx->touch.py))) {
        ctx->st.buttons &= ~(uint32_t)APAD_BTN_TOUCH_PRESS;
        ctx->st.touch_count = 0;
    }

    /* L/R double duty (this file's header comment): MOUSE mode reads them as
     * click, KEYS mode as live Shift/Ctrl, both via ctx->keys_held inside
     * the kbm builders below, independently of this clear. Cleared back out
     * of the PAD wire state for the same reason kButtonStrip's touch is --
     * pressing Shift must not also squeeze a phantom shoulder button. */
    if (s_mode == MODE_MOUSE || s_mode == MODE_KEYS) {
        ctx->st.buttons &= ~(uint32_t)(APAD_BTN_L | APAD_BTN_R);
    }

    /* -- §6.15-§6.19: build this frame's keyboard/mouse/media snapshot.
     * See this file's header comment for why every builder runs every
     * frame regardless of s_mode (an inactive one is fed synthetic "nothing
     * touched" input, which safely releases whatever it was holding) and why
     * `have` mirrors ctx->stats.inputcaps.features exactly -- the actual gate
     * lives in the engine (kbm_gate(), clients/common/apad_client.c), this is
     * only "don't bother building work nobody will send". */
    feats   = (ctx->stats.inputcaps_serial != 0u) ? ctx->stats.inputcaps.features : 0u;
    have_kb = (feats & APAD_KBM_FEATURE_KEYBOARD) != 0u;
    have_mo = (feats & APAD_KBM_FEATURE_MOUSE)    != 0u;
    have_me = (feats & APAD_KBM_FEATURE_MEDIA)    != 0u;

    memset(&kbm, 0, sizeof kbm);
    kbm.have = (uint8_t)((have_kb ? APAD_KBM_FEATURE_KEYBOARD : 0u)
                        | (have_mo ? APAD_KBM_FEATURE_MOUSE    : 0u)
                        | (have_me ? APAD_KBM_FEATURE_MEDIA    : 0u));

    /* One apad_kbm_touch_input, shared by all three builders below --
     * clients/common/apad_kbm_touch.h's own contract: `l_held`/`r_held` are
     * the RAW hardware sample (ctx->keys_held), independent of the
     * ctx->st.buttons clearing just above. */
    kin.touching = ctx->touch_held;
    kin.px       = (int)ctx->touch.px;
    kin.py       = (int)ctx->touch.py;
    kin.l_held   = (ctx->keys_held & KEY_L) != 0u;
    kin.r_held   = (ctx->keys_held & KEY_R) != 0u;

    /* KEYS runs first so MEDIA's D-pad (which also targets kbm.keyboard) ORs
     * into the same struct rather than clobbering it. */
    apad_kbm_touch_build_keyboard(&s_touch, &kin, &kbm.keyboard,
                                  have_kb && s_mode == MODE_KEYS);
    if (have_mo) {
        apad_kbm_touch_build_mouse(&s_touch, &kin, &kbm.mouse,
                                   s_mode == MODE_MOUSE);
    }
    if (have_me) {
        apad_kbm_touch_build_media(&s_touch, &kin, &kbm.media, &kbm.keyboard,
                                   s_mode == MODE_MEDIA, have_kb);
    }

    /* -- one turn of the session. ctx->st was filled by main.c from the same
     * hardware scan this frame's on-screen readout draws from, so the display
     * and the wire cannot disagree (touch/L/R inside a kbm mode excepted,
     * just above). client_ticks_ms is deliberately not set here: the engine
     * stamps it when the datagram is actually built, which is what S6.5
     * wants and is closer to the wire than a value taken at the top of a
     * frame that may then wait for a datagram. */
    state = apad_client_pump_ex(ctx->client, &ctx->st, &kbm, PUMP_WAIT_MS);
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
                                app_close_reason_text((int)ctx->stats.close_reason), 0);
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
    /* "disconnect / self-test: bottom screen buttons" removed (copy declutter
     * pass, item 1): the DISCONNECT and SELF-TEST buttons it described are
     * drawn every frame in kButtonStrip, on the very screen this hint would
     * point at. */
}

/* The PAD mode content: docs/PROTOCOL.md S6.2 / server/profiles/
 * 3ds-default.jsonc splits it in half, left -> LT and right -> RT, both
 * analog -- unless S6.12 TOUCHMAP told this client the server's ACTUAL
 * mapping, in which case that is drawn instead (session_draw_pad_from_touchmap
 * below). Either way this is a HINT for the fallback case, labelled as one on
 * screen: the client cannot know what profile the server actually loaded.
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
 * and the caller should fall back. Region rects arrive normalised 0..255,
 * +Y down -- this screen's own coordinate convention, so this is a scale
 * with no flip. */
static int session_draw_pad_from_touchmap(app_ctx *ctx)
{
    const apad_touchmap *tm = &ctx->stats.touchmap;
    const float top = 24.0f, bottom = 216.0f;
    const float usable_h = bottom - top;
    unsigned i;

    if (ctx->stats.touchmap_serial == 0u || tm->region_count == 0u) {
        return 0;
    }

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
            /* "analog: slide to squeeze" replaced with a dim downward
             * chevron -- the shape says "push into this" without a
             * sentence (copy declutter pass, item 3). */
            draw_chevron(x0 + w * 0.5f, y0 + h * 0.5f + 22.0f, 0, ui_c_dim());
        }
    }

    if (ctx->st.touch_count > 0) {
        C2D_DrawCircleSolid((float)ctx->touch.px, (float)ctx->touch.py, 0.0f,
                            6.0f, ui_c_good());
    }
    return 1;
}

static void session_draw_pad(app_ctx *ctx)
{
    const float split = UI_BOT_W * 0.5f;
    const float top = 24.0f, bottom = 216.0f;
    int touch_left  = (ctx->st.touch_count > 0) && (ctx->touch.px < (u16)split);
    int touch_right = (ctx->st.touch_count > 0) && (ctx->touch.px >= (u16)split);

    /* Server-supplied layout when there is one, the compiled-in guess when
     * there is not. */
    if (session_draw_pad_from_touchmap(ctx)) {
        return;
    }

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
    /* "analog: slide down to squeeze" x2 replaced with a dim downward
     * chevron on each half (copy declutter pass, item 3) -- both LT and RT
     * are analog in the compiled-in fallback split. */
    draw_chevron(split * 0.5f, 130.0f, 0, ui_c_dim());
    draw_chevron(split * 1.5f, 130.0f, 0, ui_c_dim());

    /* Live contact point: the one part of this screen that is a measurement
     * rather than a hint. */
    if (ctx->st.touch_count > 0) {
        C2D_DrawCircleSolid((float)ctx->touch.px, (float)ctx->touch.py, 0.0f,
                            6.0f, ui_c_good());
    }
    /* "shipped DEFAULT profile only -- the server may have an edited one"
     * removed (copy declutter pass, item 6): explanatory prose, not an
     * action/error/permission, and this screen's header comment already
     * records the caveat for anyone reading the source. */
}

static void session_draw_bottom(app_ctx *ctx)
{
    /* Shared header band: the mode tab strip, every mode, y 0..24 -- this
     * REPLACES the per-mode "touch mapping" header PAD mode used to draw
     * itself in that same band, so nothing shrinks (this file's header
     * comment). */
    draw_mode_strip(ctx);

    switch (s_mode) {
    case MODE_MOUSE: session_draw_mouse(ctx); break;
    case MODE_KEYS:  session_draw_keys(ctx);  break;
    case MODE_MEDIA: session_draw_media(ctx); break;
    case MODE_PAD:
    default:         session_draw_pad(ctx);   break;
    }

    /* Shared footer, reached by EVERY mode.
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

        /* The three explanatory lines under each question (why a BYE goes
         * out, that the connect screen counts down, that the self-test
         * blocks) are removed -- copy declutter pass, item 5. The question
         * plus the two buttons is the whole prompt now; kConfirmBox/
         * kConfirmRun/kConfirmCancel are untouched (item 9: hit boxes do not
         * move), so the extra vertical space is simply left empty rather
         * than re-laid-out. */
        if (is_disconnect) {
            ui_textf_fit(kConfirmBox.x + kConfirmBox.w * 0.5f,
                         kConfirmBox.y + 8.0f, UI_S_BODY, ui_c_warn(),
                         UI_ALIGN_CENTER, cw, "Disconnect now?");
            ui_button(&kConfirmRun, "DISCONNECT (A)", 0, 1);
        } else {
            ui_textf_fit(kConfirmBox.x + kConfirmBox.w * 0.5f,
                         kConfirmBox.y + 8.0f, UI_S_BODY, ui_c_warn(),
                         UI_ALIGN_CENTER, cw, "This drops the session");
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
