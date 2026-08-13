/* server/backends/vigem.c — Windows virtual-pad backend (docs/DESIGN.md §6.1).
 *
 * Creates one ViGEmBus XUSB target per pad slot. ViGEmBus's x360 target
 * enumerates as a real "Xbox 360 Controller for Windows" and — crucially —
 * registers an actual XInput slot, which is the entire reason this backend
 * exists rather than a user-mode HID one (docs/DESIGN.md §2.1).
 *
 * ViGEmBus was retired and archived in November 2023 and will receive no
 * further updates. Everything Windows-specific is confined to this file, so
 * when it eventually breaks the replacement is a sibling .c, not a rewrite.
 * Nothing outside server/backends/ knows this file exists.
 *
 * The quirks that live here and nowhere else:
 *
 *   - apad_pad_state is +Y UP on both sticks (backend.h, docs/PROTOCOL.md
 *     §5.3). XUSB_REPORT is documented upstream as "XINPUT_GAMEPAD-
 *     compatible", and XINPUT_GAMEPAD's sThumbLY/sThumbRY are +Y up too.
 *     So unlike uinput.c — evdev is +Y DOWN and negates (§5.4) — this
 *     backend copies the sticks through unchanged. See stick_y() below for
 *     what actually establishes that and what does not.
 *   - Triggers are 0..32767 in apad_pad_state and 0..255 in XUSB.
 *   - The D-pad is a HID hat value 0..8 in apad_pad_state and four bits of
 *     XUSB_REPORT::wButtons here.
 *
 * Cross-compiled from Linux with mingw-w64; the Windows test machine has no
 * toolchain at all (docs/DESIGN.md §2.1). Two build traps, both load-bearing:
 *
 *   - The vendored ViGEmClient.cpp includes <Windows.h> and <SetupAPI.h>
 *     with capital letters. mingw-w64 ships them lowercase.
 *     server/backends/vendor/mingw-compat/ fixes exactly that, and must be
 *     first on the include path of the C++ translation unit.
 *   - ViGEm/Client.h uses USHORT, BYTE, UCHAR and LPVOID without including
 *     anything that defines them. A C consumer MUST include <windows.h>
 *     before it or it produces a wall of unknown-type errors that reads like
 *     a broken toolchain. Hence the include order below — do not sort it.
 *   - The vendored headers must come in on -isystem, not -I. ViGEm/Common.h
 *     writes `VOID FORCEINLINE XUSB_REPORT_INIT(...)`, which expands to
 *     `void extern __inline__ ...` and trips -Wold-style-declaration (part
 *     of -Wextra) under -Werror. -isystem silences third-party headers
 *     without weakening a single warning on our own code, and without
 *     patching vendored source.
 *
 * Server code: ordinary hosted C. core/'s no-malloc / no-float / no-stdio
 * constraints do not apply (docs/CONVENTIONS.md, docs/DESIGN.md §3). This file deliberately
 * uses no stdio anyway: a backend has no business writing to the host's
 * console, same as uinput.c.
 */
#include <windows.h>      /* MUST precede ViGEm/Client.h — see above */
#include <xinput.h>       /* only for the static_asserts below              */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <ViGEm/Client.h>

#include "atticpad/protocol.h"   /* APAD_MAX_SESSIONS, APAD_HAT_* */
#include "backend.h"

#define MAX_PADS   ((int)APAD_MAX_SESSIONS)

/*
 * ViGEm describes XUSB_REPORT as "XINPUT_GAMEPAD-compatible" and this file
 * relies on that being literally true — the whole update_pad() path is a
 * field copy justified by it. Prose in a third-party header that will never
 * be updated again is a weak thing to rest on, so check it against the
 * platform's own <xinput.h> at compile time. Nothing runs; if this file
 * compiles, XUSB_REPORT and XINPUT_GAMEPAD still agree, and if they ever
 * stop agreeing the build fails loudly instead of the pad going quietly
 * wrong. Same for the button bits, which this file's tables hard-code.
 */
