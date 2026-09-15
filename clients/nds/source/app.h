/* clients/nds/source/app.h
 *
 * The screen system and the one context struct every screen reads and writes
 * -- clients/3ds/source/app.h, ported. Read that file first; this one is
 * deliberately the same shape, and the differences are the platform's:
 *
 *   REMOVED, because the hardware has none of it: the gyroscope (have_gyro,
 *   gyro_coeff, gyro_raw, app_gyro_stats/app_gyro_noise_peak, the gyro cube),
 *   the C-stick / New-3DS split (is_new3ds, have_cstick), the camera and the
 *   QR screen, the APT lifecycle hooks (last_apt_hook, app_apt_hook_name --
 *   the DS has no applet manager to be suspended by), and soc_bufsize.
 *
 *   ADDED, because the hardware needs it: APAD_SCREEN_WIFI, which runs before
 *   the connect screen. The 3DS's network is up before main() gets control;
 *   here associating with an access point is a user-visible, failable, several
 *   second operation with a scan list and a key to type, so it is a screen
 *   like any other rather than a spinner.
 *
 *   ADDED: the PIN. The 3DS answers S10 pairing by scanning a QR code (it has
 *   a camera; this console does not) and screen_connect.c there simply refuses
 *   a server that wants pairing when no scanned key is held. The DS has a real
 *   keyboard instead (kbd_nds.h), so its connect screen prompts for the PIN
 *   and hands it to apad_client_set_secret(). ctx->have_secret means the same
 *   thing on both, and the SECRET ITSELF IS STILL NOT HERE -- the engine owns
 *   the only copy, exactly as the 3DS's comment says.
 *
 * HOW A SCREEN WORKS is unchanged: one apad_screen is a name plus four
 * function pointers, main.c runs exactly one per frame,
 *
 *     scanKeys() -> fill app_ctx's input fields -> cur->update(ctx)
 *     -> ui_frame_begin() -> cur->draw_top(ctx) -> cur->draw_bottom(ctx)
 *     -> ui_frame_end()
 *
 * and `update` returns the id of the screen that should run next.
 *
 * BLOCKING WORK inside update() splits in two the same way (an _ARM substate
 * that only draws, then the state that blocks), and for a sharper reason than
 * on the 3DS: apad_selftest_run() takes about FIFTY-ONE SECONDS on this CPU
 * (PBKDF2 at ~8-10k iterations/s -- docs/PROTOCOL.md S10), against well under
 * a second there. A screen that goes quiet for that long without a progress
 * indicator is indistinguishable from a crash, which is why
 * screen_selftest.c's callback counts cases and the screen draws the count.
 */
#ifndef ATTICPAD_NDS_APP_H
#define ATTICPAD_NDS_APP_H

#include <stdint.h>

#include <nds.h>

#include "atticpad/atticpad.h"
#include "atticpad/version.h"
#include "apad_client.h"
#include "apad_ui.h"
#include "ui.h"

/* ------------------------------------------------------------------------ */
/* screens                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum {
    APAD_SCREEN_WIFI = 0,      /* associate: WFC slots, then a scan list     */
    APAD_SCREEN_CONNECT,       /* address entry, discovery, auto-reconnect   */
    APAD_SCREEN_QRSCAN,        /* DSi only: S10.3 QR pairing via the camera  */
    APAD_SCREEN_SESSION,       /* live session: status + the four KBM modes  */
    APAD_SCREEN_SELFTEST,      /* the hidden self-test (L+R+Start / SELECT)  */
    APAD_SCREEN_FATAL,         /* bring-up failed; nothing else is possible  */
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

