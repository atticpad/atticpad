/* clients/nds/source/ui.c -- the DS backend behind ui.h.
 *
 * Two 16-bit bitmap backgrounds, a software rasteriser, and an 8x8 bitmap
 * font. See ui.h for the contract and for why layouts stay in 3DS
 * coordinates; this file is the machinery.
 *
 * ===========================================================================
 * THE SCREENS ARE SWAPPED, AND THAT IS THE WHOLE VRAM DESIGN
 * ===========================================================================
 * lcdMainOnBottom() puts the MAIN 2D engine on the physical BOTTOM screen and
 * the SUB engine on the top -- the same swap
 * references/nds/examples/input/touch_input/source/main.c makes with
 * lcdSwap() for the same reason: whatever wants the most background VRAM
 * should be on the touch screen.
 *
 * It is forced by the keyboard. The sub engine can only take its backgrounds
 * from VRAM C (128 KB; VRAM H and I alias the same address range, so they are
 * not additional space), and a 16-bit 256x256 bitmap is exactly 128 KB. So a
 * sub-engine screen can have a 16-bit bitmap OR the libnds keyboard's ~44 KB
 * of tiles and map, never both. The main engine has A+B+D available and can
 * have both with room to spare. The keyboard has to be on the touch screen,
 * therefore the touch screen has to be the main engine's.
 *
 *      MAIN ENGINE -> BOTTOM SCREEN, background VRAM at 0x06000000
 *        0 KB ..   4 KB   libnds keyboard map    (mapBase 0; BgSize_T_256x512
 *                                                 is 32x64 entries = 4 KB)
 *       16 KB ..  56 KB   libnds keyboard tiles  (tileBase 1; "approximately
 *                                                 40 KiB", nds/arm9/keyboard.h)
 *       64 KB .. 192 KB   bottom bitmap buffer A (bgInit mapBase 4, 16 KB unit)
 *      192 KB .. 256 KB   free
 *      256 KB .. 384 KB   bottom bitmap buffer B (mapBase 16)
 *      banks: VRAM_A @0x06000000, VRAM_B @0x06020000, VRAM_D @0x06040000
 *
 *      SUB ENGINE -> TOP SCREEN, background VRAM at 0x06200000
 *        0 KB .. 128 KB   top bitmap, single buffer (bgInitSub mapBase 0)
 *      bank: VRAM_C
 *
 * BUFFERING, and why it is asymmetric. The bottom screen is double-buffered
 * in VRAM and flipped with bgSetMapBase() at VBlank, exactly as
 * references/nds/examples/graphics_2d/bg_bmp_16bit/source/main.c does. The top
 * screen has nowhere to put a second buffer, so it is drawn straight into live
 * VRAM -- but ui_frame_begin() is the frame's only wait and it returns AT the
 * start of VBlank, so the top screen's redraw (a ~96 KB DMA fill plus a few
 * hundred rects and glyphs) happens inside the ~4.5 ms window when the beam is
 * not reading it. That is the whole anti-tearing story for the top screen and
 * it is the reason the VBlank yield is at the START of the frame here and at
 * the END on the 3DS.
 *
 * ONE SCREEN BORROWS THE BOTTOM BUFFER OUTRIGHT. The DSi camera's NDMA only
 * completes into MAIN-ENGINE background VRAM, and the main engine is this
 * screen (see ui.h's ui_bottom_camera_lock()). While the QR screen holds that
 * lock the bottom is single-buffered on BOT_BMP_BASE_A -- 0x06010000, which
 * is the camera's own NDMA destination -- ui_frame_begin() does not flip and
 * ui_screen_bottom() does not clear.
 *
 * IT IS ALSO NOT FREE, and the number is on the screen: the diagnostics
 * overlay's app_frame_ms() reads 31 ms in melonDS, i.e. the whole frame takes
 * two VBlanks and the client runs at ~32 fps. That is a 32 Hz send rate,
 * because the engine sends one INPUT_STATE per pump and this client pumps
 * once per frame. Two things were tried and CHANGED NOTHING, so neither is
 * the cause: building this file in ARM mode instead of Thumb, and removing
 * both full-screen clears entirely. Recorded as unresolved rather than
 * guessed at.
 *
 * ===========================================================================
 * THE FONT
 * ===========================================================================
 * libnds's own default console font, reached through the documented
 * consoleGetDefault() accessor (nds/arm9/console.h) rather than by declaring
 * its `default_fontTiles` symbol by hand -- the symbol is public in libnds9.a
 * but appears in no header, so going through the accessor is the difference
 * between an API call and a link-order bet. Provenance: BlocksDS libnds,
 * Zlib licence, already linked into this ROM; 96 glyphs, ASCII 32..127,
 * 1 bit per pixel, 8 bytes per glyph, one byte per row.
 *
 * BIT ORDER IS bit 0 = LEFTMOST PIXEL, and that was READ OUT OF THE DATA, not
 * recalled: '(' (glyph index 8) is 18 0c 06 06 06 0c 18 00, which under
 * bit0-left narrows towards the left in the middle (a '(') and under
 * bit0-right would narrow towards the right (a ')'). 'A' (index 33) is
 * 3e 63 63 7f 63 63 63 and only reads as an A the same way round.
 *
 * PROPORTIONAL ADVANCES, computed once at ui_init(). The glyphs live in an
 * 8x8 cell but most of them do not fill it; a fixed 8px advance would give
 * 32 characters per line where the 3DS's proportional system font fits the
 * equivalent of ~40 in the same relative width, and every copied line of
 * prose would be a third too long before it started. Measuring each glyph's
 * rightmost lit column at init and advancing to that + 2 recovers most of
 * that: 'i' and 'l' cost 3px, 'm' and 'w' cost 8.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "ui.h"

/* apad_ticks_ms()/apad_time_since() ONLY, for the wait/swap timing split in
 * ui_frame_begin() below -- added for the frame-rate pass that separates the
 * VBlank WAIT from the actual buffer-flip WORK (main.c's phase diag panel
 * used to fold both into one "flip" number, which is why "flip ~9ms" read as
 * suspicious: it is almost entirely the wait, not the register writes, and
 * there was no way to tell the two apart from outside this file). This is the
 * one place ui.c reaches into core/ rather than staying a pure rasteriser;
 * ui.h's own contract is untouched -- these are two NEW accessor functions
 * declared in app.h (which already documents itself as a DS-specific
 * superset of the 3DS's app.h), not additions to ui.h. */
#include "atticpad/atticpad.h"

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */

/* Physical framebuffer geometry. Both screens are the same size on this
 * console, unlike the 3DS. */
#define FB_W UI_DS_W
#define FB_H UI_DS_H

/* Bitmap background stride. BgSize_B16_256x256 is 256 pixels wide whatever
 * the screen shows, so a row step is 256 and NOT FB_W-by-coincidence. */
#define FB_STRIDE 256

/* Bitmap bases, in the 16 KB units bgInit()'s `mapBase` takes for a bitmap
 * background (background.h: "the 16 KB offset into vram the bitmap data will
 * be placed"). See the VRAM MAP above. */
