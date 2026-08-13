/* net_winsock.c — Winsock2 shim (docs/DESIGN.md §3).
 *
 * The Windows instance of the same BSD-sockets-shaped API net_bsd.c
 * implements for POSIX. It exists for the Windows HOST (docs/DESIGN.md §6.4: the
 * server is a sans-IO library plus a thin host, and the host owns the
 * socket); libapadserver never touches this file.
 *
 * docs/DESIGN.md §4's layout comment on net_bsd.c lists "3DS, PSP, Vita, Switch,
 * Linux, macOS, Android" — Windows is deliberately absent. Winsock is
 * BSD-*shaped*, not BSD-compatible: INVALID_SOCKET is not -1, errors are not
 * in errno, and closesocket() is not close(). A separate file is the intended
 * design, not a fallback. Everything else — the fixed pool, the bind-only-
 * when-asked rule, the byte-by-byte address assembly — mirrors net_bsd.c
 * line for line so the two can be diffed.
 *
 * Constraints inherited from the core, deliberately:
 *   - No malloc. Sockets come from a fixed static pool.
 *   - No stdio. Errors are apad_result codes, not messages.
 *   - IPv4 only. LAN only, and the DS has no IPv6 stack at all.
 *
 * Return conventions, IDENTICAL to net_bsd.c and to the header. The one that
 * is easiest to get wrong is apad_udp_recv: 0 means "timed out, no datagram",
 * NOT an error. A port that returns negative there looks like packet loss.
 *   apad_net_init          APAD_OK, or negative. Idempotent.
 *   apad_udp_open          non-NULL, or NULL
 *   apad_udp_send          bytes sent, or negative. Never partial.
 *   apad_udp_recv          bytes received, 0 on timeout, or negative
 *   apad_udp_set_broadcast APAD_OK, or negative
 *   apad_udp_open_exclusive APAD_OK, or negative (bind-time; see the
 *                          declaration in atticpad.h for why it is not a setter)
 *
 * Build: x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Werror -pedantic,
 * link with -lws2_32. mingw headers are lowercase: <windows.h>, never
 * <Windows.h> — the capital spelling only works on a case-insensitive
 * filesystem and fails when cross-compiling from Linux.
 */

/* Guarded so that a build which globs every .c in shim/ for a non-Windows
 * target (the 3DS Makefile does exactly that, filtering out only
 * time_posix.c) compiles this to an empty object instead of failing on
 * <winsock2.h>. The real fix is for that Makefile to filter this file out
 * the way it already filters time_posix.c; this guard only keeps a blind
 * platform from breaking while it waits. */
#if defined(_WIN32)

/* GetTickCount64 and the Vista-era Winsock declarations need a version floor.
 * 0x0601 = Windows 7, which is well below anything this will run on. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

/* winsock2.h MUST precede windows.h. windows.h otherwise drags in the
 * original winsock.h and the two collide with redefinition errors. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>          /* SIO_UDP_CONNRESET */
#include <windows.h>

#include <limits.h>
#include <string.h>

#include "atticpad/atticpad.h"

/* Present since Windows 2000 but not in every mingw header generation. The
 * value is fixed by the OS ABI, so defining it when absent is safe. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* One per local port. A client needs one; the server needs one plus, at some
 * point, a discovery socket. Four is generous. */
#define APAD_SOCK_POOL 4

struct apad_sock {
    SOCKET fd;      /* SOCKET is an UNSIGNED handle. The idle value is
                     * INVALID_SOCKET (~0), never -1, and the failure test is
                     * `== INVALID_SOCKET`, never `< 0`. A pool copied from
                     * net_bsd.c without changing this is silently broken:
                     * `fd = -1` and `fd < 0` both "work" as bit patterns on
                     * a 32-bit SOCKET and stop working on 64-bit Windows,
                     * where SOCKET is 64-bit and -1 is not the sentinel the
                     * comparison expects. */
    int    in_use;
};

static struct apad_sock g_pool[APAD_SOCK_POOL];
static int g_initialised;

/* Idempotent, like net_bsd.c's guard — but here it also guards WSAStartup,
 * which is refcounted by Winsock and must be paired with WSACleanup. The shim
 * contract has no teardown entry point, so we never call WSACleanup: the
 * process exit does it. Calling it from apad_udp_close() would tear down
 * Winsock while other sockets in the pool were still open. */
int apad_net_init(void)
{
    WSADATA wsa;
    int i;

    if (g_initialised) {
        return APAD_OK;
    }
    /* WSAStartup returns the error code directly; it does NOT set the
     * last-error slot, so WSAGetLastError() is useless here. */
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return APAD_ERR_STATE;
    }
    for (i = 0; i < APAD_SOCK_POOL; i++) {
        g_pool[i].fd = INVALID_SOCKET;
        g_pool[i].in_use = 0;
    }
    g_initialised = 1;
    return APAD_OK;
}

