/* server/src/mapping.c — input state -> virtual pad state (docs/DESIGN.md §6.2, D9).
 *
 * THE MAPPING ENGINE LIVES HERE, not in core/. This is server-only code: it
 * owns deadzone, curve and inversion (docs/PROTOCOL.md §5.3 — "a client
 * MUST NOT apply a deadzone... the server profile owns deadzone, curve and
 * inversion") and the wire-to-Xbox button translation (§5.4). It does NOT
 * own the evdev +Y flip, which is a Linux/uinput quirk, not a profile
 * concern — that stays in backends/uinput.c.
 *
 * Profiles are now real (server/src/profiles.c, JSONC, docs/DESIGN.md §6.2/D6):
 * apad_mapping_apply() takes an `apad_profile *` resolved once per session
 * (server/src/main.c, at HELLO time) instead of one hardcoded default.
 * jsonc.c and profiles.c are their own translation units, compiled and
 * linked separately by scripts/build.sh's build_server() alongside this
 * file, not #included here — an earlier revision of this file amalgamated
 * them via #include as a workaround for a since-resolved build-script
 * constraint; that workaround is gone, this note is what's left of it.
 * Floating point and ordinary hosted C are fine throughout server/ —
 * core/'s constraints do not apply.
 */

#include <string.h>

#include "mapping.h"

/* ---- axis shaping: deadzone + curve, shared by sticks/triggers/touch/gyro */

static double curve_apply(double mag01, apad_curve_t curve)
{
    switch (curve) {
    case APAD_CURVE_LINEAR:
        return mag01;
    case APAD_CURVE_CUBIC:
        return mag01 * mag01 * mag01;
    case APAD_CURVE_QUADRATIC:
    default:
        return mag01 * mag01;   /* gentle near centre, full at the edge */
    }
}

/* `v` is a signed value roughly in [-1, 1] (stick/touch/gyro inputs can sit
 * fractionally outside that from rounding; harmless, `to_axis_i16` clamps
 * downstream). Returns a deadzone-and-curve-shaped value, still signed,
 * magnitude in [0, 1]: below `deadzone` collapses to exactly 0, everything
 * above is rescaled so [deadzone, 1.0] maps to [0.0, 1.0] instead of
 * leaving a dead jump at the boundary, then run through `curve`. */
static double shape_axis(double v, double deadzone, apad_curve_t curve)
{
    double mag = (v < 0.0) ? -v : v;
    double scaled;

    if (deadzone >= 1.0 || mag < deadzone) {
        return 0.0;   /* deadzone>=1.0 guarded explicitly: a malformed
                        * profile must not divide by zero below */
    }
    scaled = (mag - deadzone) / (1.0 - deadzone);
    if (scaled > 1.0) {
        scaled = 1.0;
    }
    scaled = curve_apply(scaled, curve);
    return (v < 0.0) ? -scaled : scaled;
}

static int16_t to_axis_i16(double v)
{
    double out = v * 32767.0;
    if (out > 32767.0) {
        out = 32767.0;
    }
    if (out < -32768.0) {
        out = -32768.0;
    }
    return (int16_t)out;
}

/* Deadzone + curve for one stick axis. `raw` is the wire value,
 * -32768..32767, already +Y-up (§5.3) — no evdev concerns here. */
static int16_t apply_stick_axis(int16_t raw, int invert, double deadzone,
                                apad_curve_t curve)
{
    double v = (double)raw / 32768.0;   /* -1.0 .. ~0.99997 */
    if (invert) {
        v = -v;
    }
    return to_axis_i16(shape_axis(v, deadzone, curve));
}

/* Deadzone only for a trigger axis, 0..32767 (already clamped non-negative
 * by apad_decode_input_state per §5). Linear past the deadzone: trigger
 * feel is a straight pull, not a stick's radial response. */
static int16_t apply_trigger_axis(int16_t raw, double deadzone)
{
    double v = (double)raw / 32767.0;
    if (v < 0.0) {
        v = 0.0;   /* belt and suspenders; decoder already enforces this */
    }
    return to_axis_i16(shape_axis(v, deadzone, APAD_CURVE_LINEAR));
}

/* ---- touch: regions -> buttons/triggers ---------------------------------
 *
 * Touch coordinates are wire-normalised, -32768..32767, +Y DOWN (§5.3,
 * §5.2). Rescaled here to 0..1, +Y down, to match a region rect's screen-
 * space convention.
 */
