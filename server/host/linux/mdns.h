/* server/host/linux/mdns.h -- docs/PROTOCOL.md §7 tier 1: an ADVERTISE-ONLY
 * mDNS responder for `_atticpad._udp.local` (docs/DESIGN.md §5.5, D4).
 *
 * §7 states three obligations and this file exists to satisfy exactly those
 * three, and nothing else:
 *
 *   1. advertise the service `_atticpad._udp.local`;
 *   2. carry TXT records for name, version and free slots;
 *   3. if the 5353 bind fails, tier 1 is disabled and the server UI MUST
 *      say so.
 *
 * WHY THIS IS HAND-WRITTEN AND NOT VENDORED (docs/DESIGN.md D4 says "vendor a
 * responder rather than write one"; the deviation is deliberate and is
 * D4's own stated intent -- "do not write a full mDNS stack"):
 *
 *   - The obvious candidate, mjansson/mdns (single header, public domain,
 *     ~1620 lines), is a *bidirectional* DNS-SD library: roughly half of it
 *     is the query/discovery/record-parsing path -- mdns_query_send(),
 *     mdns_discovery_send(), mdns_record_parse_{ptr,srv,a,aaaa,txt}(),
 *     the string table for name compression. This task's CONSTRAINTS say
 *     "Advertise-only. Do NOT implement browsing, conflict resolution, or a
 *     resolver." Vendoring it would put all of that in the tree with a rule
 *     that it must never be called -- the opposite of the ViGEmClient case,
 *     where every vendored line is on the one path we use.
 *   - It would not save the glue: upstream's own responder example
 *     (mdns.c, service_callback()) is ~230 lines of user code on top of the
 *     header to answer PTR/SRV/TXT/A for one service. That is most of what
 *     is below. The header replaces the record *encoder*, not the responder.
 *   - It cannot satisfy obligation 3 as written. mdns_socket_setup_ipv4()
 *     does IP_ADD_MEMBERSHIP and bind() inside one function and returns -1
 *     for either, so "the 5353 bind failed" and "the multicast join failed"
 *     are indistinguishable to the caller -- and §7 wants a message a human
 *     can act on, which is a different sentence for each.
 *   - Size, measured rather than guessed and NOT the deciding factor. This
 *     file is 1459 lines, ~950 of them code. But the part a vendored mdns.h
 *     would actually have replaced -- the encoder and the question parser,
 *     mdns_put_u8() through mdns_match_question() -- is only ~330 lines of
 *     that. The other ~620 are responder POLICY that no single-header
 *     encoder supplies: the announcement schedule, per-interface answers
 *     via IP_PKTINFO, the two rate limits, the free-slot change detection,
 *     and the status/remedy strings §7 requires. Vendoring would have
 *     traded 330 lines of ours for 1620 lines of theirs and left the other
 *     620 exactly where they are.
 *
 *   What was NOT hand-written, because it would have been a full stack:
 *   probing/conflict resolution (RFC 6762 §8.1-8.2), known-answer
 *   suppression (§7.1), duplicate-answer suppression (§7.4), the query and
 *   cache side entirely, name compression in records we emit, IPv6/AAAA.
 *   Each omission is called out at its site below.
 *
 * WHY THE HOST OWNS THIS SOCKET AND NOT shim/: mDNS is not AtticPad
 * protocol traffic. It is IPv4 multicast on 5353 with its own packet
 * format; there is no apad_addr and no apad_udp_* call anywhere in this
 * file's data path. shim/ exposes no multicast API and must not grow one
 * for a Linux-host feature. Same precedent as ipaddr.h (getifaddrs) and
 * webui.h (the HTTP listener), both of which bypass the shim for exactly
 * this reason.
 *
 * WHY THE LIBRARY DOES NOT OWN IT EITHER: libapadserver is sans-IO
 * (apadserver.h). It supplies the FACTS advertised here -- free slots and
 * the pairing flag -- through the read-only query API that already exists
 * (apad_server_list_clients(), apad_server_pairing_state()), the same two
 * calls the web UI makes. Not one line of libapadserver changed for tier 1.
 *
 * Header-only and included only by server/host/linux/main.c, for the same
 * build-script-constraint reason as assets.h, ipaddr.h and webui.h (see any
 * of them): scripts/build.sh's build_server() names an exact fixed list of
 * .c files and this task's CONSTRAINTS forbid touching scripts/. If that
 * list ever becomes a wildcard, this file becomes server/src/mdns.c with no
 * other change than the socket layer moving behind a host callback.
 *
 * PORTABILITY NOTE FOR M4: everything from mdns_put_name() down to
 * mdns_build_packet() is plain byte-pushing with no POSIX in it -- that is
 * the whole wire format. Only mdns_open/poll/send/close touch BSD sockets.
 * A Windows tier 1 is that socket layer rewritten against Winsock
 * (WSAJoinLeaf is not needed; setsockopt IP_ADD_MEMBERSHIP exists there
 * too) and nothing else. The Windows host does NOT have tier 1 today --
 * see the report accompanying this change.
 */
#ifndef ATTICPAD_HOST_LINUX_MDNS_H
#define ATTICPAD_HOST_LINUX_MDNS_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp -- DNS names compare case-insensitively */
#include <sys/socket.h>
#include <unistd.h>

#include "atticpad/atticpad.h"
#include "atticpad/version.h"
#include "apadserver.h"
#include "../common/ipaddr.h"
#include "../common/ui_mdns_status.h"  /* the host-agnostic snapshot
                                        * mdns_ui_status() below fills for
                                        * the shared UI -- see that file */

/* ---- constants ---------------------------------------------------------- */

#define MDNS_PORT_DEFAULT   5353
#define MDNS_GROUP_V4       0xE00000FBu   /* 224.0.0.251, host order */

/* §7's service type, verbatim. The trailing ".local" is the mDNS domain;
 * clients/android/AtticPadNsd.kt browses the NsdManager spelling
 * "_atticpad._udp." of this same name. */
#define MDNS_SERVICE        "_atticpad._udp.local"

/* RFC 6763 §9: the DNS-SD service-type enumeration name. Answering it costs
 * one PTR and is what `avahi-browse -a` and macOS's "browse all" walk. */
#define MDNS_META_SERVICE   "_services._dns-sd._udp.local"

/* TTLs. RFC 6762 §10 suggests 120 s for records containing host names and
 * 75 minutes for others (PTR/TXT). We use 120 s for ALL of them on purpose:
 * the TXT here carries `free`, which changes whenever a client connects or
 * disconnects, and a 4500 s TXT in a browser's cache would advertise a full
 * server as having slots for over an hour after the fact. 120 s bounds that,
 * and the change-triggered re-announce below (mdns_poll) closes the window
 * to about a second in practice. */
#define MDNS_TTL            120u
/* RFC 6762 §5.1 / §10: responses to a LEGACY unicast querier (source port
 * != 5353) must carry a TTL of at most 10 s, because such a querier has no
 * mDNS cache-coherency machinery and would otherwise hold a stale answer. */
#define MDNS_TTL_LEGACY     10u

#define MDNS_BUF            1400u   /* one Ethernet payload; we never fragment */
#define MDNS_NAME_MAX       256u
#define MDNS_MAX_RX_PER_POLL 8      /* bound the work one loop lap can do    */
#define MDNS_ANNOUNCE_COUNT 3       /* RFC 6762 §8.3: 2..8, one second apart */
#define MDNS_ANNOUNCE_GAP_MS 1000u
#define MDNS_FACTS_POLL_MS  500u    /* how often free/pairing/IP are re-read */

/* RR types we know about. */
#define MDNS_TYPE_A     1u
#define MDNS_TYPE_PTR  12u
#define MDNS_TYPE_TXT  16u
#define MDNS_TYPE_SRV  33u
#define MDNS_TYPE_ANY 255u

#define MDNS_CLASS_IN     1u
#define MDNS_CLASS_ANY  255u
#define MDNS_CACHE_FLUSH 0x8000u   /* RFC 6762 §10.2, on unique records only */
#define MDNS_UNICAST_REQ 0x8000u   /* the QU bit in a question's qclass, §5.4 */

