/* apad_kbm_touch.c — see apad_kbm_touch.h.
 *
 * Every box below is given in a 320x240 REFERENCE space (the 3DS bottom
 * screen's own resolution) and scaled to the caller's actual screen at
 * apad_kbm_touch_init() time. On the 3DS itself (320x240) the scale is 1:1,
 * so these are the exact numbers that shipped in clients/3ds/source/
 * screen_session.c before this file existed — this pass is a relocation,
 * not a redesign, and the 3DS's own drawing code (which may keep its own
 * float copies of some of these for citro2d calls) must therefore still
 * match pixel-for-pixel.
 */
#include <string.h>

#include "apad_kbm_touch.h"

#define REF_W 320
#define REF_H 240

int apad_kbm_box_hit(const apad_kbm_box *b, int px, int py)
{
    return px >= (int)b->x && px < (int)b->x + (int)b->w
        && py >= (int)b->y && py < (int)b->y + (int)b->h;
}

static int16_t scale_x(int16_t ref, int screen_w)
{
    return (int16_t)(((int32_t)ref * (int32_t)screen_w) / REF_W);
}

static int16_t scale_y(int16_t ref, int screen_h)
{
    return (int16_t)(((int32_t)ref * (int32_t)screen_h) / REF_H);
}

static void scale_box(apad_kbm_box *out, int16_t rx, int16_t ry, int16_t rw,
                      int16_t rh, int screen_w, int screen_h)
{
    out->x = scale_x(rx, screen_w);
    out->y = scale_y(ry, screen_h);
    out->w = scale_x(rw, screen_w);
    out->h = scale_y(rh, screen_h);
}

void apad_kbm_touch_init(apad_kbm_touch *t, int screen_w, int screen_h)
{
    memset(t, 0, sizeof *t);
    t->screen_w = screen_w;
    t->screen_h = screen_h;

    /* MOUSE mode (§6.16). Bottom screen y 24..190 is a relative touchpad
     * (drag -> dx/dy, short-tap-with-little-movement -> a left click); a
     * 50px gutter on the right (x 270..320) is the wheel; y 190..216 is a
     * LEFT/MIDDLE/RIGHT footer for touch users. */
    scale_box(&t->mouse_pad,    0,  24, 270, 166, screen_w, screen_h);
    scale_box(&t->mouse_wheel, 270,  24,  50, 166, screen_w, screen_h);
    scale_box(&t->mouse_btn_l,   0, 190, 106,  26, screen_w, screen_h);
    scale_box(&t->mouse_btn_m, 106, 190, 107,  26, screen_w, screen_h);
    scale_box(&t->mouse_btn_r, 213, 190, 107,  26, screen_w, screen_h);

    /* MEDIA mode (§6.17/§6.18 transport+volume, §6.15 nav cross). */
    scale_box(&t->media_prev,   4,  28,  96,  36, screen_w, screen_h);
    scale_box(&t->media_stop, 112,  28,  96,  36, screen_w, screen_h);
    scale_box(&t->media_next, 220,  28,  96,  36, screen_w, screen_h);
    scale_box(&t->media_play,   4,  68, 312,  44, screen_w, screen_h);
    scale_box(&t->media_vol_d,  4, 116,  96,  36, screen_w, screen_h);
    scale_box(&t->media_mute, 112, 116,  96,  36, screen_w, screen_h);
    scale_box(&t->media_vol_u, 220, 116,  96,  36, screen_w, screen_h);
    scale_box(&t->media_up,   132, 156,  56,  20, screen_w, screen_h);
    scale_box(&t->media_left,  76, 176,  56,  24, screen_w, screen_h);
    scale_box(&t->media_ok,   132, 176,  56,  24, screen_w, screen_h);
    scale_box(&t->media_right,188, 176,  56,  24, screen_w, screen_h);
    scale_box(&t->media_down, 132, 200,  56,  20, screen_w, screen_h);

    /* KEYS mode (§6.15). A APAD_KBM_KEY_COLS x APAD_KBM_KEY_ROWS grid
     * between the mode strip (y 0..24) and the footer (y 216..240), hit-
     * tested arithmetically rather than as 50 individual boxes. */
    t->key_grid_top = scale_y(24, screen_h);
    t->key_grid_bot = scale_y(216, screen_h);

    /* Every builder-state field above is already 0 from the memset, which is
     * exactly the "new session, every mode starts clean" reset
     * screen_session.c's session_enter() used to hand-roll — mouse_contact_
     * frames == 0 would misread as "the raw touch-down frame", so it alone
     * needs its -1 sentinel restored explicitly (see the MOUSE section
     * below: -1 means "not touching"). */
    t->mouse_contact_frames = -1;
    t->keys_active_cell = -1;
}

