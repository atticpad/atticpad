/* clients/3ds/source/gyro_cube.c
 *
 * The "console slab" -- a flat, 3DS-proportioned box drawn on the top screen
 * that tilts and turns as the user tilts the console. Product feedback, not
 * a diagnostic: it lives in the area SELECT's diagnostics overlay uses when
 * that overlay is off (app.h's ctx->show_diag), and carries no text of its
 * own (see ui_widgets.c's app_draw_status_top(), the one call site for both
 * the connect and session screens).
 *
 * WHY CPU-SIDE 3D RATHER THAN A CITRO3D PIPELINE. This client is otherwise
 * flat citro2d (ui.h's header comment) and stays that way here: a box has
 * eight vertices, so "rotate on the CPU with a 3x3 matrix, drop Z, hand
 * citro2d six filled quads" is a few dozen lines and adds no new pipeline
 * state, no shader, no texture. C2D_DrawTriangle()/C2D_DrawLine() (both in
 * c2d/base.h, pulled in through ui.h's <citro2d.h>) are the same immediate
 * GPU draw calls the rest of the client already uses for the stick dots and
 * touch marker (ui_widgets.c's draw_stick()/draw_touchpad()) -- this file
 * does not link against anything the client did not already link against.
 *
 * ORIENTATION STATE. Three float radians -- pitch, roll, yaw -- integrated
 * every call from the gyro rate ALREADY in ctx->st.gyro[] (deci-degrees/s,
 * pitch/roll/yaw order, docs/PROTOCOL.md S5). This is a READ of a value
 * main.c's app_sample_input() already produced for the wire; nothing here
 * calls hidGyroRead() a second time, and nothing here writes ctx->st.
 *
 * CUBE-SPACE AXES, chosen so a physical tilt reads the way it looks:
 *   x -- the box's WIDTH (the long axis, 1.00 units)
 *   y -- the box's HEIGHT (0.62 units), +Y up, matching every stick/axis
 *        convention in this client (docs/CONVENTIONS.md: sticks +Y up)
 *   z -- the box's THICKNESS (0.11 units), toward the viewer at rest
 * Camera looks down -Z at the origin from +Z, orthographic (Z dropped after
 * rotation, not divided by) -- see the note above gyro_cube_step_and_draw()
 * for why orthographic was chosen over a perspective divide.
 *
 *   pitch rotates about X -- nodding the top edge toward/away tips the
 *   FRONT COVER (the +Z face, amber, "the top face" below) up or down and
 *   brings a Y-facing edge into view, matching a console pitched nose-up.
 *   roll  rotates about Z -- this is the one that is an IN-PLANE rotation
 *   once Z is dropped: tipping the console so its left edge goes down spins
 *   the flat picture left-edge-down too. This is the mapping docs/CONVENTIONS.md's
 *   "sticks +Y up" convention and this client's pitch/roll/yaw ASSIGNMENT
 *   (main.c's scale_gyro() callers) make possible to get right without
 *   hardware -- but the SIGN of roll-in vs roll-on-screen is the one thing
 *   in this file that is a guess. See the header comment on
 *   gyro_cube_step_and_draw() for what to check on real hardware.
 *   yaw   rotates about Y -- turning the console like a steering wheel
 *   swings the box like a door, showing an X-facing edge.
 *
 * "TOP FACE" means the +Z face -- the one facing the camera when pitch =
 * roll = yaw = 0, i.e. the face a bird's-eye view of a closed 3DS sitting
 * flat would show. It is the only face drawn in the accent colour
 * (ui_c_accent()); every other face is a panel/border tone (ui.h's
 * palette), scaled by the same directional-light dot product so the box
 * still reads as one lit object rather than six flat colour swatches.
 *
 * NO MALLOC: every piece of state below is a static, file-scope variable,
 * matching this client's ui.c precedent (one text buffer, allocated once)
 * even though docs/CONVENTIONS.md's no-malloc-after-init rule is scoped to core/ and
 * shim/, not clients/3ds -- see this task's point 7.
 */

