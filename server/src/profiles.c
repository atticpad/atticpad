/* server/src/profiles.c — JSONC per-device mapping profiles (docs/DESIGN.md §6.2,
 * D6, D9). See profiles.h for the public shape and jsonc.h for why the
 * parser looks the way it does. Its own translation unit, linked by
 * scripts/build.sh's build_server() alongside mapping.c and jsonc.c.
 *
 * Profiles arrive as memory blobs, never as a directory path: this is
 * library code (docs/DESIGN.md §6.4) and the library does no filesystem access.
 * Directory scanning used to live at the bottom of this file behind POSIX
 * <dirent.h>, with a note that a Windows backend would need its own
 * directory-listing path here -- the library split answered that by moving
 * the scan into the host instead, so there is nothing POSIX-specific left
 * in this file and nothing for Windows to reimplement inside the library.
 *
 * Diagnostics go to the host's sink (serverlog.h), not to stderr. Same text
 * as before, minus the "[atticpad] " prefix, which the host now adds.
 */
#include "profiles.h"

#include <stdio.h>    /* snprintf: formatting only, never a stream */
#include <string.h>

#include "jsonc.h"
#include "serverlog.h"

/* Where this file's diagnostics go, for the duration of a load. Set by
 * apad_profiles_load() from the sink its caller (apad_server_create) was
 * given, and read by the dozen degrade-and-warn paths below without having
 * to thread a parameter through every one of them. NULL until the first
 * load, which apad_logf() treats as "discard". */
static const apad_log_sink *g_log;

/* ---- wire button name table (shared with mapping.c) --------------------*/

const uint32_t apad_profile_wire_btn_bits[APAD_PROFILE_BTN_COUNT] = {
    APAD_BTN_A, APAD_BTN_B, APAD_BTN_X, APAD_BTN_Y,
    APAD_BTN_L, APAD_BTN_R, APAD_BTN_L3, APAD_BTN_R3,
    APAD_BTN_START, APAD_BTN_SELECT, APAD_BTN_HOME
};
const char *apad_profile_wire_btn_names[APAD_PROFILE_BTN_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "L3", "R3", "START", "SELECT", "HOME"
};

/* Xbox-convention pad button names a profile may target (backend.h). */
typedef struct { const char *name; uint16_t bit; } named_pad_bit;
static const named_pad_bit g_pad_btn_names[] = {
    { "A",      APAD_PADBTN_A      },
    { "B",      APAD_PADBTN_B      },
    { "X",      APAD_PADBTN_X      },
    { "Y",      APAD_PADBTN_Y      },
    { "LB",     APAD_PADBTN_LB     },
    { "RB",     APAD_PADBTN_RB     },
    { "BACK",   APAD_PADBTN_BACK   },
    { "START",  APAD_PADBTN_START  },
    { "GUIDE",  APAD_PADBTN_GUIDE  },
    { "LTHUMB", APAD_PADBTN_LTHUMB },
    { "RTHUMB", APAD_PADBTN_RTHUMB }
};
#define PAD_BTN_NAME_COUNT (int)(sizeof g_pad_btn_names / sizeof g_pad_btn_names[0])

static int pad_bit_from_name(const char *name, uint16_t *out_bit)
{
    int i;
    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < PAD_BTN_NAME_COUNT; i++) {
        if (strcmp(g_pad_btn_names[i].name, name) == 0) {
            *out_bit = g_pad_btn_names[i].bit;
            return 1;
        }
    }
    return 0;
}

const char *apad_pad_btn_name(uint16_t bit)
{
    int i;
    if (bit == 0u) {
        return "NONE";
    }
    for (i = 0; i < PAD_BTN_NAME_COUNT; i++) {
        if (g_pad_btn_names[i].bit == bit) {
            return g_pad_btn_names[i].name;
        }
    }
    return "NONE";   /* an unrecognised bit has no name to give back */
}

/* ---- the built-in fallback ---------------------------------------------*/

