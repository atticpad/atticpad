/*
 * clients/psp/source/app.h -- screen system and shared context.
 *
 * Mirrors clients/3ds/source/app.h. Four screens instead of five (no camera,
 * so no QR scan) and one draw() instead of draw_top()/draw_bottom(), because
 * this hardware has one screen.
 */
#ifndef ATTICPAD_PSP_APP_H
#define ATTICPAD_PSP_APP_H

#include <pspctrl.h>
#include <stdint.h>

#include "atticpad/atticpad.h"
#include "apad_client.h"
#include "net_psp.h"

typedef enum {
    APAD_SCREEN_CONNECT = 0,   /* wi-fi wait, address entry, PIN, connect   */
    APAD_SCREEN_SESSION,       /* live session                              */
    APAD_SCREEN_SELFTEST,      /* the hidden self-test (L+R+START / SELECT) */
    APAD_SCREEN_FATAL,         /* bring-up failed; self-test still reachable */
    APAD_SCREEN_COUNT
} apad_screen_id;

typedef struct app_ctx app_ctx;

typedef struct {
    const char     *name;
    void            (*enter)(app_ctx *ctx);
    apad_screen_id  (*update)(app_ctx *ctx);
    void            (*draw)(app_ctx *ctx);
} apad_screen;

extern const apad_screen apad_screen_connect;
extern const apad_screen apad_screen_session;
extern const apad_screen apad_screen_selftest;
extern const apad_screen apad_screen_fatal;

#define APP_MSG_LEN 128

struct app_ctx {
    /* -- this frame's input, filled by main.c before update() ------------ */
    unsigned int keys_held;
    unsigned int keys_down;    /* derived: held & ~prev. PSP has no hidKeysDown */
    unsigned int keys_prev;

    /* THE RELEASED-ONCE GATE, ported verbatim in spirit from the 3DS.
     *
     * Set on the first frame nothing at all is held; cleared on every screen
     * change. Every edge-triggered action checks it through app_pressed().
     *
     * The 3DS found this on hardware three times: a key held across a screen
     * transition -- or across a gap with no controller sampling, which every
     * blocking network call is -- reads back as a FRESH press on the next
     * scan rather than as still-held. Holding L+R+START for the self-test
     * landed on the next screen with START "newly pressed" and tore the
     * session down the instant it connected. This client has the same
     * blocking calls, so it inherits the gate before it inherits the bug. */
    int keys_armed;

    /* -- hardware, probed once ------------------------------------------- */
    uint32_t caps;             /* the APAD_CAP_* mask sent in HELLO         */
    int      have_battery;

    /* -- network bring-up, published by net_psp.c's thread ---------------- */
    apad_psp_net_status net;
    int                 saved_slot;   /* the network slot the last boot used;
                                       * 0 if none saved (config_psp.h)      */

    /* -- the engine ------------------------------------------------------- */
    int                connected;     /* 0->1 edge: first ACTIVE frame this
                                       * connect; gates the config save     */
    apad_client       *client;
    apad_client_stats  stats;
    apad_input_state   st;     /* this frame's input, as the wire sees it   */

    /* -- address, edited on the connect screen ---------------------------- */
    char ip_text[16];
    char port_text[6];

    /* The typed PIN. WIPED the moment it reaches the engine: S10 puts the
     * secret on the same footing as the PIN itself, and the engine keeps the
     * only copy that should outlive that call. */
    char pin_text[APAD_CLIENT_SECRET_MAX + 1];
    int  have_secret;

    /* -- cross-screen messaging ------------------------------------------- */
    char           banner[APP_MSG_LEN];
    int            banner_level;      /* 0 info, 1 warn, 2 bad             */
    char           fatal[APP_MSG_LEN];
    apad_screen_id selftest_return;
};

/* Edge-triggered press, gated by keys_armed. Use this, never keys_down
 * directly, for anything that changes screen or tears down a session. */
int  app_pressed(const app_ctx *ctx, unsigned int mask);
/* Call after any blocking work, so a key held across it does not read as a
 * fresh press on the next frame. */
void app_disarm(app_ctx *ctx);
void app_note(app_ctx *ctx, int level, const char *fmt, ...);

/* The status band every screen shares: header, round-trip, server panel and
 * the live input readout. Returns the first y a screen may use, exactly as
 * the 3DS's app_draw_status_top() / UI_STATUS_BOTTOM contract does. */
int  app_draw_status(app_ctx *ctx, const char *title, int live);
#define UI_STATUS_BOTTOM 148

#endif /* ATTICPAD_PSP_APP_H */