#define MDNS_TXT_MAX_ITEMS 8

/* A unicast reply (a legacy querier, or one that set the QU bit) is exempt
 * from RFC 6762 §6's one-per-second multicast limit, because it goes to the
 * asker rather than to the segment. That leaves one hole worth closing: the
 * source address of a UDP query is unauthenticated, so a spoofed 40-byte
 * query draws a ~380-byte reply at an address the attacker chose -- a ~9x
 * amplifier, which is the exact shape docs/PROTOCOL.md §7 forbids for a
 * tier-2 ANNOUNCE and §8 rate limits for ERROR. Neither of those rules
 * reaches this socket (it is not AtticPad traffic), so the reasoning is
 * carried over by hand: a token bucket, refilled at MDNS_UNICAST_RATE per
 * second and burstable to MDNS_UNICAST_BURST.
 *
 * The numbers are chosen to be invisible to legitimate use and useless to
 * an attacker: real legacy queriers are rare, ask once and repeat on
 * failure, so 20/s is orders of magnitude above any honest load, while a
 * flood that would otherwise have drawn thousands of replies per second
 * gets 20. Not required by RFC 6762 -- deliberate defence in depth. */
#define MDNS_UNICAST_RATE   20u
#define MDNS_UNICAST_BURST  20u

/* ---- status, which is what §7's third obligation reports ---------------- */

typedef enum {
    MDNS_STATE_RUNNING = 0,   /* bound, joined, advertising                  */
    MDNS_STATE_DISABLED,      /* --no-mdns / ATTICPAD_MDNS=0: user's choice  */
    MDNS_STATE_BIND_FAILED,   /* §7: "If the 5353 bind fails"                */
    MDNS_STATE_SOCKET_FAILED  /* socket()/setsockopt/join -- not the bind    */
} mdns_state;

typedef struct {
    int        fd;
    mdns_state state;
    char       message[200];   /* what happened, in a sentence             */
    char       remedy[200];    /* what a human can DO about it, or ""      */

    uint16_t   bind_port;      /* 5353 unless ATTICPAD_MDNS_PORT overrode it */
    uint16_t   service_port;   /* the AtticPad UDP port, what SRV advertises */

    /* "<name>._atticpad._udp.local". Sized from its parts rather than
     * MDNS_NAME_MAX (which bounds an INBOUND name, a different thing):
     * APAD_NAME_LEN + "." + MDNS_SERVICE is 54 bytes at most, and a
     * generous 256 here makes -Wformat-truncation right to complain that
     * it cannot fit in `message` below. */
    char       instance[APAD_NAME_LEN + 32];
    char       host[128];                 /* "atticpad-<hostname>.local"    */
    char       service_name[APAD_NAME_LEN + 1];   /* the TXT `name=` value  */

    /* Announcement schedule (RFC 6762 §8.3) and change-triggered re-announce. */
    uint32_t   announce_at;
    int        announce_left;
    uint32_t   last_announce_ms;
    int        ever_announced;

    /* Cached facts, re-read from the library at most every
     * MDNS_FACTS_POLL_MS so the fast path never calls into it. */
    uint32_t   facts_at;
    int        free_slots;
    int        pads_total;
    int        pairing_open;

    /* Our own IPv4 addresses: the A record's rdata, the multicast
     * interfaces we send on, and (on change) a reason to re-announce. */
    host_own_addr addrs[HOST_MAX_OWN_ADDRS];
    unsigned      ifindex[HOST_MAX_OWN_ADDRS];   /* if_nametoindex(addrs[i]) */
    size_t        naddr;

    /* RFC 6762 §6's per-interface multicast rate limit -- see
     * mdns_emit_multicast(). Indexed the same as addrs[]; `seen` exists
     * because 0 is a legal tick value. */
    uint32_t      last_mcast_ms[HOST_MAX_OWN_ADDRS];
    int           mcast_seen[HOST_MAX_OWN_ADDRS];

    /* The unicast-reply token bucket described above MDNS_UNICAST_RATE. */
    uint32_t   uni_tokens;
    uint32_t   uni_refill_ms;

    /* Counters, purely for the UI and for proving this thing ran. */
    uint32_t   queries_rx;
    uint32_t   responses_tx;
    uint32_t   dropped_tx;      /* replies suppressed by either rate limit */
    uint32_t   announces_tx;
} mdns_responder;

static const char *mdns_state_name(mdns_state st)
{
    switch (st) {
    case MDNS_STATE_RUNNING:       return "running";
    case MDNS_STATE_DISABLED:      return "disabled";
    case MDNS_STATE_BIND_FAILED:   return "bind_failed";
    case MDNS_STATE_SOCKET_FAILED: return "socket_failed";
    default:                       return "unknown";
    }
}

/* ---- DNS wire format: encoding ----------------------------------------- */
/* Everything in this section is pure byte-pushing: no sockets, no POSIX, no
 * knowledge of AtticPad beyond the strings it is handed.
 *
 * Offsets are size_t and MDNS_ERR is the sticky failure value: once a put
 * fails, every subsequent put in the chain returns MDNS_ERR too, so a caller
 * can write ten of them and test once at the end. It is (size_t)-1 rather
 * than the more obvious 0 because the first put in a packet writes AT offset
 * 0 -- a 0 sentinel makes the header itself indistinguishable from an
 * overflow, which is exactly the bug that shipped in the first draft of this
 * file and produced a responder that answered nothing at all while looking
 * perfectly healthy. */
#define MDNS_ERR ((size_t)-1)

static size_t mdns_put_u8(uint8_t *b, size_t cap, size_t off, uint8_t v)
{
    if (off == MDNS_ERR || off + 1u > cap) {
        return MDNS_ERR;
    }
    b[off] = v;
    return off + 1u;
}

static size_t mdns_put_u16(uint8_t *b, size_t cap, size_t off, uint16_t v)
{
    if (off == MDNS_ERR || off + 2u > cap) {
        return MDNS_ERR;
    }
    b[off]     = (uint8_t)(v >> 8);
    b[off + 1] = (uint8_t)(v & 0xFFu);
    return off + 2u;
}

static size_t mdns_put_u32(uint8_t *b, size_t cap, size_t off, uint32_t v)
{
    if (off == MDNS_ERR || off + 4u > cap) {
        return MDNS_ERR;
    }
    b[off]     = (uint8_t)(v >> 24);
    b[off + 1] = (uint8_t)(v >> 16);
    b[off + 2] = (uint8_t)(v >> 8);
    b[off + 3] = (uint8_t)(v & 0xFFu);
    return off + 4u;
}

static size_t mdns_put_bytes(uint8_t *b, size_t cap, size_t off,
                             const void *src, size_t n)
{
    if (off == MDNS_ERR || off + n > cap) {
        return MDNS_ERR;
    }
    memcpy(b + off, src, n);
    return off + n;
}

/*
 * Write a dotted name as a length-prefixed label sequence terminated by a
 * root label. NO COMPRESSION POINTERS are emitted: compression is an
 * optimisation, never a requirement (RFC 1035 §4.1.4), and our largest
 * possible response is ~380 bytes against a 1400-byte buffer, so the only
 * thing a string table would buy is a class of pointer bugs. This is the
 * single biggest reason the encoder here is short and mjansson's is not.
 */
static size_t mdns_put_name(uint8_t *b, size_t cap, size_t off, const char *name)
{
    const char *p = name;

    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = (dot != NULL) ? (size_t)(dot - p) : strlen(p);

        if (len == 0u || len > 63u) {
            return MDNS_ERR;   /* empty or over-long label: emit nothing */
        }
        off = mdns_put_u8(b, cap, off, (uint8_t)len);
        off = mdns_put_bytes(b, cap, off, p, len);
        if (off == MDNS_ERR) {
            return MDNS_ERR;
        }
        p += len;
        if (*p == '.') {
            p++;
        }
    }
    return mdns_put_u8(b, cap, off, 0u);   /* root */
}

