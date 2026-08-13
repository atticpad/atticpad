/* apad_client.c — the AtticPad client engine. See apad_client.h.
 *
 * Every wire byte in this file goes through libapad's apad_encode_* /
 * apad_decode_* / apad_packet_* and every time comparison goes through
 * apad_time_after() / apad_time_since(). There is no arithmetic on a
 * sequence number and no struct is ever cast onto a packet buffer.
 *
 * The handshake and the §9 duplicate-ACK behaviour are transcribed from
 * clients/3ds/source/main.c, which is the reference client and the one that
 * has actually been proven against the server on hardware.
 */

#include <stdlib.h>
#include <string.h>

#include "apad_client.h"

#define APAD_PING_INTERVAL_MS 1000u

struct apad_client {
    apad_sock   *sock;
    apad_addr    target;
    apad_session sess;

    uint32_t caps;
    char     device_name[APAD_NAME_LEN];
    uint8_t  client_id[APAD_CLIENT_ID_LEN];

    /* §10 signed liveness (2026-08-11): the exact datagram of the WELCOME
     * this session accepted. §9 requires a retransmitted WELCOME to be
     * byte-identical, so on an ACTIVE session this is the only untagged
     * WELCOME that may draw a re-ACK; any other is a forgery and is ignored
     * before it can reach apad_session_on_recv() (which adopts session
     * parameters unconditionally — the hole the guardian found). */
    uint8_t  welcome_buf[APAD_MAX_DATAGRAM];
    uint16_t welcome_len;

    /* §9: the HELLO datagram is retransmitted BYTE-IDENTICALLY, so the bytes
     * are kept rather than re-encoded. Re-encoding would allocate a fresh
     * sequence number and silently break the retransmit contract. */
    uint8_t  hello_buf[APAD_MAX_DATAGRAM];
    int      hello_len;

    uint32_t input_interval_ms;
    uint32_t last_input_ms;
    uint32_t last_ping_ms;
    uint32_t ping_origin_ms;
    int      awaiting_pong;
    int32_t  rtt_ms;

    uint32_t tx_packets;
    uint32_t rx_packets;
    int32_t  last_error;

    uint32_t rumble_serial;
    int32_t  rumble_low, rumble_high, rumble_duration_ms;
    uint32_t led_serial;
    int32_t  led_player, led_rgb;
    uint32_t status_serial;
    int32_t  status_code;
    char     message[APAD_TEXT_LEN + 1];

    /* §10. `secret` is the only key material this struct holds that outlives
     * a session; it is wiped in destroy(). The derived key lives in
     * c->sess.key, where apad_session_close() already wipes it. */
    char     secret[APAD_CLIENT_SECRET_MAX + 1];
    int32_t  pairing_required;   /* §6.2, -1 until an ANNOUNCE is seen       */
    int32_t  auth_required;      /* §6.4 flags bit 0                         */
    int32_t  auth_state;         /* enum apad_client_auth                    */
    int32_t  error_code;         /* §6.11                                    */
};

/* ---- helpers ----------------------------------------------------------- */

static void note_error(apad_client *c, int rc)
{
    if (rc < 0) {
        c->last_error = rc;
    }
}

/*
 * §10 — the key this session's outgoing datagrams are tagged with, or NULL
 * before one is installed.
 *
 * Every apad_packet_build() call in this file passes this and not a bare
 * NULL, and that is load-bearing: codec.c sets the AUTHENTICATED flag from
 * whether a key was supplied and CLEARS it otherwise, so a NULL here would
 * silently strip the flag apad_session_next_header() had just set and put an
 * untagged INPUT_STATE on a session the server is verifying — which the
 * server drops without an ERROR (it is the amplifier §8 forbids), so the pad
 * would simply never move and nothing anywhere would say why.
 */
static const uint8_t *session_key(const apad_client *c, size_t *key_len)
{
    if (!c->sess.have_key) {
        *key_len = 0u;
        return NULL;
    }
    *key_len = (size_t)APAD_SESSION_KEY_LEN;
    return c->sess.key;
}

/*
 * A session that was told AUTH_REQUIRED and never got a single inbound tag to
 * verify did not authenticate, however it ended. Called on every path out of
 * a session so "wrong PIN" does not depend on an ERROR datagram arriving —
 * §10 lets the server answer a bad tag with ERROR code 3, but §8's rate limit
 * explicitly takes precedence over that MUST, and the session dying at the
 * 3000 ms idle timeout is a perfectly ordinary way to be told the PIN was
 * wrong.
 */
static void settle_auth(apad_client *c)
{
    if (!c->auth_required) {
        return;                        /* no pairing was ever involved */
    }
    if (c->auth_state == (int32_t)APAD_AUTH_VERIFIED) {
        return;                        /* a tag verified; the secret was right */
    }
    if (c->auth_state == (int32_t)APAD_AUTH_NEED_SECRET) {
        return;                        /* never held a key: nothing failed */
    }
    c->auth_state = (int32_t)APAD_AUTH_FAILED;
}

