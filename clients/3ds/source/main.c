/* clients/3ds/source/main.c
 *
 * AtticPad 3DS client. The reference implementation every other console port
 * is written against (this is one of two platforms with real test hardware).
 *
 * WHAT THIS FILE IS, after the UI pass. It is the app shell and nothing else:
 * service bring-up, one hardware sample per frame into an apad_input_state,
 * and the frame loop that dispatches to whichever screen is current. It owns
 * no protocol and no layout.
 *
 *   - Everything protocol-shaped goes through the SHARED client engine,
 *     clients/common/apad_client.{c,h}: the S8 handshake, the S9
 *     ACK-every-copy rule, both directions of S6.6 PING, the retransmit
 *     timers, the S10 auth state machine and the INPUT_STATE pacing. That
 *     engine was transcribed FROM this file in M3 and hoisted into
 *     clients/common in M4, so driving the session through it is a return to
 *     source rather than a new dependency -- and it is what stops the next
 *     protocol fix from having to be made once per client (the
 *     server-originated PING of S6.6 was fixed three times, once per copy).
 *     The one part of libapad still called directly from here is tier-2
 *     broadcast discovery (app_discover): it needs a broadcast socket of its
 *     own, and the engine's socket is the session's.
 *   - Everything screen-shaped lives in screen_*.c behind the table in app.h.
 *     Adding a screen is one enum value, one file and one table row.
 *   - Everything drawing-shaped lives in ui.c / ui_widgets.c.
 *
 * soc:U bring-up lives in soc_3ds.c (memalign + socInit, mirrored from
 * references/3ds/sockets/source/sockets.c). The actual BSD socket calls come
 * from the SHARED shim/net_bsd.c (docs/DESIGN.md S3), not a 3DS-specific fork. The
 * monotonic clock (osGetTime()) is 3DS-specific and lives in time_3ds.c, the
 * same shape as shim/time_posix.c for Linux.
 *
 * THE UI IS citro2d, NOT consoleInit(). Through M2 this client was a text
 * console, which was right for bring-up -- every diagnostic anyone depended
 * on was a line someone read off the top screen -- and is not what ships
 * (the deferred 3DS UI pass). The two cannot coexist:
 * consoleInit() claims a screen's framebuffer for the software renderer while
 * citro3d wants the same screen as a GPU target. There is no printf left in
 * this client, including on the self-test screen.
 *
 * Gyroscope: HIDUSER_EnableGyroscope() + hidGyroRead(), present on BOTH Old
 * and New 3DS (unlike the C-stick), which is why Old 3DS profiles map it to
 * the right stick. references/3ds/ has no gyro sample, so the call signatures
 * and the angularRate field order came from libctru's hid.h and then had to
 * be corrected against real hardware -- see scale_gyro() and the sampling
 * code below for both traps. Deliberately still NOT sent: the accelerometer;
 * its capability bit stays clear, the spec-correct way to say "not
 * available" (docs/PROTOCOL.md S5.5/S6.3). That is a scope trim, not a
 * hardware limitation. Battery level (ptm:u) shipped 2026-08-12 -- see the
 * "battery" section below.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>   /* atexit() */
#include <string.h>

#include <3ds.h>

#include "app.h"
#include "camera_3ds.h"
#include "config_3ds.h"

/* soc_3ds.c: the one 3DS-specific bring-up step (memalign + socInit) that has
 * to happen before anything in the SHARED shim/net_bsd.c can be called. See
 * soc_3ds.c's header comment for why this isn't folded into net_bsd.c. */
int apad3ds_soc_bringup(Result *out_result);
size_t apad3ds_soc_bufsize(void);

/* ------------------------------------------------------------------------ */
/* the screen table                                                         */
/* ------------------------------------------------------------------------ */

/* Indexed by apad_screen_id. Adding a screen is: an id in app.h, an extern
 * there, one screen_<name>.c, and one row here. */
static const apad_screen *const kScreens[APAD_SCREEN_COUNT] = {
    &apad_screen_connect,
    &apad_screen_session,
    &apad_screen_qrscan,
    &apad_screen_selftest,
    &apad_screen_fatal
};

const char *app_screen_name(apad_screen_id id)
{
    /* Cast because apad_screen_id is an enum with no negative enumerators, so
     * gcc knows `id < 0` can never hold and -Wtype-limits says so. */
    if ((unsigned)id >= (unsigned)APAD_SCREEN_COUNT) {
        return "?";
    }
    return kScreens[id]->name;
}

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */
/* ------------------------------------------------------------------------ */

static int16_t clamp_i16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Circle Pad / C-Stick raw range is roughly +-156 (libctru's own examples
 * treat +-164 as the outer edge). Scaled linearly to the wire's
 * -32768..32767 -- NOT clamped-then-deadzoned, just scaled: docs/PROTOCOL.md
 * S5.3 is explicit that a client MUST NOT apply a deadzone, the server
 * profile owns that entirely. +Y is already up on this hardware
 * (circlePosition matches the XInput convention) so dy is used unmodified. */
static int16_t scale_stick(int16_t raw)
{
    return clamp_i16((int32_t)raw * 205);
}

