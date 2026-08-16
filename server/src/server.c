/* server/src/server.c — libapadserver's core: the session table, the
 * session lifecycle, and backend dispatch (docs/DESIGN.md §6.4).
 *
 * This is everything server/src/main.c used to hold except the process
 * itself: the array of live sessions and every handle_* below moved here
 * verbatim, comments included. Talks to the wire only through libapad
 * (codec.c, session.c — never reimplemented here), to the virtual pad only
 * through the apad_backend interface (backend.h) so nothing in this file
 * knows uinput exists, and to the network only through cfg.on_send so
 * nothing in this file knows a socket exists either.
 *
 * The host owns the process, the socket, argv, signals and the logging
 * stream — server/host/linux/main.c on Linux. See apadserver.h for the
 * contract, and for what is still deliberately out of scope (mDNS).
 *
 * §10 pairing is wired through this file but the policy is not here: the
 * window, the secret and the five-attempt lockout live in
 * server/src/pairing.c. What this file owns is the four places pairing
 * touches the wire — ANNOUNCE.pairing_required, WELCOME's AUTH_REQUIRED
 * flag and server_nonce, the §3.1 check-7 tag verification, and passing the
 * session key to apad_packet_build. With no window open, every one of those
 * is a no-op and this server emits exactly the bytes it emitted before
 * pairing existed.
 */
#include <stdarg.h>
#include <stdio.h>    /* vsnprintf/snprintf: formatting only, never a stream */
#include <stdlib.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "apadserver.h"
#include "backend.h"
#include "mapping.h"
#include "pairing.h"
#include "serverlog.h"

typedef struct {
    int           in_use;
    int           pad_created;
    apad_session  core;
    apad_addr     peer;
    uint32_t      caps;          /* APAD_CAP_* from HELLO */
    char          device_name[APAD_NAME_LEN + 1]; /* from HELLO, for the UI */
    const apad_profile *profile; /* matched once at HELLO time, server/src/profiles.c */
    apad_mapping_state map_state; /* mapping.c's per-session touch/gyro state */
    uint8_t       retx_buf[APAD_MAX_DATAGRAM];
    size_t        retx_len;

    /* Server UI query support (apadserver.h apad_client_info). */
    uint32_t      last_ping_sent_ms;  /* last time THIS server originated a
                                        * §6.6 PING on this session; 0 means
                                        * never (a session younger than 1 s) */
    uint32_t      ping_origin_ms;     /* origin_ticks_ms of the in-flight
                                        * server-originated PING           */
    uint8_t       ping_pending;       /* 1 while awaiting its matching PONG */
    uint32_t      rtt_ms;             /* APAD_RTT_UNKNOWN until measured    */
    uint8_t       last_battery;       /* from the newest INPUT_STATE; starts
                                        * at APAD_BATTERY_UNKNOWN (255)     */
    uint32_t      rx_packets;
    uint32_t      tx_packets;

    /* apad_server_last_input() (apadserver.h) -- the raw decoded
     * INPUT_STATE the remapping editor watches live, snapshotted in
     * handle_input_state() before apad_mapping_apply() runs. input_frame is
     * 0 until have_last_input flips to 1 on the session's first accepted
     * INPUT_STATE, then increments once per accepted one -- see
     * apad_server_last_input()'s own doc comment for why 0 doubles as the
     * "nothing yet" sentinel. */
    apad_input_state last_input;
    uint32_t      input_frame;
    uint8_t       have_last_input;

    /* §10 pairing, per session. All zero for every session created outside
     * a pairing window, which is what makes the unauthenticated path
     * byte-identical to what it was before pairing existed. The KEY itself
     * is not here: apad_session_set_key() puts it in core.key, where
     * apad_session_next_header() and apad_packet_verify() can reach it and
     * apad_session_close() wipes it. */
    uint8_t       touchmap_dirty;   /* v2: profile changed, layout needs resend */
    uint8_t       profile_pinned;   /* set by apad_server_set_profile(): a
                                     * human chose this one, so a reload must
                                     * not silently re-match over the top */
    uint8_t       touchmap_repeats;  /* v2: unreliable, so send it more than once */
    uint32_t      touchmap_next_ms;  /* v2: when the next repeat is due        */
    uint8_t       auth_required;    /* its WELCOME carried AUTH_REQUIRED    */
    uint8_t       auth_verified;    /* >=1 inbound tag has verified         */
    uint8_t       auth_charged;     /* already cost the window one attempt  */
    uint32_t      pair_generation;  /* which secret its key came from       */
} server_session;

#define ERROR_RATE_LIMIT_PER_SEC 10u
#define ERROR_RATE_WINDOW_MS      1000u
#define ERROR_RATE_TRACK_SLOTS    16

/* Cap on cfg.broadcast_addrs (apadserver.h), same pattern as profiles.h's
 * APAD_PROFILES_MAX: a real machine has a handful of interfaces at most, so
 * this is generous headroom rather than a tuned limit. Entries beyond it are
 * logged and dropped in apad_server_create(), never fatal. */
#define APAD_MAX_BROADCAST_ADDRS 16

/* Keyed by source IP ONLY (§8: "counted without regard to source port") --
 * apad_addr_equal() compares IP:port, which an attacker defeats for free by
 * rotating the spoofed source port on every datagram, getting a fresh
 * allowance each time. A true sliding window (not a lazily-reset bucket,
 * which lets 2x the limit straddle a reset boundary): ts[] is a ring of the
 * timestamps of the last <=LIMIT sends to this IP, so "allow" is exactly
 * "fewer than LIMIT sends in the trailing WINDOW_MS", checked against the
 * single oldest entry once the ring is full. */
typedef struct {
    uint8_t   ip[4];
    int       in_use;
    uint32_t  ts[ERROR_RATE_LIMIT_PER_SEC];
    uint8_t   count;          /* valid entries in ts[], saturates at LIMIT */
    uint8_t   next;           /* ring write cursor, wraps mod LIMIT */
    uint32_t  last_seen_ms;   /* for LRU eviction only */
} error_rate_slot;

/* One server instance. Everything here was a file-static in main.c; it is
 * per-instance now because a host may legitimately stop and restart a
 * server inside one process (a Windows tray application does exactly that)
 * and because a test that drives two servers at once must not have them
 * share a session table.
 *
 * One thing is NOT per-instance and is called out rather than papered over:
 * profiles.c still keeps its loaded set in a file-static table, exactly as
 * it did before the split, so a second apad_server_create() in the same
 * process replaces the first server's profiles. Harmless for every host
 * that exists (one server per process) and the profile pointers a session
 * holds stay valid, but it is the one place where "two servers in one
 * process" is not yet true. Fixing it means giving profiles.c an owned
 * set object, which is a bigger change than this refactor was allowed. */
struct apad_server {
    const apad_backend *backend;
    apad_server_send_fn on_send;
    apad_server_random_fn on_random;   /* §10 only; NULL forbids pairing */
    apad_log_sink       log;
    void               *user;
    uint16_t            server_port;
    char                server_name[APAD_NAME_LEN + 1];

    /* No clock is stored here, deliberately. The host supplies `now_ms` as a
     * parameter to BOTH apad_server_tick() and apad_server_on_datagram(), so
     * every §8/§9 deadline is evaluated against the time the host actually
     * observed. An earlier version stashed the last tick's clock and let the
     * datagram path read it; the staleness was strictly directional (every
     * deadline fires EARLY, never late), and with a calloc'd instance a host
     * that delivered before its first tick built a session with
     * last_rx_ms == 0 that the next tick tore down as a 3000 ms idle timeout
     * on any machine with more than three seconds of uptime. A parameter
     * makes that unrepresentable rather than merely documented. */

    server_session      sessions[APAD_MAX_SESSIONS];
    error_rate_slot     error_rate[ERROR_RATE_TRACK_SLOTS];

    /* §7 tier 2: this host's own subnet-directed broadcast addresses,
     * copied from cfg.broadcast_addrs at create time. Only .ip is ever
     * read (is_bad_reply_target() below); empty is legal and means "cannot
     * detect a subnet broadcast", exactly as before this field existed. */
    apad_addr           broadcast_addrs[APAD_MAX_BROADCAST_ADDRS];
    size_t              broadcast_addr_count;

    /* §10. Closed until a host calls apad_server_begin_pairing(); see
     * server/src/pairing.c for everything about how it behaves while open. */
    apad_pairing        pairing;
};

/* Diagnostics: format into a stack buffer, hand the finished line to the
 * host's sink. No stream, no malloc, no "[atticpad] " prefix -- the host
 * decorates (serverlog.h). Declared there because profiles.c reports bad
 * profile files through the same path. */
void apad_logf(const apad_log_sink *sink, apad_log_level level,
               const char *fmt, ...)
{
    char    line[APAD_LOG_LINE_MAX];
    va_list ap;

    if (sink == NULL || sink->fn == NULL) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sink->fn(sink->user, level, line);
}

static int slot_valid(int slot)
{
    return slot >= 0 && slot < (int)APAD_MAX_SESSIONS;
}

static int find_session_by_addr(const apad_server *s, const apad_addr *addr)
{
    int i;
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        if (s->sessions[i].in_use && apad_addr_equal(&s->sessions[i].peer, addr)) {
            return i;
        }
    }
    return -1;
}

static int find_session_by_id(const apad_server *s, uint16_t session_id)
{
    int i;
    if (session_id == 0u) {
        return -1;
    }
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        if (s->sessions[i].in_use && s->sessions[i].core.session_id == session_id) {
            return i;
        }
    }
    return -1;
}