static_assert(sizeof(XUSB_REPORT) == sizeof(XINPUT_GAMEPAD), "XUSB layout");
static_assert(offsetof(XUSB_REPORT, wButtons)      == offsetof(XINPUT_GAMEPAD, wButtons),      "wButtons");
static_assert(offsetof(XUSB_REPORT, bLeftTrigger)  == offsetof(XINPUT_GAMEPAD, bLeftTrigger),  "bLeftTrigger");
static_assert(offsetof(XUSB_REPORT, bRightTrigger) == offsetof(XINPUT_GAMEPAD, bRightTrigger), "bRightTrigger");
static_assert(offsetof(XUSB_REPORT, sThumbLX)      == offsetof(XINPUT_GAMEPAD, sThumbLX),      "sThumbLX");
static_assert(offsetof(XUSB_REPORT, sThumbLY)      == offsetof(XINPUT_GAMEPAD, sThumbLY),      "sThumbLY");
static_assert(offsetof(XUSB_REPORT, sThumbRX)      == offsetof(XINPUT_GAMEPAD, sThumbRX),      "sThumbRX");
static_assert(offsetof(XUSB_REPORT, sThumbRY)      == offsetof(XINPUT_GAMEPAD, sThumbRY),      "sThumbRY");
static_assert(XUSB_GAMEPAD_A == XINPUT_GAMEPAD_A && XUSB_GAMEPAD_B == XINPUT_GAMEPAD_B &&
              XUSB_GAMEPAD_X == XINPUT_GAMEPAD_X && XUSB_GAMEPAD_Y == XINPUT_GAMEPAD_Y &&
              XUSB_GAMEPAD_DPAD_UP    == XINPUT_GAMEPAD_DPAD_UP &&
              XUSB_GAMEPAD_DPAD_DOWN  == XINPUT_GAMEPAD_DPAD_DOWN &&
              XUSB_GAMEPAD_DPAD_LEFT  == XINPUT_GAMEPAD_DPAD_LEFT &&
              XUSB_GAMEPAD_DPAD_RIGHT == XINPUT_GAMEPAD_DPAD_RIGHT &&
              XUSB_GAMEPAD_START == XINPUT_GAMEPAD_START &&
              XUSB_GAMEPAD_BACK  == XINPUT_GAMEPAD_BACK &&
              XUSB_GAMEPAD_LEFT_THUMB     == XINPUT_GAMEPAD_LEFT_THUMB &&
              XUSB_GAMEPAD_RIGHT_THUMB    == XINPUT_GAMEPAD_RIGHT_THUMB &&
              XUSB_GAMEPAD_LEFT_SHOULDER  == XINPUT_GAMEPAD_LEFT_SHOULDER &&
              XUSB_GAMEPAD_RIGHT_SHOULDER == XINPUT_GAMEPAD_RIGHT_SHOULDER,
              "XUSB button bits diverged from XINPUT_GAMEPAD");
/* XINPUT_GAMEPAD has no GUIDE bit to compare against: 0x0400 is undocumented
 * on Microsoft's side ("bits set but not defined above are reserved") and
 * comes from ViGEm/the XUSB driver, so it is asserted by absence, not by
 * agreement. It is the one button bit here with no second source. */

/*
 * init() return codes.
 *
 * apad_backend::init returns a plain int, so from the host's point of view
 * this is 0-or-not. The distinct negative values exist because docs/DESIGN.md §6.3
 * requires the server UI to say "ViGEmBus not installed — click to install",
 * which is a different situation from "installed but the wrong version" or
 * "installed but the handle would not open". A caller that only tests != 0
 * behaves exactly as it does with uinput.c; a caller that ever grows a way
 * to ask (see the note at the bottom of this file) gets the distinction for
 * free. VIGEM_ERROR values themselves never leave this file — that is the
 * whole point of §6.1.
 */
