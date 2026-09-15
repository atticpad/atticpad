/*
 * clients/psp/source/ui.h -- drawing for the PSP client.
 *
 * Same vocabulary as clients/3ds/source/ui.h, so a screen written against one
 * reads correctly against the other, with three deliberate differences:
 *
 *   - ONE screen. The 3DS has a 400x240 top and a 320x240 bottom and its ui.h
 *     has ui_screen_top()/ui_screen_bottom(); here there is a single 480x272
 *     surface and no such calls.
 *   - int coordinates, not float. The 3DS carries floats because citro2d's API
 *     does; this renderer writes pixels, and integers are the honest type.
 *   - no stereo, no touch. ui_box_hit() is gone with the touchscreen; the
 *     numpad is driven by the d-pad instead.
 *
 * Text is the 8x8 debug font. That is the whole font: there is no scaling, so
 * the 3DS's five UI_S_* sizes collapse to one, and the big round-trip figure
 * that docs/CONVENTIONS.md calls product rather than debug is drawn by ui_bignum() out
 * of rectangles instead of a scaled glyph.
 */
#ifndef ATTICPAD_PSP_UI_H
#define ATTICPAD_PSP_UI_H

#include <stdint.h>

#define UI_W       480          /* visible width                            */
#define UI_H       272          /* visible height                           */
#define UI_STRIDE  512          /* framebuffer pitch in PIXELS, not bytes   */
#define UI_CW        8          /* debug font cell width                    */
#define UI_CH        8          /* debug font cell height                   */

typedef struct { int x, y, w, h; } ui_box;

/* Palette. Same ten roles and the same intent as the 3DS's, so "accent means
 * focus" holds across both clients. Stored ABGR8888 -- see UI_RGB in ui.c. */
uint32_t ui_c_bg(void);
uint32_t ui_c_panel(void);
uint32_t ui_c_panel_hi(void);
uint32_t ui_c_border(void);
uint32_t ui_c_text(void);
uint32_t ui_c_dim(void);
uint32_t ui_c_accent(void);
uint32_t ui_c_good(void);
uint32_t ui_c_warn(void);
uint32_t ui_c_bad(void);

int  ui_init(void);
void ui_exit(void);

/* Clear the back buffer and point the text engine at it. */
void ui_frame_begin(void);
/* Wait for vblank, show the back buffer, swap. This is also what paces the
 * app at 60 Hz, the way C3D_FRAME_SYNCDRAW does on the 3DS. */
void ui_frame_end(void);

void ui_rect(int x, int y, int w, int h, uint32_t colour);
void ui_outline(int x, int y, int w, int h, int t, uint32_t colour);
void ui_panel(const ui_box *b, uint32_t fill, uint32_t border);
void ui_button(const ui_box *b, const char *label, int pressed, uint32_t accent);
void ui_header(const char *title, const char *right, uint32_t right_colour);

/* The console's face buttons as 9x9 pixel glyphs. "/\\" for triangle and
 * "[]" for square were what the first hardware tester could not read; the
 * shapes are what the buttons are, so the shapes are what is drawn. */
typedef enum {
    UI_GLYPH_CROSS = 0,      /* wire B */
    UI_GLYPH_CIRCLE,         /* wire A */
    UI_GLYPH_TRIANGLE,       /* wire X */
    UI_GLYPH_SQUARE          /* wire Y */
} ui_glyph;
#define UI_GLYPH_PX 9
void ui_glyph_draw(int x, int y, ui_glyph g, uint32_t colour);
/* Glyph, a gap, then text on the same baseline: "(x) type". */
void ui_hint(int x, int y, ui_glyph g, uint32_t colour, const char *text);
/* A D-pad cross with each arm lit by its bit: 1 up, 2 down, 4 left, 8 right
 * -- APAD_BTN_DPAD_* shifted down by APAD_BTN_DPAD_SHIFT. `arm` is the arm
 * size in pixels; the cross is 3*arm square. */
void ui_dpad(int x, int y, int arm, unsigned dirs, uint32_t on, uint32_t off,
             uint32_t border);

typedef enum { UI_ALIGN_LEFT = 0, UI_ALIGN_CENTER, UI_ALIGN_RIGHT } ui_align;

void ui_text(int x, int y, uint32_t colour, ui_align align, const char *s);
void ui_textf(int x, int y, uint32_t colour, ui_align align, const char *fmt, ...);
/* Truncates rather than shrinking: the font is fixed-width, so the fit is
 * exact arithmetic (max_w / UI_CW) instead of the 3DS's search loop. Same
 * name and shape so screen code reads identically. */
void ui_textf_fit(int x, int y, uint32_t colour, ui_align align, int max_w,
                  const char *fmt, ...);

/* Seven-segment digits built from ui_rect(), any size, no font asset. This is
 * what replaces the 3DS's UI_S_HUGE for the round-trip figure -- a number
 * meant to be readable from across a room. `cell` is the stroke thickness;
 * a digit is 3*cell wide and 5*cell tall. */
void ui_bignum(int x, int y, int cell, uint32_t colour, const char *digits);
int  ui_bignum_width(int cell, const char *digits);

/* DEV ONLY: write the back buffer to a binary PPM. ms0:/ is a real directory
 * on the host under an emulator, so this is how a render gets reviewed
 * pixel-exactly without screenshotting an emulator window. */
int ui_dump_ppm(const char *path);

#endif /* ATTICPAD_PSP_UI_H */
