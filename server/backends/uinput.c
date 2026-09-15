/* server/backends/uinput.c — Linux virtual-pad backend (docs/DESIGN.md §6.1).
 *
 * Creates one /dev/uinput device per pad slot, enumerating as
 * "Microsoft X-Box 360 pad" (045e:028e, BUS_USB) so evtest and every game
 * treat it exactly like real hardware. Uses the modern UI_DEV_SETUP /
 * UI_ABS_SETUP ioctls, not the legacy uinput_user_dev write() path.
 *
 * The one quirk that lives here and nowhere else (docs/PROTOCOL.md §5.4):
 * evdev sticks are +Y DOWN. apad_pad_state arrives +Y UP (matching the wire
 * and XInput — see backend.h). This file negates ABS_Y and ABS_RY at the
 * point of writing, clamping -32768 (which has no positive counterpart in
 * int16) to 32767. Nowhere else in the server needs to know that.
 *
 * Server-only, hosted C. malloc/stdio/floating point all fine here.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/uinput.h>

#include "atticpad/kbm.h"        /* APAD_MOUSEBTN_* */
#include "atticpad/protocol.h"   /* APAD_MAX_SESSIONS, APAD_HAT_* */
#include "backend.h"
#include "uinput_keymap.h"       /* apad_hid_to_evdev[], apad_media_to_evdev[] --
                                   * itself #includes atticpad/kbm.h for the
                                   * APAD_MEDIA_* vocabulary, so this include
                                   * is order-independent with the one above */

#define UINPUT_PATH   "/dev/uinput"
#define MAX_PADS      ((int)APAD_MAX_SESSIONS)

/* 045e:028e version 0x0114 — the real wired Xbox 360 pad's USB descriptor,
 * matched exactly (not just VID:PID): SDL's gamecontrollerdb.txt and every
 * other consumer that maps by full GUID (bus+vendor+product+VERSION, not
 * just vendor+product) key on this. Getting the version wrong is a silent
 * "falls back to an unmapped generic joystick" bug, not a loud one — worth
 * matching exactly since it costs nothing. Confirmed against evtest during
 * the M0 spike and again in review; a uinput device declared this way is
 * indistinguishable from real hardware. */
#define VID_MICROSOFT   0x045e
#define PID_X360_PAD    0x028e
#define VERSION_X360_PAD 0x0114

/* Real xpad reports LT/RT as 0..255 (8-bit trigger hardware), not the
 * 0..32767 apad_pad_state uses internally (backend.h). Rescaling only
 * happens here, at the point of writing — a consumer that infers the
 * trigger range from this device's declared 045e:028e identity rather than
 * from EVIOCGABS would otherwise read triggers as permanently near-pinned
 * against our wider range. */
static uint8_t scale_trigger_to_u8(int16_t v)
{
    int32_t scaled = ((int32_t)v * 255 + 16383) / 32767;   /* round to nearest */
    if (scaled < 0)   scaled = 0;
    if (scaled > 255) scaled = 255;
    return (uint8_t)scaled;
}

typedef struct {
    int fd;             /* -1 when the slot is not in use */
    uint16_t last_buttons;
    int8_t   last_hat_x, last_hat_y;
} uinput_pad;

static uinput_pad g_pads[MAX_PADS];

/* §6.15-§6.19: up to three MORE nodes per slot, created lazily and
 * independently of the pad and of each other. One node cannot sensibly be
 * both keyboard and mouse -- a device declaring alphabetic keys AND
 * REL_X/REL_Y/BTN_LEFT gets both capabilities from libinput, and an
 * application that grabs "the keyboard" also grabs the pointer. Separate
 * nodes also let a media-remote-only session materialise exactly one
 * device. Identity is BUS_USB with an AtticPad vendor/product -- NOT a
 * masquerade the way the pad's 045e:028e is: the pad only borrows Microsoft's
 * IDs because SDL's gamecontrollerdb.txt keys mappings on that GUID, and no
 * equivalent database exists for keyboards a client would need to match. */
typedef struct {
    int kb_fd, mo_fd, me_fd;   /* -1 when that device does not exist */
} uinput_kbm;

static uinput_kbm g_kbm[MAX_PADS];

#define VID_ATTICPAD          0x1d50   /* openmoko's shared open-source USB
                                        * VID -- the convention hobby/virtual
                                        * devices with no vendor of their own
                                        * use; picked for that convention, not
                                        * for any affiliation with OpenMoko */