#define APAD_VIGEM_OK                 0
#define APAD_VIGEM_ERR_ALLOC         (-1)   /* out of memory                 */
#define APAD_VIGEM_ERR_NO_DRIVER     (-2)   /* ViGEmBus not installed        */
#define APAD_VIGEM_ERR_ACCESS        (-3)   /* found, CreateFile failed      */
#define APAD_VIGEM_ERR_VERSION       (-4)   /* incompatible/forked driver    */
#define APAD_VIGEM_ERR_OTHER         (-5)

/* Feedback latch, packed into one LONG so the notification callback (which
 * runs on a thread ViGEmClient creates and detaches) and poll_feedback (which
 * runs on the server thread) can hand data over with InterlockedExchange
 * instead of a lock. A lock would have to be destroyed at some point, and
 * ViGEmClient's notification thread is detached and unjoinable, so there is
 * no moment at which destroying one is provably safe. A single word has no
 * such moment to get wrong.
 *
 * Bit 24 is the "valid" flag, deliberately not bit 31: the whole packed
 * value then stays inside LONG's positive range and there is no signed
 * shift to reason about.
 */
#define FB_LARGE_SHIFT   0
#define FB_SMALL_SHIFT   8
#define FB_LED_SHIFT     16
#define FB_VALID         (1L << 24)

typedef struct {
    PVIGEM_TARGET target;      /* allocated on first create, freed only at
                                * shutdown — see destroy_pad()             */
    int           attached;    /* 1 between create_pad and destroy_pad     */
    volatile LONG fb_latch;    /* see FB_* above                           */
} vigem_pad;

static PVIGEM_CLIENT g_client;
static vigem_pad     g_pads[MAX_PADS];
static VIGEM_ERROR   g_last_error;   /* raw driver error, for a debugger    */

/* Mirrors backend_init()'s own return value so backend_health() (below) can
 * answer without re-running vigem_connect() -- a UI may poll health() at
 * 5 Hz and re-dialing the bus that often would be wasteful at best. -1
 * ("some problem, unspecified") until the first backend_init() call, which
 * every host in this tree makes at startup before doing anything else --
 * see the backend.h doc comment on health() for what a caller gets if it
 * asks before that has happened. */
static int g_last_init_rc = -1;

/* Xbox-convention button bit -> XUSB button bit. Table, not a switch, so
 * update_pad's hot path is a straight scan — same shape as uinput.c.
 * These are already Xbox convention: mapping.c did the wire-Nintendo ->
 * Xbox physical-position translation (docs/PROTOCOL.md §5.4). Do not
 * re-translate anything here. */
static const struct { uint16_t padbtn; uint16_t xusb; } g_btn_map[] = {
    { APAD_PADBTN_A,      XUSB_GAMEPAD_A              },
    { APAD_PADBTN_B,      XUSB_GAMEPAD_B              },
    { APAD_PADBTN_X,      XUSB_GAMEPAD_X              },
    { APAD_PADBTN_Y,      XUSB_GAMEPAD_Y              },
    { APAD_PADBTN_LB,     XUSB_GAMEPAD_LEFT_SHOULDER  },
    { APAD_PADBTN_RB,     XUSB_GAMEPAD_RIGHT_SHOULDER },
    { APAD_PADBTN_BACK,   XUSB_GAMEPAD_BACK           },
    { APAD_PADBTN_START,  XUSB_GAMEPAD_START          },
    { APAD_PADBTN_GUIDE,  XUSB_GAMEPAD_GUIDE          },
    { APAD_PADBTN_LTHUMB, XUSB_GAMEPAD_LEFT_THUMB     },
    { APAD_PADBTN_RTHUMB, XUSB_GAMEPAD_RIGHT_THUMB    },
};
#define BTN_MAP_COUNT (int)(sizeof g_btn_map / sizeof g_btn_map[0])

/* HID hat compass value (apad_hat_lut, protocol.h) -> XUSB D-pad bits.
 * XUSB has no hat axis; the D-pad is four independent bits in wButtons, and
 * the diagonals are simply both bits set. Indexed by the 0..8 value straight
 * out of apad_hat_from_buttons(); index 8 is APAD_HAT_NULL. */
