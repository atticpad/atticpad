/* clients/nds/source/screen_selftest.c
 *
 * clients/3ds/source/screen_selftest.c, ported. The hidden self-test screen
 * (docs/CONVENTIONS.md: "Every client ships the hidden self-test screen (hold
 * L+R+Start)"). Reachable two ways, both of which matter:
 *
 *   - L+R+Start during the launch window, the documented combo, sampled by
 *     screen_wifi.c (the first screen a launch lands on here) across many
 *     frames rather than on one scan;
 *   - SELECT from the network, connect or fatal screen. On the session screen
 *     SELECT is an ordinary wire button so games get it back, and self-test is
 *     a touch button behind a confirmation there instead.
 *
 * SELECT is not a convenience. On the sibling 3DS the L shoulder button of the
 * development console is physically dead, so the documented combo cannot be
 * triggered there at all; any change that removes the SELECT trigger removes
 * the only working route on the one console this project can test on.
 *
 * ===========================================================================
 * THE ONE REAL DIFFERENCE FROM THE 3DS: THIS TAKES ABOUT FIFTY-ONE SECONDS
 * ===========================================================================
 * apad_selftest_run() is 1987 cases and it is PBKDF2-bound; this ARM9 does
 * roughly 8-10k iterations a second (docs/PROTOCOL.md S10 sets the count at
 * 10,000 precisely because of that figure), against a fraction of a second on
 * a 3DS. The 3DS version therefore draws one "working ..." frame and blocks;
 * doing that here would leave a motionless console for the best part of a
 * minute, which on a platform nobody can attach a debugger to is
 * indistinguishable from a crash.
 *
 * So this version RUNS THE SUITE IN SLICES. apad_selftest_run() has no
 * resume point and cannot be cut in half, so the progress comes from the
 * callback instead: it counts every case as it completes and, every
 * PROGRESS_EVERY cases, DRAWS A FRAME FROM INSIDE THE CALLBACK. That is not
 * as ugly as it sounds and it is the only shape available --
 *
 *   - the callback is called on this same thread, synchronously, between
 *     cases, so there is no re-entrancy: apad_selftest_run() is not on the
 *     stack twice and nothing in ui.c is mid-draw when it is entered;
 *   - drawing a frame includes ui_frame_begin()'s cothread_yield_irq(
 *     IRQ_VBLANK), which is also what keeps DSWiFi's lwIP cothread alive
 *     across the minute. Without it the session on the other side would not
 *     merely idle out (it does that deliberately -- the session screen sends
 *     a BYE first), the WHOLE STACK would sit unserviced;
 *   - the count is real progress, not a spinner: it comes from the suite
 *     itself, so a run that stalls stops advancing and says so.
 *
 * `name` is only valid for the callback's duration (atticpad.h), so the group
 * name is copied out of it for the progress line rather than kept as a
 * pointer.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"

enum { ST_ARM = 0, ST_RUN, ST_DONE };
static int s_sub;

static apad_selftest_result s_result;
static int s_rc;
static int s_cases;
static char s_last_fail[96];

/* The context, kept for draw_progress_frame(): it is called from inside
 * apad_selftest_run()'s callback, which has no argument of its own to carry
 * one. Set in selftest_enter(), which always runs first. */
static app_ctx *s_ctx;

/* Every 24 cases: about 40 progress frames over a 1987-case run, which is
 * often enough that the number visibly moves and rare enough that the drawing
 * itself is not what the minute is spent on. */
#define PROGRESS_EVERY 24

/* Roughly what a whole run costs, for the "about a minute" estimate. Not
 * measured live -- the point of showing it is to set an expectation before
 * the first case, when there is nothing measured yet. */
#define SELFTEST_TOTAL_ESTIMATE 1987

static const ui_box kContinueBtn = { 84.0f, 186.0f, 152.0f, 34.0f };

static void draw_progress_frame(void);

static void selftest_cb(void *user, const char *name, int passed)
{
    (void)user;
    s_cases++;
    if (!passed) {
        /* `name` is only valid for the callback's duration -- copy it. */
        snprintf(s_last_fail, sizeof s_last_fail, "%s", name);
    }
    if ((s_cases % PROGRESS_EVERY) == 0) {
        draw_progress_frame();
    }
}

