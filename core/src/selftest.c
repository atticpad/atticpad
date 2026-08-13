/* selftest.c — the hidden L+R+Start conformance screen (docs/PROTOCOL.md §13).
 *
 * Six of eight clients have no test hardware. When a Vita user says "it does
 * not work", this is the only diagnostic that separates "your codec is broken
 * on this CPU" from "your Wi-Fi is bad". It therefore must run with no OS
 * services at all: no stdio, no malloc, no floating point, no clock. Results
 * come back through a callback and a summary struct, and the client prints
 * them with whatever drawing primitives it has.
 *
 * st_seq..st_addr below are the codec's OWN invariants, and they are the
 * weaker half of the story: an implementation that misreads the spec
 * consistently will pass all of them. st_vectors runs the golden packets in
 * core/testdata/vectors.h, authored from the spec by someone who has not read
 * this codec (docs/DESIGN.md §9.1), and that is what catches a shared
 * misunderstanding.
 *
 * The vectors compile in UNCONDITIONALLY -- there is no -DAPAD_WITH_VECTORS
 * and there deliberately is no opt-out. Two reasons. (1) The build the CI gate
 * runs is `scripts/build.sh core`, which passes no extra defines; a flag-gated
 * conformance suite is one nobody runs, which is worse than none because it
 * looks like coverage. (2) The cost is const data. Measured on x86-64 at -Os,
 * this file grew from 17.7 kB to 32.9 kB: +10.3 kB text (the golden byte
 * arrays plus the runner) and +4.8 kB of relocatable const, which is the
 * tables' `const char *name` pointers -- on a bare-metal target with no PIE
 * those are plain rodata too. Call it 15 kB against 4 MB on the smallest
 * platform. Nothing here is reachable except from apad_selftest_run, so a
 * platform that truly cannot spare it can drop this whole translation unit;
 * it cannot half-drop it and still claim to run the self-test screen.
 * (Not measured on ARM/Thumb -- no cross compiler in this environment.)
 *
 * vectors.h is included by relative path because scripts/build.sh puts only
 * core/include on the include path, and scripts/ is owned by another agent.
 */

#include <string.h>

#include "atticpad/atticpad.h"

/* Generated, spec-derived, and read-only: stdint.h is its only include. */
#include "../testdata/vectors.h"

typedef struct {
    apad_selftest_result r;
    apad_selftest_cb     cb;
    void                *user;
    /* Scratch for composed vector case names. On the stack of
     * apad_selftest_run, never returned: `first_failure` is always given the
     * vector's own rodata name, which outlives the call. */
    char                 name[80];
} st_ctx;

/*
 * `stable` is reported as first_failure and MUST outlive the call (a string
 * literal, or a vector's own name in rodata). `shown` is what the callback
 * prints and may point at c->name, which the next case overwrites.
 */
static void st_check2(st_ctx *c, const char *stable, const char *shown, int ok)
{
    c->r.total++;
    if (ok) {
        c->r.passed++;
    } else {
        c->r.failed++;
        if (c->r.first_failure == NULL) {
            c->r.first_failure = stable;
        }
    }
    if (c->cb != NULL) {
        c->cb(c->user, shown, ok ? 1 : 0);
    }
}

static void st_check(st_ctx *c, const char *name, int ok)
{
    st_check2(c, name, name, ok);
}

/*
 * Compose "a/b" or "a/b/d" into c->name. No stdio and no snprintf on any of
 * the eight targets, so this is a bounded hand-rolled join; d may be NULL.
 */
static const char *st_name(st_ctx *c, const char *a, const char *b,
                           const char *d)
{
    const char *part[3];
    size_t n = 0;
    size_t i;

    part[0] = a;
    part[1] = b;
    part[2] = d;

    for (i = 0; i < 3u; i++) {
        const char *p = part[i];
        if (p == NULL) {
            break;
        }
        if (i != 0u && n + 1u < sizeof c->name) {
            c->name[n++] = '/';
        }
        while (*p != '\0' && n + 1u < sizeof c->name) {
            c->name[n++] = *p++;
        }
    }
    c->name[n] = '\0';
    return c->name;
}

/* Local byte readers. Deliberately not shared with codec.c: a test that reuses
 * the implementation's own accessors cannot catch a bug in them. */
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- §9 wrap-safe arithmetic ------------------------------------------- */

static void st_seq(st_ctx *c)
{
    int ok;

    st_check(c, "seq/newer_simple",
             apad_seq_newer(5, 4) == 1 && apad_seq_newer(4, 5) == 0);
    st_check(c, "seq/equal_is_not_newer", apad_seq_newer(7, 7) == 0);

    /* The whole point of §9: forward across 0xFFFF. */
    st_check(c, "seq/wrap_forward_0xFFFF_to_0",
             apad_seq_newer(0x0000u, 0xFFFFu) == 1);
    st_check(c, "seq/wrap_forward_0xFFFE_to_2",
             apad_seq_newer(0x0002u, 0xFFFEu) == 1);
    /* ...and backward, which a naive `>` gets right by accident. */
    st_check(c, "seq/wrap_backward_0_to_0xFFFF",
             apad_seq_newer(0xFFFFu, 0x0000u) == 0);
    st_check(c, "seq/wrap_backward_2_to_0xFFFE",
             apad_seq_newer(0xFFFEu, 0x0002u) == 0);

    /* Half-space boundary: 0x8000 apart is not "newer" under §9's rule. */
    st_check(c, "seq/half_space_not_newer",
             apad_seq_newer(0x8000u, 0x0000u) == 0
             && apad_seq_newer(0x7FFFu, 0x0000u) == 1);

    st_check(c, "seq/diff_signs",
             apad_seq_diff(5, 4) == 1
             && apad_seq_diff(4, 5) == -1
             && apad_seq_diff(0x0000u, 0xFFFFu) == 1
             && apad_seq_diff(0xFFFFu, 0x0000u) == -1
             && apad_seq_diff(0x8000u, 0x0000u) == -32768
             && apad_seq_diff(0x7FFFu, 0x0000u) == 32767);

    st_check(c, "seq/next_wraps", apad_seq_next(0xFFFFu) == 0x0000u);

    /* An exhaustive sweep of the wrap neighbourhood: every naive `>` fails
     * this and every correct implementation passes it. */
    ok = 1;
    {
        uint32_t i;
        for (i = 0; i < 64u; i++) {
            uint16_t base = (uint16_t)(0xFFE0u + i);
            if (!apad_seq_newer((uint16_t)(base + 1u), base)) { ok = 0; }
            if (apad_seq_newer(base, (uint16_t)(base + 1u)))  { ok = 0; }
        }
    }
    st_check(c, "seq/sweep_across_0xFFFF", ok);

    /* §9 tick wrap at 2^32. */
    st_check(c, "time/after_simple", apad_time_after(1000u, 999u) == 1);
    st_check(c, "time/wrap_forward_2^32",
             apad_time_after(0x00000000u, 0xFFFFFFFFu) == 1
             && apad_time_after(0x00000005u, 0xFFFFFFF0u) == 1);
    st_check(c, "time/wrap_backward_2^32",
             apad_time_after(0xFFFFFFFFu, 0x00000000u) == 0
             && apad_time_after(0xFFFFFFF0u, 0x00000005u) == 0);
    st_check(c, "time/equal_is_not_after", apad_time_after(42u, 42u) == 0);
    st_check(c, "time/since_across_wrap",
             apad_time_since(0x00000005u, 0xFFFFFFFBu) == 10u);
    st_check(c, "time/reached_across_wrap",
             apad_time_reached(0x00000000u, 0xFFFFFFFFu) == 1
             && apad_time_reached(0xFFFFFFFFu, 0x00000000u) == 0
             && apad_time_reached(77u, 77u) == 1);
}

/* ---- §5.1 D-pad LUT ---------------------------------------------------- */

static void st_hat(st_ctx *c)
{
    /* Transcribed from §5.1, not from codec.c. */
    static const uint8_t expect[16] = {
        8, 0, 4, 8, 6, 7, 5, 6,
        2, 1, 3, 2, 8, 0, 4, 8
    };
    int ok = 1;
    unsigned i;

    for (i = 0; i < 16u; i++) {
        if (apad_hat_lut[i] != expect[i]) {
            ok = 0;
        }
    }
    st_check(c, "hat/lut_all_16_indices", ok);

    /* The impossible combinations §13 calls out by name. */
    st_check(c, "hat/up_plus_down_is_null",
             apad_hat_from_buttons(APAD_BTN_DPAD_UP | APAD_BTN_DPAD_DOWN)
             == APAD_HAT_NULL);
    st_check(c, "hat/left_plus_right_is_null",
             apad_hat_from_buttons(APAD_BTN_DPAD_LEFT | APAD_BTN_DPAD_RIGHT)
             == APAD_HAT_NULL);
    st_check(c, "hat/none_is_null",
             apad_hat_from_buttons(0u) == APAD_HAT_NULL);
    st_check(c, "hat/up_right_is_NE",
             apad_hat_from_buttons(APAD_BTN_DPAD_UP | APAD_BTN_DPAD_RIGHT)
             == APAD_HAT_NE);
    st_check(c, "hat/down_left_is_SW",
             apad_hat_from_buttons(APAD_BTN_DPAD_DOWN | APAD_BTN_DPAD_LEFT)
             == APAD_HAT_SW);
    /* Non-D-pad bits must not leak into the nibble. */
    st_check(c, "hat/ignores_other_buttons",
             apad_hat_from_buttons(0xFFF0000Fu | APAD_BTN_DPAD_UP)
             == APAD_HAT_N);
}

/* ---- §10 crypto known-answer tests ------------------------------------- */

static void st_crypto(st_ctx *c)
{
    /* FIPS 180-4 sample: SHA-256("abc"). */
    static const uint8_t kat_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    /* FIPS 180-4 sample: the 448-bit message, which exercises the two-block
     * padding path that a 256-byte datagram never reaches on its own. */
    static const uint8_t kat_448[32] = {
        0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
        0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
        0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
        0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
    };
    static const uint8_t kat_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
        0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    /* RFC 4231 test case 1. */
    static const uint8_t hmac_key1[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b
    };
    static const uint8_t hmac_exp1[32] = {
        0xb0,0x34,0x4c,0x61,0xd8,0xdb,0x38,0x53,
        0x5c,0xa8,0xaf,0xce,0xaf,0x0b,0xf1,0x2b,
        0x88,0x1d,0xc2,0x00,0xc9,0x83,0x3d,0xa7,
        0x26,0xe9,0x37,0x6c,0x2e,0x32,0xcf,0xf7
    };
    /* RFC 4231 test case 2 — key shorter than the block, ASCII data. */
    static const uint8_t hmac_exp2[32] = {
        0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,
        0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
        0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,
        0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43
    };
    /* PBKDF2-HMAC-SHA256, P="password" S="salt", dkLen=32. */
    static const uint8_t pbkdf2_c1[32] = {
        0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c,
        0x43,0xe7,0x22,0x52,0x56,0xc4,0xf8,0x37,
        0xa8,0x65,0x48,0xc9,0x2c,0xcc,0x35,0x48,
        0x08,0x05,0x98,0x7c,0xb7,0x0b,0xe1,0x7b
    };
    static const uint8_t pbkdf2_c2[32] = {
        0xae,0x4d,0x0c,0x95,0xaf,0x6b,0x46,0xd3,
        0x2d,0x0a,0xdf,0xf9,0x28,0xf0,0x6d,0xd0,
        0x2a,0x30,0x3f,0x8e,0xf3,0xc2,0x51,0xdf,
        0xd6,0xe2,0xd8,0x5a,0x95,0x47,0x4c,0x43
    };

    uint8_t d[32];
    uint8_t a[8], b[8];

    apad_sha256("abc", 3u, d);
    st_check(c, "sha256/fips_abc", memcmp(d, kat_abc, 32) == 0);

    apad_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56u, d);
    st_check(c, "sha256/fips_448bit_two_blocks", memcmp(d, kat_448, 32) == 0);

    apad_sha256("", 0u, d);
    st_check(c, "sha256/empty", memcmp(d, kat_empty, 32) == 0);

    /* Streaming in awkward chunks must equal the one-shot result. */
    {
        apad_sha256_ctx sc;
        uint8_t d2[32];
        apad_sha256_init(&sc);
        apad_sha256_update(&sc, "a", 1u);
        apad_sha256_update(&sc, "b", 1u);
        apad_sha256_update(&sc, "c", 1u);
        apad_sha256_final(&sc, d2);
        st_check(c, "sha256/streaming_equals_oneshot",
                 memcmp(d2, kat_abc, 32) == 0);
    }

    apad_hmac_sha256(hmac_key1, sizeof hmac_key1, "Hi There", 8u, d);
    st_check(c, "hmac/rfc4231_case1", memcmp(d, hmac_exp1, 32) == 0);

    apad_hmac_sha256((const uint8_t *)"Jefe", 4u,
                     "what do ya want for nothing?", 28u, d);
    st_check(c, "hmac/rfc4231_case2", memcmp(d, hmac_exp2, 32) == 0);

    apad_pbkdf2_sha256((const uint8_t *)"password", 8u,
                       (const uint8_t *)"salt", 4u, 1u, d, 32u);
    st_check(c, "pbkdf2/iter1", memcmp(d, pbkdf2_c1, 32) == 0);

    apad_pbkdf2_sha256((const uint8_t *)"password", 8u,
                       (const uint8_t *)"salt", 4u, 2u, d, 32u);
    st_check(c, "pbkdf2/iter2", memcmp(d, pbkdf2_c2, 32) == 0);

    /* Constant-time compare: correctness only; timing is not testable here. */
    memset(a, 0xA5, sizeof a);
    memset(b, 0xA5, sizeof b);
    st_check(c, "ct_equal/equal", apad_ct_equal(a, b, sizeof a) == 0);
    b[7] = 0xA4u;
    st_check(c, "ct_equal/last_byte_differs", apad_ct_equal(a, b, sizeof a) != 0);
    b[7] = 0xA5u;
    b[0] = 0x00u;
    st_check(c, "ct_equal/first_byte_differs", apad_ct_equal(a, b, sizeof a) != 0);
}

/* ---- §3 header --------------------------------------------------------- */

static void st_header(st_ctx *c)
{
    uint8_t buf[APAD_HEADER_SIZE];
    apad_header h, g;
    int rc;

    memset(&h, 0, sizeof h);
    h.type        = (uint8_t)APAD_MSG_INPUT_STATE;
    h.session_id  = 0xBEEFu;
    h.sequence    = 0x1234u;
    h.payload_len = APAD_LEN_INPUT_STATE;
    h.flags       = APAD_FLAG_AUTHENTICATED;

    rc = apad_header_encode(buf, sizeof buf, &h);
    st_check(c, "header/encode_size", rc == (int)APAD_HEADER_SIZE);

    /* §3: magic is 0x4D43, on the wire as bytes 43 4D. If this fails the
     * platform's endianness assumption is wrong, which is the single most
     * likely thing to be wrong on a new console. */
    st_check(c, "header/magic_bytes_43_4D",
             buf[0] == 0x43u && buf[1] == 0x4Du);
    st_check(c, "header/version_byte", buf[2] == (uint8_t)APAD_VERSION);
    st_check(c, "header/type_byte", buf[3] == (uint8_t)APAD_MSG_INPUT_STATE);
    st_check(c, "header/fields_little_endian",
             le16(buf + 4) == 0xBEEFu
             && le16(buf + 6) == 0x1234u
             && le16(buf + 8) == APAD_LEN_INPUT_STATE
             && le16(buf + 10) == APAD_FLAG_AUTHENTICATED);

    rc = apad_header_decode(buf, sizeof buf, &g);
    st_check(c, "header/roundtrip",
             rc == (int)APAD_HEADER_SIZE
             && g.magic == APAD_MAGIC
             && g.version == (uint8_t)APAD_VERSION
             && g.type == h.type
             && g.session_id == h.session_id
             && g.sequence == h.sequence
             && g.payload_len == h.payload_len
             && g.flags == h.flags);

    /* §2: reserved flag bits are ignored on receive, never rejected. */
    buf[10] = (uint8_t)(buf[10] | 0xFCu);
    buf[11] = 0xFFu;
    rc = apad_header_decode(buf, sizeof buf, &g);
    st_check(c, "header/reserved_flag_bits_ignored_not_rejected",
             rc == (int)APAD_HEADER_SIZE
             && g.flags == APAD_FLAG_AUTHENTICATED);

    /* §2: reserved bits are zero on send even if the caller sets them. */
    h.flags = 0xFFFFu;
    (void)apad_header_encode(buf, sizeof buf, &h);
    st_check(c, "header/reserved_flag_bits_zero_on_send",
             le16(buf + 10) == (APAD_FLAG_AUTHENTICATED | APAD_FLAG_RELIABLE));

    /* §3: magic mismatch and §12: version mismatch are hard rejects. */
    h.flags = 0u;
    (void)apad_header_encode(buf, sizeof buf, &h);
    buf[0] = 0x00u;
    st_check(c, "header/bad_magic_rejected",
             apad_header_decode(buf, sizeof buf, &g) == APAD_ERR_MAGIC);
    buf[0] = 0x43u;
    buf[2] = 99u;
    st_check(c, "header/bad_version_rejected",
             apad_header_decode(buf, sizeof buf, &g) == APAD_ERR_VERSION);
    buf[2] = (uint8_t)APAD_VERSION;
    st_check(c, "header/short_buffer_rejected",
             apad_header_decode(buf, APAD_HEADER_SIZE - 1u, &g)
             == APAD_ERR_TRUNCATED);
}

