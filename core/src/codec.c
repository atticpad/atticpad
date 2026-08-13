/* codec.c — AtticPad v1 encode/decode (docs/PROTOCOL.md §2-§6).
 *
 * THE RULE THIS FILE EXISTS TO ENFORCE: no struct pointer is ever cast onto a
 * packet buffer. Every multi-byte field is assembled and disassembled one
 * byte at a time through the helpers below. Two independent reasons:
 *
 *   1. Alignment. The DS ARM9 has no alignment fault: it SILENTLY ROTATES the
 *      loaded word. The bug then looks like data corruption on one platform,
 *      intermittently, months later.
 *   2. Endianness. The wire is little-endian (§2) and the codec must not
 *      assume the host is, even though every current target happens to be.
 *
 * Byte-wise access solves both at once and costs nothing measurable: an
 * INPUT_STATE packet is 56 bytes at 125 Hz.
 *
 * memcpy is used only for opaque byte runs (nonces, ids, text) where there is
 * no endianness to get wrong and no alignment to violate.
 */

#include <string.h>

#include "atticpad/atticpad.h"

/* ---- byte-wise field access; little-endian by construction (§2) -------- */

static void wr_u8(uint8_t *p, uint8_t v)
{
    p[0] = v;
}

static uint8_t rd_u8(const uint8_t *p)
{
    return p[0];
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Signed 16-bit. Written as the unsigned bit pattern; read back through
 * unsigned arithmetic so no implementation-defined conversion is involved
 * (C99 6.3.1.3p3) and no target can surprise us. */
static void wr_i16(uint8_t *p, int16_t v)
{
    wr_u16(p, (uint16_t)v);
}

static int16_t rd_i16(const uint8_t *p)
{
    uint16_t u = rd_u16(p);
    if (u < 0x8000u) {
        return (int16_t)u;
    }
    return (int16_t)-(int32_t)(0x10000u - (uint32_t)u);
}

/* ---- tables ------------------------------------------------------------ */

/* §5.1 / docs/DESIGN.md D1. Every impossible D-pad combination resolves to
 * something sane rather than undefined. */
const uint8_t apad_hat_lut[16] = {
    8, 0, 4, 8, 6, 7, 5, 6,
    2, 1, 3, 2, 8, 0, 4, 8
};

/* §9 reliable retransmit schedule. */
const uint16_t apad_retx_delays_ms[APAD_RETX_COUNT] = { 100, 200, 400, 800 };

uint8_t apad_hat_from_buttons(uint32_t buttons)
{
    return apad_hat_lut[(buttons >> APAD_BTN_DPAD_SHIFT) & 0xFu];
}

int apad_payload_size(uint8_t type)
{
    switch (type) {
    case APAD_MSG_DISCOVER:    return (int)APAD_LEN_DISCOVER;
    case APAD_MSG_ANNOUNCE:    return (int)APAD_LEN_ANNOUNCE;
    case APAD_MSG_HELLO:       return (int)APAD_LEN_HELLO;
    case APAD_MSG_WELCOME:     return (int)APAD_LEN_WELCOME;
    case APAD_MSG_BYE:         return (int)APAD_LEN_BYE;
    case APAD_MSG_INPUT_STATE: return (int)APAD_LEN_INPUT_STATE;
    case APAD_MSG_PING:        return (int)APAD_LEN_PING;
    case APAD_MSG_PONG:        return (int)APAD_LEN_PONG;
    case APAD_MSG_RUMBLE:      return (int)APAD_LEN_RUMBLE;
    case APAD_MSG_LED:         return (int)APAD_LEN_LED;
    case APAD_MSG_STATUS:      return (int)APAD_LEN_STATUS;
    case APAD_MSG_ACK:         return (int)APAD_LEN_ACK;
    case APAD_MSG_ERROR:       return (int)APAD_LEN_ERROR;
    default:                   return APAD_ERR_TYPE;   /* §4: discard silently */
    }
}

/* ---- §2 text fields ---------------------------------------------------- */

/* Byte count of the UTF-8 sequence introduced by lead byte `b`, or 0 if `b`
 * is a continuation byte or is not a legal lead byte at all. This classifies
 * one byte; it does NOT validate a string. */
static size_t apad_utf8_seq_len(unsigned char b)
{
    if (b < 0x80u) {
        return 1u;                          /* ASCII */
    }
    if ((b & 0xE0u) == 0xC0u) {
        return 2u;
    }
    if ((b & 0xF0u) == 0xE0u) {
        return 3u;
    }
    if ((b & 0xF8u) == 0xF0u) {
        return 4u;
    }
    return 0u;                              /* continuation, or malformed */
}

/*
 * §2: "A sender MUST NOT split a multi-byte UTF-8 sequence across the end of
 * the field — truncate at a character boundary and NUL-pad the remainder."
 *
 * `field` holds `width` bytes copied from a source longer than the field, so
 * the last sequence may straddle the boundary. Returns the offset to cut at:
 * `width` if nothing straddles, otherwise the offset of the lead byte of the
 * straddling sequence, which is dropped whole.
 *
 * The backward walk is bounded at three continuation bytes because a UTF-8
 * sequence is at most four bytes long: `field` can hold attacker-supplied
 * bytes, so this must be a fixed number of steps and not a search. Past that
 * bound the input is already malformed and the cut stays where it was — §2
 * constrains the boundary, not the contents, so a caller that passed
 * malformed bytes gets them back unrepaired.
 */
static size_t apad_text_trunc_point(const char *field, size_t width)
{
    size_t back;

    for (back = 0u; back < 3u && back < width; back++) {
        size_t start = width - 1u - back;
        unsigned char b = (unsigned char)field[start];
        size_t need;

        if ((b & 0xC0u) == 0x80u) {
            continue;                       /* continuation byte; keep going */
        }
        need = apad_utf8_seq_len(b);
        if (need == 0u || start + need <= width) {
            return width;                   /* malformed lead, or it fits */
        }
        return start;                       /* drop the whole sequence */
    }

    /* Three continuation bytes and still no lead byte: whatever sits at
     * width-4 either leads a sequence that ends exactly at the field edge
     * (four bytes, so it fits) or is malformed. Either way, cut as-is. */
    return width;
}

void apad_text_set(char *field, size_t width, const char *src)
{
    size_t i = 0;

    if (field == NULL || width == 0u) {
        return;
    }
    if (src != NULL) {
        while (i < width && src[i] != '\0') {
            field[i] = src[i];
            i++;
        }
        /* Only when the copy stopped because the FIELD filled up, not because
         * the source ended: src[width] is in bounds here, every byte before
         * it having been non-NUL. */
        if (i == width && src[i] != '\0') {
            i = apad_text_trunc_point(field, width);
        }
    }
    while (i < width) {
        field[i] = '\0';   /* NUL-padded to the fixed width (§2) */
        i++;
    }
}

size_t apad_text_len(const char *field, size_t width)
{
    size_t i = 0;

    if (field == NULL) {
        return 0u;
    }
    while (i < width && field[i] != '\0') {
        i++;
    }
    return i;   /* bounded by the width even with no terminator (§2) */
}

void apad_text_get(char *dst, size_t dst_cap, const char *field, size_t width)
{
    size_t n;
    size_t i;

    if (dst == NULL || dst_cap == 0u) {
        return;
    }
    n = apad_text_len(field, width);
    if (n > dst_cap - 1u) {
        n = dst_cap - 1u;
    }
    for (i = 0; i < n; i++) {
        dst[i] = field[i];
    }
    dst[n] = '\0';
}

/* ---- §3 header --------------------------------------------------------- */

int apad_header_encode(uint8_t *buf, size_t cap, const apad_header *hdr)
{
    if (buf == NULL || hdr == NULL) {
        return APAD_ERR_ARG;
    }
    if (cap < APAD_HEADER_SIZE) {
        return APAD_ERR_BUFFER;
    }

    wr_u16(buf + APAD_OFF_MAGIC,       APAD_MAGIC);
    wr_u8 (buf + APAD_OFF_VERSION,     (uint8_t)APAD_VERSION);
    wr_u8 (buf + APAD_OFF_TYPE,        hdr->type);
    wr_u16(buf + APAD_OFF_SESSION_ID,  hdr->session_id);
    wr_u16(buf + APAD_OFF_SEQUENCE,    hdr->sequence);
    wr_u16(buf + APAD_OFF_PAYLOAD_LEN, hdr->payload_len);
    /* Reserved flag bits are zero on send (§2), whatever the caller passed. */
    wr_u16(buf + APAD_OFF_FLAGS,       (uint16_t)(hdr->flags & APAD_FLAG_KNOWN_MASK));

    return (int)APAD_HEADER_SIZE;
}

int apad_header_decode(const uint8_t *buf, size_t len, apad_header *out)
{
    uint16_t magic;
    uint8_t  version;

    if (buf == NULL || out == NULL) {
        return APAD_ERR_ARG;
    }
    if (len < APAD_HEADER_SIZE) {
        return APAD_ERR_TRUNCATED;
    }

    magic = rd_u16(buf + APAD_OFF_MAGIC);
    if (magic != APAD_MAGIC) {
        return APAD_ERR_MAGIC;
    }
    version = rd_u8(buf + APAD_OFF_VERSION);
    if (version != APAD_VERSION) {
        return APAD_ERR_VERSION;   /* §12: hard reject, never negotiate down */
    }

    out->magic       = magic;
    out->version     = version;
    out->type        = rd_u8 (buf + APAD_OFF_TYPE);
    out->session_id  = rd_u16(buf + APAD_OFF_SESSION_ID);
    out->sequence    = rd_u16(buf + APAD_OFF_SEQUENCE);
    out->payload_len = rd_u16(buf + APAD_OFF_PAYLOAD_LEN);
    /* Reserved flag bits are ignored, not rejected (§2). */
    out->flags       = (uint16_t)(rd_u16(buf + APAD_OFF_FLAGS) & APAD_FLAG_KNOWN_MASK);

    return (int)APAD_HEADER_SIZE;
}

/* ---- §10 authentication tag -------------------------------------------- */

/* Tag covers the entire datagram, header included, with the 8 tag bytes
 * zeroed during computation (§10). `total` is the full datagram length. */
static void tag_compute(const uint8_t *buf, size_t total,
                        const uint8_t *key, size_t key_len,
                        uint8_t out[APAD_TAG_SIZE])
{
    static const uint8_t zeros[APAD_TAG_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    apad_hmac_ctx ctx;
    uint8_t digest[APAD_SHA256_DIGEST_LEN];

    apad_hmac_sha256_init(&ctx, key, key_len);
    apad_hmac_sha256_update(&ctx, buf, total - APAD_TAG_SIZE);
    apad_hmac_sha256_update(&ctx, zeros, APAD_TAG_SIZE);
    apad_hmac_sha256_final(&ctx, digest);

    memcpy(out, digest, APAD_TAG_SIZE);   /* truncated to 8 bytes (§10) */
    apad_secure_zero(digest, sizeof digest);
}

int apad_packet_verify(const uint8_t *buf, size_t len,
                       const uint8_t *key, size_t key_len)
{
    uint8_t expect[APAD_TAG_SIZE];
    int rc;

    if (buf == NULL || key == NULL) {
        return APAD_ERR_ARG;
    }
    if (len < APAD_HEADER_SIZE + APAD_TAG_SIZE) {
        return APAD_ERR_TRUNCATED;
    }
    if ((rd_u16(buf + APAD_OFF_FLAGS) & APAD_FLAG_AUTHENTICATED) == 0u) {
        return APAD_ERR_AUTH;   /* nothing to verify: caller expected a tag */
    }

    tag_compute(buf, len, key, key_len, expect);
    rc = apad_ct_equal(expect, buf + (len - APAD_TAG_SIZE), APAD_TAG_SIZE);
    apad_secure_zero(expect, sizeof expect);

    return (rc == 0) ? APAD_OK : APAD_ERR_AUTH;
}

/* ---- framing ----------------------------------------------------------- */

int apad_packet_build(uint8_t *buf, size_t cap,
                      const apad_header *hdr,
                      const void *payload, uint16_t payload_len,
                      const uint8_t *key, size_t key_len)
{
    apad_header h;
    size_t total;
    int rc;

    if (buf == NULL || hdr == NULL) {
        return APAD_ERR_ARG;
    }
    if (payload_len != 0u && payload == NULL) {
        return APAD_ERR_ARG;
    }

    total = (size_t)APAD_HEADER_SIZE + (size_t)payload_len;
    if (key != NULL) {
        total += APAD_TAG_SIZE;
    }
    if (total > APAD_MAX_DATAGRAM) {
        return APAD_ERR_LENGTH;   /* §1: no fragmentation, ever */
    }
    if (cap < total) {
        return APAD_ERR_BUFFER;
    }

    h = *hdr;
    h.magic       = APAD_MAGIC;
    h.version     = (uint8_t)APAD_VERSION;
    h.payload_len = payload_len;
    if (key != NULL) {
        h.flags = (uint16_t)(h.flags | APAD_FLAG_AUTHENTICATED);
    } else {
        h.flags = (uint16_t)(h.flags & ~(uint16_t)APAD_FLAG_AUTHENTICATED);
    }

    rc = apad_header_encode(buf, cap, &h);
    if (rc < 0) {
        return rc;
    }
    if (payload_len != 0u) {
        memcpy(buf + APAD_HEADER_SIZE, payload, (size_t)payload_len);
    }
    if (key != NULL) {
        memset(buf + APAD_HEADER_SIZE + payload_len, 0, APAD_TAG_SIZE);
        tag_compute(buf, total, key, key_len,
                    buf + APAD_HEADER_SIZE + payload_len);
    }

    return (int)total;
}

int apad_packet_parse(const uint8_t *buf, size_t len, apad_packet *out)
{
    apad_header h;
    size_t expect_total;
    int rc;
    int want;

    if (buf == NULL || out == NULL) {
        return APAD_ERR_ARG;
    }

    /* §3.1 checks 1, 2 and 3, in that order: length >= 12 (discard), magic
     * (discard), version (reject, ERROR code 1). §1's 256-byte cap is
     * deliberately NOT tested here — it is not one of §3.1's seven checks,
     * and testing it first made a 300-byte datagram with garbage magic
     * return a rejection where the spec requires a silent discard. */
    rc = apad_header_decode(buf, len, &h);
    if (rc < 0) {
        return rc;
    }

    /* §3: the actual length MUST equal 12 + payload_len + (auth ? 8 : 0). */
    expect_total = (size_t)APAD_HEADER_SIZE + (size_t)h.payload_len;
    if ((h.flags & APAD_FLAG_AUTHENTICATED) != 0u) {
        expect_total += APAD_TAG_SIZE;
    }
    if (len != expect_total) {
        return APAD_ERR_LENGTH;
    }

    /* §4: unknown type -> the caller discards silently. Checked after the
     * length test so a malformed unknown-type packet still reads as unknown
     * rather than as a length error; either way it is dropped. */
    want = apad_payload_size(h.type);
    if (want == APAD_ERR_TYPE) {
        return APAD_ERR_TYPE;
    }

    /* §1: "Maximum datagram 256 bytes. No fragmentation, ever." Placed here,
     * after §3.1 check 5, so it can never pre-empt one of the seven ordered
     * checks: an oversize datagram is discarded for a bad magic or an unknown
     * type first, exactly as §3.1 requires. By this point the check can no
     * longer change any v1 outcome — the largest defined payload is HELLO at
     * 76 bytes, so check 6 below already bounds a successful parse to 96
     * bytes — but §1 is a stated limit and this is where asserting it costs
     * nothing. */
    if (len > APAD_MAX_DATAGRAM) {
        return APAD_ERR_LENGTH;
    }

    /* v1 has no variable-length payload (§12), so a known type with the wrong
     * payload_len is malformed (ERROR code 6, §6.11). */
    if ((uint16_t)want != h.payload_len) {
        return APAD_ERR_LENGTH;
    }

    out->header      = h;
    out->payload_len = h.payload_len;
    out->payload     = (h.payload_len != 0u) ? (buf + APAD_HEADER_SIZE) : NULL;
    out->tag         = ((h.flags & APAD_FLAG_AUTHENTICATED) != 0u)
                       ? (buf + len - APAD_TAG_SIZE) : NULL;

    return (int)len;
}

/* ---- payload guards ---------------------------------------------------- */

#define ENC_GUARD(need)                              \
    do {                                             \
        if (b == NULL || in == NULL) {               \
            return APAD_ERR_ARG;                     \
        }                                            \
        if (cap < (size_t)(need)) {                  \
            return APAD_ERR_BUFFER;                  \
        }                                            \
        memset(b, 0, (size_t)(need));                \
    } while (0)

#define DEC_GUARD(need)                              \
    do {                                             \
        if (b == NULL || out == NULL) {              \
            return APAD_ERR_ARG;                     \
        }                                            \
        if (len != (size_t)(need)) {                 \
            return APAD_ERR_LENGTH;                  \
        }                                            \
        memset(out, 0, sizeof *out);                 \
    } while (0)

/* ---- §6.2 ANNOUNCE ----------------------------------------------------- */

int apad_encode_announce(uint8_t *b, size_t cap, const apad_announce *in)
{
    ENC_GUARD(APAD_LEN_ANNOUNCE);

    memcpy(b + 0, in->server_name, APAD_NAME_LEN);
    wr_u8 (b + 32, in->pads_total);
    wr_u8 (b + 33, in->pads_free);
    wr_u8 (b + 34, in->pairing_required ? 1u : 0u);
    /* b[35] reserved0, b[38..39] reserved1 already zeroed by ENC_GUARD */
    wr_u16(b + 36, in->server_port);

    return (int)APAD_LEN_ANNOUNCE;
}

/* §6.2: "pairing_required — 0 or 1; any non-zero value MUST be read as 1".
 * Third of the three §6.0 "drives something a consumer acts on" fields, with
 * battery (§5.5) and LED player_index (§6.8). RECEIVE SIDE ONLY, for the same
 * reason as the other two: a sender putting 200 in a boolean is a bug that
 * should stay visible rather than be laundered on the way out.
 *
 * Note what is deliberately NOT normalised: BYE.reason, STATUS.code and
 * ERROR.code (§6.0). Those are diagnostic labels, not control inputs, and a
 * receiver MUST preserve an unrecognised value verbatim — a v1.1 peer may add
 * codes, and clamping one would destroy the only information the message
 * exists to carry. */
static uint8_t normalise_flag01(uint8_t v)
{
    return (v != 0u) ? (uint8_t)1 : (uint8_t)0;
}

int apad_decode_announce(const uint8_t *b, size_t len, apad_announce *out)
{
    DEC_GUARD(APAD_LEN_ANNOUNCE);

    memcpy(out->server_name, b + 0, APAD_NAME_LEN);
    out->pads_total       = rd_u8 (b + 32);
    out->pads_free        = rd_u8 (b + 33);
    out->pairing_required = normalise_flag01(rd_u8(b + 34));   /* §6.2 */
    out->server_port      = rd_u16(b + 36);

    return (int)APAD_LEN_ANNOUNCE;
}

/* ---- §6.3 HELLO -------------------------------------------------------- */

int apad_encode_hello(uint8_t *b, size_t cap, const apad_hello *in)
{
    ENC_GUARD(APAD_LEN_HELLO);

    memcpy(b + 0, in->client_id, APAD_CLIENT_ID_LEN);
    wr_u32(b + 16, in->caps & APAD_CAP_VALID_MASK);   /* bits 14..31 reserved */
    memcpy(b + 20, in->device_name, APAD_NAME_LEN);
    memcpy(b + 52, in->client_nonce, APAD_NONCE_LEN);
    wr_u16(b + 68, in->desired_rate_hz);
    wr_u8 (b + 70, in->proto_major);
    /* b[71] reserved0 already zero */
    wr_u32(b + 72, in->client_ticks_ms);

    return (int)APAD_LEN_HELLO;
}

int apad_decode_hello(const uint8_t *b, size_t len, apad_hello *out)
{
    DEC_GUARD(APAD_LEN_HELLO);

    memcpy(out->client_id, b + 0, APAD_CLIENT_ID_LEN);
    out->caps = rd_u32(b + 16) & APAD_CAP_VALID_MASK;
    memcpy(out->device_name, b + 20, APAD_NAME_LEN);
    memcpy(out->client_nonce, b + 52, APAD_NONCE_LEN);
    out->desired_rate_hz = rd_u16(b + 68);
    out->proto_major     = rd_u8 (b + 70);
    out->client_ticks_ms = rd_u32(b + 72);

    return (int)APAD_LEN_HELLO;
}

/* ---- §6.4 WELCOME ------------------------------------------------------ */

int apad_encode_welcome(uint8_t *b, size_t cap, const apad_welcome *in)
{
    ENC_GUARD(APAD_LEN_WELCOME);

    wr_u16(b + 0, in->session_id);
    wr_u8 (b + 2, in->pad_slot);
    wr_u8 (b + 3, (uint8_t)(in->flags & APAD_WELCOME_FLAGS_MASK));
    wr_u16(b + 4, in->input_rate_hz);
    /* b[6..7] reserved0 already zero */
    memcpy(b + 8, in->server_nonce, APAD_NONCE_LEN);
    /* b[24..55] key_material MUST be zero in v1 (§6.4); ENC_GUARD did it and
     * in->key_material is deliberately not copied. */
    wr_u32(b + 56, in->server_ticks_ms);

    return (int)APAD_LEN_WELCOME;
}

int apad_decode_welcome(const uint8_t *b, size_t len, apad_welcome *out)
{
    DEC_GUARD(APAD_LEN_WELCOME);

    out->session_id = rd_u16(b + 0);
    out->pad_slot   = rd_u8 (b + 2);
    out->flags      = (uint8_t)(rd_u8(b + 3) & APAD_WELCOME_FLAGS_MASK);
    out->input_rate_hz = rd_u16(b + 4);
    memcpy(out->server_nonce, b + 8, APAD_NONCE_LEN);
    /* §6.4: key_material MUST be ignored on receive in v1. Left zeroed. */
    out->server_ticks_ms = rd_u32(b + 56);

    return (int)APAD_LEN_WELCOME;
}

/* ---- §6.5 BYE ---------------------------------------------------------- */

int apad_encode_bye(uint8_t *b, size_t cap, const apad_bye *in)
{
    ENC_GUARD(APAD_LEN_BYE);
    wr_u8(b + 0, in->reason);
    return (int)APAD_LEN_BYE;
}

int apad_decode_bye(const uint8_t *b, size_t len, apad_bye *out)
{
    DEC_GUARD(APAD_LEN_BYE);
    out->reason = rd_u8(b + 0);
    return (int)APAD_LEN_BYE;
}

/* ---- §6.6 PING / PONG -------------------------------------------------- */

int apad_encode_ping(uint8_t *b, size_t cap, const apad_ping *in)
{
    ENC_GUARD(APAD_LEN_PING);
    wr_u32(b + 0, in->origin_ticks_ms);
    wr_u32(b + 4, in->responder_ticks_ms);
    return (int)APAD_LEN_PING;
}

int apad_decode_ping(const uint8_t *b, size_t len, apad_ping *out)
{
    DEC_GUARD(APAD_LEN_PING);
    out->origin_ticks_ms    = rd_u32(b + 0);
    out->responder_ticks_ms = rd_u32(b + 4);
    return (int)APAD_LEN_PING;
}

/* ---- §6.7 RUMBLE ------------------------------------------------------- */

int apad_encode_rumble(uint8_t *b, size_t cap, const apad_rumble *in)
{
    ENC_GUARD(APAD_LEN_RUMBLE);
    wr_u16(b + 0, in->low_freq);
    wr_u16(b + 2, in->high_freq);
    wr_u16(b + 4, in->duration_ms);
    /* b[6..7] reserved0 already zero */
    return (int)APAD_LEN_RUMBLE;
}

int apad_decode_rumble(const uint8_t *b, size_t len, apad_rumble *out)
{
    DEC_GUARD(APAD_LEN_RUMBLE);
    out->low_freq    = rd_u16(b + 0);
    out->high_freq   = rd_u16(b + 2);
    out->duration_ms = rd_u16(b + 4);
    return (int)APAD_LEN_RUMBLE;
}

/* ---- §6.8 LED ---------------------------------------------------------- */

int apad_encode_led(uint8_t *b, size_t cap, const apad_led *in)
{
    ENC_GUARD(APAD_LEN_LED);
    wr_u8(b + 0, in->player_index);
    wr_u8(b + 1, in->r);
    wr_u8(b + 2, in->g);
    wr_u8(b + 3, in->b);
    return (int)APAD_LEN_LED;
}

/* §6.8: "Values above 4 are reserved and MUST be treated as 0 (off) on
 * receive." Same shape, and the same reasoning, as normalise_battery above:
 * a decode-time normalisation so that no consumer on any platform ever sees
 * player_index = 173. RECEIVE SIDE ONLY — the encoder passes the caller's
 * value through, so a client emitting a reserved value stays visible instead
 * of being silently laundered into "off". */
static uint8_t normalise_player_index(uint8_t v)
{
    return (v > 4u) ? (uint8_t)0 : v;   /* 0 = off, 1..4 = a player slot */
}

int apad_decode_led(const uint8_t *b, size_t len, apad_led *out)
{
    DEC_GUARD(APAD_LEN_LED);
    out->player_index = normalise_player_index(rd_u8(b + 0));   /* §6.8 */
    out->r = rd_u8(b + 1);
    out->g = rd_u8(b + 2);
    out->b = rd_u8(b + 3);
    return (int)APAD_LEN_LED;
}

/* ---- §6.9 STATUS ------------------------------------------------------- */

int apad_encode_status(uint8_t *b, size_t cap, const apad_status *in)
{
    ENC_GUARD(APAD_LEN_STATUS);
    wr_u8(b + 0, in->code);
    /* b[1..3] reserved0 already zero */
    memcpy(b + 4, in->text, APAD_TEXT_LEN);
    return (int)APAD_LEN_STATUS;
}

int apad_decode_status(const uint8_t *b, size_t len, apad_status *out)
{
    DEC_GUARD(APAD_LEN_STATUS);
    out->code = rd_u8(b + 0);
    memcpy(out->text, b + 4, APAD_TEXT_LEN);
    return (int)APAD_LEN_STATUS;
}

/* ---- §6.10 ACK --------------------------------------------------------- */

int apad_encode_ack(uint8_t *b, size_t cap, const apad_ack *in)
{
    ENC_GUARD(APAD_LEN_ACK);
    wr_u16(b + 0, in->sequence);
    /* b[2..3] reserved0 already zero */
    return (int)APAD_LEN_ACK;
}

int apad_decode_ack(const uint8_t *b, size_t len, apad_ack *out)
{
    DEC_GUARD(APAD_LEN_ACK);
    out->sequence = rd_u16(b + 0);
    return (int)APAD_LEN_ACK;
}

/* ---- §6.11 ERROR ------------------------------------------------------- */

int apad_encode_error(uint8_t *b, size_t cap, const apad_error *in)
{
    ENC_GUARD(APAD_LEN_ERROR);
    wr_u16(b + 0, in->code);
    /* b[2..3] reserved0 already zero */
    memcpy(b + 4, in->text, APAD_TEXT_LEN);
    return (int)APAD_LEN_ERROR;
}

int apad_decode_error(const uint8_t *b, size_t len, apad_error *out)
{
    DEC_GUARD(APAD_LEN_ERROR);
    out->code = rd_u16(b + 0);
    memcpy(out->text, b + 4, APAD_TEXT_LEN);
    return (int)APAD_LEN_ERROR;
}

/* ---- §5 INPUT_STATE ---------------------------------------------------- */
/* The only high-rate message. 56 bytes; see input.h for the offset table. */

static int16_t clamp_trigger(int16_t v)
{
    /* §5: "Negative values in axes[4]/axes[5] MUST be treated as 0". */
    return (v < 0) ? (int16_t)0 : v;
}

/* §5.5: battery carries 0..100 or 255 for unknown; 101..254 are reserved and
 * "a decoder MUST normalise them to 255 in its decoded output". Decode-time,
 * not an obligation on the consumer, so every consumer on every platform sees
 * the same value for the same datagram.
 *
 * RECEIVE SIDE ONLY. The encoder passes the caller's value through: a sender
 * that puts a reserved value on the wire is non-conforming (§2), and silently
 * rewriting it would hide that from whoever has to debug it. So an encode of
 * 150 followed by a decode does not round-trip to 150 — by design. */
static uint8_t normalise_battery(uint8_t v)
{
    if (v > 100u && v != (uint8_t)APAD_BATTERY_UNKNOWN) {
        return (uint8_t)APAD_BATTERY_UNKNOWN;   /* 101..254 */
    }
    return v;                                   /* 0..100, and 255 itself */
}

int apad_encode_input_state(uint8_t *b, size_t cap, const apad_input_state *in)
{
    uint8_t count;
    int i;

    ENC_GUARD(APAD_LEN_INPUT_STATE);

    /* §5.1: bits 20..31 reserved, zero on send. */
    wr_u32(b + 0, in->buttons & APAD_BTN_VALID_MASK);

    for (i = 0; i < 6; i++) {
        int16_t v = in->axes[i];
        if (i == APAD_AXIS_L2 || i == APAD_AXIS_R2) {
            v = clamp_trigger(v);
        }
        wr_i16(b + 4 + (i * 2), v);
    }
    /* axes[6], axes[7] reserved and MUST be zero (§5) — left zeroed. */

    count = in->touch_count;
    if (count > APAD_TOUCH_MAX) {
        count = APAD_TOUCH_MAX;   /* §5: >2 clamped */
    }
    wr_u8(b + 20, count);
    /* b[21] reserved0 already zero */

    for (i = 0; i < APAD_TOUCH_MAX; i++) {
        uint8_t *t = b + 22 + (i * 6);
        if ((uint8_t)i >= count) {
            continue;   /* §5.2: entries >= touch_count are zero on send */
        }
        wr_u8 (t + 0, in->touches[i].id);
        wr_u8 (t + 1, in->touches[i].pressure);
        wr_i16(t + 2, in->touches[i].x);
        wr_i16(t + 4, in->touches[i].y);
    }

    for (i = 0; i < 3; i++) {
        wr_i16(b + 34 + (i * 2), in->accel[i]);
        wr_i16(b + 40 + (i * 2), in->gyro[i]);
    }

    wr_u8 (b + 46, in->battery);
    /* b[47..49] reserved1, b[50..51] reserved2 already zero */
    wr_u32(b + 52, in->client_ticks_ms);

    return (int)APAD_LEN_INPUT_STATE;
}

int apad_decode_input_state(const uint8_t *b, size_t len, apad_input_state *out)
{
    int i;

    DEC_GUARD(APAD_LEN_INPUT_STATE);

    out->buttons = rd_u32(b + 0) & APAD_BTN_VALID_MASK;   /* §5.1 reserved */

    for (i = 0; i < 6; i++) {
        int16_t v = rd_i16(b + 4 + (i * 2));
        if (i == APAD_AXIS_L2 || i == APAD_AXIS_R2) {
            v = clamp_trigger(v);
        }
        out->axes[i] = v;
    }
    out->axes[6] = 0;   /* §5: reserved, ignored on receive */
    out->axes[7] = 0;

    out->touch_count = rd_u8(b + 20);
    if (out->touch_count > APAD_TOUCH_MAX) {
        out->touch_count = APAD_TOUCH_MAX;   /* §5 */
    }
    for (i = 0; i < APAD_TOUCH_MAX; i++) {
        const uint8_t *t = b + 22 + (i * 6);
        if ((uint8_t)i >= out->touch_count) {
            continue;   /* §5.2: ignored on receive; left zeroed */
        }
        out->touches[i].id       = rd_u8 (t + 0);
        out->touches[i].pressure = rd_u8 (t + 1);
        out->touches[i].x        = rd_i16(t + 2);
        out->touches[i].y        = rd_i16(t + 4);
    }

    for (i = 0; i < 3; i++) {
        out->accel[i] = rd_i16(b + 34 + (i * 2));
        out->gyro[i]  = rd_i16(b + 40 + (i * 2));
    }

    out->battery         = normalise_battery(rd_u8(b + 46));   /* §5.5 */
    out->client_ticks_ms = rd_u32(b + 52);

    return (int)APAD_LEN_INPUT_STATE;
}

/* ---- addresses (pure byte work; kept out of the shim so every platform
 *      shares one implementation) ----------------------------------------- */

void apad_addr_set(apad_addr *a, uint8_t o0, uint8_t o1, uint8_t o2, uint8_t o3,
                   uint16_t port)
{
    if (a == NULL) {
        return;
    }
    a->ip[0] = o0;
    a->ip[1] = o1;
    a->ip[2] = o2;
    a->ip[3] = o3;
    a->port  = port;
}

void apad_addr_broadcast(apad_addr *a, uint16_t port)
{
    apad_addr_set(a, 255u, 255u, 255u, 255u, port);   /* §7 tier 2 */
}

int apad_addr_equal(const apad_addr *a, const apad_addr *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (a->port == b->port && memcmp(a->ip, b->ip, 4) == 0) ? 1 : 0;
}

int apad_addr_parse(apad_addr *a, const char *text, uint16_t port)
{
    uint8_t octets[4];
    int part = 0;
    int digits = 0;
    unsigned acc = 0;
    size_t i = 0;

    if (a == NULL || text == NULL) {
        return APAD_ERR_ARG;
    }

    for (;;) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            if (digits >= 3) {
                return APAD_ERR_ARG;
            }
            acc = (acc * 10u) + (unsigned)(c - '0');
            digits++;
            if (acc > 255u) {
                return APAD_ERR_ARG;
            }
        } else if (c == '.' || c == '\0') {
            if (digits == 0 || part > 3) {
                return APAD_ERR_ARG;
            }
            octets[part] = (uint8_t)acc;
            part++;
            acc = 0;
            digits = 0;
            if (c == '\0') {
                break;
            }
        } else {
            return APAD_ERR_ARG;
        }
        i++;
        if (i > 15u) {
            return APAD_ERR_ARG;   /* "255.255.255.255" is 15 chars */
        }
    }

    if (part != 4) {
        return APAD_ERR_ARG;
    }
    apad_addr_set(a, octets[0], octets[1], octets[2], octets[3], port);
    return APAD_OK;
}