#define BOT_BMP_BASE_A   4
#define BOT_BMP_BASE_B  16
#define TOP_BMP_BASE     0

static int s_bg_top;          /* sub engine, top screen                     */
static int s_bg_bottom;       /* main engine, bottom screen                 */
static int s_bottom_base;     /* the base currently being DISPLAYED         */
static uint16_t *s_bottom_draw;  /* the OTHER buffer -- the one to draw into */

/* Both bottom-screen buffers, resolved ONCE in ui_init() while it is already
 * toggling the displayed base to clear them. [0] is BOT_BMP_BASE_A, [1] is
 * BOT_BMP_BASE_B. The ordinary flip in ui_frame_begin() still goes through
 * bgGetGfxPtr() (the reference sample's own shape -- see the comment there);
 * these exist so the camera lock and the unlock's one restore frame can name
 * a buffer WITHOUT writing the display register to ask libnds for its
 * address, which would put the other buffer on screen for the duration of the
 * question. Address check, from libnds source/arm9/video/background.c's
 * bgGetGfxPtr(): a bitmap background's base is `BG_GFX + 0x2000 * mapBase`
 * with BG_GFX a u16* at 0x06000000, i.e. 16 KB per mapBase unit -- so [0] is
 * 0x06000000 + 4*0x4000 = 0x06010000 and [1] is 0x06000000 + 16*0x4000 =
 * 0x06040000. Both are in the MAIN engine's range, which is the whole point
 * of the camera lock below. */
static uint16_t *s_bottom_buf[2];

/* THE CAMERA LOCK (ui.h). While `s_bottom_locked`, the bottom screen is
 * single-buffered on BOT_BMP_BASE_A and nothing here clears it: the DSi
 * camera's NDMA is writing into it continuously. `s_bottom_restore` is the
 * one-frame handover on the way out -- see ui_bottom_camera_unlock(). */
static int s_bottom_locked;
static int s_bottom_restore;

static uint16_t *s_fb;        /* the surface the drawing calls write to     */

/* THE WAIT/SWAP SPLIT (see the include above). ui_frame_begin() measures
 * itself: `wait` is cothread_yield_irq(IRQ_VBLANK) alone, `swap` is the
 * pointer read + bgSetMapBase() register write after it. Read by
 * ui_flip_wait_ms()/ui_flip_swap_ms() (declared in app.h), which main.c
 * feeds into the same phase table app_frame_ms() already builds. apad_ticks_ms()
 * is a 1ms tick (time_nds.c), so `swap` reading 0 on almost every frame is
 * the EXPECTED, correct answer -- it says the register write really is free,
 * rather than leaving that as an assumption. */
static uint32_t s_flip_wait_ms;
static uint32_t s_flip_swap_ms;

/* Same idea, one call: how long clear_surface() itself took the last time it
 * ran. Exists to answer, with a number instead of a guess, whether the
 * full-screen DMA clear is where a frame's time goes -- see ui_last_clear_ms()
 * in app.h and this file's clear_surface(). */
static uint32_t s_last_clear_ms;

/* 3DS -> DS horizontal scale for the surface currently selected, as a
 * fraction. Vertical is always 4/5 (240 -> 192) on both screens. */
static int s_xnum = 4, s_xden = 5;

/* Shared formatting scratch, exactly as clients/3ds/source/ui.c: everything
 * in this client runs on the main cothread. */
static char s_scratch[256];

/* The font, filled by ui_init(). */
#define FONT_FIRST 32
#define FONT_COUNT 96
static const uint8_t *s_font;
static uint8_t s_adv[FONT_COUNT];

/* ------------------------------------------------------------------------ */
/* palette                                                                  */
/* ------------------------------------------------------------------------ */

/* Packing matches citro2d's C2D_Color32 so the copied files' colours mean
 * the same thing: byte 0 red, byte 1 green, byte 2 blue, byte 3 alpha. */
#define UI_RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) \
     | ((uint32_t)(a) << 24))

uint32_t ui_c_bg(void)       { return UI_RGBA(0x12, 0x16, 0x1E, 0xFF); }
uint32_t ui_c_panel(void)    { return UI_RGBA(0x1E, 0x26, 0x33, 0xFF); }
uint32_t ui_c_panel_hi(void) { return UI_RGBA(0x30, 0x3E, 0x52, 0xFF); }
uint32_t ui_c_border(void)   { return UI_RGBA(0x3C, 0x4C, 0x64, 0xFF); }
uint32_t ui_c_text(void)     { return UI_RGBA(0xE6, 0xEC, 0xF4, 0xFF); }
uint32_t ui_c_dim(void)      { return UI_RGBA(0x87, 0x96, 0xAB, 0xFF); }
uint32_t ui_c_accent(void)   { return UI_RGBA(0x4C, 0xC2, 0xFF, 0xFF); }
uint32_t ui_c_good(void)     { return UI_RGBA(0x5C, 0xD6, 0x7A, 0xFF); }
uint32_t ui_c_warn(void)     { return UI_RGBA(0xFF, 0xC4, 0x4C, 0xFF); }
uint32_t ui_c_bad(void)      { return UI_RGBA(0xFF, 0x6B, 0x5B, 0xFF); }

/* 8-bit-per-channel -> the hardware's 5-5-5 plus the "not transparent" bit.
 * Called once per drawing call, never inside a pixel loop. */
