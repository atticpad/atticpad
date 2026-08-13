/* server/host/common/sockcompat.h -- the thin BSD-sockets/Winsock seam
 * shared by server/host/common/webui.h (the local server UI, docs/DESIGN.md §6.3,
 * §6.4 "Host owns ... UI").
 *
 * Extracted during the task that made the Windows host serve the SAME web
 * UI the Linux host already does (see server/host/common/webui.h's own top
 * comment for the full "why one UI, not two" argument). webui.h is BSD
 * sockets almost everywhere -- socket/bind/listen/accept/recv/send are the
 * same call with the same signature on both platforms -- but a handful of
 * spots are genuinely NOT the same call, and this file is exactly those
 * spots and nothing else, so the difference is visible in one place instead
 * of sprinkled through 900+ lines of request handling:
 *
 *   - the socket handle's TYPE: `int` (POSIX, -1 is invalid) vs `SOCKET`
 *     (Winsock, an unsigned handle -- INVALID_SOCKET, not -1, and a `< 0`
 *     comparison against it is a bug that quietly never fires).
 *   - closing it: close() (POSIX) vs closesocket() (Winsock) -- calling the
 *     wrong one compiles on neither platform, so this is not optional.
 *   - making the LISTENING socket non-blocking: fcntl(F_GETFL/F_SETFL,
 *     O_NONBLOCK) (POSIX) vs ioctlsocket(FIONBIO) (Winsock).
 *   - SO_RCVTIMEO/SO_SNDTIMEO's VALUE SHAPE: a `struct timeval` (POSIX) vs a
 *     plain `DWORD` milliseconds count (Winsock) -- passing a struct timeval
 *     to Winsock's setsockopt for this option does not fail loudly, it just
 *     silently sets the wrong timeout (the DWORD Winsock actually reads is
 *     whatever bytes happen to overlay tv_sec on a struct timeval-shaped
 *     buffer), which is the kind of bug that only shows up as "the Windows
 *     UI occasionally hangs" months later. Hidden behind one call so webui.h
 *     never constructs either shape directly.
 *   - MSG_NOSIGNAL: exists only on Linux (suppresses SIGPIPE on send() to a
 *     peer that already closed); Windows never raises SIGPIPE for a socket
 *     write in the first place, so the equivalent flag is 0, not "leave it
 *     off and hope" -- an explicit, documented no-op is safer to read than a
 *     bare 0 appearing at every send() call site.
 *
 * Everything else webui.h needs (socket()/bind()/listen()/accept()/recv()/
 * send()/setsockopt() with an int flag/struct sockaddr_in/socklen_t) is
 * already the same call on both platforms via winsock2.h vs sys/socket.h,
 * so none of it is wrapped here -- wrapping it would just be a second name
 * for the same thing, exactly what this file's own header is warning
 * against ("#ifdef sprinkled through 969 lines" was the thing to avoid, not
 * every BSD socket call needing a wrapper).
 *
 * Header-only, included only by server/host/common/webui.h -- same
 * build-script-constraint reasoning as every other file in this directory
 * (see webui.h's top comment): scripts/build.sh names an exact, fixed list
 * of .c files per target and neither this task nor docs/CONVENTIONS.md permits
 * touching scripts/, so nothing here may become its own translation unit.
 */
#ifndef ATTICPAD_HOST_COMMON_SOCKCOMPAT_H
#define ATTICPAD_HOST_COMMON_SOCKCOMPAT_H

#ifdef _WIN32

/* Same version floor and same "winsock2.h before windows.h" ordering as
 * every other Windows TU in this tree (server/host/windows/main.c,
 * shim/net_winsock.c) -- windows.h otherwise drags in the legacy winsock.h
 * and the two collide with redefinition errors. Guarded rather than bare:
 * server/host/windows/main.c has almost certainly already defined
 * _WIN32_WINNT and WIN32_LEAN_AND_MEAN by the time this file is reached
 * (through common/webui.h), but this header must also stand alone if
 * something else ever includes it first. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET apad_socket_t;
#define APAD_INVALID_SOCKET INVALID_SOCKET
/* Winsock never raises SIGPIPE on send() to a peer that hung up -- there is
 * no equivalent flag to suppress, so this is 0, not a missing feature. */
#define APAD_MSG_NOSIGNAL 0

#else  /* POSIX */

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>   /* struct timeval */
#include <unistd.h>

typedef int apad_socket_t;
#define APAD_INVALID_SOCKET (-1)
#define APAD_MSG_NOSIGNAL MSG_NOSIGNAL

#endif

