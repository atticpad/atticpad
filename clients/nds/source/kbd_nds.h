/*
 * clients/nds/source/kbd_nds.h -- text entry, the libnds on-screen keyboard.
 *
 * WHAT THIS REPLACES. The 3DS client has two text-entry paths: swkbdInputText()
 * (removed there because it BLOCKS until a human taps its Connect button) and,
 * in its place, a 12-key touch numpad drawn by screen_connect.c itself. Neither
 * is right here. The DS ships a real on-screen keyboard in libnds, the SDK's
 * own sample drives it (references/nds/examples/keyboard/default_keyboard/),
 * and it is non-blocking by construction: keyboardUpdate() returns one
 * character per frame or NOKEY. So the DS gets letters as well as digits, which
 * matters for the two fields the 3DS never had to enter -- the S10 pairing PIN
 * (a PIN is not necessarily six digits: docs/PROTOCOL.md S10.1) and a WEP key
 * (screen_wifi.c).
 *
 * WHY IT IS BEHIND THIS THIN WRAPPER rather than called straight from the
 * screens, when the wrapper is four functions long: the DEFAULT initialiser
 * cannot be used, and the reason is not obvious from a screen file.
 * keyboardDemoInit() is documented as
 *
 *     keyboardInit(NULL, 3, BgType_Text4bpp, BgSize_T_256x512, 20, 0, false, true)
 *
 * -- background 3 of the SUB display, tiles at tile base 0, map at map base 20
 * (nds/arm9/keyboard.h). Every one of those four numbers is wrong for this
 * client:
 *
 *   - `false` puts it on the SUB engine, which after ui.c's lcdMainOnBottom()
 *     is the TOP screen. A keyboard has to be on the screen with the digitizer
 *     under it.
 *   - tile base 0 / map base 20 is 0..44 KB of the engine's background VRAM,
 *     which on the main engine is where ui.c does NOT put its bitmap for
 *     exactly this reason -- but only if the keyboard is asked to sit at the
 *     addresses ui.c left free. See ui.c's VRAM MAP comment; the two files
 *     have to agree and this header is where the agreement is written down.
 *   - layer 3 is one of mode 5's extended (bitmap-capable) layers, which is
 *     where ui.c's own bitmap lives. The keyboard is a text background and
 *     goes on layer 0, which also gives it priority 0 -- in front of the
 *     bitmap on layer 3, which is what "the keyboard is over the UI" means in
 *     hardware terms.
 *
 * NO PALETTE CONFLICT, which is the other thing a reader will wonder about:
 * keyboardInit() loads its palette into the main engine's BG palette, and a
 * 16-bit bitmap background is direct colour and reads no palette at all.
 */
#ifndef ATTICPAD_NDS_KBD_H
#define ATTICPAD_NDS_KBD_H

#include <stddef.h>

/* Loads the keyboard's graphics into the VRAM ui.c left for it and leaves it
 * HIDDEN. Call once, after ui_init(). Returns 1 on success, 0 on failure --
 * in which case every call below is a no-op and the client is still usable
 * with the dev hooks or a saved address, which is why this is not fatal. */
int  apad_kbd_init(void);

/* Show / hide. Showing scrolls it up over the bottom of the bottom screen;
 * everything a screen wants readable while typing must stay above
 * UI_KBD_TOP_Y (ui.h). Idempotent. */
void apad_kbd_show(void);
void apad_kbd_hide(void);
int  apad_kbd_visible(void);

/* One frame of the keyboard. Call EXACTLY ONCE per frame, after scanKeys()
 * (the keyboard reads the touchscreen through libnds's key state), and only
 * while it is visible.
 *
 * Returns one of:
 *   > 0   an ASCII character to append
 *   '\b'  backspace: delete the last character
 *   '\n'  Enter: the caller should accept the field
 *   0     nothing happened this frame
 *
 * Everything libnds can return that is neither printable nor one of those two
 * (the arrow keys, Fold, Menu, Alt, Caps, Shift, Tab -- all negative DVK_*
 * values, nds/arm9/keyboard.h) is swallowed here and reported as 0, so a
 * screen never has to know that vocabulary. */
int  apad_kbd_poll(void);

/* The editing helper every field in this client shares: applies one
 * apad_kbd_poll() result to a NUL-terminated buffer. Returns 1 if the buffer
 * changed. `filter` may be NULL (accept every printable character); otherwise
 * it is a NUL-terminated set of the only characters allowed, which is how the
 * address field stays digits-and-dots without a second copy of that rule. */
int  apad_kbd_apply(int key, char *buf, size_t cap, const char *filter);

#endif /* ATTICPAD_NDS_KBD_H */
