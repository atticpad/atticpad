/*
 * scripts/support/check_kbm_tables.c — anti-drift guard for
 * server/backends/uinput_keymap.h and server/backends/sendinput_scancodes.h.
 *
 * Both of those headers claim, in their own comments, to be row-for-row
 * mirrors of references/hid/usage-to-evdev.txt and
 * references/hid/usage-to-scancode-set1.txt. A claim like that is only
 * worth anything if something re-checks it on every build -- a table that
 * silently drifts from its own stated provenance is exactly the failure
 * references/hid/ exists to prevent (see its README.md). This program is
 * that re-check.
 *
 * What it asserts, in order:
 *   1. apad_hid_to_evdev[256] matches usage-to-evdev.txt, row for row.
 *   2. apad_hid_to_scancode_set1[256] matches usage-to-scancode-set1.txt,
 *      row for row (make byte + extended flag).
 *   3. Both arrays are exactly 256 entries (APAD_KBD_KEY_BYTES * 8, per
 *      docs/PROTOCOL.md §6.15's 256-bit keys[] bitmap).
 *   4. No two distinct HID usages share a non-zero evdev target, and no two
 *      distinct HID usages share a non-zero scancode make byte, EXCEPT the
 *      documented, hand-verified collision groups below -- anything else is
 *      a build failure, not a warning.
 *   5. Every §6.18 media control index 1..24 has a non-zero
 *      apad_media_to_evdev[] target.
 *   6. apad_media_to_evdev[] matches references/hid/consumer-to-evdev.txt's
 *      '@'-prefixed resolution rows, row for row -- correctness of the
 *      CHOICE, not just presence, the same way §1 checks the keyboard
 *      table against usage-to-evdev.txt.
 *
 * Host-side C. server/'s rules apply here, not core/'s: malloc, stdio and
 * a runtime file read are all fine (see backend.h's own header comment).
 *
 * Usage: check-kbm-tables <repo-root>
 * Invoked by scripts/build.sh's build_server(), same as
 * check_profiles_builtin.sh and stick_shape_test.c.
 */
#define _POSIX_C_SOURCE 200809L /* strtok_r, under -std=c11's __STRICT_ANSI__ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "atticpad/kbm.h"   /* APAD_MEDIA_* -- the guard checks against the
                              * same core constants uinput_keymap.h's table
                              * resolves against, so the two cannot disagree
                              * about what index a name means. */
#include "uinput_keymap.h"
#include "sendinput_scancodes.h"

static int g_failures = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_failures++;
}

/* ------------------------------------------------------------------------
 * Tiny line reader: strips the trailing newline, returns 0 at EOF.
 * ------------------------------------------------------------------------ */
static int read_line(FILE *f, char *buf, size_t buflen)
{
    if (!fgets(buf, (int)buflen, f))
        return 0;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return 1;
}

/* strtok-based tab split. Returns the field, or "" (never NULL) if the file
 * had fewer tab-separated columns than requested -- several
 * usage-to-scancode-set1.txt rows (0x00-0x03, PrintScreen, Pause) do. */
static char *next_field(char **saveptr)
{
    char *tok = strtok_r(NULL, "\t", saveptr);
    return tok ? tok : (char *)"";
}

/* ------------------------------------------------------------------------
 * §1: apad_hid_to_evdev[] vs references/hid/usage-to-evdev.txt
 * ------------------------------------------------------------------------ */
