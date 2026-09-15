/*
 * clients/psp/source/main.c -- app shell for the PSP client.
 *
 * Service bring-up, one controller sample per frame into the wire's
 * apad_input_state, and the screen dispatch loop. Mirrors
 * clients/3ds/source/main.c; the protocol lives in clients/common and core,
 * and nothing here touches a packet.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "atticpad/version.h"
#include "app.h"
#include "config_psp.h"
#include "devlog.h"
#include "power_psp.h"
#include "ui.h"

PSP_MODULE_INFO("AtticPad", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* The net sample's own figure. The default is 256 KB; a hardware boot that
 * stopped at a black screen made "does the main thread have the sample's
 * stack" a question worth not asking twice. */
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
/* Mirrors pspsdk's net sample: sceNetInit() wants a real pool, and the
 * default heap is not it. -2048 means "all but 2 MB", leaving room for the
 * two 522 KB framebuffers. */
#ifdef APAD_PSP_STDOUT
/* PSPLINK build: PSPLINK is resident in RAM while our module runs, so the
 * shipping "-2048 = all but 2 MB" heap claim collides with it and ldstart
 * crashes the debugger. A fixed 12 MB covers the two 522 KB framebuffers,
 * the 128 KB net pool and everything else, and leaves PSPLINK its space. */
PSP_HEAP_THRESHOLD_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(12288);
#else
PSP_HEAP_THRESHOLD_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-2048);
#endif

static const apad_screen *const kScreens[APAD_SCREEN_COUNT] = {
    &apad_screen_connect,
    &apad_screen_session,
    &apad_screen_selftest,
    &apad_screen_fatal
};

/* ---- input --------------------------------------------------------------- */

int app_pressed(const app_ctx *ctx, unsigned int mask)
{
    return ctx->keys_armed && ((ctx->keys_down & mask) != 0u);
}

void app_disarm(app_ctx *ctx)
{
    ctx->keys_armed = 0;
    ctx->keys_prev  = 0xFFFFFFFFu;   /* nothing counts as newly pressed until
                                      * everything has been seen released */
}

void app_note(app_ctx *ctx, int level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(ctx->banner, sizeof ctx->banner, fmt, ap);
    va_end(ap);
    ctx->banner_level = level;
}

static int16_t clamp_i16(int32_t v)
{
    if (v >  32767) { return  32767; }
    if (v < -32768) { return -32768; }
    return (int16_t)v;
}

/*
 * The nub is unsigned char, nominal centre 128. 258 = ceil(32767/127) puts
 * full right at 127*258 = 32766 and lets full left (-128*258) clamp to
 * -32768, so BOTH rails are reachable; 256 would strand 255 counts.
 *
 * This is a unit conversion and nothing else. PROTOCOL.md S5.3 gives the
 * server profile sole ownership of deadzone, curve and inversion, and a
 * client that "helpfully" centres on the resting value at boot has shipped a
 * deadzone in disguise -- and baked in whatever the stick was doing while the
 * app launched.
 */
static int16_t nub_axis(unsigned char raw)
{
    return clamp_i16(((int32_t)raw - 128) * 258);
}

/* PORTING.md: the wire is +Y up, and "PSP and Vita report +Y down and must
 * invert". Negate in int32 BEFORE clamping -- negating an int16_t of -32768
 * is signed overflow. */
static int16_t nub_axis_inv(unsigned char raw)
{
    return clamp_i16(-(((int32_t)raw - 128) * 258));
}

/*
 * Face buttons map BY POSITION, not by label.
 *
 * The wire uses the Nintendo convention (A right, B bottom, X top, Y left)
 * and the SERVER translates that to Xbox. So Circle -- the right-hand button
 * -- is wire A, and Cross is wire B. Mapping Cross to A because Cross is
 * "confirm" on a western PSP would mirror every game's face buttons, and the
 * symptom would look like a server profile bug.
 */
