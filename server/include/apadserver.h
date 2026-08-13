/* server/include/apadserver.h — libapadserver, the sans-IO server library
 * (docs/DESIGN.md §6.4).
 *
 * The library owns the session table and lifecycle, protocol handling via
 * libapad, the mapping engine, profile parsing and backend dispatch. It
 * owns no socket, no thread, no clock and no stdout: a host supplies all
 * four. That is not architectural taste -- three hosts need it. Android
 * wants its own lifecycle (a foreground Service), Windows wants a tray
 * application's message loop, and the fuzz/loopback tooling wants to drive
 * the server deterministically with no sockets at all. A library that owns
 * the loop can satisfy none of them.
 *
 * The three rules that make this work:
 *   1. Outbound datagrams leave through cfg.on_send. The library never
 *      calls apad_udp_send().
 *   2. Diagnostics leave through cfg.on_log as a formatted, NUL-terminated
 *      line. The library formats into a stack buffer; writing that line to
 *      a stream (or logcat, or a tray balloon) is the host's business.
 *   3. Time arrives as a parameter -- to apad_server_tick() AND to
 *      apad_server_on_datagram(). The library never calls apad_ticks_ms()
 *      and never stashes a clock between calls, so a test can drive it from
 *      a fake clock and no host can accidentally age one entry point's
 *      deadlines against a clock read for the other.
 *
 * What does NOT change is the apad_backend interface (server/backends/
 * backend.h, docs/DESIGN.md §6.1): the host picks a backend and passes it in, and
 * nothing outside server/backends/ knows which one is active.
 *
 * §10 pairing lives in server/src/pairing.c and is entirely opt-in: with no
 * pairing window open this server behaves exactly as it did before pairing
 * existed -- no WELCOME carries AUTH_REQUIRED, no session ever gets a key,
 * and an inbound AUTHENTICATED datagram is still silently ignored because
 * there is nothing to verify it against. See the pairing section at the
 * bottom of this header.
 *
 * Out of scope for this milestone, called out rather than half-built:
 *   - discovery.c/mdns.c tier 1 (mDNS) -- tier 2 (DISCOVER/ANNOUNCE) and
 *     tier 3 (the host printing its own port) are implemented; tier 1 is
 *     not.
 *   - any persistent record of a previously-paired client. §8's diagram
 *     says "[server shows PIN if unpaired]", but §10 defines only a window
 *     and a derivation and the wire carries no "I am already paired" signal
 *     beyond HELLO.client_id, so nothing here remembers a device across
 *     sessions. Every session inside a window derives its own key from the
 *     PIN the user is looking at.
 */
#ifndef ATTICPAD_SERVER_APADSERVER_H
#define ATTICPAD_SERVER_APADSERVER_H

#include <stddef.h>
#include <stdint.h>

#include "atticpad/atticpad.h"
#include "backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Severity of an on_log line. The library never decides what to do with a
 * level -- a headless host may print everything to stderr (the Linux host
 * does, which is why splitting the server changed no log output), a tray UI
 * may surface only WARN and above. */
typedef enum {
    APAD_LOG_INFO = 0,
    APAD_LOG_WARN,
    APAD_LOG_ERROR
} apad_log_level;

/*
 * Send one datagram to `to`. Return >= 0 on success, < 0 on failure.
 *
 * The return value is DIAGNOSTIC, not control flow. A negative return is
 * logged through on_log and changes nothing else: the library still calls
 * apad_session_on_sent() and still caches the datagram as the §9 retransmit
 * buffer, exactly as if the send had succeeded.
 *
 * That is deliberate, and it is what §9 asks for. §9's 100/200/400/800 ms
 * retransmit schedule IS the recovery for a datagram that did not reach the
 * peer, and the protocol draws no distinction between one that failed locally
 * (EAGAIN, ENOBUFS, a down interface) and one that vanished in transit -- to
 * the session state machine they are the same event with the same answer. A
 * library that skipped the accounting would disarm the only recovery the
 * protocol has, in precisely the case it was trying to be careful about: a
 * WELCOME whose first send failed would never be retried, the session would
 * never close as APAD_CLOSE_RETX_FAILED, and it would sit ACTIVE holding a
 * virtual pad until the §8 3000 ms idle timeout.
 *
 * What a failed send does cost is a sequence number: apad_session_next_header()
 * increments the session's tx_seq before on_send is ever called, so the
 * number is spent whether or not the datagram leaves. The resulting gap in
 * the series the peer observes is harmless -- §9 requires only that
 * comparisons be wrap-safe (apad_seq_newer), never that the series be
 * contiguous -- and a retransmit reuses the original number rather than
 * allocating a new one, as §9 requires.
 *
 * So a host SHOULD report failures honestly, because that is what makes a
 * dead socket visible in the log, but nothing in the library's correctness
 * depends on it.
 *
 * `buf` is owned by the library and is only valid for the duration of the
 * call: a host that queues rather than sends must copy.
 */
