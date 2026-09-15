/* atticpad/atticpad.h — libapad public API.
 *
 * The C99 core shared by every AtticPad client and by the server: codec,
 * session FSM, HMAC, self-test. Nothing here allocates, uses floating point,
 * stdio, time.h or locale. Everything the OS provides arrives through the
 * shim declared at the bottom of this file.
 *
 * docs/PROTOCOL.md is normative. Section references below point into it.
 */
#ifndef ATTICPAD_ATTICPAD_H
#define ATTICPAD_ATTICPAD_H

#include <stdint.h>
#include <stddef.h>

#include "atticpad/protocol.h"
#include "atticpad/input.h"
#include "atticpad/kbm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Result codes                                                             */
/* ======================================================================== */

/* Encode/decode functions return a non-negative byte count on success and
 * one of these on failure. Never return a bare -1. */
enum apad_result {
    APAD_OK              = 0,
    APAD_ERR_ARG         = -1,  /* NULL pointer or nonsense argument        */
    APAD_ERR_BUFFER      = -2,  /* caller's buffer too small to write into  */
    APAD_ERR_TRUNCATED   = -3,  /* datagram shorter than the fixed header   */
    APAD_ERR_MAGIC       = -4,  /* §3 magic mismatch                        */
    APAD_ERR_VERSION     = -5,  /* §3/§12 major mismatch: hard reject       */
    APAD_ERR_TYPE        = -6,  /* §4 unknown type: discard SILENTLY        */
    APAD_ERR_LENGTH      = -7,  /* §3 length/payload_len disagreement       */
    APAD_ERR_AUTH        = -8,  /* §10 tag missing or wrong                 */
    APAD_ERR_STATE       = -9,  /* §8 operation invalid in this FSM state   */
    APAD_ERR_STALE       = -10  /* §9 older than the newest already seen    */
};

/* ======================================================================== */
/* §9 wrap-safe arithmetic                                                   */
/* ======================================================================== */
/*
 * Every sequence and tick comparison in every implementation MUST route
 * through these. A naive `a > b` passes every test shorter than nine minutes
 * and then freezes the client forever.
 *
 * These are real functions, not static inlines, so that there is exactly one
 * copy of the logic linked into all eight platforms and so `nm` can prove it
 * is there. The cost is one call per comparison; at 125 Hz on a 67 MHz ARM9
 * that is not measurable.
 */

/* true if a is strictly newer than b, correct across the 0xFFFF wrap. */
int      apad_seq_newer(uint16_t a, uint16_t b);
/* Signed distance a - b in [-32768, 32767]. Negative means a is older. */
int      apad_seq_diff(uint16_t a, uint16_t b);
/* Next sequence number. Present so callers never write `seq + 1` by hand. */
uint16_t apad_seq_next(uint16_t s);

/*
 * §6.20 step 2 — how many trailing slots of an event ring to replay.
 *
 * `event_seq` is the ordinal the datagram carries, `last_seq` the highest
 * this receiver has already acted on for that type, `ring_len` the ring's
 * depth (APAD_KEYBOARD_RING_DEPTH, or APAD_MOUSE_/APAD_MEDIA_RING_DEPTH).
 * Returns 0..ring_len; replay runs from slot (ring_len - result) to slot
 * (ring_len - 1) ASCENDING, skipping "no event" slots. `out_overflow` may be
 * NULL; it is set when the gap exceeded ring_len, i.e. events were lost that
 * the ring cannot carry, which §6.20 requires be surfaced rather than
 * swallowed.
 *
 * IT NEVER RETURNS A NEGATIVE OR AN ERROR, AND THAT IS A DELIBERATE API
 * DECISION. §6.20 step 3 requires the caller to reconcile held state against
 * the snapshot UNCONDITIONALLY — including when nothing is new, and including
 * on overflow — because that is the whole convergence argument: it makes
 * every divergence from a non-conforming sender last exactly one packet
 * instead of forever. A function that returned APAD_ERR_STALE here would
 * invite `if (n < 0) return;`, which skips the reconcile and reintroduces
 * precisely the permanent-divergence bug. So a gap of zero or less returns 0,
 * meaning "replay nothing" — and the caller still reconciles.
 *
 * Do NOT call this for the FIRST accepted message of a type. §6.20 step 1
 * makes that case set last_applied = event_seq, apply NO events and apply the
 * snapshot: the ring's contents predate the receiver's interest in them.
 */
int      apad_event_ring_new(uint16_t event_seq, uint16_t last_seq,
                             int ring_len, int *out_overflow);

/* true if a is strictly after b, correct across the 2^32 wrap (~49.7 days). */
int      apad_time_after(uint32_t a, uint32_t b);
/* Elapsed milliseconds from `then` to `now`, wrap-safe. */
uint32_t apad_time_since(uint32_t now, uint32_t then);
/* true once `now` has reached `deadline` (i.e. not before it). */
int      apad_time_reached(uint32_t now, uint32_t deadline);

/* ======================================================================== */
/* §3 header and framing                                                     */
/* ======================================================================== */

typedef struct {
    uint16_t magic;       /* APAD_MAGIC */
    uint8_t  version;     /* APAD_VERSION */
    uint8_t  type;        /* enum apad_msg_type */
    uint16_t session_id;
    uint16_t sequence;
    uint16_t payload_len;
    uint16_t flags;       /* APAD_FLAG_*; reserved bits are masked off */
} apad_header;

/* A parsed datagram. `payload` and `tag` point INTO the caller's buffer;
 * they are never copied and never cast to a struct. */