#include <math.h>

#include "app.h"

/* ------------------------------------------------------------------------ */
/* constants                                                                 */
/* ------------------------------------------------------------------------ */

/* Not M_PI: newlib's math.h guards it behind __BSD_VISIBLE/__XSI_VISIBLE,
 * which -std=c99 (this Makefile's CFLAGS) turns off by defining
 * __STRICT_ANSI__ -- confirmed by trying it against the pinned devkitARM
 * container, where `(float)M_PI` fails with "'M_PI' undeclared". Every other
 * -std=c99 TU in this client that wants pi (there is none yet) will hit the
 * same wall; this is this file's own constant on purpose. */
#define CUBE_PI       3.14159265f
#define CUBE_DEG2RAD  (CUBE_PI / 180.0f)

/* 3DS-slab proportions, x:y:z = 1.0 : 0.62 : 0.11 (spec). x is width, y is
 * height (+Y up), z is thickness (toward the viewer at rest). */
#define CUBE_SX 1.00f
#define CUBE_SY 0.62f
#define CUBE_SZ 0.11f

#define CUBE_NOMINAL_DT   (1.0f / 60.0f) /* ui_frame_end() syncs to vblank
                                          * (C3D_FRAME_SYNCDRAW); this file is
                                          * only ever driven by that draw
                                          * call, so there is no clock to read
                                          * that would be more honest than
                                          * "one vblank" -- and re-centering's
                                          * 3s time constant does not need to
                                          * be more precise than that. */
#define CUBE_RECENTER_TAU 3.0f            /* seconds (spec)                  */
#define CUBE_IDLE_AFTER   2.0f            /* seconds of all-zero gyro before
                                           * the idle spin takes over (spec) */
#define CUBE_IDLE_YAW_DPS 20.0f           /* spec: "~20 deg/s yaw"           */
#define CUBE_MAX_STEP_RAD 0.35f           /* per-frame delta clamp (spec),
                                           * ~20 deg/frame at 60Hz -- room for
                                           * any real tilt, not for a glitch */

/* Stereoscopic 3D (cherry, 2026-08-12 hardware pass): per-VERTEX parallax,
 * not a uniform shift of the whole box, so faces nearer the camera (larger
 * post-rotation z -- this file's own painter's-algorithm comment below)
 * visibly pop further than faces further away. This box's largest half
 * extent is CUBE_SX/2 = 0.5 units (kVerts[]), reached whenever a face turns
 * edge-on to the camera, so the worst case is
 * |ui_stereo_eye_shift()| * CUBE_STEREO_PX_PER_UNIT_Z * 0.5 = 1 * 8 * 0.5 =
 * 4px at slider==1 -- "max pop is subtle, ~4 px at slider=1" (this task's
 * brief). HARDWARE UNVERIFIED, same spirit as the roll-sign note on
 * gyro_cube_step_and_draw() below: ui_stereo_eye_shift()'s sign convention
 * (ui.c) mirrors devkitPro's own stereoscopic_2d sample rather than a
 * checked physical result. If the cube looks like it sinks INTO the screen
 * instead of popping OUT of it on real hardware, negate this constant. */
#define CUBE_STEREO_PX_PER_UNIT_Z 8.0f

/* ------------------------------------------------------------------------ */
/* tiny 3D types                                                            */
/* ------------------------------------------------------------------------ */

typedef struct { float x, y, z; } cube_vec3;
typedef struct { float m[3][3]; } cube_mat3;

static cube_vec3 cube_vec3_make(float x, float y, float z)
{
    cube_vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static float cube_dot(cube_vec3 a, cube_vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static cube_vec3 cube_mat3_apply(const cube_mat3 *m, cube_vec3 v)
{
    cube_vec3 r;
    r.x = m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z;
    r.y = m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z;
    r.z = m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z;
    return r;
}

static cube_mat3 cube_mat3_mul(const cube_mat3 *a, const cube_mat3 *b)
{
    cube_mat3 r;
    int i, j, k;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            float s = 0.0f;
            for (k = 0; k < 3; k++) {
                s += a->m[i][k] * b->m[k][j];
            }
            r.m[i][j] = s;
        }
    }
    return r;
}