/* True if `s` is a real, usable socket handle -- NEVER write `s < 0` against
 * an apad_socket_t: SOCKET is unsigned on Windows and that comparison is
 * always false, silently. This is the one comparison webui.h is allowed to
 * make. */
static int apad_sock_valid(apad_socket_t s)
{
    return s != APAD_INVALID_SOCKET;
}

/* close() (POSIX) / closesocket() (Winsock) -- a no-op on an already-invalid
 * handle, so callers never need their own "if valid" guard first. */
static void apad_sock_close(apad_socket_t s)
{
    if (!apad_sock_valid(s)) {
        return;
    }
#ifdef _WIN32
    (void)closesocket(s);
#else
    (void)close(s);
#endif
}

/* Puts the LISTENING socket in non-blocking mode, exactly once, at
 * webui_open() time (server/host/common/webui.h) -- accepted connections
 * stay blocking with a short SO_RCVTIMEO/SO_SNDTIMEO instead, see webui.h's
 * own top comment for why. Returns 0 on success, -1 on failure (the caller
 * treats that the same as any other webui_open() failure: log it, run
 * headless). */
static int apad_sock_set_nonblocking(apad_socket_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    return (ioctlsocket(s, FIONBIO, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/*
 * Force an ACCEPTED socket into BLOCKING mode.
 *
 * This is not symmetry with apad_sock_set_nonblocking() above -- it exists
 * because the two platforms disagree about what accept() returns, and the
 * disagreement is silent:
 *
 *   POSIX: accept() does NOT inherit O_NONBLOCK from the listening socket.
 *          The accepted socket is blocking. (POSIX.1-2017, and accept(2)
 *          says so explicitly.)
 *   Winsock: accept() DOES inherit the listening socket's non-blocking
 *          (FIONBIO) state.
 *
 * webui.h's design depends on the POSIX behaviour -- see its top comment:
 * the LISTENING socket is non-blocking so accept() never stalls the UDP
 * loop, but a socket ONCE ACCEPTED is blocking with a short
 * SO_RCVTIMEO/SO_SNDTIMEO, which is what lets one request be read and
 * answered synchronously without a thread.
 *
 * Left alone on Windows, that whole scheme collapses in a way that looks
 * like a network fault rather than a bug: recv() returns WSAEWOULDBLOCK the
 * instant it is called (SO_RCVTIMEO does nothing on a non-blocking socket),
 * ui_read_request() reads zero bytes and gives up, and closesocket() then
 * runs with the client request still unread in the receive queue -- which
 * makes Windows send an RST rather than a FIN, so the client reports an
 * aborted connection while the server cheerfully reports that it bound and
 * is listening.
 *
 * MEASURED on the real Windows box (2026-08-11), A/B against one binary
 * with this call switched on and off:
 *
 *   - A client that connects and sends immediately: 30/30 requests succeed
 *     EITHER WAY. On loopback the request bytes are already in the receive
 *     buffer by the time accept() returns, so a non-blocking recv() finds
 *     them and nothing looks wrong. This is why the bug hides.
 *   - A client that connects, waits 400 ms, THEN sends: 5/5 succeed with
 *     this call, 5/5 abort without it.
 *
 * So the failure is real, it is not intermittent once you provoke it, and
 * the common case cannot detect it -- which is exactly the combination that
 * would have shipped. It is a LAN-latency-shaped bug: a browser one hop
 * away is far likelier to lose the race than curl on loopback.
 *
 * Called unconditionally on both platforms, not behind an #ifdef: on POSIX
 * it sets the state the socket already has, which costs one fcntl() per
 * request and makes the requirement visible in the code instead of resting
 * on a platform default a reader has to know.
 */
static int apad_sock_set_blocking(apad_socket_t s)
{
#ifdef _WIN32
    u_long mode = 0;
    return (ioctlsocket(s, FIONBIO, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

/* SO_RCVTIMEO/SO_SNDTIMEO in milliseconds -- see this file's top comment for
 * why the VALUE Winsock expects (a bare DWORD) is not the struct timeval
 * POSIX expects, and why silently doing the wrong one is worse than a
 * compile error. Best-effort, like the setsockopt() calls this replaces:
 * failure just means the accepted connection keeps whatever timeout (or
 * lack of one) the OS default gives it, which cannot hang the UDP loop
 * forever because webui.h's request/response size caps still bound how much
 * there is to read or write. */
static void apad_sock_set_timeout_ms(apad_socket_t s, int optname, int ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    (void)setsockopt(s, SOL_SOCKET, optname, (const char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(s, SOL_SOCKET, optname, &tv, sizeof tv);
#endif
}

#endif /* ATTICPAD_HOST_COMMON_SOCKCOMPAT_H */