/* ---- §6 payload round trips -------------------------------------------- */

static void st_payloads(st_ctx *c)
{
    uint8_t buf[APAD_MAX_PAYLOAD];
    unsigned i;

    /* §6.2 ANNOUNCE */
    {
        apad_announce in, out;
        memset(&in, 0, sizeof in);
        apad_text_set(in.server_name, APAD_NAME_LEN, "attic-desk");
        in.pads_total = 4u;
        in.pads_free = 3u;
        in.pairing_required = 1u;
        in.server_port = APAD_DEFAULT_PORT;
        st_check(c, "announce/encode_len",
                 apad_encode_announce(buf, sizeof buf, &in)
                 == (int)APAD_LEN_ANNOUNCE);
        st_check(c, "announce/port_offset_36_le",
                 le16(buf + 36) == APAD_DEFAULT_PORT);
        st_check(c, "announce/reserved_zero",
                 buf[35] == 0u && buf[38] == 0u && buf[39] == 0u);
        st_check(c, "announce/decode_len",
                 apad_decode_announce(buf, APAD_LEN_ANNOUNCE, &out)
                 == (int)APAD_LEN_ANNOUNCE);
        st_check(c, "announce/roundtrip",
                 memcmp(&in, &out, sizeof in) == 0);
        st_check(c, "announce/wrong_length_rejected",
                 apad_decode_announce(buf, APAD_LEN_ANNOUNCE - 1u, &out)
                 == APAD_ERR_LENGTH);

        /* §6.2: "0 or 1; any non-zero value MUST be read as 1." Boundaries
         * are 0 (untouched), 1 (already canonical) and everything above.
         * Driven from the raw wire byte: like §5.5 and §6.8 this is a
         * receive-side rule and the encoder deliberately does not apply it. */
        {
            int ok = 1;
            unsigned v;

            for (v = 0u; v < 256u; v++) {
                uint8_t want = (v != 0u) ? 1u : 0u;
                memset(buf, 0, APAD_LEN_ANNOUNCE);
                buf[34] = (uint8_t)v;
                if (apad_decode_announce(buf, APAD_LEN_ANNOUNCE, &out)
                    != (int)APAD_LEN_ANNOUNCE || out.pairing_required != want) {
                    ok = 0;
                }
            }
            st_check(c, "announce/pairing_required_sweep_all_256", ok);
        }

        memset(buf, 0, APAD_LEN_ANNOUNCE);
        buf[34] = 200u;
        buf[32] = 9u;                     /* neighbours must be untouched */
        buf[33] = 8u;
        (void)apad_decode_announce(buf, APAD_LEN_ANNOUNCE, &out);
        st_check(c, "announce/pairing_required_normalises_neighbours_intact",
                 out.pairing_required == 1u
                 && out.pads_total == 9u && out.pads_free == 8u);

        /* Unlike battery (§5.5) and player_index (§6.8), the ENCODER also
         * canonicalises this one, and the difference is deliberate. Those two
         * have a reserved numeric range, so a caller passing 150 or 7 has a
         * bug worth keeping visible on the wire. pairing_required is a
         * boolean whose API type is uint8_t: `in.pairing_required = flags &
         * 0x80` is idiomatic C, not a bug, and §6.2 defines the field as
         * "0 or 1". Canonicalising a truthy value is not laundering. */
        memset(&in, 0, sizeof in);
        in.pairing_required = 200u;
        (void)apad_encode_announce(buf, sizeof buf, &in);
        st_check(c, "announce/encoder_canonicalises_pairing_required",
                 buf[34] == 1u);
    }

    /* §6.3 HELLO */
    {
        apad_hello in, out;
        memset(&in, 0, sizeof in);
        for (i = 0; i < APAD_CLIENT_ID_LEN; i++) {
            in.client_id[i] = (uint8_t)(0x10u + i);
            in.client_nonce[i] = (uint8_t)(0xA0u + i);
        }
        in.caps = APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_TOUCH
                | APAD_CAP_GYRO | APAD_CAP_BATTERY;
        apad_text_set(in.device_name, APAD_NAME_LEN, "New 3DS");
        in.desired_rate_hz = 125u;
        in.proto_major = (uint8_t)APAD_VERSION;
        in.client_ticks_ms = 0xDEADBEEFu;
        st_check(c, "hello/encode_len",
                 apad_encode_hello(buf, sizeof buf, &in) == (int)APAD_LEN_HELLO);
        st_check(c, "hello/caps_offset_16_le", le32(buf + 16) == in.caps);
        st_check(c, "hello/ticks_offset_72_le",
                 le32(buf + 72) == 0xDEADBEEFu);
        st_check(c, "hello/reserved_zero", buf[71] == 0u);
        st_check(c, "hello/decode_len",
                 apad_decode_hello(buf, APAD_LEN_HELLO, &out)
                 == (int)APAD_LEN_HELLO);
        st_check(c, "hello/roundtrip", memcmp(&in, &out, sizeof in) == 0);

        /* §6.3: caps bits 14..31 reserved — ignored, not rejected. */
        buf[19] = 0xFFu;
        st_check(c, "hello/reserved_caps_bits_ignored",
                 apad_decode_hello(buf, APAD_LEN_HELLO, &out) > 0
                 && out.caps == in.caps);
    }

    /* §6.4 WELCOME */
    {
        apad_welcome in, out;
        memset(&in, 0, sizeof in);
        in.session_id = 0x0501u;
        in.pad_slot = 2u;
        in.flags = APAD_WELCOME_AUTH_REQUIRED;
        in.input_rate_hz = 60u;
        for (i = 0; i < APAD_NONCE_LEN; i++) {
            in.server_nonce[i] = (uint8_t)(0x5Au ^ i);
        }
        in.server_ticks_ms = 0x01020304u;
        st_check(c, "welcome/encode_len",
                 apad_encode_welcome(buf, sizeof buf, &in)
                 == (int)APAD_LEN_WELCOME);
        st_check(c, "welcome/ticks_offset_56_le",
                 le32(buf + 56) == 0x01020304u);
        {
            int zero = 1;
            for (i = 24u; i < 56u; i++) {
                if (buf[i] != 0u) { zero = 0; }
            }
            /* §6.4: key_material MUST be zero in v1. */
            st_check(c, "welcome/key_material_zero_in_v1", zero);
        }
        st_check(c, "welcome/reserved_zero", buf[6] == 0u && buf[7] == 0u);
        st_check(c, "welcome/decode_len",
                 apad_decode_welcome(buf, APAD_LEN_WELCOME, &out)
                 == (int)APAD_LEN_WELCOME);
        st_check(c, "welcome/roundtrip", memcmp(&in, &out, sizeof in) == 0);

        /* §6.4: key_material MUST be ignored on receive. */
        memset(buf + 24, 0xFF, 32u);
        st_check(c, "welcome/key_material_ignored_on_receive",
                 apad_decode_welcome(buf, APAD_LEN_WELCOME, &out) > 0
                 && memcmp(&in, &out, sizeof in) == 0);
    }

    /* §6.5 BYE */
    {
        apad_bye in, out;
        memset(&in, 0, sizeof in);
        in.reason = (uint8_t)APAD_BYE_SLOT_REVOKED;
        st_check(c, "bye/encode_len",
                 apad_encode_bye(buf, sizeof buf, &in) == (int)APAD_LEN_BYE);
        st_check(c, "bye/reserved_zero",
                 buf[1] == 0u && buf[2] == 0u && buf[3] == 0u);
        st_check(c, "bye/roundtrip",
                 apad_decode_bye(buf, APAD_LEN_BYE, &out) == (int)APAD_LEN_BYE
                 && memcmp(&in, &out, sizeof in) == 0);
    }

    /* §6.6 PING / PONG share a layout. */
    {
        apad_ping in, out;
        memset(&in, 0, sizeof in);
        in.origin_ticks_ms = 0xFFFFFFF0u;   /* near the 2^32 wrap on purpose */
        in.responder_ticks_ms = 0u;
        st_check(c, "ping/encode_len",
                 apad_encode_ping(buf, sizeof buf, &in) == (int)APAD_LEN_PING);
        st_check(c, "ping/origin_le", le32(buf + 0) == 0xFFFFFFF0u);
        st_check(c, "ping/responder_zero_in_ping", le32(buf + 4) == 0u);
        st_check(c, "ping/roundtrip",
                 apad_decode_ping(buf, APAD_LEN_PING, &out) == (int)APAD_LEN_PING
                 && memcmp(&in, &out, sizeof in) == 0);
        in.responder_ticks_ms = 0x00000005u;
        (void)apad_encode_ping(buf, sizeof buf, &in);
        (void)apad_decode_ping(buf, APAD_LEN_PING, &out);
        /* §6.6: RTT is computed by the original sender with §9 subtraction. */
        st_check(c, "pong/rtt_across_wrap",
                 apad_time_since(out.responder_ticks_ms, out.origin_ticks_ms)
                 == 21u);
    }

    /* §6.7 RUMBLE */
    {
        apad_rumble in, out;
        memset(&in, 0, sizeof in);
        in.low_freq = 0xFFFFu;
        in.high_freq = 0x8000u;
        in.duration_ms = 0u;   /* until superseded */
        st_check(c, "rumble/encode_len",
                 apad_encode_rumble(buf, sizeof buf, &in)
                 == (int)APAD_LEN_RUMBLE);
        st_check(c, "rumble/reserved_zero", buf[6] == 0u && buf[7] == 0u);
        st_check(c, "rumble/roundtrip",
                 apad_decode_rumble(buf, APAD_LEN_RUMBLE, &out)
                 == (int)APAD_LEN_RUMBLE
                 && memcmp(&in, &out, sizeof in) == 0);
    }

    /* §6.8 LED */
    {
        apad_led in, out;
        memset(&in, 0, sizeof in);
        in.player_index = 3u;
        in.r = 0x10u; in.g = 0x20u; in.b = 0x30u;
        st_check(c, "led/encode_len",
                 apad_encode_led(buf, sizeof buf, &in) == (int)APAD_LEN_LED);
        st_check(c, "led/byte_order",
                 buf[0] == 3u && buf[1] == 0x10u && buf[2] == 0x20u
                 && buf[3] == 0x30u);
        st_check(c, "led/roundtrip",
                 apad_decode_led(buf, APAD_LEN_LED, &out) == (int)APAD_LEN_LED
                 && memcmp(&in, &out, sizeof in) == 0);

        /* §6.8: "Values above 4 are reserved and MUST be treated as 0 (off)
         * on receive." Boundaries: 0 is off, 1 and 4 are the ends of the
         * valid player range, 5 is the first reserved value, 255 the last.
         * Driven from the raw wire byte, because this is receive-side only
         * and the encoder deliberately does not apply it. */
        {
            static const uint8_t wire[5]   = { 0u, 1u, 4u, 5u, 255u };
            static const uint8_t expect[5] = { 0u, 1u, 4u, 0u, 0u };
            int ok = 1;
            size_t k;

            for (k = 0; k < 5u; k++) {
                memset(buf, 0, APAD_LEN_LED);
                buf[0] = wire[k];
                if (apad_decode_led(buf, APAD_LEN_LED, &out)
                    != (int)APAD_LEN_LED || out.player_index != expect[k]) {
                    ok = 0;
                }
            }
            st_check(c, "led/player_index_reserved_boundaries", ok);
        }

        /* The same rule swept over all 256 wire values. */
        {
            int ok = 1;
            unsigned v;

            for (v = 0u; v < 256u; v++) {
                uint8_t want = (v <= 4u) ? (uint8_t)v : (uint8_t)0u;
                memset(buf, 0, APAD_LEN_LED);
                buf[0] = (uint8_t)v;
                (void)apad_decode_led(buf, APAD_LEN_LED, &out);
                if (out.player_index != want) {
                    ok = 0;
                }
            }
            st_check(c, "led/player_index_sweep_all_256_wire_values", ok);
        }

        /* Normalisation must not touch the colour bytes: r/g/b have no
         * reserved values, and 0xFF is a legal full-brightness channel. */
        memset(buf, 0, APAD_LEN_LED);
        buf[0] = 200u; buf[1] = 0xFFu; buf[2] = 0xFFu; buf[3] = 0xFFu;
        (void)apad_decode_led(buf, APAD_LEN_LED, &out);
        st_check(c, "led/reserved_index_does_not_touch_colour",
                 out.player_index == 0u && out.r == 0xFFu
                 && out.g == 0xFFu && out.b == 0xFFu);

        /* §6.8 is a DECODE rule: the encoder passes a reserved value through
         * so a buggy sender stays visible rather than being laundered. */
        memset(&in, 0, sizeof in);
        in.player_index = 7u;
        (void)apad_encode_led(buf, sizeof buf, &in);
        st_check(c, "led/encoder_does_not_normalise", buf[0] == 7u);
        (void)apad_decode_led(buf, APAD_LEN_LED, &out);
        st_check(c, "led/reserved_index_does_not_round_trip",
                 out.player_index == 0u);
    }

    /* §6.9 STATUS */
    {
        apad_status in, out;
        char tmp[APAD_TEXT_LEN + 1u];
        memset(&in, 0, sizeof in);
        in.code = (uint8_t)APAD_STATUS_WARNING;
        apad_text_set(in.text, APAD_TEXT_LEN, "mDNS disabled: port 5353 busy");
        st_check(c, "status/encode_len",
                 apad_encode_status(buf, sizeof buf, &in)
                 == (int)APAD_LEN_STATUS);
        st_check(c, "status/reserved_zero",
                 buf[1] == 0u && buf[2] == 0u && buf[3] == 0u);
        st_check(c, "status/roundtrip",
                 apad_decode_status(buf, APAD_LEN_STATUS, &out)
                 == (int)APAD_LEN_STATUS
                 && memcmp(&in, &out, sizeof in) == 0);
        apad_text_get(tmp, sizeof tmp, out.text, APAD_TEXT_LEN);
        st_check(c, "status/text_readback",
                 apad_text_len(out.text, APAD_TEXT_LEN) == 29u
                 && tmp[0] == 'm' && tmp[29] == '\0');

        /* §2: a text field that fills its width has no terminator. */
        for (i = 0; i < APAD_TEXT_LEN; i++) {
            in.text[i] = (char)('A' + (int)(i % 26u));
        }
        (void)apad_encode_status(buf, sizeof buf, &in);
        (void)apad_decode_status(buf, APAD_LEN_STATUS, &out);
        st_check(c, "status/text_fills_width_no_terminator",
                 apad_text_len(out.text, APAD_TEXT_LEN) == APAD_TEXT_LEN
                 && memcmp(out.text, in.text, APAD_TEXT_LEN) == 0);
        apad_text_get(tmp, sizeof tmp, out.text, APAD_TEXT_LEN);
        st_check(c, "status/text_get_terminates",
                 tmp[APAD_TEXT_LEN] == '\0');
    }

    /* §6.10 ACK */
    {
        apad_ack in, out;
        memset(&in, 0, sizeof in);
        in.sequence = 0xFFFFu;
        st_check(c, "ack/encode_len",
                 apad_encode_ack(buf, sizeof buf, &in) == (int)APAD_LEN_ACK);
        st_check(c, "ack/reserved_zero", buf[2] == 0u && buf[3] == 0u);
        st_check(c, "ack/roundtrip",
                 apad_decode_ack(buf, APAD_LEN_ACK, &out) == (int)APAD_LEN_ACK
                 && out.sequence == 0xFFFFu);
    }

    /* §6.11 ERROR */
    {
        apad_error in, out;
        memset(&in, 0, sizeof in);
        in.code = (uint16_t)APAD_ERRC_VERSION_MISMATCH;
        apad_text_set(in.text, APAD_TEXT_LEN, "server is v1, client is v2");
        st_check(c, "error/encode_len",
                 apad_encode_error(buf, sizeof buf, &in)
                 == (int)APAD_LEN_ERROR);
        st_check(c, "error/code_le", le16(buf + 0) == 1u);
        st_check(c, "error/reserved_zero", buf[2] == 0u && buf[3] == 0u);
        st_check(c, "error/roundtrip",
                 apad_decode_error(buf, APAD_LEN_ERROR, &out)
                 == (int)APAD_LEN_ERROR
                 && memcmp(&in, &out, sizeof in) == 0);
    }

    /* §4: DISCOVER carries no payload. */
    st_check(c, "discover/payload_size_zero",
             apad_payload_size((uint8_t)APAD_MSG_DISCOVER) == 0);

    /* §4: the whole size table, transcribed from the spec. */
    st_check(c, "types/payload_size_table",
             apad_payload_size(0x01u) == 0
             && apad_payload_size(0x02u) == 40
             && apad_payload_size(0x10u) == 76
             && apad_payload_size(0x11u) == 60
             && apad_payload_size(0x12u) == 4
             && apad_payload_size(0x20u) == 56
             && apad_payload_size(0x30u) == 8
             && apad_payload_size(0x31u) == 8
             && apad_payload_size(0x40u) == 8
             && apad_payload_size(0x41u) == 4
             && apad_payload_size(0x42u) == 64
             && apad_payload_size(0x50u) == 4
             && apad_payload_size(0x51u) == 64);
    st_check(c, "types/unknown_type_reported",
             apad_payload_size(0x00u) == APAD_ERR_TYPE
             && apad_payload_size(0x21u) == APAD_ERR_TYPE
             && apad_payload_size(0xFFu) == APAD_ERR_TYPE);

    /* §6.0: BYE.reason, STATUS.code and ERROR.code carry NO clamp. A receiver
     * MUST preserve an unrecognised value verbatim and MUST NOT reject the
     * packet for carrying one — they are diagnostic labels, not control
     * inputs, and a v1.1 peer may add codes. This case exists to fail if
     * someone later "helpfully" normalises them for consistency with the
     * three fields in §6.0 that ARE clamped. ERROR code 0 is reserved as
     * unassigned and is likewise preserved, not rewritten. */
    {
        apad_bye    b2;
        apad_status s2;
        apad_error  e2;

        memset(buf, 0, APAD_LEN_BYE);
        buf[0] = 200u;                 /* far outside enum apad_bye_reason */
        st_check(c, "bye/unknown_reason_preserved_not_clamped",
                 apad_decode_bye(buf, APAD_LEN_BYE, &b2) == (int)APAD_LEN_BYE
                 && b2.reason == 200u);

        memset(buf, 0, APAD_LEN_STATUS);
        buf[0] = 99u;                  /* outside enum apad_status_code */
        st_check(c, "status/unknown_code_preserved_not_clamped",
                 apad_decode_status(buf, APAD_LEN_STATUS, &s2)
                 == (int)APAD_LEN_STATUS && s2.code == 99u);

        memset(buf, 0, APAD_LEN_ERROR);
        buf[0] = 0xE8u; buf[1] = 0x03u;              /* 1000, unassigned */
        st_check(c, "error/unknown_code_preserved_not_clamped",
                 apad_decode_error(buf, APAD_LEN_ERROR, &e2)
                 == (int)APAD_LEN_ERROR && e2.code == 1000u);

        memset(buf, 0, APAD_LEN_ERROR);              /* code 0: reserved */
        st_check(c, "error/code_zero_preserved_not_rejected",
                 apad_decode_error(buf, APAD_LEN_ERROR, &e2)
                 == (int)APAD_LEN_ERROR && e2.code == 0u);
    }
}