#define PID_ATTICPAD_KEYBOARD 0xa1b0
#define PID_ATTICPAD_MOUSE    0xa1b1
#define PID_ATTICPAD_MEDIA    0xa1b2

/* Xbox-convention button bit -> Linux BTN_* code. Table, not a switch, so
 * update_pad's hot loop is a straight scan. */
static const struct { uint16_t padbtn; uint16_t code; } g_btn_map[] = {
    { APAD_PADBTN_A,      BTN_A       },
    { APAD_PADBTN_B,      BTN_B       },
    { APAD_PADBTN_X,      BTN_X       },
    { APAD_PADBTN_Y,      BTN_Y       },
    { APAD_PADBTN_LB,     BTN_TL      },
    { APAD_PADBTN_RB,     BTN_TR      },
    { APAD_PADBTN_BACK,   BTN_SELECT  },
    { APAD_PADBTN_START,  BTN_START   },
    { APAD_PADBTN_GUIDE,  BTN_MODE    },
    { APAD_PADBTN_LTHUMB, BTN_THUMBL  },
    { APAD_PADBTN_RTHUMB, BTN_THUMBR  },
};
#define BTN_MAP_COUNT (int)(sizeof g_btn_map / sizeof g_btn_map[0])

/* HID hat compass value (apad_hat_lut, protocol.h) -> (HAT0X, HAT0Y).
 * Screen-space convention: up is negative Y, matching evdev's own hat axis
 * sense. Index by the 0..8 value straight out of apad_hat_from_buttons(). */
static const int8_t g_hat_x[9] = { 0,  1, 1, 1, 0, -1, -1, -1, 0 };
static const int8_t g_hat_y[9] = { -1, -1, 0, 1, 1,  1,  0, -1, 0 };

/* evdev is +Y down; the wire (and apad_pad_state) is +Y up (§5.4). -32768
 * has no positive counterpart in int16, so it clamps to 32767 rather than
 * overflowing. */
static int16_t negate_y(int16_t v)
{
    if (v == INT16_MIN) {
        return INT16_MAX;
    }
    return (int16_t)(-v);
}

static int slot_ok(int slot)
{
    return slot >= 0 && slot < MAX_PADS;
}

static int backend_init(void)
{
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        g_pads[i].fd = -1;
        g_pads[i].last_buttons = 0u;
        g_pads[i].last_hat_x = 0;
        g_pads[i].last_hat_y = 0;

        g_kbm[i].kb_fd = -1;
        g_kbm[i].mo_fd = -1;
        g_kbm[i].me_fd = -1;
    }
    return 0;
}

static int setup_abs(int fd, uint16_t code, int32_t min, int32_t max)
{
    struct uinput_abs_setup a;

    memset(&a, 0, sizeof a);
    a.code = code;
    a.absinfo.value = 0;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.fuzz = 0;
    /* Deadzone already applied in mapping.c; no evdev-level flat region. */
    a.absinfo.flat = 0;
    a.absinfo.resolution = 0;

    if (ioctl(fd, UI_SET_ABSBIT, code) < 0) {
        return -1;
    }
    if (ioctl(fd, UI_ABS_SETUP, &a) < 0) {
        return -1;
    }
    return 0;
}

