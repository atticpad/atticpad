/*
 * Guard: a profile may aim a wire BUTTON at an analog trigger
 * ("buttons": { "L": "LT" }), and that source must COMBINE BY MAX with
 * every other source of the same trigger.
 *
 * The gap this closes: before it, LT/RT were reachable only from a real
 * analog axis (client advertised APAD_CAP_TRIGGERS), from a touch region
 * ("emit": "LT"), or from §5.4's ZL/ZR full-pull fallback. A PSP has L and
 * R and nothing else in that corner -- no ZL/ZR, no touchscreen -- so its
 * triggers were unreachable by any route at all.
 *
 * Drives the REAL apad_profiles_load() + apad_mapping_apply(), not a copy
 * of either: the profile text below goes through the same JSONC parser the
 * server reads the server/profiles/ .jsonc files with, and the assertions read the
 * apad_pad_state a backend would be handed.
 *
 * Built and run by scripts/build.sh's server target.
 */
#include <stdio.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "mapping.h"
#include "profiles.h"
#include "backend.h"

/* A PSP-shaped profile: the two shoulder buttons ARE the triggers. */
static const char *k_profile_triggers =
    "{\n"
    "  \"profile\": \"test-btn-triggers\",\n"
    "  \"match\": { \"device\": \"TriggerTest\" },\n"
    "  \"buttons\": { \"L\": \"LT\", \"R\": \"RT\" },\n"
    "  \"triggers\": { \"deadzone\": 0.02 }\n"
    "}\n";

/* An unrecognised target must warn and leave that button alone -- existing
 * behaviour, asserted here so the LT/RT addition above cannot quietly turn
 * a typo into a mapping. */
static const char *k_profile_bogus =
    "{\n"
    "  \"profile\": \"test-bogus-target\",\n"
    "  \"match\": { \"device\": \"BogusTest\" },\n"
    "  \"buttons\": { \"L\": \"L2\" }\n"
    "}\n";

static int g_warnings;

static void log_sink(void *user, apad_log_level level, const char *msg)
{
    (void)user;
    if (level != APAD_LOG_INFO) {
        g_warnings++;
    }
    printf("    log[%d]: %s\n", (int)level, msg);
}

static int g_failures;

static void check_i16(const char *what, int got, int want)
{
    if (got == want) {
        printf("    ok   %-52s = %d\n", what, got);
    } else {
        printf("    FAIL %-52s = %d, expected %d\n", what, got, want);
        g_failures++;
    }
}

static void apply(const apad_profile *p, uint32_t caps, uint32_t buttons,
                  int16_t l2_axis, apad_pad_state *out)
{
    apad_input_state in;
    apad_mapping_state st;

    memset(&in, 0, sizeof in);
    in.buttons = buttons;
    in.axes[APAD_AXIS_L2] = l2_axis;
    in.battery = 255;
    apad_mapping_state_init(&st);
    apad_mapping_apply(&in, caps, p, &st, out);
}

/* The value a raw L2 of 12000 must survive as, computed here rather than
 * asked of the engine: deadzone 0.02, linear curve (mapping.c's
 * apply_trigger_axis), i.e. ((12000/32767 - 0.02) / 0.98) * 32767
 * = 11576.18, truncated toward zero by the int16 conversion. NOT 12000 --
 * the profile's trigger deadzone rescales the live range, which is exactly
 * the shaping a client is forbidden from doing itself (docs/PROTOCOL.md
 * §5.3). What matters for THIS guard is only that it is nonzero and
 * unchanged by a released button; the exact number is pinned so a silent
 * change to the shaping shows up here too. */
#define RAW_L2       12000
#define SHAPED_L2    11576