/* Build and send one datagram of `type`. Returns the length sent, or
 * negative. `out_hdr` receives the header actually used, because
 * apad_session_on_sent() needs it to arm the §9 retransmit timer. */
static int send_msg(apad_client *c, uint8_t type,
                    const void *payload, uint16_t payload_len,
                    apad_header *out_hdr)
{
    uint8_t buf[APAD_MAX_DATAGRAM];
    apad_header hdr;
    size_t klen;
    const uint8_t *key;
    int n;

    if (apad_session_next_header(&c->sess, type, &hdr) != APAD_OK) {
        return APAD_ERR_STATE;
    }
    key = session_key(c, &klen);
    n = apad_packet_build(buf, sizeof buf, &hdr, payload, payload_len, key, klen);
    if (n < 0) {
        note_error(c, n);
        return n;
    }
    (void)apad_udp_send(c->sock, &c->target, buf, (size_t)n);
    apad_session_on_sent(&c->sess, &hdr, apad_ticks_ms());
    c->tx_packets++;
    if (out_hdr != NULL) {
        *out_hdr = hdr;
    }
    return n;
}

/*
 * §9 "Duplicates": "A receiver MUST ACK every copy of a reliable message it
 * receives, including duplicates of one it has already processed." Skipping
 * this is the failure the loopback client and the 3DS client each hit once:
 * INPUT_STATE flows perfectly and the session dies at t≈2300 ms with nothing
 * in any log, because the server exhausted its retransmits of a WELCOME the
 * client had silently accepted.
 */
static void ack_if_reliable(apad_client *c, const apad_packet *pkt)
{
    uint8_t payload[APAD_LEN_ACK];
    apad_ack ack;

    if ((pkt->header.flags & APAD_FLAG_RELIABLE) == 0u) {
        return;
    }
    memset(&ack, 0, sizeof ack);
    ack.sequence = pkt->header.sequence;
    if (apad_encode_ack(payload, sizeof payload, &ack) != (int)APAD_LEN_ACK) {
        return;
    }
    (void)send_msg(c, (uint8_t)APAD_MSG_ACK, payload, (uint16_t)sizeof payload, NULL);
}

