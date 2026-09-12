/* apad_kbm_touch.h — the touchscreen "builders" for §6.15-§6.17 KEYBOARD /
 * MOUSE / MEDIA on a single-touch resistive panel: turn one contact point
 * plus two shoulder buttons into a per-frame apad_keyboard / apad_mouse /
 * apad_media snapshot.
 *
 * Hoisted out of clients/3ds/source/screen_session.c (2026-09-09) so the NDS
 * client (256x192, written in parallel) can link the SAME tested state
 * machine instead of a second copy of the resistive-panel settle/anchor/tap/
 * drag-lock logic and the sticky-modifier latch — both found and tuned
 * against real 3DS hardware, not the kind of thing a second console should
 * re-derive from scratch. Same rationale as apad_client.c's own hoist: one
 * copy, one set of hardware lessons, protected by one caller
 * (tools/engine-client --kbm and, per-platform, evtest on the receiving PC).
 *
 * THE LAYOUT LIVES ONCE, in a 320x240 REFERENCE SPACE — the 3DS bottom
 * screen's own resolution, so every box below is the 3DS numbers verbatim —
 * and apad_kbm_touch_init() scales every box to whatever screen the caller
 * actually has: box.x = ref.x * screen_w / 320, box.y = ref.y * screen_h /
 * 240, integer math, no float. A 320x240 caller (the 3DS) gets back the
 * exact reference integers; a 256x192 caller (the NDS, exactly 0.8x on both
 * axes) gets a proportionally scaled layout without this file knowing NDS
 * exists.
 *
 * WHO OWNS WHAT. This module NEVER touches a socket, encodes a packet, or
 * decides the §6.19 gate — that is entirely clients/common/apad_client.c
 * (kbm_gate(), kbm_pump(), kbm_ring_push()). This module's only job is the
 * same one the 3DS's session screen used to do inline: turn "what is the
 * touch/L/R state right now" into "what does the wire snapshot look like",
 * once per builder per frame, for whichever of MOUSE/KEYS/MEDIA is on
 * screen. The caller (a platform's session screen) is responsible for:
 *
 *   - deciding which mode is active and passing `active` accordingly;
 *   - calling EVERY builder EVERY frame regardless of which mode is showing,
 *     with `active` 0 for the ones not on screen, so leaving a mode drives
 *     whatever it was holding through its own release path (a builder fed
 *     synthetic "nothing touched" input on the very next frame) rather than
 *     leaving something stuck — see apad_client_kbm_in's own header comment
 *     in apad_client.h for why a snapshot-per-frame caller is correct here;
 *   - drawing: this module owns hit-testing (apad_kbm_box_hit /
 *     apad_kbm_touch_key_cell) so a platform's hit test and its wire output
 *     cannot drift apart, but it draws nothing. A platform MAY keep its own
 *     float/local copies of these same box numbers purely for rendering
 *     calls that want a different type (citro2d's ui_box is float) — that is
 *     a cosmetic duplication, never a hit-testing one.
 *
 * NO FILE-SCOPE MUTABLE STATE. Two clients link this (3DS today, NDS next),
 * so every builder's state — the mouse contact machinery, the sticky
 * SHIFT/CTRL latch — lives in the apad_kbm_touch struct the caller owns, one
 * instance per live session. apad_kbm_touch_init() both computes the scaled
 * boxes AND zeroes every runtime field, so calling it once per new session
 * (matching the old screen_session.c's session_enter() reset) is correct and
 * cheap — it is pure integer arithmetic, no allocation.
 *
 * C99. No malloc, no float, no stdio — compiles under the 3DS (devkitARM),
 * Android (NDK/clang) and PSP toolchains alike; -Wall -Wextra -Werror
 * -pedantic clean.
 */
#ifndef ATTICPAD_COMMON_APAD_KBM_TOUCH_H
#define ATTICPAD_COMMON_APAD_KBM_TOUCH_H

#include <stdint.h>

#include "atticpad/atticpad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Integer, screen-space, half-open [x, x+w) x [y, y+h). */
typedef struct {
    int16_t x, y, w, h;
} apad_kbm_box;

int apad_kbm_box_hit(const apad_kbm_box *b, int px, int py);

/* One frame's raw touch + shoulder-button sample, already in the caller's
 * screen space (no scaling here — px/py are compared directly against the
 * already-scaled boxes in apad_kbm_touch). `l_held`/`r_held` are the RAW
 * hardware state (3DS: KEY_L/KEY_R via ctx->keys_held), independent of
 * whatever a platform's own wire-facing button state has done with them —
 * MOUSE mode reads them as click, KEYS mode as live Shift/Ctrl, and a
 * platform clearing them back out of its own pad-button state is that
 * platform's business, not this module's. */
typedef struct {
    int touching;
    int px, py;
    int l_held;
    int r_held;
} apad_kbm_touch_input;

/* ---- KEYS mode: the grid ------------------------------------------------ */

#define APAD_KBM_KEY_COLS 10
#define APAD_KBM_KEY_ROWS 5

enum {
    APAD_KBM_KC_NORMAL = 0,
    APAD_KBM_KC_SHIFT,
    APAD_KBM_KC_CTRL
};

/* One grid cell's identity, for a caller to DRAW from — label text, the HID
 * usage a NORMAL cell sends, and which of the two sticky-modifier cells (if
 * either) this one is. `usage` is unused (0) for SHIFT/CTRL cells. */
typedef struct {
    uint8_t     usage;
    const char *label;
    uint8_t     kind;   /* APAD_KBM_KC_* */
} apad_kbm_key_cell;

