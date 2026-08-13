/* server/src/profiles.h — JSONC per-device mapping profiles (docs/DESIGN.md §6.2,
 * D6, D9).
 *
 * Server-only, like everything under server/src/ and server/backends/ --
 * core/'s no-malloc / no-float / no-stdio constraints do not apply here.
 * A profile is resolved once per session, at HELLO time (server/src/server.c
 * matches on the decoded device_name), and held for the session's life; it
 * is never re-derived on the INPUT_STATE hot path.
 *
 * Compiled as its own translation unit, linked alongside mapping.c and
 * jsonc.c by scripts/build.sh's build_server().
 */
#ifndef ATTICPAD_SERVER_PROFILES_H
#define ATTICPAD_SERVER_PROFILES_H

#include <stddef.h>
#include <stdint.h>

#include "atticpad/atticpad.h"
#include "apadserver.h"   /* apad_profile_source */
#include "backend.h"
#include "serverlog.h"    /* apad_log_sink */

#ifdef __cplusplus
extern "C" {
#endif

#define APAD_PROFILE_NAME_LEN    64
#define APAD_PROFILE_MAX_REGIONS 8
#define APAD_PROFILES_MAX        16

/* The wire (Nintendo-convention) digital buttons a profile is allowed to
 * remap. D-pad and ZL/ZR are deliberately excluded: the D-pad always goes
 * through apad_hat_from_buttons() (never reimplemented, never remapped --
 * "never reimplement protocol logic"), and ZL/ZR are routed into LT/RT by
 * §5.4's trigger disambiguation rule, not into a digital pad button, exactly
 * as the pre-profile hardcoded table already did. TOUCH_PRESS,
 * TOUCH_REAR_PRESS and CAPTURE are also absent -- not remappable either, so
 * the editor's button vocabulary is exactly these 11 and nothing else. */
#define APAD_PROFILE_BTN_COUNT   11
extern const uint32_t apad_profile_wire_btn_bits[APAD_PROFILE_BTN_COUNT];
extern const char    *apad_profile_wire_btn_names[APAD_PROFILE_BTN_COUNT];

/* The pad-button name a profile file's "buttons"/touch "emit" values are
 * spelled with (backend.h's APAD_PADBTN_* -- "A", "LB", "RTHUMB", ...), or
 * "NONE" for bit == 0 (a wire button deliberately left unmapped, or a touch
 * region with no digital target). The read side of this table
 * (pad_bit_from_name()) has always been private to profiles.c because
 * nothing outside it needed to go the other way -- this export is new for
 * the web remapping editor (server/host/common/profile_json.h), which needs
 * to serialise a loaded apad_profile BACK into the same JSON vocabulary
 * profiles.c's own parser accepts, so a GET response round-trips through a
 * PUT unchanged. Does not touch APAD_PROFILE_BTN_COUNT or the wire-button
 * vocabulary those are frozen (server-dev docs/CONVENTIONS.md); this is a read-only
 * lookup over the existing, unmodified g_pad_btn_names table. */
const char *apad_pad_btn_name(uint16_t bit);

typedef enum {
    APAD_CURVE_LINEAR = 0,
    APAD_CURVE_QUADRATIC,
    APAD_CURVE_CUBIC
} apad_curve_t;

typedef struct {
    double       deadzone;   /* fraction of full scale, 0..1 */
    apad_curve_t curve;
    int          invert_x, invert_y;
} apad_stick_profile;

typedef struct {
    double deadzone;         /* fraction of full scale, 0..1 */
} apad_trigger_profile;

typedef enum {
    APAD_TOUCH_TARGET_BUTTON = 0,
    APAD_TOUCH_TARGET_LT,
    APAD_TOUCH_TARGET_RT
} apad_touch_target_t;

/*
 * One touch->button/trigger region. `rect` is normalised screen space,
 * 0..1, +Y down (matches the wire's touch convention, §5.3) -- [x0,y0,x1,y1].
 *
 * "Depth-into-region" (for `analog`) is defined as the touch's normalised
 * position along whichever of the rect's two axes is longer: a tall,
 * narrow rect (like docs/DESIGN.md §6.2's two half-screen LT/RT zones) reads depth
 * as vertical travel -- slide the thumb down for more pressure, which is
 * the natural feel for a soft on-screen shoulder trigger -- while a wide,
 * short rect reads it horizontally. This is a judgment call: the spec and
 * docs/DESIGN.md's example do not define "depth" precisely. Documented here as an
 * assumption, not asserted as the one true reading.
 */
typedef struct {
    double               rect[4];
    apad_touch_target_t  target;
    uint16_t             pad_bit;   /* valid when target == APAD_TOUCH_TARGET_BUTTON */
    int                  analog;    /* only meaningful when target is LT/RT */
} apad_touch_region;

typedef enum {
    APAD_TOUCH_MODE_NONE = 0,
    APAD_TOUCH_MODE_REGIONS,
    APAD_TOUCH_MODE_DELTA_STICK,
    APAD_TOUCH_MODE_ABSOLUTE_STICK
} apad_touch_mode_t;

typedef struct {
    apad_touch_mode_t mode;
    apad_touch_region regions[APAD_PROFILE_MAX_REGIONS];
    int                region_count;
    int                stick_is_right;   /* which output stick delta/absolute drives */
    double             sensitivity;
    double             deadzone;
    int                invert_x, invert_y;
} apad_touch_profile;

typedef enum {
    APAD_GYRO_AXIS_PITCH = 0,
    APAD_GYRO_AXIS_ROLL,
    APAD_GYRO_AXIS_YAW
} apad_gyro_axis_t;

/*
 * SUBSTITUTE: gyro stands in for a stick the client does not physically
 * have (docs/DESIGN.md §6.2's Old-3DS case, "gyro drives the right stick... no
 * second stick"). Never adds to a physical stick that IS present -- when
 * the matching APAD_CAP_STICK_* bit is set, SUBSTITUTE mode is a no-op for
 * that stick, same priority position as before AIM mode existed
 * (mapping.c's compute_stick(): physical > touch substitute > gyro
 * substitute > centred).
 *
 * AIM: gyro adds to whatever the physical stick is already doing (0 if the
 * client didn't advertise that stick), clamped to the axis range -- for a
 * device that HAS the stick (New 3DS's C-stick: "caps=0x000004EF"
 * advertises APAD_CAP_STICK_R). This is the Switch/Steam Deck "gyro aim"
 * pattern: the stick does coarse aiming, gyro does fine correction on top,
 * they sum rather than one replacing the other. When the physical stick
 * ISN'T present, the "physical" side of that sum is 0, which makes AIM
 * numerically IDENTICAL to SUBSTITUTE in that case (0 + gyro = gyro) --
 * mapping.c's compute_stick() comment has the proof. That equivalence is
 * exactly why one AIM-mode profile section, not two mode-specific ones, can
 * be the single default for a device family that may or may not have the
 * stick (server/profiles/3ds-default.jsonc's header comment: one profile
 * covers both an Old 3DS with no C-stick and a New 3DS with one).
 */
typedef enum {
    APAD_GYRO_MODE_NONE = 0,
    APAD_GYRO_MODE_SUBSTITUTE,
    APAD_GYRO_MODE_AIM
} apad_gyro_mode_t;

typedef struct {
    apad_gyro_mode_t  mode;
    int               stick_is_right;
    apad_gyro_axis_t  axis_x, axis_y;
    double            sensitivity;
    double            deadzone;
    int               invert_x, invert_y;
} apad_gyro_profile;

/* One fully-resolved profile. Everything mapping.c needs; nothing it has to
 * go back to JSON for on the hot path. */
typedef struct apad_profile {
    char                  name[APAD_PROFILE_NAME_LEN];
    char                  match_device[APAD_PROFILE_NAME_LEN]; /* "" = matches anything */
    uint16_t              btn_pad_bit[APAD_PROFILE_BTN_COUNT]; /* 0 = dropped */
    apad_stick_profile    left, right;
    apad_trigger_profile  trigger;
    apad_touch_profile    touch;
    apad_gyro_profile     gyro;
} apad_profile;

/*
 * Parses `count` JSONC blobs, in the order given, into the loaded set.
 * A malformed blob is logged through `log` and skipped -- never fatal, and
 * never leaves a half-applied profile in the table. Safe to call more than
 * once; each call replaces the previously loaded set. `sources == NULL` (or
 * count == 0) clears it back to "only the built-in default".
 *
 * Blobs rather than a directory path is the library/host seam (docs/DESIGN.md
 * §6.4): finding and reading files is the host's, parsing is the library's.
 * `log` is retained for this file's later diagnostics and so must outlive
 * the loaded set -- apad_server_create() passes its own sink, which lives as
 * long as the server does.
 *
 * Copies everything it keeps: the sources need not outlive the call.
 */
void apad_profiles_load(const apad_profile_source *sources, size_t count,
                        const apad_log_sink *log);

/*
 * Best match for `device_name` (NUL-terminated, as decoded from HELLO):
 * the first loaded profile whose non-empty `match_device` is a substring of
 * `device_name`, else apad_profiles_builtin_default(). Never returns NULL.
 */
const apad_profile *apad_profiles_match(const char *device_name);

/* The compiled-in fallback: always available, even if apad_profiles_load()
 * was never called, was handed nothing, or every blob it was handed failed
 * to parse. */
const apad_profile *apad_profiles_builtin_default(void);

/* server UI support (docs/DESIGN.md §6.3 "profile selector per connected client"):
 * exact lookup by `name` (apad_profile::name -- the JSONC "profile" field,
 * or the source's basename fallback), never a substring match like
 * apad_profiles_match(). Also matches the built-in default's own name, so a
 * UI offering it in its selector can always find it again by name. Returns
 * NULL if nothing matches; unlike apad_profiles_match() this has no
 * "never returns NULL" guarantee, because "no such profile" is exactly what
 * a caller here needs to be told. */
const apad_profile *apad_profiles_find(const char *name);

/* Every loaded profile plus the built-in default, for a UI's profile
 * selector -- pointers are into the file-static table (see profiles.c's
 * struct apad_server comment on why that table isn't per-instance yet) and
 * stay valid until the next apad_profiles_load() call. Writes up to `max`
 * pointers into `out` and returns the total count available (which may
 * exceed `max`); `out` may be NULL to just ask for the count. The built-in
 * default is always last. */
size_t apad_profiles_list(const apad_profile **out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_PROFILES_H */