static void handle_packet(apad_client *c, const apad_packet *pkt)
{
    c->rx_packets++;

    switch (pkt->header.type) {
    /*
     * §10 — THE ORDER HERE IS THE WHOLE POINT, and it is an order between
     * two call sites rather than two lines: drain_rx() calls
     * ack_if_reliable() BEFORE it calls this function, so by the time
     * apad_derive_session_key() below starts its 10,000 iterations the ACK
     * that discharges this WELCOME has already left the socket.
     *
     * §10 grants exactly one exemption — "except the ACK that discharges
     * WELCOME itself, which MAY be unauthenticated" — and it exists for this
     * ordering. WELCOME is the datagram carrying the server_nonce used as the
     * PBKDF2 salt, so no client can hold a key until it has parsed the very
     * message its ACK acknowledges. Deriving first would put PBKDF2 inside
     * §9's 100/200/400/800 ms retransmit schedule: 17 ms on x86 at -O2, 86 ms
     * at -O0, tens of ms on this phone — invisible here, and order-of-seconds
     * on the 67 MHz ARM9 the 3DS and DS clients will copy this pattern onto,
     * where it kills the session at t=2300 ms as APAD_CLOSE_RETX_FAILED with
     * input never having flowed and nothing in any log to explain it.
     *
     * DO NOT "tidy" this by deriving before acknowledging.
     */
    case APAD_MSG_WELCOME: {
        apad_welcome w;
        uint8_t key[APAD_SESSION_KEY_LEN];

        memset(&w, 0, sizeof w);
        if (apad_decode_welcome(pkt->payload, pkt->payload_len, &w) < 0) {
            break;
        }
        if ((w.flags & APAD_WELCOME_AUTH_REQUIRED) == 0u) {
            break;                     /* no pairing window: nothing to do */
        }
        c->auth_required = 1;
        if (c->sess.have_key) {
            /* A §9 retransmit of a WELCOME already keyed. Its ACK went out
             * above (and is tagged this time, which the server verifies);
             * re-deriving would burn PBKDF2 again for the identical key. */
            break;
        }
        if (c->secret[0] == '\0') {
            /* §8: no secret, so no key — and NOT a stall. The ACK is already
             * sent; apad_client_connect() lets the session lapse from here. */
            c->auth_state = (int32_t)APAD_AUTH_NEED_SECRET;
            break;
        }
        apad_derive_session_key(c->secret, w.server_nonce, key);
        apad_session_set_key(&c->sess, key);
        apad_secure_zero(key, sizeof key);
        c->auth_state = (int32_t)APAD_AUTH_KEYED;
        break;
    }
    case APAD_MSG_ANNOUNCE: {
        apad_announce a;
        memset(&a, 0, sizeof a);
        if (apad_decode_announce(pkt->payload, pkt->payload_len, &a) >= 0) {
            /* §6.2: any non-zero value MUST be read as 1; apad_decode_announce
             * has already normalised it. This is the §8 signal that a secret
             * must be obtained BEFORE the handshake. */
            c->pairing_required = (int32_t)(a.pairing_required ? 1 : 0);
        }
        break;
    }
    case APAD_MSG_PONG: {
        apad_ping pong;
        memset(&pong, 0, sizeof pong);
        if (apad_decode_ping(pkt->payload, pkt->payload_len, &pong) >= 0
            && c->awaiting_pong
            && pong.origin_ticks_ms == c->ping_origin_ms) {
            c->rtt_ms = (int32_t)apad_time_since(apad_ticks_ms(), c->ping_origin_ms);
            c->awaiting_pong = 0;
        }
        break;
    }
    /*
     * §6.6 — PING is "either" direction: the server originates one at 1Hz per
     * active session to measure ITS rtt_ms for this client, the mirror of the
     * PING this client sends every APAD_PING_INTERVAL_MS to measure its own.
     * Answering with a PONG is not optional; a client that only ever sends
     * PING and never answers one looks perfectly healthy from its own side
     * (its HUD shows a correct RTT) while the server's rtt_ms for it stays
     * APAD_RTT_UNKNOWN forever. Transcribed from
     * tools/loopback-client/main.c's answer_if_server_ping(), the reference
     * implementation for this exact fix.
     */
    case APAD_MSG_PING: {
        apad_ping ping, pong;
        uint8_t payload[APAD_LEN_PING];

        memset(&ping, 0, sizeof ping);
        if (apad_decode_ping(pkt->payload, pkt->payload_len, &ping) < 0) {
            break;
        }
        /* §6.6: this client does not act on ping.responder_ticks_ms ("MUST
         * NOT act on it in a PING"). */
        memset(&pong, 0, sizeof pong);
        pong.origin_ticks_ms = ping.origin_ticks_ms;      /* echoed unchanged */
        pong.responder_ticks_ms = apad_ticks_ms();         /* no MUST-be-zero
                                                              * rule for a PONG */
        if (apad_encode_ping(payload, sizeof payload, &pong) == (int)APAD_LEN_PING) {
            /* send_msg(), not a hand-rolled build+send: it is the same path
             * this client's own PING/INPUT_STATE use, so a PONG on an
             * authenticated session is tagged exactly like any other
             * outbound packet, and it carries its OWN header sequence (§6.6:
             * "MUST NOT echo the PING's") via apad_session_next_header. */
            (void)send_msg(c, (uint8_t)APAD_MSG_PONG, payload,
                           (uint16_t)sizeof payload, NULL);
        }
        break;
    }
    case APAD_MSG_RUMBLE: {
        apad_rumble r;
        memset(&r, 0, sizeof r);
        if (apad_decode_rumble(pkt->payload, pkt->payload_len, &r) >= 0) {
            c->rumble_low         = (int32_t)r.low_freq;
            c->rumble_high        = (int32_t)r.high_freq;
            c->rumble_duration_ms = (int32_t)r.duration_ms;
            c->rumble_serial++;
        }
        break;
    }
    case APAD_MSG_LED: {
        apad_led l;
        memset(&l, 0, sizeof l);
        if (apad_decode_led(pkt->payload, pkt->payload_len, &l) >= 0) {
            c->led_player = (int32_t)l.player_index;
            c->led_rgb    = ((int32_t)l.r << 16) | ((int32_t)l.g << 8) | (int32_t)l.b;
            c->led_serial++;
        }
        break;
    }
    case APAD_MSG_STATUS: {
        apad_status s;
        memset(&s, 0, sizeof s);
        if (apad_decode_status(pkt->payload, pkt->payload_len, &s) >= 0) {
            c->status_code = (int32_t)s.code;
            apad_text_get(c->message, sizeof c->message, s.text, APAD_TEXT_LEN);
            c->status_serial++;
        }
        break;
    }
    case APAD_MSG_ERROR: {
        apad_error e;
        memset(&e, 0, sizeof e);
        if (apad_decode_error(pkt->payload, pkt->payload_len, &e) >= 0) {
            /* §6.0: ERROR.code is a diagnostic label and survives decode
             * verbatim even if this build has never heard of it. Recording
             * it as-is is the whole point. */
            c->status_code = (int32_t)e.code;
            c->error_code  = (int32_t)e.code;
            apad_text_get(c->message, sizeof c->message, e.text, APAD_TEXT_LEN);
            c->status_serial++;
            if (e.code == (uint16_t)APAD_ERRC_AUTH_FAILED) {
                c->auth_state = (int32_t)APAD_AUTH_FAILED;
            }
            /* §8: the server answers any stale non-zero session_id with
             * ERROR 7 — it has already reaped the slot. §8 ALSO says any
             * §3.1-valid datagram refreshes the idle timer, INCLUDING that
             * ERROR, so a client that merely records the code livelocks:
             * ACTIVE forever, INPUT_STATE discarded silently server-side,
             * each PING drawing another rate-limited ERROR. Found on 3DS
             * hardware the first time the self-test screen (which stalls
             * the pump past the server's 3 s timeout) ran mid-session.
             * Now normative: S8 (2026-08-11 addition) makes this teardown a
             * client MUST, ruled by the spec owner after the incident.
             * Guarded on ACTIVE per the same rule: during the handshake our
             * datagrams carry session_id 0 (§8), so a code-7 ERROR then
             * could only be a stale answer meant for a previous life. */
            if (e.code == (uint16_t)APAD_ERRC_UNKNOWN_SESSION &&
                c->sess.state == (uint8_t)APAD_SESSION_ACTIVE) {
                apad_session_close(&c->sess, APAD_CLOSE_PEER_ERROR);
            }
        }
        break;
    }
    default:
        /* BYE is handled by apad_session_on_recv(). */
        break;
    }
}

