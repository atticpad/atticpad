/* server/host/windows/main.c -- the Windows host for libapadserver
 * (docs/DESIGN.md §6.4).
 *
 * The Windows twin of server/host/linux/main.c: same job (own the process,
 * the socket, the recv loop, the profile files, the log stream), same
 * shape and same ordering of concerns. Read the Linux file first -- the
 * comments here mostly explain WHERE and WHY this file diverges, not what
 * carried over unchanged. The two callbacks (host_send/host_log) and the
 * main loop's shape (recv, tick, dispatch) are intentionally identical:
 * this binary is also the second half of the M4 regression check, proving
 * that a completely different OS host produces the same wire behaviour
 * through the same library.
 *
 * The one thing this file knows about backends is which one to pass in
 * (apad_backend_vigem). Nothing else here -- and nothing in the library --
 * knows ViGEmBus exists (docs/DESIGN.md §6.1).
 *
 * Cross-compiled from Linux with x86_64-w64-mingw32-gcc; the Windows test
 * machine has no C toolchain at all (server/backends/vendor/README.md).
 * Linked -static together with libapadserver's TUs, server/backends/vigem.c,
 * the vendored ViGEmClient object, shim/net_winsock.c and shim/time_win32.c.
 * See this file's companion report for the exact link command.
 */

/* GetTickCount64 (via shim/time_win32.c, linked separately) and every
 * Winsock/iphlpapi declaration used below need a version floor. 0x0601 =
 * Windows 7, matching shim/net_winsock.c and shim/time_win32.c exactly so
 * all four TUs agree on one target. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

/* winsock2.h MUST precede windows.h, exactly as shim/net_winsock.c requires
 * and for the same reason: windows.h otherwise drags in the original
 * winsock.h and the two collide with redefinition errors. iphlpapi.h (for
 * GetAdaptersAddresses, used only to print this host's own IP -- see
 * print_own_addresses() below) depends on the winsock2/ws2tcpip types, so
 * it goes in the same block, still ahead of windows.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
/* shellapi.h: ShellExecuteW (open the browser) and Shell_NotifyIconW/
 * NOTIFYICONDATAW (the tray icon) -- see the "system tray" section below.
 * windows.h with WIN32_LEAN_AND_MEAN does NOT pull this in on its own. */
#include <shellapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>   /* wcsncpy, for NOTIFYICONDATAW::szTip below */

#include "atticpad/atticpad.h"
#include "atticpad/version.h"   /* APAD_VERSION_STR for the startup banner */
#include "apadserver.h"
/* The SAME server UI the Linux host serves -- server/host/common/, not a
 * second Windows-only one. webui.h pulls in assets.h, qr.h, ipaddr.h,
 * ui_mdns_status.h and sockcompat.h itself; see its top comment for why one
 * UI and not two, and sockcompat.h's for the short list of calls that are
 * genuinely not the same on Winsock. All header-only, because
 * scripts/build.sh names an exact list of .c files per target. */
#include "../common/webui.h"

/* Loopback-only port the UI listens on. Same default and same
 * ATTICPAD_UI_PORT override as the Linux host, so one scripted deployment
 * can set one value for both. NOT the protocol port. */
#define ATTICPAD_UI_DEFAULT_PORT 21150

#include "backends.h"   /* server/backends/backends.h -- registry of
                          * backends compiled in for this platform. FIXED
                          * (backend registry task, 2026-08-10): this file
                          * used to redeclare `extern const apad_backend
                          * apad_backend_vigem;` itself, worked around
                          * exactly like this because backend.h only ever
                          * forward-declared apad_backend_uinput. backends.h
                          * now supplies the _WIN32-guarded extern for
                          * apad_backend_vigem; nothing here redeclares it. */

#include "resource.h"   /* IDI_APPICON -- server/host/windows/atticpad.rc,
                          * shared between here and the .rc so the icon ID
                          * lives in exactly one place. Used below to load
                          * the tray icon; the .exe's own shell icon (lowest-
                          * numbered ICON resource, per atticpad.rc) is
                          * already this same resource, no separate asset. */

/* Same reasoning and same value as server/host/linux/main.c's
 * RECV_TIMEOUT_MS: bounds how late apad_server_tick() can discover a due
 * §9 retransmit or §8 idle timeout. 20 ms keeps compounded drift across the
 * four-stage 100/200/400/800 ms retransmit schedule comfortably inside the
 * 2300 ms retransmit-failure deadline. select()'s timeout (net_winsock.c)
 * does not busy-loop, same as Linux's. */
#define RECV_TIMEOUT_MS   20

/* Same bound and same reasoning as the Linux host: how many *.jsonc files
 * this host will pick up from the profiles directory, independent of the
 * library's own APAD_PROFILES_MAX -- server/host/common/profile_store.h's
 * PROFILE_STORE_MAX_FILES, pulled in transitively via "../common/webui.h"
 * above. */

static apad_sock *g_sock;

/* Set from SetConsoleCtrlHandler's callback, which runs on a thread Windows
 * creates for it -- NOT the main thread -- so this needs an interlocked
 * write, unlike the Linux host's plain sig_atomic_t (a POSIX signal handler
 * runs on the same thread it interrupts). The main loop's read is a plain
 * load: it is polled every RECV_TIMEOUT_MS regardless, so a torn read here
 * costs at most one extra 20 ms lap, not correctness. */
static volatile LONG g_should_exit;

/* ---- the two callbacks that make the library sans-IO --------------------*/

/* Identical contract and identical body to the Linux host's host_send:
 * apad_udp_send()'s result passes straight through, unchanged, negative on
 * failure. See apadserver.h's apad_server_send_fn doc for why the library
 * must still see this as "sent" for retransmit accounting even when it
 * returns < 0 -- that is a library invariant, not something either host
 * implements differently. */
static int host_send(void *user, const apad_addr *to, const uint8_t *buf,
                     size_t len)
{
    (void)user;
    return apad_udp_send(g_sock, to, buf, len);
}

