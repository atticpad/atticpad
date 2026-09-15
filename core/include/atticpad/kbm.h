/* atticpad/kbm.h — keyboard, mouse and media (docs/PROTOCOL.md §6.15–§6.19).
 *
 * The in-memory representation of the four §6.15–§6.19 messages, plus the
 * vocabulary a client needs to DRAW them: mouse button names, the §6.18 media
 * control index, the §6.19 feature and status bits, and a curated set of HID
 * usage names. §6.13's rule, restated: a wire field whose constants live only
 * in server code cannot be interpreted by the side receiving it, so the
 * vocabulary belongs to the protocol.
 *
 * These structs are NEVER cast onto a packet buffer. codec.c moves every
 * field individually, and the in-memory layout here deliberately does not
 * carry the wire's reserved bytes: `MEDIA.events` sits at in-memory offset 6
 * and at wire offset 8, which is exactly the sort of difference a cast would
 * turn into silent corruption on the DS ARM9. Each struct comment gives the
 * WIRE size (§6.15–§6.19); nothing depends on sizeof.
 *
 * Separate from input.h on purpose: input.h is "the canonical input state
 * (§5)", the fixed superset every client fills every frame. These four are
 * optional facilities, gated on an INPUTCAPS (§6.19) that many sessions never
 * see.
 *
 * C99. No malloc, no float, no stdio. Safe to include on a 67 MHz ARM9.
 */
#ifndef ATTICPAD_KBM_H
#define ATTICPAD_KBM_H

#include <stdint.h>
#include "atticpad/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- §6.20 event ring depths ------------------------------------------- *
 *
 * The ring is RIGHT-ALIGNED (§6.20): slot i carries the event with ordinal
 * `event_seq - (DEPTH - 1) + i`, so the NEWEST event is always at the last
 * slot and slots predating the session's first event are transmitted zeroed.
 * Front-packing is explicitly non-conformant — gap replay depends on knowing
 * where the newest event is without counting.
 *
 * At §6.20's 10 Hz repeat floor, 8 slots cover an 800 ms gap.
 */
#define APAD_KEYBOARD_RING_DEPTH  8u   /* §6.15 events[8] */
#define APAD_MOUSE_RING_DEPTH     4u   /* §6.16 events[4] */
#define APAD_MEDIA_RING_DEPTH     4u   /* §6.17 events[4] */

/* §6.15 — the held-key bitmap is 256 bits, one per HID usage. */
#define APAD_KEY_BITMAP_BYTES    32u

/* §6.20 — the held-repeat floor a SENDER must meet, and the watchdog a
 * RECEIVER measures against it. 1000 ms is ten missed repeats, so a
 * conforming sender never trips it. Both are obligations on the caller: core
 * has no clock (time comes through the shim), so nothing here enforces them. */
#define APAD_KBM_REPEAT_HZ       10u   /* §6.20: MUST, not SHOULD */
#define APAD_KBM_WATCHDOG_MS   1000u   /* §6.20: release everything held */

/* ---- one ring slot, for each of the three types ------------------------ *
 *
 * `flags` bit 0 is DOWN (APAD_KBM_EVENT_DOWN, protocol.h); bits 1–7 are
 * reserved and are scrubbed on decode.
 *
 * A slot whose usage / button / control is 0 is "no event" and MUST be
 * SKIPPED during replay (§6.20) — it is not an event with code zero. The
 * decoder guarantees such a slot arrives with `flags` zero too, whatever a
 * non-conforming sender put there, so two decoded structures can be compared
 * for exact equality (§6.15).
 */
typedef struct {
    uint8_t usage;    /* HID Usage Page 0x07 usage ID; 0 = no event   */
    uint8_t flags;    /* bit 0 DOWN                                    */
} apad_key_event;

typedef struct {
    uint8_t button;   /* APAD_MOUSEBTN_*, 1..5; 0 = no event           */
    uint8_t flags;    /* bit 0 DOWN                                    */
} apad_mouse_event;

typedef struct {
    uint8_t control;  /* APAD_MEDIA_* index, 1..24; 0 = no event       */
    uint8_t flags;    /* bit 0 DOWN                                    */
} apad_media_event;