/* Shared body for apad_udp_open() and apad_udp_open_exclusive(). Mirrors
 * net_bsd.c's sock_open() so the two files stay diffable.
 *
 * `exclusive` is a BIND-TIME decision on Windows even more firmly than on
 * POSIX: SO_EXCLUSIVEADDRUSE is documented as having to be set BEFORE bind(),
 * and setting it afterwards fails. That is why this is folded into open
 * rather than exposed as a post-open setter like apad_udp_set_broadcast().
 *
 * SO_EXCLUSIVEADDRUSE is the option the NOTE below apad_net_init() asked for.
 * It is set INSTEAD OF SO_REUSEADDR, never alongside it: the two are
 * contradictory, and leaving SO_REUSEADDR on is precisely what lets a second
 * process hijack a bound port on Windows.
 */
static int sock_open(uint16_t local_port, int exclusive,
                     struct apad_sock **out)
{
    struct sockaddr_in sa;
    struct apad_sock *s = NULL;
    SOCKET fd;
    BOOL behave = FALSE;
    DWORD nret = 0;
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

    /* Explicit IPPROTO_UDP, not 0, matching net_bsd.c. */
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        return APAD_ERR_STATE;
    }

    /* SO_REUSEADDR: same reason as net_bsd.c (discovery on a well-known port
     * across a server restart). setsockopt on Windows takes the option value
     * as `const char *`, not `const void *`. Return ignored, as in net_bsd.c —
     * a stack that rejects the option must still yield a usable socket.
     *
     * NOTE for whoever wires up discovery: Windows SO_REUSEADDR is NOT the
     * POSIX one. It permits a genuine hijack of a bound port rather than just
     * reuse of a lingering one. If two AtticPad servers run on one machine,
     * the second will bind 21100 successfully and steal traffic instead of
     * failing. SO_EXCLUSIVEADDRUSE is the Windows answer to that, and it is a
     * host-policy decision, not a shim decision — so it is not made here. */
    if (exclusive) {
        /* Return NOT ignored, unlike SO_REUSEADDR above: a caller that asked
         * for exclusivity and did not get it must be told, because the whole
         * point is to fail loudly instead of silently sharing the port. */
        if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       (const char *)&on, (int)sizeof on) != 0) {
            (void)closesocket(fd);
            return APAD_ERR_STATE;
        }
    } else {
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                         (const char *)&on, (int)sizeof on);
    }

    /* SIO_UDP_CONNRESET = FALSE. Windows-only, and there is no POSIX
     * equivalent to mirror. By default, when a datagram we sent provokes an
     * ICMP port-unreachable, Winsock latches that onto the UDP socket and the
     * NEXT recvfrom() fails with WSAECONNRESET — a "connection reset" on a
     * connectionless socket, caused by a peer that is already gone. Left
     * alone, a client quitting makes the server's next recv return an error
     * for no reason. Turning it off restores the POSIX behaviour net_bsd.c
     * has, which is to ignore the ICMP entirely.
     * Return ignored: on the rare stack that lacks the ioctl the socket is
     * still usable, and apad_udp_recv() below also tolerates WSAECONNRESET. */
    (void)WSAIoctl(fd, SIO_UDP_CONNRESET, &behave, (DWORD)sizeof behave,
                   NULL, 0, &nret, NULL, NULL);

    /* Bind ONLY when a specific local port was asked for — same rule and same
     * reasoning as net_bsd.c (libctru's SOC rejects bind to port 0; the first
     * sendto auto-assigns a source port everywhere). Kept identical here even
     * though Windows would accept bind(0), because the two files must be
     * diffable and because it is one fewer syscall either way. */
    if (local_port != 0u) {
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(local_port);
        if (bind(fd, (const struct sockaddr *)&sa, (int)sizeof sa) != 0) {
            (void)closesocket(fd);      /* not close() */
            /* Under `exclusive` this is the signal the caller wants: another
             * process already holds the port. With SO_EXCLUSIVEADDRUSE set,
             * Windows reports WSAEADDRINUSE here instead of letting the bind
             * succeed and stealing the traffic. */
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
    return s;   /* NULL on any failure — unchanged contract */
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
    if (setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST,
                   (const char *)&on, (int)sizeof on) != 0) {
        return APAD_ERR_STATE;
    }
    return APAD_OK;
}

