/* tools/loopback-client/main.c
 *
 * Headless AtticPad client. docs/DESIGN.md D7: "Sharing a process would muddy the
 * backend interface and, worse, create a test path that bypasses the real
 * socket layer -- which is precisely where platform bugs live.
 * tools/loopback-client stays a separate binary and is what CI's
 * integration job drives."
 *
 * This binary talks over a REAL UDP socket via the platform shim
 * (shim/net_bsd.c on Linux) -- every datagram goes through sendto()/
 * recvfrom() on an actual kernel socket, never an in-process function call
 * standing in for the network. It links libapad (core/src/) exactly like
 * any other client would, through the public API in core/include/atticpad/.
 *
 * Two modes:
 *
 *   (no args)                     Self-loopback smoke test. Opens TWO real
 *                                 UDP sockets on 127.0.0.1 in this one
 *                                 process (one standing in for "the server
 *                                 side" of the socket, one as "the client
 *                                 side"), and drives a DISCOVER + a run of
 *                                 INPUT_STATE datagrams between them,
 *                                 including a sequence-wrap crossing.
 *                                 Every send/receive still goes through the
 *                                 real BSD-sockets shim; nothing is
 *                                 short-circuited. This is a standalone
 *                                 sanity check of THIS BINARY's use of
 *                                 libapad + the socket shim -- it is not a
 *                                 substitute for testing against a real,
 *                                 separately-run server process.
 *
 *   --target <ip> <port>          Real integration mode, docs/DESIGN.md S8.1's
 *                                 "loopback client -> uinput server, asserts
 *                                 input arrives": drives the FULL S8 session
 *                                 lifecycle against a real, separately-run
 *                                 server process --
 *                                   DISCOVER -> ANNOUNCE (S7 tier 2)
 *                                   HELLO -> WELCOME     (S6.3/S6.4, HELLO is
 *                                                          RELIABLE: retried
 *                                                          per the S9
 *                                                          100/200/400/800ms
 *                                                          schedule via
 *                                                          apad_session)
 *                                   duplicate HELLO ->    (S9 "Duplicates":
 *                                   WELCOME (again)        the ORIGINAL HELLO
 *                                                          resent byte-for-
 *                                                          byte, same
 *                                                          sequence; the
 *                                                          server's answer
 *                                                          MUST be byte-
 *                                                          identical to the
 *                                                          first WELCOME,
 *                                                          not a fresh one --
 *                                                          this is exactly
 *                                                          the rule set
 *                                                          behind a session
 *                                                          that silently
 *                                                          dies at
 *                                                          t~=2300ms with
 *                                                          input flowing
 *                                                          normally)
 *                                   INPUT_STATE stream    (>3000ms, past
 *                                                          S11's idle
 *                                                          timeout, then a
 *                                                          fresh PING/PONG
 *                                                          as a liveness
 *                                                          probe -- proves
 *                                                          the duplicate
 *                                                          above didn't
 *                                                          silently kill the
 *                                                          session)
 *                                   INPUT_STATE x3        (S5, using the
 *                                                          session_id WELCOME
 *                                                          assigned, not 0;
 *                                                          the original,
 *                                                          unchanged happy-
 *                                                          path check)
 *                                   PING -> PONG          (S6.6, correlated
 *                                                          by origin_ticks_ms
 *                                                          alone; the
 *                                                          original,
 *                                                          unchanged check)
 *                                   (server PING -> our    (S6.6, NEW: the
 *                                    PONG, whenever it       server now
 *                                    arrives)                originates its
 *                                                            own PING at 1Hz
 *                                                            per active
 *                                                            session; this
 *                                                            client answers
 *                                                            it wherever in
 *                                                            the flow above
 *                                                            it lands, not
 *                                                            as a separate
 *                                                            numbered step)
 *                                   BYE                   (S6.5, best-effort,
 *                                                          so the server frees
 *                                                          the pad slot
 *                                                          instead of waiting
 *                                                          out the 3s idle
 *                                                          timeout)
 *                                 Session-lifecycle logic (retransmit timing,
 *                                 which flags to set, when to adopt
 *                                 session_id) is never hand-rolled here --
 *                                 docs/CONVENTIONS.md: "Clients call libapad and the
 *                                 shim. Never reimplement protocol logic in
 *                                 platform code." -- it all goes through the
 *                                 apad_session_* API in atticpad.h. The one
 *                                 deliberate exception is the duplicate
 *                                 HELLO resend itself, which must NOT go
 *                                 through apad_session_next_header (that
 *                                 would allocate a fresh sequence, which is
 *                                 precisely what S9 forbids for a retry) --
 *                                 it replays the exact captured bytes.
 *
 * Exit code, self-loopback mode: 0 if every internal round-trip /
 * self-consistency check passed, 1 otherwise.
 *
 * Exit code, target mode: 0 ONLY if ALL of the following hold: WELCOME
 * arrived (session established); the duplicate HELLO's answer was byte-
 * identical to the first WELCOME (S9 Duplicates); the session was still
 * alive (a PONG arrived) after >3000ms of INPUT_STATE past the duplicate;
 * the original PING correlated (echoed origin_ticks_ms unchanged); and at
 * least one server-originated PING (S6.6, 1Hz per active session) was
 * received and answered with a PONG somewhere over the run -- this is what
 * lets the server's own rtt_ms for this client become known, which is the
 * whole reason the server originates PINGs at all. This runs in CI's
 * integration job -- a silent pass is worse than useless, so a dead session
 * anywhere in this sequence is a hard failure, not a warning.
 *
 * Prints a one-line summary per step to stdout (this is a desktop tool, not
 * core/ or shim/ -- docs/CONVENTIONS.md's "no stdio" rule is scoped to those two
 * directories).
 */
#define _POSIX_C_SOURCE 200809L /* nanosleep(), matching shim/net_bsd.c's own feature-test macro */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "atticpad/atticpad.h"

/* usleep() is finicky about feature-test macros across libcs; nanosleep()
 * under _POSIX_C_SOURCE 200809L is not. Used only to pace this desktop
 * tool's traffic -- not a core/ or shim/ concern. */
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, NULL);
}

#define TEST_SERVER_PORT 21199u  /* deliberately not APAD_DEFAULT_PORT, so the
                                   * self-loopback smoke test never collides
                                   * with a real server that might also be
                                   * running on this machine. */
#define RECV_TIMEOUT_MS 500

static int g_failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  [PASS] %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        g_failures++;
    }
}

