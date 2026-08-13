/* clients/3ds/source/screen_connect.c
 *
 * The connect screen: where the client starts, where it comes BACK to when a
 * session ends, and the only screen from which the app can be exited.
 *
 * It owns four things that used to be four separate blocking console flows:
 *
 *   - the L+R+Start launch window (docs/docs/DESIGN.md S9.1). It lives here rather
 *     than in main() because main() no longer has a screen of its own to draw
 *     the countdown on. A shoulder button can be physically broken
 *     (see the agent notes), so this window is the LEAST reliable way in --
 *     which is exactly why SELECT does the same thing. As of the 2026-08-12
 *     "hide the debug surface" pass this is silent: no on-screen SELF-TEST
 *     button advertises it here any more (there never should have been one --
 *     docs/CONVENTIONS.md's convention is a HIDDEN self-test), the key just keeps
 *     working.
 *   - tier-2 broadcast discovery (docs/PROTOCOL.md S7), with the countdown
 *     that made the client usable unattended: nobody has to be standing at
 *     the console to tap "connect" in a netload loop.
 *   - tier-3 manual entry, S7's "not a debug fallback" and, on this LAN, the
 *     only path that works at all. It is now a touchscreen numpad instead of
 *     swkbdInputText(): swkbd BLOCKS until someone physically taps its
 *     Connect button, which is correct for a human correcting an address and
 *     a hang for everything else. That hang is the specific bug
 *     this pass set out to remove.
 *   - the return path from a finished session: the banner says why it ended
 *     and a countdown reconnects to the same server, so "the session dropped"
 *     costs a glance rather than a relaunch. Before this pass the main loop
 *     `break`ed and the process exited, which a user experienced as "it quit
 *     to the homescreen".
 *
 * THE RIGHT-HAND COLUMN'S THIRD SLOT is now SCAN QR (the UI pass,
 * "Deferred: QR-code pairing"). It was left as reserved empty space by the UI
 * pass and screen_qrscan.c filled it: one scan supplies the address AND the
 * S10.1 pairing key, which is why this screen's flat refusal to talk to a
 * server with pairing_required set is now conditional on not having one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

/* ------------------------------------------------------------------------ */
/* substates                                                                */
/* ------------------------------------------------------------------------ */

/* The _ARM states exist so a frame is DRAWN before the blocking call that
 * follows it -- see app.h's note on blocking work. Without them the console
 * freezes on the last frame of the previous state and the user has no idea
 * whether it is discovering, handshaking or hung. */
enum {
    CS_BOOT = 0,      /* L+R+Start window, ~2.5s                            */
    CS_DISCOVER_ARM,
    CS_DISCOVER,      /* blocking: one DISCOVER, up to 500ms for an ANNOUNCE */
    CS_IDLE,          /* editing the address; nothing happens on its own     */
    CS_COUNTDOWN,     /* auto-connect in N frames, cancelled by any input    */
    CS_CONNECT_ARM,
    CS_CONNECT        /* blocking: optional probe + the S8 handshake        */
};

static int s_sub;
static int s_boot_frames;

/* Which field the numpad types into. */
enum { FIELD_IP = 0, FIELD_PORT };
static int s_focus;

/* Latched so the button can be drawn held for the frame it is tapped on --
 * the tap is one frame long and an unlit button gives no feedback at all. */
static int s_flash_connect;
static int s_flash_scan;

/* Set when the CURRENT connect attempt was started by the countdown rather
 * than by a person. A failed automatic attempt goes back to counting down and
 * tries again; a failed manual one stops and waits, because the person who
 * pressed the button is standing right there and a client that silently
 * retries hides which address it is actually using. This is what makes an
 * unattended console recover from a server restart on its own -- the netload
 * loop is unattended most of the time. Any input cancels the retry. */
static int s_auto_attempt;

/* ~2.5s at 60Hz. Sampled across many frames, not on one scan right after
 * hid settles: hidKeysHeld() can read back 0 on the first scan or two after
 * hidInit, so a single-frame check silently misses a combo that WAS held the
 * whole time. That happened to a real person twice. */
#define BOOT_FRAMES 150

/* ~5s between automatic retries. Long enough that a restarting server has a
 * chance to be listening again, short enough that a person watching does not
 * think it has given up. */
