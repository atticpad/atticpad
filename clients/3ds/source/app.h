/* clients/3ds/source/app.h
 *
 * The screen system, and the one context struct every screen reads and
 * writes.
 *
 * HOW A SCREEN WORKS. One `apad_screen` is a name plus four function
 * pointers; main.c holds the table indexed by `apad_screen_id` and runs
 * exactly one screen per frame:
 *
 *     hidScanInput() -> fill app_ctx's input fields -> cur->update(ctx)
 *     -> ui_frame_begin() -> cur->draw_top(ctx) -> cur->draw_bottom(ctx)
 *     -> ui_frame_end()
 *
 * `update` returns the id of the screen that should run NEXT. Returning its
 * own id means "stay". main.c calls the new screen's `enter` on a change and
 * disarms the input gate (see `keys_armed` below), so a screen never has to
 * think about either.
 *
 * ADDING A SCREEN is therefore: add an id to the enum below (before
 * APAD_SCREEN_COUNT), declare its `extern const apad_screen`, write one
 * screen_<name>.c implementing the four callbacks, and add one row to
 * main.c's table. The Makefile globs every .c under the source directory, so
 * there is no build file to touch either. Nothing else in the client changes
 * -- this shape exists precisely so the QR-scanner screen
 * (deferred QR-code pairing) would be one case and not
 * a refactor. It landed the same day and it was: screen_qrscan.c, one enum
 * value, one table row, two context fields and the connect screen's reserved
 * button slot. The only thing that needed more was the Makefile, because
 * quirc is vendored third-party source with its own warning rules.
 *
 * BLOCKING WORK inside `update` (discovery, the handshake, the self-test run)
 * stops the frame, so every screen that does it splits the work in two: one
 * update sets an "about to do X" substate and returns, the frame draws that
 * state, and the NEXT update does the blocking call. The user always sees
 * what the console is busy with before it goes busy. Search for _ARM in the
 * screen files.
 */
#ifndef ATTICPAD_3DS_APP_H
#define ATTICPAD_3DS_APP_H

#include <stdint.h>

#include <3ds.h>

#include "atticpad/atticpad.h"
#include "atticpad/version.h"
#include "apad_client.h"
#include "apad_ui.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* screens                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum {
    APAD_SCREEN_CONNECT = 0,   /* address entry, discovery, auto-reconnect  */
    APAD_SCREEN_SESSION,       /* live session: status + mapping hint       */
    APAD_SCREEN_QRSCAN,        /* camera viewfinder -> S10.3 pairing URI    */
    APAD_SCREEN_SELFTEST,      /* the hidden self-test (L+R+Start / SELECT) */
    APAD_SCREEN_FATAL,         /* bring-up failed; nothing else is possible */
    APAD_SCREEN_COUNT
} apad_screen_id;

typedef struct app_ctx app_ctx;

typedef struct {
    const char     *name;
    void            (*enter)(app_ctx *ctx);
    apad_screen_id  (*update)(app_ctx *ctx);
    void            (*draw_top)(app_ctx *ctx);
    void            (*draw_bottom)(app_ctx *ctx);
} apad_screen;

extern const apad_screen apad_screen_connect;
extern const apad_screen apad_screen_session;
extern const apad_screen apad_screen_qrscan;
extern const apad_screen apad_screen_selftest;
extern const apad_screen apad_screen_fatal;

/* ------------------------------------------------------------------------ */
/* context                                                                  */
/* ------------------------------------------------------------------------ */

#define APP_MSG_LEN 128

struct app_ctx {
    /* -- this frame's input, filled by main.c before update() ----------- */
    u32           keys_held;
    u32           keys_down;
    touchPosition touch;
    int           touch_held;     /* KEY_TOUCH is down right now            */
    int           touch_pressed;  /* ...and this is the frame it went down  */

    /* THE RELEASED-ONCE GATE. Set the first frame on which nothing at all is
     * held, cleared by main.c on every screen change. Every edge-triggered UI
     * action in this client checks it via app_pressed()/touch_pressed.
     *
     * Found on hardware, three times in a row: a key held across a screen
     * transition -- or across a gap with no hidScanInput() calls, which every
     * blocking network call is -- reads back as a FRESH hidKeysDown() press
     * on the next scan rather than as still-held. Holding L+R+Start for the
     * self-test combo therefore landed on the session screen with START
     * "newly pressed" and tore the session down the instant it connected. The
     * person at the console described it as "I keep exiting cause I press
     * start". Level-triggered checks (hidKeysHeld()) were never affected;
     * this gate is only for edges. */
    int           keys_armed;