/* Gyro raw ticks -> deci-degrees/second (docs/PROTOCOL.md S5, gyro[3]).
 *
 * DIVIDE, do not multiply. HIDUSER_GetGyroscopeRawToDpsCoefficient() returns
 * an LSB-per-degree-per-second figure (the MEMS convention, measured as
 * exactly 14.375 on this console -- the InvenSense value), so
 * dps = raw / coeff. The name reads like a multiplier and that is the trap:
 * multiplying inflated readings by coeff^2 (~206x) and a console lying
 * motionless on a table reported 196, -276 and -404 deg/s. Neither libctru's
 * hid.c nor 3dbrew documents the direction -- both were checked; it had to be
 * measured. The coefficient is still shown on the connect screen so the next
 * person to doubt a reading can check it rather than trust this comment.
 *
 * Floating point is fine here: docs/CONVENTIONS.md's no-float rule is scoped to core/
 * and shim/ (which must run on a 4 MB ARM9), and this build is already
 * -mfloat-abi=hard on a CPU with an FPU. Guard against a zero coefficient so
 * a failed HIDUSER call cannot divide by zero. */
static int16_t scale_gyro(int16_t raw, float coeff)
{
    float ddps = (coeff > 0.0001f) ? (((float)raw / coeff) * 10.0f) : 0.0f;
    int32_t v = (int32_t)(ddps >= 0.0f ? ddps + 0.5f : ddps - 0.5f);
    return clamp_i16(v);
}

/* touchPosition is screen-pixel space on the 320x240 bottom screen, +Y down
 * already (docs/PROTOCOL.md S5.3: touch is screen space, +Y down -- no
 * inversion needed, unlike sticks). Scaled linearly into -32768..32767; this
 * is a unit conversion, not a deadzone, so it stays on the client side. */
static int16_t scale_touch_x(uint16_t px) { return clamp_i16((int32_t)px * 65536 / 320 - 32768); }
static int16_t scale_touch_y(uint16_t py) { return clamp_i16((int32_t)py * 65536 / 240 - 32768); }

/* ------------------------------------------------------------------------ */
/* gyroscope lifecycle                                                      */
/* ------------------------------------------------------------------------ */

/* atexit-registered, same pattern as atexit(gfxExit) / soc_3ds.c's
 * atexit(socExit): registered exactly once, right after a successful
 * HIDUSER_EnableGyroscope(), so every early-return path still disables the
 * gyro cleanly. Registered after soc bring-up, so it runs BEFORE
 * socExit/gfxExit at exit (atexit is LIFO) -- fine, since disabling the gyro
 * needs only hid:USER, which neither of those teardown calls touches. */
static bool s_gyro_active = false;

static void gyro_atexit(void)
{
    if (s_gyro_active) {
        HIDUSER_DisableGyroscope();
        s_gyro_active = false;
    }
}

/* ------------------------------------------------------------------------ */
/* battery -- ptm:u, sampled at ~1 Hz                                       */
/* ------------------------------------------------------------------------ */

/* ptm:u bring-up/teardown mirrors soc_3ds.c's shape (its own file header
 * comment): Init checked against R_SUCCEEDED, Exit registered with atexit()
 * directly since ptmuExit() already matches atexit's `void (*)(void)`
 * signature exactly, same as gfxExit()/socExit() elsewhere in this file.
 * There is no references/3ds/ sample for ptm:u (grepped; none exists), so the
 * two calls this file needs -- PTMU_GetBatteryLevel(u8*) and
 * PTMU_GetBatteryChargeState(u8*) -- were read from libctru's own
 * include/3ds/services/ptmu.h rather than recalled.
 *
 * PTMU_GetBatteryLevel() returns a coarse 0..5 reading (libctru's own
 * Doxygen comment on the call), mapped below to the wire's 0..100 percent at
 * 20% per step -- the same granularity the OS's own five-bar battery icon
 * already uses, so this client is not claiming more precision than the
 * console exposes. */
static const uint8_t kBatteryStepPercent[6] = { 0, 20, 40, 60, 80, 100 };

static bool     s_ptmu_up = false;
static uint8_t  s_battery_percent = (uint8_t)APAD_BATTERY_UNKNOWN;
static int      s_battery_charging = 0;
static uint32_t s_battery_sampled_ms = 0;

#define BATTERY_SAMPLE_INTERVAL_MS 1000u

static void ptmu_atexit(void)
{
    if (s_ptmu_up) {
        ptmuExit();
        s_ptmu_up = false;
    }
}

/* Re-samples at most once every BATTERY_SAMPLE_INTERVAL_MS (a static
 * timestamp, per this task's brief -- "not every frame"); `force` bypasses
 * the gate for the one-time bring-up read in bring_up() so the very first
 * INPUT_STATE already carries a real level instead of 255 for up to a
 * second. A failed PTMU_GetBatteryLevel() leaves the last-known value in
 * place rather than falling back to "unknown" -- a single missed IPC call is
 * not the console's battery actually vanishing. */
static void battery_sample(int force)
{
    uint32_t now;
    u8 level, charging;

    if (!s_ptmu_up) {
        return;
    }
    now = apad_ticks_ms();
    if (!force && apad_time_since(now, s_battery_sampled_ms)
                  < BATTERY_SAMPLE_INTERVAL_MS) {
        return;
    }
    s_battery_sampled_ms = now;

    if (R_SUCCEEDED(PTMU_GetBatteryLevel(&level))) {
        if (level > 5u) {
            level = 5u; /* documented 0..5 (ptmu.h); clamp rather than index
                         * kBatteryStepPercent out of bounds on a value that
                         * has never been seen to exceed it */
        }
        s_battery_percent = kBatteryStepPercent[level];
    }
    if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging))) {
        s_battery_charging = (int)charging;
    }
}