/* ------------------------------------------------------------------------ */
/* MOUSE mode                                                               */
/* ------------------------------------------------------------------------ *
 * SENSITIVITY IS THIS FILE'S PROBLEM, DELIBERATELY, unlike every other axis
 * a client fills. docs/PROTOCOL.md S5.3's "never apply a deadzone
 * client-side" is about STICKS, where the server profile owns curve and
 * deadzone; §6.15-§6.19's KBM profiles deliberately gained no equivalent
 * target for mouse sensitivity (there is no server-side "pointer speed"
 * concept at all in this protocol), so if this file scaled touch pixels 1:1
 * into wire counts a full-screen drag would move a desktop pointer about
 * 300px and nothing server-side could ever fix that. MOUSE_SENS_MUL is
 * therefore a client-owned constant, deliberately — flag it before "fixing"
 * it by hunting for a profile field that does not exist.
 *
 * RESISTIVE-PANEL EDGE NOISE (found on 3DS hardware, 2026-08-25). First real-
 * hardware feedback on this mode: tap-to-click was too insensitive to
 * reliably register, and click-and-drag "didn't really work". The 3DS
 * bottom screen is a RESISTIVE digitizer, not capacitive — it reports an
 * actual electrical contact reading, and the first sample or two after a
 * finger lands (and the last one or two before it lifts) are the least
 * reliable readings physically possible from that kind of panel: pressure
 * and contact area are still settling on the way in and easing off on the
 * way out. A tap whose finger never moved at all could still fail an
 * over-tight tap-distance slop purely from that transient — 6px was
 * stylus-grade precision, not a fingertip's, which is why MOUSE_TAP_MAX_PX
 * below is not that.
 *
 * COULD NOT BE CONFIRMED ON HARDWARE WHEN THIS WAS WRITTEN: emulator touch
 * is a mouse and will not reproduce resistive noise, which is the whole
 * point of this bug. This is applying the well-documented
 * electrical behaviour of resistive digitizers (the reason DS/3DS homebrew
 * touch code has discarded lead-in/lead-out samples for well over a decade)
 * rather than a captured trace. If a future hardware pass instruments raw
 * px/py across a tap and this ISN'T what is happening, this whole block —
 * not just the pixel constants — is what to revisit.
 *
 * The fix is three independent pieces, applied to every continuous touch:
 *   1. MOUSE_TOUCH_SETTLE_FRAMES — the first N samples of a NEW contact are
 *      read (`in->touching` itself is never wrong) but never trusted to
 *      anchor a drag or classify which region the touch began in.
 *      Classification and the drag anchor both wait for the first STABLE
 *      sample instead of the down-edge one.
 *   2. The drag accumulator holds the most recent frame-to-frame delta back
 *      by exactly one frame (mouse_pending_*) before committing it to the
 *      wire accumulators / the tap-distance total. On release, whatever
 *      delta is still pending is simply never committed — that dropped
 *      delta is always the one ending at the LAST sample before liftoff,
 *      the other resistive transient. One frame (~16.7ms) of latency on
 *      cursor motion buys this; imperceptible.
 *   3. MOUSE_TAP_MAX_PX went from 6 (stylus-grade) to a number honest for a
 *      fingertip, and gained a time bound it never had
 *      (MOUSE_TAP_MAX_FRAMES) so a slow deliberate hold that happens to stay
 *      under the pixel budget cannot misfire as a click.
 *
 * CLICK-AND-DRAG. Holding a live modifier (a platform's own "physical
 * shoulder button as click" plumbing, fed through `in->l_held`/`in->r_held`)
 * while dragging already works — it is read every frame independent of
 * touch position — but BOTH touch-only ways to click-and-drag needed fixing:
 *   - The footer LEFT/MIDDLE/RIGHT cells used to decide "is this button
 *     down" from the CURRENT touch position, every frame. Press LEFT and
 *     slide the SAME finger up into the pad to drag — the only way to
 *     click-and-drag with touch alone on a SINGLE-TOUCH resistive panel,
 *     since there is no second finger to hold the footer down while the
 *     first one drags — released the LEFT bit the instant the sample left
 *     the footer's rectangle. Fixed by latching which cell (if any) a
 *     continuous touch STARTED in (mouse_btn_latch, decided at the same
 *     first-stable-sample point as #1 above) and holding that button for
 *     the touch's entire lifetime regardless of where it travels afterward,
 *     while pad movement keeps accumulating underneath from wherever the
 *     finger currently is (mouse_was_in_pad / in_pad below are purely
 *     positional, decoupled from the latch).
 *   - Tap-then-hold-drag (tap, then a second touch within
 *     MOUSE_TAPDRAG_WINDOW_FRAMES that moves) is the other gesture every
 *     laptop trackpad has: a completed pad tap arms a short window; a new
 *     pad-origin touch inside that window is a drag-lock
 *     (mouse_tapdrag_active) — LEFT held for that touch's whole lifetime,
 *     released on liftoff. No second click event is needed for it: the
 *     button state is a literal held bit for that whole touch, so the
 *     engine's own snapshot diff synthesises the up transition.
 *
 * Tap/release evaluation happens exactly once, on the true
 * `!in->touching` edge — not the instant a drag sample leaves the pad box
 * for any reason (sliding into the wheel gutter or a footer cell mid-touch),
 * which used to fire a spurious click on a drag that merely wandered out of
 * the pad rectangle.
 */