static void hexdump(const char *label, const uint8_t *buf, size_t len) {
    size_t i;
    printf("  %s (%u bytes):", label, (unsigned)len);
    for (i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\n");
}

/* Builds and sends one INPUT_STATE datagram with distinctive, varying field
 * values so a decode bug can't hide behind a coincidence. Returns the
 * apad_input_state that was encoded, so the caller can compare it against
 * whatever the far end decodes. */
static void build_input_state(apad_input_state *st, uint16_t iter, uint32_t ticks) {
    static const uint32_t dpad_cycle[4] = {
        APAD_BTN_DPAD_UP,
        APAD_BTN_DPAD_UP | APAD_BTN_DPAD_RIGHT,
        APAD_BTN_DPAD_DOWN | APAD_BTN_DPAD_LEFT, /* impossible combo, on purpose */
        APAD_BTN_DPAD_RIGHT,
    };
    int i;

    memset(st, 0, sizeof(*st));
    st->buttons = APAD_BTN_A | dpad_cycle[iter % 4u];
    st->axes[APAD_AXIS_LX] = (int16_t)(1000 + iter);
    st->axes[APAD_AXIS_LY] = (int16_t)-(1000 + iter);
    st->axes[APAD_AXIS_RX] = 0;
    st->axes[APAD_AXIS_RY] = 0;
    st->axes[APAD_AXIS_L2] = (int16_t)(iter % 32768u);
    st->axes[APAD_AXIS_R2] = 0;
    st->touch_count = (uint8_t)(iter % 3u); /* occasionally 2, occasionally an
                                              * out-of-range value clamped
                                              * server-side is covered by the
                                              * conformance vectors, not here */
    for (i = 0; i < APAD_TOUCH_MAX; i++) {
        st->touches[i].id = (uint8_t)(i + 1);
        st->touches[i].pressure = 128;
        st->touches[i].x = (int16_t)(100 * (i + 1));
        st->touches[i].y = (int16_t)(-100 * (i + 1));
    }
    st->battery = 77;
    st->client_ticks_ms = ticks;
}

/* Compares two apad_input_state values the way the SENDER's own encoded
 * struct should compare to the RECEIVER's decoded struct, honoring the
 * §2/§5 receive-side transformations (reserved-field scrub, touch-count
 * clamp, negative-trigger clamp) rather than requiring byte-identity --
 * this tool builds well-formed input on purpose, so in practice those
 * transformations are no-ops here, but coding the comparison this way keeps
 * it honest about what "correct decode" actually means per the spec. */
static int input_state_matches(const apad_input_state *sent, const apad_input_state *got) {
    int ok = 1;
    uint8_t expect_touch_count = sent->touch_count > 2 ? 2 : sent->touch_count;
    int i;

    if ((sent->buttons & 0x000FFFFFu) != got->buttons) ok = 0;
    for (i = 0; i < 6; i++) { /* axes 0..5 are not reserved */
        int16_t want = sent->axes[i];
        if (i == APAD_AXIS_L2 || i == APAD_AXIS_R2) {
            if (want < 0) want = 0;
        }
        if (want != got->axes[i]) ok = 0;
    }
    if (got->axes[6] != 0 || got->axes[7] != 0) ok = 0;
    if (expect_touch_count != got->touch_count) ok = 0;
    if (got->reserved0 != 0) ok = 0;
    for (i = 0; i < APAD_TOUCH_MAX; i++) {
        if (i < expect_touch_count) {
            if (sent->touches[i].id != got->touches[i].id
                || sent->touches[i].pressure != got->touches[i].pressure
                || sent->touches[i].x != got->touches[i].x
                || sent->touches[i].y != got->touches[i].y) {
                ok = 0;
            }
        } else {
            if (got->touches[i].id != 0 || got->touches[i].pressure != 0
                || got->touches[i].x != 0 || got->touches[i].y != 0) {
                ok = 0;
            }
        }
    }
    if (sent->battery != got->battery) ok = 0;
    if (sent->client_ticks_ms != got->client_ticks_ms) ok = 0;
    return ok;
}

/* One INPUT_STATE round trip: encode -> apad_packet_build -> real UDP send
 * -> real UDP recv -> apad_packet_parse -> apad_decode_input_state ->
 * compare. Returns 0 on success. */
static int run_input_state_round_trip(apad_sock *tx, apad_sock *rx,
                                       const apad_addr *to, uint16_t sequence,
                                       uint16_t iter, uint32_t ticks) {
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_INPUT_STATE];
    apad_input_state sent, got;
    apad_header hdr;
    apad_packet pkt;
    apad_addr from;
    int n, m, rc;

    build_input_state(&sent, iter, ticks);
    if (apad_encode_input_state(payload, sizeof payload, &sent) != (int)APAD_LEN_INPUT_STATE) {
        printf("  [FAIL] apad_encode_input_state (seq=%u)\n", (unsigned)sequence);
        return 1;
    }

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = APAD_MAGIC;
    hdr.version = APAD_VERSION;
    hdr.type = (uint8_t)APAD_MSG_INPUT_STATE;
    hdr.session_id = 1;
    hdr.sequence = sequence;
    hdr.flags = 0;

    n = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)sizeof payload, NULL, 0);
    if (n < 0) {
        printf("  [FAIL] apad_packet_build INPUT_STATE seq=%u rc=%d\n", (unsigned)sequence, n);
        return 1;
    }

    m = apad_udp_send(tx, to, buf, (size_t)n);
    if (m != n) {
        printf("  [FAIL] apad_udp_send INPUT_STATE seq=%u sent=%d want=%d\n",
               (unsigned)sequence, m, n);
        return 1;
    }

    {
        uint8_t rbuf[APAD_MAX_DATAGRAM];
        int rn = apad_udp_recv(rx, &from, rbuf, sizeof rbuf, RECV_TIMEOUT_MS);
        if (rn <= 0) {
            printf("  [FAIL] apad_udp_recv timed out or errored for seq=%u (rc=%d)\n",
                   (unsigned)sequence, rn);
            return 1;
        }
        if (rn != n) {
            printf("  [FAIL] received length %d != sent length %d (seq=%u)\n", rn, n,
                   (unsigned)sequence);
            return 1;
        }

        memset(&pkt, 0, sizeof pkt);
        rc = apad_packet_parse(rbuf, (size_t)rn, &pkt);
        if (rc < 0) {
            printf("  [FAIL] apad_packet_parse seq=%u rc=%d\n", (unsigned)sequence, rc);
            return 1;
        }
        if (pkt.header.type != (uint8_t)APAD_MSG_INPUT_STATE
            || pkt.header.sequence != sequence
            || pkt.payload_len != APAD_LEN_INPUT_STATE) {
            printf("  [FAIL] decoded header mismatch seq=%u (got type=%u seq=%u len=%u)\n",
                   (unsigned)sequence, pkt.header.type, pkt.header.sequence, pkt.payload_len);
            return 1;
        }

        memset(&got, 0, sizeof got);
        rc = apad_decode_input_state(pkt.payload, pkt.payload_len, &got);
        if (rc < 0) {
            printf("  [FAIL] apad_decode_input_state seq=%u rc=%d\n", (unsigned)sequence, rc);
            return 1;
        }

        if (!input_state_matches(&sent, &got)) {
            printf("  [FAIL] decoded INPUT_STATE does not match what was sent (seq=%u)\n",
                   (unsigned)sequence);
            return 1;
        }
    }

    printf("  [PASS] INPUT_STATE round trip seq=%u buttons=0x%08X hat=%u ticks=%u\n",
           (unsigned)sequence, (unsigned)got.buttons,
           (unsigned)apad_hat_from_buttons(got.buttons), (unsigned)got.client_ticks_ms);
    return 0;
}