#define RETRY_FRAMES 300

/* ------------------------------------------------------------------------ */
/* layout                                                                   */
/* ------------------------------------------------------------------------ */

static const ui_box kIpField   = {   6.0f,  26.0f, 196.0f, 28.0f };
static const ui_box kPortField = { 206.0f,  26.0f, 108.0f, 28.0f };
static const ui_box kConnectBtn  = { 170.0f,  60.0f, 144.0f, 44.0f };
/* The slot the UI pass reserved (x 170..314, y 148..192) for SCAN QR grew
 * downward to y=110 in the 2026-08-12 "hide the debug surface" pass: this
 * column used to be CONNECT / SELF-TEST / SCAN QR, and removing the
 * on-screen SELF-TEST button (SELECT still works, silently) left a hole
 * between the other two rather than three same-sized buttons. */
static const ui_box kScanBtn     = { 170.0f, 110.0f, 144.0f, 82.0f };

#define PAD_X0   6.0f
#define PAD_Y0  60.0f
#define PAD_W   50.0f
#define PAD_H   38.0f
#define PAD_GAP  3.0f

/* 3x4: digits in phone order, then '.' 0 DEL. '.' is only meaningful in the
 * IP field and is rejected (not hidden) when the port has focus -- a key that
 * vanishes is a rendering bug to the person looking at it. */
static const char kPadKeys[12] = {
    '1', '2', '3',
    '4', '5', '6',
    '7', '8', '9',
    '.', '0', '\b'
};

static void pad_key_box(int i, ui_box *out)
{
    out->x = PAD_X0 + (float)(i % 3) * (PAD_W + PAD_GAP);
    out->y = PAD_Y0 + (float)(i / 3) * (PAD_H + PAD_GAP);
    out->w = PAD_W;
    out->h = PAD_H;
}

/* ------------------------------------------------------------------------ */
/* field editing                                                            */
/* ------------------------------------------------------------------------ */

static char *focused_field(app_ctx *ctx, size_t *out_cap)
{
    if (s_focus == FIELD_PORT) {
        *out_cap = sizeof ctx->port_text;
        return ctx->port_text;
    }
    *out_cap = sizeof ctx->ip_text;
    return ctx->ip_text;
}

static void field_type(app_ctx *ctx, char c)
{
    size_t cap;
    char *buf = focused_field(ctx, &cap);
    size_t n = strlen(buf);

    if (c == '.' && s_focus != FIELD_IP) {
        return;
    }
    if (c == '\b') {
        if (n > 0) {
            buf[n - 1] = '\0';
        }
    } else if (n + 1 < cap) {
        buf[n] = c;
        buf[n + 1] = '\0';
    }
    /* Any edit means the address is no longer the one a server announced, so
     * the next connect must probe for pairing_required again (S8 puts pairing
     * before the handshake). */
    ctx->from_announce = 0;
    ctx->server_name[0] = '\0';

    /* And it is no longer the address the scanned key belongs to. Forget the
     * key in the ENGINE too, not just the flag here: a stale secret would
     * still be used to derive a session key for whatever gets typed next, and
     * "wrong key" for a server that needed none is a confusing way to fail.
     * apad_client_set_secret(NULL) is documented as "forget the current
     * secret" and zeroes it (clients/common/apad_client.h). */
    if (ctx->have_secret) {
        ctx->have_secret = 0;
        (void)apad_client_set_secret(ctx->client, NULL);
    }
}

/* No strtoul: a port is at most five digits and this avoids dragging locale
 * machinery in for it. Returns 0 for anything unparseable, which the caller
 * treats as "use the default". */
static uint16_t parse_port(const char *s)
{
    uint32_t v = 0;
    int i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9' || i >= 5) {
            return 0;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    if (v == 0u || v > 65535u) {
        return 0;
    }
    return (uint16_t)v;
}

/* ------------------------------------------------------------------------ */
/* the blocking connect step                                                */
/* ------------------------------------------------------------------------ */

