/*
 * clients/nds/source/main.c -- AtticPad Nintendo DS / DSi client.
 *
 * clients/3ds/source/main.c, ported. The app shell and nothing else: bring-up,
 * one hardware sample per frame into an apad_input_state, and the frame loop
 * that dispatches to whichever screen is current. It owns no protocol and no
 * layout.
 *
 *   - Everything protocol-shaped goes through the SHARED client engine,
 *     clients/common/apad_client.{c,h}: the S8 handshake, the S9
 *     ACK-every-copy rule, both directions of S6.6 PING, the retransmit
 *     timers, the S10 auth state machine, the S6.15-S6.19 KBM gate and the
 *     INPUT_STATE pacing. There is no protocol code in this file and there
 *     must never be -- docs/DESIGN.md S7.2. The one part of libapad called directly
 *     from here is tier-2 broadcast discovery (app_discover): it needs a
 *     broadcast socket of its own, and the engine's socket is the session's.
 *   - Everything screen-shaped lives in screen_*.c behind the table in app.h.
 *   - Everything drawing-shaped lives in ui.c / ui_widgets.c.
 *
 * DSWiFi bring-up lives in wifi_nds.c (mirrored from
 * references/nds/examples/dswifi/full_ap_demo/), the SD card in config_nds.c,
 * text entry in kbd_nds.c, the millisecond clock in time_nds.c. The actual BSD
 * socket calls come from the SHARED shim/net_bsd.c (docs/DESIGN.md S3), which
 * compiles and runs on this platform unchanged -- see references/nds/README.md
 * for the compile/link/run evidence.
 *
 * =========================================================================
 * FOUR THINGS THIS FILE DOES DIFFERENTLY FROM ITS 3DS ORIGINAL
 * =========================================================================
 *
 * 1. cothread_yield_irq(IRQ_VBLANK), NEVER swiWaitForVBlank(). DSWiFi runs
 *    lwIP in its own cothread; swiWaitForVBlank() HALTs the ARM9 with that
 *    cothread behind it. The yield lives inside ui_frame_begin() (ui.c), which
 *    is the frame loop's only wait -- every path that blocks for longer than a
 *    frame either draws frames as it goes (screen_selftest.c) or is bounded
 *    and announced first (app.h's _ARM convention).
 *
 * 2. THE SESSION ENGINE IS CREATED AFTER ASSOCIATION, not during bring-up.
 *    apad_client_create() opens a UDP socket and lwIP has no interface to bind
 *    one to until the console has actually joined an access point. So
 *    bring-up brings the RADIO up and screen_wifi.c calls app_client_start()
 *    the moment it reaches ASSOCIATED.
 *
 * 3. NEVER HAND A BROADCAST ADDRESS TO THE ENGINE. On DSWiFi's lwIP, sendto()
 *    to 255.255.255.255 WITHOUT SO_BROADCAST does not return EACCES and does
 *    not return -1: the calling thread wedges inside lwIP forever while the
 *    console keeps drawing at 60fps, so it does not even look like a crash.
 *    apad_client_probe() sends on the engine's socket, which has no broadcast
 *    option and no way to be given one, so app_discover() below opens its own
 *    and calls apad_udp_set_broadcast() first -- exactly as the 3DS's
 *    app_discover() does, for a reason that is merely tidy there and fatal
 *    here.
 *
 * 4. NO APT. There is no applet manager on a DS: no aptMainLoop(), no
 *    suspend/restore hooks, no "the OS ordered this app to close" path. The
 *    loop ends when the user asks it to.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "app.h"
#include "config_nds.h"
#include "kbd_nds.h"
#include "time_nds.h"
#include "wifi_nds.h"

/* ------------------------------------------------------------------------ */
/* the screen table                                                         */
/* ------------------------------------------------------------------------ */

static const apad_screen *const kScreens[APAD_SCREEN_COUNT] = {
    &apad_screen_wifi,
    &apad_screen_connect,
    &apad_screen_qrscan,
    &apad_screen_session,
    &apad_screen_selftest,
    &apad_screen_fatal
};

const char *app_screen_name(apad_screen_id id)
{
    /* Cast because apad_screen_id is an enum with no negative enumerators, so
     * gcc knows `id < 0` can never hold and -Wtype-limits says so -- on THIS
     * ABI in particular, where an all-non-negative enum is unsigned. */
    if ((unsigned)id >= (unsigned)APAD_SCREEN_COUNT) {
        return "?";
    }
    return kScreens[id]->name;
}

/* ------------------------------------------------------------------------ */
/* identity                                                                 */
/* ------------------------------------------------------------------------ */

/* S6.3 capabilities. What this console physically has and nothing else: a
 * D-pad, four face buttons, two shoulders, a touchscreen. No sticks, no
 * triggers, no gyro, no rumble, no LED. APAD_CAP_BATTERY is added at runtime
 * only in DSi mode -- see battery_byte(). */
