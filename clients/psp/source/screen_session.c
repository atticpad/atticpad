/*
 * clients/psp/source/screen_session.c -- the live session.
 *
 * Mirrors clients/3ds/source/screen_session.c: pump the engine once per
 * frame with this frame's input, show what the server is being told, and
 * leave cleanly when asked.
 *
 * The status band above is drawn by app_draw_status() and is the same on
 * every screen, so the round-trip figure and the input lamps a person learned
 * on the connect screen mean the same thing here.
 */
#include <pspctrl.h>

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config_psp.h"
#include "devlog.h"
#include "net_psp.h"
#include "ui.h"

/* A confirm step on disconnect, for the same reason the 3DS has one: START
 * and SELECT are also gameplay buttons, and tearing down a live session by
 * brushing one is worse than one extra press. */
static int s_confirm;        /* the yes/no dialog is up */

#define DISCONNECT_HOLD_FRAMES 21   /* ~0.35 s at 60 Hz: deliberate, not slow */
static int s_combo_frames;   /* frames L+R+START has been held, for disconnect */

static void session_enter(app_ctx *ctx)
{
    s_confirm = 0;
    s_combo_frames = 0;
    (void)ctx;
}

static apad_screen_id session_update(app_ctx *ctx)
{
    int rc;

    /* One pump per frame with this frame's input. 16 ms is the frame budget:
     * the engine uses it to wait for a reply rather than spinning. */
    rc = apad_client_pump(ctx->client, &ctx->st, 16);
    apad_client_get_stats(ctx->client, &ctx->stats);

    if (rc == APAD_CLIENT_ACTIVE && !ctx->connected) {
        /* The 0->1 ACTIVE edge: S8's handshake just finished, so this
         * address is proven, not merely typed. Persist ip/port once (not
         * every frame of a long session, and never a mistyped address that
         * never connected). The pairing secret never reaches config_psp.c.
         * The connect screen already saved the network slot on radio-up. */
        apad_psp_config_save_server(ctx->ip_text, ctx->port_text);
        ctx->connected = 1;
    }

    if (rc == APAD_CLIENT_CLOSED) {
        /* The one line that tells apart the two ways a session dies here:
         * the AP went away (associated == 0, and the reconnect will rejoin),
         * or the link held but packets stopped (associated == 1, a signal
         * fade or the server). Written to ms0:/atticpad.log in the diag
         * build so a drop on hardware is diagnosable after the fact. */
        apad_devlog("session closed: reason=%d wifi_associated=%d rtt=%d tx=%u rx=%u",
                    (int)ctx->stats.close_reason, apad_psp_net_associated(),
                    (int)ctx->stats.rtt_ms,
                    (unsigned)ctx->stats.tx_packets,
                    (unsigned)ctx->stats.rx_packets);
        /* apad_ui_status_message() would word this; until the engine's
         * message catalog grows a PSP-shaped network string (see the port
         * notes) the close reason is reported plainly rather than guessed. */
        switch (ctx->stats.close_reason) {
        case APAD_CLOSE_IDLE_TIMEOUT:
            app_note(ctx, 1, "session ended: nothing from the server for 3 s");
            break;
        case APAD_CLOSE_RETX_FAILED:
            app_note(ctx, 1, "session ended: the server stopped answering");
            break;
        case APAD_CLOSE_PEER_BYE:
            app_note(ctx, 1, "the server closed the session");
            break;
        case APAD_CLOSE_PEER_ERROR:
            app_note(ctx, 1, "the server rejected the session");
            break;
        default:
            app_note(ctx, 1, "session ended");
            break;
        }
        return APAD_SCREEN_CONNECT;
    }

    /* Once the hold has filled, the yes/no dialog is up: X confirms, O backs
     * out. app_disarm() on the way out so the SELECT still held from the
     * chord is not read as a fresh press by the connect screen. */
    if (s_confirm) {
        if (app_pressed(ctx, PSP_CTRL_CROSS)) {
            apad_client_disconnect(ctx->client);   /* a real BYE: the server
                                                    * frees the slot now, not
                                                    * on timeout */
            app_note(ctx, 0, "disconnected");
            app_disarm(ctx);
            s_confirm = 0;
            return APAD_SCREEN_CONNECT;
        }
        if (app_pressed(ctx, PSP_CTRL_CIRCLE)) { s_confirm = 0; }
        return APAD_SCREEN_SESSION;
    }

    /* Disconnect is a deliberate CHORD held to a fill, then confirmed. SELECT
     * alone tore the session down mid-game (user, 2026-09-11); L+R+START then
     * collided with the self-test chord, which the connect screen re-triggered
     * from the still-held buttons on the way back. So the chord is L+R+SELECT
     * -- not the self-test's L+R+START -- held ~0.35 s while a bar fills
     * (session_draw shows it, so the hold visibly registers), then the yes/no
     * dialog. Release early cancels. The buttons still reach the PC while held. */
    {
        const unsigned chord = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT;
        if ((ctx->keys_held & chord) == chord) {
            s_combo_frames++;
            if (s_combo_frames >= DISCONNECT_HOLD_FRAMES) {
                s_confirm = 1;
                s_combo_frames = 0;
            }
        } else {
            s_combo_frames = 0;
        }
    }
    return APAD_SCREEN_SESSION;
}

