/* server/src/kbm.h — the §6.15-§6.19 diffing engine (KEYBOARD, MOUSE,
 * MEDIA), sibling to server/src/mapping.c and just as server-only.
 *
 * Same split as mapping.c: this owns per-session held state and turns each
 * accepted KEYBOARD/MOUSE/MEDIA snapshot into backend-neutral events
 * (apad_kbm_event_out, apad_mouse_motion — server/backends/backend.h), which
 * it hands to the backend directly through the kbm_events()/mouse_motion()
 * hooks. server/src/server.c owns everything this file does NOT: dispatch,
 * lazy device creation, the §6.19 INPUTCAPS delivery schedule, and routing a
 * datagram through §6.20's per-type staleness window
 * (apad_session_accept_kbm, core/src/session.c) before it ever reaches here.
 *
 * §6.20's receiver algorithm, exactly, for every accepted datagram of a
 * type:
 *   1. First accepted message of that type this session: adopt event_seq,
 *      replay no ring events, and let the unconditional reconcile below
 *      apply the snapshot (the receiver holds nothing yet, so reconciling
 *      against an all-zero shadow IS "apply the snapshot as state").
 *   2. Otherwise replay apad_event_ring_new()'s trailing slots, ascending,
 *      skipping "no event" slots -- each replayed slot becomes one
 *      apad_kbm_event_out AND updates the local shadow so step 3 only
 *      corrects what replay could not (event loss, overflow, or a
 *      non-conforming sender).
 *   3. UNCONDITIONALLY, including when nothing was replayed: diff the
 *      shadow against the datagram's snapshot and emit whatever
 *      presses/releases make them equal. This is the whole convergence
 *      argument (§6.20) and is never skipped.
 *   4. Store event_seq as last_applied.
 *
 * §6.16's baseline rule for MOUSE motion is separate from all of the above:
 * the first accepted MOUSE of a session produces zero motion and only
 * establishes the accumulator baseline; every later accepted one reports
 * apad_seq_diff(new, previous) (§6.21). A discarded (stale) packet never
 * reaches this file at all -- server.c drops it at the §6.20 window before
 * calling in, which is what "MUST NOT advance the baseline" requires.
 *
 * No profile involvement, deliberately: the JSONC schema (server/src/
 * profiles.c) is untouched by this feature. Server-only, ordinary hosted C
 * (docs/CONVENTIONS.md) -- malloc/float/stdio are all fine, though nothing here
 * actually needs any of them.
 */
#ifndef ATTICPAD_SERVER_KBM_H
#define ATTICPAD_SERVER_KBM_H

#include <stdint.h>

#include "atticpad/atticpad.h"
#include "backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-session KBM state, owned entirely by this file. server/src/server.c
 * embeds one per session slot, zero-initialises it with
 * apad_kbm_state_init() the moment a slot is newly allocated (same instant
 * it calls apad_mapping_state_init() for mapping.c's own per-session state),
 * and never reaches inside otherwise.
 *
 * "Shadow" below means this session's belief of what the backend currently
 * holds for that facility -- the thing §6.20 step 3 reconciles against each
 * datagram's snapshot. It is NOT reset by apad_kbm_release_all(): a
 * teardown zeroes the backend's own held state (which the release emits
 * events for) but a fresh apad_kbm_state_init() is what actually clears
 * this struct, exactly the same convention server.c already uses for
 * apad_mapping_state.
 */
typedef struct {
    /* ---- KEYBOARD (§6.15) --------------------------------------------- */
    uint8_t  kb_created;        /* backend keyboard device exists          */
    uint8_t  kb_have_baseline;  /* a first KEYBOARD has been applied       */
    uint16_t kb_last_applied;   /* last_applied event_seq (§6.20)          */
    uint8_t  kb_shadow[APAD_KEY_BITMAP_BYTES]; /* held-key bitmap, wire layout */
    uint32_t kb_last_accept_ms; /* watchdog: last accepted KEYBOARD (ms)   */

    /* ---- MOUSE (§6.16) ------------------------------------------------- */
    uint8_t  mo_created;
    uint8_t  mo_have_baseline;  /* accumulator baseline established        */
    uint16_t mo_last_applied;   /* last_applied event_seq for buttons only */
    uint16_t mo_shadow_buttons; /* held-button bitmask, APAD_MOUSEBTN_BIT()*/
    uint16_t mo_last_dx, mo_last_dy, mo_last_wheel, mo_last_hwheel;
    uint32_t mo_last_accept_ms; /* watchdog: covers buttons only (§6.20)   */

    /* ---- MEDIA (§6.17) -------------------------------------------------- */
    uint8_t  me_created;
    uint8_t  me_have_baseline;
    uint16_t me_last_applied;
    uint32_t me_shadow_held;    /* held mask, APAD_MEDIA_BIT()             */
    uint32_t me_last_accept_ms;
} apad_kbm_state;