static int run_discover_round_trip(apad_sock *tx, apad_sock *rx, const apad_addr *to) {
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t rbuf[APAD_MAX_DATAGRAM];
    apad_header hdr;
    apad_packet pkt;
    apad_addr from;
    int n, m, rn, rc;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = APAD_MAGIC;
    hdr.version = APAD_VERSION;
    hdr.type = (uint8_t)APAD_MSG_DISCOVER;
    hdr.session_id = 0;
    hdr.sequence = 0;
    hdr.flags = 0;

    n = apad_packet_build(buf, sizeof buf, &hdr, NULL, 0, NULL, 0);
    if (n < 0) {
        printf("  [FAIL] apad_packet_build DISCOVER rc=%d\n", n);
        return 1;
    }
    m = apad_udp_send(tx, to, buf, (size_t)n);
    if (m != n) {
        printf("  [FAIL] apad_udp_send DISCOVER sent=%d want=%d\n", m, n);
        return 1;
    }

    rn = apad_udp_recv(rx, &from, rbuf, sizeof rbuf, RECV_TIMEOUT_MS);
    if (rn <= 0) {
        printf("  [FAIL] apad_udp_recv DISCOVER timed out or errored (rc=%d)\n", rn);
        return 1;
    }

    memset(&pkt, 0, sizeof pkt);
    rc = apad_packet_parse(rbuf, (size_t)rn, &pkt);
    if (rc < 0 || pkt.header.type != (uint8_t)APAD_MSG_DISCOVER
        || pkt.header.session_id != 0 || pkt.payload_len != 0) {
        printf("  [FAIL] DISCOVER round trip mismatch (rc=%d type=%u sid=%u len=%u)\n",
               rc, pkt.header.type, pkt.header.session_id, pkt.payload_len);
        hexdump("received", rbuf, (size_t)rn);
        return 1;
    }

    printf("  [PASS] DISCOVER round trip (%d bytes) from %u.%u.%u.%u:%u\n", rn,
           from.ip[0], from.ip[1], from.ip[2], from.ip[3], (unsigned)from.port);
    return 0;
}

static int self_loopback_mode(void) {
    apad_sock *server_sock;
    apad_sock *client_sock;
    apad_addr server_addr;
    uint16_t seqs[6];
    size_t i;

    printf("== AtticPad loopback-client: self-loopback smoke test ==\n");
    printf("Two real UDP sockets on 127.0.0.1, port %u. Every datagram below\n"
           "goes through sendto()/recvfrom() on an actual kernel socket.\n\n",
           (unsigned)TEST_SERVER_PORT);

    if (apad_net_init() != APAD_OK) {
        printf("apad_net_init failed\n");
        return 1;
    }

    server_sock = apad_udp_open((uint16_t)TEST_SERVER_PORT);
    check(server_sock != NULL, "apad_udp_open (server side, fixed port)");
    if (server_sock == NULL) {
        return 1;
    }

    client_sock = apad_udp_open(0); /* ephemeral local port */
    check(client_sock != NULL, "apad_udp_open (client side, ephemeral port)");
    if (client_sock == NULL) {
        apad_udp_close(server_sock);
        return 1;
    }

    apad_addr_set(&server_addr, 127, 0, 0, 1, (uint16_t)TEST_SERVER_PORT);

    printf("\n-- DISCOVER --\n");
    (void)run_discover_round_trip(client_sock, server_sock, &server_addr);

    printf("\n-- INPUT_STATE run, including a sequence-wrap crossing at 0xFFFF --\n");
    seqs[0] = 0;
    seqs[1] = 1;
    seqs[2] = 100;
    seqs[3] = 0xFFFE;
    seqs[4] = 0xFFFF;
    seqs[5] = 0x0000; /* wraps: apad_seq_newer(seqs[5], seqs[4]) is true (§9) */
    for (i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        uint32_t ticks = (i == 4) ? 0xFFFFFFFEu : (uint32_t)(1000 + i); /* also
                                                    * exercise a tick value
                                                    * near the 2^32 wrap */
        (void)run_input_state_round_trip(client_sock, server_sock, &server_addr,
                                          seqs[i], (uint16_t)i, ticks);
    }
    check(apad_seq_newer(seqs[5], seqs[4]) != 0,
          "apad_seq_newer(0x0000, 0xFFFF) is true (S9 wrap)");
    check(apad_seq_newer(seqs[4], seqs[5]) == 0,
          "apad_seq_newer(0xFFFF, 0x0000) is false (S9 wrap, reverse)");

    apad_udp_close(client_sock);
    apad_udp_close(server_sock);

    printf("\n%d failure(s)\n", g_failures);
    return g_failures != 0;
}

/* S6.3 HELLO's client_id is "persistent, random at first run" on a real
 * client; this is a stateless CI tool, so it uses a fixed, log-friendly
 * 16-byte pattern instead -- readable in a server log, and exactly
 * APAD_CLIENT_ID_LEN bytes. */
static const char kClientId[APAD_CLIENT_ID_LEN] = "ATTICPAD-LOOPBAK";
static const char kDeviceName[] = "atticpad-loopback-client";

/* Forward declaration: full definition and header comment are below,
 * grouped with target_ping() where the S6.6 context is explained. Every
 * receive loop between here and there (target_hello, the duplicate-HELLO
 * scenario) needs to call it too, so it can't just be defined at its first
 * point of use. */
static void answer_if_server_ping(apad_session *sess, apad_sock *client_sock,
                                   const apad_addr *target, const apad_packet *pkt);

/* DISCOVER / ANNOUNCE, S7 tier 2. Not part of the reliable session FSM
 * (session_id is 0 for both, per S8), so it is sent directly rather than
 * through apad_session_*. Returns 1 and fills *pairing_required if an
 * ANNOUNCE came back within the timeout, else 0 -- this step is
 * best-effort: S7 tier 3 (manual entry) means a client can proceed to
 * HELLO without ever seeing an ANNOUNCE. */
