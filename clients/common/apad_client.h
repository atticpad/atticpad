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
 * malloc: used exactly once, in apad_client_create(), and never again. That
 * is the "no malloc AFTER init" rule from docs/CONVENTIONS.md satisfied, not waived:
 * create() is init. Since 2026-09 this file does run on the 4 MB, no-MMU
 * ARM9 the rule exists for -- the DS client links it -- and the one calloc
 * there is a few kilobytes taken once, after Wi-Fi association, before any
 * session. Anything that would allocate per session or per packet does not
 * belong in this file.
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

    /* §6.12 TOUCHMAP 0x43.
     * The layout the SERVER says this device's touchscreen maps to, so a
     * client can draw the truth instead of a compiled-in guess. Follows the
     * same serial convention as rumble/led/status: the serial changes when a
     * new one arrives, and the UI redraws off that rather than polling. Zero
     * serial means none has ever been received -- draw nothing, or whatever
     * the client drew before. */
    uint32_t      touchmap_serial;
    apad_touchmap touchmap;

    /* §6.19 INPUTCAPS 0x44 — which of §6.15–§6.17 this server accepts, and
     * what exists for the session right now. Same serial convention as
     * touchmap above, and the zero case carries the same meaning it does
     * everywhere else in this struct plus one more:
     *
     * inputcaps_serial == 0 means NO INPUTCAPS HAS EVER BEEN ACCEPTED, which
     * §6.19 makes the whole negotiation — "a client that has not received an
     * INPUTCAPS MUST NOT send KEYBOARD, MOUSE or MEDIA". A v1 server cannot
     * send this message and would discard those three anyway, so absence is
     * how it says no without knowing the question. HIDE THE KBM UI on zero;
     * showing a keyboard the server will silently drop is worse than showing
     * nothing. The engine enforces the send side of that gate on its own
     * (apad_client_pump_ex) — this field is for the UI.
     *
     * A reordered copy cannot revive stale contents: §6.20's fourth per-type
     * window is applied to every INPUTCAPS before it lands here. */
    uint32_t       inputcaps_serial;
    apad_inputcaps inputcaps;

    /* §10 pairing, all four for the UI and none of them for the protocol. */
    int32_t  pairing_required; /* §6.2 ANNOUNCE; -1 until one is seen        */
    int32_t  auth_required;    /* §6.4 WELCOME flags bit 0                   */
    int32_t  auth_state;       /* enum apad_client_auth                      */
    int32_t  error_code;       /* §6.11 ERROR code, 0 if none. NOT folded
                                * into status_code: STATUS codes are 0..2 and
                                * ERROR codes are 1..7, so one field cannot
                                * tell "warning" from "no free pad slot".    */
    uint32_t derive_ms;        /* wall time of the LAST §10 key derivation
                                * (PBKDF2, 10,000 iterations), 0 if none yet.
                                * A number the UI can show: on the DS's
                                * ARM9 this is seconds, and whether it fits
                                * inside the server's §11 3 s idle window is
                                * exactly what decides if pairing works.    */
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
 * 255.255.255.255 is therefore REFUSED with APAD_ERR_ARG rather than sent:
 * this socket has no SO_BROADCAST, and on the DS's lwIP such a send never
 * returns at all.
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