/* ---- §5 INPUT_STATE ---------------------------------------------------- */

static void st_input(st_ctx *c)
{
    uint8_t buf[APAD_LEN_INPUT_STATE];
    apad_input_state in, out;
    int i;

    /* §5 and §14.2: 56 bytes, not 48. */
    st_check(c, "input/payload_is_56_bytes",
             APAD_LEN_INPUT_STATE == 56u
             && apad_payload_size((uint8_t)APAD_MSG_INPUT_STATE) == 56);

    memset(&in, 0, sizeof in);
    in.buttons = APAD_BTN_A | APAD_BTN_DPAD_LEFT | APAD_BTN_START
               | APAD_BTN_HOME | APAD_BTN_CAPTURE;
    in.axes[APAD_AXIS_LX] = -32768;
    in.axes[APAD_AXIS_LY] =  32767;
    in.axes[APAD_AXIS_RX] = -1;
    in.axes[APAD_AXIS_RY] =  1;
    in.axes[APAD_AXIS_L2] =  0;
    in.axes[APAD_AXIS_R2] =  32767;
    in.touch_count = 2u;
    in.touches[0].id = 7u;
    in.touches[0].pressure = 200u;
    in.touches[0].x = -12345;
    in.touches[0].y =  23456;
    in.touches[1].id = 8u;
    in.touches[1].pressure = 0u;
    in.touches[1].x = 32767;
    in.touches[1].y = -32768;
    in.accel[0] = -1000; in.accel[1] = 0; in.accel[2] = 1000;
    in.gyro[0] = 32767; in.gyro[1] = -32768; in.gyro[2] = 0;
    in.battery = 87u;
    in.client_ticks_ms = 0xFFFFFFFFu;

    st_check(c, "input/encode_len",
             apad_encode_input_state(buf, sizeof buf, &in)
             == (int)APAD_LEN_INPUT_STATE);

    /* Spot-check the offsets from §5 against the bytes, one field per row. */
    st_check(c, "input/buttons_offset_0_le", le32(buf + 0) == in.buttons);
    st_check(c, "input/axes_offset_4_le",
             le16(buf + 4) == 0x8000u        /* -32768 */
             && le16(buf + 6) == 0x7FFFu);   /*  32767 */
    st_check(c, "input/touch_count_offset_20", buf[20] == 2u);
    st_check(c, "input/touch0_offset_22",
             buf[22] == 7u && buf[23] == 200u);
    st_check(c, "input/touch1_offset_28", buf[28] == 8u);
    st_check(c, "input/accel_offset_34",
             le16(buf + 34) == 0xFC18u);     /* -1000 */
    st_check(c, "input/gyro_offset_40", le16(buf + 40) == 0x7FFFu);
    st_check(c, "input/battery_offset_46", buf[46] == 87u);
    st_check(c, "input/ticks_offset_52_le", le32(buf + 52) == 0xFFFFFFFFu);
    st_check(c, "input/reserved_zero_on_send",
             buf[21] == 0u
             && buf[16] == 0u && buf[17] == 0u    /* axes[6] */
             && buf[18] == 0u && buf[19] == 0u    /* axes[7] */
             && buf[47] == 0u && buf[48] == 0u && buf[49] == 0u
             && buf[50] == 0u && buf[51] == 0u);

    st_check(c, "input/decode_len",
             apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out)
             == (int)APAD_LEN_INPUT_STATE);
    st_check(c, "input/roundtrip", memcmp(&in, &out, sizeof in) == 0);

    /* §5: touch_count > 2 clamped to 2 on receive. */
    buf[20] = 200u;
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/touch_count_gt_2_clamped", out.touch_count == 2u);

    /* §5.2: entries at index >= touch_count are ignored on receive. */
    buf[20] = 1u;
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/touch_entries_past_count_ignored",
             out.touch_count == 1u
             && out.touches[1].id == 0u && out.touches[1].pressure == 0u
             && out.touches[1].x == 0 && out.touches[1].y == 0);
    buf[20] = 2u;

    /* §5: negative axes[4]/axes[5] treated as 0 on receive. */
    buf[12] = 0x00u; buf[13] = 0x80u;   /* axes[4] = -32768 */
    buf[14] = 0xFFu; buf[15] = 0xFFu;   /* axes[5] = -1     */
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/negative_triggers_clamped_to_zero",
             out.axes[APAD_AXIS_L2] == 0 && out.axes[APAD_AXIS_R2] == 0);

    /* §5.1: buttons bits 20..31 reserved — ignored, not rejected. */
    buf[2] = 0xF0u; buf[3] = 0xFFu;
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/reserved_button_bits_ignored",
             (out.buttons & ~(uint32_t)APAD_BTN_VALID_MASK) == 0u);

    /* §5: axes[6]/axes[7] reserved — ignored, not rejected. */
    buf[16] = 0xFFu; buf[17] = 0xFFu; buf[18] = 0xFFu; buf[19] = 0xFFu;
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/reserved_axes_ignored",
             out.axes[6] == 0 && out.axes[7] == 0);

    /* §5: reserved1/reserved2 set — ignored, not rejected. */
    for (i = 47; i < 52; i++) {
        buf[i] = 0xFFu;
    }
    buf[21] = 0xFFu;
    st_check(c, "input/reserved_bytes_ignored_not_rejected",
             apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out)
             == (int)APAD_LEN_INPUT_STATE);

    /* §5: battery 255 means unknown and must survive intact. */
    memset(&in, 0, sizeof in);
    in.battery = APAD_BATTERY_UNKNOWN;
    (void)apad_encode_input_state(buf, sizeof buf, &in);
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/battery_255_unknown",
             out.battery == APAD_BATTERY_UNKNOWN);

    /* §5.5: 101..254 are reserved and MUST normalise to 255 in the decoded
     * output. The four boundaries are the whole rule: 100 is the last real
     * percentage, 101 is the first reserved value, 254 is the last, and 255
     * is the sentinel they collapse onto. Written against the raw wire byte
     * rather than through the encoder, because §5.5 is receive-side only and
     * the encoder deliberately does not apply it. */
    {
        static const uint8_t wire[6]   = { 0u, 100u, 101u, 254u, 255u, 200u };
        static const uint8_t expect[6] = { 0u, 100u, 255u, 255u, 255u, 255u };
        int ok = 1;
        size_t k;

        for (k = 0; k < 6u; k++) {
            memset(buf, 0, sizeof buf);
            buf[46] = wire[k];
            if (apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out)
                != (int)APAD_LEN_INPUT_STATE) {
                ok = 0;
            } else if (out.battery != expect[k]) {
                ok = 0;
            }
        }
        st_check(c, "input/battery_reserved_range_boundaries", ok);
    }

    /* The same rule swept over all 256 wire values, so no single value can
     * be normalised the wrong way without this failing. */
    {
        int ok = 1;
        unsigned v;

        for (v = 0u; v < 256u; v++) {
            uint8_t want = (v <= 100u) ? (uint8_t)v : (uint8_t)APAD_BATTERY_UNKNOWN;
            memset(buf, 0, sizeof buf);
            buf[46] = (uint8_t)v;
            (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
            if (out.battery != want) {
                ok = 0;
            }
        }
        st_check(c, "input/battery_sweep_all_256_wire_values", ok);
    }

    /* §5.5 is a DECODE rule. The encoder must still put the caller's value on
     * the wire unchanged: a non-conforming sender stays visible instead of
     * being silently laundered into "unknown". */
    memset(&in, 0, sizeof in);
    in.battery = 150u;
    (void)apad_encode_input_state(buf, sizeof buf, &in);
    st_check(c, "input/battery_encoder_does_not_normalise", buf[46] == 150u);
    (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
    st_check(c, "input/battery_reserved_does_not_round_trip",
             out.battery == APAD_BATTERY_UNKNOWN);

    /* §5: encoder clamps too, so reserved is zero on send. */
    in.touch_count = 9u;
    in.axes[APAD_AXIS_L2] = -5;
    in.buttons = 0xFFFFFFFFu;
    (void)apad_encode_input_state(buf, sizeof buf, &in);
    st_check(c, "input/encoder_normalises",
             buf[20] == 2u
             && le16(buf + 12) == 0u
             && le32(buf + 0) == APAD_BTN_VALID_MASK);

    /* Every i16 axis value must survive a round trip, including -32768,
     * which has no positive counterpart (§5.4). */
    {
        static const int16_t vals[7] = {
            -32768, -32767, -1, 0, 1, 32766, 32767
        };
        int ok = 1;
        int k;
        for (k = 0; k < 7; k++) {
            memset(&in, 0, sizeof in);
            in.axes[APAD_AXIS_LX] = vals[k];
            in.accel[1] = vals[k];
            in.touch_count = 1u;
            in.touches[0].y = vals[k];
            (void)apad_encode_input_state(buf, sizeof buf, &in);
            (void)apad_decode_input_state(buf, APAD_LEN_INPUT_STATE, &out);
            if (out.axes[APAD_AXIS_LX] != vals[k]
                || out.accel[1] != vals[k]
                || out.touches[0].y != vals[k]) {
                ok = 0;
            }
        }
        st_check(c, "input/i16_extremes_roundtrip", ok);
    }

    /* Wrong length is rejected in both directions. */
    st_check(c, "input/short_payload_rejected",
             apad_decode_input_state(buf, 48u, &out) == APAD_ERR_LENGTH);
    st_check(c, "input/long_payload_rejected",
             apad_decode_input_state(buf, 57u, &out) == APAD_ERR_LENGTH);
    st_check(c, "input/small_encode_buffer_rejected",
             apad_encode_input_state(buf, 55u, &in) == APAD_ERR_BUFFER);
}

/* ---- §3, §10 framing --------------------------------------------------- */

static void st_framing(st_ctx *c)
{
    uint8_t dgram[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_MAX_PAYLOAD];
    uint8_t key[APAD_SESSION_KEY_LEN];
    apad_header h;
    apad_packet pkt;
    apad_input_state st;
    int total;
    unsigned i;

    for (i = 0; i < sizeof key; i++) {
        key[i] = (uint8_t)(i * 7u);
    }

    /* Zero-length payload: DISCOVER, unauthenticated. */
    memset(&h, 0, sizeof h);
    h.type = (uint8_t)APAD_MSG_DISCOVER;
    h.session_id = 0u;    /* §6.1: MUST be 0 */
    h.sequence = 1u;
    total = apad_packet_build(dgram, sizeof dgram, &h, NULL, 0u, NULL, 0u);
    st_check(c, "frame/discover_is_12_bytes", total == (int)APAD_HEADER_SIZE);
    st_check(c, "frame/discover_parses",
             apad_packet_parse(dgram, (size_t)total, &pkt) == total
             && pkt.payload == NULL && pkt.payload_len == 0u
             && pkt.tag == NULL);

    /* INPUT_STATE, unauthenticated. */
    memset(&st, 0, sizeof st);
    st.buttons = APAD_BTN_B;
    st.client_ticks_ms = 12345u;
    (void)apad_encode_input_state(payload, sizeof payload, &st);
    memset(&h, 0, sizeof h);
    h.type = (uint8_t)APAD_MSG_INPUT_STATE;
    h.session_id = 0x0102u;
    h.sequence = 0xFFFEu;
    total = apad_packet_build(dgram, sizeof dgram, &h, payload,
                              APAD_LEN_INPUT_STATE, NULL, 0u);
    st_check(c, "frame/input_state_total_68",
             total == (int)(APAD_HEADER_SIZE + APAD_LEN_INPUT_STATE));
    st_check(c, "frame/input_state_parses",
             apad_packet_parse(dgram, (size_t)total, &pkt) == total
             && pkt.header.type == (uint8_t)APAD_MSG_INPUT_STATE
             && pkt.payload_len == APAD_LEN_INPUT_STATE
             && pkt.payload == dgram + APAD_HEADER_SIZE);

    /* §3: length MUST equal 12 + payload_len + (auth ? 8 : 0). */
    st_check(c, "frame/one_byte_short_rejected",
             apad_packet_parse(dgram, (size_t)total - 1u, &pkt)
             == APAD_ERR_LENGTH);
    st_check(c, "frame/one_byte_long_rejected",
             apad_packet_parse(dgram, (size_t)total + 1u, &pkt)
             == APAD_ERR_LENGTH);

    /* §13: truncation at every byte offset must be rejected, never parsed. */
    {
        int ok = 1;
        size_t n;
        for (n = 0; n < (size_t)total; n++) {
            if (apad_packet_parse(dgram, n, &pkt) >= 0) {
                ok = 0;
            }
        }
        st_check(c, "frame/truncation_at_every_offset_rejected", ok);
    }

    /* payload_len that disagrees with the datagram is rejected. */
    dgram[8] = 0xFFu; dgram[9] = 0x00u;
    st_check(c, "frame/payload_len_mismatch_rejected",
             apad_packet_parse(dgram, (size_t)total, &pkt) == APAD_ERR_LENGTH);
    dgram[8] = (uint8_t)APAD_LEN_INPUT_STATE; dgram[9] = 0u;

    /* §12: a known type with the wrong fixed payload size is malformed. */
    dgram[3] = (uint8_t)APAD_MSG_LED;   /* LED is 4 bytes, not 56 */
    st_check(c, "frame/known_type_wrong_size_rejected",
             apad_packet_parse(dgram, (size_t)total, &pkt) == APAD_ERR_LENGTH);

    /* §4: unknown type is reported so the caller can discard it silently. */
    dgram[3] = 0x77u;
    st_check(c, "frame/unknown_type_reported",
             apad_packet_parse(dgram, (size_t)total, &pkt) == APAD_ERR_TYPE);
    dgram[3] = (uint8_t)APAD_MSG_INPUT_STATE;

    /* §1: nothing above 256 bytes ever. */
    st_check(c, "frame/max_datagram_unauth",
             apad_packet_build(dgram, sizeof dgram, &h, payload,
                               (uint16_t)APAD_MAX_PAYLOAD, NULL, 0u)
             == (int)APAD_MAX_DATAGRAM);
    st_check(c, "frame/over_max_datagram_rejected",
             apad_packet_build(dgram, sizeof dgram, &h, payload,
                               (uint16_t)(APAD_MAX_PAYLOAD + 1u), NULL, 0u)
             == APAD_ERR_LENGTH);
    st_check(c, "frame/max_datagram_auth",
             apad_packet_build(dgram, sizeof dgram, &h, payload,
                               (uint16_t)APAD_MAX_PAYLOAD_AUTH, key, sizeof key)
             == (int)APAD_MAX_DATAGRAM);
    st_check(c, "frame/over_max_datagram_auth_rejected",
             apad_packet_build(dgram, sizeof dgram, &h, payload,
                               (uint16_t)(APAD_MAX_PAYLOAD_AUTH + 1u),
                               key, sizeof key)
             == APAD_ERR_LENGTH);
    st_check(c, "frame/small_output_buffer_rejected",
             apad_packet_build(dgram, 16u, &h, payload,
                               APAD_LEN_INPUT_STATE, NULL, 0u)
             == APAD_ERR_BUFFER);

    /* §10: authenticated datagram — tag over the whole thing with the tag
     * region zeroed, verified in constant time. */
    memset(&h, 0, sizeof h);
    h.type = (uint8_t)APAD_MSG_RUMBLE;
    h.session_id = 0x2222u;
    h.sequence = 9u;
    h.flags = APAD_FLAG_RELIABLE;
    {
        apad_rumble r;
        memset(&r, 0, sizeof r);
        r.low_freq = 0x1234u;
        r.high_freq = 0x5678u;
        r.duration_ms = 250u;
        (void)apad_encode_rumble(payload, sizeof payload, &r);
    }
    total = apad_packet_build(dgram, sizeof dgram, &h, payload,
                              APAD_LEN_RUMBLE, key, sizeof key);
    st_check(c, "auth/total_is_12_plus_8_plus_8",
             total == (int)(APAD_HEADER_SIZE + APAD_LEN_RUMBLE + APAD_TAG_SIZE));
    st_check(c, "auth/flag_set_by_build",
             (le16(dgram + 10) & APAD_FLAG_AUTHENTICATED) != 0u
             && (le16(dgram + 10) & APAD_FLAG_RELIABLE) != 0u);
    st_check(c, "auth/parses",
             apad_packet_parse(dgram, (size_t)total, &pkt) == total
             && pkt.tag == dgram + total - (int)APAD_TAG_SIZE);
    st_check(c, "auth/verify_ok",
             apad_packet_verify(dgram, (size_t)total, key, sizeof key)
             == APAD_OK);

    /* Every single-bit flip anywhere in the datagram must fail verification. */
    {
        int ok = 1;
        int n;
        for (n = 0; n < total; n++) {
            dgram[n] = (uint8_t)(dgram[n] ^ 0x01u);
            if (apad_packet_verify(dgram, (size_t)total, key, sizeof key)
                != APAD_ERR_AUTH) {
                ok = 0;
            }
            dgram[n] = (uint8_t)(dgram[n] ^ 0x01u);
        }
        st_check(c, "auth/every_bit_flip_detected", ok);
    }
    st_check(c, "auth/still_verifies_after_restore",
             apad_packet_verify(dgram, (size_t)total, key, sizeof key)
             == APAD_OK);
    {
        uint8_t wrong[APAD_SESSION_KEY_LEN];
        memcpy(wrong, key, sizeof wrong);
        wrong[0] = (uint8_t)(wrong[0] ^ 0x80u);
        st_check(c, "auth/wrong_key_rejected",
                 apad_packet_verify(dgram, (size_t)total, wrong, sizeof wrong)
                 == APAD_ERR_AUTH);
    }
    /* An unauthenticated datagram cannot satisfy a verify. */
    total = apad_packet_build(dgram, sizeof dgram, &h, payload,
                              APAD_LEN_RUMBLE, NULL, 0u);
    st_check(c, "auth/unauthenticated_fails_verify",
             apad_packet_verify(dgram, (size_t)total, key, sizeof key)
             == APAD_ERR_AUTH);

    /* §10: the derived key is a pure function of PIN and server_nonce. */
    {
        uint8_t k1[APAD_SESSION_KEY_LEN];
        uint8_t k2[APAD_SESSION_KEY_LEN];
        uint8_t nonce[APAD_NONCE_LEN];
        for (i = 0; i < APAD_NONCE_LEN; i++) {
            nonce[i] = (uint8_t)i;
        }
        /* 10,000 iterations is ~1 s on ARM9 (docs/DESIGN.md D3), so the self-test
         * uses the primitive directly at a low count and checks only that the
         * key derivation is deterministic and salt-sensitive. */
        apad_pbkdf2_sha256((const uint8_t *)"123456", 6u, nonce, sizeof nonce,
                           64u, k1, sizeof k1);
        apad_pbkdf2_sha256((const uint8_t *)"123456", 6u, nonce, sizeof nonce,
                           64u, k2, sizeof k2);
        st_check(c, "pbkdf2/deterministic", memcmp(k1, k2, sizeof k1) == 0);
        nonce[0] = 0xFFu;
        apad_pbkdf2_sha256((const uint8_t *)"123456", 6u, nonce, sizeof nonce,
                           64u, k2, sizeof k2);
        st_check(c, "pbkdf2/salt_changes_key", memcmp(k1, k2, sizeof k1) != 0);
    }

    /* §3.1 ordering vs §1's 256-byte cap. The seven checks are ordered and a
     * receiver MUST stop at the first failure; §1's cap is NOT one of them.
     * Testing the cap first made an oversize datagram with garbage magic
     * return a rejection (ERROR code 6) where §3.1 check 2 requires a silent
     * discard — and the server turns "reject" into a reply, so the deviation
     * was visible on the wire. Unreachable through apad_udp_recv, whose
     * buffer is 256 bytes, but every port calls this API directly. */
    {
        uint8_t big[APAD_HEADER_SIZE + 252u];   /* 264: over the §1 cap */
        apad_packet bp;

        /* Zeroed so the "no output" check below is a real observation about
         * apad_packet_parse rather than a read of whatever was on the stack. */
        memset(&bp, 0, sizeof bp);
        memset(big, 0, sizeof big);
        big[APAD_OFF_MAGIC + 0] = 0x43u;        /* "MC", little-endian */
        big[APAD_OFF_MAGIC + 1] = 0x4Du;
        big[APAD_OFF_VERSION]   = (uint8_t)APAD_VERSION;
        big[APAD_OFF_TYPE]      = (uint8_t)APAD_MSG_INPUT_STATE;
        big[APAD_OFF_PAYLOAD_LEN + 0] = (uint8_t)(252u & 0xFFu);
        big[APAD_OFF_PAYLOAD_LEN + 1] = (uint8_t)(252u >> 8);

        /* check 2 before the cap: garbage magic is a DISCARD, not a reject. */
        big[APAD_OFF_MAGIC + 0] = 0x00u;
        st_check(c, "frame/oversize_bad_magic_discards_not_rejects",
                 apad_packet_parse(big, sizeof big, &bp) == APAD_ERR_MAGIC);
        big[APAD_OFF_MAGIC + 0] = 0x43u;

        /* check 3 before the cap: version mismatch is ERROR code 1, not 6. */
        big[APAD_OFF_VERSION] = 2u;
        st_check(c, "frame/oversize_bad_version_is_version_error",
                 apad_packet_parse(big, sizeof big, &bp) == APAD_ERR_VERSION);
        big[APAD_OFF_VERSION] = (uint8_t)APAD_VERSION;

        /* check 5 before the cap: this datagram is self-consistent under
         * check 4 (264 == 12 + 252), so an unknown type MUST be discarded
         * silently rather than rejected for being oversize. */
        big[APAD_OFF_TYPE] = 0x99u;
        st_check(c, "frame/oversize_unknown_type_discards_not_rejects",
                 apad_packet_parse(big, sizeof big, &bp) == APAD_ERR_TYPE);
        big[APAD_OFF_TYPE] = (uint8_t)APAD_MSG_INPUT_STATE;

        /* A known type that survives checks 1-5 is still refused: §1 caps the
         * datagram at 256 bytes, and check 6 would refuse it anyway. */
        st_check(c, "frame/oversize_known_type_is_length_error",
                 apad_packet_parse(big, sizeof big, &bp) == APAD_ERR_LENGTH);

        /* A rejected oversize datagram must not produce output either. */
        st_check(c, "frame/oversize_produces_no_output",
                 bp.payload == NULL && bp.tag == NULL
                 && bp.payload_len == 0u);
    }
}

/* ---- §8, §9 session ---------------------------------------------------- */

static void st_session(st_ctx *c)
{
    apad_session s;
    apad_header h;
    apad_packet pkt;
    uint8_t dgram[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_WELCOME];
    int total;

    apad_session_init(&s, 0, 1000u);
    st_check(c, "session/starts_idle",
             s.state == (uint8_t)APAD_SESSION_IDLE && s.session_id == 0u);

    /* §8: session_id is 0 in DISCOVER and HELLO. */
    st_check(c, "session/discover_header",
             apad_session_next_header(&s, (uint8_t)APAD_MSG_DISCOVER, &h)
             == APAD_OK
             && h.session_id == 0u
             && (h.flags & APAD_FLAG_RELIABLE) == 0u);
    apad_session_on_sent(&s, &h, 1000u);
    st_check(c, "session/discover_moves_to_discovering",
             s.state == (uint8_t)APAD_SESSION_DISCOVERING);

    /* §4: HELLO is reliable. */
    st_check(c, "session/hello_is_reliable",
             apad_session_next_header(&s, (uint8_t)APAD_MSG_HELLO, &h)
             == APAD_OK
             && h.session_id == 0u
             && (h.flags & APAD_FLAG_RELIABLE) != 0u);
    apad_session_on_sent(&s, &h, 1000u);
    st_check(c, "session/hello_arms_retransmit",
             s.state == (uint8_t)APAD_SESSION_HANDSHAKING && s.retx_armed == 1u);

    /* §9: nothing fires before 100 ms, then the schedule runs. */
    st_check(c, "session/no_retx_before_100ms",
             apad_session_tick(&s, 1099u) == APAD_ACT_NONE);
    st_check(c, "session/retx_at_100ms",
             apad_session_tick(&s, 1100u) == APAD_ACT_RETRANSMIT);
    st_check(c, "session/retx_at_300ms",
             apad_session_tick(&s, 1299u) == APAD_ACT_NONE
             && apad_session_tick(&s, 1300u) == APAD_ACT_RETRANSMIT);
    st_check(c, "session/retx_at_700ms",
             apad_session_tick(&s, 1700u) == APAD_ACT_RETRANSMIT);
    st_check(c, "session/retx_at_1500ms",
             apad_session_tick(&s, 1500u + 1000u) == APAD_ACT_RETRANSMIT);
    st_check(c, "session/fails_after_schedule_exhausted",
             apad_session_tick(&s, 4000u) == APAD_ACT_TIMEOUT
             && s.state == (uint8_t)APAD_SESSION_CLOSED
             && s.close_reason == (uint8_t)APAD_CLOSE_RETX_FAILED);

    /* WELCOME moves the client to ACTIVE and adopts the session id (§8). */
    apad_session_init(&s, 0, 0u);
    (void)apad_session_next_header(&s, (uint8_t)APAD_MSG_HELLO, &h);
    apad_session_on_sent(&s, &h, 0u);
    {
        apad_welcome w;
        memset(&w, 0, sizeof w);
        w.session_id = 0x4242u;
        w.pad_slot = 1u;
        w.input_rate_hz = 125u;
        (void)apad_encode_welcome(payload, sizeof payload, &w);
    }
    memset(&h, 0, sizeof h);
    h.type = (uint8_t)APAD_MSG_WELCOME;
    h.sequence = 1u;
    h.flags = APAD_FLAG_RELIABLE;
    total = apad_packet_build(dgram, sizeof dgram, &h, payload,
                              APAD_LEN_WELCOME, NULL, 0u);
    (void)apad_packet_parse(dgram, (size_t)total, &pkt);
    st_check(c, "session/welcome_activates",
             apad_session_on_recv(&s, &pkt, 10u) == APAD_OK
             && s.state == (uint8_t)APAD_SESSION_ACTIVE
             && s.session_id == 0x4242u
             && s.pad_slot == 1u
             && s.input_rate_hz == 125u
             && s.retx_armed == 0u);

    /* §8: everything after WELCOME carries the session id. */
    st_check(c, "session/post_welcome_header_carries_id",
             apad_session_next_header(&s, (uint8_t)APAD_MSG_INPUT_STATE, &h)
             == APAD_OK
             && h.session_id == 0x4242u
             && (h.flags & APAD_FLAG_RELIABLE) == 0u);

    /* §9: INPUT_STATE is never armed for retransmission, ever. */
    h.flags = (uint16_t)(h.flags | APAD_FLAG_RELIABLE);
    apad_session_on_sent(&s, &h, 20u);
    st_check(c, "session/input_state_never_retransmits", s.retx_armed == 0u);

    /* §8/§11: keepalive floor and idle timeout. */
    st_check(c, "session/keepalive_at_100ms",
             apad_session_tick(&s, 100u) == APAD_ACT_NONE
             && apad_session_tick(&s, 120u) == APAD_ACT_KEEPALIVE);
    st_check(c, "session/idle_timeout_at_3000ms",
             apad_session_tick(&s, 3009u) == APAD_ACT_KEEPALIVE
             && apad_session_tick(&s, 3010u) == APAD_ACT_TIMEOUT
             && s.close_reason == (uint8_t)APAD_CLOSE_IDLE_TIMEOUT);

    /* §9 receive window, including across the 0xFFFF wrap. This is the test
     * that a naive `>` fails after nine minutes of play. */
    apad_session_init(&s, 1, 0u);
    st_check(c, "window/first_packet_accepted",
             apad_session_accept_input(&s, 0xFFFDu) == APAD_OK);
    st_check(c, "window/newer_accepted",
             apad_session_accept_input(&s, 0xFFFEu) == APAD_OK
             && apad_session_accept_input(&s, 0xFFFFu) == APAD_OK);
    st_check(c, "window/wrap_to_zero_accepted",
             apad_session_accept_input(&s, 0x0000u) == APAD_OK
             && apad_session_accept_input(&s, 0x0002u) == APAD_OK);
    st_check(c, "window/late_packet_from_before_wrap_discarded",
             apad_session_accept_input(&s, 0xFFFFu) == APAD_ERR_STALE);
    st_check(c, "window/duplicate_discarded",
             apad_session_accept_input(&s, 0x0002u) == APAD_ERR_STALE);
    st_check(c, "window/older_discarded",
             apad_session_accept_input(&s, 0x0001u) == APAD_ERR_STALE);
    st_check(c, "window/state_unchanged_by_stale",
             s.rx_input_seq == 0x0002u);

    /* §8 vs §9: a stale INPUT_STATE is still a packet received, so it keeps
     * the session alive even though its contents are discarded. */
    {
        apad_session t;
        apad_header th;
        apad_packet tp;
        uint8_t tdg[APAD_MAX_DATAGRAM];
        uint8_t tpl[APAD_LEN_INPUT_STATE];
        apad_input_state ts;
        int tot;

        apad_session_init(&t, 1, 0u);
        t.state = (uint8_t)APAD_SESSION_ACTIVE;
        memset(&ts, 0, sizeof ts);
        (void)apad_encode_input_state(tpl, sizeof tpl, &ts);

        memset(&th, 0, sizeof th);
        th.type = (uint8_t)APAD_MSG_INPUT_STATE;
        th.sequence = 100u;
        tot = apad_packet_build(tdg, sizeof tdg, &th, tpl,
                                APAD_LEN_INPUT_STATE, NULL, 0u);
        (void)apad_packet_parse(tdg, (size_t)tot, &tp);
        st_check(c, "session/fresh_input_accepted",
                 apad_session_on_recv(&t, &tp, 1000u) == APAD_OK);

        th.sequence = 99u;   /* reordered, arrives late */
        tot = apad_packet_build(tdg, sizeof tdg, &th, tpl,
                                APAD_LEN_INPUT_STATE, NULL, 0u);
        (void)apad_packet_parse(tdg, (size_t)tot, &tp);
        st_check(c, "session/stale_input_discarded_but_session_alive",
                 apad_session_on_recv(&t, &tp, 2000u) == APAD_ERR_STALE
                 && t.last_rx_ms == 2000u
                 && t.rx_input_seq == 100u
                 && t.state == (uint8_t)APAD_SESSION_ACTIVE);
        st_check(c, "session/stale_input_refreshes_idle_timer",
                 apad_session_tick(&t, 4500u) == APAD_ACT_NONE
                 && t.state == (uint8_t)APAD_SESSION_ACTIVE);
    }

    /* §8: BYE closes the session. */
    apad_session_init(&s, 1, 0u);
    s.state = (uint8_t)APAD_SESSION_ACTIVE;
    {
        apad_bye bye;
        memset(&bye, 0, sizeof bye);
        bye.reason = (uint8_t)APAD_BYE_SERVER_SHUTDOWN;
        (void)apad_encode_bye(payload, sizeof payload, &bye);
    }
    memset(&h, 0, sizeof h);
    h.type = (uint8_t)APAD_MSG_BYE;
    h.flags = APAD_FLAG_RELIABLE;
    total = apad_packet_build(dgram, sizeof dgram, &h, payload,
                              APAD_LEN_BYE, NULL, 0u);
    (void)apad_packet_parse(dgram, (size_t)total, &pkt);
    st_check(c, "session/bye_closes",
             apad_session_on_recv(&s, &pkt, 1u) == APAD_OK
             && s.state == (uint8_t)APAD_SESSION_CLOSED
             && s.close_reason == (uint8_t)APAD_CLOSE_PEER_BYE);

    /* §9 "What discharges a reliable message": exactly two ways — an explicit
     * ACK echoing the sequence, or receipt of the message the spec defines as
     * the direct answer. WELCOME answers HELLO and is the only such pair in
     * v1; BYE, RUMBLE, LED and STATUS MUST be acknowledged explicitly. */
    {
        apad_session cl;
        apad_header  ch;
        apad_packet  cp;
        uint8_t      cdg[APAD_MAX_DATAGRAM];
        uint8_t      cpl[APAD_LEN_WELCOME];
        apad_welcome cw;
        int          n;

        memset(&cw, 0, sizeof cw);
        cw.session_id    = 9u;
        cw.pad_slot      = 1u;
        cw.input_rate_hz = 60u;
        (void)apad_encode_welcome(cpl, sizeof cpl, &cw);
        memset(&ch, 0, sizeof ch);
        ch.type  = (uint8_t)APAD_MSG_WELCOME;
        ch.flags = APAD_FLAG_RELIABLE;
        n = apad_packet_build(cdg, sizeof cdg, &ch, cpl,
                              APAD_LEN_WELCOME, NULL, 0u);
        (void)apad_packet_parse(cdg, (size_t)n, &cp);

        /* HELLO armed, WELCOME arrives: discharged without an ACK. A client
         * that waited for an ACK of its HELLO would die at t = 2300 ms. */
        apad_session_init(&cl, 0, 0u);
        (void)apad_session_next_header(&cl, (uint8_t)APAD_MSG_HELLO, &ch);
        apad_session_on_sent(&cl, &ch, 0u);
        st_check(c, "session/hello_arms_retx", cl.retx_armed == 1u);
        st_check(c, "session/welcome_discharges_hello",
                 apad_session_on_recv(&cl, &cp, 10u) == APAD_OK
                 && cl.retx_armed == 0u
                 && cl.state == (uint8_t)APAD_SESSION_ACTIVE);

        /* BYE armed, a retransmitted WELCOME arrives: §9 gives BYE no direct
         * answer, so it MUST still be retransmitting. Reachable for real —
         * the client ACKs the WELCOME, the ACK is lost, the server resends. */
        (void)apad_session_next_header(&cl, (uint8_t)APAD_MSG_BYE, &ch);
        apad_session_on_sent(&cl, &ch, 20u);
        st_check(c, "session/bye_arms_retx",
                 cl.retx_armed == 1u
                 && cl.retx_type == (uint8_t)APAD_MSG_BYE);
        st_check(c, "session/duplicate_welcome_does_not_discharge_bye",
                 apad_session_on_recv(&cl, &cp, 30u) == APAD_OK
                 && cl.retx_armed == 1u
                 && cl.retx_type == (uint8_t)APAD_MSG_BYE);
        st_check(c, "session/bye_still_retransmits_after_welcome",
                 apad_session_tick(&cl, 20u + apad_retx_delays_ms[0])
                 == APAD_ACT_RETRANSMIT);

        /* ...and an ACK echoing its sequence is what does discharge it. */
        {
            uint8_t  adg[APAD_MAX_DATAGRAM];
            uint8_t  apl[APAD_LEN_ACK];
            apad_ack ack;
            apad_header ah;
            apad_packet ap;
            uint16_t bye_seq = cl.retx_seq;

            memset(&ack, 0, sizeof ack);
            ack.sequence = bye_seq;
            (void)apad_encode_ack(apl, sizeof apl, &ack);
            memset(&ah, 0, sizeof ah);
            ah.type = (uint8_t)APAD_MSG_ACK;
            n = apad_packet_build(adg, sizeof adg, &ah, apl,
                                  APAD_LEN_ACK, NULL, 0u);
            (void)apad_packet_parse(adg, (size_t)n, &ap);
            st_check(c, "session/ack_discharges_bye",
                     apad_session_on_recv(&cl, &ap, 40u) == APAD_OK
                     && cl.retx_armed == 0u);
        }
    }

    /* §8 server side: the server INVENTS session_id and pad_slot, it does not
     * adopt them. apad_session_server_accept is the only supported way in. */
    {
        apad_session sv;
        apad_welcome w;
        apad_header  wh;

        apad_session_init(&sv, 1, 0u);
        st_check(c, "session/server_accept_requires_hello",
                 apad_session_server_accept(&sv, 7u, 3u, 60u, 100u, NULL)
                 == APAD_ERR_STATE);

        sv.state = (uint8_t)APAD_SESSION_HANDSHAKING;   /* as if HELLO landed */
        st_check(c, "session/server_accept_rejects_zero_id",
                 apad_session_server_accept(&sv, 0u, 3u, 60u, 100u, NULL)
                 == APAD_ERR_ARG);
        st_check(c, "session/server_accept_rejects_rate_over_cap",
                 apad_session_server_accept(&sv, 7u, 3u,
                                            (uint16_t)(APAD_MAX_RATE_HZ + 1u),
                                            100u, NULL) == APAD_ERR_ARG);

        /* A stale receive window from the slot's previous occupant must not
         * survive: §9 would otherwise discard the new client's input. */
        sv.rx_input_valid = 1u;
        sv.rx_input_seq   = 40000u;

        memset(&w, 0xA5, sizeof w);
        st_check(c, "session/server_accept_activates",
                 apad_session_server_accept(&sv, 7u, 3u, 125u, 100u, &w)
                 == APAD_OK
                 && sv.state == (uint8_t)APAD_SESSION_ACTIVE
                 && sv.session_id == 7u
                 && sv.pad_slot == 3u
                 && sv.input_rate_hz == 125u
                 && sv.last_rx_ms == 100u
                 && sv.rx_input_valid == 0u);
        st_check(c, "session/server_accept_clears_rx_window",
                 apad_session_accept_input(&sv, 1u) == APAD_OK);

        st_check(c, "session/server_accept_prefills_welcome",
                 w.session_id == 7u && w.pad_slot == 3u
                 && w.input_rate_hz == 125u && w.server_ticks_ms == 100u
                 && w.flags == 0u && w.server_nonce[0] == 0u
                 && w.key_material[0] == 0u
                 && w.key_material[APAD_KEY_MATERIAL_LEN - 1u] == 0u);

        /* §8: WELCOME is not one of the three messages that carry
         * session_id 0, so the header must pick up the id assigned above. */
        st_check(c, "session/server_accept_then_welcome_header",
                 apad_session_next_header(&sv, (uint8_t)APAD_MSG_WELCOME, &wh)
                 == APAD_OK
                 && wh.session_id == 7u
                 && (wh.flags & APAD_FLAG_RELIABLE) != 0u);

        /* Rate 0 means "the server default", not "no rate at all". */
        st_check(c, "session/server_accept_rate_zero_is_default",
                 apad_session_server_accept(&sv, 7u, 3u, 0u, 200u, NULL)
                 == APAD_OK
                 && sv.input_rate_hz == (uint16_t)APAD_DEFAULT_RATE_HZ);

        /* A client-side session must refuse: it adopts from WELCOME. */
        {
            apad_session cl;
            apad_session_init(&cl, 0, 0u);
            cl.state = (uint8_t)APAD_SESSION_HANDSHAKING;
            st_check(c, "session/server_accept_refuses_client_side",
                     apad_session_server_accept(&cl, 7u, 0u, 60u, 0u, NULL)
                     == APAD_ERR_STATE);
        }

        st_check(c, "session/server_accept_null_is_arg_error",
                 apad_session_server_accept(NULL, 7u, 0u, 60u, 0u, NULL)
                 == APAD_ERR_ARG);

        /* §8/§11: the idle timer only runs in ACTIVE. Before this API existed
         * a server session never entered ACTIVE, so it never timed out. */
        st_check(c, "session/server_accept_arms_idle_timeout",
                 apad_session_tick(&sv, 200u + APAD_IDLE_TIMEOUT_MS)
                 == APAD_ACT_TIMEOUT
                 && sv.state == (uint8_t)APAD_SESSION_CLOSED
                 && sv.close_reason == (uint8_t)APAD_CLOSE_IDLE_TIMEOUT);
        st_check(c, "session/server_accept_refuses_closed",
                 apad_session_server_accept(&sv, 7u, 3u, 60u, 300u, NULL)
                 == APAD_ERR_STATE);
    }
}

/* ---- address helpers (tier-3 manual entry, §7) ------------------------- */

static void st_addr(st_ctx *c)
{
    apad_addr a, b;

    st_check(c, "addr/parse_ok",
             apad_addr_parse(&a, "192.168.1.10", APAD_DEFAULT_PORT) == APAD_OK
             && a.ip[0] == 192u && a.ip[1] == 168u
             && a.ip[2] == 1u && a.ip[3] == 10u
             && a.port == APAD_DEFAULT_PORT);
    st_check(c, "addr/parse_edges",
             apad_addr_parse(&a, "0.0.0.0", 1u) == APAD_OK
             && apad_addr_parse(&a, "255.255.255.255", 1u) == APAD_OK);
    st_check(c, "addr/parse_rejects_garbage",
             apad_addr_parse(&a, "", 1u) == APAD_ERR_ARG
             && apad_addr_parse(&a, "1.2.3", 1u) == APAD_ERR_ARG
             && apad_addr_parse(&a, "1.2.3.4.5", 1u) == APAD_ERR_ARG
             && apad_addr_parse(&a, "256.1.1.1", 1u) == APAD_ERR_ARG
             && apad_addr_parse(&a, "1.2.3.x", 1u) == APAD_ERR_ARG
             && apad_addr_parse(&a, "1..3.4", 1u) == APAD_ERR_ARG
             && apad_addr_parse(&a, "1.2.3.4444", 1u) == APAD_ERR_ARG);
    apad_addr_broadcast(&a, APAD_DEFAULT_PORT);
    apad_addr_set(&b, 255u, 255u, 255u, 255u, APAD_DEFAULT_PORT);
    st_check(c, "addr/broadcast_and_equal",
             apad_addr_equal(&a, &b) == 1);
    b.port = 1u;
    st_check(c, "addr/port_matters", apad_addr_equal(&a, &b) == 0);
}

/* ---- §10.3 pairing URI -------------------------------------------------- */
/*
 * The codec's OWN invariants for the parser. core/testdata/ carries the
 * independently-authored conformance vectors for the same grammar; these are
 * the checks that come from reading the implementation's contract (bounds,
 * untouched-on-failure, round trip) rather than from reading §10.3.
 */

static int st_uri_secret_is(const apad_pair_uri *u, const char *want)
{
    size_t i;

    for (i = 0; i < (APAD_SECRET_MAX_LEN + 1u); i++) {
        if (u->secret[i] != want[i]) {
            return 0;
        }
        if (want[i] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void st_pair_uri(st_ctx *c)
{
    apad_pair_uri u;
    apad_pair_uri zero;
    apad_addr     a;
    char          buf[APAD_PAIR_URI_MAX + 1u];
    char          longuri[APAD_PAIR_URI_MAX + 2u];
    char          secret[APAD_SECRET_MAX_LEN + 2u];
    size_t        i;
    int           rc;
    int           ok;

    memset(&zero, 0, sizeof zero);

    memset(&u, 0x5A, sizeof u);
    st_check(c, "uri/parse_canonical",
             apad_pair_uri_parse(&u, "atticpad://192.168.1.10:21100/?v=1&s=ABCDEF")
             == APAD_OK
             && u.addr.ip[0] == 192u && u.addr.ip[1] == 168u
             && u.addr.ip[2] == 1u && u.addr.ip[3] == 10u
             && u.addr.port == 21100u
             && st_uri_secret_is(&u, "ABCDEF"));

    /* §10.3: key order MUST NOT matter, unknown keys MUST be ignored. */
    st_check(c, "uri/query_order_and_unknown_keys",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?s=SECRET&v=1")
             == APAD_OK
             && u.addr.port == 1u && st_uri_secret_is(&u, "SECRET")
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?x=9&s=SECRET&v=1&zz=")
             == APAD_OK
             && st_uri_secret_is(&u, "SECRET"));

    /* §10.3: the port is REQUIRED even when it is the default. */
    st_check(c, "uri/port_required_not_defaulted",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:0/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:65536/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:65535/?v=1&s=ABCDEF")
             == APAD_OK);

    /* §10.3: an unrecognised `v` is its own outcome, not "malformed". */
    st_check(c, "uri/unsupported_version_distinct",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=2&s=ABCDEF")
             == APAD_ERR_VERSION
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=01&s=ABCDEF")
             == APAD_ERR_VERSION
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=99999999&s=ABCDEF")
             == APAD_ERR_VERSION);
    /* ...and it wins over every field it may itself have redefined. */
    st_check(c, "uri/version_checked_before_other_fields",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:0/?v=2&s=x")
             == APAD_ERR_VERSION);
    st_check(c, "uri/version_malformed_is_not_version_error",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=one&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?s=ABCDEF")
             == APAD_ERR_ARG);

    st_check(c, "uri/rejects_bad_scheme_and_host",
             apad_pair_uri_parse(&u, "") == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad:/") == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "ATTICPAD://10.0.0.1:1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "http://10.0.0.1:1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://:1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://server.local:1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.256:1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1?v=1&s=ABCDEF")
             == APAD_ERR_ARG);

    /* Duplicates of a key this version acts on: refuse rather than pick one. */
    st_check(c, "uri/rejects_duplicate_known_keys",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=ABCDEF&s=GHIJKL")
             == APAD_ERR_ARG);
    st_check(c, "uri/rejects_empty_query_segments",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?") == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=ABCDEF&")
             == APAD_ERR_ARG);

    /* §10.1 applied to the carried secret. */
    st_check(c, "uri/secret_length_bounds",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=ABCDE")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=ABCDEF")
             == APAD_OK);
    st_check(c, "uri/secret_rejects_nonprintable",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=AB CDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=AB\x7F" "CDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=1&s=AB\x01" "CDEF")
             == APAD_ERR_ARG);

    /* A 64-byte secret is legal and MUST survive whole (§10.1: used IN FULL,
     * the property the 32-byte truncation bug silently deleted). */
    for (i = 0; i < APAD_SECRET_MAX_LEN; i++) {
        secret[i] = (char)('A' + (int)(i % 26u));
    }
    secret[APAD_SECRET_MAX_LEN] = '\0';
    apad_addr_set(&a, 192u, 168u, 1u, 10u, 21100u);
    rc = apad_pair_uri_build(buf, sizeof buf, &a, secret);
    ok = (rc > 0) && ((size_t)rc <= APAD_PAIR_URI_MAX) && (buf[rc] == '\0');
    memset(&u, 0x5A, sizeof u);
    ok = ok && (apad_pair_uri_parse(&u, buf) == APAD_OK)
            && apad_addr_equal(&u.addr, &a)
            && st_uri_secret_is(&u, secret);
    st_check(c, "uri/round_trip_max_secret", ok);

    /* 65 bytes: rejected by both halves, loudly. */
    secret[APAD_SECRET_MAX_LEN]      = 'Z';
    secret[APAD_SECRET_MAX_LEN + 1u] = '\0';
    st_check(c, "uri/build_rejects_overlong_secret",
             apad_pair_uri_build(buf, sizeof buf, &a, secret) == APAD_ERR_ARG);

    st_check(c, "uri/build_canonical",
             apad_pair_uri_build(buf, sizeof buf, &a, "ABCDEF") == 43
             && memcmp(buf, "atticpad://192.168.1.10:21100/?v=1&s=ABCDEF", 44u)
                == 0);
    st_check(c, "uri/build_rejects_bad_args",
             apad_pair_uri_build(NULL, sizeof buf, &a, "ABCDEF") == APAD_ERR_ARG
             && apad_pair_uri_build(buf, sizeof buf, NULL, "ABCDEF")
                == APAD_ERR_ARG
             && apad_pair_uri_build(buf, sizeof buf, &a, NULL) == APAD_ERR_ARG
             && apad_pair_uri_build(buf, 0u, &a, "ABCDEF") == APAD_ERR_ARG
             && apad_pair_uri_build(buf, sizeof buf, &a, "ABCDE")
                == APAD_ERR_ARG
             && apad_pair_uri_build(buf, sizeof buf, &a, "AB CDEF")
                == APAD_ERR_ARG);
    /* Not §10.3's rule; see atticpad.h. A secret that cannot round trip is
     * refused rather than silently mangled. */
    st_check(c, "uri/build_rejects_unescapable_secret",
             apad_pair_uri_build(buf, sizeof buf, &a, "AB&CDEF") == APAD_ERR_ARG
             && apad_pair_uri_build(buf, sizeof buf, &a, "AB#CDEF")
                == APAD_ERR_ARG);
    /* Port 0 is invalid, not "absent" (§10.3: 1..65535, REQUIRED). */
    apad_addr_set(&a, 10u, 0u, 0u, 1u, 0u);
    st_check(c, "uri/build_rejects_port_zero",
             apad_pair_uri_build(buf, sizeof buf, &a, "ABCDEF") == APAD_ERR_ARG);

    /* Exact-fit and one-short buffers. Nothing is written when it does not
     * fit; the caller's byte past the end is never touched. */
    apad_addr_set(&a, 192u, 168u, 1u, 10u, 21100u);
    memset(buf, 0xA5, sizeof buf);
    rc = apad_pair_uri_build(buf, 43u, &a, "ABCDEF");
    ok = (rc == APAD_ERR_BUFFER) && (buf[0] == (char)0xA5);
    rc = apad_pair_uri_build(buf, 44u, &a, "ABCDEF");
    ok = ok && (rc == 43) && (buf[43] == '\0') && (buf[44] == (char)0xA5);
    st_check(c, "uri/build_buffer_bounds", ok);

    /* §10.3's 128-byte ceiling is inclusive. Build one of exactly 128 and one
     * of exactly 129 by padding an ignored query key. */
    for (i = 0; i < sizeof longuri; i++) {
        longuri[i] = 'p';
    }
    memcpy(longuri, "atticpad://10.0.0.1:1/?v=1&s=ABCDEF&q=", 38u);
    longuri[APAD_PAIR_URI_MAX] = '\0';
    st_check(c, "uri/accepts_exactly_128",
             apad_pair_uri_parse(&u, longuri) == APAD_OK);
    longuri[APAD_PAIR_URI_MAX]      = 'p';
    longuri[APAD_PAIR_URI_MAX + 1u] = '\0';
    st_check(c, "uri/rejects_129",
             apad_pair_uri_parse(&u, longuri) == APAD_ERR_ARG);
    /* No NUL anywhere in the 129 bytes it is allowed to read: rejected, and
     * byte 130 is never touched. */
    for (i = 0; i < (APAD_PAIR_URI_MAX + 1u); i++) {
        longuri[i] = 'p';
    }
    longuri[APAD_PAIR_URI_MAX + 1u] = '\0';
    st_check(c, "uri/rejects_unterminated",
             apad_pair_uri_parse(&u, longuri) == APAD_ERR_ARG);

    /* Truncation sweep: every prefix of a valid URI must be rejected, never
     * accepted and never read out of bounds. */
    ok = 1;
    for (i = 0; i < 43u; i++) {
        char cut[44];

        memcpy(cut, "atticpad://192.168.1.10:21100/?v=1&s=ABCDEF", 44u);
        cut[i] = '\0';
        if (apad_pair_uri_parse(&u, cut) == APAD_OK) {
            ok = 0;
        }
    }
    st_check(c, "uri/truncation_sweep_all_rejected", ok);

    /* A failed parse leaves the caller's struct untouched — the same rule
     * apad_packet_parse follows, and what stops a caller that ignores the
     * result code from deriving a key from stack residue. */
    memset(&u, 0, sizeof u);
    st_check(c, "uri/failure_leaves_out_untouched",
             apad_pair_uri_parse(&u, "atticpad://10.0.0.1:1/?v=2&s=ABCDEF")
             == APAD_ERR_VERSION
             && memcmp(&u, &zero, sizeof u) == 0
             && apad_pair_uri_parse(&u, "garbage") == APAD_ERR_ARG
             && memcmp(&u, &zero, sizeof u) == 0);
    st_check(c, "uri/rejects_null_args",
             apad_pair_uri_parse(NULL, "atticpad://10.0.0.1:1/?v=1&s=ABCDEF")
             == APAD_ERR_ARG
             && apad_pair_uri_parse(&u, NULL) == APAD_ERR_ARG);
}

/* ---- §13 conformance vectors (core/testdata/vectors.h) ------------------ */
/*
 * Everything below drives the independently-authored golden data through the
 * real codec. The vectors know nothing about apad_result, so the three
 * mappings between "§3.1 first failing check" and what this implementation
 * returns are written here, from the spec's table, and are themselves part of
 * what is being asserted.
 */

/* §3.1: the first check to fail decides the result code. */
static int st_vec_rc_for_check(int failed_check)
{
    switch (failed_check) {
    case 1:  return APAD_ERR_TRUNCATED;  /* length < 12: header unreadable   */
    case 2:  return APAD_ERR_MAGIC;
    case 3:  return APAD_ERR_VERSION;
    case 4:  return APAD_ERR_LENGTH;     /* != 12 + payload_len + tag        */
    case 5:  return APAD_ERR_TYPE;       /* not in the §4 table              */
    case 6:  return APAD_ERR_LENGTH;     /* payload_len != the type's size   */
    case 7:  return APAD_ERR_AUTH;       /* never reached from Section A     */
    default: return APAD_OK;
    }
}

/* §3.1: "discard" is drop silently, "reject" is drop and MAY send ERROR. The
 * caller can only tell them apart from the result code, so the mapping is an
 * invariant worth asserting rather than an implementation detail. */
static int st_vec_action_for_rc(int rc)
{
    if (rc >= 0) {
        return APAD_VEC_ACTION_ACCEPT;
    }
    switch (rc) {
    case APAD_ERR_TRUNCATED:   /* check 1 */
    case APAD_ERR_MAGIC:       /* check 2 */
    case APAD_ERR_TYPE:        /* check 5 */
        return APAD_VEC_ACTION_DISCARD;
    default:
        return APAD_VEC_ACTION_REJECT;
    }
}

/* §3.1 / §6.11: which ERROR code a rejection carries. 0 when not a rejection. */
static int st_vec_error_code_for_rc(int rc)
{
    switch (rc) {
    case APAD_ERR_VERSION: return (int)APAD_ERRC_VERSION_MISMATCH;  /* 1 */
    case APAD_ERR_AUTH:    return (int)APAD_ERRC_AUTH_FAILED;       /* 3 */
    case APAD_ERR_LENGTH:  return (int)APAD_ERRC_MALFORMED;         /* 6 */
    default:               return 0;
    }
}

/* §3.1 says "discard" and "reject", never "partially decode". A datagram that
 * fails any check must leave the caller's apad_packet exactly as it was, so a
 * caller that ignores the result code cannot end up following a payload
 * pointer into a truncated buffer. */
static int st_vec_untouched(const apad_packet *pk)
{
    return pk->header.magic == 0u && pk->header.type == 0u
        && pk->header.payload_len == 0u && pk->payload_len == 0u
        && pk->payload == NULL && pk->tag == NULL;
}

/* Section A — framing, the §3.1 ordered outcome. */
static void st_vec_frames(st_ctx *c)
{
    size_t i;

    for (i = 0; i < (size_t)APAD_FRAME_VECTOR_COUNT; i++) {
        const apad_vec_frame *v = &apad_frame_vectors[i];
        apad_packet pk;
        int rc;
        int ok;

        memset(&pk, 0, sizeof pk);
        rc = apad_packet_parse(v->bytes, (size_t)v->len, &pk);

        /* apad_packet_parse returns BYTES CONSUMED on success, NOT APAD_OK.
         * APAD_OK is 0, and 0 is also what a successful parse of a 0-byte
         * datagram would look like if one existed — so `rc == APAD_OK` is
         * wrong twice over: it reads every valid datagram as a failure, and
         * it is not even a useful "did it work" test. Success is rc >= 0,
         * and for a whole datagram rc is exactly the datagram length. */
        if (v->exp_failed_check == 0) {
            ok = (rc == (int)v->len);
        } else {
            ok = (rc == st_vec_rc_for_check(v->exp_failed_check));
        }
        st_check2(c, v->name, st_name(c, "vec/frame", v->name, "outcome"), ok);

        st_check2(c, v->name, st_name(c, "vec/frame", v->name, "action"),
                  st_vec_action_for_rc(rc) == v->exp_action);
        st_check2(c, v->name, st_name(c, "vec/frame", v->name, "error_code"),
                  st_vec_error_code_for_rc(rc) == v->exp_error_code);

        if (v->exp_failed_check != 0 || rc < 0) {
            st_check2(c, v->name, st_name(c, "vec/frame", v->name, "no_output"),
                      st_vec_untouched(&pk));
            continue;
        }

        st_check2(c, v->name, st_name(c, "vec/frame", v->name, "header"),
                  pk.header.version     == v->exp_version
                  && pk.header.type     == v->exp_type
                  && pk.header.session_id  == v->exp_session_id
                  && pk.header.sequence    == v->exp_sequence
                  && pk.header.payload_len == v->exp_payload_len
                  && pk.payload_len        == v->exp_payload_len
                  && pk.header.flags       == v->exp_flags
                  && pk.header.magic       == APAD_VEC_MAGIC);

        /* The parse must point INTO the caller's buffer and must never copy
         * or cast (§2). Checked as pointer identity, which is the only way to
         * catch a codec that quietly memcpy'd into a struct. */
        st_check2(c, v->name, st_name(c, "vec/frame", v->name, "pointers"),
                  pk.payload == ((v->exp_payload_len != 0u)
                                 ? v->bytes + APAD_VEC_HEADER_LEN : NULL)
                  && pk.tag == (((v->exp_flags & APAD_VEC_FLAG_AUTHENTICATED) != 0u)
                                ? v->bytes + v->len - APAD_VEC_TAG_LEN : NULL));
    }
}

/* Section B — both truncation sweeps, byte offset by byte offset. */
static void st_vec_truncation_sweep(st_ctx *c,
                                    const char *section,
                                    const uint8_t *canonical,
                                    const apad_vec_truncation *tab,
                                    size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        const apad_vec_truncation *v = &tab[i];
        apad_packet pk;
        int rc;

        memset(&pk, 0, sizeof pk);
        rc = apad_packet_parse(canonical, (size_t)v->trunc_len, &pk);

        st_check2(c, v->name, st_name(c, section, v->name, "outcome"),
                  rc == st_vec_rc_for_check(v->exp_failed_check));
        /* Not a restatement of the line above: every offset in the sweep must
         * also produce NO decoded output. This is the check that would catch
         * a decoder that fills in the header before validating the length —
         * which is exactly how a truncated datagram turns into an overread. */
        st_check2(c, v->name, st_name(c, section, v->name, "no_output"),
                  st_vec_untouched(&pk));
    }
}

/* Section C — INPUT_STATE decode, including every §2 scrub and §5 clamp. */
static void st_vec_input_state(st_ctx *c)
{
    size_t i;

    for (i = 0; i < (size_t)APAD_INPUT_STATE_VECTOR_COUNT; i++) {
        const apad_vec_input_state *v = &apad_input_state_vectors[i];
        apad_input_state st;
        apad_packet pk;
        int rc;
        int j;
        int ok;

        memset(&pk, 0, sizeof pk);
        rc = apad_packet_parse(v->packet, (size_t)v->packet_len, &pk);
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "frame"),
                  rc == (int)v->packet_len
                  && pk.header.type == (uint8_t)APAD_MSG_INPUT_STATE
                  && pk.header.sequence == v->exp_header_sequence
                  && pk.payload_len == (uint16_t)APAD_LEN_INPUT_STATE);
        if (rc < 0 || pk.payload == NULL) {
            continue;
        }

        /* Fill with a non-zero pattern first: a decoder that "scrubs" a
         * reserved field by simply not writing it would pass against a
         * zeroed struct and fail here, which is the bug §2 cares about. */
        memset(&st, 0x5A, sizeof st);
        rc = apad_decode_input_state(pk.payload, (size_t)pk.payload_len, &st);
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "decode"),
                  rc == (int)APAD_LEN_INPUT_STATE);
        if (rc < 0) {
            continue;
        }

        st_check2(c, v->name, st_name(c, "vec/input", v->name, "buttons"),
                  st.buttons == v->exp_buttons);

        ok = 1;
        for (j = 0; j < APAD_AXIS_COUNT; j++) {
            if (st.axes[j] != v->exp_axes[j]) {
                ok = 0;
            }
        }
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "axes"), ok);

        st_check2(c, v->name, st_name(c, "vec/input", v->name, "touch_count"),
                  st.touch_count == v->exp_touch_count);

        ok = 1;
        for (j = 0; j < APAD_TOUCH_MAX; j++) {
            if (st.touches[j].id       != v->exp_touches[j].id
                || st.touches[j].pressure != v->exp_touches[j].pressure
                || st.touches[j].x        != v->exp_touches[j].x
                || st.touches[j].y        != v->exp_touches[j].y) {
                ok = 0;
            }
        }
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "touches"), ok);

        ok = 1;
        for (j = 0; j < 3; j++) {
            if (st.accel[j] != v->exp_accel[j]) {
                ok = 0;
            }
        }
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "accel"), ok);

        ok = 1;
        for (j = 0; j < 3; j++) {
            if (st.gyro[j] != v->exp_gyro[j]) {
                ok = 0;
            }
        }
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "gyro"), ok);

        st_check2(c, v->name, st_name(c, "vec/input", v->name, "battery"),
                  st.battery == v->exp_battery);
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "ticks"),
                  st.client_ticks_ms == v->exp_client_ticks_ms);

        /* §2: every reserved field zero in the decoded output, whatever the
         * sender put on the wire. exp_reserved0 is always 0 for that reason;
         * reserved1/reserved2 have no expected field because the spec leaves
         * them no choice. */
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "reserved"),
                  st.reserved0 == v->exp_reserved0
                  && st.reserved1[0] == 0u && st.reserved1[1] == 0u
                  && st.reserved1[2] == 0u
                  && st.reserved2[0] == 0u && st.reserved2[1] == 0u);

        /* §5.1: the D-pad nibble through the hat LUT. */
        st_check2(c, v->name, st_name(c, "vec/input", v->name, "hat"),
                  apad_hat_from_buttons(st.buttons) == v->exp_hat);
    }
}