static void session_draw(app_ctx *ctx)
{
    ui_box b;
    int    y = app_draw_status(ctx, "AtticPad -- session", 1);

    b.x = 6; b.y = y; b.w = UI_W - 12; b.h = 80;
    ui_panel(&b, ui_c_panel(), ui_c_border());

    ui_text(b.x + 12, b.y + 8, ui_c_dim(), UI_ALIGN_LEFT, "SESSION");
    ui_textf(b.x + 12, b.y + 24, ui_c_text(), UI_ALIGN_LEFT,
             "pad slot %d   %u Hz", (int)ctx->stats.pad_slot,
             (unsigned)ctx->stats.input_rate_hz);
    ui_textf(b.x + 12, b.y + 38, ui_c_dim(), UI_ALIGN_LEFT,
             "tx %u   rx %u", (unsigned)ctx->stats.tx_packets,
             (unsigned)ctx->stats.rx_packets);

    if (apad_client_message(ctx->client) != NULL
        && apad_client_message(ctx->client)[0] != '\0') {
        ui_textf_fit(b.x + 12, b.y + 56, ui_c_warn(), UI_ALIGN_LEFT, b.w - 24,
                     "%s", apad_client_message(ctx->client));
    }

    if (s_confirm) {
        /* The confirmation, after the hold filled. */
        ui_box c;
        c.x = 60; c.y = UI_H / 2 - 26; c.w = UI_W - 120; c.h = 52;
        ui_panel(&c, ui_c_panel_hi(), ui_c_warn());
        ui_text(UI_W / 2, c.y + 12, ui_c_text(), UI_ALIGN_CENTER,
                "Disconnect from the server?");
        ui_hint(UI_W / 2 - 60, c.y + 29, UI_GLYPH_CROSS,  ui_c_dim(), "yes");
        ui_hint(UI_W / 2 + 14, c.y + 29, UI_GLYPH_CIRCLE, ui_c_dim(), "no");
    } else if (s_combo_frames > 0) {
        /* While the chord is held, fill a bar so the hold visibly registers
         * (a blind hold read as "not working"). Release to cancel. */
        ui_box c;
        int fill;
        c.x = 60; c.y = UI_H / 2 - 24; c.w = UI_W - 120; c.h = 48;
        ui_panel(&c, ui_c_panel_hi(), ui_c_warn());
        ui_text(UI_W / 2, c.y + 8, ui_c_text(), UI_ALIGN_CENTER,
                "disconnecting -- keep holding");
        fill = (c.w - 20) * s_combo_frames / DISCONNECT_HOLD_FRAMES;
        if (fill > c.w - 20) { fill = c.w - 20; }
        ui_rect(c.x + 10, c.y + 26, c.w - 20, 10, ui_c_bg());
        ui_rect(c.x + 10, c.y + 26, fill, 10, ui_c_accent());
    }

    ui_rect(0, UI_H - 14, UI_W, 1, ui_c_border());
    ui_text(6, UI_H - 11, ui_c_dim(), UI_ALIGN_LEFT,
            "hold L+R+SELECT: disconnect   HOME: quit");
}

const apad_screen apad_screen_session = {
    "session", session_enter, session_update, session_draw
};