typedef int (*apad_server_send_fn)(void *user, const apad_addr *to,
                                   const uint8_t *buf, size_t len);

/*
 * One diagnostic line, already formatted and NUL-terminated, with no
 * trailing newline and no "[atticpad] " prefix -- the host adds whatever
 * decoration its sink wants. `msg` is only valid for the duration of the
 * call. May be NULL in the cfg, in which case diagnostics are discarded.
 */
typedef void (*apad_server_log_fn)(void *user, apad_log_level level,
                                   const char *msg);

/*
 * Fill `buf` with `len` cryptographically secure random bytes. Return
 * non-zero on success, zero on failure. Optional -- but see below for what
 * a server without one refuses to do.
 *
 * Randomness is platform I/O, exactly like on_send and on_log, so it
 * arrives the same way: the library must not open /dev/urandom, call
 * BCryptGenRandom, or reach for any other OS entropy source itself. The
 * Linux host uses getrandom() and falls back to reading /dev/urandom; the
 * Windows host uses BCryptGenRandom.
 *
 * A server whose cfg has no on_random MUST refuse to open a pairing window
 * (apad_server_begin_pairing returns APAD_ERR_STATE and says so through
 * on_log), and one whose on_random FAILS at the moment a window is
 * requested MUST NOT open it either. The library never invents entropy and
 * never falls back to anything derived from the clock, a counter or an
 * address: a weak PIN is worse than no pairing at all, because it looks
 * like security. Everything else the server does works unchanged without
 * on_random -- pairing is the only feature that needs it.
 */
typedef int (*apad_server_random_fn)(void *user, uint8_t *buf, size_t len);

/*
 * One JSONC profile as a memory blob, not a path -- the library does no
 * filesystem access, so a host is free to read these from a directory
 * (the Linux host does), from Android assets, or from a string constant in
 * a test.
 *
 * `label` is what diagnostics about this blob will name (the Linux host
 * passes the file's path). `name` is the profile name to use if the blob
 * itself omits "profile" (the Linux host passes the basename without
 * ".jsonc", which is what the directory loader used to derive internally).
 * `text` must be NUL-terminated.
 *
 * apad_server_create() parses these before it returns and copies everything
 * it keeps, so the sources -- and the memory they point at -- need not
 * outlive the call.
 */
typedef struct {
    const char *label;
    const char *name;
    const char *text;
} apad_profile_source;

/*
 * Everything the library needs from its host. Copied by value at create
 * time; nothing in here except `user` is retained by pointer.
 */
