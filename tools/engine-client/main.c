/* tools/engine-client/main.c
 *
 * Drives the SHARED client engine (clients/common/apad_client.c) through a
 * real session against a running server, over real UDP.
 *
 * Why this exists next to tools/loopback-client: the two tools answer
 * different questions and neither can answer the other's.
 *
 *   - loopback-client is ADVERSARIAL instrumentation. It hand-drives the
 *     session at the datagram level so it can do what a well-behaved client
 *     never would: retransmit a byte-identical duplicate HELLO and compare
 *     the server's two answers verbatim (S9), cross the 0xFFFF sequence
 *     wrap on purpose, hexdump what actually went on the wire. It tests THE
 *     SERVER.
 *
 *   - engine-client is the OPPOSITE: it contains no protocol code at all.
 *     Every packet decision is made by apad_client_* — the same engine the
 *     Android client (M3) and the 3DS client link. It tests THE ENGINE, on
 *     Linux, in CI, where until this tool existed the engine only ever ran
 *     inside an installed APK on a phone. The M4 server-PING bug lived in
 *     exactly this gap: every client carried its own copy of the session
 *     driving, the harness's copy was fixed, the shipping clients' copies
 *     were not, and every harness run stayed green.
 *
 * THREE MODES, and the two new ones exist because the §6.15-§6.19 send side
 * of the engine cannot be proven by the first:
 *
 *   (default)             the session run described below.
 *   --kbm                 drives apad_client_pump_ex() against a live server:
 *                         a held key, a SUB-FRAME TAP (press and release
 *                         between two pumps -- the case a state snapshot
 *                         physically cannot express, and the entire reason
 *                         §6.20 has an event ring), pointer motion, a button
 *                         and a media control. Watch it with evtest on
 *                         "AtticPad Keyboard 0" / "AtticPad Mouse 0" /
 *                         "AtticPad Media 0". It pauses after the devices are
 *                         created so evtest can be attached to nodes that do
 *                         not exist until the first datagram of each type
 *                         arrives.
 *   --inputcaps-reorder   needs no server: a fake peer on a scratch port
 *                         completes the handshake and then delivers two
 *                         INPUTCAPS OUT OF ORDER, newer first. §6.20's fourth
 *                         per-type window must discard the older one; without
 *                         it the stale copy revives a `features` bit the
 *                         server has cleared and this client resumes sending
 *                         a type the server has stopped accepting (§6.19,
 *                         "latest wins"). Asserts the bit stays clear.
 *   --release-on-clear    also serverless: advertises KEYBOARD, waits until
 *                         this client is demonstrably holding a key, then
 *                         WITHDRAWS the feature bit. §6.19 requires one final
 *                         datagram releasing what is held BEFORE sending
 *                         stops; without it a held Ctrl stays down on the
 *                         user's desktop with nothing able to lift it. The
 *                         same run measures §6.20's 10 Hz held-repeat floor
 *                         from the only side that can measure it.
 *
 * --hold <BTN>           optional, default (target) mode only: ORs one wire
 *                         button into EVERY frame this tool sends, held for
 *                         the whole run. <BTN> is a wire (Nintendo-
 *                         convention) button name -- A B X Y L R L3 R3
 *                         START SELECT HOME ZL ZR -- matching the names a
 *                         profile's "buttons" map uses (server/src/
 *                         profiles.c's apad_profile_wire_btn_names, plus
 *                         ZL/ZR which are not remappable but ARE the §5.4
 *                         trigger fallback). Exists so a server-side
 *                         mapping that only fires while a button is DOWN
 *                         can be watched in evtest for as long as it takes
 *                         to read it -- e.g. a profile with
 *                         "buttons": { "L": "LT" }, whose ABS_Z should sit
 *                         at max for the whole run. fill_input() below
 *                         otherwise only ever sends A and B.
 *
 * --device-name <name>   optional, default (target) mode only: overrides the
 *                         HELLO device_name this tool advertises, which is
 *                         otherwise hardcoded to "AtticPad 3DS engine-client"
 *                         below. Exists so a server-side profile-matching
 *                         check (server/src/profiles.c's apad_profiles_match())
 *                         can be driven with a real device_name string
 *                         without hand-crafting a datagram — e.g.
 *                         "--device-name 'AtticPad DS'" to prove a
 *                         ds-default.jsonc-style profile is selected. Does
 *                         NOT change the caps this tool advertises
 *                         (APAD_CAP_STICK_L|_R stay set regardless), so it
 *                         cannot exercise a touch-substitutes-for-stick path
 *                         — that needs a caps mask this tool does not have a
 *                         flag for, and none was added on the theory that a
 *                         name-only override is the useful 90% of this and a
 *                         caps override is a separate, larger change better
 *                         done when something actually needs it.
 *

 * Exit code 0 requires, in order:
 *   - probe: an ANNOUNCE answered the unicast DISCOVER (S7 tier 3)
 *   - connect: HELLO -> WELCOME -> ACK completed (S8)
 *   - pump: the session stayed ACTIVE for the whole run
 *   - stats: at least one PONG came back (rtt_ms >= 0) — this is the S6.6
 *     client-originated half
 *   - disconnect: BYE sent, session CLOSED
 *
 * printf/fprintf are fine here: this is a Linux tool. docs/CONVENTIONS.md's "no stdio"
 * rule is scoped to core/ and shim/.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include "atticpad/atticpad.h"
#include "apad_client.h"

static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

/* --hold: ORed into every frame. 0 (the default) changes nothing. */
static uint32_t g_hold_buttons;