static int create_pad(int slot, apad_pad_type type)
{
    int fd;
    int i;
    struct uinput_setup us;
    char name[UINPUT_MAX_NAME_SIZE];

    (void)type;   /* only APAD_PAD_XBOX360 exists today */

    if (!slot_ok(slot)) {
        return -1;
    }
    if (g_pads[slot].fd >= 0) {
        return -1;   /* already created; caller should destroy first */
    }

    fd = open(UINPUT_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
        goto fail;
    }
    for (i = 0; i < BTN_MAP_COUNT; i++) {
        if (ioctl(fd, UI_SET_KEYBIT, g_btn_map[i].code) < 0) {
            goto fail;
        }
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) {
        goto fail;
    }
    if (setup_abs(fd, ABS_X,  -32768, 32767) < 0) goto fail;
    if (setup_abs(fd, ABS_Y,  -32768, 32767) < 0) goto fail;
    if (setup_abs(fd, ABS_RX, -32768, 32767) < 0) goto fail;
    if (setup_abs(fd, ABS_RY, -32768, 32767) < 0) goto fail;
    if (setup_abs(fd, ABS_Z,       0,   255) < 0) goto fail;   /* xpad: 8-bit trigger */
    if (setup_abs(fd, ABS_RZ,      0,   255) < 0) goto fail;   /* xpad: 8-bit trigger */
    if (setup_abs(fd, ABS_HAT0X,  -1,     1) < 0) goto fail;
    if (setup_abs(fd, ABS_HAT0Y,  -1,     1) < 0) goto fail;

    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor  = VID_MICROSOFT;
    us.id.product = PID_X360_PAD;
    us.id.version = VERSION_X360_PAD;
    /* One name per slot: multiple real 360 pads enumerate identically, so
     * this is realistic, not a tell. Slot number only helps a human reading
     * `evtest --query` pick the right /dev/input/eventN. */
    (void)snprintf(name, sizeof name, "Microsoft X-Box 360 pad %d", slot);
    memcpy(us.name, name, sizeof us.name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        goto fail;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        goto fail;
    }

    g_pads[slot].fd = fd;
    g_pads[slot].last_buttons = 0u;
    g_pads[slot].last_hat_x = 0;
    g_pads[slot].last_hat_y = 0;
    return 0;

fail:
    (void)close(fd);
    return -1;
}

static int emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return (write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) ? 0 : -1;
}