typedef struct {
    /* Required. See apad_server_send_fn -- < 0 on failure is reported
     * through on_log, not acted on. */
    apad_server_send_fn on_send;

    /* Optional; NULL discards every diagnostic. */
    apad_server_log_fn  on_log;

    /* Optional -- but a server without one cannot open a §10 pairing
     * window. See apad_server_random_fn. */
    apad_server_random_fn on_random;

    /* Passed back verbatim to on_send and on_log. The library never
     * dereferences it. */
    void               *user;

    /* The UDP port the host is listening on. Goes out in ANNOUNCE
     * (§6.2 server_port) so a client that discovered this server on a
     * broadcast port knows where to send its HELLO. The library has no
     * socket to ask. */
    uint16_t            server_port;

    /* Name advertised in ANNOUNCE. NULL means "AtticPad Server". Truncated
     * to APAD_NAME_LEN on the wire like any other §2 text field. */
    const char         *server_name;

    /* Mapping profiles (docs/DESIGN.md §6.2). Order matters and is the host's to
     * choose: apad_profiles_match() takes the FIRST profile whose
     * match.device is a substring of the client's device_name, and a
     * profile with an empty match.device is a wildcard that matches
     * everything after it. The Linux host sorts by filename so
     * "3ds-default.jsonc" is tried before "generic-default.jsonc".
     * profile_count == 0 (or profiles == NULL) is fine: the compiled-in
     * default profile is always available. */
    const apad_profile_source *profiles;
    size_t                     profile_count;

    /* §7 tier 2: this host's own subnet-directed broadcast addresses (e.g.
     * 192.168.1.255 for a /24 on 192.168.1.0). server/src/server.c's
     * is_bad_reply_target() already refuses 255.255.255.255, 224.0.0.0/4 and
     * 0.0.0.0 as an ANNOUNCE/ERROR target with no help from the host --
     * those are derivable from the address alone -- but recognising a
     * SUBNET-directed broadcast needs the interface's netmask, which this
     * sans-IO library has no socket to ask for (same reasoning as
     * server_port above: the host owns the socket). A host that can
     * enumerate its interfaces (getifaddrs()'s ifa_broadaddr, or the
     * equivalent) passes what it found here; a host that can't (or, for
     * now, the Windows host -- see server/host/windows/main.c) passes
     * NULL/0 and gets exactly today's behaviour: a spoofed DISCOVER whose
     * source claims to be a subnet broadcast still draws an ANNOUNCE.
     *
     * Only `.ip` is examined; `.port` is ignored (a host may leave it 0).
     * Copied by value at create time like every other cfg field, into a
     * fixed table capped at APAD_MAX_BROADCAST_ADDRS (server/src/server.c)
     * -- entries beyond the cap are logged and dropped, not fatal, same
     * pattern as `profiles` above overflowing APAD_PROFILES_MAX. */
    const apad_addr           *broadcast_addrs;
    size_t                     broadcast_addr_count;
} apad_server_cfg;

typedef struct apad_server apad_server;

/*
 * Allocate a server, parse `cfg`'s profiles, and bring up `backend`
 * (backend->init()). Returns NULL if cfg or backend is missing, if
 * cfg->on_send is NULL, if allocation fails, or if the backend refuses to
 * initialise -- the reason goes out through cfg->on_log in every case where
 * there is one to report.
 *
 * `backend` is retained by pointer and must outlive the server; every
 * backend in this tree is a static const struct.
 */
apad_server *apad_server_create(const apad_server_cfg *cfg,
                                const apad_backend *backend);

/*
 * Tear down every live session (which destroys their virtual pads), shut
 * the backend down, and free the server. NULL is a no-op.
 */
void apad_server_destroy(apad_server *s);

/*
 * Feed one received datagram in. Returns APAD_OK when the datagram was
 * dispatched, or the negative apad_packet_parse() code when it was rejected
 * by §3.1 -- informational only, since a rejected datagram is either
 * silently dropped or answered with an ERROR internally, exactly as §8
 * requires. A host is free to ignore the return.
 *
 * `now_ms` is when this datagram arrived, on the same monotonic millisecond
 * timebase the host passes to apad_server_tick() (apad_ticks_ms() on every
 * host in this tree). It is an explicit parameter, and there is deliberately
 * NO ordering requirement between this call and apad_server_tick(): call them
 * in whatever order the host's loop produces.
 *
 * An earlier version of this API took the clock from the last tick instead,
 * which quietly required "tick at least as often as you deliver, and tick
 * first". That requirement is gone and hosts must not be written as though it
 * still holds. It was worth removing because the staleness it introduced was
 * strictly one-directional: a §8 idle timeout or a §9 retransmit deadline
 * measured against an old clock fires EARLY, never late, so sessions are torn
 * down before their three seconds are up and the first retransmit gap is
 * compressed into something §9 itself calls indistinguishable from packet
 * loss. Any of the obvious host shapes trips it -- ticking on a timer while a
 * socket thread delivers, draining the socket in a batch before ticking, or
 * `if (n == 0) tick()`, which freezes the clock outright under sustained
 * input so retransmits and timeouts never fire at all -- and a host that
 * delivered before its first tick would create a session stamped with a clock
 * of 0 that the next tick reaps as a 3000 ms idle timeout.
 */
int apad_server_on_datagram(apad_server *s, uint32_t now_ms,
                            const apad_addr *from,
                            const uint8_t *buf, size_t len);