/*
 * §10 — an ERROR that arrived UNTAGGED on a session that requires tags.
 *
 * This is display only and it closes nothing. It exists because the server's
 * answer to a bad tag is built with no session at all (session_id 0, sequence
 * 0, no key — it has just torn the session down), so the one datagram that
 * says "wrong PIN" is by construction the one datagram that cannot be
 * verified. Refusing to look at it would leave the UI saying "disconnected"
 * for three seconds and then guessing.
 *
 * §10's "MUST NOT be acted on otherwise" is respected: nothing here touches
 * the FSM, the retransmit timer, the receive window or the key. The worst an
 * off-path forger achieves is a wrong sentence on a screen, and only on a
 * session that was already going to fail — settle_auth() reaches the same
 * conclusion on its own when the session lapses.
 */
static void note_unverified_error(apad_client *c, const apad_packet *pkt)
{
    apad_error e;

    memset(&e, 0, sizeof e);
    if (apad_decode_error(pkt->payload, pkt->payload_len, &e) < 0) {
        return;
    }
    c->error_code = (int32_t)e.code;
    apad_text_get(c->message, sizeof c->message, e.text, APAD_TEXT_LEN);
    c->status_serial++;
    if (e.code == (uint16_t)APAD_ERRC_AUTH_FAILED
        && c->auth_state == (int32_t)APAD_AUTH_KEYED) {
        c->auth_state = (int32_t)APAD_AUTH_FAILED;
    }
}

/* (untagged_is_inert() was deleted with the 2026-08-11 signed-liveness
 * ruling: untagged datagrams on an AUTH_REQUIRED session now refresh
 * nothing, so there is no longer an inert-but-alive category.) */

/* Drain everything readable, waiting at most `wait_ms` for the FIRST
 * datagram. Returns the number handled. */