#define MOUSE_SENS_MUL              3   /* wire counts per screen pixel of
                                          * drag */
#define MOUSE_TAP_MAX_PX            16  /* a fingertip on resistive glass is
                                          * not stylus-precise even holding
                                          * still — see the block comment
                                          * above */
#define MOUSE_TAP_MAX_FRAMES        24  /* ~400ms @60Hz. Bounds the OTHER
                                          * axis of "was this a tap": a slow
                                          * drift that never quite crosses
                                          * MOUSE_TAP_MAX_PX still isn't one */
#define MOUSE_WHEEL_PX_PER_DETENT   18  /* drag pixels per wheel detent      */
#define MOUSE_TOUCH_SETTLE_FRAMES    2  /* samples discarded at the START of
                                          * every contact before it is
                                          * trusted for classification or a
                                          * drag anchor; see block comment */
#define MOUSE_TAPDRAG_WINDOW_FRAMES 24  /* ~400ms. How long after a completed
                                          * tap a NEW touch counts as the
                                          * second half of a tap-then-hold
                                          * drag rather than an unrelated
                                          * fresh tap */

static int16_t mouse_clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void apad_kbm_touch_build_mouse(apad_kbm_touch *t,
                                const apad_kbm_touch_input *in,
                                apad_mouse *m, int active)
{
    int touching = active && in->touching;
    int px = in->px, py = in->py;
    int in_wheel = touching && apad_kbm_box_hit(&t->mouse_wheel, px, py);
    int in_pad   = touching && !in_wheel
                 && apad_kbm_box_hit(&t->mouse_pad, px, py);
    uint16_t buttons = 0u;

    if (t->mouse_tapdrag_window_frames > 0) {
        t->mouse_tapdrag_window_frames--;
    }

    if (!touching) {
        if (t->mouse_contact_frames >= 0) {
            /* The true release. A completed PAD-origin contact (see
             * MOUSE_TOUCH_SETTLE_FRAMES / mouse_pad_origin above) that
             * stayed short and near-still is a tap: emit the click and arm
             * the tap-then-hold window. Everything else — a footer cell, the
             * drag-lock's second touch, a real drag — needs no explicit
             * event here: its button bit has already been riding
             * m->buttons every touching frame below, so the engine's own
             * diff already emits the matching down/up pair. Whatever delta
             * was still PENDING (never committed below) is simply dropped
             * here, never added — that IS "ignore the final sample before
             * release" (block comment, point 2). */
            int was_tap = t->mouse_pad_origin
                        && t->mouse_contact_frames <= MOUSE_TAP_MAX_FRAMES
                        && (!t->mouse_dragging
                            || t->mouse_moved_px < MOUSE_TAP_MAX_PX);

            if (was_tap && !t->mouse_tapdrag_active) {
                m->events[0].button = (uint8_t)APAD_MOUSEBTN_LEFT;
                m->events[0].flags  = (uint8_t)APAD_KBM_EVENT_DOWN;
                m->events[1].button = (uint8_t)APAD_MOUSEBTN_LEFT;
                m->events[1].flags  = 0u;
                t->mouse_tapdrag_window_frames = MOUSE_TAPDRAG_WINDOW_FRAMES;
            }
        }
        t->mouse_contact_frames = -1;
        t->mouse_origin_ready   = 0;
        t->mouse_btn_latch      = 0u;
        t->mouse_pad_origin     = 0;
        t->mouse_was_in_pad     = 0;
        t->mouse_dragging       = 0;
        t->mouse_moved_px       = 0;
        t->mouse_pending_valid  = 0;
        t->mouse_tapdrag_active = 0;
    } else {
        if (t->mouse_contact_frames < 0) {
            /* True touch-down: a NEW contact starts here, but is not
             * trusted for anything yet — see MOUSE_TOUCH_SETTLE_FRAMES. */
            t->mouse_contact_frames = 0;
            t->mouse_origin_ready   = 0;
            t->mouse_btn_latch      = 0u;
            t->mouse_pad_origin     = 0;
            t->mouse_was_in_pad     = 0;
            t->mouse_dragging       = 0;
            t->mouse_moved_px       = 0;
            t->mouse_pending_valid  = 0;
        } else {
            t->mouse_contact_frames++;
        }

        if (!t->mouse_origin_ready
            && t->mouse_contact_frames >= MOUSE_TOUCH_SETTLE_FRAMES) {
            /* The first STABLE sample: classify which footer cell (if any)
             * this contact began over, and whether it began over the pad at
             * all, from THIS sample — not the noisy down-edge one. */
            t->mouse_origin_ready = 1;
            if (apad_kbm_box_hit(&t->mouse_btn_l, px, py)) {
                t->mouse_btn_latch = APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT);
            } else if (apad_kbm_box_hit(&t->mouse_btn_m, px, py)) {
                t->mouse_btn_latch = APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_MIDDLE);
            } else if (apad_kbm_box_hit(&t->mouse_btn_r, px, py)) {
                t->mouse_btn_latch = APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_RIGHT);
            } else {
                t->mouse_btn_latch = 0u;
            }
            t->mouse_pad_origin = (t->mouse_btn_latch == 0u)
                                && !apad_kbm_box_hit(&t->mouse_wheel, px, py)
                                && apad_kbm_box_hit(&t->mouse_pad, px, py);

            if (t->mouse_pad_origin && t->mouse_tapdrag_window_frames > 0) {
                /* A tap released recently enough — this new touch is the
                 * drag-lock half of a tap-then-hold, LEFT held for its
                 * whole lifetime regardless of how much it actually moves. */
                t->mouse_tapdrag_active = 1;
            }
            t->mouse_tapdrag_window_frames = 0;
        }

        /* Movement/drag tracking is POSITIONAL (current in_pad), independent
         * of which cell the touch STARTED in — this is what lets "press the
         * footer LEFT cell, slide the same finger up into the pad" move the
         * pointer while mouse_btn_latch keeps LEFT held throughout it, the
         * click-and-drag fix (block comment above). Leaving the pad (for the
         * wheel, a footer cell, or anywhere else) just PAUSES accumulation —
         * mouse_was_in_pad drops so a later re-entry re-anchors instead of
         * measuring one giant delta across whatever was in between. */
        if (in_pad && t->mouse_contact_frames >= MOUSE_TOUCH_SETTLE_FRAMES) {
            if (!t->mouse_was_in_pad) {
                t->mouse_was_in_pad = 1;
                t->mouse_dragging   = 1;
                t->mouse_last_px = px;
                t->mouse_last_py = py;
                t->mouse_pending_valid = 0;
            } else {
                int32_t dx = px - t->mouse_last_px;
                int32_t dy = py - t->mouse_last_py;

                if (t->mouse_pending_valid) {
                    /* Commit the PREVIOUS frame's delta now that contact is
                     * confirmed to have survived past it — one frame
                     * (~16.7ms) of latency buys the release-edge sample
                     * never being trusted (block comment, point 2). */
                    t->mouse_dx_accum = (uint16_t)(t->mouse_dx_accum
                        + (uint16_t)mouse_clamp16(t->mouse_pending_dx * MOUSE_SENS_MUL));
                    t->mouse_dy_accum = (uint16_t)(t->mouse_dy_accum
                        + (uint16_t)mouse_clamp16(t->mouse_pending_dy * MOUSE_SENS_MUL));
                    t->mouse_moved_px +=
                        (t->mouse_pending_dx < 0 ? -t->mouse_pending_dx : t->mouse_pending_dx)
                      + (t->mouse_pending_dy < 0 ? -t->mouse_pending_dy : t->mouse_pending_dy);
                }
                t->mouse_pending_dx = dx;
                t->mouse_pending_dy = dy;
                t->mouse_pending_valid = 1;
                t->mouse_last_px = px;
                t->mouse_last_py = py;
            }
        } else {
            t->mouse_was_in_pad = 0;
        }
    }

    /* Live buttons: a state snapshot, correct to just recompute every frame
     * and let the engine's own diff synthesise the transitions. `l_held`/
     * `r_held` are the caller's RAW hardware sample (the 3DS: KEY_L/KEY_R),
     * not whatever wire-facing pad-button state a platform derives from
     * them — so this is independent of a platform's own "clear it back out"
     * step for its own pad wire. The footer latch and the drag-lock are
     * likewise held bits, not events, for the same reason. */
    if (active) {
        if ((t->mouse_btn_latch & APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT)) != 0u
            || in->l_held
            || t->mouse_tapdrag_active) {
            buttons |= APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT);
        }
        if ((t->mouse_btn_latch & APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_RIGHT)) != 0u
            || in->r_held) {
            buttons |= APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_RIGHT);
        }
        if ((t->mouse_btn_latch & APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_MIDDLE)) != 0u) {
            buttons |= APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_MIDDLE);
        }
    }
    m->buttons = buttons;

    /* Wheel: purely positional (touching + currently over the gutter) — a
     * "hold and drag" gesture makes no sense for a detent-based scroll the
     * way it does for a click. */
    if (in_wheel) {
        if (!t->wheel_dragging) {
            t->wheel_dragging = 1;
            t->wheel_last_py = py;
        } else {
            int32_t dy = py - t->wheel_last_py;
            int32_t detents;

            /* Finger UP (dy<0, py decreasing) -> "away from the user", the
             * S6.16 positive direction — so the carry takes -dy. */
            t->wheel_carry -= dy;
            detents = t->wheel_carry / MOUSE_WHEEL_PX_PER_DETENT;
            t->wheel_carry -= detents * MOUSE_WHEEL_PX_PER_DETENT;
            t->mouse_wheel_accum = (uint16_t)(t->mouse_wheel_accum
                                              + (uint16_t)mouse_clamp16(detents));
            t->wheel_last_py = py;
        }
    } else {
        t->wheel_dragging = 0;
    }

    m->dx_accum     = t->mouse_dx_accum;
    m->dy_accum     = t->mouse_dy_accum;
    m->wheel_accum  = t->mouse_wheel_accum;
    m->hwheel_accum = t->mouse_hwheel_accum;
}

