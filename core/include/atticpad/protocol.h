/* atticpad/protocol.h — AtticPad v1 wire constants.
 *
 * Derived from docs/PROTOCOL.md, which is normative. Every constant here
 * cites the section it comes from. If this file and the spec disagree, the
 * spec is right: report the discrepancy, change neither.
 *
 * C99. No malloc, no float, no stdio. Safe to include on a 67 MHz ARM9.
 */
#ifndef ATTICPAD_PROTOCOL_H
#define ATTICPAD_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- §1 transport, §11 limits ------------------------------------------ */

#define APAD_DEFAULT_PORT        21100u
#define APAD_MAX_DATAGRAM        256u    /* §1, §11 */
#define APAD_MAX_SESSIONS        8u      /* §11 */
#define APAD_IDLE_TIMEOUT_MS     3000u   /* §11 */
#define APAD_KEEPALIVE_HZ        10u     /* §11 floor */
#define APAD_KEEPALIVE_MS        100u    /* §8: at least one packet / 100 ms */
#define APAD_DEFAULT_RATE_HZ     60u     /* §11 */
#define APAD_MAX_RATE_HZ         125u    /* §11 */
#define APAD_PAIRING_WINDOW_MS   120000u /* §11 */
#define APAD_MAX_PAIR_ATTEMPTS   5u      /* §11 */
#define APAD_PBKDF2_ITERATIONS   10000u  /* §10 */

/* ---- §3 header --------------------------------------------------------- */

#define APAD_MAGIC               0x4D43u /* "MC"; on the wire: 43 4D  (§3)   */
#define APAD_VERSION             1u      /* §3: version for this spec        */
#define APAD_HEADER_SIZE         12u     /* §3                               */
#define APAD_TAG_SIZE            8u      /* §3, §10: truncated HMAC-SHA256   */

/* Largest payload that still fits the 256-byte cap (§1). The spec states
 * only the datagram cap; these are the two derived bounds. */
#define APAD_MAX_PAYLOAD         (APAD_MAX_DATAGRAM - APAD_HEADER_SIZE)                    /* 244 */
#define APAD_MAX_PAYLOAD_AUTH    (APAD_MAX_DATAGRAM - APAD_HEADER_SIZE - APAD_TAG_SIZE)    /* 236 */

/* Header field offsets (§3). Every field is naturally aligned inside the
 * datagram, but access still goes through the byte-wise helpers in codec.c —
 * the buffer itself is not guaranteed aligned on any platform. */
#define APAD_OFF_MAGIC           0u
#define APAD_OFF_VERSION         2u
#define APAD_OFF_TYPE            3u
#define APAD_OFF_SESSION_ID      4u
#define APAD_OFF_SEQUENCE        6u
#define APAD_OFF_PAYLOAD_LEN     8u
#define APAD_OFF_FLAGS           10u

/* Header flags (§3). Bits 2..15 reserved: zero on send, ignored on receive. */
#define APAD_FLAG_AUTHENTICATED  (1u << 0)
#define APAD_FLAG_RELIABLE       (1u << 1)
#define APAD_FLAG_KNOWN_MASK     (APAD_FLAG_AUTHENTICATED | APAD_FLAG_RELIABLE)

/* ---- §4 message types -------------------------------------------------- */

enum apad_msg_type {
    APAD_MSG_DISCOVER    = 0x01,
    APAD_MSG_ANNOUNCE    = 0x02,
    APAD_MSG_HELLO       = 0x10,
    APAD_MSG_WELCOME     = 0x11,
    APAD_MSG_BYE         = 0x12,
    APAD_MSG_INPUT_STATE = 0x20,
    APAD_MSG_PING        = 0x30,
    APAD_MSG_PONG        = 0x31,
    APAD_MSG_RUMBLE      = 0x40,
    APAD_MSG_LED         = 0x41,
    APAD_MSG_STATUS      = 0x42,
    APAD_MSG_ACK         = 0x50,
    APAD_MSG_ERROR       = 0x51
};