/* Identical to the Linux host's host_log: every level goes to stderr with
 * the "[atticpad] " prefix, unfiltered, for the same reason (a level filter
 * here would hide the profile-load and teardown lines that make a live
 * session debuggable).
 *
 * NOTE, corrected 2026-08-10: the claim that used to sit here ("stderr on
 * Windows is unbuffered by default same as POSIX") is not something the
 * documented CRT behaviour actually promises, and a captured session over
 * SSH with stdout+stderr piped came back with ZERO bytes despite the server
 * demonstrably running -- consistent with stderr having been fully buffered
 * for that run and the process dying (or the pipe closing) before a flush.
 * main() now calls setvbuf(stderr, NULL, _IONBF, 0) (and the same for
 * stdout, in case anything ever prints there) before any output happens, so
 * this stream is unbuffered by explicit request rather than by an assumed
 * default. See main()'s own comment for why _IOLBF was not used: on Win32,
 * MSVCRT's setvbuf() documents _IOLBF as behaving identically to _IOFBF
 * (full buffering) -- there is no line-buffered mode on this platform, only
 * _IONBF (unbuffered) actually changes anything. */
static void host_log(void *user, apad_log_level level, const char *msg)
{
    static const char *const level_str[] = { "INFO", "WARN", "ERROR" };
    const char *lvl = (level <= APAD_LOG_ERROR) ? level_str[level] : "?";
    char line[UI_LOG_LINE_MAX];

    (void)user;
    (void)fprintf(stderr, "[atticpad] %s\n", msg);

    /* Also into the UI's ring buffer, same as the Linux host -- this is
     * what /api/state's log feed renders, and apad_server_log_fn's own doc
     * comment anticipated it ("a tray UI may surface only WARN and above").
     * Harmless under --headless: the ring is written and simply never read.
     *
     * Nothing secret reaches here: server/src/pairing.c stopped logging the
     * §10 secret precisely because this ring PERSISTS it beyond the 120 s
     * window the secret is valid for. Keep it that way. */
    (void)snprintf(line, sizeof line, "[%s] %s", lvl, msg);
    ui_log_push(line);
}

/* Rule 3 for pairing (apadserver.h): entropy is platform I/O, so
 * libapadserver never reaches for it and this host supplies it.
 * BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG is the modern CNG
 * call -- no algorithm provider to open and close, present since Vista, and
 * the documented replacement for the deprecated RtlGenRandom/CryptGenRandom
 * pair. Links -lbcrypt (scripts/windows.env).
 *
 * Returns 1 only on a complete fill: BCryptGenRandom either fills the whole
 * buffer or returns a failure NTSTATUS, so there is no short-read loop here
 * the way there is around getrandom() on Linux. A failure means no pairing
 * window opens -- libapadserver will not substitute a weaker source, and
 * neither will this.
 *
 * NOT VERIFIED ON HARDWARE: cross-compiled and linked only. See this file's
 * header for what that means. */
static int host_random(void *user, uint8_t *buf, size_t len)
{
    NTSTATUS st;

    (void)user;
    if (len > (size_t)0xFFFFFFFFu) {
        return 0;
    }
    st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? 1 : 0;
}

/* ---- tier 3: "the server MUST display its own IP prominently" -----------*/

/*
 * docs/PROTOCOL.md §7, tier 3: manual IP entry is the only discovery path
 * that always works, so the server MUST display its own IP prominently at
 * all times. This is NOT protocol I/O -- it never touches an apad_sock, and
 * neither shim (net_bsd.c nor net_winsock.c) exposes a "what is my own
 * address" call on any platform (the DS reference this shim mirrors has no
 * such concept either), so it does not belong behind apad_udp_*. It talks
 * to iphlpapi directly, the same way this file talks to SetConsoleCtrlHandler
 * directly below: both are host-owned OS surface that has nothing to do with
 * the UDP datapath the shim abstracts.
 *
 * GetAdaptersAddresses, not a "connect a UDP socket to 8.8.8.8 and read
 * getsockname()" trick: this machine may have more than one active NIC
 * (Ethernet + Wi-Fi is common), and tier 3 exists precisely so a human can
 * pick the right one rather than the server guessing. Every up,
 * non-loopback, IPv4 unicast address is printed.
 */
/*
 * §7 tier 3: "The server MUST display its own IP prominently at all times."
 *
 * The GetAdaptersAddresses() loop that used to live here -- private to this
 * file, and a near-twin of the getifaddrs() one in the Linux host -- moved
 * to server/host/common/ipaddr.h when the UI became shared, so both hosts
 * now enumerate and classify addresses through ONE function with two
 * platform bodies. That matters beyond tidiness: the UI's /api/state and
 * the §10.3 QR both pick which address to encode from this list, and two
 * independently-written enumerators would have been free to disagree about
 * what counts as a real address.
 *
 * This banner is the console equivalent of the UI's live panel, printed
 * once at startup -- the UI satisfies "at all times" by recomputing on
 * every poll.
 */
static void print_own_addresses(FILE *out)
{
    host_own_addr addrs[HOST_MAX_OWN_ADDRS];
    size_t naddr = host_enumerate_own_ipv4(addrs, HOST_MAX_OWN_ADDRS);
    size_t i;

    if (naddr == 0u) {
        (void)fprintf(out,
                      "[atticpad]   (no non-loopback IPv4 address found on "
                      "this machine)\n");
        return;
    }
    for (i = 0; i < naddr; i++) {
        (void)fprintf(out, "[atticpad]   %-15s  %s (%s)\n",
                      addrs[i].ip, addrs[i].iface,
                      host_addr_kind_name(addrs[i].kind));
    }
}

/* ---- Ctrl+C / Ctrl+Break / console close ---------------------------------*/