typedef struct {
    apad_header    header;
    const uint8_t *payload;      /* NULL when payload_len == 0 */
    uint16_t       payload_len;
    const uint8_t *tag;          /* NULL unless APAD_FLAG_AUTHENTICATED */
} apad_packet;

/* Writes APAD_HEADER_SIZE bytes. Reserved flag bits are forced to zero (§2).
 * Returns APAD_HEADER_SIZE or a negative apad_result. */
int apad_header_encode(uint8_t *buf, size_t cap, const apad_header *hdr);

/* Reads APAD_HEADER_SIZE bytes. Validates magic and version. Reserved flag
 * bits are masked off rather than rejected (§2). Does NOT validate type or
 * length; apad_packet_parse does that. */
int apad_header_decode(const uint8_t *buf, size_t len, apad_header *out);

/* Expected payload size for a type (§4), or APAD_ERR_TYPE if unknown. */
int apad_payload_size(uint8_t type);

/*
 * Build a complete datagram into buf.
 *
 * hdr->payload_len and the AUTHENTICATED flag are set by this function from
 * payload_len and key; the caller supplies type, session_id, sequence and the
 * RELIABLE flag. Pass key == NULL for an unauthenticated datagram.
 *
 * Returns the total datagram length (header + payload + tag) or a negative
 * apad_result.
 */
int apad_packet_build(uint8_t *buf, size_t cap,
                      const apad_header *hdr,
                      const void *payload, uint16_t payload_len,
                      const uint8_t *key, size_t key_len);

/*
 * Parse a received datagram. Enforces §3:
 *   len == 12 + payload_len + (AUTHENTICATED ? 8 : 0)
 * and the magic/version checks. Unknown types return APAD_ERR_TYPE so the
 * caller can discard silently (§4); known types with the wrong payload_len
 * return APAD_ERR_LENGTH.
 *
 * Does NOT verify the tag — call apad_packet_verify for that, which needs the
 * session key the caller has and the codec does not.
 */
int apad_packet_parse(const uint8_t *buf, size_t len, apad_packet *out);

/* §10 — constant-time verify of the 8-byte truncated HMAC-SHA256 tag over
 * the whole datagram with the tag region zeroed. Returns APAD_OK or
 * APAD_ERR_AUTH. `len` is the full datagram length including the tag. */
int apad_packet_verify(const uint8_t *buf, size_t len,
                       const uint8_t *key, size_t key_len);

/* ======================================================================== */
/* §2 text fields                                                            */
/* ======================================================================== */
/* UTF-8, NUL-padded to a fixed width, not necessarily NUL-terminated. */

/* Copy a C string into a fixed-width field, NUL-padding the remainder.
 * When `src` is longer than the field, §2 forbids splitting a multi-byte
 * UTF-8 sequence across the end: a sequence that would straddle the
 * boundary is dropped whole and the remainder NUL-padded, so the field may
 * hold up to three fewer bytes than `width`. Already-malformed input is
 * copied as given — this bounds the field, it does not validate UTF-8. */
void   apad_text_set(char *field, size_t width, const char *src);
/* Length of a fixed-width text field, bounded by the width. */
size_t apad_text_len(const char *field, size_t width);
/* Copy a fixed-width field out as a NUL-terminated C string. */
void   apad_text_get(char *dst, size_t dst_cap, const char *field, size_t width);

/* ======================================================================== */
/* §5, §6 payloads                                                           */
/* ======================================================================== */

typedef struct {                       /* §6.2 ANNOUNCE, 40 bytes */
    char     server_name[APAD_NAME_LEN];
    uint8_t  pads_total;
    uint8_t  pads_free;
    uint8_t  pairing_required;         /* 0 or 1; non-zero decodes as 1 (§6.2) */
    uint16_t server_port;
} apad_announce;

typedef struct {                       /* §6.3 HELLO, 76 bytes */
    uint8_t  client_id[APAD_CLIENT_ID_LEN];
    uint32_t caps;                     /* APAD_CAP_* */
    char     device_name[APAD_NAME_LEN];
    uint8_t  client_nonce[APAD_NONCE_LEN];
    uint16_t desired_rate_hz;
    uint8_t  proto_major;              /* MUST equal the header version */
    uint32_t client_ticks_ms;
} apad_hello;

typedef struct {                       /* §6.4 WELCOME, 60 bytes */
    uint16_t session_id;               /* non-zero */
    uint8_t  pad_slot;
    uint8_t  flags;                    /* APAD_WELCOME_AUTH_REQUIRED */
    uint16_t input_rate_hz;
    uint8_t  server_nonce[APAD_NONCE_LEN];        /* PBKDF2 salt (§10) */
    uint8_t  key_material[APAD_KEY_MATERIAL_LEN]; /* MUST be zero in v1 */
    uint32_t server_ticks_ms;
} apad_welcome;

typedef struct {                       /* §6.5 BYE, 4 bytes */
    uint8_t reason;                    /* enum apad_bye_reason */
} apad_bye;

typedef struct {                       /* §6.6 PING / PONG, 8 bytes */
    uint32_t origin_ticks_ms;          /* echoed unchanged in PONG */
    uint32_t responder_ticks_ms;       /* 0 in PING */
} apad_ping;

typedef struct {                       /* §6.7 RUMBLE, 8 bytes */
    uint16_t low_freq;
    uint16_t high_freq;
    uint16_t duration_ms;              /* 0 = until superseded */
} apad_rumble;