/*
 * Advance the clock: services §9 retransmits and §8 idle timeouts, and may
 * call on_send and on_log. Returns APAD_OK, or APAD_ERR_ARG for a NULL
 * server.
 *
 * Call granularity is the host's to choose and it matters: §9's first
 * retransmit gap is 100 ms and apad_session_tick() reschedules the next due
 * time from the actual (late) now, so a coarse tick compounds drift across
 * all four attempts until it reads as packet loss. The Linux host ticks
 * every 20 ms.
 *
 * §6.6 RTT: this is also where the server ORIGINATES its own PING, once per
 * ACTIVE session per second. §6.6 makes PING/PONG symmetric ("either" may
 * send either) and this server has only ever ANSWERED a client's PING
 * before now -- an inbound PONG merely refreshed liveness. A UI wanting
 * live RTT (docs/DESIGN.md §6.3) needs the server to measure it, and the only way
 * to measure a round trip is to be the one who started the clock. Nothing
 * about this is a protocol change: the wire already allows it, and a client
 * that doesn't expect a server-originated PING answers it as a normal PING
 * (its own §6.6 obligation), which is exactly the PONG this server then
 * correlates by echoed origin_ticks_ms to fill in apad_client_info::rtt_ms.
 * One in flight at a time per session; a PONG that doesn't match the
 * outstanding origin_ticks_ms (stale, or the client's own unsolicited PING
 * misread) is ignored rather than misattributed.
 */
int apad_server_tick(apad_server *s, uint32_t now_ms);

/* ======================================================================== */
/* Server UI query API — docs/DESIGN.md §6.3, §6.4                                  */
/* ======================================================================== */
/*
 * Everything below is a PURE QUERY: no clock, no side effect, safe for a UI
 * to poll at 5 Hz (or faster) without perturbing protocol state. That is
 * deliberate and it is why none of these take a `now_ms` -- a query that
 * could advance a deadline by being asked about it would let a UI's own
 * poll rate influence §8/§9 timing, which must depend only on
 * apad_server_tick()/apad_server_on_datagram() and the now_ms a host
 * actually passes them. apad_server_set_profile() is the one exception
 * that is not read-only, and it does not touch the clock either: switching
 * a profile takes effect for mapping.c's next INPUT_STATE, nothing here is
 * time-dependent.
 */

/* One connected client, everything server/host/linux's UI (and any other
 * host's) needs to render docs/DESIGN.md §6.3's list. A snapshot: nothing here
 * points into the server, so a UI may hold, copy or serialise it after the
 * call returns. */
typedef struct {
    uint8_t   slot;             /* 0..APAD_MAX_SESSIONS-1                    */
    uint16_t  session_id;
    char      device_name[APAD_NAME_LEN + 1];   /* from HELLO, NUL-terminated */
    apad_addr peer;
    uint32_t  caps;              /* APAD_CAP_* from HELLO                    */
    char      profile_name[64];  /* mirrors server/src/profiles.h's
                                   * APAD_PROFILE_NAME_LEN -- kept independent
                                   * rather than #include-ing a server-internal
                                   * header from this public one */

    /* §6.6 RTT, from the server-originated PING apad_server_tick() sends at
     * 1 Hz (see its doc comment). UINT32_MAX until the first PONG for this
     * session has been correlated -- a UI SHOULD render that as "measuring"
     * rather than "0 ms". */
    uint32_t  rtt_ms;
#define APAD_RTT_UNKNOWN 0xFFFFFFFFu

    uint8_t   battery;           /* 0..100, or APAD_BATTERY_UNKNOWN (255) --
                                   * from the most recent INPUT_STATE; 255
                                   * until the first one has arrived */
    uint8_t   authenticated;     /* apad_session::have_key: 1 once a §10 key
                                   * is installed on this session, 0 for an
                                   * unauthenticated one (which is every
                                   * session when no pairing window has ever
                                   * been opened) */
    uint32_t  rx_packets;        /* datagrams accepted on this session's
                                   * session_id since it was created */
    uint32_t  tx_packets;        /* datagrams this server sent on it, whether
                                   * or not on_send reported success (see
                                   * apad_server_send_fn: the count reflects
                                   * §9 accounting, not confirmed delivery) */
} apad_client_info;

/*
 * Write up to `max` connected clients into `out`, slot order. Returns the
 * total number of connected clients (which may exceed `max`); `out` may be
 * NULL (with max == 0) to just ask for the count. `s` may be NULL, which
 * returns 0 and writes nothing.
 */
int apad_server_list_clients(const apad_server *s, apad_client_info *out,
                             size_t max);

