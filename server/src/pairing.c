/* server/src/pairing.c — the §10 pairing window: secret generation, the
 * 120 s deadline, and the five-attempt lockout.
 *
 * WHAT IS AND IS NOT HERE
 *
 * Not here: cryptography. apad_derive_session_key(), apad_pbkdf2_sha256(),
 * apad_packet_verify(), apad_ct_equal() and apad_secure_zero() all live in
 * core/ and are conformance-tested against docs/PROTOCOL.md Appendix A.
 * This file generates a string and decides when it is valid; server.c feeds
 * that string to core's derivation. Nothing in server/ implements a
 * primitive.
 *
 * Not here either: entropy. The library must not open /dev/urandom or call
 * BCryptGenRandom -- randomness is platform I/O and arrives through
 * cfg.on_random exactly as datagrams leave through cfg.on_send. A server
 * with no on_random cannot open a window at all, and a rotation that cannot
 * get randomness closes the window rather than continuing on a burnt
 * secret. There is no clock-derived, counter-derived or address-derived
 * fallback anywhere in this file, and adding one would be a security
 * regression dressed as robustness: a PIN drawn from the millisecond clock
 * is guessable by anyone who can see the server's log timestamps, but it
 * LOOKS exactly like a real PIN to the user reading it off the screen.
 *
 * WHY REJECTION SAMPLING
 *
 * The obvious `alphabet[byte % 10]` is biased: 256 = 25*10 + 6, so the
 * digits 0..5 come up 26 times per 256 draws and 6..9 come up 25. Over the
 * six digits of a PIN that is not a rounding error -- it concentrates the
 * already-tiny 10^6 space, and §10 is explicit that the PIN's small space
 * is the thing actually protecting the session. Drawing again on a byte
 * that falls in the ragged tail costs an average of 2.4% extra draws and
 * makes the distribution exactly uniform. gen_secret() below does that for
 * ANY alphabet size; the token alphabet happens to be 32 symbols, where the
 * rejection branch is unreachable because 256 is a multiple of 32, but the
 * code does not depend on that and would stay correct if a symbol were ever
 * removed.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "pairing.h"
#include "serverlog.h"

/* §10's PIN: ten symbols, so 6 of 256 byte values are rejected per draw. */
static const char kPinAlphabet[] = "0123456789";

/*
 * The token alphabet: digits 2-9 plus A-Z without I and O. Exactly 32
 * symbols, so one token character is 5 bits and a 20-character token is
 * 100 bits -- far outside the offline brute force §10 concedes for the
 * 6-digit PIN.
 *
 * The exclusions are for the human, not the maths. 0/O and 1/I are the
 * classic transcription collisions, and a token that fails to scan is
 * exactly the case where somebody reads it aloud or types it by hand off a
 * screen. Uppercase-only is why 'L' can stay: it is lowercase 'l' that
 * collides with '1', and no lowercase letter is ever generated. Removing
 * 'L' as well would drop the alphabet to 31 and cost the power-of-two
 * property for no legibility gain.
 */
static const char kTokenAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/* Random is drawn in blocks rather than a byte at a time: one on_random
 * call per block instead of one per character, which matters when the host
 * implementation is a syscall (getrandom) or a CNG call. */
#define RANDOM_BLOCK 32u

/*
 * Bound on how many random bytes one secret may consume before the attempt
 * is declared a failure. With a 10-symbol alphabet the acceptance
 * probability is 250/256, so 20 characters need ~20.5 draws on average and
 * the chance of needing more than 512 is around 10^-100. This exists only
 * so a broken on_random that returns a constant out-of-range byte forever
 * cannot hang the caller's UI thread in an infinite loop -- it is a
 * liveness guard, not a statistical one.
 */
#define MAX_DRAWS 512u

/* ------------------------------------------------------------------ */

static int draw_bytes(apad_server_random_fn rnd, void *user,
                      uint8_t *buf, size_t len)
{
    if (rnd == NULL) {
        return 0;
    }
    return rnd(user, buf, len) != 0;
}

/*
 * Fill out[0..count-1] with uniformly-distributed characters from
 * `alphabet` and NUL-terminate. Returns 1 on success, 0 if randomness was
 * unavailable or the draw budget was exhausted.
 *
 * On failure `out` is wiped rather than left holding a partially-filled
 * buffer: a half-random secret must never be mistaken for a secret.
 */
