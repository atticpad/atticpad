/* server/backends/backend.h — the interface every virtual-pad backend
 * implements (docs/DESIGN.md §6.1).
 *
 * Nothing outside server/backends/ may know which backend is active.
 * server/src/main.c and server/src/mapping.c talk only to this struct and
 * to apad_pad_state / apad_feedback below. ViGEmBus was retired in November
 * 2023 and is feature-frozen (docs/DESIGN.md §2.1) -- assume it breaks on some
 * future Windows release. This abstraction is why that is a one-file problem
 * when it happens, not a rewrite.
 *
 * This is server code: ordinary hosted C. malloc, floating point and stdio
 * are all fine here. core/'s no-malloc / no-float / no-stdio constraints do
 * not apply to anything under server/ (docs/CONVENTIONS.md, docs/DESIGN.md §3).
 */
#ifndef ATTICPAD_SERVER_BACKEND_H
#define ATTICPAD_SERVER_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pad types a backend may be asked to create. Only XBOX360 exists today; the
 * enum exists so a future DualShock-shaped pad is not a signature change. */
typedef enum {
    APAD_PAD_XBOX360 = 0
} apad_pad_type;

/*
 * Canonical virtual pad state, backend-independent. This is mapping.c's
 * OUTPUT, already run through deadzone/curve/inversion and already
 * translated from wire (Nintendo-convention) buttons to Xbox convention
 * (docs/PROTOCOL.md §5.4).
 *
 * Convention: matches the wire, +Y UP on both sticks (docs/PROTOCOL.md
 * §5.3), same as XInput. A backend whose native device wants +Y down
 * (evdev) negates at the point of writing, not here — see uinput.c and
 * PROTOCOL.md §5.4. Keeping this struct in wire convention means mapping.c
 * has exactly one axis convention to reason about, and every backend quirk
 * stays local to that backend's .c file.
 */
typedef struct {
    uint16_t buttons;    /* APAD_PADBTN_* bitmask, Xbox-convention          */
    int16_t  lx, ly;     /* left stick,  -32768..32767, +Y up               */
    int16_t  rx, ry;     /* right stick, -32768..32767, +Y up               */
    int16_t  lt, rt;     /* triggers, 0..32767                              */
    uint8_t  hat;        /* HID hat value, apad_hat_lut: 0=N..7=NW, 8=null  */
} apad_pad_state;

/* Xbox-convention buttons the virtual pad exposes: APAD_PADBTN_* now live
 * in core/include/atticpad/protocol.h, because TOUCHMAP puts one of them on
 * the wire and a client has to be able to name it. Still distinct from the
 * wire's Nintendo-convention APAD_BTN_* -- translating between the two is
 * mapping.c's job, never the backend's. */

/* Feedback flowing server -> client (RUMBLE/LED payloads, PROTOCOL.md
 * §6.7/§6.8). Not exercised by this task — no client here advertises
 * APAD_CAP_RUMBLE/APAD_CAP_LED — but the shape belongs in the interface from
 * day one so a real force-feedback backend is an implementation, not a
 * signature change. poll_feedback may always report "nothing new". */
typedef struct {
    uint8_t  have_rumble;
    uint16_t low_freq, high_freq, duration_ms;
    uint8_t  have_led;
    uint8_t  led_player_index, led_r, led_g, led_b;
} apad_feedback;

/*
 * Machine-readable reason a backend is not fully working, docs/DESIGN.md §6.3.
 * Deliberately generic: every backend maps its own failure modes onto this
 * SAME enum, so a host or UI can branch on `state` without ever learning
 * which backend produced it. Add a value here only when it names a genuinely
 * different SITUATION a UI would show differently -- not a genuinely
 * different backend.
 */
typedef enum {
    APAD_BACKEND_HEALTH_OK = 0,              /* nothing to report            */
    APAD_BACKEND_HEALTH_DRIVER_MISSING,      /* driver/device not present    */
    APAD_BACKEND_HEALTH_PERMISSION_DENIED,   /* found, but access refused    */
    APAD_BACKEND_HEALTH_VERSION_MISMATCH,    /* found, wrong/incompatible    */
    APAD_BACKEND_HEALTH_OTHER                /* anything else (OOM, unknown) */
} apad_backend_health_state;

/*
 * One backend's current health, filled by apad_backend::health() below.
 * `message` and `remedy` are non-owning pointers into backend-owned static
 * storage (no ownership transfer, valid until the next health() call on the
 * SAME backend) -- same convention the old string-returning health() used.
 */
typedef struct {
    apad_backend_health_state state;    /* OK when there is nothing wrong  */
    const char *message;   /* human-readable, never NULL, "" when state is
                             * OK                                          */
    const char *remedy;    /* NULL, or a URL/command a HOST can act on
                             * without knowing which backend this is --
                             * "https://.../ViGEmBus/releases" or "sudo
                             * usermod -aG input $USER && re-login"        */
} apad_backend_health;

