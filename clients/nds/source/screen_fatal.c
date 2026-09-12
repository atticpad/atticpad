/* clients/nds/source/screen_fatal.c
 *
 * clients/3ds/source/screen_fatal.c, ported. Bring-up failed and there is
 * nothing the client can usefully do: DSWiFi did not initialise, or the
 * session engine could not allocate a socket once the radio was up.
 *
 * It is a screen like any other for one reason: it must still be able to run
 * the self-test. A console that cannot open a socket is exactly the case where
 * "is libapad itself intact on this hardware?" is the next question, and the
 * self-test needs no network at all to answer it.
 *
 * SELF-TEST HAS NO ON-SCREEN BUTTON HERE, the same as the 3DS: docs/CONVENTIONS.md's
 * convention is a HIDDEN self-test. SELECT is the only route in from this
 * screen -- the boot combo lives on screen_wifi.c and a launch that lands here
 * never got that far -- so keep it working before anything else in this file.
 */

#include <stdio.h>

#include "app.h"

static const ui_box kExitBtn = { 84.0f, 176.0f, 152.0f, 34.0f };

static void fatal_enter(app_ctx *ctx)
{
    (void)ctx;
}

static apad_screen_id fatal_update(app_ctx *ctx)
{
    int px = (int)ctx->touch.px, py = (int)ctx->touch.py;

    if (app_pressed(ctx, KEY_SELECT)) {
        ctx->selftest_return = APAD_SCREEN_FATAL;
        return APAD_SCREEN_SELFTEST;
    }
    if (app_pressed(ctx, KEY_B)
        || (ctx->touch_pressed && ui_box_hit(&kExitBtn, px, py))) {
        ctx->want_exit = 1;
        snprintf(ctx->exit_reason, sizeof ctx->exit_reason, "%s", ctx->fatal);
    }
    return APAD_SCREEN_FATAL;
}

static void fatal_draw_top(app_ctx *ctx)
{
    const float W = UI_TOP_W - 16.0f;
    float y = 60.0f;
    int n;

    ui_header(UI_TOP_W, ctx->is_dsi ? "AtticPad DSi" : "AtticPad DS",
              "STARTUP FAILED", ui_c_bad());

    /* THE ERROR LINE PLUS ONE HINT, wrapped -- these two, at UI_S_HEAD and
     * UI_S_SMALL on a 245px-wide (0.64-scaled) top screen, do not fit on one
     * 8x8-face line each (that was the reported bug: ui_textf_fit() cut both
     * mid-word). ui_textf_wrap()'s own return is what tells the hint where to
     * start, so a shorter/longer catalogue string in the future cannot
     * overlap the next line by construction. */
    n = ui_textf_wrap(UI_TOP_W * 0.5f, y, W, UI_S_HEAD, ui_c_bad(),
                      UI_ALIGN_CENTER, UI_LINE(UI_S_HEAD), "%s",
                      apad_ui_msg(APAD_MSG_NET_UNAVAILABLE));
    y += (float)n * UI_LINE(UI_S_HEAD) + 8.0f;

    n = ui_textf_wrap(UI_TOP_W * 0.5f, y, W, UI_S_SMALL, ui_c_text(),
                      UI_ALIGN_CENTER, UI_LINE(UI_S_SMALL), "%s",
                      apad_ui_msg(APAD_MSG_NET_UNAVAILABLE_HINT));
    y += (float)n * UI_LINE(UI_S_SMALL) + 14.0f;

    /* Developer detail, secondary, and now only behind the diagnostics
     * toggle -- docs/CONVENTIONS.md's "text only for actions, errors and permissions"
     * bar (2026-09-09 decluttering pass) means a raw internal string is not
     * something every user needs staring back at them under a failure they
     * cannot act on anyway. */
    if (ctx->show_diag) {
        ui_textf_wrap(UI_TOP_W * 0.5f, y, W, UI_S_TINY, ui_c_border(),
                     UI_ALIGN_CENTER, UI_LINE(UI_S_TINY), "details: %s",
                     ctx->fatal);
    }
}

static void fatal_draw_bottom(app_ctx *ctx)
{
    const float W = UI_BOT_W - 16.0f;

    (void)ctx;
    ui_header(UI_BOT_W, "startup failed", NULL, ui_c_dim());

    /* ONE hint, not the previous three lines: the top screen already carries
     * the error and its hint (see fatal_draw_top()), so this screen's own job
     * is just the one thing with no on-screen affordance here -- SELECT is a
     * hidden combo, not a button. EXIT is a real button below, so it gets no
     * hint of its own. */
    ui_textf_wrap(UI_BOT_W * 0.5f, 60.0f, W, UI_S_SMALL, ui_c_dim(),
                 UI_ALIGN_CENTER, UI_LINE(UI_S_SMALL),
                 "SELECT runs the built-in health check");

    ui_button(&kExitBtn, "EXIT", 0, 1);
}

const apad_screen apad_screen_fatal = {
    "fatal",
    fatal_enter,
    fatal_update,
    fatal_draw_top,
    fatal_draw_bottom
};
