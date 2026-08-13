/* server/src/pairing.h — library-internal view of the §10 pairing window.
 *
 * Not part of libapadserver's public surface: a host talks to pairing
 * through apad_server_begin_pairing / _cancel_pairing / _pairing_state in
 * server/include/apadserver.h. This header is the seam between pairing.c
 * (which owns the policy: secret generation, the 120 s window, the
 * five-attempt lockout) and server.c (which owns the session table and
 * therefore the wire).
 *
 * The split is deliberate. pairing.c knows nothing about sessions, WELCOME,
 * or datagrams; server.c knows nothing about how a PIN is generated or when
 * it rotates. What crosses this line is one struct and eight functions.
 */
#ifndef ATTICPAD_SERVER_PAIRING_H
#define ATTICPAD_SERVER_PAIRING_H

#include <stddef.h>
#include <stdint.h>

#include "apadserver.h"
#include "serverlog.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The whole pairing state. One of these lives inside struct apad_server;
 * nothing here is allocated and nothing here is shared between servers.
 *
 * `secret` is the only piece of key material the server holds outside a
 * session, and it is wiped with apad_secure_zero() every time it stops
 * being current -- on close, on expiry, and immediately before a
 * replacement is generated.
 */
typedef struct {
    int      open;            /* a window is armed                          */
    uint8_t  kind;            /* apad_pairing_kind                          */
    char     secret[APAD_PAIRING_SECRET_MAX];  /* NUL-terminated            */

    uint32_t opened_ms;       /* when the user asked for this window        */
    uint32_t deadline_ms;     /* opened_ms + APAD_PAIRING_WINDOW_MS (§11)   */

    /* The most recent now_ms the server was handed, from EITHER entry
     * point. Display only: apad_server_pairing_state() has no clock of its
     * own (it is const and takes no now_ms), so this is what its countdown
     * is measured against. Never used to decide whether a window is open
     * for the purpose of issuing a key -- that decision always uses the
     * now_ms of the datagram being handled. */
    uint32_t observed_ms;

    uint8_t  attempts_used;   /* toward APAD_MAX_PAIR_ATTEMPTS (§11)        */

    /* Bumped on every NEW secret. Sessions record the generation their key
     * was derived from, so a failure can be charged against the secret it
     * was actually a guess at -- and a stale session cannot burn an attempt
     * belonging to a secret it never saw. Also handed to UIs so they know
     * when to redraw. */
    uint32_t generation;
} apad_pairing;

/*
 * Open (or replace) a window: fresh secret, deadline at
 * now + APAD_PAIRING_WINDOW_MS, attempts reset, generation bumped.
 *
 * Returns APAD_OK, or APAD_ERR_STATE when `rnd` is NULL or fails -- in
 * which case nothing changes except that any previous window is already
 * closed and wiped (a failed re-pair must not leave the old PIN live, since
 * the user believes they just replaced it). Reports through `log`.
 */
int  apad_pairing_open(apad_pairing *p, uint32_t now, int use_token,
                       apad_server_random_fn rnd, void *user,
                       const apad_log_sink *log);

/* Close and wipe. `why` names the reason in the log; NULL logs nothing,
 * which is what a close of an already-closed window does anyway. */
void apad_pairing_close(apad_pairing *p, const apad_log_sink *log,
                        const char *why);

/* Is a window open AS OF `now`? Deadline-authoritative: returns 0 for a
 * window whose 120 s elapsed even if nothing has reaped it yet, so a late
 * or absent tick can never leak an expired secret onto the wire. */
int  apad_pairing_is_open(const apad_pairing *p, uint32_t now);

/*
 * Record that the server has observed `now`, and reap the window if it has
 * expired. Returns 1 exactly on the call where expiry happens (so the
 * caller can drop sessions that were still mid-handshake), 0 otherwise.
 * Called from apad_server_tick().
 */
int  apad_pairing_tick(apad_pairing *p, uint32_t now, const apad_log_sink *log);

/* Record that the server has observed `now`, without reaping. Called from
 * apad_server_on_datagram(), which has no business tearing sessions down
 * from underneath the handler it is about to dispatch to. */
void apad_pairing_observe(apad_pairing *p, uint32_t now);

/*
 * Charge one failed pairing attempt against the secret of `generation`.
 *
 * A charge from a stale generation is ignored: the session was guessing at
 * a secret that no longer exists, and letting it count would let one
 * confused client walk the counter down through every subsequent PIN.
 *
 * Returns 1 if this charge was the fifth and the secret has been ROTATED
 * (§11: "five failed attempts invalidate the PIN and generate a new one"),
 * 0 otherwise. The 120 s deadline is NOT extended by a rotation -- §10
 * anchors it to the moment "the user initiates pairing", not to the PIN.
 * If the rotation itself cannot get randomness the window is closed rather
 * than left running on a secret five people have already guessed at.
 */
int  apad_pairing_fail(apad_pairing *p, uint32_t generation,
                       apad_server_random_fn rnd, void *user,
                       const apad_log_sink *log);

/* The current secret, or NULL when no window is open. Valid until the next
 * open/close/rotate; callers derive a key from it immediately and never
 * store it. */
const char *apad_pairing_secret(const apad_pairing *p);

/* Fill the public snapshot. `out` must be non-NULL. */
void apad_pairing_snapshot(const apad_pairing *p, apad_pairing_info *out);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_PAIRING_H */