static void selftest_enter(app_ctx *ctx)
{
    s_ctx = ctx;
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
        /* The run has no scanKeys() in it, so whatever was held when it
         * started -- L+R+Start, most obviously -- would read as a fresh press
         * and dismiss the results page before anyone saw it. That exact
         * sequence is why this gate exists, and a minute of it makes the
         * chance of something being held far higher than on the 3DS. */
        app_disarm(ctx);
        return APAD_SCREEN_SELFTEST;

    default:
        if (app_pressed(ctx, 0xFFFFFFFFu) || ctx->touch_pressed) {
            return ctx->selftest_return;
        }
        return APAD_SCREEN_SELFTEST;
    }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void draw_top_body(void)
{
    const float W = UI_TOP_W - 16.0f;
    int done = (s_sub == ST_DONE);
    int pass = done && (s_rc == APAD_OK);

    ui_header(UI_TOP_W, "AtticPad -- self-test  v" APAD_VERSION_STR,
              done ? apad_ui_msg(apad_ui_selftest_title(s_result.failed))
                   : apad_ui_msg(APAD_MSG_SELFTEST_RUNNING),
              done ? (pass ? ui_c_good() : ui_c_bad()) : ui_c_warn());

    if (!done) {
        int pct = (s_cases * 100) / SELFTEST_TOTAL_ESTIMATE;

        if (pct > 99) {
            pct = 99;   /* the estimate is not the authority; the count is */
        }
        ui_textf_fit(UI_TOP_W * 0.5f, 40.0f, UI_S_HEAD, ui_c_text(),
                     UI_ALIGN_CENTER, W, "%s",
                     apad_ui_msg(APAD_MSG_SELFTEST_RUNNING));

        /* A real bar driven by a real count, not a spinner: a run that stops
         * advancing looks stopped, which is the whole point. */
        {
            const float bx = 40.0f, by = 84.0f, bw = UI_TOP_W - 80.0f, bh = 18.0f;
            float fill = bw * (float)pct / 100.0f;

            ui_rect(bx, by, bw, bh, ui_c_panel());
            if (fill > 0.0f) {
                ui_rect(bx, by, fill, bh, ui_c_accent());
            }
            ui_outline(bx, by, bw, bh, 1.0f, ui_c_border());
        }
        ui_textf(UI_TOP_W * 0.5f, 112.0f, UI_S_BODY, ui_c_text(),
                 UI_ALIGN_CENTER, "%d of about %d checks", s_cases,
                 SELFTEST_TOTAL_ESTIMATE);
        /* No per-case name line here any more, and the status line is
         * shorter (2026-09-09 decluttering pass): which vector is running
         * right now is diagnostic noise, not something a person waiting on
         * a progress bar needs read out to them. */
        ui_textf_fit(UI_TOP_W * 0.5f, 138.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, W, "About a minute on a DS");
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

    /* No trailing "the golden packets are authored apart from the codec..."
     * trivia any more (2026-09-09 decluttering pass): explanatory, not an
     * action, error or permission -- docs/CONVENTIONS.md's minimal-text bar. */
}

static void draw_bottom_body(const char *dest, int is_dsi)
{
    const float W = UI_BOT_W - 20.0f;
    int done = (s_sub == ST_DONE);

    ui_header(UI_BOT_W, "self-test", NULL, ui_c_dim());

    (void)dest;   /* the CONTINUE button (below) already says where this
                   * screen goes; nothing left here needs to name it again */

    ui_textf_fit(10.0f, 30.0f, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT, W, "%s",
                 apad_ui_msg(APAD_MSG_SELFTEST_SUBTITLE));

    /* No explanatory paragraph any more (2026-09-09 decluttering pass
     * removed "Checks how AtticPad encodes and decodes..." / "Nothing here
     * is platform-specific..."): title, progress, and the result are the
     * whole screen now -- docs/CONVENTIONS.md's "text only for actions, errors and
     * permissions" bar. */

    /* The one line of per-console engineering data this client has, in the
     * place docs/CONVENTIONS.md already keeps ABI/version info. */
    ui_textf_fit(10.0f, 50.0f, UI_S_TINY, ui_c_border(), UI_ALIGN_LEFT, W,
                 "libapad v" APAD_VERSION_STR "   proto v%u   %s mode",
                 (unsigned)APAD_VERSION, is_dsi ? "DSi" : "DS");

    if (done) {
        /* CONTINUE is the only way back and it is right here -- no separate
         * "any button, or touch -- back to the %s screen" hint any more. */
        ui_button(&kContinueBtn, "CONTINUE", 0, 1);
    } else {
        ui_textf_fit(UI_BOT_W * 0.5f, 200.0f, UI_S_BODY, ui_c_warn(),
                     UI_ALIGN_CENTER, W, "working ... %d", s_cases);
    }
}

/* Called FROM INSIDE apad_selftest_run()'s callback -- see this file's header
 * for why that is safe and why it is the only shape available. It draws the
 * same two halves the ordinary path draws, so a mid-run frame and an
 * end-of-run frame cannot disagree about the layout. */
static void draw_progress_frame(void)
{
    ui_frame_begin();
    ui_screen_top();
    draw_top_body();
    ui_screen_bottom();
    draw_bottom_body(app_screen_name(s_ctx->selftest_return), s_ctx->is_dsi);
    ui_frame_end();
}

static void selftest_draw_top(app_ctx *ctx)
{
    (void)ctx;
    draw_top_body();
}

static void selftest_draw_bottom(app_ctx *ctx)
{
    draw_bottom_body(app_screen_name(ctx->selftest_return), ctx->is_dsi);
}

const apad_screen apad_screen_selftest = {
    "selftest",
    selftest_enter,
    selftest_update,
    selftest_draw_top,
    selftest_draw_bottom
};