static int alloc_session_slot(const apad_server *s)
{
    int i;
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        if (!s->sessions[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int count_free_slots(const apad_server *s)
{
    int i, n = 0;
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        if (!s->sessions[i].in_use) {
            n++;
        }
    }
    return n;
}

static void free_session(apad_server *s, int slot)
{
    if (!slot_valid(slot) || !s->sessions[slot].in_use) {
        return;
    }
    if (s->sessions[slot].pad_created) {
        s->backend->destroy_pad(slot);
        s->sessions[slot].pad_created = 0;
    }
    apad_session_close(&s->sessions[slot].core, APAD_CLOSE_LOCAL);   /* wipes the §10 key */
    s->sessions[slot].in_use          = 0;
    s->sessions[slot].retx_len        = 0;
    s->sessions[slot].auth_required   = 0;
    /* v2 EXPERIMENT: cleared with the rest of the session, so a device that
     * reconnects into this slot is sent its layout again rather than
     * inheriting "already delivered" from whoever held the slot before. */
    s->sessions[slot].touchmap_dirty  = 0;
    s->sessions[slot].profile_pinned  = 0;
    s->sessions[slot].touchmap_repeats = 0;
    s->sessions[slot].touchmap_next_ms = 0;
    s->sessions[slot].auth_verified   = 0;
    s->sessions[slot].auth_charged    = 0;
    s->sessions[slot].pair_generation = 0;
    s->sessions[slot].last_ping_sent_ms = 0;
    s->sessions[slot].ping_origin_ms    = 0;
    s->sessions[slot].ping_pending      = 0;
    s->sessions[slot].rtt_ms            = APAD_RTT_UNKNOWN;
    s->sessions[slot].last_battery      = (uint8_t)APAD_BATTERY_UNKNOWN;
    s->sessions[slot].rx_packets        = 0;
    s->sessions[slot].tx_packets        = 0;
    s->sessions[slot].device_name[0]    = '\0';
    memset(&s->sessions[slot].last_input, 0, sizeof s->sessions[slot].last_input);
    s->sessions[slot].input_frame       = 0;
    s->sessions[slot].have_last_input   = 0;
    memset(&s->sessions[slot].peer, 0, sizeof s->sessions[slot].peer);
}

/* Every path that tears a session down MUST go through here, and NOT call
 * free_session() directly. Found during a teardown-observability audit: an
 * evening of real 3DS sessions logged five HELLOs and not one teardown line,
 * even minutes after the client had exited. The idle-timeout path (§8) DID
 * log correctly (tick_sessions, below) -- it was every OTHER teardown path
 * that called free_session() straight, silently, with nothing printed:
 *   - handle_bye(): a client that quits cleanly sends a best-effort BYE
 *     (every client in this tree does, e.g. clients/3ds/source/main.c) and
 *     the server tore the session down without a trace. This is almost
 *     certainly what the 3DS logs were showing -- clean exits, not crashes.
 *   - handle_hello()'s three failure-path frees (on_recv rejects the HELLO,
 *     pad creation fails, server_accept rejects the rate) were equally
 *     silent.
 *   - main()'s shutdown loop freed every live session with no log at all.
 * Logs BEFORE free_session() clears the slot, because free_session() ->
 * apad_session_close() unconditionally overwrites close_reason with
 * APAD_CLOSE_LOCAL (core/src/session.c) -- reading it after the call would
 * be wrong for a BYE- or idle-timeout-initiated close. Reads session_id
 * before the free for the same reason: free_session() never clears it, but
 * there is no reason to depend on that once a shared exit point exists. */
static void teardown_session(apad_server *s, int slot, const char *reason)
{
    if (!slot_valid(slot) || !s->sessions[slot].in_use) {
        return;
    }
    apad_logf(&s->log, APAD_LOG_INFO, "device (slot %d) disconnected: %s",
              slot, reason);
    free_session(s, slot);
}

static const char *bye_reason_str(uint8_t reason)
{
    switch (reason) {
    case APAD_BYE_NORMAL:          return "normal";
    case APAD_BYE_TIMEOUT:         return "timeout";
    case APAD_BYE_SERVER_SHUTDOWN: return "server shutdown";
    case APAD_BYE_SLOT_REVOKED:    return "slot revoked";
    /* §6.0: BYE.reason is a diagnostic label with no clamp -- an
     * implementation MUST preserve an unrecognised value verbatim rather
     * than rejecting or normalising it. Logged numerically below. */
    default:                       return "unrecognised";
    }
}

/* Names only what this server originates, for the emit() diagnostic. Not a
 * general §4 table: a type the server never sends has no business appearing
 * in a "send failed" line, and hex is more useful than a wrong name. */
static const char *server_msg_str(uint8_t type)
{
    switch (type) {
    case APAD_MSG_ANNOUNCE: return "ANNOUNCE";
    case APAD_MSG_WELCOME:  return "WELCOME";
    case APAD_MSG_PONG:     return "PONG";
    case APAD_MSG_ACK:      return "ACK";
    case APAD_MSG_ERROR:    return "ERROR";
    default:                return "datagram";
    }
}

static int ip_equal(const uint8_t a[4], const uint8_t b[4])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* §8 SHOULD: at most 10 ERROR datagrams per second per source IP, so a
 * flood of unknown-session or malformed traffic cannot turn this server
 * into a reflection amplifier pointed at whatever address the datagrams
 * claim. Fixed-size -- no malloc -- and a peer that arrives when every slot
 * is busy evicts whichever tracked address has been idle longest. Uses
 * apad_time_since() throughout: it is wrap-safe, a raw subtraction is not
 * (docs/CONVENTIONS.md). */
static int error_rate_allow(apad_server *s, const apad_addr *addr, uint32_t now)
{
    int i, free_slot, victim;

    for (i = 0; i < ERROR_RATE_TRACK_SLOTS; i++) {
        error_rate_slot *r = &s->error_rate[i];

        if (!r->in_use || !ip_equal(r->ip, addr->ip)) {
            continue;
        }
        r->last_seen_ms = now;

        if (r->count < (uint8_t)ERROR_RATE_LIMIT_PER_SEC) {
            r->ts[r->next] = now;
            r->next  = (uint8_t)((r->next + 1u) % ERROR_RATE_LIMIT_PER_SEC);
            r->count = (uint8_t)(r->count + 1u);
            return 1;
        }
        /* Ring full: r->ts[r->next] holds the oldest of the last LIMIT
         * sends. Still within the window -> deny; otherwise that oldest
         * send has aged out, so this one takes its place. */
        if (apad_time_since(now, r->ts[r->next]) < ERROR_RATE_WINDOW_MS) {
            return 0;
        }
        r->ts[r->next] = now;
        r->next = (uint8_t)((r->next + 1u) % ERROR_RATE_LIMIT_PER_SEC);
        return 1;
    }

    free_slot = -1;
    victim = 0;
    for (i = 0; i < ERROR_RATE_TRACK_SLOTS; i++) {
        if (!s->error_rate[i].in_use) {
            free_slot = i;
            break;
        }
        if (apad_time_since(now, s->error_rate[i].last_seen_ms)
            > apad_time_since(now, s->error_rate[victim].last_seen_ms)) {
            victim = i;
        }
    }
    if (free_slot < 0) {
        free_slot = victim;   /* evict whichever tracked IP is oldest */
    }

    memset(&s->error_rate[free_slot], 0, sizeof s->error_rate[free_slot]);
    memcpy(s->error_rate[free_slot].ip, addr->ip, sizeof addr->ip);
    s->error_rate[free_slot].in_use       = 1;
    s->error_rate[free_slot].last_seen_ms = now;
    s->error_rate[free_slot].ts[0]        = now;
    s->error_rate[free_slot].next         = 1u;
    s->error_rate[free_slot].count        = 1u;
    return 1;
}

/* §8/§6: never let this server be used to broadcast or multicast a reply --
 * same amplification family as the rate limiter, but a source-address
 * class check rather than a volume limit. Global broadcast, multicast and
 * "this network" are derivable from the address alone and are checked
 * unconditionally below. A subnet-directed broadcast (e.g. 192.168.1.255/24)
 * additionally needs the interface's netmask, which this sans-IO library has
 * no socket to ask for -- `s->broadcast_addrs` (apadserver.h
 * cfg.broadcast_addrs) is the host supplying that answer instead, exactly
 * the way cfg.server_port supplies the listening port this library has no
 * socket to ask about either. A host that leaves the list empty (every host
 * before this field existed, and the Windows host for now -- see
 * server/host/windows/main.c) gets exactly the old behaviour: an
 * unrecognised subnet broadcast is not caught.
 *
 * Applies to ANNOUNCE as well as ERROR, and that is not belt-and-braces: a
 * 12-byte DISCOVER draws a 52-byte ANNOUNCE (~4.3x), the Linux host enables
 * SO_BROADCAST for tier-2 discovery, and a UDP source address is trivially
 * spoofed -- so a DISCOVER claiming to come from 255.255.255.255 would
 * otherwise be answered *to* 255.255.255.255. §7 says ANNOUNCE unicasts back
 * to the source; this filter is what makes "the source" mean an address
 * worth unicasting to. ANNOUNCE deliberately is NOT rate limited -- tier-2
 * discovery is a legitimate burst path and §7 budgets no limiter there.
 *
 * NOT implemented, and not implementable here: §7's loopback rule ("reject
 * a loopback reply target when the request did not arrive on the loopback
 * interface"). Answering that needs to know which LOCAL interface a
 * datagram arrived on, per packet -- on Linux that is IP_PKTINFO
 * (recvmsg() with IP_PKTINFO set on the socket, read back via cmsg), which
 * this library cannot reach either (no socket) and which cfg has no
 * per-packet equivalent for today (apad_server_on_datagram() takes only
 * `from`, not "and which local address it targeted"). Recorded here so a
 * future audit doesn't rediscover the gap: closing it needs a new
 * apad_server_on_datagram() parameter (or an addr-pair struct replacing
 * `from`), which is a wider API change than this fix, not a missing
 * host-supplied list like the subnet-broadcast case above. A loopback
 * source is low severity in practice -- 127.0.0.0/8 amplifying against
 * itself does not leave the machine -- so it is accepted rather than
 * blocking this fix on the bigger change. */
static int is_bad_reply_target(const apad_server *s, const apad_addr *to)
{
    size_t i;

    if (to->ip[0] == 255u && to->ip[1] == 255u
        && to->ip[2] == 255u && to->ip[3] == 255u) {
        return 1;   /* 255.255.255.255 */
    }
    if (to->ip[0] >= 224u && to->ip[0] <= 239u) {
        return 1;   /* 224.0.0.0/4 multicast */
    }
    if (to->ip[0] == 0u && to->ip[1] == 0u && to->ip[2] == 0u && to->ip[3] == 0u) {
        return 1;   /* 0.0.0.0 */
    }
    if (s != NULL) {
        for (i = 0; i < s->broadcast_addr_count; i++) {
            if (memcmp(to->ip, s->broadcast_addrs[i].ip,
                      sizeof to->ip) == 0) {
                return 1;   /* a subnet-directed broadcast this host owns */
            }
        }
    }
    return 0;
}

/*
 * The §10 key this session's outgoing datagrams are tagged with, or NULL
 * when it has none.
 *
 * Every apad_packet_build() call in this file passes this rather than a
 * bare NULL, and that is load-bearing in BOTH directions. codec.c sets the
 * AUTHENTICATED flag from whether a key was supplied and clears it
 * otherwise, so handing it NULL on a session that has a key would silently
 * strip the flag apad_session_next_header() just set and put an untagged
 * datagram on a wire the client is verifying -- the client would reject
 * every reply. And on a session with no key (every session, whenever no
 * pairing window is open) this returns NULL/0, which is exactly the
 * constant the call sites used before pairing existed: same bytes, same
 * lengths.
 */
static const uint8_t *session_key(const server_session *ss, size_t *key_len)
{
    if (ss == NULL || !ss->core.have_key) {
        *key_len = 0;
        return NULL;
    }
    *key_len = (size_t)APAD_SESSION_KEY_LEN;
    return ss->core.key;
}

/* Every outbound datagram in this file goes through here.
 *
 * The return value of cfg.on_send is DIAGNOSTIC, not control flow. §9 is the
 * protocol's recovery mechanism for a datagram that did not reach the peer,
 * and it does not distinguish "the sendto() failed locally" from "the wire
 * ate it" -- they are the same event, and the 100/200/400/800 ms schedule is
 * the same answer to both. So the caller logs the failure and carries on with
 * session accounting exactly as if the datagram had left.
 *
 * BUG FIXED (protocol audit): the library split briefly made a failed send
 * skip apad_session_on_sent() and the retx_buf cache. For a WELCOME that left
 * retx_armed == 0, so the server never retried, never closed as
 * APAD_CLOSE_RETX_FAILED, and the session sat ACTIVE holding a uinput pad
 * until the §8 3000 ms idle timeout -- one transient EAGAIN/ENOBUFS disarmed
 * the only recovery §9 has.
 *
 * `ss`, when non-NULL, is the session this send belongs to -- purely for the
 * UI's tx_packets counter (apadserver.h apad_client_info). Counted here
 * rather than at each of emit()'s five call sites so a §9 retransmit counts
 * exactly like a first send, which is correct: it is a real datagram this
 * server put on the wire, whatever §9 privately thinks of why. Unconditional
 * on `rc`, same reasoning as apad_session_on_sent() below: a locally-failed
 * send still consumed a tx_seq and still gets retried, so it is still "a
 * packet this server sent" from the UI's point of view. */
static int emit(apad_server *s, const apad_addr *to, const uint8_t *buf,
                size_t len, const char *what, server_session *ss)
{
    int rc = s->on_send(s->user, to, buf, len);
    if (rc < 0) {
        apad_logf(&s->log, APAD_LOG_WARN,
                  "send of %s to %u.%u.%u.%u:%u failed (rc=%d); S9 recovery "
                  "proceeds as if it had been lost in transit",
                  what, to->ip[0], to->ip[1], to->ip[2], to->ip[3],
                  (unsigned)to->port, rc);
    }
    if (ss != NULL) {
        ss->tx_packets++;
    }
    return rc;
}

/* `ss` is the session this ERROR is being sent inside, or NULL when there is
 * none (answering an unknown session_id, or any pre-session diagnostic).
 * §9: "every datagram sent inside a session consumes that direction's
 * sequence counter... a datagram sent outside any session... carries
 * sequence 0." Rate-limited and target-filtered per §8 (above); the rate
 * limit and the target filter both take precedence over any MUST to send
 * this ERROR at all (§8: "the rate limit takes precedence over the MUST"). */
static void send_error_raw(apad_server *s, uint32_t now, const apad_addr *to,
                           server_session *ss, uint16_t code, const char *text)
{
    apad_error e;
    apad_header hdr;
    uint8_t payload[APAD_LEN_ERROR];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n, total;

    if (is_bad_reply_target(s, to)) {
        return;
    }
    if (!error_rate_allow(s, to, now)) {
        return;
    }

    memset(&e, 0, sizeof e);
    e.code = code;
    apad_text_set(e.text, sizeof e.text, text);

    n = apad_encode_error(payload, sizeof payload, &e);
    if (n < 0) {
        return;
    }

    memset(&hdr, 0, sizeof hdr);
    if (ss != NULL) {
        if (apad_session_next_header(&ss->core, (uint8_t)APAD_MSG_ERROR, &hdr)
            != APAD_OK) {
            return;
        }
    } else {
        hdr.type = (uint8_t)APAD_MSG_ERROR;   /* session_id 0, sequence 0 */
    }

    {
        size_t klen;
        const uint8_t *key = session_key(ss, &klen);
        total = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)n,
                                  key, klen);
    }
    if (total < 0) {
        return;
    }
    (void)emit(s, to, buf, (size_t)total, "ERROR", ss);
    /* Unconditional: see emit(). An ERROR sent outside a session (ss == NULL)
     * has nothing to account for; one sent inside a session consumes tx_seq
     * either way (§9) and must not leave the FSM's idea of what was sent out
     * of step with the sequence apad_session_next_header() already spent. */
    if (ss != NULL) {
        apad_session_on_sent(&ss->core, &hdr, now);
    }
}