static int drain_rx(apad_client *c, int wait_ms)
{
    int handled = 0;
    int timeout = wait_ms;

    for (;;) {
        uint8_t rbuf[APAD_MAX_DATAGRAM];
        apad_addr from;
        apad_packet pkt;
        int rn;

        rn = apad_udp_recv(c->sock, &from, rbuf, sizeof rbuf, timeout);
        if (rn <= 0) {
            break;                     /* 0 = timed out, negative = error */
        }
        timeout = 0;                   /* subsequent reads must not block */

        /* §1: a session is keyed on (source IP, source port, session_id).
         * Anything from another address is somebody else's traffic or a
         * forgery; either way it is not this session's. */
        if (!apad_addr_equal(&from, &c->target)) {
            continue;
        }
        memset(&pkt, 0, sizeof pkt);
        if (apad_packet_parse(rbuf, (size_t)rn, &pkt) < 0) {
            continue;                  /* malformed: discard, keep draining */
        }

        if (pkt.header.type == (uint8_t)APAD_MSG_WELCOME) {
            if (c->sess.state == (uint8_t)APAD_SESSION_ACTIVE
                && c->auth_required) {
                /* The gate below is §10's, so it applies only where §10
                 * reaches: AUTH_REQUIRED sessions. On an unpaired session an
                 * ACTIVE-state WELCOME falls through to the normal path —
                 * §8's unqualified refresh, §9's ACK-every-copy, and a
                 * handle_packet that no-ops on a WELCOME with the flag
                 * clear — exactly the pre-ruling behavior the spec still
                 * mandates there (guardian pass 3, finding B). */
                /* §10 signed liveness: on an ACTIVE session the only
                 * legitimate WELCOME is a §9 retransmission, and §9 makes it
                 * byte-identical. Matching copy: re-ACK (the §9 duty) and
                 * nothing else — no liveness refresh, no re-processing (the
                 * DS would re-run a one-second PBKDF2 for the same key), no
                 * apad_session_on_recv (it would let a forgery rewrite
                 * session_id/rate). Non-matching: a forgery, ignored. */
                if ((size_t)rn == (size_t)c->welcome_len
                    && memcmp(rbuf, c->welcome_buf, c->welcome_len) == 0) {
                    ack_if_reliable(c, &pkt);
                }
                continue;
            }
            /* HANDSHAKING: this is (about to be) the accepted WELCOME —
             * capture the bytes the ACTIVE-state comparison above needs. A
             * §9 duplicate during the handshake overwrites with identical
             * bytes; a different WELCOME here loses the race it was always
             * going to lose (first one wins the state machine). */
            c->welcome_len = (uint16_t)rn;
            memcpy(c->welcome_buf, rbuf, (size_t)rn);
        }

        /*
         * §3.1 check 7 — "if AUTHENTICATED, the tag verifies (§10)" — and its
         * mirror, which §3.1 does not state because it is §10's rule and not
         * a framing rule: once this session has been told AUTH_REQUIRED, the
         * ABSENCE of a tag is as wrong as a bad one. Verifying tags when they
         * happen to be present while still acting on untagged datagrams is
         * not authentication at all; an attacker would simply clear the bit.
         */
        if ((pkt.header.flags & APAD_FLAG_AUTHENTICATED) != 0u) {
            if (!c->sess.have_key
                || apad_packet_verify(rbuf, (size_t)rn, c->sess.key,
                                      (size_t)APAD_SESSION_KEY_LEN) != APAD_OK) {
                note_error(c, APAD_ERR_AUTH);
                continue;              /* §3.1: reject. A client sends no
                                        * ERROR — it would answer a datagram
                                        * anyone can forge. */
            }
            /* A tag built with our key verified, so the far end derived the
             * same key from the same secret. This is the only positive proof
             * the PIN was right that exists anywhere in the protocol. */
            if (c->auth_state == (int32_t)APAD_AUTH_KEYED) {
                c->auth_state = (int32_t)APAD_AUTH_VERIFIED;
            }
        } else if (c->auth_required
                   && pkt.header.type != (uint8_t)APAD_MSG_WELCOME) {
            /* §10 signed liveness (ruled 2026-08-11): an untagged datagram on
             * an AUTH_REQUIRED session refreshes NOTHING. It used to call
             * apad_session_on_recv() here for inert types, which let the
             * server's untagged code-7 ERRORs — spoofable by construction —
             * keep a dead paired session ACTIVE forever (the guardian traced
             * the livelock surviving on exactly this branch after the
             * unpaired path was fixed). Now the only liveness an
             * authenticated session accepts is a datagram whose tag
             * verifies; a forgotten session reaches §8's 3 s idle teardown
             * on its own. Display still works: the ERROR text is recorded,
             * acted on never. */
            if (pkt.header.type == (uint8_t)APAD_MSG_ERROR) {
                note_unverified_error(c, &pkt);
            }
            continue;
        }

        (void)apad_session_on_recv(&c->sess, &pkt, apad_ticks_ms());
        /* ACK FIRST. handle_packet() is where a WELCOME's key gets derived,
         * and §10's exemption for this one untagged ACK exists precisely so
         * that PBKDF2 runs after it rather than inside §9's retransmit
         * schedule. See the WELCOME case in handle_packet(). */
        ack_if_reliable(c, &pkt);
        handle_packet(c, &pkt);
        handled++;
    }
    return handled;
}

/* ---- lifecycle --------------------------------------------------------- */

apad_client *apad_client_create(const char *device_name, uint32_t caps)
{
    apad_client *c;
    uint32_t seed;
    size_t i;

    if (apad_net_init() != APAD_OK) {
        return NULL;
    }
    c = (apad_client *)calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    c->sock = apad_udp_open(0);        /* 0: let the OS choose the source port */
    if (c->sock == NULL) {
        free(c);
        return NULL;
    }

    c->caps = caps & APAD_CAP_VALID_MASK;
    apad_text_set(c->device_name, sizeof c->device_name,
                  (device_name != NULL) ? device_name : "AtticPad");

    /* §6.3 client_id: stable enough to identify this session, and not a
     * secret — the PIN is the secret and it never goes on the wire (§10).
     * Seeded from the monotonic clock because the shim exposes no RNG and
     * inventing one in client code would be worse than this. */
    seed = apad_ticks_ms();
    for (i = 0; i < sizeof c->client_id; i++) {
        seed = seed * 1664525u + 1013904223u;
        c->client_id[i] = (uint8_t)(seed >> 24);
    }

    c->rtt_ms = -1;
    c->pairing_required = -1;          /* §6.2: unknown until an ANNOUNCE */
    c->auth_state = (int32_t)APAD_AUTH_NONE;
    c->input_interval_ms = 1000u / APAD_DEFAULT_RATE_HZ;
    apad_session_init(&c->sess, 0 /* is_server */, apad_ticks_ms());
    return c;
}