/*
 * §6.15 KEYBOARD (0x21) — 56 bytes on the wire:
 *
 *   0  keys[32]         32  held-key bitmap; usage u is byte u>>3, bit u&7
 *   32 event_seq         2  events generated this session; wraps
 *   34 reserved0         1
 *   35 reserved1         1
 *   36 events[8]        16  2 bytes each, OLDEST at index 0
 *   52 client_ticks_ms   4
 *   == 56
 *
 * `keys[]` is a 256-bit bitmap in which THE BIT INDEX IS THE USAGE ID, LSB
 * first within a byte — the same rule §5.1 uses for `buttons`. That sidesteps
 * the six-key rollover of a real HID boot report, and puts the modifiers
 * (0xE0–0xE7) inside the same bitmap so there is no second modifier byte for
 * two implementations to disagree about.
 *
 * keys[0] bits 0–3 are reserved: HID defines usages 0x00–0x03 as no-event,
 * ErrorRollOver, POSTFail and ErrorUndefined — conditions, not keys.
 */
typedef struct {
    uint8_t        keys[APAD_KEY_BITMAP_BYTES];
    uint16_t       event_seq;
    apad_key_event events[APAD_KEYBOARD_RING_DEPTH];
    uint32_t       client_ticks_ms;
} apad_keyboard;

/*
 * §6.16 MOUSE (0x22) — 24 bytes on the wire:
 *
 *   0  dx_accum          2  wrapping accumulator, + = right
 *   2  dy_accum          2  wrapping accumulator, +Y DOWN (screen space)
 *   4  wheel_accum       2  wrapping, DETENTS, + = away from the user
 *   6  hwheel_accum      2  wrapping, detents, + = right
 *   8  buttons           2  held-button bitmask, APAD_MOUSEBTN_BIT()
 *   10 event_seq         2
 *   12 events[4]         8  2 bytes each, oldest at index 0
 *   20 client_ticks_ms   4
 *   == 24
 *
 * THE ABSOLUTE VALUE OF AN ACCUMULATOR CARRIES NO MEANING (§6.16). A receiver
 * computes motion as apad_seq_diff(now, previous) against the last ACCEPTED
 * sample and MUST NOT interpret it any other way. Three rules go with that,
 * all normative: the first accepted MOUSE of a session establishes the
 * baseline and produces ZERO motion; a packet discarded as stale under
 * §6.20's window MUST NOT advance the baseline; a dropped packet costs
 * nothing, because the next diff spans the gap.
 *
 * +Y is DOWN here, not up: this is screen space (§5.3's touch convention),
 * because every client driving this surface drives it from a touchscreen
 * where the finger and the pointer must move the same way.
 */
typedef struct {
    uint16_t         dx_accum;
    uint16_t         dy_accum;
    uint16_t         wheel_accum;
    uint16_t         hwheel_accum;
    uint16_t         buttons;
    uint16_t         event_seq;
    apad_mouse_event events[APAD_MOUSE_RING_DEPTH];
    uint32_t         client_ticks_ms;
} apad_mouse;

/*
 * §6.17 MEDIA (0x23) — 20 bytes on the wire:
 *
 *   0  held              4  control c (1..32) is bit c-1
 *   4  event_seq         2
 *   6  reserved0         1
 *   7  reserved1         1
 *   8  events[4]         8  2 bytes each, oldest at index 0
 *   16 client_ticks_ms   4
 *   == 20
 *
 * Both a held mask and an event ring, because the two answer different
 * questions: `held` lets VOLUME_UP be held for a ramp and self-heals after
 * loss the way §5's `buttons` does, while the ring lets a single NEXT_TRACK
 * tap — which begins and ends between two samples — survive a dropped packet.
 */
typedef struct {
    uint32_t         held;
    uint16_t         event_seq;
    apad_media_event events[APAD_MEDIA_RING_DEPTH];
    uint32_t         client_ticks_ms;
} apad_media;