/* TOUCHMAP 0x43 -- docs/PROTOCOL.md §6.12, added after the v1 freeze under
 * the §6.14 additivity ruling.
 *
 * Lets the server tell a client what its touchscreen currently maps to, so a
 * client can DRAW the real layout instead of a hardcoded guess. The 3DS
 * bottom screen presently draws "3ds-default" and an LT/RT split from
 * constants in screen_session.c, and has to disclaim that the server may
 * disagree -- because it genuinely cannot know.
 *
 * Deliberately carries no text. `pad_bit` is the canonical §5.7 button
 * bitmask and the client already owns button labels (3DS ui_widgets.c has
 * the table it draws A/B/X/Y/L/R/ZL/ZR from), so a Vita can render "L1"
 * where a 3DS renders "L" from the identical packet -- and no encoding,
 * truncation or localisation question ever reaches the wire.
 *
 * Rect is normalised 0..1 in eighths-of-a-percent (uint8/255), +Y down to
 * match §5.3's touch convention. On a 320x240 screen one step is 1.25 px
 * horizontally and 0.94 px vertically -- far finer than a fingertip.
 */
typedef struct {
    uint8_t  x0, y0, x1, y1;           /* normalised 0..255, +Y down     */
    uint8_t  target;                   /* 0 button, 1 LT, 2 RT           */
    uint8_t  analog;                   /* 1 = depth-into-region is analog */
    uint16_t pad_bit;                  /* §5.7 mask, when target == 0    */
} apad_touch_region_wire;

typedef struct {                       /* v2 TOUCHMAP, 68 bytes */
    uint8_t                mode;       /* 0 none, 1 regions, 2 delta, 3 absolute */
    uint8_t                region_count;
    apad_touch_region_wire regions[APAD_TOUCHMAP_MAX_REGIONS];
} apad_touchmap;

typedef struct {                       /* §6.8 LED, 4 bytes */
    uint8_t player_index;              /* 1..4, 0 = off */
    uint8_t r, g, b;
} apad_led;

typedef struct {                       /* §6.9 STATUS, 64 bytes */
    uint8_t code;                      /* enum apad_status_code */
    char    text[APAD_TEXT_LEN];
} apad_status;

typedef struct {                       /* §6.10 ACK, 4 bytes */
    uint16_t sequence;                 /* the sequence being acknowledged */
} apad_ack;

typedef struct {                       /* §6.11 ERROR, 64 bytes */
    uint16_t code;                     /* enum apad_error_code */
    char     text[APAD_TEXT_LEN];
} apad_error;

/*
 * Every encoder writes exactly APAD_LEN_<TYPE> bytes, zeroing every reserved
 * byte and masking every reserved bit (§2). Returns the byte count or a
 * negative apad_result.
 *
 * Every decoder requires len == APAD_LEN_<TYPE> exactly and ignores reserved
 * fields rather than rejecting them (§2): reserved bits arrive as zero in the
 * decoded struct. Returns the byte count consumed or a negative apad_result.
 *
 * §6.0 splits the out-of-range rules in two, and the split is deliberate:
 *   - normalised on decode, because a consumer acts on them —
 *     ANNOUNCE.pairing_required (non-zero -> 1, §6.2), LED.player_index
 *     (>4 -> 0, §6.8), and INPUT_STATE.battery (101..254 -> 255, §5.5).
 *   - preserved verbatim, because they are diagnostic labels — BYE.reason,
 *     STATUS.code and ERROR.code. An unrecognised value MUST survive decode
 *     and MUST NOT cause a reject; a v1.1 peer may add codes and clamping one
 *     would destroy the only information the message carries.
 * battery and player_index are normalised on DECODE ONLY: they have a
 * reserved numeric range, so a caller passing 150 or 7 has a bug that should
 * stay visible on the wire rather than be laundered into "unknown"/"off".
 * pairing_required is different and apad_encode_announce canonicalises it too
 * — it is a boolean carried in a uint8_t, where `x = flags & 0x80` is
 * idiomatic rather than wrong, and §6.2 defines the field as 0 or 1.
 *
 * DISCOVER (§6.1) has no payload and therefore no codec pair.
 */
int apad_encode_announce   (uint8_t *b, size_t cap, const apad_announce   *in);
int apad_decode_announce   (const uint8_t *b, size_t len, apad_announce   *out);
int apad_encode_hello      (uint8_t *b, size_t cap, const apad_hello      *in);
int apad_decode_hello      (const uint8_t *b, size_t len, apad_hello      *out);
int apad_encode_welcome    (uint8_t *b, size_t cap, const apad_welcome    *in);
int apad_decode_welcome    (const uint8_t *b, size_t len, apad_welcome    *out);
int apad_encode_bye        (uint8_t *b, size_t cap, const apad_bye        *in);
int apad_decode_bye        (const uint8_t *b, size_t len, apad_bye        *out);
int apad_encode_ping       (uint8_t *b, size_t cap, const apad_ping       *in);
int apad_decode_ping       (const uint8_t *b, size_t len, apad_ping       *out);
/* §6.12 */
int apad_encode_touchmap   (uint8_t *b, size_t cap, const apad_touchmap   *in);
int apad_decode_touchmap   (const uint8_t *b, size_t len, apad_touchmap   *out);

int apad_encode_rumble     (uint8_t *b, size_t cap, const apad_rumble     *in);
int apad_decode_rumble     (const uint8_t *b, size_t len, apad_rumble     *out);
int apad_encode_led        (uint8_t *b, size_t cap, const apad_led        *in);
int apad_decode_led        (const uint8_t *b, size_t len, apad_led        *out);
int apad_encode_status     (uint8_t *b, size_t cap, const apad_status     *in);
int apad_decode_status     (const uint8_t *b, size_t len, apad_status     *out);
int apad_encode_ack        (uint8_t *b, size_t cap, const apad_ack        *in);
int apad_decode_ack        (const uint8_t *b, size_t len, apad_ack        *out);
int apad_encode_error      (uint8_t *b, size_t cap, const apad_error      *in);
int apad_decode_error      (const uint8_t *b, size_t len, apad_error      *out);