/* TRIED AND REVERTED (frame-rate pass, 2026-09-09): a second per-frame call
 * to apad_client_pump_ex() -- placed after ui_frame_begin()'s VBlank wait, to
 * give the 60Hz INPUT_STATE deadline a second chance to be caught inside one
 * drawn frame instead of only once at update() -- looked safe from this
 * file's side (apad_client.c's own elapsed-time gate stops INPUT_STATE from
 * being sent twice) but is NOT safe from clients/common/apad_client.c's
 * kbm_pump() side: kbm_pump() re-ingests kbm->keyboard/mouse/media's EDGE
 * EVENT RINGS on every call it is given a non-NULL kbm, with no time gate at
 * all (unlike INPUT_STATE) -- so resending the same cached snapshot
 * DOUBLE-INGESTS whatever key/button/media edge happened that frame. A/B
 * tested against the same build without this change, on the same box, same
 * server, same profile: WITHOUT it the session ran >40s with zero drops;
 * WITH it the server logged a fresh idle-timeout roughly every 3 seconds,
 * repeating. Root-caused by reading kbm_pump() (clients/common/apad_client.c),
 * not guessed. A correct version of this idea exists -- clear the cached
 * snapshot's event rings (but not its level-triggered fields: held keys,
 * held buttons, the mouse motion accumulators) before the second call, so
 * only the safe-to-repeat part of the snapshot is resent -- but it needs its
 * own build-and-run verification cycle this pass did not have time left for,
 * so it is NOT implemented. Left here as a memo for whoever tries this next,
 * rather than silently absent. */

extern const apad_screen apad_screen_wifi;
extern const apad_screen apad_screen_connect;
extern const apad_screen apad_screen_qrscan;
extern const apad_screen apad_screen_session;
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
    int           lid_closed;     /* KEY_LID: everything reads as released  */

    /* THE RELEASED-ONCE GATE, verbatim from the 3DS including the reason.
     * Set on the first frame with nothing held, cleared by main.c on every
     * screen change. Every edge-triggered UI action checks it through
     * app_pressed()/touch_pressed.
     *
     * Found on 3DS hardware three times over: a key held across a screen
     * transition -- or across a gap with no scanKeys() in it, which every
     * blocking network call is -- reads back as a FRESH press on the next
     * scan rather than as still-held. Holding L+R+Start for the self-test
     * therefore landed on the next screen with START "newly pressed". The
     * gap here is far worse than there (a 51 s self-test), so this gate is
     * more load-bearing on this console, not less. */
    int           keys_armed;

    /* -- hardware, probed once at startup ------------------------------- */
    int      is_dsi;          /* isDSiMode(): WPA2 possible, real battery   */
    uint32_t caps;            /* the APAD_CAP_* mask sent in HELLO          */

    /* -- live input snapshot, refilled by main.c every frame ------------ */
    /* The exact struct handed to apad_client_pump_ex(), so the on-screen
     * readout and the wire cannot disagree. Sampled on every screen. */
    apad_input_state st;

    /* Hidden diagnostics overlay, off by default; SELECT toggles it from the
     * session screen WITHOUT touching st.buttons (main.c already filled
     * those), so a SELECT press still reaches the wire as an ordinary
     * button. */
    int              show_diag;

    /* -- session -------------------------------------------------------- */
    apad_client       *client;
    apad_client_stats  stats;
    int                connected;

    /* -- target --------------------------------------------------------- */
    char ip_text[16];
    char port_text[6];
    int  from_announce;
    char server_name[APAD_NAME_LEN + 1];

    /* A S10.1 secret for this target has been handed to the session engine.
     * THE SECRET ITSELF IS NOT HERE and must never be: apad_client owns the
     * one copy. This flag says only "the connect screen may send a HELLO to a
     * server that says pairing_required". Set when a PIN is accepted on the
     * connect screen's keypad; cleared, along with the engine's copy, the
     * moment the address is edited by hand. */
    int  have_secret;

    /* -- wi-fi ---------------------------------------------------------- */
    /* The SSID this launch actually associated with, for the header line and
     * for config_nds.c. NOT the key: like the pairing secret, a network key
     * never reaches a file or a screen once it has been used. */
    char ssid[33];
    int  wifi_ready;
    int  force_ds_mode;        /* L held at boot: use the DS-mode radio     */

    /* -- cross-screen messages ------------------------------------------ */
    char banner[APP_MSG_LEN];
    int  banner_level;          /* 0 info, 1 warning, 2 error               */
    int  reconnect_frames;
    int  want_discovery;
    int  want_connect_now;
    int  want_boot_combo;       /* the wifi screen owns the L+R+Start window */
    apad_screen_id selftest_return;
    int  want_exit;
    char fatal[APP_MSG_LEN];
    char exit_reason[APP_MSG_LEN];
};