/* ---- §10.3 pairing URI (same reasoning as the address helpers above: pure
 *      byte work, and THREE implementations must agree on it byte for byte,
 *      so there is exactly one copy of it) ---------------------------------
 *
 *      atticpad://<ipv4>:<port>/?v=1&s=<secret>
 *
 * Everything below treats the input as hostile — a QR code is whatever
 * someone printed — which in practice means two rules:
 *
 *   1. The length is established ONCE, bounded, before any field is looked
 *      at, and every scan below is bounded by it. No loop can walk past the
 *      NUL or past APAD_PAIR_URI_MAX.
 *   2. Every bound is tested BEFORE the dereference it guards
 *      (`i < len && uri[i]`, never the other way round). This is the exact
 *      ordering bug that produced a real heap overread in
 *      apad_derive_session_key.
 *
 * No strtol, no sscanf, no locale — same constraints as apad_addr_parse, for
 * the same reason.
 */

/* Length of a NUL-terminated string, bounded. Returns the index of the NUL,
 * or max + 1 if there is no NUL in s[0 .. max]. Reads at most max + 1 bytes,
 * which is the minimum needed to tell a legal max-byte string from an
 * over-long one, and never reads past the NUL. */
static size_t uri_len_bounded(const char *s, size_t max)
{
    size_t i;

    for (i = 0; i <= max; i++) {
        if (s[i] == '\0') {
            return i;
        }
    }
    return max + 1u;
}