static const uint16_t g_hat_to_xusb[9] = {
    XUSB_GAMEPAD_DPAD_UP,                                 /* 0 N  */
    XUSB_GAMEPAD_DPAD_UP   | XUSB_GAMEPAD_DPAD_RIGHT,     /* 1 NE */
    XUSB_GAMEPAD_DPAD_RIGHT,                              /* 2 E  */
    XUSB_GAMEPAD_DPAD_DOWN | XUSB_GAMEPAD_DPAD_RIGHT,     /* 3 SE */
    XUSB_GAMEPAD_DPAD_DOWN,                               /* 4 S  */
    XUSB_GAMEPAD_DPAD_DOWN | XUSB_GAMEPAD_DPAD_LEFT,      /* 5 SW */
    XUSB_GAMEPAD_DPAD_LEFT,                               /* 6 W  */
    XUSB_GAMEPAD_DPAD_UP   | XUSB_GAMEPAD_DPAD_LEFT,      /* 7 NW */
    0u                                                    /* 8 null */
};

/*
 * The stick-Y question, answered explicitly because getting it wrong is a
 * silent inverted-look bug rather than a crash.
 *
 * apad_pad_state is +Y UP (backend.h, §5.3). XUSB wants +Y up too, so this
 * is the identity — the function exists to be the place that says so, and
 * to be the one line that changes if the first real hardware run disagrees.
 *
 * What actually establishes it, and what does not:
 *
 *   - ViGEm's own headers state NO sign convention anywhere.
 *     include/ViGEm/Common.h declares only
 *         typedef struct _XUSB_REPORT { ... SHORT sThumbLX, sThumbLY; ... }
 *     under the comment "Represents an XINPUT_GAMEPAD-compatible report
 *     structure", and include/ViGEm/km/BusShared.h says nothing about axes
 *     at all. Read on its own, ViGEm does not answer this question.
 *   - Microsoft's XINPUT_GAMEPAD reference does, in the sThumbLX entry:
 *     "Each of the thumbstick axis members is a signed value between -32768
 *     and 32767 ... Negative values signify down or to the left. Positive
 *     values signify up or to the right." That is the same convention
 *     §5.3 says the wire uses, so the copy is straight through.
 *   - The bridge between the two is ViGEm's compatibility claim, which the
 *     static_asserts near the top of this file check for layout but cannot
 *     check for sign. A driver that transposed the sign would still pass
 *     them.
 *
 * So: well-grounded, and still not empirically verified on hardware.
 * Confirm it on the first real run before trusting it (the harness sets the
 * two sticks to opposite corners precisely so a human can).
 *
 * Note there is deliberately no INT16_MIN special case here, unlike
 * uinput.c's negate_y(): nothing is negated, so -32768 passes straight
 * through and the clamp §5.4 requires of a +Y-down backend has no
 * counterpart on this one.
 */
static SHORT stick_y(int16_t v)
{
    return (SHORT)v;
}

/* apad_pad_state triggers are 0..32767; XUSB's are 0..255.
 *
 * Byte-for-byte the same expression as uinput.c's scale_trigger_to_u8(), on
 * purpose: the same wire input must produce the same trigger byte whichever
 * backend is loaded, or a profile tuned on Linux feels different on Windows
 * for no reason a user could ever discover. Round to nearest with ties
 * resolved downward (+16383 rather than +16384 — half of 32767 is 16383.5);
 * the endpoints are exact, 0 -> 0 and 32767 -> 255. */
static BYTE scale_trigger_to_u8(int16_t v)
{
    int32_t scaled = ((int32_t)v * 255 + 16383) / 32767;
    if (scaled < 0)   scaled = 0;
    if (scaled > 255) scaled = 255;
    return (BYTE)scaled;
}