static void touch_to_unit(int16_t x, int16_t y, double *nx, double *ny)
{
    *nx = ((double)x + 32768.0) / 65535.0;
    *ny = ((double)y + 32768.0) / 65535.0;
}

/* "Depth into region": see profiles.h's apad_touch_region comment for the
 * (explicitly assumption-flagged) definition — normalised position along
 * whichever of the rect's two axes is longer. */
static double region_depth(const apad_touch_region *r, double nx, double ny)
{
    double w = r->rect[2] - r->rect[0];
    double h = r->rect[3] - r->rect[1];
    double depth;

    if (h >= w) {
        depth = (h > 0.0) ? (ny - r->rect[1]) / h : 0.0;
    } else {
        depth = (w > 0.0) ? (nx - r->rect[0]) / w : 0.0;
    }
    if (depth < 0.0) {
        depth = 0.0;
    }
    if (depth > 1.0) {
        depth = 1.0;
    }
    return depth;
}

/* Applies every active touch contact against `tp`'s regions (mode ==
 * APAD_TOUCH_MODE_REGIONS only — a no-op otherwise). A region-driven
 * trigger value is combined with whatever §5.4's trigger disambiguation
 * already put in out->lt/rt by taking the larger of the two: a 3DS-style
 * profile can give a device with no analog trigger (or no ZL/ZR at all) an
 * analog trigger from the touchscreen without suppressing a REAL ZL/ZR
 * press on hardware that has both. The first matching region wins for a
 * given touch point (regions are not expected to overlap, but if a profile
 * author makes them, this is the documented tie-break). */
static void apply_touch_regions(const apad_input_state *in,
                                 const apad_touch_profile *tp,
                                 apad_pad_state *out)
{
    int i, r;

    if (tp->mode != APAD_TOUCH_MODE_REGIONS) {
        return;
    }
    for (i = 0; i < (int)in->touch_count && i < APAD_TOUCH_MAX; i++) {
        double nx, ny;
        touch_to_unit(in->touches[i].x, in->touches[i].y, &nx, &ny);

        for (r = 0; r < tp->region_count; r++) {
            const apad_touch_region *rg = &tp->regions[r];
            int16_t v;

            if (nx < rg->rect[0] || nx > rg->rect[2]
                || ny < rg->rect[1] || ny > rg->rect[3]) {
                continue;
            }
            if (rg->target == APAD_TOUCH_TARGET_LT || rg->target == APAD_TOUCH_TARGET_RT) {
                v = rg->analog ? to_axis_i16(region_depth(rg, nx, ny))
                               : (int16_t)APAD_TRIGGER_MAX;
                if (rg->target == APAD_TOUCH_TARGET_LT) {
                    if (v > out->lt) { out->lt = v; }
                } else {
                    if (v > out->rt) { out->rt = v; }
                }
            } else {
                out->buttons = (uint16_t)(out->buttons | rg->pad_bit);
            }
            break;   /* one region per touch point */
        }
    }
}

/* ---- touch: delta / absolute -> a stick ---------------------------------*/

static void compute_touch_stick(const apad_input_state *in,
                                 const apad_touch_profile *tp,
                                 apad_mapping_state *state,
                                 int16_t *out_x, int16_t *out_y)
{
    double dx, dy;

    if (in->touch_count == 0) {
        if (tp->mode == APAD_TOUCH_MODE_DELTA_STICK) {
            state->touch_tracking = 0;   /* §6.2: return to centre on release */
        }
        *out_x = 0;
        *out_y = 0;
        return;
    }

    if (tp->mode == APAD_TOUCH_MODE_DELTA_STICK) {
        int16_t tx = in->touches[0].x, ty = in->touches[0].y;
        if (!state->touch_tracking) {
            state->touch_tracking = 1;
            state->touch_anchor_x = tx;
            state->touch_anchor_y = ty;
        }
        dx = (double)(tx - state->touch_anchor_x) / 32768.0;
        /* touch is +Y down, sticks are +Y up (§5.3) — invert here, before
         * the profile's own separate invert_y is layered on top below. */
        dy = -((double)(ty - state->touch_anchor_y) / 32768.0);
    } else {   /* APAD_TOUCH_MODE_ABSOLUTE_STICK */
        dx = (double)in->touches[0].x / 32768.0;
        dy = -((double)in->touches[0].y / 32768.0);
    }

    dx *= tp->sensitivity;
    dy *= tp->sensitivity;
    if (tp->invert_x) { dx = -dx; }
    if (tp->invert_y) { dy = -dy; }

    *out_x = to_axis_i16(shape_axis(dx, tp->deadzone, APAD_CURVE_LINEAR));
    *out_y = to_axis_i16(shape_axis(dy, tp->deadzone, APAD_CURVE_LINEAR));
}

