/* server/backends/sendinput.c — Windows keyboard/mouse/media backend
 * (docs/DESIGN.md §6.1, docs/PROTOCOL.md §6.15-§6.19), exporting
 * apad_backend_sendinput.
 *
 * This backend does virtual PADS: create_pad/update_pad/poll_feedback/
 * destroy_pad are all NULL (backend.h documents NULL as "this backend has
 * none of this" for the five KBM hooks, and the SAME "leave it NULL" is
 * legal for the five ORIGINAL hooks too — nothing in server/src/server.c
 * calls them on a backend that is never handed to apad_server_create()
 * directly, which is exactly sendinput.c's situation: it is only ever
 * reached through server/backends/win32.c's composite, which forwards pad
 * calls to apad_backend_vigem instead. See win32.c's header for why the
 * split is a composite backend rather than one file doing both jobs.
 *
 * Everything this file injects goes through User32's SendInput(), which has
 * no notion of "device" or "handle" the way uinput.c's UI_DEV_CREATE gives
 * one — injection is global to the desktop session. create_kbm()/
 * destroy_kbm() are therefore no-ops that only validate (slot, dev) and
 * return success; there is no resource to allocate or release. One
 * consequence worth stating plainly, because it is easy to miss reading
 * this file's calls in isolation: if two AtticPad clients are both
 * connected and both driving keyboard input, their keystrokes land in the
 * SAME injected stream — there is no per-slot separation on this backend,
 * unlike uinput.c's per-slot /dev/uinput nodes. That is a real limitation
 * of SendInput, not a bug in this file, and it has not been exercised
 * against a live two-client session because this tree cannot execute a
 * PE32+ binary (see this file's own build-vs-run note and the task report
 * that introduced it).
 *
 * KEYBOARD injects via scancode, deliberately, not virtual-key: `ki.wVk =
 * 0`, `ki.wScan = APAD_SC_MAKE(...)`, `KEYEVENTF_SCANCODE` set, plus
 * `KEYEVENTF_EXTENDEDKEY` exactly where sendinput_scancodes.h's
 * APAD_SC_IS_EXT() says so (docs/PROTOCOL.md §6.15's arrow-cluster-vs-
 * keypad ambiguity — see that header for the full rule). This mirrors
 * uinput.c injecting evdev KEY_* codes rather than pretending to be a
 * layout-aware higher-level API: a game reading raw scancodes (many do, to
 * stay keyboard-layout-independent) sees the same thing a real keyboard
 * would send.
 *
 * MEDIA is the one place this backend CANNOT use scancodes at all.
 * Windows has no PS/2 Scan Code Set 1 assignment for "next track" or
 * "volume up" — those live only as virtual-key codes (VK_MEDIA_*,
 * VK_VOLUME_*, VK_BROWSER_*, VK_LAUNCH_*), and KEYEVENTF_SCANCODE and a
 * meaningful wVk are mutually exclusive within one KEYBDINPUT: setting
 * KEYEVENTF_SCANCODE tells SendInput to derive everything from wScan and
 * ignore wVk. A media control therefore has to leave KEYEVENTF_SCANCODE
 * OFF and set wVk instead — the exact opposite of every keyboard key this
 * file injects. That flag incompatibility is the concrete, backend-level
 * reason docs/PROTOCOL.md §6.17 gave media its own KBM message type
 * (0x23) instead of folding it into §6.15's usage-ID keyboard bitmap: on
 * at least this one target platform, "media key" and "keyboard key" are
 * not even injected through the same code path.
 *
 * PrintScreen and Pause are not injected here at all: usage 0x46 and
 * usage 0x48 both map to 0 (no mapping) in sendinput_scancodes.h, because
 * they are not simple make/break pairs (see that header's own comment).
 * references/hid/README.md's own "what could not be verified" section
 * recommends driving them through VK_SNAPSHOT / VK_PAUSE instead of a raw
 * scancode sequence, and is explicit that the recommendation itself is
 * UNVERIFIED upstream (not checked against current Windows behaviour).
 * This file does not implement that recommendation — adding it would mean
 * asserting confidence this task does not have. If a future change adds
 * it, it belongs here, as its own small case, with the same "unverified"
 * flag carried forward, not quietly folded into the scancode table.
 *
 * Server code: ordinary hosted C. malloc/stdio/floating point all fine
 * here (docs/CONVENTIONS.md, backend.h's own header). This file uses none of them.
 *
 * BUILD STATUS: cross-compiled clean with mingw-w64 (scripts/build.sh
 * windows). NEVER RUN. This tree has no way to execute a PE32+ binary
 * (server/backends/vigem.c's header makes the same disclosure) — this file
 * is, at the time it was written, the single largest unexercised surface
 * in the project: every SendInput() call below, the scancode/VK split, the
 * extended-key bit, the wheel scaling, and XBUTTON1/XBUTTON2 have not been
 * observed doing anything on a real Windows session. See the task report
 * for the exact manual test plan a person on Windows hardware must run.
 */
