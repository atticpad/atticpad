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

#include "atticpad/protocol.h"   /* APAD_MAX_SESSIONS, APAD_HAT_* */
#include "backend.h"

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

static void backend_shutdown(void)
{
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        destroy_pad(i);
    }
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
    .health        = backend_health
};
