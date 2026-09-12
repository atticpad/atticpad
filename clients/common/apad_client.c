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
    uint32_t derive_ms;      /* apad_client_stats.derive_ms; see there   */

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
    /* Last TOUCHMAP the server sent (§6.12). */
    uint32_t      touchmap_serial;
    apad_touchmap touchmap;

    /* §6.19 INPUTCAPS. `inputcaps_serial == 0` is the negotiation itself:
     * until one has been accepted this client MUST NOT send KEYBOARD, MOUSE
     * or MEDIA at all. Everything below it is send-side §6.15-§6.17 state. */
    uint32_t       inputcaps_serial;
    apad_inputcaps inputcaps;

    /*
     * §6.15-§6.17 SEND STATE. One block per facility, all the same shape:
     *   kb_keys, mo_buttons + the mo_ accumulators, me_held -- the held
     *             snapshot that goes on the wire, and which §6.20 makes the
     *             receiver's authority
     *   *_seq     §6.20 event_seq: every event ever generated this session
     *   *_ring    the RIGHT-ALIGNED ring, newest always in the last slot
     *   *_tx_ms   when the last datagram of this type went out
     *   *_trail   trailing copies still owed after going idle
     *   *_dirty   something changed and has not been sent yet
     *   *_engaged the caller has driven this facility at least once, so a
     *             baseline packet is worth sending; until then this client
     *             creates no device on the server and sends nothing
     *
     * The ring is NOT cleared after a send. §6.20 requires each datagram to
     * carry the last D events regardless of whether they have already been
     * transmitted -- that redundancy is the entire mechanism by which a
     * dropped datagram costs nothing.
     */
    uint8_t          kb_keys[APAD_KEY_BITMAP_BYTES];
    uint16_t         kb_seq;
    apad_key_event   kb_ring[APAD_KEYBOARD_RING_DEPTH];
    uint32_t         kb_tx_ms;
    uint32_t         kb_trail_due_ms;
    uint8_t          kb_trail;
    uint8_t          kb_dirty;
    uint8_t          kb_engaged;

    uint16_t         mo_dx, mo_dy, mo_wheel, mo_hwheel;
    uint16_t         mo_buttons;
    uint16_t         mo_seq;
    apad_mouse_event mo_ring[APAD_MOUSE_RING_DEPTH];
    uint32_t         mo_tx_ms;
    uint32_t         mo_trail_due_ms;
    uint8_t          mo_trail;
    uint8_t          mo_dirty;
    uint8_t          mo_engaged;

    uint32_t         me_held;
    uint16_t         me_seq;
    apad_media_event me_ring[APAD_MEDIA_RING_DEPTH];
    uint32_t         me_tx_ms;
    uint32_t         me_trail_due_ms;
    uint8_t          me_trail;
    uint8_t          me_dirty;
    uint8_t          me_engaged;

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

/* ---- §6.15-§6.20 keyboard / mouse / media, send side -------------------- *
 *
 * Everything §6.19 and §6.20 ask of a SENDER lives in this block, once, so
 * that no platform layer has to carry a copy of it (apad_client.h's charter,
 * and docs/DESIGN.md §7.2). The three facilities are deliberately written out three
 * times rather than folded behind a table of function pointers: the shapes
 * differ in exactly the places that matter (256 bits vs 5 vs 32, a ring of 8
 * vs 4, accumulators on one of them only), and a generic version would hide
 * those differences behind casts on structs this file is forbidden to cast.
 */

/* §6.20's held-repeat floor is 10 Hz and it is a MUST: below it, a
 * change-only sender is indistinguishable from a dead one and the receiver's
 * 1000 ms watchdog releases the chord under the user's fingers. A 100 ms
 * period sits exactly ON the floor, so any scheduling jitter puts this client
 * under a MUST; 80 ms (12.5 Hz) costs two 56-byte datagrams a second while a
 * key is held and cannot. */
#define APAD_KBM_REPEAT_MS  80u

/* §6.15: "three copies about 50 ms apart after the last key releases". They
 * are what makes a release, and a tap that lives only in the ring, survive
 * packet loss -- the one thing a state snapshot cannot repair by itself. */
#define APAD_KBM_TRAIL_MS      50u
#define APAD_KBM_TRAIL_COPIES   3u

/* §11's ceiling, as a minimum spacing between two datagrams of one type. It
 * is safe to delay a keyboard change by up to this long precisely BECAUSE of
 * the ring: two transitions coalesced into one datagram both still reach the
 * host, in order, which is not true of a snapshot-only protocol. */
#define APAD_KBM_MIN_TX_MS  (1000u / APAD_MAX_RATE_HZ)

/*
 * §6.19's negotiation, in one place. No INPUTCAPS accepted yet -> nothing may
 * be sent, full stop: a v1 server cannot send that message and would discard
 * KEYBOARD/MOUSE/MEDIA anyway, so absence is how it declines without knowing
 * the question was asked. Then the matching `features` bit, which is the
 * server saying it will accept this type right now.
 */
static int kbm_gate(const apad_client *c, uint32_t feature)
{
    return (c->sess.state == (uint8_t)APAD_SESSION_ACTIVE
            && c->inputcaps_serial != 0u
            && (c->inputcaps.features & feature) != 0u) ? 1 : 0;
}