/*
 * §6.19 INPUTCAPS (0x44) — 16 bytes on the wire, server → client:
 *
 *   0  features          4  APAD_KBM_FEATURE_*: which types the server ACCEPTS
 *   4  status            4  APAD_KBM_STATUS_*: what exists RIGHT NOW
 *   8  media_mask        4  which §6.18 controls the server will drive
 *   12 mouse_rate_hz     2  0 = use the session's input_rate_hz
 *   14 reserved0         2
 *   == 16
 *
 * A CLIENT THAT HAS NOT RECEIVED AN INPUTCAPS MUST NOT SEND KEYBOARD, MOUSE
 * OR MEDIA (§6.19). Absence is the negotiation: a v1 server cannot send this
 * message and would discard those three anyway, so it never receives them.
 * This is why no §6.3 `caps` bit exists for these facilities and why none may
 * be added — §6.14 rules that widening `caps` is a v2 change, because a v1
 * server SCRUBS a reserved capability bit rather than ignoring it.
 *
 * `features` and `status` are separate because they answer different
 * questions at different times: features is what the server will accept and
 * is known before anything is created, status is what exists at this instant
 * and changes as devices are created on first use.
 *
 * If a `features` bit CLEARS, the client MUST stop sending that type and MUST
 * first release everything it holds for that facility (§6.19, §6.20) —
 * otherwise the last thing the server saw held stays held with nothing left
 * able to lift it.
 */
typedef struct {
    uint32_t features;      /* APAD_KBM_FEATURE_* */
    uint32_t status;        /* APAD_KBM_STATUS_*  */
    uint32_t media_mask;    /* APAD_MEDIA_BIT() of each index it will drive */
    uint16_t mouse_rate_hz; /* 0 = the session's input_rate_hz              */
} apad_inputcaps;

/* ---- §6.19 features and status ----------------------------------------- */

#define APAD_KBM_FEATURE_KEYBOARD  (1u << 0)   /* server accepts 0x21 */
#define APAD_KBM_FEATURE_MOUSE     (1u << 1)   /* accepts 0x22        */
#define APAD_KBM_FEATURE_MEDIA     (1u << 2)   /* accepts 0x23        */
/* bits 3..31 reserved, MUST be zero (§6.19) */

#define APAD_KBM_STATUS_KEYBOARD_READY (1u << 0)
#define APAD_KBM_STATUS_MOUSE_READY    (1u << 1)
#define APAD_KBM_STATUS_MEDIA_READY    (1u << 2)
/* Input is injected into the host's input stream rather than delivered by a
 * device, and some applications may ignore it. A bit, not a sentence: the
 * client writes its own warning (§6.13's rule). */
#define APAD_KBM_STATUS_SYNTHETIC      (1u << 3)
/* bits 4..31 reserved, MUST be zero (§6.19) */

/* ---- §6.16 mouse buttons ----------------------------------------------- *
 *
 * One number does double duty: it is the BIT POSITION plus one in
 * `MOUSE.buttons`, and the EVENT INDEX in `MOUSE.events[].button`. Zero is
 * "no event", which is why the indices start at 1.
 */
#define APAD_MOUSEBTN_LEFT     1u
#define APAD_MOUSEBTN_RIGHT    2u
#define APAD_MOUSEBTN_MIDDLE   3u
#define APAD_MOUSEBTN_BACK     4u
#define APAD_MOUSEBTN_FORWARD  5u
#define APAD_MOUSEBTN_MAX      5u   /* 6..255 are not buttons (§6.16) */

/* Bit for button index b (1..5) in `MOUSE.buttons`. */
#define APAD_MOUSEBTN_BIT(b)   ((uint16_t)(1u << ((unsigned)(b) - 1u)))

/* ---- §6.18 media control index ----------------------------------------- *
 *
 * AtticPad's own DENSE vocabulary, not a HID usage: a dense small index is
 * what lets `held` be one fixed-width mask, where a sparse 16-bit Consumer
 * Page usage would force a variable-length held LIST and destroy the
 * self-healing property that mask has.
 *
 * 32 slots are allocated and 24 are assigned. An index assigned in a future
 * revision MUST NOT be sent unless the server has set that index's bit in
 * INPUTCAPS.media_mask (§6.18) — reserving the room alone would buy nothing,
 * because a receiver predating the assignment SCRUBS those bits (§6.14).
 */