int apad_udp_send(apad_sock *s, const apad_addr *to, const void *buf, size_t len)
{
    struct sockaddr_in sa;
    int n;
    uint32_t ip;

    if (s == NULL || !s->in_use || to == NULL || buf == NULL) {
        return APAD_ERR_ARG;
    }
    if (len > APAD_MAX_DATAGRAM) {
        return APAD_ERR_LENGTH;   /* §1: no fragmentation, ever */
    }

    /* Assemble the address byte by byte and hand it to htonl, so this line
     * reads the same on every platform regardless of host endianness. */
    ip = ((uint32_t)to->ip[0] << 24) | ((uint32_t)to->ip[1] << 16)
       | ((uint32_t)to->ip[2] << 8)  | (uint32_t)to->ip[3];

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(ip);
    sa.sin_port = htons(to->port);

    /* sendto takes an int length on Windows, and returns int rather than
     * ssize_t. len is already bounded by APAD_MAX_DATAGRAM above, so the
     * narrowing cast cannot truncate. Winsock has no restartable-syscall
     * concept, but WSAEINTR still exists (it is raised by a WSACancelBlocking
     * call), so the retry loop is kept rather than assumed away. */
    do {
        n = sendto(s->fd, (const char *)buf, (int)len, 0,
                   (const struct sockaddr *)&sa, (int)sizeof sa);
    } while (n == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);

    if (n == SOCKET_ERROR) {
        return APAD_ERR_STATE;
    }
    return n;
}

int apad_udp_recv(apad_sock *s, apad_addr *from, void *buf, size_t cap,
                  int timeout_ms)
{
    struct sockaddr_in sa;
    struct timeval tv;
    fd_set rfds;
    int salen;
    int caplen;
    int n;
    int rc;
    int err;
    uint32_t ip;

    if (s == NULL || !s->in_use || buf == NULL || cap == 0u) {
        return APAD_ERR_ARG;
    }

    /* recvfrom takes an int buffer length. cap is a size_t supplied by the
     * caller, so clamp rather than truncate; a short read is correct
     * behaviour for a datagram that fits, and APAD_MAX_DATAGRAM is orders of
     * magnitude below INT_MAX anyway. */
    caplen = (cap > (size_t)INT_MAX) ? INT_MAX : (int)cap;

    if (timeout_ms >= 0) {
        do {
            FD_ZERO(&rfds);
            FD_SET(s->fd, &rfds);
            tv.tv_sec  = (long)(timeout_ms / 1000);
            tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
            /* First argument is ignored on Windows (fd_set is an array of
             * SOCKETs, not a bitmask indexed by descriptor), but it must
             * still be passed. 0 is the conventional value. WSAPoll would
             * also work; select keeps this file diffable against net_bsd.c. */
            rc = select(0, &rfds, NULL, NULL, &tv);
        } while (rc == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);

        if (rc == SOCKET_ERROR) {
            return APAD_ERR_STATE;
        }
        if (rc == 0) {
            return 0;   /* timeout: no datagram, not an error */
        }
    }

    memset(&sa, 0, sizeof sa);
    salen = (int)sizeof sa;     /* int here, not socklen_t */
    do {
        n = recvfrom(s->fd, (char *)buf, caplen, 0,
                     (struct sockaddr *)&sa, &salen);
    } while (n == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);

    if (n == SOCKET_ERROR) {
        err = WSAGetLastError();
        /* WSAEWOULDBLOCK: same as EAGAIN/EWOULDBLOCK in net_bsd.c — nothing
         * to read, which is not an error.
         * WSAECONNRESET: the stale-ICMP case described in apad_udp_open. It
         * refers to a datagram we already sent, not to the one we are trying
         * to read, so it is "no datagram this call" and not a failure. The
         * ioctl above normally prevents it; this is the belt to that braces.
         * WSAEMSGSIZE: a datagram larger than the buffer. Winsock discards
         * the remainder and reports an error where POSIX would return the
         * truncated length. §1 forbids datagrams above APAD_MAX_DATAGRAM, so
         * anything hitting this is not ours; drop it like a timeout rather
         * than tearing the session down over a stray packet. */
        if (err == WSAEWOULDBLOCK || err == WSAECONNRESET || err == WSAEMSGSIZE) {
            return 0;
        }
        return APAD_ERR_STATE;
    }

    if (from != NULL) {
        ip = ntohl(sa.sin_addr.s_addr);
        apad_addr_set(from,
                      (uint8_t)((ip >> 24) & 0xFFu),
                      (uint8_t)((ip >> 16) & 0xFFu),
                      (uint8_t)((ip >> 8) & 0xFFu),
                      (uint8_t)(ip & 0xFFu),
                      ntohs(sa.sin_port));
    }
    return n;
}

void apad_udp_close(apad_sock *s)
{
    if (s == NULL || !s->in_use) {
        return;
    }
    (void)closesocket(s->fd);       /* not close() */
    s->fd = INVALID_SOCKET;         /* not -1 */
    s->in_use = 0;
}

#else  /* !_WIN32 */

/* ISO C forbids an empty translation unit and -pedantic enforces it, so the
 * non-Windows build of this file needs one declaration that emits nothing. */
typedef int apad_net_winsock_unused_on_this_platform;

#endif /* _WIN32 */