uint16_t ui_rgb15(uint32_t c)
{
    unsigned r = (c >>  0) & 0xFFu;
    unsigned g = (c >>  8) & 0xFFu;
    unsigned b = (c >> 16) & 0xFFu;

    return (uint16_t)(0x8000u | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* ------------------------------------------------------------------------ */
/* coordinates                                                              */
/* ------------------------------------------------------------------------ */

/* THE ONLY float -> int conversions in this file, and they all happen here,
 * on the way in from a public entry point. Nothing below this line does float
 * arithmetic. */
static int f2i(float v)
{
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static int sx(int v) { return (v * s_xnum) / s_xden; }
static int sy(int v) { return (v * 4) / 5; }

/* A 3DS width/height turned into a DS one that is never zero when the source
 * was not: the copied layouts draw 1px separators and outlines, and 1 * 0.8
 * truncates to 0 -- an invisible border where the 3DS has a visible one. */
static int span(int a, int b)
{
    return (b > a) ? (b - a) : 1;
}

/* ------------------------------------------------------------------------ */
/* raster                                                                   */
/* ------------------------------------------------------------------------ */

/* ===========================================================================
 * A LONG DMA BURST KILLS THE DSi CAMERA. THIS IS THE CAP.
 * ===========================================================================
 * The camera's NDMA and nds/dma.h's classic DMA are different units, but they
 * arbitrate for the same bus, and the classic one WINS for as long as its
 * burst lasts -- libnds's dmaFillWords()/dmaFillHalfWords()/dmaCopy() each
 * busy-wait on dmaBusy() until the whole transfer is done. The DSi camera
 * cannot wait: it hands the transfer 512 words at a time out of a two-buffer
 * ping-pong, and if the NDMA has not drained a buffer by the time the sensor
 * fills the other one, the camera block raises its overrun bit and STOPS --
 * permanently, with the frame half delivered. cameraStartTransfer() clears
 * only bits 0-3 and 15 of REG_CAM_CNT, so the error bit survives even a
 * re-arm; the transfer never restarts, ndmaBusy() and cameraTransferActive()
 * both stay true forever, and camera_nds.c's poll never sees a completion
 * again.
 *
 * MEASURED, NOT REASONED: with clear_surface()'s single 96 KB dmaFillWords()
 * running on the top screen every other frame, the QR screen's own diagnostic
 * line read `frames 1  decodes 0` with the bottom screen showing exactly the
 * first 88 of 192 rows of one camera frame -- the transfer died mid-frame at
 * the first full-screen fill. The same code with the cap below reaches a
 * climbing frame counter and decodes. The mechanism is confirmed in melonDS's
 * own DSi_Camera.cpp (`SwapPixelBuffers()`: "the swap fails if the other
 * buffer isn't empty" -> `Cnt |= (1<<4); Transferring = false;`) and the
 * overrun bit it sets is real DSi hardware, not an emulator invention -- so
 * this is a hardware hazard that happened to be reproducible here, not a
 * workaround for the emulator.
 *
 * THE NUMBER. The camera swaps buffers every CAM_CNT_SCANLINES(4) = 4
 * scanlines. A burst that fits comfortably inside that window cannot starve
 * it; 512 halfwords is two full rows of this surface, an order of magnitude
 * under the budget, and the per-burst register setup it costs is invisible
 * next to the fill itself. The cap applies ONLY while the camera holds the
 * bottom screen (ui_bottom_camera_lock()), so every other screen's drawing is
 * byte-for-byte the single-burst path it has always been. */
#define CAM_DMA_MAX_HW 512u

static void dma_fill(uint16_t col, uint16_t *dst, uint32_t halfwords)
{
    if (!s_bottom_locked) {
        dmaFillHalfWords(col, dst, halfwords * 2u);
        return;
    }
    while (halfwords > 0u) {
        uint32_t n = (halfwords > CAM_DMA_MAX_HW) ? CAM_DMA_MAX_HW : halfwords;

        dmaFillHalfWords(col, dst, n * 2u);
        dst += n;
        halfwords -= n;
    }
}

static void dma_blit(const uint16_t *src, uint16_t *dst, uint32_t halfwords)
{
    if (!s_bottom_locked) {
        dmaCopy(src, dst, halfwords * 2u);
        return;
    }
    while (halfwords > 0u) {
        uint32_t n = (halfwords > CAM_DMA_MAX_HW) ? CAM_DMA_MAX_HW : halfwords;

        dmaCopy(src, dst, n * 2u);
        src += n;
        dst += n;
        halfwords -= n;
    }
}

/* Every rectangle in this file goes through here, so clipping is stated once.
 * A 400x240 layout squeezed into 256x192 genuinely does overflow in places
 * (see ui.h), and an unclipped blit would be writing past the end of a VRAM
 * bank.
 *
 * THE 32-BIT PAIRED-STORE INNER LOOP WAS NOT PREMATURE WHEN IT WAS ADDED --
 * see the git history for the half-drawn-lamps artefact that motivated it --
 * but it is no longer the fast path for anything wide enough to matter. The
 * frame-rate pass that follows shim/net_nds.c (2026-09-09) MEASURED (SELECT's
 * diag panel, before/after, same PAD screen, same 32-frame average -- not
 * guessed) that routing kModeStrip's full-width fill through DMA instead of
 * this CPU loop alone took draw_bottom_ms from ~10ms to ~6ms on a screen that
 * makes exactly TWO full-width fill_px() calls a frame. Given that ratio,
 * every ROW-SIZED fill in this file -- a button, a panel, a KEYS cell -- is
 * now routed through nds/dma.h's dmaFillHalfWords() too, one DMA burst per
 * row, rather than the paired 32-bit CPU stores. This says something about
 * this console's actual VRAM timing that the header comment above (32-bit
 * stores "roughly halve the bus traffic") did not capture: a CPU store to
 * VRAM costs more than the raw bus-cycle count suggests, almost certainly
 * because the video hardware's own VRAM fetch has priority over the CPU
 * during active display, and DMA does not pay that per-STORE penalty the way
 * a tight CPU loop does. UNVERIFIED beyond that: this is melonDS's DMA/VRAM
 * timing model, not measured on real hardware, and is exactly the kind of
 * platform behaviour flagged as unverified in this pass's report.
 *
 * THE THRESHOLD. A 1px-wide vertical outline strip (ui_outline()'s two side
 * strips, called on every panel and button in this file) is NOT contiguous
 * in VRAM row-to-row -- each row is FB_STRIDE halfwords from the last -- so
 * it is h separate one-halfword transfers either way, and a DMA's own
 * register-setup-then-poll cost is worse than a single CPU store for that
 * shape. FILL_DMA_MIN_W draws the line: rows narrower than that stay on the
 * CPU path (still paired 32-bit where the width allows it), rows at or above
 * it go through DMA. 8 is not tuned per se -- it just excludes the common 1px
 * and 2px outline/separator cases from paying DMA setup for almost nothing to
 * transfer, while catching every button, panel, glyph background and cell
 * fill in this client, which are all >=17px wide. */
#define FILL_DMA_MIN_W 8
static void fill_px(int x, int y, int w, int h, uint16_t col)
{
    uint32_t pair;
    int yy;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < 0)  { w += x; x = 0; }
    if (y < 0)  { h += y; y = 0; }
    if (x + w > FB_W) { w = FB_W - x; }
    if (y + h > FB_H) { h = FB_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    pair = ((uint32_t)col << 16) | (uint32_t)col;

    /* FULL-WIDTH FAST PATH. When a fill spans x=0..FB_W, every one of its
     * rows is CONTIGUOUS in VRAM (FB_STRIDE == FB_W here, unlike a bitmap
     * background that is wider than its visible area), so the whole h*FB_W
     * halfword region is one DMA fill instead of h separate CPU row loops.
     * dmaFillHalfWords() is nds/dma.h's own hardware fill (see references/
     * nds/); it blocks until done, so this is a drop-in replacement with
     * identical pixels, not an async hazard. Triggers on the mode strip and
     * any other caller that fills a full-width band (kModeStrip is the one
     * that matters today: every mode draws it every frame). Rects narrower
     * than the full width -- almost everything else in this file -- still go
     * through the row loop below, because a per-row DMA for a ~100px-wide
     * button would pay the DMA setup/poll cost more often than the paired
     * 32-bit stores it would replace. */
    if (x == 0 && w == FB_W) {
        dma_fill(col, s_fb + (size_t)y * FB_STRIDE,
                 (uint32_t)h * FB_STRIDE);
        return;
    }

    if (w >= FILL_DMA_MIN_W) {
        for (yy = 0; yy < h; yy++) {
            uint16_t *p = s_fb + (size_t)(y + yy) * FB_STRIDE + (size_t)x;

            dma_fill(col, p, (uint32_t)w);
        }
        return;
    }

    for (yy = 0; yy < h; yy++) {
        uint16_t *p = s_fb + (size_t)(y + yy) * FB_STRIDE + (size_t)x;
        int n = w;

        /* The row start is only 32-bit aligned on an even x; fix that up with
         * one halfword, then run 32-bit stores. FB_STRIDE is even, so the
         * alignment of a row start depends on x alone. */
        if (((x & 1) != 0) && n > 0) {
            *p++ = col;
            n--;
        }
        while (n >= 2) {
            *(uint32_t *)(void *)p = pair;
            p += 2;
            n -= 2;
        }
        if (n > 0) {
            *p = col;
        }
    }
}

/* See app.h for why this exists outside ui.h's frozen contract and why `x`/
 * `y`/`w`/`h` are RAW DS PIXELS rather than 3DS-scaled coordinates. `src` is
 * `h` rows of `src_stride` PIXELS (not bytes) each, already in the native
 * BGR555 this surface stores (ui_rgb15()'s own packing), so there is no per-
 * pixel conversion here -- only clipping and a row copy, exactly fill_px()'s
 * own shape and threshold (FILL_DMA_MIN_W) for the same reason: a DMA's
 * setup/poll cost is not worth paying for a handful of pixels, but every row
 * this screen actually blits (APAD_NDS_CAM_W == 256, camera_nds.h) is the
 * full surface width and always qualifies. */
void ui_blit_rgb15(int x, int y, const uint16_t *src, int w, int h,
                   int src_stride)
{
    int sx0 = 0, sy0 = 0;
    int yy;

    if (src == NULL || w <= 0 || h <= 0 || src_stride < w) {
        return;
    }
    if (x < 0)  { sx0 = -x; w += x; x = 0; }
    if (y < 0)  { sy0 = -y; h += y; y = 0; }
    if (x + w > FB_W) { w = FB_W - x; }
    if (y + h > FB_H) { h = FB_H - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (yy = 0; yy < h; yy++) {
        const uint16_t *srow = src + (size_t)(sy0 + yy) * (size_t)src_stride
                              + (size_t)sx0;
        uint16_t *drow = s_fb + (size_t)(y + yy) * FB_STRIDE + (size_t)x;

        if (w >= FILL_DMA_MIN_W) {
            dma_blit(srow, drow, (uint32_t)w);
        } else {
            int i;

            for (i = 0; i < w; i++) {
                drow[i] = srow[i];
            }
        }
    }
}

static void clear_surface(uint32_t colour)
{
    uint16_t c = ui_rgb15(colour);
    uint32_t pair = ((uint32_t)c << 16) | (uint32_t)c;
    uint32_t t0 = apad_ticks_ms();

    /* The visible area only: the bitmap is 256x256 but rows 192..255 are
     * never displayed, and filling them would be a third more work per frame
     * for nothing. */
    if (s_bottom_locked) {
        /* THE ONE THAT KILLED THE CAMERA -- see dma_fill()'s comment above.
         * 96 KB in a single burst is ~24576 bus words with no gap for the
         * camera's NDMA to drain its 512-word buffer in. Chunked, the pixels
         * are identical. */
        dma_fill(c, s_fb, (uint32_t)(FB_STRIDE * FB_H));
    } else {
        dmaFillWords(pair, s_fb, (uint32_t)(FB_STRIDE * FB_H * 2));
    }
    s_last_clear_ms = apad_time_since(apad_ticks_ms(), t0);
}

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                */
/* ------------------------------------------------------------------------ */

static void font_measure(void)
{
    int g;

    for (g = 0; g < FONT_COUNT; g++) {
        const uint8_t *rows = s_font + (size_t)g * 8u;
        int right = -1;
        int r;

        for (r = 0; r < 8; r++) {
            unsigned bits = rows[r];
            int b;

            for (b = 7; b > right; b--) {
                if (bits & (1u << b)) {
                    right = b;
                    break;
                }
            }
        }
        /* +2: one column of ink-free gap, one for the pixel after the last
         * lit one. A blank glyph (space) gets a fixed 4, since "as wide as
         * nothing" would run words together. */
        s_adv[g] = (uint8_t)((right >= 0) ? (right + 2) : 4);
    }
}

int ui_init(void)
{
    const PrintConsole *def = consoleGetDefault();

    if (def == NULL || def->font.gfx == NULL) {
        return 0;
    }
    s_font = (const uint8_t *)def->font.gfx;
    font_measure();

    /* Main engine on the touch screen -- see this file's header comment. */
    lcdMainOnBottom();

    /* Mode 5 on both engines: BG0/BG1 text, BG2/BG3 extended (which is what a
     * bitmap background needs). The keyboard takes BG0 of the main engine
     * (kbd_nds.c), the bitmaps take BG3 of each. */
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);

    vramSetPrimaryBanks(VRAM_A_MAIN_BG_0x06000000,
                        VRAM_B_MAIN_BG_0x06020000,
                        VRAM_C_SUB_BG_0x06200000,
                        VRAM_D_MAIN_BG_0x06040000);

    s_bg_bottom = bgInit(3, BgType_Bmp16, BgSize_B16_256x256,
                         BOT_BMP_BASE_A, 0);
    s_bg_top    = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256,
                            TOP_BMP_BASE, 0);
    if (s_bg_bottom < 0 || s_bg_top < 0) {
        return 0;
    }
    s_bottom_base = BOT_BMP_BASE_A;

    /* Both buffers of the bottom screen and the top screen's single one start
     * as whatever was in VRAM, which on a freshly-booted console is noise.
     * Clear all three before the first frame is presented. */
    s_fb = bgGetGfxPtr(s_bg_top);
    clear_surface(ui_c_bg());

    bgSetMapBase(s_bg_bottom, BOT_BMP_BASE_B);
    s_fb = bgGetGfxPtr(s_bg_bottom);
    s_bottom_buf[1] = s_fb;
    clear_surface(ui_c_bg());
    bgSetMapBase(s_bg_bottom, BOT_BMP_BASE_A);
    s_fb = bgGetGfxPtr(s_bg_bottom);
    s_bottom_buf[0] = s_fb;
    clear_surface(ui_c_bg());
    s_bottom_locked = 0;
    s_bottom_restore = 0;
    /* Sane until the first ui_frame_begin() swaps it -- nothing should draw
     * before then, but a NULL here would be a crash rather than a wrong
     * pixel. */
    s_bottom_draw = s_fb;

    bgUpdate();
    return 1;
}

void ui_exit(void)
{
    /* Nothing to release: no allocation, and the video hardware is torn down
     * by the loader on return from main(). */
}

void ui_frame_begin(void)
{
    uint32_t t0, t1;

    /* cothread_yield_irq(IRQ_VBLANK), NEVER swiWaitForVBlank(): the latter
     * HALTs the ARM9 with DSWiFi's lwIP cothread behind it. This is the
     * frame's only wait -- and, per the timing split below, almost the
     * entire cost main.c used to book to "flip". */
    t0 = apad_ticks_ms();
    cothread_yield_irq(IRQ_VBLANK);
    t1 = apad_ticks_ms();

    /* THE FLIP, IN THE SAMPLE'S EXACT ORDER, and the order is the whole point.
     * references/nds/examples/graphics_2d/bg_bmp_16bit/source/main.c takes the
     * pointer FIRST and switches the displayed base SECOND:
     *
     *     uint16_t *backbuffer = bgGetGfxPtr(bg);
     *     if (bgGetMapBase(bg) == 8) bgSetMapBase(bg, 0); else bgSetMapBase(bg, 8);
     *
     * so `backbuffer` names the buffer that STOPS being displayed on the next
     * line -- the one it is safe to draw into. Doing it the other way round
     * compiles, runs, and draws every frame into the buffer the hardware is
     * scanning out at that moment: the console then shows the clear and every
     * rectangle appearing live, and a screenshot catches the frame half
     * drawn. That is not hypothetical, it is what this file did for its first
     * three test runs (bottom screen blank below whatever line the draw had
     * reached). */
    if (s_bottom_locked) {
        /* THE CAMERA OWNS THE BOTTOM SCREEN (ui.h). No flip: the NDMA
         * transfer camera_nds.c arms is pointed at ONE fixed VRAM address and
         * a flip would move the displayed buffer out from under it every
         * frame -- half the frames would show the buffer the camera is not
         * writing. The displayed base stays BOT_BMP_BASE_A and drawing calls
         * (which this screen makes none of on the bottom) land on the live
         * image rather than behind it. */
        s_bottom_draw = s_bottom_buf[0];
    } else if (s_bottom_restore) {
        /* THE ONE-FRAME HANDOVER OUT OF THE LOCK. Keep showing the camera's
         * last image (base A, still displayed) for this one frame and draw
         * the returning screen into B instead. Without it, this frame's
         * normal flip would present B -- which still holds whatever was on
         * screen before the camera screen was entered -- for one visible
         * frame. Next frame the ordinary path below resumes and presents the
         * B we are about to draw. */
        s_bottom_restore = 0;
        s_bottom_draw = s_bottom_buf[1];
    } else {
        s_bottom_draw = bgGetGfxPtr(s_bg_bottom);
        s_bottom_base = (s_bottom_base == BOT_BMP_BASE_A) ? BOT_BMP_BASE_B
                                                          : BOT_BMP_BASE_A;
        bgSetMapBase(s_bg_bottom, s_bottom_base);
    }

    s_flip_wait_ms = apad_time_since(t1, t0);
    s_flip_swap_ms = apad_time_since(apad_ticks_ms(), t1);
}

uint32_t ui_flip_wait_ms(void) { return s_flip_wait_ms; }
uint32_t ui_flip_swap_ms(void) { return s_flip_swap_ms; }
uint32_t ui_last_clear_ms(void) { return s_last_clear_ms; }

void ui_screen_top(void)
{
    s_fb = bgGetGfxPtr(s_bg_top);
    s_xnum = 256;      /* 400 -> 256 */
    s_xden = 400;
    clear_surface(ui_c_bg());
}

void ui_screen_bottom(void)
{
    s_fb = s_bottom_draw;
    s_xnum = 4;        /* 320 -> 256 */
    s_xden = 5;
    if (s_bottom_locked) {
        /* The camera's NDMA is filling these pixels right now (ui.h). The
         * surface is still selected, so a caller that WANTS to composite over
         * the live image can; what is skipped is only the clear. main.c calls
         * this unconditionally every frame, which is exactly why the skip has
         * to live here rather than at the one call site that cares. */
        return;
    }
    clear_surface(ui_c_bg());
}

uint16_t *ui_bottom_camera_lock(void)
{
    if (!s_bottom_locked) {
        /* Clear FIRST, then present it: the other order would put buffer A's
         * stale contents on screen for the millisecond the clear takes. */
        s_fb = s_bottom_buf[0];
        clear_surface(ui_c_bg());

        bgSetMapBase(s_bg_bottom, BOT_BMP_BASE_A);
        s_bottom_base = BOT_BMP_BASE_A;
        s_bottom_draw = s_bottom_buf[0];
        s_bottom_restore = 0;
        s_bottom_locked = 1;
    }
    return s_bottom_buf[0];
}

void ui_bottom_camera_unlock(void)
{
    if (s_bottom_locked) {
        s_bottom_locked = 0;
        s_bottom_restore = 1;   /* consumed by the next ui_frame_begin() */
    }
}

void ui_frame_end(void)
{
    /* bgSetMapBase() writes the control register directly (bg_bmp_16bit never
     * calls bgUpdate() either), so this is only here to flush any shadowed
     * scroll/affine state the keyboard layer may have touched. */
    bgUpdate();
}

/* Stereo: one eye on this console. */
int   ui_stereo_slider_active(void) { return 0; }
void  ui_screen_top_right(void)     { }
int   ui_top_pass_is_first(void)    { return 1; }
float ui_stereo_eye_shift(void)     { return 0.0f; }

/* ------------------------------------------------------------------------ */
/* boxes                                                                    */
/* ------------------------------------------------------------------------ */

int ui_box_hit(const ui_box *b, int px, int py)
{
    /* Raw DS pixels in, 320x240 bottom-screen space out: the inverse of
     * ui_screen_bottom()'s 4/5. Done on the TOUCH POINT rather than on the
     * box so that the box stays the copied file's own numbers. */
    int x = (px * 5) / 4;
    int y = (py * 5) / 4;
    int bx = f2i(b->x), by = f2i(b->y);
    int bw = f2i(b->w), bh = f2i(b->h);

    return (x >= bx && x < bx + bw && y >= by && y < by + bh);
}

/* ------------------------------------------------------------------------ */
/* font                                                                     */
/* ------------------------------------------------------------------------ */

/* The 3DS's continuous `scale` mapped to this console's three faces.
 *
 * The thresholds are set by INK HEIGHT, not by the 3DS's 30px line box: the
 * system font's visible cap height is roughly 55% of that box, so a 3DS
 * UI_S_SMALL glyph is ~8px of ink, which after the DS's own 0.8 shrink is
 * ~7px -- the same as this 8x8 face's ink. That is why everything from
 * UI_S_TINY through UI_S_BODY lands on the same face and only the two big
 * scales step up.
 *
 * `bold` (the glyph drawn a second time one pixel to the right) is how
 * UI_S_HEAD stays distinguishable from UI_S_BODY when both are the same
 * 8 pixels tall. It is a deviation from the 3DS, listed as such in the
 * report: there, a heading is genuinely larger. */
static void face_for(float scale, int *out_px, int *out_bold)
{
    if (scale >= 1.45f) {
        *out_px = 3; *out_bold = 0;
    } else if (scale >= 0.95f) {
        *out_px = 2; *out_bold = 0;
    } else {
        *out_px = 1; *out_bold = (scale >= 0.65f) ? 1 : 0;
    }
}

/* The 3DS line box (30 * scale) in DS pixels (* 0.8). Used only to place the
 * glyph vertically inside the box the caller thinks it is drawing into. */
static int line_box_px(float scale)
{
    int h = f2i(scale * 24.0f);

    return (h > 0) ? h : 1;
}

static int text_width(const char *s, int px, int bold)
{
    int w = 0;

    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        int g = (c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT)
                ? (c - FONT_FIRST) : ('?' - FONT_FIRST);

        w += s_adv[g] * px + bold;
    }
    return w;
}

/* One glyph, blitted directly.
 *
 * DELIBERATELY NOT fill_px() PER LIT PIXEL, which is what this function used
 * to be: a screen of text is a few thousand lit pixels and a call with a full
 * clip test on each of them is what pushed the top screen's redraw past the
 * VBlank window (see fill_px()'s comment for the artifact that produced).
 * Here the row is clipped once, the row pointer is computed once, and the
 * inner loop is a store.
 *
 * RUN-MERGED FOR px>=2 ONLY, added alongside fill_px()'s DMA path in the same
 * pass and DELIBERATELY NOT applied to px==1. face_for() only ever sets
 * `bold` alongside px==1 (its px==2/px==3 branches both hard-code bold=0), so
 * px>=2 never needs to reason about the bold overlap below -- adjacent lit
 * glyph columns b, b+1 map to a genuinely CONTIGUOUS physical run (b*px ..
 * (b+1)*px, no overlap). This walks each row once, finds each run of
 * contiguous lit columns, and issues ONE store sequence per run instead of
 * one store per lit pixel -- a DMA fill for runs at or above FILL_DMA_MIN_W
 * (fill_px()'s own threshold, for the same per-store-vs-DMA cost this pass
 * measured), a plain loop below it. A 3px-wide digit stroke (UI_S_HUGE, the
 * RTT figure) with 3+ lit columns is already a 9px run and lands on the DMA
 * side. px==1 (UI_S_TINY/SMALL/BODY/HEAD -- nearly all of this client's text)
 * is EXCLUDED on purpose: MEASURED (SELECT's diag panel, before/after, same
 * screen) to be a WASH AT BEST for that case -- an 8x8 glyph's lit runs at
 * px==1 are mostly 1-2 bits wide, so the run-length scan this adds costs more
 * than the single-pixel loop it would fall back to anyway, since FILL_DMA_MIN_W
 * (8) is rarely reached. The plain path below, unchanged from before this
 * pass, stays the one every ordinary label goes through. */
static void draw_glyph(int x, int y, int g, int px, int bold, uint16_t col)
{
    const uint8_t *rows = s_font + (size_t)g * 8u;
    int w = px + bold;
    int r;

    if (x >= FB_W || x + 8 * px + bold <= 0) {
        return;
    }

    if (px >= 2) {
        for (r = 0; r < 8; r++) {
            unsigned bits = rows[r];
            int yy0 = y + r * px;
            int sub;

            if (bits == 0u) {
                continue;
            }
            for (sub = 0; sub < px; sub++) {
                int yy = yy0 + sub;
                uint16_t *line;
                int b = 0;

                if (yy < 0 || yy >= FB_H) {
                    continue;
                }
                line = s_fb + (size_t)yy * FB_STRIDE;
                while (b < 8) {
                    int runlen, xx0, ww;

                    if ((bits & (1u << b)) == 0u) {
                        b++;
                        continue;
                    }
                    runlen = 1;
                    while (b + runlen < 8
                           && (bits & (1u << (b + runlen))) != 0u) {
                        runlen++;
                    }
                    xx0 = x + b * px;
                    ww = runlen * px;
                    if (xx0 < 0) { ww += xx0; xx0 = 0; }
                    if (xx0 + ww > FB_W) { ww = FB_W - xx0; }
                    if (ww > 0) {
                        if (ww >= FILL_DMA_MIN_W) {
                            dma_fill(col, line + xx0, (uint32_t)ww);
                        } else {
                            int i;

                            for (i = 0; i < ww; i++) {
                                line[xx0 + i] = col;
                            }
                        }
                    }
                    b += runlen;
                }
            }
        }
        return;
    }

    /* THE px==1 PATH, unchanged from before this pass (see the comment above):
     * covers UI_S_TINY/SMALL/BODY (bold==0) and UI_S_HEAD (bold==1), where a
     * +1px bold repeat can make column b+1's ink start before column b's
     * ends, so runs are not simply concatenable the way they are at px>=2.
     * One store per lit pixel -- this is the overwhelming majority of this
     * client's text, and the pass MEASURED that run-merging it is not worth
     * doing, so it deliberately still isn't. */
    for (r = 0; r < 8; r++) {
        unsigned bits = rows[r];
        int sub;

        if (bits == 0u) {
            continue;
        }
        for (sub = 0; sub < px; sub++) {
            int yy = y + r * px + sub;
            uint16_t *line;
            int b;

            if (yy < 0 || yy >= FB_H) {
                continue;
            }
            line = s_fb + (size_t)yy * FB_STRIDE;
            for (b = 0; b < 8; b++) {
                int x0, i;

                if ((bits & (1u << b)) == 0u) {
                    continue;
                }
                x0 = x + b * px;
                for (i = 0; i < w; i++) {
                    int xx = x0 + i;

                    if (xx >= 0 && xx < FB_W) {
                        line[xx] = col;
                    }
                }
            }
        }
    }
}

/* Draws `s` with its LEFT edge at DS pixel (x, y). Stops at the right screen
 * edge -- see fill_px()'s comment. */
static void draw_string(int x, int y, const char *s, int px, int bold,
                        uint16_t col)
{
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        int g = (c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT)
                ? (c - FONT_FIRST) : ('?' - FONT_FIRST);

        if (x >= FB_W) {
            return;
        }
        if (c != ' ') {
            draw_glyph(x, y, g, px, bold, col);
        }
        x += s_adv[g] * px + bold;
    }
}