/* ------------------------------------------------------------------------ */
/* KEYS mode                                                                */
/* ------------------------------------------------------------------------ *
 * §6.15. A 10x5 grid, hit-tested arithmetically (col = px * COLS / width,
 * row = (py - top) * ROWS / height) rather than 50 individual boxes.
 *
 * STICKY MODIFIERS. The touchscreen is resistive and reports one contact, so
 * there is no chording two fingers the way a real keyboard's Shift+letter
 * works. Tapping the on-screen SHIFT/CTRL cell toggles a LATCH
 * (key_shift_armed/key_ctrl_armed) that ORs the modifier's HID usage into
 * keys[] starting the moment a NORMAL key is next touched, and disarms
 * itself the instant THAT key is released — "armed for the next key",
 * exactly one. A platform's own live shoulder-button modifiers (the 3DS:
 * physical L/R, fed through `in->l_held`/`in->r_held`) are the other, LIVE
 * half: held exactly as long as the button is, on top of whatever the latch
 * is doing, so a real chord (Shift+letter, Ctrl+C) is one finger on a letter
 * plus a shoulder button, not two taps.
 *
 * THE STICKY LATCH MUST NOT REACH THE WIRE ON ITS OWN — "armed for the next
 * key" means invisible until that key is actually touched, not a Shift held
 * down the instant the latch is armed (found live on Azahar: arming SHIFT
 * used to send a lone LEFTSHIFT immediately, before any letter was ever
 * touched, from an earlier version of this code that OR'd the live and
 * sticky sources unconditionally). Gating the sticky half inside the
 * concurrently-touched NORMAL-key branch is what makes shift/ctrl and the
 * letter appear on the wire together and disappear together, one
 * contiguous chord.
 */
