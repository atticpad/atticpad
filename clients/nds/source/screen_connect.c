/* clients/nds/source/screen_connect.c
 *
 * clients/3ds/source/screen_connect.c, ported. Where the client goes once the
 * network is up, where it comes BACK to when a session ends, and the only
 * screen from which the app can be exited. It owns:
 *
 *   - tier-2 broadcast discovery (docs/PROTOCOL.md S7), with the countdown
 *     that makes the client usable unattended: nobody has to be standing at
 *     the console to tap CONNECT.
 *   - tier-3 manual entry, S7's "not a debug fallback" and, on most LANs, the
 *     only path that works at all.
 *   - S10 pairing: the PIN prompt.
 *   - the return path from a finished session: the banner says why it ended
 *     and a countdown reconnects to the same server, so "the session dropped"
 *     costs a glance rather than a relaunch.
 *
 * THREE DIFFERENCES FROM THE 3DS FILE, and all three are about text entry:
 *
 *   1. NO NUMPAD. The 3DS draws a 12-key touch numpad because swkbdInputText()
 *      BLOCKS until a human taps its Connect button -- correct for a person
 *      correcting an address, a hang for everything else. The DS has libnds's
 *      on-screen keyboard, which is non-blocking by construction
 *      (keyboardUpdate() returns one character per frame or nothing), so the
 *      numpad is gone and kbd_nds.c is what types. The FIELD SEMANTICS are
 *      unchanged, including the one that matters: editing the address forgets
 *      the pairing secret, in the engine as well as here.
 *   2. SCAN QR IS DSi-ONLY, AND SHARES A SLOT WITH ENTER PIN. There is no
 *      camera on a DS or DS Lite, so in DS mode (ctx->is_dsi == 0) the button
 *      in that slot is ENTER PIN, unchanged from before this screen had a QR
 *      option at all. In DSi mode the SAME BOX becomes SCAN QR instead --
 *      not an addition beside it, because the layout has no third slot above
 *      the keyboard (see the layout comment below) -- and PIN entry is still
 *      reachable, via X, exactly as the bottom-screen hint line already
 *      advertises ("X PIN"). This is a real, named difference from the 3DS's
 *      own binding, where X reaches the QR screen because there is no
 *      keyboard for X to conflict with; here X was already spoken for.
 *   3. NO BOOT COMBO. It moved to screen_wifi.c, which is now the first
 *      screen a launch lands on.
 *
 * SO THIS SCREEN CAN NOW DO WHAT THE 3DS's CANNOT: connect to a server with
 * pairing switched on, by typing a PIN, on every unit; and on a DSi it can
 * ALSO scan the same QR code the 3DS does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "kbd_nds.h"
#include "wifi_nds.h"

/* ------------------------------------------------------------------------ */
/* substates                                                                */
/* ------------------------------------------------------------------------ */

/* The _ARM states exist so a frame is DRAWN before the blocking call that
 * follows -- see app.h. Without them the console freezes on the last frame of
 * the previous state and the user has no idea whether it is discovering,
 * handshaking or hung. */
enum {
    CS_DISCOVER_ARM = 0,
    CS_DISCOVER,      /* blocking: one DISCOVER, up to 500ms for an ANNOUNCE */
    CS_IDLE,          /* editing the address; nothing happens on its own     */
    CS_COUNTDOWN,     /* auto-connect in N frames, cancelled by any input    */
    CS_CONNECT_ARM,
    CS_CONNECT,       /* blocking: optional probe + the S8 handshake         */
    CS_PIN,           /* typing the S10 pairing PIN                          */
    CS_PIN_ARM,       /* one drawn frame before the ~1s key derivation       */
    CS_PIN_APPLY
};

static int s_sub;

/* Which field the keyboard types into. */
enum { FIELD_IP = 0, FIELD_PORT };
static int s_focus;

/* Latched so a button can be drawn held for the frame it is tapped on -- the
 * tap is one frame long and an unlit button gives no feedback at all. */
static int s_flash_connect;
static int s_flash_pin;

/* Set when the CURRENT connect attempt was started by the countdown rather
 * than by a person. A failed automatic attempt goes back to counting down and
 * tries again; a failed manual one stops and waits, because the person who
 * pressed the button is standing right there and a client that silently
 * retries hides which address it is actually using. */
