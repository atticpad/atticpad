/* server/host/common/profile_json.h -- apad_profile <-> JSON, for the web
 * remapping editor's profile routes (docs/DESIGN.md's editor task, webui.h).
 *
 * Two directions, both pure serialisation -- no filesystem access (that is
 * profile_store.h's job) and no protocol logic (mapping/matching stay in
 * server/src/mapping.c and profiles.c, never reimplemented here):
 *
 *   profile_json_serialize()   apad_profile*  -> JSON  (GET /api/profile/{name},
 *                                                        and the per-entry
 *                                                        shape inside GET
 *                                                        /api/profiles)
 *   profile_json_build_jsonc() parsed request  -> JSONC file text (PUT
 *                                                        /api/profile/{name},
 *                                                        before
 *                                                        profile_store_save())
 *
 * The two are DELIBERATELY the same JSON vocabulary server/src/profiles.c's
 * own parser already reads (build_profile() and its read_*() helpers,
 * profiles.c): "profile", "match":{"device":...}, "buttons", "sticks" (with
 * "left"/"right" sub-objects), "triggers", "touch", "gyro". A GET response
 * is therefore already a valid PUT body -- an editor can round-trip a
 * profile it fetched without any client-side translation layer, and this
 * file does not need (and does not have) a schema distinct from profiles.c's
 * own. Two GET-only annotation fields, "builtin" and "editable", ride along
 * in the SAME object (profile_json_serialize() appends them) and are simply
 * ignored on the way back in (profile_json_build_jsonc() drops them, along
 * with "profile" itself, before re-emitting -- see that function's comment
 * for why "profile" is forced rather than trusted).
 *
 * Header-only, same build-script-constraint reason as every other file in
 * this directory (see webui.h's top comment). Depends on strbuf.h (ui_strbuf,
 * sb_*), jsonc.h (json_value, for the PUT direction) and profiles.h
 * (apad_profile and friends) -- all already linked into every build_server()/
 * build_windows() binary.
 */
#ifndef ATTICPAD_HOST_COMMON_PROFILE_JSON_H
#define ATTICPAD_HOST_COMMON_PROFILE_JSON_H

#include <string.h>

#include "strbuf.h"
#include "jsonc.h"
#include "profiles.h"

/* ---- enum <-> JSON string tables ----------------------------------------
 *
 * Deliberately NOT exported from profiles.c/h: these are the exact inverse
 * of profiles.c's own curve_from_name()/touch_mode_from_name()/
 * gyro_mode_from_name()/gyro_axis_from_name(), which stay private to that
 * file (nothing there needed the reverse direction before this task) and
 * stay UNTOUCHED by this addition -- this is a second, independent set of
 * tiny switch statements, the same "read the enum's spelling back out"
 * pattern webui.h's own backend_health_state_name() already uses for
 * apad_backend_health_state. Both sides must obviously agree on the
 * strings; if profiles.c's vocabulary ever changes these need to change with
 * it, exactly as true of backend_health_state_name() and backend.h's enum.
 */
static const char *profile_json_curve_name(apad_curve_t c)
{
    switch (c) {
    case APAD_CURVE_LINEAR:    return "linear";
    case APAD_CURVE_CUBIC:     return "cubic";
    case APAD_CURVE_QUADRATIC: /* fall through */
    default:                   return "quadratic";
    }
}

static const char *profile_json_touch_mode_name(apad_touch_mode_t m)
{
    switch (m) {
    case APAD_TOUCH_MODE_REGIONS:        return "regions";
    case APAD_TOUCH_MODE_DELTA_STICK:    return "delta_stick";
    case APAD_TOUCH_MODE_ABSOLUTE_STICK: return "absolute_stick";
    case APAD_TOUCH_MODE_NONE:           /* fall through */
    default:                             return "none";
    }
}

static const char *profile_json_gyro_mode_name(apad_gyro_mode_t m)
{
    switch (m) {
    case APAD_GYRO_MODE_SUBSTITUTE: return "substitute";
    case APAD_GYRO_MODE_AIM:        return "aim";
    case APAD_GYRO_MODE_NONE:       /* fall through */
    default:                        return "none";
    }
}

static const char *profile_json_gyro_axis_name(apad_gyro_axis_t a)
{
    switch (a) {
    case APAD_GYRO_AXIS_PITCH: return "pitch";
    case APAD_GYRO_AXIS_ROLL:  return "roll";
    case APAD_GYRO_AXIS_YAW:   /* fall through */
    default:                   return "yaw";
    }
}

/* ---- apad_profile -> JSON ------------------------------------------------ */

