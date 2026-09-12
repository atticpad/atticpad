/*
 * clients/psp/source/screen_fatal.c -- bring-up failed.
 *
 * Mirrors clients/3ds/source/screen_fatal.c, including the part that matters
 * most: the self-test is STILL REACHABLE from here. The conformance suite
 * needs no network and no display beyond text, so a console that cannot reach
 * a server can still answer "is the protocol code intact, or is it this
 * machine?" -- which is the question someone in that position actually has.
 */
#include <pspctrl.h>

#include "app.h"
#include "ui.h"

static void fatal_enter(app_ctx *ctx)
{
    (void)ctx;
}

static apad_screen_id fatal_update(app_ctx *ctx)
{
    if (app_pressed(ctx, PSP_CTRL_SELECT)) {
        ctx->selftest_return = APAD_SCREEN_FATAL;
        return APAD_SCREEN_SELFTEST;
    }
    return APAD_SCREEN_FATAL;
}

static void fatal_draw(app_ctx *ctx)
{
    ui_box b;

    ui_header("AtticPad -- cannot start", "FAILED", ui_c_bad());

    b.x = 6; b.y = 24; b.w = UI_W - 12; b.h = 96;
    ui_panel(&b, ui_c_panel(), ui_c_bad());
    ui_text(b.x + 10, b.y + 10, ui_c_bad(), UI_ALIGN_LEFT, "BRING-UP FAILED");
    ui_textf_fit(b.x + 10, b.y + 30, ui_c_text(), UI_ALIGN_LEFT, b.w - 20,
                 "%s", ctx->fatal);

    /* The likeliest cause on real hardware, stated plainly rather than left
     * as a hex code. sceNetApctlConnect() fails when there is no saved
     * network in the slot it was asked for, or the WLAN switch is off. */
    ui_text(b.x + 10, b.y + 52, ui_c_dim(), UI_ALIGN_LEFT,
            "If this is a network error, check that the WLAN switch");
    ui_text(b.x + 10, b.y + 62, ui_c_dim(), UI_ALIGN_LEFT,
            "is on and that the PSP has a saved Wi-Fi connection.");
    ui_text(b.x + 10, b.y + 78, ui_c_dim(), UI_ALIGN_LEFT,
            "Settings > Network Settings > Infrastructure Mode");

    ui_rect(0, UI_H - 14, UI_W, 1, ui_c_border());
    ui_text(6, UI_H - 11, ui_c_dim(), UI_ALIGN_LEFT,
            "SELECT: self-test (works without a network)   HOME: quit");
}

const apad_screen apad_screen_fatal = {
    "fatal", fatal_enter, fatal_update, fatal_draw
};