/* Wire button names, for --hold. Deliberately a local table rather than an
 * include of server/src/profiles.h: this is a CLIENT tool and must not link
 * server code. The 11 remappable names match that file's
 * apad_profile_wire_btn_names[] exactly; ZL/ZR are here too because the
 * "no APAD_CAP_TRIGGERS -> ZL/ZR drive LT/RT" fallback is worth being able
 * to drive from the same flag. */
static const struct { const char *name; uint32_t bit; } k_wire_buttons[] = {
    { "A", APAD_BTN_A },       { "B", APAD_BTN_B },
    { "X", APAD_BTN_X },       { "Y", APAD_BTN_Y },
    { "L", APAD_BTN_L },       { "R", APAD_BTN_R },
    { "ZL", APAD_BTN_ZL },     { "ZR", APAD_BTN_ZR },
    { "L3", APAD_BTN_L3 },     { "R3", APAD_BTN_R3 },
    { "START", APAD_BTN_START },
    { "SELECT", APAD_BTN_SELECT },
    { "HOME", APAD_BTN_HOME }
};

static uint32_t wire_button_from_name(const char *name) {
    size_t i;
    for (i = 0; i < sizeof k_wire_buttons / sizeof k_wire_buttons[0]; i++) {
        if (strcmp(k_wire_buttons[i].name, name) == 0) {
            return k_wire_buttons[i].bit;
        }
    }
    return 0u;
}

/* Same idea as loopback-client's build_input_state: distinctive, varying
 * values so a human watching evtest (or the server log) can tell frames
 * apart — but built as the ENGINE'S CALLER would build it, and left to the
 * engine to encode. */
static void fill_input(apad_input_state *st, unsigned iter) {
    memset(st, 0, sizeof *st);
    st->buttons = (iter % 2u == 0u) ? (APAD_BTN_A | APAD_BTN_DPAD_RIGHT)
                                    : APAD_BTN_B;
    st->buttons |= g_hold_buttons;
    /* Computed in int32 and subtracted in range: S9 warns off the
     * unsigned-wrap-then-narrow construct (C99 6.3.1.3p3). */
    st->axes[0] = (int16_t)((int32_t)((iter * 997u) % 65536u) - 32768); /* LX sweep */
    st->axes[1] = (int16_t)(iter % 2u == 0u ? 12000 : -12000);
    st->battery = 255; /* unknown, S5.5 */
    st->client_ticks_ms = apad_ticks_ms();
}

/* ===================================================================== *
 * --kbm : the §6.15-§6.19 send side, against a live server              *
 * ===================================================================== */

/* Pump until `ms` have passed, feeding the engine `kbm` (which may be NULL)
 * every iteration. Returns 0 while the session is ACTIVE, -1 once it is not.
 *
 * Handing the SAME submission struct to every pump in a phase is correct and
 * deliberate: `keys[]`/`buttons`/`held` are a state snapshot, so repeating
 * them is repeating the truth, and the engine only puts a datagram on the
 * wire when §6.20's cadence says one is due. The events[] queue is the one
 * part that must NOT be repeated, which is why every phase below that submits
 * events does so for exactly one pump. */
static int pump_for(apad_client *c, unsigned ms, const apad_client_kbm_in *kbm)
{
    uint32_t start = apad_ticks_ms();
    apad_input_state in;

    do {
        memset(&in, 0, sizeof in);
        in.battery = 255;
        if (apad_client_pump_ex(c, &in, kbm, 8) != APAD_CLIENT_ACTIVE) {
            return -1;
        }
    } while (apad_time_since(apad_ticks_ms(), start) < ms);
    return 0;
}

static int pump_once(apad_client *c, const apad_client_kbm_in *kbm)
{
    apad_input_state in;

    memset(&in, 0, sizeof in);
    in.battery = 255;
    return (apad_client_pump_ex(c, &in, kbm, 8) == APAD_CLIENT_ACTIVE) ? 0 : -1;
}