/* ---- gyro -> a stick ------------------------------------------------------
 *
 * docs/PROTOCOL.md defines gyro as a rate (deci-degrees/second, §5) but
 * does not define how a rate becomes a stick deflection — this is a
 * mapping-engine judgment call, flagged as an explicit assumption rather
 * than asserted as spec. 6000 (600 deg/s) is picked as a representative
 * "full stick deflection" rate among common consumer gyro full-scale
 * ranges; a profile's `sensitivity` is the intended per-device tuning knob
 * on top of it, not a reason to pick a different constant per device.
 */
#define APAD_GYRO_FULL_SCALE_DDS 6000.0

static void compute_gyro_stick(const apad_input_state *in,
                               const apad_gyro_profile *gp,
                               int16_t *out_x, int16_t *out_y)
{
    double gx = (double)in->gyro[gp->axis_x] / APAD_GYRO_FULL_SCALE_DDS;
    double gy = (double)in->gyro[gp->axis_y] / APAD_GYRO_FULL_SCALE_DDS;

    gx *= gp->sensitivity;
    gy *= gp->sensitivity;
    if (gp->invert_x) { gx = -gx; }
    if (gp->invert_y) { gy = -gy; }

    *out_x = to_axis_i16(shape_axis(gx, gp->deadzone, APAD_CURVE_LINEAR));
    *out_y = to_axis_i16(shape_axis(gy, gp->deadzone, APAD_CURVE_LINEAR));
}

/* Sum two shaped axis values and clamp to int16 range. Used only by gyro
 * AIM mode: -32768 has no positive counterpart (docs/PROTOCOL.md §5.4, same
 * asymmetry uinput.c's negate_y() has to clamp for) so this clamps both
 * ends explicitly rather than relying on wraparound. */
static int16_t clamp_add_i16(int16_t a, int16_t b)
{
    int32_t sum = (int32_t)a + (int32_t)b;
    if (sum > 32767) {
        sum = 32767;
    }
    if (sum < -32768) {
        sum = -32768;
    }
    return (int16_t)sum;
}

/* ---- stick assembly -------------------------------------------------------
 *
 * Two gyro relationships to a physical stick, both gated on APAD_CAP_GYRO
 * and on the profile's gyro section actually targeting THIS stick
 * (profiles.h's apad_gyro_mode_t has the full rationale):
 *
 *   SUBSTITUTE — physical > touch substitute > gyro substitute > centred.
 *   docs/DESIGN.md §6.2: gyro-to-right-stick is "essential on Old 3DS, which has no
 *   second stick" -- replaces a stick that isn't physically there. Never
 *   adds to a physical stick that IS present; this path (and its priority
 *   position) is exactly what existed before AIM mode.
 *
 *   AIM — physical stick's own shaped output, PLUS the gyro's shaped
 *   output, clamped: a New 3DS's C-stick (APAD_CAP_STICK_R) does coarse
 *   aiming, gyro does fine correction on top, matching how gyro aim works
 *   on Switch/Steam Deck. When the client did NOT advertise the stick
 *   `caps` bit, the "physical" side of that sum is simply 0 -- which makes
 *   AIM mode numerically IDENTICAL to SUBSTITUTE in that case (0 + gyro =
 *   gyro), not a different, weaker behaviour. This is why one profile using
 *   "mode": "aim" can be the single default for both an Old 3DS (no
 *   STICK_R -> gyro alone drives the stick, unchanged from before) and a
 *   New 3DS (STICK_R present -> gyro adds fine correction on top of the
 *   C-stick) -- see server/profiles/3ds-default.jsonc's header comment.
 *   Still gated on APAD_CAP_TOUCH taking priority when there's no physical
 *   stick, same as SUBSTITUTE: a device with a working touch-stick
 *   substitute should not have that overridden by a gyro-only fallback.
 *
 * Both are gated on the matching APAD_CAP_* bit regardless of what the
 * profile asks for: a profile cannot manufacture hardware the client never
 * advertised.
 */