/*
 * §6.19 mouse cadence. `mouse_rate_hz` is A REQUEST, NOT AN AUTHORISATION:
 * the field exists so a server can ask for LESS than the session rate (a
 * pointer rarely needs 125 Hz), and a client that reads a value above §11's
 * ceiling MUST send at the ceiling instead. The decoder preserves whatever
 * the server actually said -- clamping belongs here, at the send rate, and
 * nowhere else.
 */
static uint32_t kbm_mouse_interval_ms(const apad_client *c)
{
    uint32_t hz = (uint32_t)c->inputcaps.mouse_rate_hz;
    uint32_t ms;

    if (hz == 0u) {                       /* §6.19: 0 = the session's rate */
        hz = (uint32_t)c->sess.input_rate_hz;
    }
    if (hz == 0u) {
        hz = (uint32_t)APAD_DEFAULT_RATE_HZ;
    }
    if (hz > (uint32_t)APAD_MAX_RATE_HZ) {
        hz = (uint32_t)APAD_MAX_RATE_HZ;  /* §11 */
    }
    ms = 1000u / hz;
    return (ms == 0u) ? 1u : ms;
}

/* ---- the §6.20 ring ---------------------------------------------------- *
 *
 * RIGHT-ALIGNED, and that is normative rather than a choice: for depth D,
 * slot i carries the event with ordinal `event_seq - (D-1) + i`, so the
 * NEWEST is always in slot D-1 and slots whose ordinal predates the session's
 * first event are transmitted zeroed. Front-packing satisfies "oldest first"
 * too and is explicitly NON-CONFORMANT, because gap replay finds the newest
 * event by position rather than by counting.
 *
 * Shifting down by one and writing slot D-1 maintains that invariant for
 * free, and `apad_seq_next` keeps the ordinal in step: the first event of a
 * session lands in slot D-1 with event_seq == 1, which is its ordinal, and
 * the D-1 zeroed slots above it are exactly the ordinals <= 0 that never
 * existed.
 */
static void kbm_ring_push_key(apad_client *c, uint8_t usage, uint8_t flags)
{
    size_t i;

    for (i = 0u; i + 1u < (size_t)APAD_KEYBOARD_RING_DEPTH; i++) {
        c->kb_ring[i] = c->kb_ring[i + 1u];
    }
    c->kb_ring[APAD_KEYBOARD_RING_DEPTH - 1u].usage = usage;
    c->kb_ring[APAD_KEYBOARD_RING_DEPTH - 1u].flags = flags;
    c->kb_seq = apad_seq_next(c->kb_seq);
    c->kb_dirty = 1u;
}

static void kbm_ring_push_mouse(apad_client *c, uint8_t button, uint8_t flags)
{
    size_t i;

    for (i = 0u; i + 1u < (size_t)APAD_MOUSE_RING_DEPTH; i++) {
        c->mo_ring[i] = c->mo_ring[i + 1u];
    }
    c->mo_ring[APAD_MOUSE_RING_DEPTH - 1u].button = button;
    c->mo_ring[APAD_MOUSE_RING_DEPTH - 1u].flags  = flags;
    c->mo_seq = apad_seq_next(c->mo_seq);
    c->mo_dirty = 1u;
}

static void kbm_ring_push_media(apad_client *c, uint8_t control, uint8_t flags)
{
    size_t i;

    for (i = 0u; i + 1u < (size_t)APAD_MEDIA_RING_DEPTH; i++) {
        c->me_ring[i] = c->me_ring[i + 1u];
    }
    c->me_ring[APAD_MEDIA_RING_DEPTH - 1u].control = control;
    c->me_ring[APAD_MEDIA_RING_DEPTH - 1u].flags   = flags;
    c->me_seq = apad_seq_next(c->me_seq);
    c->me_dirty = 1u;
}

/* ---- sending one datagram of each type --------------------------------- */

static void kbm_send_keyboard(apad_client *c, uint32_t now)
{
    uint8_t       payload[APAD_LEN_KEYBOARD];
    apad_keyboard kb;

    memset(&kb, 0, sizeof kb);
    memcpy(kb.keys, c->kb_keys, sizeof kb.keys);
    kb.event_seq = c->kb_seq;
    memcpy(kb.events, c->kb_ring, sizeof kb.events);
    kb.client_ticks_ms = now;
    if (apad_encode_keyboard(payload, sizeof payload, &kb)
        == (int)APAD_LEN_KEYBOARD) {
        (void)send_msg(c, (uint8_t)APAD_MSG_KEYBOARD, payload,
                       (uint16_t)APAD_LEN_KEYBOARD, NULL);
    }
    c->kb_tx_ms = now;
    c->kb_dirty = 0u;
}

static void kbm_send_mouse(apad_client *c, uint32_t now)
{
    uint8_t    payload[APAD_LEN_MOUSE];
    apad_mouse mo;

    memset(&mo, 0, sizeof mo);
    mo.dx_accum     = c->mo_dx;
    mo.dy_accum     = c->mo_dy;
    mo.wheel_accum  = c->mo_wheel;
    mo.hwheel_accum = c->mo_hwheel;
    mo.buttons      = c->mo_buttons;
    mo.event_seq    = c->mo_seq;
    memcpy(mo.events, c->mo_ring, sizeof mo.events);
    mo.client_ticks_ms = now;
    if (apad_encode_mouse(payload, sizeof payload, &mo) == (int)APAD_LEN_MOUSE) {
        (void)send_msg(c, (uint8_t)APAD_MSG_MOUSE, payload,
                       (uint16_t)APAD_LEN_MOUSE, NULL);
    }
    c->mo_tx_ms = now;
    c->mo_dirty = 0u;
}