static int update_pad(int slot, const apad_pad_state *state)
{
    int fd;
    int i;
    uint16_t changed;
    int8_t hx, hy;

    if (!slot_ok(slot) || state == NULL) {
        return -1;
    }
    fd = g_pads[slot].fd;
    if (fd < 0) {
        return -1;
    }

    changed = (uint16_t)(state->buttons ^ g_pads[slot].last_buttons);
    if (changed != 0u) {
        for (i = 0; i < BTN_MAP_COUNT; i++) {
            if (changed & g_btn_map[i].padbtn) {
                int down = (state->buttons & g_btn_map[i].padbtn) ? 1 : 0;
                if (emit(fd, EV_KEY, g_btn_map[i].code, down) < 0) {
                    return -1;
                }
            }
        }
        g_pads[slot].last_buttons = state->buttons;
    }

    if (emit(fd, EV_ABS, ABS_X,  state->lx) < 0) return -1;
    if (emit(fd, EV_ABS, ABS_Y,  negate_y(state->ly)) < 0) return -1;
    if (emit(fd, EV_ABS, ABS_RX, state->rx) < 0) return -1;
    if (emit(fd, EV_ABS, ABS_RY, negate_y(state->ry)) < 0) return -1;
    if (emit(fd, EV_ABS, ABS_Z,  scale_trigger_to_u8(state->lt)) < 0) return -1;
    if (emit(fd, EV_ABS, ABS_RZ, scale_trigger_to_u8(state->rt)) < 0) return -1;

    hx = (state->hat <= 8u) ? g_hat_x[state->hat] : 0;
    hy = (state->hat <= 8u) ? g_hat_y[state->hat] : 0;
    if (hx != g_pads[slot].last_hat_x) {
        if (emit(fd, EV_ABS, ABS_HAT0X, hx) < 0) return -1;
        g_pads[slot].last_hat_x = hx;
    }
    if (hy != g_pads[slot].last_hat_y) {
        if (emit(fd, EV_ABS, ABS_HAT0Y, hy) < 0) return -1;
        g_pads[slot].last_hat_y = hy;
    }

    return emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int poll_feedback(int slot, apad_feedback *out)
{
    /* uinput force-feedback (EV_FF) is a separate, considerably larger
     * ioctl surface (UI_SET_EVBIT(EV_FF), FF upload/erase via read()) that
     * no client in this task advertises APAD_CAP_RUMBLE/LED for. Report
     * "nothing new" rather than guess at an untested path. */
    if (!slot_ok(slot) || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    return 0;
}

static void destroy_pad(int slot)
{
    if (!slot_ok(slot)) {
        return;
    }
    if (g_pads[slot].fd >= 0) {
        (void)ioctl(g_pads[slot].fd, UI_DEV_DESTROY);
        (void)close(g_pads[slot].fd);
        g_pads[slot].fd = -1;
    }
}

/* Forward-declared: destroy_kbm() is defined further down (with the rest of
 * the §6.15-§6.19 hooks), but backend_shutdown() above create_pad's own
 * teardown needs to reach it too, mirroring destroy_pad's loop. */
static void destroy_kbm(int slot, apad_kbm_device dev);

static void backend_shutdown(void)
{
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        destroy_pad(i);
        destroy_kbm(i, APAD_KBM_DEV_KEYBOARD);
        destroy_kbm(i, APAD_KBM_DEV_MOUSE);
        destroy_kbm(i, APAD_KBM_DEV_MEDIA);
    }
}

/* ======================================================================== */
/* §6.15-§6.19 keyboard/mouse/media (backend.h's five optional hooks)       */
/* ======================================================================== */

static int *kbm_fd_ptr(int slot, apad_kbm_device dev)
{
    if (!slot_ok(slot)) {
        return NULL;
    }
    switch (dev) {
    case APAD_KBM_DEV_KEYBOARD: return &g_kbm[slot].kb_fd;
    case APAD_KBM_DEV_MOUSE:    return &g_kbm[slot].mo_fd;
    case APAD_KBM_DEV_MEDIA:    return &g_kbm[slot].me_fd;
    default:                    return NULL;
    }
}

/* APAD_MOUSEBTN_* (atticpad/kbm.h, 1..5) -> Linux BTN_* code. §6.16's own
 * BACK/FORWARD names map to BTN_SIDE/BTN_EXTRA -- the pair real 5-button
 * mice actually report for "back"/"forward" (X11/libinput's own convention,
 * buttons 8/9), even though evdev separately defines BTN_BACK/BTN_FORWARD
 * codes that see far less real-hardware use. */
static uint16_t mouse_btn_to_evdev(uint16_t btn)
{
    switch (btn) {
    case APAD_MOUSEBTN_LEFT:    return BTN_LEFT;
    case APAD_MOUSEBTN_RIGHT:   return BTN_RIGHT;
    case APAD_MOUSEBTN_MIDDLE:  return BTN_MIDDLE;
    case APAD_MOUSEBTN_BACK:    return BTN_SIDE;
    case APAD_MOUSEBTN_FORWARD: return BTN_EXTRA;
    default:                    return 0u;
    }
}

static uint32_t kbm_caps(void)
{
    /* NOT APAD_KBM_CAP_SYNTHETIC: every device this backend creates is a
     * real evdev node (UI_DEV_CREATE), indistinguishable from physical
     * hardware to anything above it -- see the file header. */
    return (uint32_t)(APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE
                      | APAD_KBM_CAP_MEDIA);
}

static int create_kbm(int slot, apad_kbm_device dev)
{
    int  *fdp = kbm_fd_ptr(slot, dev);
    int   fd;
    int   i;
    struct uinput_setup us;
    char  name[UINPUT_MAX_NAME_SIZE];

    if (fdp == NULL || *fdp >= 0) {
        return -1;   /* bad (slot, dev), or already created */
    }

    fd = open(UINPUT_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB;
    us.id.vendor  = VID_ATTICPAD;

    switch (dev) {
    case APAD_KBM_DEV_KEYBOARD:
        if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
            goto fail;
        }
        /* Every evdev target apad_hid_to_evdev[] names, once each -- 0 (no
         * mapping) and KEY_UNKNOWN (240, the kernel's own "this HID usage
         * has no evdev equivalent" sentinel, see uinput_keymap.h) both mean
         * "not a real key", so neither is declared. Re-declaring the same
         * code for a second HID usage that targets it (uinput_keymap.h's
         * documented duplicates) is harmless -- UI_SET_KEYBIT is
         * idempotent. */
        for (i = 0; i < (int)(sizeof apad_hid_to_evdev
                              / sizeof apad_hid_to_evdev[0]); i++) {
            uint16_t code = apad_hid_to_evdev[i];
            if (code == 0u || code == (uint16_t)KEY_UNKNOWN) {
                continue;
            }
            if (ioctl(fd, UI_SET_KEYBIT, code) < 0) {
                goto fail;
            }
        }
        if (ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_MSCBIT, MSC_SCAN) < 0) {
            goto fail;
        }
        /* EV_REP: kernel autorepeat. Real USB HID keyboards (usbhid)
         * declare this too -- see this file's own build/verification notes
         * for the evtest + text-editor evidence behind keeping it. */
        if (ioctl(fd, UI_SET_EVBIT, EV_REP) < 0) {
            goto fail;
        }
        us.id.product = PID_ATTICPAD_KEYBOARD;
        (void)snprintf(name, sizeof name, "AtticPad Keyboard %d", slot);
        break;

    case APAD_KBM_DEV_MOUSE:
        if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_RELBIT, REL_X) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_RELBIT, REL_WHEEL) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_RELBIT, REL_HWHEEL) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_KEYBIT, BTN_SIDE) < 0) {
            goto fail;
        }
        if (ioctl(fd, UI_SET_KEYBIT, BTN_EXTRA) < 0) {
            goto fail;
        }
        us.id.product = PID_ATTICPAD_MOUSE;
        (void)snprintf(name, sizeof name, "AtticPad Mouse %d", slot);
        break;

    case APAD_KBM_DEV_MEDIA:
        if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
            goto fail;
        }
        for (i = 0; i < (int)(sizeof apad_media_to_evdev
                              / sizeof apad_media_to_evdev[0]); i++) {
            uint16_t code = apad_media_to_evdev[i];
            if (code == 0u) {
                continue;
            }
            if (ioctl(fd, UI_SET_KEYBIT, code) < 0) {
                goto fail;
            }
        }
        us.id.product = PID_ATTICPAD_MEDIA;
        (void)snprintf(name, sizeof name, "AtticPad Media %d", slot);
        break;

    default:
        goto fail;
    }

    memcpy(us.name, name, sizeof us.name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        goto fail;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        goto fail;
    }

    *fdp = fd;
    return 0;

