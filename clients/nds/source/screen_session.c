/* clients/nds/source/screen_session.c
 *
 * clients/3ds/source/screen_session.c, ported. The live session screen:
 * everything protocol-shaped is one call -- apad_client_pump_ex() from
 * clients/common/apad_client.c -- and this file only decides what to draw and
 * which screen to go to next.
 *
 * The bottom screen's header band is a 4-way tab strip -- PAD / MOUSE / KEYS
 * / MEDIA -- switching what the rest of the bottom screen shows. DISCONNECT
 * and SELF-TEST are touch buttons in the footer, each behind a confirmation:
 *
 *   - DISCONNECT, because a stray brush against the bottom screen during play
 *     is far likelier than an accidental START press, and there is no
 *     un-disconnecting a live session.
 *   - SELF-TEST, because running it DOES drop the session: apad_selftest_run()
 *     is one blocking call with no pump inside it, the server stops hearing
 *     INPUT_STATE, and it reaps the session per docs/PROTOCOL.md S8 --
 *     correctly. On THIS console that call is about fifty-one seconds long
 *     rather than the 3DS's fraction of one, so the warning is not a
 *     formality. Answering yes sends a proper BYE first, which frees the pad
 *     slot immediately instead of leaving the server to time it out.
 *
 * The confirmation is drawn as an overlay while the pump KEEPS RUNNING
 * underneath, so reading the warning cannot itself cost the session (S11's
 * idle timeout is 3000ms).
 *
 * WHO OWNS WHAT: this file NEVER encodes a KEYBOARD/MOUSE/MEDIA packet,
 * computes an event ring ordinal, or decides S6.19's gate --
 * clients/common/apad_client.c does every one of those. It does not run the
 * touch state machines either: the resistive-panel mouse settle/anchor/tap/
 * drag-lock machinery and the sticky SHIFT/CTRL latch live in
 * clients/common/apad_kbm_touch.c, tuned against real 3DS hardware, and THIS
 * CLIENT LINKING THAT SAME FILE IS THE ENTIRE REASON IT WAS HOISTED THERE.
 * This screen's only job is building one apad_kbm_touch_input per frame from
 * ctx->touch/ctx->keys_held, handing it to the three builders, and handing
 * the result to apad_client_pump_ex().
 *
 * DIFFERENCES FROM THE 3DS FILE:
 *
 *   - apad_kbm_touch_init() is given 256x192, not 320x240, and is fed RAW DS
 *     touch pixels. It scales its own 320x240 reference boxes by 4/5; ui.c
 *     scales this file's drawing boxes by 4/5; the two therefore agree
 *     exactly, which is why every drawing box below is still the 3DS's own
 *     number.
 *   - C2D_DrawCircleSolid() becomes ui_dot(), which takes 3DS coordinates --
 *     so the raw touch point has to be converted on the way in (px * 5 / 4).
 *     Every place that compares a raw touch pixel against a 320-space layout
 *     number does the same conversion; getting that wrong would put the
 *     LT/RT split at 200px instead of 160.
 *   - the gyro line is gone (no gyroscope).
 *   - config_3ds.c becomes config_nds.c, and the saved record gains the
 *     network (SSID, key, security) -- written by screen_wifi.c, not here.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apad_kbm_touch.h"
#include "apad_glyph_mouse.h"
#include "config_nds.h"

/* ZERO, and that is the whole DS delta on this line. The 3DS passes 8 (half a
 * frame) so that at a server-negotiated 30 Hz the pump still has time to
 * draw; the ceiling is on IDLE waiting, not on the send cadence, which the
 * engine drives from the clock either way.
 *
 * Here it MUST be 0. A blocking receive on DSWiFi's lwIP never returns when
 * nothing arrives -- select()'s timeout does not fire, measured in melonDS
 * on 2026-09-09 (main.c's discovery section has the isolation). During a
 * healthy session that is invisible, because an ACK is always already queued
 * by the time the next pump asks; the moment the server stops answering, an
 * 8 ms wait becomes a permanent freeze instead of the S11 idle timeout this
 * screen is supposed to notice. With 0 the drain is non-blocking, the frame
 * loop's own VBlank is what paces the session, and a server that goes away
 * ends the session in 3000 ms exactly as S11 says it should.
 *
 * Nothing is lost by it: this client already runs one pump per rendered
 * frame, so a 60 Hz session sends on exactly the same schedule either way. */
#define PUMP_WAIT_MS 0

#define RECONNECT_FRAMES 300   /* ~5s */