#define APAD_NDS_CAPS_BASE (APAD_CAP_DPAD | APAD_CAP_FACE4 | \
                            APAD_CAP_SHOULDER | APAD_CAP_TOUCH)

/* The server matches its profile on this exact string
 * (server/profiles/ds-default.jsonc). "AtticPad DSi" contains "AtticPad DS"
 * as a substring, so one profile covers both; note that a bare "DS" would
 * also match "AtticPad 3DS", which is why neither the client nor the profile
 * uses one. */
#define APAD_NDS_NAME_DS  "AtticPad DS"
#define APAD_NDS_NAME_DSI "AtticPad DSi"

/* ------------------------------------------------------------------------ */
/* battery                                                                  */
/* ------------------------------------------------------------------------ */

/* S5.5: 0..100, or 255 for unknown.
 *
 * getBatteryLevel() returns bits 0-3 as a level and bit 7 as "charger
 * connected" (nds/system.h). On a DS or DS Lite that level is ONE BIT of real
 * information -- libnds reports 15 for "high" and 3 for "low" because the
 * hardware has nothing finer -- so reporting it as a percentage would be
 * inventing six bits that do not exist. DS mode therefore sends 255 and
 * APAD_CAP_BATTERY is not claimed at all, which is S5.5's own way of saying
 * "this device has no battery reading": absence is the capability bit, not a
 * sentinel in the field.
 *
 * UNVERIFIED: the DSi mapping below. The DSi reports a real 0-15 level, but
 * which physical charge each step corresponds to is not documented in
 * nds/system.h and is not linear on the published hardware datasheets. A
 * linear level*100/15 is the honest first approximation and is flagged as
 * such rather than tuned from memory. */
static int s_battery = -1;

static uint8_t battery_byte(void)
{
    u32 raw;

    if (!isDSiMode()) {
        return (uint8_t)APAD_BATTERY_UNKNOWN;
    }
    raw = getBatteryLevel() & BATTERY_LEVEL_MASK;
    if (raw > 15u) {
        raw = 15u;
    }
    return (uint8_t)((raw * 100u) / 15u);
}

int app_battery_percent(void)
{
    return s_battery;
}

/* ------------------------------------------------------------------------ */
/* frame time                                                               */
/* ------------------------------------------------------------------------ */

static uint32_t s_frame_prev_ms;
static uint32_t s_frame_accum_ms;
static int      s_frame_count;
static int      s_frame_avg_ms = 17;

static void frame_time_sample(void)
{
    uint32_t now = apad_ticks_ms();

    if (s_frame_prev_ms != 0u) {
        s_frame_accum_ms += apad_time_since(now, s_frame_prev_ms);
        s_frame_count++;
        if (s_frame_count >= 32) {
            s_frame_avg_ms = (int)(s_frame_accum_ms / 32u);
            s_frame_accum_ms = 0u;
            s_frame_count = 0;
        }
    }
    s_frame_prev_ms = now;
}

int app_frame_ms(void)
{
    return s_frame_avg_ms;
}

/* PER-PHASE TIMING, added for the frame-rate pass (2026-09-09): app_frame_ms()
 * above says the whole loop is slow but not WHERE, and "measure, don't guess"
 * is this platform's own rule (the ARM-vs-Thumb and screen-clear experiments
 * that changed nothing before this). Five numbers, matching the five things
 * main()'s loop actually does in order: sampling hardware, running the
 * current screen's update() (which is where apad_client_pump_ex() lives on
 * the session screen), the two draw calls, and the VBlank wait/flip
 * (ui_frame_begin()). Same 32-frame rolling average as app_frame_ms(), same
 * reason: a single frame's number is too noisy from IRQ/DMA jitter to read
 * off a screenshot. Shown on the session screen's existing diag panel
 * (SELECT), never gated separately -- it is a reading, not a toggle of its
 * own. */
static app_frame_phases s_phase_avg;
static app_frame_phases s_phase_accum;
static int              s_phase_count;

