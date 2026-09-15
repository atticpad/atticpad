/*
 * clients/psp/source/ui.c -- software 2D drawing into a double-buffered
 * framebuffer, with text from the SDK's 8x8 debug font.
 *
 * WHY NOT sceGu. The GE is a DMA engine reading a display list out of main
 * RAM, so using it means getting list alignment, buffer strides, scissor,
 * 2D transform vertices AND cache flushes before the hardware reads a buffer
 * all correct with no console to catch a coherency bug. That is the largest
 * new failure surface this port could take on, and it buys blending and
 * textured text that a numpad, some panels and a status readout do not need.
 * Writing pixels costs 480*272*4 = 522 KB per frame, which at 60 Hz is about
 * 31 MB/s of uncached writes -- comfortable, and if it ever is not, clear
 * only the rows a screen uses.
 *
 * UNCACHED. Both buffers are addressed through 0x40000000, the uncached
 * mirror of VRAM. Drawing through the cache would need a writeback before
 * the display controller reads the buffer, and forgetting that once produces
 * tearing that looks like a drawing bug. Uncached writes to VRAM are the
 * normal idiom for a software renderer here.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* PSP 8888 is ABGR in memory: 0xAABBGGRR. Writing the palette in RGB terms
 * and converting here keeps the constants readable next to the 3DS's. */