static void set_builtin_default(apad_profile *p)
{
    int i;

    memset(p, 0, sizeof *p);
    (void)snprintf(p->name, sizeof p->name, "builtin-default");
    p->match_device[0] = '\0';

    /* §5.4 physical-position mapping: wire A(right)->Xbox B, B(bottom)->A,
     * X(top)->Y, Y(left)->X. Identical to mapping.c's pre-profile table. */
    for (i = 0; i < APAD_PROFILE_BTN_COUNT; i++) {
        p->btn_pad_bit[i] = 0;
    }
    p->btn_pad_bit[0] = APAD_PADBTN_B;      /* A */
    p->btn_pad_bit[1] = APAD_PADBTN_A;      /* B */
    p->btn_pad_bit[2] = APAD_PADBTN_Y;      /* X */
    p->btn_pad_bit[3] = APAD_PADBTN_X;      /* Y */
    p->btn_pad_bit[4] = APAD_PADBTN_LB;     /* L */
    p->btn_pad_bit[5] = APAD_PADBTN_RB;     /* R */
    p->btn_pad_bit[6] = APAD_PADBTN_LTHUMB; /* L3 */
    p->btn_pad_bit[7] = APAD_PADBTN_RTHUMB; /* R3 */
    p->btn_pad_bit[8] = APAD_PADBTN_START;  /* START */
    p->btn_pad_bit[9] = APAD_PADBTN_BACK;   /* SELECT */
    p->btn_pad_bit[10] = APAD_PADBTN_GUIDE; /* HOME */

    p->left.deadzone  = 0.08;
    p->left.curve     = APAD_CURVE_QUADRATIC;
    p->right.deadzone = 0.08;
    p->right.curve    = APAD_CURVE_QUADRATIC;
    p->trigger.deadzone = 0.02;
    p->touch.mode = APAD_TOUCH_MODE_NONE;
    p->gyro.mode = APAD_GYRO_MODE_NONE;
}

static const apad_profile *g_builtin_default_ptr(void)
{
    static apad_profile builtin;
    static int initialized;
    if (!initialized) {
        set_builtin_default(&builtin);
        initialized = 1;
    }
    return &builtin;
}

const apad_profile *apad_profiles_builtin_default(void)
{
    return g_builtin_default_ptr();
}

/* ---- the loaded set ------------------------------------------------------*/

static apad_profile g_profiles[APAD_PROFILES_MAX];
static int          g_profile_count;

/* ---- JSON -> apad_profile -------------------------------------------------
 *
 * Every reader here degrades: a missing key keeps whatever the caller
 * pre-filled (the built-in default, applied before any of these run), and
 * a present-but-unrecognised value logs a warning and otherwise behaves the
 * same way -- one bad enum string skips that ONE field, never the whole
 * profile.
 */

static apad_curve_t curve_from_name(const char *name, apad_curve_t fallback,
                                    const char *file)
{
    if (name == NULL) {
        return fallback;
    }
    if (strcmp(name, "linear") == 0) {
        return APAD_CURVE_LINEAR;
    }
    if (strcmp(name, "quadratic") == 0) {
        return APAD_CURVE_QUADRATIC;
    }
    if (strcmp(name, "cubic") == 0) {
        return APAD_CURVE_CUBIC;
    }
    apad_logf(g_log, APAD_LOG_WARN,
                            "profiles: %s: unrecognised curve \"%s\", "
                            "leaving previous value", file, name);
    return fallback;
}

static void read_stick(const json_value *node, apad_stick_profile *st,
                       const char *file)
{
    const json_value *v;

    if (node == NULL) {
        return;
    }
    st->deadzone = json_num(json_obj_get(node, "deadzone"), st->deadzone);
    st->curve = curve_from_name(json_str(json_obj_get(node, "curve"), NULL),
                                st->curve, file);
    st->invert_x = json_bool(json_obj_get(node, "invert_x"), st->invert_x);
    st->invert_y = json_bool(json_obj_get(node, "invert_y"), st->invert_y);
    v = json_obj_get(node, "invert");   /* convenience: invert both axes at once */
    if (v != NULL) {
        st->invert_x = st->invert_y = json_bool(v, 0);
    }
}