static void kbm_send_media(apad_client *c, uint32_t now)
{
    uint8_t    payload[APAD_LEN_MEDIA];
    apad_media me;

    memset(&me, 0, sizeof me);
    me.held      = c->me_held;
    me.event_seq = c->me_seq;
    memcpy(me.events, c->me_ring, sizeof me.events);
    me.client_ticks_ms = now;
    if (apad_encode_media(payload, sizeof payload, &me) == (int)APAD_LEN_MEDIA) {
        (void)send_msg(c, (uint8_t)APAD_MSG_MEDIA, payload,
                       (uint16_t)APAD_LEN_MEDIA, NULL);
    }
    c->me_tx_ms = now;
    c->me_dirty = 0u;
}

/* ---- §6.19 / §6.20 release --------------------------------------------- *
 *
 * "A sender MUST release before it stops sending: on leaving the mode, on a
 * `features` bit clearing (§6.19), and before BYE."
 *
 * The reason this is a MUST and not housekeeping: a physical keyboard
 * releases its keys when it is unplugged and an injected one does not. Where
 * INPUTCAPS.status reports SYNTHETIC there is no device to unplug, so a held
 * Ctrl stays held on the user's desktop until something explicitly lifts it,
 * and once this client has stopped sending there is nothing left that can.
 * §6.20 calls that "the one failure this whole section exists to prevent".
 *
 * An UP event per held bit, then one datagram carrying the cleared snapshot.
 * If more bits are held than the ring can carry, the ring overflows and the
 * receiver's §6.20 step 3 reconcile against the all-zero snapshot finishes
 * the job -- the snapshot is the authority, the ring only adds the edges.
 *
 * Sends unconditionally when something is held, INCLUDING from the
 * features-cleared path: it must go out while the old `features` still
 * permits it, which is why handle_packet() calls this BEFORE storing the new
 * capabilities.
 */
static void kbm_release_keyboard(apad_client *c, uint32_t now)
{
    unsigned byte_i;
    int      held = 0;

    for (byte_i = 0u; byte_i < APAD_KEY_BITMAP_BYTES; byte_i++) {
        unsigned bit;

        if (c->kb_keys[byte_i] == 0u) {
            continue;
        }
        for (bit = 0u; bit < 8u; bit++) {
            uint8_t mask = (uint8_t)(1u << bit);

            if ((c->kb_keys[byte_i] & mask) == 0u) {
                continue;
            }
            kbm_ring_push_key(c, (uint8_t)(byte_i * 8u + bit), 0u /* UP */);
            held = 1;
        }
        c->kb_keys[byte_i] = 0u;
    }
    if (held) {
        kbm_send_keyboard(c, now);
    }
    c->kb_trail = 0u;
    c->kb_engaged = 0u;
}

static void kbm_release_mouse(apad_client *c, uint32_t now)
{
    unsigned b;
    int      held = 0;

    for (b = 1u; b <= APAD_MOUSEBTN_MAX; b++) {
        uint16_t bit = APAD_MOUSEBTN_BIT(b);

        if ((c->mo_buttons & bit) == 0u) {
            continue;
        }
        kbm_ring_push_mouse(c, (uint8_t)b, 0u /* UP */);
        held = 1;
    }
    c->mo_buttons = 0u;
    if (held) {
        kbm_send_mouse(c, now);
    }
    c->mo_trail = 0u;
    c->mo_engaged = 0u;
}

static void kbm_release_media(apad_client *c, uint32_t now)
{
    unsigned ctl;
    int      held = 0;

    for (ctl = 1u; ctl <= APAD_MEDIA_INDEX_MAX; ctl++) {
        uint32_t bit = APAD_MEDIA_BIT(ctl);

        if ((c->me_held & bit) == 0u) {
            continue;
        }
        kbm_ring_push_media(c, (uint8_t)ctl, 0u /* UP */);
        held = 1;
    }
    c->me_held = 0u;
    if (held) {
        kbm_send_media(c, now);
    }
    c->me_trail = 0u;
    c->me_engaged = 0u;
}

/* Release every facility this client still holds, for a teardown that is not
 * a capability change: §6.20's "before BYE", and the same call covers §11's
 * idle timeout path because disconnect() runs on the way out of both. */
static void kbm_release_all(apad_client *c, uint32_t now)
{
    if (kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_KEYBOARD)) {
        kbm_release_keyboard(c, now);
    }
    if (kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_MOUSE)) {
        kbm_release_mouse(c, now);
    }
    if (kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_MEDIA)) {
        kbm_release_media(c, now);
    }
}