/* ACK inside an established session -- always has one, since an ACK the
 * server itself originates is always the answer to a reliable message
 * received on a real session (handle_bye's BYE-ACK today). Routes through
 * apad_session_next_header like any other in-session send, so it consumes
 * this direction's tx_seq (§9) and carries the session's real session_id --
 * no separate `to`/session_id parameters to keep in sync with `ss`. ACK
 * itself is never reliable (§4 table), so nothing gets armed for
 * retransmit; apad_session_on_sent() confirms that by construction. */
static void send_ack(apad_server *s, uint32_t now, server_session *ss,
                     uint16_t acked_sequence)
{
    apad_ack a;
    apad_header hdr;
    uint8_t payload[APAD_LEN_ACK];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n, total;

    memset(&a, 0, sizeof a);
    a.sequence = acked_sequence;

    n = apad_encode_ack(payload, sizeof payload, &a);
    if (n < 0) {
        return;
    }
    if (apad_session_next_header(&ss->core, (uint8_t)APAD_MSG_ACK, &hdr) != APAD_OK) {
        return;
    }
    {
        size_t klen;
        const uint8_t *key = session_key(ss, &klen);
        total = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)n,
                                  key, klen);
    }
    if (total < 0) {
        return;
    }
    (void)emit(s, &ss->peer, buf, (size_t)total, "ACK", ss);
    apad_session_on_sent(&ss->core, &hdr, now);   /* unconditional: see emit() */
}

/* v2 EXPERIMENT (experiment/touchmap-v2): defined below, called from the
 * HELLO handler above it. */
static void send_touchmap(apad_server *s, server_session *ss, uint32_t now);

/* Send one payload through a session's tx sequence/flags, cache it for
 * retransmit if the session FSM armed it (§9). */
static int send_via_session(apad_server *s, server_session *ss, uint8_t type,
                            const void *payload, uint16_t len, uint32_t now)
{
    apad_header hdr;
    uint8_t buf[APAD_MAX_DATAGRAM];
    int rc, total;

    rc = apad_session_next_header(&ss->core, type, &hdr);
    if (rc != APAD_OK) {
        return rc;
    }
    {
        size_t klen;
        const uint8_t *key = session_key(ss, &klen);
        total = apad_packet_build(buf, sizeof buf, &hdr, payload, len, key, klen);
    }
    if (total < 0) {
        return total;
    }
    rc = emit(s, &ss->peer, buf, (size_t)total, server_msg_str(type), ss);
    /* Session accounting proceeds whatever emit() reported. A WELCOME whose
     * send failed locally still has to arm §9, or nothing ever retries it and
     * nothing ever closes the session as APAD_CLOSE_RETX_FAILED (see emit()).
     * The failure is returned to the caller below for its own diagnostics
     * only -- it must not gate what follows. */
    apad_session_on_sent(&ss->core, &hdr, now);
    /* BUG FIXED (protocol audit): this must key on the message JUST SENT
     * being reliable, not on ss->core.retx_armed. apad_session_on_sent()
     * returns early for a non-RELIABLE header without touching retx_armed
     * (core/src/session.c) -- so if a WELCOME is armed and un-ACKed and a
     * PONG goes out through this same function meanwhile, retx_armed is
     * still 1 from the WELCOME and the old `if (ss->core.retx_armed)` check
     * clobbered retx_buf with the PONG's bytes. tick_sessions then
     * retransmitted a duplicate PONG instead of the WELCOME, the client
     * never got its WELCOME, and the session failed as
     * APAD_CLOSE_RETX_FAILED at t=2300ms even though INPUT_STATE may have
     * been flowing the whole time. hdr.flags is the just-built header for
     * THIS send, so it is the correct signal: apad_session_next_header()
     * sets APAD_FLAG_RELIABLE exactly when this type is reliable (§4). */
    if ((hdr.flags & APAD_FLAG_RELIABLE) != 0u) {
        memcpy(ss->retx_buf, buf, (size_t)total);
        ss->retx_len = (size_t)total;
    }
    return (rc < 0) ? APAD_ERR_STATE : total;
}

static void handle_discover(apad_server *s, uint32_t now, const apad_addr *from,
                            const apad_packet *pkt)
{
    apad_announce ann;
    apad_header hdr;
    uint8_t payload[APAD_LEN_ANNOUNCE];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n, total;

    /* §8's unknown-session rule is type-agnostic and DISCOVER is not one of
     * its exceptions (INPUT_STATE is the only one): "a server receiving a
     * packet with an unknown non-zero session_id MUST reply ERROR code 7 and
     * MUST NOT create a session". §6.1 requires a DISCOVER's session_id to be
     * 0, so a non-zero one is malformed or hostile either way and gets no
     * ANNOUNCE -- the ERROR path is rate limited (§8), the ANNOUNCE path
     * deliberately is not. Mirrors handle_hello's identical guard: a
     * session_id that happens to match a live session is just as nonstandard,
     * so it is answered with silence rather than an ERROR that would be a lie.
     */
    if (pkt->header.session_id != 0u) {
        if (find_session_by_id(s, pkt->header.session_id) < 0) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                           "unknown session");
        }
        return;
    }
    /* Before building anything: a spoofed DISCOVER must not turn this server
     * into a 4.3x amplifier aimed at a broadcast or multicast address. See
     * is_bad_reply_target(). */
    if (is_bad_reply_target(s, from)) {
        return;
    }

    memset(&ann, 0, sizeof ann);
    apad_text_set(ann.server_name, sizeof ann.server_name, s->server_name);
    ann.pads_total       = (uint8_t)APAD_MAX_SESSIONS;
    ann.pads_free        = (uint8_t)count_free_slots(s);
    /* §6.2: 1 exactly while a §10 pairing window is open, because that is
     * precisely when a client's HELLO will be answered with an
     * AUTH_REQUIRED WELCOME and it will need the PIN the user is looking
     * at. Outside a window this is 0 and the field is what it always was --
     * a client that discovers this server can connect with no secret. Note
     * this is "you will be asked to pair", not "you are already paired":
     * nothing here remembers a device between sessions (apadserver.h). */
    ann.pairing_required = (uint8_t)(apad_pairing_is_open(&s->pairing, now) ? 1 : 0);
    /* The host's listening port, from cfg: the library has no socket to ask
     * (§6.2 -- a client that found this server on a broadcast port needs to
     * be told where to send its HELLO). */
    ann.server_port      = s->server_port;

    n = apad_encode_announce(payload, sizeof payload, &ann);
    if (n < 0) {
        return;
    }

    memset(&hdr, 0, sizeof hdr);
    hdr.type = (uint8_t)APAD_MSG_ANNOUNCE;   /* session_id 0 (§6.1/§8) */

    total = apad_packet_build(buf, sizeof buf, &hdr, payload, (uint16_t)n,
                              NULL, 0);
    if (total < 0) {
        return;
    }
    (void)emit(s, from, buf, (size_t)total, "ANNOUNCE", NULL);
}