/* "sticks": accepts a shared set of fields applied to both left and right
 * (docs/DESIGN.md §6.2's own example: `{"left": "left", "deadzone": 0.08, "curve":
 * "quadratic"}` is exactly this flatter shape), a nested {"left": {...},
 * "right": {...}} nested-object form for per-stick control, or both --
 * flat fields apply as defaults first, then a nested per-stick object
 * overrides just that stick. Deliberately permissive: docs/DESIGN.md's example
 * is illustrative, not a strict schema, so both readings are honoured
 * rather than picking one and rejecting the other. */
static void read_sticks(const json_value *sticks, apad_profile *p, const char *file)
{
    const json_value *left, *right;

    if (sticks == NULL || sticks->type != JSON_OBJECT) {
        return;
    }
    read_stick(sticks, &p->left, file);
    read_stick(sticks, &p->right, file);

    left = json_obj_get(sticks, "left");
    if (left != NULL && left->type == JSON_OBJECT) {
        read_stick(left, &p->left, file);
    }
    right = json_obj_get(sticks, "right");
    if (right != NULL && right->type == JSON_OBJECT) {
        read_stick(right, &p->right, file);
    }
}

static void read_buttons(const json_value *buttons, apad_profile *p, const char *file)
{
    int i;

    if (buttons == NULL || buttons->type != JSON_OBJECT) {
        return;
    }
    for (i = 0; i < APAD_PROFILE_BTN_COUNT; i++) {
        const json_value *v = json_obj_get(buttons, apad_profile_wire_btn_names[i]);
        const char *name;
        uint16_t bit;

        if (v == NULL) {
            continue;
        }
        name = json_str(v, NULL);
        if (name == NULL) {
            continue;
        }
        if (strcmp(name, "NONE") == 0) {
            p->btn_pad_bit[i] = 0;
            continue;
        }
        if (pad_bit_from_name(name, &bit)) {
            p->btn_pad_bit[i] = bit;
        } else {
            apad_logf(g_log, APAD_LOG_WARN,
                                    "profiles: %s: unrecognised pad button "
                                    "\"%s\" for wire button \"%s\", leaving unmapped",
                                    file, name, apad_profile_wire_btn_names[i]);
        }
    }
}

static void read_triggers(const json_value *triggers, apad_profile *p)
{
    if (triggers == NULL) {
        return;
    }
    p->trigger.deadzone = json_num(json_obj_get(triggers, "deadzone"),
                                   p->trigger.deadzone);
}

/* target: "LT" / "RT" for the analog triggers, any of g_pad_btn_names for a
 * digital button, or "NONE" to make a region a no-op (documents intent
 * better than simply omitting it). */
static int read_touch_target(const json_value *region, apad_touch_region *out,
                             const char *file)
{
    const char *emit = json_str(json_obj_get(region, "emit"), NULL);
    uint16_t bit;

    if (emit == NULL) {
        apad_logf(g_log, APAD_LOG_WARN,
                                "profiles: %s: touch region missing \"emit\"",
                                file);
        return 0;
    }
    if (strcmp(emit, "LT") == 0) {
        out->target = APAD_TOUCH_TARGET_LT;
        out->pad_bit = 0;
        return 1;
    }
    if (strcmp(emit, "RT") == 0) {
        out->target = APAD_TOUCH_TARGET_RT;
        out->pad_bit = 0;
        return 1;
    }
    if (strcmp(emit, "NONE") == 0) {
        out->target = APAD_TOUCH_TARGET_BUTTON;
        out->pad_bit = 0;
        return 1;
    }
    if (pad_bit_from_name(emit, &bit)) {
        out->target = APAD_TOUCH_TARGET_BUTTON;
        out->pad_bit = bit;
        return 1;
    }
    apad_logf(g_log, APAD_LOG_WARN,
                            "profiles: %s: touch region has unrecognised "
                            "\"emit\" value \"%s\", skipping region", file, emit);
    return 0;
}