static int target_discover(apad_sock *client_sock, const apad_addr *target,
                            int *pairing_required) {
    uint8_t buf[APAD_MAX_DATAGRAM];
    apad_header hdr;
    int n, m;

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = APAD_MAGIC;
    hdr.version = APAD_VERSION;
    hdr.type = (uint8_t)APAD_MSG_DISCOVER;
    n = apad_packet_build(buf, sizeof buf, &hdr, NULL, 0, NULL, 0);
    m = apad_udp_send(client_sock, target, buf, (size_t)n);
    printf("sent DISCOVER (%d bytes)\n", m);

    {
        uint8_t rbuf[APAD_MAX_DATAGRAM];
        apad_addr from;
        int rn = apad_udp_recv(client_sock, &from, rbuf, sizeof rbuf, RECV_TIMEOUT_MS);

        if (rn <= 0) {
            printf("no ANNOUNCE reply to DISCOVER within %dms -- continuing anyway "
                   "(S7 tier 3 manual entry doesn't require it)\n", RECV_TIMEOUT_MS);
            return 0;
        }

        {
            apad_packet pkt;
            memset(&pkt, 0, sizeof pkt);
            if (apad_packet_parse(rbuf, (size_t)rn, &pkt) < 0
                || pkt.header.type != (uint8_t)APAD_MSG_ANNOUNCE) {
                printf("received %d bytes that did not parse as ANNOUNCE -- continuing\n", rn);
                return 0;
            }
            {
                apad_announce ann;
                char name[APAD_NAME_LEN + 1];

                memset(&ann, 0, sizeof ann);
                if (apad_decode_announce(pkt.payload, pkt.payload_len, &ann) < 0) {
                    printf("ANNOUNCE received but failed to decode -- continuing\n");
                    return 0;
                }
                apad_text_get(name, sizeof name, ann.server_name, APAD_NAME_LEN);
                printf("received ANNOUNCE (%d bytes) from %u.%u.%u.%u:%u: "
                       "server_name=\"%s\" pads=%u/%u pairing_required=%u\n",
                       rn, from.ip[0], from.ip[1], from.ip[2], from.ip[3],
                       (unsigned)from.port, name, (unsigned)ann.pads_free,
                       (unsigned)ann.pads_total, (unsigned)ann.pairing_required);
                *pairing_required = ann.pairing_required;
                return 1;
            }
        }
    }
}

/* Captures the exact bytes of the original HELLO and the WELCOME it earned,
 * so a later duplicate-HELLO scenario can (a) resend the identical HELLO
 * datagram -- S9: "a sender MUST NOT allocate a new sequence number for a
 * retry" -- and (b) compare the server's second answer against the first
 * byte for byte -- S9: "MUST retransmit its original answer verbatim". */
typedef struct {
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    int     hello_len;
    uint8_t welcome_buf[APAD_MAX_DATAGRAM];
    int     welcome_len;
} target_hello_result;

/* S6.3/S6.4/S9: HELLO, reliable, retried on the S9 100/200/400/800ms
 * schedule (failing at t=2300ms) until WELCOME moves the session to ACTIVE.
 * Entirely driven by apad_session_* -- this function never decides *when*
 * to retransmit, only performs the send apad_session_tick asks for. Returns
 * 1 iff WELCOME was received and the session adopted session_id/pad_slot;
 * on success, fills *result with the exact HELLO and WELCOME datagrams. */
static int target_hello(apad_session *sess, apad_sock *client_sock, const apad_addr *target,
                         target_hello_result *result) {
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    uint8_t hello_payload[APAD_LEN_HELLO];
    apad_hello hello;
    apad_header hdr;
    int hello_len;
    uint32_t now, loop_start;

    memset(&hello, 0, sizeof hello);
    memcpy(hello.client_id, kClientId, APAD_CLIENT_ID_LEN);
    hello.caps = APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_STICK_L
               | APAD_CAP_TRIGGERS | APAD_CAP_TOUCH;
    apad_text_set(hello.device_name, sizeof hello.device_name, kDeviceName);
    memset(hello.client_nonce, 0xA5, sizeof hello.client_nonce); /* unused without
                                                                   * S10 PIN pairing */
    hello.desired_rate_hz = (uint16_t)APAD_DEFAULT_RATE_HZ;
    hello.proto_major = (uint8_t)APAD_VERSION;
    hello.client_ticks_ms = apad_ticks_ms();

    if (apad_encode_hello(hello_payload, sizeof hello_payload, &hello) != (int)APAD_LEN_HELLO) {
        printf("  [FAIL] apad_encode_hello\n");
        return 0;
    }
    if (apad_session_next_header(sess, (uint8_t)APAD_MSG_HELLO, &hdr) != APAD_OK) {
        printf("  [FAIL] apad_session_next_header(HELLO)\n");
        return 0;
    }
    hello_len = apad_packet_build(hello_buf, sizeof hello_buf, &hdr, hello_payload,
                                   (uint16_t)sizeof hello_payload, NULL, 0);
    if (hello_len < 0) {
        printf("  [FAIL] apad_packet_build(HELLO) rc=%d\n", hello_len);
        return 0;
    }
    memcpy(result->hello_buf, hello_buf, (size_t)hello_len);
    result->hello_len = hello_len;

    now = apad_ticks_ms();
    (void)apad_udp_send(client_sock, target, hello_buf, (size_t)hello_len);
    apad_session_on_sent(sess, &hdr, now);
    printf("sent HELLO seq=%u (%d bytes), device_name=\"%s\"\n",
           (unsigned)hdr.sequence, hello_len, kDeviceName);

    loop_start = now;
    for (;;) {
        int action;
        uint8_t rbuf[APAD_MAX_DATAGRAM];
        apad_addr from;
        int rn;

        now = apad_ticks_ms();
        action = apad_session_tick(sess, now);
        if (action == APAD_ACT_RETRANSMIT) {
            (void)apad_udp_send(client_sock, target, hello_buf, (size_t)hello_len);
            printf("  (retransmitting HELLO per S9 schedule, t=%ums)\n",
                   (unsigned)apad_time_since(now, loop_start));
        } else if (action == APAD_ACT_TIMEOUT) {
            printf("  [FAIL] HELLO retransmits exhausted (S9: 100/200/400/800ms, "
                   "fails at t=2300ms) -- no WELCOME\n");
            return 0;
        }

        rn = apad_udp_recv(client_sock, &from, rbuf, sizeof rbuf, 100);
        if (rn > 0) {
            apad_packet pkt;
            memset(&pkt, 0, sizeof pkt);
            if (apad_packet_parse(rbuf, (size_t)rn, &pkt) >= 0) {
                if (pkt.header.type == (uint8_t)APAD_MSG_ANNOUNCE) {
                    continue; /* a stray/duplicate ANNOUNCE; not part of the session */
                }
                answer_if_server_ping(sess, client_sock, target, &pkt); /* S6.6, no-op unless PING */
                (void)apad_session_on_recv(sess, &pkt, apad_ticks_ms());
                if (pkt.header.type == (uint8_t)APAD_MSG_WELCOME
                    && sess->state == APAD_SESSION_ACTIVE) {
                    apad_welcome w;
                    memset(&w, 0, sizeof w);
                    if (apad_decode_welcome(pkt.payload, pkt.payload_len, &w) >= 0) {
                        printf("  [PASS] WELCOME received: session_id=%u pad_slot=%u "
                               "input_rate_hz=%u auth_required=%u\n",
                               (unsigned)w.session_id, (unsigned)w.pad_slot,
                               (unsigned)w.input_rate_hz,
                               (unsigned)(w.flags & APAD_WELCOME_AUTH_REQUIRED));
                    }
                    memcpy(result->welcome_buf, rbuf, (size_t)rn);
                    result->welcome_len = rn;

                    /* WELCOME is itself RELIABLE (S4), server -> client. The
                     * server arms its own S9 retransmit schedule for it and
                     * -- confirmed empirically: a client that never ACKs
                     * WELCOME gets its session torn down by the server as
                     * "reliable delivery failed" once that schedule
                     * exhausts (S9's t=2300ms), even while INPUT_STATE is
                     * still flowing fine, since INPUT_STATE does not
                     * disarm it. S9: "ACK echoes the acknowledged
                     * sequence." ACK itself is not reliable (S4), so this
                     * is a single fire-and-forget send, no retransmit loop
                     * needed. */
                    {
                        uint8_t ack_buf[APAD_MAX_DATAGRAM];
                        uint8_t ack_payload[APAD_LEN_ACK];
                        apad_ack ack;
                        apad_header ack_hdr;
                        int ack_n;

                        memset(&ack, 0, sizeof ack);
                        ack.sequence = pkt.header.sequence; /* S6.10: sequence being acked */
                        (void)apad_encode_ack(ack_payload, sizeof ack_payload, &ack);
                        if (apad_session_next_header(sess, (uint8_t)APAD_MSG_ACK, &ack_hdr)
                            == APAD_OK) {
                            ack_n = apad_packet_build(ack_buf, sizeof ack_buf, &ack_hdr,
                                                       ack_payload, (uint16_t)sizeof ack_payload,
                                                       NULL, 0);
                            if (ack_n >= 0) {
                                (void)apad_udp_send(client_sock, target, ack_buf, (size_t)ack_n);
                                printf("sent ACK for WELCOME seq=%u (disarms the server's S9 "
                                       "retransmit of WELCOME)\n", (unsigned)ack.sequence);
                            }
                        }
                    }
                    return 1;
                }
            }
        }

        /* Belt-and-suspenders wall-clock cap: the S9 schedule fails by
         * t=2300ms on its own; this only guards against this harness
         * looping forever if that signal is ever missed. */
        if (apad_time_since(apad_ticks_ms(), loop_start) > 4000u) {
            printf("  [FAIL] gave up waiting for WELCOME after 4000ms wall clock\n");
            return 0;
        }
    }
}