enum { SS_LIVE = 0, SS_CONFIRM_SELFTEST, SS_CONFIRM_DISCONNECT };
static int s_sub;

static int s_flash_disconnect;
static int s_flash_selftest;

/* S6.10/S6.11 STATUS and ERROR both surface here. The human-readable text is
 * shown for STATUS_MSG_TTL_FRAMES after it changes and then drops off; the
 * numeric code is diagnostics-overlay only. */
static uint32_t s_last_status_serial;
static int32_t  s_last_error_code;
static int      s_msg_is_error;
static int      s_msg_fresh_frames;
#define STATUS_MSG_TTL_FRAMES 300

static const ui_box kConfirmBox    = {  24.0f,  52.0f, 272.0f, 132.0f };
static const ui_box kConfirmRun    = {  40.0f, 146.0f, 110.0f,  30.0f };
static const ui_box kConfirmCancel = { 170.0f, 146.0f, 110.0f,  30.0f };

/* THE RESERVED RECTANGLE. The touch-mapping panel above ends at y=216;
 * everything from there to the screen edge is this screen's own UI, not part
 * of the touch surface the server maps. A contact anywhere in kButtonStrip is
 * UI-only and must never reach the wire as a touch contact. One set of
 * numbers, not two copies that can drift. */
static const ui_box kButtonStrip   = {   0.0f, 216.0f, UI_BOT_W, UI_BOT_H - 216.0f };
static const ui_box kDisconnectBtn = {   4.0f, 217.0f, 152.0f,  22.0f };
static const ui_box kSelftestBtn   = { 164.0f, 217.0f, 152.0f,  22.0f };

/* ------------------------------------------------------------------------ */
/* mode strip (PAD / MOUSE / KEYS / MEDIA)                                  */
/* ------------------------------------------------------------------------ */

enum { MODE_PAD = 0, MODE_MOUSE, MODE_KEYS, MODE_MEDIA, MODE_COUNT };
static int s_mode;

/* The shared touch/mouse/keyboard/media builder state
 * (clients/common/apad_kbm_touch.h) for this screen's single live session. */
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

/* Raw DS touch pixel -> the 320x240 space every layout constant in this file
 * is written in. ui_box_hit() does this internally; these two are for the
 * places that compare against a bare number or hand a point to ui_dot(). */
static int tx3(const app_ctx *ctx) { return ((int)ctx->touch.px * 5) / 4; }
static int ty3(const app_ctx *ctx) { return ((int)ctx->touch.py * 5) / 4; }

