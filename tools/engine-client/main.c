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

#include "atticpad/atticpad.h"
#include "apad_client.h"

static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Same idea as loopback-client's build_input_state: distinctive, varying
 * values so a human watching evtest (or the server log) can tell frames
 * apart — but built as the ENGINE'S CALLER would build it, and left to the
 * engine to encode. */
static void fill_input(apad_input_state *st, unsigned iter) {
    memset(st, 0, sizeof *st);
    st->buttons = (iter % 2u == 0u) ? (APAD_BTN_A | APAD_BTN_DPAD_RIGHT)
                                    : APAD_BTN_B;
    /* Computed in int32 and subtracted in range: S9 warns off the
     * unsigned-wrap-then-narrow construct (C99 6.3.1.3p3). */
    st->axes[0] = (int16_t)((int32_t)((iter * 997u) % 65536u) - 32768); /* LX sweep */
    st->axes[1] = (int16_t)(iter % 2u == 0u ? 12000 : -12000);
    st->battery = 255; /* unknown, S5.5 */
    st->client_ticks_ms = apad_ticks_ms();
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

    if (argc < 4 || strcmp(argv[1], "--target") != 0) {
        fprintf(stderr, "usage: %s --target <ip> <port> [iters [--secret <pin>]]\n", argv[0]);
        return 2;
    }
    ip = argv[2];
    port = (uint16_t)atoi(argv[3]);
    if (argc >= 5) {
        iters = (unsigned)atoi(argv[4]);
    }
    if (argc >= 7 && strcmp(argv[5], "--secret") == 0) {
        secret = argv[6];
    }

    if (apad_net_init() != APAD_OK) {
        fprintf(stderr, "apad_net_init failed\n");
        return 1;
    }

    c = apad_client_create("AtticPad 3DS engine-client",
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
