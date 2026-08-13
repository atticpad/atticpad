/* clients/3ds/source/screen_selftest.c
 *
 * The hidden self-test screen (docs/CONVENTIONS.md: "Every client ships the hidden
 * self-test screen (hold L+R+Start)"). Reachable two ways, both of which
 * matter:
 *
 *   - L+R+Start during the launch window, the documented combo;
 *   - SELECT from the connect, qrscan or fatal screen. The session screen is
 *     the one exception: since 2026-08-12 (see screen_session.c) SELECT is an
 *     ordinary wire button there so games get it back, and self-test is a
 *     touch button (with a confirm prompt, since running it drops the
 *     session) in that screen's footer instead.
 *
 * SELECT is not a convenience. The L shoulder button on the development
 * console is physically dead -- proven by differential capture, BTN_TR 10
 * events / BTN_TL 0 -- so the documented combo CANNOT be triggered there at
 * all. Any future change that removes the SELECT trigger removes the only
 * working route on the one console this project can test on.
 *
 * Until 2026-08-12 there was a third route, an on-screen SELF-TEST button on
 * the connect screen (and, briefly, the fatal screen). It was removed as
 * part of hiding this client's debug surface by default -- docs/CONVENTIONS.md's
 * convention was always a HIDDEN self-test, and a labelled button on the
 * first screen every user sees was never that. SELECT alone is still the
 * ONLY reliable entry on this hardware; nothing about removing the button
 * touched it.
 *
 * This screen also carries the per-console engineering detail that used to
 * sit unconditionally on the connect screen's top half: soc:U buffer size,
 * the measured gyro raw-to-dps coefficient, and the last APT lifecycle hook
 * seen. It is ABI/calibration data, not something every user needs on the
 * first screen they land on, and this is where docs/CONVENTIONS.md's "keep the
 * self-test screen's ABI/version info" already put version strings.
 *
 * The screen stays text-shaped, because 990 conformance checks summarised in
 * four numbers is text. It draws that text through ui_textf() like every
 * other screen: consoleInit() and citro2d fight over the same framebuffer, so
 * this codebase picks one world and stays in it (see ui.h).
 */

#include <stdio.h>
#include <string.h>

#include "app.h"

enum { ST_ARM = 0, ST_RUN, ST_DONE };
static int s_sub;

/* apad_selftest_run() is one blocking call with no place to yield, so the
 * "running" frame has to be drawn BEFORE it starts (ST_ARM) -- see app.h on
 * blocking work. It completes in well under a second on this hardware, so
 * there is nothing to make interruptible; the split exists so the screen is
 * never blank while it works. */

static apad_selftest_result s_result;
static int s_rc;
static int s_cases;
static char s_last_fail[96];

static const ui_box kContinueBtn = { 84.0f, 186.0f, 152.0f, 34.0f };

static void selftest_cb(void *user, const char *name, int passed)
{
    (void)user;
    s_cases++;
    if (!passed) {
        /* `name` is only valid for the callback's duration (atticpad.h) --
         * copy it. */
        strncpy(s_last_fail, name, sizeof s_last_fail - 1);
        s_last_fail[sizeof s_last_fail - 1] = '\0';
    }
}

static void selftest_enter(app_ctx *ctx)
{
    (void)ctx;
    s_sub = ST_ARM;
    memset(&s_result, 0, sizeof s_result);
    s_rc = 0;
    s_cases = 0;
    s_last_fail[0] = '\0';
}

static apad_screen_id selftest_update(app_ctx *ctx)
{
    switch (s_sub) {
        case ST_ARM:
            s_sub = ST_RUN;
            return APAD_SCREEN_SELFTEST;

        case ST_RUN:
            s_rc = apad_selftest_run(&s_result, selftest_cb, NULL);
            s_sub = ST_DONE;
            /* The run has no hidScanInput() in it, so whatever was held when
             * it started -- L+R+Start, most obviously -- would read as a
             * fresh press and dismiss the results page before anyone saw it.
             * That exact sequence is why this gate exists. */
            app_disarm(ctx);
            return APAD_SCREEN_SELFTEST;

        default:
            if (app_pressed(ctx, 0xFFFFFFFFu) || ctx->touch_pressed) {
                return ctx->selftest_return;
            }
            return APAD_SCREEN_SELFTEST;
    }
}