/* Backend health, docs/DESIGN.md §6.3 ("Backend status ('ViGEmBus not installed —
 * click to install')"). See server/backends/backend.h's health() hook --
 * this is a thin, backend-neutral wrapper around it. `state`/`message`/
 * `remedy` are copies of the same fields in apad_backend_health -- copied
 * out (not pointers) so this struct is safe to hold or hand to another
 * thread past the next health() call, same reasoning as apad_client_info.
 *
 * `ok` and `state` deliberately overlap: `ok` is the one-branch summary a
 * UI that does not care WHY can use ("green dot or not"); `state` is what a
 * UI that wants to say "click to install" versus "check your udev rule"
 * branches on instead. Neither one requires knowing which backend is
 * active -- that is the entire point of apad_backend_health_state living in
 * backend.h, shared by every backend. */
typedef struct {
    char name[32];       /* backend->name                                   */
    int  ok;             /* 1: nothing to report. 0: see `state`/`message`. */
    apad_backend_health_state state;  /* OK mirrors ok == 1                 */
    char message[160];   /* human-readable reason, "" when ok == 1          */
    char remedy[160];    /* "" when there is nothing a host can act on;
                           * otherwise a URL or command a host MAY show as
                           * an action ("click to install", "run this") --
                           * see backend.h's apad_backend_health doc comment
                           * for the contract a host may rely on            */
} apad_backend_status;

/*
 * Copy the active backend's current health into `*out`. Returns APAD_OK, or
 * APAD_ERR_ARG if either pointer is NULL. Cheap: at most one call into the
 * backend's own health() (uinput.c's checks a file's access(2) bits; it does
 * not open the device), so safe to call every UI poll.
 *
 * A backend whose init() FAILED never produces a live apad_server to call
 * this on in the first place (apad_server_create() returns NULL) -- a host
 * that wants to show backend status on that path calls
 * backend->health() directly on its own linked-in apad_backend (filling an
 * apad_backend_health, backend.h), which is legal and is exactly what
 * backend.h's health() doc comment anticipates. That is precisely the
 * "ViGEmBus not installed" case a UI most needs, and it is reachable that
 * way even before (or after a failed) apad_server_create().
 */
int apad_server_backend_status(const apad_server *s, apad_backend_status *out);

/*
 * Switch the profile mapping.c uses for session `slot`'s next INPUT_STATE.
 * `profile_name` is matched exactly against apad_profile::name via
 * apad_profiles_find() (server/src/profiles.h) -- never the substring
 * match apad_profiles_match() uses at HELLO time, because a UI picking from
 * an exact list of loaded names has no use for "first substring match" and
 * every reason to want "this one or an error".
 *
 * Returns:
 *   APAD_OK         switched.
 *   APAD_ERR_ARG     `s` or `profile_name` is NULL, `slot` is out of range,
 *                    or no loaded profile has that exact name.
 *   APAD_ERR_STATE   `slot` is in range but has no connected session.
 *
 * Does not reset apad_mapping_state (per-session touch/gyro anchor state):
 * a live profile swap keeps whatever return-to-centre anchor was already in
 * effect rather than snapping it, which matches what a session's own first
 * profile match at HELLO time already does for a reused slot.
 */
int apad_server_set_profile(apad_server *s, uint8_t slot,
                            const char *profile_name);