static void compute_stick(int right_side, const apad_input_state *in, uint32_t caps,
                          const apad_profile *p, apad_mapping_state *state,
                          int16_t *out_x, int16_t *out_y)
{
    uint32_t stick_cap = right_side ? (uint32_t)APAD_CAP_STICK_R
                                     : (uint32_t)APAD_CAP_STICK_L;
    const apad_stick_profile *st = right_side ? &p->right : &p->left;
    const apad_gyro_profile *gp = &p->gyro;
    int have_stick = (caps & stick_cap) != 0;
    int gyro_targets_this_stick = (caps & APAD_CAP_GYRO)
        && gp->mode != APAD_GYRO_MODE_NONE
        && (gp->stick_is_right != 0) == (right_side != 0);
    int16_t phys_x = 0, phys_y = 0;

    if (have_stick) {
        int axis_x = right_side ? APAD_AXIS_RX : APAD_AXIS_LX;
        int axis_y = right_side ? APAD_AXIS_RY : APAD_AXIS_LY;
        phys_x = apply_stick_axis(in->axes[axis_x], st->invert_x, st->deadzone, st->curve);
        phys_y = apply_stick_axis(in->axes[axis_y], st->invert_y, st->deadzone, st->curve);

        if (gyro_targets_this_stick && gp->mode == APAD_GYRO_MODE_AIM) {
            int16_t gx, gy;
            compute_gyro_stick(in, gp, &gx, &gy);
            *out_x = clamp_add_i16(phys_x, gx);
            *out_y = clamp_add_i16(phys_y, gy);
            return;
        }
        *out_x = phys_x;
        *out_y = phys_y;
        return;
    }

    if ((caps & APAD_CAP_TOUCH)
        && (p->touch.mode == APAD_TOUCH_MODE_DELTA_STICK
            || p->touch.mode == APAD_TOUCH_MODE_ABSOLUTE_STICK)
        && (p->touch.stick_is_right != 0) == (right_side != 0)) {
        compute_touch_stick(in, &p->touch, state, out_x, out_y);
        return;
    }

    if (gyro_targets_this_stick) {
        /* No physical stick to add to: SUBSTITUTE and AIM are the same
         * computation here (see comment above compute_stick). */
        compute_gyro_stick(in, gp, out_x, out_y);
        return;
    }

    *out_x = 0;
    *out_y = 0;
}

/* ---- public API -----------------------------------------------------------*/

void apad_mapping_state_init(apad_mapping_state *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof *st);
}

void apad_mapping_apply(const apad_input_state *in, uint32_t caps,
                        const apad_profile *profile,
                        apad_mapping_state *state,
                        apad_pad_state *out)
{
    const apad_profile *p = (profile != NULL) ? profile : apad_profiles_builtin_default();
    apad_mapping_state scratch_state;
    int i;

    if (in == NULL || out == NULL) {
        return;
    }
    if (state == NULL) {
        /* Never crash for a caller that hasn't wired up per-session state
         * yet; delta-stick return-to-centre just won't persist across
         * calls in that case. */
        apad_mapping_state_init(&scratch_state);
        state = &scratch_state;
    }
    memset(out, 0, sizeof *out);

    for (i = 0; i < APAD_PROFILE_BTN_COUNT; i++) {
        if (in->buttons & apad_profile_wire_btn_bits[i]) {
            out->buttons = (uint16_t)(out->buttons | p->btn_pad_bit[i]);
        }
    }

    /* §5.1: D-pad is bits 4..7, contiguous. Use the core's own LUT rather
     * than re-deriving hat logic here — never reimplement protocol logic.
     * Deliberately not profile-remappable: the D-pad's physical layout has
     * no ambiguity to resolve the way face buttons do. */
    out->hat = apad_hat_from_buttons(in->buttons);

    compute_stick(0 /* left */,  in, caps, p, state, &out->lx, &out->ly);
    compute_stick(1 /* right */, in, caps, p, state, &out->rx, &out->ry);

    /* §5.4 trigger disambiguation: honour APAD_CAP_TRIGGERS rather than
     * guessing from which fields happen to be nonzero. */
    if (caps & APAD_CAP_TRIGGERS) {
        out->lt = apply_trigger_axis(in->axes[APAD_AXIS_L2], p->trigger.deadzone);
        out->rt = apply_trigger_axis(in->axes[APAD_AXIS_R2], p->trigger.deadzone);
    } else {
        out->lt = (in->buttons & APAD_BTN_ZL) ? (int16_t)APAD_TRIGGER_MAX : 0;
        out->rt = (in->buttons & APAD_BTN_ZR) ? (int16_t)APAD_TRIGGER_MAX : 0;
    }

    if (caps & APAD_CAP_TOUCH) {
        apply_touch_regions(in, &p->touch, out);
    }
}
