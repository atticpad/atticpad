/* net_bsd.c — BSD-sockets shim (docs/DESIGN.md §3).
 *
 * The load-bearing assumption of the whole design is that every target has a
 * BSD-sockets-shaped API: dswifi, soc:U, pspnet, VitaSDK net, libnx bsd,
 * Winsock, the Android NDK. This file is the POSIX/Linux instance of it and
 * the reference the console shims are ported from.
 *
 * Constraints inherited from the core, deliberately:
 *   - No malloc. Sockets come from a fixed static pool. A handheld that runs
 *     out of sockets should fail predictably rather than fragment its heap.
 *   - No stdio. Errors are apad_result codes, not messages.
 *   - IPv4 only. LAN only, and the DS has no IPv6 stack at all.
 *
 * Return conventions, which the console ports MUST match:
 *   apad_net_init         APAD_OK, or negative
 *   apad_udp_open         non-NULL, or NULL
 *   apad_udp_send         bytes sent, or negative
 *   apad_udp_recv         bytes received, 0 on timeout, or negative
 *   apad_udp_set_broadcast APAD_OK, or negative
 *   apad_udp_open_exclusive APAD_OK, or negative (bind-time; see the
 *                          declaration in atticpad.h for why it is not a setter)
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
/* htonl/htons/ntohl/ntohs. glibc pulls these in transitively via
 * <netinet/in.h>, but libctru does NOT — on the 3DS they live here and
 * nowhere else, so omitting this compiles fine on Linux and fails on every
 * console. POSIX puts them in <arpa/inet.h>; this is the portable spelling. */
#include <arpa/inet.h>

#include "atticpad/atticpad.h"

/* One per local port. A client needs one; the server needs one plus, at some
 * point, a discovery socket. Four is generous and costs 16 bytes. */
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

/* Shared body for apad_udp_open() and apad_udp_open_exclusive().
 *
 * `exclusive` is a BIND-TIME decision, which is why it lives here and not in
 * a post-open setter shaped like apad_udp_set_broadcast(): by the time a
 * caller holds an apad_sock*, bind() has already happened and the port's
 * sharing semantics are fixed. There is no portable way to change them after
 * the fact.
 *
 * On POSIX exclusivity is obtained by OMITTING SO_REUSEADDR, not by setting
 * anything. Measured on Linux 7.0 with two UDP sockets on one unicast port:
 *
 *     both sockets SO_REUSEADDR      -> both bind() succeed
 *     neither socket SO_REUSEADDR    -> second bind() fails EADDRINUSE
 *     first without, second with     -> second bind() fails EADDRINUSE
 *
 * So the hijack that net_winsock.c's comment attributes to Windows exists
 * here too for UDP: a second AtticPad server would bind 21100 alongside the
 * first and datagrams would go to only one of them. Omitting the option is
 * what turns that into a clean EADDRINUSE.
 */
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

    /* Explicit IPPROTO_UDP, not 0. Linux happily infers the protocol from
     * SOCK_DGRAM, but neither known-good piece of 3DS code relies on that:
     * references/3ds/sockets/source/sockets.c passes IPPROTO_IP, and the M0
     * spike that ran on real hardware passed IPPROTO_UDP. Being explicit
     * costs nothing and is what every console sample does. */
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return APAD_ERR_STATE;
    }
    /* Discovery listens on a well-known port and the server may restart while
     * a socket lingers; SO_REUSEADDR keeps that from being a two-minute wait. */
    /* Return deliberately ignored: libctru's SOC implements only a subset of
     * socket options and the devkitPro sample never calls setsockopt at all.
     * A console that rejects this must still get a usable socket. */
    if (!exclusive) {
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on,
                         (socklen_t)sizeof on);
    }

    /* Bind ONLY when a specific local port was asked for.
     *
     * Binding to port 0 to request an ephemeral port works on Linux, but
     * libctru's SOC rejects it outright — measured on real 3DS hardware:
     *
     *     bind(fd, ANY, port 0)     -> rc=-1 errno=22 (Invalid argument)
     *     bind(fd, ANY, port 21100) -> rc=0  errno=0  (Success)
     *
     * A UDP socket does not need an explicit bind to receive replies: the
     * first sendto() auto-assigns a source port on every platform we target,
     * and that is the port the peer answers to. So when the caller does not
     * care which port it gets, the correct portable action is to not bind at
     * all — which is also one fewer syscall on the constrained targets.
     *
     * Callers that DO need a fixed port (the server on 21100, discovery)
     * pass it explicitly and get a real bind. */
    if (local_port != 0u) {
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(local_port);
        if (bind(fd, (const struct sockaddr *)&sa, (socklen_t)sizeof sa) != 0) {
            (void)close(fd);
            /* Under `exclusive` this is the signal the caller wants: on a
             * system where socket() worked, a failed bind on a fixed port
             * means somebody else already holds it. */
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

    /* Assemble the address byte by byte and hand it to htonl, so this line
     * reads the same on every platform regardless of host endianness. */
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

    if (s == NULL || !s->in_use || buf == NULL || cap == 0u) {
        return APAD_ERR_ARG;
    }

    if (timeout_ms >= 0) {
        do {
            FD_ZERO(&rfds);
            FD_SET(s->fd, &rfds);
            tv.tv_sec  = (time_t)(timeout_ms / 1000);
            tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
            rc = select(s->fd + 1, &rfds, NULL, NULL, &tv);
        } while (rc < 0 && errno == EINTR);

        if (rc < 0) {
            return APAD_ERR_STATE;
        }
        if (rc == 0) {
            return 0;   /* timeout: no datagram, not an error */
        }
    }

    memset(&sa, 0, sizeof sa);
    salen = (socklen_t)sizeof sa;
    do {
        n = recvfrom(s->fd, buf, cap, 0, (struct sockaddr *)&sa, &salen);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
    return (int)n;
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