static void handle_hello(apad_server *s, const apad_addr *from,
                         const apad_packet *pkt, uint32_t now)
{
    apad_hello h;
    apad_welcome w;
    uint8_t payload[APAD_LEN_WELCOME];
    int rc, wn;
    int slot;
    server_session *ss;

    rc = apad_decode_hello(pkt->payload, pkt->payload_len, &h);
    if (rc < 0) {
        send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_MALFORMED, "bad HELLO");
        return;
    }
    /* §6.3: proto_major MUST equal the header version. apad_session_on_recv
     * checks this too on the server-side HELLO branch; checked here as well
     * so a bad HELLO never allocates a slot in the first place. */
    if (h.proto_major != pkt->header.version) {
        send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_VERSION_MISMATCH,
                       "protocol version mismatch");
        return;
    }
    /* §8: a HELLO's session_id is always 0 -- a nonzero one is either
     * unknown (-> ERROR 7, no session created) or, weirder, collides with a
     * real one, which is just as nonstandard. Either way, do not let a
     * bogus nonzero session_id reach the slot allocator below. */
    if (pkt->header.session_id != 0u) {
        if (find_session_by_id(s, pkt->header.session_id) < 0) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                           "unknown session");
        }
        return;
    }

    slot = find_session_by_addr(s, from);

    /* §9 "Duplicates": a HELLO for a peer whose session is already ACTIVE is
     * either a legitimate §9 retransmission (the client's WELCOME-ACK was
     * lost) or an application resending HELLO outright -- either way, per
     * the spec, resend the ORIGINAL WELCOME bytes verbatim and generate
     * nothing new. Reproduced and confirmed the alternative before this fix:
     * calling apad_session_server_accept + send_via_session again allocates
     * a NEW tx_seq and re-arms retx_seq to it, so the ACK already in flight
     * for WELCOME #1 no longer matches anything, and the session dies as
     * APAD_CLOSE_RETX_FAILED at t=2300ms with INPUT_STATE flowing normally.
     *
     * Deliberately does NOT call apad_session_on_recv or
     * apad_session_server_accept here:
     *   - on_recv's server-side HELLO case unconditionally sets state back
     *     to HANDSHAKING (core/session.c; not something server/ can change),
     *     which would stop the §8 idle timeout from firing again until
     *     something moved it back to ACTIVE.
     *   - server_accept resets the §9 receive window (rx_input_valid = 0),
     *     which would let a stale, reordered INPUT_STATE back in mid-session
     *     -- exactly what §9 forbids.
     * A direct refresh of last_rx_ms is the narrowest correct action: §8
     * requires that this datagram refresh liveness, and nothing more. */
    if (slot >= 0 && s->sessions[slot].core.state == (uint8_t)APAD_SESSION_ACTIVE
        && s->sessions[slot].retx_len > 0u) {
        s->sessions[slot].core.last_rx_ms = now;
        s->sessions[slot].rx_packets++;
        (void)emit(s, from, s->sessions[slot].retx_buf,
                   s->sessions[slot].retx_len, "WELCOME (duplicate answer)",
                   &s->sessions[slot]);
        return;
    }

    if (slot < 0) {
        slot = alloc_session_slot(s);
        if (slot < 0) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_NO_FREE_SLOT,
                           "no free pad slot");
            return;
        }
        apad_session_init(&s->sessions[slot].core, 1 /* is_server */, now);
        s->sessions[slot].peer         = *from;
        s->sessions[slot].in_use       = 1;
        s->sessions[slot].pad_created  = 0;
        s->sessions[slot].retx_len     = 0;

        /* UI query state (apadserver.h apad_client_info): a calloc'd slot's
         * zero-initialised rtt_ms/last_battery would otherwise read as "0 ms
         * RTT" and "0% battery" instead of "unknown", and free_session()
         * only resets these on a PRIOR teardown of this slot -- a slot's
         * first-ever occupant reaches here without having gone through it.
         *
         * last_ping_sent_ms = now (NOT 0): apad_server_tick()'s PING
         * origination reads this as "when did we last send one", measured
         * from THIS session's own start, not from the process's epoch. Seeding
         * it with 0 would make apad_time_since(now, 0) equal `now` itself, so
         * a session created deep into a long-running process's uptime (every
         * real session, in practice -- apad_ticks_ms() counts from boot) would
         * get its first PING on the very next tick instead of one second in. */
        s->sessions[slot].last_ping_sent_ms = now;
        s->sessions[slot].ping_origin_ms    = 0;
        s->sessions[slot].ping_pending      = 0;
        s->sessions[slot].rtt_ms            = APAD_RTT_UNKNOWN;
        s->sessions[slot].last_battery      = (uint8_t)APAD_BATTERY_UNKNOWN;
        s->sessions[slot].rx_packets        = 0;
        s->sessions[slot].tx_packets        = 0;

        /* Profile match happens once, here, for a genuinely new occupant of
         * this slot -- never re-derived per INPUT_STATE (docs/DESIGN.md §6.2: the
         * mapping engine is server-side interpretation, resolved at
         * session setup, not on the hot path). mapping.c's per-session
         * touch/gyro state (delta-stick return-to-centre) starts fresh for
         * the same reason a brand-new session shouldn't inherit a stale
         * touch anchor from whatever occupied this slot before. */
        apad_text_get(s->sessions[slot].device_name,
                      sizeof s->sessions[slot].device_name,
                      h.device_name, sizeof h.device_name);
        s->sessions[slot].profile = apad_profiles_match(s->sessions[slot].device_name);
        apad_mapping_state_init(&s->sessions[slot].map_state);
    }
    ss = &s->sessions[slot];
    ss->caps = h.caps & (uint32_t)APAD_CAP_VALID_MASK;

    rc = apad_session_on_recv(&ss->core, pkt, now);
    if (rc < 0) {
        send_error_raw(s, now, from, NULL,
                       (rc == APAD_ERR_VERSION)
                           ? (uint16_t)APAD_ERRC_VERSION_MISMATCH
                           : (uint16_t)APAD_ERRC_MALFORMED,
                       "HELLO rejected");
        teardown_session(s, slot, "connection rejected");
        return;
    }

    if (!ss->pad_created) {
        if (s->backend->create_pad(slot, APAD_PAD_XBOX360) != 0) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_NO_FREE_SLOT,
                           "virtual pad creation failed");
            teardown_session(s, slot, "virtual pad creation failed");
            return;
        }
        ss->pad_created = 1;
    }

    /* Server-side session bring-up: assigns session_id/pad_slot, moves the
     * FSM to ACTIVE (which is what arms the §8/§11 3-second idle timeout —
     * apad_session_tick only checks it in ACTIVE), clears the §9 receive
     * window for a reused slot, and prefills `w` so the WELCOME on the wire
     * can never drift from the session that issued it. session_id = slot
     * index + 1 is this server's own numbering scheme, not spec-mandated;
     * apad_session_server_accept only requires it be non-zero (§6.4).
     * h.desired_rate_hz == 0 becomes APAD_DEFAULT_RATE_HZ inside the call; a
     * client asking for more than APAD_MAX_RATE_HZ gets APAD_ERR_ARG rather
     * than a silent clamp, so it is rejected below instead of hidden. Only
     * reached for a session NOT already ACTIVE (the duplicate-HELLO fast
     * path above returns before this point), so the §9 receive-window reset
     * here only ever applies to a slot's first accept or a genuinely new
     * occupant of a reused slot -- never mid-session. */
    rc = apad_session_server_accept(&ss->core, (uint16_t)(slot + 1),
                                    (uint8_t)slot, h.desired_rate_hz, now, &w);
    if (rc < 0) {
        send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_MALFORMED,
                       (rc == APAD_ERR_ARG) ? "desired_rate_hz out of range"
                                            : "HELLO rejected");
        teardown_session(s, slot,
                         (rc == APAD_ERR_ARG) ? "requested input rate not supported"
                                              : "connection rejected");
        return;
    }
    /* §10. Outside a pairing window this whole block is skipped and
     * w.flags/w.server_nonce stay zero exactly as they always did:
     * AUTH_REQUIRED clear, no key, an unauthenticated session. Inside one,
     * this is the entire server half of the handshake.
     *
     * w.session_id/pad_slot/input_rate_hz/server_ticks_ms were prefilled by
     * apad_session_server_accept above; w.key_material stays zero because
     * §6.4 says it MUST be in v1 -- using it would be a v2 wire change
     * (docs/DESIGN.md D3 reserves it for a wrapped long-term key behind a
     * capability bit).
     *
     * The nonce is generated PER SESSION, not per window. Two clients
     * pairing off the same PIN must not end up with the same key: the PIN
     * is the shared secret and the nonce is what separates the sessions
     * derived from it, so a per-window nonce would give every client on the
     * LAN the identical session key and let each read and forge the
     * others' traffic for the price of the PIN they were all told anyway.
     * It is also why a client cannot precompute: PBKDF2's 10,000 iterations
     * cannot start until the WELCOME lands. */
    if (apad_pairing_is_open(&s->pairing, now)) {
        uint8_t nonce[APAD_NONCE_LEN];
        uint8_t key[APAD_SESSION_KEY_LEN];
        const char *secret = apad_pairing_secret(&s->pairing);

        if (secret == NULL || s->on_random == NULL
            || s->on_random(s->user, nonce, sizeof nonce) == 0) {
            /* No nonce, no salt, no derivation. There is no acceptable
             * substitute -- a predictable salt hands an attacker a
             * precomputable PBKDF2 over a 10^6 PIN space -- so the session
             * is refused and the window is closed rather than left open
             * issuing WELCOMEs this server cannot salt. */
            apad_secure_zero(nonce, sizeof nonce);
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_PAIRING_CLOSED,
                           "pairing unavailable: no entropy for server_nonce");
            apad_pairing_close(&s->pairing, &s->log,
                               "could not generate a secure key for a "
                               "connecting device");
            teardown_session(s, slot, "could not generate a secure key");
            return;
        }
        memcpy(w.server_nonce, nonce, sizeof w.server_nonce);
        w.flags = (uint8_t)(w.flags | APAD_WELCOME_AUTH_REQUIRED);

        /* Derive and stash the key, but do NOT install it until the WELCOME
         * has been sent: apad_session_set_key() sets session->authenticated,
         * apad_session_next_header() would then flag the WELCOME itself
         * AUTHENTICATED, and §10 is explicit that the tag starts on the
         * packet AFTER welcome -- WELCOME is the datagram that carries the
         * salt the client needs in order to compute a tag at all. */
        apad_derive_session_key(secret, nonce, key);

        wn = apad_encode_welcome(payload, sizeof payload, &w);
        if (wn < 0) {
            apad_secure_zero(key, sizeof key);
            apad_secure_zero(nonce, sizeof nonce);
            teardown_session(s, slot, "could not build the connection reply");
            return;
        }
        (void)send_via_session(s, ss, (uint8_t)APAD_MSG_WELCOME, payload,
                               (uint16_t)wn, now);

        apad_session_set_key(&ss->core, key);
        ss->auth_required   = 1u;
        ss->auth_verified   = 0u;
        ss->auth_charged    = 0u;
        ss->pair_generation = s->pairing.generation;
        apad_secure_zero(key, sizeof key);
        apad_secure_zero(nonce, sizeof nonce);

        apad_logf(&s->log, APAD_LOG_INFO,
                  "device \"%s\" (slot %d) needs a PIN to finish connecting",
                  ss->device_name, slot);
    } else {
        wn = apad_encode_welcome(payload, sizeof payload, &w);
        if (wn < 0) {
            teardown_session(s, slot, "could not build the connection reply");
            return;
        }
        (void)send_via_session(s, ss, (uint8_t)APAD_MSG_WELCOME, payload,
                               (uint16_t)wn, now);

        /* Name the matched profile here. "My edit does not apply" is
         * otherwise indistinguishable from "my edit applied but does not do
         * what I expected", and the difference is one substring match nobody
         * can see. */
        apad_logf(&s->log, APAD_LOG_INFO,
                  "device \"%s\" connected (slot %d), profile \"%s\"",
                  ss->device_name, slot,
                  (ss->profile != NULL) ? ss->profile->name : "none");
    }
}