int main(void)
{
    apad_profile_source srcs[2];
    const apad_profile *p, *bogus, *builtin;
    apad_pad_state out;
    uint32_t caps_no_trig = APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_SHOULDER;
    uint32_t caps_trig    = caps_no_trig | APAD_CAP_TRIGGERS;

    memset(srcs, 0, sizeof srcs);
    srcs[0].label = "test:buttons-to-triggers";
    srcs[0].name  = "test-btn-triggers";
    srcs[0].text  = k_profile_triggers;
    srcs[1].label = "test:bogus-target";
    srcs[1].name  = "test-bogus-target";
    srcs[1].text  = k_profile_bogus;

    printf("== profile buttons -> analog triggers ==\n");
    {
        /* apad_profiles_load() wants the library-internal sink type; build
         * it the same way server.c does, from a plain callback. */
        apad_log_sink sink;
        sink.fn = log_sink;
        sink.user = NULL;
        apad_profiles_load(srcs, 2, &sink);
    }

    p = apad_profiles_match("AtticPad TriggerTest");
    if (p == NULL || strcmp(p->name, "test-btn-triggers") != 0) {
        printf("    FAIL profile did not load/match (got \"%s\")\n",
               (p != NULL) ? p->name : "(null)");
        return 1;
    }
    printf("  profile \"%s\" loaded\n", p->name);

    /* --- 1. no APAD_CAP_TRIGGERS (the PSP case): L is the only route. --- */
    printf("  [1] client WITHOUT APAD_CAP_TRIGGERS\n");
    apply(p, caps_no_trig, 0u, 0, &out);
    check_i16("nothing held: lt", out.lt, 0);
    check_i16("nothing held: rt", out.rt, 0);

    apply(p, caps_no_trig, APAD_BTN_L, 0, &out);
    check_i16("L held: lt", out.lt, APAD_TRIGGER_MAX);
    check_i16("L held: rt", out.rt, 0);
    check_i16("L held: pad buttons (LB must NOT also fire)", (int)out.buttons, 0);

    apply(p, caps_no_trig, APAD_BTN_R, 0, &out);
    check_i16("R held: rt", out.rt, APAD_TRIGGER_MAX);
    check_i16("R held: lt", out.lt, 0);

    apply(p, caps_no_trig, APAD_BTN_L | APAD_BTN_R, 0, &out);
    check_i16("L+R held: lt", out.lt, APAD_TRIGGER_MAX);
    check_i16("L+R held: rt", out.rt, APAD_TRIGGER_MAX);

    /* --- 2. WITH APAD_CAP_TRIGGERS: max wins, both ways round. --------- */
    printf("  [2] client WITH APAD_CAP_TRIGGERS, raw L2 = %d\n", RAW_L2);
    apply(p, caps_trig, 0u, RAW_L2, &out);
    check_i16("analog only, L released: lt", out.lt, SHAPED_L2);

    apply(p, caps_trig, APAD_BTN_L, RAW_L2, &out);
    check_i16("analog + L held: lt (max wins)", out.lt, APAD_TRIGGER_MAX);

    /* The released-button direction is the one an assignment (rather than a
     * max) would break: a real analog pull must not be stamped back to 0
     * just because the button aimed at the same trigger is up. Same input
     * as the first case in this block, restated as the regression it is. */
    apply(p, caps_trig, APAD_BTN_A, RAW_L2, &out);
    check_i16("analog + unrelated button: lt (not clobbered)", out.lt, SHAPED_L2);

    /* --- 3. §5.4's ZL/ZR fallback is untouched by any of this. --------- */
    printf("  [3] §5.4 ZL fallback, same profile, no APAD_CAP_TRIGGERS\n");
    apply(p, caps_no_trig, APAD_BTN_ZL, 0, &out);
    check_i16("ZL held: lt", out.lt, APAD_TRIGGER_MAX);

    /* --- 4. an unrecognised target warns and maps nothing. ------------- */
    printf("  [4] unrecognised target \"L2\"\n");
    bogus = apad_profiles_match("AtticPad BogusTest");
    if (bogus == NULL || strcmp(bogus->name, "test-bogus-target") != 0) {
        printf("    FAIL bogus profile did not load (got \"%s\")\n",
               (bogus != NULL) ? bogus->name : "(null)");
        return 1;
    }
    if (g_warnings == 0) {
        printf("    FAIL no warning was logged for \"L2\"\n");
        g_failures++;
    } else {
        printf("    ok   %d warning(s) logged\n", g_warnings);
    }
    check_i16("bogus target: btn_trigger[L]", (int)bogus->btn_trigger[4],
              (int)APAD_BTN_TRIGGER_NONE);
    /* "leaving unmapped" in the log line means "not changed by this file":
     * the builtin default's own L -> LB survives, which is what the engine
     * then emits. Asserted against the builtin rather than a literal so
     * this does not have to be edited if that default ever changes. */
    builtin = apad_profiles_builtin_default();
    check_i16("bogus target: btn_pad_bit[L] (builtin default kept)",
              (int)bogus->btn_pad_bit[4], (int)builtin->btn_pad_bit[4]);
    apply(bogus, caps_no_trig, APAD_BTN_L, 0, &out);
    check_i16("bogus target, L held: lt", out.lt, 0);
    check_i16("bogus target, L held: pad buttons", (int)out.buttons,
              (int)builtin->btn_pad_bit[4]);

    /* --- 5. the round-trip name the web editor serialises. ------------- */
    printf("  [5] JSON round-trip vocabulary\n");
    if (strcmp(apad_profile_btn_target_name(p, 4), "LT") == 0 &&
        strcmp(apad_profile_btn_target_name(p, 5), "RT") == 0 &&
        strcmp(apad_profile_btn_target_name(p, 0), "B") == 0) {
        printf("    ok   L->\"LT\", R->\"RT\", A->\"B\"\n");
    } else {
        printf("    FAIL target names: L->\"%s\" R->\"%s\" A->\"%s\"\n",
               apad_profile_btn_target_name(p, 4),
               apad_profile_btn_target_name(p, 5),
               apad_profile_btn_target_name(p, 0));
        g_failures++;
    }

    apad_profiles_load(NULL, 0, NULL);   /* leave the table as we found it */

    if (g_failures != 0) {
        printf("== trigger-button guard: %d assertion(s) failed. See "
               "read_buttons() in server/src/profiles.c and the btn_trigger "
               "loop at the end of apad_mapping_apply() ==\n", g_failures);
        return 1;
    }
    printf("== trigger-button guard: all assertions passed ==\n");
    return 0;
}
