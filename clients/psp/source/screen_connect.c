/*
 * clients/psp/source/screen_connect.c -- wi-fi wait, address entry, PIN,
 * and the handshake.
 *
 * Mirrors clients/3ds/source/screen_connect.c's substate machine. The one
 * genuinely new state is CS_PIN: the 3DS gets its pairing secret from a
 * camera and simply refuses to connect when it has none ("scan its QR code"),
 * which was never the right behaviour -- only the consequence of having no
 * keypad. This console types it.
 *
 * BLOCKING WORK SPANS FRAMES. apad_client_probe() and apad_client_connect()
 * can take seconds and the engine has no progress callback (a known gap in
 * PORTING.md's ledger). So each is split: an _ARM state draws "connecting..."
 * and the next frame actually calls it. Fixing the gap belongs in the engine,
 * not here -- but the engine is shared with a hardware-proven client, so this
 * port lives with it exactly as the 3DS does.
 */
#include <pspctrl.h>
#include <pspnet_apctl.h>

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config_psp.h"
#include "devlog.h"
#include "numpad.h"
#include "ui.h"

typedef enum {
    CS_NETWAIT = 0,   /* the radio is associating; keep drawing            */
    CS_IDLE,          /* editing the address                               */
    CS_PIN,           /* typing the six digits the PC is showing           */
    CS_CONNECT_ARM,   /* draw "connecting", then connect on the next frame */
    CS_CONNECT
} cs_sub;

typedef enum { FIELD_IP = 0, FIELD_PORT } cs_field;

static cs_sub   s_sub;
static cs_field s_focus;
static numpad   s_pad;
static int      s_boot_frames;
static int      s_attempt;               /* handshake attempts this connect */
#define CONNECT_ATTEMPTS 3
/* One-shot DISCOVER, no retransmit (S4): on an 802.11b radio that may be
 * dozing, 500 ms read a running server as absent. The DS uses 3000. */
#define PROBE_TIMEOUT_MS 1500
static int      s_slots[APAD_PSP_NET_SLOTS];   /* saved connections, 1-based */
static int      s_nslots;
static int      s_slot_ix;                     /* index into s_slots in use */

#define BOOT_WINDOW_FRAMES 150   /* ~2.5 s at 60 Hz */

static void begin_rejoin(app_ctx *ctx);
static int  rejoin_slot(const app_ctx *ctx);

static void connect_enter(app_ctx *ctx)
{
    int i;

    s_focus = FIELD_IP;
    s_boot_frames = BOOT_WINDOW_FRAMES;
    numpad_init(&s_pad);
    ctx->connected = 0;   /* a new visit to connect: the next ACTIVE edge
                           * saves again, so a changed address is persisted */

    /* Forget any PIN from the last session. Pairing here is window-only: the
     * server issues a NEW PIN for each pairing window, so a secret that
     * worked once is stale on the next connect. Keeping it made the client
     * skip the PIN prompt and silently reuse the dead key -- the server then
     * dropped every tagged packet and the session idle-timed-out with
     * rx=1 (2026-09-11: "when I try to pair with a new pin it doesn't work
     * and the console doesn't ask for a pin"). Clearing it here makes the
     * probe below prompt for the current PIN every time pairing is required.
     * A truly persistent, remembered pairing is the 0.7 plan, not this. */
    if (ctx->have_secret) {
        if (ctx->client != NULL) {
            (void)apad_client_set_secret(ctx->client, NULL);
        }
        ctx->have_secret = 0;
        ctx->pin_text[0] = '\0';
        apad_devlog("cleared last session's PIN -- will re-prompt if paired");
    }
    s_nslots  = apad_psp_net_slots(s_slots, APAD_PSP_NET_SLOTS);
    s_slot_ix = 0;
    for (i = 0; i < s_nslots; i++) {
        if (s_slots[i] == ctx->net.slot) { s_slot_ix = i; }
    }

    /* Three ways to arrive here: fresh boot (still associating), a clean
     * disconnect (link fine), or a session that died because Wi-Fi went
     * away. Tell them apart by the live radio state, not by the stale
     * net.ready flag from the last successful bring-up. */
    if (apad_psp_net_associated()) {
        s_sub = CS_IDLE;                 /* link is up: edit / connect */
    } else if (ctx->net.ready) {
        begin_rejoin(ctx);               /* had an IP, lost it: rejoin */
    } else {
        s_sub = CS_NETWAIT;              /* boot: the bring-up is still running */
    }
}