static int slot_ok(int slot)
{
    return slot >= 0 && slot < MAX_PADS;
}

/*
 * Rumble/LED arriving from the driver. Runs on a thread ViGEmClient spawned
 * and detached inside vigem_target_x360_register_notification(); it is NOT
 * the server thread. Everything it touches must be either immutable or
 * interlocked.
 *
 * g_pads is a file-scope array that is never freed, so a callback that fires
 * after destroy_pad() — which is possible, ViGEmClient's own thread has an
 * unavoidable check-then-call race against unregister — writes to live
 * memory and is merely stale, not unsafe. create_pad() clears the latch, so
 * the staleness cannot outlive the slot.
 */
static VOID CALLBACK on_x360_notification(PVIGEM_CLIENT client,
                                          PVIGEM_TARGET target,
                                          UCHAR large, UCHAR small,
                                          UCHAR led, LPVOID user)
{
    int slot = (int)(intptr_t)user;
    LONG packed;

    (void)client;
    (void)target;

    if (!slot_ok(slot)) {
        return;
    }

    packed = FB_VALID
           | ((LONG)large << FB_LARGE_SHIFT)
           | ((LONG)small << FB_SMALL_SHIFT)
           | ((LONG)led   << FB_LED_SHIFT);

    /* Last writer wins. A rumble level is a level, not an event: dropping an
     * intermediate value the server never got round to reading is correct,
     * and a queue would only add latency to the one that matters. */
    (void)InterlockedExchange(&g_pads[slot].fb_latch, packed);
}

static int backend_init(void)
{
    VIGEM_ERROR err;
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        g_pads[i].target = NULL;
        g_pads[i].attached = 0;
        g_pads[i].fb_latch = 0;
    }

    if (g_client != NULL) {
        g_last_init_rc = APAD_VIGEM_OK;
        return APAD_VIGEM_OK;    /* already initialised */
    }

    g_client = vigem_alloc();
    if (g_client == NULL) {
        g_last_error = VIGEM_ERROR_NONE;
        g_last_init_rc = APAD_VIGEM_ERR_ALLOC;
        return APAD_VIGEM_ERR_ALLOC;
    }

    err = vigem_connect(g_client);
    g_last_error = err;
    if (!VIGEM_SUCCESS(err)) {
        vigem_free(g_client);
        g_client = NULL;

        switch (err) {
        case VIGEM_ERROR_BUS_NOT_FOUND:
            /* The common case by far: ViGEmBus is simply not installed.
             * docs/DESIGN.md §6.3 wants the UI to offer the install here. */
            g_last_init_rc = APAD_VIGEM_ERR_NO_DRIVER;
            return APAD_VIGEM_ERR_NO_DRIVER;
        case VIGEM_ERROR_BUS_ACCESS_FAILED:
            g_last_init_rc = APAD_VIGEM_ERR_ACCESS;
            return APAD_VIGEM_ERR_ACCESS;
        case VIGEM_ERROR_BUS_VERSION_MISMATCH:
            /* The bus device exists but rejected IOCTL_VIGEM_CHECK_VERSION.
             * This is the shape the HP Omen forked 2018 ViGEmBus is expected
             * to take (docs/DESIGN.md §2.1's "secondary gotcha") — a device that
             * answers the interface GUID but not this driver's version
             * handshake. Not confirmed against an actual Omen machine. */
            g_last_init_rc = APAD_VIGEM_ERR_VERSION;
            return APAD_VIGEM_ERR_VERSION;
        default:
            g_last_init_rc = APAD_VIGEM_ERR_OTHER;
            return APAD_VIGEM_ERR_OTHER;
        }
    }

    g_last_init_rc = APAD_VIGEM_OK;
    return APAD_VIGEM_OK;
}