static int run_kbm(apad_client *c, unsigned attach_wait_ms)
{
    apad_client_stats st;
    apad_client_kbm_in k;
    uint32_t start;
    unsigned i;

    /* §6.19: nothing may be sent until an INPUTCAPS has been accepted.
     * Absence IS the negotiation, so a server that never sends one leaves
     * this loop at the timeout and the run is a legitimate no-op. */
    start = apad_ticks_ms();
    for (;;) {
        if (pump_once(c, NULL) != 0) {
            fprintf(stderr, "kbm: session left ACTIVE waiting for INPUTCAPS\n");
            return 1;
        }
        apad_client_get_stats(c, &st);
        if (st.inputcaps_serial != 0u) {
            break;
        }
        if (apad_time_since(apad_ticks_ms(), start) > 4000u) {
            fprintf(stderr, "kbm: no INPUTCAPS in 4 s -- server does not "
                            "advertise §6.15-§6.17 (features would be 0)\n");
            return 1;
        }
    }
    printf("kbm: INPUTCAPS serial=%u features=0x%08lx status=0x%08lx "
           "media_mask=0x%08lx mouse_rate_hz=%u\n",
           st.inputcaps_serial,
           (unsigned long)st.inputcaps.features,
           (unsigned long)st.inputcaps.status,
           (unsigned long)st.inputcaps.media_mask,
           (unsigned)st.inputcaps.mouse_rate_hz);
    if ((st.inputcaps.features & (APAD_KBM_FEATURE_KEYBOARD
                                  | APAD_KBM_FEATURE_MOUSE
                                  | APAD_KBM_FEATURE_MEDIA)) == 0u) {
        fprintf(stderr, "kbm: server advertises no facility; nothing to drive\n");
        return 1;
    }

    /* Warm-up. The server creates each uinput node lazily, on the first
     * datagram of that type -- so the device an operator wants to watch does
     * not exist until this client has already sent something. Send one
     * all-clear snapshot per facility, then pause: nothing is held, so the
     * pause costs no datagrams at all and the receiver's watchdog has nothing
     * to release. */
    memset(&k, 0, sizeof k);
    k.have = (uint8_t)(APAD_KBM_FEATURE_KEYBOARD | APAD_KBM_FEATURE_MOUSE
                       | APAD_KBM_FEATURE_MEDIA);
    if (pump_for(c, 200u, &k) != 0) {
        return 1;
    }
    apad_client_get_stats(c, &st);
    printf("kbm: devices created (status=0x%08lx). Attach evtest now; "
           "waiting %u ms.\n", (unsigned long)st.inputcaps.status,
           attach_wait_ms);
    fflush(stdout);
    if (pump_for(c, attach_wait_ms, NULL) != 0) {
        return 1;
    }

    /* ---- 1. a HELD key: A down, held 600 ms, up.
     * The hold is the §6.20 10 Hz repeat obligation in its natural habitat:
     * this loop submits the same snapshot every 8 ms and the engine emits at
     * its own floor, which is what stops the server's 1000 ms watchdog from
     * releasing a key the user is still pressing. */
    printf("kbm: [1] KEY_A down (held 600 ms)\n");
    fflush(stdout);
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_KEYBOARD;
    k.keyboard.keys[APAD_KEY_BYTE(APAD_HID_KEY_A)] =
        APAD_KEY_MASK(APAD_HID_KEY_A);
    if (pump_for(c, 600u, &k) != 0) {
        return 1;
    }
    printf("kbm: [1] KEY_A up\n");
    fflush(stdout);
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_KEYBOARD;         /* keys[] now empty */
    if (pump_for(c, 300u, &k) != 0) {
        return 1;
    }

    /* ---- 2. THE SUB-FRAME TAP. B is pressed and released between two
     * pumps, so keys[] is byte-identical before and after and a
     * snapshot-only protocol loses the keystroke entirely. Both transitions
     * are submitted in ONE pump, and both must come out of evtest. Note the
     * single pump_once: submitting a queue twice would send the tap twice. */
    printf("kbm: [2] KEY_B sub-frame tap (down+up in one pump, keys[] "
           "unchanged)\n");
    fflush(stdout);
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_KEYBOARD;
    k.keyboard.events[0].usage = APAD_HID_KEY_B;
    k.keyboard.events[0].flags = APAD_KBM_EVENT_DOWN;
    k.keyboard.events[1].usage = APAD_HID_KEY_B;
    k.keyboard.events[1].flags = 0u;            /* release */
    if (pump_once(c, &k) != 0) {
        return 1;
    }
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_KEYBOARD;
    if (pump_for(c, 400u, &k) != 0) {           /* let the trailing copies go */
        return 1;
    }

    /* ---- 3. pointer motion. §6.16: a FREE-RUNNING WRAPPING ACCUMULATOR,
     * never a per-frame delta -- the receiver diffs consecutive accepted
     * samples and the absolute value means nothing. */
    printf("kbm: [3] pointer motion, 40 steps of +6,+3 (accumulator)\n");
    fflush(stdout);
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_MOUSE;
    for (i = 0; i < 40u; i++) {
        k.mouse.dx_accum = (uint16_t)(k.mouse.dx_accum + 6u);
        k.mouse.dy_accum = (uint16_t)(k.mouse.dy_accum + 3u);
        if (pump_for(c, 20u, &k) != 0) {
            return 1;
        }
    }

    /* ---- 4. left button, held then released. */
    printf("kbm: [4] mouse LEFT down (held 300 ms) then up\n");
    fflush(stdout);
    k.mouse.buttons = APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT);
    if (pump_for(c, 300u, &k) != 0) {
        return 1;
    }
    k.mouse.buttons = 0u;
    if (pump_for(c, 300u, &k) != 0) {
        return 1;
    }

    /* ---- 5. a media control, held then released. */
    printf("kbm: [5] MEDIA PLAY_PAUSE down (held 200 ms) then up\n");
    fflush(stdout);
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_MEDIA;
    k.media.held = APAD_MEDIA_BIT(APAD_MEDIA_PLAY_PAUSE);
    if (pump_for(c, 200u, &k) != 0) {
        return 1;
    }
    k.media.held = 0u;
    if (pump_for(c, 400u, &k) != 0) {
        return 1;
    }

    /* ---- 6. a MEDIA sub-frame tap, for the same reason as [2]. */
    printf("kbm: [6] MEDIA NEXT_TRACK sub-frame tap\n");
    fflush(stdout);
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_MEDIA;
    k.media.events[0].control = APAD_MEDIA_NEXT_TRACK;
    k.media.events[0].flags   = APAD_KBM_EVENT_DOWN;
    k.media.events[1].control = APAD_MEDIA_NEXT_TRACK;
    k.media.events[1].flags   = 0u;
    if (pump_once(c, &k) != 0) {
        return 1;
    }
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_MEDIA;
    if (pump_for(c, 400u, &k) != 0) {
        return 1;
    }

    apad_client_get_stats(c, &st);
    printf("kbm: done. tx=%u rx=%u\n", st.tx_packets, st.rx_packets);
    return 0;
}

/* ===================================================================== *
 * --inputcaps-reorder : §6.20's fourth per-type window                  *
 * ===================================================================== */