static void check_evdev_table(const char *repo_root)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/references/hid/usage-to-evdev.txt", repo_root);
    FILE *f = fopen(path, "r");
    if (!f) {
        fail("cannot open %s", path);
        return;
    }

    char line[512];
    int rows = 0;
    int seen[256] = { 0 };
    while (read_line(f, line, sizeof(line))) {
        if (line[0] != '0' || line[1] != 'x')
            continue; /* comment or blank line */

        char linecopy[512];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char *save = NULL;
        char *f_usage = strtok_r(linecopy, "\t", &save);
        char *f_name = next_field(&save);
        char *f_code = next_field(&save);
        if (!f_usage || f_code[0] == '\0') {
            fail("usage-to-evdev.txt: unparsable row: \"%s\"", line);
            continue;
        }

        long usage = strtol(f_usage, NULL, 16);
        long code = strtol(f_code, NULL, 10);
        if (usage < 0 || usage > 0xFF) {
            fail("usage-to-evdev.txt: usage 0x%lX out of 0x00-0xFF range", usage);
            continue;
        }
        rows++;
        seen[usage]++;

        uint16_t table_code = apad_hid_to_evdev[usage];
        if ((long)table_code != code) {
            fail("apad_hid_to_evdev[0x%02lX] (%s) = %u, but "
                 "usage-to-evdev.txt says %ld -- table has drifted from its "
                 "own vendored reference",
                 usage, f_name, table_code, code);
        }
    }
    fclose(f);

    if (rows != 256) {
        fail("usage-to-evdev.txt has %d data rows, expected exactly 256 "
             "(0x00-0xFF)", rows);
    }
    for (int i = 0; i < 256; i++) {
        if (seen[i] != 1) {
            fail("usage-to-evdev.txt: usage 0x%02X appears %d times, "
                 "expected exactly once", i, seen[i]);
        }
    }

    if (g_failures == 0)
        printf("  OK: apad_hid_to_evdev[256] matches usage-to-evdev.txt "
               "(256/256 rows)\n");
}

/* ------------------------------------------------------------------------
 * §2: apad_hid_to_scancode_set1[] vs
 * references/hid/usage-to-scancode-set1.txt
 * ------------------------------------------------------------------------ */
static void check_scancode_table(const char *repo_root)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/references/hid/usage-to-scancode-set1.txt", repo_root);
    FILE *f = fopen(path, "r");
    if (!f) {
        fail("cannot open %s", path);
        return;
    }

    char line[512];
    int rows = 0;
    int before_failures = g_failures;
    while (read_line(f, line, sizeof(line))) {
        if (line[0] != '0' || line[1] != 'x')
            continue;

        char linecopy[512];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char *save = NULL;
        char *f_usage = strtok_r(linecopy, "\t", &save);
        char *f_name = next_field(&save);
        char *f_make = next_field(&save);
        /* f_break, f_note not needed for the check -- SendInput only ever
         * needs the make byte plus the extended flag to synthesise a
         * press/release pair, and the guard's job is the make byte + Ext
         * column, not the break byte (which is always make | 0x80 in Set 1
         * and carries no separate information). */
        next_field(&save); /* break, unused */
        char *f_ext = next_field(&save);
        if (!f_usage) {
            fail("usage-to-scancode-set1.txt: unparsable row: \"%s\"", line);
            continue;
        }

        long usage = strtol(f_usage, NULL, 16);
        if (usage < 0 || usage > 0xFF) {
            fail("usage-to-scancode-set1.txt: usage 0x%lX out of range", usage);
            continue;
        }
        rows++;

        uint16_t expected;
        if (f_make[0] == '\0' || strcmp(f_make, "-") == 0) {
            /* Status condition (0x00-0x03) or SPECIAL (PrintScreen/Pause):
             * this table deliberately encodes these as 0 -- see
             * sendinput_scancodes.h's header comment. */
            expected = 0;
        } else {
            long make = strtol(f_make, NULL, 16);
            expected = (uint16_t)make;
            if (strcmp(f_ext, "yes") == 0)
                expected |= APAD_SC_EXT_BIT;
        }

        uint16_t table_val = apad_hid_to_scancode_set1[usage];
        if (table_val != expected) {
            fail("apad_hid_to_scancode_set1[0x%02lX] (%s) = 0x%02X%s, but "
                 "usage-to-scancode-set1.txt says make=0x%02X ext=%s -- "
                 "table has drifted from its own vendored reference",
                 usage, f_name, APAD_SC_MAKE(table_val),
                 APAD_SC_IS_EXT(table_val) ? "+EXT" : "",
                 (unsigned)APAD_SC_MAKE(expected),
                 f_ext[0] ? f_ext : "(none)");
        }
    }
    fclose(f);

    if (rows != 110) {
        fail("usage-to-scancode-set1.txt has %d data rows, expected "
             "exactly 110 (0x00-0x65 and 0xE0-0xE7)", rows);
    }

    if (g_failures == before_failures)
        printf("  OK: apad_hid_to_scancode_set1[256] matches "
               "usage-to-scancode-set1.txt (110/110 rows)\n");
}

