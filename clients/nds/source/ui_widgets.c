/* clients/nds/source/ui_widgets.c
 *
 * clients/3ds/source/ui_widgets.c, ported. The top screen is deliberately
 * IDENTICAL in shape whether the client is connected or not, for the reason
 * the 3DS file gives: a status display whose fields move when the state
 * changes makes a person re-read it every time, and keeping the RTT box in
 * the same place showing "--" is what makes "the RTT is the big number
 * top-left" a fact someone can learn once. The live input readout is drawn on
 * the connect screen too, so a user can prove the buttons work BEFORE there is
 * a network to blame.
 *
 * DIFFERENCES FROM THE 3DS FILE, all of them removals plus one substitution:
 *
 *   - draw_stick() and both of its call sites are GONE. This console has no
 *     analog stick of any kind, ctx->st.axes[] is permanently zero, and a
 *     centred stick dot would be a picture of hardware that is not there.
 *     The touch widget moves into the space the two sticks vacated -- one
 *     layout change, listed in the report -- because the touchscreen IS this
 *     console's stick (server/profiles/ds-default.jsonc drives the left stick
 *     from it in absolute mode) and it deserves the room.
 *   - app_draw_gyro_line() and the gyro cube are GONE: no gyroscope.
 *   - the ZL/ZR lamps and their `needs_cstick` column are GONE, along with
 *     the "New 3DS / Old 3DS" caps suffix. The DS has neither.
 *   - the big RTT figure goes through ui_bignum() instead of ui_textf() at
 *     UI_S_HUGE. Same number, same box, a face that exists on this console.
 *   - every fixed label inside a small box (the button lamps) goes through
 *     ui_textf_fit() rather than ui_textf(). The top screen squeezes 400px of
 *     3DS layout into 256px, so a three-character lamp label that fitted a
 *     23px box there has 14px here; shrinking the FIT RULE is what this
 *     client is allowed to change, the copied layout numbers are not.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"

/* ------------------------------------------------------------------------ */
/* layout -- 3DS top-screen coordinates, unchanged                          */
/* ------------------------------------------------------------------------ */

static const ui_box kRttBox   = {   6.0f,  26.0f, 112.0f, 72.0f };
static const ui_box kInfoBox  = { 124.0f,  26.0f, 270.0f, 72.0f };
static const ui_box kInputBox = {   6.0f, 102.0f, 388.0f,  92.0f };

/* ------------------------------------------------------------------------ */
/* button lights                                                            */
/* ------------------------------------------------------------------------ */

/* A compact lamp per wire button: lit means the bit is set in the
 * apad_input_state this frame's INPUT_STATE will carry. Reading the SAME
 * struct the engine is about to encode (ctx->st, not the raw keysHeld()) is
 * the point -- a lamp that read the hardware directly would still light up if
 * the button-to-bit mapping below it were wrong, which is exactly the bug it
 * should be catching. */
typedef struct {
    const char *label;
    uint32_t    bit;
} btn_lamp;

static const btn_lamp kLamps[] = {
    { "A",  APAD_BTN_A },
    { "B",  APAD_BTN_B },
    { "X",  APAD_BTN_X },
    { "Y",  APAD_BTN_Y },
    { "L",  APAD_BTN_L },
    { "R",  APAD_BTN_R },
    { "^",  APAD_BTN_DPAD_UP },
    { "v",  APAD_BTN_DPAD_DOWN },
    { "<",  APAD_BTN_DPAD_LEFT },
    { ">",  APAD_BTN_DPAD_RIGHT },
    { "STA", APAD_BTN_START },
    { "SEL", APAD_BTN_SELECT },
    { "TCH", APAD_BTN_TOUCH_PRESS }
};
#define LAMP_COUNT ((int)(sizeof kLamps / sizeof kLamps[0]))

static void draw_lamps(app_ctx *ctx, float x, float y)
{
    /* WIDER THAN THE 3DS's 23px, and this is one of the two deliberate layout
     * deviations in this file. 400px of 3DS top screen becomes 256px here, so
     * a 23px lamp is 14 physical pixels and a three-character label ("STA")
     * shrank to one. 27+2 keeps all 13 inside kInputBox (13*29-2 = 375, and
     * the row starts 8px in, so 383 of the box's 388) and gives 17 physical
     * pixels -- enough for three glyphs of the 8x8 face. */
    const float w = 27.0f, h = 17.0f, gap = 2.0f;
    int i;

    for (i = 0; i < LAMP_COUNT; i++) {
        ui_box b;
        int on = (ctx->st.buttons & kLamps[i].bit) != 0u;
        uint32_t fill, ink;

        b.x = x + (float)i * (w + gap);
        b.y = y;
        b.w = w;
        b.h = h;

        if (on) {
            fill = ui_c_accent();
            ink  = ui_c_bg();
        } else {
            fill = ui_c_panel();
            ink  = ui_c_dim();
        }
        ui_rect(b.x, b.y, b.w, b.h, fill);
        ui_outline(b.x, b.y, b.w, b.h, 1.0f, on ? ui_c_accent() : ui_c_border());
        ui_textf_fit(b.x + b.w * 0.5f, b.y + 1.0f, 0.40f, ink, UI_ALIGN_CENTER,
                     b.w - 2.0f, "%s", kLamps[i].label);
    }
}