/*
 * §5 INPUT_STATE. The decoder applies the receive-side normalisations the
 * spec mandates, so no caller has to remember them:
 *   - touch_count > 2 clamped to 2
 *   - touch entries at index >= touch_count zeroed
 *   - axes[4] / axes[5] negative treated as 0
 *   - axes[6] / axes[7] and buttons bits 20..31 zeroed (reserved)
 *   - battery 101..254 normalised to 255 / APAD_BATTERY_UNKNOWN (§5.5)
 * The encoder applies the same normalisations so that reserved bytes and bits
 * are zero on send — except the §5.5 battery rule, which is receive-side only:
 * a sender putting a reserved value on the wire is non-conforming (§2), and
 * the encoder does not hide that by rewriting it.
 */
int apad_encode_input_state(uint8_t *b, size_t cap, const apad_input_state *in);
int apad_decode_input_state(const uint8_t *b, size_t len, apad_input_state *out);

/*
 * §6.15-§6.19 KEYBOARD, MOUSE, MEDIA and INPUTCAPS. The structs are in
 * atticpad/kbm.h; the same return convention and the same reserved-field
 * discipline as every pair above.
 *
 * The decoders apply, and the encoders do NOT, these value normalisations —
 * the §6.0 split again, and for the reason battery and player_index are
 * decode-only: each is a reserved NUMERIC RANGE, so a caller that puts one on
 * the wire has a bug that should stay visible rather than be laundered.
 *   - KEYBOARD: an events[] slot whose usage is 0..3 decodes as usage 0 AND
 *     flags 0 (§6.15)
 *   - MOUSE: a button outside 1..5 decodes as button 0 AND flags 0 (§6.16)
 *   - MEDIA: a control outside §6.18's assigned 1..24 decodes as control 0
 *     AND flags 0 (§6.17)
 * "AND flags 0" is not incidental: it is what makes a "no event" slot decode
 * byte-identically whatever a non-conforming sender transmitted, which is
 * what lets a conformance vector compare two decoded structures for exact
 * equality (§6.15).
 *
 * Reserved BITS are masked on both sides as everywhere else (§2): keys[0]
 * bits 0-3, event flags bits 1-7, MOUSE.buttons bits 5-15, MEDIA.held and
 * INPUTCAPS.media_mask bits 24-31, features bits 3-31, status bits 4-31.
 *
 * What a decoder deliberately does NOT touch: HID usages 0xA5-0xAF,
 * 0xDE-0xDF and 0xE8-0xFF, which HID 1.12 leaves reserved. §6.15 requires
 * them passed through unchanged so a frozen decoder does not hard-code one
 * revision of the HID specification.
 */
int apad_encode_keyboard   (uint8_t *b, size_t cap, const apad_keyboard   *in);
int apad_decode_keyboard   (const uint8_t *b, size_t len, apad_keyboard   *out);
int apad_encode_mouse      (uint8_t *b, size_t cap, const apad_mouse      *in);
int apad_decode_mouse      (const uint8_t *b, size_t len, apad_mouse      *out);
int apad_encode_media      (uint8_t *b, size_t cap, const apad_media      *in);
int apad_decode_media      (const uint8_t *b, size_t len, apad_media      *out);
int apad_encode_inputcaps  (uint8_t *b, size_t cap, const apad_inputcaps  *in);
int apad_decode_inputcaps  (const uint8_t *b, size_t len, apad_inputcaps  *out);

/* ======================================================================== */
/* §10 SHA-256 / HMAC / PBKDF2                                               */
/* ======================================================================== */

typedef struct {
    uint32_t state[8];
    uint64_t total;      /* bytes absorbed */
    uint8_t  block[64];
    uint8_t  used;
} apad_sha256_ctx;

typedef struct {
    apad_sha256_ctx inner;
    uint8_t         opad[64];
} apad_hmac_ctx;

#define APAD_SHA256_DIGEST_LEN 32
#define APAD_SHA256_BLOCK_LEN  64

void apad_sha256_init(apad_sha256_ctx *c);
void apad_sha256_update(apad_sha256_ctx *c, const void *data, size_t len);
void apad_sha256_final(apad_sha256_ctx *c, uint8_t out[APAD_SHA256_DIGEST_LEN]);
void apad_sha256(const void *data, size_t len, uint8_t out[APAD_SHA256_DIGEST_LEN]);

void apad_hmac_sha256_init(apad_hmac_ctx *c, const uint8_t *key, size_t key_len);
void apad_hmac_sha256_update(apad_hmac_ctx *c, const void *data, size_t len);
void apad_hmac_sha256_final(apad_hmac_ctx *c, uint8_t out[APAD_SHA256_DIGEST_LEN]);
void apad_hmac_sha256(const uint8_t *key, size_t key_len,
                      const void *data, size_t len,
                      uint8_t out[APAD_SHA256_DIGEST_LEN]);

/* §10 — PBKDF2-HMAC-SHA256. `iterations` must be >= 1. No allocation: the
 * whole derivation runs in a fixed frame — about 700 bytes for the driver
 * (two saved SHA-256 contexts plus a working copy), ~1.2 KB peak including
 * the SHA-256 message schedule. */
void apad_pbkdf2_sha256(const uint8_t *pw, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t *out, size_t out_len);

