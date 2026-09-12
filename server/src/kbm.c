/* server/src/kbm.c — the §6.15-§6.19 diffing engine. See kbm.h for the
 * shape of the split between this file and server.c.
 *
 * Server-only, ordinary hosted C (docs/CONVENTIONS.md). No profile involvement: the
 * JSONC schema (profiles.c) is untouched by this feature, and this file
 * knows nothing about it.
 */
#include <string.h>

#include "kbm.h"

void apad_kbm_state_init(apad_kbm_state *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof *st);
}

void apad_kbm_note_created(apad_kbm_state *st, apad_kbm_device dev)
{
    if (st == NULL) {
        return;
    }
    switch (dev) {
    case APAD_KBM_DEV_KEYBOARD: st->kb_created = 1u; break;
    case APAD_KBM_DEV_MOUSE:    st->mo_created = 1u; break;
    case APAD_KBM_DEV_MEDIA:    st->me_created = 1u; break;
    default: break;
    }
}

int apad_kbm_is_created(const apad_kbm_state *st, apad_kbm_device dev)
{
    if (st == NULL) {
        return 0;
    }
    switch (dev) {
    case APAD_KBM_DEV_KEYBOARD: return st->kb_created ? 1 : 0;
    case APAD_KBM_DEV_MOUSE:    return st->mo_created ? 1 : 0;
    case APAD_KBM_DEV_MEDIA:    return st->me_created ? 1 : 0;
    default: return 0;
    }
}

/* ======================================================================== */
/* KEYBOARD (§6.15) — 256-bit bitmap, ring depth 8                          */
/* ======================================================================== */

void apad_kbm_apply_keyboard(const apad_backend *backend, int slot,
                             apad_kbm_state *st, const apad_keyboard *in,
                             uint32_t now, unsigned *out_lost)
{
    /* Ring replay (<= depth) plus a full-bitmap reconcile (<= 256 bits) --
     * see kbm.h's file header for why both are needed and why the reconcile
     * is unconditional. */
    apad_kbm_event_out evbuf[APAD_KEYBOARD_RING_DEPTH
                             + APAD_KEY_BITMAP_BYTES * 8u];
    size_t   n = 0;
    unsigned byte_i;

    if (out_lost != NULL) {
        *out_lost = 0u;
    }
    if (backend == NULL || st == NULL || in == NULL) {
        return;
    }
    st->kb_last_accept_ms = now;

    if (!st->kb_have_baseline) {
        /* §6.20 step 1: first accepted -- no ring replay. kb_shadow starts
         * all-zero (apad_kbm_state_init), so the unconditional reconcile
         * below is exactly "apply the snapshot as state". */
        st->kb_have_baseline = 1u;
    } else {
        int overflow = 0;
        int gap = apad_event_ring_new(in->event_seq, st->kb_last_applied,
                                      (int)APAD_KEYBOARD_RING_DEPTH,
                                      &overflow);
        int start = (int)APAD_KEYBOARD_RING_DEPTH - gap;
        int idx;

        /* The reconcile below absorbs the STATE divergence a gap leaves
         * behind, but not the EDGE: a key pressed and released entirely
         * inside a lost span leaves the snapshot identical either way, so
         * nothing downstream of the reconcile can ever tell that keystroke
         * happened. `overflow` is the only signal that it might have --
         * report it (as an event count, not a bare flag) rather than
         * discarding it; the caller decides how to surface it (§6.20 step
         * 2, kbm.h's own doc comment on this function). */
        if (overflow && out_lost != NULL) {
            int true_gap = apad_seq_diff(in->event_seq, st->kb_last_applied);
            if (true_gap > (int)APAD_KEYBOARD_RING_DEPTH) {
                *out_lost = (unsigned)(true_gap - (int)APAD_KEYBOARD_RING_DEPTH);
            }
        }

        for (idx = start; idx < (int)APAD_KEYBOARD_RING_DEPTH; idx++) {
            const apad_key_event *e = &in->events[idx];
            unsigned byte, mask;

            if (e->usage == 0u) {
                continue;   /* "no event" slot: skip (§6.20) */
            }
            byte = APAD_KEY_BYTE(e->usage);
            mask = APAD_KEY_MASK(e->usage);

            if (backend->kbm_events != NULL
                && n < (sizeof evbuf / sizeof evbuf[0])) {
                evbuf[n].device = (uint8_t)APAD_KBM_DEV_KEYBOARD;
                evbuf[n].code   = e->usage;
                evbuf[n].down   =
                    (uint8_t)((e->flags & APAD_KBM_EVENT_DOWN) ? 1u : 0u);
                n++;
            }
            if (e->flags & APAD_KBM_EVENT_DOWN) {
                st->kb_shadow[byte] = (uint8_t)(st->kb_shadow[byte] | mask);
            } else {
                st->kb_shadow[byte] =
                    (uint8_t)(st->kb_shadow[byte] & (unsigned)~mask);
            }
        }
    }

    /* §6.20 step 3: unconditional reconcile against the snapshot, every
     * time, including a gap of zero. This is the whole convergence
     * argument -- never skip it. */
    for (byte_i = 0; byte_i < APAD_KEY_BITMAP_BYTES; byte_i++) {
        uint8_t diff = (uint8_t)(st->kb_shadow[byte_i] ^ in->keys[byte_i]);
        unsigned bit;

        if (diff == 0u) {
            continue;
        }
        for (bit = 0; bit < 8u; bit++) {
            uint8_t mask = (uint8_t)(1u << bit);
            unsigned usage;

            if (!(diff & mask)) {
                continue;
            }
            usage = byte_i * 8u + bit;
            if (backend->kbm_events != NULL
                && n < (sizeof evbuf / sizeof evbuf[0])) {
                evbuf[n].device = (uint8_t)APAD_KBM_DEV_KEYBOARD;
                evbuf[n].code   = (uint16_t)usage;
                evbuf[n].down   =
                    (uint8_t)((in->keys[byte_i] & mask) ? 1u : 0u);
                n++;
            }
        }
    }
    memcpy(st->kb_shadow, in->keys, sizeof st->kb_shadow);
    st->kb_last_applied = in->event_seq;

    if (backend->kbm_events != NULL && n > 0u) {
        (void)backend->kbm_events(slot, evbuf, n);
    }
}

