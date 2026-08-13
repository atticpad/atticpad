/* apad_client.h — the AtticPad client engine, in C. Shared by every client:
 * born in clients/android (M3), hoisted to clients/common once the 3DS client
 * and tools/loopback-client turned out to be re-implementing the same §8/§9
 * driving by hand — the M4 server-PING bug (fixed three times, once per copy)
 * is the failure mode this hoist removes.
 *
 * WHY THIS LAYER EXISTS AT ALL, rather than the same logic per platform:
 * docs/DESIGN.md §7.2 — "Do not reimplement the protocol in Kotlin." The 193
 * conformance vectors and the frozen v1 wire format only protect code that
 * goes through libapad, so everything between libapad and the platform —
 * handshake, retransmit, ACK-every-copy, PING answering, the pump — lives
 * here, once, and the platform layer never sees a packet.
 *
 * WHAT THIS IS NOT: it is not sans-IO. It owns a socket (through shim/, per
 * docs/DESIGN.md §7.2 — "Route sockets through shim/, not Kotlin"). It does NOT own
 * a thread, a lock, or a lifecycle: every function here must be called from
 * one thread, and which thread that is, is the host's business. That split is
 * deliberately the same one docs/DESIGN.md §6.4 draws for `libapadserver` — library
 * owns protocol, host owns the thread model — because the Android host's whole
 * reason for existing is that it has opinions about lifecycle (a foreground
 * Service) that a library must not preempt.
 *
 * malloc: used exactly once, in apad_client_create(). The no-malloc rule in
 * docs/CONVENTIONS.md scopes to core/ and shim/, which must run on a 4 MB ARM9 with no
 * MMU. This file is client code for a device with gigabytes, and it never
 * allocates again after create.
 */
#ifndef ATTICPAD_COMMON_APAD_CLIENT_H
#define ATTICPAD_COMMON_APAD_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#include "atticpad/atticpad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors enum apad_session_state, plus one value the FSM has no room for:
 * "the socket is open but connect() has not been called yet". */
enum apad_client_state {
    APAD_CLIENT_IDLE = 0,
    APAD_CLIENT_HANDSHAKING,
    APAD_CLIENT_ACTIVE,
    APAD_CLIENT_CLOSED
};

/*
 * §10.1 — the secret is an opaque byte string and its length depends on the
 * channel it arrived through: six digits when a human typed it, twenty-odd
 * characters when a camera did. A client MUST NOT assume six digits. 64 is
 * the ceiling §10.1 names; the +1 is the NUL, because apad_derive_session_key
 * takes a C string.
 *
 * KNOWN GAP, reported not worked around: core's apad_derive_session_key()
 * stops at 32 bytes of secret (core/src/hmac_sha256.c), so anything longer
 * derives the same key as its first 32 bytes. Today's server draws from
 * APAD_PAIRING_SECRET_MAX == 32 (NUL included) so nothing on this LAN can
 * reach the cut, and both sides truncate through the same function and stay
 * interoperable — but a 40-character token from some other server would
 * silently lose entropy here. Accepting the full 64 at this boundary is
 * deliberate: the limit belongs in core, and hiding it behind a shorter
 * client buffer would make it unfindable.
 */
#define APAD_CLIENT_SECRET_MAX 64

/* How far §10 has got on this session. Drives the UI's whole vocabulary:
 * "enter a PIN" and "wrong PIN" are different sentences from "handshake
 * failed", and only this distinguishes them. */
enum apad_client_auth {
    APAD_AUTH_NONE = 0,      /* no pairing involved; WELCOME had no flag     */
    APAD_AUTH_NEED_SECRET,   /* WELCOME asked and there was nothing to derive
                              * from — §8's "prompt at leisure, reconnect"   */
    APAD_AUTH_KEYED,         /* key derived and installed, nothing proven yet */
    APAD_AUTH_VERIFIED,      /* an inbound tag verified: the secret was right */
    APAD_AUTH_FAILED         /* ERROR code 3, or the session died unproven   */
};

/* Everything the UI wants to know, snapshotted. Filled by
 * apad_client_get_stats(); never points into the client. */
typedef struct {
    int32_t  state;            /* enum apad_client_state                     */
    int32_t  session_id;
    int32_t  pad_slot;
    int32_t  input_rate_hz;    /* the rate the SERVER asked for (§6.4)       */
    int32_t  rtt_ms;           /* -1 until the first PONG                    */
    int32_t  close_reason;     /* enum apad_session_close                    */
    uint32_t tx_packets;
    uint32_t rx_packets;
    int32_t  last_error;       /* the most recent negative apad_result       */

    /* Serials increment once per message received. The host polls them and
     * acts on a change, so a missed poll costs one buzz rather than
     * desynchronising a queue. */
    uint32_t rumble_serial;
    int32_t  rumble_low;
    int32_t  rumble_high;
    int32_t  rumble_duration_ms;
    uint32_t led_serial;
    int32_t  led_player;
    int32_t  led_rgb;          /* 0x00RRGGBB                                 */
    uint32_t status_serial;
    int32_t  status_code;

    /* §10 pairing, all four for the UI and none of them for the protocol. */
    int32_t  pairing_required; /* §6.2 ANNOUNCE; -1 until one is seen        */
    int32_t  auth_required;    /* §6.4 WELCOME flags bit 0                   */
    int32_t  auth_state;       /* enum apad_client_auth                      */
    int32_t  error_code;       /* §6.11 ERROR code, 0 if none. NOT folded
                                * into status_code: STATUS codes are 0..2 and
                                * ERROR codes are 1..7, so one field cannot
                                * tell "warning" from "no free pad slot".    */
} apad_client_stats;

