/* clients/nds/source/ui.h
 *
 * The DS client's rendering primitives -- a DS IMPLEMENTATION OF THE 3DS
 * CLIENT'S ui.h, function for function, so that clients/3ds/source/screen_*.c
 * can be copied into this directory and compile almost unchanged. Read
 * clients/3ds/source/ui.h first: this header exists to be the same contract,
 * and every difference below is annotated as a difference.
 *
 * THE ONE BIG IDEA: LAYOUTS STAY IN 3DS COORDINATES.
 * ---------------------------------------------------------------------------
 * The copied screens lay out against a 400x240 top screen and a 320x240 bottom
 * one. The DS has 256x192 twice. Rather than re-deriving every rectangle in
 * the copied files (which would make `diff clients/3ds/source/screen_x.c
 * clients/nds/source/screen_x.c` unreadable and would guarantee the two
 * clients drift), ui.c SCALES AT DRAW TIME:
 *
 *      bottom screen   x, y  ->  * 4 / 5           (320x240 -> 256x192)
 *      top screen      x     ->  * 256 / 400       (400     -> 256)
 *                      y     ->  * 4 / 5           (240     -> 192)
 *
 * and ui_box_hit() maps a raw DS touch point BACK into 320x240 space before
 * testing, so a screen's hit boxes and its drawing stay one set of numbers.
 * This also lines up exactly with clients/common/apad_kbm_touch.c, which
 * scales its own 320x240 reference boxes by screen_w/320 = 256/320 = 4/5:
 * the KBM builders and the drawing agree because both are the same 0.8.
 *
 * FLOAT AT THE BOUNDARY, INTEGER EVERYWHERE ELSE. The DS ARM9 has no FPU, and
 * docs/CONVENTIONS.md forbids floating point in this kind of code. The signatures below
 * still take float because the copied screens pass float literals (`6.0f`,
 * `UI_S_SMALL`, `UI_LINE(...)`) and rewriting those call sites is exactly the
 * churn this design avoids. Every entry point converts its floats to int ONCE,
 * on the way in, and no float arithmetic happens inside a loop anywhere in
 * ui.c. Most call sites pass compile-time constants, so gcc folds the
 * conversion away entirely.
 *
 * WHAT IS DIFFERENT FROM THE 3DS, all of it visible in this file:
 *
 *   - `scale` is a FLOAT on the 3DS (a continuously-scalable vector font).
 *     Here it selects one of three integer sizes of an 8x8 bitmap font, plus
 *     a "bold" flag for headings. See ui.c's font section.
 *   - ui_textf_fit() shrinks the same way the 3DS does until it runs out of
 *     sizes, and then TRUNCATES instead of shrinking further -- there is no
 *     0.30f floor to fall back on when the smallest face is 8 pixels tall.
 *   - ui_bignum() is new. The 3DS draws its big RTT figure with the system
 *     font at UI_S_HUGE; a bitmap font blown up 3x is what stands in for that
 *     here, and giving it its own call keeps "the big number" one decision
 *     rather than a scale constant sprinkled through two files.
 *   - the ui_stereo_* family is present and inert (there is no second eye on
 *     this console). Kept so the copied main loop compiles unchanged.
 *   - C2D_DrawCircleSolid() has no equivalent, so ui_dot() stands in for the
 *     places the copied code drew a filled circle. It is a small square on
 *     this hardware; at 0.8 scale a 3.5px circle is 3px across and the
 *     difference is not visible.
 */
#ifndef ATTICPAD_NDS_UI_H
#define ATTICPAD_NDS_UI_H

#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* geometry -- 3DS NUMBERS, DELIBERATELY                                    */
/* ------------------------------------------------------------------------ */

/* These are the 3DS's screen sizes and they are what the copied screens lay
 * out against. The physical DS sizes are below, for the two places that
 * genuinely need them: main.c's touch normalisation (which fills the WIRE and
 * must not go through this scaling at all) and apad_kbm_touch_init(). */