static void profile_json_write_stick(ui_strbuf *sb, const apad_stick_profile *st)
{
    sb_appendf(sb, "{\"deadzone\":%g,\"curve\":", st->deadzone);
    sb_json_string(sb, profile_json_curve_name(st->curve));
    sb_appendf(sb, ",\"invert_x\":%s,\"invert_y\":%s}",
              st->invert_x ? "true" : "false", st->invert_y ? "true" : "false");
}

static void profile_json_write_touch_emit(ui_strbuf *sb, const apad_touch_region *r)
{
    if (r->target == APAD_TOUCH_TARGET_LT) {
        sb_append(sb, "\"LT\"");
    } else if (r->target == APAD_TOUCH_TARGET_RT) {
        sb_append(sb, "\"RT\"");
    } else {
        sb_json_string(sb, apad_pad_btn_name(r->pad_bit));
    }
}

/*
 * Serialises `p` into `sb` as one JSON object matching profiles.c's own
 * "buttons"/"sticks"/"triggers"/"touch"/"gyro" vocabulary (this file's top
 * comment), plus two GET-only annotations:
 *
 *   "builtin"  -- `p` IS apad_profiles_builtin_default(): the compiled-in
 *                 fallback, which has no backing file at all (never came
 *                 from server/profiles/, never will) and so cannot be PUT or
 *                 DELETEd through the web API regardless of `editable`.
 *   "editable" -- 0 for `builtin` and for a shipped profile
 *                 (profile_store_is_shipped(), server/host/common/
 *                 profile_store.h); 1 for anything else the operator can
 *                 legitimately PUT/DELETE. A front end greys out the
 *                 edit/delete controls on `editable: false` and offers
 *                 "duplicate & customize" instead (this task's own product
 *                 decision, not enforced here -- this file only reports the
 *                 fact, webui.h's routes are what actually refuse the write).
 *
 * Caller passes both flags in rather than this function computing them,
 * because computing "editable" needs profile_store_is_shipped()
 * (profile_store.h) and this file intentionally has no filesystem
 * dependency of its own -- see this file's top comment on the
 * profile_json.h/profile_store.h split.
 */
static void profile_json_serialize(ui_strbuf *sb, const apad_profile *p,
                                   int builtin, int editable)
{
    int i;

    sb_append(sb, "{\"profile\":");
    sb_json_string(sb, p->name);
    sb_append(sb, ",\"match\":{\"device\":");
    sb_json_string(sb, p->match_device);
    sb_append(sb, "},\"buttons\":{");
    for (i = 0; i < APAD_PROFILE_BTN_COUNT; i++) {
        if (i > 0) {
            sb_append(sb, ",");
        }
        sb_json_string(sb, apad_profile_wire_btn_names[i]);
        sb_append(sb, ":");
        sb_json_string(sb, apad_pad_btn_name(p->btn_pad_bit[i]));
    }
    sb_append(sb, "},\"sticks\":{\"left\":");
    profile_json_write_stick(sb, &p->left);
    sb_append(sb, ",\"right\":");
    profile_json_write_stick(sb, &p->right);
    sb_appendf(sb, "},\"triggers\":{\"deadzone\":%g},", p->trigger.deadzone);

    sb_append(sb, "\"touch\":{\"mode\":");
    sb_json_string(sb, profile_json_touch_mode_name(p->touch.mode));
    sb_appendf(sb, ",\"stick\":\"%s\",\"sensitivity\":%g,\"deadzone\":%g,"
                   "\"invert_x\":%s,\"invert_y\":%s,\"regions\":[",
              p->touch.stick_is_right ? "right" : "left",
              p->touch.sensitivity, p->touch.deadzone,
              p->touch.invert_x ? "true" : "false",
              p->touch.invert_y ? "true" : "false");
    for (i = 0; i < p->touch.region_count; i++) {
        const apad_touch_region *r = &p->touch.regions[i];
        if (i > 0) {
            sb_append(sb, ",");
        }
        sb_appendf(sb, "{\"rect\":[%g,%g,%g,%g],\"emit\":",
                  r->rect[0], r->rect[1], r->rect[2], r->rect[3]);
        profile_json_write_touch_emit(sb, r);
        sb_appendf(sb, ",\"analog\":%s}", r->analog ? "true" : "false");
    }
    sb_append(sb, "]},");

    sb_append(sb, "\"gyro\":{\"mode\":");
    sb_json_string(sb, profile_json_gyro_mode_name(p->gyro.mode));
    sb_appendf(sb, ",\"stick\":\"%s\",\"axis_x\":",
              p->gyro.stick_is_right ? "right" : "left");
    sb_json_string(sb, profile_json_gyro_axis_name(p->gyro.axis_x));
    sb_append(sb, ",\"axis_y\":");
    sb_json_string(sb, profile_json_gyro_axis_name(p->gyro.axis_y));
    sb_appendf(sb, ",\"sensitivity\":%g,\"deadzone\":%g,"
                   "\"invert_x\":%s,\"invert_y\":%s},",
              p->gyro.sensitivity, p->gyro.deadzone,
              p->gyro.invert_x ? "true" : "false",
              p->gyro.invert_y ? "true" : "false");

    sb_appendf(sb, "\"builtin\":%s,\"editable\":%s}",
              builtin ? "true" : "false", editable ? "true" : "false");
}

