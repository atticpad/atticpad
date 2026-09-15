/*
 * Guard: within ONE accepted KEYBOARD report, the server must emit that
 * report's modifier transitions (HID usages 0xE0-0xE7) BEFORE its other key
 * transitions.
 *
 * The bug this closes, found on real 3DS hardware (2026-09-15): the
 * on-screen KEYS mode has a sticky Shift latch, so tapping SHIFT and then a
 * letter puts LEFTSHIFT and the letter into the SAME keys[] bitmap. The
 * §6.20 step-3 reconcile walked the bitmap in ascending usage order, so the
 * batch came out as [A down, LEFTSHIFT down]; a batch is flushed under one
 * evdev SYN_REPORT (one SendInput call on Windows) and consumers apply it in
 * order, so the host typed a LOWERCASE letter. A physically held modifier
 * always worked because it lands in an earlier report. A real USB HID
 * keyboard report carries its modifier byte before its key array, which is
 * exactly the order this reproduces.
 *
 * Drives the REAL apad_kbm_apply_keyboard() -- not a copy of it -- through a
 * fake backend whose kbm_events() records the exact sequence the backend
 * would inject. Built and run by scripts/build.sh's server target.
 */
#include <stdio.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "backend.h"
#include "kbm.h"

/* ---- fake backend: record the batch, in order ------------------------- */

#define MAX_REC 64

typedef struct {
    uint8_t  device;
    uint16_t code;
    uint8_t  down;
} rec_ev;

static rec_ev  g_rec[MAX_REC];
static size_t  g_rec_n;
static unsigned g_batches;      /* how many kbm_events() calls happened */

static int fake_kbm_events(int slot, const apad_kbm_event_out *ev, size_t n)
{
    size_t i;

    (void)slot;
    g_batches++;
    for (i = 0; i < n && g_rec_n < MAX_REC; i++) {
        g_rec[g_rec_n].device = ev[i].device;
        g_rec[g_rec_n].code   = ev[i].code;
        g_rec[g_rec_n].down   = ev[i].down;
        g_rec_n++;
    }
    return 0;
}

static uint32_t fake_kbm_caps(void)
{
    return APAD_KBM_CAP_KEYBOARD;
}

/* Designated initializers: every other field is legally zero-filled (C99
 * 6.7.8p21), which is also how a real backend declares the hooks it does
 * not implement. */
static const apad_backend k_fake = {
    .name       = "fake-recorder",
    .kbm_caps   = fake_kbm_caps,
    .kbm_events = fake_kbm_events
};

/* ---- assertions -------------------------------------------------------- */

static int g_failures;

static const char *usage_name(uint16_t u)
{
    switch (u) {
    case APAD_HID_KEY_A:         return "A";
    case APAD_HID_KEY_B:         return "B";
    case APAD_HID_KEY_LEFTCTRL:  return "LEFTCTRL";
    case APAD_HID_KEY_LEFTSHIFT: return "LEFTSHIFT";
    case APAD_HID_KEY_RIGHTGUI:  return "RIGHTGUI";
    default:                     return "?";
    }
}

/* `want` is a flat list of (usage, down) pairs, terminated by usage 0. */
static void check_seq(const char *what, const uint16_t *want, size_t want_n)
{
    size_t i;
    int    ok = (want_n == g_rec_n);

    for (i = 0; ok && i < want_n; i++) {
        if (g_rec[i].code != want[i * 2] ||
            g_rec[i].down != (uint8_t)want[i * 2 + 1] ||
            g_rec[i].device != (uint8_t)APAD_KBM_DEV_KEYBOARD) {
            ok = 0;
        }
    }
    printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    printf("         got ");
    for (i = 0; i < g_rec_n; i++) {
        printf("%s%s %s", (i > 0) ? ", " : "", usage_name(g_rec[i].code),
               g_rec[i].down ? "down" : "up");
    }
    if (g_rec_n == 0) {
        printf("(nothing)");
    }
    printf("\n");
    if (!ok) {
        printf("         want ");
        for (i = 0; i < want_n; i++) {
            printf("%s%s %s", (i > 0) ? ", " : "", usage_name(want[i * 2]),
                   want[i * 2 + 1] ? "down" : "up");
        }
        if (want_n == 0) {
            printf("(nothing)");
        }
        printf("\n");
        g_failures++;
    }
}