/* ---- ingesting one pump's worth from the platform ---------------------- *
 *
 * Two sources of truth arrive together and the order between them is the
 * whole contract (see apad_client.h): the caller's events[] QUEUE carries
 * transitions that a snapshot cannot express -- above all the sub-frame tap,
 * pressed and released between two pumps -- and the caller's snapshot is
 * authoritative for what is held afterwards.
 *
 * So: replay the queue into the ring first, then diff the resulting shadow
 * against the caller's snapshot and synthesise an event for every remaining
 * difference. A platform that submits nothing but state still gets correct
 * edges; a platform that submits a tap gets the tap AND agrees with its own
 * snapshot. Both paths converge on the same shadow, which is what the
 * datagram then carries.
 *
 * The queue is read from index 0 until the first zero code (§6.20's "no
 * event"), which is why apad_client.h tells callers to memset first.
 *
 * Out-of-range codes are NOT normalised here (§6.20: normalisation is
 * receive-side, so a sender's bug stays visible to the sender's own tests) --
 * but a code outside the range its HELD MASK can address is kept out of that
 * mask, because APAD_MEDIA_BIT(33) would shift a uint32_t by 32 and that is
 * undefined behaviour rather than a protocol question.
 */
/*
 * The FIRST submission of a facility always produces a datagram, even when
 * nothing at all is held and nothing changed. Two reasons, and the second is
 * the one that would otherwise be found the hard way:
 *
 *   - The receiver creates its device on the first datagram of a type, so
 *     until one arrives there is nothing for a user to watch, nothing for
 *     INPUTCAPS.status to report ready, and the whole facility is invisible.
 *   - §6.16 rule 1 and §6.20 step 1 both consume the FIRST accepted message
 *     of a type as a BASELINE and apply no motion and no events from it. If
 *     that first message were the user's first real movement, that movement
 *     would be silently swallowed. Spending one all-clear datagram at
 *     engagement puts the baseline where it costs nothing.
 */
static void kbm_engage(uint8_t *engaged, uint8_t *dirty)
{
    if (!*engaged) {
        *engaged = 1u;
        *dirty   = 1u;
    }
}

static void kbm_ingest_keyboard(apad_client *c, const apad_keyboard *in)
{
    size_t   i;
    unsigned byte_i;

    for (i = 0u; i < (size_t)APAD_KEYBOARD_RING_DEPTH; i++) {
        uint8_t usage = in->events[i].usage;
        uint8_t down  = (uint8_t)(in->events[i].flags & APAD_KBM_EVENT_DOWN);

        if (usage == 0u) {
            break;                         /* end of the submitted queue */
        }
        kbm_ring_push_key(c, usage, down);
        if (down) {
            c->kb_keys[APAD_KEY_BYTE(usage)] =
                (uint8_t)(c->kb_keys[APAD_KEY_BYTE(usage)]
                          | APAD_KEY_MASK(usage));
        } else {
            c->kb_keys[APAD_KEY_BYTE(usage)] =
                (uint8_t)(c->kb_keys[APAD_KEY_BYTE(usage)]
                          & (uint8_t)~APAD_KEY_MASK(usage));
        }
    }

    for (byte_i = 0u; byte_i < APAD_KEY_BITMAP_BYTES; byte_i++) {
        uint8_t diff = (uint8_t)(c->kb_keys[byte_i] ^ in->keys[byte_i]);
        unsigned bit;

        if (diff == 0u) {
            continue;
        }
        for (bit = 0u; bit < 8u; bit++) {
            uint8_t mask = (uint8_t)(1u << bit);

            if ((diff & mask) == 0u) {
                continue;
            }
            kbm_ring_push_key(c, (uint8_t)(byte_i * 8u + bit),
                              (uint8_t)((in->keys[byte_i] & mask)
                                        ? APAD_KBM_EVENT_DOWN : 0u));
        }
        c->kb_keys[byte_i] = in->keys[byte_i];
    }
    kbm_engage(&c->kb_engaged, &c->kb_dirty);
}

static void kbm_ingest_mouse(apad_client *c, const apad_mouse *in)
{
    size_t   i;
    unsigned b;

    for (i = 0u; i < (size_t)APAD_MOUSE_RING_DEPTH; i++) {
        uint8_t button = in->events[i].button;
        uint8_t down   = (uint8_t)(in->events[i].flags & APAD_KBM_EVENT_DOWN);

        if (button == 0u) {
            break;
        }
        kbm_ring_push_mouse(c, button, down);
        if (button <= (uint8_t)APAD_MOUSEBTN_MAX) {
            uint16_t bit = APAD_MOUSEBTN_BIT(button);

            c->mo_buttons = down ? (uint16_t)(c->mo_buttons | bit)
                                 : (uint16_t)(c->mo_buttons & (uint16_t)~bit);
        }
    }

    for (b = 1u; b <= APAD_MOUSEBTN_MAX; b++) {
        uint16_t bit = APAD_MOUSEBTN_BIT(b);
        int      was = (c->mo_buttons & bit) ? 1 : 0;
        int      is  = (in->buttons   & bit) ? 1 : 0;

        if (was != is) {
            kbm_ring_push_mouse(c, (uint8_t)b,
                                (uint8_t)(is ? APAD_KBM_EVENT_DOWN : 0u));
        }
    }
    c->mo_buttons = (uint16_t)(in->buttons & APAD_MOUSEBTN_VALID_MASK);

    /* §6.16: the accumulators are the caller's own free-running wrapping
     * counters and go on the wire verbatim -- their absolute value carries
     * no meaning and the receiver only ever diffs consecutive accepted
     * samples. A CHANGE is what schedules a datagram; an unchanged
     * accumulator already encodes "no motion" exactly, which is why §6.16
     * exempts motion from the held-repeat obligation. */
    if (in->dx_accum != c->mo_dx || in->dy_accum != c->mo_dy
        || in->wheel_accum != c->mo_wheel || in->hwheel_accum != c->mo_hwheel) {
        c->mo_dirty = 1u;
    }
    c->mo_dx     = in->dx_accum;
    c->mo_dy     = in->dy_accum;
    c->mo_wheel  = in->wheel_accum;
    c->mo_hwheel = in->hwheel_accum;
    kbm_engage(&c->mo_engaged, &c->mo_dirty);
}