/* S6.19: nothing may be sent before an INPUTCAPS is accepted, and after that
 * only the bits `features` sets. Asked here purely to decide what to DRAW and
 * BUILD -- the actual gate lives in the engine. */
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
            ui_textf_fit(b->x + b->w * 0.5f, b->y + 4.0f, UI_S_SMALL, ink,
                         UI_ALIGN_CENTER, b->w - 4.0f, "%s", kModeLabel[i]);
        } else {
            /* Drawn dim, not hidden, and not hit-testable -- the same "this
             * console has no ZL" treatment ui_widgets.c gives an unusable
             * lamp. The "why" line is squeezed to two lines inside the same
             * box, because there is nowhere else on a 256px screen for
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
 * S6.16. Bottom screen y 24..190 is a relative touchpad (drag -> dx/dy,
 * short-tap-with-little-movement -> a left click); a 50px gutter on the right
 * is the wheel; y 190..216 is a LEFT/MIDDLE/RIGHT footer. Physical L/R double
 * as left/right click on top of that.
 *
 * The state machine and its MOUSE_* tuning constants live in
 * clients/common/apad_kbm_touch.c. This section keeps only what belongs to
 * THIS screen: the boxes used to DRAW (numerically the same boxes
 * apad_kbm_touch_init() computed into s_touch, kept here as float ui_box
 * copies purely because ui_rect()/ui_button() want that type) and the
 * highlight drawing. Every HIT TEST goes through apad_kbm_box_hit() against
 * s_touch's copy, so the drawn hit-box and the wire builder's cannot drift.
 */
static const ui_box kMousePad   = {   0.0f,  24.0f, 270.0f, 166.0f };
static const ui_box kMouseWheel = { 270.0f,  24.0f,  50.0f, 166.0f };
static const ui_box kMouseBtnL  = {   0.0f, 190.0f, 106.0f,  26.0f };
static const ui_box kMouseBtnM  = { 106.0f, 190.0f, 107.0f,  26.0f };
static const ui_box kMouseBtnR  = { 213.0f, 190.0f, 107.0f,  26.0f };

/* The mouse in the middle of the trackpad: clients/common/apad_glyph_mouse.h,
 * drawn as FAINT LINEWORK -- outline, the two-button seam and the wheel, all
 * in the border tone the stick crosshair and the grid lines use, with no
 * fill, no highlight, no drop shadow. It is a background cue that says "this
 * is a trackpad", not a foreground icon; the body stays the box colour so it
 * recedes whether the pad is idle or being touched. Blitted 1:1 in DS pixels
 * so the outline stays a clean single-pixel line. */
#define MOUSE_GLYPH_BUF_W APAD_GLYPH_MOUSE_W
#define MOUSE_GLYPH_BUF_H APAD_GLYPH_MOUSE_H

static void draw_mouse_glyph(int cx, int cy, uint32_t under)
{
    static uint16_t buf[MOUSE_GLYPH_BUF_W * MOUSE_GLYPH_BUF_H];
    const uint16_t c_u    = ui_rgb15(under);
    const uint16_t c_line = ui_rgb15(ui_c_border());
    int x, y;

    for (y = 0; y < APAD_GLYPH_MOUSE_H; y++) {
        for (x = 0; x < APAD_GLYPH_MOUSE_W; x++) {
            char ch = apad_glyph_mouse[y][x];
            /* o = outline, g = button seam / wheel slot, w = roller: the only
             * marks drawn, all in one faint tone. Everything else (body,
             * highlight, shadow) stays the box colour underneath. */
            buf[y * MOUSE_GLYPH_BUF_W + x] =
                (ch == 'o' || ch == 'g' || ch == 'w') ? c_line : c_u;
        }
    }
    ui_blit_rgb15(cx - APAD_GLYPH_MOUSE_W / 2, cy - APAD_GLYPH_MOUSE_H / 2,
                  buf, MOUSE_GLYPH_BUF_W, MOUSE_GLYPH_BUF_H, MOUSE_GLYPH_BUF_W);
}

static void session_draw_mouse(app_ctx *ctx)
{
    int touching = ctx->touch_held;
    int px = (int)ctx->touch.px, py = (int)ctx->touch.py;
    int in_pad   = touching && apad_kbm_box_hit(&s_touch.mouse_pad, px, py);
    int in_wheel = touching && apad_kbm_box_hit(&s_touch.mouse_wheel, px, py);
    /* Reflects the SAME state apad_kbm_touch_build_mouse() actually put on
     * the wire, not a fresh spatial hit-test -- so a footer LEFT press that
     * has since slid up into the pad still shows LEFT held. */
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
    /* Centre of kMousePad (3DS 135,107) in DS pixels at this screen's 0.8
     * scale: (108, 85). Integers on purpose -- this runs every frame. */
    draw_mouse_glyph(108, 85, in_pad ? ui_c_panel_hi() : ui_c_panel());
    if (touching && (in_pad || s_touch.mouse_dragging)) {
        ui_dot((float)tx3(ctx), (float)ty3(ctx), 5.0f, ui_c_good());
    }

    ui_rect(kMouseWheel.x, kMouseWheel.y, kMouseWheel.w, kMouseWheel.h,
            in_wheel ? ui_c_panel_hi() : ui_c_panel());
    ui_outline(kMouseWheel.x, kMouseWheel.y, kMouseWheel.w, kMouseWheel.h,
              1.0f, ui_c_border());
    /* The up/down arrow pair IS the gutter's whole label now (2026-09-09
     * decluttering pass dropped the "scroll" word between them) -- these are
     * the same "^"/"v" glyphs the MEDIA nav cross and the touchmap arrows
     * already use for the same meaning elsewhere in this client, not prose. */
    ui_textf(kMouseWheel.x + kMouseWheel.w * 0.5f, kMouseWheel.y + 6.0f,
             UI_S_SMALL, ui_c_dim(), UI_ALIGN_CENTER, "^");
    ui_textf(kMouseWheel.x + kMouseWheel.w * 0.5f,
             kMouseWheel.y + kMouseWheel.h - 20.0f, UI_S_SMALL, ui_c_dim(),
             UI_ALIGN_CENTER, "v");

    ui_button(&kMouseBtnL, "LEFT",   l_down, 0);
    ui_button(&kMouseBtnM, "MIDDLE", m_down, 0);
    ui_button(&kMouseBtnR, "RIGHT",  r_down, 0);
}

/* ------------------------------------------------------------------------ */
/* KEYS mode                                                                */
/* ------------------------------------------------------------------------ *
 * S6.15. A APAD_KBM_KEY_COLS x APAD_KBM_KEY_ROWS grid. Hit-testing, the grid
 * contents and the sticky-SHIFT/CTRL latch live in
 * clients/common/apad_kbm_touch.c. This section keeps only the DRAWING
 * geometry -- still the 3DS's 320x240 numbers, which ui.c scales -- and reads
 * s_touch for the latch highlight, so this drawing and the wire builder
 * cannot disagree.
 *
 * TEN COLUMNS ON A 256-PIXEL SCREEN is 25 pixels a cell, against 32 on the
 * 3DS. A one-character label fits comfortably; the multi-character ones
 * (SHIFT, CTRL, ENTER, SPACE, BKSP) go through ui_textf_fit() and land on the
 * smallest face. Listed in the report as a legibility deviation -- the
 * alternative was a smaller GRID, and dropping keys off a keyboard is worse
 * than a tight label. */
#define KEY_GRID_TOP  24.0f
#define KEY_GRID_BOT 216.0f
#define KEY_CELL_W  (UI_BOT_W / (float)APAD_KBM_KEY_COLS)
#define KEY_CELL_H  ((KEY_GRID_BOT - KEY_GRID_TOP) / (float)APAD_KBM_KEY_ROWS)

/* The shared grid's long labels (SHIFT, CTRL, ENTER, BKSP) do not fit a
 * 25-pixel cell at the 8-pixel face -- ui_textf_fit() used to cut them to
 * "SHI"/"CT"/"EN"/"BK", which reads as damage. Three-letter forms people
 * already know are drawn INSTEAD, on this console only; the grid itself,
 * its usages and its hit boxes are the shared ones and are untouched.
 * SPACE spans four cells and fits as it is. */
static const char *key_label_ds(const char *label)
{
    static const struct { const char *from, *to; } kShort[] = {
        { "SHIFT", "SHF" }, { "CTRL", "CTL" }, { "ENTER", "ENT" },
        { "BKSP",  "BSP" }
    };
    size_t i;

    for (i = 0; i < sizeof kShort / sizeof kShort[0]; i++) {
        if (strcmp(label, kShort[i].from) == 0) {
            return kShort[i].to;
        }
    }
    return label;
}

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

            /* The four SPACE cells merge visually into one wide bar -- same
             * usage in every one, so hit-testing is still the plain per-cell
             * arithmetic; only the DRAWING merges. */
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
            ui_textf_fit(x + w * 0.5f,
                        y + (KEY_CELL_H - UI_LINE(UI_S_SMALL)) * 0.5f,
                        UI_S_SMALL, ink, UI_ALIGN_CENTER, w - 2.0f, "%s",
                        key_label_ds(def->label));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* MEDIA mode                                                               */
/* ------------------------------------------------------------------------ *
 * S6.17/S6.18 for transport and volume; the D-pad+OK navigation cross is
 * S6.15 KEYBOARD arrow keys/Enter instead, gated on FEATURE_KEYBOARD
 * independently of the FEATURE_MEDIA gate the whole tab is already under.
 * The builder lives in clients/common/apad_kbm_touch.c; this section keeps
 * only the DRAWING boxes and the highlight drawing, whose hit tests go
 * through apad_kbm_box_hit() against s_touch.
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
        ui_outline(kMediaLeft.x, kMediaUp.y,
                  kMediaRight.x + kMediaRight.w - kMediaLeft.x,
                  kMediaDown.y + kMediaDown.h - kMediaUp.y, 1.0f, ui_c_border());
        /* Shortened from "...capability (not offered)" -- the parenthetical
         * repeated what "needs keyboard capability" already said. */
        ui_textf_wrap(UI_BOT_W * 0.5f, kMediaOk.y + 2.0f, 300.0f, UI_S_TINY,
                     ui_c_border(), UI_ALIGN_CENTER, 0.0f,
                     "nav cross needs keyboard capability");
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
     * recomputes the scaled boxes AND zeroes every builder's runtime state in
     * one call.
     *
     * 256x192, NOT 320x240 -- this is the one number that has to be the
     * PHYSICAL screen rather than the layout space, because the builders
     * hit-test the RAW touch coordinates this client feeds them. Passing
     * 320x240 here would compile, draw identically, and put every mouse
     * gesture and key cell a quarter of a screen out. */
    s_mode = MODE_PAD;
    apad_kbm_touch_init(&s_touch, UI_DS_W, UI_DS_H);
}

