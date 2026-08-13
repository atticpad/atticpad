/* session.c — handshake and session state machine (docs/PROTOCOL.md §8, §9).
 *
 * One apad_session is one (source IP, source port, session_id) triple as far
 * as the server is concerned (§1); the client keeps exactly one. Everything
 * is caller-owned: the server's array of APAD_MAX_SESSIONS is a static array,
 * and nothing here allocates.
 *
 * The two rules this file exists to enforce:
 *   §9  INPUT_STATE older than the newest already seen is DISCARDED. A late
 *       packet is worse than no packet — applying it moves the stick
 *       backwards, which the user feels as a stutter.
 *   §9  INPUT_STATE is never retransmitted, even if a caller sets RELIABLE.
 *
 * Every comparison of a sequence or a tick goes through seq.c. There is no
 * bare `>` in this file and there must never be one.
 */

#include <string.h>

#include "atticpad/atticpad.h"

/* §4: which types carry the RELIABLE flag. */
static int type_is_reliable(uint8_t type)
{
    switch (type) {
    case APAD_MSG_HELLO:
    case APAD_MSG_WELCOME:
    case APAD_MSG_BYE:
    case APAD_MSG_RUMBLE:
    case APAD_MSG_LED:
    case APAD_MSG_STATUS:
        return 1;
    default:
        return 0;
    }
}

void apad_session_init(apad_session *s, int is_server, uint32_t now)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof *s);
    s->state         = (uint8_t)APAD_SESSION_IDLE;
    s->is_server     = (uint8_t)(is_server ? 1 : 0);
    s->close_reason  = (uint8_t)APAD_CLOSE_NONE;
    s->input_rate_hz = (uint16_t)APAD_DEFAULT_RATE_HZ;
    s->last_rx_ms    = now;
    s->last_tx_ms    = now;
}

void apad_session_set_key(apad_session *s, const uint8_t key[APAD_SESSION_KEY_LEN])
{
    if (s == NULL || key == NULL) {
        return;
    }
    memcpy(s->key, key, APAD_SESSION_KEY_LEN);
    s->have_key      = 1u;
    s->authenticated = 1u;   /* §10: every packet after WELCOME carries a tag */
}

void apad_session_close(apad_session *s, int reason)
{
    if (s == NULL) {
        return;
    }
    s->state        = (uint8_t)APAD_SESSION_CLOSED;
    s->close_reason = (uint8_t)reason;
    s->retx_armed   = 0u;
    s->authenticated = 0u;
    s->have_key      = 0u;
    apad_secure_zero(s->key, sizeof s->key);
}

/*
 * Server-side counterpart to the client's "adopt from WELCOME" path in
 * apad_session_on_recv (case APAD_MSG_WELCOME). The server is the side that
 * INVENTS session_id and pad_slot, so there is nothing on the wire for it to
 * adopt; without this entry point a server has to write apad_session's
 * fields directly, which the header says not to do.
 *
 * Call order matters and is the whole reason this is one function rather
 * than three setters:
 *
 *   1. apad_packet_parse()            -- the inbound HELLO
 *   2. apad_session_on_recv()         -- HELLO moves the server to
 *                                        HANDSHAKING and records peer_rate_hz
 *   3. apad_session_server_accept()   -- assign id/slot/rate, go ACTIVE
 *   4. apad_encode_welcome(&w)        -- w has been prefilled by step 3
 *   5. apad_session_next_header(APAD_MSG_WELCOME)  -- picks up the non-zero
 *                                        session_id assigned in step 3 (§8:
 *                                        WELCOME is not one of the three
 *                                        messages that carry session_id 0)
 *   6. apad_packet_build() + send + apad_session_on_sent()
 *
 * Doing step 3 after step 5 would put session_id 0 in the WELCOME header.
 */
