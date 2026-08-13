/* clients/3ds/source/ui_widgets.c
 *
 * The composite widgets that are bigger than a rectangle and shared by more
 * than one screen: the whole top-screen status layout, and the one-line gyro
 * noise figure.
 *
 * The top screen is deliberately IDENTICAL in shape whether the client is
 * connected or not. A status display whose fields move when the state changes
 * makes a person re-read it every time; keeping the RTT box in the same place
 * showing "--" is what makes "the RTT is the big number top-left" a fact
 * someone can learn once. This is also why the live input readout is drawn on
 * the connect screen too: it lets a user prove the buttons and the circle pad
 * work BEFORE there is a network to blame.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"

/* ------------------------------------------------------------------------ */
/* layout                                                                   */
/* ------------------------------------------------------------------------ */

static const ui_box kRttBox   = {   6.0f,  26.0f, 112.0f, 72.0f };
static const ui_box kInfoBox  = { 124.0f,  26.0f, 270.0f, 72.0f };
static const ui_box kInputBox = {   6.0f, 102.0f, 388.0f,  92.0f };

/* ------------------------------------------------------------------------ */
/* button lights                                                            */
/* ------------------------------------------------------------------------ */

/* A compact lamp per wire button: lit means the bit is set in the
 * apad_input_state this frame's INPUT_STATE will carry. Reading the SAME
 * struct the engine is about to encode (ctx->st, not the raw hidKeysHeld())
 * is the point -- a lamp that reads the hardware directly would still light
 * up if the button-to-bit mapping below it were wrong, which is exactly the
 * bug it should be catching. */
typedef struct {
    const char *label;
    uint32_t    bit;
    int         needs_cstick;   /* ZL/ZR only exist on a New 3DS           */
} btn_lamp;

static const btn_lamp kLamps[] = {
    { "A",  APAD_BTN_A,          0 },
    { "B",  APAD_BTN_B,          0 },
    { "X",  APAD_BTN_X,          0 },
    { "Y",  APAD_BTN_Y,          0 },
    { "L",  APAD_BTN_L,          0 },
    { "R",  APAD_BTN_R,          0 },
    { "ZL", APAD_BTN_ZL,         1 },
    { "ZR", APAD_BTN_ZR,         1 },
    { "^",  APAD_BTN_DPAD_UP,    0 },
    { "v",  APAD_BTN_DPAD_DOWN,  0 },
    { "<",  APAD_BTN_DPAD_LEFT,  0 },
    { ">",  APAD_BTN_DPAD_RIGHT, 0 },
    /* START/SELECT joined this list on 2026-08-12: until then the two keys
     * were reserved by the UI (disconnect / self-test) and their wire bits
     * were never set, so a lamp for them would have been permanently dark and
     * misleading. They are ordinary wire buttons now (main.c's
     * app_sample_input(), screen_session.c's touch-button footer), so they
     * belong in the same "does the bit the wire will carry actually light
     * up" proof as everything else here. */
    { "STA", APAD_BTN_START,     0 },
    { "SEL", APAD_BTN_SELECT,    0 },
    { "TCH", APAD_BTN_TOUCH_PRESS, 0 }
};
#define LAMP_COUNT ((int)(sizeof kLamps / sizeof kLamps[0]))

static void draw_lamps(app_ctx *ctx, float x, float y)
{
    /* w shrank from 26 to 23 when STA/SEL joined the row (LAMP_COUNT went
     * 13 -> 15): at the old width the row's total 15*(26+2) - 2 = 418px ran
     * off the right edge of both kInputBox (388px wide) and the 400px top
     * screen itself. 15*(23+2) - 2 = 373px clears kInputBox's inner width
     * (380px, x+8 to the box's right edge) with room to spare. */
    const float w = 23.0f, h = 17.0f, gap = 2.0f;
    int i;

    for (i = 0; i < LAMP_COUNT; i++) {
        ui_box b;
        int on       = (ctx->st.buttons & kLamps[i].bit) != 0u;
        int unusable = kLamps[i].needs_cstick && !ctx->have_cstick;
        uint32_t fill, ink;

        b.x = x + (float)i * (w + gap);
        b.y = y;
        b.w = w;
        b.h = h;

        if (unusable) {
            /* Drawn, not hidden: "this console has no ZL" is information, and
             * a lamp that silently disappears looks like a rendering bug. */
            fill = ui_c_bg();
            ink  = ui_c_border();
        } else if (on) {
            fill = ui_c_accent();
            ink  = ui_c_bg();
        } else {
            fill = ui_c_panel();
            ink  = ui_c_dim();
        }
        ui_rect(b.x, b.y, b.w, b.h, fill);
        ui_outline(b.x, b.y, b.w, b.h, 1.0f, on ? ui_c_accent() : ui_c_border());
        ui_textf(b.x + b.w * 0.5f, b.y + 1.0f, 0.40f, ink, UI_ALIGN_CENTER,
                 "%s", kLamps[i].label);
    }
}