void apad_kbm_state_init(apad_kbm_state *st);

/*
 * Record that `dev`'s backend device was just created successfully.
 * server.c calls backend->create_kbm() itself -- lazy creation is dispatch,
 * not diffing, and belongs there (mirrors handle_hello's create_pad) -- and
 * reports the result back here so the reconcile logic above and the two
 * queries below know a device now exists. Idempotent.
 */
void apad_kbm_note_created(apad_kbm_state *st, apad_kbm_device dev);

/*
 * True once apad_kbm_note_created() has recorded `dev` as created and it has
 * not since been released by apad_kbm_release_all(). This -- not a direct
 * struct read -- is how server.c builds INPUTCAPS.status's *_READY bits
 * without reaching inside apad_kbm_state.
 */
int apad_kbm_is_created(const apad_kbm_state *st, apad_kbm_device dev);

/*
 * Apply one ACCEPTED KEYBOARD/MOUSE/MEDIA datagram: run §6.20's receiver
 * algorithm above, translate the result into apad_kbm_event_out (and, for
 * MOUSE, an apad_mouse_motion) and hand it to `backend` via kbm_events()/
 * mouse_motion(). `slot` is the backend's own pad/session slot index -- the
 * same one create_pad/create_kbm/destroy_kbm already use.
 *
 * The caller (server.c) is responsible for everything before this point:
 * confirming the facility was advertised (INPUTCAPS.features), lazily
 * creating the device, and passing only a datagram that already survived
 * §6.20's per-type staleness window (apad_session_accept_kbm). `backend`
 * must be non-NULL and its device for this facility must already be
 * created; calling this before create_kbm succeeded is a caller bug (mirrors
 * every other backend.h hook's contract).
 *
 * `now` refreshes this facility's §6.20 watchdog timestamp -- see
 * apad_kbm_watchdog() below.
 *
 * `out_lost`, if non-NULL, is set to the number of events this call could
 * not replay because event_seq's gap since the last accepted datagram of
 * this type exceeded the ring depth (0 -- the common case -- whenever it
 * did not, including the first-accepted baseline). §6.20 step 2 requires
 * this to be made observable; this file has no log sink and stays that way
 * (a sans-IO diffing engine, matching mapping.c's own split), so it hands
 * the number back rather than logging it itself. Read it even when it is
 * 0 -- the caller decides how and how often to report it, not this file.
 * Critically, the unconditional reconcile in step 3 above restores the
 * *shadow state* the gap disturbed, but it CANNOT reconstruct an edge that
 * happened entirely inside the gap (e.g. a tap pressed and released before
 * the next accepted snapshot) -- the state converges, the keystroke is
 * still gone, and `out_lost` is the only place that loss is visible at
 * all. A caller MUST NOT treat a reconciled shadow as evidence nothing was
 * lost.
 */
void apad_kbm_apply_keyboard(const apad_backend *backend, int slot,
                             apad_kbm_state *st, const apad_keyboard *in,
                             uint32_t now, unsigned *out_lost);
void apad_kbm_apply_mouse(const apad_backend *backend, int slot,
                          apad_kbm_state *st, const apad_mouse *in,
                          uint32_t now, unsigned *out_lost);
void apad_kbm_apply_media(const apad_backend *backend, int slot,
                          apad_kbm_state *st, const apad_media *in,
                          uint32_t now, unsigned *out_lost);

/*
 * §6.20 watchdog: a receiver that believes something is held for a facility
 * and has accepted no datagram of that type for APAD_KBM_WATCHDOG_MS (1000)
 * MUST release everything it holds for that facility. Call once per tick,
 * per live session, regardless of whether anything is actually held or
 * created -- a no-op when nothing is. Motion is exempt (§6.20: "the
 * held-repeat obligation covers buttons only"), so this only ever releases
 * KEYBOARD.keys[], MOUSE.buttons and MEDIA.held.
 */
void apad_kbm_watchdog(const apad_backend *backend, int slot,
                       apad_kbm_state *st, uint32_t now);

/*
 * Release everything held across all three facilities and destroy any
 * device this session created, in that order (§6.20: "before it destroys or
 * detaches whatever it injects into"). MUST be called on every teardown
 * path, for any reason -- BYE, idle timeout, a revoked slot, or server
 * shutdown. Idempotent: safe to call on a state that never created
 * anything, and safe to call twice.
 */
void apad_kbm_release_all(const apad_backend *backend, int slot,
                          apad_kbm_state *st);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_KBM_H */
