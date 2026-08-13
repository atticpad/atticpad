/* server/host/linux/main.c — the Linux host for libapadserver (docs/DESIGN.md §6.4).
 *
 * The thin half of the split. This file owns the process and everything
 * that comes with it: argv, signals, the UDP socket (through shim/), the
 * recv loop, reading profile files off disk, and the stderr logging stream.
 * It owns no protocol logic whatsoever -- every datagram goes straight into
 * apad_server_on_datagram() and every reply comes back out through
 * host_send().
 *
 * It is also the regression check for the split itself: the Windows host at
 * M4 is the first real test of the API, and this binary's job is to prove
 * that moving the session table into a library changed nothing on the wire.
 * tools/loopback-client exercises DISCOVER/ANNOUNCE, HELLO/WELCOME + the §9
 * ACK, and PING/PONG against it.
 *
 * The one thing this file knows about backends is which one to pass in
 * (apad_backend_uinput). Nothing else here -- and nothing in the library --
 * knows uinput exists (docs/DESIGN.md §6.1).
 *
 * As of the server UI task (docs/DESIGN.md §6.3, §6.4 "Host owns ... UI"), this
 * file ALSO owns a small HTTP server on 127.0.0.1 -- server/host/linux/
 * webui.h, header-only for the same build-script-constraint reason
 * documented at the top of that file. `--headless` (any argv position)
 * skips it entirely and reproduces exactly this file's pre-UI behaviour
 * (no listening TCP socket, no extra log lines, identical exit codes) --
 * that is what tools/ and CI's smoke run rely on continuing to work
 * unchanged, and neither passes the flag, which is deliberate: the default
 * is now UI-on, matching what a human running this server actually wants,
 * and --headless is the opt-out a script reaches for.
 */
/* POSIX.1-2008 for opendir/readdir/closedir/strdup -- same feature-test
 * macro shim/net_bsd.c already uses for the same reason (glibc hides these
 * under -std=c11 without it). Must precede every system header include.
 * These came with the directory scan when it moved out of profiles.c. */
#define _POSIX_C_SOURCE 200809L
/* getrandom(2) and <sys/random.h> are glibc extensions, hidden by a bare
 * _POSIX_C_SOURCE. _DEFAULT_SOURCE puts them back without pulling in the
 * whole of _GNU_SOURCE. The /dev/urandom fallback below exists for the
 * runtime case (an old kernel, a chroot), not for this. */
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>   /* getpid, ssize_t */

#include "atticpad/atticpad.h"
#include "atticpad/version.h"   /* APAD_VERSION_STR for the startup banner */
#include "apadserver.h"
#include "backends.h"   /* server/backends/backends.h -- registry of
                          * backends compiled in for this platform; this
                          * host names apad_backend_uinput, guarded there
                          * for non-_WIN32 builds */