static void sample_input(app_ctx *ctx)
{
    SceCtrlData pad;
    unsigned int h;

    sceCtrlPeekBufferPositive(&pad, 1);   /* Peek, not Read: Read blocks for
                                           * vblank and ui_frame_end() already
                                           * owns that wait. */
    h = pad.Buttons;

    ctx->keys_down = h & ~ctx->keys_prev;
    ctx->keys_held = h;
    ctx->keys_prev = h;
    if (h == 0u) {
        ctx->keys_armed = 1;              /* released once: edges count again */
    }

    memset(&ctx->st, 0, sizeof ctx->st);
    if (h & PSP_CTRL_CIRCLE)   { ctx->st.buttons |= APAD_BTN_A; }
    if (h & PSP_CTRL_CROSS)    { ctx->st.buttons |= APAD_BTN_B; }
    if (h & PSP_CTRL_TRIANGLE) { ctx->st.buttons |= APAD_BTN_X; }
    if (h & PSP_CTRL_SQUARE)   { ctx->st.buttons |= APAD_BTN_Y; }
    if (h & PSP_CTRL_UP)       { ctx->st.buttons |= APAD_BTN_DPAD_UP; }
    if (h & PSP_CTRL_DOWN)     { ctx->st.buttons |= APAD_BTN_DPAD_DOWN; }
    if (h & PSP_CTRL_LEFT)     { ctx->st.buttons |= APAD_BTN_DPAD_LEFT; }
    if (h & PSP_CTRL_RIGHT)    { ctx->st.buttons |= APAD_BTN_DPAD_RIGHT; }
    if (h & PSP_CTRL_LTRIGGER) { ctx->st.buttons |= APAD_BTN_L; }
    if (h & PSP_CTRL_RTRIGGER) { ctx->st.buttons |= APAD_BTN_R; }
    if (h & PSP_CTRL_START)    { ctx->st.buttons |= APAD_BTN_START; }
    if (h & PSP_CTRL_SELECT)   { ctx->st.buttons |= APAD_BTN_SELECT; }
    /* HOME and HOLD are never sent. HOME belongs to the system menu, and a
     * bit stuck on while that menu is up would be pressed forever; HOLD is a
     * switch, not a button, and its real consequence is drawn instead. */

    ctx->st.axes[APAD_AXIS_LX] = nub_axis(pad.Lx);
    ctx->st.axes[APAD_AXIS_LY] = nub_axis_inv(pad.Ly);
    /* axes[2..7] stay zero: no right stick, no analog triggers, and S5
     * reserves 6-7 as MUST-be-zero. SceCtrlData has Rx/Ry, but they are only
     * meaningful on hardware this client does not target. */

    ctx->st.battery = 255;                /* 255 = unknown (S5.5) */
    if (ctx->have_battery) {
        int pct = scePowerGetBatteryLifePercent();
        if (pct >= 0 && pct <= 100) {
            ctx->st.battery = (uint8_t)pct;
        }
    }
    ctx->st.client_ticks_ms = apad_ticks_ms();
}

/* ---- the shared status band ---------------------------------------------- */