/*
 * §6.15-§6.19 keyboard/mouse/media support (docs/DESIGN.md follow-on to §6.1). The
 * five hooks below are OPTIONAL -- a backend that leaves them all NULL (every
 * backend that predates this addition, via C99 6.7.8p21's zero-fill on a
 * designated initializer that omits a field) is correctly reported as
 * KBM-incapable and nothing above server/backends/ needs to know why.
 *
 * Same abstraction discipline as the rest of this file: `kbm_caps()` is the
 * generic capability answer a caller asks, exactly the shape health() already
 * uses ("what can you do", never "who are you"). A caller may not branch on
 * which backend produced a given caps bit.
 */

/* Which device create_kbm()/destroy_kbm() is being asked about. Three
 * separate devices, not one: a single evdev node declaring both alphabetic
 * keys and REL_X/REL_Y/BTN_LEFT gets BOTH capabilities from libinput, and an
 * application that grabs "the keyboard" also grabs the pointer. Separate
 * nodes also let a media-remote-only session materialise exactly one device
 * instead of a full keyboard/mouse pair it will never use. */
typedef enum {
    APAD_KBM_DEV_KEYBOARD = 0,
    APAD_KBM_DEV_MOUSE,
    APAD_KBM_DEV_MEDIA
} apad_kbm_device;

/* kbm_caps() bits. Positions are deliberately identical to
 * atticpad/kbm.h's APAD_KBM_FEATURE_* / APAD_KBM_STATUS_* wire bits (0, 1, 2)
 * so a caller can use the low three bits of this return value directly as
 * INPUTCAPS.features -- that is a convenience of the numbering, not a
 * requirement this header enforces; nothing here #includes kbm.h. */
#define APAD_KBM_CAP_KEYBOARD  (1u << 0)
#define APAD_KBM_CAP_MOUSE     (1u << 1)
#define APAD_KBM_CAP_MEDIA     (1u << 2)
/* This backend injects input into the host's input stream rather than
 * creating a real device an application can enumerate -- mirrors §6.19's
 * INPUTCAPS.status SYNTHETIC bit. uinput.c does NOT set this: a uinput
 * device is a real evdev device to everything above it. A future
 * SendInput-shaped Windows backend would. */
#define APAD_KBM_CAP_SYNTHETIC (1u << 3)

/*
 * One event to inject: a key/button/control transition. `code` is
 * interpreted per `device` and the BACKEND translates it -- the caller
 * (server/src/kbm.c) never does:
 *   APAD_KBM_DEV_KEYBOARD  code = a USB HID Usage Page 0x07 usage ID
 *   APAD_KBM_DEV_MOUSE     code = APAD_MOUSEBTN_* (atticpad/kbm.h), 1..5
 *   APAD_KBM_DEV_MEDIA     code = an APAD_MEDIA_* §6.18 index, 1..24
 * `down` is 1 for a press, 0 for a release.
 */
typedef struct {
    uint8_t  device;   /* apad_kbm_device */
    uint16_t code;
    uint8_t  down;
} apad_kbm_event_out;

/* Relative pointer motion + wheel detents for one MOUSE datagram (§6.16),
 * already converted from the wire's wrapping accumulators to a per-datagram
 * delta -- server/src/kbm.c does that arithmetic (apad_seq_diff, §6.21), so
 * a backend never sees an accumulator, only a motion vector. Screen-space
 * convention throughout: +X right, +Y DOWN, same as the wire (§6.16) -- a
 * backend whose native device wants +Y up negates at the point of writing,
 * the same pattern uinput.c already uses for the pad's own ABS_Y (§5.4). */
typedef struct {
    int32_t dx, dy, wheel, hwheel;
} apad_mouse_motion;