    /* -- hardware, probed once at startup ------------------------------- */
    int      is_new3ds;
    int      have_cstick;
    int      have_gyro;
    float    gyro_coeff;      /* HIDUSER_GetGyroscopeRawToDpsCoefficient()  */
    uint32_t caps;            /* the APAD_CAP_* mask sent in HELLO          */
    unsigned soc_bufsize;
    int      last_apt_hook;   /* enum APT_HookType, or -1 for none seen     */

    /* -- live input snapshot, refilled by main.c every frame ------------ */
    /* This is the exact struct handed to apad_client_pump(), so the on-screen
     * readout and the wire cannot disagree. Sampled on every screen, not just
     * the session one, so the connect screen can prove the buttons work
     * before anyone blames the network. */
    apad_input_state st;
    angularRate      gyro_raw;   /* unconverted ticks, for the debug line   */

    /* Hidden diagnostics overlay (2026-08-12, "hide the debug surface" pass).
     * Off by default (zeroed with the rest of ctx in main()). SELECT toggles
     * it from the session screen -- WITHOUT touching st.buttons, which
     * main.c's app_sample_input() already filled before update() ran, so a
     * SELECT press still reaches the wire as an ordinary button for whatever
     * game is running. This flag only controls what THIS client draws: the
     * server/session panel, the live input readout and the gyro noise line
     * in app_draw_status_top()/app_draw_gyro_line(), and the raw STATUS/ERROR
     * code on the session screen. The big RTT figure is never gated on this
     * -- it is the one number docs/CONVENTIONS.md calls product, not debug. */
    int              show_diag;

    /* -- session -------------------------------------------------------- */
    apad_client       *client;
    apad_client_stats  stats;
    int                connected;  /* the engine last reported ACTIVE       */

    /* -- target --------------------------------------------------------- */
    /* Edited digit by digit on the connect screen's numpad and parsed with
     * apad_addr_parse() only when connecting, so a half-typed address is a
     * normal state rather than an error. */
    char ip_text[16];
    char port_text[6];
    int  from_announce;               /* target came from a tier-2 ANNOUNCE */
    char server_name[APAD_NAME_LEN + 1];

    /* A S10.1 secret for this target has been handed to the session engine.
     * The SECRET ITSELF IS NOT HERE and must never be: apad_client owns the
     * one copy (clients/common/apad_client.h -- it is copied in and never
     * written anywhere else), and docs/PROTOCOL.md S10 treats it as exactly
     * as sensitive as the PIN it stands in for. This flag says only "the
     * connect screen may send a HELLO to a server that says
     * pairing_required", which is the one decision that needs to know.
     * Set by the QR screen on a successful scan; cleared, along with the
     * engine's copy, the moment the address is edited by hand. */
    int  have_secret;

    /* -- cross-screen messages ------------------------------------------ */
    char banner[APP_MSG_LEN];   /* shown on the connect screen: why we came
                                 * back, or why connecting failed           */
    int  banner_level;          /* 0 info, 1 warning, 2 error               */
    int  reconnect_frames;      /* >0: the connect screen counts this down
                                 * and then connects to the same target     */
    int  want_discovery;        /* run tier-2 broadcast discovery on entry   */
    int  want_connect_now;      /* the connect screen connects on entry,
                                 * skipping discovery and the countdown --
                                 * set by the QR screen, which has just been
                                 * told the address by the server itself     */
    int  want_boot_combo;       /* the connect screen owns the L+R+Start
                                 * launch window on its first entry          */
    apad_screen_id selftest_return;  /* where the self-test screen goes back */
    int  want_exit;             /* a deliberate app exit was requested       */
    char fatal[APP_MSG_LEN];    /* APAD_SCREEN_FATAL's message               */
    char exit_reason[APP_MSG_LEN];
};

/* ------------------------------------------------------------------------ */
/* shared helpers (main.c)                                                  */
/* ------------------------------------------------------------------------ */

/* An edge-triggered key press that has passed the released-once gate. */
static inline int app_pressed(const app_ctx *ctx, u32 mask)
{
    return ctx->keys_armed && ((ctx->keys_down & mask) != 0u);
}

/* Re-arm the gate: nothing counts as pressed again until a frame is seen with
 * nothing held. main.c does this on every screen change; a screen must ALSO
 * do it after any blocking call it makes, because a key held across a gap
 * with no hidScanInput() in it (the handshake is up to four seconds of that)
 * reads back as a fresh press on the next scan. */