fail:
    (void)close(fd);
    return -1;
}

/*
 * Inject `n` events, all belonging to whichever device(s) `ev[].device`
 * names -- in practice always one device per call, since server/src/kbm.c
 * calls in per accepted datagram and a datagram is one type. An event whose
 * device was never created (fd < 0) is skipped rather than failing the
 * whole batch, mirroring how an unmapped code (0, or KEY_UNKNOWN) is
 * skipped rather than treated as an error -- both are "nothing to inject",
 * not a fault. One SYN_REPORT per distinct device actually touched, at the
 * end of the batch, exactly like update_pad's own single trailing SYN.
 */
static int kbm_events(int slot, const apad_kbm_event_out *ev, size_t n)
{
    size_t i;
    int    touched_kb = 0, touched_mo = 0, touched_me = 0;

    if (!slot_ok(slot) || ev == NULL) {
        return -1;
    }

    for (i = 0; i < n; i++) {
        int      fd;
        uint16_t code;

        switch ((apad_kbm_device)ev[i].device) {
        case APAD_KBM_DEV_KEYBOARD:
            fd = g_kbm[slot].kb_fd;
            if (fd < 0) {
                continue;
            }
            code = (ev[i].code < (sizeof apad_hid_to_evdev
                                  / sizeof apad_hid_to_evdev[0]))
                       ? apad_hid_to_evdev[ev[i].code] : 0u;
            if (code == 0u || code == (uint16_t)KEY_UNKNOWN) {
                continue;
            }
            /* MSC_SCAN before EV_KEY, carrying the raw HID usage -- the
             * same order a real USB HID keyboard's driver reports. */
            if (emit(fd, EV_MSC, MSC_SCAN, ev[i].code) < 0) {
                return -1;
            }
            if (emit(fd, EV_KEY, code, ev[i].down ? 1 : 0) < 0) {
                return -1;
            }
            touched_kb = 1;
            break;

        case APAD_KBM_DEV_MOUSE:
            fd = g_kbm[slot].mo_fd;
            if (fd < 0) {
                continue;
            }
            code = mouse_btn_to_evdev(ev[i].code);
            if (code == 0u) {
                continue;
            }
            if (emit(fd, EV_KEY, code, ev[i].down ? 1 : 0) < 0) {
                return -1;
            }
            touched_mo = 1;
            break;

        case APAD_KBM_DEV_MEDIA:
            fd = g_kbm[slot].me_fd;
            if (fd < 0) {
                continue;
            }
            code = (ev[i].code < (sizeof apad_media_to_evdev
                                  / sizeof apad_media_to_evdev[0]))
                       ? apad_media_to_evdev[ev[i].code] : 0u;
            if (code == 0u) {
                continue;
            }
            if (emit(fd, EV_KEY, code, ev[i].down ? 1 : 0) < 0) {
                return -1;
            }
            touched_me = 1;
            break;

        default:
            break;
        }
    }

    if (touched_kb && emit(g_kbm[slot].kb_fd, EV_SYN, SYN_REPORT, 0) < 0) {
        return -1;
    }
    if (touched_mo && emit(g_kbm[slot].mo_fd, EV_SYN, SYN_REPORT, 0) < 0) {
        return -1;
    }
    if (touched_me && emit(g_kbm[slot].me_fd, EV_SYN, SYN_REPORT, 0) < 0) {
        return -1;
    }
    return 0;
}