typedef struct {
    int  (*init)(void);
    int  (*create_pad)(int slot, apad_pad_type type);
    int  (*update_pad)(int slot, const apad_pad_state *state);
    int  (*poll_feedback)(int slot, apad_feedback *out);
    void (*destroy_pad)(int slot);
    void (*shutdown)(void);
    const char *name;

    /*
     * Optional (may be NULL, and every field in a positional initializer
     * that omits it is legally zero -- both existing backends predate this
     * field). Added for the server UI (docs/DESIGN.md §6.3: "Backend status
     * ('ViGEmBus not installed -- click to install')"), which needs a
     * distinction init()'s bare int cannot make: "not installed" and
     * "installed but the wrong version" and "installed but permission
     * denied" all read as the same -1.
     *
     * `out` must be non-NULL; the hook always writes it in full (never a
     * partial struct) so a caller never needs to pre-zero it. `out->state`
     * is APAD_BACKEND_HEALTH_OK exactly when there is nothing to report --
     * that is the ONE field worth branching on, same rule as
     * apad_pairing_info::open. `out->message` is always a short,
     * human-readable, NUL-terminated static string (no ownership transfer,
     * a caller must copy before the next call): "" when state is OK,
     * otherwise something like "/dev/uinput: permission denied" or
     * "ViGEmBus driver not found". `out->remedy` is NULL when there is
     * nothing a host can DO about it, or a static string a host MAY act on
     * without knowing which backend produced it -- a URL to open
     * ("click to install") or a command/path to show the user ("check your
     * udev rule"). Deliberately callable independently of init(): a backend
     * whose init() has failed (or has not been attempted yet) leaves
     * apad_server_create() returning NULL, so there would otherwise be no
     * server object left to ask. A host may call this directly on its own
     * linked-in `apad_backend` before ever creating a server; the library
     * also exposes it read-only through apad_server_backend_status() once a
     * server exists.
     *
     * Kept backend-neutral on purpose (server-dev agent memory
     * backend-interface-fit-findings, point 1): nothing outside
     * server/backends/ may branch on which backend is active. `state` is a
     * generic enum every backend maps its own situation onto, `message` and
     * `remedy` are plain strings, and a host acts on `state`/`remedy` alone
     * -- never on `name` or on an `if (backend == X)`.
     */
    void (*health)(apad_backend_health *out);

    /*
     * §6.15-§6.19 keyboard/mouse/media, all five OPTIONAL (NULL is legal and
     * means "this backend has none of this" -- exactly what a designated
     * initializer that omits them already produces for every backend
     * predating this addition, C99 6.7.8p21). A caller checks `kbm_caps`
     * for NULL, and each bit of its return value, before ever calling the
     * other four; it is a caller bug to call create_kbm/kbm_events/
     * mouse_motion/destroy_kbm for a facility kbm_caps() did not advertise.
     */

    /* APAD_KBM_CAP_* bitmask of what this backend can do. NULL means 0
     * (nothing) without being called -- a caller must check for NULL first,
     * the same convention `health` already established. */
    uint32_t (*kbm_caps)(void);

    /* Create one of the (up to) three per-session KBM devices, lazily, on
     * first use -- mirrors create_pad's shape and slot numbering. Returns 0
     * on success, nonzero on failure (no separate health-style diagnostic:
     * a caller that wants a reason calls health() itself, same as
     * create_pad). Calling it twice for the same (slot, dev) without an
     * intervening destroy_kbm is a caller bug, exactly like create_pad. */
    int      (*create_kbm)(int slot, apad_kbm_device dev);

    /* Inject `n` key/button/control transitions for `slot`, all belonging
     * to whichever device(s) `ev[].device` names -- create_kbm for that
     * device must already have succeeded. Returns 0 on success, nonzero on
     * failure. A backend that batches injects (uinput.c: one EV_SYN per
     * distinct device touched, at the end of the batch) may reorder within
     * a call but MUST preserve each event's own device's press/release
     * order -- the caller has already reduced the datagram's ring + snapshot
     * to the minimal ordered sequence that reaches the same held state.
     *
     * "Preserve the order for one device" is load-bearing, not a formality:
     * within one keyboard batch the caller emits a report's MODIFIER
     * transitions (HID usages 0xE0-0xE7) before its other keys, so that a
     * Shift arriving in the same report as a letter still applies to that
     * letter -- the order a real USB HID report gets by putting its
     * modifier byte first. A backend that sorted a keyboard batch by code
     * would type the lowercase letter. See server/src/kbm.c's comment on
     * §6.20 step 3. */
    int      (*kbm_events)(int slot, const apad_kbm_event_out *ev, size_t n);

    /* Apply one MOUSE datagram's relative motion for `slot` -- create_kbm
     * for APAD_KBM_DEV_MOUSE must already have succeeded. Returns 0 on
     * success, nonzero on failure. Distinct from kbm_events() because
     * motion is continuous (an accumulator delta) where button transitions
     * are discrete edges -- folding them into one call would force every
     * backend to special-case "this event has no device/code/down". */
    int      (*mouse_motion)(int slot, const apad_mouse_motion *m);

    /* Destroy one of the three per-session KBM devices. A no-op if it was
     * never created (mirrors destroy_pad's idempotence). The CALLER is
     * responsible for releasing everything held (§6.20) with kbm_events()
     * BEFORE calling this -- a backend must not need to invent release
     * events of its own when a device it did not choose to remove goes
     * away. */
    void     (*destroy_kbm)(int slot, apad_kbm_device dev);
} apad_backend;

/* Each backend exposes exactly one such symbol, named apad_backend_<id>.
 * backend.h deliberately does NOT declare any of them -- doing so would
 * mean "add a backend" requires editing this interface, which contradicts
 * the interface's own point. See server/backends/backends.h, which a host
 * includes to get the extern(s) for whichever backend(s) are compiled in
 * for its platform. Nothing here or in backends.h picks a backend; that
 * decision stays where it always was, at the apad_server_create() call in
 * the host's main().
 */

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_BACKEND_H */
