/* hmac_sha256.c — SHA-256, HMAC-SHA256, PBKDF2 (docs/PROTOCOL.md §10).
 *
 * Written from FIPS 180-4 (SHA-256), FIPS 198-1 (HMAC) and RFC 8018 (PBKDF2).
 * No third-party code, no allocation, no floating point, no stdio. Fixed
 * stack cost: the PBKDF2 driver below uses about 200 bytes plus the contexts.
 *
 * Everything here must run on a 67 MHz ARM9. PBKDF2 at the 10,000 iterations
 * §10 mandates costs roughly one second there, which is the whole reason the
 * iteration count is 10,000 and not 600,000 (docs/DESIGN.md D3).
 *
 * The security of this protocol does not come from any of this code; it comes
 * from the 120-second pairing window, the five-attempt limit, and LAN-only
 * binding (§10). What this code must do is be correct and constant-time where
 * it claims to be.
 */

#include <string.h>

#include "atticpad/atticpad.h"

/* ---- SHA-256 (FIPS 180-4 §6.2) ----------------------------------------- */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror32(uint32_t x, unsigned n)
{
    return (uint32_t)((x >> n) | (x << (32u - n)));
}

#define BSIG0(x) (ror32((x), 2)  ^ ror32((x), 13) ^ ror32((x), 22))
#define BSIG1(x) (ror32((x), 6)  ^ ror32((x), 11) ^ ror32((x), 25))
#define SSIG0(x) (ror32((x), 7)  ^ ror32((x), 18) ^ ((x) >> 3))
#define SSIG1(x) (ror32((x), 17) ^ ror32((x), 19) ^ ((x) >> 10))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* SHA-256 is big-endian on the wire, unlike the AtticPad protocol. Assembled
 * byte by byte for the same reasons: no host assumption, no unaligned load. */