/* v2 EXPERIMENT (branch experiment/touchmap-v2): tell the client what its
 * touchscreen currently maps to, so it can draw the real layout.
 *
 * Sent once, right after WELCOME, because that is the moment the session's
 * profile is decided and the client is about to render its first frame. A
 * profile can be hot-reloaded later (apad_profiles_load), which this does NOT
 * yet chase -- see the branch's open questions.
 *
 * Silent when the profile has no touch mapping: a client that hears nothing
 * keeps whatever it drew before, and "no message" is the honest encoding of
 * "this profile does not use the touchscreen".
 */
static void send_touchmap(apad_server *s, server_session *ss, uint32_t now)
{
    const apad_profile *p = ss->profile;
    apad_touchmap tm;
    uint8_t payload[APAD_LEN_TOUCHMAP];
    int n, i;

    if (p == NULL || p->touch.mode == APAD_TOUCH_MODE_NONE) {
        return;
    }

    memset(&tm, 0, sizeof tm);
    tm.mode = (uint8_t)p->touch.mode;
    tm.region_count = (uint8_t)((p->touch.region_count < 0) ? 0
                        : (p->touch.region_count > (int)APAD_TOUCHMAP_MAX_REGIONS
                           ? (int)APAD_TOUCHMAP_MAX_REGIONS
                           : p->touch.region_count));
    for (i = 0; i < (int)tm.region_count; i++) {
        const apad_touch_region *r = &p->touch.regions[i];
        /* 0..1 -> 0..255. The profile's rect is already normalised and +Y
         * down (profiles.h), which is the wire convention too, so this is a
         * scale and nothing else -- no axis flip to get wrong. */
        tm.regions[i].x0 = (uint8_t)(r->rect[0] * 255.0 + 0.5);
        tm.regions[i].y0 = (uint8_t)(r->rect[1] * 255.0 + 0.5);
        tm.regions[i].x1 = (uint8_t)(r->rect[2] * 255.0 + 0.5);
        tm.regions[i].y1 = (uint8_t)(r->rect[3] * 255.0 + 0.5);
        tm.regions[i].target  = (uint8_t)r->target;
        tm.regions[i].analog  = (uint8_t)(r->analog ? 1 : 0);
        tm.regions[i].pad_bit = r->pad_bit;
    }
    n = apad_encode_touchmap(payload, sizeof payload, &tm);
    if (n < 0) {
        return;
    }
    (void)send_via_session(s, ss, (uint8_t)APAD_MSG_TOUCHMAP, payload,
                           (uint16_t)n, now);
    apad_logf(&s->log, APAD_LOG_INFO,
              "sent touch layout to \"%s\": %u region(s), profile \"%s\"",
              ss->device_name, (unsigned)tm.region_count, p->name);
}

static void handle_input_state(apad_server *s, const apad_addr *from,
                               const apad_packet *pkt, uint32_t now)
{
    int slot;
    server_session *ss;
    apad_input_state st;
    apad_pad_state pad;
    int rc;

    slot = find_session_by_id(s, pkt->header.session_id);
    if (slot < 0) {
        return;   /* unknown session: silently drop the high-rate stream */
    }
    ss = &s->sessions[slot];
    if (!apad_addr_equal(&ss->peer, from)) {
        return;   /* source doesn't match the session's peer; ignore */
    }

    rc = apad_session_on_recv(&ss->core, pkt, now);
    if (rc != APAD_OK) {
        return;   /* §9: stale INPUT_STATE discarded, session stays alive */
    }

    rc = apad_decode_input_state(pkt->payload, pkt->payload_len, &st);
    if (rc < 0) {
        return;
    }

    /* apad_decode_input_state() already applied §5.5's normalisation
     * (101..254 -> 255/APAD_BATTERY_UNKNOWN), so this is directly the value
     * apad_client_info::battery should show. */
    ss->last_battery = st.battery;

    /* apad_server_last_input() (apadserver.h): a raw snapshot of THIS
     * decoded struct, taken here -- before apad_mapping_apply() below ever
     * touches it -- so a UI watching it sees exactly what the client sent,
     * never a profile's interpretation of it. */
    ss->last_input      = st;
    ss->input_frame++;
    ss->have_last_input = 1u;

    apad_mapping_apply(&st, ss->caps, ss->profile, &ss->map_state, &pad);
    (void)s->backend->update_pad(slot, &pad);
}

static void handle_ping(apad_server *s, const apad_addr *from,
                        const apad_packet *pkt, uint32_t now)
{
    int slot;
    server_session *ss;
    apad_ping in, out;
    uint8_t payload[APAD_LEN_PONG];
    int rc, n;

    slot = find_session_by_id(s, pkt->header.session_id);
    if (slot < 0) {
        if (pkt->header.session_id != 0u) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                           "unknown session");
        }
        return;
    }
    ss = &s->sessions[slot];
    if (!apad_addr_equal(&ss->peer, from)) {
        return;
    }
    (void)apad_session_on_recv(&ss->core, pkt, now);

    rc = apad_decode_ping(pkt->payload, pkt->payload_len, &in);
    if (rc < 0) {
        return;
    }
    memset(&out, 0, sizeof out);
    out.origin_ticks_ms     = in.origin_ticks_ms;   /* §6.6: echoed unchanged */
    out.responder_ticks_ms  = now;

    n = apad_encode_ping(payload, sizeof payload, &out);
    if (n < 0) {
        return;
    }
    (void)send_via_session(s, ss, (uint8_t)APAD_MSG_PONG, payload,
                           (uint16_t)n, now);
}

/*
 * The other half of the RTT measurement apad_server_tick() originates
 * (apadserver.h's doc comment on apad_server_tick): a PONG answering THIS
 * server's own §6.6 PING. §6.6: "Correlation is by origin_ticks_ms alone" --
 * a PONG carries its own header sequence, not the PING's, so the sequence
 * number is not usable for this and the payload's echoed origin_ticks_ms is
 * the only link back to the PING that caused it.
 *
 * Distinct from handle_ping() above (which answers a client-ORIGINATED
 * PING): §6.6 makes both directions legal on the same message pair, and a
 * session may be doing both at once -- a client sending its own 1 Hz PING
 * while this server sends its own. Nothing here interferes with that path;
 * handle_ping()'s PONG reply and this session's ping_pending/ping_origin_ms
 * bookkeeping are independent.
 */
static void handle_pong(apad_server *s, const apad_addr *from,
                        const apad_packet *pkt, uint32_t now)
{
    int slot;
    server_session *ss;
    apad_ping in;

    slot = find_session_by_id(s, pkt->header.session_id);
    if (slot < 0) {
        if (pkt->header.session_id != 0u) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                           "unknown session");
        }
        return;
    }
    ss = &s->sessions[slot];
    if (!apad_addr_equal(&ss->peer, from)) {
        return;
    }
    (void)apad_session_on_recv(&ss->core, pkt, now);   /* §8: refresh liveness */

    if (apad_decode_ping(pkt->payload, pkt->payload_len, &in) < 0) {
        return;
    }
    /* Not this session's outstanding PING (a stray/duplicate PONG, or one
     * that arrived after apad_server_tick() already started a newer PING) --
     * ignore rather than misattribute the RTT. apad_time_since() is
     * wrap-safe (docs/CONVENTIONS.md); a raw != would still be correct for equality
     * but time_since is used below anyway so the comparison stays in one
     * idiom. */
    if (!ss->ping_pending || in.origin_ticks_ms != ss->ping_origin_ms) {
        return;
    }
    ss->rtt_ms = apad_time_since(now, ss->ping_origin_ms);
    ss->ping_pending = 0u;
}