#include <windows.h>

#include <stddef.h>
#include <stdint.h>

#include "atticpad/kbm.h"   /* APAD_MOUSEBTN_*, APAD_MEDIA_*, APAD_MEDIA_ASSIGNED_MAX */
#include "atticpad/protocol.h"   /* APAD_MAX_SESSIONS */
#include "backend.h"
#include "sendinput_scancodes.h" /* apad_hid_to_scancode_set1[], APAD_SC_MAKE/IS_EXT */

#define MAX_SLOTS ((int)APAD_MAX_SESSIONS)

/* Largest single kbm_events() batch any caller in this tree can hand us
 * today: server/src/kbm.c's own worst case is the keyboard watchdog's
 * full-release sweep, `apad_kbm_event_out evbuf[APAD_KEY_BITMAP_BYTES *
 * 8u]` = 256 (kbm.c). Sized to that exactly rather than to whichever
 * caller happens to be smallest (mouse/media are both well under this),
 * so one constant covers every device this file handles. If a future
 * caller ever hands more than this in one call, build_inputs() below
 * chunks into multiple SendInput() calls rather than silently dropping
 * events -- see its own comment. */
#define MAX_BATCH 256

static int slot_ok(int slot)
{
    return slot >= 0 && slot < MAX_SLOTS;
}

/* ======================================================================== */
/* init / health / shutdown -- no real resource, see the file header        */
/* ======================================================================== */

static int backend_init(void)
{
    /* Nothing to open: SendInput is a User32 export, and user32.dll is
     * always present and already loaded in any process that can create a
     * window -- which this console/tray-icon host does (server/host/
     * windows/main.c). There is no driver, no handle, no version to
     * mismatch. */
    return 0;
}

static void backend_shutdown(void)
{
    /* Nothing was ever allocated (create_kbm() below holds no resource),
     * so there is nothing to release. A real function rather than NULL so
     * win32.c's composite can call it unconditionally, the same way it
     * calls apad_backend_vigem's shutdown() unconditionally. */
}

static void backend_health(apad_backend_health *out)
{
    /* Always OK: this backend has no driver to be missing, no permission
     * to be denied, no version to mismatch -- User32 is part of every
     * Windows session. The one real limitation this backend has (injected
     * input can be ignored by games with exclusive raw-input capture, or
     * by anti-cheat) is not a FAULT to report through health() -- it is a
     * permanent property of the mechanism, which is exactly what
     * APAD_KBM_CAP_SYNTHETIC (kbm_caps() below) exists to carry instead.
     * health() answers "is something wrong right now"; SYNTHETIC answers
     * "what kind of thing is this, always". Conflating the two would mean
     * a client's §6.19 SYNTHETIC warning banner flickering based on
     * whatever health() last reported, which is not what it means. */
    out->state   = APAD_BACKEND_HEALTH_OK;
    out->message = "";
    out->remedy  = NULL;
}

/* ======================================================================== */
/* §6.15-§6.19 keyboard/mouse/media                                         */
/* ======================================================================== */

static uint32_t kbm_caps(void)
{
    return (uint32_t)(APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE
                      | APAD_KBM_CAP_MEDIA | APAD_KBM_CAP_SYNTHETIC);
}

static int create_kbm(int slot, apad_kbm_device dev)
{
    /* No handle to open -- see the file header. Validate the arguments
     * exactly as far as a real backend would (bad slot, bad device enum
     * value), so a caller bug here fails the same way it would against
     * uinput.c, rather than silently "succeeding" for an out-of-range
     * value. */
    if (!slot_ok(slot)) {
        return -1;
    }
    switch (dev) {
    case APAD_KBM_DEV_KEYBOARD:
    case APAD_KBM_DEV_MOUSE:
    case APAD_KBM_DEV_MEDIA:
        return 0;
    default:
        return -1;
    }
}

static void destroy_kbm(int slot, apad_kbm_device dev)
{
    /* No-op: mirrors create_kbm() holding no resource. Idempotent by
     * construction -- there is nothing a second call could double-free. */
    (void)slot;
    (void)dev;
}