static int s_auto_attempt;

/* THE PIN, and it lives here rather than in app_ctx for the same reason the
 * network key lives in screen_wifi.c: docs/PROTOCOL.md S10 treats it as a
 * secret, so it exists in one buffer, becomes one apad_client_set_secret()
 * call, and is wiped. It is never drawn back to the screen in full and never
 * reaches config_nds.c. APAD_CLIENT_SECRET_MAX is the engine's own ceiling
 * (apad_client.h) and S10.1 is explicit that a client MUST NOT assume six
 * digits, so this is not a six-character buffer. */
static char s_pin[APAD_CLIENT_SECRET_MAX + 1];

#define RETRY_FRAMES 300   /* ~5s between automatic retries */

/* How long the reachability probe below waits for an ANNOUNCE. THREE SECONDS,
 * not the 500 ms the 3DS gives its own probe, and the number was measured
 * rather than chosen: under melonDS's libslirp NAT the first round trip after
 * association routinely took over 600 ms, and a 600 ms window made a server
 * that was demonstrably running (proved from the host with
 * tools/engine-client in the same second) look absent. Three seconds is
 * generous for a LAN and still short enough that a wrong address is a pause
 * rather than a hang. */
#define PROBE_TIMEOUT_MS 3000

/* ------------------------------------------------------------------------ */
/* layout -- 3DS bottom-screen (320x240) coordinates                        */
/* ------------------------------------------------------------------------ */

/* Everything interactive stays ABOVE UI_KBD_TOP_Y (ui.h): below that line the
 * libnds keyboard's background layer owns the screen, and a button drawn
 * under it would be invisible and still tappable. That is the one constraint
 * this console adds to the copied layout, and it is why the 3DS's SCAN QR box
 * (y 110..192) could not simply be relabelled in place. */
static const ui_box kIpField    = {   6.0f,  26.0f, 196.0f, 28.0f };
static const ui_box kPortField  = { 206.0f,  26.0f, 108.0f, 28.0f };
static const ui_box kConnectBtn = { 170.0f,  60.0f, 144.0f, 44.0f };
static const ui_box kPinBtn     = {   6.0f,  60.0f, 150.0f, 44.0f };

static const ui_box kPinField   = {   6.0f,  26.0f, 308.0f, 28.0f };
static const ui_box kPinOkBtn   = { 170.0f,  60.0f, 144.0f, 44.0f };
static const ui_box kPinCancel  = {   6.0f,  60.0f, 150.0f, 44.0f };

/* The keyboard's top edge in DS pixels, for the touch exclusion below. */
#define KBD_TOP_PX ((int)(UI_KBD_TOP_Y * 4.0f / 5.0f))

/* A tap that belongs to THIS SCREEN rather than to the keyboard. libnds reads
 * the same touchscreen we do and there is no way to ask it "did you take that
 * one", so the split is spatial: while the keyboard is up, everything below
 * its top edge is its. */