/* The shared tail of ui_textf()/ui_textf_fit()/ui_bignum(): s_scratch is
 * already formatted. `max_w_ds` <= 0 means "do not shrink or truncate".
 *
 * THE FIT RULE, and where it stops matching the 3DS. The 3DS multiplies its
 * scale by max_w/measured_w and carries on down to a 0.30 floor, because its
 * font is a vector one. Here there are three faces and no fourth: step down
 * through them, and when the smallest one still does not fit, TRUNCATE the
 * string to the characters that do. Truncating rather than overflowing is the
 * same intent -- text must not run off the screen -- reached the only way an
 * 8x8 bitmap face can reach it. */
static void draw_scratch(float x, float y, float scale, uint32_t colour,
                         int align, int max_w_ds)
{
    int px, bold;
    int dx, dy, w;
    uint16_t col = ui_rgb15(colour);

    face_for(scale, &px, &bold);
    dy = sy(f2i(y)) + (line_box_px(scale) - 8 * px) / 2;
    if (dy < 0) {
        dy = 0;
    }

    w = text_width(s_scratch, px, bold);
    if (max_w_ds > 0) {
        while (w > max_w_ds && px > 1) {
            px--;
            bold = 0;
            w = text_width(s_scratch, px, bold);
        }
        if (w > max_w_ds) {
            /* Truncate. Walk back from the end rather than forward from the
             * start so a right- or centre-aligned string still lands where
             * the caller put it. */
            size_t n = strlen(s_scratch);

            while (n > 0u && w > max_w_ds) {
                n--;
                s_scratch[n] = '\0';
                w = text_width(s_scratch, px, bold);
            }
        }
    }

    dx = sx(f2i(x));
    if (align == UI_ALIGN_CENTER) {
        dx -= w / 2;
    } else if (align == UI_ALIGN_RIGHT) {
        dx -= w;
    }
    draw_string(dx, dy, s_scratch, px, bold, col);
}