/*
 * signal(SIGINT/SIGTERM), what the Linux host uses, is thin on Windows: the
 * CRT emulates SIGINT for Ctrl+C alone and nothing catches a console window
 * closing. SetConsoleCtrlHandler is the real mechanism and catches all of
 * CTRL_C_EVENT, CTRL_BREAK_EVENT, CTRL_CLOSE_EVENT (the console window's X
 * button / a taskkill), CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT.
 *
 * This callback runs on a thread Windows creates specifically for it, not on
 * the thread running main()'s loop, and -- this is the part that makes
 * "prompt" a hard requirement rather than a nicety -- CTRL_CLOSE_EVENT,
 * CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT all carry an OS-enforced
 * timeout (on the order of a few seconds) between this handler returning and
 * Windows forcibly terminating the process, whether or not shutdown
 * actually finished. So this handler does the absolute minimum -- set a flag
 * and return TRUE immediately -- and all the real work (tearing down
 * sessions, which tears down every ViGEmBus pad, then closing the socket)
 * happens on the main thread, which notices the flag within one
 * RECV_TIMEOUT_MS lap. Doing the teardown work IN this handler would risk
 * losing the race against that timeout instead of racing it on purpose.
 */
static BOOL WINAPI console_handler(DWORD ctrl_type)
{
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        (void)InterlockedExchange(&g_should_exit, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ---- browser + system tray (this task: "present as an app, not a console
 * window", docs/DESIGN.md §6.3/M4 phase 8's "tray UI") --------------------------- */

/*
 * THE THREADING CONSTRAINT (read before touching anything below). A tray
 * icon needs a window to receive Shell_NotifyIcon's callbacks, and a window
 * needs a message pump -- but GetMessage() BLOCKS, and the UDP recv loop in
 * main() IS the product; its latency is the whole point of this project.
 *
 * So this does NOT get its own thread, and nothing here calls GetMessage().
 * tray_pump() (below) uses PeekMessageW(..., PM_REMOVE) instead -- never
 * blocks, drains whatever is queued and returns immediately -- called from
 * inside main()'s own loop, right next to the existing webui_poll() call,
 * on the exact same thread every apad_server_* call already runs on. That
 * loop already wakes at least once per RECV_TIMEOUT_MS (apad_udp_recv's own
 * timeout), which is exactly the guarantee webui_poll() already relies on
 * for the same reason: apadserver.h's THREADING note ("an apad_server is
 * not internally synchronised") means every apad_server_* call --
 * including the ones the tray menu makes (begin_pairing/cancel_pairing/
 * pairing_state/list_clients/backend_status) -- must stay on that one
 * thread. No lock is added anywhere because none is needed: there is still
 * only one thread touching the library.
 *
 * The window itself is message-only (HWND_MESSAGE): never shown, no client
 * area, no taskbar entry, exists purely so Shell_NotifyIcon has an HWND to
 * post WM_TRAYICON to.
 */

#define WM_TRAYICON        (WM_APP + 1)
#define TRAY_WNDCLASS_NAME L"AtticPadTrayWndClass"
#define TRAY_UID           1u

enum {
    IDM_TRAY_OPEN = 1,
    IDM_TRAY_PAIR_PIN,
    IDM_TRAY_PAIR_TOKEN,
    IDM_TRAY_CANCEL_PAIR,
    IDM_TRAY_QUIT
};

typedef struct {
    HWND            hwnd;
    NOTIFYICONDATAW nid;
    int             added;        /* NIM_ADD succeeded -- gates NIM_DELETE
                                    * on shutdown and gates whether the
                                    * console gets hidden (see
                                    * hide_console_if_owned's call site: a
                                    * hidden console with no tray icon up
                                    * would leave no visible surface at
                                    * all) */
    apad_server    *server;       /* for the pairing/roster menu items and
                                    * the tooltip; set once in tray_init(),
                                    * never NULL after a successful call */
    uint16_t        ui_port;      /* for the tray's own "Open AtticPad" */
    uint32_t        tooltip_ms;   /* apad_ticks_ms() of the last NIM_MODIFY;
                                    * throttles the tooltip refresh to about
                                    * once a second rather than once a
                                    * RECV_TIMEOUT_MS (20 ms) lap */
} tray_state;

/* File-scope, not threaded through lParam/CREATESTRUCT: WndProc is a plain
 * callback and this process has exactly one tray icon for its whole
 * lifetime, so a global is simpler than plumbing a context pointer through
 * CreateWindowExW's lpParam and WM_NCCREATE -- same pattern as this file's
 * existing g_sock/g_should_exit. */
static tray_state g_tray;

/* Every byte of `src` must already be ASCII (true of everything this file
 * feeds it: a decimal port number wrapped in a fixed literal URL, and
 * apad_server_* diagnostic text that is itself ASCII-only per this file's
 * own convention) -- a direct widen, not a real codepage conversion, and
 * deliberately not MultiByteToWideChar for that reason: this avoids ever
 * depending on which ANSI codepage is active. */
static void ascii_to_wide(const char *src, wchar_t *dst, size_t dst_cap)
{
    size_t i;
    for (i = 0; i + 1 < dst_cap && src[i] != '\0'; i++) {
        dst[i] = (wchar_t)(unsigned char)src[i];
    }
    if (dst_cap > 0) {
        dst[i] = L'\0';
    }
}

/*
 * Opens "http://127.0.0.1:<ui_port>/" in the shell's default browser. Used
 * both for the startup auto-open (item 1) and the tray's own "Open
 * AtticPad" (item 2) -- one function, so both paths log identically.
 *
 * Logs the ShellExecuteW result either way. Per this task's brief: an SSH
 * session has no interactive desktop, so this call may "fail" in a way that
 * has nothing to do with a bug -- the log line says which of "genuinely
 * failed" or "worked, but nothing here can show it" it is, since only a
 * human at the real console can tell those apart from outside.
 * ShellExecuteW returns a value > 32 on success; anything <= 32 is one of
 * the documented SE_ERR_* codes (MSDN's ShellExecute return-value table).
 */
static void open_browser(uint16_t ui_port)
{
    char narrow[64];
    wchar_t url[64];
    HINSTANCE r;

    (void)snprintf(narrow, sizeof narrow, "http://127.0.0.1:%u/",
                   (unsigned)ui_port);
    narrow[sizeof narrow - 1] = '\0';
    ascii_to_wide(narrow, url, sizeof url / sizeof url[0]);

    r = ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) {
        (void)fprintf(stderr,
                      "[atticpad] browser: opened %s (ShellExecuteW ok)\n",
                      narrow);
    } else {
        (void)fprintf(stderr,
                      "[atticpad] browser: ShellExecuteW(%s) returned code "
                      "%ld -- either it genuinely failed, or this session "
                      "has no interactive desktop to show it on (e.g. some "
                      "SSH sessions, or a service) -- open that URL by hand "
                      "if you have a desktop\n", narrow, (long)(INT_PTR)r);
    }
}

/*
 * WM_TRAYICON's lParam carries the mouse/keyboard message that triggered
 * it -- the classic (pre-NOTIFYICON_VERSION_4) Shell_NotifyIcon contract,
 * which is what this file uses (no NIM_SETVERSION call below): wParam is
 * the icon's uID, lParam is the raw window message (WM_LBUTTONUP,
 * WM_RBUTTONUP, ...). Kept deliberately simple -- version 4's packed
 * x/y-in-wParam form buys nothing this menu needs.
 */
static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam)
{
    switch (msg) {
    case WM_TRAYICON:
        switch ((UINT)lparam) {
        case WM_LBUTTONUP:
            /* The primary action (this task's brief: "left click or double
             * click: open the UI ... must be the most obvious one"). A
             * double click delivers a WM_LBUTTONUP for EACH of its two
             * clicks before the trailing WM_LBUTTONDBLCLK, so handling
             * WM_LBUTTONUP alone already covers both a single and a double
             * click -- WM_LBUTTONDBLCLK is explicitly ignored below so a
             * double click does not open two browser tabs. */
            open_browser(g_tray.ui_port);
            break;
        case WM_LBUTTONDBLCLK:
            break;   /* already handled via the two WM_LBUTTONUPs above */
        case WM_RBUTTONUP: {
            HMENU menu = CreatePopupMenu();
            apad_pairing_info info;
            POINT pt;

            if (menu == NULL) {
                break;
            }
            /* Read state with apad_server_pairing_state() (apadserver.h),
             * exactly as this task's brief asks, to grey out "Cancel
             * pairing" when no window is open. */
            memset(&info, 0, sizeof info);
            (void)apad_server_pairing_state(g_tray.server, &info);

            (void)AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN,
                              L"Open AtticPad");
            (void)AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            (void)AppendMenuW(menu, MF_STRING, IDM_TRAY_PAIR_PIN,
                              L"Pair with a PIN");
            (void)AppendMenuW(menu, MF_STRING, IDM_TRAY_PAIR_TOKEN,
                              L"Pair with a QR token");
            (void)AppendMenuW(menu,
                              MF_STRING | (info.open ? 0u
                                          : (UINT)(MF_GRAYED | MF_DISABLED)),
                              IDM_TRAY_CANCEL_PAIR, L"Cancel pairing");
            (void)AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            (void)AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"Quit");

            /* SetForegroundWindow before TrackPopupMenu, then post a
             * harmless WM_NULL right after it returns: the documented
             * Shell_NotifyIcon idiom (MSDN's tray-icon sample) for making
             * the popup dismiss correctly on an outside click, not
             * optional polish -- without it the menu can get stuck open
             * behind other windows once this message-only window loses
             * whatever "foreground" means for a window with no visible
             * surface. */
            (void)SetForegroundWindow(hwnd);
            (void)GetCursorPos(&pt);
            (void)TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                                 pt.x, pt.y, 0, hwnd, NULL);
            (void)PostMessageW(hwnd, WM_NULL, 0, 0);
            (void)DestroyMenu(menu);
            break;
        }
        default:
            break;
        }
        return 0;
    case WM_COMMAND: {
        /* Same clock every other apad_server_* caller in this file uses;
         * cheap and side-effect free (shim/time_win32.c). */
        uint32_t now = apad_ticks_ms();
        switch (LOWORD(wparam)) {
        case IDM_TRAY_OPEN:
            open_browser(g_tray.ui_port);
            break;
        case IDM_TRAY_PAIR_PIN:
            /* use_token = 0: §10's 6-digit PIN (apadserver.h,
             * apad_pairing_kind). Failure (no on_random, or it failed) is
             * already reported through host_log -- nothing further to do
             * here. */
            (void)apad_server_begin_pairing(g_tray.server, now, 0);
            break;
        case IDM_TRAY_PAIR_TOKEN:
            /* use_token = 1: the longer QR-shaped token. */
            (void)apad_server_begin_pairing(g_tray.server, now, 1);
            break;
        case IDM_TRAY_CANCEL_PAIR:
            apad_server_cancel_pairing(g_tray.server, now);
            break;
        case IDM_TRAY_QUIT:
            /* Same flag and same interlocked write console_handler() uses
             * above -- set from here (still the main thread, via
             * DispatchMessageW inside tray_pump()) rather than a new one,
             * so there remains exactly one exit signal the main loop
             * checks. */
            (void)InterlockedExchange(&g_should_exit, 1);
            break;
        default:
            break;
        }
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

/*
 * Registers the tray window class, creates the message-only window, and
 * adds the icon (NIM_ADD). Returns 1 on success, 0 otherwise -- every
 * failure is logged with its GetLastError() code, per this task's brief,
 * since the difference between "this is broken" and "this SSH session has
 * no desktop to host a tray icon on" is otherwise invisible from outside.
 * Never fatal either way: the server is fully functional without a tray
 * icon, exactly as it is with --headless.
 */
static int tray_init(apad_server *server, uint16_t ui_port)
{
    WNDCLASSEXW wc;
    HINSTANCE hinst = GetModuleHandleW(NULL);

    memset(&g_tray, 0, sizeof g_tray);
    g_tray.server  = server;
    g_tray.ui_port = ui_port;

    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = tray_wndproc;
    wc.hInstance     = hinst;
    wc.lpszClassName = TRAY_WNDCLASS_NAME;
    if (RegisterClassExW(&wc) == 0) {
        (void)fprintf(stderr,
                      "[atticpad] tray: RegisterClassExW failed (error "
                      "%lu) -- no tray icon this run\n",
                      (unsigned long)GetLastError());
        return 0;
    }

    g_tray.hwnd = CreateWindowExW(0, TRAY_WNDCLASS_NAME, NULL, 0,
                                  0, 0, 0, 0, HWND_MESSAGE, NULL, hinst,
                                  NULL);
    if (g_tray.hwnd == NULL) {
        (void)fprintf(stderr,
                      "[atticpad] tray: CreateWindowExW failed (error %lu) "
                      "-- no tray icon this run (a session with no "
                      "interactive desktop -- some SSH sessions, or "
                      "running as a service -- cannot host one)\n",
                      (unsigned long)GetLastError());
        (void)UnregisterClassW(TRAY_WNDCLASS_NAME, hinst);
        return 0;
    }

    memset(&g_tray.nid, 0, sizeof g_tray.nid);
    g_tray.nid.cbSize           = sizeof g_tray.nid;
    g_tray.nid.hWnd             = g_tray.hwnd;
    g_tray.nid.uID              = TRAY_UID;
    g_tray.nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.nid.uCallbackMessage = WM_TRAYICON;
    /* Ask Windows for the tray's own icon size and let it pick the matching
     * render out of the .ico's nine (16..256) -- per this task's brief, NOT
     * loading 256 and letting it scale down. */
    g_tray.nid.hIcon = LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APPICON),
                                  IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON),
                                  LR_DEFAULTCOLOR);
    (void)wcsncpy(g_tray.nid.szTip, L"AtticPad server",
                 (sizeof g_tray.nid.szTip / sizeof g_tray.nid.szTip[0]) - 1);

    if (!Shell_NotifyIconW(NIM_ADD, &g_tray.nid)) {
        (void)fprintf(stderr,
                      "[atticpad] tray: Shell_NotifyIcon(NIM_ADD) failed "
                      "(error %lu) -- no tray icon this run (no desktop, "
                      "or Explorer is not running to host one)\n",
                      (unsigned long)GetLastError());
        (void)DestroyWindow(g_tray.hwnd);
        g_tray.hwnd = NULL;
        (void)UnregisterClassW(TRAY_WNDCLASS_NAME, hinst);
        return 0;
    }

    g_tray.added = 1;
    (void)fprintf(stderr, "[atticpad] tray: icon added\n");
    return 1;
}