static void read_touch_regions(const json_value *regions, apad_profile *p,
                               const char *file)
{
    size_t i;

    if (regions == NULL || regions->type != JSON_ARRAY) {
        return;
    }
    for (i = 0; i < regions->count && p->touch.region_count < APAD_PROFILE_MAX_REGIONS; i++) {
        const json_value *region = regions->items[i];
        const json_value *rect = json_obj_get(region, "rect");
        apad_touch_region r;

        memset(&r, 0, sizeof r);
        if (rect == NULL || rect->type != JSON_ARRAY || rect->count != 4) {
            apad_logf(g_log, APAD_LOG_WARN,
                                    "profiles: %s: touch region %u missing/bad "
                                    "\"rect\" (need [x0,y0,x1,y1]), skipping",
                                    file, (unsigned)i);
            continue;
        }
        r.rect[0] = json_num(rect->items[0], 0.0);
        r.rect[1] = json_num(rect->items[1], 0.0);
        r.rect[2] = json_num(rect->items[2], 1.0);
        r.rect[3] = json_num(rect->items[3], 1.0);
        if (!read_touch_target(region, &r, file)) {
            continue;
        }
        r.analog = json_bool(json_obj_get(region, "analog"), 0);
        p->touch.regions[p->touch.region_count++] = r;
    }
}

static apad_touch_mode_t touch_mode_from_name(const char *name, const char *file)
{
    if (name == NULL || strcmp(name, "none") == 0) {
        return APAD_TOUCH_MODE_NONE;
    }
    if (strcmp(name, "regions") == 0) {
        return APAD_TOUCH_MODE_REGIONS;
    }
    if (strcmp(name, "delta_stick") == 0) {
        return APAD_TOUCH_MODE_DELTA_STICK;
    }
    if (strcmp(name, "absolute_stick") == 0) {
        return APAD_TOUCH_MODE_ABSOLUTE_STICK;
    }
    apad_logf(g_log, APAD_LOG_WARN,
                            "profiles: %s: unrecognised touch mode \"%s\", "
                            "treating as \"none\"", file, name);
    return APAD_TOUCH_MODE_NONE;
}

static void read_touch(const json_value *touch, apad_profile *p, const char *file)
{
    const char *stick_name;

    if (touch == NULL) {
        return;
    }
    p->touch.mode = touch_mode_from_name(
        json_str(json_obj_get(touch, "mode"), NULL), file);
    stick_name = json_str(json_obj_get(touch, "stick"), NULL);
    if (stick_name != NULL) {
        p->touch.stick_is_right = (strcmp(stick_name, "right") == 0);
    }
    p->touch.sensitivity = json_num(json_obj_get(touch, "sensitivity"), 1.0);
    p->touch.deadzone    = json_num(json_obj_get(touch, "deadzone"), 0.0);
    p->touch.invert_x    = json_bool(json_obj_get(touch, "invert_x"), 0);
    p->touch.invert_y    = json_bool(json_obj_get(touch, "invert_y"), 0);
    read_touch_regions(json_obj_get(touch, "regions"), p, file);
}

static apad_gyro_axis_t gyro_axis_from_name(const char *name, apad_gyro_axis_t fallback,
                                            const char *file)
{
    if (name == NULL) {
        return fallback;
    }
    if (strcmp(name, "pitch") == 0) {
        return APAD_GYRO_AXIS_PITCH;
    }
    if (strcmp(name, "roll") == 0) {
        return APAD_GYRO_AXIS_ROLL;
    }
    if (strcmp(name, "yaw") == 0) {
        return APAD_GYRO_AXIS_YAW;
    }
    apad_logf(g_log, APAD_LOG_WARN,
                            "profiles: %s: unrecognised gyro axis \"%s\"",
                            file, name);
    return fallback;
}

