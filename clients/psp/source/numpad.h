/*
 * clients/psp/source/numpad.h -- the d-pad-driven numeric keypad.
 *
 * The 3DS gets its digits from a touchscreen (clients/3ds/source/
 * screen_connect.c's kPadKeys) and its pairing secret from a camera. This
 * console has neither, so one widget serves both jobs: typing an address, and
 * typing the six-digit PIN the PC is showing.
 */
#ifndef ATTICPAD_PSP_NUMPAD_H
#define ATTICPAD_PSP_NUMPAD_H

#define NUMPAD_KEYS 12

typedef struct {
    int cursor;          /* 0..11, row-major over a 3x4 grid */
} numpad;

void numpad_init(numpad *np);
/* Move the cursor. Wraps, because a cursor that stops dead at an edge feels
 * broken on a d-pad. */
void numpad_move(numpad *np, unsigned int keys_down);

/* What the focused key types: '0'..'9', '.', or '\b' for DEL. */
char numpad_char(const numpad *np);

/* `allow_dot` draws the '.' key dim and makes it inert -- used in PIN mode,
 * where a dot cannot appear. Drawn dim rather than removed: a key that
 * vanishes is a rendering bug to the person looking at it. */
void numpad_draw(const numpad *np, int x, int y, int allow_dot);
int  numpad_width(void);
int  numpad_height(void);

#endif /* ATTICPAD_PSP_NUMPAD_H */