/* §10.1: printable ASCII 0x21..0x7E. NUL is excluded by construction — every
 * caller below derives its length from a bounded NUL scan. */
static int uri_secret_char_ok(char c)
{
    unsigned char u = (unsigned char)c;

    return (u >= 0x21u && u <= 0x7Eu) ? 1 : 0;
}

/* Append `len` bytes, refusing to write past `cap`. Returns 0 if it would
 * not fit, in which case nothing is written. `cap` excludes the NUL. */
static int uri_append(char *dst, size_t cap, size_t *n, const char *src,
                      size_t len)
{
    size_t i;

    if (*n > cap || (cap - *n) < len) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        dst[*n + i] = src[i];
    }
    *n += len;
    return 1;
}

/* Decimal, no stdio. `v` must be <= 65535, so five digits is the ceiling. */
static int uri_append_u32(char *dst, size_t cap, size_t *n, uint32_t v)
{
    char digits[5];
    size_t nd = 0;
    size_t i;

    if (v == 0u) {
        digits[nd++] = '0';
    }
    while (v > 0u && nd < sizeof digits) {
        digits[nd++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    if (*n > cap || (cap - *n) < nd) {
        return 0;
    }
    for (i = 0; i < nd; i++) {
        dst[*n + i] = digits[nd - 1u - i];   /* reverse: built least-first */
    }
    *n += nd;
    return 1;
}

int apad_pair_uri_parse(apad_pair_uri *out, const char *uri)
{
    static const char scheme[] = "atticpad://";
    const size_t scheme_len = sizeof scheme - 1u;   /* 11, NUL excluded */

    apad_pair_uri tmp;
    char   host[16];        /* "255.255.255.255" + NUL */
    size_t len, p, i;
    size_t host_len = 0;
    int    host_bad = 0;
    uint32_t port = 0;
    size_t port_digits = 0;
    int    port_bad = 0;
    size_t v_at = 0, v_len = 0, s_at = 0, s_len = 0;
    int    have_v = 0, have_s = 0;

    if (out == NULL || uri == NULL) {
        return APAD_ERR_ARG;
    }

    /* §10.3: the whole URI MUST be <= 128 bytes. An unterminated buffer lands
     * here too — uri_len_bounded returns max + 1 and never read past 128. */
    len = uri_len_bounded(uri, APAD_PAIR_URI_MAX);
    if (len > APAD_PAIR_URI_MAX) {
        return APAD_ERR_ARG;
    }

    /* scheme: lowercase, exact. */
    if (len < scheme_len) {
        return APAD_ERR_ARG;
    }
    for (i = 0; i < scheme_len; i++) {
        if (uri[i] != scheme[i]) {
            return APAD_ERR_ARG;
        }
    }
    p = scheme_len;

    /* host: everything up to the ':'. Copied out so apad_addr_parse — the one
     * dotted-quad parser, which already refuses hostnames — can be reused
     * rather than a second one written. An over-long host is remembered, not
     * rejected here, so that an unrecognised `v` still wins below. */
    while (p < len && uri[p] != ':') {
        if (host_len < (sizeof host - 1u)) {
            host[host_len] = uri[p];
            host_len++;
        } else {
            host_bad = 1;
        }
        p++;
    }
    if (p >= len) {
        return APAD_ERR_ARG;   /* no ':' — no port can follow */
    }
    host[host_len] = '\0';
    if (host_len == 0u) {
        host_bad = 1;
    }
    p++;   /* the ':' */

    /* port: decimal digits. Range checked after `v`, for the same reason. */
    while (p < len && uri[p] >= '0' && uri[p] <= '9') {
        if (port_digits < 5u) {
            port = (port * 10u) + (uint32_t)(uri[p] - '0');
        } else {
            port_bad = 1;   /* >5 digits cannot be 1..65535; stop accumulating */
        }
        port_digits++;
        p++;
    }
    if (port_digits == 0u) {
        port_bad = 1;
    }

    /* "/?" — both literals of §10.3's template, required. */
    if ((p + 1u) >= len || uri[p] != '/' || uri[p + 1u] != '?') {
        return APAD_ERR_ARG;
    }
    p += 2u;

    /* query: key=value pairs separated by '&', in any order (§10.3). Unknown
     * keys are IGNORED so a later revision can add one; a duplicate of a key
     * this version acts on is rejected, because choosing one of two secrets
     * is exactly the "partial interpretation" §10.3 forbids. */
    while (p < len) {
        size_t k_at = p;
        size_t k_len;
        size_t val_at;
        size_t val_len = 0;
        int    has_eq = 0;

        while (p < len && uri[p] != '=' && uri[p] != '&') {
            p++;
        }
        k_len = p - k_at;
        val_at = p;
        if (p < len && uri[p] == '=') {
            has_eq = 1;
            p++;
            val_at = p;
            while (p < len && uri[p] != '&') {
                p++;
            }
            val_len = p - val_at;
        }
        if (p < len) {
            p++;                       /* the '&' */
            if (p >= len) {
                return APAD_ERR_ARG;   /* trailing '&': an empty segment */
            }
        }
        if (k_len == 0u) {
            return APAD_ERR_ARG;       /* "&&", "?=x": no key at all */
        }

        if (k_len == 1u && uri[k_at] == 'v') {
            if (have_v || !has_eq) {
                return APAD_ERR_ARG;
            }
            have_v = 1;
            v_at   = val_at;
            v_len  = val_len;
        } else if (k_len == 1u && uri[k_at] == 's') {
            if (have_s || !has_eq) {
                return APAD_ERR_ARG;
            }
            have_s = 1;
            s_at   = val_at;
            s_len  = val_len;
        }
        /* anything else: ignored, per §10.3 */
    }

    /* `v` first: REQUIRED, and an unrecognised value is rejected outright
     * with no partial interpretation (§10.3). Checked before the port, the
     * address and the secret so that a URI from a newer server reports
     * "newer than I am" rather than "broken". */
    if (!have_v) {
        return APAD_ERR_ARG;
    }
    if (v_len == 0u) {
        return APAD_ERR_ARG;   /* "v=" carries no version at all */
    }
    for (i = 0; i < v_len; i++) {
        if (uri[v_at + i] < '0' || uri[v_at + i] > '9') {
            return APAD_ERR_ARG;   /* not a version number: malformed */
        }
    }
    if (v_len != 1u || uri[v_at] != '1') {
        return APAD_ERR_VERSION;   /* "2", "01", "99999999" — all unknown */
    }

    /* §10.3: port 1..65535, REQUIRED, never defaulted. */
    if (port_bad || port < 1u || port > 65535u) {
        return APAD_ERR_ARG;
    }
    /* §10.3: an IPv4 dotted quad, never a hostname. */
    if (host_bad) {
        return APAD_ERR_ARG;
    }

    /* §10.1: 6..64 bytes of printable ASCII 0x21..0x7E. */
    if (!have_s) {
        return APAD_ERR_ARG;
    }
    if (s_len < APAD_SECRET_MIN_LEN || s_len > APAD_SECRET_MAX_LEN) {
        return APAD_ERR_ARG;
    }
    for (i = 0; i < s_len; i++) {
        if (!uri_secret_char_ok(uri[s_at + i])) {
            return APAD_ERR_ARG;
        }
    }

    /* Everything validated: build the result in a local and publish it in one
     * go, so a caller that ignores the return code cannot find a half-filled
     * struct — the same rule apad_packet_parse follows. */
    memset(&tmp, 0, sizeof tmp);
    if (apad_addr_parse(&tmp.addr, host, (uint16_t)port) != APAD_OK) {
        return APAD_ERR_ARG;
    }
    for (i = 0; i < s_len; i++) {
        tmp.secret[i] = uri[s_at + i];
    }
    tmp.secret[s_len] = '\0';

    *out = tmp;
    return APAD_OK;
}

int apad_pair_uri_build(char *buf, size_t cap, const apad_addr *addr,
                        const char *secret)
{
    char   tmp[APAD_PAIR_URI_MAX + 1u];
    size_t n = 0;
    size_t slen, i;
    int    ok = 1;

    if (buf == NULL || addr == NULL || secret == NULL || cap == 0u) {
        return APAD_ERR_ARG;
    }
    /* §10.3: 1..65535. Port 0 is not "absent", it is invalid. */
    if (addr->port == 0u) {
        return APAD_ERR_ARG;
    }

    /* §10.1, enforced here because this is a layer that CAN reject — unlike
     * apad_derive_session_key, which returns void and silently truncates. */
    slen = uri_len_bounded(secret, APAD_SECRET_MAX_LEN);
    if (slen < APAD_SECRET_MIN_LEN || slen > APAD_SECRET_MAX_LEN) {
        return APAD_ERR_ARG;
    }
    for (i = 0; i < slen; i++) {
        if (!uri_secret_char_ok(secret[i])) {
            return APAD_ERR_ARG;
        }
        /* Not §10.3's rule but this implementation's: neither byte can
         * survive a round trip through a grammar with no escaping. See the
         * declaration in atticpad.h for the full argument. */
        if (secret[i] == '&' || secret[i] == '#') {
            return APAD_ERR_ARG;
        }
    }

    ok = ok && uri_append(tmp, APAD_PAIR_URI_MAX, &n, "atticpad://", 11u);
    for (i = 0; i < 4u; i++) {
        if (i > 0u) {
            ok = ok && uri_append(tmp, APAD_PAIR_URI_MAX, &n, ".", 1u);
        }
        ok = ok && uri_append_u32(tmp, APAD_PAIR_URI_MAX, &n,
                                  (uint32_t)addr->ip[i]);
    }
    ok = ok && uri_append(tmp, APAD_PAIR_URI_MAX, &n, ":", 1u);
    ok = ok && uri_append_u32(tmp, APAD_PAIR_URI_MAX, &n,
                              (uint32_t)addr->port);
    ok = ok && uri_append(tmp, APAD_PAIR_URI_MAX, &n, "/?v=1&s=", 8u);
    ok = ok && uri_append(tmp, APAD_PAIR_URI_MAX, &n, secret, slen);
    if (!ok) {
        /* Unreachable with validated inputs — the longest legal form is 104
         * bytes (§10.3 says "about 103") against a 128-byte ceiling. Kept
         * because a bound that is only true by arithmetic nobody re-derives
         * is how a buffer overflow arrives with the next field. */
        return APAD_ERR_BUFFER;
    }
    tmp[n] = '\0';

    if (cap < (n + 1u)) {
        return APAD_ERR_BUFFER;   /* nothing written to buf */
    }
    memcpy(buf, tmp, n + 1u);
    return (int)n;
}