/* Formats into s_scratch, with two fast paths that matter a great deal here.
 *
 * newlib's vsnprintf is a general-purpose formatter with float support linked
 * in, and on a 67 MHz ARM9 it costs a few hundred microseconds per call. A
 * busy frame of this client makes forty to sixty text calls across two
 * screens, which is milliseconds of the ~16 ms a 60 Hz frame has -- and the
 * frame loop IS the send rate on this client (one pump per frame), so that
 * comes straight off the controller's polling rate.
 *
 * Most of those calls are one of two shapes: a bare literal with no
 * conversions at all, or the `"%s", str` form that ui_textf_fit()'s signature
 * pushes every caller into. Both are a string copy. Anything else still goes
 * to vsnprintf, so no format behaviour changes. */
static void format_scratch(const char *fmt, va_list ap)
{
    if (fmt[0] == '%' && fmt[1] == 's' && fmt[2] == '\0') {
        const char *src = va_arg(ap, const char *);
        size_t n;

        if (src == NULL) {
            src = "(null)";
        }
        n = strlen(src);
        if (n > sizeof s_scratch - 1u) {
            n = sizeof s_scratch - 1u;
        }
        memcpy(s_scratch, src, n);
        s_scratch[n] = '\0';
        return;
    }
    if (strchr(fmt, '%') == NULL) {
        size_t n = strlen(fmt);

        if (n > sizeof s_scratch - 1u) {
            n = sizeof s_scratch - 1u;
        }
        memcpy(s_scratch, fmt, n);
        s_scratch[n] = '\0';
        return;
    }
    vsnprintf(s_scratch, sizeof s_scratch, fmt, ap);
}