/* Fixed payload sizes (§4). v1 has no variable-length payload (§12). */
#define APAD_LEN_DISCOVER        0u
#define APAD_LEN_ANNOUNCE        40u
#define APAD_LEN_HELLO           76u
#define APAD_LEN_WELCOME         60u
#define APAD_LEN_BYE             4u
#define APAD_LEN_INPUT_STATE     56u   /* §5 — 56, not 48; see §5 and §14.2 */
#define APAD_LEN_PING            8u
#define APAD_LEN_PONG            8u
#define APAD_LEN_RUMBLE          8u
#define APAD_LEN_LED             4u
#define APAD_LEN_STATUS          64u
#define APAD_LEN_ACK             4u
#define APAD_LEN_ERROR           64u

/* Fixed text field widths (§2: UTF-8, NUL-padded, not necessarily
 * NUL-terminated, always bounded by the fixed width). */
#define APAD_NAME_LEN            32u   /* §6.2 server_name, §6.3 device_name */
#define APAD_TEXT_LEN            60u   /* §6.9 STATUS.text, §6.11 ERROR.text */
#define APAD_CLIENT_ID_LEN       16u   /* §6.3 */
#define APAD_NONCE_LEN           16u   /* §6.3 client_nonce, §6.4 server_nonce */
#define APAD_KEY_MATERIAL_LEN    32u   /* §6.4 — MUST be zero in v1 */
#define APAD_SESSION_KEY_LEN     32u   /* §10 PBKDF2-HMAC-SHA256 output */

/* §10.1 — bounds on the shared secret handed to apad_derive_session_key.
 * NOT a wire-format property and NOT covered by the v1 freeze: the secret
 * never appears on the wire (§10), so these bound what a conforming
 * implementation must ACCEPT, they do not describe any field. §10.1 says so
 * explicitly ("This is not a wire-format change and does not require
 * protocol v2"). MIN is for callers that can reject; the derivation itself
 * returns void and cannot. */
#define APAD_SECRET_MIN_LEN       6u   /* §10.1 typed 6-digit PIN          */
#define APAD_SECRET_MAX_LEN      64u   /* §10.1 upper bound, used IN FULL  */

/* §10.3 — the pairing URI's hard ceiling, INCLUSIVE: a 128-byte URI is legal
 * and a 129-byte one is not. Like the secret bounds above this is NOT a
 * wire-format property and does NOT trip the v1 freeze — the URI is the
 * out-of-band channel (§10) and never appears in a datagram. §10.3 states the
 * longest legal form is about 103 bytes with a 64-byte secret, so this bounds
 * every buffer without excluding anything §10.1 permits. It does not include
 * the NUL terminator: a buffer that must hold any legal URI is
 * APAD_PAIR_URI_MAX + 1 bytes. */
#define APAD_PAIR_URI_MAX       128u   /* §10.3 whole-URI ceiling, bytes   */

/* ---- §5.1 button bitmask — frozen at v1, never renumber ---------------- */

#define APAD_BTN_A               (1u << 0)
#define APAD_BTN_B               (1u << 1)
#define APAD_BTN_X               (1u << 2)
#define APAD_BTN_Y               (1u << 3)
#define APAD_BTN_DPAD_UP         (1u << 4)
#define APAD_BTN_DPAD_DOWN       (1u << 5)
#define APAD_BTN_DPAD_LEFT       (1u << 6)
#define APAD_BTN_DPAD_RIGHT      (1u << 7)
#define APAD_BTN_L               (1u << 8)
#define APAD_BTN_R               (1u << 9)
#define APAD_BTN_ZL              (1u << 10)
#define APAD_BTN_ZR              (1u << 11)
#define APAD_BTN_L3              (1u << 12)
#define APAD_BTN_R3              (1u << 13)
#define APAD_BTN_START           (1u << 14)
#define APAD_BTN_SELECT          (1u << 15)
#define APAD_BTN_HOME            (1u << 16)
#define APAD_BTN_TOUCH_PRESS     (1u << 17)
#define APAD_BTN_TOUCH_REAR_PRESS (1u << 18)
#define APAD_BTN_CAPTURE         (1u << 19)
/* bits 20..31 reserved, MUST be zero (§5.1) */
#define APAD_BTN_VALID_MASK      0x000FFFFFu