/* S9 "Duplicates" (new since this tool's happy path was written):
 *
 *   - A retransmission MUST be byte-identical to the original, including
 *     its sequence. A sender MUST NOT allocate a new sequence number for a
 *     retry.
 *   - A peer that receives a duplicate of a request it has already
 *     answered MUST retransmit its original answer verbatim, and MUST NOT
 *     generate a fresh one.
 *   - A receiver MUST ACK every copy of a reliable message it receives,
 *     including duplicates of one it has already processed and
 *     acknowledged.
 *
 * This is exactly the rule set behind the failure the coordinator
 * reproduced against the real server -- a session that dies at t~=2300ms
 * with input flowing normally and nothing in any log to explain it:
 *
 *   HELLO from 127.0.0.1:33855 "scratch-sender" -> session 1, pad slot 0
 *   HELLO from 127.0.0.1:33855 "scratch-sender" -> session 1, pad slot 0
 *   session 1 (slot 0) closed: reliable delivery failed
 *
 * That happens when a server allocates a NEW session/WELCOME for the
 * duplicate HELLO instead of replaying the original: the ACK the client
 * already sent for WELCOME #1 no longer matches WELCOME #2's (new,
 * different) sequence, so the server's retransmit of WELCOME #2 goes
 * unacknowledged and the session dies once that schedule exhausts.
 *
 * This function resends the ORIGINAL HELLO byte-for-byte (not through
 * apad_session_next_header, which would wrongly allocate a new sequence)
 * and asserts the server's answer is byte-identical to the first WELCOME,
 * including its header sequence -- then ACKs that copy too, the client-side
 * half of the same rule. Returns 1 iff the duplicate was answered correctly. */
static int target_duplicate_hello_scenario(apad_session *sess, apad_sock *client_sock,
                                            const apad_addr *target,
                                            const target_hello_result *first) {
    uint8_t rbuf[APAD_MAX_DATAGRAM];
    uint32_t start;
    int rn = 0;
    int got_reply = 0;

    printf("resending the ORIGINAL HELLO datagram byte-for-byte (%d bytes, same "
           "sequence -- not a fresh apad_session_next_header call, per S9: \"a "
           "sender MUST NOT allocate a new sequence number for a retry\")\n",
           first->hello_len);
    (void)apad_udp_send(client_sock, target, first->hello_buf, (size_t)first->hello_len);

    start = apad_ticks_ms();
    for (;;) {
        apad_addr from;

        if (apad_time_since(apad_ticks_ms(), start) > 2000u) {
            break;
        }
        rn = apad_udp_recv(client_sock, &from, rbuf, sizeof rbuf, 200);
        if (rn > 0) {
            apad_packet pkt;
            memset(&pkt, 0, sizeof pkt);
            if (apad_packet_parse(rbuf, (size_t)rn, &pkt) >= 0) {
                if (pkt.header.type == (uint8_t)APAD_MSG_WELCOME) {
                    got_reply = 1;
                    break;
                }
                answer_if_server_ping(sess, client_sock, target, &pkt); /* S6.6, no-op unless PING */
            }
            /* a stray ANNOUNCE or similar; keep waiting for the WELCOME */
        }
    }

    if (!got_reply) {
        printf("  [FAIL] no WELCOME received in reply to the duplicate HELLO within "
               "2000ms -- possibly the known server-side bug above; see the report\n");
        return 0;
    }

    {
        apad_header hdr1, hdr2;
        int byte_identical;

        memset(&hdr1, 0, sizeof hdr1);
        memset(&hdr2, 0, sizeof hdr2);
        (void)apad_header_decode(first->welcome_buf, (size_t)first->welcome_len, &hdr1);
        (void)apad_header_decode(rbuf, (size_t)rn, &hdr2);

        byte_identical = (rn == first->welcome_len)
                          && (memcmp(rbuf, first->welcome_buf, (size_t)rn) == 0);

        printf("  original WELCOME:  %d bytes, header sequence=%u\n",
               first->welcome_len, (unsigned)hdr1.sequence);
        printf("  duplicate's answer: %d bytes, header sequence=%u\n",
               rn, (unsigned)hdr2.sequence);
        check(byte_identical,
              "duplicate HELLO's answer is byte-identical to the original WELCOME, "
              "including header sequence (S9 Duplicates: server MUST retransmit its "
              "original answer verbatim, MUST NOT generate a fresh one)");
        if (!byte_identical) {
            hexdump("original WELCOME ", first->welcome_buf, (size_t)first->welcome_len);
            hexdump("duplicate response", rbuf, (size_t)rn);
        }

        /* Client-side half of the same rule: ACK every copy, not just the
         * first (S9: "MUST ACK every copy... An ACK is not a statement
         * about the first copy; it is the answer to the datagram in
         * hand.") */
        {
            uint8_t ack_buf[APAD_MAX_DATAGRAM];
            uint8_t ack_payload[APAD_LEN_ACK];
            apad_ack ack;
            apad_header ack_hdr;
            int ack_n;

            memset(&ack, 0, sizeof ack);
            ack.sequence = hdr2.sequence;
            (void)apad_encode_ack(ack_payload, sizeof ack_payload, &ack);
            if (apad_session_next_header(sess, (uint8_t)APAD_MSG_ACK, &ack_hdr) == APAD_OK) {
                ack_n = apad_packet_build(ack_buf, sizeof ack_buf, &ack_hdr, ack_payload,
                                           (uint16_t)sizeof ack_payload, NULL, 0);
                if (ack_n >= 0) {
                    (void)apad_udp_send(client_sock, target, ack_buf, (size_t)ack_n);
                    printf("sent ACK for the DUPLICATE WELCOME's sequence=%u too (S9: "
                           "every copy, not just the first)\n", (unsigned)ack.sequence);
                }
            }
        }

        return byte_identical;
    }
}