/* Leaves the session cleanly and hands the connect screen a reason plus a
 * countdown. Every exit from this screen goes through here so none of them
 * can forget the countdown. */
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

    /* The hidden diagnostics toggle. Read before the confirm-overlay branch
     * so it works regardless of s_sub; it only ever touches ctx->show_diag,
     * never ctx->st.buttons, so a SELECT press still reaches the wire as an
     * ordinary button for whatever game is running. */
    if (app_pressed(ctx, KEY_SELECT)) {
        ctx->show_diag = !ctx->show_diag;
        /* The top screen's diag panel must not wait for the
         * every-other-frame skip's next turn -- see main.c's header comment
         * on app_top_force_redraw(). */
        app_top_force_redraw();
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
             * silent client: the pad slot comes back at once. */
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
     * displayed mode is no longer available, fall back to PAD. */
    if (s_mode != MODE_PAD && !mode_available(ctx, s_mode)) {
        s_mode = MODE_PAD;
    }

    /* THE TOUCH EXCLUSION. kButtonStrip and kModeStrip are UI in every mode;
     * everything else is UI too whenever s_mode != MODE_PAD, because MOUSE /
     * KEYS / MEDIA have repurposed that whole area and a stray contact there
     * must not ALSO arrive as a S5 pad touch (and, through the server's
     * profile, as a phantom stick deflection) on top of whatever S6.15-S6.17
     * packets this frame's builders produce from the same coordinates. */
    if (ctx->touch_held
        && (s_mode != MODE_PAD
            || ui_box_hit(&kModeStrip, (int)ctx->touch.px, (int)ctx->touch.py)
            || ui_box_hit(&kButtonStrip, (int)ctx->touch.px, (int)ctx->touch.py))) {
        ctx->st.buttons &= ~(uint32_t)APAD_BTN_TOUCH_PRESS;
        ctx->st.touch_count = 0;
    }

    /* L/R double duty: MOUSE mode reads them as click, KEYS mode as live
     * Shift/Ctrl, both via ctx->keys_held inside the builders below. Cleared
     * back out of the PAD wire state for the same reason kButtonStrip's touch
     * is -- pressing Shift must not also squeeze a phantom shoulder button. */
    if (s_mode == MODE_MOUSE || s_mode == MODE_KEYS) {
        ctx->st.buttons &= ~(uint32_t)(APAD_BTN_L | APAD_BTN_R);
    }

    /* -- S6.15-S6.19: build this frame's keyboard/mouse/media snapshot.
     * Every builder runs every frame regardless of s_mode (an inactive one is
     * fed synthetic "nothing touched" input, which safely releases whatever
     * it was holding); `have` mirrors ctx->stats.inputcaps.features only so
     * work nobody will send is skipped -- the actual gate lives in the
     * engine. */
    feats   = (ctx->stats.inputcaps_serial != 0u) ? ctx->stats.inputcaps.features : 0u;
    have_kb = (feats & APAD_KBM_FEATURE_KEYBOARD) != 0u;
    have_mo = (feats & APAD_KBM_FEATURE_MOUSE)    != 0u;
    have_me = (feats & APAD_KBM_FEATURE_MEDIA)    != 0u;

    memset(&kbm, 0, sizeof kbm);
    kbm.have = (uint8_t)((have_kb ? APAD_KBM_FEATURE_KEYBOARD : 0u)
                        | (have_mo ? APAD_KBM_FEATURE_MOUSE    : 0u)
                        | (have_me ? APAD_KBM_FEATURE_MEDIA    : 0u));

    /* One apad_kbm_touch_input, shared by all three builders. RAW DS pixels:
     * apad_kbm_touch_init() was given this console's own 256x192 and scaled
     * its boxes to match. `l_held`/`r_held` are the RAW hardware sample,
     * independent of the ctx->st.buttons clearing just above. */
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
     * hardware scan this frame's readout draws from. client_ticks_ms is
     * deliberately not set here: the engine stamps it when the datagram is
     * actually built. */
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
         * ACTIVE. Persist the address so a returning unit's connect screen
         * prefills next launch. Gated on the edge, not on every ACTIVE frame,
         * so a long session does not touch the card on every tick; and gated
         * on ACTIVE, not on anything typed, so a mistyped address is never
         * what gets saved. Never the PIN -- it never reaches config_nds.c at
         * all.
         *
         * ADDRESS ONLY. The network half of the record belongs to
         * screen_wifi.c, which writes it the moment the radio associates; this
         * call used to pass ctx->ssid too, and would now silently forget a
         * saved network on every boot that joined through Wifi_AutoConnect()
         * (where DSWiFi cannot say which SSID it picked, so ctx->ssid is
         * empty). */
        apad_nds_config_save_address(ctx->ip_text, ctx->port_text);
    }
    ctx->connected = 1;
    return APAD_SCREEN_SESSION;
}