int apad_session_server_accept(apad_session *s,
                               uint16_t session_id,
                               uint8_t pad_slot,
                               uint16_t input_rate_hz,
                               uint32_t now,
                               apad_welcome *out_welcome)
{
    if (s == NULL) {
        return APAD_ERR_ARG;
    }
    if (!s->is_server) {
        return APAD_ERR_STATE;   /* the client adopts, it does not assign */
    }
    if (session_id == 0u) {
        return APAD_ERR_ARG;     /* §6.4: non-zero; §8: 0 means "no session" */
    }
    if (input_rate_hz > (uint16_t)APAD_MAX_RATE_HZ) {
        return APAD_ERR_ARG;     /* §11: 125 Hz ceiling. A client's nonsense
                                  * desired_rate_hz is the caller's to clamp;
                                  * silently capping it here would hide it. */
    }
    /* Only a session that has actually seen a HELLO may be accepted. ACTIVE
     * is allowed so that a retransmitted HELLO (§9) re-accepts idempotently
     * rather than failing. */
    if (s->state != (uint8_t)APAD_SESSION_HANDSHAKING
        && s->state != (uint8_t)APAD_SESSION_ACTIVE) {
        return APAD_ERR_STATE;
    }

    s->session_id    = session_id;
    s->pad_slot      = pad_slot;
    s->input_rate_hz = (input_rate_hz != 0u)
                       ? input_rate_hz : (uint16_t)APAD_DEFAULT_RATE_HZ;
    s->state         = (uint8_t)APAD_SESSION_ACTIVE;

    /* §9: forget the previous occupant's receive window. A pad slot that is
     * reused by a new client would otherwise reject that client's first
     * ~32767 INPUT_STATE packets as stale, because they are "older" than the
     * sequence the old client left behind. Clearing can never wrongly reject:
     * the next INPUT_STATE seen simply becomes the newest. */
    s->rx_input_valid = 0u;
    s->rx_input_seq   = 0u;

    /* §8/§11: the 3-second idle timer only runs in ACTIVE, so it starts here.
     * apad_session_tick would otherwise never time a server session out --
     * the server FSM never entered ACTIVE at all before this function. */
    s->last_rx_ms = now;

    if (out_welcome != NULL) {
        memset(out_welcome, 0, sizeof *out_welcome);
        out_welcome->session_id      = s->session_id;
        out_welcome->pad_slot        = s->pad_slot;
        out_welcome->input_rate_hz   = s->input_rate_hz;
        out_welcome->server_ticks_ms = now;
        /* flags and server_nonce are left zero on purpose: whether pairing is
         * required, and what salt to publish, are decisions this function
         * does not make. A caller doing §10 auth sets
         * APAD_WELCOME_AUTH_REQUIRED and fills server_nonce afterwards.
         * key_material stays zero -- §6.4 says it MUST be zero in v1. */
    }

    return APAD_OK;
}

int apad_session_next_header(apad_session *s, uint8_t type, apad_header *out)
{
    if (s == NULL || out == NULL) {
        return APAD_ERR_ARG;
    }
    if (apad_payload_size(type) == APAD_ERR_TYPE) {
        return APAD_ERR_TYPE;
    }
    if (s->state == (uint8_t)APAD_SESSION_CLOSED) {
        return APAD_ERR_STATE;
    }

    memset(out, 0, sizeof *out);
    out->magic   = APAD_MAGIC;
    out->version = (uint8_t)APAD_VERSION;
    out->type    = type;

    /* §8: session_id is 0 in DISCOVER, ANNOUNCE and HELLO; non-zero after
     * WELCOME. Anything else carries whatever the session holds, which is 0
     * until WELCOME lands — exactly what the spec asks for. */
    switch (type) {
    case APAD_MSG_DISCOVER:
    case APAD_MSG_ANNOUNCE:
    case APAD_MSG_HELLO:
        out->session_id = 0u;
        break;
    default:
        out->session_id = s->session_id;
        break;
    }

    out->sequence    = s->tx_seq;
    out->payload_len = 0u;   /* apad_packet_build fills this in */
    out->flags       = 0u;
    if (type_is_reliable(type)) {
        out->flags = (uint16_t)(out->flags | APAD_FLAG_RELIABLE);
    }
    if (s->authenticated && s->have_key) {
        out->flags = (uint16_t)(out->flags | APAD_FLAG_AUTHENTICATED);
    }

    s->tx_seq = apad_seq_next(s->tx_seq);
    return APAD_OK;
}