static void kbm_ingest_media(apad_client *c, const apad_media *in)
{
    size_t   i;
    unsigned ctl;
    uint32_t want;

    for (i = 0u; i < (size_t)APAD_MEDIA_RING_DEPTH; i++) {
        uint8_t control = in->events[i].control;
        uint8_t down    = (uint8_t)(in->events[i].flags & APAD_KBM_EVENT_DOWN);

        if (control == 0u) {
            break;
        }
        kbm_ring_push_media(c, control, down);
        if (control <= (uint8_t)APAD_MEDIA_INDEX_MAX) {
            uint32_t bit = APAD_MEDIA_BIT(control);

            c->me_held = down ? (c->me_held | bit) : (c->me_held & ~bit);
        }
    }

    want = in->held;
    for (ctl = 1u; ctl <= APAD_MEDIA_INDEX_MAX; ctl++) {
        uint32_t bit = APAD_MEDIA_BIT(ctl);
        int      was = (c->me_held & bit) ? 1 : 0;
        int      is  = (want       & bit) ? 1 : 0;

        if (was != is) {
            kbm_ring_push_media(c, (uint8_t)ctl,
                                (uint8_t)(is ? APAD_KBM_EVENT_DOWN : 0u));
        }
    }
    c->me_held = want;
    kbm_engage(&c->me_engaged, &c->me_dirty);
}

/*
 * §6.20's cadence, per facility. Three ways a datagram becomes due, and they
 * are checked in the order they matter:
 *
 *   1. DIRTY -- something changed. Sent as soon as §11's ceiling allows.
 *      Delaying a change by up to 8 ms is safe here and nowhere else in this
 *      protocol: the ring carries both transitions of a tap that coalesced
 *      into one datagram, so nothing is lost, only slightly deferred.
 *   2. HELD -- anything held repeats at better than the 10 Hz floor. MUST.
 *      For MOUSE the repeat covers BUTTONS ONLY, so the interval is the
 *      faster of the two schedules: a server that asked for a 5 Hz pointer
 *      would otherwise drag the button repeat under §6.20's floor and the
 *      watchdog would release a button the user is still holding.
 *   3. TRAILING -- three copies about 50 ms apart once the facility is idle,
 *      armed only by a change (never by a trailing copy, which would repeat
 *      forever). This is what carries a release, or a tap that exists only in
 *      the ring, across packet loss.
 *
 * An idle facility with nothing held and nothing owed sends NOTHING. A client
 * showing a keyboard nobody is typing on costs zero datagrams.
 */
static void kbm_pump_keyboard(apad_client *c, uint32_t now)
{
    int held = 0;
    unsigned i;

    for (i = 0u; i < APAD_KEY_BITMAP_BYTES; i++) {
        if (c->kb_keys[i] != 0u) {
            held = 1;
            break;
        }
    }
    if (c->kb_dirty) {
        if (apad_time_since(now, c->kb_tx_ms) < APAD_KBM_MIN_TX_MS) {
            return;
        }
        kbm_send_keyboard(c, now);
        if (held) {
            c->kb_trail = 0u;
        } else {
            c->kb_trail        = (uint8_t)APAD_KBM_TRAIL_COPIES;
            c->kb_trail_due_ms = now + APAD_KBM_TRAIL_MS;
        }
        return;
    }
    if (held) {
        if (apad_time_since(now, c->kb_tx_ms) >= APAD_KBM_REPEAT_MS) {
            kbm_send_keyboard(c, now);
        }
        return;
    }
    if (c->kb_trail > 0u && apad_time_reached(now, c->kb_trail_due_ms)) {
        kbm_send_keyboard(c, now);
        c->kb_trail--;
        c->kb_trail_due_ms = now + APAD_KBM_TRAIL_MS;
    }
}

static void kbm_pump_mouse(apad_client *c, uint32_t now)
{
    uint32_t interval = kbm_mouse_interval_ms(c);
    int      held     = (c->mo_buttons != 0u) ? 1 : 0;

    if (c->mo_dirty) {
        if (apad_time_since(now, c->mo_tx_ms) < interval) {
            return;
        }
        kbm_send_mouse(c, now);
        if (held) {
            c->mo_trail = 0u;
        } else {
            c->mo_trail        = (uint8_t)APAD_KBM_TRAIL_COPIES;
            c->mo_trail_due_ms = now + APAD_KBM_TRAIL_MS;
        }
        return;
    }
    if (held) {
        uint32_t repeat = (interval < APAD_KBM_REPEAT_MS) ? interval
                                                          : APAD_KBM_REPEAT_MS;
        if (apad_time_since(now, c->mo_tx_ms) >= repeat) {
            kbm_send_mouse(c, now);
        }
        return;
    }
    if (c->mo_trail > 0u && apad_time_reached(now, c->mo_trail_due_ms)) {
        kbm_send_mouse(c, now);
        c->mo_trail--;
        c->mo_trail_due_ms = now + APAD_KBM_TRAIL_MS;
    }
}

