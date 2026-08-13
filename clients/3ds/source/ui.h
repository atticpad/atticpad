/* clients/3ds/source/ui.h
 *
 * The 3DS client's rendering primitives: a thin, opinionated wrapper over
 * citro2d/citro3d so that no screen file ever touches a C3D_RenderTarget, a
 * C2D_TextBuf or a frame boundary directly.
 *
 * WHY THIS EXISTS AT ALL, rather than printf onto a console: through M2 this
 * client was consoleInit() + printf, which was exactly right for bring-up
 * (every diagnostic anyone read off the top screen was a line of text) and is
 * not what ships -- the deferred 3DS UI pass. The two
 * worlds do NOT coexist: consoleInit() takes over a screen's framebuffer with
 * the software renderer while citro3d wants that same screen as a GPU render
 * target, and whichever ran last wins the screen. So this codebase is now
 * entirely citro2d -- INCLUDING the self-test screen, which stays text-shaped
 * but draws its text through ui_textf() like everything else.
 *
 * Geometry constants are the real hardware's: the top screen is 400x240 and
 * the bottom is 320x240. The system font's glyph box is 30px tall at scale
 * 1.0 with the baseline at 25px (citro2d's c2d/text.h says so), which is why
 * every scale below is expressed as "how many pixels tall a line is" -- a
 * layout done in scale units alone is unreadable six months later.
 *
 * No allocation happens after ui_init(): one text buffer is allocated there
 * and reused every frame (cleared in ui_frame_begin()). docs/CONVENTIONS.md's no-malloc
 * rule is scoped to core/ and shim/, but a client that quietly allocates per
 * frame on a 128 MB console is still a client that dies after an hour.
 */
#ifndef ATTICPAD_3DS_UI_H
#define ATTICPAD_3DS_UI_H

#include <stdint.h>

#include <3ds.h>
#include <citro2d.h>

/* ------------------------------------------------------------------------ */
/* geometry                                                                 */
/* ------------------------------------------------------------------------ */

#define UI_TOP_W    400.0f
#define UI_TOP_H    240.0f
#define UI_BOT_W    320.0f
#define UI_BOT_H    240.0f

/* Text scales. The comment on each is the resulting line height in pixels
 * (30px * scale), because that is the number layout is actually done in. */
#define UI_S_TINY   0.42f   /* ~13px -- footnotes, the debug detail line  */
#define UI_S_SMALL  0.50f   /* ~15px -- most body text                    */
#define UI_S_BODY   0.58f   /* ~17px -- field values, button labels       */
#define UI_S_HEAD   0.72f   /* ~22px -- panel headings                    */
#define UI_S_HUGE   1.55f   /* ~47px -- the RTT number, readable across a
                             *          room, which is the whole point of
                             *          putting it on the top screen      */

/* Line height for a given scale, so screens can lay out a column of text
 * without repeating the 30.0f. */
#define UI_LINE(scale) ((scale) * 30.0f)

/* ------------------------------------------------------------------------ */
/* palette                                                                  */
/* ------------------------------------------------------------------------ */
/* Dark, because this is a status display that someone glances at while
 * playing something else, and a white screen at arm's length in a dim room
 * is a lamp. Colours are looked up through functions rather than macros:
 * C2D_Color32() is a function in C mode (c2d/base.h, C2D_CONSTEXPR expands
 * to nothing without C++), so a #define would not be a constant expression
 * and could not initialise a static. */

uint32_t ui_c_bg(void);        /* screen background                        */
uint32_t ui_c_panel(void);     /* panel fill                               */
uint32_t ui_c_panel_hi(void);  /* raised/active panel fill                 */
uint32_t ui_c_border(void);    /* panel and button outlines                */
uint32_t ui_c_text(void);      /* primary text                             */
uint32_t ui_c_dim(void);       /* labels, hints, anything secondary        */
uint32_t ui_c_accent(void);    /* focus, the live RTT figure, headers      */
uint32_t ui_c_good(void);      /* connected, PASS                          */
uint32_t ui_c_warn(void);      /* counting down, "this will drop the ..."  */
uint32_t ui_c_bad(void);       /* FAIL, errors, close reasons              */

/* ------------------------------------------------------------------------ */
/* boxes                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    float x, y, w, h;
} ui_box;

/* Touch hit test. Takes the raw touchPosition pixel coordinates -- the
 * bottom screen is the only touchable one, so a ui_box being tested is
 * always in bottom-screen space and no screen argument is needed. */
int ui_box_hit(const ui_box *b, int px, int py);

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                */
/* ------------------------------------------------------------------------ */

/* gfxInitDefault() must already have been called. Returns 0 on failure (the
 * caller has no screen at that point and can only bail). */
int  ui_init(void);
void ui_exit(void);

/* One frame is: ui_frame_begin(), then for each screen ui_screen_top() /
 * ui_screen_bottom() followed by that screen's drawing, then ui_frame_end().
 * ui_frame_end() syncs to vblank (C3D_FRAME_SYNCDRAW), so it replaces the
 * gfxFlushBuffers()/gfxSwapBuffers()/gspWaitForVBlank() trio the console
 * build used and is what paces the whole app at 60 Hz. */
void ui_frame_begin(void);
void ui_screen_top(void);
void ui_screen_bottom(void);
void ui_frame_end(void);