/* The name of the saved network in use, for the wait panel. */
static const char *net_label(const app_ctx *ctx)
{
    return (ctx->net.name[0] != '\0') ? ctx->net.name : "saved network";
}

/* The saved-network slot to (re)join: the one in use, else the current pick,
 * else slot 1. */
static int rejoin_slot(const app_ctx *ctx)
{
    if (ctx->net.slot > 0) { return ctx->net.slot; }
    if (s_nslots > 0)      { return s_slots[s_slot_ix]; }
    return 1;
}

/* Bring the radio back up and wait on it. Called when the link has dropped:
 * a session that just idle-timed-out, or a connect that got no answer while
 * the address is known good, is a de-associated radio far more often than a
 * dead server. The DS client does the same (screen_connect.c: "Wi-Fi
 * dropped -- rejoining"). */
static void begin_rejoin(app_ctx *ctx)
{
    apad_devlog("wifi de-associated -- rejoining slot %d", rejoin_slot(ctx));
    (void)apad_psp_net_start(rejoin_slot(ctx));   /* no-op if already busy */
    s_sub = CS_NETWAIT;
    s_attempt = 0;
    app_note(ctx, 1, "Wi-Fi dropped -- rejoining");
}

static const char *state_word(int state)
{
    switch (state) {
    case 1:  return "scanning";
    case 2:  return "joining";
    case 3:  return "getting an address";
    case 5:
    case 6:  return "key exchange";
    default: return "starting";
    }
}

static char *focused_field(app_ctx *ctx, size_t *cap)
{
    if (s_focus == FIELD_PORT) {
        *cap = sizeof ctx->port_text;
        return ctx->port_text;
    }
    *cap = sizeof ctx->ip_text;
    return ctx->ip_text;
}

/* Type one character into a buffer, or delete. Editing the ADDRESS forgets
 * any secret, in the engine as well as here: a key that was right for one
 * server presents as "wrong PIN" against another, which is a maddening thing
 * to debug. clients/3ds/source/screen_connect.c's field_type() does the
 * same pair. */
static void field_type(app_ctx *ctx, char c)
{
    size_t cap, n;
    char  *buf;

    if (s_sub == CS_PIN) {
        buf = ctx->pin_text;
        cap = sizeof ctx->pin_text;
    } else {
        buf = focused_field(ctx, &cap);
    }
    n = strlen(buf);

    if (c == '\b') {
        if (n > 0) { buf[n - 1] = '\0'; }
    } else if (n + 1 < cap) {
        buf[n]     = c;
        buf[n + 1] = '\0';
    }

    if (s_sub != CS_PIN && ctx->have_secret) {
        (void)apad_client_set_secret(ctx->client, NULL);
        ctx->have_secret = 0;
    }
}

static uint16_t parse_port(const app_ctx *ctx)
{
    unsigned long v = 0;
    const char *p;

    for (p = ctx->port_text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') { return 0; }
        v = v * 10u + (unsigned long)(*p - '0');
        if (v > 65535u) { return 0; }
    }
    return (uint16_t)v;
}

