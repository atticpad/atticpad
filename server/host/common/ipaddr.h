/* server/host/common/ipaddr.h -- own-address enumeration, shared by both
 * hosts (docs/PROTOCOL.md §7 tier 3: "the server MUST display its own IP
 * prominently at all times") and by §10.3's QR/pairing-URI address
 * selector (server/host/common/webui.h, assets.h).
 *
 * ONE interface, TWO implementations, per this task's own instruction: the
 * types (host_addr_kind, host_own_addr), host_addr_kind_name() and
 * host_pick_default_addr() are identical logic on both platforms and are
 * written once, below. host_enumerate_own_ipv4() -- the one genuine fork,
 * getifaddrs(3) vs GetAdaptersAddresses() -- is a single function with two
 * complete bodies behind #ifdef _WIN32, not one body with POSIX and Win32
 * calls interleaved: the two loops share no code today (different APIs,
 * different iteration shapes) and interleaving them would only make both
 * harder to read for no shared benefit.
 *
 * This file used to be server/host/linux/ipaddr.h, Linux-only; the Windows
 * host's own address enumeration lived instead as a private, unshared
 * print_own_addresses() inside server/host/windows/main.c. Moved here (and
 * given the classification host_addr_kind already provided on Linux) so
 * BOTH hosts' §7 tier-3 banner AND the §10.3 QR address selector go through
 * one enumerator instead of two independently-written GetAdaptersAddresses/
 * getifaddrs loops that could silently drift. server/host/windows/main.c's
 * print_own_addresses() now calls host_enumerate_own_ipv4() below rather
 * than querying the adapter list itself.
 *
 * Header-only, included by server/host/common/webui.h, server/host/linux/
 * mdns.h, and both hosts' main.c -- same build-script-constraint reasoning
 * as every other header in this directory (see webui.h's top comment).
 */
#ifndef ATTICPAD_HOST_COMMON_IPADDR_H
#define ATTICPAD_HOST_COMMON_IPADDR_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdlib.h>   /* realloc/free -- the GetAdaptersAddresses retry loop */
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define HOST_MAX_OWN_ADDRS 16

/* Not IFNAMSIZ (net/if.h, POSIX-only -- unavailable on Windows without
 * dragging in another header for one constant): 32 bytes comfortably fits
 * both a real Linux interface name ("eth0", "wlan0", "docker0", IFNAMSIZ
 * is 16) and the short ASCII label the Windows implementation derives below
 * (it does not read an adapter's FriendlyName -- see that implementation's
 * comment for why). */
#define HOST_IFACE_LEN 32

/*
 * §10.3's QR/URI can only ever encode ONE address, and a machine can
 * legitimately have several: a real LAN address, zero or more container/
 * virtual/VPN-adjacent adapters, and a Tailscale address, all non-loopback
 * and all UP. Picking the first one enumerated is an accident of kernel or
 * Winsock ordering, not a choice -- on a box with Docker or a VPN client
 * installed that is frequently an address a phone on the LAN cannot route
 * to at all, and the failure mode is indistinguishable from a broken
 * encoder ("the QR doesn't work"). This enum is what turns "accident" into
 * "choice": every address is still reported (nothing is hidden, see
 * host_enumerate_own_ipv4() below), but each carries a classification a UI
 * can use to rank and label them (server/host/common/webui.h's callers,
 * assets.h's address selector).
 *
 *   HOST_ADDR_LAN       -- the default candidate. Not "guaranteed
 *                          reachable" (this host cannot know what a given
 *                          phone can route to), just "not one of the two
 *                          known-bad patterns below".
 *   HOST_ADDR_TAILSCALE -- correct for a phone that is ALSO on the tailnet,
 *                          useless for one that is only on the LAN. Neither
 *                          hidden (a tailnet phone needs to find it) nor
 *                          preferred by default (most phones testing over
 *                          LAN are not on the tailnet) -- offered.
 *   HOST_ADDR_VIRTUAL   -- container bridges, VPN/tunnel-shaped adapters,
 *                          and similar interfaces that (almost) never have
 *                          an actual phone on the other end. Reported for
 *                          completeness (§7 tier 3 says every address, not
 *                          just the good ones) but ranked last and never
 *                          the default.
 *
 * The classification is a heuristic over interface identity and address
 * range, stated here rather than hidden in a scoring function so it is easy
 * to extend: nothing about it is protocol-governed, unlike everything in
 * core/. The TWO signals differ per platform (see each implementation
 * below) because the identifying information each OS actually hands back
 * differs, but the CGNAT address-range check (Tailscale's own allocator,
 * 100.64.0.0/10, RFC 6598) is shared: it is a sturdier cross-platform
 * signal than any adapter name, since Tailscale draws from the same range
 * wherever it runs.
 */