/* For ui_widgets.c's header glyph (app.h). -1 means "no battery reading at
 * all" -- ptm:u never came up, which is also why APAD_CAP_BATTERY never
 * entered ctx->caps in bring_up() (docs/PROTOCOL.md S5.5: absence is the
 * capability bit, not a sentinel in the field). Otherwise 0..100, the exact
 * value this frame's ctx->st.battery carries -- the glyph and the wire can
 * never disagree, same principle as app_sample_input()'s comment on why
 * ctx->st is sampled on every screen. */
int app_battery_percent(void)
{
    return s_ptmu_up ? (int)s_battery_percent : -1;
}

/* Display-only: docs/PROTOCOL.md S5.5 says battery on the wire is a level,
 * nothing else, so charging state never reaches ctx->st -- it only colours
 * how this client's own glyph reads. */
int app_battery_charging(void)
{
    return s_ptmu_up ? s_battery_charging : 0;
}

/* ------------------------------------------------------------------------ */
/* gyro readout smoothing -- DISPLAY ONLY, never touches st.gyro[]          */
/* ------------------------------------------------------------------------ */

/* Per docs/PROTOCOL.md S5.3 the wire carries raw values and the server
 * profile owns ALL filtering -- a client that pre-smoothed st.gyro[] before
 * encoding it would destroy information the server can never recover, exactly
 * like applying a client-side deadzone. So st.gyro[] is untouched. This is a
 * second, purely cosmetic copy that only ever feeds the on-screen readout: a
 * rolling ~0.5s (30 frames @ 60Hz) window per axis, because three flickering
 * raw int16s are unreadable ("very noisy hard to say", user report).
 *
 * What survives from the six-line bar-graph version this replaced is the
 * NOISE figure. The bars existed to diagnose the 200x raw-to-dps bug, which
 * is fixed and recorded; the noise floor is worth keeping on screen forever,
 * because it is the number that says whether the server profile's gyro
 * deadzone is sized anywhere near right. */
#define GYRO_DISPLAY_WINDOW 30

static int16_t s_gyro_hist[3][GYRO_DISPLAY_WINDOW]; /* [axis][sample] */
static int     s_gyro_hist_idx = 0;
static int     s_gyro_hist_count = 0;

static void gyro_display_push(const int16_t gyro[3])
{
    int axis;

    for (axis = 0; axis < 3; axis++) {
        s_gyro_hist[axis][s_gyro_hist_idx] = gyro[axis];
    }
    s_gyro_hist_idx = (s_gyro_hist_idx + 1) % GYRO_DISPLAY_WINDOW;
    if (s_gyro_hist_count < GYRO_DISPLAY_WINDOW) {
        s_gyro_hist_count++;
    }
}