static void phase_time_sample(uint32_t sample_ms, uint32_t update_ms,
                              uint32_t draw_top_ms, uint32_t draw_bottom_ms,
                              uint32_t flip_wait_ms, uint32_t flip_swap_ms)
{
    s_phase_accum.sample_ms      += sample_ms;
    s_phase_accum.update_ms      += update_ms;
    s_phase_accum.draw_top_ms    += draw_top_ms;
    s_phase_accum.draw_bottom_ms += draw_bottom_ms;
    s_phase_accum.flip_wait_ms   += flip_wait_ms;
    s_phase_accum.flip_swap_ms   += flip_swap_ms;
    s_phase_count++;
    if (s_phase_count >= 32) {
        s_phase_avg.sample_ms      = s_phase_accum.sample_ms / 32u;
        s_phase_avg.update_ms      = s_phase_accum.update_ms / 32u;
        s_phase_avg.draw_top_ms    = s_phase_accum.draw_top_ms / 32u;
        s_phase_avg.draw_bottom_ms = s_phase_accum.draw_bottom_ms / 32u;
        s_phase_avg.flip_wait_ms   = s_phase_accum.flip_wait_ms / 32u;
        s_phase_avg.flip_swap_ms   = s_phase_accum.flip_swap_ms / 32u;
        memset(&s_phase_accum, 0, sizeof s_phase_accum);
        s_phase_count = 0;
    }
}

const app_frame_phases *app_frame_phases_ms(void)
{
    return &s_phase_avg;
}

/* The S5.5 field. Written as a function rather than inline at the two call
 * sites because APAD_BATTERY_UNKNOWN is an unsigned literal and s_battery is
 * a signed int, and a `?:` between the two changes signedness -- which this
 * toolchain rejects under -Werror=sign-compare. */
static uint8_t battery_wire(void)
{
    if (s_battery < 0) {
        return (uint8_t)APAD_BATTERY_UNKNOWN;
    }
    return (uint8_t)s_battery;
}

/* ------------------------------------------------------------------------ */
/* cross-screen messaging                                                   */
/* ------------------------------------------------------------------------ */

void app_note(app_ctx *ctx, int level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(ctx->banner, sizeof ctx->banner, fmt, ap);
    va_end(ap);
    ctx->banner_level = level;
    /* A banner changing is exactly the kind of top-screen change the
     * every-other-frame skip (see main.c's own header comment) must not
     * delay -- see app_top_force_redraw(). */
    app_top_force_redraw();
}

/* Why the session ended, for the connect screen's banner. The engine reports
 * it as an enum apad_session_close in apad_client_stats.close_reason instead
 * of any screen watching for a BYE itself. */
const char *app_close_reason_text(int reason)
{
    switch (reason) {
    case APAD_CLOSE_LOCAL:        return "session closed locally";
    case APAD_CLOSE_PEER_BYE:     return "server sent BYE";
    case APAD_CLOSE_IDLE_TIMEOUT: return "idle timeout -- nothing received";
    case APAD_CLOSE_RETX_FAILED:  return "server stopped answering";
    case APAD_CLOSE_PEER_ERROR:   return "server no longer knows this session";
    default:                      return "session ended";
    }
}

/* ------------------------------------------------------------------------ */
/* S7 discovery -- broadcast (tier 2) ONLY. Tier-3 unicast discovery is
 * apad_client_probe() now (screen_connect.c), not this file -- see the
 * comment where app_probe() used to live, below.                          */
/* ------------------------------------------------------------------------ *
 *
 * ONE DISCOVER, ONE ANNOUNCE, ON A SOCKET OF ITS OWN, LONG-LIVED. Separate
 * from the engine's because a broadcast needs SO_BROADCAST and the engine's
 * socket belongs to the session (and, independently of that, must never be
 * handed a broadcast target at all -- see point 3 below). NOT closed after
 * each discovery, unlike a naive port of the 3DS's app_discover(): see
 * disc_sock()'s own comment for the measurement that made a socket held for
 * the app's lifetime the right trade here.
 *
 * =========================================================================
 * WHY apad_udp_recv() USED TO NEED A POLL LOOP HERE, AND STILL DOES
 * =========================================================================
 * shim/net_nds.c's apad_udp_recv() now honours a positive timeout_ms
 * correctly (references/nds/README.md "Outcome B"), so the workaround this
 * comment used to describe -- polling with timeout 0 because ANY positive
 * timeout wedged the console forever on silence -- is gone from
 * apad_client_probe()/apad_client_connect(), which screen_connect.c now
 * calls directly, exactly as the 3DS does.
 *
 * await_announce() below STILL polls with timeout 0 and its own deadline,
 * for an unrelated reason: a DISCOVER is not a reliable message (§4 -- no
 * ACK, no retransmit timer), so nothing resends a lost one, and this file
 * found that a single lost datagram under melonDS's NAT failed a one-shot
 * probe about half the time. The RESEND-EVERY-400MS loop below is what fixes
 * that, and a resending loop has to be a poll by construction -- there is no
 * "wait up to 3000ms, resending every 400ms" shape as a single blocking
 * call. So this stays a poll loop even though the underlying recv is no
 * longer broken; apad_client_probe() has no equivalent resend and is a
 * genuine single DISCOVER; app_discover() below is not.
 */