int apad_client_set_secret(apad_client *c, const char *secret)
{
    size_t n = 0u;

    if (c == NULL) {
        return APAD_ERR_ARG;
    }
    /* Wipe before overwriting, and on the way out of every early return: a
     * rejected secret must not leave the previous one live, because the user
     * who just typed a new one believes the old one is gone. */
    apad_secure_zero(c->secret, sizeof c->secret);
    if (secret == NULL) {
        return APAD_OK;
    }
    while (secret[n] != '\0') {
        if (n >= (size_t)APAD_CLIENT_SECRET_MAX) {
            return APAD_ERR_ARG;       /* §10.1 ceiling; never truncate */
        }
        n++;
    }
    /* §10.1: "an opaque byte string ... MUST NOT assume six digits." No
     * digit check, no length check beyond the ceiling — a QR token is coming
     * and it is neither six long nor decimal. */
    memcpy(c->secret, secret, n);
    c->secret[n] = '\0';
    return APAD_OK;
}

int apad_client_probe(apad_client *c, const char *ip, uint16_t port,
                      int timeout_ms)
{
    uint8_t buf[APAD_MAX_DATAGRAM];
    apad_header hdr;
    uint32_t start;
    int rc, n;

    if (c == NULL || ip == NULL) {
        return APAD_ERR_ARG;
    }
    rc = apad_addr_parse(&c->target, ip,
                         (port != 0u) ? port : (uint16_t)APAD_DEFAULT_PORT);
    if (rc != APAD_OK) {
        note_error(c, rc);
        return rc;
    }

    apad_session_init(&c->sess, 0, apad_ticks_ms());
    c->pairing_required = -1;

    if (apad_session_next_header(&c->sess, (uint8_t)APAD_MSG_DISCOVER, &hdr)
        != APAD_OK) {
        return APAD_ERR_STATE;
    }
    /* §6.1: 0 bytes, session_id 0. Not reliable (§4), so there is no
     * retransmit to drive and no ACK to wait for — one datagram, one wait. */
    n = apad_packet_build(buf, sizeof buf, &hdr, NULL, 0u, NULL, 0);
    if (n < 0) {
        note_error(c, n);
        return n;
    }
    (void)apad_udp_send(c->sock, &c->target, buf, (size_t)n);
    apad_session_on_sent(&c->sess, &hdr, apad_ticks_ms());
    c->tx_packets++;

    start = apad_ticks_ms();
    for (;;) {
        (void)drain_rx(c, 25);
        if (c->pairing_required >= 0) {
            return APAD_OK;
        }
        if (apad_time_since(apad_ticks_ms(), start) > (uint32_t)timeout_ms) {
            /* §7 tier 3 has no ANNOUNCE at all. Not an error the user should
             * ever be shown — the WELCOME will say whether pairing is on. */
            return APAD_ERR_STATE;
        }
    }
}

