/* net_nds.c — DSWiFi/lwIP shim for the Nintendo DS / DSi (docs/DESIGN.md §3).
 *
 * A COPY of shim/net_bsd.c, not an edit of it (docs/CONVENTIONS.md's blind-platform
 * rule; shim/net_bsd.c stays hardware-proven on the 3DS and untouched). Every
 * function here is byte-for-byte net_bsd.c's EXCEPT apad_udp_recv(), which
 * this file exists to replace. See references/nds/README.md ("Outcome B")
 * for how this was found and references/nds/examples/dswifi/get_website/
 * source/main.c for the non-blocking-socket idiom this platform's own SDK
 * sample uses.
 *
 * =========================================================================
 * WHY apad_udp_recv() CANNOT BE net_bsd.c's select()+recvfrom()
 * =========================================================================
 * ROOT CAUSE, read out of DSWiFi's own lwIP port
 * (https://codeberg.org/blocksds/dswifi, lwip/source/api/... /
 * source/arm9/lwip/sys.c, STABLE-2_2_1_RELEASE as vendored 2026-09-05):
 *
 *     u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
 *     {
 *         ...
 *         uint32_t start_time = wifi_sys_ms_counter;
 *         while (cosema_try_wait(sem) == false) {
 *             uint32_t diff = wifi_sys_ms_counter - start_time;
 *             if (diff >= timeout) return SYS_ARCH_TIMEOUT;
 *             cothread_yield_signal(cosema_to_signal_id(sem));
 *         }
 *     }
 *
 * and sys_arch_mbox_fetch() (what a blocking recv() actually waits on) is
 * the same shape. The deadline check is real and sits right there — but it
 * is only ever RE-EVALUATED after cothread_yield_signal() returns, and that
 * call does not return on a timer: it returns when something SIGNALS the
 * cothread, i.e. when a datagram actually arrives. A socket nothing ever
 * answers signals nothing, so the loop parks in cothread_yield_signal()
 * forever and the deadline above it is never looked at again. That is true
 * of select() and of a plain blocking recvfrom() equally, since both end up
 * inside this same wait — confirmed by isolation on 2026-09-09 (an on-screen
 * step marker: a 20 ms select()+recvfrom() on a socket with nothing arriving
 * wedged the console permanently; the identical call with timeout_ms == 0
 * returned at once). It is invisible in a live session, because there is
 * always a datagram already queued by the time the next call is made, and it
 * is fatal the instant nothing answers — a mistyped address or a server that
 * is not running.
 *
 * A ZERO select() timeout does not go through this wait at all: lwIP treats
 * {0,0} as "poll, do not block" and returns immediately either way. That
 * path is proven in this exact stack, independent of the analysis above:
 * clients/nds/source/main.c's discovery poll and clients/nds/source/
 * screen_session.c's PUMP_WAIT_MS have both called shim/net_bsd.c's
 * apad_udp_recv(sock, ..., 0) every frame since the front-end port, and
 * references/nds/README.md records a 626-packet, 10-second melonDS session
 * built entirely out of that call. THIS FILE'S apad_udp_recv() IS THAT SAME
 * ZERO-TIMEOUT POLL, called in a loop with a real deadline kept OUTSIDE
 * lwIP (by apad_ticks_ms()/apad_time_reached(), which this platform's own
 * 1 kHz timer tick drives independently of DSWiFi's cothread) and a
 * cothread_yield_irq(IRQ_VBLANK) between attempts so the Wi-Fi cothread
 * gets scheduled and has a chance to deliver something.
 *
 * recvfrom(..., MSG_DONTWAIT, ...) was also confirmed to reach the same
 * non-waiting path — lwIP's lwip_recvfrom_udp_raw() sets apiflags
 * NETCONN_DONTBLOCK from MSG_DONTWAIT and calls
 * netconn_recv_udp_raw_netbuf_flags(), which is the try-fetch, not the
 * wait, side of the same code the analysis above reads — but that reading is
 * from the current upstream lwIP source rather than something exercised on
 * this console today, where the zero-timeout select() path already has an
 * emulator-verified track record. Both are documented here; only the second
 * is used, on the "measure, don't guess" rule for a platform nobody can
 * attach a debugger to.
 *
 * timeout_ms < 0 (the shim's documented "block") is implemented as the same
 * loop with no deadline: it yields a frame between polls forever rather than
 * truly blocking, which still cannot wedge the console the way a genuine
 * blocking recv would. Nothing in this codebase calls it that way today.
 *
 * =========================================================================
 * SOCKET REUSE — closesocket() vs close()
 * =========================================================================
 * apad_udp_close() below still calls close(fd), UNCHANGED from net_bsd.c.
 * references/nds/README.md's "Outcome A" measurement already established
 * that libnds routes close() to DSWiFi's posix layer through its
 * file-descriptor table and that the get_website sample relies on the same
 * routing, and the open/close-in-a-loop check this port added (see the
 * platform skill and the report this file shipped with) found socket() and
 * sendto() both still working after 10 open/close cycles on one run. The
 * "a socket per probe fails after 2–3 opens" finding from the front-end port
 * is therefore most likely a symptom of the SAME recv-timeout bug this file
 * fixes (a probe whose reply never came wedged inside the timed recv, which
 * from the outside looked exactly like "stopped working"), not a leak in
 * close(). If a future run finds sockets genuinely not being freed,
 * apad_udp_close() is the one line to change, to closesocket() (also
 * exported by DSWiFi's posix.c) — noted here so that fix does not require
 * re-deriving this reasoning.
 *
 * Constraints inherited from the core, deliberately (verbatim from
 * net_bsd.c, and still true here):
 *   - No malloc. Sockets come from a fixed static pool.
 *   - No stdio. Errors are apad_result codes, not messages.
 *   - IPv4 only.
 *
 * Return conventions, which this file MUST match (shim/net_bsd.c's own list):
 *   apad_net_init         APAD_OK, or negative
 *   apad_udp_open         non-NULL, or NULL
 *   apad_udp_send         bytes sent, or negative
 *   apad_udp_recv         bytes received, 0 on timeout, or negative
 *   apad_udp_set_broadcast APAD_OK, or negative
 *   apad_udp_open_exclusive APAD_OK, or negative
 */