static inline void app_disarm(app_ctx *ctx)
{
    ctx->keys_armed = 0;
}

/* Sets the connect screen's banner. level: 0 info, 1 warning, 2 error. */
void app_note(app_ctx *ctx, int level, const char *fmt, ...);

/* Human text for an enum apad_session_close, for the banner. */
const char *app_close_reason_text(int reason);

/* Human text for an enum APT_HookType, for the exit/debug lines. */
const char *app_apt_hook_name(int hook);

/* A screen's display name, for prompts like "back to the connect screen".
 * Reads the same table main.c dispatches through, so it cannot drift. */
const char *app_screen_name(apad_screen_id id);

/* Tier-2 broadcast DISCOVER on a socket of its own (the engine's socket
 * belongs to the session). On success fills ctx->ip_text / ctx->port_text /
 * ctx->server_name and sets from_announce. Returns 1 on an ANNOUNCE, 0
 * otherwise. Blocks for up to ~500ms. */
int app_discover(app_ctx *ctx);

/* Display-only gyro statistics over a rolling ~0.5s window, in deci-degrees
 * per second. `axis` is 0 pitch / 1 roll / 2 yaw, matching st.gyro[]. The
 * values on the wire are never smoothed -- docs/PROTOCOL.md S5.3 gives the
 * server profile sole ownership of filtering. */
void app_gyro_stats(int axis, int32_t *out_mean, int16_t *out_noise);

/* Peak noise (half the peak-to-peak spread) across all three axes, the one
 * number the six-line bar diagnostic collapsed into. */
int16_t app_gyro_noise_peak(void);

/* Battery (ptm:u), sampled at ~1 Hz in app_sample_input() (main.c). -1 when
 * ptm:u never came up (bring_up() then also leaves APAD_CAP_BATTERY clear in
 * ctx->caps -- docs/PROTOCOL.md S5.5, absence is the capability bit); else
 * the exact value this frame's ctx->st.battery carries: 0..100, or 255
 * while ptm:u is up but no read has succeeded yet (S5.5's "unknown" --
 * callers drawing a percentage must check for it). Charging is
 * display-only: the wire has no bit for it. */
int app_battery_percent(void);
int app_battery_charging(void);

/* ------------------------------------------------------------------------ */
/* composite widgets (ui_widgets.c)                                         */
/* ------------------------------------------------------------------------ */

/* The whole top screen's status layout, shared by the connect and session
 * screens so that a reading learned on one is valid on the other: title bar,
 * the big RTT figure, the server/session panel, and the live input readout.
 * `live` selects the connected presentation (real RTT, session id, pad slot)
 * from the idle one. Leaves everything below y=UI_STATUS_BOTTOM free for the
 * calling screen. */
#define UI_STATUS_BOTTOM 198.0f

void app_draw_status_top(app_ctx *ctx, int live);

/* One line: mean rate per axis in deg/s plus the peak noise figure. Replaces
 * the six-line bar-graph diagnostic that existed to find the 200x
 * raw-to-dps bug -- that bug is fixed and recorded, and what is worth keeping
 * on screen forever is the noise floor, because it is the number that says
 * whether the server profile's gyro deadzone is sized anywhere near right. */
void app_draw_gyro_line(app_ctx *ctx, float x, float y);

/* ------------------------------------------------------------------------ */
/* the gyro cube (gyro_cube.c)                                              */
/* ------------------------------------------------------------------------ */

/* A flat, 3DS-proportioned box, rotated by hidGyroRead()-driven pitch/roll/
 * yaw (READ from ctx->st.gyro[], already sampled by main.c's
 * app_sample_input() -- this does not call hidGyroRead() a second time) and
 * drawn CPU-side with citro2d's plain triangle/line primitives. Product
 * feedback, not a diagnostic: no text is ever drawn by this call. `cx`/`cy`
 * are the screen-space centre to draw around; `scale` is the desired
 * on-screen pixel size of the box's HEIGHT at identity orientation (the
 * caller does not need to know the box's width:height:thickness ratio).
 * See gyro_cube.c's header comment for the axis/rotation-order convention
 * and what is unverified against real hardware. */
void gyro_cube_step_and_draw(app_ctx *ctx, float cx, float cy, float scale);

#endif /* ATTICPAD_3DS_APP_H */