/* ------------------------------------------------------------------------ */
/* shared helpers (main.c)                                                  */
/* ------------------------------------------------------------------------ */

static inline int app_pressed(const app_ctx *ctx, u32 mask)
{
    return ctx->keys_armed && ((ctx->keys_down & mask) != 0u);
}

/* Re-arm the gate: nothing counts as pressed again until a frame is seen with
 * nothing held. main.c does this on every screen change; a screen must ALSO
 * do it after any blocking call it makes. */
static inline void app_disarm(app_ctx *ctx)
{
    ctx->keys_armed = 0;
}

void app_note(app_ctx *ctx, int level, const char *fmt, ...);
const char *app_close_reason_text(int reason);
const char *app_screen_name(apad_screen_id id);

/* Tier-2 broadcast DISCOVER on a socket of its own (the engine's socket
 * belongs to the session, and on this platform handing a broadcast address to
 * apad_client_probe() does not merely fail -- it wedges lwIP forever, see
 * main.c). On success fills ctx->ip_text / ctx->port_text / ctx->server_name
 * and sets from_announce. Returns 1 on an ANNOUNCE, 0 otherwise. Blocks for
 * up to ~500ms. */
int app_discover(app_ctx *ctx);

/* NO app_probe() HERE. Tier-3 unicast discovery used to be duplicated in
 * main.c because apad_client_probe() (clients/common/apad_client.c) never
 * returned when nothing answered on this platform's old shim. Fixed in
 * shim/net_nds.c (references/nds/README.md "Outcome B"): apad_client_probe()
 * now works exactly as it does on the 3DS, and screen_connect.c calls it
 * directly. */

/* Creates the session engine (apad_client_create) and stores it in
 * ctx->client. Returns 1 on success; on failure fills ctx->fatal and returns
 * 0, and the caller goes to APAD_SCREEN_FATAL.
 *
 * NOT part of bring-up, unlike every other client in this repo, and the
 * reason is DSWiFi: apad_client_create() opens a UDP socket, and there is no
 * interface for lwIP to bind one to until the console has actually associated
 * with an access point. So screen_wifi.c calls this at the moment it reaches
 * ASSOCIATED. Idempotent: a second call with a client already created is a
 * no-op returning 1. */
int app_client_start(app_ctx *ctx);

/* Battery, docs/PROTOCOL.md S5.5. -1 means "this console cannot measure one"
 * -- which is every DS and DS Lite, where getBatteryLevel() carries a single
 * low/ok bit and APAD_CAP_BATTERY is therefore never set. 0..100 in DSi mode.
 * See main.c's battery section for the DSi mapping and what is unverified
 * about it. */
int app_battery_percent(void);

/* Milliseconds between the last two frames, averaged over the last 32.
 * DIAGNOSTIC, and on this console a load-bearing one: the whole client is
 * paced by the frame loop -- the engine sends one INPUT_STATE per pump and
 * there is one pump per frame -- so a frame time above 17 ms is directly a
 * send rate below 60 Hz, and nothing else on screen would say so. Shown on
 * the diagnostics overlay (SELECT). */
int app_frame_ms(void);

/* Forces the top screen to redraw on the NEXT frame regardless of the
 * every-other-frame skip main.c otherwise applies (see main.c's header
 * comment on the skip). main.c calls this itself on every screen change;
 * app_note() calls it too, because a banner appearing is exactly the kind
 * of change a person is looking at the instant it happens. A screen that
 * flips ctx->show_diag (SELECT) should call it for the same reason. Safe to
 * call every frame if a caller is unsure -- it only ever costs one extra
 * top-screen redraw, never a correctness issue. */
void app_top_force_redraw(void);