/* ------------------------------------------------------------------------
 * §3: bounds
 * ------------------------------------------------------------------------ */
static void check_bounds(void)
{
    size_t evdev_n = sizeof(apad_hid_to_evdev) / sizeof(apad_hid_to_evdev[0]);
    size_t sc_n = sizeof(apad_hid_to_scancode_set1) / sizeof(apad_hid_to_scancode_set1[0]);
    if (evdev_n != 256)
        fail("apad_hid_to_evdev has %zu entries, expected 256 "
             "(APAD_KBD_KEY_BYTES * 8, docs/PROTOCOL.md §6.15)", evdev_n);
    if (sc_n != 256)
        fail("apad_hid_to_scancode_set1 has %zu entries, expected 256", sc_n);
    if (evdev_n == 256 && sc_n == 256)
        printf("  OK: both tables are exactly 256 entries wide\n");
}

/* ------------------------------------------------------------------------
 * §4a: evdev duplicate-target check, with a hand-verified allowlist.
 *
 * Every group below is cited directly from usage-to-evdev.txt's own header
 * comment or is a duplicate visible directly in its rows (see
 * uinput_keymap.h's header comment for the story on each). Anything NOT in
 * this table is an unreviewed collision and fails the build.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint16_t code;
    int count;
    uint8_t usages[4];
} dup_group_t;

static const dup_group_t kEvdevAllowed[] = {
    /* KEY_BACKSLASH: US usage 0x31 vs International Non-US-hash usage 0x32
     * -- mutually exclusive by keyboard layout, not a mapping error. */
    { 43, 2, { 0x31, 0x32 } },
    /* KEY_DELETE: the kernel's hid_keyboard[] array itself reuses this
     * value at three indices (0x4C "Delete Forward", 0x9C, 0xD8). */
    { 111, 3, { 0x4C, 0x9C, 0xD8 } },
    /* KEY_MUTE: dedicated Keyboard-page AC usage (0x7F) and the
     * Consumer-page-shaped alias in the 0xE8-0xFB block (0xEF). */
    { 113, 2, { 0x7F, 0xEF } },
    /* KEY_VOLUMEDOWN: same shape as KEY_MUTE above. */
    { 114, 2, { 0x81, 0xEE } },
    /* KEY_VOLUMEUP: same shape as KEY_MUTE above. */
    { 115, 2, { 0x80, 0xED } },
    /* KEY_STOP: same shape as KEY_MUTE above. */
    { 128, 2, { 0x78, 0xF3 } },
    /* KEY_FIND: same shape as KEY_MUTE above. */
    { 136, 2, { 0x7E, 0xF4 } },
};

