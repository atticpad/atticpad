/*
 * server/backends/uinput_keymap.h — HID Usage Page 0x07 (Keyboard/Keypad)
 * usage ID -> Linux evdev KEY_* code, plus AtticPad's §6.18 media control
 * index -> evdev KEY_* code.
 *
 * MIRRORED, NOT WRITTEN FROM MEMORY. Every row of apad_hid_to_evdev[] below
 * is a row-for-row transcription of references/hid/usage-to-evdev.txt, which
 * is itself a derived data table from the Linux kernel's own
 * hid_keyboard[256] array (drivers/hid/hid-input.c, tag v6.6) — see that
 * file's header and references/hid/README.md for full provenance. Do not
 * hand-edit a value here without first updating usage-to-evdev.txt from the
 * primary source and re-deriving. scripts/support/check_kbm_tables.c
 * enforces this at build time by re-parsing usage-to-evdev.txt and diffing
 * it against this array; a value changed here without the reference file
 * changing too will fail the build.
 *
 * Index is the HID usage ID (0x00-0xFF). Value is the evdev numeric code.
 * 0 (KEY_RESERVED) marks the four HID "status condition" usages 0x00-0x03,
 * which are not physical keys — see docs/PROTOCOL.md §6.15's own reservation
 * of those bits in KEYBOARD.keys[]. 240 (KEY_UNKNOWN) is a DIFFERENT
 * sentinel: it is the kernel's own "this HID usage has no evdev equivalent"
 * marker, reproduced faithfully because that is what hid_keyboard[] says at
 * those 82 indices — it is not this file inventing a second null value.
 * Both 0 and 240 mean "do not inject anything" to a consumer of this table;
 * they are kept numerically distinct only so this array matches the kernel
 * array byte-for-byte, which is what makes the drift check meaningful.
 *
 * A few evdev targets are legitimately hit by more than one HID usage —
 * KEY_BACKSLASH (US-layout usage 0x31 and non-US usage 0x32, mutually
 * exclusive by keyboard layout), KEY_DELETE (0x4C, 0x9C, 0xD8 — the kernel
 * array's own reuse), and the keypad-area AL/AC duplicates (KEY_MUTE,
 * KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_STOP, KEY_FIND each hit once from the
 * dedicated Keyboard-page usage in the 0x7E-0x81 block and once more from the
 * Consumer-page-shaped alias in the 0xE8-0xFB block). All of these are
 * listed in usage-to-evdev.txt's own header comment or are visible directly
 * in its rows; scripts/support/check_kbm_tables.c allowlists exactly this
 * set and fails the build on any OTHER non-zero duplicate.
 *
 * See docs/PROTOCOL.md §6.15 for the wire format this table serves (a
 * 256-bit bitmap, bit index == HID usage ID) and §6.18 for the media control
 * vocabulary the second table below serves.
 *
 * Server-only. Per docs/CONVENTIONS.md and backend.h's own header comment, this is
 * ordinary hosted C — the mapping engine and its data live in server/, never
 * in core/.
 */
#ifndef ATTICPAD_SERVER_BACKENDS_UINPUT_KEYMAP_H
#define ATTICPAD_SERVER_BACKENDS_UINPUT_KEYMAP_H

#include <stdint.h>

#include "atticpad/kbm.h"   /* APAD_MEDIA_* -- see this file's own comment
                              * on apad_media_to_evdev[] below for why this
                              * table must never redefine that vocabulary. */