#include "mdns.h"    /* server/host/linux/mdns.h -- docs/PROTOCOL.md §7 tier 1,
                      * the advertise-only mDNS responder. Header-only for the
                      * same build-script reason as webui.h; owns its own 5353
                      * socket because mDNS is not AtticPad protocol traffic and
                      * shim/ exposes no multicast API (see that file's top). */
#include "../common/profiles_builtin.h"   /* profiles compiled into the binary */
#include "../common/webui.h"   /* server/host/common/webui.h -- the HTTP UI, header-only */

/* Bounds how late apad_session_tick() can discover a due retransmit.
 * §9's first retransmit gap is 100 ms; at 200 ms poll granularity that
 * could fire as late as t~200 ms, and since apad_session_tick() reschedules
 * the NEXT due time from the actual (late) now, the drift compounds across
 * all four attempts -- §9 warns this reads as indistinguishable from packet
 * loss. 20 ms bounds total compounded drift to ~80 ms across four stages,
 * comfortably inside the 2300 ms retransmit-failure deadline and the 3000 ms
 * idle timeout. Cheap: select() with a short timeout does not busy-loop. */
#define RECV_TIMEOUT_MS   20

/* The UI's HTTP port. Deliberately far from APAD_DEFAULT_PORT (21100, UDP)
 * so the two are never mistaken for each other in a log line or a `curl`
 * command -- this one is TCP, loopback-only, and speaks HTTP, not the wire
 * protocol. ATTICPAD_UI_PORT overrides, same convention as
 * ATTICPAD_PROFILES_DIR above. */
#define ATTICPAD_UI_DEFAULT_PORT 21150

/* How many *.jsonc files this host will pick up from the profiles
 * directory -- server/host/common/profile_store.h's PROFILE_STORE_MAX_FILES,
 * pulled in transitively via "../common/webui.h" above. Independent of the
 * library's own APAD_PROFILES_MAX (the size of its loaded-profile table):
 * that one bounds what parsing keeps, this one bounds the directory scan --
 * the library must not be handed a filesystem limit and the host must not
 * be handed a table size. */

static apad_sock *g_sock;
static volatile sig_atomic_t g_should_exit;

/* §7 tier 1. File scope for symmetry with g_sock: the responder is a
 * process-lifetime resource, and mdns_close() must run on the shutdown
 * path whether or not the UI was ever started. */
static mdns_responder g_mdns;

/* §10 pairing is user-initiated by definition, and this host has no UI to
 * initiate it from: it is a headless daemon whose only screen is stderr. A
 * signal is the one "the human at the keyboard did something" channel a
 * process like this has --
 *
 *     kill -USR1 $(pidof atticpad-server)   # open a 120 s pairing window
 *     kill -USR2 $(pidof atticpad-server)   # cancel it
 *
 * -- and it is explicit and deliberate in exactly the way §10 asks for. It
 * is a stand-in, not the design: the tray UI and the Android foreground
 * service call apad_server_begin_pairing() from a button. Set
 * ATTICPAD_PAIR_TOKEN=1 to make USR1 generate the long QR-shaped token
 * instead of the 6-digit PIN.
 *
 * Handlers do nothing but set a flag; the work happens in the main loop
 * where there is a clock to pass and no async-signal-safety question. */
static volatile sig_atomic_t g_want_pair;
static volatile sig_atomic_t g_want_unpair;

/* Build "<dir containing this executable>/<leaf>" into `out`.
 *
 * Returns 0 on success, -1 if the executable path cannot be resolved or does
 * not fit -- callers fall back to a relative path in that case rather than
 * failing to start, because not finding profiles is never fatal.
 *
 * /proc/self/exe is Linux-specific and this is the Linux host, so that is
 * fine here; it also resolves symlinks, which is what a user who symlinked
 * the binary into ~/bin would expect. */
static int exe_relative_dir(char *out, size_t cap, const char *leaf)
{
    char path[4096];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", path, sizeof path - 1u);
    if (n <= 0 || (size_t)n >= sizeof path - 1u) {
        return -1;
    }
    path[n] = '\0';
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';
    if ((size_t)snprintf(out, cap, "%s/%s", path, leaf) >= cap) {
        return -1;
    }
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_should_exit = 1;
}

static void on_pair_signal(int sig)
{
    if (sig == SIGUSR2) {
        g_want_unpair = 1;
    } else {
        g_want_pair = 1;
    }
}

/* ---- the two callbacks that make the library sans-IO --------------------*/

/* Rule 1 (apadserver.h): outbound datagrams leave through here, so the
 * library never touches a socket. Returns apad_udp_send's result unchanged,
 * negative on failure -- the library depends on that to avoid consuming a
 * tx sequence or arming a retransmit for a datagram that never left. */
static int host_send(void *user, const apad_addr *to, const uint8_t *buf,
                     size_t len)
{
    (void)user;
    return apad_udp_send(g_sock, to, buf, len);
}

/* Rule 2: diagnostics leave through here. The "[atticpad] " prefix and the
 * newline that used to be baked into every fprintf in the server now belong
 * to the sink, which is why the split changed no log output. Every level
 * goes to stderr: even with the UI running this is still a process whose
 * only guaranteed screen is stderr (--headless has no UI at all), and a
 * level filter here would hide exactly the profile and teardown lines that
 * make a live session debuggable.
 *
 * Also pushes into webui.h's ring buffer (server/host/linux/webui.h
 * ui_log_push()) so the UI's "Log" panel and /api/state's "log" array show
 * the same lines this prints -- apadserver.h's on_log doc comment
 * anticipates exactly this ("the callback's own doc comment anticipates a
 * UI consuming it"). Harmless to call even with --headless: the ring buffer
 * just sits there unread. */
static void host_log(void *user, apad_log_level level, const char *msg)
{
    static const char *const level_str[] = { "INFO", "WARN", "ERROR" };
    const char *lvl = (level <= APAD_LOG_ERROR) ? level_str[level] : "?";
    char line[UI_LOG_LINE_MAX];

    (void)user;
    (void)fprintf(stderr, "[atticpad] %s\n", msg);
    (void)snprintf(line, sizeof line, "[%s] %s", lvl, msg);
    ui_log_push(line);
}

/* Rule 3 for pairing (apadserver.h): entropy is platform I/O and the
 * library must not go looking for it. getrandom(2) first -- no file
 * descriptor to exhaust, no /dev to be missing inside a container, and it
 * blocks only until the kernel pool is initialised, which on any machine
 * that has finished booting far enough to run this is already true.
 *
 * The read loop is not decoration: getrandom() may return a short count
 * (it caps at 33554431 bytes, irrelevant here, but it is also interruptible
 * by a signal -- and this process takes SIGUSR1 for pairing, so EINTR on
 * the very call that generates a pairing secret is a real path, not a
 * theoretical one).
 *
 * /dev/urandom is the fallback for a kernel older than 3.17 or a chroot
 * where the syscall is filtered. Returns 1 only when the FULL buffer was
 * filled; a partial fill is a failure, and libapadserver turns that into
 * "no pairing window opens" rather than a weaker secret. */
static int fill_urandom(uint8_t *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;

    if (f == NULL) {
        return 0;
    }
    got = fread(buf, 1, len, f);
    (void)fclose(f);
    return got == len;
}

static int host_random(void *user, uint8_t *buf, size_t len)
{
    size_t off = 0;

    (void)user;
    while (off < len) {
        ssize_t n = getrandom(buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fill_urandom(buf, len);   /* ENOSYS, EPERM (seccomp), ... */
        }
        if (n == 0) {
            return fill_urandom(buf, len);
        }
        off += (size_t)n;
    }
    return 1;
}

/* Print the pairing window the way a UI would render it, from the public
 * snapshot rather than from anything this host tracked itself -- this is
 * also the check that apad_server_pairing_state() is actually usable by the
 * thing it was designed for. §7 tier 3 and §8's "[server shows PIN if
 * unpaired]" both land here on a headless box: stdout is the screen.
 *
 * This, and NOT the on_log sink, is the only place a secret may reach
 * stderr: apad_logf() lines about pairing (pairing.c) are deliberately
 * secret-free because on_log also feeds webui.h's log ring, which
 * /api/state serves over HTTP and which outlives the 120 s window. The
 * secret lives only in the query snapshot below and in this direct
 * fprintf, which never touches host_log()/ui_log_push(). */
static void show_pairing(apad_server *server)
{
    apad_pairing_info info;

    if (apad_server_pairing_state(server, &info) != APAD_OK) {
        return;
    }
    if (!info.open) {
        (void)fprintf(stderr, "[atticpad] no pairing window is open\n");
        return;
    }
    (void)fprintf(stderr,
                  "[atticpad] ==========================================\n"
                  "[atticpad]   PAIRING %s:  %s\n"
                  "[atticpad]   %u s left, %u attempts left\n"
                  "[atticpad] ==========================================\n",
                  (info.kind == (uint8_t)APAD_PAIRING_TOKEN) ? "TOKEN" : "PIN",
                  info.secret, (unsigned)(info.ms_remaining / 1000u),
                  (unsigned)info.attempts_remaining);
}

/* Tracks the last generation this process has already printed via
 * show_pairing(), so poll_pairing_display() below prints exactly once per
 * secret rather than once per main-loop iteration. Generation 0 can never
 * be a live secret (apad_pairing_open()'s first regenerate() takes it to
 * 1), so the initial value never collides with a real window. */
static uint32_t g_last_shown_pairing_gen = 0;

/* Polled once per main-loop iteration rather than only right after a
 * SIGUSR1: apad_pairing_fail() (pairing.c, S11) can rotate the secret to a
 * new generation after five bad attempts WITHOUT the host ever calling
 * apad_server_begin_pairing() again, and that rotation's log line is now
 * deliberately secret-free (see the comment above host_log() and
 * pairing.c). Polling the generation counter is how a headless operator
 * still finds out the PIN changed underneath them, instead of the only
 * record of the new secret being one this process never shows anyone. */
static void poll_pairing_display(apad_server *server)
{
    apad_pairing_info info;

    if (apad_server_pairing_state(server, &info) != APAD_OK) {
        return;
    }
    if (info.open && info.generation != g_last_shown_pairing_gen) {
        show_pairing(server);
        g_last_shown_pairing_gen = info.generation;
    }
}

/* ---- profile files: the host half of the profiles seam -------------------
 *
 * load_profile_files()/free_profile_files() and their has_jsonc_suffix/
 * cmp_str/read_whole_file helpers used to live here, hand-written against
 * <dirent.h>. They now live in server/host/common/profile_store.h
 * (profile_store_scan()/profile_store_free_files()) -- already pulled in
 * transitively via "../common/webui.h" above -- shared with
 * server/host/windows/main.c's near-identical FindFirstFileA twin (which
 * this task's own remapping-editor work found and consolidated) and with
 * webui.h's new PUT/DELETE /api/profile/{name} routes, which need the exact
 * same scan to rebuild the profile set after a write. See profile_store.h's
 * top comment for the full "why one file, not two" reasoning.
 */

int main(int argc, char **argv)
{
    uint16_t port = (uint16_t)APAD_DEFAULT_PORT;
    uint16_t ui_port = (uint16_t)ATTICPAD_UI_DEFAULT_PORT;
    int headless = 0;
    int mdns_enabled = 1;
    apad_socket_t ui_fd = APAD_INVALID_SOCKET;
    static const char server_name_used[] = "AtticPad Server";
    apad_server_cfg cfg;
    apad_server *server;
    profile_store_file files[PROFILE_STORE_MAX_FILES];
    apad_profile_source sources[PROFILE_STORE_MAX_FILES];
    size_t file_count, i;
    apad_addr bcast_addrs[HOST_MAX_OWN_ADDRS];   /* §7 tier 2, see below */
    size_t bcast_count = 0;
    /* ATTICPAD_PROFILES_DIR-or-default, resolved once below and kept alive
     * for the whole process: webui.h's new PUT/DELETE /api/profile/{name}
     * and POST /api/profiles/reload routes need to re-scan this SAME
     * directory on every write, long after the startup block that first
     * reads it has gone out of scope. */
    const char *profiles_dir;
    /* profile_store.h formats "<dir>/<name>.jsonc" into a char[512],
     * so anything longer is a truncation waiting to happen -- keep this
     * comfortably inside that and fall back to a relative path if the
     * executable lives somewhere deeper. */
    char        exe_profiles_dir[320];

    /* Matches server/host/windows/main.c's fix for the same class of bug,
     * checked here for the same latent problem (2026-08-10 report): glibc
     * DOES already document stderr as always unbuffered by default,
     * independent of whether it is a tty, a file or a pipe, so this host
     * was not actually caught by it -- everything in this file prints to
     * stderr, never stdout, and confirmed via `strace -e write` that a
     * piped run flushes every line immediately, unlike a from-scratch
     * repro of the Windows report. Setting it explicitly anyway rather than
     * leaning on that default: it costs nothing, it makes both hosts'
     * buffering policy state the same fact instead of one relying on
     * documented default behaviour and the other on an explicit call, and
     * it stops stdout (unused today, block-buffered to a pipe like any
     * other stream if that ever changes) from quietly reintroducing this
     * exact bug the day something is first printed there. Must be the
     * first thing main() does -- setvbuf() requires no I/O on the stream
     * yet. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);

    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536) {
            port = (uint16_t)p;
        }
    }
    /* --headless: reproduce exactly this file's pre-UI behaviour (see the
     * file header comment). Scans every argv entry rather than a fixed
     * position so it can follow the port argument (`atticpad-server 21100
     * --headless`) without disturbing argv[1]'s existing port-parsing
     * contract above -- every existing caller (scripts/build.sh's smoke
     * test, tools/loopback-client's target, CI) passes at most a bare port
     * and never this flag, so none of them are affected either way. */
    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
        }
        /* --no-mdns is a SEPARATE switch from --headless on purpose. tier 1
         * is a network service, not a screen: a user running two servers on
         * one machine, or on a network where multicast is filtered or
         * unwelcome, wants mDNS off and the UI on, and a script wants the
         * UI off and does not care about mDNS. Collapsing the two would
         * also silently change what --headless has always meant to
         * scripts/build.sh and CI (no listening TCP socket -- nothing about
         * UDP). ATTICPAD_MDNS=0 does the same thing for an environment that
         * cannot edit an argv. */
        if (strcmp(argv[i], "--no-mdns") == 0) {
            mdns_enabled = 0;
        }
    }
    {
        const char *p = getenv("ATTICPAD_UI_PORT");
        if (p != NULL) {
            int v = atoi(p);
            if (v > 0 && v < 65536) {
                ui_port = (uint16_t)v;
            }
        }
    }

    /* JSONC per-device profiles (docs/DESIGN.md §6.2, server/src/profiles.c).
     * ATTICPAD_PROFILES_DIR lets a packaged install point this somewhere
     * other than a source checkout; "server/profiles" (relative to the
     * process's cwd) is the default so running from the repo root -- what
     * every build/test invocation in this tree does -- just works. A
     * missing directory or an unparsable file inside it is never fatal:
     * the read below logs and the library falls back to the compiled-in
     * default (see profiles.c). */
    profiles_dir = getenv("ATTICPAD_PROFILES_DIR");
    if (profiles_dir == NULL) {
        /* Next to the binary, NOT relative to the cwd. This used to be
         * "server/profiles", which is a path inside a source checkout: it
         * made every build/test invocation from the repo root work and made
         * a downloaded binary depend on which directory the user happened to
         * be in. exe_relative_dir() matches what the Windows host has always
         * done, so "drop a profiles/ folder beside the server" now means the
         * same thing on both. */
        if (exe_relative_dir(exe_profiles_dir, sizeof exe_profiles_dir,
                             "profiles") == 0) {
            profiles_dir = exe_profiles_dir;
        } else {
            profiles_dir = "profiles";
        }
    }
    file_count = profile_store_scan(profiles_dir, files,
                                    (size_t)PROFILE_STORE_MAX_FILES);
    if (file_count == 0u) {
        /* Missing directory, empty directory, or nothing with a .jsonc
         * suffix -- profile_store_scan() does not distinguish these (never
         * fatal either way, see profile_store.h), so neither does this
         * message.
         *
         * Fall back to the profiles compiled into this binary rather than to
         * the library's single built-in default. That difference is the
         * whole point: the built-in default has no touch regions and no gyro
         * aim, so a downloaded server would hand a 3DS a pad with dead
         * triggers while the client kept drawing them. */
        for (i = 0; i < ATTICPAD_BUILTIN_PROFILE_COUNT; i++) {
            sources[i].label = ATTICPAD_BUILTIN_PROFILES[i].label;
            sources[i].name  = ATTICPAD_BUILTIN_PROFILES[i].name;
            sources[i].text  = ATTICPAD_BUILTIN_PROFILES[i].text;
        }
        file_count = ATTICPAD_BUILTIN_PROFILE_COUNT;
        (void)fprintf(stderr,
                      "[atticpad] profiles: none on disk in \"%s\" -- using "
                      "the %u profiles built into this binary\n",
                      profiles_dir, (unsigned)ATTICPAD_BUILTIN_PROFILE_COUNT);
    } else {
        for (i = 0; i < file_count; i++) {
            sources[i].label = files[i].label;
            sources[i].name  = files[i].name;
            sources[i].text  = files[i].text;
        }
    }

    if (apad_net_init() != APAD_OK) {
        (void)fprintf(stderr, "[atticpad] apad_net_init failed\n");
        profile_store_free_files(files, file_count);
        return 1;
    }
    g_sock = apad_udp_open(port);
    if (g_sock == NULL) {
        (void)fprintf(stderr,
                      "[atticpad] failed to bind UDP :%u (already running?)\n",
                      (unsigned)port);
        profile_store_free_files(files, file_count);
        return 1;
    }
    (void)apad_udp_set_broadcast(g_sock, 1);   /* tier-2 discovery replies */

    /* §7 tier 2: hand the library this host's subnet-directed broadcast
     * addresses (apadserver.h cfg.broadcast_addrs, server/src/server.c
     * is_bad_reply_target()) so a spoofed DISCOVER claiming to come from,
     * say, 192.168.1.255 gets no ANNOUNCE. Same getifaddrs() walk as the
     * tier-3 banner below (ipaddr.h's own comment says fresh-every-call is
     * fine and cheap for a handful of interfaces); queried again there
     * rather than reused because that walk runs after apad_server_create()
     * and this one has to run before it -- cfg is consumed by the call. */
    {
        host_own_addr oaddrs[HOST_MAX_OWN_ADDRS];
        size_t noaddr = host_enumerate_own_ipv4(oaddrs, HOST_MAX_OWN_ADDRS);
        size_t j;

        bcast_count = 0;
        for (j = 0; j < noaddr && bcast_count < HOST_MAX_OWN_ADDRS; j++) {
            if (!oaddrs[j].has_bcast) {
                continue;
            }
            bcast_addrs[bcast_count].ip[0] = oaddrs[j].bcast[0];
            bcast_addrs[bcast_count].ip[1] = oaddrs[j].bcast[1];
            bcast_addrs[bcast_count].ip[2] = oaddrs[j].bcast[2];
            bcast_addrs[bcast_count].ip[3] = oaddrs[j].bcast[3];
            bcast_addrs[bcast_count].port  = 0;   /* cfg ignores it */
            bcast_count++;
        }
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.on_send       = host_send;
    cfg.on_log        = host_log;
    cfg.on_random     = host_random;   /* §10 pairing secrets and nonces */
    cfg.user          = NULL;   /* g_sock is file-scope; nothing to carry */
    cfg.server_port   = port;
    cfg.server_name   = NULL;   /* library default: "AtticPad Server" */
    cfg.profiles      = (file_count > 0) ? sources : NULL;
    cfg.profile_count = file_count;
    cfg.broadcast_addrs      = (bcast_count > 0) ? bcast_addrs : NULL;
    cfg.broadcast_addr_count = bcast_count;

    /* Parses the profiles and brings the backend up; logs why through
     * host_log if it refuses. */
    server = apad_server_create(&cfg, &apad_backend_uinput);
    /* The library copied what it keeps, so the file blobs are done. */
    profile_store_free_files(files, file_count);
    if (server == NULL) {
        apad_udp_close(g_sock);
        return 1;
    }

    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);
    (void)signal(SIGUSR1, on_pair_signal);
    (void)signal(SIGUSR2, on_pair_signal);

    /* The version belongs on the FIRST line the server ever prints: a bug
     * report that does not name a version costs more to triage than the
     * version costs to print (core/include/atticpad/version.h). */
    (void)fprintf(stderr,
                  "[atticpad] AtticPad server %s (protocol v%u)\n",
                  APAD_VERSION_STR, (unsigned)APAD_VERSION);
    (void)fprintf(stderr,
                  "[atticpad] server listening on UDP :%u, backend \"%s\", "
                  "%u pad slots\n",
                  (unsigned)port, apad_backend_uinput.name,
                  (unsigned)APAD_MAX_SESSIONS);
    (void)fprintf(stderr,
                  "[atticpad] devices can connect without a PIN. To require "
                  "one, open AtticPad in your browser and choose Pair. "
                  "(headless: kill -USR1 %ld opens pairing, -USR2 cancels)\n",
                  (long)getpid());

    /* docs/PROTOCOL.md §7: "The server MUST display its own IP prominently
     * at all times." The UI (below, when not --headless) satisfies "at all
     * times" by recomputing this on every /api/state poll; this one banner
     * line is the --headless equivalent -- printed once, since a headless
     * daemon has no persistent screen to keep it on beyond stderr. */
    {
        host_own_addr addrs[HOST_MAX_OWN_ADDRS];
        size_t naddr = host_enumerate_own_ipv4(addrs, HOST_MAX_OWN_ADDRS);
        size_t j;

        if (naddr == 0u) {
            (void)fprintf(stderr,
                          "[atticpad] no network address found -- is this "
                          "PC connected to a network?\n");
        } else {
            (void)fprintf(stderr,
                          "[atticpad] if your device can't find this PC "
                          "automatically, enter this address on it:");
            for (j = 0; j < naddr; j++) {
                (void)fprintf(stderr, " %s(%s)", addrs[j].ip, addrs[j].iface);
            }
            (void)fprintf(stderr, "\n");
        }
    }

    /* §7 tier 1. Started AFTER the tier-3 banner above and deliberately
     * before the UI: a failed 5353 bind must be on stderr even when there
     * is no UI to render it (--headless), because §7's "the server UI MUST
     * say so" has no other place to land on a headless box -- exactly the
     * argument the tier-3 banner above already makes for its own line.
     * Never fatal: tier 1 is a convenience and tiers 2 and 3 carry the load
     * (docs/DESIGN.md §5.5). g_mdns.message/remedy are themselves jargon-free
     * customer copy as of the copy sweep (server/host/linux/mdns.h) -- this
     * line only adds the "Automatic discovery:" framing on top. */
    mdns_open(&g_mdns, server_name_used, port, mdns_enabled);
    if (g_mdns.state == MDNS_STATE_RUNNING) {
        (void)fprintf(stderr, "[atticpad] automatic discovery: on\n");
    } else {
        (void)fprintf(stderr, "[atticpad] automatic discovery: off -- %s\n",
                      g_mdns.message);
        if (g_mdns.remedy[0] != '\0') {
            (void)fprintf(stderr, "[atticpad]   %s\n", g_mdns.remedy);
        }
    }

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
        }
    }

    while (!g_should_exit) {
        apad_addr from;
        uint8_t buf[APAD_MAX_DATAGRAM];
        int n = apad_udp_recv(g_sock, &from, buf, sizeof buf, RECV_TIMEOUT_MS);
        uint32_t now = apad_ticks_ms();

        /* Both entry points take the clock explicitly, so this order is a
         * convenience rather than a requirement (apadserver.h): one
         * apad_ticks_ms() per iteration, read after the up-to-20 ms select()
         * so it reflects when this datagram actually arrived. */
        (void)apad_server_tick(server, now);

        /* Consumed here, not in the handler: begin_pairing needs a clock
         * and may log, neither of which belongs in a signal handler. The
         * flag is cleared before the call so a signal arriving during it is
         * not lost. */
        if (g_want_pair) {
            const char *want_token = getenv("ATTICPAD_PAIR_TOKEN");
            g_want_pair = 0;
            (void)apad_server_begin_pairing(server, now,
                                            want_token != NULL
                                                && want_token[0] == '1');
            /* Display, on success or on an S11 rotation later, is handled
             * by poll_pairing_display() below -- not here -- so both paths
             * go through the same generation-tracked print. A failure
             * already went out through host_log with the reason (no
             * on_random, or on_random failed) -- apadserver.h. */
        }
        if (g_want_unpair) {
            g_want_unpair = 0;
            apad_server_cancel_pairing(server, now);
        }
        poll_pairing_display(server);

        if (n > 0) {
            (void)apad_server_on_datagram(server, now, &from, buf, (size_t)n);
        }
        /* n == 0: recv timeout, nothing arrived. n < 0: transient socket
         * error; the loop just tries again. */

        /* webui.h's own top comment explains the threading choice: this is
         * a non-blocking accept() (instant if nothing pending, the common
         * case) followed by a fully synchronous, timeout-bounded handling
         * of at most one connection, once per lap of this same loop. */
        /* Same non-blocking contract as webui_poll below, and for a much
         * stricter reason: this socket shares the loop that carries
         * INPUT_STATE. It is O_NONBLOCK, it drains at most
         * MDNS_MAX_RX_PER_POLL datagrams per lap, it never allocates and
         * its sends are best-effort -- there is no path through mdns_poll()
         * that can wait on anything. */
        mdns_poll(&g_mdns, server, now);

        {
            /* mdns.h owns what a mdns_responder means; webui.h only ever
             * sees this snapshot, which is what lets the SAME webui.h
             * compile into a host with no responder at all. */
            ui_mdns_status ui_mdns;
            mdns_ui_status(&g_mdns, &ui_mdns);
            webui_poll(ui_fd, server, now, port, server_name_used, profiles_dir,
                      &ui_mdns);
        }
    }

    (void)fprintf(stderr, "[atticpad] shutting down\n");
    /* RFC 6762 §10.1 goodbye (TTL 0) before anything else goes away, so a
     * browser drops this server the instant it exits instead of offering a
     * dead entry for the record TTL. */
    mdns_close(&g_mdns);
    webui_close(ui_fd);
    apad_server_destroy(server);   /* tears down live sessions, then the backend */
    apad_udp_close(g_sock);
    return 0;
}