static const apad_kbm_key_cell kKeyGrid[APAD_KBM_KEY_ROWS][APAD_KBM_KEY_COLS] = {
    { { APAD_HID_KEY_1, "1", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_2, "2", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_3, "3", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_4, "4", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_5, "5", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_6, "6", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_7, "7", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_8, "8", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_9, "9", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_0, "0", APAD_KBM_KC_NORMAL } },
    { { APAD_HID_KEY_Q, "Q", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_W, "W", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_E, "E", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_R, "R", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_T, "T", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_Y, "Y", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_U, "U", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_I, "I", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_O, "O", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_P, "P", APAD_KBM_KC_NORMAL } },
    { { APAD_HID_KEY_A, "A", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_S, "S", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_D, "D", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_F, "F", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_G, "G", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_H, "H", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_J, "J", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_K, "K", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_L, "L", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_BACKSPACE, "BKSP", APAD_KBM_KC_NORMAL } },
    { { 0, "SHIFT", APAD_KBM_KC_SHIFT },           { APAD_HID_KEY_Z, "Z", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_X, "X", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_C, "C", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_V, "V", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_B, "B", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_N, "N", APAD_KBM_KC_NORMAL }, { APAD_HID_KEY_M, "M", APAD_KBM_KC_NORMAL },
      { 0, "CTRL", APAD_KBM_KC_CTRL },
      { APAD_HID_KEY_ENTER, "ENTER", APAD_KBM_KC_NORMAL } },
    { { APAD_HID_KEY_ESCAPE, "ESC", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_TAB, "TAB", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_SPACE, "SPACE", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_SPACE, "", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_SPACE, "", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_SPACE, "", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_LEFT, "<", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_DOWN, "v", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_UP, "^", APAD_KBM_KC_NORMAL },
      { APAD_HID_KEY_RIGHT, ">", APAD_KBM_KC_NORMAL } }
};