static apad_screen_id do_connect(app_ctx *ctx)
{
    apad_addr parsed;
    uint16_t port = parse_port(ctx->port_text);
    int rc;

    s_sub = CS_IDLE;   /* every failure path below lands back in editing     */

    /* The probe and the handshake below are up to 4.5s with no hidScanInput()
     * in them. Anything still held when they return would read as a fresh
     * press on the next scan, so the gate goes back down before either. */
    app_disarm(ctx);

    if (apad_addr_parse(&parsed, ctx->ip_text, port ? port : (uint16_t)APAD_DEFAULT_PORT)
        != APAD_OK) {
        app_note(ctx, 2, "\"%s\" is not an IPv4 address", ctx->ip_text);
        s_auto_attempt = 0;   /* retrying an unparseable address never helps */
        return APAD_SCREEN_CONNECT;
    }
    if (port == 0u) {
        port = (uint16_t)APAD_DEFAULT_PORT;
        snprintf(ctx->port_text, sizeof ctx->port_text, "%u", (unsigned)port);
    }

    /* Tier 3: nothing has announced itself from this address, so
     * pairing_required (S6.2) is still unknown -- and S8 puts pairing BEFORE
     * the handshake, so ask now, while a refusal costs no pad slot. A timeout
     * is the ordinary tier-3 outcome on this LAN, not an error worth showing:
     * the WELCOME would say so anyway. */
    if (!ctx->from_announce) {
        if (apad_client_probe(ctx->client, ctx->ip_text, port, 500) == APAD_OK) {
            apad_client_get_stats(ctx->client, &ctx->stats);
            if (ctx->stats.pairing_required == 1 && !ctx->have_secret) {
                /* Do not send a HELLO at all: with no secret this client
                 * cannot answer AUTH_REQUIRED, and taking a pad slot it must
                 * then let lapse is worse than refusing.
                 *
                 * WITH a secret -- which on this console means a scanned
                 * S10.3 code, since there is still no PIN keypad -- this
                 * falls through and connects normally; the engine derives the
                 * session key and signs the ACK. */
                app_note(ctx, 2, "server requires pairing -- scan its QR code "
                                 "(SCAN QR / X)");
                s_auto_attempt = 0;
                return APAD_SCREEN_CONNECT;
            }
        }
    }

    rc = apad_client_connect(ctx->client, ctx->ip_text, port,
                             (uint16_t)APAD_DEFAULT_RATE_HZ, 4000);
    if (rc == APAD_OK) {
        apad_client_get_stats(ctx->client, &ctx->stats);
        ctx->banner[0] = '\0';
        return APAD_SCREEN_SESSION;
    }
    if (rc == APAD_ERR_AUTH) {
        /* The engine has already sent the ACK S9 requires and closed the local
         * session by the time this returns -- nothing is stuck, the server
         * reaps the slot in ~3s. APAD_ERR_AUTH means specifically "the WELCOME
         * wanted auth and there was no secret to derive a key from", so having
         * one already and still landing here means the pairing window closed
         * or the code was for a different server. */
        app_note(ctx, 2, "%s",
                 ctx->have_secret
                     ? "server rejected the scanned key -- rescan its QR code"
                     : "server asked for a pairing key -- scan its QR code "
                       "(SCAN QR / X)");
        s_auto_attempt = 0;
        return APAD_SCREEN_CONNECT;
    }

    app_note(ctx, 2, "handshake failed (rc=%d) -- is the server running?", rc);
    if (s_auto_attempt) {
        /* Automatic attempt: go back to counting down and try again. This is
         * the server-was-restarted case, and on an unattended console nobody
         * is there to tap CONNECT. Touching anything stops it. */
        ctx->reconnect_frames = RETRY_FRAMES;
        s_sub = CS_COUNTDOWN;
    }
    return APAD_SCREEN_CONNECT;
}

/* ------------------------------------------------------------------------ */
/* screen callbacks                                                         */
/* ------------------------------------------------------------------------ */