/* Sections D and E — the §9 wrap-safe helpers. */
static void st_vec_wrap(st_ctx *c)
{
    size_t i;

    for (i = 0; i < (size_t)APAD_SEQ_NEWER_VECTOR_COUNT; i++) {
        const apad_vec_seq_newer *v = &apad_seq_newer_vectors[i];
        st_check2(c, v->name, st_name(c, "vec/seq_newer", v->name, NULL),
                  (apad_seq_newer(v->a, v->b) != 0) == (v->expect_newer != 0));
    }
    for (i = 0; i < (size_t)APAD_TIME_AFTER_VECTOR_COUNT; i++) {
        const apad_vec_time_after *v = &apad_time_after_vectors[i];
        st_check2(c, v->name, st_name(c, "vec/time_after", v->name, NULL),
                  (apad_time_after(v->a, v->b) != 0) == (v->expect_after != 0));
    }
}

/* Section F — Appendix A: PBKDF2, the derived key, and the truncated tag. */
static void st_vec_auth(st_ctx *c)
{
    uint8_t key[APAD_SESSION_KEY_LEN];
    size_t i;

    for (i = 0; i < (size_t)APAD_PBKDF2_VECTOR_COUNT; i++) {
        const apad_vec_pbkdf2 *v = &apad_pbkdf2_vectors[i];

        apad_pbkdf2_sha256(v->pin, (size_t)v->pin_len,
                           v->salt, (size_t)v->salt_len,
                           v->iterations, key, sizeof key);
        st_check2(c, v->name, st_name(c, "vec/pbkdf2", v->name, NULL),
                  memcmp(key, v->expected_key, sizeof key) == 0);

        /* The same derivation through the API clients actually call. §10 says
         * the PIN is six digits and the salt is the 16-byte server_nonce, so
         * this only applies to a vector shaped that way. */
        if (v->pin_len == 6u && v->salt_len == (uint32_t)APAD_NONCE_LEN
            && v->iterations == (uint32_t)APAD_PBKDF2_ITERATIONS) {
            char pin[7];
            size_t k;
            for (k = 0; k < 6u; k++) {
                pin[k] = (char)v->pin[k];
            }
            pin[6] = '\0';
            memset(key, 0, sizeof key);
            apad_derive_session_key(pin, v->salt, key);
            st_check2(c, v->name,
                      st_name(c, "vec/pbkdf2", v->name, "derive_session_key"),
                      memcmp(key, v->expected_key, sizeof key) == 0);
        }
    }

    for (i = 0; i < (size_t)APAD_AUTH_TAG_VECTOR_COUNT; i++) {
        const apad_vec_auth_tag *v = &apad_auth_tag_vectors[i];
        int rc = apad_packet_verify(v->datagram, (size_t)v->datagram_len,
                                    v->key, (size_t)APAD_SESSION_KEY_LEN);
        st_check2(c, v->name, st_name(c, "vec/auth", v->name, NULL),
                  (rc == APAD_OK) == (v->exp_verify_ok != 0));

        /* Verification alone only proves the RECEIVE side. Rebuild the same
         * datagram from its parts and require it byte-identical to Appendix
         * A: that pins the send side — little-endian header assembly, the
         * zeroed-tag-region rule, and the 8-byte truncation — to the one
         * value in the spec that was derived without any implementation in
         * view. Only meaningful for the vector that is supposed to verify. */
        if (v->exp_verify_ok != 0
            && v->datagram_len == APAD_VEC_HEADER_LEN + APAD_LEN_PING
                                  + APAD_VEC_TAG_LEN) {
            uint8_t rebuilt[APAD_VEC_HEADER_LEN + APAD_LEN_PING
                            + APAD_VEC_TAG_LEN];
            apad_header h;
            int total;

            memset(&h, 0, sizeof h);
            h.type       = v->datagram[APAD_OFF_TYPE];
            h.session_id = le16(v->datagram + APAD_OFF_SESSION_ID);
            h.sequence   = le16(v->datagram + APAD_OFF_SEQUENCE);
            h.flags      = 0u;   /* apad_packet_build sets AUTHENTICATED */

            total = apad_packet_build(rebuilt, sizeof rebuilt, &h,
                                      v->datagram + APAD_VEC_HEADER_LEN,
                                      (uint16_t)APAD_LEN_PING,
                                      v->key, (size_t)APAD_SESSION_KEY_LEN);
            st_check2(c, v->name, st_name(c, "vec/auth", v->name, "rebuild"),
                      total == (int)v->datagram_len
                      && memcmp(rebuilt, v->datagram,
                                (size_t)v->datagram_len) == 0);
        }
    }
}