/* ------------------------------------------------------------------------ */
/* touch widget                                                             */
/* ------------------------------------------------------------------------ */

/* The bottom screen in miniature, with the current touch point on it. Same
 * +Y-down screen space as the wire's touch coordinates (docs/PROTOCOL.md
 * S5.3), so no flip -- and on this console that asymmetry has nothing to be
 * asymmetric WITH, since draw_stick() is gone. */
static void draw_touchpad(app_ctx *ctx, float x, float y, float w, float h)
{
    ui_box b;

    b.x = x; b.y = y; b.w = w; b.h = h;
    ui_panel(&b, ui_c_bg(), ui_c_border());
    if (ctx->st.touch_count > 0) {
        /* The wire's -32768..32767, back to a position inside this widget.
         * Integer, and deliberately not the float expression the 3DS uses:
         * this runs every frame on a CPU with no FPU. */
        int tx = (int)(((int32_t)ctx->st.touches[0].x + 32768) >> 8);
        int ty = (int)(((int32_t)ctx->st.touches[0].y + 32768) >> 8);

        ui_dot(x + (float)(((int)w * tx) >> 8),
               y + (float)(((int)h * ty) >> 8), 3.5f, ui_c_good());
    }
    ui_textf_fit(x + w * 0.5f, y + h + 1.0f, 0.38f, ui_c_dim(),
                 UI_ALIGN_CENTER, w, "touch");
}

/* ------------------------------------------------------------------------ */
/* battery glyph -- header row, no words                                    */
/* ------------------------------------------------------------------------ */

/* GLYPH + NUMBER, nothing else, exactly as the 3DS: outline rect,
 * proportional fill, one small nub -- the shape every OS battery icon uses,
 * so nothing about it needs a legend. Absent entirely on a DS or DS Lite,
 * where app_battery_percent() returns -1 because the hardware has one bit of
 * charge information and APAD_CAP_BATTERY is never claimed (main.c). */
#define BATT_GLYPH_W    16.0f
#define BATT_GLYPH_H     9.0f
#define BATT_NUB_W       2.0f
#define BATT_NUB_H       5.0f
/* The room ui_header_ex()'s right_pad reserves so the state text can't run
 * under the battery glyph + its "100%" number. NOTE this is LARGER than the
 * 3DS's 54: these are 3DS-space (400px) units that ui_header_ex() scales down
 * by 256/400, but the "100%" number is drawn with the DS's fixed 8x8 face,
 * which is proportionally WIDER than the 3DS's small system font at the same
 * UI_S_TINY. At 54 the number's left edge poked ~6px past the reserve and the
 * green "Connected" HUD word overlapped it (seen on a 3DS in TWL/DSi mode).
 * Glyph+nub is BATT_GLYPH_W+BATT_NUB_W=18; "100%" at the 8x8 face measures
 * ~24 native px = ~37 in this 3DS space; 18+37 = 55, so 72 leaves a clear
 * gap. Only spent when a battery reading exists (DSi); DS mode passes 0. */
#define BATT_RESERVE_PX 72.0f

static uint32_t battery_colour(int percent)
{
    if (percent <= 20) return ui_c_bad();
    if (percent <= 40) return ui_c_warn();
    return ui_c_good();
}

