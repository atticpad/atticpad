/*
 * server/backends/sendinput_scancodes.h — HID Usage Page 0x07
 * (Keyboard/Keypad) usage ID -> PS/2 Scan Code Set 1, for the Windows
 * SendInput backend.
 *
 * MIRRORED, NOT WRITTEN FROM MEMORY. Every row of apad_hid_to_scancode_set1[]
 * below is a row-for-row transcription of
 * references/hid/usage-to-scancode-set1.txt — READ THAT FILE'S HEADER AND
 * references/hid/README.md BEFORE TOUCHING THIS TABLE. That file is, in its
 * own words, "the highest-risk file in references/hid/": there is no single
 * kernel array to transcribe the way the evdev table has, so its values are
 * cross-checked by hand between Microsoft's Keyboard Scan Code Specification
 * rev 1.3a and Andries Brouwer's independent scancode page. Do not hand-edit
 * a value here without first updating usage-to-scancode-set1.txt from those
 * primary sources and re-deriving. scripts/support/check_kbm_tables.c
 * enforces this at build time by re-parsing usage-to-scancode-set1.txt and
 * diffing it against this array.
 *
 * --- PACKED REPRESENTATION ---
 * Index is the HID usage ID. Value packs two things in one uint16_t:
 *   bits 0-7   the Set 1 MAKE byte (0x00 = no mapping; no real Set 1 make
 *              code is ever 0x00, so this is a safe sentinel)
 *   bit  8     APAD_SC_EXT_BIT — set if, and only if, injecting this key
 *              MUST be preceded by an 0xE0 prefix byte / MUST set
 *              KEYEVENTF_EXTENDEDKEY on SendInput.
 * Use the APAD_SC_MAKE() / APAD_SC_IS_EXT() accessors below rather than
 * masking by hand at the call site.
 *
 * --- THE EXTENDED-KEY RULE (see the reference file for the full version) ---
 * Scan Code Set 1 reused the plain keypad byte range for the newer 101-key
 * arrow/navigation cluster; the 0xE0 prefix is the ONLY thing distinguishing
 * "Keypad 4 / Left Arrow, sent as the dedicated keypad key" (plain 0x4B)
 * from "the dedicated arrow-cluster Left Arrow key" (E0 0x4B). Getting
 * APAD_SC_EXT_BIT backwards for a row produces a scancode that silently does
 * nothing, or types the wrong key, with no error from SendInput.
 *
 * --- THE ROW-DOUBLING ANOMALY (carried across from the README, read before
 * "fixing" anything here) ---
 * The Microsoft .doc this table is sourced from prints a second, spurious
 * "E0_"-prefixed copy of nearly every key's row directly underneath the real
 * one — including for keys with no possible extended form at all (Backspace,
 * Tab, Space, every letter, every digit). references/hid/usage-to-scancode-
 * set1.txt used ONLY the first (non-doubled) row for a key's own base value,
 * and marked Ext=yes ONLY where Andries Brouwer's page INDEPENDENTLY lists
 * that exact key among its documented escaped scancodes, or where the
 * Microsoft document's own prose (Notes 1-5) or a key's sole printed row
 * (Right Alt, Right Ctrl, Left Win, Right Win, Application — which print
 * only the E0-prefixed row, no bare duplicate underneath) settles it without
 * relying on the doubled-row mechanism at all. If a future reader opens the
 * .doc directly and sees "E0_0E" under Backspace's row, that is the known
 * anomaly, not a sign this table is wrong — see references/hid/README.md's
 * "row-doubling anomaly" section for the full writeup and a working
 * hypothesis for why it happens.
 *
 * --- DUPLICATE MAKE BYTES ARE EXPECTED, AND ARE A FEATURE OF SET 1 ITSELF
 * ---
 * Set 1 reuses a make byte across the plain-keypad key and the newer
 * arrow/nav-cluster key at the same physical byte value (disambiguated only
 * by APAD_SC_EXT_BIT), across Left/Right Ctrl, Left/Right Alt, Return/Keypad
 * Enter, and Slash/Keypad Slash, and across the US-layout Backslash and the
 * International-layout Non-US-hash usage (0x31/0x32, mutually exclusive by
 * keyboard layout, both non-extended). scripts/support/check_kbm_tables.c
 * allowlists exactly these fifteen pairs and fails the build on any OTHER
 * make-byte collision.
 *
 * --- PRINTSCREEN AND PAUSE ARE EXPLICIT NON-MAPPINGS, NOT AN OVERSIGHT ---
 * Usage 0x46 (PrintScreen) and usage 0x48 (Pause) are NOT a plain make/break
 * pair at all — PrintScreen's byte sequence depends on which modifier is
 * already held, and Pause has no break code and uses a rarer 0xE1 prefix (see
 * the reference file's SPECIAL section for the full three-variant breakdown
 * of each). This table deliberately maps both to 0 (no mapping) rather than
 * encoding a wrong single byte sequence. The reference file's own
 * recommendation is that a later agent inject these on Windows via
 * SendInput's VK_SNAPSHOT and VK_PAUSE virtual-key codes instead of a raw
 * scancode sequence — but that recommendation is EXPLICITLY FLAGGED
 * UNVERIFIED upstream (not checked against current Windows behaviour), so it
 * is recorded here as a pointer for whoever implements the Windows keyboard
 * backend, not baked into this table as though it were settled.
 *
 * --- SCOPE ---
 * Usages 0x66-0xE7's gap and 0xE8-0xFF have no Set 1 assignment in the
 * Microsoft document's Appendix A/C tables at all (locking Caps/Num/Scroll
 * variants, F13-F24, Kanji/LANG IME keys, the Execute/Help/Menu/... block,
 * Mute/Volume/Power) — there was nothing to extract, not something skipped.
 * They default to 0 (no mapping) via this array's static initialisation,
 * same as usages 0x00-0x03 (HID status conditions, not physical keys).
 *
 * See docs/PROTOCOL.md §6.15 for the wire format this table serves.
 *
 * Server-only. Per docs/CONVENTIONS.md and backend.h's own header comment, this is
 * ordinary hosted C — the mapping engine and its data live in server/, never
 * in core/.
 */