const apad_kbm_key_cell *apad_kbm_touch_key_def(int row, int col)
{
    if (row < 0 || row >= APAD_KBM_KEY_ROWS || col < 0 || col >= APAD_KBM_KEY_COLS) {
        return NULL;
    }
    return &kKeyGrid[row][col];
}

int apad_kbm_touch_key_cell(const apad_kbm_touch *t, int px, int py)
{
    int col, row;
    int top = (int)t->key_grid_top, bot = (int)t->key_grid_bot;

    if (bot <= top || t->screen_w <= 0) {
        return -1;
    }
    if (py < top || py >= bot || px < 0 || px >= t->screen_w) {
        return -1;
    }
    col = (px * APAD_KBM_KEY_COLS) / t->screen_w;
    row = ((py - top) * APAD_KBM_KEY_ROWS) / (bot - top);
    if (col >= APAD_KBM_KEY_COLS) col = APAD_KBM_KEY_COLS - 1;
    if (row >= APAD_KBM_KEY_ROWS) row = APAD_KBM_KEY_ROWS - 1;
    return row * APAD_KBM_KEY_COLS + col;
}

void apad_kbm_touch_build_keyboard(apad_kbm_touch *t,
                                   const apad_kbm_touch_input *in,
                                   apad_keyboard *kb, int active)
{
    int cell = (active && in->touching)
             ? apad_kbm_touch_key_cell(t, in->px, in->py) : -1;
    int is_new_press = (cell >= 0 && cell != t->keys_active_cell);
    int is_release    = (cell != t->keys_active_cell) && t->keys_active_cell >= 0;
    const apad_kbm_key_cell *def = NULL;
    int shift_on, ctrl_on;

    if (is_release) {
        /* The PREVIOUSLY held cell just let go: consume any sticky modifier
         * THAT press spent — "armed for the next key", one key only. */
        if (t->key_consume_shift) { t->key_shift_armed = 0; t->key_consume_shift = 0; }
        if (t->key_consume_ctrl)  { t->key_ctrl_armed  = 0; t->key_consume_ctrl  = 0; }
    }
    if (cell >= 0) {
        def = &kKeyGrid[cell / APAD_KBM_KEY_COLS][cell % APAD_KBM_KEY_COLS];
    }
    if (is_new_press && def != NULL) {
        if (def->kind == APAD_KBM_KC_SHIFT) {
            t->key_shift_armed = !t->key_shift_armed;
        } else if (def->kind == APAD_KBM_KC_CTRL) {
            t->key_ctrl_armed = !t->key_ctrl_armed;
        } else {
            /* A normal key, freshly pressed: snapshot which modifiers this
             * particular press is spending, so releasing IT (not some later
             * key struck while this one is still down) is what disarms them. */
            t->key_consume_shift = t->key_shift_armed;
            t->key_consume_ctrl  = t->key_ctrl_armed;
        }
    }
    t->keys_active_cell = cell;

    if (!active) {
        return;   /* kb->keys[] is already all-zero (caller memset it) */
    }

    /* Live modifiers: read from the caller's RAW sample, so this is
     * independent of whatever a platform does with its own pad-button wire
     * state afterwards. Unlike the sticky latch below, this goes on the wire
     * UNCONDITIONALLY while KEYS mode is active — "live ... while held"
     * means held is held, with or without a letter also touched, the same
     * as a real keyboard's Shift key on its own contributes nothing but is
     * still physically down. */
    if (in->l_held) {
        kb->keys[APAD_KEY_BYTE(APAD_HID_KEY_LEFTSHIFT)]
            |= APAD_KEY_MASK(APAD_HID_KEY_LEFTSHIFT);
    }
    if (in->r_held) {
        kb->keys[APAD_KEY_BYTE(APAD_HID_KEY_LEFTCTRL)]
            |= APAD_KEY_MASK(APAD_HID_KEY_LEFTCTRL);
    }

    if (def != NULL && def->kind == APAD_KBM_KC_NORMAL) {
        shift_on = t->key_shift_armed || in->l_held;
        ctrl_on  = t->key_ctrl_armed  || in->r_held;
        if (shift_on) {
            kb->keys[APAD_KEY_BYTE(APAD_HID_KEY_LEFTSHIFT)]
                |= APAD_KEY_MASK(APAD_HID_KEY_LEFTSHIFT);
        }
        if (ctrl_on) {
            kb->keys[APAD_KEY_BYTE(APAD_HID_KEY_LEFTCTRL)]
                |= APAD_KEY_MASK(APAD_HID_KEY_LEFTCTRL);
        }
        kb->keys[APAD_KEY_BYTE(def->usage)] |= APAD_KEY_MASK(def->usage);
    }
}