int app_draw_status(app_ctx *ctx, const char *title, int live)
{
    ui_box b;
    int    i;
    /* Lamps in the console's own vocabulary: the four shapes as glyphs (in
     * the order they sit on the face: top, right, bottom, left), then the
     * named buttons. The wire bits behind them are Nintendo-lettered
     * (sample_input() above); the tester never needs to know that. */
    static const struct { unsigned bit; const char *name; int glyph; } kLamps[] = {
        { APAD_BTN_X, NULL, UI_GLYPH_TRIANGLE }, { APAD_BTN_A, NULL, UI_GLYPH_CIRCLE },
        { APAD_BTN_B, NULL, UI_GLYPH_CROSS },    { APAD_BTN_Y, NULL, UI_GLYPH_SQUARE },
        { APAD_BTN_L, "L", -1 }, { APAD_BTN_R, "R", -1 },
        { APAD_BTN_START, "ST", -1 }, { APAD_BTN_SELECT, "SE", -1 }
    };

    ui_header(title,
              live ? "connected" : "not connected",
              live ? ui_c_good() : ui_c_dim());

    /* round-trip */
    b.x = 6; b.y = 22; b.w = 150; b.h = 70;
    ui_panel(&b, ui_c_panel(), ui_c_border());
    ui_text(b.x + 8, b.y + 6, ui_c_dim(), UI_ALIGN_LEFT, "ROUND TRIP");
    /* rtt_ms is -1 until the first PONG. ui_bignum() draws digits and skips
     * everything else, so "-1" came out as a lone "1" -- a screen confidently
     * reporting 1 ms when it means "not measured yet". Show the same "--" the
     * disconnected case shows until there is a real figure. */
    if (live && ctx->stats.rtt_ms >= 0) {
        /* Clamped, not just sized: an int32 needs 11 bytes and the compiler
         * is right to say so. But a four-digit round trip is already a dead
         * link, and five seven-segment digits do not fit the panel either --
         * so cap the DISPLAY at 9999 rather than widening the buffer and
         * pretending 100000 ms would be a useful thing to render. */
        int  ms = (ctx->stats.rtt_ms > 9999) ? 9999 : (int)ctx->stats.rtt_ms;
        char n[8];
        snprintf(n, sizeof n, "%d", ms);
        ui_bignum(b.x + 10, b.y + 22, 7, ui_c_accent(), n);
        ui_text(b.x + b.w - 8, b.y + b.h - 16, ui_c_dim(), UI_ALIGN_RIGHT, "ms");
    } else if (live) {
        /* Bounded, not free-form: this panel is 150 wide and the string ran
         * into the one beside it. ui_textf_fit() cannot overflow by
         * construction. */
        ui_textf_fit(b.x + 10, b.y + 34, ui_c_dim(), UI_ALIGN_LEFT, b.w - 20,
                     "-- no pong yet");
    } else {
        ui_text(b.x + 10, b.y + 34, ui_c_dim(), UI_ALIGN_LEFT, "--");
    }

    /* server / this device */
    b.x = 164; b.y = 22; b.w = UI_W - 170; b.h = 70;
    ui_panel(&b, ui_c_panel(), ui_c_border());
    ui_text(b.x + 8, b.y + 6, ui_c_dim(), UI_ALIGN_LEFT, "DEVICE");
    ui_textf(b.x + 8, b.y + 20, ui_c_text(), UI_ALIGN_LEFT,
             "AtticPad PSP  %s", APAD_VERSION_STR);
    if (ctx->stats.derive_ms > 0u) {
        /* The pairing key-derivation time. Over ~2500 ms and the paired
         * handshake is racing the server's 3 s idle timer; the colour says
         * so. Diagnostic, but it earns its place: it is the one number that
         * tells a failed pairing apart from a lost packet. */
        uint32_t d = ctx->stats.derive_ms;
        ui_textf(b.x + 8, b.y + 34, (d > 2500u) ? ui_c_warn() : ui_c_dim(),
                 UI_ALIGN_LEFT, "caps 0x%04X   PIN key %ums", (unsigned)ctx->caps, d);
    } else {
        ui_textf(b.x + 8, b.y + 34, ui_c_dim(), UI_ALIGN_LEFT,
                 "caps 0x%04X   battery %s",
                 (unsigned)ctx->caps,
                 (ctx->st.battery == 255) ? "n/a" : "ok");
    }
    if (ctx->net.ready && ctx->net.power_save) {
        /* Actionable, so it stays: the one setting on the console that
         * decides whether a UDP session holds. */
        ui_textf_fit(b.x + 8, b.y + 48, ui_c_warn(), UI_ALIGN_LEFT, b.w - 16,
                     "%s   WLAN power save ON: Settings > Power Save", ctx->net.ip);
    } else if (ctx->net.ready) {
        ui_textf_fit(b.x + 8, b.y + 48, ui_c_dim(), UI_ALIGN_LEFT, b.w - 16,
                     "this psp: %s", ctx->net.ip);
    }

    /* live input readout: proof the pad is being read, on every screen */
    b.x = 6; b.y = 98; b.w = UI_W - 12; b.h = 44;
    ui_panel(&b, ui_c_panel(), ui_c_border());
    ui_text(b.x + 8, b.y + 5, ui_c_dim(), UI_ALIGN_LEFT, "INPUT");
    for (i = 0; i < (int)(sizeof kLamps / sizeof kLamps[0]); i++) {
        ui_box l;
        int on = (ctx->st.buttons & kLamps[i].bit) != 0;
        l.x = b.x + 8 + i * 26; l.y = b.y + 18; l.w = 22; l.h = 18;
        ui_panel(&l, on ? ui_c_accent() : ui_c_bg(), ui_c_border());
        if (kLamps[i].glyph >= 0) {
            ui_glyph_draw(l.x + (l.w - UI_GLYPH_PX) / 2, l.y + (l.h - UI_GLYPH_PX) / 2,
                          (ui_glyph)kLamps[i].glyph, on ? ui_c_bg() : ui_c_dim());
        } else {
            ui_text(l.x + l.w / 2, l.y + 5, on ? ui_c_bg() : ui_c_dim(),
                    UI_ALIGN_CENTER, kLamps[i].name);
        }
    }
    /* The D-pad, which the lamps used to leave out entirely: four arms, each
     * lit while its direction is held. */
    ui_dpad(b.x + 8 + 8 * 26 + 10, b.y + 6, 10,
            (ctx->st.buttons & APAD_BTN_DPAD_MASK) >> APAD_BTN_DPAD_SHIFT,
            ui_c_accent(), ui_c_bg(), ui_c_border());
    {   /* the nub, drawn where it actually is */
        ui_box n; int cx, cy, dx, dy;
        n.x = b.x + b.w - 42; n.y = b.y + 3; n.w = 38; n.h = 38;
        ui_panel(&n, ui_c_bg(), ui_c_border());
        cx = n.x + n.w / 2; cy = n.y + n.h / 2;
        ui_rect(n.x + 2, cy, n.w - 4, 1, ui_c_panel_hi());
        ui_rect(cx, n.y + 2, 1, n.h - 4, ui_c_panel_hi());
        dx = (ctx->st.axes[APAD_AXIS_LX] * 15) / 32767;
        /* screen is +Y down, the wire is +Y up: flip back for the dot only */
        dy = -(ctx->st.axes[APAD_AXIS_LY] * 15) / 32767;
        ui_rect(cx + dx - 2, cy + dy - 2, 5, 5, ui_c_accent());
    }

    /* HOLD makes the pad read all-zero. Without this, "it connected but
     * nothing works" is the bug report, and the switch is the answer. */
    if (ctx->keys_held & PSP_CTRL_HOLD) {
        ui_rect(0, UI_STATUS_BOTTOM - 6, UI_W, 12, ui_c_warn());
        ui_text(UI_W / 2, UI_STATUS_BOTTOM - 4, ui_c_bg(), UI_ALIGN_CENTER,
                "HOLD switch is on -- no input is being sent");
    }
    return UI_STATUS_BOTTOM;
}