/* ONE DISCOVERY SOCKET FOR THE WHOLE RUN, opened lazily and never closed.
 *
 * The 3DS opens one per discovery and closes it again, because shim/net_bsd.c
 * has a four-socket pool and it did not want to hold one. THAT IS THE WRONG
 * TRADE ON THIS PLATFORM, measured: with a socket per probe, the first two or
 * three probes of a run worked and every one after that failed instantly, on
 * a server that a host-side tools/engine-client run proved was answering in
 * the same second. Two sockets for the life of the app (this one and the
 * engine's) is well inside the pool, and it removes a per-probe failure mode
 * that presents to a user as "the server is not there".
 *
 * SO_BROADCAST is set once and left on. It does not stop the socket sending
 * unicast, so tier-2 (broadcast) and tier-3 (unicast) discovery share it --
 * and on this platform a broadcast send WITHOUT that option does not fail, it
 * wedges lwIP forever (the M5 bring-up spike's finding), so having it always
 * on is one fewer way to hit that. */
static apad_sock *s_disc_sock;

static apad_sock *disc_sock(void)
{
    if (s_disc_sock == NULL) {
        s_disc_sock = apad_udp_open(0);
        if (s_disc_sock != NULL
            && apad_udp_set_broadcast(s_disc_sock, 1) != APAD_OK) {
            /* Kept anyway: unicast tier-3 works without it and is the path
             * that actually matters. A broadcast send would be the thing to
             * avoid, and app_discover() below is the only caller that does
             * one. */
        }
    }
    return s_disc_sock;
}

/* Send the DISCOVER, then poll for an ANNOUNCE for up to `timeout_ms`,
 * yielding a frame between polls. Fills `out_from` and `out_ann` on success.
 * Returns 1 on an ANNOUNCE, 0 on timeout.
 *
 * THE DISCOVER IS RE-SENT EVERY DISCOVER_RESEND_MS, and that is not belt and
 * braces -- it is what made this reliable in melonDS. A DISCOVER is not a
 * reliable message (docs/PROTOCOL.md S4/S6.1: no ACK, no retransmit timer),
 * so nothing else will ever resend it, and a single lost datagram on the way
 * out is indistinguishable from "no server there". Under the emulator's NAT
 * the first datagram after association was lost often enough that a
 * single-shot probe failed roughly half the time against a server that was
 * demonstrably answering. Re-sending costs one 16-byte datagram every 400 ms
 * for at most three seconds. */
#define DISCOVER_RESEND_MS 400u

static int await_announce(apad_sock *sock, const apad_addr *to,
                          const uint8_t *disc, size_t disc_len, int timeout_ms,
                          const uint8_t *prefer_ip,
                          apad_addr *out_from, apad_announce *out_ann)
{
    uint8_t rbuf[APAD_MAX_DATAGRAM];
    apad_packet pkt;
    apad_addr first_from;
    apad_announce first_ann;
    uint32_t start;
    uint32_t last_send;
    int drain;
    int have_first = 0;

    /* The socket is shared and long-lived now, so it can be holding a late
     * ANNOUNCE from a previous discovery. Throw those away before sending, or
     * a probe of one address could be satisfied by an answer from another. */
    for (drain = 0; drain < 8; drain++) {
        if (apad_udp_recv(sock, out_from, rbuf, sizeof rbuf, 0) <= 0) {
            break;
        }
    }

    start = apad_ticks_ms();
    (void)apad_udp_send(sock, to, disc, disc_len);
    last_send = start;

    for (;;) {
        int rn = apad_udp_recv(sock, out_from, rbuf, sizeof rbuf, 0);
        uint32_t now;

        if (rn > 0) {
            memset(&pkt, 0, sizeof pkt);
            if (apad_packet_parse(rbuf, (size_t)rn, &pkt) >= 0
                && pkt.header.type == (uint8_t)APAD_MSG_ANNOUNCE) {
                memset(out_ann, 0, sizeof *out_ann);
                if (apad_decode_announce(pkt.payload, pkt.payload_len,
                                         out_ann) >= 0) {
                    /* Nothing to prefer, or this IS the preferred one:
                     * done. Otherwise remember the first answer and keep
                     * listening for the one the user last used. */
                    if (prefer_ip == NULL
                        || memcmp(out_from->ip, prefer_ip, 4) == 0) {
                        return 1;
                    }
                    if (!have_first) {
                        first_from = *out_from;
                        first_ann = *out_ann;
                        have_first = 1;
                    }
                }
            }
            continue;   /* something else on the wire: keep looking */
        }
        now = apad_ticks_ms();
        if (apad_time_since(now, start) > (uint32_t)timeout_ms) {
            if (have_first) {
                *out_from = first_from;
                *out_ann = first_ann;
                return 1;
            }
            return 0;
        }
        if (apad_time_since(now, last_send) >= DISCOVER_RESEND_MS) {
            (void)apad_udp_send(sock, to, disc, disc_len);
            last_send = now;
        }
        /* The yield is what makes this a poll rather than a spin: it lets
         * DSWiFi's lwIP cothread run, which is what actually delivers the
         * datagram we are waiting for. */
        cothread_yield_irq(IRQ_VBLANK);
    }
}