/* ---- the TXT record's contents (§7 obligation 2) ------------------------ */
/*
 * §7 asks for "TXT records for name, version, and free slots". Those three
 * are `name`, `version` and `free`. The other three exist so that a client
 * that found this server through tier 1 learns exactly what a tier-2
 * ANNOUNCE would have told it (§6.2: server_name, pads_total, pads_free,
 * pairing_required, server_port) -- discovery should not depend on which
 * tier happened to win the race. `server_port` is not duplicated here: it
 * is the SRV record's port, which is where DNS-SD says it belongs.
 *
 * `version` versus `proto`: §7 says "version" without saying which, and
 * this project has two -- APAD_VERSION_STR, the build (core/include/
 * atticpad/version.h), and APAD_VERSION, the frozen wire version (§3). A
 * browsing client needs the second to know whether it can talk at all and a
 * human needs the first to know what is running, so both are published
 * rather than guessing which the spec meant.
 *
 * Returns the number of strings written. Each is a single TXT
 * character-string, so each must stay under 256 bytes -- the longest here
 * is `name=` plus APAD_NAME_LEN, i.e. 37.
 */
static size_t mdns_txt_items(const mdns_responder *m,
                             char items[][96], size_t max)
{
    size_t n = 0;

    if (n < max) {
        (void)snprintf(items[n++], 96, "name=%s", m->service_name);
    }
    if (n < max) {
        (void)snprintf(items[n++], 96, "version=%s", APAD_VERSION_STR);
    }
    if (n < max) {
        (void)snprintf(items[n++], 96, "proto=%u", (unsigned)APAD_VERSION);
    }
    if (n < max) {
        (void)snprintf(items[n++], 96, "free=%d", m->free_slots);
    }
    if (n < max) {
        (void)snprintf(items[n++], 96, "pads=%d", m->pads_total);
    }
    if (n < max) {
        (void)snprintf(items[n++], 96, "pairing=%d", m->pairing_open ? 1 : 0);
    }
    return n;
}

/* ---- record emission ---------------------------------------------------- */

typedef enum {
    MDNS_REC_META_PTR = 0,   /* _services._dns-sd._udp.local PTR -> service  */
    MDNS_REC_PTR,            /* service PTR -> instance                      */
    MDNS_REC_SRV,            /* instance SRV -> host:port                    */
    MDNS_REC_TXT,            /* instance TXT                                 */
    MDNS_REC_A               /* host A -> every local IPv4 (one RR each)     */
} mdns_rec_kind;

/*
 * Append one record kind. Returns the number of RRs actually appended (A
 * can be several, or zero on a machine with no non-loopback address) and
 * writes the new offset through `off`. On overflow `*off` becomes 0 and the
 * caller aborts the whole packet -- a truncated mDNS response is worse than
 * no response, because a browser will cache the half of it that parsed.
 */
static int mdns_put_record(const mdns_responder *m, uint8_t *b, size_t cap,
                           size_t *off, mdns_rec_kind kind, uint32_t ttl,
                           int cache_flush, int addr_index)
{
    /* RFC 6762 §10.2: the cache-flush bit belongs on records this responder
     * is the unique authority for (SRV, TXT, A) and NOT on a shared PTR,
     * where several instances legitimately answer the same name. It is also
     * never set on a legacy unicast response -- the caller passes 0 then. */
    uint16_t shared_class = (uint16_t)MDNS_CLASS_IN;
    uint16_t unique_class = (uint16_t)(MDNS_CLASS_IN |
                                       (cache_flush ? MDNS_CACHE_FLUSH : 0u));
    size_t o = *off;
    size_t rdlen_at;
    size_t rdstart;
    int rrs = 0;

    switch (kind) {
    case MDNS_REC_META_PTR:
    case MDNS_REC_PTR: {
        const char *owner = (kind == MDNS_REC_META_PTR)
                                ? MDNS_META_SERVICE : MDNS_SERVICE;
        const char *target = (kind == MDNS_REC_META_PTR)
                                ? MDNS_SERVICE : m->instance;

        o = mdns_put_name(b, cap, o, owner);
        o = mdns_put_u16(b, cap, o, (uint16_t)MDNS_TYPE_PTR);
        o = mdns_put_u16(b, cap, o, shared_class);
        o = mdns_put_u32(b, cap, o, ttl);
        rdlen_at = o;
        o = mdns_put_u16(b, cap, o, 0u);
        rdstart = o;
        o = mdns_put_name(b, cap, o, target);
        if (o == MDNS_ERR) {
            break;
        }
        (void)mdns_put_u16(b, cap, rdlen_at, (uint16_t)(o - rdstart));
        rrs = 1;
        break;
    }
    case MDNS_REC_SRV:
        o = mdns_put_name(b, cap, o, m->instance);
        o = mdns_put_u16(b, cap, o, (uint16_t)MDNS_TYPE_SRV);
        o = mdns_put_u16(b, cap, o, unique_class);
        o = mdns_put_u32(b, cap, o, ttl);
        rdlen_at = o;
        o = mdns_put_u16(b, cap, o, 0u);
        rdstart = o;
        o = mdns_put_u16(b, cap, o, 0u);   /* priority */
        o = mdns_put_u16(b, cap, o, 0u);   /* weight   */
        o = mdns_put_u16(b, cap, o, m->service_port);
        o = mdns_put_name(b, cap, o, m->host);
        if (o == MDNS_ERR) {
            break;
        }
        (void)mdns_put_u16(b, cap, rdlen_at, (uint16_t)(o - rdstart));
        rrs = 1;
        break;

    case MDNS_REC_TXT: {
        char   items[MDNS_TXT_MAX_ITEMS][96];
        size_t nitems = mdns_txt_items(m, items, (size_t)MDNS_TXT_MAX_ITEMS);
        size_t i;

        o = mdns_put_name(b, cap, o, m->instance);
        o = mdns_put_u16(b, cap, o, (uint16_t)MDNS_TYPE_TXT);
        o = mdns_put_u16(b, cap, o, unique_class);
        o = mdns_put_u32(b, cap, o, ttl);
        rdlen_at = o;
        o = mdns_put_u16(b, cap, o, 0u);
        rdstart = o;
        for (i = 0; i < nitems; i++) {
            size_t len = strlen(items[i]);
            if (len > 255u) {
                len = 255u;   /* cannot happen at these key widths; bounded
                               * anyway, because a length byte that lies is
                               * how a parser on the other end gets wedged */
            }
            o = mdns_put_u8(b, cap, o, (uint8_t)len);
            o = mdns_put_bytes(b, cap, o, items[i], len);
        }
        if (nitems == 0u) {
            o = mdns_put_u8(b, cap, o, 0u);   /* RFC 6763 §6.1: never empty  */
        }
        if (o == MDNS_ERR) {
            break;
        }
        (void)mdns_put_u16(b, cap, rdlen_at, (uint16_t)(o - rdstart));
        rrs = 1;
        break;
    }
    case MDNS_REC_A: {
        size_t i;
        /* ONE ADDRESS PER PACKET when `addr_index` names an interface, and
         * that is not an optimisation -- it is the difference between a
         * phone connecting and not. This machine has six non-loopback IPv4
         * addresses (two NICs, three docker bridges, a tailscale link). A
         * single host name carrying all six lets the resolver pick any of
         * them, and the first live run of this file handed avahi-browse
         * 100.66.55.59, the tailscale address, which no client on the LAN
         * can reach. Every real responder publishes the address OF THE
         * INTERFACE the answer goes out on; so does this one now, and
         * mdns_emit_multicast() builds a separate packet per interface for
         * exactly that reason. addr_index < 0 means "every address", used
         * only when the interface is genuinely unknown. */
        for (i = 0; i < m->naddr; i++) {
            struct in_addr in;

            if (addr_index >= 0 && (size_t)addr_index != i) {
                continue;
            }
            if (inet_pton(AF_INET, m->addrs[i].ip, &in) != 1) {
                continue;
            }
            o = mdns_put_name(b, cap, o, m->host);
            o = mdns_put_u16(b, cap, o, (uint16_t)MDNS_TYPE_A);
            o = mdns_put_u16(b, cap, o, unique_class);
            o = mdns_put_u32(b, cap, o, ttl);
            o = mdns_put_u16(b, cap, o, 4u);
            o = mdns_put_bytes(b, cap, o, &in.s_addr, 4u);
            if (o == MDNS_ERR) {
                break;
            }
            rrs++;
        }
        break;
    }
    default:
        break;
    }

    *off = o;
    return (o == MDNS_ERR) ? 0 : rrs;
}