static int create_pad(int slot, apad_pad_type type)
{
    VIGEM_ERROR err;

    (void)type;   /* only APAD_PAD_XBOX360 exists today */

    if (!slot_ok(slot) || g_client == NULL) {
        return -1;
    }
    if (g_pads[slot].attached) {
        return -1;   /* already created; caller should destroy first */
    }

    if (g_pads[slot].target == NULL) {
        /* vigem_target_x360_alloc() already sets VID/PID to 045e:028e, the
         * real wired Xbox 360 pad — no vigem_target_set_vid/pid needed. The
         * USB *version* (0x0114, which uinput.c has to set by hand so SDL's
         * GUID-keyed gamecontrollerdb matches) is reported by the driver's
         * own descriptor and is not settable from here. */
        g_pads[slot].target = vigem_target_x360_alloc();
        if (g_pads[slot].target == NULL) {
            return -1;
        }
    }

    err = vigem_target_add(g_client, g_pads[slot].target);
    g_last_error = err;
    if (!VIGEM_SUCCESS(err)) {
        return -1;
    }

    /* Discard anything a previous occupant of this slot left in the latch
     * before the new pad can be polled. */
    (void)InterlockedExchange(&g_pads[slot].fb_latch, 0);

    /* Feedback is best-effort: a pad that cannot report rumble is still a
     * working pad, so a failure here is not a failure of create_pad. */
    err = vigem_target_x360_register_notification(g_client,
                                                  g_pads[slot].target,
                                                  on_x360_notification,
                                                  (LPVOID)(intptr_t)slot);
    if (!VIGEM_SUCCESS(err)) {
        g_last_error = err;
    }

    g_pads[slot].attached = 1;
    return 0;
}

static int update_pad(int slot, const apad_pad_state *state)
{
    XUSB_REPORT report;
    uint16_t buttons = 0u;
    VIGEM_ERROR err;
    int i;

    if (!slot_ok(slot) || state == NULL) {
        return -1;
    }
    if (!g_pads[slot].attached || g_client == NULL) {
        return -1;
    }

    XUSB_REPORT_INIT(&report);

    for (i = 0; i < BTN_MAP_COUNT; i++) {
        if (state->buttons & g_btn_map[i].padbtn) {
            buttons |= g_btn_map[i].xusb;
        }
    }
    /* Out-of-range hat values are treated as centred rather than rejected:
     * a malformed byte should not take the whole pad down mid-session. */
    if (state->hat <= 8u) {
        buttons |= g_hat_to_xusb[state->hat];
    }
    report.wButtons = (USHORT)buttons;

    report.sThumbLX = (SHORT)state->lx;
    report.sThumbLY = stick_y(state->ly);
    report.sThumbRX = (SHORT)state->rx;
    report.sThumbRY = stick_y(state->ry);

    report.bLeftTrigger  = scale_trigger_to_u8(state->lt);
    report.bRightTrigger = scale_trigger_to_u8(state->rt);

    /* Unlike uinput.c, there is no point diffing against the previous state:
     * XUSB is a whole-report protocol, so the driver call is one IOCTL
     * either way and suppressing it would only risk dropping a report the
     * driver wanted. */
    err = vigem_target_x360_update(g_client, g_pads[slot].target, report);
    g_last_error = err;
    return VIGEM_SUCCESS(err) ? 0 : -1;
}

static int poll_feedback(int slot, apad_feedback *out)
{
    LONG packed;

    if (!slot_ok(slot) || out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof *out);

    /* Take-and-clear: whatever was there is now ours, and a second poll
     * with no driver traffic in between correctly reports nothing new. */
    packed = InterlockedExchange(&g_pads[slot].fb_latch, 0);
    if ((packed & FB_VALID) == 0) {
        return 0;
    }

    /* One XUSB output report carries motors and LED together, so a single
     * notification sets both halves of apad_feedback. */
    out->have_rumble = 1u;
    /* XUSB motors are 8-bit, apad_feedback is 16-bit. *257 maps 0..255 onto
     * 0..65535 exactly at both ends (0->0, 255->65535) with no rounding. */
    out->low_freq  = (uint16_t)((((packed >> FB_LARGE_SHIFT) & 0xFF) * 257));
    out->high_freq = (uint16_t)((((packed >> FB_SMALL_SHIFT) & 0xFF) * 257));
    /* XUSB rumble is a level held until the next output report, not a timed
     * effect. There is no duration to report; 0 means "until superseded",
     * which is what the client must implement for it to feel right. */
    out->duration_ms = 0u;

    out->have_led = 1u;
    /* The 360 LED is a player-slot ring, not an RGB lamp: LedNumber is a
     * pattern index, and the leading four patterns are the player numbers.
     * r/g/b stay zero — this backend has no colour to report and inventing
     * one would be a lie a DS4 backend would have to contradict. */
    out->led_player_index = (uint8_t)((packed >> FB_LED_SHIFT) & 0xFF);

    return 0;
}

