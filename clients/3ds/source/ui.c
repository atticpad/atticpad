/* clients/3ds/source/ui.c -- citro2d rendering primitives. See ui.h for why
 * this layer exists and why consoleInit() is gone from this client entirely.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* ------------------------------------------------------------------------ */
/* state                                                                    */
/* ------------------------------------------------------------------------ */

static C3D_RenderTarget *s_top_left;
static C3D_RenderTarget *s_top_right;   /* stereo cherry; see ui.h          */
static C3D_RenderTarget *s_bottom;

/* Stereo state, all of it read/written on the main thread only (same as
 * everything else in this file). Cached once per frame in ui_frame_begin()
 * rather than read again mid-frame, so a slider read that straddled a frame
 * boundary cannot make the left and right passes disagree about it. */
static float s_stereo_slider;  /* 0.0..1.0, this frame's osGet3DSliderState */
static int   s_stereo_on;      /* s_stereo_slider > 0.0f                    */
static float s_eye_sign;       /* +1 during ui_screen_top(), -1 during
                                * ui_screen_top_right(), meaningless
                                * otherwise (nobody reads it outside a top
                                * pass) */

/* ONE text buffer, cleared at the top of every frame. citro2d's text buffer
 * holds parsed glyph runs, and C2D_DrawText() converts them to quads at the
 * moment it is called -- so clearing once per frame, before any parsing, is
 * safe, while clearing mid-frame after a draw would not be.
 *
 * 4096 glyphs is roughly 8x the busiest screen this client has (the self-test
 * result page). Sized generously on purpose: a text buffer that fills up does
 * not crash, C2D_TextParse() just stops early and the text silently truncates,
 * which is the kind of bug that only shows up on the one screen nobody looked
 * at twice. */
#define UI_TEXT_GLYPHS 4096
static C2D_TextBuf s_textbuf;

/* Shared formatting scratch. Single-threaded by construction: everything in
 * this client runs on the main thread (the session engine explicitly does not
 * own one -- clients/common/apad_client.h). */
static char s_scratch[256];

/* ------------------------------------------------------------------------ */
/* palette                                                                  */
/* ------------------------------------------------------------------------ */

uint32_t ui_c_bg(void)       { return C2D_Color32(0x12, 0x16, 0x1E, 0xFF); }
uint32_t ui_c_panel(void)    { return C2D_Color32(0x1E, 0x26, 0x33, 0xFF); }
uint32_t ui_c_panel_hi(void) { return C2D_Color32(0x30, 0x3E, 0x52, 0xFF); }
uint32_t ui_c_border(void)   { return C2D_Color32(0x3C, 0x4C, 0x64, 0xFF); }
uint32_t ui_c_text(void)     { return C2D_Color32(0xE6, 0xEC, 0xF4, 0xFF); }
uint32_t ui_c_dim(void)      { return C2D_Color32(0x87, 0x96, 0xAB, 0xFF); }
uint32_t ui_c_accent(void)   { return C2D_Color32(0x4C, 0xC2, 0xFF, 0xFF); }
uint32_t ui_c_good(void)     { return C2D_Color32(0x5C, 0xD6, 0x7A, 0xFF); }
uint32_t ui_c_warn(void)     { return C2D_Color32(0xFF, 0xC4, 0x4C, 0xFF); }
uint32_t ui_c_bad(void)      { return C2D_Color32(0xFF, 0x6B, 0x5B, 0xFF); }

/* ------------------------------------------------------------------------ */
/* boxes                                                                    */
/* ------------------------------------------------------------------------ */

int ui_box_hit(const ui_box *b, int px, int py)
{
    float x = (float)px, y = (float)py;

    return (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h);
}

/* ------------------------------------------------------------------------ */
/* lifecycle                                                                */
/* ------------------------------------------------------------------------ */

int ui_init(void)
{
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return 0;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        return 0;
    }
    C2D_Prepare();

    /* Stereoscopic 3D (2026-08-12 hardware pass): gfxSet3D(true) is set ONCE,
     * here, and never toggled -- mirrors devkitPro's own
     * examples/3ds/graphics/gpu/stereoscopic_2d/source/main.cpp exactly (see
     * ui.h's header comment on why this file follows that sample rather than
     * references/3ds/, which has no graphics sample at all). Both top targets
     * exist for the app's whole life; ui_frame_begin() reads the slider every
     * frame and ui_screen_top_right() is what a caller skips when it reads
     * zero, not this call. */
    gfxSet3D(true);
    s_top_left  = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_top_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    s_bottom    = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (s_top_left == NULL || s_top_right == NULL || s_bottom == NULL) {
        C2D_Fini();
        C3D_Fini();
        return 0;
    }

    s_textbuf = C2D_TextBufNew(UI_TEXT_GLYPHS);
    if (s_textbuf == NULL) {
        C2D_Fini();
        C3D_Fini();
        return 0;
    }

    /* The system shared font is mapped by C2D_Init() (citro2d calls
     * fontEnsureMapped() itself -- confirmed by nm on libcitro2d.a in the
     * pinned image), so there is no font to load and no .bcfnt to ship. Every
     * C2D_Text below therefore leaves .font NULL and gets the system font. */
    return 1;
}

void ui_exit(void)
{
    if (s_textbuf != NULL) {
        C2D_TextBufDelete(s_textbuf);
        s_textbuf = NULL;
    }
    C2D_Fini();
    C3D_Fini();
}