/* §10 — derive the session key from the shared secret and the server_nonce
 * carried in WELCOME. The secret never appears on the wire.
 *
 * `pin` is a NUL-terminated string; the name is historical, the value need
 * not be digits. §10.1 requires ANY length from APAD_SECRET_MIN_LEN to
 * APAD_SECRET_MAX_LEN (6..64 bytes) to be accepted and used IN FULL: a typed
 * 6-digit PIN and a scanned 16+ character token are both conforming and
 * derive identically. §10.1 also forbids an embedded NUL, which is what
 * makes a C string a sufficient carrier.
 *
 * OVER-LENGTH BEHAVIOUR, STATED BECAUSE IT CANNOT BE SIGNALLED: a secret
 * longer than APAD_SECRET_MAX_LEN is TRUNCATED to its first
 * APAD_SECRET_MAX_LEN bytes and the remainder is ignored silently. This
 * function returns void, so it cannot reject, and §10.1 makes such a secret
 * non-conforming input in the first place — there is no correct key to
 * derive from it. The consequence a caller must plan for is that two
 * over-length secrets sharing their first APAD_SECRET_MAX_LEN bytes derive
 * the SAME session key, with no error anywhere. A caller that GENERATES or
 * ACCEPTS secrets is therefore the only layer that can enforce the bound,
 * and MUST do so; this function cannot do it on the caller's behalf.
 */
void apad_derive_session_key(const char *pin,
                             const uint8_t server_nonce[APAD_NONCE_LEN],
                             uint8_t out[APAD_SESSION_KEY_LEN]);

/* Constant-time compare. Returns 0 iff equal. Runtime depends on n only. */
int apad_ct_equal(const void *a, const void *b, size_t n);

/* Zero a buffer without the compiler eliding the store. */
void apad_secure_zero(void *p, size_t n);

/* ======================================================================== */
/* §8, §9 session state machine                                              */
/* ======================================================================== */

enum apad_session_state {
    APAD_SESSION_IDLE = 0,   /* nothing sent yet                            */
    APAD_SESSION_DISCOVERING,/* DISCOVER sent, waiting for ANNOUNCE          */
    APAD_SESSION_HANDSHAKING,/* HELLO sent, waiting for WELCOME              */
    APAD_SESSION_ACTIVE,     /* WELCOME received: session_id is valid        */
    APAD_SESSION_CLOSED      /* BYE, timeout, or reliable delivery failure   */
};

/* What apad_session_tick is telling the caller to do next. */
enum apad_session_action {
    APAD_ACT_NONE = 0,
    APAD_ACT_RETRANSMIT,     /* re-send the armed reliable message (§9)      */
    APAD_ACT_KEEPALIVE,      /* §8: at least one packet every 100 ms         */
    APAD_ACT_TIMEOUT         /* §8/§11: 3 s idle, or retransmits exhausted   */
};

/* Why the session closed, for the UI. */
enum apad_session_close {
    APAD_CLOSE_NONE = 0,
    APAD_CLOSE_LOCAL,        /* caller asked                                */
    APAD_CLOSE_PEER_BYE,     /* peer sent BYE                               */
    APAD_CLOSE_IDLE_TIMEOUT, /* §11 3000 ms with nothing received           */
    APAD_CLOSE_RETX_FAILED,  /* §9 100/200/400/800 exhausted                */
    APAD_CLOSE_PEER_ERROR    /* peer declared the session dead (ERROR 7) —
                              * appended after the 2026-08-11 3DS hardware
                              * find; NOT wire-visible, safe to extend      */
};

/*
 * §6.20 — the receive windows for KEYBOARD, MOUSE, MEDIA and INPUTCAPS are
 * INDEPENDENT of one another and of §9's INPUT_STATE window, so each needs
 * its own slot. This enum indexes rx_kbm_seq[] / rx_kbm_valid[] below and is
 * the `cls` argument of apad_session_accept_kbm.
 *
 * ALL FOUR OF §6.20'S WINDOWS LIVE HERE, INCLUDING THE SERVER->CLIENT ONE.
 * INPUTCAPS was left out when the first three landed, on the reasoning that
 * core windows only what core routes and the server never receives an
 * INPUTCAPS. That produced the wrong outcome: §6.20 states ONE rule for four
 * types, nothing in the tree implemented the fourth, and a reordered
 * INPUTCAPS would have revived stale `features`/`status` — which §6.19's
 * "latest wins" forbids, and whose observable cost is a cleared feature bit
 * coming back and a client resuming a type the server has stopped accepting.
 * One rule, one implementation (the argument §9 makes for its own helpers);
 * a client passes APAD_KBM_CLASS_INPUTCAPS and gets the identical window.
 *
 * A SESSION USES EITHER THE FIRST THREE SLOTS OR THE FOURTH, NEVER BOTH: the
 * three client->server types can only be received by a server, the one
 * server->client type only by a client. Three bytes of the four-slot array
 * are therefore always idle on any given peer. That is deliberate — the cost
 * is measured (see the size note on apad_session below) and it buys a single
 * shared implementation instead of a private copy of the same window living
 * in client code, out of reach of anything core can test.
 *
 * Appending a value is safe: this enum is NOT wire-visible (it indexes
 * internal arrays and nothing else), so it does not touch the v1 freeze —
 * the same precedent as APAD_CLOSE_PEER_ERROR above. Do NOT renumber the
 * first three.
 */
enum apad_kbm_class {
    APAD_KBM_CLASS_KEYBOARD = 0,
    APAD_KBM_CLASS_MOUSE,
    APAD_KBM_CLASS_MEDIA,
    APAD_KBM_CLASS_INPUTCAPS,   /* §6.19, server->client: the client's slot */
    APAD_KBM_CLASS_COUNT
};