/* One connect attempt: probe first (S7 tier 3), then handshake. */
static apad_screen_id do_connect(app_ctx *ctx)
{
    uint16_t port = parse_port(ctx);
    int rc;

    if (ctx->ip_text[0] == '\0' || port == 0) {
        app_note(ctx, 2, "enter an address and port first");
        return APAD_SCREEN_CONNECT;
    }

    /* The probe answers "does this server want a PIN?" before a HELLO is
     * sent, so a client with no secret never takes a pad slot it must then
     * let lapse. */
    apad_devlog("connect: probing %s:%u", ctx->ip_text, (unsigned)port);
    if (s_attempt == 0
        && apad_client_probe(ctx->client, ctx->ip_text, port, PROBE_TIMEOUT_MS) == APAD_OK) {
        apad_client_get_stats(ctx->client, &ctx->stats);
        if (ctx->stats.pairing_required == 1 && !ctx->have_secret) {
            app_note(ctx, 0, "this server is paired -- type the PIN it shows");
            ctx->pin_text[0] = '\0';
            s_sub = CS_PIN;
            numpad_init(&s_pad);
            return APAD_SCREEN_CONNECT;
        }
    }

    s_attempt++;
    rc = apad_client_connect(ctx->client, ctx->ip_text, port, 60, 5000);
    apad_devlog("connect: attempt %d -> %d (PIN key derive %u ms)",
                s_attempt, rc, (unsigned)ctx->stats.derive_ms);
    app_disarm(ctx);            /* the button that started this was held */
    apad_client_get_stats(ctx->client, &ctx->stats);

    /* One attempt is five HELLOs over 2.3 s (S9). A radio that dozes can
     * miss all five; a PC's never does. Try again, up to three times, with a
     * frame drawn in between so the count is visible -- and only for the
     * no-answer case: a refused PIN is an answer. */
    if (rc == APAD_ERR_STATE && s_attempt < CONNECT_ATTEMPTS) {
        s_sub = CS_CONNECT_ARM;
        return APAD_SCREEN_CONNECT;
    }

    if (rc == APAD_OK) {
        ctx->banner[0] = '\0';
        s_attempt = 0;
        return APAD_SCREEN_SESSION;
    }
    s_attempt = 0;

    if (rc == APAD_ERR_AUTH) {
        /* The engine has already ACKed and closed locally; nothing is stuck.
         * Forget the wrong key in the ENGINE too -- leaving it installed
         * makes the next attempt derive the same wrong session key and burn
         * another try against the server's lockout counter. */
        (void)apad_client_set_secret(ctx->client, NULL);
        ctx->have_secret = 0;
        ctx->pin_text[0] = '\0';
        app_note(ctx, 2, "that PIN was not accepted -- check the PC and retry");
        s_sub = CS_PIN;
        numpad_init(&s_pad);
        return APAD_SCREEN_CONNECT;
    }

    /* No answer AND the radio has dropped its IP: the AP went away
     * mid-use, not the server. Bring the link back rather than telling the
     * person the server is unreachable, which it may not be. */
    if (!apad_psp_net_associated()) {
        begin_rejoin(ctx);
        return APAD_SCREEN_CONNECT;
    }
    app_note(ctx, 2, "could not connect to %s:%u", ctx->ip_text, (unsigned)port);
    s_sub = CS_IDLE;
    return APAD_SCREEN_CONNECT;
}