void ui_textf(float x, float y, float scale, uint32_t colour, int align,
              const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    format_scratch(fmt, ap);
    va_end(ap);
    draw_scratch(x, y, scale, colour, align, 0);
}

void ui_textf_fit(float x, float y, float scale, uint32_t colour, int align,
                  float max_w, const char *fmt, ...)
{
    va_list ap;
    int max_w_ds;

    va_start(ap, fmt);
    format_scratch(fmt, ap);
    va_end(ap);

    /* max_w arrives in the caller's 3DS pixels like every other coordinate,
     * so it shrinks by the same factor the layout does. */
    max_w_ds = sx(f2i(max_w));
    draw_scratch(x, y, scale, colour, align, max_w_ds);
}

void ui_bignum(float x, float y, uint32_t colour, int align, const char *fmt,
               ...)
{
    va_list ap;

    va_start(ap, fmt);
    format_scratch(fmt, ap);
    va_end(ap);
    draw_scratch(x, y, UI_S_HUGE, colour, align, 0);
}

/* ------------------------------------------------------------------------ */
/* word wrap                                                                 */
/* ------------------------------------------------------------------------ */

/* One line's worth of scratch, built word by word. 96 is generous: at this
 * screen's narrowest usable width (UI_BOT_W - 16, scaled) even the 8x8 face's
 * widest glyphs do not fit half that many characters on one physical line, so
 * this is headroom, not a tight budget -- and it is a stack buffer, not an
 * allocation. */