/* ======================================================================== */
/* MOUSE (§6.16) — 5-button mask + ring depth 4, plus accumulator motion    */
/* ======================================================================== */

void apad_kbm_apply_mouse(const apad_backend *backend, int slot,
                          apad_kbm_state *st, const apad_mouse *in,
                          uint32_t now, unsigned *out_lost)
{
    apad_kbm_event_out evbuf[APAD_MOUSE_RING_DEPTH + APAD_MOUSEBTN_MAX];
    size_t   n = 0;
    unsigned b;
    apad_mouse_motion motion;
    int      first;

    if (out_lost != NULL) {
        *out_lost = 0u;
    }
    if (backend == NULL || st == NULL || in == NULL) {
        return;
    }
    st->mo_last_accept_ms = now;
    /* The first accepted MOUSE of a session -- or of a freshly created
     * pointer device -- is the SAME event §6.16 and §6.20 both key off, so
     * one flag drives both the button ring's "first accepted" rule and the
     * motion baseline below; they can never disagree about when "first"
     * was. */
    first = !st->mo_have_baseline;

    /* ---- buttons: §6.20's ring-replay-then-reconcile, same shape as
     * KEYBOARD's above, at 5 bits instead of 256. */
    if (!first) {
        int overflow = 0;
        int gap = apad_event_ring_new(in->event_seq, st->mo_last_applied,
                                      (int)APAD_MOUSE_RING_DEPTH, &overflow);
        int start = (int)APAD_MOUSE_RING_DEPTH - gap;
        int idx;

        /* See apad_kbm_apply_keyboard()'s comment on the identical
         * `overflow` handling just above -- same reasoning, smaller ring. */
        if (overflow && out_lost != NULL) {
            int true_gap = apad_seq_diff(in->event_seq, st->mo_last_applied);
            if (true_gap > (int)APAD_MOUSE_RING_DEPTH) {
                *out_lost = (unsigned)(true_gap - (int)APAD_MOUSE_RING_DEPTH);
            }
        }
        for (idx = start; idx < (int)APAD_MOUSE_RING_DEPTH; idx++) {
            const apad_mouse_event *e = &in->events[idx];
            uint16_t bit;

            if (e->button == 0u) {
                continue;
            }
            bit = APAD_MOUSEBTN_BIT(e->button);
            if (backend->kbm_events != NULL
                && n < (sizeof evbuf / sizeof evbuf[0])) {
                evbuf[n].device = (uint8_t)APAD_KBM_DEV_MOUSE;
                evbuf[n].code   = e->button;
                evbuf[n].down   =
                    (uint8_t)((e->flags & APAD_KBM_EVENT_DOWN) ? 1u : 0u);
                n++;
            }
            if (e->flags & APAD_KBM_EVENT_DOWN) {
                st->mo_shadow_buttons =
                    (uint16_t)(st->mo_shadow_buttons | bit);
            } else {
                st->mo_shadow_buttons =
                    (uint16_t)(st->mo_shadow_buttons & (uint16_t)~bit);
            }
        }
    }

    for (b = 1u; b <= APAD_MOUSEBTN_MAX; b++) {
        uint16_t bit      = APAD_MOUSEBTN_BIT(b);
        uint8_t  was_down = (uint8_t)((st->mo_shadow_buttons & bit) ? 1u : 0u);
        uint8_t  is_down  = (uint8_t)((in->buttons & bit) ? 1u : 0u);

        if (was_down == is_down) {
            continue;
        }
        if (backend->kbm_events != NULL
            && n < (sizeof evbuf / sizeof evbuf[0])) {
            evbuf[n].device = (uint8_t)APAD_KBM_DEV_MOUSE;
            evbuf[n].code   = (uint16_t)b;
            evbuf[n].down   = is_down;
            n++;
        }
    }
    st->mo_shadow_buttons = in->buttons;
    st->mo_last_applied   = in->event_seq;

    if (backend->kbm_events != NULL && n > 0u) {
        (void)backend->kbm_events(slot, evbuf, n);
    }

    /* ---- motion: §6.16's accumulator-diff rule. The first accepted MOUSE
     * MUST produce zero motion and only establish the baseline -- a stale
     * (discarded) packet never reaches this function at all, so there is
     * nothing here to guard against advancing the baseline on one. */
    if (first) {
        motion.dx = motion.dy = motion.wheel = motion.hwheel = 0;
    } else {
        motion.dx     = apad_seq_diff(in->dx_accum,     st->mo_last_dx);
        motion.dy     = apad_seq_diff(in->dy_accum,     st->mo_last_dy);
        motion.wheel  = apad_seq_diff(in->wheel_accum,  st->mo_last_wheel);
        motion.hwheel = apad_seq_diff(in->hwheel_accum, st->mo_last_hwheel);
    }
    st->mo_last_dx       = in->dx_accum;
    st->mo_last_dy       = in->dy_accum;
    st->mo_last_wheel    = in->wheel_accum;
    st->mo_last_hwheel   = in->hwheel_accum;
    st->mo_have_baseline = 1u;

    if (backend->mouse_motion != NULL
        && (motion.dx != 0 || motion.dy != 0
            || motion.wheel != 0 || motion.hwheel != 0)) {
        (void)backend->mouse_motion(slot, &motion);
    }
}