int apad_client_connect(apad_client *c, const char *ip, uint16_t port,
                        uint16_t desired_rate_hz, int timeout_ms)
{
    uint8_t payload[APAD_LEN_HELLO];
    apad_hello hello;
    apad_header hdr;
    uint32_t start;
    int rc;

    if (c == NULL || ip == NULL) {
        return APAD_ERR_ARG;
    }
    rc = apad_addr_parse(&c->target, ip, (port != 0u) ? port : (uint16_t)APAD_DEFAULT_PORT);
    if (rc != APAD_OK) {
        note_error(c, rc);
        return rc;
    }

    /* A fresh session per connect: a previous CLOSED one would reject the
     * HELLO on state grounds. */
    apad_session_init(&c->sess, 0, apad_ticks_ms());
    c->rtt_ms = -1;
    c->awaiting_pong = 0;
    c->tx_packets = 0;
    c->rx_packets = 0;
    c->message[0] = '\0';
    /* §10 state is per-session, not per-client: the nonce is generated per
     * session (two clients pairing off one PIN must not share a key), so a
     * reconnect derives a NEW key from the SAME secret. apad_session_init
     * has already wiped the old one. The secret itself deliberately
     * survives — §8's degradation path is "prompt at leisure, reconnect". */
    c->auth_required = 0;
    c->auth_state    = (int32_t)APAD_AUTH_NONE;
    c->error_code    = 0;

    memset(&hello, 0, sizeof hello);
    memcpy(hello.client_id, c->client_id, sizeof hello.client_id);
    hello.caps = c->caps;
    memcpy(hello.device_name, c->device_name, sizeof hello.device_name);
    /* client_nonce is unused until pairing.c exists on the server side (§10);
     * it is not left zero because §6.3 gives it no "absent" encoding and a
     * zero nonce would read as a real one. */
    memset(hello.client_nonce, 0xA5, sizeof hello.client_nonce);
    hello.desired_rate_hz = (desired_rate_hz != 0u) ? desired_rate_hz
                                                    : (uint16_t)APAD_DEFAULT_RATE_HZ;
    hello.proto_major     = (uint8_t)APAD_VERSION;
    hello.client_ticks_ms = apad_ticks_ms();

    if (apad_encode_hello(payload, sizeof payload, &hello) != (int)APAD_LEN_HELLO) {
        return APAD_ERR_BUFFER;
    }
    if (apad_session_next_header(&c->sess, (uint8_t)APAD_MSG_HELLO, &hdr) != APAD_OK) {
        return APAD_ERR_STATE;
    }
    c->hello_len = apad_packet_build(c->hello_buf, sizeof c->hello_buf, &hdr,
                                     payload, (uint16_t)sizeof payload, NULL, 0);
    if (c->hello_len < 0) {
        note_error(c, c->hello_len);
        return c->hello_len;
    }

    (void)apad_udp_send(c->sock, &c->target, c->hello_buf, (size_t)c->hello_len);
    apad_session_on_sent(&c->sess, &hdr, apad_ticks_ms());
    c->tx_packets++;

    start = apad_ticks_ms();
    for (;;) {
        int action = apad_session_tick(&c->sess, apad_ticks_ms());

        if (action == APAD_ACT_RETRANSMIT) {
            /* Byte-identical, same sequence — §9. */
            (void)apad_udp_send(c->sock, &c->target, c->hello_buf, (size_t)c->hello_len);
        } else if (action == APAD_ACT_TIMEOUT) {
            settle_auth(c);
            return APAD_ERR_STATE;
        }

        (void)drain_rx(c, 50);

        if (c->sess.state == APAD_SESSION_ACTIVE) {
            /* WELCOME arrived; apad_session_on_recv adopted session_id, slot
             * and rate, and drain_rx already sent the §9 ACK for it. */
            uint16_t rate = c->sess.input_rate_hz;

            if (c->auth_state == (int32_t)APAD_AUTH_NEED_SECRET) {
                /* §8: "A client that reaches WELCOME with AUTH_REQUIRED set
                 * and has no secret MUST NOT stall inside the handshake
                 * waiting for the user. It sends the ACK §9 requires, lets
                 * the session lapse, prompts at leisure, and reconnects with
                 * a fresh HELLO. The cost is one pad slot held for three
                 * seconds."
                 *
                 * The ACK is already gone. Closing locally is how the session
                 * lapses: no BYE, because a BYE on an auth-required session
                 * would need a tag this client cannot produce and the server
                 * would drop it silently, and because §8 describes lapsing,
                 * not a goodbye. The server reaps the slot at 3000 ms. */
                apad_session_close(&c->sess, APAD_CLOSE_LOCAL);
                return APAD_ERR_AUTH;
            }
            if (rate == 0u) {
                rate = (uint16_t)APAD_DEFAULT_RATE_HZ;
            }
            if (rate > (uint16_t)APAD_MAX_RATE_HZ) {
                rate = (uint16_t)APAD_MAX_RATE_HZ;
            }
            c->input_interval_ms = 1000u / rate;
            if (c->input_interval_ms == 0u) {
                c->input_interval_ms = 1u;
            }
            c->last_input_ms = apad_ticks_ms();
            c->last_ping_ms  = apad_ticks_ms();
            return APAD_OK;
        }
        if (c->sess.state == APAD_SESSION_CLOSED) {
            settle_auth(c);
            return (c->auth_state == (int32_t)APAD_AUTH_FAILED) ? APAD_ERR_AUTH
                                                                : APAD_ERR_STATE;
        }
        if (apad_time_since(apad_ticks_ms(), start) > (uint32_t)timeout_ms) {
            settle_auth(c);
            return APAD_ERR_STATE;
        }
    }
}