/* __NDS__ IS THE WHOLE FILE'S GUARD, and it is load-bearing, not decoration.
 * clients/3ds/Makefile globs SOURCEDIRS with `../../shim` in it (unlike
 * clients/nds/Makefile, which deliberately avoids that trap with one-line
 * wrapper TUs -- see shared-source-build-traps.md) and therefore compiles
 * EVERY .c file it finds in shim/, this one included, the moment it exists
 * on disk. Confirmed by running the 3DS build after adding this file: it
 * failed on `nds.h: No such file or directory` from devkitARM's own
 * toolchain, which obviously has no BlocksDS headers. __NDS__ is defined
 * automatically by BlocksDS's own ds_arm9.specs (confirmed with `-dM -E`
 * against the pinned image, not assumed) -- nothing here passes it by hand
 * -- so on every OTHER target this whole file compiles down to one harmless
 * typedef and shim/net_bsd.c (unedited, per docs/CONVENTIONS.md) is still what links.
 * Do not "fix" the 3DS break by touching clients/3ds/Makefile -- it is off
 * limits for this task, and the DS Makefile's wrapper-TU approach was
 * already chosen there for the identical reason a decade of shared-C
 * platforms keep rediscovering. */
#if defined(__NDS__)

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
/* htonl/htons/ntohl/ntohs — DSWiFi's lwIP puts these in <arpa/inet.h>, same
 * as every other non-glibc target this shim runs on (see net_bsd.c's own
 * note; libctru is the other example). */
#include <arpa/inet.h>

#include <nds.h>   /* cothread_yield_irq(), IRQ_VBLANK */

#include "atticpad/atticpad.h"