/* ------------------------------------------------------------------------ */

static void session_draw_top(app_ctx *ctx)
{
    app_draw_status_top(ctx, 1);

    /* The STATUS/ERROR line. Overlay ON: the raw code, for as long as the
     * overlay stays on. Overlay OFF: human text only, and only for
     * STATUS_MSG_TTL_FRAMES after it last changed -- a line that never goes
     * away reads as a permanent warning long after it stopped being news. */
    if (ctx->show_diag) {
        ui_textf_fit(8.0f, 204.0f, UI_S_TINY,
                     s_msg_is_error ? ui_c_bad() : ui_c_dim(), UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f, "%s %u: %s",
                     s_msg_is_error ? "ERROR" : "STATUS",
                     (unsigned)ctx->stats.status_code,
                     apad_client_message(ctx->client));
    } else if (s_msg_fresh_frames > 0) {
        ui_textf_fit(8.0f, 204.0f, UI_S_TINY,
                     s_msg_is_error ? ui_c_bad() : ui_c_dim(), UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f, "%s", apad_client_message(ctx->client));
    }

    if (ctx->show_diag) {
        /* THE FIVE-PHASE BREAKDOWN (main.c's app_frame_phases_ms()), added
         * for the frame-rate pass that follows shim/net_nds.c. Replaces the
         * button hint below rather than fighting it for the same 20px of
         * 3DS-space the top screen has left under app_draw_status_top()'s
         * UI_STATUS_BOTTOM -- diag mode already trades the hint for the
         * STATUS/ERROR code just above, so doing the same here is
         * consistent rather than a new exception. `update` is where
         * apad_client_pump_ex() runs on this screen, so it is the number to
         * compare against the server's /api/state rx_packets rate; `flip` is
         * mostly the VBlank wait, not work -- see main.c's loop comment. */
        const app_frame_phases *ph = app_frame_phases_ms();

        /* Two lines, not one: the wait/swap split and the clear_ms reading
         * (added for the draw_bottom pass that follows the recv-timeout fix)
         * do not fit alongside the original five numbers in the space one
         * line of UI_S_TINY had -- and this line already clipped before that
         * split existed (see clients/nds/source/main.c's own comment on
         * ui_textf_fit()'s truncation), so splitting in two is a real fix,
         * not just cosmetic. `clr` is ui_last_clear_ms(), the DMA full-screen
         * clear_surface() call ui_screen_bottom() makes every frame -- shown
         * here because "is the clear where draw_bottom's time goes" is
         * exactly the question this pass had to answer with a number rather
         * than a guess. */
        ui_textf_fit(8.0f, 217.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f,
                     "frame %dms  smp%lu upd%lu top%lu bot%lu(clr%lu)  key%lums",
                     app_frame_ms(),
                     (unsigned long)ph->sample_ms,
                     (unsigned long)ph->update_ms,
                     (unsigned long)ph->draw_top_ms,
                     (unsigned long)ph->draw_bottom_ms,
                     (unsigned long)ui_last_clear_ms(),
                 (unsigned long)ctx->stats.derive_ms);
        ui_textf_fit(8.0f, 228.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f, "flip wait%lu swap%lu",
                     (unsigned long)ph->flip_wait_ms,
                     (unsigned long)ph->flip_swap_ms);
    }
    /* No else branch here any more (2026-09-09 decluttering pass): the
     * DISCONNECT and SELF-TEST buttons are right there on the bottom screen
     * -- a hint that just names them again added nothing. */
}