/* S5 INPUT_STATE, using the session_id apad_session adopted from WELCOME.
 * Not reliable (S4/S9): apad_session_on_sent never arms a retransmit for
 * it, matching "INPUT_STATE MUST NOT be retransmitted" (S9). */
static void target_input_state_burst(apad_session *sess, apad_sock *client_sock,
                                      const apad_addr *target, int count) {
    int i;

    for (i = 0; i < count; i++) {
        uint8_t buf[APAD_MAX_DATAGRAM];
        uint8_t payload[APAD_LEN_INPUT_STATE];
        apad_input_state st;
        apad_header hdr;
        int n, m;

        build_input_state(&st, (uint16_t)i, apad_ticks_ms());
        (void)apad_encode_input_state(payload, sizeof payload, &st);

        if (apad_session_next_header(sess, (uint8_t)APAD_MSG_INPUT_STATE, &hdr) != APAD_OK) {
            printf("  [FAIL] apad_session_next_header(INPUT_STATE)\n");
            return;
        }
        n = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)sizeof payload, NULL, 0);
        if (n < 0) {
            printf("  [FAIL] apad_packet_build(INPUT_STATE) rc=%d\n", n);
            return;
        }
        m = apad_udp_send(client_sock, target, buf, (size_t)n);
        apad_session_on_sent(sess, &hdr, apad_ticks_ms());
        printf("sent INPUT_STATE session_id=%u seq=%u (%d bytes)\n",
               (unsigned)hdr.session_id, (unsigned)hdr.sequence, m);
    }
}

/* S6.6: the server now originates its own PING at 1Hz per active session
 * (new server-side behaviour this tool predates -- it used to only ever
 * SEND a PING and never answer one, so the server's rtt_ms for this client
 * stayed unknown). Set once this client has received a server-originated
 * PING and replied with a correct PONG; checked at the end of target_mode()
 * so a regression here shows up as a real failure, not a silent gap. Only
 * meaningful in target mode -- self_loopback_mode's two sockets never run a
 * real session so a server-originated PING can never arrive there. */
static int g_server_ping_answered = 0;

/* Answers a server-originated PING with a PONG echoing origin_ticks_ms
 * unchanged (S6.6: "echoed unchanged (PONG)"; correlation is by
 * origin_ticks_ms alone, so the PONG carries its OWN header sequence, never
 * the PING's -- apad_session_next_header, not a copy of ping_hdr). A no-op
 * for any other packet type, so every receive loop in target mode can call
 * this unconditionally on every parsed packet without first checking the
 * type itself. This is the client-side half of S6.6; the existing
 * target_ping() below is unchanged and still covers the other half (this
 * client PINGing the server). */
static void answer_if_server_ping(apad_session *sess, apad_sock *client_sock,
                                   const apad_addr *target, const apad_packet *pkt) {
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_PING];
    apad_ping ping, pong;
    apad_header pong_hdr;
    int n;

    if (pkt->header.type != (uint8_t)APAD_MSG_PING) {
        return;
    }

    memset(&ping, 0, sizeof ping);
    if (apad_decode_ping(pkt->payload, pkt->payload_len, &ping) < 0) {
        printf("  [FAIL] apad_decode_ping on server-originated PING\n");
        return;
    }

    /* S6.6: PING and PONG share one payload layout and one decoder; this
     * client does not act on ping.responder_ticks_ms (S6.6: "MUST NOT act
     * on it in a PING"). The PONG's own responder_ticks_ms is this client's
     * clock at reply time -- S6.6 gives that field no MUST-be-zero rule for
     * a PONG, unlike for a PING. */
    memset(&pong, 0, sizeof pong);
    pong.origin_ticks_ms = ping.origin_ticks_ms;   /* S6.6: echoed unchanged */
    pong.responder_ticks_ms = apad_ticks_ms();
    if (apad_encode_ping(payload, sizeof payload, &pong) != (int)APAD_LEN_PING) {
        printf("  [FAIL] apad_encode_ping (PONG reply)\n");
        return;
    }

    if (apad_session_next_header(sess, (uint8_t)APAD_MSG_PONG, &pong_hdr) != APAD_OK) {
        printf("  [FAIL] apad_session_next_header(PONG)\n");
        return;
    }
    n = apad_packet_build(buf, sizeof buf, &pong_hdr, payload, (uint16_t)sizeof payload, NULL, 0);
    if (n < 0) {
        printf("  [FAIL] apad_packet_build(PONG) rc=%d\n", n);
        return;
    }
    (void)apad_udp_send(client_sock, target, buf, (size_t)n);
    apad_session_on_sent(sess, &pong_hdr, apad_ticks_ms());
    printf("  received server-originated PING origin_ticks_ms=%u -- replied PONG "
           "seq=%u (S6.6)\n", (unsigned)ping.origin_ticks_ms, (unsigned)pong_hdr.sequence);
    g_server_ping_answered = 1;
}

/* S6.6 PING -> PONG. Correlation is by origin_ticks_ms ALONE -- the PONG
 * carries its own header sequence and must not echo the PING's. Returns 1
 * iff a PONG arrived and echoed origin_ticks_ms unchanged. */