static cube_mat3 cube_mat3_rot_x(float a)
{
    cube_mat3 r;
    float c = cosf(a), s = sinf(a);

    r.m[0][0] = 1.0f; r.m[0][1] = 0.0f; r.m[0][2] = 0.0f;
    r.m[1][0] = 0.0f; r.m[1][1] = c;    r.m[1][2] = -s;
    r.m[2][0] = 0.0f; r.m[2][1] = s;    r.m[2][2] = c;
    return r;
}

static cube_mat3 cube_mat3_rot_y(float a)
{
    cube_mat3 r;
    float c = cosf(a), s = sinf(a);

    r.m[0][0] = c;    r.m[0][1] = 0.0f; r.m[0][2] = s;
    r.m[1][0] = 0.0f; r.m[1][1] = 1.0f; r.m[1][2] = 0.0f;
    r.m[2][0] = -s;   r.m[2][1] = 0.0f; r.m[2][2] = c;
    return r;
}

static cube_mat3 cube_mat3_rot_z(float a)
{
    cube_mat3 r;
    float c = cosf(a), s = sinf(a);

    r.m[0][0] = c;    r.m[0][1] = -s;   r.m[0][2] = 0.0f;
    r.m[1][0] = s;    r.m[1][1] = c;    r.m[1][2] = 0.0f;
    r.m[2][0] = 0.0f; r.m[2][1] = 0.0f; r.m[2][2] = 1.0f;
    return r;
}

/* pitch about X, then yaw about Y, then roll about Z -- see the file header
 * for why roll-about-Z is the one that must go last (it is the only one that
 * survives the orthographic Z-drop as a plain in-plane rotation). */
static cube_mat3 cube_orientation(float pitch, float roll, float yaw)
{
    cube_mat3 rx = cube_mat3_rot_x(pitch);
    cube_mat3 ry = cube_mat3_rot_y(yaw);
    cube_mat3 rz = cube_mat3_rot_z(roll);
    cube_mat3 rxy = cube_mat3_mul(&rx, &ry);

    return cube_mat3_mul(&rz, &rxy);
}

/* ------------------------------------------------------------------------ */
/* the box                                                                  */
/* ------------------------------------------------------------------------ */

static const cube_vec3 kVerts[8] = {
    { -0.5f * CUBE_SX, -0.5f * CUBE_SY, -0.5f * CUBE_SZ }, /* 0 */
    {  0.5f * CUBE_SX, -0.5f * CUBE_SY, -0.5f * CUBE_SZ }, /* 1 */
    {  0.5f * CUBE_SX,  0.5f * CUBE_SY, -0.5f * CUBE_SZ }, /* 2 */
    { -0.5f * CUBE_SX,  0.5f * CUBE_SY, -0.5f * CUBE_SZ }, /* 3 */
    { -0.5f * CUBE_SX, -0.5f * CUBE_SY,  0.5f * CUBE_SZ }, /* 4 */
    {  0.5f * CUBE_SX, -0.5f * CUBE_SY,  0.5f * CUBE_SZ }, /* 5 */
    {  0.5f * CUBE_SX,  0.5f * CUBE_SY,  0.5f * CUBE_SZ }, /* 6 */
    { -0.5f * CUBE_SX,  0.5f * CUBE_SY,  0.5f * CUBE_SZ }, /* 7 */
};

typedef struct {
    int       idx[4];   /* quad, drawn as two triangles idx[0,1,2] idx[0,2,3] */
    cube_vec3 normal;    /* local-space, unit length, axis-aligned            */
} cube_face;

/* Face 0 is deliberately the +Z ("top") face: at identity orientation it is
 * the one facing the camera, and the math self-test (this file's report)
 * checks exactly that. */