static apad_screen_id connect_update(app_ctx *ctx)
{
    /* The boot window runs CONCURRENTLY with the wi-fi wait: the self-test
     * needs no network, so making someone wait for a radio to run it would
     * be backwards. */
    if (s_boot_frames > 0) {
        s_boot_frames--;
        if ((ctx->keys_held & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_START))
            == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_START)) {
            ctx->selftest_return = APAD_SCREEN_CONNECT;
            return APAD_SCREEN_SELFTEST;
        }
    }
    if (app_pressed(ctx, PSP_CTRL_SELECT)) {
        ctx->selftest_return = APAD_SCREEN_CONNECT;
        return APAD_SCREEN_SELFTEST;
    }

    apad_psp_net_get(&ctx->net);
    if (ctx->net.failed && !ctx->net.joinfail) {
        snprintf(ctx->fatal, sizeof ctx->fatal, "%s returned 0x%08X",
                 (ctx->net.stage != NULL) ? ctx->net.stage : "network bring-up",
                 (unsigned)ctx->net.err);
        return APAD_SCREEN_FATAL;
    }
    if (ctx->net.failed && ctx->net.joinfail && s_sub == CS_NETWAIT) {
        /* The access point said no (or slot 1 is empty). Not fatal: the
         * console may hold another saved network that will say yes. X
         * tries the same one again, L / R move through the saved ones. */
        int moved = 0;
        if (app_pressed(ctx, PSP_CTRL_LTRIGGER) && s_nslots > 1) {
            s_slot_ix = (s_slot_ix + s_nslots - 1) % s_nslots; moved = 1;
        } else if (app_pressed(ctx, PSP_CTRL_RTRIGGER) && s_nslots > 1) {
            s_slot_ix = (s_slot_ix + 1) % s_nslots; moved = 1;
        }
        if (moved || app_pressed(ctx, PSP_CTRL_CROSS)) {
            (void)apad_psp_net_start(s_nslots ? s_slots[s_slot_ix] : 1);
        }
        return APAD_SCREEN_CONNECT;
    }

    switch (s_sub) {
    case CS_NETWAIT:
        if (ctx->net.ready) {
            /* The AP that actually came up is worth remembering: next boot
             * tries it first instead of failing through the dead slots. Saved
             * on radio-up, not on a server session (a user may never reach
             * one), the way the DS saves its network on ASSOCIATED. */
            apad_psp_config_save_slot(ctx->net.slot);
            /* The radio is up, so the shim's sockets can exist now. The
             * engine opens its own socket in create(), which is why this
             * waits rather than running at startup. */
            if (ctx->client == NULL) {
                if (apad_net_init() != APAD_OK) {
                    snprintf(ctx->fatal, sizeof ctx->fatal,
                             "apad_net_init() failed after the radio came up");
                    return APAD_SCREEN_FATAL;
                }
                ctx->client = apad_client_create("AtticPad PSP", ctx->caps);
                apad_devlog("engine created: %s (caps 0x%04X)",
                            (ctx->client != NULL) ? "ok" : "FAILED",
                            (unsigned)ctx->caps);
                if (ctx->client == NULL) {
                    snprintf(ctx->fatal, sizeof ctx->fatal,
                             "apad_client_create() failed -- no socket");
                    return APAD_SCREEN_FATAL;
                }
            }
            s_sub = CS_IDLE;
            if (ctx->want_reconnect && ctx->ip_text[0] != '\0') {
                /* The radio is back after a suspend and a session was live
                 * when the console went away: go straight back to it. Same
                 * path a button press takes, once (app.h). */
                ctx->want_reconnect = 0;
                s_attempt = 0;
                s_sub = CS_CONNECT_ARM;
                apad_devlog("resume: radio back -- reconnecting to %s:%s",
                            ctx->ip_text, ctx->port_text);
            }
#ifdef APAD_AUTO_HOST
            {   /* dev only: connect without waiting for a button nobody can
                 * press in a headless run. Once, so a failure does not spin. */
                static int once;
                if (!once) { once = 1; s_sub = CS_CONNECT_ARM; }
            }
#endif
        }
        return APAD_SCREEN_CONNECT;

    case CS_CONNECT_ARM:
        s_sub = CS_CONNECT;             /* draw "connecting" first */
        return APAD_SCREEN_CONNECT;

    case CS_CONNECT:
        return do_connect(ctx);

    case CS_PIN:
        numpad_move(&s_pad, ctx->keys_down);
        if (app_pressed(ctx, PSP_CTRL_CROSS)) {
            field_type(ctx, numpad_char(&s_pad));
        }
        if (app_pressed(ctx, PSP_CTRL_CIRCLE)) {
            if (ctx->pin_text[0] == '\0') {
                s_sub = CS_IDLE;        /* empty + back = give up on the PIN */
            } else {
                field_type(ctx, '\b');
            }
        }
        if (app_pressed(ctx, PSP_CTRL_TRIANGLE | PSP_CTRL_START)) {
            if (ctx->pin_text[0] == '\0') {
                app_note(ctx, 1, "type the PIN shown on the PC first");
            } else if (apad_client_set_secret(ctx->client, ctx->pin_text) != APAD_OK) {
                app_note(ctx, 2, "that PIN is too long");
            } else {
                /* The engine copied it and owns the only copy that should
                 * outlive this call. */
                memset(ctx->pin_text, 0, sizeof ctx->pin_text);
                ctx->have_secret = 1;
                s_sub = CS_CONNECT_ARM;
            }
        }
        return APAD_SCREEN_CONNECT;

    case CS_IDLE:
    default:
        numpad_move(&s_pad, ctx->keys_down);
        if (app_pressed(ctx, PSP_CTRL_CROSS))  { field_type(ctx, numpad_char(&s_pad)); }
        if (app_pressed(ctx, PSP_CTRL_CIRCLE)) { field_type(ctx, '\b'); }
        if (app_pressed(ctx, PSP_CTRL_SQUARE | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) {
            s_focus = (s_focus == FIELD_IP) ? FIELD_PORT : FIELD_IP;
        }
        if (app_pressed(ctx, PSP_CTRL_TRIANGLE | PSP_CTRL_START)) {
            s_sub = CS_CONNECT_ARM;
        }
        return APAD_SCREEN_CONNECT;
    }
}