static int usage_arrays_equal(const uint8_t *a, int an, const uint8_t *b, int bn)
{
    if (an != bn)
        return 0;
    for (int i = 0; i < an; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void check_evdev_duplicates(void)
{
    /* Build actual (code -> sorted usage list) groups from the shipped
     * table, skipping 0 (no mapping) and 240 (KEY_UNKNOWN, the kernel's own
     * "no evdev equivalent" sentinel -- see uinput_keymap.h). Only codes
     * that actually appear in the table are checked, so this stays correct
     * however wide the evdev KEY_* range grows. */
    int seen_code[65536] = { 0 };
    for (int u = 0; u < 256; u++) {
        seen_code[apad_hid_to_evdev[u]] = 1;
    }

    int before = g_failures;
    for (int code = 1; code <= 65535; code++) {
        if (code == 240 || !seen_code[code])
            continue;
        uint8_t found[256];
        int n = 0;
        for (int u = 0; u < 256; u++) {
            if (apad_hid_to_evdev[u] == code) {
                if (n < 256)
                    found[n] = (uint8_t)u;
                n++;
            }
        }
        if (n <= 1)
            continue;

        const dup_group_t *allowed = NULL;
        for (size_t i = 0; i < sizeof(kEvdevAllowed) / sizeof(kEvdevAllowed[0]); i++) {
            if (kEvdevAllowed[i].code == code) {
                allowed = &kEvdevAllowed[i];
                break;
            }
        }
        if (!allowed) {
            fail("apad_hid_to_evdev: evdev code %d is hit by %d HID usages "
                 "(first two: 0x%02X, 0x%02X) with no allowlist entry -- "
                 "either a real bug or a legitimate collision that needs "
                 "documenting in check_kbm_tables.c's kEvdevAllowed[]",
                 code, n, found[0], n > 1 ? found[1] : 0);
            continue;
        }
        if (!usage_arrays_equal(found, n, allowed->usages, allowed->count)) {
            fail("apad_hid_to_evdev: evdev code %d's duplicate usage set "
                 "changed -- table has %d usages now, allowlist expects %d. "
                 "Update kEvdevAllowed[] deliberately if this is intended, "
                 "do not silence it by widening the count blindly",
                 code, n, allowed->count);
        }
    }
    if (g_failures == before)
        printf("  OK: apad_hid_to_evdev has no unreviewed duplicate targets "
               "(%zu allowlisted groups)\n",
               sizeof(kEvdevAllowed) / sizeof(kEvdevAllowed[0]));
}

/* ------------------------------------------------------------------------
 * §4b: scancode duplicate-MAKE-BYTE check (extended flag intentionally
 * ignored here -- that is exactly what disambiguates the legitimate pairs,
 * per sendinput_scancodes.h's header comment).
 * ------------------------------------------------------------------------ */
static const dup_group_t kScancodeAllowed[] = {
    { 0x1C, 2, { 0x28, 0x58 } }, /* Return / Keypad Enter */
    { 0x1D, 2, { 0xE0, 0xE4 } }, /* Left/Right Control */
    { 0x2B, 2, { 0x31, 0x32 } }, /* US Backslash / Intl Non-US-hash */
    { 0x35, 2, { 0x38, 0x54 } }, /* / and ? / Keypad / */
    { 0x38, 2, { 0xE2, 0xE6 } }, /* Left/Right Alt */
    { 0x47, 2, { 0x4A, 0x5F } }, /* Home / Keypad 7-Home */
    { 0x48, 2, { 0x52, 0x60 } }, /* UpArrow / Keypad 8-Up */
    { 0x49, 2, { 0x4B, 0x61 } }, /* PageUp / Keypad 9-PageUp */
    { 0x4B, 2, { 0x50, 0x5C } }, /* LeftArrow / Keypad 4-Left */
    { 0x4D, 2, { 0x4F, 0x5E } }, /* RightArrow / Keypad 6-Right */
    { 0x4F, 2, { 0x4D, 0x59 } }, /* End / Keypad 1-End */
    { 0x50, 2, { 0x51, 0x5A } }, /* DownArrow / Keypad 2-Down */
    { 0x51, 2, { 0x4E, 0x5B } }, /* PageDown / Keypad 3-PageDn */
    { 0x52, 2, { 0x49, 0x62 } }, /* Insert / Keypad 0-Insert */
    { 0x53, 2, { 0x4C, 0x63 } }, /* Delete Forward / Keypad .-Delete */
};

static void check_scancode_duplicates(void)
{
    int before = g_failures;
    for (int make = 1; make <= 0xFF; make++) {
        uint8_t found[256];
        int n = 0;
        for (int u = 0; u < 256; u++) {
            uint16_t v = apad_hid_to_scancode_set1[u];
            if (v != 0 && APAD_SC_MAKE(v) == make) {
                if (n < 256)
                    found[n] = (uint8_t)u;
                n++;
            }
        }
        if (n <= 1)
            continue;

        const dup_group_t *allowed = NULL;
        for (size_t i = 0; i < sizeof(kScancodeAllowed) / sizeof(kScancodeAllowed[0]); i++) {
            if (kScancodeAllowed[i].code == make) {
                allowed = &kScancodeAllowed[i];
                break;
            }
        }
        if (!allowed) {
            fail("apad_hid_to_scancode_set1: make byte 0x%02X is hit by %d "
                 "HID usages (first two: 0x%02X, 0x%02X) with no allowlist "
                 "entry", make, n, found[0], n > 1 ? found[1] : 0);
            continue;
        }
        if (!usage_arrays_equal(found, n, allowed->usages, allowed->count)) {
            fail("apad_hid_to_scancode_set1: make byte 0x%02X's duplicate "
                 "usage set changed -- table has %d usages now, allowlist "
                 "expects %d", make, n, allowed->count);
        }
    }
    if (g_failures == before)
        printf("  OK: apad_hid_to_scancode_set1 has no unreviewed duplicate "
               "make bytes (%zu allowlisted pairs)\n",
               sizeof(kScancodeAllowed) / sizeof(kScancodeAllowed[0]));
}

/* ------------------------------------------------------------------------
 * §5: every §6.18 media control index 1..24 must have a non-zero target.
 * ------------------------------------------------------------------------ */
static void check_media_coverage(void)
{
    int before = g_failures;
    for (int idx = 1; idx <= (int)APAD_MEDIA_ASSIGNED_MAX; idx++) {
        if (apad_media_to_evdev[idx] == 0) {
            fail("apad_media_to_evdev[%d] is 0 -- every docs/PROTOCOL.md "
                 "§6.18 control index 1..24 must have a target", idx);
        }
    }
    if (APAD_MEDIA_ASSIGNED_MAX != 24u) {
        fail("APAD_MEDIA_ASSIGNED_MAX is %u, but docs/PROTOCOL.md §6.18 "
             "defines indices 1..24", (unsigned)APAD_MEDIA_ASSIGNED_MAX);
    }
    if (g_failures == before)
        printf("  OK: all 24 §6.18 media control indices have a non-zero "
               "apad_media_to_evdev[] target\n");
}

/* ------------------------------------------------------------------------
 * §6: apad_media_to_evdev[] vs references/hid/consumer-to-evdev.txt's
 * machine-readable '@' rows -- correctness, not just presence.
 *
 * consumer-to-evdev.txt is a derived data table from the same pinned kernel
 * tag as usage-to-evdev.txt (hid-input.c's HID_UP_CONSUMER switch, not
 * hid_keyboard[]). Its '@' rows are §6.18 control name -> consumer usage ->
 * KEY_* name -> evdev code, one per §6.18 control that has a Consumer-page
 * equivalent. This maps each control name to the APAD_MEDIA_* index
 * uinput_keymap.h already defines for it and asserts the shipped table's
 * value matches -- the same drift check §1 runs for the keyboard table,
 * now extended to media.
 * ------------------------------------------------------------------------ */
typedef struct {
    const char *name;
    int idx;
} media_name_t;

static const media_name_t kMediaNames[] = {
    { "PLAY_PAUSE", APAD_MEDIA_PLAY_PAUSE },
    { "PLAY", APAD_MEDIA_PLAY },
    { "PAUSE", APAD_MEDIA_PAUSE },
    { "STOP", APAD_MEDIA_STOP },
    { "NEXT_TRACK", APAD_MEDIA_NEXT_TRACK },
    { "PREV_TRACK", APAD_MEDIA_PREV_TRACK },
    { "FAST_FORWARD", APAD_MEDIA_FAST_FORWARD },
    { "REWIND", APAD_MEDIA_REWIND },
    { "VOLUME_UP", APAD_MEDIA_VOLUME_UP },
    { "VOLUME_DOWN", APAD_MEDIA_VOLUME_DOWN },
    { "MUTE", APAD_MEDIA_MUTE },
    { "EJECT", APAD_MEDIA_EJECT },
    { "RECORD", APAD_MEDIA_RECORD },
    { "BRIGHTNESS_UP", APAD_MEDIA_BRIGHTNESS_UP },
    { "BRIGHTNESS_DOWN", APAD_MEDIA_BRIGHTNESS_DOWN },
    { "LAUNCH_BROWSER", APAD_MEDIA_LAUNCH_BROWSER },
    { "LAUNCH_MAIL", APAD_MEDIA_LAUNCH_MAIL },
    { "LAUNCH_CALC", APAD_MEDIA_LAUNCH_CALC },
    { "SEARCH", APAD_MEDIA_SEARCH },
    { "NAV_HOME", APAD_MEDIA_NAV_HOME },
    { "NAV_BACK", APAD_MEDIA_NAV_BACK },
    { "NAV_FORWARD", APAD_MEDIA_NAV_FORWARD },
    { "REFRESH", APAD_MEDIA_REFRESH },
    { "BOOKMARKS", APAD_MEDIA_BOOKMARKS },
};

static void check_media_table(const char *repo_root)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/references/hid/consumer-to-evdev.txt", repo_root);
    FILE *f = fopen(path, "r");
    if (!f) {
        fail("cannot open %s", path);
        return;
    }

    char line[512];
    int rows = 0;
    int before = g_failures;
    int seen[25] = { 0 }; /* index 1..24 used */
    while (read_line(f, line, sizeof(line))) {
        if (line[0] != '@')
            continue; /* comment, blank, or a raw 0x.. usage row */

        char linecopy[512];
        strncpy(linecopy, line + 1, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char *save = NULL;
        char *f_name = strtok_r(linecopy, "\t", &save);
        next_field(&save); /* consumer usage hex, informational only */
        char *f_key = next_field(&save);
        char *f_code = next_field(&save);
        if (!f_name || f_code[0] == '\0') {
            fail("consumer-to-evdev.txt: unparsable '@' row: \"%s\"", line);
            continue;
        }

        const media_name_t *m = NULL;
        for (size_t i = 0; i < sizeof(kMediaNames) / sizeof(kMediaNames[0]); i++) {
            if (strcmp(kMediaNames[i].name, f_name) == 0) {
                m = &kMediaNames[i];
                break;
            }
        }
        if (!m) {
            fail("consumer-to-evdev.txt: '@%s' does not match any "
                 "APAD_MEDIA_* control name known to check_kbm_tables.c",
                 f_name);
            continue;
        }
        rows++;
        if (m->idx >= 0 && m->idx <= 24)
            seen[m->idx]++;

        long code = strtol(f_code, NULL, 10);
        uint16_t table_code = apad_media_to_evdev[m->idx];
        if ((long)table_code != code) {
            fail("apad_media_to_evdev[APAD_MEDIA_%s] = %u, but "
                 "consumer-to-evdev.txt says %s -> %ld -- table has drifted "
                 "from its own vendored reference",
                 f_name, table_code, f_key, code);
        }
    }
    fclose(f);

    if (rows != 24) {
        fail("consumer-to-evdev.txt has %d '@' rows, expected exactly 24 "
             "(one per docs/PROTOCOL.md §6.18 control)", rows);
    }
    for (int i = 1; i <= 24; i++) {
        if (seen[i] != 1) {
            fail("consumer-to-evdev.txt: §6.18 control index %d appears %d "
                 "times in '@' rows, expected exactly once", i, seen[i]);
        }
    }

    if (g_failures == before)
        printf("  OK: apad_media_to_evdev[] matches consumer-to-evdev.txt's "
               "vendored Consumer-page resolution (24/24 controls)\n");
}

int main(int argc, char **argv)
{
    const char *repo_root = (argc > 1) ? argv[1] : ".";

    printf("== check_kbm_tables ==\n");
    check_evdev_table(repo_root);
    check_scancode_table(repo_root);
    check_bounds();
    check_evdev_duplicates();
    check_scancode_duplicates();
    check_media_coverage();
    check_media_table(repo_root);

    if (g_failures > 0) {
        fprintf(stderr, "== check_kbm_tables: FAIL (%d problem%s) ==\n",
                g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    printf("== check_kbm_tables: OK -- uinput_keymap.h and "
           "sendinput_scancodes.h match references/hid/ exactly ==\n");
    return 0;
}