static void handle_bye(apad_server *s, const apad_addr *from,
                       const apad_packet *pkt, uint32_t now)
{
    int slot = find_session_by_id(s, pkt->header.session_id);
    server_session *ss;
    apad_bye bye;
    char reason_text[48];
    const char *reason_str = "unparseable";

    if (slot < 0) {
        /* §8: unknown non-zero session_id -> ERROR 7, no session created.
         * INPUT_STATE is the sole silent exception (self-DoS risk at up to
         * 125 pkt/s); BYE has no such volume, so it complies literally. */
        if (pkt->header.session_id != 0u) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                           "unknown session");
        }
        return;
    }
    ss = &s->sessions[slot];
    if (!apad_addr_equal(&ss->peer, from)) {
        return;
    }
    /* §9 "What discharges a reliable message" + "Duplicates": WELCOME is
     * the only defined direct answer in v1 (to HELLO). BYE has none, so it
     * MUST be acknowledged explicitly, and every copy (including a
     * retransmitted duplicate BYE) MUST be ACKed, not just the first --
     * a receiver that only ACKs the first copy kills the session on the
     * first lost ACK.
     *
     * KNOWN DIVERGENCE from that last rule, recorded rather than papered
     * over: only the FIRST copy of a BYE is ACKed here. teardown_session()
     * below frees the slot, so find_session_by_id() returns -1 for a
     * retransmitted BYE and it falls out through the `slot < 0` branch above
     * as ERROR 7 -- not the ACK §9 requires for every copy. (An earlier
     * version of this comment cited that same -1 as the reason ACKing a
     * duplicate was harmless; it is in fact the mechanism by which the
     * duplicate is never ACKed at all.) Closing it properly needs a tombstone
     * of recently-closed session ids so a post-teardown BYE can still be
     * recognised and answered, which is a design change and not a local fix.
     * Impact is low: the peer that sent the BYE is exiting, and its own §9
     * retries cost it four datagrams before it gives up on a session this
     * server has already torn down. */
    send_ack(s, now, ss, pkt->header.sequence);
    (void)apad_session_on_recv(&ss->core, pkt, now);   /* closes the FSM */

    /* BUG FIXED (teardown-observability audit): this used to be a bare
     * free_session(slot) call with nothing logged at all. Every client in
     * this tree sends a best-effort BYE on a clean exit (§6.5), so a normal
     * app close was torn down completely silently -- indistinguishable in
     * the log from the session never having existed. Decode the reason the
     * PEER gave for closing (not ss->core.close_reason, which free_session()
     * -> apad_session_close() below unconditionally overwrites to
     * APAD_CLOSE_LOCAL regardless of why the session actually ended). */
    if (apad_decode_bye(pkt->payload, pkt->payload_len, &bye) >= 0) {
        reason_str = bye_reason_str(bye.reason);
        (void)snprintf(reason_text, sizeof reason_text, "%s", reason_str);
    } else {
        (void)snprintf(reason_text, sizeof reason_text, "reason unclear");
    }
    teardown_session(s, slot, reason_text);
}

static void handle_ack(apad_server *s, const apad_addr *from,
                       const apad_packet *pkt, uint32_t now)
{
    int slot = find_session_by_id(s, pkt->header.session_id);
    server_session *ss;

    if (slot < 0) {
        if (pkt->header.session_id != 0u) {
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                           "unknown session");
        }
        return;
    }
    ss = &s->sessions[slot];
    if (!apad_addr_equal(&ss->peer, from)) {
        return;
    }
    (void)apad_session_on_recv(&ss->core, pkt, now);   /* clears retx_armed */

    /* v2 EXPERIMENT: the layout goes out HERE, not straight after WELCOME.
     * §9 allows one reliable message in flight and a second arm REPLACES the
     * first, so arming a reliable TOUCHMAP a line after WELCOME would cancel
     * WELCOME's own retransmit -- trading a reliably-delivered layout for an
     * unreliable handshake. Once this ACK has discharged the WELCOME the slot
     * is free, and the client has demonstrably received it. */
    /* Mark, do not send: apad_server_tick() owns delivery and repetition,
     * so connect and profile-edit take the identical path. */
    ss->touchmap_dirty = 1u;
}

/* Tear down every session that was issued an AUTH_REQUIRED WELCOME and has
 * not yet produced a datagram whose tag verified. Called when the pairing
 * window closes -- by expiry (§11's 120 s) or by the host cancelling.
 *
 * §10: the secret is valid "for 120 seconds after the user initiates
 * pairing, and never otherwise", so a handshake that has not proved
 * knowledge of it before the window shuts has not been authorised, and
 * leaving it holding a virtual pad would make the window's end
 * meaningless. A session that DID authenticate is left alone: it is paired,
 * its key is derived, and closing the window is not an instruction to
 * disconnect the client that just used it. */
static void drop_unproven_sessions(apad_server *s, const char *why)
{
    int i;
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        server_session *ss = &s->sessions[i];
        if (ss->in_use && ss->auth_required && !ss->auth_verified) {
            teardown_session(s, i, why);
        }
    }
}

/*
 * §3.1 check 7 -- "if AUTHENTICATED, the tag verifies (§10); on failure
 * reject, ERROR code 3" -- plus its mirror image, which §3.1 does not state
 * because it is §10's rule rather than a framing rule: a session that was
 * issued an AUTH_REQUIRED WELCOME accepts NOTHING unauthenticated
 * afterwards.
 *
 * Returns 1 to continue dispatching this datagram, 0 to drop it. A 0 return
 * may mean the session was torn down, so the caller must not touch it after.
 *
 * BOTH halves matter and the second is the one that would be easy to
 * forget. Verifying tags when they are present, while still accepting
 * untagged INPUT_STATE on the same session, is not authentication at all --
 * an attacker would simply omit the flag. §10 says "every packet after
 * WELCOME sets AUTHENTICATED", so on an auth-required session the absence
 * of the flag is as wrong as a bad tag.
 *
 * The no-window path is untouched by all of this: no session ever has a key
 * or auth_required, so the first branch falls into the same silent drop the
 * server has always done for an unverifiable AUTHENTICATED datagram, and
 * the second branch never fires.
 */
static int check_auth(apad_server *s, uint32_t now, const apad_addr *from,
                      const apad_packet *pkt, const uint8_t *buf, size_t len)
{
    int slot = find_session_by_id(s, pkt->header.session_id);
    server_session *ss = (slot >= 0) ? &s->sessions[slot] : NULL;

    /* UI rx_packets (apadserver.h apad_client_info): every datagram that
     * names a known session AND actually comes from that session's peer,
     * counted once here rather than at each handle_*() below -- this is the
     * one place every session-bearing datagram passes through regardless of
     * type. Deliberately gated on the peer address matching, same as every
     * handler below does independently: a spoofed session_id from a
     * different source must not inflate this session's counter. Two classes
     * this does NOT count, both by design: DISCOVER/HELLO (session_id is
     * always 0 by §6.1/§6.3, so there is no session yet to attribute them
     * to -- the handshake is 1-2 datagrams and not what this counter is
     * for) and anything §3.1 already rejected before check_auth() runs
     * (apad_server_on_datagram() returns before calling this at all). */
    if (ss != NULL && apad_addr_equal(&ss->peer, from)) {
        ss->rx_packets++;
    }

    if ((pkt->header.flags & APAD_FLAG_AUTHENTICATED) != 0u) {
        if (ss == NULL || !ss->core.have_key
            || !apad_addr_equal(&ss->peer, from)) {
            /* Nothing to verify against. Silently ignored, exactly as this
             * server has always treated an AUTHENTICATED datagram, and
             * deliberately NOT answered with ERROR 7 even for an unknown
             * session_id: a §3.1 check-7 failure and an unknown session are
             * indistinguishable from here, and answering would add a
             * 76-byte reply to an unauthenticated datagram anyone can
             * forge. */
            return 0;
        }
        if (apad_packet_verify(buf, len, ss->core.key,
                               (size_t)APAD_SESSION_KEY_LEN) != APAD_OK) {
            uint32_t gen = ss->pair_generation;
            int      charge = ss->auth_required && !ss->auth_charged;

            /* ONE attempt per session, not one per datagram. See the
             * apad_pairing_fail() call site comment below and pairing.c. */
            if (charge) {
                ss->auth_charged = 1u;
            }
            send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_AUTH_FAILED,
                           "authentication failed (S10 tag)");
            teardown_session(s, slot, "authentication check failed");
            if (charge) {
                (void)apad_pairing_fail(&s->pairing, gen, s->on_random,
                                        s->user, &s->log);
            }
            return 0;
        }
        ss->auth_verified = 1u;
        return 1;
    }

    if (ss != NULL && ss->auth_required && apad_addr_equal(&ss->peer, from)) {
        /* §10, the one explicit exemption: the ACK that discharges WELCOME
         * MAY be unauthenticated, so let it through while this session has
         * not yet authenticated.
         *
         * It is not a courtesy, it is forced. WELCOME is the datagram
         * CARRYING the server_nonce used as the PBKDF2 salt, so a client
         * cannot hold a key until it has parsed the very message its ACK
         * acknowledges -- and deriving takes 10,000 iterations. Measured at
         * 17 ms here at -O2 and 86 ms at -O0; a 67 MHz ARM9 is order-of-
         * seconds. Requiring a tag on this one ACK would put that derivation
         * inside §9's 100/200/400/800 ms schedule and kill the session at
         * t=2300 ms as APAD_CLOSE_RETX_FAILED -- on exactly the constrained
         * clients this protocol exists for, and with input never having
         * flowed, so it would read as an unexplained handshake failure.
         *
         * Nothing is trusted by allowing it. An ACK carries only a sequence
         * number; the worst an off-path forger achieves is stopping a
         * retransmit of a WELCOME the real client has already received or
         * already missed. Every datagram that CHANGES anything still needs a
         * verifying tag.
         */
        if (pkt->header.type == (uint8_t)APAD_MSG_ACK && !ss->auth_verified) {
            return 1;
        }

        /* Anything else untagged on a session that requires tags is dropped
         * silently and NOT counted as a pairing attempt: it is a client
         * ignoring AUTH_REQUIRED, not somebody guessing the PIN, and at up
         * to 125 INPUT_STATE per second an ERROR reply here would be the
         * same self-inflicted amplifier §8 forbids for unknown-session
         * INPUT_STATE. */
        return 0;
    }
    return 1;
}

/*
 * One received datagram, dispatched. Returns APAD_OK when §3.1 accepted the
 * datagram (whatever the handler then decided to do with it) and the
 * negative apad_packet_parse() code when §3.1 rejected it -- informational
 * only, since a rejected datagram is already either dropped silently or
 * answered with an ERROR right here, exactly as §8 requires. Was static
 * handle_datagram() when the socket lived in this file.
 *
 * `now_ms` is this datagram's arrival time on the host's clock, on the same
 * monotonic timebase as apad_server_tick(). It is a parameter and not the
 * last tick's stashed value on purpose -- see the struct comment above and
 * apadserver.h. There is no ordering requirement between this call and
 * apad_server_tick().
 */