#define WRAP_LINE_MAX 96

/* Draws (or, if `draw` is 0, does nothing but exists so the caller shares one
 * code path) one already-assembled line. `w` is its DS-pixel width, already
 * measured by the caller via text_width() -- recomputing it here would be the
 * one place this function's cost depended on string length twice over. */
static void draw_wrap_line(const char *s, int w, int dx0, int dy, int align,
                           int px, int bold, uint16_t col, int draw)
{
    int dx = dx0;

    if (!draw) {
        return;
    }
    if (align == UI_ALIGN_CENTER) {
        dx -= w / 2;
    } else if (align == UI_ALIGN_RIGHT) {
        dx -= w;
    }
    draw_string(dx, dy, s, px, bold, col);
}

/* THE WRAP ITSELF. Greedy word-fill, one pass, no lookahead: append the next
 * space-delimited word to the current line if it fits, otherwise start a new
 * line with it. Never splits a word -- see ui.h -- except the one case that
 * cannot be helped, a single word wider than `max_w_ds` on its own, which
 * gets its own line and is truncated exactly as ui_textf_fit() truncates.
 *
 * SHARED BY DRAWING AND MEASURING: `draw` gates only the pixel writes inside
 * draw_wrap_line(); the line-counting logic above it runs identically either
 * way, so ui_textf_wrap() and ui_text_measure() cannot disagree about how
 * many lines a string takes -- they are the same walk. */
static int wrap_run(int max_w_ds, int px, int bold, int step_ds, int dx0,
                    int dy0, int align, uint16_t col, int draw)
{
    const char *p = s_scratch;
    char line[WRAP_LINE_MAX];
    int line_len = 0, line_w = 0;
    int lines = 0;
    int dy = dy0;
    int space_w = text_width(" ", px, bold);

    line[0] = '\0';
    for (;;) {
        const char *wstart;
        char word[WRAP_LINE_MAX];
        size_t wlen;
        int word_w;

        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            if (line_len > 0) {
                draw_wrap_line(line, line_w, dx0, dy, align, px, bold, col,
                               draw);
                lines++;
            }
            break;
        }
        wstart = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        wlen = (size_t)(p - wstart);
        if (wlen > sizeof word - 1u) {
            wlen = sizeof word - 1u;   /* a pathological "word" with no space
                                        * in it anywhere -- the truncation
                                        * below still bounds it to max_w_ds */
        }
        memcpy(word, wstart, wlen);
        word[wlen] = '\0';
        word_w = text_width(word, px, bold);

        if (word_w > max_w_ds) {
            /* Does not fit even alone. Flush whatever line is pending, then
             * this word gets its own, truncated from the end exactly the way
             * draw_scratch()'s own fit rule does -- see ui.h: reword the
             * source string in the DS screen file instead, wherever this is
             * actually reachable. */
            if (line_len > 0) {
                draw_wrap_line(line, line_w, dx0, dy, align, px, bold, col,
                               draw);
                lines++;
                dy += step_ds;
                line_len = 0;
                line[0] = '\0';
                line_w = 0;
            }
            while (word[0] != '\0'
                   && text_width(word, px, bold) > max_w_ds) {
                word[strlen(word) - 1u] = '\0';
            }
            draw_wrap_line(word, text_width(word, px, bold), dx0, dy, align,
                          px, bold, col, draw);
            lines++;
            dy += step_ds;
            continue;
        }

        {
            int sep_w = (line_len > 0) ? space_w : 0;

            if (line_len > 0 && line_w + sep_w + word_w > max_w_ds) {
                draw_wrap_line(line, line_w, dx0, dy, align, px, bold, col,
                               draw);
                lines++;
                dy += step_ds;
                line_len = 0;
                line[0] = '\0';
                line_w = 0;
                sep_w = 0;
            }
            if (line_len > 0 && (size_t)line_len < sizeof line - 1u) {
                line[line_len++] = ' ';
                line_w += sep_w;
                line[line_len] = '\0';
            }
            if ((size_t)line_len + wlen < sizeof line) {
                memcpy(line + line_len, word, wlen + 1u);
                line_len += (int)wlen;
                line_w += word_w;
            }
        }
    }
    return lines;
}