static void selftest_draw_top(app_ctx *ctx)
{
    const float W = UI_TOP_W - 16.0f;
    int done = (s_sub == ST_DONE);
    int pass = done && (s_rc == APAD_OK);

    (void)ctx;
    ui_header(UI_TOP_W, "AtticPad -- self-test  v" APAD_VERSION_STR,
              done ? apad_ui_msg(apad_ui_selftest_title(s_result.failed))
                   : apad_ui_msg(APAD_MSG_SELFTEST_RUNNING),
              done ? (pass ? ui_c_good() : ui_c_bad()) : ui_c_warn());

    if (!done) {
        ui_textf_fit(UI_TOP_W * 0.5f, 96.0f, UI_S_HEAD, ui_c_text(),
                     UI_ALIGN_CENTER, W, "%s",
                     apad_ui_msg(APAD_MSG_SELFTEST_RUNNING));
        ui_textf_fit(UI_TOP_W * 0.5f, 124.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, W, "%s",
                     apad_ui_msg(APAD_MSG_SELFTEST_SUBTITLE));
        return;
    }

    ui_textf_fit(UI_TOP_W * 0.5f, 40.0f, UI_S_HUGE,
                 pass ? ui_c_good() : ui_c_bad(), UI_ALIGN_CENTER, W, "%s",
                 apad_ui_msg(apad_ui_selftest_title(s_result.failed)));

    ui_textf(UI_TOP_W * 0.5f, 100.0f, UI_S_HEAD, ui_c_text(), UI_ALIGN_CENTER,
             "%u / %u passed", (unsigned)s_result.passed,
             (unsigned)s_result.total);
    ui_textf_fit(UI_TOP_W * 0.5f, 126.0f, UI_S_SMALL,
                 s_result.failed ? ui_c_bad() : ui_c_dim(), UI_ALIGN_CENTER,
                 W, "%u failed   (%d cases seen)",
                 (unsigned)s_result.failed, s_cases);

    /* Case names are composed from core/testdata's vector names and can be
     * long; ui_textf_fit() is what keeps them on the screen. */
    if (s_result.first_failure != NULL) {
        ui_textf_fit(UI_TOP_W * 0.5f, 152.0f, UI_S_SMALL, ui_c_bad(),
                     UI_ALIGN_CENTER, W, "first failure: %s",
                     s_result.first_failure);
    }
    if (s_last_fail[0] != '\0') {
        ui_textf_fit(UI_TOP_W * 0.5f, 170.0f, UI_S_SMALL, ui_c_bad(),
                     UI_ALIGN_CENTER, W, "last failure:  %s", s_last_fail);
    }

    ui_textf_fit(UI_TOP_W * 0.5f, 206.0f, UI_S_TINY, ui_c_dim(),
                 UI_ALIGN_CENTER, W,
                 "the golden packets are authored independently of the codec");
    ui_textf_fit(UI_TOP_W * 0.5f, 220.0f, UI_S_TINY, ui_c_dim(),
                 UI_ALIGN_CENTER, W,
                 "-- both agreeing is the signal, and it needs no build flag");
}

static void selftest_draw_bottom(app_ctx *ctx)
{
    const float W = UI_BOT_W - 20.0f;
    int done = (s_sub == ST_DONE);
    const char *dest = app_screen_name(ctx->selftest_return);

    ui_header(UI_BOT_W, "self-test", NULL, ui_c_dim());

    ui_textf_fit(10.0f, 34.0f, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, W, "%s",
                 apad_ui_msg(APAD_MSG_SELFTEST_SUBTITLE));
    ui_textf_fit(10.0f, 52.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT, W,
                 "Checks how AtticPad encodes and decodes its network");
    ui_textf_fit(10.0f, 65.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT, W,
                 "messages, handles sequence numbers, and verifies its");
    ui_textf_fit(10.0f, 78.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT, W,
                 "built-in test data.");

    ui_textf_fit(10.0f, 104.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT, W,
                 "It exercises nothing platform-specific: a PASS here");
    ui_textf_fit(10.0f, 117.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT, W,
                 "says the shared AtticPad code is intact on this console,");
    ui_textf_fit(10.0f, 130.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT, W,
                 "not that the network, gyro or touchscreen work.");

    /* Relocated from the connect screen's top half (2026-08-12, "hide the
     * debug surface" pass): per-console calibration/engineering data, at
     * home on the one screen that already keeps ABI/version info. */
    ui_textf_fit(10.0f, 148.0f, UI_S_TINY, ui_c_border(), UI_ALIGN_LEFT, W,
                 "soc %u B (0x1000-aligned)   gyro coeff %.6f   apt hook %s",
                 ctx->soc_bufsize, (double)ctx->gyro_coeff,
                 app_apt_hook_name(ctx->last_apt_hook));

    if (done) {
        ui_button(&kContinueBtn, "CONTINUE", 0, 1);
        ui_textf_fit(UI_BOT_W * 0.5f, 224.0f, UI_S_TINY, ui_c_dim(),
                     UI_ALIGN_CENTER, W,
                     "any button, or touch -- back to the %s screen", dest);
    } else {
        ui_textf_fit(UI_BOT_W * 0.5f, 200.0f, UI_S_BODY, ui_c_warn(),
                     UI_ALIGN_CENTER, W, "working ...");
    }
}

const apad_screen apad_screen_selftest = {
    "selftest",
    selftest_enter,
    selftest_update,
    selftest_draw_top,
    selftest_draw_bottom
};