/*
 * One session. Caller-owned, zero allocation, ~80 bytes. The server keeps an
 * array of APAD_MAX_SESSIONS of these; a client keeps one.
 *
 * Treat every field as read-only from outside this header.
 */
typedef struct {
    uint8_t  state;            /* enum apad_session_state                   */
    uint8_t  is_server;        /* 1 on the server side                      */
    uint8_t  close_reason;     /* enum apad_session_close                   */
    uint8_t  authenticated;    /* tag every outgoing packet (§10)           */

    uint16_t session_id;
    uint16_t tx_seq;           /* sequence for the NEXT outgoing packet     */

    uint16_t rx_input_seq;     /* newest INPUT_STATE sequence accepted      */
    uint8_t  rx_input_valid;
    uint8_t  pad_slot;

    /* §6.20 per-type receive windows, one per enum apad_kbm_class — four of
     * them, the fourth being the client's INPUTCAPS slot. These MUST NOT
     * share rx_input_seq above: a KEYBOARD at sequence 101 would advance a
     * shared window past an INPUT_STATE still in flight at 100, and live stick
     * data would be discarded as stale when it was merely interleaved. The
     * failure is silent and looks exactly like packet loss. */
    uint16_t rx_kbm_seq[APAD_KBM_CLASS_COUNT];
    uint8_t  rx_kbm_valid[APAD_KBM_CLASS_COUNT];

    uint16_t input_rate_hz;
    uint16_t peer_rate_hz;

    uint32_t last_rx_ms;
    uint32_t last_tx_ms;

    /* §9 reliable delivery — one message in flight at a time. */
    uint8_t  retx_armed;
    uint8_t  retx_attempt;     /* 0..APAD_RETX_COUNT                        */
    uint8_t  retx_type;
    uint16_t retx_seq;
    uint32_t retx_due_ms;

    uint8_t  key[APAD_SESSION_KEY_LEN];
    uint8_t  have_key;
} apad_session;

/* Initialise. `now` is apad_ticks_ms(). Never fails. */
void apad_session_init(apad_session *s, int is_server, uint32_t now);

/* Fill a header for the next outgoing packet of this type and consume a
 * sequence number. Sets RELIABLE and AUTHENTICATED per §4 and §10. Returns
 * APAD_OK or a negative apad_result. */
int apad_session_next_header(apad_session *s, uint8_t type, apad_header *out);

/* Record that a datagram of `len` bytes went out at `now`, and arm the
 * retransmit timer if the header had RELIABLE set (§9). INPUT_STATE is never
 * armed even if a caller sets the bit — §9 forbids retransmitting it. */
void apad_session_on_sent(apad_session *s, const apad_header *hdr, uint32_t now);

/*
 * Feed a parsed inbound packet in. Applies §8 and §9:
 *   - refreshes the idle timer
 *   - ACK clears the armed reliable message
 *   - INPUT_STATE older than the newest already seen returns APAD_ERR_STALE
 *   - WELCOME moves HANDSHAKING -> ACTIVE and adopts session_id/slot/rate
 *   - BYE closes the session
 * Returns APAD_OK, APAD_ERR_STALE (drop it), or a negative apad_result.
 */
int apad_session_on_recv(apad_session *s, const apad_packet *pkt, uint32_t now);

/*
 * Server side of the handshake: assign this session's own session_id and
 * pad_slot and move it to ACTIVE. The client side ADOPTS both from a
 * received WELCOME (apad_session_on_recv); the server INVENTS them, so it
 * has nothing to adopt and needs this instead of writing the struct.
 *
 *   session_id     MUST be non-zero (§6.4, §8). The caller owns the
 *                  numbering; it is only ever compared for equality.
 *   pad_slot       whatever the server's slot allocator returned.
 *   input_rate_hz  the rate the server WANTS (§6.4). 0 means
 *                  APAD_DEFAULT_RATE_HZ; above APAD_MAX_RATE_HZ is
 *                  APAD_ERR_ARG rather than a silent clamp, so a client
 *                  asking for a nonsense rate stays visible to the caller.
 *   out_welcome    optional. When non-NULL it is zeroed and prefilled with
 *                  session_id, pad_slot, input_rate_hz and
 *                  server_ticks_ms = now, so the WELCOME on the wire cannot
 *                  drift from the session that issued it. flags and
 *                  server_nonce are left zero: those are pairing decisions.
 *
 * Requires the session to be HANDSHAKING (a HELLO arrived) or already
 * ACTIVE (a retransmitted HELLO re-accepts idempotently); anything else is
 * APAD_ERR_STATE, as is calling it on a client-side session. Also clears the
 * §9 receive window, so a reused pad slot does not reject the new client's
 * input as stale.
 *
 * Call it BEFORE apad_session_next_header(APAD_MSG_WELCOME) — that is where
 * the header picks up the non-zero session_id (§8).
 *
 * Returns APAD_OK or a negative apad_result.
 */
int apad_session_server_accept(apad_session *s,
                               uint16_t session_id,
                               uint8_t pad_slot,
                               uint16_t input_rate_hz,
                               uint32_t now,
                               apad_welcome *out_welcome);

/* Drive timers. Returns an enum apad_session_action. Call it every loop. */
int apad_session_tick(apad_session *s, uint32_t now);

/* §9 receive window, exposed for the server's per-pad fast path. */
int apad_session_accept_input(apad_session *s, uint16_t seq);