/* ------------------------------------------------------------------------ */
/* stick and touch widgets                                                  */
/* ------------------------------------------------------------------------ */

/* A dot in a box. `ax`/`ay` are wire axis values, so +Y is UP (XInput
 * convention, docs/CONVENTIONS.md) and the dot's screen Y is therefore NEGATED here --
 * this widget is the only place in the client that flips it, and it flips it
 * for the screen, never for the wire. */
static void draw_stick(float x, float y, float size, int16_t ax, int16_t ay,
                       int enabled, const char *label)
{
    ui_box b;
    float r  = size * 0.5f - 4.0f;
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float dx = ((float)ax / 32768.0f) * r;
    float dy = ((float)ay / 32768.0f) * r;

    b.x = x; b.y = y; b.w = size; b.h = size;
    ui_panel(&b, ui_c_bg(), ui_c_border());
    /* crosshair */
    ui_rect(x + 2.0f, cy, size - 4.0f, 1.0f, ui_c_panel_hi());
    ui_rect(cx, y + 2.0f, 1.0f, size - 4.0f, ui_c_panel_hi());
    if (enabled) {
        C2D_DrawCircleSolid(cx + dx, cy - dy, 0.0f, 3.5f, ui_c_accent());
    }
    ui_textf(cx, y + size + 1.0f, 0.38f, enabled ? ui_c_dim() : ui_c_border(),
             UI_ALIGN_CENTER, "%s", label);
}

/* The bottom screen in miniature, with the current touch point on it. Same
 * +Y-down screen space as the wire's touch coordinates (docs/PROTOCOL.md
 * S5.3), so no flip here -- and that asymmetry with draw_stick() above is the
 * convention working, not a mistake. */
static void draw_touchpad(app_ctx *ctx, float x, float y, float w, float h)
{
    ui_box b;

    b.x = x; b.y = y; b.w = w; b.h = h;
    ui_panel(&b, ui_c_bg(), ui_c_border());
    if (ctx->st.touch_count > 0) {
        float tx = x + ((float)ctx->st.touches[0].x + 32768.0f) / 65536.0f * w;
        float ty = y + ((float)ctx->st.touches[0].y + 32768.0f) / 65536.0f * h;

        C2D_DrawCircleSolid(tx, ty, 0.0f, 3.5f, ui_c_good());
    }
    ui_textf(x + w * 0.5f, y + h + 1.0f, 0.38f, ui_c_dim(), UI_ALIGN_CENTER,
             "touch");
}

/* ------------------------------------------------------------------------ */
/* gyro                                                                     */
/* ------------------------------------------------------------------------ */

void app_draw_gyro_line(app_ctx *ctx, float x, float y)
{
    int32_t mean[3];
    int16_t noise[3];
    int i;

    /* Diagnostics overlay only (app.h's ctx->show_diag): a noise figure is
     * exactly the "gyro noise readout" kind of debug clutter the 2026-08-12
     * hide-the-debug-surface pass was asked to stop showing everywhere by
     * default. SELECT (session screen) still reveals it -- nothing here was
     * removed, only defaulted off. */
    if (!ctx->show_diag) {
        return;
    }
    if (!ctx->have_gyro) {
        ui_textf_fit(x, y, UI_S_TINY, ui_c_border(), UI_ALIGN_LEFT,
                     UI_TOP_W - 2.0f * x, "gyro  not available on this console");
        return;
    }
    for (i = 0; i < 3; i++) {
        app_gyro_stats(i, &mean[i], &noise[i]);
    }
    /* deci-degrees/s on the wire (docs/PROTOCOL.md S5), degrees/s on screen:
     * nobody thinks in tenths, and the deadzone this figure is compared
     * against is quoted in deg/s in server/profiles/3ds-default.jsonc. */
    ui_textf_fit(x, y, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
             UI_TOP_W - 2.0f * x,
             "gyro  pitch %+.1f  roll %+.1f  yaw %+.1f deg/s   noise +-%.1f",
             (double)mean[0] / 10.0, (double)mean[1] / 10.0,
             (double)mean[2] / 10.0,
             (double)app_gyro_noise_peak() / 10.0);
}