static void draw_battery_glyph(float right_x, float mid_y, int percent)
{
    uint32_t colour = battery_colour(percent);
    float body_x = right_x - BATT_GLYPH_W - BATT_NUB_W;
    float body_y = mid_y - BATT_GLYPH_H * 0.5f;
    /* Integer, unlike the 3DS's float expression: this draws every frame on a
     * CPU with no FPU. 12 is BATT_GLYPH_W - 4. */
    float fill_w = (float)((12 * percent) / 100);

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
    ui_header_ex(UI_TOP_W, ctx->is_dsi ? "AtticPad DSi" : "AtticPad DS",
                 state_text, state_colour,
                 (batt >= 0 && batt <= 100) ? BATT_RESERVE_PX : 0.0f);
    if (batt >= 0 && batt <= 100) {
        draw_battery_glyph(UI_TOP_W - 6.0f, 11.0f, batt);
    }

    /* -- RTT, big, always in the same place -------------------------------
     * Free diagnostics for every user, and the one number docs/CONVENTIONS.md calls
     * product rather than debug -- so it is never gated on show_diag. It is
     * -1 until the first PONG comes back, and for the second after a lost
     * PONG before the engine asks again; both show as "--" rather than as a
     * number that would read like a measurement. */
    ui_panel(&kRttBox, ui_c_panel(), ui_c_border());
    ui_textf(kRttBox.x + 6.0f, kRttBox.y + 2.0f, UI_S_TINY, ui_c_dim(),
             UI_ALIGN_LEFT, "RTT");
    if (live && ctx->stats.rtt_ms >= 0) {
        ui_bignum(kRttBox.x + kRttBox.w * 0.5f, kRttBox.y + 14.0f,
                  ui_c_accent(), UI_ALIGN_CENTER, "%d", (int)ctx->stats.rtt_ms);
    } else if (live) {
        ui_textf_fit(kRttBox.x + kRttBox.w * 0.5f, kRttBox.y + 24.0f,
                     UI_S_BODY, ui_c_border(), UI_ALIGN_CENTER,
                     kRttBox.w - 12.0f, "%s",
                     apad_ui_msg(APAD_MSG_RTT_MEASURING));
    } else {
        ui_bignum(kRttBox.x + kRttBox.w * 0.5f, kRttBox.y + 14.0f,
                  ui_c_border(), UI_ALIGN_CENTER, "--");
    }
    ui_textf(kRttBox.x + kRttBox.w - 6.0f, kRttBox.y + kRttBox.h - 16.0f,
             UI_S_SMALL, ui_c_dim(), UI_ALIGN_RIGHT, "ms");

    /* -- server / session panel -------------------------------------------
     * Diagnostics overlay only, off by default: session id, pad slot, tx/rx
     * counts and the caps mask are internals. Unlike the 3DS there is nothing
     * to put here when the overlay is off (that console draws its gyro cube
     * in this space), so the box is simply left empty -- the layout does not
     * move, which is the whole point of this screen. */
    if (ctx->show_diag) {
    ui_panel(&kInfoBox, ui_c_panel(), ui_c_border());
    lx = kInfoBox.x + 8.0f;
    ly = kInfoBox.y + 3.0f;
    {
        /* 84, not the 3DS's 62. The top screen's 400px of layout becomes
         * 256 physical pixels, so a 62px label column is 39px here and
         * "session" alone is 45 -- the label and its value overlapped. */
        const float vx = lx + 84.0f;
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
            /* One string, not a left-aligned value plus a right-aligned
             * one: at this console's 0.64 horizontal scale the two collided
             * in the middle of the box. */
            ui_textf_fit(vx, ly, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                         kInfoBox.x + kInfoBox.w - 8.0f - vx,
                         "%uHz tx%u rx%u",
                         (unsigned)ctx->stats.input_rate_hz,
                         (unsigned)ctx->stats.tx_packets,
                         (unsigned)ctx->stats.rx_packets);
        } else {
            ui_textf(vx, ly, UI_S_SMALL, ui_c_border(), UI_ALIGN_LEFT,
                     "%u Hz (requested)", (unsigned)APAD_DEFAULT_RATE_HZ);
        }
        ly += step;

        ui_textf(lx, ly, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, "caps");
        ui_textf_fit(vx, ly, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                     kInfoBox.x + kInfoBox.w - 8.0f - vx, "0x%04lX  %dms/f",
                     (unsigned long)ctx->caps, app_frame_ms());
    }
    }

    /* -- live input readout: always on ------------------------------------
     * Seeing your own presses light up is the product, not a diagnostic. */
    ui_panel(&kInputBox, ui_c_panel(), ui_c_border());
    ui_textf(kInputBox.x + 8.0f, kInputBox.y + 2.0f, UI_S_TINY, ui_c_dim(),
             UI_ALIGN_LEFT, "INPUT");
    if (ctx->show_diag) {
        ui_textf(kInputBox.x + kInputBox.w - 8.0f, kInputBox.y + 2.0f,
                 UI_S_TINY, ui_c_dim(), UI_ALIGN_RIGHT, "buttons 0x%05lX",
                 (unsigned long)ctx->st.buttons);
    }
    draw_lamps(ctx, kInputBox.x + 8.0f, kInputBox.y + 17.0f);

    /* The touch widget takes the space the 3DS's two stick dials had, at the
     * 3DS's own y. Wider than the 53px it gets there because the touchscreen
     * is this console's only analog input. */
    draw_touchpad(ctx, kInputBox.x + 10.0f, kInputBox.y + 38.0f, 66.0f, 40.0f);

    if (ctx->show_diag) {
        const float tx = kInputBox.x + 90.0f;
        float ty = kInputBox.y + 40.0f;

        if (ctx->st.touch_count > 0) {
            ui_textf(tx, ty, UI_S_SMALL, ui_c_text(), UI_ALIGN_LEFT,
                     "TX %6d   TY %6d", (int)ctx->st.touches[0].x,
                     (int)ctx->st.touches[0].y);
        } else {
            ui_textf(tx, ty, UI_S_SMALL, ui_c_border(), UI_ALIGN_LEFT,
                     "TX     --   TY     --");
        }
        /* No "no sticks / gyro here" line any more (2026-09-09 decluttering
         * pass): this console visibly has no stick/gyro readout to point at
         * in the first place, so the sentence explained the absence of
         * something nobody was looking for. */
    }
}