static void kbm_pump_media(apad_client *c, uint32_t now)
{
    int held = (c->me_held != 0u) ? 1 : 0;

    if (c->me_dirty) {
        if (apad_time_since(now, c->me_tx_ms) < APAD_KBM_MIN_TX_MS) {
            return;
        }
        kbm_send_media(c, now);
        if (held) {
            c->me_trail = 0u;
        } else {
            c->me_trail        = (uint8_t)APAD_KBM_TRAIL_COPIES;
            c->me_trail_due_ms = now + APAD_KBM_TRAIL_MS;
        }
        return;
    }
    if (held) {
        if (apad_time_since(now, c->me_tx_ms) >= APAD_KBM_REPEAT_MS) {
            kbm_send_media(c, now);
        }
        return;
    }
    if (c->me_trail > 0u && apad_time_reached(now, c->me_trail_due_ms)) {
        kbm_send_media(c, now);
        c->me_trail--;
        c->me_trail_due_ms = now + APAD_KBM_TRAIL_MS;
    }
}

/*
 * One pump's worth of §6.15-§6.17, gated by §6.19.
 *
 * A facility is ingested only while its gate is open. Accumulating events
 * into a ring nobody may transmit would run event_seq away from a server that
 * has never seen one of these datagrams -- harmless (§6.20 step 1 discards a
 * first message's ring) but pointless, and it would make the moment the gate
 * opens look like a burst of history rather than a baseline.
 *
 * Called with kbm == NULL too, from apad_client_pump(): a caller that
 * alternates between the two entry points still owes §6.20's repeats and
 * trailing copies for whatever it left held, and this is where those are
 * paid. A client that has never touched the facility holds nothing, is not
 * engaged, and sends nothing at all -- which is why apad_client_pump()'s
 * behaviour is unchanged by any of this.
 */
static void kbm_pump(apad_client *c, const apad_client_kbm_in *kbm, uint32_t now)
{
    int kb_on = kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_KEYBOARD);
    int mo_on = kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_MOUSE);
    int me_on = kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_MEDIA);

    if (kbm != NULL) {
        if (kb_on && (kbm->have & (uint8_t)APAD_KBM_FEATURE_KEYBOARD) != 0u) {
            kbm_ingest_keyboard(c, &kbm->keyboard);
        }
        if (mo_on && (kbm->have & (uint8_t)APAD_KBM_FEATURE_MOUSE) != 0u) {
            kbm_ingest_mouse(c, &kbm->mouse);
        }
        if (me_on && (kbm->have & (uint8_t)APAD_KBM_FEATURE_MEDIA) != 0u) {
            kbm_ingest_media(c, &kbm->media);
        }
    }
    if (kb_on && c->kb_engaged) {
        kbm_pump_keyboard(c, now);
    }
    if (mo_on && c->mo_engaged) {
        kbm_pump_mouse(c, now);
    }
    if (me_on && c->me_engaged) {
        kbm_pump_media(c, now);
    }
}

/* How long apad_client_pump_ex() may sleep before a §6.20 obligation comes
 * due, or -1 when nothing is owed. Without this the pump would wait for the
 * next INPUT_STATE and a 125 Hz pointer would go out at the 60 Hz session
 * rate -- the schedule would be right and the clock would ignore it. */