/* §5.1 / D1: the D-pad is a contiguous nibble on purpose. */
#define APAD_BTN_DPAD_SHIFT      4
#define APAD_BTN_DPAD_MASK       (0xFu << APAD_BTN_DPAD_SHIFT)

/* HID hat values produced by apad_hat_lut / apad_hat_from_buttons. */
#define APAD_HAT_N               0
#define APAD_HAT_NE              1
#define APAD_HAT_E               2
#define APAD_HAT_SE              3
#define APAD_HAT_S               4
#define APAD_HAT_SW              5
#define APAD_HAT_W               6
#define APAD_HAT_NW              7
#define APAD_HAT_NULL            8

/* index: bit0=UP bit1=DOWN bit2=LEFT bit3=RIGHT  (§5.1)
 * value: HID hat, 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=null
 * Defined in codec.c; declared extern so it is one object across eight
 * platforms rather than a per-translation-unit copy. */
extern const uint8_t apad_hat_lut[16];

/* Convenience: (buttons >> 4) & 0xF, then the LUT. Branchless. */
uint8_t apad_hat_from_buttons(uint32_t buttons);

/* ---- §6.3 capability bitmask ------------------------------------------- */

#define APAD_CAP_DPAD            (1u << 0)
#define APAD_CAP_FACE4           (1u << 1)
#define APAD_CAP_SHOULDER        (1u << 2)   /* L, R */
#define APAD_CAP_SHOULDER2       (1u << 3)   /* ZL, ZR as digital buttons */
#define APAD_CAP_TRIGGERS        (1u << 4)   /* analog L2/R2 in axes[4], axes[5] */
#define APAD_CAP_STICK_L         (1u << 5)
#define APAD_CAP_STICK_R         (1u << 6)
#define APAD_CAP_TOUCH           (1u << 7)
#define APAD_CAP_TOUCH_REAR      (1u << 8)   /* Vita */
#define APAD_CAP_ACCEL           (1u << 9)
#define APAD_CAP_GYRO            (1u << 10)
#define APAD_CAP_RUMBLE          (1u << 11)
#define APAD_CAP_LED             (1u << 12)
#define APAD_CAP_BATTERY         (1u << 13)
/* bits 14..31 reserved, MUST be zero (§6.3) */
#define APAD_CAP_VALID_MASK      0x00003FFFu

/* ---- §6.4 WELCOME flags ------------------------------------------------ */

#define APAD_WELCOME_AUTH_REQUIRED (1u << 0)
#define APAD_WELCOME_FLAGS_MASK    0x01u

/* ---- §6.5 BYE reasons -------------------------------------------------- */

enum apad_bye_reason {
    APAD_BYE_NORMAL          = 0,
    APAD_BYE_TIMEOUT         = 1,
    APAD_BYE_SERVER_SHUTDOWN = 2,
    APAD_BYE_SLOT_REVOKED    = 3
};

/* ---- §6.9 STATUS codes ------------------------------------------------- */

enum apad_status_code {
    APAD_STATUS_INFO    = 0,
    APAD_STATUS_WARNING = 1,
    APAD_STATUS_ERROR   = 2
};

/* ---- §6.11 ERROR codes ------------------------------------------------- */

enum apad_error_code {
    APAD_ERRC_VERSION_MISMATCH = 1,
    APAD_ERRC_NO_FREE_SLOT     = 2,
    APAD_ERRC_AUTH_FAILED      = 3,
    APAD_ERRC_PAIRING_CLOSED   = 4,
    APAD_ERRC_TOO_MANY_TRIES   = 5,
    APAD_ERRC_MALFORMED        = 6,
    APAD_ERRC_UNKNOWN_SESSION  = 7
};

/* §9 retransmit schedule for RELIABLE messages, then fail the session. */
#define APAD_RETX_COUNT          4
extern const uint16_t apad_retx_delays_ms[APAD_RETX_COUNT]; /* 100 200 400 800 */

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_PROTOCOL_H */