/* index = HID usage ID (0x00-0xFF); value = evdev KEY_* code, 0 = no mapping */
static const uint16_t apad_hid_to_evdev[256] = {
    [0x00] = 0, /* KEY_RESERVED */
    [0x01] = 0, /* KEY_RESERVED */
    [0x02] = 0, /* KEY_RESERVED */
    [0x03] = 0, /* KEY_RESERVED */
    [0x04] = 30, /* KEY_A */
    [0x05] = 48, /* KEY_B */
    [0x06] = 46, /* KEY_C */
    [0x07] = 32, /* KEY_D */
    [0x08] = 18, /* KEY_E */
    [0x09] = 33, /* KEY_F */
    [0x0A] = 34, /* KEY_G */
    [0x0B] = 35, /* KEY_H */
    [0x0C] = 23, /* KEY_I */
    [0x0D] = 36, /* KEY_J */
    [0x0E] = 37, /* KEY_K */
    [0x0F] = 38, /* KEY_L */
    [0x10] = 50, /* KEY_M */
    [0x11] = 49, /* KEY_N */
    [0x12] = 24, /* KEY_O */
    [0x13] = 25, /* KEY_P */
    [0x14] = 16, /* KEY_Q */
    [0x15] = 19, /* KEY_R */
    [0x16] = 31, /* KEY_S */
    [0x17] = 20, /* KEY_T */
    [0x18] = 22, /* KEY_U */
    [0x19] = 47, /* KEY_V */
    [0x1A] = 17, /* KEY_W */
    [0x1B] = 45, /* KEY_X */
    [0x1C] = 21, /* KEY_Y */
    [0x1D] = 44, /* KEY_Z */
    [0x1E] = 2, /* KEY_1 */
    [0x1F] = 3, /* KEY_2 */
    [0x20] = 4, /* KEY_3 */
    [0x21] = 5, /* KEY_4 */
    [0x22] = 6, /* KEY_5 */
    [0x23] = 7, /* KEY_6 */
    [0x24] = 8, /* KEY_7 */
    [0x25] = 9, /* KEY_8 */
    [0x26] = 10, /* KEY_9 */
    [0x27] = 11, /* KEY_0 */
    [0x28] = 28, /* KEY_ENTER */
    [0x29] = 1, /* KEY_ESC */
    [0x2A] = 14, /* KEY_BACKSPACE */
    [0x2B] = 15, /* KEY_TAB */
    [0x2C] = 57, /* KEY_SPACE */
    [0x2D] = 12, /* KEY_MINUS */
    [0x2E] = 13, /* KEY_EQUAL */
    [0x2F] = 26, /* KEY_LEFTBRACE */
    [0x30] = 27, /* KEY_RIGHTBRACE */
    [0x31] = 43, /* KEY_BACKSLASH */
    [0x32] = 43, /* KEY_BACKSLASH */
    [0x33] = 39, /* KEY_SEMICOLON */
    [0x34] = 40, /* KEY_APOSTROPHE */
    [0x35] = 41, /* KEY_GRAVE */
    [0x36] = 51, /* KEY_COMMA */
    [0x37] = 52, /* KEY_DOT */
    [0x38] = 53, /* KEY_SLASH */
    [0x39] = 58, /* KEY_CAPSLOCK */
    [0x3A] = 59, /* KEY_F1 */
    [0x3B] = 60, /* KEY_F2 */
    [0x3C] = 61, /* KEY_F3 */
    [0x3D] = 62, /* KEY_F4 */
    [0x3E] = 63, /* KEY_F5 */
    [0x3F] = 64, /* KEY_F6 */
    [0x40] = 65, /* KEY_F7 */
    [0x41] = 66, /* KEY_F8 */
    [0x42] = 67, /* KEY_F9 */
    [0x43] = 68, /* KEY_F10 */
    [0x44] = 87, /* KEY_F11 */
    [0x45] = 88, /* KEY_F12 */
    [0x46] = 99, /* KEY_SYSRQ */
    [0x47] = 70, /* KEY_SCROLLLOCK */
    [0x48] = 119, /* KEY_PAUSE */
    [0x49] = 110, /* KEY_INSERT */
    [0x4A] = 102, /* KEY_HOME */
    [0x4B] = 104, /* KEY_PAGEUP */
    [0x4C] = 111, /* KEY_DELETE */
    [0x4D] = 107, /* KEY_END */
    [0x4E] = 109, /* KEY_PAGEDOWN */
    [0x4F] = 106, /* KEY_RIGHT */
    [0x50] = 105, /* KEY_LEFT */
    [0x51] = 108, /* KEY_DOWN */
    [0x52] = 103, /* KEY_UP */
    [0x53] = 69, /* KEY_NUMLOCK */
    [0x54] = 98, /* KEY_KPSLASH */
    [0x55] = 55, /* KEY_KPASTERISK */
    [0x56] = 74, /* KEY_KPMINUS */
    [0x57] = 78, /* KEY_KPPLUS */
    [0x58] = 96, /* KEY_KPENTER */
    [0x59] = 79, /* KEY_KP1 */
    [0x5A] = 80, /* KEY_KP2 */
    [0x5B] = 81, /* KEY_KP3 */
    [0x5C] = 75, /* KEY_KP4 */
    [0x5D] = 76, /* KEY_KP5 */
    [0x5E] = 77, /* KEY_KP6 */
    [0x5F] = 71, /* KEY_KP7 */
    [0x60] = 72, /* KEY_KP8 */
    [0x61] = 73, /* KEY_KP9 */
    [0x62] = 82, /* KEY_KP0 */
    [0x63] = 83, /* KEY_KPDOT */
    [0x64] = 86, /* KEY_102ND */
    [0x65] = 127, /* KEY_COMPOSE */
    [0x66] = 116, /* KEY_POWER */
    [0x67] = 117, /* KEY_KPEQUAL */
    [0x68] = 183, /* KEY_F13 */
    [0x69] = 184, /* KEY_F14 */
    [0x6A] = 185, /* KEY_F15 */
    [0x6B] = 186, /* KEY_F16 */
    [0x6C] = 187, /* KEY_F17 */
    [0x6D] = 188, /* KEY_F18 */
    [0x6E] = 189, /* KEY_F19 */
    [0x6F] = 190, /* KEY_F20 */
    [0x70] = 191, /* KEY_F21 */
    [0x71] = 192, /* KEY_F22 */
    [0x72] = 193, /* KEY_F23 */
    [0x73] = 194, /* KEY_F24 */
    [0x74] = 134, /* KEY_OPEN */
    [0x75] = 138, /* KEY_HELP */
    [0x76] = 130, /* KEY_PROPS */
    [0x77] = 132, /* KEY_FRONT */
    [0x78] = 128, /* KEY_STOP */
    [0x79] = 129, /* KEY_AGAIN */
    [0x7A] = 131, /* KEY_UNDO */
    [0x7B] = 137, /* KEY_CUT */
    [0x7C] = 133, /* KEY_COPY */
    [0x7D] = 135, /* KEY_PASTE */
    [0x7E] = 136, /* KEY_FIND */
    [0x7F] = 113, /* KEY_MUTE */
    [0x80] = 115, /* KEY_VOLUMEUP */
    [0x81] = 114, /* KEY_VOLUMEDOWN */
    [0x82] = 240, /* KEY_UNKNOWN */
    [0x83] = 240, /* KEY_UNKNOWN */
    [0x84] = 240, /* KEY_UNKNOWN */
    [0x85] = 121, /* KEY_KPCOMMA */
    [0x86] = 240, /* KEY_UNKNOWN */
    [0x87] = 89, /* KEY_RO */
    [0x88] = 93, /* KEY_KATAKANAHIRAGANA */
    [0x89] = 124, /* KEY_YEN */
    [0x8A] = 92, /* KEY_HENKAN */
    [0x8B] = 94, /* KEY_MUHENKAN */
    [0x8C] = 95, /* KEY_KPJPCOMMA */
    [0x8D] = 240, /* KEY_UNKNOWN */
    [0x8E] = 240, /* KEY_UNKNOWN */
    [0x8F] = 240, /* KEY_UNKNOWN */
    [0x90] = 122, /* KEY_HANGEUL */
    [0x91] = 123, /* KEY_HANJA */
    [0x92] = 90, /* KEY_KATAKANA */
    [0x93] = 91, /* KEY_HIRAGANA */
    [0x94] = 85, /* KEY_ZENKAKUHANKAKU */
    [0x95] = 240, /* KEY_UNKNOWN */
    [0x96] = 240, /* KEY_UNKNOWN */
    [0x97] = 240, /* KEY_UNKNOWN */
    [0x98] = 240, /* KEY_UNKNOWN */
    [0x99] = 240, /* KEY_UNKNOWN */
    [0x9A] = 240, /* KEY_UNKNOWN */
    [0x9B] = 240, /* KEY_UNKNOWN */
    [0x9C] = 111, /* KEY_DELETE */
    [0x9D] = 240, /* KEY_UNKNOWN */
    [0x9E] = 240, /* KEY_UNKNOWN */
    [0x9F] = 240, /* KEY_UNKNOWN */
    [0xA0] = 240, /* KEY_UNKNOWN */
    [0xA1] = 240, /* KEY_UNKNOWN */
    [0xA2] = 240, /* KEY_UNKNOWN */
    [0xA3] = 240, /* KEY_UNKNOWN */
    [0xA4] = 240, /* KEY_UNKNOWN */
    [0xA5] = 240, /* KEY_UNKNOWN */
    [0xA6] = 240, /* KEY_UNKNOWN */
    [0xA7] = 240, /* KEY_UNKNOWN */
    [0xA8] = 240, /* KEY_UNKNOWN */
    [0xA9] = 240, /* KEY_UNKNOWN */
    [0xAA] = 240, /* KEY_UNKNOWN */
    [0xAB] = 240, /* KEY_UNKNOWN */
    [0xAC] = 240, /* KEY_UNKNOWN */
    [0xAD] = 240, /* KEY_UNKNOWN */
    [0xAE] = 240, /* KEY_UNKNOWN */
    [0xAF] = 240, /* KEY_UNKNOWN */
    [0xB0] = 240, /* KEY_UNKNOWN */
    [0xB1] = 240, /* KEY_UNKNOWN */
    [0xB2] = 240, /* KEY_UNKNOWN */
    [0xB3] = 240, /* KEY_UNKNOWN */
    [0xB4] = 240, /* KEY_UNKNOWN */
    [0xB5] = 240, /* KEY_UNKNOWN */
    [0xB6] = 179, /* KEY_KPLEFTPAREN */
    [0xB7] = 180, /* KEY_KPRIGHTPAREN */
    [0xB8] = 240, /* KEY_UNKNOWN */
    [0xB9] = 240, /* KEY_UNKNOWN */
    [0xBA] = 240, /* KEY_UNKNOWN */
    [0xBB] = 240, /* KEY_UNKNOWN */
    [0xBC] = 240, /* KEY_UNKNOWN */
    [0xBD] = 240, /* KEY_UNKNOWN */
    [0xBE] = 240, /* KEY_UNKNOWN */
    [0xBF] = 240, /* KEY_UNKNOWN */
    [0xC0] = 240, /* KEY_UNKNOWN */
    [0xC1] = 240, /* KEY_UNKNOWN */
    [0xC2] = 240, /* KEY_UNKNOWN */
    [0xC3] = 240, /* KEY_UNKNOWN */
    [0xC4] = 240, /* KEY_UNKNOWN */
    [0xC5] = 240, /* KEY_UNKNOWN */
    [0xC6] = 240, /* KEY_UNKNOWN */
    [0xC7] = 240, /* KEY_UNKNOWN */
    [0xC8] = 240, /* KEY_UNKNOWN */
    [0xC9] = 240, /* KEY_UNKNOWN */
    [0xCA] = 240, /* KEY_UNKNOWN */
    [0xCB] = 240, /* KEY_UNKNOWN */
    [0xCC] = 240, /* KEY_UNKNOWN */
    [0xCD] = 240, /* KEY_UNKNOWN */
    [0xCE] = 240, /* KEY_UNKNOWN */
    [0xCF] = 240, /* KEY_UNKNOWN */
    [0xD0] = 240, /* KEY_UNKNOWN */
    [0xD1] = 240, /* KEY_UNKNOWN */
    [0xD2] = 240, /* KEY_UNKNOWN */
    [0xD3] = 240, /* KEY_UNKNOWN */
    [0xD4] = 240, /* KEY_UNKNOWN */
    [0xD5] = 240, /* KEY_UNKNOWN */
    [0xD6] = 240, /* KEY_UNKNOWN */
    [0xD7] = 240, /* KEY_UNKNOWN */
    [0xD8] = 111, /* KEY_DELETE */
    [0xD9] = 240, /* KEY_UNKNOWN */
    [0xDA] = 240, /* KEY_UNKNOWN */
    [0xDB] = 240, /* KEY_UNKNOWN */
    [0xDC] = 240, /* KEY_UNKNOWN */
    [0xDD] = 240, /* KEY_UNKNOWN */
    [0xDE] = 240, /* KEY_UNKNOWN */
    [0xDF] = 240, /* KEY_UNKNOWN */
    [0xE0] = 29, /* KEY_LEFTCTRL */
    [0xE1] = 42, /* KEY_LEFTSHIFT */
    [0xE2] = 56, /* KEY_LEFTALT */
    [0xE3] = 125, /* KEY_LEFTMETA */
    [0xE4] = 97, /* KEY_RIGHTCTRL */
    [0xE5] = 54, /* KEY_RIGHTSHIFT */
    [0xE6] = 100, /* KEY_RIGHTALT */
    [0xE7] = 126, /* KEY_RIGHTMETA */
    [0xE8] = 164, /* KEY_PLAYPAUSE */
    [0xE9] = 166, /* KEY_STOPCD */
    [0xEA] = 165, /* KEY_PREVIOUSSONG */
    [0xEB] = 163, /* KEY_NEXTSONG */
    [0xEC] = 161, /* KEY_EJECTCD */
    [0xED] = 115, /* KEY_VOLUMEUP */
    [0xEE] = 114, /* KEY_VOLUMEDOWN */
    [0xEF] = 113, /* KEY_MUTE */
    [0xF0] = 150, /* KEY_WWW */
    [0xF1] = 158, /* KEY_BACK */
    [0xF2] = 159, /* KEY_FORWARD */
    [0xF3] = 128, /* KEY_STOP */
    [0xF4] = 136, /* KEY_FIND */
    [0xF5] = 177, /* KEY_SCROLLUP */
    [0xF6] = 178, /* KEY_SCROLLDOWN */
    [0xF7] = 176, /* KEY_EDIT */
    [0xF8] = 142, /* KEY_SLEEP */
    [0xF9] = 152, /* KEY_COFFEE */
    [0xFA] = 173, /* KEY_REFRESH */
    [0xFB] = 140, /* KEY_CALC */
    [0xFC] = 240, /* KEY_UNKNOWN */
    [0xFD] = 240, /* KEY_UNKNOWN */
    [0xFE] = 240, /* KEY_UNKNOWN */
    [0xFF] = 240, /* KEY_UNKNOWN */
};