/* Builds the S6.1 DISCOVER once. Returns its length or -1. */
static int build_discover(uint8_t *buf, size_t cap)
{
    apad_header hdr;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = APAD_MAGIC;
    hdr.version = APAD_VERSION;
    hdr.type = (uint8_t)APAD_MSG_DISCOVER;
    return apad_packet_build(buf, cap, &hdr, NULL, 0, NULL, 0);
}

/* app_probe() -- the tier-3 unicast DISCOVER/ANNOUNCE poll that used to live
 * here as a workaround for apad_client_probe() -- IS GONE as of the
 * shim/net_nds.c fix (references/nds/README.md "Outcome B"). Its entire
 * reason to exist was that the engine's drain_rx(c, 25)/drain_rx(c, 50)
 * inside apad_client_probe()/apad_client_connect() called shim/net_bsd.c's
 * apad_udp_recv() with a positive timeout, which never returned on this
 * platform's lwIP when nothing answered. Now that shim/net_nds.c's
 * apad_udp_recv() honours its timeout by construction, apad_client_probe()
 * IS the tier-3 reachability check -- same DISCOVER, same ANNOUNCE, same
 * core codec, no longer duplicated here -- and screen_connect.c calls it
 * exactly as clients/3ds/source/screen_connect.c does. This file keeps only
 * app_discover(), the tier-2 BROADCAST half, which apad_client_probe() must
 * never be handed (see this file's header, point 3: a broadcast send without
 * SO_BROADCAST is its own, separate wedge, and the engine's socket has no
 * way to set that option). */

/* S7 tier 2. apad_udp_set_broadcast() BEFORE the send is not optional on this
 * platform either: without it a broadcast sendto() wedges lwIP forever (the
 * M5 bring-up spike's finding, and a different bug from the timeout one
 * above). */