/*
 * Copy the most recently DECODED INPUT_STATE for session `slot` into `*out`
 * -- the same apad_input_state handle_input_state() feeds into
 * apad_mapping_apply(), snapshotted BEFORE any profile is applied to it.
 * This is raw observation of what the client is sending, on the wire's own
 * axis convention (+Y up, §5.3) and battery encoding (§5.5) -- exactly what
 * a remapping editor wants to watch live while an operator drags a
 * deadzone slider or slides a touch region, independent of whatever
 * mapping is currently being applied to it. Never re-derives or reapplies
 * mapping.c; the mapping engine itself remains server-only (server/src/
 * mapping.c) and unreachable from this public header.
 *
 * If `out_frame` is non-NULL, `*out_frame` is written with this session's
 * OWN INPUT_STATE counter: 0 until the session's first accepted INPUT_STATE,
 * then incremented once per accepted one (never reset for the life of the
 * session -- a slot reused by a later HELLO starts a fresh session and a
 * fresh counter at 0, same convention as apad_client_info::rx_packets/
 * tx_packets). Comparing two poll results' `*out_frame` is how a caller
 * tells "the client sent something new since I last asked" from "nothing
 * has arrived" without its own clock or a byte-for-byte diff of the struct
 * (which would have to account for reserved/padding bytes) -- exactly the
 * freshness signal a UI polling at 5-30 Hz needs, and 0 doubles as the
 * "nothing has ever arrived on this slot" sentinel a poller can render
 * uniformly rather than branching on a separate error code for that case.
 *
 * Returns:
 *   APAD_OK         `*out` (and `*out_frame`, if requested) filled with the
 *                    live values.
 *   APAD_ERR_STATE   `slot` names a connected session that has not yet
 *                    produced one accepted INPUT_STATE -- `*out` is zeroed
 *                    (not left uninitialised) and `*out_frame`, if
 *                    requested, is 0, so a caller that ignores the return
 *                    value still gets a coherent "nothing yet" snapshot.
 *   APAD_ERR_ARG     `s` or `out` is NULL, `slot` is out of range, or
 *                    `slot` has no connected session. (Deliberately lumped
 *                    with the argument-error case rather than given its own
 *                    code, unlike apad_server_set_profile()'s APAD_ERR_STATE
 *                    for "no session": this query has no session-scoped
 *                    ACTION to distinguish from a bad argument the way a
 *                    mutation like set_profile does, so "nothing to read"
 *                    and "you asked for something that cannot exist" are
 *                    the same outcome here.)
 */
int apad_server_last_input(const apad_server *s, uint8_t slot,
                           apad_input_state *out, uint32_t *out_frame);

/*
 * Reload the profile set from `sources`/`count` (apad_profiles_load()'s own
 * parameters, forwarded verbatim -- see apad_profile_source's doc comment
 * above) and re-resolve EVERY live session's `const apad_profile *` against
 * the newly loaded set, so a profile file edited (or added, or removed) on
 * disk while the server is running takes effect immediately, without a
 * restart and without dropping a single connected client.
 *
 * This MUST live in the library rather than being "just call
 * apad_profiles_load() again" from a host: server/src/profiles.c keeps its
 * loaded set in a file-static table (server/src/server.c's struct
 * apad_server comment documents the hazard), and every session holds a raw
 * `const apad_profile *` into that table (matched once at HELLO time and
 * never re-derived on the INPUT_STATE hot path). apad_profiles_load()
 * OVERWRITES that table's contents in place -- it does not reallocate a new
 * one -- so a session's existing pointer does not dangle after a reload the
 * way a naive "free the old table, build a new one" scheme would, but it
 * DOES silently start reading a different profile's bytes at the same
 * address the instant the reload runs, unless something re-resolves it.
 * That "something" has to be this library, both because it is the only code
 * that holds the session table at all and because doing it from a host
 * would need every session's identity threaded back out through the public
 * API just so the host could hand it to apad_profiles_find() itself --
 * strictly worse than one function that already has both.
 *
 * Re-resolution, per live session, in this order:
 *   1. If the session currently has a profile, look its NAME up again via
 *      apad_profiles_find() (exact match) -- this is what keeps a session on
 *      "the same profile, freshly reloaded" when an operator edits a file's
 *      contents without renaming it (the common case: adjusting a deadzone,
 *      adding a touch region).
 *   2. Otherwise, or if that name no longer exists in the new set (the file
 *      was deleted, or renamed), fall back to apad_profiles_match() against
 *      the session's own device_name -- the same substring match HELLO-time
 *      resolution already uses, so a session whose profile vanished lands
 *      exactly where a brand-new HELLO from the same device would today.
 * A session whose profile identity changes as a result gets one on_log INFO
 * line naming the old and new profile names -- this is the one place a
 * profile swap can happen with no human having asked for it directly (unlike
 * apad_server_set_profile(), always a deliberate UI action), so it is worth
 * a line even though nothing else about the session changes: mapping.c's
 * per-session touch/gyro anchor state is untouched, exactly as
 * apad_server_set_profile() already documents for its own live swap.
 *
 * Returns APAD_OK, or APAD_ERR_ARG if `s` is NULL. Never fails on a bad
 * profile blob within `sources` -- exactly like apad_profiles_load() itself,
 * a malformed file is logged and skipped, never fatal to the reload as a
 * whole.
 */
int apad_server_reload_profiles(apad_server *s, const apad_profile_source *sources,
                                size_t count);