static int touch_is_ours(const app_ctx *ctx)
{
    if (!ctx->touch_pressed) {
        return 0;
    }
    if (apad_kbd_visible() && (int)ctx->touch.py >= KBD_TOP_PX) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* field editing                                                            */
/* ------------------------------------------------------------------------ */

static void field_edited(app_ctx *ctx)
{
    /* Any edit means the address is no longer the one a server announced, so
     * the next connect must probe for pairing_required again (S8 puts pairing
     * before the handshake). */
    ctx->from_announce = 0;
    ctx->server_name[0] = '\0';

    /* And it is no longer the address the PIN belongs to. Forget the secret
     * in the ENGINE too, not just the flag here: a stale secret would still
     * be used to derive a session key for whatever gets typed next, and
     * "wrong PIN" for a server that needed none is a confusing way to fail.
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

    s_sub = CS_IDLE;   /* every failure path below lands back in editing */

    /* The probe and the handshake below are seconds with no scanKeys() in
     * them. Anything still held when they return would read as a fresh press
     * on the next scan, so the gate goes back down before either. */
    app_disarm(ctx);

    if (apad_addr_parse(&parsed, ctx->ip_text,
                        port ? port : (uint16_t)APAD_DEFAULT_PORT) != APAD_OK) {
        /* Shortened from "\"%s\" is not an IPv4 address" -- ip_text can be up
         * to 15 characters and the quoted original ran close to this
         * screen's 256px banner width. */
        app_note(ctx, 2, "bad address: %s", ctx->ip_text);
        s_auto_attempt = 0;   /* retrying an unparseable address never helps */
        return APAD_SCREEN_CONNECT;
    }
    if (port == 0u) {
        port = (uint16_t)APAD_DEFAULT_PORT;
        snprintf(ctx->port_text, sizeof ctx->port_text, "%u", (unsigned)port);
    }

    /* THE §8 PAIRING PROBE, now called exactly as
     * clients/3ds/source/screen_connect.c calls it: apad_client_probe()
     * directly, on the engine's own socket. Until shim/net_nds.c
     * (references/nds/README.md "Outcome B") this had to be app_probe(), a
     * hand-rolled duplicate that polled instead of waiting, because the old
     * shim's apad_udp_recv() never returned when nothing answered a timed
     * receive -- apad_client_probe()'s internal drain_rx(c, 25) is exactly
     * that shape. Now that the shim honours its timeout, the duplicate is
     * gone; a timeout here is the ordinary tier-3 outcome (S7: "not a debug
     * fallback"), not something to route around.
     *
     * S8 puts pairing BEFORE the handshake, and S6.2's pairing_required is
     * what says whether a PIN is needed -- asking now costs no pad slot,
     * where finding out from a WELCOME costs one that then has to lapse.
     *
     * PROBE_TIMEOUT_MS is 3000, not the 3DS's 500: apad_client_probe() sends
     * exactly ONE DISCOVER with no retransmit (S4: DISCOVER is not a
     * reliable message, and the engine does not resend it the way
     * app_discover()'s tier-2 loop does its own). Under melonDS's
     * indirect/libslirp NAT the first datagram after association was lost or
     * delayed past 600ms often enough that a short window read a demonstrably
     * running server as absent; this is unchanged by the shim fix and is a
     * real, reported difference from app_discover()'s resend-every-400ms
     * robustness. A generous one-shot window is the only lever this call
     * leaves to pull. */
    if (!ctx->from_announce || !ctx->have_secret) {
        if (apad_client_probe(ctx->client, ctx->ip_text, port,
                              PROBE_TIMEOUT_MS) == APAD_OK) {
            apad_client_get_stats(ctx->client, &ctx->stats);
            ctx->from_announce = 1;
            if (ctx->stats.pairing_required == 1 && !ctx->have_secret) {
                /* Do not send a HELLO at all: with no secret this client
                 * cannot answer AUTH_REQUIRED, and taking a pad slot it must
                 * then let lapse is worse than asking first. Unlike the 3DS
                 * (whose only channel is a QR scan) this is a PROMPT rather
                 * than a refusal -- there is a keyboard here. */
                app_note(ctx, 1, "%s", apad_ui_msg(APAD_MSG_NEED_PIN));
                s_auto_attempt = 0;
                s_pin[0] = '\0';
                apad_kbd_show();
                s_sub = CS_PIN;
                return APAD_SCREEN_CONNECT;
            }
        }
        /* A probe timeout is NOT treated as fatal here, on purpose: S7 says
         * tier 3 has no ANNOUNCE at all, so plenty of legitimate servers
         * never answer a DISCOVER, and the WELCOME below will say whether
         * pairing is on. apad_client_connect() below runs either way -- the
         * exact shape clients/3ds/source/screen_connect.c uses. The one
         * platform-specific check this console still makes on its own is the
         * radio: nothing else polls Wi-Fi association once screen_wifi.c has
         * handed over, and "no reply at all" looks identical whether the
         * server is down or this console's AP link dropped -- the two need
         * opposite actions from the person holding it. */
        else if (!apad_nds_wifi_associated()) {
            app_note(ctx, 2, "Wi-Fi dropped -- rejoining");
            ctx->wifi_ready = 0;
            return APAD_SCREEN_WIFI;
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
         * wanted auth and there was no secret to derive a key from". */
        app_note(ctx, ctx->have_secret ? 2 : 1, "%s",
                 apad_ui_msg(ctx->have_secret ? APAD_MSG_WRONG_PIN
                                              : APAD_MSG_NEED_PIN));
        s_auto_attempt = 0;
        s_pin[0] = '\0';
        apad_kbd_show();
        s_sub = CS_PIN;
        return APAD_SCREEN_CONNECT;
    }

    /* Shortened from "handshake failed (rc=%d) -- is the server running?" --
     * the rhetorical question added width without adding information.
     * When a key derivation ran before the failure, its wall time goes on
     * the line: a paired connect that dies of the server's 3 s idle timeout
     * looks exactly like a dead server otherwise, and the number says which
     * (found on a DSi, 2026-09-10). */
    apad_client_get_stats(ctx->client, &ctx->stats);
    if (ctx->stats.derive_ms != 0u) {
        app_note(ctx, 2, "handshake failed (rc=%d) -- key took %lu ms", rc,
                 (unsigned long)ctx->stats.derive_ms);
    } else {
        app_note(ctx, 2, "handshake failed (rc=%d)", rc);
    }
    if (s_auto_attempt) {
        ctx->reconnect_frames = RETRY_FRAMES;
        s_sub = CS_COUNTDOWN;
    }
    return APAD_SCREEN_CONNECT;
}

/* ------------------------------------------------------------------------ */
/* the blocking PIN step                                                    */
/* ------------------------------------------------------------------------ */

/* apad_client_set_secret() only copies; the ~1s of PBKDF2 happens inside
 * apad_client_connect() when the WELCOME asks for auth (docs/PROTOCOL.md S10:
 * 10,000 iterations, and this CPU does 8-10k a second -- see the platform
 * skill). So the "deriving" frame has to be drawn before the CONNECT that
 * follows, not before set_secret(): CS_PIN_ARM draws it, CS_PIN_APPLY runs
 * the handshake. */
static apad_screen_id do_pin_apply(app_ctx *ctx)
{
    if (apad_client_set_secret(ctx->client, s_pin) != APAD_OK) {
        app_note(ctx, 2, "That PIN is too long for this server");
        memset(s_pin, 0, sizeof s_pin);
        s_sub = CS_PIN;
        return APAD_SCREEN_CONNECT;
    }
    ctx->have_secret = 1;
    memset(s_pin, 0, sizeof s_pin);   /* the engine has the only copy now */

    /* from_announce is already 1 (the probe that asked for this PIN set it)
     * and have_secret is now 1, so do_connect()'s reachability gate is
     * skipped and the handshake runs straight away -- the server answered
     * seconds ago and re-proving it would only add a round trip in front of
     * a PBKDF2 the user is already waiting on. */
    s_sub = CS_CONNECT;
    return do_connect(ctx);
}

/* ------------------------------------------------------------------------ */
/* screen callbacks                                                         */
/* ------------------------------------------------------------------------ */

static void connect_enter(app_ctx *ctx)
{
    s_flash_connect = 0;
    s_flash_pin = 0;
    s_focus = FIELD_IP;
    s_auto_attempt = 0;
    memset(s_pin, 0, sizeof s_pin);

    if (ctx->want_connect_now) {
        ctx->want_connect_now = 0;
        ctx->want_discovery = 0;
        ctx->reconnect_frames = 0;
        s_sub = CS_CONNECT_ARM;
    } else if (ctx->want_discovery) {
        s_sub = CS_DISCOVER_ARM;
    } else if (ctx->reconnect_frames > 0) {
        s_sub = CS_COUNTDOWN;
    } else {
        s_sub = CS_IDLE;
    }
    if (s_sub == CS_IDLE) {
        apad_kbd_show();
    } else {
        apad_kbd_hide();
    }
}

/* Touch and key handling shared by CS_IDLE and CS_COUNTDOWN. Returns the
 * screen to switch to, or APAD_SCREEN_CONNECT to stay. Sets *acted when the
 * user did anything at all, which is what cancels the countdown. */
static apad_screen_id handle_ui(app_ctx *ctx, int *acted)
{
    int key;

    *acted = 0;

    if (app_pressed(ctx, KEY_SELECT)) {
        *acted = 1;
        ctx->selftest_return = APAD_SCREEN_CONNECT;
        return APAD_SCREEN_SELFTEST;
    }
    if (app_pressed(ctx, KEY_B)) {
        /* THE deliberate app exit. START is "disconnect" on the session
         * screen; leaving the app is a separate, explicit action on the one
         * screen where it cannot interrupt anything. */
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
        apad_kbd_hide();
        return APAD_SCREEN_CONNECT;
    }
    if (app_pressed(ctx, KEY_X)) {
        /* X is free on this screen and reaches the PIN prompt without looking
         * down at the touchscreen -- the same reasoning the 3DS gives for
         * putting SCAN QR on X. */
        *acted = 1;
        s_flash_pin = 1;
        s_pin[0] = '\0';
        s_sub = CS_PIN;
        apad_kbd_show();
        return APAD_SCREEN_CONNECT;
    }
    if (app_pressed(ctx, KEY_L) || app_pressed(ctx, KEY_R)) {
        *acted = 1;
        s_focus = (s_focus == FIELD_IP) ? FIELD_PORT : FIELD_IP;
        return APAD_SCREEN_CONNECT;
    }

    /* Typing. The field's own filter is what keeps the address to digits and
     * dots and the port to digits, so there is no second copy of that rule
     * anywhere. */
    key = apad_kbd_poll();
    if (key == '\n') {
        *acted = 1;
        s_flash_connect = 1;
        s_auto_attempt = 0;
        s_sub = CS_CONNECT_ARM;
        apad_kbd_hide();
        return APAD_SCREEN_CONNECT;
    }
    if (key != 0) {
        int changed;

        *acted = 1;
        if (s_focus == FIELD_PORT) {
            changed = apad_kbd_apply(key, ctx->port_text,
                                     sizeof ctx->port_text, "0123456789");
        } else {
            changed = apad_kbd_apply(key, ctx->ip_text, sizeof ctx->ip_text,
                                     "0123456789.");
        }
        if (changed) {
            field_edited(ctx);
        }
    }

    if (touch_is_ours(ctx)) {
        int px = (int)ctx->touch.px, py = (int)ctx->touch.py;

        *acted = 1;
        if (ui_box_hit(&kIpField, px, py)) {
            s_focus = FIELD_IP;
        } else if (ui_box_hit(&kPortField, px, py)) {
            s_focus = FIELD_PORT;
        } else if (ui_box_hit(&kConnectBtn, px, py)) {
            s_flash_connect = 1;
            s_auto_attempt = 0;
            s_sub = CS_CONNECT_ARM;
            apad_kbd_hide();
        } else if (ui_box_hit(&kPinBtn, px, py)) {
            if (ctx->is_dsi) {
                /* SCAN QR in this build (see this file's header, point 2).
                 * apad_kbd_hide() rather than _show(): this screen is about
                 * to leave, and a keyboard should not still be up over
                 * whatever screen is drawn next. */
                s_flash_pin = 1;
                apad_kbd_hide();
                return APAD_SCREEN_QRSCAN;
            }
            s_flash_pin = 1;
            s_pin[0] = '\0';
            s_sub = CS_PIN;
            apad_kbd_show();
        }
    }
    return APAD_SCREEN_CONNECT;
}

static apad_screen_id connect_update(app_ctx *ctx)
{
    int acted;
    apad_screen_id next;

    switch (s_sub) {
    case CS_DISCOVER_ARM:
        s_sub = CS_DISCOVER;
        return APAD_SCREEN_CONNECT;

    case CS_DISCOVER:
        ctx->want_discovery = 0;
        app_disarm(ctx);   /* ~500ms of blocking I/O follows */
        if (app_discover(ctx)) {
            app_note(ctx, 0, "%s", apad_ui_msg(APAD_MSG_SERVER_FOUND));
            ctx->reconnect_frames = 60;   /* ~1s, cancellable */
            s_sub = CS_COUNTDOWN;
        } else if (ctx->ip_text[0] != '\0') {
            /* "Trying the last address" is only a real fallback when ip_text
             * actually holds one -- the config file, a previous ANNOUNCE, or
             * the user's own typing. A FRESH unit with nothing in the field
             * has no last address and nothing to say about it: no attempt, no
             * message, the screen just waits on the keyboard. */
            app_note(ctx, 1, "%s", apad_ui_msg(APAD_MSG_SERVER_NOT_FOUND));
            ctx->reconnect_frames = 90;   /* ~1.5s */
            s_sub = CS_COUNTDOWN;
        } else {
            s_sub = CS_IDLE;
            apad_kbd_show();
        }
        return APAD_SCREEN_CONNECT;

    case CS_COUNTDOWN:
        next = handle_ui(ctx, &acted);
        if (next != APAD_SCREEN_CONNECT || ctx->want_exit) {
            ctx->reconnect_frames = 0;
            return next;
        }
        if (acted) {
            /* Any deliberate input cancels the countdown. It does NOT cancel
             * the connect the user just asked for -- handle_ui() already
             * moved s_sub in that case. */
            ctx->reconnect_frames = 0;
            if (s_sub == CS_COUNTDOWN) {
                s_sub = CS_IDLE;
                apad_kbd_show();
            }
            return APAD_SCREEN_CONNECT;
        }
        if (--ctx->reconnect_frames <= 0) {
            ctx->reconnect_frames = 0;
            s_auto_attempt = 1;   /* nobody asked; retry on failure */
            s_sub = CS_CONNECT_ARM;
            apad_kbd_hide();
        }
        return APAD_SCREEN_CONNECT;

    case CS_CONNECT_ARM:
        s_sub = CS_CONNECT;
        return APAD_SCREEN_CONNECT;

    case CS_CONNECT:
        return do_connect(ctx);

    case CS_PIN: {
        int key = apad_kbd_poll();

        if (app_pressed(ctx, KEY_B)
            || (touch_is_ours(ctx)
                && ui_box_hit(&kPinCancel, (int)ctx->touch.px,
                              (int)ctx->touch.py))) {
            memset(s_pin, 0, sizeof s_pin);
            s_sub = CS_IDLE;
            return APAD_SCREEN_CONNECT;
        }
        if (key == '\n' || app_pressed(ctx, KEY_A)
            || (touch_is_ours(ctx)
                && ui_box_hit(&kPinOkBtn, (int)ctx->touch.px,
                              (int)ctx->touch.py))) {
            if (s_pin[0] == '\0') {
                app_note(ctx, 1, "%s", apad_ui_msg(APAD_MSG_NEED_PIN));
                return APAD_SCREEN_CONNECT;
            }
            apad_kbd_hide();
            s_sub = CS_PIN_ARM;
            return APAD_SCREEN_CONNECT;
        }
        /* No filter: S10.1 forbids assuming six digits, so anything the
         * keyboard can produce is accepted and the server decides. */
        (void)apad_kbd_apply(key, s_pin, sizeof s_pin, NULL);
        return APAD_SCREEN_CONNECT;
    }

    case CS_PIN_ARM:
        s_sub = CS_PIN_APPLY;
        return APAD_SCREEN_CONNECT;

    case CS_PIN_APPLY:
        app_disarm(ctx);
        return do_pin_apply(ctx);

    case CS_IDLE:
    default:
        return handle_ui(ctx, &acted);
    }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void connect_draw_top(app_ctx *ctx)
{
    uint32_t banner_colour;

    app_draw_status_top(ctx, 0);

    if (ctx->banner[0] != '\0') {
        banner_colour = (ctx->banner_level >= 2) ? ui_c_bad()
                      : (ctx->banner_level == 1) ? ui_c_warn()
                                                 : ui_c_dim();
        /* UI_STATUS_BOTTOM (198) is where app_draw_status_top() ends,
         * leaving ~40 3DS px to the screen edge -- wrapped (never truncated
         * mid-word, see ui.h) and packed at the DS-native line step
         * (line_height 0.0f) rather than UI_LINE(UI_S_SMALL)'s roomier
         * rhythm, so up to three lines of a long banner ("That PIN didn't
         * match..." and friends, from apad_ui_strings.c) fit that budget. */
        ui_textf_wrap(8.0f, UI_STATUS_BOTTOM + 2.0f, UI_TOP_W - 16.0f,
                     UI_S_SMALL, banner_colour, UI_ALIGN_LEFT, 0.0f, "%s",
                     ctx->banner);
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

    if (s_sub == CS_PIN || s_sub == CS_PIN_ARM || s_sub == CS_PIN_APPLY) {
        ui_header(UI_BOT_W, "AtticPad -- pairing",
                  (s_sub == CS_PIN) ? "enter the PIN" : "checking...",
                  ui_c_warn());

        /* MASKED. docs/PROTOCOL.md S10 puts the PIN and a pairing key on the
         * same footing, and a secret that stays legible on a screen someone
         * else can see is not one. The length is shown because "did that
         * keypress register" is a real question on a resistive panel. */
        {
            size_t n = strlen(s_pin);
            char mask[APAD_CLIENT_SECRET_MAX + 1];
            size_t i;

            for (i = 0; i < n && i < sizeof mask - 1u; i++) {
                mask[i] = '*';
            }
            mask[i] = '\0';
            draw_field(&kPinField, "PIN SHOWN ON YOUR PC", mask, 1);
        }

        ui_button(&kPinOkBtn, (s_sub == CS_PIN) ? "PAIR (A)" : "working...",
                  0, 1);
        ui_button(&kPinCancel, "CANCEL (B)", 0, 0);
        if (s_sub != CS_PIN) {
            ui_textf_fit(UI_BOT_W * 0.5f, 108.0f, UI_S_SMALL, ui_c_warn(),
                         UI_ALIGN_CENTER, UI_BOT_W - 20.0f,
                         "Checking the PIN -- about a second");
        }
        return;
    }

    switch (s_sub) {
    case CS_DISCOVER_ARM:
    case CS_DISCOVER:
        snprintf(right, sizeof right, "searching...");
        break;
    case CS_COUNTDOWN:
        /* Integer tenths: no float arithmetic in a per-frame path. */
        snprintf(right, sizeof right, "connecting in %d.%ds",
                 ctx->reconnect_frames / 60, (ctx->reconnect_frames % 60) / 6);
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

    ui_button(&kConnectBtn, "CONNECT", s_flash_connect, 1);
    /* DSi: this box is SCAN QR, not ENTER PIN -- see this file's header,
     * point 2. PIN entry still works, via X. */
    ui_button(&kPinBtn, ctx->is_dsi ? "SCAN QR" : "ENTER PIN", s_flash_pin, 0);
    s_flash_connect = 0;
    s_flash_pin = 0;

    /* ONE LINE, NOT TWO. There is exactly one text row between the buttons
     * (y 104) and the keyboard's top edge (UI_KBD_TOP_Y, y 120), which at
     * this console's 0.8 scale is 12 physical pixels -- room for a single
     * 8px line. The 3DS has both a key-hint line and a pairing/discovery
     * line and puts them 100px apart; here they would be drawn on top of
     * each other, which is exactly what the first build did. Whichever is
     * more informative wins: a server name or a held key is news, the key
     * hints are not. */
    if (ctx->have_secret) {
        /* The one visible sign that pairing happened. Says a key is HELD, not
         * what it is. */
        ui_textf_fit(6.0f, 112.0f, UI_S_TINY, ui_c_good(), UI_ALIGN_LEFT,
                     UI_BOT_W - 12.0f, "%s",
                     apad_ui_msg(APAD_MSG_PAIRED_KEY_HELD));
    } else if (ctx->from_announce && ctx->server_name[0] != '\0') {
        ui_textf_fit(6.0f, 112.0f, UI_S_TINY, ui_c_good(), UI_ALIGN_LEFT,
                     UI_BOT_W - 12.0f, "found: %s", ctx->server_name);
    } else {
        /* CONNECT and ENTER PIN/SCAN QR are already on-screen buttons, so
         * only the two actions with no button of their own survive the
         * 2026-09-09 decluttering pass: switching which field L/R edits, and
         * exiting (B only -- there is no EXIT button on this screen). */
        ui_textf_fit(6.0f, 112.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
                     UI_BOT_W - 12.0f, "L/R: switch field   B: exit");
    }
}

const apad_screen apad_screen_connect = {
    "connect",
    connect_enter,
    connect_update,
    connect_draw_top,
    connect_draw_bottom
};