/* §6.16: +Y is DOWN here (screen space), and so is evdev's REL_Y -- the
 * OPPOSITE situation from the pad's ABS_Y (§5.4), which needs negate_y().
 * No flip needed: the wire's mouse convention and evdev's already agree. */
static int mouse_motion(int slot, const apad_mouse_motion *m)
{
    int fd;

    if (!slot_ok(slot) || m == NULL) {
        return -1;
    }
    fd = g_kbm[slot].mo_fd;
    if (fd < 0) {
        return -1;
    }
    if (m->dx != 0 && emit(fd, EV_REL, REL_X, m->dx) < 0) {
        return -1;
    }
    if (m->dy != 0 && emit(fd, EV_REL, REL_Y, m->dy) < 0) {
        return -1;
    }
    if (m->wheel != 0 && emit(fd, EV_REL, REL_WHEEL, m->wheel) < 0) {
        return -1;
    }
    if (m->hwheel != 0 && emit(fd, EV_REL, REL_HWHEEL, m->hwheel) < 0) {
        return -1;
    }
    return emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void destroy_kbm(int slot, apad_kbm_device dev)
{
    int *fdp = kbm_fd_ptr(slot, dev);

    if (fdp == NULL || *fdp < 0) {
        return;
    }
    (void)ioctl(*fdp, UI_DEV_DESTROY);
    (void)close(*fdp);
    *fdp = -1;
}

/* backend.h's health() hook. backend_init() above never actually touches
 * /dev/uinput -- it always succeeds -- so this is the one place that does,
 * and it is deliberately a cheap access(2) rather than an open(): a UI may
 * poll this at 5 Hz and opening (and immediately closing) the device node
 * repeatedly would be a needless extra syscall pair per slot for no better
 * an answer, since the real create_pad() open below already reports its
 * own failure into the per-session log when a client actually connects.
 * Static buffer: fine under this interface's single-threaded contract (the
 * struct comment already requires a caller to copy before calling again),
 * and it means no allocation on a path a UI may hit every 200 ms. */
static void backend_health(apad_backend_health *out)
{
    static char msg[160];
    int         saved_errno;

    if (access(UINPUT_PATH, R_OK | W_OK) == 0) {
        out->state   = APAD_BACKEND_HEALTH_OK;
        out->message = "";
        out->remedy  = NULL;
        return;
    }

    saved_errno = errno;
    /* ENOENT means the node itself is absent -- either uinput is not
     * loaded, or this kernel was built without it. Anything else that
     * access(2) can return here (chiefly EACCES) means the node exists and
     * this process is simply not allowed to touch it -- the udev-rule case
     * this backend was written against. */
    out->state = (saved_errno == ENOENT)
                     ? APAD_BACKEND_HEALTH_DRIVER_MISSING
                     : APAD_BACKEND_HEALTH_PERMISSION_DENIED;
    (void)snprintf(msg, sizeof msg, "%s: %s", UINPUT_PATH,
                   strerror(saved_errno));
    out->message = msg;
    out->remedy  = (out->state == APAD_BACKEND_HEALTH_DRIVER_MISSING)
        ? "modprobe uinput"
        : "add a udev rule granting this user rw on " UINPUT_PATH
          " (see server/backends/README or 60-atticpad-uinput.rules), "
          "then re-login";
}

const apad_backend apad_backend_uinput = {
    .init          = backend_init,
    .create_pad    = create_pad,
    .update_pad    = update_pad,
    .poll_feedback = poll_feedback,
    .destroy_pad   = destroy_pad,
    .shutdown      = backend_shutdown,
    .name          = "uinput",
    .health        = backend_health,
    .kbm_caps      = kbm_caps,
    .create_kbm    = create_kbm,
    .kbm_events    = kbm_events,
    .mouse_motion  = mouse_motion,
    .destroy_kbm   = destroy_kbm
};