/*
 * §6.20 receive window for one of KEYBOARD / MOUSE / MEDIA / INPUTCAPS.
 * `cls` is an enum apad_kbm_class. Returns APAD_OK to accept, APAD_ERR_STALE
 * to discard the datagram, or APAD_ERR_ARG.
 *
 * apad_session_on_recv calls this itself for the three CLIENT->SERVER types,
 * which are the only ones it can route: it is shared by both peers, and a
 * client's INPUTCAPS window has to be judged at the point the client acts on
 * the caps, so routing it here as well would apply the window twice and
 * silently discard every INPUTCAPS after the first. A client therefore calls
 * this directly with APAD_KBM_CLASS_INPUTCAPS, which is also why the function
 * is public — the same reason apad_session_accept_input is.
 *
 * A discard here matters beyond dropping the packet: §6.16 requires that a
 * stale MOUSE MUST NOT advance the motion baseline, or a reordered packet
 * becomes a backwards jerk followed by a compensating forward one; and a
 * stale INPUTCAPS would revive `features`/`status` the server has already
 * moved on from (§6.19 "latest wins").
 */
int apad_session_accept_kbm(apad_session *s, int cls, uint16_t seq);

/* Install the PBKDF2-derived session key (§10). */
void apad_session_set_key(apad_session *s, const uint8_t key[APAD_SESSION_KEY_LEN]);

/* Close locally; wipes the key. */
void apad_session_close(apad_session *s, int reason);

/* ======================================================================== */
/* §13 conformance self-test                                                 */
/* ======================================================================== */
/*
 * Runs on-device behind the hidden L+R+Start screen. Uses no stdio: results
 * come back through the callback and the summary struct, and the caller
 * prints them with whatever it has.
 *
 * The golden packets in core/testdata/vectors.h run as part of this, with no
 * build flag. They are authored independently from this codec (docs/DESIGN.md §9.1)
 * and are the part that actually catches an endianness or alignment bug; the
 * codec's own invariant checks are the weaker of the two, and a conformance
 * suite that has to be switched on is a conformance suite that six untestable
 * platforms ship without.
 *
 * `name` is valid only for the duration of the callback — the vector cases
 * compose their names into a scratch buffer. Copy it if you need it later.
 * `first_failure` in the result is a stable string and outlives the call.
 */
typedef void (*apad_selftest_cb)(void *user, const char *name, int passed);

typedef struct {
    uint16_t total;
    uint16_t passed;
    uint16_t failed;
    const char *first_failure;   /* NULL if none */
} apad_selftest_result;

/* Returns APAD_OK if every case passed, APAD_ERR_STATE otherwise.
 * `cb` and `out` may both be NULL. */
int apad_selftest_run(apad_selftest_result *out, apad_selftest_cb cb, void *user);

/* ======================================================================== */
/* Platform shim (docs/DESIGN.md §3) — implemented in shim/, not in core/           */
/* ======================================================================== */
/*
 * Every platform provides exactly this and nothing more. IPv4 only: the DS
 * has no IPv6 stack and the protocol is LAN-only by design.
 */

typedef struct {
    uint8_t  ip[4];    /* network order, ip[0] is the first dotted octet */
    uint16_t port;     /* host order */
} apad_addr;

typedef struct apad_sock apad_sock;

/* Return conventions every platform shim MUST match. A console port that
 * quietly returns -1 for "timed out" will look like packet loss on hardware
 * nobody can attach a debugger to. */

/* APAD_OK, or negative. Idempotent. */
int        apad_net_init(void);
/* Non-NULL, or NULL on failure. local_port 0 lets the OS choose. Sockets come
 * from a fixed pool; there is no allocation. */
apad_sock *apad_udp_open(uint16_t local_port);
/* Bytes sent, or negative. Never partial: UDP is all-or-nothing. */
int        apad_udp_send(apad_sock *s, const apad_addr *to, const void *buf, size_t len);
/* Bytes received, 0 on timeout, or negative. timeout_ms < 0 blocks. */
int        apad_udp_recv(apad_sock *s, apad_addr *from, void *buf, size_t cap, int timeout_ms);
/* APAD_OK, or negative. Required for §7 tier-2 discovery. */
int        apad_udp_set_broadcast(apad_sock *s, int enable);

/* Open a socket that holds `local_port` EXCLUSIVELY: while it is open, a
 * second process attempting the same port MUST fail to bind rather than
 * silently share or steal the traffic. Returns APAD_OK, or negative:
 *
 *   APAD_ERR_ARG    `out` is NULL, or local_port is 0. Exclusivity of an
 *                   unbound socket is meaningless, so 0 is rejected rather
 *                   than quietly treated as "no bind" the way apad_udp_open
 *                   treats it.
 *   APAD_ERR_BUFFER the fixed socket pool is exhausted — nothing to do with
 *                   the port.
 *   APAD_ERR_STATE  the port could not be taken exclusively. On any system
 *                   where socket creation works at all, this means ANOTHER
 *                   PROCESS ALREADY HOLDS IT, and it is the case a host is
 *                   expected to report to the user.
 *
 * Why this is an open and not a setter like apad_udp_set_broadcast(): port
 * sharing is decided AT BIND TIME on every target. Windows documents
 * SO_EXCLUSIVEADDRUSE as having to be set before bind(), and on POSIX
 * exclusivity comes from NOT setting SO_REUSEADDR before bind. Once a caller
 * holds an apad_sock* the bind has happened and neither is still reachable,
 * so a post-open accessor could not implement this on any platform.
 *
 * This is NOT a Windows-only concern, despite SO_REUSEADDR being far more
 * dangerous there. Measured on Linux: two UDP sockets that both set
 * SO_REUSEADDR bind the same unicast port successfully, so plain
 * apad_udp_open() — which sets it, for fast restart across a lingering
 * socket — permits a second instance on POSIX too. A host that must be a
 * singleton on its port wants this function on every platform, not just
 * Windows.
 */