void app_gyro_stats(int axis, int32_t *out_mean, int16_t *out_noise)
{
    int32_t sum = 0;
    int16_t vmin = 32767, vmax = -32768;
    int i;

    if (s_gyro_hist_count == 0 || axis < 0 || axis > 2) {
        *out_mean = 0;
        *out_noise = 0;
        return;
    }
    for (i = 0; i < s_gyro_hist_count; i++) {
        int16_t v = s_gyro_hist[axis][i];

        sum += v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    *out_mean = sum / s_gyro_hist_count;
    *out_noise = (int16_t)((vmax - vmin) / 2);
}

int16_t app_gyro_noise_peak(void)
{
    int16_t worst = 0;
    int axis;

    for (axis = 0; axis < 3; axis++) {
        int32_t mean;
        int16_t noise;

        app_gyro_stats(axis, &mean, &noise);
        if (noise > worst) {
            worst = noise;
        }
    }
    return worst;
}

/* ------------------------------------------------------------------------ */
/* APT lifecycle diagnostics -- why did the main loop end?                  */
/* ------------------------------------------------------------------------ */

/* Coordinator report (2026-08-10 CIA install): the app "returned to the HOME
 * menu on its own, unprompted" -- consistent with the top-level
 * `while (aptMainLoop())` condition going false with NOTHING on screen to say
 * which or why. This app never explicitly unhooks: process exit tears the
 * hook down, same as every other *Init() here with no matching *Exit(). */
static volatile int s_last_apt_hook = -1; /* -1 == no hook observed yet */

/* THE HOME-THEN-SWITCH-APP CRASH, its mechanism, and why this callback is
 * where the fix belongs.
 *
 * references/3ds/ has NO aptHook sample at all (grepped; every sample there
 * is a bare `while (aptMainLoop())`), so this was read from the authority
 * that exists instead: devkitPro/libctru's own source/services/apt.c
 * (fetched and read directly, not recalled). Two facts from it that were not
 * knowable any other way on this project:
 *
 *   1. aptMainLoop() ITSELF BLOCKS for the whole time the console sits at
 *      the HOME menu. `aptHandleJumpToHome()` -> `aptJumpToHomeMenu()` fires
 *      APTHOOK_ONSUSPEND, releases this app's GSP rights, and then calls
 *      `aptWaitForWakeUp()`, which sits in a receive loop until the OS sends
 *      either a resume or a "wake up so you can exit" command -- all of it
 *      SYNCHRONOUSLY on the thread that called aptMainLoop(), i.e. THIS
 *      app's main thread, never on the separate aptEventHandlerThread
 *      libctru also runs. So ONSUSPEND/ONRESTORE fire here with no
 *      cross-thread race against anything this app's main thread owns
 *      (camera start/stop is documented UI-thread-only in camera_3ds.c).
 *   2. `aptMainLoop()` returns false for exactly one reason:
 *      `return !aptShouldClose();`, and `aptShouldClose()` is only ever true
 *      because APT set FLAG_ORDERTOCLOSE (or FLAG_CANCELLED). There is no
 *      other way out of this loop. So every time this app's own
 *      `while (aptMainLoop())` ends, the OS has ALREADY ordered a close --
 *      which, when the user just picked something from the HOME menu, means
 *      another app is at that moment starting up and waiting on exactly the
 *      services this app might still be holding: cam:u (the QR screen's
 *      capture thread, camera_3ds.c), the GPU/GSP citro3d owns, and the
 *      socket. That, not a mysterious/unexplained aptMainLoop() return, is
 *      the actual "console crashes when the user goes home and switches to
 *      another app": this app used to hold all three for up to three more
 *      seconds via hold_exit_screen() below, fighting the incoming app for
 *      them. See main()'s comment at the loop's exit for the other half of
 *      the fix.
 *
 * So: stop the camera thread and disable the gyro RIGHT HERE, on
 * APTHOOK_ONSUSPEND, rather than waiting for the atexit()-registered
 * teardown that only runs once main() returns -- a capture thread left
 * running for however long the console sits at the HOME menu (unknown, could
 * be minutes) is cam:u held away from every other app with no idea whether
 * it is coming back, exactly "the classic hard crash" this task named.
 * apad3ds_cam_stop() is idempotent (a no-op if the camera was never
 * running), so this is safe to call on every suspend regardless of which
 * screen the user was on. ONRESTORE brings the gyro back; the camera does
 * NOT restart itself here -- screen_qrscan.c's QR_SCAN case already checks
 * apad3ds_cam_running() every update() and re-arms via QR_ARM if it finds it
 * stopped, which is the same "draw a frame before the blocking calls" shape
 * every other blocking sequence in this client already uses (app.h). */
static void apt_hook_cb(APT_HookType hook, void *param)
{
    app_ctx *ctx = (app_ctx *)param;

    s_last_apt_hook = (int)hook;

    switch (hook) {
        case APTHOOK_ONSUSPEND:
            apad3ds_cam_stop();
            if (s_gyro_active) {
                HIDUSER_DisableGyroscope();
                s_gyro_active = false;
            }
            break;

        case APTHOOK_ONRESTORE:
            if (ctx != NULL && ctx->have_gyro && !s_gyro_active) {
                if (R_SUCCEEDED(HIDUSER_EnableGyroscope())) {
                    s_gyro_active = true;
                }
            }
            break;

        case APTHOOK_ONEXIT:
            /* Belt and braces: on this libctru, ONEXIT fires from aptExit()
             * during process teardown, which normally runs after this app's
             * own atexit(cam_atexit)/atexit(gyro_atexit) already have (see
             * camera_3ds.h and this file's gyro_atexit()) -- so this is
             * expected to be a no-op in practice, kept because the exact
             * ordering relative to atexit() is the one part of this file's
             * APT reading that could not be confirmed without hardware. */
            apad3ds_cam_stop();
            if (s_gyro_active) {
                HIDUSER_DisableGyroscope();
                s_gyro_active = false;
            }
            break;

        default:
            break;
    }
}

const char *app_apt_hook_name(int hook)
{
    switch (hook) {
        case APTHOOK_ONSUSPEND: return "ONSUSPEND";
        case APTHOOK_ONRESTORE: return "ONRESTORE";
        case APTHOOK_ONSLEEP:   return "ONSLEEP";
        case APTHOOK_ONWAKEUP:  return "ONWAKEUP";
        case APTHOOK_ONEXIT:    return "ONEXIT";
        default:                return "(none)";
    }
}

/* ------------------------------------------------------------------------ */
/* cross-screen messaging                                                   */
/* ------------------------------------------------------------------------ */

void app_note(app_ctx *ctx, int level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(ctx->banner, sizeof ctx->banner, fmt, ap);
    va_end(ap);
    ctx->banner_level = level;
}

/* Why the session ended, for the connect screen's banner. The engine reports
 * it as an enum apad_session_close in apad_client_stats.close_reason instead
 * of any screen watching for a BYE itself. */
const char *app_close_reason_text(int reason)
{
    switch (reason) {
        case APAD_CLOSE_LOCAL:        return "session closed locally";
        case APAD_CLOSE_PEER_BYE:     return "server sent BYE";
        case APAD_CLOSE_IDLE_TIMEOUT: return "idle timeout -- 3000ms with nothing received (S11)";
        case APAD_CLOSE_RETX_FAILED:  return "retransmits exhausted (S9) -- server stopped answering";
        case APAD_CLOSE_PEER_ERROR:   return "server no longer knows this session (ERROR 7)";
        default:                      return "session ended";
    }
}

/* ------------------------------------------------------------------------ */
/* tier-2 broadcast discovery                                               */
/* ------------------------------------------------------------------------ */

/* A socket of its own, because it needs SO_BROADCAST and because the engine's
 * socket belongs to the session. Closed again immediately: shim/net_bsd.c's
 * pool is four sockets, and holding one for the rest of the run to send a
 * single DISCOVER would be the kind of leak that only shows up on the console
 * with the fewest resources. */
int app_discover(app_ctx *ctx)
{
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t rbuf[APAD_MAX_DATAGRAM];
    apad_sock *sock;
    apad_addr bcast, from;
    apad_header hdr;
    apad_packet pkt;
    apad_announce ann;
    int n, rn;

    sock = apad_udp_open(0);
    if (sock == NULL) {
        return 0;
    }
    if (apad_udp_set_broadcast(sock, 1) != APAD_OK) {
        apad_udp_close(sock);
        return 0;
    }

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = APAD_MAGIC;
    hdr.version = APAD_VERSION;
    hdr.type = (uint8_t)APAD_MSG_DISCOVER;
    n = apad_packet_build(buf, sizeof buf, &hdr, NULL, 0, NULL, 0);
    if (n < 0) {
        apad_udp_close(sock);
        return 0;
    }

    apad_addr_broadcast(&bcast, (uint16_t)APAD_DEFAULT_PORT);
    (void)apad_udp_send(sock, &bcast, buf, (size_t)n);

    rn = apad_udp_recv(sock, &from, rbuf, sizeof rbuf, 500);
    apad_udp_close(sock);
    if (rn <= 0) {
        return 0;
    }

    memset(&pkt, 0, sizeof pkt);
    if (apad_packet_parse(rbuf, (size_t)rn, &pkt) < 0
        || pkt.header.type != (uint8_t)APAD_MSG_ANNOUNCE) {
        return 0;
    }
    memset(&ann, 0, sizeof ann);
    if (apad_decode_announce(pkt.payload, pkt.payload_len, &ann) < 0) {
        return 0;
    }

    apad_text_get(ctx->server_name, sizeof ctx->server_name, ann.server_name,
                  APAD_NAME_LEN);
    /* server_port is authoritative (docs/PROTOCOL.md S6.2); the IP comes from
     * the reply's source address. */
    snprintf(ctx->ip_text, sizeof ctx->ip_text, "%u.%u.%u.%u",
             (unsigned)from.ip[0], (unsigned)from.ip[1], (unsigned)from.ip[2],
             (unsigned)from.ip[3]);
    snprintf(ctx->port_text, sizeof ctx->port_text, "%u",
             (unsigned)ann.server_port);
    ctx->from_announce = 1;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* one hardware sample per frame                                            */
/* ------------------------------------------------------------------------ */

/* Fills ctx->st, the exact struct apad_client_pump() encodes. Run on EVERY
 * screen, not just the session one, so the connect screen's readout can prove
 * the controls work before there is a network to blame -- and so the readout
 * and the wire can never show different things. */
static const char kDeviceName[] = "AtticPad 3DS";

static void app_sample_input(app_ctx *ctx)
{
    circlePosition cpad, cstick;
    u32 held = ctx->keys_held;

    memset(&ctx->st, 0, sizeof ctx->st);

    if (held & KEY_A) ctx->st.buttons |= APAD_BTN_A;
    if (held & KEY_B) ctx->st.buttons |= APAD_BTN_B;
    if (held & KEY_X) ctx->st.buttons |= APAD_BTN_X;
    if (held & KEY_Y) ctx->st.buttons |= APAD_BTN_Y;
    if (held & KEY_DUP) ctx->st.buttons |= APAD_BTN_DPAD_UP;
    if (held & KEY_DDOWN) ctx->st.buttons |= APAD_BTN_DPAD_DOWN;
    if (held & KEY_DLEFT) ctx->st.buttons |= APAD_BTN_DPAD_LEFT;
    if (held & KEY_DRIGHT) ctx->st.buttons |= APAD_BTN_DPAD_RIGHT;
    if (held & KEY_L) ctx->st.buttons |= APAD_BTN_L;
    if (held & KEY_R) ctx->st.buttons |= APAD_BTN_R;
    if (ctx->have_cstick) {
        if (held & KEY_ZL) ctx->st.buttons |= APAD_BTN_ZL;
        if (held & KEY_ZR) ctx->st.buttons |= APAD_BTN_ZR;
    }
    if (held & KEY_START) ctx->st.buttons |= APAD_BTN_START;
    if (held & KEY_SELECT) ctx->st.buttons |= APAD_BTN_SELECT;
    /* Until 2026-08-12 KEY_SELECT and KEY_START were reserved by the UI
     * (self-test trigger and disconnect) and never reached the wire, which
     * left games with no usable Start/Select on the virtual pad. Those UI
     * actions moved to touchscreen buttons on the session screen's bottom
     * strip (screen_session.c), so the physical keys are now ordinary wire
     * buttons like every other one above -- including OUTSIDE a session,
     * where that is harmless: nothing sends ctx->st until session_update()
     * calls apad_client_pump(), and the connect/fatal/qrscan screens still
     * read KEY_SELECT themselves via app_pressed() for their own self-test
     * entry points, unchanged. */

    hidCircleRead(&cpad);
    ctx->st.axes[APAD_AXIS_LX] = scale_stick(cpad.dx);
    ctx->st.axes[APAD_AXIS_LY] = scale_stick(cpad.dy); /* +Y already up: do
                                                        * NOT invert. */
    if (ctx->have_cstick) {
        irrstScanInput();
        hidCstickRead(&cstick);
        ctx->st.axes[APAD_AXIS_RX] = scale_stick(cstick.dx);
        ctx->st.axes[APAD_AXIS_RY] = scale_stick(cstick.dy);
    }

    if (ctx->have_gyro) {
        /* libctru's angularRate field COMMENTS ARE WRONG about which field is
         * which. Its header labels x as Roll and y as Pitch. Measured on real
         * hardware, the opposite holds: tilting the top screen (pitch) moves
         * x, tipping the console sideways (roll) moves y. The first build
         * followed the header's labels and had pitch and roll transposed.
         * Also note the in-memory order is x, z, y -- accessed by field name
         * below, so it never matters here (and per docs/CONVENTIONS.md this is never
         * memcpy'd or cast onto a packet buffer regardless; codec.c owns
         * that). docs/PROTOCOL.md S5's gyro[3] order is pitch, roll, yaw. */
        hidGyroRead(&ctx->gyro_raw);
        ctx->st.gyro[0] = scale_gyro(ctx->gyro_raw.x, ctx->gyro_coeff); /* pitch, NOT y */
        ctx->st.gyro[1] = scale_gyro(ctx->gyro_raw.y, ctx->gyro_coeff); /* roll,  NOT x */
        ctx->st.gyro[2] = scale_gyro(ctx->gyro_raw.z, ctx->gyro_coeff); /* yaw */
        gyro_display_push(ctx->st.gyro);
    }

    if (ctx->touch_held) {
        ctx->st.buttons |= APAD_BTN_TOUCH_PRESS;
        ctx->st.touch_count = 1;
        ctx->st.touches[0].id = 1;
        ctx->st.touches[0].pressure = 0; /* binary touch, no pressure sensor */
        ctx->st.touches[0].x = scale_touch_x(ctx->touch.px);
        ctx->st.touches[0].y = scale_touch_y(ctx->touch.py); /* +Y already
                                                              * down: screen
                                                              * space. */
    }

    /* battery_sample() self-gates to ~1 Hz; ctx->st was memset to 0 above, so
     * a console with no ptm:u reading (s_ptmu_up false) sends 0 -- the
     * zero-fill docs/PROTOCOL.md S5.5 specifies for hardware that has none --
     * and APAD_CAP_BATTERY was never set in ctx->caps either (bring_up()),
     * which is what actually tells a server the field means nothing. */
    battery_sample(0);
    if (s_ptmu_up) {
        ctx->st.battery = s_battery_percent;
    }
}

/* ------------------------------------------------------------------------ */
/* bring-up                                                                 */
/* ------------------------------------------------------------------------ */

/* Returns 1 on success. On failure fills ctx->fatal and returns 0; the caller
 * starts on APAD_SCREEN_FATAL, which can still run the self-test. */
static int bring_up(app_ctx *ctx)
{
    Result soc_rc = 0;
    bool is_new3ds = false;

    if (!apad3ds_soc_bringup(&soc_rc)) {
        snprintf(ctx->fatal, sizeof ctx->fatal,
                 "socInit failed: 0x%08lX", (unsigned long)soc_rc);
        return 0;
    }
    ctx->soc_bufsize = (unsigned)apad3ds_soc_bufsize();

    if (apad_net_init() != APAD_OK) {
        snprintf(ctx->fatal, sizeof ctx->fatal,
                 "apad_net_init() failed (shim/net_bsd.c)");
        return 0;
    }

    if (R_SUCCEEDED(APT_CheckNew3DS(&is_new3ds)) && is_new3ds) {
        ctx->is_new3ds = 1;
        /* The C-stick and ZL/ZR come from the Circle Pad Pro / New 3DS IR
         * service, not from hid -- hence irrstInit() rather than anything
         * gyro-shaped. New 3DS only; on an Old 3DS the server's profile
         * substitutes gyro for the missing right stick. */
        if (R_SUCCEEDED(irrstInit())) {
            ctx->have_cstick = 1;
        }
    }

    /* Gyroscope: present on BOTH Old and New 3DS. HIDUSER_EnableGyroscope()
     * and the coefficient read both go over hid:USER, already up via
     * gfxInitDefault() -- there is no gyroInit() in libctru, and irrstInit()
     * above is unrelated. */
    if (R_SUCCEEDED(HIDUSER_EnableGyroscope())) {
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&ctx->gyro_coeff))) {
            ctx->have_gyro = 1;
            s_gyro_active = true;
            atexit(gyro_atexit);
        } else {
            HIDUSER_DisableGyroscope();
        }
    }

    /* Battery: ptm:u, mirroring soc_3ds.c's own Init-checked/Exit-via-atexit
     * shape (see the battery_sample() section above for why -- no
     * references/3ds/ sample exists for this service). An immediate FORCED
     * sample follows a successful Init so the very first HELLO/INPUT_STATE
     * already carries a real percentage rather than 255 for up to a second. */
    if (R_SUCCEEDED(ptmuInit())) {
        s_ptmu_up = true;
        atexit(ptmu_atexit);
        battery_sample(1);
    }

    ctx->caps = APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_SHOULDER
              | APAD_CAP_STICK_L | APAD_CAP_TOUCH;
    if (ctx->have_cstick) {
        ctx->caps |= APAD_CAP_STICK_R | APAD_CAP_SHOULDER2;
    }
    if (ctx->have_gyro) {
        ctx->caps |= APAD_CAP_GYRO;
    }
    if (s_ptmu_up) {
        ctx->caps |= APAD_CAP_BATTERY;
    }

    /* The engine owns its socket and calls apad_net_init() itself, but soc:U
     * must already be up -- which is why this is last. */
    ctx->client = apad_client_create(kDeviceName, ctx->caps);
    if (ctx->client == NULL) {
        snprintf(ctx->fatal, sizeof ctx->fatal,
                 "apad_client_create() failed (socket or memory)");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* the exit screen                                                          */
/* ------------------------------------------------------------------------ */

/* Holds a screen showing WHY the app is ending, instead of falling straight
 * back to HOME with no explanation -- "an app that vanishes silently is
 * exactly what cost us three iterations on the START collision"
 * (coordinator). Deliberately does NOT gate on aptMainLoop(): if APT is why
 * the loop ended, aptMainLoop() may already report false permanently, and
 * gating this on it would make it impossible to show in exactly the case it
 * exists for. A bounded ~3s of frames is a best-effort hold -- readable if
 * the OS lets the process keep running a little longer, silently cut short if
 * it does not. There is nothing more a userland app can do about a hard
 * OS-level teardown. */
static void hold_exit_screen(app_ctx *ctx)
{
    int frame;

    for (frame = 0; frame < 180; frame++) {
        hidScanInput();
        if (frame > 10 && hidKeysDown() != 0u) {
            break;
        }
        ui_frame_begin();
        ui_screen_top();
        ui_header(UI_TOP_W, "AtticPad 3DS  v" APAD_VERSION_STR, "exiting",
                  ui_c_dim());
        ui_textf(UI_TOP_W * 0.5f, 70.0f, UI_S_HEAD, ui_c_text(),
                 UI_ALIGN_CENTER, "session ended");
        ui_textf_fit(UI_TOP_W * 0.5f, 108.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, UI_TOP_W - 16.0f, "%s",
                     ctx->exit_reason);
        ui_textf(UI_TOP_W * 0.5f, 150.0f, UI_S_TINY, ui_c_border(),
                 UI_ALIGN_CENTER, "last APT hook: %s",
                 app_apt_hook_name(ctx->last_apt_hook));
        ui_textf(UI_TOP_W * 0.5f, 200.0f, UI_S_TINY, ui_c_dim(),
                 UI_ALIGN_CENTER, "press any button, or wait ~3s");
        ui_screen_bottom();
        ui_header(UI_BOT_W, "goodbye", NULL, ui_c_dim());
        ui_frame_end();
    }
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    /* static, not on the stack: app_ctx carries the input snapshot and several
     * message buffers, and the 3DS main thread's stack is not generous. */
    static app_ctx ctx;
    /* static is LOAD-BEARING, learned from a real crash: aptHook() chains
     * this cookie into APT's hook LIST — the cookie IS the list node, and
     * this app never unhooks it (see apt_hook_cb's comment). The old
     * console-UI client kept it on main()'s stack and survived, because
     * nothing walked the list after main returned. citro3d changed that:
     * C3D_Fini() (atexit-ordered before gfxExit) calls aptUnhook() for its
     * OWN hook, which walks the chain THROUGH this node — and a node in a
     * dead stack frame is garbage by then. On hardware that read data-aborts
     * with FAR = an instruction word (the "next" pointer was read from
     * memory now holding code). Static storage keeps the node alive for
     * exactly as long as the registration does. */
    static aptHookCookie apt_hook_cookie;
    apad_screen_id cur;

    (void)argc;
    (void)argv;

    memset(&ctx, 0, sizeof ctx);
    ctx.last_apt_hook = -1;

    gfxInitDefault();
    atexit(gfxExit);   /* registered BEFORE ui_init()'s renderer and before
                        * apad3ds_soc_bringup()'s atexit(socExit), so both run
                        * first at shutdown (atexit is LIFO) -- mirrors
                        * references/3ds/sockets/source/sockets.c. */
    if (!ui_init()) {
        /* No renderer means no way to say anything at all. There is nothing
         * useful left to do and nothing to say it on. */
        return 1;
    }
    atexit(ui_exit);

    /* Registered as early as possible so it captures a suspend/sleep/exit
     * event during bring-up too, not just inside the frame loop. `&ctx`
     * (not NULL) because ONRESTORE needs ctx->have_gyro to know whether to
     * re-enable the gyroscope -- see apt_hook_cb()'s header comment. ctx is
     * `static`, so the pointer stays valid for the hook's whole life. */
    aptHook(&apt_hook_cookie, apt_hook_cb, &ctx);

    /* Defaults for the connect screen's fields. EMPTY unless config_3ds.c's
     * saved file says otherwise -- a fresh unit must never dial an address
     * nobody chose (2026-08-12 report: "it still prefills the server (this
     * box I believe) on first boot and tries to connect"; this line used to
     * hardcode this project's own dev server's LAN address via a bare
     * snprintf() into ctx.ip_text, which is exactly that bug). ctx.ip_text
     * is already "" from the memset above; apad3ds_config_load() only fills
     * it back in from sdmc:/3ds/atticpad/atticpad.cfg, written by
     * screen_session.c the first time a PREVIOUS connect reached ACTIVE --
     * never from anything typed but not yet connected, and never with the
     * pairing secret
     * (config_3ds.h). Tier-2 discovery (ctx.want_discovery below) still runs
     * first and overwrites whatever this loads the moment the LAN answers a
     * DISCOVER. */
    snprintf(ctx.port_text, sizeof ctx.port_text, "%u",
             (unsigned)APAD_DEFAULT_PORT);
    (void)apad3ds_config_load(ctx.ip_text, sizeof ctx.ip_text,
                              ctx.port_text, sizeof ctx.port_text);
    ctx.want_boot_combo = 1;
    ctx.want_discovery = 1;
    ctx.selftest_return = APAD_SCREEN_CONNECT;

    cur = bring_up(&ctx) ? APAD_SCREEN_CONNECT : APAD_SCREEN_FATAL;
    kScreens[cur]->enter(&ctx);

    {
    /* Which way did the loop end: this app's own decision (ctx.want_exit,
     * set by screen_connect.c / screen_fatal.c) or APT's? The distinction
     * matters for what happens right after the loop -- see below and
     * apt_hook_cb()'s header comment for why `while (aptMainLoop())` itself
     * returning false ALWAYS means the OS ordered this app to close. */
    int loop_ended_by_apt = 1;

    while (aptMainLoop()) {
        apad_screen_id next;

        hidScanInput();
        ctx.keys_held = hidKeysHeld();
        ctx.keys_down = hidKeysDown();
        ctx.last_apt_hook = s_last_apt_hook;

        /* THE RELEASED-ONCE GATE, in one place for the whole app. See app.h's
         * comment on keys_armed for the hardware failure this prevents: a key
         * held across a screen change, or across a blocking call with no
         * hidScanInput() in it, reads back as a fresh press. */
        if (ctx.keys_held == 0u) {
            ctx.keys_armed = 1;
        }

        hidTouchRead(&ctx.touch);
        ctx.touch_held = (ctx.keys_held & KEY_TOUCH) != 0u;
        ctx.touch_pressed = ctx.keys_armed
                         && ((ctx.keys_down & KEY_TOUCH) != 0u);

        app_sample_input(&ctx);

        next = kScreens[cur]->update(&ctx);
        if (ctx.want_exit) {
            loop_ended_by_apt = 0;
            break;
        }
        if (next != cur) {
            cur = next;
            ctx.keys_armed = 0;   /* the new screen must see a release first */
            kScreens[cur]->enter(&ctx);
        }

        ui_frame_begin();
        ui_screen_top();
        kScreens[cur]->draw_top(&ctx);
        if (ui_stereo_slider_active()) {
            /* The right-eye pass (ui.h). Skipped outright at slider==0,
             * which is what keeps that case pixel-identical to how this
             * client rendered before this pass existed. draw_top() simply
             * runs a second time rather than being duplicated by hand:
             * every flat UI element draws at the same coordinates both
             * times (zero parallax by construction), and only
             * gyro_cube_step_and_draw() reads the per-eye shift
             * (ui_stereo_eye_shift()) to move -- it also gates its own
             * once-per-frame orientation integration on
             * ui_top_pass_is_first() so calling draw_top() twice cannot
             * make the cube spin twice as fast. */
            ui_screen_top_right();
            kScreens[cur]->draw_top(&ctx);
        }
        ui_screen_bottom();
        kScreens[cur]->draw_bottom(&ctx);
        ui_frame_end();
    }

    /* THE OTHER HALF OF THE HOME-THEN-SWITCH-APP FIX (apt_hook_cb()'s header
     * comment has the mechanism). Camera and gyro first, and BEFORE any more
     * drawing: if the loop ended because APT ordered a close, another app is
     * at this exact moment starting up and wants cam:u / GSP / the socket
     * back, and every millisecond this app keeps them is a millisecond taken
     * from that app's own bring-up. apad3ds_cam_stop() is idempotent, so this
     * costs nothing extra on the ordinary path where ONSUSPEND or ONEXIT
     * (apt_hook_cb()) already did it. */
    apad3ds_cam_stop();
    if (s_gyro_active) {
        HIDUSER_DisableGyroscope();
        s_gyro_active = false;
    }

    /* An empty exit_reason means the loop's own condition ended it rather than
     * a deliberate exit -- the coordinator's top suspicion for the unprompted
     * return-to-HOME. */
    if (ctx.exit_reason[0] == '\0') {
        snprintf(ctx.exit_reason, sizeof ctx.exit_reason,
                 "aptMainLoop() returned false (last APT hook=%s)",
                 app_apt_hook_name(ctx.last_apt_hook));
    }

    if (loop_ended_by_apt) {
        /* APT has ALREADY ordered this app to close (the only way
         * `while (aptMainLoop())` above can have ended -- confirmed against
         * libctru's own source, not assumed; see apt_hook_cb()'s header
         * comment). No held screen here: every one of the up to 180 frames
         * (~3s) hold_exit_screen() used to draw in this exact situation was
         * this app continuing to hold citro3d's GPU rights, the socket and
         * (if the camera screen was active) cam:u, all three wanted right
         * now by whatever the user just launched -- which is the crash this
         * task exists to fix. Return through main() as fast as the
         * remaining teardown allows, exactly like every references/3ds/
         * sample (none of them draw anything after their main loop ends). */
    } else {
        /* App-driven exit (screen_fatal.c / screen_connect.c's EXIT button):
         * APT has not asked for anything, nobody else is waiting on this
         * console's resources, and the original UX problem this screen
         * fixed -- "the app vanished with no explanation" -- is still real
         * for this path. Safe to hold here. */
        hold_exit_screen(&ctx);
    }
    }

    /* Closes the socket, and sends a BYE first if a session somehow survived
     * the loop -- the aptMainLoop()-went-false path has no other chance to say
     * goodbye. */
    apad_client_destroy(ctx.client);
    return 0;
}