/* ======================================================================== */
/* MEDIA (§6.17) — 32-bit held mask (24 assigned) + ring depth 4            */
/* ======================================================================== */

void apad_kbm_apply_media(const apad_backend *backend, int slot,
                          apad_kbm_state *st, const apad_media *in,
                          uint32_t now, unsigned *out_lost)
{
    apad_kbm_event_out evbuf[APAD_MEDIA_RING_DEPTH + APAD_MEDIA_ASSIGNED_MAX];
    size_t   n = 0;
    unsigned c;

    if (out_lost != NULL) {
        *out_lost = 0u;
    }
    if (backend == NULL || st == NULL || in == NULL) {
        return;
    }
    st->me_last_accept_ms = now;

    if (!st->me_have_baseline) {
        st->me_have_baseline = 1u;
    } else {
        int overflow = 0;
        int gap = apad_event_ring_new(in->event_seq, st->me_last_applied,
                                      (int)APAD_MEDIA_RING_DEPTH, &overflow);
        int start = (int)APAD_MEDIA_RING_DEPTH - gap;
        int idx;

        /* See apad_kbm_apply_keyboard()'s comment on the identical
         * `overflow` handling -- same reasoning, MEDIA's own ring depth. */
        if (overflow && out_lost != NULL) {
            int true_gap = apad_seq_diff(in->event_seq, st->me_last_applied);
            if (true_gap > (int)APAD_MEDIA_RING_DEPTH) {
                *out_lost = (unsigned)(true_gap - (int)APAD_MEDIA_RING_DEPTH);
            }
        }
        for (idx = start; idx < (int)APAD_MEDIA_RING_DEPTH; idx++) {
            const apad_media_event *e = &in->events[idx];
            uint32_t bit;

            if (e->control == 0u) {
                continue;
            }
            bit = APAD_MEDIA_BIT(e->control);
            if (backend->kbm_events != NULL
                && n < (sizeof evbuf / sizeof evbuf[0])) {
                evbuf[n].device = (uint8_t)APAD_KBM_DEV_MEDIA;
                evbuf[n].code   = e->control;
                evbuf[n].down   =
                    (uint8_t)((e->flags & APAD_KBM_EVENT_DOWN) ? 1u : 0u);
                n++;
            }
            if (e->flags & APAD_KBM_EVENT_DOWN) {
                st->me_shadow_held |= bit;
            } else {
                st->me_shadow_held &= ~bit;
            }
        }
    }

    for (c = 1u; c <= APAD_MEDIA_ASSIGNED_MAX; c++) {
        uint32_t bit      = APAD_MEDIA_BIT(c);
        uint8_t  was_down = (uint8_t)((st->me_shadow_held & bit) ? 1u : 0u);
        uint8_t  is_down  = (uint8_t)((in->held & bit) ? 1u : 0u);

        if (was_down == is_down) {
            continue;
        }
        if (backend->kbm_events != NULL
            && n < (sizeof evbuf / sizeof evbuf[0])) {
            evbuf[n].device = (uint8_t)APAD_KBM_DEV_MEDIA;
            evbuf[n].code   = (uint16_t)c;
            evbuf[n].down   = is_down;
            n++;
        }
    }
    st->me_shadow_held   = in->held;
    st->me_last_applied  = in->event_seq;

    if (backend->kbm_events != NULL && n > 0u) {
        (void)backend->kbm_events(slot, evbuf, n);
    }
}