/* ======================================================================== */
/* §10 pairing — server/src/pairing.c                                        */
/* ======================================================================== */
/*
 * What §10 actually asks for, and what this API is:
 *
 *   - the server generates a secret during "an explicit, user-initiated
 *     pairing window" (§10). apad_server_begin_pairing() IS that user
 *     action arriving: the library has no UI, so a host wires it to a
 *     button, a tray menu item, or -- on the headless Linux host -- SIGUSR1.
 *   - both sides derive a session key with PBKDF2-HMAC-SHA256, 10,000
 *     iterations, salted with the 16-byte WELCOME.server_nonce. The server
 *     half of that is automatic: while a window is open, every WELCOME
 *     carries APAD_WELCOME_AUTH_REQUIRED and a FRESH per-session nonce, and
 *     the session's key is installed before the next datagram can arrive.
 *   - the secret is valid for 120 s (§11 APAD_PAIRING_WINDOW_MS) and never
 *     otherwise, and five failed attempts (§11 APAD_MAX_PAIR_ATTEMPTS)
 *     invalidate it and generate a new one.
 *   - the secret NEVER appears on the wire. It appears only in
 *     apad_pairing_info (for the UI to render) and in one on_log line.
 *
 * Two things this API deliberately does NOT do:
 *
 *   - It does not remember anything across sessions. See the file header.
 *   - It does not touch WELCOME.key_material, which stays zero as §6.4
 *     requires. Using it would be a v2 wire change.
 *
 * THREADING: an apad_server is not internally synchronised, and these three
 * calls are no exception. A host that begins pairing from a UI thread while
 * a socket thread is inside apad_server_on_datagram() must serialise them
 * itself, exactly as it already must for tick and on_datagram.
 */

/* Which alphabet the window's secret was drawn from. Both feed the same
 * apad_derive_session_key(), which takes a C string of any length, so this
 * is a UI and entropy distinction only -- ZERO wire difference. */
typedef enum {
    /* §10's 6-digit PIN: what a user types on a console's on-screen
     * keyboard, where every extra character is real work. ~20 bits. */
    APAD_PAIRING_PIN   = 0,
    /* A longer random token for a channel that transcribes it for the user
     * -- a QR code the phone client scans. ~100 bits, so the offline
     * brute-force §10 admits to for the PIN is not feasible against it.
     * Drawn from an alphabet with no 0/O and no 1/I/l, because a token that
     * fails to scan gets read aloud or typed by hand. */
    APAD_PAIRING_TOKEN = 1
} apad_pairing_kind;

#define APAD_PAIRING_PIN_LEN     6u    /* §10 "6-digit PIN"               */
#define APAD_PAIRING_TOKEN_LEN   20u   /* 20 * log2(32) = 100 bits        */

/* Buffer size, NUL included. Tied to core's APAD_SECRET_MAX_LEN (§10.1's
 * own 6..64 byte ceiling, atticpad/protocol.h) rather than an independent
 * 64 so the two constants cannot drift: apad_derive_session_key() already
 * accepts and uses a secret of that length IN FULL, so a server that could
 * only ever GENERATE or HOLD up to 31 usable bytes would make §10.1's
 * longer scanned-token path unreachable even though core and every client
 * already support it. Both alphabets in pairing.c stay far under this
 * (6 and 20 bytes) -- this is the ceiling a future longer token, or a
 * secret typed in from elsewhere, is allowed to reach. */
#define APAD_PAIRING_SECRET_MAX  (APAD_SECRET_MAX_LEN + 1u)

/*
 * A snapshot of the pairing window, shaped for whatever renders it: a tray
 * balloon, a console line, a QR bitmap, a 3DS bottom screen.
 *
 * Every field is a plain value, copied out -- nothing here points into the
 * server, so a UI may hold it, hand it to another thread, or render it long
 * after the window has closed (it will simply be describing the past).
 */