/* What a response is to contain. Set by mdns_match_question() from the
 * query, or all-of-it for an unsolicited announcement. */
typedef struct {
    int meta_ptr;
    int ptr;
    int srv;
    int txt;
    int a;
} mdns_want;

/*
 * Build a complete response packet.
 *
 *   `q_id`      the query's ID, echoed only for a legacy unicast response;
 *               0 otherwise (RFC 6762 §18.1).
 *   `question`  the query's verbatim question section, copied back for a
 *               legacy response (RFC 6762 §6.7) and NULL otherwise. Copying
 *               the bytes rather than re-encoding is exact AND
 *               offset-preserving: they land at offset 12 in our packet
 *               just as they did in theirs, so any compression pointer
 *               inside them still points where it pointed.
 *   `legacy`    caps TTLs at 10 s and suppresses the cache-flush bit.
 *   `goodbye`   TTL 0 everywhere: RFC 6762 §10.1's "these records are gone".
 *
 * Returns the packet length, or 0 if nothing was worth sending.
 */
static size_t mdns_build_packet(const mdns_responder *m, uint8_t *b, size_t cap,
                                const mdns_want *w, uint16_t q_id,
                                const uint8_t *question, size_t qlen,
                                uint16_t qdcount, int legacy, int goodbye,
                                int addr_index)
{
    size_t   off = 0;
    uint32_t ttl = goodbye ? 0u : (legacy ? (uint32_t)MDNS_TTL_LEGACY
                                          : (uint32_t)MDNS_TTL);
    int      cache_flush = legacy ? 0 : 1;
    int      an = 0, ar = 0;
    size_t   an_at, ar_at;

    off = mdns_put_u16(b, cap, 0u, q_id);
    /* QR | AA. Authoritative is not optional in mDNS: every response is by
     * definition from the authority for the name (RFC 6762 §18.4). */
    off = mdns_put_u16(b, cap, off, 0x8400u);
    off = mdns_put_u16(b, cap, off, qdcount);
    an_at = off;
    off = mdns_put_u16(b, cap, off, 0u);   /* ancount, patched below */
    off = mdns_put_u16(b, cap, off, 0u);   /* nscount: always 0 here  */
    ar_at = off;
    off = mdns_put_u16(b, cap, off, 0u);   /* arcount, patched below */
    if (off == MDNS_ERR) {
        return 0u;
    }
    if (question != NULL && qlen > 0u) {
        off = mdns_put_bytes(b, cap, off, question, qlen);
        if (off == MDNS_ERR) {
            return 0u;
        }
    }

    /* Answers, in the order a resolver most wants them. */
    if (w->meta_ptr) {
        an += mdns_put_record(m, b, cap, &off, MDNS_REC_META_PTR, ttl, 0, addr_index);
    }
    if (w->ptr) {
        an += mdns_put_record(m, b, cap, &off, MDNS_REC_PTR, ttl, 0, addr_index);
    }
    if (w->srv) {
        an += mdns_put_record(m, b, cap, &off, MDNS_REC_SRV, ttl, cache_flush, addr_index);
    }
    if (w->txt) {
        an += mdns_put_record(m, b, cap, &off, MDNS_REC_TXT, ttl, cache_flush, addr_index);
    }
    if (w->a) {
        an += mdns_put_record(m, b, cap, &off, MDNS_REC_A, ttl, cache_flush, addr_index);
    }
    if (off == MDNS_ERR) {
        return 0u;
    }

    /* Additionals: RFC 6763 §12 -- a PTR answer SHOULD carry the SRV, TXT
     * and A that a resolver is about to ask for anyway, and an SRV answer
     * SHOULD carry the A. This is the difference between one round trip and
     * three, and on a phone waking its Wi-Fi radio that is the difference a
     * user feels. */
    if (w->ptr) {
        if (!w->srv) {
            ar += mdns_put_record(m, b, cap, &off, MDNS_REC_SRV, ttl, cache_flush, addr_index);
        }
        if (!w->txt) {
            ar += mdns_put_record(m, b, cap, &off, MDNS_REC_TXT, ttl, cache_flush, addr_index);
        }
        if (!w->a) {
            ar += mdns_put_record(m, b, cap, &off, MDNS_REC_A, ttl, cache_flush, addr_index);
        }
    } else if (w->srv && !w->a) {
        ar += mdns_put_record(m, b, cap, &off, MDNS_REC_A, ttl, cache_flush, addr_index);
    }
    if (off == MDNS_ERR) {
        return 0u;
    }
    if (an == 0 && ar == 0) {
        return 0u;
    }

    (void)mdns_put_u16(b, cap, an_at, (uint16_t)an);
    (void)mdns_put_u16(b, cap, ar_at, (uint16_t)ar);
    return off;
}

/* ---- DNS wire format: the only decoding this file does ------------------ */
/*
 * Question names only. There is no RR parser here and there must not be
 * one: an advertise-only responder never consumes an answer, which is what
 * makes "no cache, no conflict resolution, no known-answer suppression" a
 * coherent position rather than a hole.
 *
 * Compression pointers ARE followed, with a jump budget: a question name
 * legally never uses one (it is the first thing after the header, so there
 * is nothing behind it to point at but the header), but a malicious packet
 * can contain a pointer loop and this must terminate on it rather than
 * spin inside the input loop.
 */
static int mdns_parse_name(const uint8_t *pkt, size_t len, size_t off,
                           char *out, size_t out_cap, size_t *next_off)
{
    size_t o = off;
    size_t written = 0;
    int    jumps = 0;
    int    jumped = 0;

    out[0] = '\0';
    for (;;) {
        uint8_t l;

        if (o >= len) {
            return -1;
        }
        l = pkt[o];
        if ((l & 0xC0u) == 0xC0u) {
            size_t target;
            if (o + 1u >= len || ++jumps > 8) {
                return -1;
            }
            target = (size_t)(((uint16_t)(l & 0x3Fu) << 8) | pkt[o + 1u]);
            if (!jumped) {
                *next_off = o + 2u;
                jumped = 1;
            }
            if (target >= o) {
                return -1;   /* forward or self pointer: never legal, and the
                              * cheapest possible loop guard */
            }
            o = target;
            continue;
        }
        if ((l & 0xC0u) != 0u) {
            return -1;   /* 0x40/0x80: reserved label types (RFC 6891) */
        }
        o++;
        if (l == 0u) {
            if (!jumped) {
                *next_off = o;
            }
            out[written] = '\0';
            return 0;
        }
        if (o + l > len || written + l + 2u > out_cap) {
            return -1;
        }
        if (written > 0u) {
            out[written++] = '.';
        }
        memcpy(out + written, pkt + o, l);
        written += l;
        o += l;
    }
}

/* Does this question ask for something we advertise? Accumulates into `w`
 * so a multi-question query gets one response. Returns 1 if it matched. */
static int mdns_match_question(const mdns_responder *m, const char *name,
                               uint16_t qtype, uint16_t qclass, mdns_want *w)
{
    uint16_t cls = (uint16_t)(qclass & 0x7FFFu);
    int any = (qtype == (uint16_t)MDNS_TYPE_ANY);
    int hit = 0;

    if (cls != (uint16_t)MDNS_CLASS_IN && cls != (uint16_t)MDNS_CLASS_ANY) {
        return 0;
    }
    if (strcasecmp(name, MDNS_SERVICE) == 0) {
        if (any || qtype == (uint16_t)MDNS_TYPE_PTR) {
            w->ptr = 1;
            hit = 1;
        }
    } else if (strcasecmp(name, m->instance) == 0) {
        if (any || qtype == (uint16_t)MDNS_TYPE_SRV) {
            w->srv = 1;
            hit = 1;
        }
        if (any || qtype == (uint16_t)MDNS_TYPE_TXT) {
            w->txt = 1;
            hit = 1;
        }
    } else if (strcasecmp(name, m->host) == 0) {
        if (any || qtype == (uint16_t)MDNS_TYPE_A) {
            w->a = 1;
            hit = 1;
        }
    } else if (strcasecmp(name, MDNS_META_SERVICE) == 0) {
        if (any || qtype == (uint16_t)MDNS_TYPE_PTR) {
            w->meta_ptr = 1;
            hit = 1;
        }
    }
    return hit;
}