static void destroy_pad(int slot)
{
    if (!slot_ok(slot)) {
        return;
    }
    if (!g_pads[slot].attached) {
        return;
    }

    /* Order matters, and it is the reverse of create_pad's:
     *
     *   1. unregister — clears the callback pointer so ViGEmClient's
     *      detached notification thread returns instead of calling back
     *      into us;
     *   2. remove — unplugs the child, which is what actually aborts that
     *      thread's pending DeviceIoControl and lets it exit (the cancel
     *      event vigem_target_x360_unregister_notification sets is not
     *      what wakes it).
     *
     * There is deliberately no vigem_target_free() here. ViGEmClient's
     * notification thread dereferences the target after its IOCTL returns,
     * and it is detached and unjoinable, so freeing a target while that
     * thread may still be unwinding is an upstream use-after-free we cannot
     * close from the outside. Keeping one target object per slot alive for
     * the process lifetime removes the hazard from the path that actually
     * runs often — every client disconnect — and lets the slot be reused by
     * the next create_pad() at zero cost. The objects are freed once, in
     * shutdown(). */
    vigem_target_x360_unregister_notification(g_pads[slot].target);
    (void)vigem_target_remove(g_client, g_pads[slot].target);

    g_pads[slot].attached = 0;
    (void)InterlockedExchange(&g_pads[slot].fb_latch, 0);
}

static void backend_shutdown(void)
{
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        destroy_pad(i);
    }

    if (g_client != NULL) {
        /* Closes the bus handle, which also unplugs anything still attached
         * and makes every notification thread's pending IOCTL fail. */
        vigem_disconnect(g_client);
    }

    /* The only place vigem_target_free() is safe-ish, and even here it is a
     * race against ViGEmClient's detached threads unwinding. Give them a
     * moment to notice the bus handle went away before pulling the target
     * objects out from under them. 50 ms, once, on shutdown; the server is
     * on its way out and has nothing better to do. Remove this only
     * alongside an upstream fix that makes those threads joinable. */
    if (g_client != NULL) {
        Sleep(50);
    }

    for (i = 0; i < MAX_PADS; i++) {
        if (g_pads[i].target != NULL) {
            vigem_target_free(g_pads[i].target);
            g_pads[i].target = NULL;
        }
        g_pads[i].fb_latch = 0;
    }

    if (g_client != NULL) {
        vigem_free(g_client);
        g_client = NULL;
    }
}

/* backend.h's health() hook. Deliberately reports g_last_init_rc rather than
 * re-calling vigem_connect() -- see that variable's comment. Before the
 * first backend_init() call (which every host makes at startup) this says
 * "backend not yet initialised" rather than guessing; every host in this
 * tree calls apad_server_create() -> backend->init() before starting
 * whatever serves a UI, so a live server's apad_server_backend_status()
 * never actually observes this branch, but a host calling health() directly
 * on its own linked-in apad_backend_vigem (backend.h's doc comment
 * explicitly allows this, precisely for the case where init() has already
 * failed and there is no apad_server left to ask) can. */