typedef struct {
    /* 0 when no window is open. THE ONLY FIELD WORTH BRANCHING ON: when it
     * is 0 every other field is zeroed, including `secret`, so a UI cannot
     * accidentally display a stale PIN. */
    int      open;

    /* apad_pairing_kind. Tells a UI whether to draw six big digits or a QR
     * code. */
    uint8_t  kind;

    /* The secret, NUL-terminated and directly displayable: ASCII digits for
     * APAD_PAIRING_PIN, ASCII uppercase-and-digits for APAD_PAIRING_TOKEN.
     * Never grouped, spaced or hyphenated by the library -- how to lay out
     * "123456" is a UI decision, and the string a client types must be
     * exactly these bytes. */
    char     secret[APAD_PAIRING_SECRET_MAX];

    /* Milliseconds left of the 120 s window, 0 once it has expired.
     *
     * READ THIS BEFORE BUILDING A COUNTDOWN. The library owns no clock, so
     * this is measured against the most recent now_ms the host passed to
     * apad_server_tick() or apad_server_on_datagram() -- it is exactly as
     * fresh as the host's tick rate (20 ms on the Linux host) and it does
     * not advance on its own between calls. That staleness is display-only
     * and cannot let an expired secret be used: every decision that MATTERS
     * -- whether a HELLO gets an AUTH_REQUIRED WELCOME at all -- is taken
     * against the now_ms of the datagram being handled, not against this. */
    uint32_t ms_remaining;

    /* Failed attempts left before the secret is invalidated and replaced
     * (§11: five). Starts at APAD_MAX_PAIR_ATTEMPTS and returns there when
     * a new secret is generated. A UI showing "2 tries left" should show
     * this, not count its own. */
    uint8_t  attempts_remaining;

    /* Increments every time a NEW secret is generated -- on each
     * begin_pairing and on each five-failure rotation. A UI that polls
     * compares this to redraw a PIN or re-render a QR code without
     * comparing (or caching) the secret itself. Never reset for the life of
     * the server. */
    uint32_t generation;
} apad_pairing_info;

/*
 * Open a pairing window: generate a fresh secret, arm the 120 s deadline
 * against `now_ms`, and reset the attempt counter to APAD_MAX_PAIR_ATTEMPTS.
 * `use_token` picks the alphabet: 0 for §10's 6-digit PIN, non-zero for the
 * longer QR-shaped token.
 *
 * Calling it while a window is already open REPLACES that window -- new
 * secret, new deadline, attempts reset. That is what a user pressing "Pair"
 * a second time means, and it is the only way to get a new PIN without
 * waiting for five failures. Sessions still mid-handshake against the OLD
 * secret are dropped by the same rule that drops them when a window closes
 * (below); sessions that already authenticated are untouched.
 *
 * Returns APAD_OK, or:
 *   APAD_ERR_ARG    `s` is NULL.
 *   APAD_ERR_STATE  cfg.on_random was NULL, or it failed. No window opens
 *                   and no secret exists. The library will NOT substitute
 *                   the clock, a counter, or anything else -- see
 *                   apad_server_random_fn. Both cases are reported through
 *                   on_log; a UI SHOULD surface the failure rather than
 *                   showing an empty PIN box.
 *
 * On success one on_log line at APAD_LOG_INFO carries the secret, because
 * on a headless host that log line is the only "server UI" §8's diagram
 * ("[server shows PIN if unpaired]") can refer to.
 */
int apad_server_begin_pairing(apad_server *s, uint32_t now_ms, int use_token);

/*
 * Close the window early: wipe the secret with apad_secure_zero() and stop
 * issuing AUTH_REQUIRED WELCOMEs. Idempotent; a NULL server is a no-op.
 *
 * `now_ms` is not needed to close the window -- it is taken so the library
 * keeps a fresh notion of "now" for apad_server_pairing_state()'s countdown
 * and so this entry point looks like every other one in this header.
 *
 * Sessions that were issued an AUTH_REQUIRED WELCOME but have not yet
 * produced one datagram with a verifying tag are torn down here: §10 says
 * the secret is valid "for 120 seconds ... and never otherwise", and a
 * handshake that has not proved knowledge of it by the time the window
 * shuts has not been authorised. Sessions that DID authenticate keep
 * running -- their key is derived and the window closing does not unpair
 * them.
 */
void apad_server_cancel_pairing(apad_server *s, uint32_t now_ms);

/*
 * Copy the current pairing state into `*out`. Returns APAD_OK, or
 * APAD_ERR_ARG if either pointer is NULL. `*out` is fully written on
 * success (zeroed except `open` when no window is open), so a caller never
 * has to initialise it.
 *
 * Cheap and side-effect free: safe to call every UI frame. Takes no clock
 * on purpose -- it reports the state as of the last now_ms the server was
 * given, and it is const because it changes nothing. See
 * apad_pairing_info::ms_remaining for what that means for a countdown.
 */
int apad_server_pairing_state(const apad_server *s, apad_pairing_info *out);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_APADSERVER_H */