static void draw_field(int x, int y, int w, const char *label,
                       const char *value, int focused)
{
    ui_box f;

    ui_text(x, y, ui_c_dim(), UI_ALIGN_LEFT, label);
    f.x = x; f.y = y + 10; f.w = w; f.h = 18;
    ui_panel(&f, ui_c_bg(), focused ? ui_c_accent() : ui_c_border());
    ui_textf_fit(f.x + 6, f.y + 5, ui_c_text(), UI_ALIGN_LEFT, w - 12,
                 "%s%s", value, focused ? "_" : "");
}

static void connect_draw(app_ctx *ctx)
{
    int y = app_draw_status(ctx, "AtticPad -- connect", 0);
    int i, step;

    if (s_sub == CS_NETWAIT) {
        ui_box b;
        b.x = 6; b.y = y; b.w = UI_W - 12; b.h = 80;
        ui_panel(&b, ui_c_panel(), ui_c_border());
        ui_textf_fit(b.x + 12, b.y + 10, ui_c_dim(), UI_ALIGN_LEFT, b.w - 24,
                     "WI-FI   %s   (%d of %d saved)", net_label(ctx),
                     s_nslots ? s_slot_ix + 1 : 0, s_nslots);
        if (ctx->net.failed) {
            ui_textf_fit(b.x + 12, b.y + 26, ui_c_bad(), UI_ALIGN_LEFT, b.w - 24,
                         s_nslots ? "could not join %s" : "no saved Wi-Fi connection on this PSP",
                         net_label(ctx));
            ui_text(b.x + 12, b.y + 44, ui_c_dim(), UI_ALIGN_LEFT,
                    "PSPs join WEP or WPA (not WPA2-only) over 802.11b");
            ui_hint(b.x + 12, b.y + 59, UI_GLYPH_CROSS, ui_c_text(), "try again");
            if (s_nslots > 1) {
                ui_text(b.x + 12 + 110, b.y + 60, ui_c_text(), UI_ALIGN_LEFT,
                        "L / R  another saved network");
            }
        } else {
            /* 0..4 as the SDK numbers them; the WPA key-exchange states 5
             * and 6 sit between joining and getting an address. */
            int st = ctx->net.state;
            step = (st < 0) ? 0 : (st > 4) ? 2 : st;
            ui_textf(b.x + 12, b.y + 26, ui_c_accent(), UI_ALIGN_LEFT,
                     "%s", state_word(st));
            for (i = 0; i < 4; i++) {
                ui_box c;
                c.x = b.x + 12 + i * 46; c.y = b.y + 44; c.w = 40; c.h = 10;
                ui_panel(&c, (i < step) ? ui_c_accent() : ui_c_bg(), ui_c_border());
            }
            ui_text(b.x + 12, b.y + 60, ui_c_dim(), UI_ALIGN_LEFT,
                    "the self-test works without a network");
        }
    } else if (s_sub == CS_CONNECT_ARM || s_sub == CS_CONNECT) {
        ui_box b;
        b.x = 6; b.y = y; b.w = UI_W - 12; b.h = 80;
        ui_panel(&b, ui_c_panel(), ui_c_accent());
        ui_textf(UI_W / 2, b.y + 32, ui_c_accent(), UI_ALIGN_CENTER,
                 "connecting to %s...", ctx->ip_text);
        if (s_attempt > 0) {
            ui_textf(UI_W / 2, b.y + 48, ui_c_dim(), UI_ALIGN_CENTER,
                     "no answer yet -- attempt %d of %d", s_attempt + 1, CONNECT_ATTEMPTS);
        } else {
            ui_text(UI_W / 2, b.y + 48, ui_c_dim(), UI_ALIGN_CENTER,
                    "this can take a few seconds");
        }
    } else if (s_sub == CS_PIN) {
        char masked[16];
        size_t n = strlen(ctx->pin_text), k;

        for (k = 0; k < n && k < sizeof masked - 1; k++) { masked[k] = ctx->pin_text[k]; }
        masked[k] = '\0';
        /* Shown, not masked: the PIN is on the PC's screen two metres away,
         * and typing blind on a d-pad grid buys nothing. */
        draw_field(6, y, 150, "PIN FROM THE PC", masked, 1);
        ui_text(166, y + 16, ui_c_dim(), UI_ALIGN_LEFT, "six digits");
        numpad_draw(&s_pad, 6, y + 34, 0);
        ui_hint(166, y + 39, UI_GLYPH_CROSS,    ui_c_dim(), "type");
        ui_hint(166, y + 51, UI_GLYPH_CIRCLE,   ui_c_dim(), "delete / back");
        ui_hint(166, y + 63, UI_GLYPH_TRIANGLE, ui_c_dim(), "submit");
    } else {
        draw_field(6,   y, 150, "SERVER ADDRESS", ctx->ip_text,  s_focus == FIELD_IP);
        draw_field(166, y,  70, "PORT",           ctx->port_text, s_focus == FIELD_PORT);
        numpad_draw(&s_pad, 6, y + 34, s_focus == FIELD_IP);
        ui_hint(166, y + 39, UI_GLYPH_CROSS,    ui_c_dim(), "type");
        ui_hint(166, y + 51, UI_GLYPH_CIRCLE,   ui_c_dim(), "delete");
        ui_hint(166, y + 63, UI_GLYPH_SQUARE,   ui_c_dim(), "L  R   field");
        ui_hint(166, y + 75, UI_GLYPH_TRIANGLE, ui_c_dim(), "connect");
    }

    if (ctx->banner[0] != '\0') {
        uint32_t c = (ctx->banner_level >= 2) ? ui_c_bad()
                   : (ctx->banner_level == 1) ? ui_c_warn() : ui_c_dim();
        ui_textf_fit(6, UI_H - 26, c, UI_ALIGN_LEFT, UI_W - 12, "%s", ctx->banner);
    }
    ui_rect(0, UI_H - 14, UI_W, 1, ui_c_border());
    ui_text(6, UI_H - 11, ui_c_dim(), UI_ALIGN_LEFT,
            (s_boot_frames > 0)
                ? "L+R+START or SELECT: self-test   HOME: quit"
                : "SELECT: self-test   HOME: quit");
}

const apad_screen apad_screen_connect = {
    "connect", connect_enter, connect_update, connect_draw
};