/* ------------------------------------------------------------------------ */
/* battery glyph -- header row, no words (2026-08-12 hardware pass)         */
/* ------------------------------------------------------------------------ */

/* GLYPH + NUMBER, nothing else: today's UX bar is "no explanatory text, no
 * advertised gestures, visual feedback over words", and "a battery indicator
 * is a glyph + number, not a sentence" is the literal brief. Outline rect,
 * proportional fill, one small nub -- the shape every OS battery icon uses,
 * so nothing about it needs a legend. */
#define BATT_GLYPH_W    16.0f
#define BATT_GLYPH_H     9.0f
#define BATT_NUB_W       2.0f
#define BATT_NUB_H       5.0f
/* Fixed pixel budget (glyph + nub + gap + "100%" at UI_S_TINY), the same
 * fixed-box-layout style as kRttBox/kInfoBox/kInputBox above rather than a
 * C2D_TextGetDimensions() measurement -- ui_header_ex()'s right_pad only
 * needs an upper bound to keep the state text clear of this, not the exact
 * width of today's percentage. */
#define BATT_RESERVE_PX 54.0f

static uint32_t battery_colour(int percent)
{
    if (percent <= 20) return ui_c_bad();
    if (percent <= 40) return ui_c_warn();
    return ui_c_good();
}

/* Right-aligned so its right edge sits at `right_x`; vertically centred on
 * `mid_y`. `charging` has no glyph state of its own today (the wire has no
 * charging bit either, docs/PROTOCOL.md S5.5) -- it is accepted so the call
 * site does not need an #ifdef the day this glyph grows a charging nub. */
static void draw_battery_glyph(float right_x, float mid_y, int percent,
                               int charging)
{
    uint32_t colour = battery_colour(percent);
    float body_x = right_x - BATT_GLYPH_W - BATT_NUB_W;
    float body_y = mid_y - BATT_GLYPH_H * 0.5f;
    float fill_w = (BATT_GLYPH_W - 4.0f) * ((float)percent / 100.0f);

    (void)charging;

    ui_outline(body_x, body_y, BATT_GLYPH_W, BATT_GLYPH_H, 1.0f, colour);
    if (fill_w > 0.0f) {
        ui_rect(body_x + 2.0f, body_y + 2.0f, fill_w, BATT_GLYPH_H - 4.0f,
                colour);
    }
    ui_rect(body_x + BATT_GLYPH_W,
            body_y + (BATT_GLYPH_H - BATT_NUB_H) * 0.5f, BATT_NUB_W,
            BATT_NUB_H, colour);

    ui_textf(body_x - 4.0f, mid_y - UI_LINE(UI_S_TINY) * 0.5f, UI_S_TINY,
             colour, UI_ALIGN_RIGHT, "%d%%", percent);
}

/* ------------------------------------------------------------------------ */
/* the top screen                                                           */
/* ------------------------------------------------------------------------ */