/* One per local port, verbatim from net_bsd.c: a client needs one for the
 * session and one for discovery (main.c's disc_sock()). Four is generous. */
#define APAD_SOCK_POOL 4

struct apad_sock {
    int fd;
    int in_use;
};

static struct apad_sock g_pool[APAD_SOCK_POOL];
static int g_initialised;

int apad_net_init(void)
{
    int i;

    if (g_initialised) {
        return APAD_OK;
    }
    for (i = 0; i < APAD_SOCK_POOL; i++) {
        g_pool[i].fd = -1;
        g_pool[i].in_use = 0;
    }
    g_initialised = 1;
    return APAD_OK;
}

/* Shared body for apad_udp_open() and apad_udp_open_exclusive() — verbatim
 * from net_bsd.c; see that file's own comment for why `exclusive` has to be
 * a bind-time decision and why a fixed port is bound but an ephemeral one is
 * not (libctru's SOC rejects bind-to-port-0; DSWiFi's posix layer has not
 * been proven to accept or reject it, so the same "do not bind unless a
 * caller asked for a specific port" rule is kept rather than risking an
 * unverified difference). */
static int sock_open(uint16_t local_port, int exclusive,
                     struct apad_sock **out)
{
    struct sockaddr_in sa;
    struct apad_sock *s = NULL;
    int fd;
    int on = 1;
    int i;

    *out = NULL;

    if (!g_initialised && apad_net_init() != APAD_OK) {
        return APAD_ERR_STATE;
    }
    for (i = 0; i < APAD_SOCK_POOL; i++) {
        if (!g_pool[i].in_use) {
            s = &g_pool[i];
            break;
        }
    }
    if (s == NULL) {
        return APAD_ERR_BUFFER;   /* fixed pool exhausted, not a port problem */
    }

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return APAD_ERR_STATE;
    }
    if (!exclusive) {
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on,
                         (socklen_t)sizeof on);
    }

    if (local_port != 0u) {
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(local_port);
        if (bind(fd, (const struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) {
            (void)close(fd);
            return APAD_ERR_STATE;
        }
    }

    s->fd = fd;
    s->in_use = 1;
    *out = s;
    return APAD_OK;
}

apad_sock *apad_udp_open(uint16_t local_port)
{
    struct apad_sock *s = NULL;

    (void)sock_open(local_port, 0, &s);
    return s;
}

int apad_udp_open_exclusive(apad_sock **out, uint16_t local_port)
{
    if (out == NULL || local_port == 0u) {
        return APAD_ERR_ARG;
    }
    return sock_open(local_port, 1, out);
}

int apad_udp_set_broadcast(apad_sock *s, int enable)
{
    int on = enable ? 1 : 0;

    if (s == NULL || !s->in_use) {
        return APAD_ERR_ARG;
    }
    if (setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &on,
                   (socklen_t)sizeof on) != 0) {
        return APAD_ERR_STATE;
    }
    return APAD_OK;
}

int apad_udp_send(apad_sock *s, const apad_addr *to, const void *buf, size_t len)
{
    struct sockaddr_in sa;
    ssize_t n;
    uint32_t ip;

    if (s == NULL || !s->in_use || to == NULL || buf == NULL) {
        return APAD_ERR_ARG;
    }
    if (len > APAD_MAX_DATAGRAM) {
        return APAD_ERR_LENGTH;   /* §1: no fragmentation, ever */
    }

    ip = ((uint32_t)to->ip[0] << 24) | ((uint32_t)to->ip[1] << 16)
       | ((uint32_t)to->ip[2] << 8)  | (uint32_t)to->ip[3];

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ip);
    sa.sin_port = htons(to->port);

    do {
        n = sendto(s->fd, buf, len, 0,
                   (const struct sockaddr *)&sa, (socklen_t)sizeof sa);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        return APAD_ERR_STATE;
    }
    return (int)n;
}