/* ------------------------------------------------------------------------ */
/* MEDIA mode                                                               */
/* ------------------------------------------------------------------------ *
 * §6.17/§6.18 for transport and volume; the D-pad+OK navigation cross is
 * §6.15 KEYBOARD arrow keys/Enter instead (the vocabulary a real
 * media-centre remote's nav cross drives), gated on FEATURE_KEYBOARD
 * (`have_kb`) independently of whatever gated MEDIA mode itself.
 *
 * §6.17's own header comment on `held` + a ring: "held lets VOLUME_UP be
 * held for a ramp ... the ring lets a single NEXT_TRACK tap ... survive a
 * dropped packet." Both of those are already the engine's job once this
 * file hands it a live `held` snapshot every frame — press sets the bit, the
 * engine's diff emits the DOWN transition (and repeats it at the §6.20
 * floor for the ramp); release clears it, the diff emits UP. No events[]
 * needed here at all.
 */
void apad_kbm_touch_build_media(apad_kbm_touch *t,
                                const apad_kbm_touch_input *in,
                                apad_media *me, apad_keyboard *kb,
                                int active, int have_kb)
{
    int touching = active && in->touching;
    int px = in->px, py = in->py;

    me->held = 0u;
    if (!touching) {
        return;
    }
    if (apad_kbm_box_hit(&t->media_prev, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_PREV_TRACK);
    } else if (apad_kbm_box_hit(&t->media_stop, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_STOP);
    } else if (apad_kbm_box_hit(&t->media_next, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_NEXT_TRACK);
    } else if (apad_kbm_box_hit(&t->media_play, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_PLAY_PAUSE);
    } else if (apad_kbm_box_hit(&t->media_vol_d, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_VOLUME_DOWN);
    } else if (apad_kbm_box_hit(&t->media_mute, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_MUTE);
    } else if (apad_kbm_box_hit(&t->media_vol_u, px, py)) {
        me->held |= APAD_MEDIA_BIT(APAD_MEDIA_VOLUME_UP);
    } else if (have_kb) {
        uint8_t usage = 0u;

        if (apad_kbm_box_hit(&t->media_up, px, py))          usage = APAD_HID_KEY_UP;
        else if (apad_kbm_box_hit(&t->media_down, px, py))   usage = APAD_HID_KEY_DOWN;
        else if (apad_kbm_box_hit(&t->media_left, px, py))   usage = APAD_HID_KEY_LEFT;
        else if (apad_kbm_box_hit(&t->media_right, px, py))  usage = APAD_HID_KEY_RIGHT;
        else if (apad_kbm_box_hit(&t->media_ok, px, py))     usage = APAD_HID_KEY_ENTER;
        if (usage != 0u) {
            kb->keys[APAD_KEY_BYTE(usage)] |= APAD_KEY_MASK(usage);
        }
    }
}
