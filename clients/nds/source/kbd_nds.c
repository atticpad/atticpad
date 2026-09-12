/* clients/nds/source/kbd_nds.c -- see kbd_nds.h for why the default
 * initialiser cannot be used and what the four numbers below have to agree
 * with in ui.c.
 *
 * MIRRORED FROM references/nds/examples/keyboard/default_keyboard/source/
 * main.c: keyboardDemoInit() / keyboardShow() / keyboardUpdate(), driven once
 * per frame after scanKeys(), with the returned character appended unless it
 * is '\b'. The ONE deviation is the initialiser, which is keyboardInit() with
 * this client's own layer/base/display arguments instead of the demo's -- the
 * sample's own doc comment (nds/arm9/keyboard.h) spells keyboardDemoInit() out
 * as exactly that call with different numbers, so this is the same API used
 * the way the header says to use it, not a different one.
 */

#include <string.h>

#include <nds.h>

#include "kbd_nds.h"

/* Must match ui.c's VRAM MAP.
 *
 *   layer 0     a text background in mode 5, and priority 0 -- in front of
 *               ui.c's bitmap on layer 3.
 *   mapBase 0   2 KB units: the map lands at 0 KB and is 4 KB long
 *               (BgSize_T_256x512 is 32x64 entries of 2 bytes).
 *   tileBase 1  16 KB units: tiles land at 16 KB and are ~40 KB long, ending
 *               at ~56 KB -- below the 64 KB where ui.c's first bitmap buffer
 *               starts.
 *   true        the MAIN engine, which ui.c has put on the bottom screen.
 */
#define KBD_LAYER      0
#define KBD_MAP_BASE   0
#define KBD_TILE_BASE  1

static int s_ready;
static int s_visible;

int apad_kbd_init(void)
{
    Keyboard *kb = keyboardInit(NULL, KBD_LAYER, BgType_Text4bpp,
                                BgSize_T_256x512, KBD_MAP_BASE, KBD_TILE_BASE,
                                true, true);

    if (kb == NULL) {
        s_ready = 0;
        return 0;
    }
    /* CTRL/ALT are treated as ordinary keys: this client's only modifier
     * story is the KEYS-mode sticky latch in clients/common/apad_kbm_touch.c,
     * which is a different keyboard entirely (that one drives the PC's
     * keyboard; this one types into a text field). Left explicit rather than
     * relying on the documented default so the two are not confused. */
    (void)keyboardModifierModeSet(KeyboardModifiersIgnore);

    s_ready = 1;
    s_visible = 0;
    keyboardHide();
    return 1;
}

void apad_kbd_show(void)
{
    if (s_ready && !s_visible) {
        keyboardShow();
        s_visible = 1;
    }
}

void apad_kbd_hide(void)
{
    if (s_ready && s_visible) {
        keyboardHide();
        s_visible = 0;
    }
}

int apad_kbd_visible(void)
{
    return s_visible;
}

int apad_kbd_poll(void)
{
    int16_t c;

    if (!s_ready || !s_visible) {
        return 0;
    }
    c = keyboardUpdate();
    if (c == DVK_BACKSPACE) {
        return '\b';
    }
    if (c == DVK_ENTER) {
        return '\n';
    }
    /* DVK_TAB is '\t' and DVK_SPACE is ' ', both of which are >= 32 or
     * deliberately excluded below; every other control key is negative. */
    if (c >= 32 && c < 127) {
        return (int)c;
    }
    return 0;
}

int apad_kbd_apply(int key, char *buf, size_t cap, const char *filter)
{
    size_t n;

    if (buf == NULL || cap == 0u || key == 0 || key == '\n') {
        return 0;
    }
    n = strlen(buf);

    if (key == '\b') {
        if (n == 0u) {
            return 0;
        }
        buf[n - 1u] = '\0';
        return 1;
    }
    if (filter != NULL && strchr(filter, key) == NULL) {
        return 0;
    }
    if (n + 1u >= cap) {
        return 0;
    }
    buf[n] = (char)key;
    buf[n + 1u] = '\0';
    return 1;
}