/*
 * docs/PROTOCOL.md §6.18 media control index (1..24, 25-32 reserved) ->
 * Linux evdev KEY_* code.
 *
 * MIRRORED, NOT WRITTEN FROM MEMORY, THE SAME AS apad_hid_to_evdev[] ABOVE.
 * §6.18 is AtticPad's own dense vocabulary, not a HID usage page, but every
 * one of its 24 controls names a function a real HID Consumer Page (0x0C)
 * device already reports, and references/hid/consumer-to-evdev.txt is the
 * vendored ground truth for that page: a derived data table from the same
 * pinned kernel tag (v6.6) as the keyboard table above, this time from
 * hid-input.c's `case HID_UP_CONSUMER:` switch rather than hid_keyboard[].
 * scripts/support/check_kbm_tables.c re-parses that file and diffs it
 * against apad_media_to_evdev[] below the same way it diffs the keyboard
 * table -- a value changed here without consumer-to-evdev.txt changing too
 * will fail the build.
 *
 * Every row below resolves to the exact usage->KEY_* case the kernel's own
 * switch takes; consumer-to-evdev.txt's tail section spells out, per §6.18
 * control, which Consumer usage backs it and why, including the one row
 * (STOP) where an earlier, non-vendored pass at this table had the wrong
 * evdev target and the four rows (PLAY, PAUSE, EJECT, SEARCH) that earlier
 * pass could only guess at. All four guesses turned out to match the
 * kernel's own switch; none needed to change numerically. STOP did: an
 * earlier pass used KEY_STOP (128, HID Consumer "AC Stop" usage 0x226, a
 * browser/document stop-loading control) where the transport-block context
 * of §6.18's STOP calls for KEY_STOPCD (166, Consumer usage 0x0b7) instead.
 * See consumer-to-evdev.txt's STOP note for the full reasoning.
 *
 * KEY_PAUSE (119) is worth a human's eyes even though it is now vendored:
 * it is the SAME evdev code apad_hid_to_evdev[0x48] above already uses for
 * the physical Keyboard-page Pause/Break key, reused here across two
 * different message types (KEYBOARD vs MEDIA). That is not a collision in
 * the sense check_kbm_tables.c checks for (different arrays, different wire
 * messages), and it is exactly what the kernel's own tables do too -- MEDIA
 * and KEYBOARD are different HID pages that happen to name the same evdev
 * target for "pause", which is a fact about the hardware vocabulary, not a
 * bug in this table.
 *
 * Nothing in this table is UNVERIFIED any longer. If a §6.18 revision adds a
 * 25th+ control with no Consumer-page equivalent in consumer-to-evdev.txt's
 * scope, that is the condition under which a genuinely unverified row would
 * return -- re-derive consumer-to-evdev.txt from the primary source first,
 * per its own header, rather than filling the new slot from memory.
 */