static void connect_enter(app_ctx *ctx)
{
    s_flash_connect = 0;
    s_flash_scan = 0;
    s_focus = FIELD_IP;
    s_auto_attempt = 0;

    if (ctx->want_connect_now) {
        /* Back from the QR screen. The address came from the server itself,
         * so there is nothing to discover, nothing to type and no reason to
         * make anyone tap CONNECT: go straight into the handshake. Checked
         * FIRST, ahead of the boot combo and discovery, because a scan is the
         * most specific intent this screen can be handed. */
        ctx->want_connect_now = 0;
        ctx->want_boot_combo = 0;
        ctx->want_discovery = 0;
        ctx->reconnect_frames = 0;
        s_sub = CS_CONNECT_ARM;
    } else if (ctx->want_boot_combo) {
        s_sub = CS_BOOT;
        s_boot_frames = BOOT_FRAMES;
    } else if (ctx->want_discovery) {
        s_sub = CS_DISCOVER_ARM;
    } else if (ctx->reconnect_frames > 0) {
        s_sub = CS_COUNTDOWN;
    } else {
        s_sub = CS_IDLE;
    }
}

/* Touch and key handling shared by CS_IDLE and CS_COUNTDOWN. Returns the
 * screen to switch to, or APAD_SCREEN_CONNECT to stay. Sets *acted when the
 * user did anything at all, which is what cancels the countdown. */
static apad_screen_id handle_ui(app_ctx *ctx, int *acted)
{
    *acted = 0;

    if (app_pressed(ctx, KEY_SELECT)) {
        *acted = 1;
        ctx->selftest_return = APAD_SCREEN_CONNECT;
        return APAD_SCREEN_SELFTEST;
    }
    if (app_pressed(ctx, KEY_B)) {
        /* THE deliberate app exit. START used to be "quit" and is now
         * "disconnect"; leaving the app is a separate, explicit action on the
         * one screen where it cannot interrupt anything. */
        *acted = 1;
        ctx->want_exit = 1;
        snprintf(ctx->exit_reason, sizeof ctx->exit_reason,
                 "user exited (B on the connect screen)");
        return APAD_SCREEN_CONNECT;
    }
    if (app_pressed(ctx, KEY_A)) {
        *acted = 1;
        s_flash_connect = 1;
        s_auto_attempt = 0;   /* a person asked for this one */
        s_sub = CS_CONNECT_ARM;
        return APAD_SCREEN_CONNECT;
    }
    if (app_pressed(ctx, KEY_X)) {
        /* X is free on this screen and the scanner needs a button reachable
         * without looking down at the touchscreen -- the console is about to
         * be pointed away from the person holding it. */
        *acted = 1;
        s_flash_scan = 1;
        return APAD_SCREEN_QRSCAN;
    }

    if (ctx->touch_pressed) {
        int px = (int)ctx->touch.px, py = (int)ctx->touch.py;
        int i;

        *acted = 1;
        if (ui_box_hit(&kIpField, px, py)) {
            s_focus = FIELD_IP;
        } else if (ui_box_hit(&kPortField, px, py)) {
            s_focus = FIELD_PORT;
        } else if (ui_box_hit(&kConnectBtn, px, py)) {
            s_flash_connect = 1;
            s_auto_attempt = 0;
            s_sub = CS_CONNECT_ARM;
        } else if (ui_box_hit(&kScanBtn, px, py)) {
            s_flash_scan = 1;
            return APAD_SCREEN_QRSCAN;
        } else {
            for (i = 0; i < 12; i++) {
                ui_box b;

                pad_key_box(i, &b);
                if (ui_box_hit(&b, px, py)) {
                    field_type(ctx, kPadKeys[i]);
                    break;
                }
            }
        }
    }
    return APAD_SCREEN_CONNECT;
}