/*
 * A fake peer, one thread, no server binary. It does the minimum §8 needs --
 * answer HELLO with a WELCOME, wait for the ACK §9 requires -- and then
 * delivers two INPUTCAPS OUT OF ORDER on purpose:
 *
 *   sequence N+1   features = MOUSE            (the NEWER one, sent FIRST)
 *   sequence N     features = KEYBOARD|MOUSE   (the OLDER one, sent SECOND)
 *
 * The older copy is the one that would turn KEYBOARD back on. §6.19 says the
 * most recently ACCEPTED INPUTCAPS is current and §6.20's per-type window
 * decides what "accepted" means, so the second datagram must be discarded and
 * KEYBOARD must stay clear. Get that wrong and a client resumes sending a
 * type the server has stopped accepting, holding keys nothing is listening
 * for -- silent, and only on a reordered network.
 *
 * Everything the fake peer sends is built with apad_packet_build() and the
 * real encoders, so this tool still contains no hand-rolled wire format.
 */
#define FAKE_SID 0x1234u

typedef struct {
    apad_sock *sock;
    uint16_t   port;
    volatile int stop;
    volatile int caps_sent;

    /* --release-on-clear bookkeeping; see fake_release_thread below. */
    volatile int      stage;               /* 0 pre-ACK, 1 advertised, 2 cleared */
    volatile int      kb_held_seen;
    volatile uint32_t kb_last_ms;
    volatile uint32_t kb_max_gap;
    volatile int      release_seen;
    volatile int      release_had_up_event;
    volatile int      kb_after_release;
} fake_peer;

static int fake_send(fake_peer *fp, const apad_addr *to, uint8_t type,
                     uint16_t seq, uint16_t flags,
                     const void *payload, uint16_t payload_len)
{
    uint8_t     buf[APAD_MAX_DATAGRAM];
    apad_header hdr;
    int         n;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic      = APAD_MAGIC;
    hdr.version    = (uint8_t)APAD_VERSION;
    hdr.type       = type;
    hdr.session_id = FAKE_SID;
    hdr.sequence   = seq;
    hdr.flags      = flags;
    n = apad_packet_build(buf, sizeof buf, &hdr, payload, payload_len, NULL, 0);
    if (n < 0) {
        return n;
    }
    return apad_udp_send(fp->sock, to, buf, (size_t)n);
}

static void *fake_peer_thread(void *arg)
{
    fake_peer *fp = (fake_peer *)arg;

    while (!fp->stop) {
        uint8_t     rbuf[APAD_MAX_DATAGRAM];
        apad_addr   from;
        apad_packet pkt;
        int         rn;

        rn = apad_udp_recv(fp->sock, &from, rbuf, sizeof rbuf, 50);
        if (rn <= 0) {
            continue;
        }
        memset(&pkt, 0, sizeof pkt);
        if (apad_packet_parse(rbuf, (size_t)rn, &pkt) < 0) {
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_HELLO) {
            apad_welcome w;
            uint8_t      payload[APAD_LEN_WELCOME];

            memset(&w, 0, sizeof w);
            w.session_id      = FAKE_SID;
            w.pad_slot        = 0u;
            w.flags           = 0u;          /* no pairing: §10 stays out */
            w.input_rate_hz   = 60u;
            w.server_ticks_ms = apad_ticks_ms();
            if (apad_encode_welcome(payload, sizeof payload, &w)
                == (int)APAD_LEN_WELCOME) {
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_WELCOME, 0u,
                                (uint16_t)APAD_FLAG_RELIABLE,
                                payload, (uint16_t)APAD_LEN_WELCOME);
            }
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_ACK && !fp->caps_sent) {
            apad_inputcaps newer, older;
            uint8_t        p_new[APAD_LEN_INPUTCAPS];
            uint8_t        p_old[APAD_LEN_INPUTCAPS];

            /* §6.19 delivery: "not before the ACK that discharges WELCOME". */
            memset(&newer, 0, sizeof newer);
            newer.features   = APAD_KBM_FEATURE_MOUSE;
            newer.status     = APAD_KBM_STATUS_MOUSE_READY;
            newer.media_mask = 0u;
            memset(&older, 0, sizeof older);
            older.features   = APAD_KBM_FEATURE_KEYBOARD | APAD_KBM_FEATURE_MOUSE;
            older.status     = APAD_KBM_STATUS_KEYBOARD_READY
                               | APAD_KBM_STATUS_MOUSE_READY;
            older.media_mask = 0u;

            if (apad_encode_inputcaps(p_new, sizeof p_new, &newer)
                    == (int)APAD_LEN_INPUTCAPS
                && apad_encode_inputcaps(p_old, sizeof p_old, &older)
                    == (int)APAD_LEN_INPUTCAPS) {
                /* Sequence 2 first, then sequence 1. */
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_INPUTCAPS, 2u, 0u,
                                p_new, (uint16_t)APAD_LEN_INPUTCAPS);
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_INPUTCAPS, 1u, 0u,
                                p_old, (uint16_t)APAD_LEN_INPUTCAPS);
                fp->caps_sent = 1;
            }
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_PING) {
            apad_ping ping, pong;
            uint8_t   payload[APAD_LEN_PING];

            memset(&ping, 0, sizeof ping);
            if (apad_decode_ping(pkt.payload, pkt.payload_len, &ping) < 0) {
                continue;
            }
            memset(&pong, 0, sizeof pong);
            pong.origin_ticks_ms    = ping.origin_ticks_ms;
            pong.responder_ticks_ms = apad_ticks_ms();
            if (apad_encode_ping(payload, sizeof payload, &pong)
                == (int)APAD_LEN_PING) {
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_PONG, 3u, 0u,
                                payload, (uint16_t)APAD_LEN_PING);
            }
            continue;
        }
    }
    return NULL;
}