/* Section I — §10.1 secret-length boundaries.
 *
 * Section F's driver reaches apad_derive_session_key for a 6-byte PIN only,
 * because §10 originally mandated six digits. §10.1 now requires any length
 * from APAD_SECRET_MIN_LEN to APAD_SECRET_MAX_LEN to be accepted and used in
 * full, and the failure mode it guards against is SILENT: a derivation that
 * truncates the secret still produces a well-formed 32-byte key, still
 * pairs, still authenticates every datagram. Nothing differs on the wire —
 * only the amount of entropy that actually reached PBKDF2. No test that
 * feeds a short secret can see it, which is why this table exists.
 *
 * Each vector is checked twice and the PAIR is what localises a failure:
 *   - apad_pbkdf2_sha256 is handed an explicit length, so it tests the
 *     primitive. A 64-byte secret is exactly APAD_SHA256_BLOCK_LEN, the
 *     boundary where HMAC stops padding the key and starts hashing it, so
 *     this half carries coverage Section F's 6-byte vector cannot.
 *   - apad_derive_session_key is handed a NUL-terminated string and must
 *     find the length itself, so it tests the length scan.
 * Primitive passing while the API path fails means the bug is in the scan.
 */
static void st_vec_secret_length(st_ctx *c)
{
    uint8_t key[APAD_SESSION_KEY_LEN];
    size_t i;

    for (i = 0; i < (size_t)APAD_SECRET_LENGTH_VECTOR_COUNT; i++) {
        const apad_vec_pbkdf2 *v = &apad_secret_length_vectors[i];
        char secret[APAD_SECRET_MAX_LEN + 1u];
        size_t k;

        /* A vector this driver cannot express through the public API is
         * recorded as a FAILURE, never skipped. Silently skipping is the
         * dead-vector failure mode with extra steps: the case count moves,
         * so nothing looks wrong, and the vector still never ran. */
        if (v->pin_len > APAD_SECRET_MAX_LEN
            || v->salt_len != (uint32_t)APAD_NONCE_LEN
            || v->iterations != (uint32_t)APAD_PBKDF2_ITERATIONS) {
            st_check2(c, v->name,
                      st_name(c, "vec/secret_len", v->name, "usable"), 0);
            continue;
        }

        memset(key, 0, sizeof key);
        apad_pbkdf2_sha256(v->pin, (size_t)v->pin_len,
                           v->salt, (size_t)v->salt_len,
                           v->iterations, key, sizeof key);
        st_check2(c, v->name,
                  st_name(c, "vec/secret_len", v->name, "pbkdf2"),
                  memcmp(key, v->expected_key, sizeof key) == 0);

        /* Rebuild the NUL terminator locally rather than trusting the
         * generated array to carry one: apad_derive_session_key's contract
         * is a C string, and §10.1 forbids an embedded NUL, so the string
         * this driver passes must be built to that contract explicitly. */
        for (k = 0; k < (size_t)v->pin_len; k++) {
            secret[k] = (char)v->pin[k];
        }
        secret[v->pin_len] = '\0';

        memset(key, 0, sizeof key);
        apad_derive_session_key(secret, v->salt, key);
        st_check2(c, v->name,
                  st_name(c, "vec/secret_len", v->name, "derive_session_key"),
                  memcmp(key, v->expected_key, sizeof key) == 0);
    }
}