/*
 * Tooltip content (this task's brief: "something live and useful -- client
 * count, backend health, whether a pairing window is open"). Throttled to
 * roughly once a second: NIM_MODIFY is a cheap IPC call to Explorer, but
 * this function is called from a loop iteration that runs every
 * RECV_TIMEOUT_MS (20 ms), and the tooltip cannot usefully change that
 * often. The §10 secret NEVER appears here -- only apad_pairing_info::open,
 * never ::secret (see server-dev agent memory secrets-through-query-api:
 * the same rule that keeps the secret out of on_log applies to the tray).
 */
static void tray_update_tooltip(uint32_t now_ms)
{
    apad_backend_status backend;
    apad_pairing_info pairing;
    int nclients;
    char narrow[128];
    wchar_t tip[128];

    if (!g_tray.added) {
        return;
    }
    if (g_tray.tooltip_ms != 0u && (now_ms - g_tray.tooltip_ms) < 1000u) {
        return;
    }
    g_tray.tooltip_ms = now_ms;

    nclients = apad_server_list_clients(g_tray.server, NULL, 0);
    memset(&backend, 0, sizeof backend);
    (void)apad_server_backend_status(g_tray.server, &backend);
    memset(&pairing, 0, sizeof pairing);
    (void)apad_server_pairing_state(g_tray.server, &pairing);

    (void)snprintf(narrow, sizeof narrow, "AtticPad - %d client%s - %s%s",
                   nclients, nclients == 1 ? "" : "s",
                   backend.ok ? "backend OK" : "backend needs attention",
                   pairing.open ? " - pairing window open" : "");
    narrow[sizeof narrow - 1] = '\0';
    ascii_to_wide(narrow, tip, sizeof tip / sizeof tip[0]);

    (void)wcsncpy(g_tray.nid.szTip, tip,
                 (sizeof g_tray.nid.szTip / sizeof g_tray.nid.szTip[0]) - 1);
    g_tray.nid.szTip[(sizeof g_tray.nid.szTip
                     / sizeof g_tray.nid.szTip[0]) - 1] = L'\0';
    g_tray.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    (void)Shell_NotifyIconW(NIM_MODIFY, &g_tray.nid);
}