/* ---- sockets ------------------------------------------------------------ */

/*
 * Build and send an unsolicited announcement or a multicast response.
 *
 * ONE PACKET IS BUILT PER INTERFACE, not one packet fanned out: the A
 * record has to carry the address of the interface it leaves on (see
 * MDNS_REC_A). `only` restricts that to a single interface -- the one a
 * query arrived on -- and -1 means every one of them, which is what an
 * announcement wants.
 *
 * Per-interface IP_MULTICAST_IF rather than one send on the default route:
 * a laptop docked with Ethernet while Wi-Fi is still up has two networks
 * and the phone is on exactly one of them -- picking whichever the routing
 * table prefers is a coin flip that looks like "discovery is flaky".
 * ipaddr.h already enumerates them for §7 tier 3, so this costs one
 * setsockopt and one small build per interface per announcement.
 *
 * Never blocks: the socket is O_NONBLOCK and a failed send is dropped. mDNS
 * is unreliable multicast by construction and the announcement repeats.
 */
static int mdns_emit_multicast(mdns_responder *m, const mdns_want *w,
                               int goodbye, int only, uint32_t now,
                               int rate_limit)
{
    struct sockaddr_in dst;
    uint8_t            buf[MDNS_BUF];
    size_t             len;
    size_t             i;
    int                sent = 0;

    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)m->bind_port);
    dst.sin_addr.s_addr = htonl(MDNS_GROUP_V4);

    if (m->naddr == 0u) {
        len = mdns_build_packet(m, buf, sizeof buf, w, 0u, NULL, 0u, 0u,
                                0, goodbye, -1);
        if (len > 0u) {
            (void)sendto(m->fd, buf, len, 0,
                         (struct sockaddr *)&dst, sizeof dst);
            sent++;
        }
        return sent;
    }
    for (i = 0; i < m->naddr; i++) {
        struct in_addr ifa;

        if (only >= 0 && (size_t)only != i) {
            continue;
        }
        /* RFC 6762 §6: "a Multicast DNS responder MUST NOT multicast a
         * record on a given interface until at least one second has elapsed
         * since the last time that record was multicast on that particular
         * interface." Implemented per interface, exactly as worded.
         *
         * It is also this project's §8-shaped defence on a path §8 does not
         * reach. A 40-byte PTR query draws a ~380-byte response, and unlike
         * an ANNOUNCE that response goes to the whole segment by design, so
         * an unthrottled responder is a ~9x amplifier any host on the LAN
         * can aim at the group. §7 forbids exactly that shape of thing for
         * tier 2 and the reasoning carries over. Measured before adding it:
         * a 1000-queries-per-second flood drew 4384 multicast responses.
         *
         * Unicast replies (QU and legacy, in mdns_handle) are deliberately
         * NOT limited: they go to one asker, cost that asker's own bandwidth
         * and cannot be aimed at anyone else. Announcements pass
         * rate_limit == 0 because their own schedule already spaces them a
         * second apart, and a goodbye must never be suppressed. */
        if (rate_limit && m->mcast_seen[i] &&
            !apad_time_after(now, m->last_mcast_ms[i] + 1000u)) {
            continue;
        }
        if (inet_pton(AF_INET, m->addrs[i].ip, &ifa) != 1) {
            continue;
        }
        len = mdns_build_packet(m, buf, sizeof buf, w, 0u, NULL, 0u, 0u,
                                0, goodbye, (int)i);
        if (len == 0u) {
            continue;
        }
        (void)setsockopt(m->fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
        (void)sendto(m->fd, buf, len, 0, (struct sockaddr *)&dst, sizeof dst);
        m->last_mcast_ms[i] = now;
        m->mcast_seen[i]    = 1;
        sent++;
    }
    return sent;
}

static void mdns_send_unicast(mdns_responder *m, const struct sockaddr_in *to,
                              const uint8_t *buf, size_t len)
{
    (void)sendto(m->fd, buf, len, 0, (const struct sockaddr *)to,
                 (socklen_t)sizeof *to);
}

/* Map the interface a datagram arrived on (IP_PKTINFO, below) to an index
 * into m->addrs, or -1 if it is not one we have an address for -- a query
 * that came in over loopback, most often. */
static int mdns_addr_index_for_ifindex(const mdns_responder *m, unsigned ifidx)
{
    size_t i;

    if (ifidx == 0u) {
        return -1;
    }
    for (i = 0; i < m->naddr; i++) {
        if (m->ifindex[i] == ifidx) {
            return (int)i;
        }
    }
    return -1;
}

/* if_nametoindex() for every enumerated address. Recomputed whenever the
 * address list is, because an interface that goes down and comes back can
 * be handed a different index. */
static void mdns_fill_ifindex(mdns_responder *m)
{
    size_t i;

    for (i = 0; i < m->naddr; i++) {
        m->ifindex[i] = if_nametoindex(m->addrs[i].iface);
    }
}

/* Re-read the three facts the advertisement carries. Cheap, but not free
 * (apad_server_list_clients copies a snapshot per client), so it is rate
 * limited by the caller. Returns 1 if anything a browser cached changed. */
static int mdns_refresh_facts(mdns_responder *m, const apad_server *server)
{
    apad_pairing_info pinfo;
    host_own_addr     addrs[HOST_MAX_OWN_ADDRS];
    size_t            naddr;
    int               nclients;
    int               free_slots;
    int               pairing_open = 0;
    int               changed = 0;

    nclients = apad_server_list_clients(server, NULL, 0u);
    if (nclients < 0) {
        nclients = 0;
    }
    free_slots = (int)APAD_MAX_SESSIONS - nclients;
    if (free_slots < 0) {
        free_slots = 0;
    }
    if (apad_server_pairing_state(server, &pinfo) == APAD_OK) {
        pairing_open = pinfo.open ? 1 : 0;
    }
    naddr = host_enumerate_own_ipv4(addrs, HOST_MAX_OWN_ADDRS);

    if (free_slots != m->free_slots || pairing_open != m->pairing_open) {
        changed = 1;
    }
    /* Compared string by string rather than with one memcmp over the array:
     * host_enumerate_own_ipv4() snprintf()s into fixed-size char fields and
     * leaves the bytes past the NUL untouched, so a memcmp over an
     * uninitialised stack buffer reports "changed" on every single call --
     * which here would mean an unsolicited multicast every second, forever.
     * Found by reasoning about the buffer, not by watching it happen. */
    if (naddr != m->naddr) {
        changed = 1;   /* D4's "announce on start and IP change" */
    } else {
        size_t k;
        for (k = 0; k < naddr; k++) {
            if (strcmp(addrs[k].ip, m->addrs[k].ip) != 0 ||
                strcmp(addrs[k].iface, m->addrs[k].iface) != 0) {
                changed = 1;
                break;
            }
        }
    }
    m->free_slots   = free_slots;
    m->pairing_open = pairing_open;
    m->naddr        = naddr;
    memcpy(m->addrs, addrs, naddr * sizeof addrs[0]);
    mdns_fill_ifindex(m);
    return changed;
}

static void mdns_announce(mdns_responder *m, uint32_t now, int goodbye)
{
    mdns_want w;

    memset(&w, 0, sizeof w);
    w.ptr = 1;
    w.srv = 1;
    w.txt = 1;
    w.a   = 1;
    /* An announcement is an unsolicited response: qdcount 0, ID 0, every
     * record in the ANSWER section (RFC 6762 §8.3), including the ones a
     * query would have got as additionals -- hence build with w.srv/txt/a
     * set rather than letting the additional-section logic place them. */
    (void)mdns_emit_multicast(m, &w, goodbye, -1, now, 0);
    m->announces_tx++;
    m->last_announce_ms = now;
    m->ever_announced = 1;
}