/* ---- driving one report ------------------------------------------------ */

static apad_kbm_state g_st;
static uint16_t       g_seq;

static void key_set(apad_keyboard *kb, uint16_t usage)
{
    kb->keys[APAD_KEY_BYTE(usage)] =
        (uint8_t)(kb->keys[APAD_KEY_BYTE(usage)] | APAD_KEY_MASK(usage));
}

/*
 * Apply one report whose keys[] holds exactly `held` (usage list, 0
 * terminated) and whose ring is empty. event_seq advances by one each time,
 * so there is never a gap: this is the ordinary no-loss path, which is what
 * the 3DS hits and what this guard is about.
 */
static void apply_held(const uint16_t *held)
{
    apad_keyboard kb;
    unsigned      lost = 0u;
    size_t        i;

    memset(&kb, 0, sizeof kb);
    for (i = 0; held[i] != 0u; i++) {
        key_set(&kb, held[i]);
    }
    kb.event_seq = ++g_seq;

    g_rec_n = 0;
    g_batches = 0;
    apad_kbm_apply_keyboard(&k_fake, 0, &g_st, &kb, 1000u, &lost);
    if (lost != 0u) {
        printf("    FAIL unexpected out_lost = %u\n", lost);
        g_failures++;
    }
}

int main(void)
{
    static const uint16_t none[]      = { 0 };
    static const uint16_t shift_a[]   = { APAD_HID_KEY_A,
                                          APAD_HID_KEY_LEFTSHIFT, 0 };
    static const uint16_t b_only[]    = { APAD_HID_KEY_B, 0 };
    static const uint16_t chord3[]    = { APAD_HID_KEY_A,
                                          APAD_HID_KEY_LEFTSHIFT,
                                          APAD_HID_KEY_LEFTCTRL, 0 };

    apad_kbm_state_init(&g_st);
    apad_kbm_note_created(&g_st, APAD_KBM_DEV_KEYBOARD);

    printf("== KEYBOARD report ordering: modifiers before keys ==\n");

    /* [0] baseline: an empty first report emits nothing (§6.20 step 1 --
     * shadow already all-zero, so the reconcile has no work). It also
     * establishes kb_have_baseline so every later report takes the ring
     * path. */
    printf("  [0] baseline (empty report)\n");
    apply_held(none);
    check_seq("empty baseline emits nothing", NULL, 0);

    /* [1] THE BUG. A and LEFTSHIFT appear in the same report. Ascending
     * usage order would be [A down, LEFTSHIFT down] -- a lowercase letter
     * on the host. */
    printf("  [1] A + LEFTSHIFT in ONE report\n");
    {
        static const uint16_t want[] = { APAD_HID_KEY_LEFTSHIFT, 1,
                                         APAD_HID_KEY_A, 1 };
        apply_held(shift_a);
        check_seq("LEFTSHIFT down precedes A down", want, 2);
        if (g_batches != 1u) {
            printf("    FAIL expected ONE kbm_events() batch, got %u\n",
                   g_batches);
            g_failures++;
        } else {
            printf("    ok   one batch (one SYN_REPORT / one SendInput)\n");
        }
    }

    /* [2] both cleared in one report. The implementation applies the same
     * modifier-first rule on RELEASE, because it mirrors the HID report's
     * field order rather than special-casing direction -- asserted here as
     * the order it actually produces, not as an independent requirement.
     * Releasing Shift before the letter is harmless: the letter's own
     * release carries no character. */
    printf("  [2] both cleared in ONE report\n");
    {
        static const uint16_t want[] = { APAD_HID_KEY_LEFTSHIFT, 0,
                                         APAD_HID_KEY_A, 0 };
        apply_held(none);
        check_seq("LEFTSHIFT up precedes A up", want, 2);
    }

    /* [3] a release and a press in the same report: hold Shift+A again,
     * then a report with Shift cleared and B newly set. The modifier's
     * RELEASE must still lead, so B is typed unshifted. */
    printf("  [3] LEFTSHIFT released and B pressed in ONE report\n");
    apply_held(shift_a);
    g_rec_n = 0;
    {
        static const uint16_t want[] = { APAD_HID_KEY_LEFTSHIFT, 0,
                                         APAD_HID_KEY_A, 0,
                                         APAD_HID_KEY_B, 1 };
        apply_held(b_only);
        check_seq("LEFTSHIFT up precedes A up and B down", want, 3);
    }

    /* [4] two modifiers plus a letter. Both modifiers lead, and within the
     * modifier group usage order still holds (LEFTCTRL 0xE0 before
     * LEFTSHIFT 0xE1). */
    printf("  [4] LEFTCTRL + LEFTSHIFT + A in ONE report\n");
    apply_held(none);          /* release B, left over from [3] */
    {
        static const uint16_t want[] = { APAD_HID_KEY_LEFTCTRL, 1,
                                         APAD_HID_KEY_LEFTSHIFT, 1,
                                         APAD_HID_KEY_A, 1 };
        apply_held(chord3);
        check_seq("LEFTCTRL, then LEFTSHIFT, then A", want, 3);
    }

    /* [5] REGRESSION: the ring-replay half of §6.20 must NOT be reordered.
     * Those slots are a client-ordered sequence of EDGES -- [A down, A up]
     * is a tap, and sorting it would turn it into nonsense. Construct a gap
     * by advancing event_seq past last_applied: g = 2 replays the two
     * NEWEST slots (the ring is right-aligned, §6.20 -- newest at D-1), in
     * ascending slot order, BEFORE the reconcile runs. */
    printf("  [5] regression: ring replay keeps its own order\n");
    apply_held(none);          /* clear everything, re-sync */
    {
        apad_keyboard kb;
        unsigned      lost = 0u;
        static const uint16_t want[] = { APAD_HID_KEY_A, 1,   /* replay  */
                                         APAD_HID_KEY_A, 0,   /* replay  */
                                         APAD_HID_KEY_LEFTSHIFT, 1,
                                         APAD_HID_KEY_B, 1 }; /* reconcile */

        memset(&kb, 0, sizeof kb);
        /* Ring: a complete A tap, oldest-first in the two newest slots. */
        kb.events[APAD_KEYBOARD_RING_DEPTH - 2].usage = APAD_HID_KEY_A;
        kb.events[APAD_KEYBOARD_RING_DEPTH - 2].flags = APAD_KBM_EVENT_DOWN;
        kb.events[APAD_KEYBOARD_RING_DEPTH - 1].usage = APAD_HID_KEY_A;
        kb.events[APAD_KEYBOARD_RING_DEPTH - 1].flags = 0u;
        /* Snapshot: Shift+B, neither of which the ring mentions -- so the
         * reconcile has real work to do after the replay, and its own
         * modifier-first rule is visible in the same batch. */
        key_set(&kb, APAD_HID_KEY_LEFTSHIFT);
        key_set(&kb, APAD_HID_KEY_B);
        g_seq = (uint16_t)(g_seq + 2u);
        kb.event_seq = g_seq;

        g_rec_n = 0;
        g_batches = 0;
        apad_kbm_apply_keyboard(&k_fake, 0, &g_st, &kb, 1000u, &lost);
        check_seq("replayed A down/A up keep order, then modifiers, then keys",
                  want, 4);
        if (lost != 0u) {
            printf("    FAIL out_lost = %u for a gap inside the ring\n", lost);
            g_failures++;
        }
    }

    printf("== %s (%d failure%s) ==\n", (g_failures == 0) ? "PASS" : "FAIL",
           g_failures, (g_failures == 1) ? "" : "s");
    return (g_failures == 0) ? 0 : 1;
}