void app_draw_status_top(app_ctx *ctx, int live)
{
    const char *state_text;
    uint32_t    state_colour;
    float       lx, ly;
    int         batt = app_battery_percent();

    if (live) {
        /* The session HUD line (clients/common/apad_ui.h): one call gives the
         * same word every client derives from this stats snapshot, instead of
         * a screen hand-rolling "CONNECTED" as a fact rather than a reading. */
        apad_ui_session_line hud;

        apad_ui_session_status(&ctx->stats, &hud);
        state_text   = apad_ui_msg(hud.id);
        state_colour = ui_c_good();
    } else {
        state_text   = "not connected";
        state_colour = ui_c_dim();
    }
    /* No version string here (2026-08-12 pass): it is one more thing on a
     * screen a customer looks at constantly, and it already lives on the
     * self-test screen (docs/CONVENTIONS.md's ABI/version home) for anyone who needs
     * it.
     *
     * Battery glyph: header row, right slot, to the LEFT of the state text
     * (ui_header_ex()'s right_pad reserves the room so they cannot overlap).
     * Shown on both the connect and session screens because both call this
     * function -- glyph-only when idle ("not connected"), glyph beside the
     * live session word once connected, per this task's brief. Absent
     * entirely (no reserved space either) when app_battery_percent() is -1
     * (ptm:u never came up) OR above 100 (255: ptm:u is up but no read has
     * succeeded yet -- drawing that would be a 255% glyph, audit finding B). */
    ui_header_ex(UI_TOP_W, "AtticPad 3DS", state_text, state_colour,
                 (batt >= 0 && batt <= 100) ? BATT_RESERVE_PX : 0.0f);
    if (batt >= 0 && batt <= 100) {
        draw_battery_glyph(UI_TOP_W - 6.0f, 11.0f, batt,
                           app_battery_charging());
    }

    /* -- RTT, big, always in the same place -------------------------------
     * "Draw a live RTT readout on the top screen. Free diagnostics for every
     * user." It is -1 until the first PONG comes back, and also for the one
     * second after a lost PONG before the engine asks again -- both show as
     * "--" rather than as a number that would read like a measurement. */
    ui_panel(&kRttBox, ui_c_panel(), ui_c_border());
    ui_textf(kRttBox.x + 6.0f, kRttBox.y + 2.0f, UI_S_TINY, ui_c_dim(),
             UI_ALIGN_LEFT, "RTT");
    if (live && ctx->stats.rtt_ms >= 0) {
        ui_textf(kRttBox.x + kRttBox.w * 0.5f, kRttBox.y + 14.0f, UI_S_HUGE,
                 ui_c_accent(), UI_ALIGN_CENTER, "%d", (int)ctx->stats.rtt_ms);
    } else if (live) {
        /* -1 until the first PONG: a number here would read like a
         * measurement rather than the absence of one. */
        ui_textf_fit(kRttBox.x + kRttBox.w * 0.5f, kRttBox.y + 24.0f,
                     UI_S_BODY, ui_c_border(), UI_ALIGN_CENTER,
                     kRttBox.w - 12.0f, "%s",
                     apad_ui_msg(APAD_MSG_RTT_MEASURING));
    } else {
        ui_textf(kRttBox.x + kRttBox.w * 0.5f, kRttBox.y + 14.0f, UI_S_HUGE,
                 ui_c_border(), UI_ALIGN_CENTER, "--");
    }
    ui_textf(kRttBox.x + kRttBox.w - 6.0f, kRttBox.y + kRttBox.h - 16.0f,
             UI_S_SMALL, ui_c_dim(), UI_ALIGN_RIGHT, "ms");

    /* -- server / session panel, live input readout -----------------------
     * Diagnostics overlay only (app.h's ctx->show_diag), off by default.
     * Session id, pad slot, tx/rx packet counts and the caps mask are
     * exactly the "session-id/slot internals", "rx/tx packet counters" and
     * "anything hex" this pass was told to stop showing on every frame; the
     * button lamps, stick/touch dots and LX/LY/RX/RY/TX/TY numbers are the
     * "input preview" in the same list. Nothing here was removed -- SELECT
     * on the session screen still turns it back on, same struct, same
     * numbers, just not drawn unconditionally in front of every user.
     * The lamps/sticks/touch dots below are NOT in that list -- seeing your
     * own presses light up is the product, not a diagnostic (user review,
     * 2026-08-12) -- so only the panels of numbers hide. */
    if (ctx->show_diag) {
    ui_panel(&kInfoBox, ui_c_panel(), ui_c_border());
    lx = kInfoBox.x + 8.0f;
    ly = kInfoBox.y + 3.0f;
    {
        const float vx = lx + 62.0f;
        const float step = 17.0f;

        ui_textf(lx, ly, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, "server");
        ui_textf_fit(vx, ly, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                     kInfoBox.x + kInfoBox.w - 8.0f - vx, "%s:%s",
                     ctx->ip_text, ctx->port_text);
        ly += step;

        ui_textf(lx, ly, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, "session");
        if (live) {
            ui_textf(vx, ly, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                     "%u", (unsigned)ctx->stats.session_id);
            ui_textf(kInfoBox.x + kInfoBox.w - 8.0f, ly, UI_S_SMALL,
                     ui_c_text(), UI_ALIGN_RIGHT, "pad slot %u",
                     (unsigned)ctx->stats.pad_slot);
        } else {
            ui_textf(vx, ly, UI_S_SMALL, ui_c_border(), UI_ALIGN_LEFT, "--");
        }
        ly += step;

        ui_textf(lx, ly, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, "rate");
        if (live) {
            ui_textf(vx, ly, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, "%u Hz",
                     (unsigned)ctx->stats.input_rate_hz);
            ui_textf(kInfoBox.x + kInfoBox.w - 8.0f, ly, UI_S_SMALL,
                     ui_c_dim(), UI_ALIGN_RIGHT, "tx %u  rx %u",
                     (unsigned)ctx->stats.tx_packets,
                     (unsigned)ctx->stats.rx_packets);
        } else {
            ui_textf(vx, ly, UI_S_SMALL, ui_c_border(), UI_ALIGN_LEFT,
                     "%u Hz (requested)", (unsigned)APAD_DEFAULT_RATE_HZ);
        }
        ly += step;

        ui_textf(lx, ly, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, "caps");
        ui_textf(vx, ly, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT, "0x%08lX%s",
                 (unsigned long)ctx->caps,
                 ctx->is_new3ds ? "   New 3DS" : "   Old 3DS");
    }

    } else {
        /* The gyro cube (app.h's gyro_cube_step_and_draw(), gyro_cube.c):
         * product feedback for the gyro, not a diagnostic, so it lives
         * exactly where the diagnostics panel above would otherwise be --
         * centred in kInfoBox, at ~90% of its height. Called from here
         * rather than from each screen's draw_top() so the connect and
         * session screens (both callers of app_draw_status_top()) share one
         * continuous animation instead of two independently-seeded ones. */
        gyro_cube_step_and_draw(ctx, kInfoBox.x + kInfoBox.w * 0.5f,
                                 kInfoBox.y + kInfoBox.h * 0.5f,
                                 kInfoBox.h * 0.9f);
    }

    /* -- live input readout: always on ------------------------------------ */
    ui_panel(&kInputBox, ui_c_panel(), ui_c_border());
    ui_textf(kInputBox.x + 8.0f, kInputBox.y + 2.0f, UI_S_TINY, ui_c_dim(),
             UI_ALIGN_LEFT, "INPUT");
    if (ctx->show_diag) {
        ui_textf(kInputBox.x + kInputBox.w - 8.0f, kInputBox.y + 2.0f,
                 UI_S_TINY, ui_c_dim(), UI_ALIGN_RIGHT, "buttons 0x%05lX",
                 (unsigned long)ctx->st.buttons);
    }
    draw_lamps(ctx, kInputBox.x + 8.0f, kInputBox.y + 17.0f);

    /* y+38 rather than y+40: each widget's caption sits UNDER it, and at
     * y+40 the caption's descenders clipped the panel's bottom edge by a
     * pixel. */
    draw_stick(kInputBox.x + 10.0f, kInputBox.y + 38.0f, 40.0f,
               ctx->st.axes[APAD_AXIS_LX], ctx->st.axes[APAD_AXIS_LY], 1,
               "circle");
    draw_stick(kInputBox.x + 58.0f, kInputBox.y + 38.0f, 40.0f,
               ctx->st.axes[APAD_AXIS_RX], ctx->st.axes[APAD_AXIS_RY],
               ctx->have_cstick, "c-stick");
    draw_touchpad(ctx, kInputBox.x + 108.0f, kInputBox.y + 38.0f, 53.0f, 40.0f);

    if (ctx->show_diag) {
        const float tx = kInputBox.x + 176.0f;
        float ty = kInputBox.y + 40.0f;

        ui_textf(tx, ty, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                 "LX %6d   LY %6d", (int)ctx->st.axes[APAD_AXIS_LX],
                 (int)ctx->st.axes[APAD_AXIS_LY]);
        ty += 15.0f;
        if (ctx->have_cstick) {
            ui_textf(tx, ty, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                     "RX %6d   RY %6d", (int)ctx->st.axes[APAD_AXIS_RX],
                     (int)ctx->st.axes[APAD_AXIS_RY]);
        } else {
            ui_textf(tx, ty, UI_S_SMALL, ui_c_border(), UI_ALIGN_LEFT,
                     "RX     --   RY     -- (Old 3DS)");
        }
        ty += 15.0f;
        if (ctx->st.touch_count > 0) {
            ui_textf(tx, ty, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                     "TX %6d   TY %6d", (int)ctx->st.touches[0].x,
                     (int)ctx->st.touches[0].y);
        } else {
            ui_textf(tx, ty, UI_S_SMALL, ui_c_border(), UI_ALIGN_LEFT,
                     "TX     --   TY     --");
        }
    }
}