static int target_ping(apad_session *sess, apad_sock *client_sock, const apad_addr *target) {
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_PING];
    apad_ping ping;
    apad_header ping_hdr;
    uint32_t ping_start;
    int n, m;
    int pong_ok = 0;

    memset(&ping, 0, sizeof ping);
    ping.origin_ticks_ms = apad_ticks_ms();
    ping.responder_ticks_ms = 0;
    (void)apad_encode_ping(payload, sizeof payload, &ping);

    if (apad_session_next_header(sess, (uint8_t)APAD_MSG_PING, &ping_hdr) != APAD_OK) {
        printf("  [FAIL] apad_session_next_header(PING)\n");
        return 0;
    }
    n = apad_packet_build(buf, sizeof buf, &ping_hdr, payload, (uint16_t)sizeof payload, NULL, 0);
    m = apad_udp_send(client_sock, target, buf, (size_t)n);
    apad_session_on_sent(sess, &ping_hdr, apad_ticks_ms());
    printf("sent PING seq=%u origin_ticks_ms=%u (%d bytes)\n",
           (unsigned)ping_hdr.sequence, (unsigned)ping.origin_ticks_ms, m);

    ping_start = apad_ticks_ms();
    for (;;) {
        uint8_t rbuf[APAD_MAX_DATAGRAM];
        apad_addr from;
        int rn;

        if (apad_time_since(apad_ticks_ms(), ping_start) > 2000u) {
            printf("  [FAIL] no PONG within 2000ms\n");
            return 0;
        }
        rn = apad_udp_recv(client_sock, &from, rbuf, sizeof rbuf, 200);
        if (rn <= 0) {
            continue;
        }
        {
            apad_packet pkt;
            memset(&pkt, 0, sizeof pkt);
            if (apad_packet_parse(rbuf, (size_t)rn, &pkt) < 0) {
                continue;
            }
            if (pkt.header.type != (uint8_t)APAD_MSG_PONG) {
                answer_if_server_ping(sess, client_sock, target, &pkt); /* S6.6, no-op unless PING */
                (void)apad_session_on_recv(sess, &pkt, apad_ticks_ms());
                continue; /* e.g. a duplicate WELCOME, or the server's own PING; keep
                           * waiting for OUR PONG */
            }
            {
                apad_ping pong;
                memset(&pong, 0, sizeof pong);
                if (apad_decode_ping(pkt.payload, pkt.payload_len, &pong) < 0) {
                    return 0;
                }
                {
                    int ticks_match = (pong.origin_ticks_ms == ping.origin_ticks_ms);
                    int seq_distinct = (pkt.header.sequence != ping_hdr.sequence);
                    check(ticks_match, "PONG echoes origin_ticks_ms unchanged (S6.6 correlation)");
                    check(seq_distinct, "PONG carries its own header sequence, "
                                        "does not echo PING's (S6.6, informational)");
                    pong_ok = ticks_match;
                }
                return pong_ok;
            }
        }
    }
}

/* Streams INPUT_STATE past S11's 3000ms idle timeout, then probes liveness
 * with a fresh PING/PONG. If the duplicate-HELLO handling above -- or
 * anything else -- silently killed the session, no PONG arrives here even
 * though INPUT_STATE was flowing the entire time: exactly the failure S9's
 * Duplicates rules exist to prevent ("a session that dies... with input
 * flowing normally and nothing in any log to explain it"). Returns 1 iff
 * the session is still alive. */
static int target_survive_past_idle_timeout(apad_session *sess, apad_sock *client_sock,
                                             const apad_addr *target) {
    uint32_t start = apad_ticks_ms();
    int iter = 1000; /* distinct range from the earlier INPUT_STATE burst, for
                       * readable logs */
    int sent = 0;

    printf("streaming INPUT_STATE for >3000ms (past the S11 idle timeout) to prove "
           "the session survived the duplicate HELLO/WELCOME above...\n");
    while (apad_time_since(apad_ticks_ms(), start) < 3200u) {
        uint8_t buf[APAD_MAX_DATAGRAM];
        uint8_t payload[APAD_LEN_INPUT_STATE];
        apad_input_state st;
        apad_header hdr;
        int n;

        build_input_state(&st, (uint16_t)iter, apad_ticks_ms());
        (void)apad_encode_input_state(payload, sizeof payload, &st);
        if (apad_session_next_header(sess, (uint8_t)APAD_MSG_INPUT_STATE, &hdr) == APAD_OK) {
            n = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)sizeof payload,
                                   NULL, 0);
            if (n >= 0) {
                (void)apad_udp_send(client_sock, target, buf, (size_t)n);
                apad_session_on_sent(sess, &hdr, apad_ticks_ms());
                sent++;
            }
        }
        iter++;

        /* This is the longest continuous stretch in the whole run
         * (deliberately >3000ms, S11), so it is the likeliest place for the
         * server's 1Hz S6.6 PING to land. A real client stays reactive to
         * inbound traffic across its whole session, not only while
         * explicitly waiting for one particular reply the way target_ping()
         * below does -- a non-blocking poll (timeout 0) here drains and
         * answers whatever is already queued without changing this loop's
         * pacing at all; the sleep_ms(50) below is unchanged. */
        {
            uint8_t rbuf[APAD_MAX_DATAGRAM];
            apad_addr from;
            int rn = apad_udp_recv(client_sock, &from, rbuf, sizeof rbuf, 0);
            if (rn > 0) {
                apad_packet pkt;
                memset(&pkt, 0, sizeof pkt);
                if (apad_packet_parse(rbuf, (size_t)rn, &pkt) >= 0) {
                    answer_if_server_ping(sess, client_sock, target, &pkt); /* S6.6 */
                    (void)apad_session_on_recv(sess, &pkt, apad_ticks_ms());
                }
            }
        }

        sleep_ms(50); /* ~50ms between packets: a realistic input rate, and
                        * enough packets to comfortably cross the 3000ms mark */
    }
    printf("streamed %d more INPUT_STATE datagrams over %ums\n", sent,
           (unsigned)apad_time_since(apad_ticks_ms(), start));

    printf("liveness probe: PING -> PONG after >3000ms of traffic...\n");
    {
        int alive = target_ping(sess, client_sock, target);
        check(alive, "session still alive past the S11 3000ms idle timeout "
                      "(PONG received to a fresh PING)");
        if (!alive) {
            printf("  NOTE: no PONG after >3000ms of INPUT_STATE traffic is exactly "
                   "the S9 Duplicates failure mode -- the session may have been "
                   "silently torn down. Check the server's own log for a "
                   "\"closed: reliable delivery failed\" line around this point "
                   "before concluding this harness is at fault.\n");
        }
        return alive;
    }
}

/* S6.5 BYE: best-effort clean close so the server frees the pad slot
 * immediately (S8/S11) instead of waiting out the 3s idle timeout. The
 * client is exiting either way, so this gives the S9 retransmit at most one
 * chance to fire rather than blocking for the whole schedule. */
