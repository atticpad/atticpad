/* atticpad/input.h — the canonical input state (docs/PROTOCOL.md §5).
 *
 * This is the in-memory representation. It is NEVER cast onto a packet
 * buffer; codec.c moves every field individually. The layout below happens
 * to mirror the wire layout, which makes the spec easy to check against —
 * but nothing depends on that, and nothing may.
 */
#ifndef ATTICPAD_INPUT_H
#define ATTICPAD_INPUT_H

#include <stdint.h>
#include "atticpad/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APAD_AXIS_COUNT     8
#define APAD_TOUCH_MAX      2

/* Axis indices (§5, axis assignment table). */
#define APAD_AXIS_LX        0
#define APAD_AXIS_LY        1   /* +Y up  (§5.3) */
#define APAD_AXIS_RX        2
#define APAD_AXIS_RY        3   /* +Y up  (§5.3) */
#define APAD_AXIS_L2        4   /* 0..32767; negative treated as 0 on receive */
#define APAD_AXIS_R2        5   /* 0..32767; negative treated as 0 on receive */
/* indices 6..7 reserved, MUST be zero (§5) */

#define APAD_TRIGGER_MAX    32767
#define APAD_BATTERY_UNKNOWN 255u

/* §5.2 — one touch contact, 6 bytes on the wire at 22 + 6*i. */
typedef struct {
    uint8_t id;        /* tracking id, stable for the life of a contact */
    uint8_t pressure;  /* 0 = unknown / binary touch */
    int16_t x;         /* normalised -32768..32767 */
    int16_t y;         /* normalised -32768..32767, +Y DOWN (§5.3) */
} apad_touch;

/*
 * §5 — INPUT_STATE payload, 56 bytes on the wire.
 *
 * 56, not 48. docs/DESIGN.md §5.7 comments this struct as 48 bytes; that comment
 * predates client_ticks_ms. reserved2[2] is the padding before
 * client_ticks_ms, declared explicitly rather than left implied, because
 * implied padding is exactly what differs between compilers (§5, §14.2).
 *
 * Wire offsets, for cross-checking against §5:
 *   0  buttons          4   (§5.1 bitmask)
 *   4  axes[8]          16
 *   20 touch_count      1   (0..2; >2 clamped on receive)
 *   21 reserved0        1
 *   22 touches[2]       12  (§5.2)
 *   34 accel[3]         6   milli-g, X Y Z
 *   40 gyro[3]          6   deci-degrees/second, pitch roll yaw
 *   46 battery          1   0..100, 255 unknown; 101..254 decode as 255 (§5.5)
 *   47 reserved1[3]     3
 *   50 reserved2[2]     2
 *   52 client_ticks_ms  4
 *   == 56
 */
typedef struct {
    uint32_t   buttons;
    int16_t    axes[APAD_AXIS_COUNT];
    uint8_t    touch_count;
    uint8_t    reserved0;
    apad_touch touches[APAD_TOUCH_MAX];
    int16_t    accel[3];
    int16_t    gyro[3];
    uint8_t    battery;
    uint8_t    reserved1[3];
    uint8_t    reserved2[2];
    uint32_t   client_ticks_ms;
} apad_input_state;

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_INPUT_H */