int apad_client_pump(apad_client *c, const apad_input_state *in, int max_wait_ms)
{
    uint32_t now;
    uint32_t elapsed;
    int wait;
    int action;

    if (c == NULL) {
        return APAD_CLIENT_CLOSED;
    }
    if (c->sess.state != APAD_SESSION_ACTIVE) {
        return (c->sess.state == APAD_SESSION_CLOSED) ? APAD_CLIENT_CLOSED
                                                      : APAD_CLIENT_IDLE;
    }

    /* Wait only as long as remains before the next INPUT_STATE is due, so the
     * send cadence is driven by the clock rather than by whether a packet
     * happened to arrive. */
    now     = apad_ticks_ms();
    elapsed = apad_time_since(now, c->last_input_ms);
    wait    = (elapsed >= c->input_interval_ms)
                  ? 0 : (int)(c->input_interval_ms - elapsed);
    if (max_wait_ms >= 0 && wait > max_wait_ms) {
        wait = max_wait_ms;
    }
    (void)drain_rx(c, wait);

    now    = apad_ticks_ms();
    action = apad_session_tick(&c->sess, now);
    if (action == APAD_ACT_TIMEOUT) {
        apad_session_close(&c->sess, APAD_CLOSE_IDLE_TIMEOUT);
        settle_auth(c);
        return APAD_CLIENT_CLOSED;
    }
    if (c->sess.state != APAD_SESSION_ACTIVE) {
        settle_auth(c);
        return APAD_CLIENT_CLOSED;
    }

    /* INPUT_STATE at the negotiated rate. It is never RELIABLE and never
     * retransmitted (§9): a stale pad position is worse than a missing one.
     * It also satisfies §8's 100 ms keepalive floor on its own at any rate
     * this client negotiates, so APAD_ACT_KEEPALIVE needs no separate
     * handling here. */
    if (in != NULL && apad_time_since(now, c->last_input_ms) >= c->input_interval_ms) {
        uint8_t payload[APAD_LEN_INPUT_STATE];
        apad_input_state st = *in;

        st.client_ticks_ms = now;
        if (apad_encode_input_state(payload, sizeof payload, &st)
            == (int)APAD_LEN_INPUT_STATE) {
            (void)send_msg(c, (uint8_t)APAD_MSG_INPUT_STATE, payload,
                           (uint16_t)sizeof payload, NULL);
        }
        c->last_input_ms = now;
    }

    if (!c->awaiting_pong && apad_time_since(now, c->last_ping_ms) >= APAD_PING_INTERVAL_MS) {
        uint8_t payload[APAD_LEN_PING];
        apad_ping ping;

        memset(&ping, 0, sizeof ping);
        ping.origin_ticks_ms = now;
        if (apad_encode_ping(payload, sizeof payload, &ping) == (int)APAD_LEN_PING
            && send_msg(c, (uint8_t)APAD_MSG_PING, payload,
                        (uint16_t)sizeof payload, NULL) >= 0) {
            c->ping_origin_ms = now;
            c->awaiting_pong  = 1;
        }
        c->last_ping_ms = now;
    } else if (c->awaiting_pong && apad_time_since(now, c->last_ping_ms) >= APAD_PING_INTERVAL_MS) {
        /* Unanswered: stop waiting so the next tick can ask again rather than
         * blocking RTT reporting forever on one lost datagram. */
        c->awaiting_pong = 0;
        c->last_ping_ms  = now;
    }

    return APAD_CLIENT_ACTIVE;
}

void apad_client_get_stats(const apad_client *c, apad_client_stats *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (c == NULL) {
        out->state            = APAD_CLIENT_CLOSED;
        out->rtt_ms           = -1;
        out->pairing_required = -1;
        return;
    }
    switch (c->sess.state) {
    case APAD_SESSION_ACTIVE:       out->state = APAD_CLIENT_ACTIVE;      break;
    case APAD_SESSION_HANDSHAKING:  out->state = APAD_CLIENT_HANDSHAKING; break;
    case APAD_SESSION_CLOSED:       out->state = APAD_CLIENT_CLOSED;      break;
    default:                        out->state = APAD_CLIENT_IDLE;        break;
    }
    out->session_id         = (int32_t)c->sess.session_id;
    out->pad_slot           = (int32_t)c->sess.pad_slot;
    out->input_rate_hz      = (int32_t)c->sess.input_rate_hz;
    out->rtt_ms             = c->rtt_ms;
    out->close_reason       = (int32_t)c->sess.close_reason;
    out->tx_packets         = c->tx_packets;
    out->rx_packets         = c->rx_packets;
    out->last_error         = c->last_error;
    out->rumble_serial      = c->rumble_serial;
    out->rumble_low         = c->rumble_low;
    out->rumble_high        = c->rumble_high;
    out->rumble_duration_ms = c->rumble_duration_ms;
    out->led_serial         = c->led_serial;
    out->led_player         = c->led_player;
    out->led_rgb            = c->led_rgb;
    out->status_serial      = c->status_serial;
    out->status_code        = c->status_code;
    out->pairing_required   = c->pairing_required;
    out->auth_required      = c->auth_required;
    out->auth_state         = c->auth_state;
    out->error_code         = c->error_code;
}

const char *apad_client_message(const apad_client *c)
{
    return (c != NULL) ? c->message : "";
}

void apad_client_disconnect(apad_client *c)
{
    if (c == NULL) {
        return;
    }
    if (c->sess.state == APAD_SESSION_ACTIVE) {
        uint8_t payload[APAD_LEN_BYE];
        apad_bye bye;

        memset(&bye, 0, sizeof bye);
        bye.reason = (uint8_t)APAD_BYE_NORMAL;
        if (apad_encode_bye(payload, sizeof payload, &bye) == (int)APAD_LEN_BYE) {
            (void)send_msg(c, (uint8_t)APAD_MSG_BYE, payload,
                           (uint16_t)sizeof payload, NULL);
        }
    }
    apad_session_close(&c->sess, APAD_CLOSE_LOCAL);
}

void apad_client_destroy(apad_client *c)
{
    if (c == NULL) {
        return;
    }
    apad_client_disconnect(c);
    if (c->sock != NULL) {
        apad_udp_close(c->sock);
    }
    /* §10: the secret is the one thing here that outlives a session. It never
     * went on the wire and it must not be left in a freed heap block either. */
    apad_secure_zero(c->secret, sizeof c->secret);
    free(c);
}