static int run_inputcaps_reorder(uint16_t port)
{
    fake_peer         fp;
    pthread_t         th;
    apad_client      *c;
    apad_client_stats st;
    uint32_t          start;
    int               rc, failures = 0;

    memset(&fp, 0, sizeof fp);
    fp.port = port;
    rc = apad_udp_open_exclusive(&fp.sock, port);
    if (rc != APAD_OK || fp.sock == NULL) {
        fprintf(stderr, "inputcaps-reorder: cannot bind scratch port %u "
                        "(rc=%d)\n", (unsigned)port, rc);
        return 1;
    }
    if (pthread_create(&th, NULL, fake_peer_thread, &fp) != 0) {
        fprintf(stderr, "inputcaps-reorder: pthread_create failed\n");
        apad_udp_close(fp.sock);
        return 1;
    }

    c = apad_client_create("engine-client reorder", APAD_CAP_FACE4);
    if (c == NULL) {
        fprintf(stderr, "inputcaps-reorder: apad_client_create failed\n");
        fp.stop = 1;
        (void)pthread_join(th, NULL);
        apad_udp_close(fp.sock);
        return 1;
    }
    rc = apad_client_connect(c, "127.0.0.1", port, 60, 3000);
    if (rc != APAD_OK) {
        fprintf(stderr, "inputcaps-reorder: connect failed (rc=%d)\n", rc);
        failures++;
        goto out;
    }

    start = apad_ticks_ms();
    for (;;) {
        if (pump_once(c, NULL) != 0) {
            fprintf(stderr, "inputcaps-reorder: session left ACTIVE\n");
            failures++;
            goto out;
        }
        apad_client_get_stats(c, &st);
        if (st.inputcaps_serial != 0u
            && apad_time_since(apad_ticks_ms(), start) > 300u) {
            break;   /* both copies have had ample time to arrive */
        }
        if (apad_time_since(apad_ticks_ms(), start) > 3000u) {
            fprintf(stderr, "inputcaps-reorder: no INPUTCAPS arrived\n");
            failures++;
            goto out;
        }
    }

    printf("inputcaps-reorder: delivered seq=2 (features=MOUSE) then "
           "seq=1 (features=KEYBOARD|MOUSE)\n");
    printf("inputcaps-reorder: client holds serial=%u features=0x%08lx "
           "status=0x%08lx\n",
           st.inputcaps_serial, (unsigned long)st.inputcaps.features,
           (unsigned long)st.inputcaps.status);

    if ((st.inputcaps.features & APAD_KBM_FEATURE_KEYBOARD) != 0u) {
        fprintf(stderr, "  [FAIL] the stale copy REVIVED the cleared KEYBOARD "
                        "bit -- §6.20's fourth window is not being applied\n");
        failures++;
    } else {
        printf("  [PASS] KEYBOARD stayed clear: the older copy was discarded "
               "(§6.20 per-type window)\n");
    }
    if ((st.inputcaps.features & APAD_KBM_FEATURE_MOUSE) == 0u) {
        fprintf(stderr, "  [FAIL] MOUSE is not set; the newer copy was not "
                        "applied at all\n");
        failures++;
    } else {
        printf("  [PASS] MOUSE set: the newer copy IS current (§6.19 latest "
               "wins)\n");
    }
    if (st.inputcaps_serial != 1u) {
        fprintf(stderr, "  [FAIL] inputcaps_serial=%u, expected exactly 1 "
                        "accepted copy\n", st.inputcaps_serial);
        failures++;
    } else {
        printf("  [PASS] exactly one copy accepted (serial=1)\n");
    }
    if ((st.inputcaps.status & APAD_KBM_STATUS_KEYBOARD_READY) != 0u) {
        fprintf(stderr, "  [FAIL] stale `status` revived too\n");
        failures++;
    } else {
        printf("  [PASS] `status` is the newer copy's as well\n");
    }

out:
    apad_client_destroy(c);
    fp.stop = 1;
    (void)pthread_join(th, NULL);
    apad_udp_close(fp.sock);
    printf("inputcaps-reorder: %d failure(s)\n", failures);
    return (failures == 0) ? 0 : 1;
}

/* ===================================================================== *
 * --release-on-clear : §6.19's release, and §6.20's repeat floor        *
 * ===================================================================== */

/*
 * The same fake peer, driving the OTHER half of the gate. It advertises
 * KEYBOARD, waits until this client is demonstrably holding a key, then
 * CLEARS the feature bit and watches what comes back.
 *
 * §6.19: "If a features bit clears, the client MUST stop sending that type,
 * and MUST first release everything it holds for that facility -- otherwise
 * the last thing the server saw held stays held with nothing left able to
 * lift it." On a SYNTHETIC backend there is no device to unplug, so a Ctrl
 * held at that instant stays held on the user's desktop forever. Both halves
 * are asserted here: one final datagram with the bitmap cleared AND an UP
 * event in the ring, and then silence.
 *
 * The same run measures the §6.20 held-repeat floor from the receiving end,
 * which is the only end that can measure it: 10 Hz is a MUST because a
 * change-only sender is indistinguishable from a dead one, and the 1000 ms
 * watchdog is what would otherwise release the key under the user's fingers.
 */