void apad_session_on_sent(apad_session *s, const apad_header *hdr, uint32_t now)
{
    if (s == NULL || hdr == NULL) {
        return;
    }
    s->last_tx_ms = now;

    /* Advance the client FSM on the packet that actually left the box, not on
     * the one we merely encoded. */
    if (!s->is_server) {
        if (hdr->type == APAD_MSG_DISCOVER && s->state == (uint8_t)APAD_SESSION_IDLE) {
            s->state = (uint8_t)APAD_SESSION_DISCOVERING;
        } else if (hdr->type == APAD_MSG_HELLO
                   && s->state != (uint8_t)APAD_SESSION_ACTIVE) {
            s->state = (uint8_t)APAD_SESSION_HANDSHAKING;
        }
    }

    /* §9: INPUT_STATE MUST NOT be retransmitted. Refuse to arm it even if a
     * caller hands us a header with RELIABLE set. */
    if (hdr->type == APAD_MSG_INPUT_STATE) {
        return;
    }
    if ((hdr->flags & APAD_FLAG_RELIABLE) == 0u) {
        return;
    }
    /* One reliable message in flight at a time. A second arm replaces the
     * first: the newer control message supersedes it. */
    s->retx_armed   = 1u;
    s->retx_attempt = 0u;
    s->retx_type    = hdr->type;
    s->retx_seq     = hdr->sequence;
    s->retx_due_ms  = now + apad_retx_delays_ms[0];
}

int apad_session_accept_input(apad_session *s, uint16_t seq)
{
    if (s == NULL) {
        return APAD_ERR_ARG;
    }
    if (!s->rx_input_valid) {
        s->rx_input_valid = 1u;
        s->rx_input_seq   = seq;
        return APAD_OK;
    }
    /* §9: discard anything not strictly newer than the newest already seen.
     * Equal counts as stale — it is a duplicate. */
    if (!apad_seq_newer(seq, s->rx_input_seq)) {
        return APAD_ERR_STALE;
    }
    s->rx_input_seq = seq;
    return APAD_OK;
}