/* ======================================================================== */
/* Watchdog and teardown release (§6.20)                                    */
/* ======================================================================== */

static void release_keyboard(const apad_backend *backend, int slot,
                             apad_kbm_state *st)
{
    apad_kbm_event_out evbuf[APAD_KEY_BITMAP_BYTES * 8u];
    size_t   n = 0;
    unsigned byte_i;

    if (backend->kbm_events != NULL) {
        for (byte_i = 0; byte_i < APAD_KEY_BITMAP_BYTES; byte_i++) {
            uint8_t  bits = st->kb_shadow[byte_i];
            unsigned bit;

            if (bits == 0u) {
                continue;
            }
            for (bit = 0; bit < 8u; bit++) {
                if (!(bits & (1u << bit))) {
                    continue;
                }
                evbuf[n].device = (uint8_t)APAD_KBM_DEV_KEYBOARD;
                evbuf[n].code   = (uint16_t)(byte_i * 8u + bit);
                evbuf[n].down   = 0u;
                n++;
            }
        }
        if (n > 0u) {
            (void)backend->kbm_events(slot, evbuf, n);
        }
    }
    memset(st->kb_shadow, 0, sizeof st->kb_shadow);
}

static void release_mouse_buttons(const apad_backend *backend, int slot,
                                  apad_kbm_state *st)
{
    apad_kbm_event_out evbuf[APAD_MOUSEBTN_MAX];
    size_t   n = 0;
    unsigned b;

    if (backend->kbm_events != NULL) {
        for (b = 1u; b <= APAD_MOUSEBTN_MAX; b++) {
            uint16_t bit = APAD_MOUSEBTN_BIT(b);

            if (!(st->mo_shadow_buttons & bit)) {
                continue;
            }
            evbuf[n].device = (uint8_t)APAD_KBM_DEV_MOUSE;
            evbuf[n].code   = (uint16_t)b;
            evbuf[n].down   = 0u;
            n++;
        }
        if (n > 0u) {
            (void)backend->kbm_events(slot, evbuf, n);
        }
    }
    st->mo_shadow_buttons = 0u;
}