typedef enum {
    HOST_ADDR_LAN = 0,
    HOST_ADDR_TAILSCALE,
    HOST_ADDR_VIRTUAL
} host_addr_kind;

typedef struct {
    char           iface[HOST_IFACE_LEN];
    char           ip[16];        /* dotted quad + NUL, e.g. "255.255.255.255" */
    uint8_t        raw[4];        /* the same address, network order -- for
                                    * building an apad_addr directly, no
                                    * string round trip through inet_pton()  */
    host_addr_kind kind;

    /* This interface's subnet-directed broadcast address (e.g.
     * 192.168.1.255 for a /24 on 192.168.1.0), network order -- feeds
     * apad_server_cfg.broadcast_addrs (apadserver.h), docs/PROTOCOL.md §7's
     * "a subnet-directed broadcast it can identify". `has_bcast` is 0 when
     * this interface has none (not IFF_BROADCAST, e.g. a point-to-point
     * link) or on a platform that does not fill this in yet -- see each
     * body below; the POSIX one reads ifa_broadaddr from the SAME
     * getifaddrs() walk this function already does, the Windows one leaves
     * it 0 for every entry (GetAdaptersAddresses wiring is a follow-up, not
     * done here -- server/host/windows/main.c passes an empty
     * broadcast_addrs list, which is exactly today's behaviour). */
    uint8_t        bcast[4];
    int            has_bcast;
} host_own_addr;

static const char *host_addr_kind_name(host_addr_kind k)
{
    switch (k) {
    case HOST_ADDR_TAILSCALE: return "tailscale";
    case HOST_ADDR_VIRTUAL:   return "virtual";
    case HOST_ADDR_LAN:       /* fall through */
    default:                  return "lan";
    }
}

/* Tailscale's own address allocator draws from 100.64.0.0/10 (the shared
 * CGNAT range, RFC 6598) wherever it runs, which is a sturdier signal than
 * an adapter's name alone (a Windows/macOS Tailscale interface is not
 * necessarily named anything containing "tailscale") -- shared by both
 * implementations below; either platform's own name-based signal is offered
 * in ADDITION to this, never instead of it. `raw` is 4 bytes, network byte
 * order, exactly what both host_enumerate_own_ipv4() bodies already extract
 * a `struct sockaddr_in`'s sin_addr into. */
static int host_ip_is_cgnat(const uint8_t raw[4])
{
    uint32_t h = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                 ((uint32_t)raw[2] << 8)  |  (uint32_t)raw[3];
    /* 100.64.0.0/10 == 100.64.0.0 through 100.127.255.255 */
    return (h & 0xFFC00000u) == 0x64400000u;
}

#ifdef _WIN32

/* Windows implementation. Deliberately does NOT read an adapter's
 * FriendlyName or Description (both wide strings, IP_ADAPTER_ADDRESSES):
 * this host was swept for non-ASCII deliberately (docs/CONVENTIONS.md, this task's
 * brief) -- a FriendlyName can legitimately contain any Unicode codepoint a
 * user or a driver author chose, and converting it correctly would need
 * WideCharToMultiByte plus a fallback for the conversion failing, all to
 * populate a value used only as a label. Skipped entirely rather than
 * risking a mangled byte reaching a terminal or a JSON string: `iface`
 * below is a short, hand-picked ASCII classification of the adapter's
 * IF_TYPE instead (RFC-defined constants, ifdef.h), and `kind` -- not
 * `iface` -- is what a UI actually branches or ranks on. */
static host_addr_kind host_classify_addr(IFTYPE if_type, const uint8_t raw[4])
{
    if (host_ip_is_cgnat(raw)) {
        return HOST_ADDR_TAILSCALE;
    }
    /* IF_TYPE_TUNNEL: WireGuard and many VPN clients present a TUN-shaped
     * adapter. IF_TYPE_PPP: the VPN clients that do not. Coarser than the
     * Linux implementation's interface-name-prefix list below (no name is
     * read at all, per this function's own comment), but catches the
     * common "this address is a VPN/tunnel, not the real LAN" case without
     * needing one. */
    if (if_type == IF_TYPE_TUNNEL || if_type == IF_TYPE_PPP) {
        return HOST_ADDR_VIRTUAL;
    }
    return HOST_ADDR_LAN;
}