static const cube_face kFaces[6] = {
    { { 4, 5, 6, 7 }, {  0.0f,  0.0f,  1.0f } }, /* +Z: top/front cover      */
    { { 1, 0, 3, 2 }, {  0.0f,  0.0f, -1.0f } }, /* -Z: back cover           */
    { { 1, 2, 6, 5 }, {  1.0f,  0.0f,  0.0f } }, /* +X: right edge           */
    { { 0, 4, 7, 3 }, { -1.0f,  0.0f,  0.0f } }, /* -X: left edge            */
    { { 3, 7, 6, 2 }, {  0.0f,  1.0f,  0.0f } }, /* +Y: top edge (height)    */
    { { 0, 1, 5, 4 }, {  0.0f, -1.0f,  0.0f } }, /* -Y: bottom edge (height) */
};
#define CUBE_FACE_COUNT ((int)(sizeof kFaces / sizeof kFaces[0]))

/* Directional light, fixed, roughly "from above and slightly to the right of
 * the viewer" -- picked so the amber top face reads bright at rest (dot with
 * (0,0,1) is the z component alone) without the side faces going flat black.
 * Normalised once per call; the cost is three multiplies and a sqrt on a
 * platform that runs this every vblank, which is not worth a static-init
 * dance. */
static cube_vec3 cube_light_dir(void)
{
    cube_vec3 l = cube_vec3_make(0.35f, 0.55f, 0.76f);
    float len = sqrtf(cube_dot(l, l));

    if (len > 0.0f) {
        l.x /= len; l.y /= len; l.z /= len;
    }
    return l;
}

static uint32_t cube_shade(uint32_t base, float k)
{
    uint32_t r = base & 0xFFu;
    uint32_t g = (base >> 8) & 0xFFu;
    uint32_t b = (base >> 16) & 0xFFu;
    uint32_t a = (base >> 24) & 0xFFu;

    r = (uint32_t)((float)r * k);
    g = (uint32_t)((float)g * k);
    b = (uint32_t)((float)b * k);
    return C2D_Color32((u8)r, (u8)g, (u8)b, (u8)a);
}

static uint32_t cube_face_base_colour(int face)
{
    switch (face) {
    case 0:  return ui_c_accent();    /* the top face: the one statement */
    case 1:  return ui_c_panel();     /* opposite cover                  */
    case 2:  /* fallthrough */
    case 3:  return ui_c_panel_hi();  /* left/right edges                */
    default: return ui_c_border();    /* top/bottom (height) edges       */
    }
}

/* ------------------------------------------------------------------------ */
/* orientation integration -- exposed to gyro_cube_selftest_native.c        */
/* ------------------------------------------------------------------------ */

static float cube_wrap_angle(float a)
{
    while (a > CUBE_PI)  { a -= 2.0f * CUBE_PI; }
    while (a < -CUBE_PI) { a += 2.0f * CUBE_PI; }
    return a;
}