/* Take one token for a unicast reply, refilling first. Returns 0 when the
 * bucket is empty, which means "drop this reply" -- see MDNS_UNICAST_RATE
 * for why an unauthenticated source address makes that the right answer. */
static int mdns_unicast_allowed(mdns_responder *m, uint32_t now)
{
    uint32_t elapsed;

    if (m->uni_refill_ms == 0u) {
        m->uni_refill_ms = now;
        m->uni_tokens    = MDNS_UNICAST_BURST;
    }
    elapsed = now - m->uni_refill_ms;
    if (elapsed >= 1000u / MDNS_UNICAST_RATE) {
        uint32_t add = (elapsed * MDNS_UNICAST_RATE) / 1000u;
        m->uni_tokens = (m->uni_tokens + add > MDNS_UNICAST_BURST)
                            ? MDNS_UNICAST_BURST : m->uni_tokens + add;
        m->uni_refill_ms = now;
    }
    if (m->uni_tokens == 0u) {
        return 0;
    }
    m->uni_tokens--;
    return 1;
}

/*
 * Handle one received datagram. Everything that is not a query for one of
 * our four names is dropped in silence -- including every response, which
 * is how this responder stays advertise-only in the presence of avahi and
 * adb chattering on the same group.
 */
static void mdns_handle(mdns_responder *m, uint32_t now, const uint8_t *pkt,
                        size_t len, const struct sockaddr_in *from,
                        int addr_index)
{
    uint16_t  flags, qdcount;
    size_t    off = 12u;
    mdns_want w;
    int       matched = 0;
    int       legacy;
    int       unicast_req = 0;
    uint16_t  q_id;
    uint8_t   out[MDNS_BUF];
    size_t    outlen;
    unsigned  i;

    if (len < 12u) {
        return;
    }
    q_id   = (uint16_t)(((uint16_t)pkt[0] << 8) | pkt[1]);
    flags  = (uint16_t)(((uint16_t)pkt[2] << 8) | pkt[3]);
    qdcount = (uint16_t)(((uint16_t)pkt[4] << 8) | pkt[5]);

    if ((flags & 0x8000u) != 0u) {
        return;   /* a response, not a query: not ours to act on */
    }
    if ((flags & 0x7800u) != 0u) {
        return;   /* opcode != QUERY (RFC 6762 §18.3: MUST be ignored) */
    }
    if (qdcount == 0u) {
        return;
    }

    memset(&w, 0, sizeof w);
    for (i = 0; i < qdcount; i++) {
        char     name[MDNS_NAME_MAX];
        size_t   next = 0;
        uint16_t qtype, qclass;

        if (mdns_parse_name(pkt, len, off, name, sizeof name, &next) != 0) {
            return;
        }
        if (next + 4u > len) {
            return;
        }
        qtype  = (uint16_t)(((uint16_t)pkt[next] << 8) | pkt[next + 1u]);
        qclass = (uint16_t)(((uint16_t)pkt[next + 2u] << 8) | pkt[next + 3u]);
        off = next + 4u;
        if (mdns_match_question(m, name, qtype, qclass, &w)) {
            matched = 1;
            if ((qclass & MDNS_UNICAST_REQ) != 0u) {
                unicast_req = 1;   /* the QU bit, RFC 6762 §5.4 */
            }
        }
    }
    m->queries_rx++;
    if (!matched) {
        return;
    }

    /* KNOWN-ANSWER SUPPRESSION IS NOT IMPLEMENTED (RFC 6762 §7.1), and the
     * answer/authority sections of the query are ignored entirely. The
     * standard says SHOULD, and the cost of not doing it is one redundant
     * ~380-byte multicast per browse, against the cost of doing it: a full
     * RR parser and a cache, which is the "full mDNS stack" D4 rules out.
     * DUPLICATE-ANSWER SUPPRESSION (§7.4) is skipped for the same reason.
     *
     * The response is also sent IMMEDIATELY rather than after §6's random
     * 20-120 ms delay for shared records. That delay exists so that several
     * responders answering the same shared PTR do not collide; we are the
     * only advertiser of our own instance, and a timer here would mean
     * holding parsed query state across main-loop laps for no gain. */

    legacy = (ntohs(from->sin_port) != (uint16_t)m->bind_port);
    if (legacy) {
        /* RFC 6762 §6.7: a legacy querier gets its ID back, its question
         * repeated, short TTLs, no cache-flush bit, and a unicast reply. */
        if (!mdns_unicast_allowed(m, now)) {
            m->dropped_tx++;
            return;
        }
        outlen = mdns_build_packet(m, out, sizeof out, &w, q_id,
                                   pkt + 12u, off - 12u, qdcount, 1, 0,
                                   addr_index);
        if (outlen > 0u) {
            mdns_send_unicast(m, from, out, outlen);
            m->responses_tx++;
        }
        return;
    }

    if (unicast_req) {
        /* The QU bit (RFC 6762 §5.4): a full mDNS querier that wants the
         * answer to itself rather than to the group, typically the first
         * query after a interface comes up. */
        if (!mdns_unicast_allowed(m, now)) {
            m->dropped_tx++;
            return;
        }
        outlen = mdns_build_packet(m, out, sizeof out, &w, 0u, NULL, 0u, 0u,
                                   0, 0, addr_index);
        if (outlen == 0u) {
            return;
        }
        mdns_send_unicast(m, from, out, outlen);
    } else {
        /* Answered only on the interface the query arrived on -- the other
         * networks this machine is on did not ask. May legitimately send
         * nothing at all, when §6's one-per-second limit says so; a querier
         * that missed the last response repeats, which is how mDNS works. */
        if (mdns_emit_multicast(m, &w, 0, addr_index, now, 1) == 0) {
            m->dropped_tx++;
            return;
        }
    }
    m->responses_tx++;
}

/* ---- lifecycle ---------------------------------------------------------- */

/* One DNS label, sanitised: everything outside [A-Za-z0-9-] becomes '-'.
 * Used for the host label only. The service INSTANCE name is deliberately
 * NOT sanitised this way -- RFC 6763 §4.1.1 says an instance name is free
 * UTF-8 and "AtticPad Server" is meant to read as itself in a phone's
 * discovery list -- but its dots ARE replaced, since a dot there would
 * silently split it into two labels and change the name. */
static void mdns_label_from(char *out, size_t cap, const char *in, int strict)
{
    size_t i = 0;

    while (in[i] != '\0' && i + 1u < cap && i < 63u) {
        char c = in[i];
        if (c == '.') {
            c = '-';
        } else if (strict && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '-')) {
            c = '-';
        }
        out[i] = strict ? (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c) : c;
        i++;
    }
    out[i] = '\0';
    if (i == 0u) {
        (void)snprintf(out, cap, "atticpad");
    }
}

static void mdns_fail(mdns_responder *m, mdns_state st, const char *msg,
                      const char *remedy)
{
    if (m->fd >= 0) {
        (void)close(m->fd);
        m->fd = -1;
    }
    m->state = st;
    (void)snprintf(m->message, sizeof m->message, "%s", msg);
    (void)snprintf(m->remedy, sizeof m->remedy, "%s", remedy);
}

/*
 * Bring the responder up. NEVER fatal: §7 is explicit that a failed tier 1
 * disables tier 1 and nothing else ("acceptable because tier 3 always
 * works", docs/DESIGN.md §5.5), so every failure path here ends in a state a UI
 * can render, not in a return code the caller has to handle.
 *
 * Diagnostic environment knobs, both of which exist because §7's third
 * obligation is untestable otherwise -- a MUST you cannot exercise is a
 * MUST you have not implemented:
 *
 *   ATTICPAD_MDNS=0           off switch (same as --no-mdns)
 *   ATTICPAD_MDNS_PORT=<n>    bind somewhere else; <1024 reproduces EACCES
 *   ATTICPAD_MDNS_NO_REUSE=1  skip SO_REUSEADDR/SO_REUSEPORT, which on any
 *                             machine already running avahi or adb
 *                             reproduces EADDRINUSE exactly
 */