#define UI_TOP_W    400.0f
#define UI_TOP_H    240.0f
#define UI_BOT_W    320.0f
#define UI_BOT_H    240.0f

#define UI_DS_W     256
#define UI_DS_H     192

/* Text scales, verbatim from the 3DS so copied call sites read identically.
 * The comment on each is the 3DS line height; ui.c maps the number to an
 * integer glyph size. */
#define UI_S_TINY   0.42f   /* ~13px on the 3DS -> 8px face here          */
#define UI_S_SMALL  0.50f   /* ~15px            -> 8px face               */
#define UI_S_BODY   0.58f   /* ~17px            -> 8px face               */
#define UI_S_HEAD   0.72f   /* ~22px            -> 8px face, bold         */
#define UI_S_HUGE   1.55f   /* ~47px            -> 24px face              */

#define UI_LINE(scale) ((scale) * 30.0f)

/* ------------------------------------------------------------------------ */
/* palette                                                                  */
/* ------------------------------------------------------------------------ */
/* Same values as clients/3ds/source/ui.c, same packing as citro2d's
 * C2D_Color32 (byte 0 red, byte 1 green, byte 2 blue, byte 3 alpha), so a
 * copied file's colour constants and comments still mean what they say.
 * ui.c converts to the hardware's RGB15 once per call. */

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

/* ------------------------------------------------------------------------ */
/* boxes                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    float x, y, w, h;
} ui_box;

/* Touch hit test. `px`/`py` are RAW DS touchscreen pixels (0..255, 0..191)
 * straight out of touchRead(); the box is in 320x240 bottom-screen space like
 * everything else a screen file declares, and this call is where the two
 * meet. */
int ui_box_hit(const ui_box *b, int px, int py);

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                */
/* ------------------------------------------------------------------------ */

/* Sets up the video hardware and both bitmap layers. Returns 0 on failure.
 * Must be called before anything else here, and before the libnds keyboard is
 * initialised -- ui_init() owns the VRAM map that leaves the keyboard room.
 * See ui.c's VRAM MAP comment. */
int  ui_init(void);
void ui_exit(void);

/* One frame is: ui_frame_begin(), then ui_screen_top() + that screen's
 * drawing, then ui_screen_bottom() + its drawing, then ui_frame_end().
 *
 * THE VBLANK IS AT THE START HERE, not the end as on the 3DS, and that is
 * load-bearing rather than stylistic:
 *
 *   - ui_frame_begin() yields until VBlank (cothread_yield_irq(IRQ_VBLANK),
 *     NEVER swiWaitForVBlank() -- that parks the ARM9 with DSWiFi's lwIP
 *     cothread behind it) and then presents the bottom screen's back buffer.
 *   - the TOP screen has no back buffer (the sub engine's entire background
 *     VRAM is one 128 KB bank and a 16-bit 256x256 bitmap is exactly that),
 *     so it is drawn straight into live VRAM. Starting that draw at the top
 *     of the ~4.5 ms VBlank window, when the beam is not reading, is what
 *     keeps it tear-free.
 *
 * So the pacing call is ui_frame_begin() and there is no other yield in the
 * frame loop. ui_frame_end() exists to keep the copied loop's shape and to
 * flush the register shadows. */
void ui_frame_begin(void);
void ui_screen_top(void);
void ui_screen_bottom(void);
void ui_frame_end(void);

/* ------------------------------------------------------------------------ */
/* stereoscopic 3D -- INERT ON THIS CONSOLE                                 */
/* ------------------------------------------------------------------------ */
/* The DS has one eye. These exist only so the copied frame loop and any
 * copied caller compile without an #ifdef; ui_stereo_slider_active() is
 * always 0, so ui_screen_top_right() is never reached. */
int   ui_stereo_slider_active(void);
void  ui_screen_top_right(void);
int   ui_top_pass_is_first(void);
float ui_stereo_eye_shift(void);

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

enum {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER,
    UI_ALIGN_RIGHT
};