int apad_session_on_recv(apad_session *s, const apad_packet *pkt, uint32_t now)
{
    if (s == NULL || pkt == NULL) {
        return APAD_ERR_ARG;
    }
    if (s->state == (uint8_t)APAD_SESSION_CLOSED) {
        return APAD_ERR_STATE;
    }

    /* Liveness and freshness are different questions and must not be
     * conflated. §8 tears down a session with "no packet received for 3
     * seconds": a reordered or duplicate INPUT_STATE IS a packet received, so
     * it refreshes the idle timer. §9 then says the packet's CONTENTS are
     * stale and MUST be discarded, because applying it moves the stick
     * backwards and the user feels a stutter. So: refresh first, judge after. */
    s->last_rx_ms = now;

    if (pkt->header.type == APAD_MSG_INPUT_STATE) {
        int rc = apad_session_accept_input(s, pkt->header.sequence);
        if (rc != APAD_OK) {
            return rc;   /* caller drops the payload; the session stays alive */
        }
    }

    switch (pkt->header.type) {
    case APAD_MSG_WELCOME: {
        apad_welcome w;
        int rc = apad_decode_welcome(pkt->payload, pkt->payload_len, &w);
        if (rc < 0) {
            return rc;
        }
        if (w.session_id == 0u) {
            return APAD_ERR_LENGTH;   /* §6.4: MUST be non-zero */
        }
        s->session_id    = w.session_id;
        s->pad_slot      = w.pad_slot;
        s->input_rate_hz = w.input_rate_hz;
        s->state         = (uint8_t)APAD_SESSION_ACTIVE;
        /* §9 "What discharges a reliable message": WELCOME is the direct
         * answer to HELLO, and the only such pair in v1. Discharge the armed
         * message ONLY if it is that HELLO. Clearing whatever happened to be
         * armed would misfire on a reachable sequence: client ACKs a WELCOME
         * and sends BYE, the ACK is lost, the server's WELCOME retransmit
         * arrives, and the client silently stops retransmitting a BYE that
         * §9 says requires an explicit ACK. */
        if (s->retx_armed && s->retx_type == (uint8_t)APAD_MSG_HELLO) {
            s->retx_armed = 0u;
        }
        break;
    }
    case APAD_MSG_HELLO: {
        apad_hello h;
        int rc = apad_decode_hello(pkt->payload, pkt->payload_len, &h);
        if (rc < 0) {
            return rc;
        }
        /* §6.3: proto_major MUST equal the header version. §12: hard reject. */
        if (h.proto_major != pkt->header.version) {
            return APAD_ERR_VERSION;
        }
        s->peer_rate_hz = h.desired_rate_hz;
        if (s->is_server) {
            s->state = (uint8_t)APAD_SESSION_HANDSHAKING;
        }
        break;
    }
    case APAD_MSG_ANNOUNCE:
        /* §8: ANNOUNCE does not advance the FSM. The caller may receive
         * several, picks one server, and sends HELLO. */
        break;
    case APAD_MSG_ACK: {
        apad_ack a;
        int rc = apad_decode_ack(pkt->payload, pkt->payload_len, &a);
        if (rc < 0) {
            return rc;
        }
        if (s->retx_armed && a.sequence == s->retx_seq) {
            s->retx_armed = 0u;   /* §9: ACK echoes the sequence acknowledged */
        }
        break;
    }
    case APAD_MSG_BYE:
        apad_session_close(s, APAD_CLOSE_PEER_BYE);
        break;
    default:
        break;
    }

    return APAD_OK;
}

int apad_session_tick(apad_session *s, uint32_t now)
{
    if (s == NULL) {
        return APAD_ACT_NONE;
    }
    if (s->state == (uint8_t)APAD_SESSION_CLOSED) {
        return APAD_ACT_NONE;
    }

    /* §9: doubling gaps of 100, 200, 400 and 800 ms. Four retransmits go out
     * at t = 100, 300, 700 and 1500 ms measured from the original send, and
     * the session fails at t = 2300 ms if nothing has discharged it.
     *
     * Checked before the idle timeout so a dead peer is reported as a
     * delivery failure rather than a generic timeout — the two mean different
     * things to the UI. */
    if (s->retx_armed && apad_time_reached(now, s->retx_due_ms)) {
        if (s->retx_attempt >= (uint8_t)APAD_RETX_COUNT) {
            apad_session_close(s, APAD_CLOSE_RETX_FAILED);
            return APAD_ACT_TIMEOUT;
        }
        s->retx_attempt = (uint8_t)(s->retx_attempt + 1u);
        s->retx_due_ms  = now + apad_retx_delays_ms[
            (s->retx_attempt < (uint8_t)APAD_RETX_COUNT)
                ? s->retx_attempt : (uint8_t)(APAD_RETX_COUNT - 1)];
        return APAD_ACT_RETRANSMIT;
    }

    /* §8, §11: 3000 ms with nothing received tears the session down. Only
     * meaningful once the session exists. */
    if (s->state == (uint8_t)APAD_SESSION_ACTIVE
        && apad_time_since(now, s->last_rx_ms) >= APAD_IDLE_TIMEOUT_MS) {
        apad_session_close(s, APAD_CLOSE_IDLE_TIMEOUT);
        return APAD_ACT_TIMEOUT;
    }

    /* §8: a client MUST send at least one packet every 100 ms, even when
     * nothing changed. The server has no such obligation. */
    if (!s->is_server
        && s->state == (uint8_t)APAD_SESSION_ACTIVE
        && apad_time_since(now, s->last_tx_ms) >= APAD_KEEPALIVE_MS) {
        return APAD_ACT_KEEPALIVE;
    }

    return APAD_ACT_NONE;
}