static void *fake_release_thread(void *arg)
{
    fake_peer *fp = (fake_peer *)arg;

    while (!fp->stop) {
        uint8_t     rbuf[APAD_MAX_DATAGRAM];
        apad_addr   from;
        apad_packet pkt;
        int         rn;

        rn = apad_udp_recv(fp->sock, &from, rbuf, sizeof rbuf, 20);
        if (rn <= 0) {
            continue;
        }
        memset(&pkt, 0, sizeof pkt);
        if (apad_packet_parse(rbuf, (size_t)rn, &pkt) < 0) {
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_HELLO) {
            apad_welcome w;
            uint8_t      payload[APAD_LEN_WELCOME];

            memset(&w, 0, sizeof w);
            w.session_id      = FAKE_SID;
            w.input_rate_hz   = 60u;
            w.server_ticks_ms = apad_ticks_ms();
            if (apad_encode_welcome(payload, sizeof payload, &w)
                == (int)APAD_LEN_WELCOME) {
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_WELCOME, 0u,
                                (uint16_t)APAD_FLAG_RELIABLE,
                                payload, (uint16_t)APAD_LEN_WELCOME);
            }
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_ACK && fp->stage == 0) {
            apad_inputcaps ic;
            uint8_t        p[APAD_LEN_INPUTCAPS];

            memset(&ic, 0, sizeof ic);
            ic.features = APAD_KBM_FEATURE_KEYBOARD;
            ic.status   = APAD_KBM_STATUS_KEYBOARD_READY
                          | APAD_KBM_STATUS_SYNTHETIC;
            if (apad_encode_inputcaps(p, sizeof p, &ic)
                == (int)APAD_LEN_INPUTCAPS) {
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_INPUTCAPS, 1u, 0u,
                                p, (uint16_t)APAD_LEN_INPUTCAPS);
                fp->stage = 1;
            }
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_KEYBOARD) {
            apad_keyboard kb;
            uint32_t      now = apad_ticks_ms();
            int           a_held;

            if (apad_decode_keyboard(pkt.payload, pkt.payload_len, &kb) < 0) {
                continue;
            }
            a_held = (kb.keys[APAD_KEY_BYTE(APAD_HID_KEY_A)]
                      & APAD_KEY_MASK(APAD_HID_KEY_A)) ? 1 : 0;

            if (fp->release_seen) {
                fp->kb_after_release++;      /* §6.19: sending MUST have stopped */
                continue;
            }
            if (a_held && fp->kb_held_seen > 0) {
                uint32_t gap = apad_time_since(now, fp->kb_last_ms);
                if (gap > fp->kb_max_gap) {
                    fp->kb_max_gap = gap;
                }
            }
            fp->kb_last_ms = now;

            if (fp->stage == 1) {
                if (a_held) {
                    fp->kb_held_seen++;
                }
                /* Long enough that several §6.20 repeats have been measured. */
                if (fp->kb_held_seen >= 8) {
                    apad_inputcaps ic;
                    uint8_t        p[APAD_LEN_INPUTCAPS];

                    memset(&ic, 0, sizeof ic);
                    ic.features = 0u;         /* KEYBOARD withdrawn */
                    ic.status   = APAD_KBM_STATUS_SYNTHETIC;
                    if (apad_encode_inputcaps(p, sizeof p, &ic)
                        == (int)APAD_LEN_INPUTCAPS) {
                        (void)fake_send(fp, &from, (uint8_t)APAD_MSG_INPUTCAPS,
                                        2u, 0u, p,
                                        (uint16_t)APAD_LEN_INPUTCAPS);
                        fp->stage = 2;
                    }
                }
                continue;
            }
            if (fp->stage == 2 && !a_held) {
                size_t last = (size_t)APAD_KEYBOARD_RING_DEPTH - 1u;
                unsigned i;
                int any = 0;

                for (i = 0; i < APAD_KEY_BITMAP_BYTES; i++) {
                    if (kb.keys[i] != 0u) { any = 1; break; }
                }
                if (!any) {
                    fp->release_seen = 1;
                    /* §6.20: the release is an EDGE, so it must be in the ring
                     * as well as absent from the snapshot -- newest event in
                     * the last slot, because the ring is right-aligned. */
                    if (kb.events[last].usage == APAD_HID_KEY_A
                        && (kb.events[last].flags & APAD_KBM_EVENT_DOWN) == 0u) {
                        fp->release_had_up_event = 1;
                    }
                }
            }
            continue;
        }
        if (pkt.header.type == (uint8_t)APAD_MSG_PING) {
            apad_ping ping, pong;
            uint8_t   payload[APAD_LEN_PING];

            memset(&ping, 0, sizeof ping);
            if (apad_decode_ping(pkt.payload, pkt.payload_len, &ping) < 0) {
                continue;
            }
            memset(&pong, 0, sizeof pong);
            pong.origin_ticks_ms    = ping.origin_ticks_ms;
            pong.responder_ticks_ms = apad_ticks_ms();
            if (apad_encode_ping(payload, sizeof payload, &pong)
                == (int)APAD_LEN_PING) {
                (void)fake_send(fp, &from, (uint8_t)APAD_MSG_PONG, 3u, 0u,
                                payload, (uint16_t)APAD_LEN_PING);
            }
            continue;
        }
    }
    return NULL;
}

