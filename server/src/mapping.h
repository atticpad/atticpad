/* server/src/mapping.h — the mapping engine (docs/DESIGN.md §6.2, D9).
 *
 * THE MAPPING ENGINE LIVES HERE, in server/, never in core/. No client ever
 * evaluates a mapping profile, so subjecting it to the DS's no-malloc /
 * no-float constraints would buy nothing and cost every handheld binary
 * size. Server code may malloc and use floating point freely (docs/CONVENTIONS.md).
 */
#ifndef ATTICPAD_SERVER_MAPPING_H
#define ATTICPAD_SERVER_MAPPING_H

#include <stdint.h>

#include "atticpad/atticpad.h"
#include "backend.h"
#include "profiles.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-session state the mapping engine needs to carry BETWEEN calls, owned
 * entirely by mapping.c. server/src/main.c just embeds one per session,
 * zero-inits it once (apad_mapping_state_init, at session creation, same
 * moment a profile is matched), and passes the same pointer back on every
 * subsequent INPUT_STATE for that session -- it never reaches inside.
 *
 * Needed for exactly one mode: touch->right/left-stick "delta" mapping,
 * which is relative to wherever the contact started and must snap back to
 * centre the instant the contact lifts (docs/DESIGN.md §6.2: "touch delta to
 * right stick with return-to-centre"). Every other mode (regions, absolute
 * touch, gyro, buttons, physical sticks) is a pure function of the current
 * INPUT_STATE alone and carries no state.
 */
typedef struct {
    int     touch_tracking;            /* 1 while a delta-mode contact is down */
    int16_t touch_anchor_x, touch_anchor_y;   /* wire coords when it started */
} apad_mapping_state;

void apad_mapping_state_init(apad_mapping_state *st);

/*
 * Turn one decoded wire INPUT_STATE into the backend-independent pad state,
 * per `profile` (server/src/profiles.c -- match once at HELLO time, hold
 * for the session). `caps` is the APAD_CAP_* mask the client advertised in
 * HELLO, used for §5.4's trigger-disambiguation rule and to gate every
 * profile feature (touch, gyro, second stick) on the hardware actually
 * being present -- a profile can only ask for what the capability mask
 * says exists. `state` is this session's apad_mapping_state, updated in
 * place.
 *
 * Owns everything docs/PROTOCOL.md assigns to the server: deadzone, curve,
 * axis inversion, and the wire (Nintendo-convention) -> Xbox-convention
 * button and face-button translation. Never applies the evdev +Y flip --
 * that is a backend quirk, not a profile concern, and lives in uinput.c.
 */
void apad_mapping_apply(const apad_input_state *in, uint32_t caps,
                        const apad_profile *profile,
                        apad_mapping_state *state,
                        apad_pad_state *out);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_MAPPING_H */