static void mdns_open(mdns_responder *m, const char *server_name,
                      uint16_t service_port, int enabled)
{
    struct sockaddr_in sa;
    char   hostlabel[64];
    char   inst[APAD_NAME_LEN + 1];
    char   hostname[128];
    int    on = 1;
    int    no_reuse;
    unsigned char ttl = 255;      /* RFC 6762 §11 */
    unsigned char loop_on = 1;
    size_t i;
    int    joined = 0;
    const char *env;

    memset(m, 0, sizeof *m);
    m->fd = -1;
    m->service_port = service_port;
    m->pads_total   = (int)APAD_MAX_SESSIONS;
    m->free_slots   = (int)APAD_MAX_SESSIONS;
    (void)snprintf(m->service_name, sizeof m->service_name, "%s",
                   (server_name != NULL && server_name[0] != '\0')
                       ? server_name : "AtticPad Server");

    m->bind_port = (uint16_t)MDNS_PORT_DEFAULT;
    env = getenv("ATTICPAD_MDNS_PORT");
    if (env != NULL) {
        int p = atoi(env);
        if (p > 0 && p < 65536) {
            m->bind_port = (uint16_t)p;
        }
    }

    /* Names, built once. The SRV target is "atticpad-<hostname>.local"
     * rather than the machine's own "<hostname>.local" on purpose: this
     * responder does no probing and no conflict resolution, so it must not
     * claim a name another responder on this LAN may already own and
     * defend. avahi has no interest in a name it never registered, which is
     * what lets the two coexist on one host with no negotiation at all. */
    if (gethostname(hostname, sizeof hostname) != 0) {
        (void)snprintf(hostname, sizeof hostname, "host");
    }
    hostname[sizeof hostname - 1] = '\0';
    {
        char *dot = strchr(hostname, '.');
        if (dot != NULL) {
            *dot = '\0';   /* first label only; ".local" is our domain */
        }
    }
    mdns_label_from(hostlabel, sizeof hostlabel, hostname, 1);
    (void)snprintf(m->host, sizeof m->host, "atticpad-%s.local", hostlabel);
    mdns_label_from(inst, sizeof inst, m->service_name, 0);
    (void)snprintf(m->instance, sizeof m->instance, "%s." MDNS_SERVICE, inst);

    env = getenv("ATTICPAD_MDNS");
    if (!enabled || (env != NULL && env[0] == '0')) {
        m->state = MDNS_STATE_DISABLED;
        (void)snprintf(m->message, sizeof m->message,
                       "automatic discovery is switched off");
        (void)snprintf(m->remedy, sizeof m->remedy,
                       "drop --no-mdns (or unset ATTICPAD_MDNS=0) to "
                       "advertise " MDNS_SERVICE);
        return;
    }

    m->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->fd < 0) {
        char msg[200];
        (void)snprintf(msg, sizeof msg,
                       "could not create a network socket for automatic "
                       "discovery (%s)", strerror(errno));
        mdns_fail(m, MDNS_STATE_SOCKET_FAILED, msg,
                  "check the process's file-descriptor limit (ulimit -n)");
        return;
    }

    /* docs/DESIGN.md §5.5 and D4 both name these two by name, and they are the
     * entire reason a second responder can exist on a box where avahi and
     * adb already hold 5353: on Linux a UDP socket may share an address and
     * port with an existing one when both set SO_REUSEADDR, and multicast
     * datagrams are then delivered to EVERY socket joined to the group
     * rather than to just one of them. SO_REUSEPORT is set as well because
     * a peer that used only SO_REUSEPORT would otherwise refuse to share --
     * the kernel requires the new socket to match whichever flag the
     * incumbent used. Neither is checked for failure: a kernel without
     * SO_REUSEPORT (pre-3.9) simply returns ENOPROTOOPT and the bind below
     * is the real test. */
    no_reuse = (getenv("ATTICPAD_MDNS_NO_REUSE") != NULL);
    if (!no_reuse) {
        (void)setsockopt(m->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
        (void)setsockopt(m->fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
    }

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(m->bind_port);
    if (bind(m->fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int err = errno;
        char msg[200];
        char rem[200];

        /* §7's third obligation, in the exact place it applies. The message
         * has to name the port AND the reason, because the two failures a
         * user actually hits want opposite responses. */
        (void)snprintf(msg, sizeof msg,
                       "could not listen for discovery requests on UDP :%u "
                       "(%s). Devices can still find this PC by searching, "
                       "or you can enter its address on them directly.",
                       (unsigned)m->bind_port, strerror(err));
        if (err == EADDRINUSE) {
            (void)snprintf(rem, sizeof rem,
                           "another program on this PC already owns UDP :%u "
                           "without allowing it to be shared -- run: "
                           "ss -lunp 'sport = :%u'",
                           (unsigned)m->bind_port, (unsigned)m->bind_port);
        } else if (err == EACCES || err == EPERM) {
            (void)snprintf(rem, sizeof rem,
                           "ports below 1024 need extra permission on this "
                           "PC; unset ATTICPAD_MDNS_PORT to use the standard "
                           ":%u",
                           (unsigned)MDNS_PORT_DEFAULT);
        } else {
            (void)snprintf(rem, sizeof rem,
                           "start the server with --no-mdns to stop retrying "
                           "automatic discovery");
        }
        mdns_fail(m, MDNS_STATE_BIND_FAILED, msg, rem);
        return;
    }

    (void)setsockopt(m->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    /* Loopback ON: the verification path for this feature (and the Android
     * emulator's, and avahi's own cache) is a browser running on THIS
     * machine. With IP_MULTICAST_LOOP off, a responder is invisible to
     * every process on its own host, which is a spectacularly confusing way
     * for tier 1 to look broken. */
    (void)setsockopt(m->fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loop_on, sizeof loop_on);
    /* IP_PKTINFO turns every recvmsg() into "and it arrived on eth0". That
     * is the only way a socket bound to INADDR_ANY can know which of this
     * machine's networks asked -- and without it the A record in the reply
     * is a guess (see MDNS_REC_A). Not fatal if it fails: mdns_poll() falls
     * back to answering with every address, which is what this file did
     * before and is merely imprecise, not wrong. */
    (void)setsockopt(m->fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof on);

    /* Join 224.0.0.251 on each interface we have an address on. Receiving
     * queries requires membership; sending does not. A per-interface join
     * is what makes a multi-homed box answer on the network the phone is
     * actually on, and matches mdns_send_multicast()'s per-interface send. */
    m->naddr = host_enumerate_own_ipv4(m->addrs, HOST_MAX_OWN_ADDRS);
    mdns_fill_ifindex(m);
    for (i = 0; i < m->naddr; i++) {
        struct ip_mreq req;
        memset(&req, 0, sizeof req);
        req.imr_multiaddr.s_addr = htonl(MDNS_GROUP_V4);
        if (inet_pton(AF_INET, m->addrs[i].ip, &req.imr_interface) != 1) {
            continue;
        }
        if (setsockopt(m->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &req, sizeof req) == 0) {
            joined++;
        }
    }
    if (joined == 0) {
        struct ip_mreq req;
        memset(&req, 0, sizeof req);
        req.imr_multiaddr.s_addr = htonl(MDNS_GROUP_V4);
        req.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(m->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &req, sizeof req) != 0) {
            char msg[200];
            (void)snprintf(msg, sizeof msg,
                           "automatic discovery could not join its network "
                           "group on :%u (%s)",
                           (unsigned)m->bind_port, strerror(errno));
            mdns_fail(m, MDNS_STATE_SOCKET_FAILED, msg,
                      "check that a network connection is up and that "
                      "multicast traffic is not blocked (ip link show, "
                      "ip maddr show)");
            return;
        }
    }

    /* Non-blocking, without exception. This socket is polled from the same
     * loop that carries INPUT_STATE; a blocking recvfrom here would stall
     * the input path behind an unrelated network's mDNS chatter, and that
     * latency IS the product. */
    {
        int fl = fcntl(m->fd, F_GETFL, 0);
        if (fl < 0 || fcntl(m->fd, F_SETFL, fl | O_NONBLOCK) < 0) {
            char msg[200];
            (void)snprintf(msg, sizeof msg,
                           "could not configure the automatic-discovery "
                           "socket (%s)", strerror(errno));
            mdns_fail(m, MDNS_STATE_SOCKET_FAILED, msg, "");
            return;
        }
    }

    m->state = MDNS_STATE_RUNNING;
    (void)snprintf(m->message, sizeof m->message,
                   "advertising %s on UDP :%u", m->instance,
                   (unsigned)m->bind_port);
    m->remedy[0] = '\0';
    m->announce_left = MDNS_ANNOUNCE_COUNT;
    m->announce_at   = 0u;   /* patched to `now` on the first poll */
}

/*
 * Once per lap of the host's datagram loop. Three jobs, none of which may
 * block: run the announcement schedule, notice a change in the facts the
 * TXT carries, and drain up to MDNS_MAX_RX_PER_POLL queries.
 *
 * FREE SLOTS ARE HANDLED BOTH WAYS, deliberately:
 *   - every query is answered from the CURRENT count, because the TXT is
 *     built at send time from mdns_txt_items() and there is no cached
 *     packet anywhere in this file; and
 *   - a change also triggers an unsolicited re-announce, rate limited to
 *     one per second (RFC 6762 §8.3's spacing).
 * Serving current values alone would leave a browser that already cached
 * the TXT showing "8 free" for up to the 120 s TTL; re-announcing alone
 * would race a query that arrives between the change and the announcement.
 * Doing both costs one extra multicast per connect/disconnect.
 */
static void mdns_poll(mdns_responder *m, const apad_server *server, uint32_t now)
{
    int i;

    if (m->state != MDNS_STATE_RUNNING || m->fd < 0) {
        return;
    }
    if (m->announce_at == 0u && m->announce_left > 0) {
        m->announce_at = now;
    }

    if (apad_time_after(now, m->facts_at + MDNS_FACTS_POLL_MS) ||
        m->facts_at == 0u) {
        int changed = mdns_refresh_facts(m, server);
        m->facts_at = now;
        if (changed && m->ever_announced && m->announce_left == 0 &&
            apad_time_after(now, m->last_announce_ms + MDNS_ANNOUNCE_GAP_MS)) {
            m->announce_left = 1;
            m->announce_at   = now;
        }
    }

    if (m->announce_left > 0 && !apad_time_after(m->announce_at, now)) {
        mdns_announce(m, now, 0);
        m->announce_left--;
        m->announce_at = now + MDNS_ANNOUNCE_GAP_MS;
    }

    for (i = 0; i < MDNS_MAX_RX_PER_POLL; i++) {
        uint8_t            buf[MDNS_BUF];
        struct sockaddr_in from;
        struct iovec       iov;
        struct msghdr      msg;
        struct cmsghdr    *cm;
        union {
            struct cmsghdr align;
            unsigned char  raw[CMSG_SPACE(sizeof(struct in_pktinfo))];
        } control;
        unsigned           ifidx = 0u;
        ssize_t            n;

        memset(&from, 0, sizeof from);
        memset(&msg, 0, sizeof msg);
        memset(&control, 0, sizeof control);
        iov.iov_base = buf;
        iov.iov_len  = sizeof buf;
        msg.msg_name       = &from;
        msg.msg_namelen    = (socklen_t)sizeof from;
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = control.raw;
        msg.msg_controllen = sizeof control.raw;

        n = recvmsg(m->fd, &msg, 0);
        if (n <= 0) {
            break;   /* EAGAIN on an empty non-blocking socket: the norm */
        }
        if (from.sin_family != AF_INET) {
            continue;
        }
        for (cm = CMSG_FIRSTHDR(&msg); cm != NULL;
             cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo pi;
                memcpy(&pi, CMSG_DATA(cm), sizeof pi);
                ifidx = (unsigned)pi.ipi_ifindex;
            }
        }
        mdns_handle(m, now, buf, (size_t)n, &from,
                    mdns_addr_index_for_ifindex(m, ifidx));
    }
}

/*
 * RFC 6762 §10.1: a responder that shuts down cleanly SHOULD send its
 * records once more with TTL 0, so browsers drop the entry immediately
 * instead of showing a dead server for the remaining TTL. Cheap good
 * manners, and it is what makes `avahi-browse` print a REMOVE line the
 * instant this process exits.
 */
static void mdns_close(mdns_responder *m)
{
    if (m->state == MDNS_STATE_RUNNING && m->fd >= 0) {
        mdns_announce(m, m->last_announce_ms, 1);
    }
    if (m->fd >= 0) {
        (void)close(m->fd);
        m->fd = -1;
    }
}

/*
 * Fill `out` from a live responder, for the shared server UI
 * (server/host/common/webui.h's /api/state, docs/PROTOCOL.md §7's "if the
 * 5353 bind fails, the server UI MUST say so").
 *
 * THIS IS THE ONLY FUNCTION OUTSIDE THIS FILE'S OWN INTERNALS THAT READS A
 * mdns_responder'S FIELDS. webui.h used to do it directly, which was fine
 * while webui.h was Linux-only and stopped being fine the moment the same
 * header also had to compile into server/host/windows/main.c -- a
 * mdns_responder is a live multicast socket plus per-interface RFC 6762
 * rate-limit state, none of which exists on a host with no responder. The
 * ui_mdns_status struct is the seam; this function is the only crossing.
 *
 * Snapshot semantics: everything is COPIED (the two const char * fields
 * point at string literals with static storage duration, not at anything
 * owned by `m`), so `out` stays valid and correct even if the responder is
 * later closed. That is what lets the UI render a status for a responder
 * that has already failed.
 */
static void mdns_ui_status(const mdns_responder *m, ui_mdns_status *out)
{
    char   items[MDNS_TXT_MAX_ITEMS][96];
    size_t nitems;
    size_t k;

    memset(out, 0, sizeof *out);
    out->implemented  = 1;
    out->ok           = (m->state == MDNS_STATE_RUNNING);
    out->state        = mdns_state_name(m->state);
    out->service      = MDNS_SERVICE;
    out->bind_port    = m->bind_port;
    out->service_port = m->service_port;
    (void)snprintf(out->instance, sizeof out->instance, "%s", m->instance);
    (void)snprintf(out->host,     sizeof out->host,     "%s", m->host);
    (void)snprintf(out->message,  sizeof out->message,  "%s", m->message);
    (void)snprintf(out->remedy,   sizeof out->remedy,   "%s", m->remedy);

    /* The LIVE record set, rebuilt from the same mdns_txt_items() that fills
     * the packets on the wire -- not a description of it -- so what the UI
     * shows and what a phone receives cannot drift. */
    nitems = mdns_txt_items(m, items, (size_t)MDNS_TXT_MAX_ITEMS);
    if (nitems > (size_t)UI_MDNS_TXT_MAX_ITEMS) {
        nitems = (size_t)UI_MDNS_TXT_MAX_ITEMS;
    }
    for (k = 0; k < nitems; k++) {
        /* memcpy with an explicit bound, not snprintf("%s"): both buffers
         * are 96 bytes and mdns_txt_items() always NUL-terminates, but GCC
         * cannot see that items[k] stops before the end of the enclosing
         * 8x96 array and reports a truncation that cannot happen
         * (-Werror=format-truncation). Bounding the copy here says what is
         * actually true and keeps the warning switched on for the cases
         * where it would be real. */
        size_t len = strlen(items[k]);

        if (len >= sizeof out->txt[k]) {
            len = sizeof out->txt[k] - 1u;
        }
        memcpy(out->txt[k], items[k], len);
        out->txt[k][len] = '\0';
    }
    out->ntxt = nitems;

    out->queries_rx   = m->queries_rx;
    out->responses_tx = m->responses_tx;
    out->dropped_tx   = m->dropped_tx;
    out->announces_tx = m->announces_tx;
}

#endif /* ATTICPAD_HOST_LINUX_MDNS_H */