static int gen_secret(char *out, size_t out_cap, const char *alphabet,
                      size_t alpha_len, size_t count,
                      apad_server_random_fn rnd, void *user)
{
    uint8_t  block[RANDOM_BLOCK];
    size_t   have = 0, next = 0, written = 0;
    uint32_t draws = 0;
    unsigned limit;

    if (out == NULL || count + 1u > out_cap || alpha_len == 0u
        || alpha_len > 256u) {
        return 0;
    }
    memset(out, 0, out_cap);

    /* The largest multiple of alpha_len that fits in a byte. Anything at or
     * above it is the ragged tail and gets redrawn. For alpha_len 32 this
     * is 256, i.e. nothing is ever rejected; for 10 it is 250. */
    limit = (unsigned)((256u / alpha_len) * alpha_len);

    while (written < count) {
        uint8_t b;

        if (draws >= MAX_DRAWS) {
            apad_secure_zero(out, out_cap);
            apad_secure_zero(block, sizeof block);
            return 0;
        }
        if (have == 0u) {
            if (!draw_bytes(rnd, user, block, sizeof block)) {
                apad_secure_zero(out, out_cap);
                apad_secure_zero(block, sizeof block);
                return 0;
            }
            have = sizeof block;
            next = 0;
        }
        b = block[next++];
        have--;
        draws++;

        if ((unsigned)b >= limit) {
            continue;   /* ragged tail: redraw rather than fold it in */
        }
        out[written++] = alphabet[(size_t)b % alpha_len];
    }
    out[written] = '\0';
    apad_secure_zero(block, sizeof block);
    return 1;
}

/* Generate into p->secret according to p->kind. Wipes the old secret first
 * -- there is no moment where two secrets are live. */
static int regenerate(apad_pairing *p, int use_token,
                      apad_server_random_fn rnd, void *user)
{
    const char *alphabet;
    size_t      alpha_len, count;

    apad_secure_zero(p->secret, sizeof p->secret);

    if (use_token) {
        alphabet  = kTokenAlphabet;
        alpha_len = sizeof kTokenAlphabet - 1u;
        count     = (size_t)APAD_PAIRING_TOKEN_LEN;
    } else {
        alphabet  = kPinAlphabet;
        alpha_len = sizeof kPinAlphabet - 1u;
        count     = (size_t)APAD_PAIRING_PIN_LEN;
    }
    if (!gen_secret(p->secret, sizeof p->secret, alphabet, alpha_len, count,
                    rnd, user)) {
        return 0;
    }
    p->kind = (uint8_t)(use_token ? APAD_PAIRING_TOKEN : APAD_PAIRING_PIN);
    p->generation++;
    return 1;
}

static const char *kind_str(uint8_t kind)
{
    return (kind == (uint8_t)APAD_PAIRING_TOKEN) ? "token" : "PIN";
}

/* ------------------------------------------------------------------ */

int apad_pairing_open(apad_pairing *p, uint32_t now, int use_token,
                      apad_server_random_fn rnd, void *user,
                      const apad_log_sink *log)
{
    if (p == NULL) {
        return APAD_ERR_ARG;
    }
    if (rnd == NULL) {
        /* Not a soft failure and not something to work around. §10's whole
         * security argument rests on the secret being unguessable for 120
         * seconds; a host that cannot supply entropy cannot have pairing,
         * and saying so is more useful than a PIN that is not one. */
        apad_logf(log, APAD_LOG_ERROR,
                  "pairing refused: this host supplied no cfg.on_random, and "
                  "the server will not invent entropy for a S10 secret");
        return APAD_ERR_STATE;
    }

    /* Close FIRST, then generate. If generation fails the user is left with
     * no window rather than with the previous PIN still quietly live after
     * they asked for a new one. */
    apad_pairing_close(p, log, NULL);

    if (!regenerate(p, use_token, rnd, user)) {
        apad_logf(log, APAD_LOG_ERROR,
                  "pairing refused: cfg.on_random failed, so no S10 secret "
                  "could be generated (no window is open)");
        return APAD_ERR_STATE;
    }

    p->open          = 1;
    p->opened_ms     = now;
    p->deadline_ms   = now + (uint32_t)APAD_PAIRING_WINDOW_MS;
    p->observed_ms   = now;
    p->attempts_used = 0u;

    /* Deliberately secret-free: this line reaches BOTH stderr and the host
     * UI's log ring (apadserver.h's on_log doc comment; see
     * server/host/linux/webui.h ui_log_push()), and the ring outlives the
     * window -- /api/state stops carrying the secret the instant
     * `pairing.open` goes false, but a log line lives forever, which would
     * let the secret leak long after it authorised anything (the §10.3
     * reasoning for a displayed URI applies just as much to a displayed
     * PIN/token). The secret itself travels through the query API ONLY:
     * apad_server_pairing_state() / apad_pairing_snapshot() below. A
     * headless host has no other screen, so it MUST poll that and print the
     * secret itself -- see server/host/linux/main.c show_pairing(), which
     * does exactly that via a direct fprintf(stderr,...) that never touches
     * this log sink or its ring buffer. */
    apad_logf(log, APAD_LOG_INFO,
              "pairing open - PIN valid for %u s, %u attempts allowed",
              (unsigned)(APAD_PAIRING_WINDOW_MS / 1000u),
              (unsigned)APAD_MAX_PAIR_ATTEMPTS);
    return APAD_OK;
}