/* APAD_MEDIA_* is §6.18 wire vocabulary. It lives in atticpad/kbm.h (core),
 * included above, and MUST NOT be redefined here -- two copies of the same
 * dense index vocabulary is exactly the drift trap this table's own drift
 * guard (scripts/support/check_kbm_tables.c) exists to prevent, just one
 * level up: the guard can't catch two #define blocks disagreeing with each
 * other, only a table disagreeing with references/hid/. See
 * atticpad/kbm.h's own comment on APAD_MEDIA_PLAY_PAUSE for the numbering. */

/* index = §6.18 media control index (1..24 used, 0 and 25..32 unused/
 * reserved and left 0); value = evdev KEY_* code, 0 = no mapping */
static const uint16_t apad_media_to_evdev[33] = {
    [APAD_MEDIA_PLAY_PAUSE]      = 164, /* KEY_PLAYPAUSE, consumer usage 0x0cd */
    [APAD_MEDIA_PLAY]            = 207, /* KEY_PLAY, consumer usage 0x0b0 */
    [APAD_MEDIA_PAUSE]           = 119, /* KEY_PAUSE, consumer usage 0x0b1 */
    [APAD_MEDIA_STOP]            = 166, /* KEY_STOPCD, consumer usage 0x0b7 (transport Stop, not "AC Stop" 0x226) */
    [APAD_MEDIA_NEXT_TRACK]      = 163, /* KEY_NEXTSONG, consumer usage 0x0b5 */
    [APAD_MEDIA_PREV_TRACK]      = 165, /* KEY_PREVIOUSSONG, consumer usage 0x0b6 */
    [APAD_MEDIA_FAST_FORWARD]    = 208, /* KEY_FASTFORWARD, consumer usage 0x0b3 */
    [APAD_MEDIA_REWIND]          = 168, /* KEY_REWIND, consumer usage 0x0b4 */
    [APAD_MEDIA_VOLUME_UP]       = 115, /* KEY_VOLUMEUP, consumer usage 0x0e9 */
    [APAD_MEDIA_VOLUME_DOWN]     = 114, /* KEY_VOLUMEDOWN, consumer usage 0x0ea */
    [APAD_MEDIA_MUTE]            = 113, /* KEY_MUTE, consumer usage 0x0e2 */
    [APAD_MEDIA_EJECT]           = 161, /* KEY_EJECTCD, consumer usage 0x0b8 */
    [APAD_MEDIA_RECORD]          = 167, /* KEY_RECORD, consumer usage 0x0b2 */
    [APAD_MEDIA_BRIGHTNESS_UP]   = 225, /* KEY_BRIGHTNESSUP, consumer usage 0x06f */
    [APAD_MEDIA_BRIGHTNESS_DOWN] = 224, /* KEY_BRIGHTNESSDOWN, consumer usage 0x070 */
    [APAD_MEDIA_LAUNCH_BROWSER]  = 150, /* KEY_WWW, consumer usage 0x08a/0x196, "AL Internet Browser" */
    [APAD_MEDIA_LAUNCH_MAIL]     = 155, /* KEY_MAIL, consumer usage 0x18a */
    [APAD_MEDIA_LAUNCH_CALC]     = 140, /* KEY_CALC, consumer usage 0x192, "AL Calculator" */
    [APAD_MEDIA_SEARCH]          = 217, /* KEY_SEARCH, consumer usage 0x221 (not KEY_FIND / 0x21f) */
    [APAD_MEDIA_NAV_HOME]        = 172, /* KEY_HOMEPAGE, consumer usage 0x223, "AC Home" */
    [APAD_MEDIA_NAV_BACK]        = 158, /* KEY_BACK, consumer usage 0x224, "AC Back" */
    [APAD_MEDIA_NAV_FORWARD]     = 159, /* KEY_FORWARD, consumer usage 0x225, "AC Forward" */
    [APAD_MEDIA_REFRESH]         = 173, /* KEY_REFRESH, consumer usage 0x227, "AC Refresh" */
    [APAD_MEDIA_BOOKMARKS]       = 156, /* KEY_BOOKMARKS, consumer usage 0x182/0x22a, "AC Bookmarks" */
};

#endif /* ATTICPAD_SERVER_BACKENDS_UINPUT_KEYMAP_H */