static const char *host_iftype_label(IFTYPE if_type)
{
    switch (if_type) {
    case IF_TYPE_ETHERNET_CSMACD: return "ethernet";
    case IF_TYPE_IEEE80211:       return "wifi";
    case IF_TYPE_TUNNEL:          return "tunnel";
    case IF_TYPE_PPP:             return "ppp";
    default:                      return "other";
    }
}

/* Windows twin of the POSIX body below: every up, non-loopback IPv4
 * address, classified. Same GetAdaptersAddresses retry pattern
 * server/host/windows/main.c's print_own_addresses() used to run itself
 * (now a thin caller of this function -- see that function's own comment).
 * Returns the count found (0 on any failure or an interfaceless box --
 * never negative, since "no address" is a state a UI must render, not an
 * error the caller has to branch on separately). */
static size_t host_enumerate_own_ipv4(host_own_addr *out, size_t max)
{
    ULONG size = 15360;   /* Microsoft's own recommended starting size */
    IP_ADAPTER_ADDRESSES *buf = NULL;
    IP_ADAPTER_ADDRESSES *ad;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    int attempt;
    size_t n = 0;

    for (attempt = 0; attempt < 3; attempt++) {
        IP_ADAPTER_ADDRESSES *nb = realloc(buf, size);
        if (nb == NULL) {
            free(buf);
            return 0;
        }
        buf = nb;
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST |
                                  GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, buf, &size);
        if (rc != ERROR_BUFFER_OVERFLOW) {
            break;
        }
        /* size was updated in place to what is actually needed; loop and
         * retry with the bigger buffer. */
    }
    if (rc != NO_ERROR) {
        free(buf);
        return 0;
    }

    for (ad = buf; ad != NULL && n < max; ad = ad->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *ua;

        if (ad->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (ua = ad->FirstUnicastAddress; ua != NULL && n < max;
             ua = ua->Next) {
            const struct sockaddr_in *sin;
            const unsigned char *b;

            if (ua->Address.lpSockaddr == NULL ||
                ua->Address.lpSockaddr->sa_family != AF_INET) {
                continue;   /* IPv6 or unset; the wire is IPv4-only */
            }
            sin = (const struct sockaddr_in *)
                (const void *)ua->Address.lpSockaddr;
            b = (const unsigned char *)&sin->sin_addr;

            out[n].raw[0] = b[0];
            out[n].raw[1] = b[1];
            out[n].raw[2] = b[2];
            out[n].raw[3] = b[3];
            (void)snprintf(out[n].ip, sizeof out[n].ip, "%u.%u.%u.%u",
                          (unsigned)b[0], (unsigned)b[1],
                          (unsigned)b[2], (unsigned)b[3]);
            (void)snprintf(out[n].iface, sizeof out[n].iface, "%s",
                          host_iftype_label(ad->IfType));
            out[n].kind = host_classify_addr(ad->IfType, out[n].raw);
            /* Not filled in yet -- see host_own_addr::has_bcast's doc
             * comment. GetAdaptersAddresses does not hand back a broadcast
             * address directly; deriving one needs the unicast entry's
             * OnLinkPrefixLength (Vista+) applied to `raw`, which is a
             * follow-up, not done here. */
            out[n].has_bcast = 0;
            n++;
        }
    }
    free(buf);
    return n;
}

#else  /* POSIX */

/* True if `prefix` is a case-sensitive prefix of `s` -- interface names on
 * Linux are already lower-case-by-convention (eth0, wlan0, docker0,
 * tailscale0, ...), so no case-folding is attempted. */
static int host_str_starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static host_addr_kind host_classify_addr(const char *iface,
                                         const uint8_t raw[4])
{
    static const char *const virtual_prefixes[] = {
        "docker", "br-", "veth", "virbr", "vnet", "tun", "tap", "dummy",
        "cni", "flannel", "kube-ipvs", "podman", "lxcbr", "lxdbr", "wg"
    };
    size_t i;

    if (host_str_starts_with(iface, "tailscale") || host_ip_is_cgnat(raw)) {
        return HOST_ADDR_TAILSCALE;
    }
    for (i = 0; i < sizeof virtual_prefixes / sizeof virtual_prefixes[0]; i++) {
        if (host_str_starts_with(iface, virtual_prefixes[i])) {
            return HOST_ADDR_VIRTUAL;
        }
    }
    return HOST_ADDR_LAN;
}