/* "mode": "none" (default) | "substitute" (gyro stands in for a stick the
 * client doesn't have -- old name "right_stick"/"left_stick" retired, see
 * profiles.h's apad_gyro_mode_t) | "aim" (gyro adds to a physical stick's
 * output, new -- profiles.h's apad_gyro_mode_t comment). Which stick is a
 * separate "stick": "left"|"right" field, same shape as "touch"'s existing
 * "stick" field, instead of folding it into the mode name -- one axis of
 * variation per key. */
static apad_gyro_mode_t gyro_mode_from_name(const char *name, const char *file)
{
    if (name == NULL || strcmp(name, "none") == 0) {
        return APAD_GYRO_MODE_NONE;
    }
    if (strcmp(name, "substitute") == 0) {
        return APAD_GYRO_MODE_SUBSTITUTE;
    }
    if (strcmp(name, "aim") == 0) {
        return APAD_GYRO_MODE_AIM;
    }
    apad_logf(g_log, APAD_LOG_WARN,
                            "profiles: %s: unrecognised gyro mode \"%s\", "
                            "disabling gyro", file, name);
    return APAD_GYRO_MODE_NONE;
}

static void read_gyro(const json_value *gyro, apad_profile *p, const char *file)
{
    const char *stick_name;

    if (gyro == NULL) {
        return;
    }
    p->gyro.mode = gyro_mode_from_name(json_str(json_obj_get(gyro, "mode"), NULL), file);
    if (p->gyro.mode == APAD_GYRO_MODE_NONE) {
        return;
    }

    stick_name = json_str(json_obj_get(gyro, "stick"), NULL);
    if (stick_name != NULL) {
        p->gyro.stick_is_right = (strcmp(stick_name, "right") == 0);
    }
    p->gyro.axis_x = gyro_axis_from_name(json_str(json_obj_get(gyro, "axis_x"), NULL),
                                        APAD_GYRO_AXIS_YAW, file);
    p->gyro.axis_y = gyro_axis_from_name(json_str(json_obj_get(gyro, "axis_y"), NULL),
                                        APAD_GYRO_AXIS_PITCH, file);
    p->gyro.sensitivity = json_num(json_obj_get(gyro, "sensitivity"), 1.0);
    /* 0.02 (2% of the 600 deg/s full-scale, i.e. 12 deg/s) default kept from
     * the pre-aim substitute mode. Real hardware numbers exist now (see
     * profiles/3ds-gyro-aim.jsonc's header comment): resting bias 1.8 deg/s,
     * noise floor +-0.2 deg/s, so 12 deg/s is comfortably above the noise
     * floor but is a GUESS beyond "safe", not a validated value -- flagged,
     * not hidden. */
    p->gyro.deadzone    = json_num(json_obj_get(gyro, "deadzone"), 0.02);
    p->gyro.invert_x    = json_bool(json_obj_get(gyro, "invert_x"), 0);
    p->gyro.invert_y    = json_bool(json_obj_get(gyro, "invert_y"), 0);
}

/* Builds one profile from a parsed JSONC document. Starts from the built-in
 * default so any field the file omits keeps a sane value -- a profile file
 * is a set of overrides, not a from-scratch declaration. Returns 0 (and
 * logs why) if `root` isn't even an object; otherwise always succeeds,
 * because every per-field reader above degrades instead of failing. */
static int build_profile(const json_value *root, apad_profile *out,
                         const char *file, const char *default_name)
{
    const char *name;
    const json_value *match;

    if (root == NULL || root->type != JSON_OBJECT) {
        apad_logf(g_log, APAD_LOG_WARN,
                                "profiles: %s: top-level JSON value is not "
                                "an object, skipping file", file);
        return 0;
    }
    set_builtin_default(out);

    name = json_str(json_obj_get(root, "profile"), default_name);
    (void)snprintf(out->name, sizeof out->name, "%s", name);

    match = json_obj_get(root, "match");
    {
        const char *dev = json_str(json_obj_get(match, "device"), "");
        (void)snprintf(out->match_device, sizeof out->match_device, "%s", dev);
    }

    read_buttons(json_obj_get(root, "buttons"), out, file);
    read_sticks(json_obj_get(root, "sticks"), out, file);
    read_triggers(json_obj_get(root, "triggers"), out);
    read_touch(json_obj_get(root, "touch"), out, file);
    read_gyro(json_obj_get(root, "gyro"), out, file);
    return 1;
}