#define APAD_MEDIA_PLAY_PAUSE       1u
#define APAD_MEDIA_PLAY             2u
#define APAD_MEDIA_PAUSE            3u
#define APAD_MEDIA_STOP             4u
#define APAD_MEDIA_NEXT_TRACK       5u
#define APAD_MEDIA_PREV_TRACK       6u
#define APAD_MEDIA_FAST_FORWARD     7u
#define APAD_MEDIA_REWIND           8u
#define APAD_MEDIA_VOLUME_UP        9u
#define APAD_MEDIA_VOLUME_DOWN     10u
#define APAD_MEDIA_MUTE            11u
#define APAD_MEDIA_EJECT           12u
#define APAD_MEDIA_RECORD          13u
#define APAD_MEDIA_BRIGHTNESS_UP   14u
#define APAD_MEDIA_BRIGHTNESS_DOWN 15u
#define APAD_MEDIA_LAUNCH_BROWSER  16u
#define APAD_MEDIA_LAUNCH_MAIL     17u
#define APAD_MEDIA_LAUNCH_CALC     18u
#define APAD_MEDIA_SEARCH          19u
#define APAD_MEDIA_NAV_HOME        20u
#define APAD_MEDIA_NAV_BACK        21u
#define APAD_MEDIA_NAV_FORWARD     22u
#define APAD_MEDIA_REFRESH         23u
#define APAD_MEDIA_BOOKMARKS       24u
/* 25..32 reserved, MUST be zero (§6.18) */

#define APAD_MEDIA_ASSIGNED_MAX    24u  /* highest index §6.18 assigns  */
#define APAD_MEDIA_INDEX_MAX       32u  /* highest index `held` can carry */

/* Bit for control index c (1..32) in `MEDIA.held` / `INPUTCAPS.media_mask`. */
#define APAD_MEDIA_BIT(c)          ((uint32_t)1u << ((unsigned)(c) - 1u))

/* ---- §6.15 HID usages, curated ----------------------------------------- *
 *
 * USB HID Usage Page 0x07. Enough to build a key grid without hard-coding
 * numbers: letters, digits, F1–F12, the eight modifiers, arrows and the
 * common editing keys. DELIBERATELY NOT the whole page — the page is large,
 * mostly irrelevant to a handheld's on-screen keyboard, and every name here
 * is one more thing to keep correct.
 *
 * Anything absent is still perfectly sendable: the wire carries the number,
 * and codec.c passes through any usage it is given (see the decoder's note on
 * 0xA5–0xAF, 0xDE–0xDF and 0xE8–0xFF).
 */
#define APAD_HID_KEY_A            0x04u
#define APAD_HID_KEY_B            0x05u
#define APAD_HID_KEY_C            0x06u
#define APAD_HID_KEY_D            0x07u
#define APAD_HID_KEY_E            0x08u
#define APAD_HID_KEY_F            0x09u
#define APAD_HID_KEY_G            0x0Au
#define APAD_HID_KEY_H            0x0Bu
#define APAD_HID_KEY_I            0x0Cu
#define APAD_HID_KEY_J            0x0Du
#define APAD_HID_KEY_K            0x0Eu
#define APAD_HID_KEY_L            0x0Fu
#define APAD_HID_KEY_M            0x10u
#define APAD_HID_KEY_N            0x11u
#define APAD_HID_KEY_O            0x12u
#define APAD_HID_KEY_P            0x13u
#define APAD_HID_KEY_Q            0x14u
#define APAD_HID_KEY_R            0x15u
#define APAD_HID_KEY_S            0x16u
#define APAD_HID_KEY_T            0x17u
#define APAD_HID_KEY_U            0x18u
#define APAD_HID_KEY_V            0x19u
#define APAD_HID_KEY_W            0x1Au
#define APAD_HID_KEY_X            0x1Bu
#define APAD_HID_KEY_Y            0x1Cu
#define APAD_HID_KEY_Z            0x1Du

/* Digit row. 1..9 are contiguous from 0x1E; ZERO IS 0x27, AFTER 9, not
 * before 1 — the single easiest thing to get wrong on this page. */