/* printf-shaped text. `x` is the left edge, centre or right edge per `align`;
 * `y` is the TOP of the 3DS line box, and ui.c places the 8px face inside
 * that box the way the 3DS system font's ink sits inside its own -- which is
 * what makes a column of copied `y += 15.0f` lines still look like a column.
 *
 * Unlike the 3DS this ALWAYS clips at the screen edge. A 400px-wide layout
 * squeezed into 256px has less horizontal room than the original, so a label
 * that fitted there can run off here; clipping is the difference between an
 * ugly line and a wild write past the end of the framebuffer. */
void ui_textf(float x, float y, float scale, uint32_t colour, int align,
              const char *fmt, ...);

/* Same, shrunk to fit `max_w` (in 3DS pixels, like every other coordinate
 * here). Steps down through the available faces first, exactly as the 3DS
 * shrinks its scale; when the smallest face still does not fit it TRUNCATES,
 * because there is nothing below 8x8. See ui.c.
 *
 * ONE LINE ONLY. A copied 3DS sentence squeezed into this console's narrower
 * screens routinely does not fit even the smallest 8x8 face on one line, and
 * this function's only remaining move at that point is to cut it off
 * mid-word -- exactly the bug reported off real hardware (2026-09-09: "SHI"/
 * truncated confirm-overlay sentences). Use ui_textf_wrap() below for
 * anything that is genuinely prose (more than a few words); keep this one for
 * headers, buttons, field values and other short single-line labels, where
 * a hard width ceiling is the point. */
void ui_textf_fit(float x, float y, float scale, uint32_t colour, int align,
                  float max_w, const char *fmt, ...);

/* ADDED FOR THE DS: THE WORD-WRAPPING PRIMITIVE, and the reason it exists at
 * all -- see ui_textf_fit()'s comment just above. Formats like every other
 * ui_textf*() call, then lays the result out inside a `w`-3DS-pixel-wide box,
 * breaking ONLY at spaces (never inside a word) and never shrinking the face:
 * 8x8 is this console's floor (see the file header), so the only way left to
 * keep a long sentence on screen is more LINES, not a smaller glyph.
 *
 * `y` is the top of the FIRST line, exactly like ui_textf()'s `y`.
 * `line_height` is the 3DS-pixel step between lines -- pass UI_LINE(scale)
 * for the same rhythm every other column of text in a copied screen already
 * uses, or 0.0f for this call's own DS-native default (the face's glyph
 * height + 2px, i.e. what a caller gets who has no existing rhythm to match).
 *
 * Returns the number of lines actually drawn, so a caller can advance `y` by
 * `lines * line_height` before drawing whatever comes next -- this client's
 * layouts are static 3DS numbers (see the file header's ONE BIG IDEA) and
 * nothing here asks ui.c how tall a string is any other way.
 *
 * THE ONE CASE THIS CANNOT WRAP: a single WORD wider than `w` has no space in
 * it to break at. It is drawn alone on its own line and TRUNCATED exactly the
 * way ui_textf_fit() truncates -- a safety net, not a designed path. Prefer
 * rewording the source string in the DS screen file over relying on it. */
int ui_textf_wrap(float x, float y, float w, float scale, uint32_t colour,
                  int align, float line_height, const char *fmt, ...);

/* Same wrapping, no drawing: how many lines ui_textf_wrap() would produce for
 * the same (w, scale, fmt...). For a caller that needs a wrapped string's
 * height BEFORE laying out whatever surrounds it. */
int ui_text_measure(float w, float scale, const char *fmt, ...);

/* The big number: the top screen's RTT figure. 3x the base face (24px tall),
 * which is as close as an 8x8 font gets to the 3DS's UI_S_HUGE at 0.8 scale
 * (47px * 0.8 = 38px there, 24px here -- the DS screen is 192px tall and a
 * 38px digit would eat a fifth of it). */
void ui_bignum(float x, float y, uint32_t colour, int align, const char *fmt,
               ...);