/* APAD_MOUSEBTN_* (atticpad/kbm.h, 1..5) -> the (down-flag, up-flag,
 * mouseData) SendInput needs for that button. BACK/FORWARD go through
 * MOUSEEVENTF_XDOWN/XUP with mouseData = XBUTTON1/XBUTTON2 -- the same
 * pair uinput.c's mouse_btn_to_evdev() maps BACK/FORWARD onto (BTN_SIDE/
 * BTN_EXTRA), for the same reason: XBUTTON1/2 are what a real 5-button
 * mouse's "back"/"forward" buttons report on Windows. */
static int mouse_btn_flags(uint16_t btn, int down, DWORD *flags_out,
                            DWORD *data_out)
{
    switch (btn) {
    case APAD_MOUSEBTN_LEFT:
        *flags_out = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        *data_out  = 0;
        return 1;
    case APAD_MOUSEBTN_RIGHT:
        *flags_out = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        *data_out  = 0;
        return 1;
    case APAD_MOUSEBTN_MIDDLE:
        *flags_out = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        *data_out  = 0;
        return 1;
    case APAD_MOUSEBTN_BACK:
        *flags_out = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        *data_out  = XBUTTON1;
        return 1;
    case APAD_MOUSEBTN_FORWARD:
        *flags_out = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        *data_out  = XBUTTON2;
        return 1;
    default:
        return 0;
    }
}

/* §6.18 media control index (1..24, APAD_MEDIA_* in atticpad/kbm.h) ->
 * Windows virtual-key code. UNLIKE apad_hid_to_scancode_set1[] and
 * apad_media_to_evdev[] (uinput_keymap.h), this table is NOT a transcribed
 * vendored reference: Win32's multimedia virtual-key range (VK_BROWSER_*
 * 0xA6-0xAC, VK_VOLUME_* 0xAD-0xAF, VK_MEDIA_* 0xB0-0xB3, VK_LAUNCH_*
 * 0xB4-0xB7) is the complete, fixed set <winuser.h> defines -- there is no
 * separate primary document to mirror the way HID scancodes and evdev
 * codes have one. Confirmed present under this exact spelling in the
 * mingw-w64 <winuser.h> this backend is built against.
 *
 * Windows genuinely has FEWER media virtual-keys than §6.18 has controls.
 * Six controls -- PLAY, PAUSE (as distinct from the PLAY_PAUSE toggle),
 * FAST_FORWARD, REWIND, EJECT, RECORD -- and three more -- BRIGHTNESS_UP,
 * BRIGHTNESS_DOWN, LAUNCH_BROWSER, LAUNCH_CALC -- have NO standard Win32
 * VK_* equivalent at all (brightness and most launch keys are delivered
 * through WM_APPCOMMAND/ACPI on real hardware, which SendInput cannot
 * synthesize). Mapped to 0 (no mapping, skipped) rather than guessed at:
 * VK_LAUNCH_APP1/APP2 exist but are OEM/user-configurable "my computer" /
 * "calculator" style keys with no guaranteed target, and inventing a
 * mapping to a key that might launch the wrong thing is worse than
 * dropping the control. This is a real, permanent gap in this backend's
 * media coverage, not an oversight -- report it as such. */