static void target_bye(apad_session *sess, apad_sock *client_sock, const apad_addr *target) {
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_BYE];
    apad_bye bye;
    apad_header hdr;
    uint32_t bye_start;
    int n, m;

    memset(&bye, 0, sizeof bye);
    bye.reason = (uint8_t)APAD_BYE_NORMAL;
    (void)apad_encode_bye(payload, sizeof payload, &bye);

    if (apad_session_next_header(sess, (uint8_t)APAD_MSG_BYE, &hdr) != APAD_OK) {
        printf("  [FAIL] apad_session_next_header(BYE)\n");
        apad_session_close(sess, APAD_CLOSE_LOCAL);
        return;
    }
    n = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)sizeof payload, NULL, 0);
    m = apad_udp_send(client_sock, target, buf, (size_t)n);
    apad_session_on_sent(sess, &hdr, apad_ticks_ms());
    printf("sent BYE seq=%u reason=normal (%d bytes)\n", (unsigned)hdr.sequence, m);

    bye_start = apad_ticks_ms();
    for (;;) {
        uint8_t rbuf[APAD_MAX_DATAGRAM];
        apad_addr from;
        int rn, action;
        uint32_t now = apad_ticks_ms();

        if (apad_time_since(now, bye_start) > 350u) {
            break; /* best-effort only */
        }
        action = apad_session_tick(sess, now);
        if (action == APAD_ACT_RETRANSMIT) {
            (void)apad_udp_send(client_sock, target, buf, (size_t)n);
        } else if (action == APAD_ACT_TIMEOUT) {
            break;
        }
        rn = apad_udp_recv(client_sock, &from, rbuf, sizeof rbuf, 100);
        if (rn > 0) {
            apad_packet pkt;
            memset(&pkt, 0, sizeof pkt);
            if (apad_packet_parse(rbuf, (size_t)rn, &pkt) >= 0) {
                (void)apad_session_on_recv(sess, &pkt, apad_ticks_ms());
                if (pkt.header.type == (uint8_t)APAD_MSG_ACK) {
                    printf("  BYE acknowledged\n");
                    break;
                }
            }
        }
    }
    apad_session_close(sess, APAD_CLOSE_LOCAL);
}

static int target_mode(const char *ip_text, const char *port_text) {
    apad_sock *client_sock;
    apad_addr target;
    apad_session sess;
    target_hello_result hello_result;
    uint16_t port;
    int pairing_required = 0;
    int discover_ok, welcome_ok, pong_ok, duplicate_ok, survive_ok, server_ping_ok, success;

    port = (uint16_t)atoi(port_text);
    if (apad_addr_parse(&target, ip_text, port) != APAD_OK) {
        printf("could not parse target address '%s'\n", ip_text);
        return 1;
    }

    printf("== AtticPad loopback-client: target mode, %s:%u ==\n", ip_text, (unsigned)port);
    printf("(docs/DESIGN.md S8.1 integration flow: DISCOVER/ANNOUNCE, HELLO/WELCOME,\n"
           " duplicate HELLO/WELCOME (S9 Duplicates), INPUT_STATE, PING/PONG (both\n"
           " directions, S6.6), BYE -- exit code gates on WELCOME + duplicate-answer +\n"
           " post-idle-timeout PONG + answering a server-originated PING)\n\n");

    if (apad_net_init() != APAD_OK) {
        printf("apad_net_init failed\n");
        return 1;
    }
    client_sock = apad_udp_open(0);
    check(client_sock != NULL, "apad_udp_open (client)");
    if (client_sock == NULL) {
        return 1;
    }

    g_server_ping_answered = 0; /* S6.6: fresh per run, see answer_if_server_ping() */
    apad_session_init(&sess, 0 /* is_server */, apad_ticks_ms());

    printf("-- DISCOVER / ANNOUNCE (S7) --\n");
    discover_ok = target_discover(client_sock, &target, &pairing_required);
    check(discover_ok, "DISCOVER/ANNOUNCE (S7 tier 2 discovery)");
    if (pairing_required) {
        printf("NOTE: server reports pairing_required=1. This automated client has no\n"
               "PIN-entry path (S10 pairing is explicit and user-initiated) -- HELLO is\n"
               "still attempted, but the server may require an authenticated session this\n"
               "tool cannot provide.\n");
    }

    printf("\n-- HELLO / WELCOME (S6.3/S6.4/S9) --\n");
    memset(&hello_result, 0, sizeof hello_result);
    welcome_ok = target_hello(&sess, client_sock, &target, &hello_result);
    check(welcome_ok, "HELLO -> WELCOME (session established)");

    if (!welcome_ok) {
        apad_udp_close(client_sock);
        printf("\ntarget-mode result: FAIL (welcome_ok=0)\n");
        return 1;
    }
    printf("adopted session_id=%u pad_slot=%u\n",
           (unsigned)sess.session_id, (unsigned)sess.pad_slot);

    /* S9 "Duplicates": exercised here, right after the first HELLO/WELCOME/
     * ACK cycle completes, so a duplicate handled wrongly has the maximum
     * amount of the rest of this run (INPUT_STATE streaming, the liveness
     * PONG below, the original short burst, BYE) to reveal a session that
     * silently died. This is an ADDITIONAL phase; nothing below it that
     * existed before is removed or altered. */
    printf("\n-- duplicate HELLO / WELCOME (S9 \"Duplicates\") --\n");
    duplicate_ok = target_duplicate_hello_scenario(&sess, client_sock, &target, &hello_result);

    printf("\n-- survive past the S11 3000ms idle timeout, then liveness PONG --\n");
    survive_ok = target_survive_past_idle_timeout(&sess, client_sock, &target);

    printf("\n-- INPUT_STATE x3, session_id=%u --\n", (unsigned)sess.session_id);
    target_input_state_burst(&sess, client_sock, &target, 3);

    printf("\n-- PING / PONG (S6.6) --\n");
    pong_ok = target_ping(&sess, client_sock, &target);
    check(pong_ok, "PING -> PONG round trip");

    /* S6.6: checked last, after every phase above has had a chance to
     * receive and answer the server's 1Hz per-session PING -- see
     * answer_if_server_ping() and its call sites in target_hello(),
     * target_duplicate_hello_scenario(), target_survive_past_idle_timeout()
     * and target_ping() above. This tool used to only ever SEND a PING and
     * never answer one, so the server's rtt_ms for this client stayed
     * unknown; this is the assertion that keeps that regression covered. */
    server_ping_ok = g_server_ping_answered;
    check(server_ping_ok, "received and answered at least one server-originated "
                           "PING over the run (S6.6)");

    printf("\n-- BYE (S6.5), best-effort --\n");
    target_bye(&sess, client_sock, &target);

    apad_udp_close(client_sock);

    success = welcome_ok && pong_ok && duplicate_ok && survive_ok && server_ping_ok;
    printf("\ntarget-mode result: %s (welcome_ok=%d duplicate_ok=%d survive_ok=%d "
           "pong_ok=%d server_ping_ok=%d)\n",
           success ? "PASS" : "FAIL", welcome_ok, duplicate_ok, survive_ok, pong_ok,
           server_ping_ok);
    if (!success && welcome_ok && (!duplicate_ok || !survive_ok)) {
        printf("This failure is in the S9 Duplicates path, not the original happy path\n"
               "(which passed: welcome_ok=1). If server-dev's fix for the duplicate-HELLO\n"
               "session-teardown bug is not yet deployed on the server under test, this is\n"
               "a correct detection of a KNOWN open bug, not a harness fault -- check the\n"
               "server's own log for \"closed: reliable delivery failed\" before assuming\n"
               "otherwise.\n");
    }
    return success ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "--target") == 0) {
        return target_mode(argv[2], argv[3]);
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--target <ip> <port>]\n", argv[0]);
        return 2;
    }
    return self_loopback_mode();
}