/* Per-phase breakdown of the same frame, same 32-frame rolling average,
 * added for the frame-rate pass that follows shim/net_nds.c (2026-09-09):
 * app_frame_ms() says the loop is slow, this says WHERE. `update_ms` is
 * where apad_client_pump_ex() lives on the session screen, so it is the
 * number to compare against the server's measured rx_packets rate.
 *
 * flip_wait_ms/flip_swap_ms REPLACE the single `flip_ms` this used to be
 * (second frame-rate pass, same date): a single "flip ~9ms" number could not
 * say whether that was the VBlank WAIT or actual WORK (a buffer-pointer read
 * plus one register write), so ui_frame_begin() now times itself and
 * ui_flip_wait_ms()/ui_flip_swap_ms() below report the two halves
 * separately. Expect swap to read ~0 almost always -- that is the answer,
 * not a bug: the flip really is a register write, and the wait is where the
 * rest of a frame's unused VBlank budget already goes.
 *
 * Shown on the session screen's diag panel (SELECT), main.c owns the only
 * writer. */
typedef struct {
    uint32_t sample_ms;        /* hardware sampling (scanKeys/touchRead/...) */
    uint32_t update_ms;        /* the current screen's update(), incl. pump  */
    uint32_t draw_top_ms;      /* the current screen's draw_top()            */
    uint32_t draw_bottom_ms;   /* the current screen's draw_bottom()         */
    uint32_t flip_wait_ms;     /* ui_frame_begin(): cothread_yield_irq() only */
    uint32_t flip_swap_ms;     /* ui_frame_begin(): buffer pointer + register */
} app_frame_phases;

const app_frame_phases *app_frame_phases_ms(void);

/* ui.c's own timing, read by main.c to fill the two flip_* fields above and
 * by screen_session.c's diag panel to show clear_ms alongside draw_bottom_ms.
 * Implemented in ui.c (see its header comment on the wait/swap split); NOT
 * additions to ui.h -- that file stays the frozen 3DS-mirrored contract, and
 * this diagnostics-only surface lives here instead, next to app_frame_ms()
 * and app_frame_phases_ms() which already play the same role. */
uint32_t ui_flip_wait_ms(void);
uint32_t ui_flip_swap_ms(void);
uint32_t ui_last_clear_ms(void);

/* RAW PIXEL BLIT, added for the DSi QR camera screen (screen_qrscan.c) --
 * NOT part of ui.h's frozen 3DS-mirrored contract, because the 3DS has
 * nothing to mirror here (its camera viewfinder is a GPU texture draw
 * through citro2d, camera_3ds.c's apad3ds_cam_image()/C2D_DrawImageAt()).
 * Lives here for the same reason ui_flip_wait_ms() etc. do: a DS-only
 * capability declared next to ui.c's other DS-only accessors rather than
 * added to the contract every other screen file assumes is 3DS-shaped.
 *
 * `x`/`y` are RAW DS PIXELS on the CURRENT drawing surface (whichever of
 * ui_screen_top()/ui_screen_bottom() the caller selected this frame) --
 * UNLIKE every ui.h primitive, these are NOT 3DS-coordinate-space and are
 * NOT scaled by ui.c's sx()/sy(). There is no 3DS layout to stay numerically
 * compatible with: the source is native DS camera pixels (APAD_NDS_CAM_W x
 * APAD_NDS_CAM_H, camera_nds.h), and scaling a live camera image through the
 * 3DS-coordinate indirection that every OTHER call in this file goes through
 * would be actively wrong here, not merely redundant.
 *
 * `src` is `src_h` rows of `src_stride` PIXELS each (not bytes), tightly
 * packed native DS BGR555 (ui.c's rgb15() packing -- see camera_nds.h).
 * Clipped to the surface exactly as fill_px() clips a rect. */
void ui_blit_rgb15(int x, int y, const uint16_t *src, int w, int h,
                   int src_stride);

/* ------------------------------------------------------------------------ */
/* composite widgets (ui_widgets.c)                                         */
/* ------------------------------------------------------------------------ */

/* The whole top screen's status layout, shared by the connect and session
 * screens so a reading learned on one is valid on the other. `live` selects
 * the connected presentation. Leaves everything below y=UI_STATUS_BOTTOM free
 * for the calling screen. Coordinates are 3DS top-screen (400x240) space --
 * ui.c scales. */
#define UI_STATUS_BOTTOM 198.0f

void app_draw_status_top(app_ctx *ctx, int live);

#endif /* ATTICPAD_NDS_APP_H */