#define APAD_HID_KEY_1            0x1Eu
#define APAD_HID_KEY_2            0x1Fu
#define APAD_HID_KEY_3            0x20u
#define APAD_HID_KEY_4            0x21u
#define APAD_HID_KEY_5            0x22u
#define APAD_HID_KEY_6            0x23u
#define APAD_HID_KEY_7            0x24u
#define APAD_HID_KEY_8            0x25u
#define APAD_HID_KEY_9            0x26u
#define APAD_HID_KEY_0            0x27u

#define APAD_HID_KEY_ENTER        0x28u
#define APAD_HID_KEY_ESCAPE       0x29u
#define APAD_HID_KEY_BACKSPACE    0x2Au
#define APAD_HID_KEY_TAB          0x2Bu
#define APAD_HID_KEY_SPACE        0x2Cu

/* Punctuation, 0x2D-0x38 contiguous: a full physical-layout on-screen
 * keyboard needs these to type a path or a URL, not just letters/digits
 * (added alongside the Android client's redesign — see its KeyGridView). */
#define APAD_HID_KEY_MINUS        0x2Du   /* - _ */
#define APAD_HID_KEY_EQUAL        0x2Eu   /* = + */
#define APAD_HID_KEY_LEFTBRACE    0x2Fu   /* [ { */
#define APAD_HID_KEY_RIGHTBRACE   0x30u   /* ] } */
#define APAD_HID_KEY_BACKSLASH    0x31u   /* \ | */
#define APAD_HID_KEY_SEMICOLON    0x33u   /* ; : */
#define APAD_HID_KEY_APOSTROPHE   0x34u   /* ' " */
#define APAD_HID_KEY_GRAVE        0x35u   /* ` ~ */
#define APAD_HID_KEY_COMMA        0x36u   /* , < */
#define APAD_HID_KEY_PERIOD       0x37u   /* . > */
#define APAD_HID_KEY_SLASH        0x38u   /* / ? */

#define APAD_HID_KEY_F1           0x3Au
#define APAD_HID_KEY_F2           0x3Bu
#define APAD_HID_KEY_F3           0x3Cu
#define APAD_HID_KEY_F4           0x3Du
#define APAD_HID_KEY_F5           0x3Eu
#define APAD_HID_KEY_F6           0x3Fu
#define APAD_HID_KEY_F7           0x40u
#define APAD_HID_KEY_F8           0x41u
#define APAD_HID_KEY_F9           0x42u
#define APAD_HID_KEY_F10          0x43u
#define APAD_HID_KEY_F11          0x44u
#define APAD_HID_KEY_F12          0x45u

#define APAD_HID_KEY_DELETE       0x4Cu   /* Delete Forward, not Backspace */
#define APAD_HID_KEY_RIGHT        0x4Fu
#define APAD_HID_KEY_LEFT         0x50u
#define APAD_HID_KEY_DOWN         0x51u
#define APAD_HID_KEY_UP           0x52u

/* The eight modifiers ride INSIDE keys[] like any other usage (§6.15). */
#define APAD_HID_KEY_LEFTCTRL     0xE0u
#define APAD_HID_KEY_LEFTSHIFT    0xE1u
#define APAD_HID_KEY_LEFTALT      0xE2u
#define APAD_HID_KEY_LEFTGUI      0xE3u   /* Windows / Command / Meta */
#define APAD_HID_KEY_RIGHTCTRL    0xE4u
#define APAD_HID_KEY_RIGHTSHIFT   0xE5u
#define APAD_HID_KEY_RIGHTALT     0xE6u
#define APAD_HID_KEY_RIGHTGUI     0xE7u

/* Byte and bit of usage u inside `keys[]` (§6.15: the bit index IS the usage
 * ID, LSB first within a byte). Written as macros so a client can build a
 * bitmap without calling into the codec. */
#define APAD_KEY_BYTE(u)  ((unsigned)(u) >> 3)
#define APAD_KEY_MASK(u)  ((uint8_t)(1u << ((unsigned)(u) & 7u)))

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_KBM_H */