/* ------------------------------------------------------------------------ */
/* stereoscopic 3D -- top screen only (cherry, 2026-08-12 hardware pass)    */
/* ------------------------------------------------------------------------ */

/* Mirrors devkitPro's own citro2d sample,
 * examples/3ds/graphics/gpu/stereoscopic_2d/source/main.cpp (read directly
 * from the pinned devkitARM image, not recalled -- references/3ds/ has no
 * graphics sample at all): gfxSet3D(true) is set ONCE at ui_init() and never
 * toggled, both a GFX_LEFT and a GFX_RIGHT top target exist for the app's
 * whole life, and it is osGet3DSliderState() -- read once per frame in
 * ui_frame_begin(), multiplied into a caller's own per-eye offset -- that
 * makes slider==0 collapse to zero parallax, not a branch on the slider
 * value. ONE deliberate deviation from the sample: the right-eye PASS is
 * skipped outright when the slider reads 0 (ui_stereo_slider_active() is
 * then false), since at that multiplier it would draw pixel-identical
 * geometry to the left pass for zero benefit -- "skip the second draw for
 * free performance if the API makes that easy" (this task's brief). That is
 * also what keeps slider==0 pixel-identical to how this client rendered
 * before this pass existed: the left pass alone, unchanged. */
int   ui_stereo_slider_active(void); /* this frame's osGet3DSliderState()>0 */

/* The right-eye pass. Call only when ui_stereo_slider_active() is true, once
 * ui_screen_top()'s (left) pass has already drawn -- main.c's frame loop is
 * the only call site. */
void  ui_screen_top_right(void);

/* True during ui_screen_top()'s pass (the only pass at slider==0, or the
 * LEFT pass under stereo); false only during ui_screen_top_right()'s. For a
 * caller whose draw runs on both top passes under stereo (gyro_cube.c) to
 * gate a once-per-real-frame state advance so it does not double-step when
 * draw_top() runs twice. */
int   ui_top_pass_is_first(void);

/* +slider on the left pass, -slider on the right, 0 whenever
 * ui_stereo_slider_active() is false (including the ordinary, non-stereo
 * left-only pass). A caller multiplies this by its own per-eye pixel
 * constant and a depth value to get a signed screen-space offset -- see
 * gyro_cube.c, the one thing in this client that reads it. The sign
 * convention (left = +, right = -) mirrors the devkitPro sample's
 * "+offsetUpper*slider" / "-offsetUpper*slider" exactly rather than
 * guessing one; whether that makes an object POP OUT or SINK IN on this
 * client's own geometry has not been checked against a physical slider. */
float ui_stereo_eye_shift(void);

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

enum {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER,
    UI_ALIGN_RIGHT
};

/* printf-shaped text. `x` is the left edge, the centre or the right edge
 * depending on `align`; `y` is the TOP of the line (not the baseline).
 * Formatted into a shared scratch buffer and drawn immediately, so nothing
 * needs to stay alive across the call. */
void ui_textf(float x, float y, float scale, uint32_t colour, int align,
              const char *fmt, ...);

/* Same, but shrinks `scale` until the string fits inside `max_w` pixels.
 *
 * THIS EXISTS BECAUSE NOBODY CAN MEASURE A STRING BY EYE. The bottom screen
 * is 320px wide and the system font's advance widths are per-glyph, so
 * whether a sentence fits is a question only C2D_TextGetDimensions() can
 * answer -- and getting it wrong means text silently runs off the edge, which
 * is exactly the kind of defect that survives review and is found by someone
 * holding the console. Every line of prose in this client goes through this,
 * and only short fixed labels use ui_textf() directly. Shrinking is capped so
 * a pathological string becomes small rather than invisible. */
void ui_textf_fit(float x, float y, float scale, uint32_t colour, int align,
                  float max_w, const char *fmt, ...);

void ui_rect(float x, float y, float w, float h, uint32_t colour);

/* A `t`-pixel outline drawn as four rectangles. citro2d has no stroke
 * primitive; four rects is what everything else does too. */
void ui_outline(float x, float y, float w, float h, float t, uint32_t colour);

/* Filled rect plus outline -- the shape almost every panel and button in
 * this client is. */
void ui_panel(const ui_box *b, uint32_t fill, uint32_t border);

/* A labelled, tappable box. `pressed` draws it held (raised fill), `accent`
 * draws its border and label in the accent colour, for the one primary
 * action on a screen. */
void ui_button(const ui_box *b, const char *label, int pressed, int accent);

/* Horizontal title bar across the top of a screen: `title` on the left,
 * `right` (may be NULL) on the right in `right_colour`. */
void ui_header(float width, const char *title, const char *right,
               uint32_t right_colour);

/* Same as ui_header(), but reserves `right_pad` extra pixels to the LEFT of
 * the right-hand text slot -- for a caller (app_draw_status_top(), the
 * battery glyph) that draws something of its own in that header row and
 * needs the state text to stop short of it rather than run underneath.
 * ui_header() is a thin wrapper calling this with right_pad = 0.0f, so every
 * other screen's call site is unaffected. */
void ui_header_ex(float width, const char *title, const char *right,
                  uint32_t right_colour, float right_pad);

#endif /* ATTICPAD_3DS_UI_H */