/* The fixed 10x5 layout, indexed row*APAD_KBM_KEY_COLS+col — same content on
 * every screen this module serves; it is not part of the scaled layout
 * because it names KEYS, not pixels. NULL for an out-of-range row/col. */
const apad_kbm_key_cell *apad_kbm_touch_key_def(int row, int col);

/* ---- the context struct -------------------------------------------------- */

typedef struct apad_kbm_touch {
    int screen_w, screen_h;

    /* ---- MOUSE mode (§6.16), boxes scaled from the 320x240 reference ---- */
    apad_kbm_box mouse_pad;      /* drag to move / tap to click              */
    apad_kbm_box mouse_wheel;    /* right-edge gutter, drag to scroll        */
    apad_kbm_box mouse_btn_l;    /* footer LEFT/MIDDLE/RIGHT, latch-on-start */
    apad_kbm_box mouse_btn_m;
    apad_kbm_box mouse_btn_r;

    /* ---- MEDIA mode (§6.17/§6.18 transport+volume, §6.15 nav cross) ---- */
    apad_kbm_box media_prev;
    apad_kbm_box media_stop;
    apad_kbm_box media_next;
    apad_kbm_box media_play;
    apad_kbm_box media_vol_d;
    apad_kbm_box media_mute;
    apad_kbm_box media_vol_u;
    apad_kbm_box media_up;
    apad_kbm_box media_left;
    apad_kbm_box media_ok;
    apad_kbm_box media_right;
    apad_kbm_box media_down;

    /* ---- KEYS mode (§6.15): vertical grid bounds, scaled; cols/rows are
     * the fixed APAD_KBM_KEY_COLS/ROWS above, not part of the scaled layout
     * since they name a count, not a pixel. */
    int16_t key_grid_top;
    int16_t key_grid_bot;

    /* ---- MOUSE builder's own state (was screen_session.c's s_mouse_*) --- *
     * See apad_kbm_touch_build_mouse()'s own comment in apad_kbm_touch.c for
     * what each field does and the resistive-panel hardware finding that
     * shaped it. Session-lifetime free-running accumulators plus
     * per-CONTACT bookkeeping, all reset by apad_kbm_touch_init(). */
    uint16_t mouse_dx_accum;
    uint16_t mouse_dy_accum;
    uint16_t mouse_wheel_accum;
    uint16_t mouse_hwheel_accum;
    int      mouse_contact_frames;
    int      mouse_origin_ready;
    uint16_t mouse_btn_latch;
    int      mouse_pad_origin;
    int      mouse_was_in_pad;
    int      mouse_dragging;
    int32_t  mouse_last_px, mouse_last_py;
    int32_t  mouse_moved_px;
    int      mouse_pending_valid;
    int32_t  mouse_pending_dx, mouse_pending_dy;
    int      mouse_tapdrag_active;
    int      mouse_tapdrag_window_frames;
    int      wheel_dragging;
    int32_t  wheel_last_py;
    int32_t  wheel_carry;

    /* ---- KEYS builder's own state (was screen_session.c's s_key_*) ----- */
    int keys_active_cell;      /* -1 = nothing currently touched            */
    int key_shift_armed;       /* sticky latch, toggled by a tap            */
    int key_ctrl_armed;
    int key_consume_shift;     /* THIS held key's shift came from the latch */
    int key_consume_ctrl;
} apad_kbm_touch;

/* Computes every scaled box from the 320x240 reference layout and zeroes
 * every runtime field above — both the one-time layout setup AND the
 * per-new-session reset a caller's old code used to hand-roll (screen_
 * session.c's session_enter()). screen_w/h must be > 0. */
void apad_kbm_touch_init(apad_kbm_touch *t, int screen_w, int screen_h);

/* -1 if (px, py) is not inside the KEYS grid, else row*APAD_KBM_KEY_COLS+col
 * — the same arithmetic hit test screen_session.c used to do inline, now
 * against the SCALED grid bounds (t->key_grid_top/bot) instead of a
 * 320x240-only constant. */
int apad_kbm_touch_key_cell(const apad_kbm_touch *t, int px, int py);

/* One frame's §6.16 MOUSE snapshot. `m` is NOT memset by this function —
 * the caller is expected to have memset the whole apad_client_kbm_in once,
 * per apad_client.h's own contract (events[] must read as "no event" past
 * whatever this call fills). `active` 0 feeds the state machine synthetic
 * "nothing touched" input, which safely runs the ordinary release path. */
void apad_kbm_touch_build_mouse(apad_kbm_touch *t,
                                const apad_kbm_touch_input *in,
                                apad_mouse *m, int active);

/* One frame's §6.15 KEYBOARD snapshot from the grid alone (physical L/R live
 * Shift/Ctrl and the sticky latch). `kb` is OR'd into, not overwritten —
 * MEDIA's nav cross (below) targets the same struct and must run after this
 * without clobbering it; call KEYS first in a frame that also calls MEDIA. */
void apad_kbm_touch_build_keyboard(apad_kbm_touch *t,
                                   const apad_kbm_touch_input *in,
                                   apad_keyboard *kb, int active);

/* One frame's §6.17 MEDIA snapshot, plus (when `have_kb`) the nav cross's
 * arrow/Enter usages OR'd into `kb` — the vocabulary a media-centre remote's
 * D-pad drives, gated on keyboard capability independently of whether MEDIA
 * itself is offered. `kb` should be the same struct passed to
 * apad_kbm_touch_build_keyboard() in the same frame. */
void apad_kbm_touch_build_media(apad_kbm_touch *t,
                                const apad_kbm_touch_input *in,
                                apad_media *me, apad_keyboard *kb,
                                int active, int have_kb);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_COMMON_APAD_KBM_TOUCH_H */