#define UI_RGB(r, g, b) \
    ((uint32_t)0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

/* 512 * 272 * 4 = 0x88000. Two of these fit in the PSP's 2 MB of VRAM with
 * room to spare, so the second buffer starts exactly one buffer in. */
#define UI_FB_PIXELS (UI_STRIDE * UI_H)
#define UI_FB_BYTES  (UI_FB_PIXELS * 4)

static uint32_t *s_buf[2];
static int       s_back;      /* index of the buffer being drawn into */
static int       s_ready;

/* The same ten roles as clients/3ds/source/ui.c, same hex values. */
uint32_t ui_c_bg(void)       { return UI_RGB(0x12, 0x16, 0x1E); }
uint32_t ui_c_panel(void)    { return UI_RGB(0x1E, 0x26, 0x33); }
uint32_t ui_c_panel_hi(void) { return UI_RGB(0x30, 0x3E, 0x52); }
uint32_t ui_c_border(void)   { return UI_RGB(0x3C, 0x4C, 0x64); }
uint32_t ui_c_text(void)     { return UI_RGB(0xE6, 0xEC, 0xF4); }
uint32_t ui_c_dim(void)      { return UI_RGB(0x87, 0x96, 0xAB); }
uint32_t ui_c_accent(void)   { return UI_RGB(0x4C, 0xC2, 0xFF); }
uint32_t ui_c_good(void)     { return UI_RGB(0x5C, 0xD6, 0x7A); }
uint32_t ui_c_warn(void)     { return UI_RGB(0xFF, 0xC4, 0x4C); }
uint32_t ui_c_bad(void)      { return UI_RGB(0xFF, 0x6B, 0x5B); }

int ui_init(void)
{
    uint8_t *vram = (uint8_t *)sceGeEdramGetAddr();

    if (vram == NULL) {
        return -1;
    }
    /* 0x40000000 = the uncached mirror. See the header comment. */
    s_buf[0] = (uint32_t *)((uintptr_t)vram | 0x40000000u);
    s_buf[1] = (uint32_t *)((uintptr_t)(vram + UI_FB_BYTES) | 0x40000000u);
    s_back   = 0;

    /* The display setup every SDK sample gets from pspDebugScreenInit():
     * set the mode, clear the first buffer, and point the controller at it
     * BEFORE the first frame. Previously this was skipped (setup=0 below)
     * on the theory that the XMB leaves the mode set -- the first hardware
     * run showed a black screen where PPSSPP showed the UI, and this is the
     * one display call the samples make that the port did not. */
    sceDisplaySetMode(0, UI_W, UI_H);
    {
        uint32_t *p = s_buf[0];
        int       i;
        for (i = 0; i < UI_FB_PIXELS; i++) { p[i] = ui_c_bg(); }
    }
    sceDisplaySetFrameBuf(s_buf[0], UI_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);

    /* setup=0: the display is ours (above); this call only points the text
     * engine at a buffer and tells it the pixel format. */
    pspDebugScreenInitEx(s_buf[s_back], PSP_DISPLAY_PIXEL_FORMAT_8888, 0);

    /* Glyphs must NOT paint their cell background. By default the debug font
     * fills each 8x8 cell with its back colour before drawing the glyph,
     * which is invisible against the app background and then very visible as
     * a black box the moment text is drawn over a filled widget -- a lit
     * button lamp came out as a solid block with no letter in it. */
    pspDebugScreenEnableBackColor(0);
    s_ready = 1;
    return 0;
}

void ui_exit(void)
{
    s_ready = 0;
}

void ui_frame_begin(void)
{
    uint32_t *p = s_buf[s_back];
    uint32_t  c = ui_c_bg();
    int       i;

    for (i = 0; i < UI_FB_PIXELS; i++) {
        p[i] = c;
    }
    /* Re-point the text engine every frame: it is the only way PutChar knows
     * which of the two buffers is currently the back one. */
    pspDebugScreenSetBase(s_buf[s_back]);
}

void ui_frame_end(void)
{
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf(s_buf[s_back], UI_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);
    s_back ^= 1;
}

/* ---- face-button glyphs --------------------------------------------------- */

static const char *const kGlyph[4][UI_GLYPH_PX] = {
    {   /* cross */
        "#.......#",
        "##.....##",
        ".##...##.",
        "..##.##..",
        "...###...",
        "..##.##..",
        ".##...##.",
        "##.....##",
        "#.......#" },
    {   /* circle */
        "...###...",
        ".##...##.",
        ".#.....#.",
        "#.......#",
        "#.......#",
        "#.......#",
        ".#.....#.",
        ".##...##.",
        "...###..." },
    {   /* triangle */
        "....#....",
        "....#....",
        "...#.#...",
        "...#.#...",
        "..#...#..",
        "..#...#..",
        ".#.....#.",
        ".#.....#.",
        "#########" },
    {   /* square */
        "#########",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#########" },
};

void ui_glyph_draw(int x, int y, ui_glyph g, uint32_t colour)
{
    int row, col;

    if ((unsigned)g >= 4u) { return; }
    for (row = 0; row < UI_GLYPH_PX; row++) {
        for (col = 0; col < UI_GLYPH_PX; col++) {
            if (kGlyph[g][row][col] == '#') {
                ui_rect(x + col, y + row, 1, 1, colour);
            }
        }
    }
}

void ui_hint(int x, int y, ui_glyph g, uint32_t colour, const char *text)
{
    ui_glyph_draw(x, y, g, colour);
    ui_text(x + UI_GLYPH_PX + 5, y + 1, colour, UI_ALIGN_LEFT, text);
}

void ui_dpad(int x, int y, int arm, unsigned dirs, uint32_t on, uint32_t off,
             uint32_t border)
{
    /* Five cells: centre and four arms. Border first, cells inset by one so
     * the cross reads as one shape with four lit regions, not five boxes. */
    ui_rect(x + arm, y, arm, 3 * arm, border);
    ui_rect(x, y + arm, 3 * arm, arm, border);
    ui_rect(x + arm + 1, y + 1,           arm - 2, arm - 1, (dirs & 1u) ? on : off);
    ui_rect(x + arm + 1, y + 2 * arm,     arm - 2, arm - 1, (dirs & 2u) ? on : off);
    ui_rect(x + 1,       y + arm + 1,     arm - 1, arm - 2, (dirs & 4u) ? on : off);
    ui_rect(x + 2 * arm, y + arm + 1,     arm - 1, arm - 2, (dirs & 8u) ? on : off);
    ui_rect(x + arm + 1, y + arm + 1,     arm - 2, arm - 2, off);
}

void ui_rect(int x, int y, int w, int h, uint32_t colour)
{
    uint32_t *fb = s_buf[s_back];
    int row, col;

    if (!s_ready || w <= 0 || h <= 0) {
        return;
    }
    /* Clip rather than trust callers: a screen that computes a box slightly
     * off-surface should look wrong, not corrupt the next buffer. */
    if (x < 0)      { w += x; x = 0; }
    if (y < 0)      { h += y; y = 0; }
    if (x + w > UI_W) { w = UI_W - x; }
    if (y + h > UI_H) { h = UI_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (row = 0; row < h; row++) {
        uint32_t *dst = fb + (size_t)(y + row) * UI_STRIDE + x;
        for (col = 0; col < w; col++) {
            dst[col] = colour;
        }
    }
}

void ui_outline(int x, int y, int w, int h, int t, uint32_t colour)
{
    if (t <= 0) {
        return;
    }
    ui_rect(x, y, w, t, colour);
    ui_rect(x, y + h - t, w, t, colour);
    ui_rect(x, y, t, h, colour);
    ui_rect(x + w - t, y, t, h, colour);
}

void ui_panel(const ui_box *b, uint32_t fill, uint32_t border)
{
    ui_rect(b->x, b->y, b->w, b->h, fill);
    ui_outline(b->x, b->y, b->w, b->h, 1, border);
}

void ui_button(const ui_box *b, const char *label, int pressed, uint32_t accent)
{
    ui_panel(b, pressed ? ui_c_panel_hi() : ui_c_panel(),
             pressed ? accent : ui_c_border());
    if (label != NULL) {
        ui_text(b->x + b->w / 2, b->y + (b->h - UI_CH) / 2,
                pressed ? ui_c_text() : ui_c_dim(), UI_ALIGN_CENTER, label);
    }
}

void ui_header(const char *title, const char *right, uint32_t right_colour)
{
    ui_rect(0, 0, UI_W, 16, ui_c_panel());
    ui_rect(0, 16, UI_W, 1, ui_c_border());
    ui_text(6, 4, ui_c_text(), UI_ALIGN_LEFT, title);
    if (right != NULL) {
        ui_text(UI_W - 6, 4, right_colour, UI_ALIGN_RIGHT, right);
    }
}

void ui_text(int x, int y, uint32_t colour, ui_align align, const char *s)
{
    int n, i, sx;

    if (!s_ready || s == NULL) {
        return;
    }
    n  = (int)strlen(s);
    sx = x;
    if (align == UI_ALIGN_CENTER) { sx = x - (n * UI_CW) / 2; }
    if (align == UI_ALIGN_RIGHT)  { sx = x - n * UI_CW; }

    for (i = 0; i < n; i++) {
        int cx = sx + i * UI_CW;
        if (cx < 0 || cx + UI_CW > UI_W || y < 0 || y + UI_CH > UI_H) {
            continue;   /* clip per glyph; the font engine does not clip */
        }
        pspDebugScreenPutChar(cx, y, colour, (unsigned char)s[i]);
    }
}

/* One scratch buffer, as clients/3ds/source/ui.c does: this is a UI, it draws
 * from one thread, and a per-call buffer on a 32 MB console is waste. */
static char s_scratch[256];

void ui_textf(int x, int y, uint32_t colour, ui_align align, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_scratch, sizeof s_scratch, fmt, ap);
    va_end(ap);
    ui_text(x, y, colour, align, s_scratch);
}

void ui_textf_fit(int x, int y, uint32_t colour, ui_align align, int max_w,
                  const char *fmt, ...)
{
    va_list ap;
    int     max_chars;

    va_start(ap, fmt);
    vsnprintf(s_scratch, sizeof s_scratch, fmt, ap);
    va_end(ap);

    max_chars = max_w / UI_CW;
    if (max_chars < 1) {
        return;
    }
    if ((int)strlen(s_scratch) > max_chars) {
        /* Truncate with an ellipsis so a clipped address reads as clipped
         * rather than as a different address. */
        if (max_chars >= 3) {
            s_scratch[max_chars - 3] = '.';
            s_scratch[max_chars - 2] = '.';
            s_scratch[max_chars - 1] = '.';
        }
        s_scratch[max_chars] = '\0';
    }
    ui_text(x, y, colour, align, s_scratch);
}

/*
 * Seven-segment digits.
 *
 *    -- a --        segments, as (x, y, w, h) in cell units:
 *   |       |         a top, b top-right, c bottom-right,
 *   f       b         d bottom, e bottom-left, f top-left, g middle
 *   |       |
 *    -- g --
 *   |       |
 *   e       c
 *   |       |
 *    -- d --
 */
static const unsigned char kSeg[10] = {
    /* 0 */ 0x3F, /* 1 */ 0x06, /* 2 */ 0x5B, /* 3 */ 0x4F, /* 4 */ 0x66,
    /* 5 */ 0x6D, /* 6 */ 0x7D, /* 7 */ 0x07, /* 8 */ 0x7F, /* 9 */ 0x6F
};

int ui_bignum_width(int cell, const char *digits)
{
    int w = 0;
    const char *p;

    for (p = digits; p != NULL && *p != '\0'; p++) {
        w += (*p == ':' || *p == '.') ? (cell * 2) : (cell * 4);
    }
    return (w > 0) ? (w - cell) : 0;   /* trailing gap is not part of it */
}

void ui_bignum(int x, int y, int cell, uint32_t colour, const char *digits)
{
    const char *p;
    int cx = x;

    if (digits == NULL || cell <= 0) {
        return;
    }
    for (p = digits; *p != '\0'; p++) {
        if (*p == ':' || *p == '.') {
            ui_rect(cx, y + cell * 3, cell, cell, colour);
            cx += cell * 2;
            continue;
        }
        if (*p >= '0' && *p <= '9') {
            unsigned s = kSeg[*p - '0'];
            if (s & 0x01) ui_rect(cx,            y,            cell * 3, cell,     colour); /* a */
            if (s & 0x02) ui_rect(cx + cell * 2, y,            cell,     cell * 3, colour); /* b */
            if (s & 0x04) ui_rect(cx + cell * 2, y + cell * 2, cell,     cell * 3, colour); /* c */
            if (s & 0x08) ui_rect(cx,            y + cell * 4, cell * 3, cell,     colour); /* d */
            if (s & 0x10) ui_rect(cx,            y + cell * 2, cell,     cell * 3, colour); /* e */
            if (s & 0x20) ui_rect(cx,            y,            cell,     cell * 3, colour); /* f */
            if (s & 0x40) ui_rect(cx,            y + cell * 2, cell * 3, cell,     colour); /* g */
        }
        cx += cell * 4;
    }
}

/*
 * DEV ONLY. Dump the buffer that was most recently PRESENTED (not the one
 * being drawn into) as a binary PPM.
 *
 * This exists because the alternative -- screenshotting the emulator's window
 * -- captures a scaled, filtered copy of the screen through a compositor,
 * which is the wrong thing to review a 480x272 layout with. ms0:/ is a plain
 * host directory under PPSSPP, so this writes the exact pixels the PSP
 * composed, and it works identically on hardware with a memory stick.
 */
int ui_dump_ppm(const char *path)
{
    /* ui_frame_end() has already flipped s_back, so the finished frame is
     * the OTHER buffer. Dumping s_back would capture the half-drawn next
     * frame -- a subtle way to review the wrong image. */
    const uint32_t *fb = s_buf[s_back ^ 1];
    FILE *f;
    int   y, x;

    if (!s_ready || (f = fopen(path, "wb")) == NULL) {
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    for (y = 0; y < UI_H; y++) {
        const uint32_t *row = fb + (size_t)y * UI_STRIDE;
        for (x = 0; x < UI_W; x++) {
            uint32_t p = row[x];                 /* 0xAABBGGRR */
            unsigned char rgb[3];
            rgb[0] = (unsigned char)( p        & 0xFF);   /* R */
            rgb[1] = (unsigned char)((p >>  8) & 0xFF);   /* G */
            rgb[2] = (unsigned char)((p >> 16) & 0xFF);   /* B */
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}