void ui_rect(float x, float y, float w, float h, uint32_t colour);
void ui_outline(float x, float y, float w, float h, float t, uint32_t colour);
void ui_panel(const ui_box *b, uint32_t fill, uint32_t border);
void ui_button(const ui_box *b, const char *label, int pressed, int accent);
void ui_header(float width, const char *title, const char *right,
               uint32_t right_colour);
void ui_header_ex(float width, const char *title, const char *right,
                  uint32_t right_colour, float right_pad);

/* Stands in for citro2d's C2D_DrawCircleSolid() at the call sites the copied
 * screens use it (the live touch point, the mouse-pad contact dot). `r` is
 * the 3DS radius. */
void ui_dot(float cx, float cy, float r, uint32_t colour);

/* The surface's native BGR555 packing of a ui colour, for callers that
 * prepare pixels themselves and hand them to ui_blit_rgb15(). */
uint16_t ui_rgb15(uint32_t colour);

/* ------------------------------------------------------------------------ */
/* the libnds keyboard's window                                             */
/* ------------------------------------------------------------------------ */

/* The bottom screen's y (in 3DS 320x240 coordinates) below which the libnds
 * keyboard's background layer sits when it is shown -- kbd_nds.c owns the
 * keyboard itself, this constant is here because it is a LAYOUT fact and
 * screens need to keep their content above it. See kbd_nds.h. */
#define UI_KBD_TOP_Y 120.0f

/* ------------------------------------------------------------------------ */
/* the bottom screen's VRAM, lent to the DSi camera                         */
/* ------------------------------------------------------------------------ */

/* DS-ONLY, like UI_KBD_TOP_Y and ui_bignum() above: there is nothing on the
 * 3DS to mirror, because the 3DS's viewfinder is a citro2d texture draw and
 * its camera never writes into a framebuffer this file owns.
 *
 * WHY THIS EXISTS AT ALL. The DSi camera is moved by NDMA, and on this
 * hardware that transfer only ever COMPLETES when its destination is
 * MAIN-ENGINE background VRAM (0x06000000..): a main-RAM destination delivers
 * a partial first frame and then never signals completion again. That was
 * established by isolation against the vendored BlocksDS
 * references/nds/examples/peripherals/camera sample -- the stock sample
 * targeting bgGetGfxPtr() reaches 600+ frames, the same sample changed ONLY to
 * target a memalign(32) main-RAM buffer stalls at frame 1. ui.c's main engine
 * is the BOTTOM screen (see ui.c's VRAM MAP), so the live preview has to be
 * the bottom screen, and the QR screen's own text and CANCEL move to the top.
 *
 * ui_bottom_camera_lock() hands out a stable main-engine VRAM bitmap base for
 * the camera's NDMA to own outright, and for as long as it is held:
 *
 *   - the bottom screen stops double-buffering (a free-running DMA cannot
 *     follow a buffer identity that changes every frame), so ui_frame_begin()
 *     does not flip;
 *   - ui_screen_bottom() no longer CLEARS -- it still selects the surface, so
 *     an ordinary ui_* call still works if a caller wants to composite on top
 *     of the live image, but it will not wipe the camera's pixels first.
 *
 * The returned pointer is 96 KB of the 128 KB bitmap (256x192 of a 256x256
 * surface, tightly packed at stride 256 -- exactly what the camera's preview
 * mode writes, per libnds camera.twl.c's `REG_NDMA_LENGTH = (256*192)>>1`).
 * Never NULL after a successful ui_init(). Idempotent.
 *
 * ui_bottom_camera_unlock() restores ordinary double-buffered drawing so that
 * every other screen is pixel-identical afterwards. It costs ONE frame in
 * which the not-yet-displayed buffer is redrawn while the camera's last image
 * stays on screen -- without that the flip would present a buffer holding a
 * stale pre-camera frame for one frame. Safe to call when not locked. */
uint16_t *ui_bottom_camera_lock(void);
void ui_bottom_camera_unlock(void);

#endif /* ATTICPAD_NDS_UI_H */