/*
 * Every UP, non-loopback IPv4 address on this machine, filtered the same
 * way as the Windows implementation's IfOperStatusUp / IF_TYPE_SOFTWARE_
 * LOOPBACK pair above: a box can have Ethernet and Wi-Fi both live at once,
 * and §7 tier 3 exists precisely so a human picks the address the client
 * can actually reach rather than the server guessing one. getifaddrs(3) is
 * queried fresh on every call rather than cached at startup, because "at
 * all times" means the UI's answer stays correct if a laptop's Wi-Fi
 * reassociates or a USB NIC comes up mid-run, not just correct at the
 * moment the process started. Returns the count found (0 on any
 * getifaddrs() failure or an interfaceless box -- never negative).
 */
static size_t host_enumerate_own_ipv4(host_own_addr *out, size_t max)
{
    struct ifaddrs *ifs, *p;
    size_t n = 0;

    if (getifaddrs(&ifs) != 0) {
        return 0;
    }
    for (p = ifs; p != NULL && n < max; p = p->ifa_next) {
        const struct sockaddr_in *sin;
        const unsigned char *b;

        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((p->ifa_flags & IFF_UP) == 0u ||
            (p->ifa_flags & IFF_LOOPBACK) != 0u) {
            continue;
        }
        sin = (const struct sockaddr_in *)(const void *)p->ifa_addr;
        b = (const unsigned char *)&sin->sin_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, out[n].ip,
                      sizeof out[n].ip) == NULL) {
            continue;
        }
        out[n].raw[0] = b[0];
        out[n].raw[1] = b[1];
        out[n].raw[2] = b[2];
        out[n].raw[3] = b[3];
        (void)snprintf(out[n].iface, sizeof out[n].iface, "%s", p->ifa_name);
        out[n].kind = host_classify_addr(p->ifa_name, out[n].raw);

        /* ifa_broadaddr is a member of the SAME union as ifa_dstaddr
         * (point-to-point peer address) in <ifaddrs.h>; it is only meaningful
         * when IFF_BROADCAST is set, which a P2P/tunnel interface generally
         * does not have -- checking the flag rather than just "is it
         * non-NULL" avoids reading a P2P destination as if it were a
         * broadcast address. */
        out[n].has_bcast = 0;
        if ((p->ifa_flags & IFF_BROADCAST) != 0u && p->ifa_broadaddr != NULL &&
            p->ifa_broadaddr->sa_family == AF_INET) {
            const struct sockaddr_in *bsin =
                (const struct sockaddr_in *)(const void *)p->ifa_broadaddr;
            const unsigned char *bb = (const unsigned char *)&bsin->sin_addr;
            out[n].bcast[0] = bb[0];
            out[n].bcast[1] = bb[1];
            out[n].bcast[2] = bb[2];
            out[n].bcast[3] = bb[3];
            out[n].has_bcast = 1;
        }
        n++;
    }
    freeifaddrs(ifs);
    return n;
}

#endif /* _WIN32 */

/* Picks the address a fresh QR/URI should default to: the first
 * HOST_ADDR_LAN entry, falling back to the first HOST_ADDR_TAILSCALE and
 * then the first HOST_ADDR_VIRTUAL entry, in that order, so SOME address is
 * always chosen when the list is non-empty. Identical on both platforms --
 * it only ever looks at `kind`. Returns the index into `addrs`, or
 * (size_t)-1 if naddr == 0. */
static size_t host_pick_default_addr(const host_own_addr *addrs, size_t naddr)
{
    host_addr_kind want;

    for (want = HOST_ADDR_LAN; ; want = (host_addr_kind)(want + 1)) {
        size_t i;
        for (i = 0; i < naddr; i++) {
            if (addrs[i].kind == want) {
                return i;
            }
        }
        if (want == HOST_ADDR_VIRTUAL) {
            break;
        }
    }
    return (naddr > 0u) ? 0u : (size_t)-1;
}

#endif /* ATTICPAD_HOST_COMMON_IPADDR_H */