static int run_release_on_clear(uint16_t port)
{
    fake_peer          fp;
    pthread_t          th;
    apad_client       *c;
    apad_client_kbm_in k;
    uint32_t           start;
    int                rc, failures = 0;

    memset(&fp, 0, sizeof fp);
    rc = apad_udp_open_exclusive(&fp.sock, port);
    if (rc != APAD_OK || fp.sock == NULL) {
        fprintf(stderr, "release-on-clear: cannot bind scratch port %u "
                        "(rc=%d)\n", (unsigned)port, rc);
        return 1;
    }
    if (pthread_create(&th, NULL, fake_release_thread, &fp) != 0) {
        fprintf(stderr, "release-on-clear: pthread_create failed\n");
        apad_udp_close(fp.sock);
        return 1;
    }
    c = apad_client_create("engine-client release", APAD_CAP_FACE4);
    if (c == NULL) {
        fprintf(stderr, "release-on-clear: apad_client_create failed\n");
        fp.stop = 1;
        (void)pthread_join(th, NULL);
        apad_udp_close(fp.sock);
        return 1;
    }
    rc = apad_client_connect(c, "127.0.0.1", port, 60, 3000);
    if (rc != APAD_OK) {
        fprintf(stderr, "release-on-clear: connect failed (rc=%d)\n", rc);
        failures++;
        goto out;
    }

    /* Hold A throughout. The caller never stops pressing it -- the ONLY
     * thing that lifts this key is the engine's own §6.19 release. */
    memset(&k, 0, sizeof k);
    k.have = APAD_KBM_FEATURE_KEYBOARD;
    k.keyboard.keys[APAD_KEY_BYTE(APAD_HID_KEY_A)] =
        APAD_KEY_MASK(APAD_HID_KEY_A);

    start = apad_ticks_ms();
    while (apad_time_since(apad_ticks_ms(), start) < 2500u) {
        if (pump_once(c, &k) != 0) {
            fprintf(stderr, "release-on-clear: session left ACTIVE\n");
            failures++;
            goto out;
        }
    }

    printf("release-on-clear: peer saw %d held KEYBOARD datagrams, worst "
           "inter-arrival gap %u ms\n", fp.kb_held_seen,
           (unsigned)fp.kb_max_gap);
    if (fp.stage < 2) {
        fprintf(stderr, "  [FAIL] the peer never got far enough to clear the "
                        "feature bit (stage=%d)\n", fp.stage);
        failures++;
    }
    if (fp.kb_max_gap == 0u || fp.kb_max_gap > 100u) {
        fprintf(stderr, "  [FAIL] held repeat gap %u ms is not §6.20's 10 Hz "
                        "floor or better\n", (unsigned)fp.kb_max_gap);
        failures++;
    } else {
        printf("  [PASS] held repeat stayed at or better than 10 Hz "
               "(worst gap %u ms <= 100 ms)\n", (unsigned)fp.kb_max_gap);
    }
    if (!fp.release_seen) {
        fprintf(stderr, "  [FAIL] features cleared and NO release datagram "
                        "followed -- the key stays held forever (§6.19)\n");
        failures++;
    } else {
        printf("  [PASS] a KEYBOARD with keys[] cleared went out after the "
               "features bit cleared\n");
    }
    if (!fp.release_had_up_event) {
        fprintf(stderr, "  [FAIL] the release carried no UP event in the ring's "
                        "newest slot (§6.20 right-alignment)\n");
        failures++;
    } else {
        printf("  [PASS] the release carried KEY_A UP in the ring's newest "
               "slot\n");
    }
    if (fp.kb_after_release != 0) {
        fprintf(stderr, "  [FAIL] %d KEYBOARD datagram(s) sent AFTER the "
                        "release; §6.19 says sending must stop\n",
                fp.kb_after_release);
        failures++;
    } else {
        printf("  [PASS] no KEYBOARD datagram after the release: sending "
               "stopped\n");
    }

out:
    apad_client_destroy(c);
    fp.stop = 1;
    (void)pthread_join(th, NULL);
    apad_udp_close(fp.sock);
    printf("release-on-clear: %d failure(s)\n", failures);
    return (failures == 0) ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *ip;
    uint16_t port;
    unsigned iters = 240; /* ~4s at 60 Hz — comfortably past several PINGs */
    const char *secret = NULL; /* S10 pairing secret; enables paired-session runs */
    apad_client *c;
    apad_client_stats st;
    int rc;
    unsigned i;
    int kbm_mode = 0;
    unsigned attach_wait_ms = 4000u;
    const char *device_name = "AtticPad 3DS engine-client";
    int a;

    /* --inputcaps-reorder needs no server and no target: it brings its own
     * peer up on a scratch port. Handled before the --target parse for that
     * reason. Never port 21100 -- that is a human's long-lived dev server. */
    if (argc >= 2 && strcmp(argv[1], "--release-on-clear") == 0) {
        uint16_t p = (argc >= 3) ? (uint16_t)atoi(argv[2]) : (uint16_t)21188;
        if (apad_net_init() != APAD_OK) {
            fprintf(stderr, "apad_net_init failed\n");
            return 1;
        }
        return run_release_on_clear(p);
    }
    if (argc >= 2 && strcmp(argv[1], "--inputcaps-reorder") == 0) {
        uint16_t p = (argc >= 3) ? (uint16_t)atoi(argv[2]) : (uint16_t)21187;
        if (apad_net_init() != APAD_OK) {
            fprintf(stderr, "apad_net_init failed\n");
            return 1;
        }
        return run_inputcaps_reorder(p);
    }

    if (argc < 4 || strcmp(argv[1], "--target") != 0) {
        fprintf(stderr,
                "usage: %s --target <ip> <port> [iters [--secret <pin>]] "
                "[--kbm [attach_wait_ms]] [--device-name <name>] "
                "[--hold <BTN>]\n"
                "       %s --inputcaps-reorder [scratch_port]\n"
                "       %s --release-on-clear [scratch_port]\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    for (a = 4; a < argc; a++) {
        if (strcmp(argv[a], "--kbm") == 0) {
            kbm_mode = 1;
            if (a + 1 < argc && argv[a + 1][0] != '-') {
                attach_wait_ms = (unsigned)atoi(argv[a + 1]);
            }
        } else if (strcmp(argv[a], "--device-name") == 0 && a + 1 < argc) {
            device_name = argv[a + 1];
            a++;
        } else if (strcmp(argv[a], "--hold") == 0 && a + 1 < argc) {
            g_hold_buttons = wire_button_from_name(argv[a + 1]);
            if (g_hold_buttons == 0u) {
                fprintf(stderr, "--hold: unknown wire button \"%s\"\n",
                        argv[a + 1]);
                return 2;
            }
            printf("holding wire button %s (0x%05lx) for the whole run\n",
                   argv[a + 1], (unsigned long)g_hold_buttons);
            a++;
        }
    }
    ip = argv[2];
    port = (uint16_t)atoi(argv[3]);
    if (argc >= 5 && argv[4][0] != '-') {
        iters = (unsigned)atoi(argv[4]);
    }
    if (argc >= 7 && strcmp(argv[5], "--secret") == 0) {
        secret = argv[6];
    }

    if (apad_net_init() != APAD_OK) {
        fprintf(stderr, "apad_net_init failed\n");
        return 1;
    }

    c = apad_client_create(device_name,
                           APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_SHOULDER |
                           APAD_CAP_STICK_L | APAD_CAP_STICK_R |
                           APAD_CAP_TOUCH);
    if (c == NULL) {
        fprintf(stderr, "apad_client_create failed\n");
        return 1;
    }

    /* S7 tier 3: unicast DISCOVER. Also tells us up front whether the server
     * wants pairing (S6.2) — this tool has no human to ask, so a pairing
     * server is a hard fail rather than a hang. */
    rc = apad_client_probe(c, ip, port, 2000);
    if (rc != APAD_OK) {
        fprintf(stderr, "probe: no ANNOUNCE from %s:%u (rc=%d)\n", ip, port, rc);
        apad_client_destroy(c);
        return 1;
    }
    apad_client_get_stats(c, &st);
    printf("probe: ANNOUNCE ok, pairing_required=%d\n", (int)st.pairing_required);
    if (secret != NULL) {
        if (apad_client_set_secret(c, secret) != APAD_OK) {
            fprintf(stderr, "set_secret rejected\n");
            apad_client_destroy(c);
            return 1;
        }
        printf("secret installed (S10)\n");
    } else if (st.pairing_required == 1) {
        fprintf(stderr, "server requires pairing; engine-client is unattended "
                        "(pass --secret <pin>)\n");
        apad_client_destroy(c);
        return 1;
    }

    rc = apad_client_connect(c, ip, port, 60, 5000);
    if (rc != APAD_OK) {
        fprintf(stderr, "connect failed (rc=%d): %s\n", rc, apad_client_message(c));
        apad_client_destroy(c);
        return 1;
    }
    apad_client_get_stats(c, &st);
    printf("connected: session_id=%d pad_slot=%d rate=%d Hz\n",
           (int)st.session_id, (int)st.pad_slot, (int)st.input_rate_hz);

    /* §6.15-§6.19. Runs INSTEAD of the plain INPUT_STATE loop below: the two
     * exercise different halves of the engine and interleaving them would
     * make an evtest capture harder to read for no gain. */
    if (kbm_mode) {
        int krc = run_kbm(c, attach_wait_ms);
        if (krc != 0) {
            apad_client_destroy(c);
            return krc;
        }
        apad_client_get_stats(c, &st);
        printf("pumped kbm sequence: tx=%u rx=%u rtt_ms=%d\n",
               st.tx_packets, st.rx_packets, (int)st.rtt_ms);
        apad_client_disconnect(c);
        apad_client_get_stats(c, &st);
        printf("disconnected: state=%d\n", (int)st.state);
        apad_client_destroy(c);
        sleep_ms(20);
        printf("engine-client: PASS\n");
        return 0;
    }

    for (i = 0; i < iters; i++) {
        apad_input_state in;
        fill_input(&in, i);
        rc = apad_client_pump(c, &in, 16);
        if (rc != APAD_CLIENT_ACTIVE) {
            apad_client_get_stats(c, &st);
            fprintf(stderr,
                    "session left ACTIVE at iter %u: state=%d close_reason=%d "
                    "last_error=%d msg=\"%s\"\n",
                    i, (int)st.state, (int)st.close_reason,
                    (int)st.last_error, apad_client_message(c));
            apad_client_destroy(c);
            return 1;
        }
    }

    apad_client_get_stats(c, &st);
    printf("pumped %u iterations: tx=%u rx=%u rtt_ms=%d\n",
           iters, st.tx_packets, st.rx_packets, (int)st.rtt_ms);
    if (st.rtt_ms < 0) {
        fprintf(stderr, "no PONG ever arrived (rtt_ms=%d) — S6.6 broken\n",
                (int)st.rtt_ms);
        apad_client_destroy(c);
        return 1;
    }

    apad_client_disconnect(c);
    apad_client_get_stats(c, &st);
    printf("disconnected: state=%d\n", (int)st.state);
    apad_client_destroy(c);

    /* Give the BYE datagram a moment to leave before the process exits;
     * purely cosmetic for the server log. */
    sleep_ms(20);
    printf("engine-client: PASS\n");
    return 0;
}