int        apad_udp_open_exclusive(apad_sock **out, uint16_t local_port);
void       apad_udp_close(apad_sock *s);
/* Monotonic, wraps at 2^32 (~49.7 days). The wrap is real and intended; every
 * comparison goes through apad_time_after() / apad_time_since(). */
uint32_t   apad_ticks_ms(void);

/* Convenience, shim-side and allocation-free. */
void apad_addr_set(apad_addr *a, uint8_t o0, uint8_t o1, uint8_t o2, uint8_t o3, uint16_t port);
void apad_addr_broadcast(apad_addr *a, uint16_t port);   /* 255.255.255.255 */
int  apad_addr_equal(const apad_addr *a, const apad_addr *b);
/* Parse "192.168.1.10" (tier-3 manual entry, §7). Returns APAD_OK or
 * APAD_ERR_ARG. No stdio, no locale, no strtol. */
int  apad_addr_parse(apad_addr *a, const char *text, uint16_t port);

/* ======================================================================== */
/* §10.3 pairing URI                                                         */
/* ======================================================================== */
/*
 *     atticpad://<ipv4>:<port>/?v=1&s=<secret>
 *
 * One string carrying the address and the §10.1 secret, so a camera-equipped
 * client obtains both in one scan. It lives in core/ — below apad_addr,
 * because it contains one — because THREE independent implementations parse
 * it (the server, the Android client, the 3DS client) and §10.3 makes the
 * encoding normative precisely so they agree byte for byte. Triplicating the
 * parser triplicates its bugs. It is byte-wise work on caller-supplied
 * buffers: no allocation, no float, no stdio, no strtol.
 *
 * The URI is exactly as sensitive as a displayed PIN (§10.3): show it only
 * during an open pairing window and NEVER write it to a log.
 *
 * `v` is the URI PAYLOAD version and is NOT the protocol version (§10.3 says
 * so explicitly). It versions this string only, so the payload can gain a
 * field without touching a wire format frozen at v1.
 */

typedef struct {
    apad_addr addr;                            /* ip and port, both required */
    char      secret[APAD_SECRET_MAX_LEN + 1]; /* NUL-terminated, §10.1      */
} apad_pair_uri;

/* Parse. `uri` is a NUL-terminated string of at most APAD_PAIR_URI_MAX bytes.
 * Nothing past the NUL is ever read, and at most APAD_PAIR_URI_MAX + 1 bytes
 * are read in total: a 128-byte URI is legal (§10.3's ceiling is inclusive),
 * so its terminator is the 129th byte and finding it is what tells a legal
 * maximum-length URI from an over-long one. A caller handing this function a
 * 129-byte buffer with no NUL anywhere in it therefore gets a rejection with
 * no overread, but a 128-byte buffer with no NUL is not a C string and is
 * caller error, exactly as it would be for strlen. Returns APAD_OK, or:
 *
 *   APAD_ERR_VERSION  `v` is present, well-formed and NOT 1 — i.e. "this
 *                     server is newer than I am", which is what a client
 *                     should say to the user. §10.3 requires an unrecognised
 *                     `v` be rejected outright with NO partial
 *                     interpretation, so nothing else in the URI is trusted
 *                     when this is returned. This code is REUSED from §3/§12's
 *                     wire-version mismatch rather than a new one being added:
 *                     enum apad_result is shared by eight platforms and the
 *                     meaning ("a version I do not support") carries exactly.
 *   APAD_ERR_ARG      malformed in any other way — bad scheme, a hostname
 *                     instead of a dotted quad, a missing or out-of-range
 *                     port, a missing/duplicate `v` or `s`, a secret that
 *                     violates §10.1, or a URI over APAD_PAIR_URI_MAX bytes.
 *
 * `v` is checked BEFORE the port range, the address and the secret, so a
 * newer URI whose other fields this version cannot interpret still reports
 * the useful error. Structure it cannot even scan (a future `v` that changes
 * the grammar) can only be APAD_ERR_ARG.
 *
 * `*out` is written ONLY on success and is left untouched on every failure.
 *
 * Treat the input as hostile: a QR code is whatever someone printed.
 */
int apad_pair_uri_parse(apad_pair_uri *out, const char *uri);

/* Build the canonical form into `buf` (`cap` bytes, NUL included). Returns
 * the byte count written EXCLUDING the NUL, or:
 *
 *   APAD_ERR_BUFFER  `cap` is too small for the result plus its NUL.
 *   APAD_ERR_ARG     NULL argument, a port of 0 (§10.3 requires 1..65535 and
 *                    the port is REQUIRED even when it is 21100), or a
 *                    `secret` that is not §10.1-conforming: 6..64 bytes of
 *                    printable ASCII 0x21..0x7E, no embedded NUL.
 *
 * Nothing is written to `buf` unless the return value is positive.
 *
 * ONE EXTRA RESTRICTION, and it is this implementation's choice rather than
 * §10.3's: a secret containing '&' or '#' is REJECTED even though §10.1's
 * alphabet permits both bytes. §10.3 defines no escaping mechanism, so '&'
 * would re-parse as the query separator and '#' is a fragment delimiter to
 * every generic URI consumer a QR library might hand the string to. Emitting
 * a URI that parses back to a DIFFERENT secret is a silent failure on the
 * authentication path, and a loud rejection is the only honest alternative.
 * §10.1's own generated-token alphabet contains neither byte, so no
 * conforming generator is affected. The parser is deliberately not symmetric
 * here: it accepts '#' inside a secret literally, because inventing a
 * fragment rule the spec does not define would be worse.
 */
int apad_pair_uri_build(char *buf, size_t cap,
                        const apad_addr *addr, const char *secret);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_ATTICPAD_H */
