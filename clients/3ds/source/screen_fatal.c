/* clients/3ds/source/screen_fatal.c
 *
 * Bring-up failed and there is nothing the client can usefully do: soc:U did
 * not come up, shim/net_bsd.c's apad_net_init() refused, or the session
 * engine could not allocate a socket. Every one of those used to print a line
 * and wait for B on a console that was about to vanish.
 *
 * It is a screen like any other, for one reason: it must still be able to run
 * the self-test. A console that cannot open a socket is exactly the case
 * where "is libapad itself intact on this hardware?" is the next question,
 * and the self-test needs no network at all to answer it.
 *
 * SELF-TEST HAS NO ON-SCREEN BUTTON HERE (2026-08-12, "hide the debug
 * surface" pass). This is the one screen main.c's L+R+Start boot window
 * never runs on (bring_up() failed before APAD_SCREEN_CONNECT was ever
 * entered), so SELECT is the ONLY route into self-test from here -- keep it
 * working before anything else on this file.
 */

#include <stdio.h>

#include "app.h"

static const ui_box kExitBtn     = {  84.0f, 176.0f, 152.0f, 34.0f };

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
    /* No version string here (2026-08-12 pass) -- see ui_widgets.c's
     * app_draw_status_top() for the same call. The self-test screen still
     * carries it. */
    ui_header(UI_TOP_W, "AtticPad 3DS", "STARTUP FAILED", ui_c_bad());

    const float W = UI_TOP_W - 16.0f;

    /* Error line plus ONE hint sentence (copy declutter pass, item 8) --
     * "Another app may be using the network - close it and relaunch" is
     * dropped as a duplicate of the catalog's own APAD_MSG_NET_UNAVAILABLE_
     * HINT, which says the same thing and is the one that stays. */
    ui_textf_fit(UI_TOP_W * 0.5f, 60.0f, UI_S_HEAD, ui_c_bad(),
                 UI_ALIGN_CENTER, W, "%s",
                 apad_ui_msg(APAD_MSG_NET_UNAVAILABLE));
    ui_textf_fit(UI_TOP_W * 0.5f, 96.0f, UI_S_SMALL, ui_c_text(),
                 UI_ALIGN_CENTER, W, "%s",
                 apad_ui_msg(APAD_MSG_NET_UNAVAILABLE_HINT));

    /* Developer detail, secondary: this client has no log path (3DS has no
     * stdio/filesystem story worth building for this), so what would go to a
     * log on another platform is rendered here instead -- small, dim, and
     * below everything a customer needs in order to act. */
    ui_textf_fit(UI_TOP_W * 0.5f, 168.0f, UI_S_TINY, ui_c_border(),
                 UI_ALIGN_CENTER, W, "details: %s", ctx->fatal);
}

static void fatal_draw_bottom(app_ctx *ctx)
{
    const float W = UI_BOT_W - 16.0f;

    (void)ctx;
    ui_header(UI_BOT_W, "startup failed", NULL, ui_c_dim());

    /* "The network is not usable this launch." removed -- it repeats the top
     * screen's error line, and the two-line self-test explanation collapsed
     * to one sentence (copy declutter pass, item 8). This sentence is the
     * exception the pass allows: this screen has no on-screen button for
     * self-test (SELECT is the only route -- see this file's header
     * comment), so unlike the removed hints elsewhere it isn't pointing at
     * something already visible. "B: exit" is dropped since EXIT is right
     * there (item 1). */
    ui_textf_fit(UI_BOT_W * 0.5f, 60.0f, UI_S_TINY, ui_c_dim(),
                 UI_ALIGN_CENTER, W,
                 "The self-test needs no network -- run it to check libapad");

    ui_button(&kExitBtn, "EXIT", 0, 1);
}

const apad_screen apad_screen_fatal = {
    "fatal",
    fatal_enter,
    fatal_update,
    fatal_draw_top,
    fatal_draw_bottom
};