/*
 * §6.15–§6.17 — one pump's worth of keyboard, mouse and media, handed down
 * from the platform layer. Read and not retained.
 *
 * `have` is a bitmask of APAD_KBM_FEATURE_KEYBOARD / _MOUSE / _MEDIA (kbm.h)
 * naming which of the three sub-structs below are filled this call. The same
 * bit positions as INPUTCAPS.features on purpose: "what I am driving" and
 * "what the server accepts" are then one `&` apart. A facility absent from
 * `have` is left exactly as it was — its held state keeps being repeated at
 * §6.20's floor, which is what a receiver's watchdog needs; absence means
 * "no news", never "release everything".
 *
 * WHAT THE CALLER OWNS AND WHAT THE ENGINE OWNS. Getting this split wrong is
 * the one way to use this struct incorrectly, so it is spelled out:
 *
 *   Caller fills          Engine owns (whatever you put there is IGNORED)
 *   ------------------    ---------------------------------------------
 *   keyboard.keys[]       keyboard.event_seq, keyboard.client_ticks_ms
 *   mouse.buttons         mouse.event_seq,    mouse.client_ticks_ms
 *   mouse.*_accum         media.event_seq,    media.client_ticks_ms
 *   media.held
 *   *.events[]  (see below — a QUEUE here, a RING on the wire)
 *
 * `keys[]`, `buttons` and `held` are the CURRENT held state and are
 * authoritative: §6.20 makes the snapshot the authority and reconciles the
 * receiver against it on every single packet, so whatever you put here is
 * what the host ends up holding. They must describe the state AFTER any
 * events submitted in the same call.
 *
 * `mouse.dx_accum` and friends are FREE-RUNNING WRAPPING COUNTERS the caller
 * keeps for the life of the session — add this frame's motion and never
 * reset them. §6.16: the absolute value carries no meaning, a receiver only
 * ever diffs consecutive accepted samples, and the wrap is the point. Do not
 * put a per-frame delta here; a receiver would read it as a jump back to near
 * zero. +X right, +Y DOWN (screen space, §6.16), wheels in DETENTS.
 *
 * `events[]` IS A SUBMISSION QUEUE, NOT THE WIRE RING. Fill it from index 0,
 * oldest first, with the transitions that happened since the previous pump,
 * and terminate it with a zero code (`usage`/`button`/`control` == 0) or by
 * filling the array. It is read up to the first zero code, so memset the
 * struct before filling it. The engine assigns each submitted event its
 * §6.20 ordinal, places it in the RIGHT-ALIGNED ring it maintains itself,
 * and rolls `event_seq` — none of which platform code should be reproducing
 * (front-packing that ring is explicitly non-conformant, and it is a mistake
 * that costs one keystroke in a way nothing local can see).
 *
 * THE SUB-FRAME TAP IS EXACTLY WHY THIS QUEUE EXISTS. A key, button or
 * control that is pressed and released between two pumps leaves `keys[]` /
 * `buttons` / `held` byte-identical, so a state snapshot cannot express it at
 * all and the tap simply never happens on the host. Submit both transitions:
 *
 *     memset(&k, 0, sizeof k);
 *     k.have = APAD_KBM_FEATURE_KEYBOARD;
 *     k.keyboard.events[0].usage = APAD_HID_KEY_A;
 *     k.keyboard.events[0].flags = APAD_KBM_EVENT_DOWN;
 *     k.keyboard.events[1].usage = APAD_HID_KEY_A;
 *     k.keyboard.events[1].flags = 0;              // release
 *     // keyboard.keys[] stays all-zero: nothing is held afterwards
 *
 * Both reach the server in one datagram and it emits two events. A platform
 * that only ever samples state (a key grid polled once a frame) can leave
 * events[] empty: the engine diffs `keys[]` against its own shadow and
 * synthesises the transitions, which is correct for everything except a tap
 * shorter than the pump interval — the case the queue is for.
 *
 * A caller may submit at most one ring's worth per pump (8 keyboard, 4 mouse,
 * 4 media). More than that in one call cannot be represented on the wire by
 * anyone; pump more often.
 *
 * The engine does NOT normalise out-of-range event codes, deliberately —
 * §6.20 makes that receive-side, so that a sender's bug stays visible to the
 * one test suite positioned to catch it.
 */
typedef struct {
    uint8_t       have;      /* APAD_KBM_FEATURE_* bits */
    apad_keyboard keyboard;
    apad_mouse    mouse;
    apad_media    media;
} apad_client_kbm_in;

/*
 * apad_client_pump() plus §6.15–§6.19. `kbm` may be NULL, which is exactly
 * what apad_client_pump() passes — the two are one function and a client that
 * drives no keyboard, mouse or media puts nothing extra on the wire.
 *
 * What the engine does with it, so no platform has to (§6.19, §6.20):
 *
 *   - THE GATE. Nothing is emitted for a type until an INPUTCAPS has been
 *     accepted AND its `features` bit is set. This is §6.19's negotiation and
 *     it lives here so that no client can forget it and start talking to a
 *     v1 server that will only discard it.
 *   - THE RELEASE. When a `features` bit CLEARS, everything held for that
 *     facility is released — on the wire, before sending stops. Skipping it
 *     leaves a key held on the user's desktop with nothing able to lift it,
 *     which §6.20 calls the one failure the section exists to prevent. The
 *     same release runs before the BYE in apad_client_disconnect().
 *   - THE CADENCE. Keyboard and media on change; mouse at
 *     INPUTCAPS.mouse_rate_hz (or the session rate when 0) and never above
 *     §11's 125 Hz ceiling whatever that field asks for — it is a request,
 *     not an authorisation. Anything held repeats at better than §6.20's
 *     10 Hz floor, which is a MUST: a change-only sender is indistinguishable
 *     from a dead one and the receiver's 1000 ms watchdog would release the
 *     chord under the user's fingers. Three trailing copies about 50 ms apart
 *     once a facility goes idle, so the last release survives packet loss.
 *   - THE RING. Right-aligned, per §6.20, with ordinals assigned here.
 *
 * Returns the current enum apad_client_state, like apad_client_pump().
 */
int apad_client_pump_ex(apad_client *c, const apad_input_state *in,
                        const apad_client_kbm_in *kbm, int max_wait_ms);

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