/* ---- blob loading --------------------------------------------------------*/

/*
 * The library/host seam (docs/DESIGN.md §6.4). Everything on the "read files off
 * disk" side of it -- opendir/readdir, the .jsonc suffix filter, the
 * filename sort, reading a file into memory -- left this file with the
 * split and lives in the host (server/host/linux/main.c). What stayed is
 * "parse this text into an apad_profile", which is the part Windows and
 * Android share verbatim and the part a test wants to drive from a string
 * constant.
 *
 * Load order is therefore the host's to choose and it is load-bearing:
 * apad_profiles_match() takes the FIRST profile whose match.device is a
 * substring of the device name, and an empty match.device matches
 * everything, so a wildcard profile placed first would shadow every
 * specific one behind it. The Linux host sorts by filename, which is what
 * the directory loader here used to do for the same reason
 * ("3ds-default.jsonc" before "generic-default.jsonc").
 */
void apad_profiles_load(const apad_profile_source *sources, size_t count,
                        const apad_log_sink *log)
{
    size_t i;

    g_profile_count = 0;
    g_log = log;
    if (sources == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        const char *label = (sources[i].label != NULL) ? sources[i].label
                                                       : "<memory>";
        json_value *root;
        char err[128];

        if (sources[i].text == NULL) {
            apad_logf(g_log, APAD_LOG_WARN,
                      "profiles: %s: no profile text, skipping", label);
            continue;
        }

        err[0] = '\0';
        root = json_parse(sources[i].text, err, sizeof err);
        if (root == NULL) {
            apad_logf(g_log, APAD_LOG_WARN,
                      "profiles: %s: JSONC parse error: %s -- "
                      "skipping, falling back to defaults for this file",
                      label, err[0] ? err : "unknown error");
            continue;
        }

        if (g_profile_count < APAD_PROFILES_MAX) {
            if (build_profile(root, &g_profiles[g_profile_count], label,
                              (sources[i].name != NULL) ? sources[i].name
                                                        : label)) {
                apad_logf(g_log, APAD_LOG_INFO,
                          "profiles: loaded \"%s\" (%s) match=\"%s\"",
                          g_profiles[g_profile_count].name, label,
                          g_profiles[g_profile_count].match_device);
                g_profile_count++;
            }
        }
        json_free(root);
    }
}

const apad_profile *apad_profiles_match(const char *device_name)
{
    int i;

    if (device_name != NULL) {
        for (i = 0; i < g_profile_count; i++) {
            const char *m = g_profiles[i].match_device;
            if (m[0] == '\0') {
                return &g_profiles[i];   /* wildcard: e.g. generic-default.jsonc */
            }
            if (strstr(device_name, m) != NULL) {
                return &g_profiles[i];
            }
        }
    }
    return apad_profiles_builtin_default();
}

const apad_profile *apad_profiles_find(const char *name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < g_profile_count; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) {
            return &g_profiles[i];
        }
    }
    if (strcmp(apad_profiles_builtin_default()->name, name) == 0) {
        return apad_profiles_builtin_default();
    }
    return NULL;
}

size_t apad_profiles_list(const apad_profile **out, size_t max)
{
    size_t total = (size_t)g_profile_count + 1u;   /* +1: the built-in default */
    size_t i, n = 0;

    if (out == NULL) {
        return total;
    }
    for (i = 0; i < (size_t)g_profile_count && n < max; i++) {
        out[n++] = &g_profiles[i];
    }
    if (n < max) {
        out[n++] = apad_profiles_builtin_default();
    }
    return total;
}