/* THE ONE FUNCTION THIS FILE EXISTS FOR. See the file header for why a
 * positive timeout cannot be select()'s job on this stack: the wait below is
 * kept ENTIRELY outside lwIP, using this platform's own 1 kHz tick
 * (time_nds.c) for the deadline and the proven ZERO-timeout select()+
 * recvfrom() (net_bsd.c's own code, unchanged) as the one-shot poll.
 *
 * One poll happens before the first yield, so timeout_ms == 0 costs exactly
 * one non-blocking select()+recvfrom() and returns — no different from
 * calling shim/net_bsd.c's apad_udp_recv(..., 0) today, which this
 * platform's discovery and session-pump code already do every frame. */
int apad_udp_recv(apad_sock *s, apad_addr *from, void *buf, size_t cap,
                  int timeout_ms)
{
    struct sockaddr_in sa;
    socklen_t salen;
    struct timeval tv;
    fd_set rfds;
    ssize_t n;
    int rc;
    uint32_t ip;
    uint32_t deadline = 0u;
    int has_deadline = (timeout_ms > 0);

    if (s == NULL || !s->in_use || buf == NULL || cap == 0u) {
        return APAD_ERR_ARG;
    }
    if (has_deadline) {
        deadline = apad_ticks_ms() + (uint32_t)timeout_ms;
    }

    for (;;) {
        /* A ZERO select() timeout does not go through the broken wait at
         * all (see the file header) — this is the same call net_bsd.c makes
         * unconditionally, just re-issued in a loop here. */
        FD_ZERO(&rfds);
        FD_SET(s->fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        do {
            rc = select(s->fd + 1, &rfds, NULL, NULL, &tv);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            return APAD_ERR_STATE;
        }
        if (rc > 0) {
            memset(&sa, 0, sizeof sa);
            salen = (socklen_t)sizeof sa;
            do {
                n = recvfrom(s->fd, buf, cap, 0, (struct sockaddr *)&sa,
                             &salen);
            } while (n < 0 && errno == EINTR);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* select() said readable, recvfrom() disagreed --
                     * treat as "nothing yet" rather than an error and keep
                     * polling; net_bsd.c does the same for its one-shot
                     * case. */
                } else {
                    return APAD_ERR_STATE;
                }
            } else {
                if (from != NULL) {
                    ip = ntohl(sa.sin_addr.s_addr);
                    apad_addr_set(from,
                                  (uint8_t)((ip >> 24) & 0xFFu),
                                  (uint8_t)((ip >> 16) & 0xFFu),
                                  (uint8_t)((ip >> 8) & 0xFFu),
                                  (uint8_t)(ip & 0xFFu),
                                  ntohs(sa.sin_port));
                }
                return (int)n;
            }
        }

        /* Nothing this pass. timeout_ms == 0: the shim contract is
         * "0 on timeout", and one poll IS the whole timeout window. */
        if (timeout_ms == 0) {
            return 0;
        }
        if (has_deadline && apad_time_reached(apad_ticks_ms(), deadline)) {
            return 0;
        }
        /* timeout_ms < 0 falls through here forever (this shim's documented
         * "block"), yielding a frame between polls rather than truly
         * blocking -- nothing in this codebase calls it that way today. */

        /* THE YIELD IS WHAT ACTUALLY DELIVERS THE DATAGRAM: it lets DSWiFi's
         * lwIP cothread run. Never swiWaitForVBlank() -- see ui.c's header
         * for why that HALTs the ARM9 with the Wi-Fi cothread behind it. */
        cothread_yield_irq(IRQ_VBLANK);
    }
}

void apad_udp_close(apad_sock *s)
{
    if (s == NULL || !s->in_use) {
        return;
    }
    (void)close(s->fd);
    s->fd = -1;
    s->in_use = 0;
}

#else /* !__NDS__ */

/* Every other target compiles shim/net_bsd.c (or its own net_*.c) for these
 * symbols, untouched by this file. This typedef exists only so a strict
 * compiler never sees an empty translation unit. */
typedef int apad_net_nds_not_built_on_this_target;

#endif /* __NDS__ */