static float cube_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* One integration step. `have_reading` false means "no gyro on this console
 * at all" (ctx->have_gyro == 0) as well as "read all zero" -- both look the
 * same to this function on purpose (spec point 4: "if gyro reads are all
 * zero for >2s ... fall back"). `pitch`/`roll`/`yaw` and `idle_secs` are
 * caller-owned state, updated in place, so the native math self-test can
 * drive this function directly without touching citro2d. */
static void cube_integrate(float dps[3], int have_reading, float dt,
                            float *pitch, float *roll, float *yaw,
                            float *idle_secs)
{
    int zero_reading = !have_reading ||
        (dps[0] == 0.0f && dps[1] == 0.0f && dps[2] == 0.0f);
    float decay = expf(-dt / CUBE_RECENTER_TAU);
    int idle;

    if (zero_reading) {
        *idle_secs += dt;
    } else {
        *idle_secs = 0.0f;
    }
    idle = (*idle_secs > CUBE_IDLE_AFTER);

    if (idle) {
        /* No live data: keep the box visibly alive with a slow yaw spin
         * (spec: ~20 deg/s) rather than freezing. Pitch/roll are NOT driven
         * here, and the re-centering below still applies to them, so the box
         * settles flat while it spins -- looks intentional rather than
         * broken. Yaw itself is exempt from re-centering while idle, or the
         * spin would fight its own decay back to zero. */
        *pitch *= decay;
        *roll  *= decay;
        *yaw   += CUBE_IDLE_YAW_DPS * CUBE_DEG2RAD * dt;
    } else {
        float dpitch = cube_clampf(dps[0] * CUBE_DEG2RAD * dt,
                                    -CUBE_MAX_STEP_RAD, CUBE_MAX_STEP_RAD);
        float droll  = cube_clampf(dps[1] * CUBE_DEG2RAD * dt,
                                    -CUBE_MAX_STEP_RAD, CUBE_MAX_STEP_RAD);
        float dyaw   = cube_clampf(dps[2] * CUBE_DEG2RAD * dt,
                                    -CUBE_MAX_STEP_RAD, CUBE_MAX_STEP_RAD);

        *pitch = (*pitch + dpitch) * decay;
        *roll  = (*roll  + droll)  * decay;
        *yaw   = (*yaw   + dyaw)   * decay;
    }

    *pitch = cube_wrap_angle(*pitch);
    *roll  = cube_wrap_angle(*roll);
    *yaw   = cube_wrap_angle(*yaw);
}

/* ------------------------------------------------------------------------ */
/* public entry point                                                       */
/* ------------------------------------------------------------------------ */

/* `scale` is the desired ON-SCREEN pixel size of the box's HEIGHT axis
 * (CUBE_SY) at identity orientation -- callers that want "fit inside a box
 * H pixels tall at 90%" pass H * 0.9f directly; this file owns the
 * width:height:thickness ratio and converts internally, so no caller needs
 * to know CUBE_SY.
 *
 * PROJECTION: orthographic (Z dropped after rotation, not divided by).
 * Chosen over a perspective divide because this box is meant to read as a
 * flat, icon-like status glyph rather than a scene with real depth -- a
 * perspective camera close enough to show foreshortening on an object this
 * small on screen also exaggerates edges toward the frame corners, which is
 * a distraction on a 72px-tall readout. The painter's-algorithm face sort
 * below already gives correct occlusion without it.
 *
 * HARDWARE UNVERIFIED (report this): the sign of "roll" as read from
 * ctx->st.gyro[1] versus which way this file spins the box in-plane. main.c
 * already resolved ONE hardware sign trap (pitch/roll axis-swap, its
 * scale_gyro() comment) against real hardware; this file adds a SECOND
 * mapping on top -- gyro sign to on-screen rotation direction -- that has
 * not been checked against a physical console. If tilting the left edge
 * down turns the box the wrong way, negate `roll` where it is read below. */
void gyro_cube_step_and_draw(app_ctx *ctx, float cx, float cy, float scale)
{
    static float s_pitch = 0.0f, s_roll = 0.0f, s_yaw = 0.0f;
    static float s_idle_secs = 0.0f;

    float dps[3];
    cube_mat3 R;
    cube_vec3 rverts[8];
    cube_vec3 light = cube_light_dir();
    float px_scale = scale / CUBE_SY;
    float eye_shift;
    int face_order[CUBE_FACE_COUNT];
    float face_z[CUBE_FACE_COUNT];
    int i, f;

    /* Channel mapping fixed against real hardware (2026-08-12): the sensor's
     * pitch reads opposite to the box's pitch axis, and its roll/yaw arrive
     * swapped relative to how this file spins the slab (in-plane spin is
     * driven by the YAW rate, the height-axis turn by the ROLL rate). */
    dps[0] = -(float)ctx->st.gyro[0] / 10.0f; /* deci-deg/s -> deg/s, pitch */
    dps[1] =  (float)ctx->st.gyro[2] / 10.0f; /* in-plane spin <- yaw rate  */
    dps[2] =  (float)ctx->st.gyro[1] / 10.0f; /* height-axis  <- roll rate  */

    /* Advance orientation on the first top pass only (ui.h): under stereo
     * this function is called twice per real frame, once per eye, and only
     * the DRAW half (below) may run twice -- otherwise the box would spin at
     * double rate whenever the slider is open. At slider==0 every call is
     * "first", so this is a no-op change from before stereo existed. */
    if (ui_top_pass_is_first()) {
        cube_integrate(dps, ctx->have_gyro, CUBE_NOMINAL_DT,
                       &s_pitch, &s_roll, &s_yaw, &s_idle_secs);
    }

    R = cube_orientation(s_pitch, s_roll, s_yaw);
    for (i = 0; i < 8; i++) {
        rverts[i] = cube_mat3_apply(&R, kVerts[i]);
    }

    /* Painter's algorithm: sort all 6 faces far-to-near by centroid Z (the
     * camera looks down -Z from +Z, so a LARGER post-rotation Z is CLOSER
     * and must draw LAST). Six elements -- a manual insertion sort is
     * clearer here than pulling in qsort() for a five-comparison job. */
    for (f = 0; f < CUBE_FACE_COUNT; f++) {
        const cube_face *fc = &kFaces[f];
        float z = (rverts[fc->idx[0]].z + rverts[fc->idx[1]].z +
                   rverts[fc->idx[2]].z + rverts[fc->idx[3]].z) * 0.25f;

        face_order[f] = f;
        face_z[f] = z;
    }
    for (i = 1; i < CUBE_FACE_COUNT; i++) {
        float zv = face_z[i];
        int   fv = face_order[i];
        int   j = i - 1;

        while (j >= 0 && face_z[j] > zv) {
            face_z[j + 1] = face_z[j];
            face_order[j + 1] = face_order[j];
            j--;
        }
        face_z[j + 1] = zv;
        face_order[j + 1] = fv;
    }

    /* 0 outside a stereo pass and on the mono/left-only pass at slider==0
     * (ui.h) -- so every sx[v] below is IDENTICAL to before stereo existed
     * in that case, satisfying "slider=0 must be pixel-identical to today"
     * without a separate code path for it. */
    eye_shift = ui_stereo_eye_shift();

    for (f = 0; f < CUBE_FACE_COUNT; f++) {
        const cube_face *fc = &kFaces[face_order[f]];
        cube_vec3 n = cube_mat3_apply(&R, fc->normal);
        float k = cube_clampf(cube_dot(n, light), 0.35f, 1.0f);
        uint32_t colour = cube_shade(cube_face_base_colour(face_order[f]), k);
        float sx[4], sy[4];
        int v;

        for (v = 0; v < 4; v++) {
            cube_vec3 p = rverts[fc->idx[v]];
            /* Per-vertex, depth-dependent: a face nearer the camera (larger
             * p.z -- this file's own painter's-algorithm sort above already
             * establishes that convention) shifts more than one further
             * away, which is what makes this read as depth rather than a
             * flat cardboard cutout sliding sideways. */
            float parallax = eye_shift * CUBE_STEREO_PX_PER_UNIT_Z * p.z;

            sx[v] = cx + p.x * px_scale + parallax;
            sy[v] = cy - p.y * px_scale; /* +Y up (docs/CONVENTIONS.md) -> screen -Y */
        }

        C2D_DrawTriangle(sx[0], sy[0], colour, sx[1], sy[1], colour,
                          sx[2], sy[2], colour, 0.5f);
        C2D_DrawTriangle(sx[0], sy[0], colour, sx[2], sy[2], colour,
                          sx[3], sy[3], colour, 0.5f);

        /* 1px darker outline: cheap (four short lines) and it is what turns
         * six flat-shaded triangles into a box a glance can parse. ui_c_bg()
         * rather than a darkened `colour` because it is the one tone in the
         * palette darker than every face colour at every lighting level. */
        for (v = 0; v < 4; v++) {
            int w = (v + 1) & 3;

            C2D_DrawLine(sx[v], sy[v], ui_c_bg(), sx[w], sy[w], ui_c_bg(),
                         1.0f, 0.5f);
        }
    }
}
