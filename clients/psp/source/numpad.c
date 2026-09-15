#include <pspctrl.h>

#include "numpad.h"
#include "ui.h"

#define KEY_W 42
#define KEY_H 16
#define KEY_GX 46
#define KEY_GY 17

static const char kChars[NUMPAD_KEYS] = {
    '1', '2', '3',
    '4', '5', '6',
    '7', '8', '9',
    '.', '0', '\b'
};
static const char *const kLabels[NUMPAD_KEYS] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "DEL"
};

void numpad_init(numpad *np) { np->cursor = 0; }

int numpad_width(void)  { return KEY_GX * 3 - (KEY_GX - KEY_W); }
int numpad_height(void) { return KEY_GY * 4 - (KEY_GY - KEY_H); }

void numpad_move(numpad *np, unsigned int keys_down)
{
    int col = np->cursor % 3;
    int row = np->cursor / 3;

    if (keys_down & PSP_CTRL_LEFT)  { col = (col + 2) % 3; }
    if (keys_down & PSP_CTRL_RIGHT) { col = (col + 1) % 3; }
    if (keys_down & PSP_CTRL_UP)    { row = (row + 3) % 4; }
    if (keys_down & PSP_CTRL_DOWN)  { row = (row + 1) % 4; }
    np->cursor = row * 3 + col;
}

char numpad_char(const numpad *np)
{
    if (np->cursor < 0 || np->cursor >= NUMPAD_KEYS) { return 0; }
    return kChars[np->cursor];
}

void numpad_draw(const numpad *np, int x, int y, int allow_dot)
{
    int i;

    for (i = 0; i < NUMPAD_KEYS; i++) {
        ui_box k;
        int focused = (i == np->cursor);
        int inert   = (kChars[i] == '.' && !allow_dot);

        k.x = x + (i % 3) * KEY_GX;
        k.y = y + (i / 3) * KEY_GY;
        k.w = KEY_W;
        k.h = KEY_H;

        ui_panel(&k, focused ? ui_c_panel_hi() : ui_c_panel(),
                 focused ? ui_c_accent() : ui_c_border());
        ui_text(k.x + k.w / 2, k.y + (k.h - UI_CH) / 2,
                inert ? ui_c_border() : (focused ? ui_c_text() : ui_c_dim()),
                UI_ALIGN_CENTER, kLabels[i]);
    }
}