/* The PAD mode content: server/profiles/ds-default.jsonc drives the LEFT
 * STICK from this screen in absolute mode -- the touchscreen is this
 * console's only analog input -- unless S6.12 TOUCHMAP told this client the
 * server's ACTUAL mapping, in which case that is drawn instead. Either way
 * this is a HINT for the fallback case, labelled as one on screen: the client
 * cannot know what profile the server actually loaded. */
static const char *touchmap_label(const apad_touch_region_wire *r)
{
    if (r->target == 1u) { return "LT"; }
    if (r->target == 2u) { return "RT"; }
    /* pad_bit is the OUTPUT pad's button (Xbox naming), not the Nintendo
     * APAD_BTN_* this client sends -- a touch region says what the PC will
     * see, which is the useful thing to show. */
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

/* Returns 1 if it drew a server-supplied layout, 0 if there is none and the
 * caller should fall back. Region rects arrive normalised 0..255, +Y down --
 * this screen's own coordinate convention, so this is a scale with no flip. */
static int session_draw_pad_from_touchmap(app_ctx *ctx)
{
    const apad_touchmap *tm = &ctx->stats.touchmap;
    const int top = 24, bottom = 216;
    const int usable_h = bottom - top;
    int tpx = tx3(ctx), tpy = ty3(ctx);
    unsigned i;

    if (ctx->stats.touchmap_serial == 0u || tm->region_count == 0u) {
        return 0;
    }

    for (i = 0; i < tm->region_count; i++) {
        const apad_touch_region_wire *r = &tm->regions[i];
        /* Integer throughout: this runs every frame with no FPU underneath.
         * 320 and `usable_h` are the 3DS layout space ui.c scales from. */
        int x0 = ((int)r->x0 * 320) / 255;
        int x1 = ((int)r->x1 * 320) / 255;
        int y0 = top + ((int)r->y0 * usable_h) / 255;
        int y1 = top + ((int)r->y1 * usable_h) / 255;
        int w  = x1 - x0, h = y1 - y0;
        int hit;

        if (w <= 0 || h <= 0) {
            continue;   /* a degenerate rect is the server's bug, not a crash */
        }
        hit = (ctx->st.touch_count > 0)
              && (tpx >= x0) && (tpx < x1) && (tpy >= y0) && (tpy < y1);

        ui_rect((float)x0, (float)y0, (float)w, (float)h,
                hit ? ui_c_panel_hi() : ui_c_panel());
        ui_outline((float)x0, (float)y0, (float)w, (float)h, 1.0f,
                   ui_c_border());
        ui_textf_fit((float)(x0 + w / 2), (float)(y0 + h / 2 - 12), 1.30f,
                     hit ? ui_c_accent() : ui_c_text(), UI_ALIGN_CENTER,
                     (float)(w - 4), "%s", touchmap_label(r));
        if (r->analog) {
            ui_textf_fit((float)(x0 + w / 2), (float)(y0 + h / 2 + 26),
                         UI_S_TINY, ui_c_dim(), UI_ALIGN_CENTER,
                         (float)(w - 8), "analog: slide to squeeze");
        }
    }

    if (ctx->st.touch_count > 0) {
        ui_dot((float)tpx, (float)tpy, 6.0f, ui_c_good());
    }
    return 1;
}

/* A large dim circle outline, for the PAD fallback's "this touchscreen IS
 * the stick" glyph (2026-09-09 decluttering pass replaced the "LEFT STICK" /
 * "centre is neutral..." captions with this). Integer midpoint circle --
 * NO SQRT, NO TRIG, NO FLOAT PER PIXEL: this file already computes every
 * shape's geometry in plain integers and only introduces floats at the
 * ui_rect() call boundary (session_draw_pad_from_touchmap() above does the
 * same thing for server-supplied rects), and a per-scanline sqrt() every
 * frame is exactly the drawing cost the frame-rate pass fought elsewhere in
 * this client (see ui.c's header). Eight ui_rect() 1x1 calls per step trace
 * the ring by 8-way symmetry; this is a decorative glyph, not a hit target,
 * so the small gaps a basic Bresenham circle leaves at shallow angles do not
 * matter the way they would for a touch boundary. */
static void draw_stick_ring(float ccx, float ccy, float r, uint32_t colour)
{
    int cx = (int)(ccx + 0.5f);
    int cy = (int)(ccy + 0.5f);
    int ir = (r >= 1.0f) ? (int)(r + 0.5f) : 1;
    int x = ir, y = 0, err = 0;

    while (x >= y) {
        ui_rect((float)(cx + x), (float)(cy + y), 1.0f, 1.0f, colour);
        ui_rect((float)(cx - x), (float)(cy + y), 1.0f, 1.0f, colour);
        ui_rect((float)(cx + x), (float)(cy - y), 1.0f, 1.0f, colour);
        ui_rect((float)(cx - x), (float)(cy - y), 1.0f, 1.0f, colour);
        ui_rect((float)(cx + y), (float)(cy + x), 1.0f, 1.0f, colour);
        ui_rect((float)(cx - y), (float)(cy + x), 1.0f, 1.0f, colour);
        ui_rect((float)(cx + y), (float)(cy - x), 1.0f, 1.0f, colour);
        ui_rect((float)(cx - y), (float)(cy - x), 1.0f, 1.0f, colour);
        y++;
        if (err <= 0) {
            err += 2 * y + 1;
        }
        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}

static void session_draw_pad(app_ctx *ctx)
{
    const float top = 24.0f, bottom = 216.0f;

    /* Server-supplied layout when there is one, the compiled-in guess when
     * there is not. */
    if (session_draw_pad_from_touchmap(ctx)) {
        return;
    }

    /* The DS's fallback picture is NOT the 3DS's LT/RT split: this console
     * has no stick, so ds-default.jsonc maps the whole surface to the LEFT
     * STICK in absolute mode -- thumb position IS stick position. One box
     * with a crosshair says that; two half-screen trigger pads would be a
     * picture of the 3DS's profile, not this one's. */
    ui_rect(0.0f, top, UI_BOT_W, bottom - top,
            (ctx->st.touch_count > 0) ? ui_c_panel_hi() : ui_c_panel());
    ui_outline(0.0f, top, UI_BOT_W, bottom - top, 1.0f, ui_c_border());
    ui_rect(0.0f, (top + bottom) * 0.5f, UI_BOT_W, 1.0f, ui_c_border());
    ui_rect(UI_BOT_W * 0.5f, top, 1.0f, bottom - top, ui_c_border());

    /* The stick glyph IS the explanation now (2026-09-09 decluttering pass
     * dropped "shipped DEFAULT profile...", "LEFT STICK" and "centre is
     * neutral..."): a big ring with a centre dot in the middle of a
     * touch-reactive box reads as "this is an absolute stick" without
     * needing the words spelled out, the same way the mouse glyph replaced
     * its own caption above. */
    draw_stick_ring(UI_BOT_W * 0.5f, (top + bottom) * 0.5f, 70.0f,
                    ui_c_border());
    ui_dot(UI_BOT_W * 0.5f, (top + bottom) * 0.5f, 3.0f, ui_c_border());

    /* Live contact point: the one part of this screen that is a measurement
     * rather than a hint. */
    if (ctx->st.touch_count > 0) {
        ui_dot((float)tx3(ctx), (float)ty3(ctx), 6.0f, ui_c_good());
    }
}

static void session_draw_bottom(app_ctx *ctx)
{
    draw_mode_strip(ctx);

    switch (s_mode) {
    case MODE_MOUSE: session_draw_mouse(ctx); break;
    case MODE_KEYS:  session_draw_keys(ctx);  break;
    case MODE_MEDIA: session_draw_media(ctx); break;
    case MODE_PAD:
    default:         session_draw_pad(ctx);   break;
    }

    /* Shared footer, reached by EVERY mode. kButtonStrip is the reserved
     * rectangle a touch inside never reaches the wire (session_update()'s
     * exclusion check hit-tests the same box). */
    ui_button(&kDisconnectBtn, "DISCONNECT", s_flash_disconnect, 0);
    ui_button(&kSelftestBtn, "SELF-TEST", s_flash_selftest, 0);
    s_flash_disconnect = 0;
    s_flash_selftest = 0;

    if (s_sub == SS_CONFIRM_SELFTEST || s_sub == SS_CONFIRM_DISCONNECT) {
        int is_disconnect = (s_sub == SS_CONFIRM_DISCONNECT);
        const float cw = kConfirmBox.w - 20.0f;

        ui_panel(&kConfirmBox, ui_c_panel(), ui_c_warn());

        /* TITLE QUESTION PLUS THE TWO BUTTONS ONLY (2026-09-09 decluttering
         * pass): the three-line explanations that used to fill the rest of
         * this box ("The session ends now (BYE), and the connect screen
         * counts down before reconnecting." / "The check takes about a
         * minute here, so the session closes first and reconnects after.")
         * were read once and then in the way every time after -- YES/RUN and
         * CANCEL are the only two things a person here actually needs.
         * kConfirmBox/kConfirmRun/kConfirmCancel keep their original
         * geometry unchanged (a hit box, not something this pass touches),
         * so the box now has a plain gap below the question -- a smaller
         * footprint would have meant moving the buttons, which is exactly
         * what stays out of scope here. */
        if (is_disconnect) {
            ui_textf_fit(kConfirmBox.x + kConfirmBox.w * 0.5f,
                         kConfirmBox.y + 8.0f, UI_S_BODY, ui_c_warn(),
                         UI_ALIGN_CENTER, cw, "Disconnect now?");
            ui_button(&kConfirmRun, "YES (A)", 0, 1);
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