static apad_screen_id connect_update(app_ctx *ctx)
{
    int acted;
    apad_screen_id next;

    switch (s_sub) {
        case CS_BOOT:
            /* Level-triggered on purpose: hidKeysHeld() needs no released-once
             * gate, and this combo is HELD by definition. */
            if ((ctx->keys_held & (KEY_L | KEY_R | KEY_START))
                == (KEY_L | KEY_R | KEY_START)) {
                ctx->want_boot_combo = 0;
                ctx->selftest_return = APAD_SCREEN_CONNECT;
                return APAD_SCREEN_SELFTEST;
            }
            if (app_pressed(ctx, KEY_SELECT)) {
                ctx->want_boot_combo = 0;
                ctx->selftest_return = APAD_SCREEN_CONNECT;
                return APAD_SCREEN_SELFTEST;
            }
            if (ctx->touch_pressed || app_pressed(ctx, KEY_A | KEY_B)) {
                /* Someone is already at the console: skip the window and let
                 * them type. */
                ctx->want_boot_combo = 0;
                ctx->want_discovery = 0;
                s_sub = CS_IDLE;
                return APAD_SCREEN_CONNECT;
            }
            if (--s_boot_frames <= 0) {
                ctx->want_boot_combo = 0;
                s_sub = ctx->want_discovery ? CS_DISCOVER_ARM : CS_IDLE;
            }
            return APAD_SCREEN_CONNECT;

        case CS_DISCOVER_ARM:
            s_sub = CS_DISCOVER;
            return APAD_SCREEN_CONNECT;

        case CS_DISCOVER:
            ctx->want_discovery = 0;
            app_disarm(ctx);   /* ~500ms of blocking I/O follows -- see
                                * app_disarm()'s comment */
            if (app_discover(ctx)) {
                app_note(ctx, 0, "Found \"%s\" on the network",
                         ctx->server_name);
                ctx->reconnect_frames = 60;   /* ~1s, cancellable            */
                s_sub = CS_COUNTDOWN;
            } else if (ctx->ip_text[0] != '\0') {
                /* "Trying the last address" (APAD_MSG_SERVER_NOT_FOUND) is
                 * only a real fallback when ip_text actually holds one --
                 * the config file (config_3ds.c), a previous ANNOUNCE, a
                 * scanned QR code, or the user's own typing. A FRESH unit
                 * with nothing in the field has no "last address" to try and
                 * nothing to say about it: no attempt, no message, the
                 * screen just sits on the numpad/QR options (CS_IDLE below).
                 * See main.c's boot-defaults comment for why ip_text can be
                 * empty here at all. */
                app_note(ctx, 1, "%s", apad_ui_msg(APAD_MSG_SERVER_NOT_FOUND));
                ctx->reconnect_frames = 90;   /* ~1.5s, as before            */
                s_sub = CS_COUNTDOWN;
            } else {
                s_sub = CS_IDLE;
            }
            return APAD_SCREEN_CONNECT;

        case CS_COUNTDOWN:
            next = handle_ui(ctx, &acted);
            if (next != APAD_SCREEN_CONNECT || ctx->want_exit) {
                ctx->reconnect_frames = 0;
                return next;
            }
            if (acted) {
                /* Any deliberate input cancels the countdown. It does NOT
                 * cancel the connect the user just asked for -- handle_ui()
                 * already moved s_sub to CS_CONNECT_ARM in that case. */
                ctx->reconnect_frames = 0;
                if (s_sub == CS_COUNTDOWN) {
                    s_sub = CS_IDLE;
                }
                return APAD_SCREEN_CONNECT;
            }
            if (--ctx->reconnect_frames <= 0) {
                ctx->reconnect_frames = 0;
                s_auto_attempt = 1;   /* nobody asked; retry on failure */
                s_sub = CS_CONNECT_ARM;
            }
            return APAD_SCREEN_CONNECT;

        case CS_CONNECT_ARM:
            s_sub = CS_CONNECT;
            return APAD_SCREEN_CONNECT;

        case CS_CONNECT:
            return do_connect(ctx);

        case CS_IDLE:
        default:
            next = handle_ui(ctx, &acted);
            return next;
    }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void connect_draw_top(app_ctx *ctx)
{
    uint32_t banner_colour;

    app_draw_status_top(ctx, 0);

    app_draw_gyro_line(ctx, 8.0f, UI_STATUS_BOTTOM + 1.0f);

    /* The soc-buffer-size / gyro-coefficient / last-APT-hook line that used
     * to live here unconditionally moved to the self-test screen
     * (screen_selftest.c) in the 2026-08-12 "hide the debug surface" pass:
     * it is per-console calibration/engineering data, not something every
     * user needs to see on the first screen they land on. Still there,
     * reachable the same way self-test always was (L+R+Start / SELECT). */

    if (ctx->banner[0] != '\0') {
        banner_colour = (ctx->banner_level >= 2) ? ui_c_bad()
                      : (ctx->banner_level == 1) ? ui_c_warn()
                                                 : ui_c_dim();
        ui_textf_fit(8.0f, 225.0f, UI_S_SMALL, banner_colour, UI_ALIGN_LEFT,
                     UI_TOP_W - 16.0f, "%s", ctx->banner);
    }
}

static void draw_field(const ui_box *b, const char *label, const char *value,
                       int focused)
{
    ui_panel(b, focused ? ui_c_panel_hi() : ui_c_panel(),
             focused ? ui_c_accent() : ui_c_border());
    ui_textf(b->x + 5.0f, b->y + 1.0f, 0.36f, ui_c_dim(), UI_ALIGN_LEFT,
             "%s", label);
    ui_textf_fit(b->x + 5.0f, b->y + 10.0f, UI_S_BODY, ui_c_text(),
                 UI_ALIGN_LEFT, b->w - 10.0f, "%s%s", value,
                 focused ? "_" : "");
}

static void connect_draw_bottom(app_ctx *ctx)
{
    char right[48];
    int i;

    switch (s_sub) {
        case CS_BOOT:
            snprintf(right, sizeof right, "self-test window %ds",
                     (s_boot_frames + 59) / 60);
            break;
        case CS_DISCOVER_ARM:
        case CS_DISCOVER:
            snprintf(right, sizeof right, "searching...");
            break;
        case CS_COUNTDOWN:
            snprintf(right, sizeof right, "connecting in %.1fs",
                     (double)ctx->reconnect_frames / 60.0);
            break;
        case CS_CONNECT_ARM:
        case CS_CONNECT:
            snprintf(right, sizeof right, "connecting...");
            break;
        default:
            snprintf(right, sizeof right, "enter address");
            break;
    }
    ui_header(UI_BOT_W, "AtticPad -- connect", right,
              (s_sub == CS_COUNTDOWN) ? ui_c_warn() : ui_c_accent());

    draw_field(&kIpField, "SERVER IP", ctx->ip_text, s_focus == FIELD_IP);
    draw_field(&kPortField, "PORT", ctx->port_text, s_focus == FIELD_PORT);

    for (i = 0; i < 12; i++) {
        ui_box b;
        char label[4];

        pad_key_box(i, &b);
        if (kPadKeys[i] == '\b') {
            snprintf(label, sizeof label, "DEL");
        } else {
            snprintf(label, sizeof label, "%c", kPadKeys[i]);
        }
        /* '.' is inert while the port field has focus; drawn dim so that is
         * visible rather than mysterious. */
        if (kPadKeys[i] == '.' && s_focus != FIELD_IP) {
            ui_panel(&b, ui_c_bg(), ui_c_border());
            ui_textf(b.x + b.w * 0.5f, b.y + (b.h - UI_LINE(UI_S_BODY)) * 0.5f,
                     UI_S_BODY, ui_c_border(), UI_ALIGN_CENTER, "%s", label);
        } else {
            ui_button(&b, label, 0, 0);
        }
    }

    ui_button(&kConnectBtn, "CONNECT", s_flash_connect, 1);
    ui_button(&kScanBtn, "SCAN QR", s_flash_scan, 0);
    s_flash_connect = 0;
    s_flash_scan = 0;

    ui_textf_fit(170.0f, 196.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
                 144.0f, "A: connect   X: scan   B: exit");

    if (ctx->have_secret) {
        /* The one visible sign that a scan happened. Says a key is HELD, not
         * what it is -- docs/PROTOCOL.md S10 puts a pairing key on the same
         * footing as the PIN, and neither belongs on a screen once it has
         * been read. */
        ui_textf_fit(6.0f, 224.0f, UI_S_TINY, ui_c_good(), UI_ALIGN_LEFT,
                     UI_BOT_W - 12.0f, "%s",
                     apad_ui_msg(APAD_MSG_PAIRED_KEY_HELD));
    } else if (ctx->from_announce && ctx->server_name[0] != '\0') {
        ui_textf_fit(6.0f, 224.0f, UI_S_TINY, ui_c_good(), UI_ALIGN_LEFT,
                     UI_BOT_W - 12.0f, "discovered: %s", ctx->server_name);
    } else {
        ui_textf_fit(6.0f, 224.0f, UI_S_TINY, ui_c_border(), UI_ALIGN_LEFT,
                     UI_BOT_W - 12.0f, "Enter the address shown on your PC");
    }
}

const apad_screen apad_screen_connect = {
    "connect",
    connect_enter,
    connect_update,
    connect_draw_top,
    connect_draw_bottom
};