int app_discover(app_ctx *ctx)
{
    uint8_t buf[APAD_MAX_DATAGRAM];
    apad_sock *sock;
    apad_addr bcast, from, saved;
    apad_announce ann;
    const uint8_t *prefer;
    int n, ok;

    sock = disc_sock();
    if (sock == NULL) {
        return 0;
    }
    n = build_discover(buf, sizeof buf);
    if (n < 0) {
        return 0;
    }

    /* WHICH SERVER WINS WHEN SEVERAL ANSWER. Nothing here is hardcoded: the
     * address in ctx->ip_text is either "" (fresh unit), the one config_nds.c
     * saved after the last successful session, or the one the previous
     * discovery picked. If it names a host that answers this DISCOVER, that
     * host wins even when another one answered first; only when it stays
     * silent for the whole window does the first responder take over. Without
     * this, a LAN with two servers always "chose" the faster machine at boot
     * (2026-09-10 report: "the linux server ip is hardcoded at boot no?"). */
    prefer = NULL;
    if (ctx->ip_text[0] != '\0'
        && apad_addr_parse(&saved, ctx->ip_text, 0) == APAD_OK) {
        prefer = saved.ip;
    }

    apad_addr_broadcast(&bcast, (uint16_t)APAD_DEFAULT_PORT);
    ok = await_announce(sock, &bcast, buf, (size_t)n, 900, prefer, &from,
                        &ann);
    if (!ok) {
        return 0;
    }

    apad_text_get(ctx->server_name, sizeof ctx->server_name, ann.server_name,
                  APAD_NAME_LEN);
    /* server_port is authoritative (docs/PROTOCOL.md S6.2); the IP comes from
     * the reply's source address. */
    snprintf(ctx->ip_text, sizeof ctx->ip_text, "%u.%u.%u.%u",
             (unsigned)from.ip[0], (unsigned)from.ip[1], (unsigned)from.ip[2],
             (unsigned)from.ip[3]);
    snprintf(ctx->port_text, sizeof ctx->port_text, "%u",
             (unsigned)ann.server_port);
    ctx->from_announce = 1;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* the session engine                                                       */
/* ------------------------------------------------------------------------ */

int app_client_start(app_ctx *ctx)
{
    const char *name;

    if (ctx->client != NULL) {
        return 1;
    }
    if (apad_net_init() != APAD_OK) {
        snprintf(ctx->fatal, sizeof ctx->fatal,
                 "apad_net_init() failed (shim/net_bsd.c)");
        return 0;
    }
    name = ctx->is_dsi ? APAD_NDS_NAME_DSI : APAD_NDS_NAME_DS;
    ctx->client = apad_client_create(name, ctx->caps);
    if (ctx->client == NULL) {
        snprintf(ctx->fatal, sizeof ctx->fatal,
                 "apad_client_create() failed (socket or memory)");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* one hardware sample per frame                                            */
/* ------------------------------------------------------------------------ */

static int16_t clamp_i16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Touch is screen space and +Y is ALREADY DOWN there (docs/PROTOCOL.md S5.3),
 * so unlike a stick it is not inverted -- only scaled. The DS touchscreen is
 * 256x192 pixels. This is a unit conversion, not a deadzone; deadzone is the
 * server's, always (and this console's touch IS its left stick, per
 * server/profiles/ds-default.jsonc, so getting this wrong would be getting
 * the stick wrong). */
static int16_t scale_touch_x(uint16_t px)
{
    return clamp_i16((int32_t)px * 65536 / UI_DS_W - 32768);
}

static int16_t scale_touch_y(uint16_t py)
{
    return clamp_i16((int32_t)py * 65536 / UI_DS_H - 32768);
}

/* Fills ctx->st, the exact struct apad_client_pump_ex() encodes. Run on EVERY
 * screen, not just the session one, so the connect screen's readout can prove
 * the controls work before there is a network to blame -- and so the readout
 * and the wire can never show different things.
 *
 * BY POSITION, not by label. The DS's A is where a pad's A is, so KEY_A maps
 * to APAD_BTN_A and no relabelling happens here; the server's profile owns
 * every opinion about what A should do. axes[] stays zero -- this console has
 * no stick, and sending a centred one it does not have is the same lie as
 * sending a battery percentage it cannot measure.
 *
 * THE LID. KEY_LID is set while the console is CLOSED. Everything reads as
 * released then: a lid shut on a held D-pad would otherwise leave the
 * direction stuck on the PC's virtual pad for as long as it stayed shut, and
 * S6.20's "the snapshot is the authority" makes that the client's problem to
 * avoid, not the server's to guess at.
 *
 * client_ticks_ms is deliberately left at 0: apad_client_pump() overwrites it
 * with its own `now` at send time, so filling it here would be dead code that
 * looked load-bearing. */
static void app_sample_input(app_ctx *ctx)
{
    u32 held = ctx->keys_held;

    memset(&ctx->st, 0, sizeof ctx->st);   /* every reserved field zeroed, S5 */

    if (ctx->lid_closed) {
        ctx->st.battery = battery_wire();
        return;
    }

    if (held & KEY_A)      { ctx->st.buttons |= APAD_BTN_A; }
    if (held & KEY_B)      { ctx->st.buttons |= APAD_BTN_B; }
    if (held & KEY_X)      { ctx->st.buttons |= APAD_BTN_X; }
    if (held & KEY_Y)      { ctx->st.buttons |= APAD_BTN_Y; }
    if (held & KEY_L)      { ctx->st.buttons |= APAD_BTN_L; }
    if (held & KEY_R)      { ctx->st.buttons |= APAD_BTN_R; }
    if (held & KEY_START)  { ctx->st.buttons |= APAD_BTN_START; }
    if (held & KEY_SELECT) { ctx->st.buttons |= APAD_BTN_SELECT; }
    if (held & KEY_UP)     { ctx->st.buttons |= APAD_BTN_DPAD_UP; }
    if (held & KEY_DOWN)   { ctx->st.buttons |= APAD_BTN_DPAD_DOWN; }
    if (held & KEY_LEFT)   { ctx->st.buttons |= APAD_BTN_DPAD_LEFT; }
    if (held & KEY_RIGHT)  { ctx->st.buttons |= APAD_BTN_DPAD_RIGHT; }

    if (ctx->touch_held) {
        ctx->st.buttons |= APAD_BTN_TOUCH_PRESS;
        ctx->st.touch_count = 1;
        ctx->st.touches[0].id = 1;       /* same convention as the 3DS client */
        ctx->st.touches[0].pressure = 0; /* binary touch, no pressure sensor  */
        ctx->st.touches[0].x = scale_touch_x(ctx->touch.px);
        ctx->st.touches[0].y = scale_touch_y(ctx->touch.py);
    }

    ctx->st.battery = battery_wire();
}

/* ------------------------------------------------------------------------ */
/* bring-up                                                                 */
/* ------------------------------------------------------------------------ */

/* Returns 1 on success. On failure fills ctx->fatal and returns 0; the caller
 * starts on APAD_SCREEN_FATAL, which can still run the self-test. */
static int bring_up(app_ctx *ctx)
{
    if (!ui_init()) {
        /* No renderer means no way to say anything at all, on either screen.
         * There is nothing useful left to do and nothing to say it on. */
        return 0;
    }
    /* AFTER ui_init(): the keyboard's tiles and map go into the VRAM ui_init()
     * deliberately left free for them, and it needs the video mode already
     * set. Not fatal if it fails -- a client with a saved address or a dev
     * hook is still usable without a keyboard. */
    (void)apad_kbd_init();

    ctx->is_dsi = isDSiMode() ? 1 : 0;
    ctx->caps = APAD_NDS_CAPS_BASE;
    if (ctx->is_dsi) {
        ctx->caps |= APAD_CAP_BATTERY;
        s_battery = (int)battery_byte();
    }

    /* The tick must exist before anything asks the time -- DSWiFi bring-up
     * and every engine timeout are measured with it. Timer 1: DSWiFi owns
     * timer 3 on the ARM9 (LIBNDS_DEFAULT_TIMER_WIFI) and libnds's system
     * counter owns timer 2. */
    apad_nds_time_init();

    if (apad_nds_wifi_init(ctx->force_ds_mode) != 0) {
        snprintf(ctx->fatal, sizeof ctx->fatal, "Wifi_InitDefault() failed");
        return 0;
    }

    /* The card, for the saved address. Never fatal: an emulator with no DLDI
     * image and a flashcart whose loader did not patch this ROM both land
     * here, and neither should stop a controller from working. */
    (void)apad_nds_config_mount();
    return 1;
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */

/* THE TOP-SCREEN SKIP, added for the frame-rate pass that follows
 * shim/net_nds.c (2026-09-09). MEASURED with the new per-phase diag panel,
 * not guessed: on the session screen, sample+update read ~0ms (the pump is
 * a handful of memcmp/memcpy calls against a 4-slot ring, not the cost
 * anyone suspected), draw_top read ~8ms and draw_bottom ~10ms -- 18ms of
 * drawing against a 16.7ms VBlank period, which is exactly why the loop
 * missed every OTHER VBlank and app_frame_ms() read ~31ms (two VBlanks).
 * fill_px()'s 32-bit paired stores and clear_surface()'s dmaFillWords() --
 * this file's other candidate fixes -- are ALREADY what ui.c does; there was
 * no further win available there without doing less work, not doing the
 * same work faster.
 *
 * The top screen's own content -- RTT, session/rate/caps, the STATUS/ERROR
 * line, the diag phase table -- changes at the rate of a PONG or a status
 * push (docs/PROTOCOL.md S6.6/S6.10), which is nowhere near 60Hz; the
 * bottom screen is where the touch feedback a person is actually watching
 * lives, and that one is left drawn every frame. So: draw the top screen on
 * even frames only, forced back on for one frame on every screen change (a
 * transition must never show a stale title from the PREVIOUS screen) and
 * whenever ctx->show_diag is toggled (SELECT) or the connect/session banner
 * text changes, both of which are the two things a person is most likely to
 * be looking at the instant they change. Skipping it costs nothing else: the
 * top screen has no double buffer to keep in sync (ui.c draws it straight
 * into live VRAM), so a skipped frame is simply the previous one, still
 * correct pixels, one VBlank stale at most. */
static int s_top_force_next = 1;

void app_top_force_redraw(void)
{
    s_top_force_next = 1;
}

int main(int argc, char **argv)
{
    /* static, not on the stack: app_ctx carries the input snapshot and
     * several message buffers, and this console's main stack is not
     * generous. */
    static app_ctx ctx;
    apad_screen_id cur;
    int top_parity = 0;

    (void)argc;
    (void)argv;

    memset(&ctx, 0, sizeof ctx);

    /* Boot chords, sampled before anything else can consume them. scanKeys()
     * must run before keysHeld() returns anything at all. L forces the DS-mode
     * radio even on a DSi, which is how a user reproduces a DS-only problem
     * (and it is a HELD chord, so one scan is enough here -- unlike the
     * L+R+Start self-test window, which screen_wifi.c samples across many
     * frames because the first scan or two after bring-up can read 0). */
    scanKeys();
    ctx.force_ds_mode = (keysHeld() & KEY_L) ? 1 : 0;

    snprintf(ctx.port_text, sizeof ctx.port_text, "%u",
             (unsigned)APAD_DEFAULT_PORT);
    ctx.want_boot_combo = 1;
    ctx.want_discovery = 1;
    ctx.selftest_return = APAD_SCREEN_CONNECT;

    cur = bring_up(&ctx) ? APAD_SCREEN_WIFI : APAD_SCREEN_FATAL;

    /* Defaults for the connect screen's fields. EMPTY unless config_nds.c's
     * saved file says otherwise -- a fresh unit must never dial an address
     * nobody chose. ctx.ip_text is already "" from the memset above.
     * Discovery (ctx.want_discovery) still runs first and overwrites whatever
     * this loads the moment the LAN answers a DISCOVER. */
    (void)apad_nds_config_load(ctx.ip_text, sizeof ctx.ip_text,
                               ctx.port_text, sizeof ctx.port_text,
                               ctx.ssid, sizeof ctx.ssid);

    /* DEV HOOKS, all three compiled out of a shipping build. They exist
     * because a headless emulator run has nobody to press a button and no
     * key bindings configured -- see clients/nds/Makefile and build.sh's
     * .hooks stamp, which forces a clean whenever one of them changes so a
     * "shipping" build can never silently reuse an object with an address
     * baked into it. */
#ifdef APAD_AUTO_HOST
    snprintf(ctx.ip_text, sizeof ctx.ip_text, "%s", APAD_AUTO_HOST);
    ctx.want_discovery = 0;
    ctx.want_connect_now = 1;
#endif
#ifdef APAD_AUTO_PORT
    snprintf(ctx.port_text, sizeof ctx.port_text, "%s", APAD_AUTO_PORT);
#endif

    kScreens[cur]->enter(&ctx);

    for (;;) {
        apad_screen_id next;
        uint32_t t_start, t_sampled, t_updated, t_flipped, t_top;

        t_start = apad_ticks_ms();

        scanKeys();
        ctx.keys_held = keysHeld();
        ctx.keys_down = keysDown();
        ctx.lid_closed = (ctx.keys_held & KEY_LID) != 0u;

        /* THE RELEASED-ONCE GATE, in one place for the whole app. See app.h's
         * comment on keys_armed for the failure this prevents: a key held
         * across a screen change, or across a blocking call with no scanKeys()
         * in it, reads back as a fresh press. */
        if (ctx.keys_held == 0u) {
            ctx.keys_armed = 1;
        }

        touchRead(&ctx.touch);
        ctx.touch_held = (ctx.keys_held & KEY_TOUCH) != 0u;
        ctx.touch_pressed = ctx.keys_armed
                         && ((ctx.keys_down & KEY_TOUCH) != 0u);

        /* Battery is a DSi-only reading and changes on a scale of minutes;
         * once a second is far more often than it can move. */
        if (ctx.is_dsi && (apad_ticks_ms() % 1000u) < 17u) {
            s_battery = (int)battery_byte();
        }

        app_sample_input(&ctx);
        t_sampled = apad_ticks_ms();

        /* THE "update" PHASE. On the session screen this is where
         * apad_client_pump_ex() lives (screen_session.c), so it is also
         * where the send actually happens -- the number to watch if the
         * server-side packet rate does not match this bucket's sum with
         * "sample" and "flip". */
        next = kScreens[cur]->update(&ctx);
        if (ctx.want_exit) {
            break;
        }
        if (next != cur) {
            cur = next;
            ctx.keys_armed = 0;   /* the new screen must see a release first */
            s_top_force_next = 1;   /* never show the OLD screen's title */
            kScreens[cur]->enter(&ctx);
        }
        t_updated = apad_ticks_ms();

        /* THE "flip" PHASE: ui_frame_begin()'s cothread_yield_irq(IRQ_VBLANK)
         * wait plus the double-buffer flip. Mostly WAITING, not working --
         * see ui.c's header for why the yield sits at the start of the frame
         * here -- but it is still wall-clock time between one INPUT_STATE and
         * the next, so it belongs in the same table as the other four.
         * ui_flip_wait_ms()/ui_flip_swap_ms() read ui.c's OWN split of this
         * same call (see app.h), which is more precise than timing around the
         * call from here -- and is what actually separates the wait from the
         * work, rather than lumping both into one number. */
        ui_frame_begin();
        frame_time_sample();
        t_flipped = apad_ticks_ms();

        /* THE TOP-SCREEN SKIP (see this file's header comment above main()).
         * top_parity alternates every frame; s_top_force_next (screen
         * changes, app_top_force_redraw() callers) always wins. A skipped
         * frame costs t_top == t_flipped, i.e. this bucket reads 0 for that
         * frame and the 32-frame average simply reflects how often it ran. */
        top_parity ^= 1;
        if (s_top_force_next || top_parity == 0) {
            ui_screen_top();
            kScreens[cur]->draw_top(&ctx);
            s_top_force_next = 0;
        }
        t_top = apad_ticks_ms();

        ui_screen_bottom();
        kScreens[cur]->draw_bottom(&ctx);
        ui_frame_end();

        phase_time_sample(apad_time_since(t_sampled, t_start),
                          apad_time_since(t_updated, t_sampled),
                          apad_time_since(t_top, t_flipped),
                          apad_time_since(apad_ticks_ms(), t_top),
                          ui_flip_wait_ms(), ui_flip_swap_ms());
    }

    /* Closes the socket, and sends a BYE first if a session somehow survived
     * the loop. */
    apad_client_destroy(ctx.client);
    ui_exit();
    return 0;
}