static const uint16_t apad_media_to_vk[APAD_MEDIA_ASSIGNED_MAX + 1u] = {
    [APAD_MEDIA_PLAY_PAUSE]  = VK_MEDIA_PLAY_PAUSE,
    [APAD_MEDIA_PLAY]        = 0,   /* no distinct Windows VK, see above */
    [APAD_MEDIA_PAUSE]       = 0,   /* no distinct Windows VK, see above */
    [APAD_MEDIA_STOP]        = VK_MEDIA_STOP,
    [APAD_MEDIA_NEXT_TRACK]  = VK_MEDIA_NEXT_TRACK,
    [APAD_MEDIA_PREV_TRACK]  = VK_MEDIA_PREV_TRACK,
    [APAD_MEDIA_FAST_FORWARD]    = 0,   /* no Windows VK */
    [APAD_MEDIA_REWIND]          = 0,   /* no Windows VK */
    [APAD_MEDIA_VOLUME_UP]       = VK_VOLUME_UP,
    [APAD_MEDIA_VOLUME_DOWN]     = VK_VOLUME_DOWN,
    [APAD_MEDIA_MUTE]            = VK_VOLUME_MUTE,
    [APAD_MEDIA_EJECT]           = 0,   /* no Windows VK */
    [APAD_MEDIA_RECORD]          = 0,   /* no Windows VK */
    [APAD_MEDIA_BRIGHTNESS_UP]   = 0,   /* no Windows VK (ACPI/OEM only) */
    [APAD_MEDIA_BRIGHTNESS_DOWN] = 0,   /* no Windows VK (ACPI/OEM only) */
    [APAD_MEDIA_LAUNCH_BROWSER]  = 0,   /* no dedicated Windows VK */
    [APAD_MEDIA_LAUNCH_MAIL]     = VK_LAUNCH_MAIL,
    [APAD_MEDIA_LAUNCH_CALC]     = 0,   /* VK_LAUNCH_APP1/2 unreliable, see above */
    [APAD_MEDIA_SEARCH]          = VK_BROWSER_SEARCH,
    [APAD_MEDIA_NAV_HOME]        = VK_BROWSER_HOME,
    [APAD_MEDIA_NAV_BACK]        = VK_BROWSER_BACK,
    [APAD_MEDIA_NAV_FORWARD]     = VK_BROWSER_FORWARD,
    [APAD_MEDIA_REFRESH]         = VK_BROWSER_REFRESH,
    [APAD_MEDIA_BOOKMARKS]       = VK_BROWSER_FAVORITES,
};

/* Fill one INPUT slot for a keyboard event. Returns 1 if it produced
 * something to send, 0 if the usage has no mapping (skip, not an error --
 * same convention uinput.c's kbm_events() uses for an unmapped code). */
static int fill_keyboard_input(INPUT *in, uint16_t code, int down)
{
    uint16_t packed = (code < 256u) ? apad_hid_to_scancode_set1[code] : 0u;
    uint8_t  make    = APAD_SC_MAKE(packed);

    if (make == 0u) {
        return 0;
    }

    in->type      = INPUT_KEYBOARD;
    in->ki.wVk    = 0;
    in->ki.wScan  = make;
    in->ki.dwFlags = KEYEVENTF_SCANCODE
                    | (down ? 0u : (DWORD)KEYEVENTF_KEYUP)
                    | (APAD_SC_IS_EXT(packed) ? (DWORD)KEYEVENTF_EXTENDEDKEY : 0u);
    in->ki.time        = 0;
    in->ki.dwExtraInfo = 0;
    return 1;
}

static int fill_mouse_button_input(INPUT *in, uint16_t code, int down)
{
    DWORD flags, data;

    if (!mouse_btn_flags(code, down, &flags, &data)) {
        return 0;
    }
    in->type       = INPUT_MOUSE;
    in->mi.dx      = 0;
    in->mi.dy      = 0;
    in->mi.mouseData   = data;
    in->mi.dwFlags     = flags;
    in->mi.time        = 0;
    in->mi.dwExtraInfo = 0;
    return 1;
}

/* Media: wVk set, NO KEYEVENTF_SCANCODE -- see this file's header for why
 * that is not a copy/paste omission. */
static int fill_media_input(INPUT *in, uint16_t code, int down)
{
    uint16_t vk = (code <= APAD_MEDIA_ASSIGNED_MAX) ? apad_media_to_vk[code] : 0u;

    if (vk == 0u) {
        return 0;
    }
    in->type       = INPUT_KEYBOARD;
    in->ki.wVk     = (WORD)vk;
    in->ki.wScan   = 0;
    in->ki.dwFlags = down ? 0u : (DWORD)KEYEVENTF_KEYUP;
    in->ki.time        = 0;
    in->ki.dwExtraInfo = 0;
    return 1;
}

/*
 * Inject `n` key/button/control transitions. Builds an INPUT[] array and
 * calls SendInput() once per batch of up to MAX_BATCH events -- almost
 * always once total, since no caller in this tree hands more than
 * MAX_BATCH in a single call today (see that constant's comment). Chunks
 * rather than truncates if a future caller ever does, so a long batch loses
 * no events, only the single-syscall property.
 *
 * A per-event mapping failure (unmapped usage/code) is skipped, exactly
 * like uinput.c's kbm_events() skips one -- "nothing to inject" is not a
 * fault. A SendInput() call whose return value is short (fewer events
 * injected than requested -- Microsoft's documented signal that another
 * thread's input was already in flight, or a UIPI-blocked target) is
 * reported as failure for the events SendInput did not report as sent;
 * this backend cannot know WHICH of the batch's events were the ones
 * dropped (SendInput gives no per-element result), so it can only report
 * the batch, same coarseness create_kbm/kbm_events already has for a
 * caller.
 */
