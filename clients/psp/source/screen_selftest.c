/*
 * clients/psp/source/screen_selftest.c -- the hidden conformance self-test.
 *
 * Mirrors clients/3ds/source/screen_selftest.c, including its three-frame
 * shape. apad_selftest_run() blocks for about a second and has no yield
 * points, so running it in the same frame that entered the screen would show
 * a blank display for that second and look like a hang. Instead: one frame to
 * arm and draw "running", the next to actually run it, then the result.
 *
 * Every client ships this screen -- it is not optional (docs/CONVENTIONS.md). It is what
 * lets someone answer "is the protocol code intact on this device?" with no
 * server, no network, and no PC.
 */
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>

#include "atticpad/version.h"

#include "app.h"
#include "ui.h"

typedef enum { ST_ARM = 0, ST_RUN, ST_DONE } st_sub;

static st_sub                 s_sub;
static apad_selftest_result   s_result;
static int                    s_rc;
static char                   s_first_fail[64];

static void on_case(void *user, const char *name, int passed)
{
    (void)user;
    if (!passed && s_first_fail[0] == '\0' && name != NULL) {
        /* `name` is only valid for the duration of the callback. */
        size_t n = strlen(name);
        if (n > sizeof s_first_fail - 1) { n = sizeof s_first_fail - 1; }
        memcpy(s_first_fail, name, n);
        s_first_fail[n] = '\0';
    }
}

static void selftest_enter(app_ctx *ctx)
{
    (void)ctx;
    s_sub = ST_ARM;
    s_rc  = 0;
    s_first_fail[0] = '\0';
    memset(&s_result, 0, sizeof s_result);
}

static apad_screen_id selftest_update(app_ctx *ctx)
{
    switch (s_sub) {
    case ST_ARM:
        s_sub = ST_RUN;                 /* draw "running" first */
        return APAD_SCREEN_SELFTEST;

    case ST_RUN:
        s_rc = apad_selftest_run(&s_result, on_case, NULL);
        s_sub = ST_DONE;
        /* The combo that got here was held across a blocking second. */
        app_disarm(ctx);
        return APAD_SCREEN_SELFTEST;

    default:
        if (app_pressed(ctx, 0xFFFFFFFFu)) {
            return ctx->selftest_return;
        }
        return APAD_SCREEN_SELFTEST;
    }
}

static void selftest_draw(app_ctx *ctx)
{
    ui_box b;
    int    pass;

    ui_header("AtticPad -- self-test", "", ui_c_dim());

    b.x = 6; b.y = 24; b.w = UI_W - 12; b.h = 120;
    ui_panel(&b, ui_c_panel(), ui_c_border());

    if (s_sub != ST_DONE) {
        ui_text(b.x + 12, b.y + 40, ui_c_accent(), UI_ALIGN_LEFT,
                "running conformance vectors...");
        ui_text(b.x + 12, b.y + 56, ui_c_dim(), UI_ALIGN_LEFT,
                "this takes about a second and does not use the network");
        return;
    }

    pass = (s_rc == APAD_OK && s_result.failed == 0);
    ui_text(b.x + 12, b.y + 10, ui_c_dim(), UI_ALIGN_LEFT, "RESULT");
    ui_text(b.x + 12, b.y + 26, pass ? ui_c_good() : ui_c_bad(), UI_ALIGN_LEFT,
            pass ? "PASSED" : "FAILED");

    ui_textf(b.x + 12, b.y + 48, ui_c_text(), UI_ALIGN_LEFT,
             "%u / %u passed, %u failed",
             (unsigned)s_result.passed, (unsigned)s_result.total,
             (unsigned)s_result.failed);

    if (s_first_fail[0] != '\0') {
        ui_textf_fit(b.x + 12, b.y + 64, ui_c_bad(), UI_ALIGN_LEFT, b.w - 24,
                     "first failure: %s", s_first_fail);
    }

    /* What this run could NOT check, said on the screen rather than assumed.
     * The suite covers the protocol; it says nothing about this console's
     * radio, its nub, or its battery. */
    ui_textf(b.x + 12, b.y + 84, ui_c_dim(), UI_ALIGN_LEFT,
             "libapad %s   wire v%u   caps 0x%04X",
             APAD_VERSION_STR, (unsigned)APAD_VERSION, (unsigned)ctx->caps);
    ui_text(b.x + 12, b.y + 96, ui_c_dim(), UI_ALIGN_LEFT,
            "covers the protocol only -- not the radio, nub or battery");

    ui_rect(0, UI_H - 14, UI_W, 1, ui_c_border());
    ui_text(6, UI_H - 11, ui_c_dim(), UI_ALIGN_LEFT, "any button: back");
}

const apad_screen apad_screen_selftest = {
    "selftest", selftest_enter, selftest_update, selftest_draw
};