static void backend_health(apad_backend_health *out)
{
    /* This is the URL a "click to install" UI action opens -- kept here,
     * not in the host, because knowing where ViGEmBus's releases live is
     * exactly the kind of backend-specific fact backend.h's health() doc
     * comment says a host must never have to know. */
    static const char kViGEmReleasesUrl[] =
        "https://github.com/nefarius/ViGEmBus/releases";

    switch (g_last_init_rc) {
    case APAD_VIGEM_OK:
        out->state   = APAD_BACKEND_HEALTH_OK;
        out->message = "";
        out->remedy  = NULL;
        return;
    case APAD_VIGEM_ERR_ALLOC:
        out->state   = APAD_BACKEND_HEALTH_OTHER;
        out->message = "vigem_alloc() failed (out of memory)";
        out->remedy  = NULL;
        return;
    case APAD_VIGEM_ERR_NO_DRIVER:
        out->state   = APAD_BACKEND_HEALTH_DRIVER_MISSING;
        out->message = "ViGEmBus not installed";
        out->remedy  = kViGEmReleasesUrl;
        return;
    case APAD_VIGEM_ERR_ACCESS:
        out->state   = APAD_BACKEND_HEALTH_PERMISSION_DENIED;
        out->message = "ViGEmBus found but could not be opened (driver "
                       "access denied)";
        out->remedy  = NULL;
        return;
    case APAD_VIGEM_ERR_VERSION:
        out->state   = APAD_BACKEND_HEALTH_VERSION_MISMATCH;
        out->message = "ViGEmBus found but rejected the version handshake "
                       "(incompatible or forked driver)";
        out->remedy  = kViGEmReleasesUrl;
        return;
    default:
        out->state   = APAD_BACKEND_HEALTH_OTHER;
        out->message = "backend not yet initialised, or ViGEmBus init "
                       "failed for an unrecognised reason";
        out->remedy  = NULL;
        return;
    }
}

const apad_backend apad_backend_vigem = {
    .init          = backend_init,
    .create_pad    = create_pad,
    .update_pad    = update_pad,
    .poll_feedback = poll_feedback,
    .destroy_pad   = destroy_pad,
    .shutdown      = backend_shutdown,
    .name          = "vigem",
    .health        = backend_health
};

/*
 * Note for whoever revisits apad_backend (docs/DESIGN.md §6.1):
 *
 * FIXED (server UI task, 2026-08-10): init() returning a bare int was the one
 * place the interface pinched on Windows. On Linux an init failure is "check
 * your udev rule"; on Windows it is one of four genuinely different
 * situations, one of which (§6.3's "ViGEmBus not installed — click to
 * install") the UI is specifically required to distinguish and offer an
 * action for. backend.h now carries an optional `health()` hook -- exactly
 * the `const char *(*last_error)(void)` shape this note used to ask for --
 * and backend_health() above maps g_last_init_rc onto it. Stayed
 * backend-neutral: apad_server_backend_status() (server/include/apadserver.h)
 * reads backend->name and backend->health() without knowing which backend is
 * behind them, and a host may also call health() directly on its own
 * linked-in apad_backend_vigem/apad_backend_uinput before any apad_server
 * exists at all -- see backend.h's doc comment on why that matters when
 * init() itself is what failed.
 *
 * EXTENDED (backend registry + status task, 2026-08-10): health()'s single
 * string was still not enough for a host to act on without learning which
 * backend produced it -- there was no way to tell "click to install" (open a
 * URL) apart from "check your udev rule" (no action available) except by
 * pattern-matching the message text, which is exactly the
 * `if (backend == vigem)` this interface exists to prevent. health() now
 * fills a generic apad_backend_health{state, message, remedy} (backend.h);
 * `state` is the same five-value enum every backend maps its own situation
 * onto, and `remedy` is a URL or command the HOST may act on without ever
 * knowing it came from ViGEmBus. Also: backend.h no longer hard-codes
 * `extern const apad_backend apad_backend_uinput;` -- see
 * server/backends/backends.h, which both hosts now include instead of
 * redeclaring their own backend's extern locally.
 */