static int kbm_next_due_ms(const apad_client *c, uint32_t now)
{
    uint32_t best = 0xFFFFFFFFu;
    uint32_t el;

    if (kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_KEYBOARD) && c->kb_engaged) {
        unsigned i;
        int held = 0;
        for (i = 0u; i < APAD_KEY_BITMAP_BYTES; i++) {
            if (c->kb_keys[i] != 0u) { held = 1; break; }
        }
        if (c->kb_dirty) {
            el = apad_time_since(now, c->kb_tx_ms);
            best = (el >= APAD_KBM_MIN_TX_MS) ? 0u : (APAD_KBM_MIN_TX_MS - el);
        } else if (held) {
            el = apad_time_since(now, c->kb_tx_ms);
            if (el >= APAD_KBM_REPEAT_MS) { best = 0u; }
            else if (APAD_KBM_REPEAT_MS - el < best) { best = APAD_KBM_REPEAT_MS - el; }
        } else if (c->kb_trail > 0u) {
            el = apad_time_since(now, c->kb_tx_ms);
            if (el >= APAD_KBM_TRAIL_MS) { best = 0u; }
            else if (APAD_KBM_TRAIL_MS - el < best) { best = APAD_KBM_TRAIL_MS - el; }
        }
    }
    if (kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_MOUSE) && c->mo_engaged) {
        uint32_t interval = kbm_mouse_interval_ms(c);
        uint32_t want = 0xFFFFFFFFu;

        el = apad_time_since(now, c->mo_tx_ms);
        if (c->mo_dirty) {
            want = (el >= interval) ? 0u : (interval - el);
        } else if (c->mo_buttons != 0u) {
            uint32_t repeat = (interval < APAD_KBM_REPEAT_MS) ? interval
                                                              : APAD_KBM_REPEAT_MS;
            want = (el >= repeat) ? 0u : (repeat - el);
        } else if (c->mo_trail > 0u) {
            want = (el >= APAD_KBM_TRAIL_MS) ? 0u : (APAD_KBM_TRAIL_MS - el);
        }
        if (want < best) { best = want; }
    }
    if (kbm_gate(c, (uint32_t)APAD_KBM_FEATURE_MEDIA) && c->me_engaged) {
        uint32_t want = 0xFFFFFFFFu;

        el = apad_time_since(now, c->me_tx_ms);
        if (c->me_dirty) {
            want = (el >= APAD_KBM_MIN_TX_MS) ? 0u : (APAD_KBM_MIN_TX_MS - el);
        } else if (c->me_held != 0u) {
            want = (el >= APAD_KBM_REPEAT_MS) ? 0u : (APAD_KBM_REPEAT_MS - el);
        } else if (c->me_trail > 0u) {
            want = (el >= APAD_KBM_TRAIL_MS) ? 0u : (APAD_KBM_TRAIL_MS - el);
        }
        if (want < best) { best = want; }
    }
    return (best == 0xFFFFFFFFu) ? -1 : (int)best;
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
        {
            /* Timed, because on a 67-133 MHz ARM9 this is the single slowest
             * thing the client ever does and the number is not academic: the
             * server's §11 idle timer is already running from the ACK that
             * just left, and if this takes longer than 3 s the pairing dies
             * here, silently, every time. The UI shows it (stats.derive_ms). */
            uint32_t t0 = apad_ticks_ms();
            apad_derive_session_key(c->secret, w.server_nonce, key);
            c->derive_ms = apad_time_since(apad_ticks_ms(), t0);
            if (c->derive_ms == 0u) { c->derive_ms = 1u; }   /* "happened" vs "never" */
        }
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
    /* §6.12: store the server's touch layout for the UI. Never
     * affects input handling -- this is a drawing hint and nothing else, so a
     * malformed one costs a redraw, not a session. */
    case APAD_MSG_TOUCHMAP: {
        apad_touchmap tm;
        memset(&tm, 0, sizeof tm);
        if (apad_decode_touchmap(pkt->payload, pkt->payload_len, &tm) >= 0) {
            c->touchmap = tm;
            c->touchmap_serial++;
        }
        break;
    }
    /*
     * §6.19 INPUTCAPS (0x44). Three things happen here and the ORDER IS
     * NORMATIVE.
     *
     * 1. §6.20's per-type window, "before anything else in this section".
     *    This is the fourth of §6.20's four windows and the only one on the
     *    server->client side, so it is the client that has to run it; core
     *    holds the slot (APAD_KBM_CLASS_INPUTCAPS) so the rule §6.20 states
     *    once has one implementation rather than a private copy here.
     *    Without it a reordered copy REVIVES STALE features/status, which
     *    §6.19's "latest wins" forbids -- and the concrete cost is not
     *    cosmetic: a feature bit the server has cleared comes back, and this
     *    client resumes sending a type the server has stopped accepting,
     *    holding keys the server is no longer listening to.
     *
     *    apad_session_on_recv() deliberately does NOT route this type (see
     *    kbm_class_for_type in core/src/session.c); if it did, this call
     *    would be the second application of the same window and would report
     *    every INPUTCAPS after the first as stale.
     *
     * 2. Release before a cleared `features` bit takes effect. §6.19: "If a
     *    features bit clears, the client MUST stop sending that type, and
     *    MUST first release everything it holds for that facility -- otherwise
     *    the last thing the server saw held stays held with nothing left able
     *    to lift it." The release datagram has to go out while the OLD
     *    capabilities still permit it, which is why this runs before the
     *    store below and not after.
     *
     * 3. Store, and bump the serial -- touchmap_serial's convention, so a UI
     *    redraws off a change rather than polling. A serial of 0 means none
     *    has ever been accepted, which §6.19 makes the negotiation itself.
     */
    case APAD_MSG_INPUTCAPS: {
        apad_inputcaps ic;
        uint32_t       had;
        uint32_t       now;

        if (apad_session_accept_kbm(&c->sess, APAD_KBM_CLASS_INPUTCAPS,
                                    pkt->header.sequence) != APAD_OK) {
            break;                     /* §6.20: reordered or duplicate */
        }
        memset(&ic, 0, sizeof ic);
        if (apad_decode_inputcaps(pkt->payload, pkt->payload_len, &ic) < 0) {
            break;
        }
        had = (c->inputcaps_serial != 0u) ? c->inputcaps.features : 0u;
        now = apad_ticks_ms();
        if ((had & (uint32_t)APAD_KBM_FEATURE_KEYBOARD) != 0u
            && (ic.features & (uint32_t)APAD_KBM_FEATURE_KEYBOARD) == 0u) {
            kbm_release_keyboard(c, now);
        }
        if ((had & (uint32_t)APAD_KBM_FEATURE_MOUSE) != 0u
            && (ic.features & (uint32_t)APAD_KBM_FEATURE_MOUSE) == 0u) {
            kbm_release_mouse(c, now);
        }
        if ((had & (uint32_t)APAD_KBM_FEATURE_MEDIA) != 0u
            && (ic.features & (uint32_t)APAD_KBM_FEATURE_MEDIA) == 0u) {
            kbm_release_media(c, now);
        }
        c->inputcaps = ic;
        c->inputcaps_serial++;
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
    /* The limited broadcast address is refused, not sent. This socket never
     * has SO_BROADCAST set -- the probe is the §6.1 "unicast to a manually
     * entered address" case -- and on DSWiFi's lwIP a sendto() to
     * 255.255.255.255 without that option does not fail: it never returns,
     * and the console keeps drawing at 60 fps around a thread that is wedged
     * forever (found on the DS bring-up, 2026-09-09). A tier-2 broadcast
     * DISCOVER is the platform's own job on its own socket, the way
     * clients/3ds/source/main.c's app_discover() does it. */
    {
        apad_addr bcast;
        apad_addr_broadcast(&bcast, c->target.port);
        if (apad_addr_equal(&c->target, &bcast)) {
            note_error(c, APAD_ERR_ARG);
            return APAD_ERR_ARG;
        }
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

    /* §6.19/§6.20 are per-SESSION, not per-client, and both halves matter on
     * a reconnect. The capabilities belong to the session that advertised
     * them, so they go back to "never received" -- the gate closes and this
     * client sends nothing again until a new INPUTCAPS arrives, which is
     * exactly right if the far end is a different server or the same one
     * without uinput this time. And event_seq counts "every event ever
     * generated ON THIS SESSION" (§6.20), so the counters, the rings and the
     * shadows all start over; carrying the old event_seq into a new session
     * would make the receiver's first-accepted baseline meaningless. */
    c->inputcaps_serial = 0u;
    memset(&c->inputcaps, 0, sizeof c->inputcaps);
    memset(c->kb_keys, 0, sizeof c->kb_keys);
    memset(c->kb_ring, 0, sizeof c->kb_ring);
    c->kb_seq = 0u; c->kb_tx_ms = 0u; c->kb_trail_due_ms = 0u;
    c->kb_trail = 0u; c->kb_dirty = 0u; c->kb_engaged = 0u;
    memset(c->mo_ring, 0, sizeof c->mo_ring);
    c->mo_dx = 0u; c->mo_dy = 0u; c->mo_wheel = 0u; c->mo_hwheel = 0u;
    c->mo_buttons = 0u; c->mo_seq = 0u; c->mo_tx_ms = 0u;
    c->mo_trail_due_ms = 0u; c->mo_trail = 0u; c->mo_dirty = 0u;
    c->mo_engaged = 0u;
    memset(c->me_ring, 0, sizeof c->me_ring);
    c->me_held = 0u; c->me_seq = 0u; c->me_tx_ms = 0u;
    c->me_trail_due_ms = 0u; c->me_trail = 0u; c->me_dirty = 0u;
    c->me_engaged = 0u;

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

/*
 * apad_client_pump() is this function with kbm == NULL, and that is the whole
 * relationship: one implementation, one set of §8/§9 timers, no second copy
 * of the session loop that could drift from the first. Every existing caller
 * (clients/3ds, clients/psp, clients/android, tools/engine-client) keeps
 * working untouched, and a client that hands no keyboard, mouse or media puts
 * not one extra byte on the wire.
 */
int apad_client_pump_ex(apad_client *c, const apad_input_state *in,
                        const apad_client_kbm_in *kbm, int max_wait_ms)
{
    uint32_t now;
    uint32_t elapsed;
    int wait;
    int action;
    int kbm_wait;

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
    /* §6.20's schedules are independent of the INPUT_STATE clock and can be
     * faster than it (a 125 Hz pointer under a 60 Hz session, or a 50 ms
     * trailing copy). Sleeping to the next INPUT_STATE regardless would leave
     * the cadence correct on paper and wrong on the wire. */
    kbm_wait = kbm_next_due_ms(c, now);
    if (kbm_wait >= 0 && kbm_wait < wait) {
        wait = kbm_wait;
    }
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

    /* §6.15-§6.17, gated by §6.19. After INPUT_STATE, which is the session's
     * obligation, and before PING, which is only diagnostics. Runs even when
     * kbm is NULL: repeats and trailing copies already owed are still owed. */
    kbm_pump(c, kbm, now);

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

int apad_client_pump(apad_client *c, const apad_input_state *in, int max_wait_ms)
{
    return apad_client_pump_ex(c, in, NULL, max_wait_ms);
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
    out->touchmap_serial    = c->touchmap_serial;
    out->touchmap           = c->touchmap;
    out->inputcaps_serial   = c->inputcaps_serial;
    out->derive_ms          = c->derive_ms;
    out->inputcaps          = c->inputcaps;
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

        /* §6.20: "A sender MUST release before it stops sending ... and
         * before BYE." Ordered before the BYE for the obvious reason -- the
         * server tears the session down on the BYE, and a release arriving
         * after it has nothing left to act on. Sends nothing at all unless
         * this client actually holds something. */
        kbm_release_all(c, apad_ticks_ms());

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