static int kbm_events(int slot, const apad_kbm_event_out *ev, size_t n)
{
    INPUT  batch[MAX_BATCH];
    size_t offset;
    int    rc = 0;

    if (!slot_ok(slot) || ev == NULL) {
        return -1;
    }

    for (offset = 0; offset < n; ) {
        UINT count = 0;
        size_t i;
        size_t chunk_end = offset + MAX_BATCH;
        if (chunk_end > n) {
            chunk_end = n;
        }

        for (i = offset; i < chunk_end; i++) {
            INPUT in;
            int   ok = 0;

            switch ((apad_kbm_device)ev[i].device) {
            case APAD_KBM_DEV_KEYBOARD:
                ok = fill_keyboard_input(&in, ev[i].code, ev[i].down);
                break;
            case APAD_KBM_DEV_MOUSE:
                ok = fill_mouse_button_input(&in, ev[i].code, ev[i].down);
                break;
            case APAD_KBM_DEV_MEDIA:
                ok = fill_media_input(&in, ev[i].code, ev[i].down);
                break;
            default:
                ok = 0;
                break;
            }
            if (ok) {
                batch[count] = in;
                count++;
            }
        }

        if (count > 0u) {
            UINT sent = SendInput(count, batch, (int)sizeof(INPUT));
            if (sent != count) {
                rc = -1;
            }
        }
        offset = chunk_end;
    }

    return rc;
}

/* §6.16: +Y is DOWN on the wire (screen space) -- MOUSEEVENTF_MOVE's dy is
 * ALSO screen-space +Y down by convention (this is how every Win32 mouse
 * hook and every game reading WM_MOUSEMOVE/raw input already interprets a
 * relative move), so no flip is needed here either, the same "no flip
 * needed" situation uinput.c's own mouse_motion() documents for evdev's
 * REL_Y. Move + wheel + hwheel are batched into ONE SendInput() call when
 * more than one is non-zero, mirroring kbm_events()'s one-call-per-batch
 * shape and uinput.c's single trailing SYN_REPORT. */
static int mouse_motion(int slot, const apad_mouse_motion *m)
{
    INPUT  batch[3];
    UINT   count = 0;

    if (!slot_ok(slot) || m == NULL) {
        return -1;
    }

    if (m->dx != 0 || m->dy != 0) {
        batch[count].type = INPUT_MOUSE;
        batch[count].mi.dx = (LONG)m->dx;
        batch[count].mi.dy = (LONG)m->dy;
        batch[count].mi.mouseData = 0;
        batch[count].mi.dwFlags = MOUSEEVENTF_MOVE;
        batch[count].mi.time = 0;
        batch[count].mi.dwExtraInfo = 0;
        count++;
    }
    if (m->wheel != 0) {
        batch[count].type = INPUT_MOUSE;
        batch[count].mi.dx = 0;
        batch[count].mi.dy = 0;
        batch[count].mi.mouseData = (DWORD)(LONG)(m->wheel * WHEEL_DELTA);
        batch[count].mi.dwFlags = MOUSEEVENTF_WHEEL;
        batch[count].mi.time = 0;
        batch[count].mi.dwExtraInfo = 0;
        count++;
    }
    if (m->hwheel != 0) {
        batch[count].type = INPUT_MOUSE;
        batch[count].mi.dx = 0;
        batch[count].mi.dy = 0;
        batch[count].mi.mouseData = (DWORD)(LONG)(m->hwheel * WHEEL_DELTA);
        batch[count].mi.dwFlags = MOUSEEVENTF_HWHEEL;
        batch[count].mi.time = 0;
        batch[count].mi.dwExtraInfo = 0;
        count++;
    }

    if (count == 0u) {
        return 0;
    }
    return (SendInput(count, batch, (int)sizeof(INPUT)) == count) ? 0 : -1;
}

const apad_backend apad_backend_sendinput = {
    .init          = backend_init,
    .create_pad    = NULL,
    .update_pad    = NULL,
    .poll_feedback = NULL,
    .destroy_pad   = NULL,
    .shutdown      = backend_shutdown,
    .name          = "sendinput",
    .health        = backend_health,
    .kbm_caps      = kbm_caps,
    .create_kbm    = create_kbm,
    .kbm_events    = kbm_events,
    .mouse_motion  = mouse_motion,
    .destroy_kbm   = destroy_kbm
};