/*
 * Drains whatever is queued for this thread's windows -- just the tray's
 * message-only window -- and returns immediately. NEVER calls GetMessage();
 * see this section's header comment for why that distinction is the entire
 * point. Called once per main-loop iteration, right next to webui_poll().
 */
static void tray_pump(void)
{
    MSG msg;

    if (g_tray.hwnd == NULL) {
        return;
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        (void)TranslateMessage(&msg);
        (void)DispatchMessageW(&msg);
    }
}

/*
 * NIM_DELETE, then tear down the window and class. Idempotent (safe to call
 * even when tray_init() never succeeded, or was never called) -- called
 * from every exit path in main(), because a tray icon left behind after the
 * process dies is the classic failure mode here.
 */
static void tray_shutdown(void)
{
    HINSTANCE hinst = GetModuleHandleW(NULL);

    if (g_tray.added) {
        (void)Shell_NotifyIconW(NIM_DELETE, &g_tray.nid);
        g_tray.added = 0;
    }
    if (g_tray.hwnd != NULL) {
        (void)DestroyWindow(g_tray.hwnd);
        g_tray.hwnd = NULL;
    }
    (void)UnregisterClassW(TRAY_WNDCLASS_NAME, hinst);
}

/*
 * Hides the console window, but ONLY when this process is its sole owner --
 * i.e. it was double-clicked (or launched with no console of its own and
 * Windows allocated a fresh one) rather than started from an existing
 * cmd.exe/PowerShell/SSH session, where GetConsoleProcessList() returns
 * more than this one pid because the launching shell (and, over SSH,
 * sshd's own console client) shares the same console. This is EXACTLY the
 * distinction this task's brief asks for: hiding it unconditionally would
 * break the SSH-redirected verification path this binary is checked with.
 *
 * Called only after the tray icon is confirmed up (see the call site in
 * main()) -- hiding the console with no tray icon and no desktop surface
 * would leave a running process with nothing visible at all.
 */