int ui_textf_wrap(float x, float y, float w, float scale, uint32_t colour,
                  int align, float line_height, const char *fmt, ...)
{
    va_list ap;
    int px, bold;
    int max_w_ds, dx0, dy0, step_ds;
    uint16_t col = ui_rgb15(colour);

    va_start(ap, fmt);
    format_scratch(fmt, ap);
    va_end(ap);

    face_for(scale, &px, &bold);
    max_w_ds = sx(f2i(w));
    if (max_w_ds < 8) {
        max_w_ds = 8;   /* a degenerate box still gets one glyph's worth of
                         * room rather than truncating every word to nothing */
    }
    dx0 = sx(f2i(x));
    dy0 = sy(f2i(y));
    step_ds = (line_height > 0.0f) ? sy(f2i(line_height)) : (8 * px + 2);

    return wrap_run(max_w_ds, px, bold, step_ds, dx0, dy0, align, col, 1);
}

int ui_text_measure(float w, float scale, const char *fmt, ...)
{
    va_list ap;
    int px, bold;
    int max_w_ds;

    va_start(ap, fmt);
    format_scratch(fmt, ap);
    va_end(ap);

    face_for(scale, &px, &bold);
    max_w_ds = sx(f2i(w));
    if (max_w_ds < 8) {
        max_w_ds = 8;
    }
    return wrap_run(max_w_ds, px, bold, 0, 0, 0, UI_ALIGN_LEFT, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* shapes                                                                   */
/* ------------------------------------------------------------------------ */

void ui_rect(float x, float y, float w, float h, uint32_t colour)
{
    int ix = f2i(x), iy = f2i(y);
    int x0 = sx(ix), y0 = sy(iy);
    int x1 = sx(ix + f2i(w)), y1 = sy(iy + f2i(h));

    fill_px(x0, y0, span(x0, x1), span(y0, y1), ui_rgb15(colour));
}

void ui_outline(float x, float y, float w, float h, float t, uint32_t colour)
{
    /* Four rects, same as clients/3ds/source/ui.c. Written in DS pixels
     * rather than as four ui_rect() calls so the corners meet exactly: the
     * independent rounding of x and x+w would otherwise leave a hole. */
    int ix = f2i(x), iy = f2i(y);
    int x0 = sx(ix), y0 = sy(iy);
    int x1 = sx(ix + f2i(w)), y1 = sy(iy + f2i(h));
    int bw = span(x0, x1), bh = span(y0, y1);
    int it = sx(f2i(t));
    uint16_t col = ui_rgb15(colour);

    if (it < 1) {
        it = 1;   /* a 1px 3DS outline is 0.8px here; it must still show */
    }
    fill_px(x0, y0, bw, it, col);
    fill_px(x0, y0 + bh - it, bw, it, col);
    fill_px(x0, y0, it, bh, col);
    fill_px(x0 + bw - it, y0, it, bh, col);
}

void ui_panel(const ui_box *b, uint32_t fill, uint32_t border)
{
    ui_rect(b->x, b->y, b->w, b->h, fill);
    ui_outline(b->x, b->y, b->w, b->h, 1.0f, border);
}

void ui_dot(float cx, float cy, float r, uint32_t colour)
{
    int ir = sx(f2i(r));

    if (ir < 1) {
        ir = 1;
    }
    /* A square, not a circle: three or four pixels across is below the size
     * at which the difference is visible, and a circle rasteriser would be
     * the only place in this file needing one. */
    fill_px(sx(f2i(cx)) - ir, sy(f2i(cy)) - ir, ir * 2, ir * 2,
            ui_rgb15(colour));
}

void ui_button(const ui_box *b, const char *label, int pressed, int accent)
{
    uint32_t fill   = pressed ? ui_c_accent()
                              : (accent ? ui_c_panel_hi() : ui_c_panel());
    uint32_t border = accent ? ui_c_accent() : ui_c_border();
    uint32_t ink    = pressed ? ui_c_bg()
                              : (accent ? ui_c_accent() : ui_c_text());

    ui_rect(b->x, b->y, b->w, b->h, fill);
    ui_outline(b->x, b->y, b->w, b->h, 1.0f, border);
    ui_textf_fit(b->x + b->w * 0.5f,
                 b->y + (b->h - UI_LINE(UI_S_BODY)) * 0.5f - 1.0f,
                 UI_S_BODY, ink, UI_ALIGN_CENTER, b->w - 8.0f, "%s", label);
}

void ui_header_ex(float width, const char *title, const char *right,
                  uint32_t right_colour, float right_pad)
{
    const float h = 22.0f;
    float text_right = width - 6.0f - right_pad;
    /* right_pad is already subtracted from text_right (it moves the right
     * text's right edge left of the battery glyph). It must NOT also be
     * subtracted from the width cap, or the state text gets squeezed twice:
     * with BATT_RESERVE_PX=72 that cut "not connected" to "not con" even
     * though there was room to its left. The cap only exists to keep the
     * right text clear of the LEFT title, which right_pad has nothing to do
     * with. right_pad==0 callers are unaffected (0.40*width either way). */
    float text_max_w = width * 0.40f;

    ui_rect(0.0f, 0.0f, width, h, ui_c_panel());
    ui_rect(0.0f, h - 1.0f, width, 1.0f, ui_c_border());
    ui_textf_fit(6.0f, 2.0f, UI_S_BODY, ui_c_text(), UI_ALIGN_LEFT,
                 width * 0.58f - 6.0f, "%s", title);
    if (right != NULL && text_max_w > 0.0f) {
        ui_textf_fit(text_right, 2.0f, UI_S_BODY, right_colour,
                     UI_ALIGN_RIGHT, text_max_w, "%s", right);
    }
}

void ui_header(float width, const char *title, const char *right,
               uint32_t right_colour)
{
    ui_header_ex(width, title, right, right_colour, 0.0f);
}
