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