static void release_media(const apad_backend *backend, int slot,
                          apad_kbm_state *st)
{
    apad_kbm_event_out evbuf[APAD_MEDIA_ASSIGNED_MAX];
    size_t   n = 0;
    unsigned c;

    if (backend->kbm_events != NULL) {
        for (c = 1u; c <= APAD_MEDIA_ASSIGNED_MAX; c++) {
            uint32_t bit = APAD_MEDIA_BIT(c);

            if (!(st->me_shadow_held & bit)) {
                continue;
            }
            evbuf[n].device = (uint8_t)APAD_KBM_DEV_MEDIA;
            evbuf[n].code   = (uint16_t)c;
            evbuf[n].down   = 0u;
            n++;
        }
        if (n > 0u) {
            (void)backend->kbm_events(slot, evbuf, n);
        }
    }
    st->me_shadow_held = 0u;
}

void apad_kbm_watchdog(const apad_backend *backend, int slot,
                       apad_kbm_state *st, uint32_t now)
{
    if (backend == NULL || st == NULL) {
        return;
    }
    if (st->kb_created
        && apad_time_since(now, st->kb_last_accept_ms) >= APAD_KBM_WATCHDOG_MS) {
        release_keyboard(backend, slot, st);
    }
    /* Motion is exempt (§6.20: "the held-repeat obligation covers buttons
     * only") -- only the button mask can be "held" for MOUSE. */
    if (st->mo_created
        && apad_time_since(now, st->mo_last_accept_ms) >= APAD_KBM_WATCHDOG_MS) {
        release_mouse_buttons(backend, slot, st);
    }
    if (st->me_created
        && apad_time_since(now, st->me_last_accept_ms) >= APAD_KBM_WATCHDOG_MS) {
        release_media(backend, slot, st);
    }
}

void apad_kbm_release_all(const apad_backend *backend, int slot,
                          apad_kbm_state *st)
{
    if (backend == NULL || st == NULL) {
        return;
    }
    /* Release everything held BEFORE destroying the device it was injected
     * into (§6.20: "before it destroys or detaches whatever it injects
     * into"). */
    if (st->kb_created) {
        release_keyboard(backend, slot, st);
    }
    if (st->mo_created) {
        release_mouse_buttons(backend, slot, st);
    }
    if (st->me_created) {
        release_media(backend, slot, st);
    }
    if (backend->destroy_kbm != NULL) {
        if (st->kb_created) {
            backend->destroy_kbm(slot, APAD_KBM_DEV_KEYBOARD);
        }
        if (st->mo_created) {
            backend->destroy_kbm(slot, APAD_KBM_DEV_MOUSE);
        }
        if (st->me_created) {
            backend->destroy_kbm(slot, APAD_KBM_DEV_MEDIA);
        }
    }
    st->kb_created = 0u;
    st->mo_created = 0u;
    st->me_created = 0u;
}