/* Section G — the eleven §6 payloads (§6.2 … §6.11).
 *
 * Each vector carries a whole datagram, so every one of these exercises the
 * §3.1 framing path and the payload decoder together: parse first, then hand
 * pkt.payload to the type's decoder. That is exactly how a client uses the
 * API, and it means a wrong APAD_LEN_* would fail here as a framing error
 * rather than silently decoding from the wrong offset.
 *
 * The helper returns the payload pointer, or NULL after recording why. Every
 * caller must check it: a §6 decoder given a NULL payload is UB, and these
 * run on platforms with no MMU to catch it.
 */
static const uint8_t *st_vec_g_payload(st_ctx *c, const char *section,
                                       const char *name,
                                       const uint8_t *packet, uint32_t plen,
                                       uint8_t want_type, uint16_t want_len)
{
    apad_packet pk;
    int rc;

    memset(&pk, 0, sizeof pk);
    rc = apad_packet_parse(packet, (size_t)plen, &pk);
    /* Bytes consumed, not APAD_OK — see st_vec_frames. */
    st_check2(c, name, st_name(c, section, name, "frame"),
              rc == (int)plen
              && pk.header.type == want_type
              && pk.payload_len == want_len
              && pk.payload == packet + APAD_HEADER_SIZE);
    if (rc < 0 || pk.payload == NULL || pk.payload_len != want_len) {
        return NULL;
    }
    return pk.payload;
}