void apad_pairing_close(apad_pairing *p, const apad_log_sink *log,
                        const char *why)
{
    int was_open;

    if (p == NULL) {
        return;
    }
    was_open = p->open;

    /* Unconditional, not "if (was_open)": the buffer is wiped on every path
     * out of this function so there is no arrangement of calls that leaves
     * a dead secret sitting in the server's memory. */
    apad_secure_zero(p->secret, sizeof p->secret);
    p->open          = 0;
    p->kind          = 0u;
    p->opened_ms     = 0u;
    p->deadline_ms   = 0u;
    p->attempts_used = 0u;
    /* generation and observed_ms deliberately survive: generation is a
     * monotonic UI signal for the life of the server, and observed_ms is
     * the clock, not state. */

    if (was_open && why != NULL) {
        apad_logf(log, APAD_LOG_INFO, "pairing window closed: %s", why);
    }
}

int apad_pairing_is_open(const apad_pairing *p, uint32_t now)
{
    if (p == NULL || !p->open) {
        return 0;
    }
    /* apad_time_reached, never `now >= deadline`: the raw comparison is
     * wrong across the 2^32 ms wrap at ~49.7 days of uptime, which is
     * exactly the sort of bug that passes every test and then bricks a
     * long-running server (docs/CONVENTIONS.md). */
    return apad_time_reached(now, p->deadline_ms) ? 0 : 1;
}

int apad_pairing_tick(apad_pairing *p, uint32_t now, const apad_log_sink *log)
{
    if (p == NULL) {
        return 0;
    }
    p->observed_ms = now;

    if (p->open && apad_time_reached(now, p->deadline_ms)) {
        apad_pairing_close(p, log, "120 s window elapsed");
        return 1;
    }
    return 0;
}

void apad_pairing_observe(apad_pairing *p, uint32_t now)
{
    if (p != NULL) {
        p->observed_ms = now;
    }
}

int apad_pairing_fail(apad_pairing *p, uint32_t generation,
                      apad_server_random_fn rnd, void *user,
                      const apad_log_sink *log)
{
    if (p == NULL || !p->open) {
        return 0;
    }
    /* A guess at a secret that has already been replaced is not a guess at
     * this one. Without this check, one client that keeps retrying with a
     * PIN from two rotations ago walks the counter down through every
     * subsequent PIN and the user watches the number on screen change every
     * few seconds for no visible reason. */
    if (generation != p->generation) {
        return 0;
    }

    if (p->attempts_used < (uint8_t)APAD_MAX_PAIR_ATTEMPTS) {
        p->attempts_used = (uint8_t)(p->attempts_used + 1u);
    }
    if (p->attempts_used < (uint8_t)APAD_MAX_PAIR_ATTEMPTS) {
        apad_logf(log, APAD_LOG_WARN,
                  "pairing: failed attempt %u of %u (S11) -- %u left",
                  (unsigned)p->attempts_used,
                  (unsigned)APAD_MAX_PAIR_ATTEMPTS,
                  (unsigned)(APAD_MAX_PAIR_ATTEMPTS - p->attempts_used));
        return 0;
    }

    /* §11: five failed attempts invalidate the secret and generate a new
     * one. The DEADLINE is not touched: §10 anchors the 120 s to "after the
     * user initiates pairing", not to the secret, so a rotation at t=100 s
     * leaves 20 s -- the user is present and can press Pair again. The
     * attempt counter DOES reset, and that is not a hole: the five guesses
     * that were just spent were spent against a secret that no longer
     * exists, and an attacker cannot see the new one. Rotating IS the
     * defence; the counter is only what triggers it. */
    if (!regenerate(p, p->kind == (uint8_t)APAD_PAIRING_TOKEN, rnd, user)) {
        apad_pairing_close(p, log,
                           "5 failed attempts and no replacement PIN could "
                           "be generated");
        return 1;
    }
    p->attempts_used = 0u;
    /* Secret-free for the same reason as the open-window line above: the
     * replacement secret goes out through apad_server_pairing_state(),
     * never through this sink. */
    apad_logf(log, APAD_LOG_WARN,
              "pairing: %u failed attempts (S11) -- new %s issued, "
              "generation %u, same 120 s deadline",
              (unsigned)APAD_MAX_PAIR_ATTEMPTS, kind_str(p->kind),
              (unsigned)p->generation);
    return 1;
}

const char *apad_pairing_secret(const apad_pairing *p)
{
    if (p == NULL || !p->open) {
        return NULL;
    }
    return p->secret;
}

void apad_pairing_snapshot(const apad_pairing *p, apad_pairing_info *out)
{
    uint32_t left;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (p == NULL || !p->open) {
        return;
    }
    /* Measured against observed_ms, the last clock the server was handed --
     * see apad_pairing::observed_ms and the ms_remaining comment in
     * apadserver.h. A window whose deadline has passed but which nothing
     * has reaped yet reports closed here rather than showing a live PIN
     * with 0 ms left. */
    if (apad_time_reached(p->observed_ms, p->deadline_ms)) {
        return;
    }
    left = apad_time_since(p->deadline_ms, p->observed_ms);

    out->open               = 1;
    out->kind               = p->kind;
    memcpy(out->secret, p->secret, sizeof out->secret);
    out->secret[sizeof out->secret - 1u] = '\0';
    out->ms_remaining       = left;
    out->attempts_remaining =
        (uint8_t)((uint8_t)APAD_MAX_PAIR_ATTEMPTS - p->attempts_used);
    out->generation         = p->generation;
}