/* ---- parsed JSON -> JSONC file text -------------------------------------- */

/* Re-emits `v` as plain JSON text (no comments -- this is round-tripping
 * already-parsed data, not authoring one by hand). Whitespace and key order
 * are not preserved: json_value (jsonc.h) does not remember either, the same
 * way any DOM-shaped JSON library loses them, and profiles.c's reader does
 * not care about either -- every key is looked up by name
 * (json_obj_get()), never by position. */
static void profile_json_write_value(ui_strbuf *sb, const json_value *v)
{
    size_t i;

    if (v == NULL) {
        sb_append(sb, "null");
        return;
    }
    switch (v->type) {
    case JSON_NULL:   sb_append(sb, "null");  break;
    case JSON_FALSE:  sb_append(sb, "false"); break;
    case JSON_TRUE:   sb_append(sb, "true");  break;
    case JSON_NUMBER: sb_appendf(sb, "%g", v->num); break;
    case JSON_STRING: sb_json_string(sb, v->str); break;
    case JSON_ARRAY:
        sb_append(sb, "[");
        for (i = 0; i < v->count; i++) {
            if (i > 0) {
                sb_append(sb, ",");
            }
            profile_json_write_value(sb, v->items[i]);
        }
        sb_append(sb, "]");
        break;
    case JSON_OBJECT:
        sb_append(sb, "{");
        for (i = 0; i < v->count; i++) {
            if (i > 0) {
                sb_append(sb, ",");
            }
            sb_json_string(sb, v->keys[i]);
            sb_append(sb, ":");
            profile_json_write_value(sb, v->items[i]);
        }
        sb_append(sb, "}");
        break;
    default:
        sb_append(sb, "null");
        break;
    }
}

/*
 * Builds complete JSONC file TEXT for one profile from `root` (a parsed PUT
 * request body, in the SAME schema profile_json_serialize() emits -- this
 * file's top comment) and `name` (the route's {name} segment). Returns a
 * malloc'd, NUL-terminated string the caller owns (free() it), or NULL on
 * allocation failure.
 *
 * `name` ALWAYS wins over whatever "profile" key `root` carries -- forced
 * here, not merely defaulted, and every other top-level key is copied
 * through verbatim (minus "profile" itself, plus the two GET-only
 * annotations "builtin"/"editable", both dropped rather than written back).
 * This is deliberate, not a missing feature: profile_store.h's own top
 * comment documents that the FILENAME STEM (this `name`) is the key
 * GET/PUT/DELETE /api/profile/{name} all address a profile by, while
 * apad_profiles_find()/apad_profiles_match() key off the loaded
 * apad_profile::name (the JSON "profile" field) -- letting a PUT body's
 * "profile" field silently rename a file's on-disk identity out from under
 * its own filename would reintroduce exactly the mismatch that comment
 * warns about, self-inflicted by this API rather than a hand-edited file.
 * Forcing it here is what keeps every profile this API ever writes
 * trivially addressable by both keys at once.
 *
 * The one-line comment header ("generated by the AtticPad editor") is this
 * task's own product decision (no JSONC comment preservation on round trip,
 * so a file this API touches says plainly that it is now editor-managed).
 */
static char *profile_json_build_jsonc(const json_value *root, const char *name)
{
    ui_strbuf sb;
    size_t i;

    sb_init(&sb);
    sb_append(&sb, "// generated by the AtticPad editor\n{\n  \"profile\": ");
    sb_json_string(&sb, name);
    if (root != NULL && root->type == JSON_OBJECT) {
        for (i = 0; i < root->count; i++) {
            if (strcmp(root->keys[i], "profile") == 0 ||
                strcmp(root->keys[i], "builtin") == 0 ||
                strcmp(root->keys[i], "editable") == 0) {
                continue;
            }
            sb_append(&sb, ",\n  ");
            sb_json_string(&sb, root->keys[i]);
            sb_append(&sb, ": ");
            profile_json_write_value(&sb, root->items[i]);
        }
    }
    sb_append(&sb, "\n}\n");
    return sb_take(&sb);   /* transfers ownership; sb needs no sb_free() now */
}

#endif /* ATTICPAD_HOST_COMMON_PROFILE_JSON_H */