int apad_server_on_datagram(apad_server *s, uint32_t now_ms,
                            const apad_addr *from,
                            const uint8_t *buf, size_t len)
{
    apad_packet pkt;
    uint32_t now = now_ms;
    int rc;

    if (s == NULL || from == NULL || buf == NULL) {
        return APAD_ERR_ARG;
    }
    rc = apad_packet_parse(buf, len, &pkt);

    switch (rc) {
    case APAD_ERR_TRUNCATED:
    case APAD_ERR_MAGIC:
    case APAD_ERR_TYPE:
        return rc;   /* §3.1 checks 1, 2, 5: discard silently */
    case APAD_ERR_VERSION:
        send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_VERSION_MISMATCH,
                       "protocol version mismatch");
        return rc;
    case APAD_ERR_LENGTH:
        send_error_raw(s, now, from, NULL, (uint16_t)APAD_ERRC_MALFORMED,
                       "length/payload_len mismatch");
        return rc;
    default:
        if (rc < 0) {
            return rc;
        }
        break;
    }

    /* Keep pairing's idea of "now" fresh for the UI countdown, but do NOT
     * let it reap an expired window from here: the reap tears sessions
     * down, and doing that in the middle of the datagram path would pull
     * the session out from under the handler about to run. Expiry belongs
     * to apad_server_tick(); apad_pairing_is_open() is deadline-authoritative
     * anyway, so an unreaped window can never issue a key. */
    apad_pairing_observe(&s->pairing, now);

    /* §3.1 check 7 and §10's "every packet after WELCOME is authenticated".
     * With no pairing window open this is the same silent drop of an
     * unverifiable AUTHENTICATED datagram the server has always done. */
    if (!check_auth(s, now, from, &pkt, buf, len)) {
        return APAD_OK;
    }

    switch (pkt.header.type) {
    case APAD_MSG_DISCOVER:
        handle_discover(s, now, from, &pkt);
        break;
    case APAD_MSG_HELLO:
        handle_hello(s, from, &pkt, now);
        break;
    case APAD_MSG_INPUT_STATE:
        handle_input_state(s, from, &pkt, now);
        break;
    case APAD_MSG_PING:
        handle_ping(s, from, &pkt, now);
        break;
    case APAD_MSG_PONG:
        handle_pong(s, from, &pkt, now);
        break;
    case APAD_MSG_BYE:
        handle_bye(s, from, &pkt, now);
        break;
    case APAD_MSG_ACK:
        handle_ack(s, from, &pkt, now);
        break;
    default: {
        /* ANNOUNCE/WELCOME/RUMBLE/LED/STATUS/ERROR are server->client; a
         * client sending one is nonstandard, but §8's unknown-session rule
         * is type-agnostic (INPUT_STATE is the sole silent exception), so it
         * still applies here rather than being a special case. PONG is NOT
         * in this list -- §6.6 makes it legal "either" direction and it now
         * has its own case above (handle_pong(), correlating this server's
         * own §6.6 PING for the RTT the UI shows). */
        int slot = find_session_by_id(s, pkt.header.session_id);
        if (slot < 0) {
            if (pkt.header.session_id != 0u) {
                send_error_raw(s, now, from, NULL,
                               (uint16_t)APAD_ERRC_UNKNOWN_SESSION,
                               "unknown session");
            }
        } else if (apad_addr_equal(&s->sessions[slot].peer, from)) {
            /* §8: "any datagram that passes §3.1 refreshes the idle
             * timer" -- this one does, even though nothing here otherwise
             * acts on it. Deliberately NOT apad_session_on_recv(): its
             * APAD_MSG_WELCOME case unconditionally ADOPTS session_id/
             * pad_slot/input_rate_hz and sets state ACTIVE regardless of
             * is_server (core/session.c is the client-side adoption path,
             * reused as-is on the server) -- calling it here on a
             * wrong-direction WELCOME would let a spoofed one overwrite
             * this server's own session bookkeeping. A direct liveness
             * refresh is the narrowest correct action for a message class
             * that has nothing else defined to do. */
            s->sessions[slot].core.last_rx_ms = now;
        }
        break;
    }
    }
    return APAD_OK;
}

/*
 * Advance the clock: §9 retransmits and §8 timeouts. Was static
 * tick_sessions(), called from main()'s loop with a clock it read itself;
 * the clock is the host's to supply now. Nothing is stashed -- the datagram
 * path takes its own `now_ms`, so the two entry points are independent and a
 * host may order and interleave them however its loop happens to be shaped.
 */
int apad_server_tick(apad_server *s, uint32_t now_ms)
{
    int i;

    if (s == NULL) {
        return APAD_ERR_ARG;
    }

    /* v2 EXPERIMENT: deliver the touch layout.
     *
     * TOUCHMAP is deliberately UNRELIABLE -- §9 delivery would let a client
     * that does not ACK it be torn down, which is exactly what happened to a
     * test client that claimed support and stayed silent. An unreliable
     * message costs a lost layout at worst, never a lost session.
     *
     * Robustness comes from repetition instead: three copies about 250 ms
     * apart. 68 bytes each, on a LAN where the loss that would drop all
     * three is the kind that breaks the session anyway. A profile edit
     * re-arms the same three. */
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        server_session *ss = &s->sessions[i];

        if (!ss->in_use) {
            continue;
        }
        if (ss->touchmap_dirty) {
            ss->touchmap_dirty   = 0;
            ss->touchmap_repeats = 3u;
            ss->touchmap_next_ms = now_ms;
        }
        if (ss->touchmap_repeats > 0u &&
            apad_time_after(now_ms, ss->touchmap_next_ms)) {
            ss->touchmap_repeats--;
            ss->touchmap_next_ms = now_ms + 250u;
            send_touchmap(s, ss, now_ms);
        }
    }

    /* §11's 120 s window. Reaped here rather than in the datagram path
     * because this is the entry point allowed to tear sessions down. */
    if (apad_pairing_tick(&s->pairing, now_ms, &s->log)) {
        drop_unproven_sessions(s, "pairing window expired before the "
                                  "handshake authenticated");
    }

    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        server_session *ss = &s->sessions[i];
        int act;

        if (!ss->in_use) {
            continue;
        }
        act = apad_session_tick(&ss->core, now_ms);
        switch (act) {
        case APAD_ACT_RETRANSMIT:
            if (ss->retx_len > 0u) {
                (void)emit(s, &ss->peer, ss->retx_buf, ss->retx_len,
                           "S9 retransmit", ss);
            }
            break;
        case APAD_ACT_TIMEOUT:
            teardown_session(s, i,
                             ss->core.close_reason == APAD_CLOSE_RETX_FAILED
                                 ? "reliable delivery failed"
                                 : "idle timeout");
            break;
        default:
            break;
        }

        /* §6.6 RTT origination, once per ACTIVE session per second -- see
         * apad_server_tick()'s own doc comment in apadserver.h for why this
         * server originates PING at all now, not just answers one.
         * `ss->in_use` is rechecked because APAD_ACT_TIMEOUT above may have
         * just freed this slot via teardown_session(); a freed slot must not
         * have a PING built against its (now cleared) session_id/tx_seq.
         * apad_time_since() is wrap-safe (docs/CONVENTIONS.md).
         *
         * `!ss->core.retx_armed`: do not add unrelated traffic while a §9
         * reliable message (in practice, only ever WELCOME on the server's
         * send side) is still outstanding and being retransmitted on its
         * own tight 100/200/400/800 ms schedule. Nothing about PING/PONG's
         * sequence-number space actually conflicts with that schedule --
         * they are unrelated messages -- but a session that has not yet
         * proven its handshake landed is exactly the wrong moment to be
         * optimistic about spending an extra datagram on housekeeping, and
         * withholding it here costs the UI nothing but the first second of
         * a "measuring..." RTT display. */
        if (ss->in_use && ss->core.state == (uint8_t)APAD_SESSION_ACTIVE
            && !ss->core.retx_armed
            && apad_time_since(now_ms, ss->last_ping_sent_ms) >= 1000u) {
            apad_ping out;
            uint8_t   payload[APAD_LEN_PING];
            int       n;

            memset(&out, 0, sizeof out);
            out.origin_ticks_ms    = now_ms;
            out.responder_ticks_ms = 0u;   /* §6.6: MUST be zero in a PING */

            n = apad_encode_ping(payload, sizeof payload, &out);
            if (n >= 0) {
                /* Return ignored on purpose, same as every other
                 * send_via_session() call in this file: emit()'s failure is
                 * diagnostic only (apadserver.h on_send doc comment), and
                 * §6.6 PING/PONG isn't RELIABLE anyway -- a lost PING just
                 * means this second's RTT sample is missing, which the next
                 * one 1 s later replaces. What matters is that
                 * last_ping_sent_ms advances regardless, so a socket error
                 * cannot turn into a PING sent every tick instead of once a
                 * second. */
                (void)send_via_session(s, ss, (uint8_t)APAD_MSG_PING, payload,
                                       (uint16_t)n, now_ms);
                ss->last_ping_sent_ms = now_ms;
                ss->ping_origin_ms    = now_ms;
                ss->ping_pending      = 1u;
            }
        }
    }
    return APAD_OK;
}

/* Advertised in ANNOUNCE when the host does not name itself. Was a literal
 * inside handle_discover() before the split; it is host-shaped configuration
 * (a tray application will want the machine's name here), so cfg owns it and
 * this is only the fallback. */
#define APAD_SERVER_DEFAULT_NAME "AtticPad Server"