static void st_vec_section_g(st_ctx *c)
{
    size_t i;

    /* §6.2 ANNOUNCE — including the non-zero-to-1 pairing_required rule. */
    for (i = 0; i < (size_t)APAD_ANNOUNCE_VECTOR_COUNT; i++) {
        const apad_vec_announce *v = &apad_announce_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/announce", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_ANNOUNCE,
                                             (uint16_t)APAD_LEN_ANNOUNCE);
        apad_announce a;
        if (pl == NULL) {
            continue;
        }
        memset(&a, 0x5A, sizeof a);
        st_check2(c, v->name, st_name(c, "vec/announce", v->name, "decode"),
                  apad_decode_announce(pl, APAD_LEN_ANNOUNCE, &a)
                  == (int)APAD_LEN_ANNOUNCE);
        st_check2(c, v->name, st_name(c, "vec/announce", v->name, "server_name"),
                  memcmp(a.server_name, v->exp_server_name, APAD_NAME_LEN) == 0);
        st_check2(c, v->name, st_name(c, "vec/announce", v->name, "fields"),
                  a.pads_total == v->exp_pads_total
                  && a.pads_free == v->exp_pads_free
                  && a.server_port == v->exp_server_port);
        st_check2(c, v->name,
                  st_name(c, "vec/announce", v->name, "pairing_required"),
                  a.pairing_required == v->exp_pairing_required);
    }

    /* §6.3 HELLO — including caps bits 14..31 masked off. */
    for (i = 0; i < (size_t)APAD_HELLO_VECTOR_COUNT; i++) {
        const apad_vec_hello *v = &apad_hello_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/hello", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_HELLO,
                                             (uint16_t)APAD_LEN_HELLO);
        apad_hello hv;
        if (pl == NULL) {
            continue;
        }
        memset(&hv, 0x5A, sizeof hv);
        st_check2(c, v->name, st_name(c, "vec/hello", v->name, "decode"),
                  apad_decode_hello(pl, APAD_LEN_HELLO, &hv)
                  == (int)APAD_LEN_HELLO);
        st_check2(c, v->name, st_name(c, "vec/hello", v->name, "client_id"),
                  memcmp(hv.client_id, v->exp_client_id,
                         APAD_CLIENT_ID_LEN) == 0);
        st_check2(c, v->name, st_name(c, "vec/hello", v->name, "caps"),
                  hv.caps == v->exp_caps);
        st_check2(c, v->name, st_name(c, "vec/hello", v->name, "device_name"),
                  memcmp(hv.device_name, v->exp_device_name,
                         APAD_NAME_LEN) == 0);
        st_check2(c, v->name, st_name(c, "vec/hello", v->name, "client_nonce"),
                  memcmp(hv.client_nonce, v->exp_client_nonce,
                         APAD_NONCE_LEN) == 0);
        st_check2(c, v->name, st_name(c, "vec/hello", v->name, "scalars"),
                  hv.desired_rate_hz == v->exp_desired_rate_hz
                  && hv.proto_major == v->exp_proto_major
                  && hv.client_ticks_ms == v->exp_client_ticks_ms);
    }

    /* §6.4 WELCOME, decode side — key_material MUST arrive all-zero. */
    for (i = 0; i < (size_t)APAD_WELCOME_VECTOR_COUNT; i++) {
        const apad_vec_welcome *v = &apad_welcome_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/welcome", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_WELCOME,
                                             (uint16_t)APAD_LEN_WELCOME);
        apad_welcome w;
        if (pl == NULL) {
            continue;
        }
        memset(&w, 0x5A, sizeof w);
        st_check2(c, v->name, st_name(c, "vec/welcome", v->name, "decode"),
                  apad_decode_welcome(pl, APAD_LEN_WELCOME, &w)
                  == (int)APAD_LEN_WELCOME);
        st_check2(c, v->name, st_name(c, "vec/welcome", v->name, "scalars"),
                  w.session_id == v->exp_session_id
                  && w.pad_slot == v->exp_pad_slot
                  && w.flags == v->exp_flags
                  && w.input_rate_hz == v->exp_input_rate_hz
                  && w.server_ticks_ms == v->exp_server_ticks_ms);
        st_check2(c, v->name, st_name(c, "vec/welcome", v->name, "server_nonce"),
                  memcmp(w.server_nonce, v->exp_server_nonce,
                         APAD_NONCE_LEN) == 0);
        /* The wire bytes here are 0xFF: §6.4 says key_material MUST be zero
         * in v1 and MUST be ignored on receive, so the decoder scrubs it. */
        st_check2(c, v->name, st_name(c, "vec/welcome", v->name, "key_material"),
                  memcmp(w.key_material, v->exp_key_material,
                         APAD_KEY_MATERIAL_LEN) == 0);
    }

    /* §6.4 WELCOME, ENCODE side. Different shape from every other Section G
     * table: it hands the encoder a deliberately non-zero key_material and
     * requires the 60 bytes on the wire to come back with it zeroed. A
     * decode-only table cannot test that — the send side is where v1's
     * "key_material MUST be zero" is actually enforced. */
    for (i = 0; i < (size_t)APAD_WELCOME_ENCODE_VECTOR_COUNT; i++) {
        const apad_vec_welcome_encode *v = &apad_welcome_encode_vectors[i];
        uint8_t enc[APAD_LEN_WELCOME];
        apad_welcome w;
        int n;

        memset(&w, 0, sizeof w);
        w.session_id      = v->in_session_id;
        w.pad_slot        = v->in_pad_slot;
        w.flags           = v->in_flags;
        w.input_rate_hz   = v->in_input_rate_hz;
        w.server_ticks_ms = v->in_server_ticks_ms;
        memcpy(w.server_nonce, v->in_server_nonce, APAD_NONCE_LEN);
        memcpy(w.key_material, v->in_key_material, APAD_KEY_MATERIAL_LEN);

        memset(enc, 0xA5, sizeof enc);
        n = apad_encode_welcome(enc, sizeof enc, &w);
        st_check2(c, v->name, st_name(c, "vec/welcome_encode", v->name, "len"),
                  n == (int)v->exp_payload_len);
        st_check2(c, v->name, st_name(c, "vec/welcome_encode", v->name, "bytes"),
                  n == (int)v->exp_payload_len
                  && memcmp(enc, v->exp_payload,
                            (size_t)v->exp_payload_len) == 0);
    }

    /* §6.5 BYE — reason is NOT clamped (§6.0): a diagnostic label. */
    for (i = 0; i < (size_t)APAD_BYE_VECTOR_COUNT; i++) {
        const apad_vec_bye *v = &apad_bye_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/bye", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_BYE,
                                             (uint16_t)APAD_LEN_BYE);
        apad_bye bv;
        if (pl == NULL) {
            continue;
        }
        memset(&bv, 0x5A, sizeof bv);
        st_check2(c, v->name, st_name(c, "vec/bye", v->name, "decode"),
                  apad_decode_bye(pl, APAD_LEN_BYE, &bv) == (int)APAD_LEN_BYE);
        st_check2(c, v->name, st_name(c, "vec/bye", v->name, "reason"),
                  bv.reason == v->exp_reason);
    }

    /* §6.6 PING and PONG share a payload shape; both halves must be read at
     * the right offset, which is why the vectors put distinct values in each. */
    for (i = 0; i < (size_t)APAD_PING_VECTOR_COUNT; i++) {
        const apad_vec_ping *v = &apad_ping_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/ping", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_PING,
                                             (uint16_t)APAD_LEN_PING);
        apad_ping p;
        if (pl == NULL) {
            continue;
        }
        memset(&p, 0x5A, sizeof p);
        st_check2(c, v->name, st_name(c, "vec/ping", v->name, "decode"),
                  apad_decode_ping(pl, APAD_LEN_PING, &p) == (int)APAD_LEN_PING);
        st_check2(c, v->name, st_name(c, "vec/ping", v->name, "ticks"),
                  p.origin_ticks_ms == v->exp_origin_ticks_ms
                  && p.responder_ticks_ms == v->exp_responder_ticks_ms);
    }
    for (i = 0; i < (size_t)APAD_PONG_VECTOR_COUNT; i++) {
        const apad_vec_ping *v = &apad_pong_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/pong", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_PONG,
                                             (uint16_t)APAD_LEN_PONG);
        apad_ping p;
        if (pl == NULL) {
            continue;
        }
        memset(&p, 0x5A, sizeof p);
        st_check2(c, v->name, st_name(c, "vec/pong", v->name, "decode"),
                  apad_decode_ping(pl, APAD_LEN_PONG, &p) == (int)APAD_LEN_PONG);
        st_check2(c, v->name, st_name(c, "vec/pong", v->name, "ticks"),
                  p.origin_ticks_ms == v->exp_origin_ticks_ms
                  && p.responder_ticks_ms == v->exp_responder_ticks_ms);
    }

    /* §6.7 RUMBLE */
    for (i = 0; i < (size_t)APAD_RUMBLE_VECTOR_COUNT; i++) {
        const apad_vec_rumble *v = &apad_rumble_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/rumble", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_RUMBLE,
                                             (uint16_t)APAD_LEN_RUMBLE);
        apad_rumble r;
        if (pl == NULL) {
            continue;
        }
        memset(&r, 0x5A, sizeof r);
        st_check2(c, v->name, st_name(c, "vec/rumble", v->name, "decode"),
                  apad_decode_rumble(pl, APAD_LEN_RUMBLE, &r)
                  == (int)APAD_LEN_RUMBLE);
        st_check2(c, v->name, st_name(c, "vec/rumble", v->name, "fields"),
                  r.low_freq == v->exp_low_freq
                  && r.high_freq == v->exp_high_freq
                  && r.duration_ms == v->exp_duration_ms);
    }

    /* §6.8 LED — the clamp the pre-freeze audit found missing. */
    for (i = 0; i < (size_t)APAD_LED_VECTOR_COUNT; i++) {
        const apad_vec_led *v = &apad_led_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/led", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_LED,
                                             (uint16_t)APAD_LEN_LED);
        apad_led l;
        if (pl == NULL) {
            continue;
        }
        memset(&l, 0x5A, sizeof l);
        st_check2(c, v->name, st_name(c, "vec/led", v->name, "decode"),
                  apad_decode_led(pl, APAD_LEN_LED, &l) == (int)APAD_LEN_LED);
        st_check2(c, v->name, st_name(c, "vec/led", v->name, "player_index"),
                  l.player_index == v->exp_player_index);
        st_check2(c, v->name, st_name(c, "vec/led", v->name, "colour"),
                  l.r == v->exp_r && l.g == v->exp_g && l.b == v->exp_b);
    }

    /* §6.9 STATUS — code is NOT clamped (§6.0); text is bounded by width. */
    for (i = 0; i < (size_t)APAD_STATUS_VECTOR_COUNT; i++) {
        const apad_vec_status *v = &apad_status_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/status", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_STATUS,
                                             (uint16_t)APAD_LEN_STATUS);
        apad_status sv;
        if (pl == NULL) {
            continue;
        }
        memset(&sv, 0x5A, sizeof sv);
        st_check2(c, v->name, st_name(c, "vec/status", v->name, "decode"),
                  apad_decode_status(pl, APAD_LEN_STATUS, &sv)
                  == (int)APAD_LEN_STATUS);
        st_check2(c, v->name, st_name(c, "vec/status", v->name, "code"),
                  sv.code == v->exp_code);
        st_check2(c, v->name, st_name(c, "vec/status", v->name, "text"),
                  memcmp(sv.text, v->exp_text, APAD_TEXT_LEN) == 0);
    }

    /* §6.10 ACK — the sequence field carries the wrap boundaries verbatim. */
    for (i = 0; i < (size_t)APAD_ACK_VECTOR_COUNT; i++) {
        const apad_vec_ack *v = &apad_ack_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/ack", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_ACK,
                                             (uint16_t)APAD_LEN_ACK);
        apad_ack av;
        if (pl == NULL) {
            continue;
        }
        memset(&av, 0x5A, sizeof av);
        st_check2(c, v->name, st_name(c, "vec/ack", v->name, "decode"),
                  apad_decode_ack(pl, APAD_LEN_ACK, &av) == (int)APAD_LEN_ACK);
        st_check2(c, v->name, st_name(c, "vec/ack", v->name, "sequence"),
                  av.sequence == v->exp_sequence);
    }

    /* §6.11 ERROR — code is NOT clamped (§6.0), and a trailing split UTF-8
     * sequence MUST be tolerated rather than rejected (§2). */
    for (i = 0; i < (size_t)APAD_ERROR_VECTOR_COUNT; i++) {
        const apad_vec_error *v = &apad_error_vectors[i];
        const uint8_t *pl = st_vec_g_payload(c, "vec/error", v->name,
                                             v->packet, v->packet_len,
                                             (uint8_t)APAD_MSG_ERROR,
                                             (uint16_t)APAD_LEN_ERROR);
        apad_error ev;
        if (pl == NULL) {
            continue;
        }
        memset(&ev, 0x5A, sizeof ev);
        st_check2(c, v->name, st_name(c, "vec/error", v->name, "decode"),
                  apad_decode_error(pl, APAD_LEN_ERROR, &ev)
                  == (int)APAD_LEN_ERROR);
        st_check2(c, v->name, st_name(c, "vec/error", v->name, "code"),
                  ev.code == v->exp_code);
        st_check2(c, v->name, st_name(c, "vec/error", v->name, "text"),
                  memcmp(ev.text, v->exp_text, APAD_TEXT_LEN) == 0);
    }
}