#ifndef ATTICPAD_SERVER_BACKENDS_SENDINPUT_SCANCODES_H
#define ATTICPAD_SERVER_BACKENDS_SENDINPUT_SCANCODES_H

#include <stdint.h>

#define APAD_SC_EXT_BIT 0x0100u

/* Extract the Set 1 make byte (low 8 bits). 0 = no mapping. */
#define APAD_SC_MAKE(v) ((uint8_t)((v) & 0xFFu))

/* Non-zero iff this key must be sent with an 0xE0 prefix / KEYEVENTF_EXTENDEDKEY. */
#define APAD_SC_IS_EXT(v) (((v) & APAD_SC_EXT_BIT) != 0)

/* index = HID usage ID; value = packed (make byte | APAD_SC_EXT_BIT), 0 = no mapping */
static const uint16_t apad_hid_to_scancode_set1[256] = {
    [0x00] = 0, /* Reserved (no event) -- no mapping (status, not a key) */
    [0x01] = 0, /* Keyboard ErrorRollOver -- no mapping (status, not a key) */
    [0x02] = 0, /* Keyboard POSTFail -- no mapping (status, not a key) */
    [0x03] = 0, /* Keyboard ErrorUndefined -- no mapping (status, not a key) */
    [0x04] = 0x1E, /* Keyboard a and A */
    [0x05] = 0x30, /* Keyboard b and B */
    [0x06] = 0x2E, /* Keyboard c and C */
    [0x07] = 0x20, /* Keyboard d and D */
    [0x08] = 0x12, /* Keyboard e and E */
    [0x09] = 0x21, /* Keyboard f and F */
    [0x0A] = 0x22, /* Keyboard g and G */
    [0x0B] = 0x23, /* Keyboard h and H */
    [0x0C] = 0x17, /* Keyboard i and I */
    [0x0D] = 0x24, /* Keyboard j and J */
    [0x0E] = 0x25, /* Keyboard k and K */
    [0x0F] = 0x26, /* Keyboard l and L */
    [0x10] = 0x32, /* Keyboard m and M */
    [0x11] = 0x31, /* Keyboard n and N */
    [0x12] = 0x18, /* Keyboard o and O */
    [0x13] = 0x19, /* Keyboard p and P */
    [0x14] = 0x10, /* Keyboard q and Q */
    [0x15] = 0x13, /* Keyboard r and R */
    [0x16] = 0x1F, /* Keyboard s and S */
    [0x17] = 0x14, /* Keyboard t and T */
    [0x18] = 0x16, /* Keyboard u and U */
    [0x19] = 0x2F, /* Keyboard v and V */
    [0x1A] = 0x11, /* Keyboard w and W */
    [0x1B] = 0x2D, /* Keyboard x and X */
    [0x1C] = 0x15, /* Keyboard y and Y */
    [0x1D] = 0x2C, /* Keyboard z and Z */
    [0x1E] = 0x02, /* Keyboard 1 and ! */
    [0x1F] = 0x03, /* Keyboard 2 and @ */
    [0x20] = 0x04, /* Keyboard 3 and # */
    [0x21] = 0x05, /* Keyboard 4 and $ */
    [0x22] = 0x06, /* Keyboard 5 and % */
    [0x23] = 0x07, /* Keyboard 6 and ^ */
    [0x24] = 0x08, /* Keyboard 7 and & */
    [0x25] = 0x09, /* Keyboard 8 and * */
    [0x26] = 0x0A, /* Keyboard 9 and ( */
    [0x27] = 0x0B, /* Keyboard 0 and ) */
    [0x28] = 0x1C, /* Keyboard Return (ENTER) */
    [0x29] = 0x01, /* Keyboard ESCAPE */
    [0x2A] = 0x0E, /* Keyboard DELETE (Backspace) */
    [0x2B] = 0x0F, /* Keyboard Tab */
    [0x2C] = 0x39, /* Keyboard Spacebar */
    [0x2D] = 0x0C, /* Keyboard - and _ */
    [0x2E] = 0x0D, /* Keyboard = and + */
    [0x2F] = 0x1A, /* Keyboard [ and { */
    [0x30] = 0x1B, /* Keyboard ] and } */
    [0x31] = 0x2B, /* Keyboard \ and | */
    [0x32] = 0x2B, /* Keyboard Non-US # and ~ */
    [0x33] = 0x27, /* Keyboard ; and : */
    [0x34] = 0x28, /* Keyboard ' and " */
    [0x35] = 0x29, /* Keyboard Grave Accent and Tilde */
    [0x36] = 0x33, /* Keyboard , and < */
    [0x37] = 0x34, /* Keyboard . and > */
    [0x38] = 0x35, /* Keyboard / and ? */
    [0x39] = 0x3A, /* Keyboard CapsLock */
    [0x3A] = 0x3B, /* Keyboard F1 */
    [0x3B] = 0x3C, /* Keyboard F2 */
    [0x3C] = 0x3D, /* Keyboard F3 */
    [0x3D] = 0x3E, /* Keyboard F4 */
    [0x3E] = 0x3F, /* Keyboard F5 */
    [0x3F] = 0x40, /* Keyboard F6 */
    [0x40] = 0x41, /* Keyboard F7 */
    [0x41] = 0x42, /* Keyboard F8 */
    [0x42] = 0x43, /* Keyboard F9 */
    [0x43] = 0x44, /* Keyboard F10 */
    [0x44] = 0x57, /* Keyboard F11 */
    [0x45] = 0x58, /* Keyboard F12 */
    [0x46] = 0, /* Keyboard PrintScreen -- no mapping (SPECIAL, see header comment) */
    [0x47] = 0x46, /* Keyboard ScrollLock */
    [0x48] = 0, /* Keyboard Pause -- no mapping (SPECIAL, see header comment) */
    [0x49] = 0x52 | APAD_SC_EXT_BIT, /* Keyboard Insert */
    [0x4A] = 0x47 | APAD_SC_EXT_BIT, /* Keyboard Home */
    [0x4B] = 0x49 | APAD_SC_EXT_BIT, /* Keyboard PageUp */
    [0x4C] = 0x53 | APAD_SC_EXT_BIT, /* Keyboard Delete Forward */
    [0x4D] = 0x4F | APAD_SC_EXT_BIT, /* Keyboard End */
    [0x4E] = 0x51 | APAD_SC_EXT_BIT, /* Keyboard PageDown */
    [0x4F] = 0x4D | APAD_SC_EXT_BIT, /* Keyboard RightArrow */
    [0x50] = 0x4B | APAD_SC_EXT_BIT, /* Keyboard LeftArrow */
    [0x51] = 0x50 | APAD_SC_EXT_BIT, /* Keyboard DownArrow */
    [0x52] = 0x48 | APAD_SC_EXT_BIT, /* Keyboard UpArrow */
    [0x53] = 0x45, /* Keypad NumLock and Clear */
    [0x54] = 0x35 | APAD_SC_EXT_BIT, /* Keypad / */
    [0x55] = 0x37, /* Keypad * */
    [0x56] = 0x4A, /* Keypad - */
    [0x57] = 0x4E, /* Keypad + */
    [0x58] = 0x1C | APAD_SC_EXT_BIT, /* Keypad ENTER */
    [0x59] = 0x4F, /* Keypad 1 and End */
    [0x5A] = 0x50, /* Keypad 2 and Down Arrow */
    [0x5B] = 0x51, /* Keypad 3 and PageDn */
    [0x5C] = 0x4B, /* Keypad 4 and Left Arrow */
    [0x5D] = 0x4C, /* Keypad 5 */
    [0x5E] = 0x4D, /* Keypad 6 and Right Arrow */
    [0x5F] = 0x47, /* Keypad 7 and Home */
    [0x60] = 0x48, /* Keypad 8 and Up Arrow */
    [0x61] = 0x49, /* Keypad 9 and PageUp */
    [0x62] = 0x52, /* Keypad 0 and Insert */
    [0x63] = 0x53, /* Keypad . and Delete */
    [0x64] = 0x56, /* Keyboard Non-US \ and | */
    [0x65] = 0x5D | APAD_SC_EXT_BIT, /* Keyboard Application */
    [0xE0] = 0x1D, /* Keyboard LeftControl */
    [0xE1] = 0x2A, /* Keyboard LeftShift */
    [0xE2] = 0x38, /* Keyboard LeftAlt */
    [0xE3] = 0x5B | APAD_SC_EXT_BIT, /* Keyboard Left GUI */
    [0xE4] = 0x1D | APAD_SC_EXT_BIT, /* Keyboard RightControl */
    [0xE5] = 0x36, /* Keyboard RightShift */
    [0xE6] = 0x38 | APAD_SC_EXT_BIT, /* Keyboard RightAlt */
    [0xE7] = 0x5C | APAD_SC_EXT_BIT, /* Keyboard Right GUI */
};

#endif /* ATTICPAD_SERVER_BACKENDS_SENDINPUT_SCANCODES_H */