/* ---- shell --------------------------------------------------------------- */

static int exit_cb(int a1, int a2, void *arg)
{
    (void)a1; (void)a2; (void)arg;
#ifndef APAD_PSP_STDOUT
    sceKernelExitGame();   /* shipping: HOME exits to the XMB */
#endif
    /* PSPLINK build: do NOT ExitGame -- it would kill the debugger with us.
     * End the run from pspsh (`kill`) instead. */
    return 0;
}

static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_cb, NULL));
    /* The power callback goes on this same thread, as pspsdk's power sample
     * registers both of its callbacks on one CallbackThread. Everything it
     * does is count events; the frame loop below acts on them. */
    apad_psp_power_register();
    sceKernelSleepThreadCB();
    return 0;
}

/*
 * The console settings this client depends on, in one place because they are
 * applied TWICE: once at startup and once more after a resume. All three are
 * idempotent and none costs anything measurable, which is the whole argument
 * for re-applying them blind -- nothing available here could establish
 * whether a real PSP keeps the CPU clock or the analog sampling mode across a
 * suspend, and getting either wrong is a silent failure (a 222 MHz derive
 * that misses the pairing window; a nub stuck at 128 while every button
 * still works).
 */
static void console_settings_apply(void)
{
    /* Full clock, from the first instruction. The PSP boots apps at 222 MHz;
     * the paired handshake derives a key with 10,000 PBKDF2 iterations, and
     * the engine's own note (apad_client.c) is that a derive over 3 s makes
     * the server's idle timer fire and the pairing "dies here, silently,
     * every time." At 222 MHz that derive sat on the 3 s edge -- pairing
     * that worked one time in three -- so 333 MHz is not a nicety here, it
     * is what keeps the handshake inside the window. -lpsppower is already
     * linked for the battery reading. (333, 333, 166) is the SDK power
     * sample's own full-speed triple. */
    scePowerSetClockFrequency(333, 333, 166);

    /* WITHOUT THE SECOND CALL Lx/Ly READ 128 FOREVER, and the digital
     * buttons work regardless -- so the nub looks dead for a reason nothing
     * reports. Both calls are in the SDK's controller sample. */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

static void setup_callbacks(void)
{
    SceUID th = sceKernelCreateThread("apad_cb", cb_thread, 0x11, 0xFA0,
                                      THREAD_ATTR_USER, 0);
    if (th >= 0) {
        sceKernelStartThread(th, 0, 0);
    }
}

/*
 * SUSPEND AND RESUME.
 *
 * A PSP suspend tears the WLAN and the network libraries down underneath a
 * running app. Everything this client holds across one is therefore stale:
 * the engine's socket, the association, and the session the server still
 * thinks is live. Reported from hardware 2026-09-15 as "rejoining wifi
 * doesn't work after a power switch" -- the client had no power callback at
 * all, so it never even learned the console had been away.
 *
 * What happens here, in order, and why that order:
 *
 *   1. The session goes first, while the socket still exists as far as the
 *      shim is concerned. apad_client_destroy() sends its BYE (harmless if
 *      it goes nowhere), closes the shim socket and returns its slot to the
 *      fixed pool -- the pool being fixed is what makes a create/destroy
 *      cycle per resume safe rather than a leak. Doing this AFTER
 *      sceNetInetTerm() would be calling into a library that no longer
 *      exists.
 *   2. The console settings that a suspend may or may not have kept.
 *   3. The network stack, down and up from sceNetInit() onward
 *      (apad_psp_net_restart(), which mirrors pspSdkInetTerm()'s order).
 *   4. Back to the connect screen, which already knows how to wait for a
 *      radio and then re-create the engine -- the same path a fresh boot
 *      takes. Nothing about the reconnect is new code.
 *
 * apad_client_create() mallocs, and this is the one thing in this client
 * that calls it more than once. That is still inside docs/CONVENTIONS.md's "no malloc
 * after init" as apad_client.h reads it (create() IS init): the block is
 * freed by the destroy above before the next create, it is the same size
 * every time, and it happens at most once per suspend -- not per session,
 * not per packet.
 *
 * Returns the screen to be on afterwards.
 */
static apad_screen_id power_apply(app_ctx *ctx, const apad_psp_power_event *ev,
                                  apad_screen_id cur)
{
    apad_devlog("power: event suspended=%d resumed=%d (screen %d)",
                ev->suspended, ev->resumed, (int)cur);

    /* Was a session live when the console went away? Then the reconnect
     * should not wait for a button (app.h). Read before the teardown below
     * clears it. */
    if (ctx->connected) {
        ctx->want_reconnect = 1;
    }

    /* 1. The session and its socket, on either edge: a resume seen without
     *    its suspend (the flags are documented as unreliable) leaves exactly
     *    the same dead socket behind. */
    if (ctx->client != NULL) {
        apad_client_destroy(ctx->client);
        ctx->client = NULL;
        apad_devlog("power: session closed and socket released");
    }
    ctx->connected = 0;
    memset(&ctx->stats, 0, sizeof ctx->stats);
    ctx->have_secret = 0;
    memset(ctx->pin_text, 0, sizeof ctx->pin_text);

    if (ev->resumed) {
        console_settings_apply();
        apad_devlog("power: resume -- cpu %d MHz, analog sampling re-applied",
                    scePowerGetCpuClockFrequency());
        if (apad_psp_net_restart() < 0) {
            apad_devlog("power: net restart refused (bring-up still running)");
        }
        /* Refresh the copy the screens read BEFORE re-entering one, so the
         * connect screen sees ready == 0 and waits for the radio instead of
         * offering to dial out over a stack that is being rebuilt. */
        apad_psp_net_get(&ctx->net);
    }

    /* The self-test needs no network and blocks the loop anyway; let it
     * finish rather than yanking the screen out from under it. The fatal
     * screen is left alone too -- whatever put it there has not changed. */
    if (cur == APAD_SCREEN_SELFTEST || cur == APAD_SCREEN_FATAL) {
        return cur;
    }
    return APAD_SCREEN_CONNECT;
}

int main(void)
{
    static app_ctx ctx;
    apad_screen_id cur = APAD_SCREEN_CONNECT, next;
    apad_psp_power_event pev;

    /* The log is opened BEFORE the callback thread, not after: opening it
     * truncates the file, and the callback thread logs whether the power
     * callback registered. With the old order that line was written and then
     * erased (or raced), which is exactly the line a console owner needs
     * when a resume does nothing. */
    apad_devlog_open("ms0:/atticpad.log");
    apad_devlog("AtticPad PSP %s starting", APAD_VERSION_STR);
    setup_callbacks();

    console_settings_apply();
    apad_devlog("cpu clock set to %d MHz", scePowerGetCpuClockFrequency());

    /* Drop the main thread BELOW the network stack. sceNetInit() creates its
     * callout and interrupt threads at priority 0x20, the same priority the
     * PSP gives an app's main thread -- so at equal priority a CPU-bound
     * stretch on the main loop does not let them preempt, it starves them.
     * That is not hypothetical: the paired handshake's 10,000-iteration
     * PBKDF2 runs on this thread right after the engine QUEUES the WELCOME
     * ACK, and while it spins the stack never gets the CPU to actually put
     * that ACK on the wire. The server retransmits its WELCOME, never sees
     * an ACK, and drops the session as "reliable delivery failed" -- which
     * is exactly what the server logged, and exactly why an UNPAIRED connect
     * (no derive) worked every time while a paired one failed. At 0x30 the
     * stack preempts, the ACK leaves immediately, and the derive runs in the
     * background. Nothing else here is latency-sensitive at 60 Hz. */
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0x30);
    apad_devlog("main thread priority lowered to 0x30 (below the net stack)");

    memset(&ctx, 0, sizeof ctx);
    ctx.keys_armed = 1;
    ctx.selftest_return = APAD_SCREEN_CONNECT;
    strcpy(ctx.port_text, "21100");   /* the documented default; the address
                                       * is deliberately blank (docs/CONVENTIONS.md) */
    /* Prefill from the last session: the address this stick last reached
     * ACTIVE with, and the saved network slot whose radio last came up. An
     * empty address on a fresh stick is exactly right -- see config_psp.h.
     * The returned slot is applied below only if it still exists. */
    ctx.saved_slot = apad_psp_config_load(ctx.ip_text, sizeof ctx.ip_text,
                                          ctx.port_text, sizeof ctx.port_text);
#ifdef APAD_AUTO_HOST
    snprintf(ctx.ip_text, sizeof ctx.ip_text, "%s", APAD_AUTO_HOST);
#endif
#ifdef APAD_AUTO_PORT
    snprintf(ctx.port_text, sizeof ctx.port_text, "%s", APAD_AUTO_PORT);
#endif
    ctx.caps = APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_SHOULDER
             | APAD_CAP_STICK_L;
    /* The battery bit is gated on the pack actually being present: a PSP on
     * AC with the pack out has no level to report, and S5.5 makes a clear
     * capability the way to say so. */
    ctx.have_battery = scePowerIsBatteryExist();
    if (ctx.have_battery) {
        ctx.caps |= APAD_CAP_BATTERY;
    }

    /* Display FIRST, then the network. The first hardware run ended in a
     * black screen with the WLAN light blinking, i.e. the network thread
     * ran and nothing was ever shown. With one frame on screen before any
     * network call, where a boot stops is visible without a log: no frame
     * means display init, a frame that never changes means what follows. */
    if (ui_init() != 0) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("ui_init failed\n");
        for (;;) { sceDisplayWaitVblankStart(); }
    }
    apad_devlog("ui_init ok");
    ui_frame_begin();
    ui_header("AtticPad PSP", "starting", ui_c_dim());
    ui_frame_end();
    apad_devlog("first frame shown");

    /* Modules before the thread, as the net sample does. */
    if (apad_psp_net_modules() == 0) {
        int slots[APAD_PSP_NET_SLOTS];
        int n = apad_psp_net_slots(slots, APAD_PSP_NET_SLOTS);
        int start = (n > 0) ? slots[0] : 1;   /* lowest saved slot by default */
        int i;
        /* The slot that worked last time, if it still exists -- so a console
         * with a working AP in slot 3 does not start every boot failing to
         * join slot 1. Falls back to the lowest slot if the saved one is
         * gone (the network was deleted in Settings). */
        for (i = 0; i < n; i++) {
            if (slots[i] == ctx.saved_slot) { start = ctx.saved_slot; break; }
        }
        apad_devlog("net modules loaded; %d saved connection(s), starting slot %d"
                    " (saved %d)", n, start, ctx.saved_slot);
        (void)apad_psp_net_start(start);
        apad_devlog("net thread started on slot %d", start);
    } else {
        apad_devlog("net modules FAILED");
    }

    /* The engine owns its socket and calls apad_net_init() itself, but the
     * platform's networking must already be up -- which on this console is
     * the thread above, so creation is deferred to the first frame the
     * connect screen finds the radio ready. */

    kScreens[cur]->enter(&ctx);
    apad_devlog("entering the frame loop");
    for (;;) {
        sample_input(&ctx);

#ifdef APAD_PSP_FAKE_RESUME
        /* DEV ONLY. PPSSPP cannot suspend a PSP, so this is the only way the
         * resume path gets RUN rather than merely reviewed: N seconds after
         * boot, raise the same counters the kernel callback raises, once.
         * Never in a shipped build. */
        {
            static int frames;
            static int fired;
            if (!fired && ++frames >= (APAD_PSP_FAKE_RESUME) * 60) {
                fired = 1;
                apad_devlog("DEV: injecting a synthetic suspend + resume"
                            " (APAD_PSP_FAKE_RESUME=%d)", (APAD_PSP_FAKE_RESUME));
                apad_psp_power_inject();
            }
        }
#endif
        /* Every frame, not only when something is expected: the poll also
         * runs the RESUME_COMPLETE fallback timer (power_psp.c). */
        if (apad_psp_power_poll(&pev)) {
            apad_screen_id forced = power_apply(&ctx, &pev, cur);

            if (forced != cur) {
                cur = forced;
            }
            if (cur == APAD_SCREEN_CONNECT) {
                /* Re-enter even when already there: connect_enter() is what
                 * decides between "edit the address" and "wait for the
                 * radio", and after a resume the answer has changed. */
                ctx.keys_armed = 0;
                ctx.keys_prev  = 0xFFFFFFFFu;
                kScreens[cur]->enter(&ctx);
                /* After enter(), so it is not overwritten by its own
                 * rejoin note. The existing banner vocabulary, one line,
                 * no instructions. */
                if (pev.resumed) {
                    app_note(&ctx, 1, "resumed -- rejoining");
                } else {
                    /* Suspend seen but the resume has not been signalled
                     * yet: the session is gone, the radio is not (yet).
                     * Saying "rejoining" here would be a claim about work
                     * that has not started. */
                    app_note(&ctx, 1, "suspended -- session closed");
                }
            }
        }

        next = kScreens[cur]->update(&ctx);

        ui_frame_begin();
        kScreens[cur]->draw(&ctx);
        ui_frame_end();

#ifdef APAD_PSP_SHOTS
        /* DEV ONLY. Dump each screen the app actually REACHES, once, a few
         * frames after arriving so nothing is caught half-drawn.
         *
         * It used to force transitions on a timer, which raced the radio:
         * the tour moved off the connect screen before the network came up,
         * so the session was never reached and the shot never existed. A
         * passive observer cannot perturb what it is trying to photograph. */
        {
            static const char *seen[APAD_SCREEN_COUNT];
            static int         age;
            static const char *last;
            const char *name = kScreens[cur]->name;
            int i, known = 0;

            if (name != last) { last = name; age = 0; }
            age++;
            for (i = 0; i < APAD_SCREEN_COUNT; i++) {
                if (seen[i] == name) { known = 1; break; }
            }
            if (!known && age == 40) {
                char path[64];
                snprintf(path, sizeof path, "ms0:/shot-%s.ppm", name);
                ui_dump_ppm(path);
                for (i = 0; i < APAD_SCREEN_COUNT; i++) {
                    if (seen[i] == NULL) { seen[i] = name; break; }
                }
            }
        }
#endif

        if (next != cur) {
            cur = next;
            /* Clear the gate on every transition: see app.h. */
            ctx.keys_armed = 0;
            ctx.keys_prev  = 0xFFFFFFFFu;
            kScreens[cur]->enter(&ctx);
        }
    }
    return 0;
}