/*
 * Section H: the §2 text-field rule, driven directly against apad_text_set /
 * apad_text_len / apad_text_get rather than only through a payload decode.
 *
 * H.1 is an encode-side table, so it needs an encode-side driver in the shape
 * of st_vec_section_g's apad_welcome_encode loop: feed the vector's input,
 * compare the produced bytes. The scratch field is pre-filled with 0xA5 so
 * that "NUL-pad the remainder" is a real assertion — a zeroed buffer cannot
 * tell padding from never-written — and the byte past the width is checked
 * to catch a truncation loop that walks off the end of a caller's field.
 */
static void st_vec_text(st_ctx *c)
{
    size_t i;

    for (i = 0; i < (size_t)APAD_TEXT_SET_VECTOR_COUNT; i++) {
        const apad_vec_text_set *v = &apad_text_set_vectors[i];
        char buf[80];
        size_t w = (size_t)v->width;

        /* The widths in use are APAD_NAME_LEN and APAD_TEXT_LEN; a vector
         * wider than the scratch buffer is a harness bug, not a codec one. */
        if (w + 1u > sizeof buf) {
            st_check2(c, v->name, st_name(c, "vec/text_set", v->name, "width"),
                      0);
            continue;
        }

        memset(buf, 0xA5, sizeof buf);
        apad_text_set(buf, w, v->in_src);

        st_check2(c, v->name, st_name(c, "vec/text_set", v->name, "field"),
                  memcmp(buf, v->exp_field, w) == 0);
        st_check2(c, v->name, st_name(c, "vec/text_set", v->name, "no_overrun"),
                  (unsigned char)buf[w] == 0xA5u);
    }

    for (i = 0; i < (size_t)APAD_TEXT_GET_VECTOR_COUNT; i++) {
        const apad_vec_text_get *v = &apad_text_get_vectors[i];
        const char *field = (const char *)v->in_field;
        char dst[80];
        size_t cap = (size_t)v->dst_cap;

        if (cap + 1u > sizeof dst) {
            st_check2(c, v->name, st_name(c, "vec/text_get", v->name, "cap"),
                      0);
            continue;
        }

        st_check2(c, v->name, st_name(c, "vec/text_get", v->name, "len"),
                  apad_text_len(field, (size_t)v->width)
                  == (size_t)v->exp_len);

        memset(dst, 0xA5, sizeof dst);
        apad_text_get(dst, cap, field, (size_t)v->width);
        st_check2(c, v->name, st_name(c, "vec/text_get", v->name, "cstr"),
                  memcmp(dst, v->exp_cstr, (size_t)v->exp_cstr_len) == 0);
    }
}

/*
 * Section J: the §10.3 pairing URI. Two tables, one per direction.
 *
 * J.1 asserts only what the vector states — parses or is rejected, and on a
 * parse the ip/port/secret it must yield. It deliberately does NOT assert
 * WHICH negative code a rejection returns: the vectors carry a boolean,
 * §10.3 names no result codes, and the malformed-vs-unsupported-version
 * split is this implementation's contract (see atticpad.h), asserted by
 * st_pair_uri above. A vector's own strlen is checked against the bounded
 * scan so a length typo in the table shows up as itself rather than as a
 * parser bug.
 *
 * J.2 is an encode-side table, same shape as st_vec_text's: build into a
 * 0xA5-prefilled buffer, compare the exact bytes, the returned count, the
 * NUL, and the byte past it.
 */
static void st_vec_pair_uri(st_ctx *c)
{
    size_t i;

    for (i = 0; i < (size_t)APAD_PAIR_URI_PARSE_VECTOR_COUNT; i++) {
        const apad_vec_pair_uri_parse *v = &apad_pair_uri_parse_vectors[i];
        apad_pair_uri u;
        size_t n = 0;
        int rc;

        while (n <= (size_t)v->uri_len && v->uri[n] != '\0') {
            n++;
        }
        st_check2(c, v->name, st_name(c, "vec/pair_uri", v->name, "table_len"),
                  n == (size_t)v->uri_len);

        memset(&u, 0x5A, sizeof u);
        rc = apad_pair_uri_parse(&u, v->uri);

        if (v->exp_ok != 0u) {
            size_t k;
            int ok = (rc == APAD_OK);

            st_check2(c, v->name,
                      st_name(c, "vec/pair_uri", v->name, "parses"), ok);
            if (!ok) {
                continue;
            }
            st_check2(c, v->name, st_name(c, "vec/pair_uri", v->name, "addr"),
                      memcmp(u.addr.ip, v->exp_ip, 4u) == 0
                      && u.addr.port == v->exp_port);

            ok = 1;
            for (k = 0; k < (APAD_SECRET_MAX_LEN + 1u); k++) {
                if (u.secret[k] != v->exp_secret[k]) {
                    ok = 0;
                    break;
                }
                if (v->exp_secret[k] == '\0') {
                    break;
                }
            }
            st_check2(c, v->name,
                      st_name(c, "vec/pair_uri", v->name, "secret"), ok);
        } else {
            /* Rejected. Any negative apad_result satisfies the vector; that
             * it is NOT APAD_OK is the whole assertion, plus that a rejected
             * parse wrote nothing (0x5A prefill still intact). */
            st_check2(c, v->name,
                      st_name(c, "vec/pair_uri", v->name, "rejected"),
                      rc < 0);
            st_check2(c, v->name,
                      st_name(c, "vec/pair_uri", v->name, "out_untouched"),
                      u.secret[0] == (char)0x5A
                      && u.addr.ip[0] == 0x5Au);
        }
    }

    for (i = 0; i < (size_t)APAD_PAIR_URI_BUILD_VECTOR_COUNT; i++) {
        const apad_vec_pair_uri_build *v = &apad_pair_uri_build_vectors[i];
        char buf[APAD_PAIR_URI_MAX + 2u];
        apad_addr a;
        int rc;

        apad_addr_set(&a, v->in_ip[0], v->in_ip[1], v->in_ip[2], v->in_ip[3],
                      v->in_port);
        memset(buf, 0xA5, sizeof buf);
        rc = apad_pair_uri_build(buf, sizeof buf - 1u, &a, v->in_secret);

        st_check2(c, v->name,
                  st_name(c, "vec/pair_uri_build", v->name, "len"),
                  rc == (int)v->exp_uri_len);
        if (rc != (int)v->exp_uri_len) {
            continue;
        }
        st_check2(c, v->name,
                  st_name(c, "vec/pair_uri_build", v->name, "bytes"),
                  memcmp(buf, v->exp_uri, (size_t)v->exp_uri_len + 1u) == 0);
        st_check2(c, v->name,
                  st_name(c, "vec/pair_uri_build", v->name, "no_overrun"),
                  (unsigned char)buf[(size_t)v->exp_uri_len + 1u] == 0xA5u);
    }
}

static void st_vectors(st_ctx *c)
{
    int ok = 1;
    size_t i;

    /* §5.1: the LUT the spec prints, against the one object codec.c exports.
     * st_hat already transcribes it from the spec by hand; this compares it
     * against a second, independent transcription. */
    for (i = 0; i < 16u; i++) {
        if (apad_hat_lut[i] != apad_vec_hat_lut_ref[i]) {
            ok = 0;
        }
    }
    st_check(c, "vec/hat_lut_ref", ok);

    st_vec_frames(c);
    st_vec_truncation_sweep(c, "vec/trunc", apad_vec_truncation_canonical,
                            apad_truncation_vectors,
                            (size_t)APAD_TRUNCATION_VECTOR_COUNT);
    st_vec_truncation_sweep(c, "vec/auth_trunc",
                            apad_vec_auth_truncation_canonical,
                            apad_auth_truncation_vectors,
                            (size_t)APAD_AUTH_TRUNCATION_VECTOR_COUNT);
    st_vec_input_state(c);
    st_vec_wrap(c);
    st_vec_auth(c);
    st_vec_secret_length(c);
    st_vec_section_g(c);
    st_vec_text(c);
    st_vec_pair_uri(c);
}

/* ---- entry point ------------------------------------------------------- */

int apad_selftest_run(apad_selftest_result *out, apad_selftest_cb cb, void *user)
{
    st_ctx c;

    memset(&c, 0, sizeof c);
    c.cb   = cb;
    c.user = user;

    st_seq(&c);
    st_hat(&c);
    st_crypto(&c);
    st_header(&c);
    st_payloads(&c);
    st_input(&c);
    st_framing(&c);
    st_session(&c);
    st_addr(&c);
    st_pair_uri(&c);

    /* §13: the independently-authored golden data, last so that a failure in
     * the codec's own invariants is reported first — if both fail, the
     * invariant is the more informative one. */
    st_vectors(&c);

    if (out != NULL) {
        *out = c.r;
    }
    return (c.r.failed == 0u) ? APAD_OK : APAD_ERR_STATE;
}