static void hide_console_if_owned(void)
{
    DWORD pids[2];
    DWORD n = GetConsoleProcessList(pids, 2);

    if (n == 1) {
        HWND console = GetConsoleWindow();
        if (console != NULL) {
            (void)fprintf(stderr,
                          "[atticpad] console: hiding it now (this process "
                          "is its sole owner) -- use the tray icon from "
                          "here on, or relaunch with --console to keep "
                          "this window\n");
            (void)ShowWindow(console, SW_HIDE);
        }
    }
}

/* ---- profile files: the host half of the profiles seam -------------------
 *
 * load_profile_files()/free_profile_files() and their has_jsonc_suffix/
 * cmp_str/xstrdup/read_whole_file helpers used to live here, hand-written
 * against FindFirstFileA/FindNextFileA -- a near-twin of the Linux host's
 * own opendir/readdir copy. Both now live in one place,
 * server/host/common/profile_store.h (profile_store_scan()/
 * profile_store_free_files()), pulled in transitively via
 * "../common/webui.h" above; see that file's top comment for the
 * consolidation this task's remapping-editor work did (it found this exact
 * duplication) and for why the platform fork is kept as two complete
 * bodies rather than interleaved.
 */

/*
 * Directory the executable itself lives in, NUL-terminated, no trailing
 * separator. Used as the base for the default profiles directory instead of
 * the current working directory: docs/DESIGN.md §6.3 wants this to run as a tray
 * app or (eventually) a service, and neither is launched from a shell
 * sitting in the right folder the way every build/test invocation in this
 * tree runs from the repo root on Linux. `out` must be at least MAX_PATH
 * bytes. Falls back to "." if the module path cannot be read at all -- a
 * degraded default, not a fatal error, matching how a missing profiles
 * directory is handled everywhere else in this file.
 */
static void exe_dir(char *out, size_t out_cap)
{
    char path[MAX_PATH];
    DWORD n;
    char *slash;

    if (out_cap == 0) {
        return;
    }
    n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path) {
        (void)snprintf(out, out_cap, ".");
        out[out_cap - 1] = '\0';
        return;
    }
    slash = strrchr(path, '\\');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        (void)snprintf(path, sizeof path, ".");
    }
    (void)snprintf(out, out_cap, "%s", path);
    out[out_cap - 1] = '\0';
}