void ui_frame_begin(void)
{
    /* Read once per frame, not once per pass: see s_stereo_slider's comment
     * above for why. osGet3DSliderState() is 0.0..1.0 (3ds/os.h). */
    s_stereo_slider = osGet3DSliderState();
    s_stereo_on = (s_stereo_slider > 0.0f);
    C2D_TextBufClear(s_textbuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void ui_screen_top(void)
{
    s_eye_sign = 1.0f;
    C2D_TargetClear(s_top_left, ui_c_bg());
    C2D_SceneBegin(s_top_left);
}

void ui_screen_top_right(void)
{
    s_eye_sign = -1.0f;
    C2D_TargetClear(s_top_right, ui_c_bg());
    C2D_SceneBegin(s_top_right);
}

int ui_stereo_slider_active(void)
{
    return s_stereo_on;
}

int ui_top_pass_is_first(void)
{
    return s_eye_sign >= 0.0f;
}

float ui_stereo_eye_shift(void)
{
    return s_eye_sign * s_stereo_slider;
}

void ui_screen_bottom(void)
{
    /* The bottom screen is not an eye pass: reset the sign so
     * ui_top_pass_is_first() reads true here regardless of whether the
     * right-eye pass ran this frame (audit finding D -- without this it
     * flips with the 3D slider for any future bottom-screen caller). */
    s_eye_sign = 1.0f;
    C2D_TargetClear(s_bottom, ui_c_bg());
    C2D_SceneBegin(s_bottom);
}

void ui_frame_end(void)
{
    C3D_FrameEnd(0);
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

static u32 align_flags(int align)
{
    switch (align) {
        case UI_ALIGN_CENTER: return C2D_WithColor | C2D_AlignCenter;
        case UI_ALIGN_RIGHT:  return C2D_WithColor | C2D_AlignRight;
        default:              return C2D_WithColor | C2D_AlignLeft;
    }
}

/* Parses s_scratch and draws it. `max_w` <= 0 means "do not shrink". */
static void draw_scratch(float x, float y, float scale, uint32_t colour,
                         int align, float max_w)
{
    C2D_Text text;

    if (C2D_TextParse(&text, s_textbuf, s_scratch) == NULL) {
        return; /* buffer full: drop this string rather than draw garbage */
    }
    C2D_TextOptimize(&text);

    if (max_w > 0.0f) {
        float w = 0.0f;

        C2D_TextGetDimensions(&text, scale, scale, &w, NULL);
        if (w > max_w && w > 0.0f) {
            scale *= max_w / w;
            /* Floor: below this the system font stops being legible on a
             * 240px-tall screen, and a string that would need it is a string
             * to shorten in the source, not to render as a grey smear. */
            if (scale < 0.30f) {
                scale = 0.30f;
            }
        }
    }
    C2D_DrawText(&text, align_flags(align), x, y, 0.0f, scale, scale, colour);
}

void ui_textf(float x, float y, float scale, uint32_t colour, int align,
              const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_scratch, sizeof s_scratch, fmt, ap);
    va_end(ap);
    draw_scratch(x, y, scale, colour, align, 0.0f);
}

void ui_textf_fit(float x, float y, float scale, uint32_t colour, int align,
                  float max_w, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_scratch, sizeof s_scratch, fmt, ap);
    va_end(ap);
    draw_scratch(x, y, scale, colour, align, max_w);
}

void ui_rect(float x, float y, float w, float h, uint32_t colour)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, colour);
}

void ui_outline(float x, float y, float w, float h, float t, uint32_t colour)
{
    ui_rect(x,         y,         w, t,         colour);
    ui_rect(x,         y + h - t, w, t,         colour);
    ui_rect(x,         y,         t, h,         colour);
    ui_rect(x + w - t, y,         t, h,         colour);
}

void ui_panel(const ui_box *b, uint32_t fill, uint32_t border)
{
    ui_rect(b->x, b->y, b->w, b->h, fill);
    ui_outline(b->x, b->y, b->w, b->h, 1.0f, border);
}

void ui_button(const ui_box *b, const char *label, int pressed, int accent)
{
    uint32_t fill   = pressed ? ui_c_accent() : (accent ? ui_c_panel_hi() : ui_c_panel());
    uint32_t border = accent ? ui_c_accent() : ui_c_border();
    uint32_t ink    = pressed ? ui_c_bg() : (accent ? ui_c_accent() : ui_c_text());

    ui_rect(b->x, b->y, b->w, b->h, fill);
    ui_outline(b->x, b->y, b->w, b->h, 1.0f, border);
    /* Vertically centred by eye: the glyph box is UI_LINE(UI_S_BODY) tall and
     * the visible ink sits high within it, so half the leftover space minus a
     * pixel lands better than the arithmetic centre. */
    ui_textf_fit(b->x + b->w * 0.5f,
                 b->y + (b->h - UI_LINE(UI_S_BODY)) * 0.5f - 1.0f,
                 UI_S_BODY, ink, UI_ALIGN_CENTER, b->w - 8.0f, "%s", label);
}

void ui_header_ex(float width, const char *title, const char *right,
                  uint32_t right_colour, float right_pad)
{
    const float h = 22.0f;
    float text_right = width - 6.0f - right_pad;
    float text_max_w = width * 0.40f - right_pad;

    ui_rect(0.0f, 0.0f, width, h, ui_c_panel());
    ui_rect(0.0f, h - 1.0f, width, 1.0f, ui_c_border());
    /* Split 60/40: the title is fixed and short, the right-hand slot carries
     * variable state text ("connecting in 1.5s"). Both shrink rather than
     * collide. right_pad (0 for every caller except app_draw_status_top())
     * carves its own reserved sliver out of the 40% before the text is laid
     * out, so a battery glyph and the state text never overlap. */
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