static uint32_t be32_read(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void be32_write(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void sha256_block(uint32_t *h, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    unsigned t;

    for (t = 0; t < 16u; t++) {
        w[t] = be32_read(block + (t * 4u));
    }
    for (t = 16u; t < 64u; t++) {
        w[t] = SSIG1(w[t - 2]) + w[t - 7] + SSIG0(w[t - 15]) + w[t - 16];
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    for (t = 0; t < 64u; t++) {
        uint32_t t1 = hh + BSIG1(e) + CH(e, f, g) + K[t] + w[t];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);
        hh = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;

    /* w is deliberately NOT wiped here. PBKDF2 at 10,000 iterations runs
     * ~40,000 compressions; a 256-byte volatile wipe per compression is ~10 MB
     * of byte stores, which on a 67 MHz ARM9 would roughly double the pairing
     * time that docs/DESIGN.md D3 already calls the user's tolerance limit. The
     * schedule is scratch stack that the next compression overwrites, and the
     * key material itself is wiped by the context wipes below. */
}

void apad_sha256_init(apad_sha256_ctx *c)
{
    if (c == NULL) {
        return;
    }
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->total = 0u;
    c->used  = 0u;
    memset(c->block, 0, sizeof c->block);
}

void apad_sha256_update(apad_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (c == NULL || (p == NULL && len != 0u)) {
        return;
    }
    c->total += (uint64_t)len;

    if (c->used != 0u) {
        size_t take = (size_t)(APAD_SHA256_BLOCK_LEN - c->used);
        if (take > len) {
            take = len;
        }
        memcpy(c->block + c->used, p, take);
        c->used = (uint8_t)(c->used + take);
        p   += take;
        len -= take;
        if (c->used == APAD_SHA256_BLOCK_LEN) {
            sha256_block(c->state, c->block);
            c->used = 0u;
        }
    }
    while (len >= (size_t)APAD_SHA256_BLOCK_LEN) {
        sha256_block(c->state, p);
        p   += APAD_SHA256_BLOCK_LEN;
        len -= APAD_SHA256_BLOCK_LEN;
    }
    if (len != 0u) {
        memcpy(c->block, p, len);
        c->used = (uint8_t)len;
    }
}

void apad_sha256_final(apad_sha256_ctx *c, uint8_t out[APAD_SHA256_DIGEST_LEN])
{
    uint64_t bits;
    unsigned i;

    if (c == NULL || out == NULL) {
        return;
    }
    bits = c->total * 8u;

    c->block[c->used] = 0x80u;
    c->used = (uint8_t)(c->used + 1u);
    if (c->used > 56u) {
        memset(c->block + c->used, 0, (size_t)(APAD_SHA256_BLOCK_LEN - c->used));
        sha256_block(c->state, c->block);
        c->used = 0u;
    }
    memset(c->block + c->used, 0, (size_t)(56u - c->used));
    be32_write(c->block + 56, (uint32_t)((bits >> 32) & 0xFFFFFFFFu));
    be32_write(c->block + 60, (uint32_t)(bits & 0xFFFFFFFFu));
    sha256_block(c->state, c->block);

    for (i = 0; i < 8u; i++) {
        be32_write(out + (i * 4u), c->state[i]);
    }
    apad_secure_zero(c, sizeof *c);
}

void apad_sha256(const void *data, size_t len, uint8_t out[APAD_SHA256_DIGEST_LEN])
{
    apad_sha256_ctx c;
    apad_sha256_init(&c);
    apad_sha256_update(&c, data, len);
    apad_sha256_final(&c, out);
}

/* ---- HMAC-SHA256 (FIPS 198-1) ------------------------------------------ */

void apad_hmac_sha256_init(apad_hmac_ctx *c, const uint8_t *key, size_t key_len)
{
    uint8_t k0[APAD_SHA256_BLOCK_LEN];
    uint8_t ipad[APAD_SHA256_BLOCK_LEN];
    unsigned i;

    if (c == NULL) {
        return;
    }
    memset(k0, 0, sizeof k0);
    if (key_len > (size_t)APAD_SHA256_BLOCK_LEN) {
        apad_sha256(key, key_len, k0);          /* K0 = H(K), zero-padded */
    } else if (key_len != 0u && key != NULL) {
        memcpy(k0, key, key_len);
    }

    for (i = 0; i < (unsigned)APAD_SHA256_BLOCK_LEN; i++) {
        ipad[i]    = (uint8_t)(k0[i] ^ 0x36u);
        c->opad[i] = (uint8_t)(k0[i] ^ 0x5Cu);
    }

    apad_sha256_init(&c->inner);
    apad_sha256_update(&c->inner, ipad, sizeof ipad);

    apad_secure_zero(k0, sizeof k0);
    apad_secure_zero(ipad, sizeof ipad);
}

void apad_hmac_sha256_update(apad_hmac_ctx *c, const void *data, size_t len)
{
    if (c == NULL) {
        return;
    }
    apad_sha256_update(&c->inner, data, len);
}

void apad_hmac_sha256_final(apad_hmac_ctx *c, uint8_t out[APAD_SHA256_DIGEST_LEN])
{
    uint8_t inner[APAD_SHA256_DIGEST_LEN];
    apad_sha256_ctx outer;

    if (c == NULL || out == NULL) {
        return;
    }
    apad_sha256_final(&c->inner, inner);

    apad_sha256_init(&outer);
    apad_sha256_update(&outer, c->opad, sizeof c->opad);
    apad_sha256_update(&outer, inner, sizeof inner);
    apad_sha256_final(&outer, out);

    apad_secure_zero(inner, sizeof inner);
    apad_secure_zero(c, sizeof *c);
}

void apad_hmac_sha256(const uint8_t *key, size_t key_len,
                      const void *data, size_t len,
                      uint8_t out[APAD_SHA256_DIGEST_LEN])
{
    apad_hmac_ctx c;
    apad_hmac_sha256_init(&c, key, key_len);
    apad_hmac_sha256_update(&c, data, len);
    apad_hmac_sha256_final(&c, out);
}

/* ---- PBKDF2-HMAC-SHA256 (RFC 8018 §5.2) -------------------------------- */

void apad_pbkdf2_sha256(const uint8_t *pw, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t *out, size_t out_len)
{
    uint8_t u[APAD_SHA256_DIGEST_LEN];
    uint8_t t[APAD_SHA256_DIGEST_LEN];
    uint8_t counter[4];
    uint32_t block = 1u;
    size_t done = 0u;

    if (out == NULL || out_len == 0u || iterations == 0u) {
        return;
    }

    while (done < out_len) {
        apad_hmac_ctx c;
        uint32_t iter;
        size_t take;
        unsigned i;

        /* INT(block), big-endian per RFC 8018. */
        counter[0] = (uint8_t)((block >> 24) & 0xFFu);
        counter[1] = (uint8_t)((block >> 16) & 0xFFu);
        counter[2] = (uint8_t)((block >> 8) & 0xFFu);
        counter[3] = (uint8_t)(block & 0xFFu);

        apad_hmac_sha256_init(&c, pw, pw_len);
        apad_hmac_sha256_update(&c, salt, salt_len);
        apad_hmac_sha256_update(&c, counter, sizeof counter);
        apad_hmac_sha256_final(&c, u);
        memcpy(t, u, sizeof t);

        for (iter = 1u; iter < iterations; iter++) {
            apad_hmac_sha256(pw, pw_len, u, sizeof u, u);
            for (i = 0; i < (unsigned)APAD_SHA256_DIGEST_LEN; i++) {
                t[i] = (uint8_t)(t[i] ^ u[i]);
            }
        }

        take = out_len - done;
        if (take > (size_t)APAD_SHA256_DIGEST_LEN) {
            take = (size_t)APAD_SHA256_DIGEST_LEN;
        }
        memcpy(out + done, t, take);
        done += take;
        block++;
    }

    apad_secure_zero(u, sizeof u);
    apad_secure_zero(t, sizeof t);
}

void apad_derive_session_key(const char *pin,
                             const uint8_t server_nonce[APAD_NONCE_LEN],
                             uint8_t out[APAD_SESSION_KEY_LEN])
{
    size_t n = 0u;

    if (out == NULL) {
        return;
    }
    memset(out, 0, APAD_SESSION_KEY_LEN);
    if (pin == NULL || server_nonce == NULL) {
        return;
    }
    /* §10.1: the secret is 6..64 bytes and MUST be used IN FULL — it is not
     * necessarily six digits. Bound tested BEFORE the dereference so the scan
     * cannot read past APAD_SECRET_MAX_LEN even if the caller's string is
     * unterminated. Over-length input is truncated here, which is documented
     * on the declaration; see the note there for why this function cannot
     * reject instead. */
    while (n < (size_t)APAD_SECRET_MAX_LEN && pin[n] != '\0') {
        n++;
    }
    /* §10: PBKDF2-HMAC-SHA256, 10,000 iterations, salted with server_nonce.
     * The secret itself never appears on the wire. */
    apad_pbkdf2_sha256((const uint8_t *)pin, n,
                       server_nonce, APAD_NONCE_LEN,
                       APAD_PBKDF2_ITERATIONS,
                       out, APAD_SESSION_KEY_LEN);
}

/* ---- constant-time helpers --------------------------------------------- */

int apad_ct_equal(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    uint8_t diff = 0u;
    size_t i;

    if (a == NULL || b == NULL) {
        return 1;
    }
    /* No early exit: runtime depends on n and nothing else (§10). */
    for (i = 0; i < n; i++) {
        diff = (uint8_t)(diff | (uint8_t)(pa[i] ^ pb[i]));
    }
    return (diff == 0u) ? 0 : 1;
}

void apad_secure_zero(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    size_t i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        q[i] = 0u;
    }
}