apad_server *apad_server_create(const apad_server_cfg *cfg,
                                const apad_backend *backend)
{
    apad_server *s;

    if (cfg == NULL || backend == NULL || cfg->on_send == NULL) {
        return NULL;   /* nothing to log through yet, or nowhere to send */
    }
    /* Server code is ordinary hosted C: malloc is fine here and always was
     * (docs/CONVENTIONS.md). core/'s no-malloc rule stops at core/ and shim/. */
    s = calloc(1, sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    s->backend     = backend;
    s->on_send     = cfg->on_send;
    s->on_random   = cfg->on_random;   /* NULL is legal; pairing is then refused */
    s->log.fn      = cfg->on_log;
    s->log.user    = cfg->user;
    s->user        = cfg->user;
    s->server_port = cfg->server_port;
    (void)snprintf(s->server_name, sizeof s->server_name, "%s",
                   (cfg->server_name != NULL && cfg->server_name[0] != '\0')
                       ? cfg->server_name
                       : APAD_SERVER_DEFAULT_NAME);

    /* JSONC per-device profiles (docs/DESIGN.md §6.2, server/src/profiles.c), as
     * memory blobs -- the library does no filesystem access, so finding and
     * reading the files is the host's half of this (apadserver.h). A blob
     * that fails to parse is logged and skipped, never fatal: profiles.c
     * always keeps the compiled-in default available. */
    apad_profiles_load(cfg->profiles, cfg->profile_count, &s->log);

    /* §7 tier 2 subnet-broadcast filtering (is_bad_reply_target() above).
     * Copied into a fixed table for the same reason profiles are: the
     * host's array need not outlive this call. Only .ip is ever read back
     * out of it. */
    if (cfg->broadcast_addrs != NULL) {
        size_t i;
        for (i = 0; i < cfg->broadcast_addr_count; i++) {
            if (s->broadcast_addr_count >= APAD_MAX_BROADCAST_ADDRS) {
                apad_logf(&s->log, APAD_LOG_WARN,
                          "cfg.broadcast_addrs: %u address(es) exceed the "
                          "%u this server tracks -- the rest are ignored "
                          "for §7 subnet-broadcast filtering",
                          (unsigned)cfg->broadcast_addr_count,
                          (unsigned)APAD_MAX_BROADCAST_ADDRS);
                break;
            }
            s->broadcast_addrs[s->broadcast_addr_count] = cfg->broadcast_addrs[i];
            s->broadcast_addr_count++;
        }
    }

    if (backend->init() != 0) {
        apad_logf(&s->log, APAD_LOG_ERROR,
                  "backend \"%s\" init failed (no /dev/uinput access?)",
                  backend->name);
        free(s);
        return NULL;
    }
    return s;
}

/* ======================================================================== */
/* §10 pairing — the host-facing half. Policy is in server/src/pairing.c.    */
/* ======================================================================== */

int apad_server_begin_pairing(apad_server *s, uint32_t now_ms, int use_token)
{
    int rc;

    if (s == NULL) {
        return APAD_ERR_ARG;
    }
    /* Replacing a window invalidates the secret every in-flight handshake
     * was derived from, so those sessions go the same way they go when a
     * window expires. Done BEFORE the new window opens, so a session
     * created by the new secret can never be caught by it. Sessions that
     * already authenticated survive: re-pairing a second device is not a
     * reason to disconnect the first. */
    if (s->pairing.open) {
        drop_unproven_sessions(s, "pairing window replaced before the "
                                  "handshake authenticated");
    }
    rc = apad_pairing_open(&s->pairing, now_ms, use_token, s->on_random,
                           s->user, &s->log);
    return rc;
}

void apad_server_cancel_pairing(apad_server *s, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    apad_pairing_observe(&s->pairing, now_ms);
    if (s->pairing.open) {
        apad_pairing_close(&s->pairing, &s->log, "cancelled by the host");
        drop_unproven_sessions(s, "pairing cancelled before the handshake "
                                  "authenticated");
    }
}

int apad_server_pairing_state(const apad_server *s, apad_pairing_info *out)
{
    if (s == NULL || out == NULL) {
        return APAD_ERR_ARG;
    }
    apad_pairing_snapshot(&s->pairing, out);
    return APAD_OK;
}

/* ======================================================================== */
/* Server UI query API -- apadserver.h                                      */
/* ======================================================================== */

int apad_server_list_clients(const apad_server *s, apad_client_info *out,
                             size_t max)
{
    int i;
    size_t n = 0;

    if (s == NULL) {
        return 0;
    }
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        const server_session *ss = &s->sessions[i];

        if (!ss->in_use) {
            continue;
        }
        if (out != NULL && n < max) {
            apad_client_info *c = &out[n];

            memset(c, 0, sizeof *c);
            c->slot          = (uint8_t)i;
            c->session_id    = ss->core.session_id;
            (void)snprintf(c->device_name, sizeof c->device_name, "%s",
                          ss->device_name);
            c->peer          = ss->peer;
            c->caps          = ss->caps;
            (void)snprintf(c->profile_name, sizeof c->profile_name, "%s",
                          (ss->profile != NULL) ? ss->profile->name : "");
            c->rtt_ms        = ss->rtt_ms;
            c->battery       = ss->last_battery;
            c->authenticated = ss->core.have_key;
            c->rx_packets    = ss->rx_packets;
            c->tx_packets    = ss->tx_packets;
        }
        n++;
    }
    return (int)n;
}

int apad_server_backend_status(const apad_server *s, apad_backend_status *out)
{
    apad_backend_health health;

    if (s == NULL || out == NULL) {
        return APAD_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    (void)snprintf(out->name, sizeof out->name, "%s", s->backend->name);

    /* health() is optional (backend.h): no hook means "nothing more
     * specific than init() already said", which by construction is "it
     * worked" -- this server object only exists because backend->init()
     * succeeded. */
    if (s->backend->health == NULL) {
        out->ok    = 1;
        out->state = APAD_BACKEND_HEALTH_OK;
        return APAD_OK;
    }

    memset(&health, 0, sizeof health);
    s->backend->health(&health);
    out->ok    = (health.state == APAD_BACKEND_HEALTH_OK) ? 1 : 0;
    out->state = health.state;
    (void)snprintf(out->message, sizeof out->message, "%s",
                   (health.message != NULL) ? health.message : "");
    if (health.remedy != NULL) {
        (void)snprintf(out->remedy, sizeof out->remedy, "%s", health.remedy);
    }
    return APAD_OK;
}

int apad_server_set_profile(apad_server *s, uint8_t slot,
                            const char *profile_name)
{
    const apad_profile *p;

    if (s == NULL || profile_name == NULL || !slot_valid((int)slot)) {
        return APAD_ERR_ARG;
    }
    if (!s->sessions[slot].in_use) {
        return APAD_ERR_STATE;
    }
    p = apad_profiles_find(profile_name);
    if (p == NULL) {
        return APAD_ERR_ARG;
    }
    s->sessions[slot].profile = p;
    s->sessions[slot].profile_pinned = 1u;
    /* v2 EXPERIMENT: switching profiles changes the touch layout just as much
     * as editing one does, so the client has to be told. Missing this was the
     * third of three ways to change a mapping -- connect and save both
     * resent, and only picking an existing profile silently did not. */
    s->sessions[slot].touchmap_dirty = 1u;
    apad_logf(&s->log, APAD_LOG_INFO,
              "device (slot %u): profile switched to \"%s\"",
              (unsigned)slot, p->name);
    return APAD_OK;
}

int apad_server_last_input(const apad_server *s, uint8_t slot,
                           apad_input_state *out, uint32_t *out_frame)
{
    const server_session *ss;

    if (s == NULL || out == NULL || !slot_valid((int)slot)) {
        return APAD_ERR_ARG;
    }
    ss = &s->sessions[slot];
    if (!ss->in_use) {
        return APAD_ERR_ARG;
    }
    if (!ss->have_last_input) {
        memset(out, 0, sizeof *out);
        if (out_frame != NULL) {
            *out_frame = 0u;
        }
        return APAD_ERR_STATE;
    }
    *out = ss->last_input;
    if (out_frame != NULL) {
        *out_frame = ss->input_frame;
    }
    return APAD_OK;
}

int apad_server_reload_profiles(apad_server *s, const apad_profile_source *sources,
                                size_t count)
{
    /* Captured BEFORE apad_profiles_load() runs -- see this function's own
     * apadserver.h doc comment. apad_profiles_load() overwrites the
     * file-static g_profiles[] table IN PLACE (profiles.c, and the struct
     * apad_server comment above documents the hazard this guards against):
     * a session's existing `const apad_profile *` points INTO that table, so
     * reading ss->profile->name AFTER the reload would risk attributing
     * session i's OLD identity to whatever profile the reload happened to
     * place at the same array slot, not to the profile this session was
     * actually matched against a moment ago. */
    char old_names[APAD_MAX_SESSIONS][APAD_PROFILE_NAME_LEN];
    int  had_profile[APAD_MAX_SESSIONS];
    int  i;

    if (s == NULL) {
        return APAD_ERR_ARG;
    }

    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        const server_session *ss = &s->sessions[i];
        had_profile[i] = (ss->in_use && ss->profile != NULL);
        if (had_profile[i]) {
            (void)snprintf(old_names[i], sizeof old_names[i], "%s",
                           ss->profile->name);
        } else {
            old_names[i][0] = '\0';
        }
    }

    apad_profiles_load(sources, count, &s->log);

    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        server_session *ss = &s->sessions[i];
        const apad_profile *newp;

        if (!ss->in_use) {
            continue;
        }
        /* 1. Exact match on the OLD name first, so a file edited in place
         * (deadzone tweak, a new touch region -- the common case) keeps this
         * session on "the same profile, freshly reloaded" rather than
         * falling through to a substring re-match it does not need.
         * 2. apad_profiles_match() on the session's own device_name
         * otherwise -- the file was renamed or deleted, so this lands the
         * session exactly where a brand-new HELLO from the same device
         * would land today (apad_profiles_match() never returns NULL). */
        /* Land this session where a brand-new HELLO from the same device
         * would land NOW. Keeping the old name instead -- which is what this
         * did until 0.4.0 -- meant a profile the user had just created or
         * edited could never take over a live session: the old name still
         * resolved, so the session stayed on it and the edit appeared to do
         * nothing until a reconnect. That is the single most confusing thing
         * the editor can do, because the file on disk plainly says otherwise.
         *
         * A profile a HUMAN picked in the UI is the exception and stays put:
         * re-matching over a deliberate choice would be the same bug with
         * the roles reversed. */
        if (ss->profile_pinned && had_profile[i]) {
            newp = apad_profiles_find(old_names[i]);
            if (newp == NULL) {
                newp = apad_profiles_match(ss->device_name);
            }
        } else {
            newp = apad_profiles_match(ss->device_name);
        }
        if (strcmp(old_names[i], newp->name) != 0) {
            apad_logf(&s->log, APAD_LOG_INFO,
                      "device (slot %d): profile changed from \"%s\" to \"%s\"",
                      i, had_profile[i] ? old_names[i] : "(none)", newp->name);
        }
        ss->profile = newp;
        /* v2 EXPERIMENT: the layout this client is drawing is now stale --
         * a region moved, a profile was edited, or it matched a different
         * file entirely. Marked rather than sent, because this function has
         * no clock: apadserver.h is explicit that the host supplies now_ms,
         * and send_via_session() needs one. apad_server_tick() flushes it. */
        ss->touchmap_dirty = 1;
    }
    return APAD_OK;
}

void apad_server_destroy(apad_server *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    /* Every live session goes through teardown_session(), which is the one
     * shared exit point (see its comment above): main()'s old shutdown loop
     * called free_session() directly and logged nothing at all. */
    for (i = 0; i < (int)APAD_MAX_SESSIONS; i++) {
        if (s->sessions[i].in_use) {
            teardown_session(s, i, "server shutdown");
        }
    }
    /* Wipe the §10 secret before the allocation goes back to the heap.
     * apad_session_close() (via teardown_session above) already wiped every
     * session key; this is the one piece of key material that lives outside
     * a session. */
    apad_pairing_close(&s->pairing, &s->log, NULL);
    s->backend->shutdown();
    free(s);
}