typedef struct apad_client apad_client;

/* Allocates and opens a UDP socket. NULL on failure. `device_name` is copied
 * into the §6.3 fixed-width field; `caps` is an APAD_CAP_* bitmask. */
apad_client *apad_client_create(const char *device_name, uint32_t caps);

/*
 * §10 — hand the client the pairing secret the user carried over out of band.
 * Copied immediately; the caller's buffer is not retained and the client
 * never writes it anywhere but its own memory (§10: the secret MUST NEVER
 * appear on the wire, and storing it would be the same mistake one layer up).
 *
 * NULL or "" forgets the current secret. Length 1..APAD_CLIENT_SECRET_MAX;
 * §10.1 requires 6..64 to be accepted and forbids assuming six digits, and
 * this deliberately accepts shorter as well rather than second-guessing a
 * server whose pairing UI this client cannot see. Anything longer is
 * APAD_ERR_ARG, not a silent truncation — a truncated secret derives a wrong
 * key and presents as "wrong PIN" for a PIN that was right.
 *
 * Survives connect(): the point of §8's degradation path is to prompt once
 * and then reconnect with a fresh HELLO. Returns APAD_OK or APAD_ERR_ARG.
 */
int apad_client_set_secret(apad_client *c, const char *secret);

/*
 * §7 tier 2/3 — send one DISCOVER to `ip`:`port` and wait up to `timeout_ms`
 * for the ANNOUNCE, so that `pairing_required` (§6.2) is known BEFORE any
 * HELLO goes out. §8 is explicit that pairing happens before the handshake
 * and that `ANNOUNCE.pairing_required` is the signal to obtain a secret
 * first; without this a client only learns it from a WELCOME, by which time
 * it is holding a pad slot it is about to let lapse.
 *
 * Unicast, not broadcast: §6.1 allows "unicast to a manually entered
 * address", which is the tier-3 case, and a broadcast DISCOVER is the tier-2
 * job of the host's own discovery UI (Android does tier 1 through NSD).
 *
 * RESETS the session, so call it before connect(), never during one. Returns
 * APAD_OK when an ANNOUNCE arrived (read the answer from
 * apad_client_get_stats().pairing_required), or APAD_ERR_STATE on timeout —
 * which is the ordinary tier-3 outcome and not an error the user should see.
 */
int apad_client_probe(apad_client *c, const char *ip, uint16_t port,
                      int timeout_ms);

/*
 * Sends HELLO to `ip`:`port` and drives §8's handshake to WELCOME, including
 * the ACK that §9 requires. Blocking, bounded by `timeout_ms`. Returns
 * APAD_OK or a negative apad_result.
 *
 * APAD_ERR_AUTH specifically means "the WELCOME carried AUTH_REQUIRED and
 * this client had no secret to derive a key from". §8: a client in that
 * position MUST NOT stall inside the handshake waiting for a human — it
 * sends the ACK, lets the session lapse, prompts at leisure and reconnects.
 * By the time this returns the ACK is already on the wire and the local
 * session is closed; the caller's job is to obtain a secret, call
 * apad_client_set_secret() and call this again.
 */
int apad_client_connect(apad_client *c, const char *ip, uint16_t port,
                        uint16_t desired_rate_hz, int timeout_ms);

/*
 * One iteration of the session loop:
 *   - waits for inbound datagrams until the next INPUT_STATE is due
 *     (bounded by `max_wait_ms`), handling every one that arrives
 *   - runs §9 retransmit / §8 idle timers
 *   - sends INPUT_STATE at the negotiated rate, and PING once a second
 *
 * `in` is the caller's current input snapshot; it is read and not retained.
 * Returns the current enum apad_client_state, so a host loop can be
 * `while (apad_client_pump(...) == APAD_CLIENT_ACTIVE)`.
 */
int apad_client_pump(apad_client *c, const apad_input_state *in, int max_wait_ms);

void apad_client_get_stats(const apad_client *c, apad_client_stats *out);

/* Text of the most recent STATUS or ERROR. NUL-terminated, valid until the
 * next one arrives. Empty string if none. */
const char *apad_client_message(const apad_client *c);

/* Sends BYE and closes the session, but keeps the socket so the same client
 * can connect() again. */
void apad_client_disconnect(apad_client *c);

/* Closes the socket and frees. Safe on NULL. */
void apad_client_destroy(apad_client *c);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_COMMON_APAD_CLIENT_H */