int main(int argc, char **argv)
{
    uint16_t port = (uint16_t)APAD_DEFAULT_PORT;
    uint16_t ui_port = (uint16_t)ATTICPAD_UI_DEFAULT_PORT;
    /* Must match what the library actually uses: cfg.server_name is left
     * NULL below, which asks libapadserver for its own default, and this is
     * that default spelled out so the UI reports the name a client really
     * sees. The Linux host keeps the identical constant for the identical
     * reason. NOT NULL -- the UI renders this into JSON. */
    static const char server_name_used[] = "AtticPad Server";
    int headless = 0;
    int no_browser = 0;    /* --no-browser: skip the startup ShellExecuteW,
                             * tray icon (if any) still opens it on request */
    int force_console = 0; /* --console: never hide the console window,
                             * even when this process is its sole owner */
    apad_socket_t ui_fd = APAD_INVALID_SOCKET;
    ui_mdns_status ui_mdns;
    apad_server_cfg cfg;
    apad_server *server;
    profile_store_file files[PROFILE_STORE_MAX_FILES];
    apad_profile_source sources[PROFILE_STORE_MAX_FILES];
    size_t file_count, i;
    char base_dir[MAX_PATH];
    char default_profiles_dir[MAX_PATH + 16];
    /* Resolved once below, kept alive for the whole process: webui.h's
     * PUT/DELETE /api/profile/{name} and POST /api/profiles/reload routes
     * re-scan this SAME directory on every write, long after the startup
     * block that first computed it (same reasoning as the Linux host's own
     * profiles_dir hoist). */
    const char *profiles_dir;

    /* Force both streams unbuffered before the FIRST byte of output, full
     * stop -- this must be the first thing main() does. Without this, a
     * redirected/piped stdout or stderr (the normal case when reaching this
     * machine over SSH, which is the whole point of this diagnostic) is
     * fully buffered by the CRT, and a process that exits (or is killed)
     * before its buffer fills or is explicitly flushed prints NOTHING --
     * observed live: a captured session came back with 0 bytes while the
     * server was demonstrably working. _IONBF, not _IOLBF: on Win32,
     * MSVCRT's setvbuf() documents _IOLBF as behaving exactly like _IOFBF
     * (full buffering) -- Windows has no real line-buffered mode, so
     * _IOLBF would silently NOT fix this bug. Both streams, not just
     * stderr: nothing here writes to stdout today, but the fix costs
     * nothing and stdout getting used later (e.g. a future `--json` machine
     * -readable mode) must not silently reintroduce this. setvbuf() itself
     * requires the stream not have had any I/O yet, hence "first thing". */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);

    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536) {
            port = (uint16_t)p;
        }
    }

    /* --headless, --no-browser, --console: any argv position, same reason
     * and same scan as the Linux host's --headless handling below --
     * `atticpad-server.exe 21100 --headless` keeps working and none of
     * these arguments needs to know about the others or about argv[1]'s
     * port. --headless (pre-existing) disables the whole UI surface: no
     * web UI, and (this task) no tray icon either -- a tray icon whose only
     * button opens a UI that was never started is not useful. --no-browser
     * and --console are this task's additions: see their uses below for
     * exactly what each one skips. */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--headless") == 0) {
                headless = 1;
            } else if (strcmp(argv[i], "--no-browser") == 0) {
                no_browser = 1;
            } else if (strcmp(argv[i], "--console") == 0) {
                force_console = 1;
            }
        }
    }
    {
        const char *p = getenv("ATTICPAD_UI_PORT");
        if (p != NULL && *p != '\0') {
            int v = atoi(p);
            if (v > 0 && v < 65536) {
                ui_port = (uint16_t)v;
            }
        }
    }

    /* JSONC per-device profiles (docs/DESIGN.md §6.2, server/src/profiles.c).
     * ATTICPAD_PROFILES_DIR overrides, same env var name as the Linux host
     * so a scripted deployment can set one value for both. Absent that,
     * default to "<the .exe's own directory>\profiles" rather than the
     * Linux host's cwd-relative "server/profiles" -- see exe_dir()'s
     * comment for why. A missing directory or an unparsable file inside it
     * is never fatal: logged below, and the library falls back to its
     * compiled-in default (see profiles.c). */
    profiles_dir = getenv("ATTICPAD_PROFILES_DIR");
    if (profiles_dir == NULL) {
        exe_dir(base_dir, sizeof base_dir);
        (void)snprintf(default_profiles_dir, sizeof default_profiles_dir,
                        "%s\\profiles", base_dir);
        default_profiles_dir[sizeof default_profiles_dir - 1] = '\0';
        profiles_dir = default_profiles_dir;
    }
    file_count = profile_store_scan(profiles_dir, files,
                                    (size_t)PROFILE_STORE_MAX_FILES);
    if (file_count == 0u) {
        (void)fprintf(stderr,
                      "[atticpad] profiles: no *.jsonc files loaded from "
                      "\"%s\" -- using the built-in default profile only\n",
                      profiles_dir);
    }
    for (i = 0; i < file_count; i++) {
        sources[i].label = files[i].label;
        sources[i].name  = files[i].name;
        sources[i].text  = files[i].text;
    }

    if (apad_net_init() != APAD_OK) {
        (void)fprintf(stderr, "[atticpad] apad_net_init failed\n");
        profile_store_free_files(files, file_count);
        return 1;
    }
    g_sock = apad_udp_open(port);
    if (g_sock == NULL) {
        (void)fprintf(stderr,
                      "[atticpad] failed to bind UDP :%u (already "
                      "running?)\n", (unsigned)port);
        profile_store_free_files(files, file_count);
        return 1;
    }
    (void)apad_udp_set_broadcast(g_sock, 1);   /* tier-2 discovery replies */

    /* SO_EXCLUSIVEADDRUSE: deliberately NOT set, and this is a gap, not a
     * decision made and closed. shim/net_winsock.c already documents why it
     * matters -- Windows SO_REUSEADDR (which apad_udp_open() sets
     * unconditionally, same as net_bsd.c) permits a genuine hijack of a
     * bound port rather than POSIX's harmless reuse-of-a-lingering-socket,
     * so a second `atticpad-server.exe` started against the same port would
     * bind successfully and silently steal traffic instead of failing here
     * with "already running?" above.
     *
     * This host cannot fix that: `apad_sock` (core/include/atticpad/
     * atticpad.h) is an opaque type whose only definition is the `struct
     * apad_sock { SOCKET fd; int in_use; }` inside net_winsock.c itself, and
     * the shim's public surface (apad_net_init/apad_udp_open/_send/_recv/
     * _set_broadcast/_close) has no call that exposes the underlying SOCKET
     * or that takes an extra setsockopt. There is no supported way for a
     * host to reach the fd apad_udp_open() created. Per this task's brief
     * ("if the shim gives you no way to set it, REPORT that rather than
     * editing the shim") this is reported, not patched around: shim/ is not
     * this file's to change, and adding a parallel raw Winsock socket bound
     * to the same port from here would not set SO_EXCLUSIVEADDRUSE on the
     * socket that matters (the one apad_udp_recv() actually reads) anyway.
     *
     * The fix, when someone makes it, is a shim addition shaped like
     * apad_udp_set_broadcast() -- e.g. apad_udp_set_exclusive(apad_sock *s,
     * int enable) -- called from here exactly where this comment is. */

    memset(&cfg, 0, sizeof cfg);
    cfg.on_send       = host_send;
    cfg.on_log        = host_log;
    /* §10 pairing secrets and per-session nonces. There is no trigger wired
     * to it in this console host yet -- Windows has no SIGUSR1, and the
     * tray application (docs/DESIGN.md §6.4) is where a "Pair" menu item belongs.
     * Supplying on_random now means the library is ready the moment that UI
     * exists, and costs nothing meanwhile: with no window open the server
     * behaves exactly as it does today. */
    cfg.on_random     = host_random;
    cfg.user          = NULL;   /* g_sock is file-scope; nothing to carry */
    cfg.server_port   = port;
    cfg.server_name   = NULL;   /* library default: "AtticPad Server" */
    cfg.profiles      = (file_count > 0) ? sources : NULL;
    cfg.profile_count = file_count;
    /* §7 tier 2 subnet-broadcast filtering (apadserver.h cfg.broadcast_addrs,
     * server/src/server.c is_bad_reply_target()). Left at the memset above's
     * NULL/0 -- GetAdaptersAddresses does not hand back a broadcast address
     * directly, deriving one needs a unicast entry's OnLinkPrefixLength
     * applied to its address (server/host/common/ipaddr.h's
     * host_own_addr::has_bcast doc comment), which is a follow-up, not done
     * here. An empty list is exactly today's behaviour: no regression, just
     * the gap docs/PROTOCOL.md §7 already allows ("a subnet-directed
     * broadcast it can identify" -- this host cannot, yet). */

    /* Parses the profiles and brings the backend up (ViGEmBus connect);
     * logs why through host_log if it refuses -- most commonly "ViGEmBus
     * not installed", see server/backends/vigem.c's init() codes. */
    server = apad_server_create(&cfg, &apad_backend_vigem);
    /* The library copied what it keeps, so the file blobs are done. */
    profile_store_free_files(files, file_count);
    if (server == NULL) {
        apad_udp_close(g_sock);
        return 1;
    }

    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        /* Not fatal -- Ctrl+C simply would not shut down cleanly, and the
         * process is still fully functional. Logged so a silent SIGKILL-
         * shaped exit later is not a mystery. */
        (void)fprintf(stderr,
                      "[atticpad] warning: SetConsoleCtrlHandler failed "
                      "(error %lu) -- close this window from Task Manager, "
                      "not the X button, if a clean shutdown matters\n",
                      (unsigned long)GetLastError());
    }

    /* ASCII-only from here down (no em-dashes, no smart quotes, no box
     * drawing): this is the diagnostic a user reads when something is
     * wrong, over a Windows console codepage that mangles anything above
     * 0x7F, and a mangled diagnostic is worse than none. */
    /* See the Linux host: the version goes on the first line printed. */
    (void)fprintf(stderr,
                  "[atticpad] AtticPad server %s (protocol v%u)\n",
                  APAD_VERSION_STR, (unsigned)APAD_VERSION);
    (void)fprintf(stderr,
                  "[atticpad] server listening on UDP :%u, backend \"%s\", "
                  "%u pad slots\n",
                  (unsigned)port, apad_backend_vigem.name,
                  (unsigned)APAD_MAX_SESSIONS);
    (void)fprintf(stderr,
                  "[atticpad] connect a client to one of these addresses, "
                  "port %u:\n", (unsigned)port);
    print_own_addresses(stderr);

    /* §7 tier 1 is NOT implemented on this host -- there is no mDNS
     * responder here (server/host/linux/mdns.h is Linux-only and stayed
     * there). Say so, in the UI and on the console, rather than leaving the
     * block absent: "no tier 1" and "tier 1 silently broken" look identical
     * to a user otherwise, and §7 requires the difference be visible.
     * Filled once -- it cannot change while the process runs. */
    ui_mdns_status_not_implemented(&ui_mdns);
    (void)fprintf(stderr, "[atticpad] automatic discovery: %s\n",
                  ui_mdns.message);

    /* The UI. apad_net_init() above already did WSAStartup (shim/
     * net_winsock.c), which every Winsock call below depends on -- opening
     * the UI before the protocol socket would be a use-before-init. */
    if (!headless) {
        ui_fd = webui_open(ui_port);
        if (!apad_sock_valid(ui_fd)) {
            (void)fprintf(stderr,
                          "[atticpad] UI: failed to bind 127.0.0.1:%u "
                          "(already running, or ATTICPAD_UI_PORT taken?) "
                          "-- continuing headless\n",
                          (unsigned)ui_port);
        } else {
            (void)fprintf(stderr,
                          "[atticpad] UI: http://127.0.0.1:%u/ "
                          "(loopback only -- pass --headless to disable)\n",
                          (unsigned)ui_port);
            (void)fprintf(stderr,
                          "[atticpad] UI: open that in a browser to pair a "
                          "device -- it shows the PIN, the QR code and the "
                          "120-second countdown\n");
        }
    }

    /* This task: present as an app, not a bare console window. Tray icon
     * and startup browser launch, both skipped under --headless (a tray
     * icon whose only useful button opens a UI that was never started is
     * not worth having); the browser launch is separately skippable with
     * --no-browser while keeping the tray. See this file's "browser +
     * system tray" section above for the threading and console-ownership
     * reasoning behind each call here. */
    if (!headless) {
        if (tray_init(server, ui_port) && !force_console) {
            /* Only after the tray icon is confirmed up -- a hidden console
             * with no tray icon (e.g. no interactive desktop) would leave
             * this process with no visible surface at all. --console
             * overrides unconditionally. */
            hide_console_if_owned();
        }
        if (!no_browser) {
            if (apad_sock_valid(ui_fd)) {
                open_browser(ui_port);
            } else {
                (void)fprintf(stderr,
                              "[atticpad] browser: not opening -- the UI "
                              "failed to bind (see above); pass "
                              "--no-browser to silence this line on future "
                              "runs\n");
            }
        }
    } else {
        (void)fprintf(stderr,
                      "[atticpad] --headless: no tray icon, no browser "
                      "launch\n");
    }

    while (InterlockedCompareExchange(&g_should_exit, 0, 0) == 0) {
        apad_addr from;
        uint8_t buf[APAD_MAX_DATAGRAM];
        int n = apad_udp_recv(g_sock, &from, buf, sizeof buf, RECV_TIMEOUT_MS);
        uint32_t now = apad_ticks_ms();

        /* Both entry points take the clock explicitly, so this order is a
         * convenience rather than a requirement (apadserver.h): one
         * apad_ticks_ms() per iteration, read after the up-to-20 ms select()
         * so it reflects when this datagram actually arrived. Identical to
         * the Linux host's loop. */
        (void)apad_server_tick(server, now);

        if (n > 0) {
            (void)apad_server_on_datagram(server, now, &from, buf, (size_t)n);
        }
        /* n == 0: recv timeout, nothing arrived. n < 0: transient socket
         * error; the loop just tries again. */

        /* Same single-threaded contract as the Linux host: the UI's accept()
         * is non-blocking and one request is served to completion here,
         * between datagrams. apad_udp_recv() above already bounds an
         * iteration at RECV_TIMEOUT_MS, so the UI is polled at least that
         * often without a thread, and a UI request cannot be serviced
         * concurrently with input processing -- which is exactly why no
         * lock is needed around apad_server_*. */
        webui_poll(ui_fd, server, now, port, server_name_used, profiles_dir,
                  &ui_mdns);

        /* Same non-blocking, same-thread contract as webui_poll() just
         * above: tray_pump() drains the message-only window's queue with
         * PeekMessageW (never GetMessage(), never blocks) and
         * tray_update_tooltip() makes at most one throttled NIM_MODIFY
         * call. Both are no-ops when tray_init() never ran or failed
         * (g_tray.hwnd/added stay NULL/0). See this file's "browser +
         * system tray" section for why this is safe to call every
         * iteration without a lock. */
        tray_pump();
        tray_update_tooltip(now);
    }

    (void)fprintf(stderr, "[atticpad] shutting down\n");
    /* NIM_DELETE before anything else: a tray icon left behind after the
     * process dies is the classic failure mode here, and tray_shutdown()
     * is safe to call unconditionally (idempotent, no-op if the tray was
     * never added). */
    tray_shutdown();
    webui_close(ui_fd);
    apad_server_destroy(server);   /* tears down live sessions, then the backend */
    apad_udp_close(g_sock);
    return 0;
}
