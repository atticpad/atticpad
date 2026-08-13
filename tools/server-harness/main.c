/* tools/server-harness/main.c
 *
 * Drives libapadserver (server/include/apadserver.h) directly through its
 * public API: no socket, no thread, and no real clock anywhere in this
 * process. docs/DESIGN.md S6.4 gave one of the reasons the server was split into a
 * sans-IO library plus a thin host: "the existing fuzz and loopback tooling
 * wants to drive the server deterministically with no sockets at all." This
 * is that tool.
 *
 * Everything here is a "client" only in the sense that it hand-assembles
 * wire bytes and hands them to apad_server_on_datagram() -- never through
 * apad_session_* (that is the real client-side FSM, exercised by
 * tools/loopback-client instead). That is deliberate: several scenarios
 * below need bytes a conforming client-side FSM would never willingly
 * produce -- a spoofed source address, a byte-for-byte resend outside any
 * retransmit schedule, a HELLO delivered before the harness has ticked the
 * server even once. tools/loopback-client cannot reach any of these paths:
 * it talks over a real UDP socket, so it can neither forge a source address
 * nor make a send() fail on demand.
 *
 * Two things make this driveable at all:
 *   - a fake clock: every apad_server_on_datagram() / apad_server_tick()
 *     call below passes an explicit uint32_t the test chose, never
 *     apad_ticks_ms(). Nothing in libapadserver calls apad_ticks_ms() either
 *     (apadserver.h's contract) -- this file is the thing that proves that,
 *     not merely asserts it: if the library ever regressed to reading the
 *     real clock, ticks and deliveries below would drift out of sync with
 *     wall time and the schedule-boundary checks (exactly-100/300/700/1500ms
 *     etc.) would fail nondeterministically under CI load, rather than
 *     staying exact regardless of how slowly this process actually runs.
 *   - an on_send the test can fail on demand and that records every
 *     outbound datagram, so assertions can inspect bytes, not just counts.
 *
 * Every check() cites the docs/PROTOCOL.md section (or the apadserver.h
 * comment) it enforces. Exit code is 0 iff every check passed, matching
 * tools/loopback-client's convention.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "atticpad/atticpad.h"
#include "apadserver.h"

/* ========================================================================
 * Recording backend.
 *
 * server/backends/backend.h's apad_backend function pointers take an `int
 * slot`, never a `void *user` (unlike apad_server_send_fn / on_log above,
 * both of which do). That is a real piece of friction for a test double:
 * this recording state has to be a file static rather than something a
 * harness_state could own and pass through cfg.user, because backend.h
 * gives it nowhere to travel. Harmless here (every scenario below runs to
 * completion, checks its counters, and tears its server down before the
 * next scenario creates one), but it is the one thing in this exercise that
 * could not be made fully instance-local, and it would matter to a test
 * that wanted two servers alive at once. Reported as a finding.
 * ======================================================================== */

typedef struct {
    int init_calls;
    int create_calls;
    int update_calls;
    int poll_calls;
    int destroy_calls;
    int shutdown_calls;
    int last_slot;
    apad_pad_state last_state;
} backend_state;

static backend_state g_backend;

static int rec_init(void) {
    g_backend.init_calls++;
    return 0;
}
static int rec_create_pad(int slot, apad_pad_type type) {
    (void)slot;
    (void)type;
    g_backend.create_calls++;
    return 0;
}
static int rec_update_pad(int slot, const apad_pad_state *state) {
    g_backend.update_calls++;
    g_backend.last_slot = slot;
    g_backend.last_state = *state;
    return 0;
}
static int rec_poll_feedback(int slot, apad_feedback *out) {
    (void)slot;
    g_backend.poll_calls++;
    memset(out, 0, sizeof *out);
    return 0;
}
static void rec_destroy_pad(int slot) {
    (void)slot;
    g_backend.destroy_calls++;
}
static void rec_shutdown(void) {
    g_backend.shutdown_calls++;
}

static const apad_backend kRecordingBackend = {
    .init          = rec_init,
    .create_pad    = rec_create_pad,
    .update_pad    = rec_update_pad,
    .poll_feedback = rec_poll_feedback,
    .destroy_pad   = rec_destroy_pad,
    .shutdown      = rec_shutdown,
    .name          = "recording-test-backend"
    /* .health left NULL (backend.h: optional) -- this harness never
     * exercises apad_server_backend_status()'s "backend reports a problem"
     * path; server-dev agent memory server-ui-* covers that live against
     * the real uinput backend instead. */
};

/* ========================================================================
 * on_send / on_log recording, and on-demand send failure.
 *
 * cfg.user IS threaded through to both of these (apadserver.h), so unlike
 * the backend above this state is fully per-scenario: a fresh harness_state
 * for every apad_server_create() call below, no file statics needed here.
 * ======================================================================== */

#define MAX_RECORDED_SENDS 64

typedef struct {
    apad_addr to;
    uint8_t   buf[APAD_MAX_DATAGRAM];
    size_t    len;
    uint8_t   type;         /* decoded header.type, for convenience         */
    uint16_t  session_id;   /* decoded header.session_id                    */
    uint16_t  sequence;     /* decoded header.sequence                      */
    int       send_rc;      /* what fake_send returned for this call        */
} recorded_send;

typedef struct {
    recorded_send sends[MAX_RECORDED_SENDS];
    int count;
    int fail_call_index;    /* -1: never fail. Else the 0-based on_send call
                              * (within this harness_state's lifetime) to
                              * force -1 on, imitating a transient local
                              * send failure (EAGAIN/ENOBUFS/a down
                              * interface -- apadserver.h treats all of
                              * these identically to a datagram lost in
                              * transit). */
} harness_state;

static int fake_send(void *user, const apad_addr *to, const uint8_t *buf, size_t len) {
    harness_state *hs = (harness_state *)user;
    recorded_send *r;
    apad_header hdr;

    if (hs->count >= MAX_RECORDED_SENDS) {
        return 0; /* harness capacity, not a protocol condition */
    }
    r = &hs->sends[hs->count];
    r->to = *to;
    r->len = (len > sizeof r->buf) ? sizeof r->buf : len;
    memcpy(r->buf, buf, r->len);

    memset(&hdr, 0, sizeof hdr);
    (void)apad_header_decode(buf, len, &hdr);
    r->type       = hdr.type;
    r->session_id = hdr.session_id;
    r->sequence   = hdr.sequence;

    r->send_rc = (hs->count == hs->fail_call_index) ? -1 : 0;
    hs->count++;
    return r->send_rc;
}

static void fake_log(void *user, apad_log_level level, const char *msg) {
    const char *lvl;
    (void)user;
    switch (level) {
    case APAD_LOG_ERROR: lvl = "ERROR"; break;
    case APAD_LOG_WARN:  lvl = "WARN";  break;
    default:              lvl = "INFO";  break;
    }
    printf("    [server %s] %s\n", lvl, msg);
}

static apad_server *make_server(harness_state *hs) {
    apad_server_cfg cfg;

    memset(hs, 0, sizeof *hs);
    hs->fail_call_index = -1;
    memset(&g_backend, 0, sizeof g_backend);

    memset(&cfg, 0, sizeof cfg);
    cfg.on_send        = fake_send;
    cfg.on_log         = fake_log;
    cfg.user           = hs;
    cfg.server_port    = (uint16_t)APAD_DEFAULT_PORT;
    cfg.server_name    = "server-harness";
    cfg.profiles       = NULL;
    cfg.profile_count  = 0;
    return apad_server_create(&cfg, &kRecordingBackend);
}

/* Like make_server(), but with cfg.broadcast_addrs populated -- the S7
 * subnet-directed-broadcast filter (apadserver.h, server/src/server.c
 * is_bad_reply_target()) is entirely host-supplied data, so it is untestable
 * through make_server()'s empty cfg. */
static apad_server *make_server_with_broadcasts(harness_state *hs,
                                                 const apad_addr *bcast,
                                                 size_t bcast_count) {
    apad_server_cfg cfg;

    memset(hs, 0, sizeof *hs);
    hs->fail_call_index = -1;
    memset(&g_backend, 0, sizeof g_backend);

    memset(&cfg, 0, sizeof cfg);
    cfg.on_send              = fake_send;
    cfg.on_log                = fake_log;
    cfg.user                  = hs;
    cfg.server_port           = (uint16_t)APAD_DEFAULT_PORT;
    cfg.server_name           = "server-harness";
    cfg.profiles              = NULL;
    cfg.profile_count         = 0;
    cfg.broadcast_addrs       = bcast;
    cfg.broadcast_addr_count  = bcast_count;
    return apad_server_create(&cfg, &kRecordingBackend);
}

/* ========================================================================
 * PASS/FAIL plumbing, matching tools/loopback-client's convention.
 * ======================================================================== */

static int g_failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  [PASS] %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        g_failures++;
    }
}

static void hexdump(const char *label, const uint8_t *buf, size_t len) {
    size_t i;
    printf("    %s (%u bytes):", label, (unsigned)len);
    for (i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\n");
}

/* ========================================================================
 * Datagram builders. Raw header + apad_packet_build -- this harness plays
 * the client side of the wire by hand, on purpose (see the file header).
 * ======================================================================== */

static int build_raw(uint8_t *buf, size_t cap, uint8_t type, uint16_t session_id,
                      uint16_t sequence, const void *payload, uint16_t payload_len) {
    apad_header hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic      = APAD_MAGIC;
    hdr.version    = (uint8_t)APAD_VERSION;
    hdr.type       = type;
    hdr.session_id = session_id;
    hdr.sequence   = sequence;
    return apad_packet_build(buf, cap, &hdr, payload, payload_len, NULL, 0);
}

static int build_discover(uint8_t *buf, size_t cap, uint16_t session_id) {
    return build_raw(buf, cap, (uint8_t)APAD_MSG_DISCOVER, session_id, 0, NULL, 0);
}

static int build_hello(uint8_t *buf, size_t cap, uint16_t sequence, uint8_t seed,
                        const char *device_name, uint32_t client_ticks_ms) {
    apad_hello h;
    uint8_t payload[APAD_LEN_HELLO];
    int n;

    memset(&h, 0, sizeof h);
    memset(h.client_id, seed, sizeof h.client_id);
    h.caps = APAD_CAP_DPAD | APAD_CAP_FACE4 | APAD_CAP_STICK_L;
    apad_text_set(h.device_name, sizeof h.device_name, device_name);
    memset(h.client_nonce, (int)(uint8_t)(seed ^ 0xFFu), sizeof h.client_nonce);
    h.desired_rate_hz = 0; /* -> server default (APAD_DEFAULT_RATE_HZ) */
    h.proto_major     = (uint8_t)APAD_VERSION;
    h.client_ticks_ms = client_ticks_ms;

    n = apad_encode_hello(payload, sizeof payload, &h);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_HELLO, 0, sequence, payload, (uint16_t)n);
}

static int build_ack(uint8_t *buf, size_t cap, uint16_t session_id, uint16_t sequence,
                      uint16_t acked_seq) {
    apad_ack a;
    uint8_t payload[APAD_LEN_ACK];
    int n;

    memset(&a, 0, sizeof a);
    a.sequence = acked_seq;
    n = apad_encode_ack(payload, sizeof payload, &a);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_ACK, session_id, sequence, payload, (uint16_t)n);
}

static int build_input_state(uint8_t *buf, size_t cap, uint16_t session_id,
                              uint16_t sequence, uint32_t client_ticks_ms) {
    apad_input_state st;
    uint8_t payload[APAD_LEN_INPUT_STATE];
    int n;

    memset(&st, 0, sizeof st);
    st.buttons = APAD_BTN_A;
    st.axes[APAD_AXIS_LX] = 1234;
    st.axes[APAD_AXIS_LY] = -1234;
    st.client_ticks_ms = client_ticks_ms;
    n = apad_encode_input_state(payload, sizeof payload, &st);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_INPUT_STATE, session_id, sequence,
                     payload, (uint16_t)n);
}

static int decode_error_code(const recorded_send *r, uint16_t *code_out) {
    apad_packet pkt;
    apad_error e;

    memset(&pkt, 0, sizeof pkt);
    if (apad_packet_parse(r->buf, r->len, &pkt) < 0) {
        return 0;
    }
    if (pkt.header.type != (uint8_t)APAD_MSG_ERROR) {
        return 0;
    }
    memset(&e, 0, sizeof e);
    if (apad_decode_error(pkt.payload, pkt.payload_len, &e) < 0) {
        return 0;
    }
    *code_out = e.code;
    return 1;
}

/* One HELLO -> WELCOME round trip. Returns 1 and fills *session_id_out /
 * *welcome_seq_out iff exactly the expected WELCOME landed. */
static int establish_hello(apad_server *srv, harness_state *hs, apad_addr peer,
                           uint32_t t0, uint8_t seed, const char *device_name,
                           uint16_t hello_seq, uint8_t hello_buf_out[APAD_MAX_DATAGRAM],
                           int *hello_len_out, uint16_t *session_id_out,
                           uint16_t *welcome_seq_out) {
    int n = build_hello(hello_buf_out, APAD_MAX_DATAGRAM, hello_seq, seed, device_name, t0);
    if (n < 0) {
        return 0;
    }
    *hello_len_out = n;
    (void)apad_server_on_datagram(srv, t0, &peer, hello_buf_out, (size_t)n);
    if (hs->count < 1) {
        return 0;
    }
    if (hs->sends[hs->count - 1].type != (uint8_t)APAD_MSG_WELCOME) {
        return 0;
    }
    *session_id_out  = hs->sends[hs->count - 1].session_id;
    *welcome_seq_out = hs->sends[hs->count - 1].sequence;
    return 1;
}

/* ========================================================================
 * Scenario group A: DISCOVER / ANNOUNCE (docs/PROTOCOL.md S6.1, S7, S8).
 * ======================================================================== */

static void scenario_discover_plain(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr from;
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n;

    printf("\n-- DISCOVER, session_id=0 (ordinary tier-2 discovery) --\n");
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&from, 10, 0, 0, 5, 40000);
    n = build_discover(buf, sizeof buf, 0);
    check(n > 0, "build DISCOVER datagram");

    (void)apad_server_on_datagram(srv, 1000, &from, buf, (size_t)n);
    check(hs.count == 1, "exactly one reply datagram (S7 tier 2: DISCOVER draws an ANNOUNCE)");
    if (hs.count >= 1) {
        check(hs.sends[0].type == (uint8_t)APAD_MSG_ANNOUNCE,
              "the reply is ANNOUNCE (S4 message table)");
        check(apad_addr_equal(&hs.sends[0].to, &from) != 0,
              "ANNOUNCE unicasts back to the exact source (S7 tier 2: "
              "'ANNOUNCE unicast back to the source')");
        check(hs.sends[0].session_id == 0,
              "ANNOUNCE header session_id is 0 (S8: 'session_id is 0 in "
              "DISCOVER, ANNOUNCE and HELLO')");
        {
            apad_packet pkt;
            apad_announce ann;
            memset(&pkt, 0, sizeof pkt);
            memset(&ann, 0, sizeof ann);
            check(apad_packet_parse(hs.sends[0].buf, hs.sends[0].len, &pkt) >= 0
                  && apad_decode_announce(pkt.payload, pkt.payload_len, &ann) >= 0,
                  "ANNOUNCE payload decodes as 40 bytes (S6.2)");
            check(ann.server_port == (uint16_t)APAD_DEFAULT_PORT,
                  "ANNOUNCE.server_port carries cfg.server_port, not a "
                  "socket-derived value (apadserver.h: 'the library has no "
                  "socket to ask')");
        }
    }
    apad_server_destroy(srv);
}

static void scenario_discover_unknown_session(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr from;
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n;

    printf("\n-- DISCOVER, session_id=0x1234 (unknown, non-zero) --\n");
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&from, 10, 0, 0, 5, 40000);
    n = build_discover(buf, sizeof buf, 0x1234);
    check(n > 0, "build DISCOVER datagram");

    (void)apad_server_on_datagram(srv, 1000, &from, buf, (size_t)n);
    check(hs.count == 1, "exactly one reply datagram (not zero, not two)");
    if (hs.count >= 1) {
        uint16_t code = 0;
        check(hs.sends[0].type == (uint8_t)APAD_MSG_ERROR,
              "the reply is ERROR, not ANNOUNCE (S8: 'a server receiving a "
              "packet with an unknown non-zero session_id MUST reply ERROR "
              "code 7 and MUST NOT create a session')");
        check(hs.sends[0].session_id == 0 && hs.sends[0].sequence == 0,
              "the ERROR carries session_id 0 / sequence 0 (S9: 'a datagram "
              "sent outside any session... carries sequence 0')");
        check(decode_error_code(&hs.sends[0], &code) && code == 7u,
              "ERROR.code is 7, unknown session (S8, S6.11)");
    }
    apad_server_destroy(srv);
}

static void scenario_discover_spoofed(const uint8_t ip[4], const char *label) {
    harness_state hs;
    apad_server *srv;
    apad_addr from;
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n;
    char what[320];

    printf("\n-- DISCOVER spoofed from %s --\n", label);
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&from, ip[0], ip[1], ip[2], ip[3], 9999);
    n = build_discover(buf, sizeof buf, 0);
    check(n > 0, "build DISCOVER datagram");

    (void)apad_server_on_datagram(srv, 1000, &from, buf, (size_t)n);
    snprintf(what, sizeof what,
             "no datagram sent at all for a DISCOVER claiming to be from %s "
             "(inferred from S7: 'ANNOUNCE unicast back to the source' -- "
             "%s cannot be the source of a unicast reply; docs/PROTOCOL.md "
             "does not spell out source-address filtering as its own MUST, "
             "see the report)",
             label, label);
    check(hs.count == 0, what);
    apad_server_destroy(srv);
}

/* Host-supplied subnet-directed broadcast filtering: docs/PROTOCOL.md §7
 * ("a broadcast address (255.255.255.255, or a subnet-directed broadcast it
 * can identify)"), apadserver.h cfg.broadcast_addrs, server/src/server.c
 * is_bad_reply_target(). Unlike scenario_discover_spoofed() above (a
 * global/multicast/all-zero address the library rejects with NO host help),
 * a subnet broadcast like 192.168.1.255 is only rejected when the host told
 * the server it owns that address -- so this scenario configures the server
 * with one via make_server_with_broadcasts() and checks two things in one
 * server instance: the configured broadcast address draws no ANNOUNCE, and
 * an ordinary unicast address on the SAME /24 still draws one (no regression
 * for a normal client sharing that subnet). */
static void scenario_discover_subnet_broadcast(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr configured_bcast;
    apad_addr from;
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n;

    printf("\n-- DISCOVER spoofed from a host-configured subnet broadcast "
           "(192.168.1.255/24) --\n");

    apad_addr_set(&configured_bcast, 192, 168, 1, 255, 0);   /* port ignored */
    srv = make_server_with_broadcasts(&hs, &configured_bcast, 1);
    check(srv != NULL, "apad_server_create with cfg.broadcast_addrs = "
                       "{192.168.1.255}");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&from, 192, 168, 1, 255, 9999);
    n = build_discover(buf, sizeof buf, 0);
    check(n > 0, "build DISCOVER datagram");
    (void)apad_server_on_datagram(srv, 1000, &from, buf, (size_t)n);
    check(hs.count == 0,
          "no datagram sent at all for a DISCOVER claiming to be from "
          "192.168.1.255 -- this server was TOLD that is its own subnet "
          "broadcast address (§7: 'a subnet-directed broadcast it can "
          "identify')");

    apad_server_destroy(srv);

    printf("\n-- DISCOVER from an ordinary address on the same /24 "
           "(no regression) --\n");
    srv = make_server_with_broadcasts(&hs, &configured_bcast, 1);
    check(srv != NULL, "apad_server_create with cfg.broadcast_addrs = "
                       "{192.168.1.255}");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&from, 192, 168, 1, 50, 41050);
    n = build_discover(buf, sizeof buf, 0);
    check(n > 0, "build DISCOVER datagram");
    (void)apad_server_on_datagram(srv, 1000, &from, buf, (size_t)n);
    check(hs.count == 1 && hs.sends[0].type == (uint8_t)APAD_MSG_ANNOUNCE,
          "a normal unicast client on the same /24 (192.168.1.50) still "
          "gets its ANNOUNCE -- the subnet-broadcast filter matches the "
          "configured address exactly, it does not reject the whole "
          "subnet");

    apad_server_destroy(srv);
}

/* ========================================================================
 * Scenario B: a HELLO delivered before ANY apad_server_tick() call, at a
 * clock value far from zero. apadserver.h names this exact bug: an earlier
 * API took the clock from the last tick instead of a parameter, so a host
 * that delivered before its first tick built a session stamped with a clock
 * of 0 that the next tick reaped as a 3000ms idle timeout.
 * ======================================================================== */

static void scenario_before_first_tick(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int hello_len, n;
    uint16_t sid = 0, wseq = 0;

    printf("\n-- HELLO delivered BEFORE any apad_server_tick() call, clock at 60000ms --\n");
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&peer, 10, 0, 0, 7, 41007);
    check(establish_hello(srv, &hs, peer, 60000u, 0x30, "harness-E", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO at t=60000, before any tick, draws a WELCOME (S8 handshake)");

    (void)apad_server_tick(srv, 60000u);
    check(hs.count == 1,
          "the tick immediately following, at the SAME clock value, does not "
          "tear the session down (apadserver.h: now_ms is an explicit "
          "parameter to both entry points, never a clock stashed from the "
          "last tick; S8's 3-second idle rule has nothing to fire against "
          "here)");

    n = build_input_state(buf, sizeof buf, sid, 2, 60100u);
    check(n > 0, "build INPUT_STATE");
    (void)apad_server_on_datagram(srv, 60100u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 1,
          "INPUT_STATE reaches the backend afterward -- the session survived "
          "the tick that follows its first packet");

    apad_server_destroy(srv);
}

/* ========================================================================
 * Scenario C: the S8/S11 3000ms idle timeout, exact boundary behaviour.
 * Two separate server instances (one per timing point) rather than one
 * chained test: interacting with a session to observe its state also
 * refreshes its idle timer (S8), so the only clean way to test "still alive
 * at t=X" without that observation itself being what keeps it alive is to
 * tick first and interact second, once, per instance.
 * ======================================================================== */

static void scenario_idle_timeout(int expect_torn_down, uint32_t tick_at) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int hello_len, n;
    uint16_t sid = 0, wseq = 0;
    char what[320];

    printf("\n-- S8/S11 idle timeout: session established at t=0, single tick at t=%u --\n",
           (unsigned)tick_at);
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&peer, 10, 0, 0, 8, 41008);
    check(establish_hello(srv, &hs, peer, 0u, 0x40, "harness-idle", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO at t=0 draws a WELCOME");
    n = build_ack(buf, sizeof buf, sid, 2, wseq);
    check(n > 0, "build ACK for the WELCOME");
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    /* last_rx_ms is now 0 (the ACK itself refreshed it, S8: "any datagram
     * that passes S3.1 refreshes the idle timer"). Nothing else is sent
     * from here on, so the single tick below is the only further clock
     * input the idle check ever sees. */

    (void)apad_server_tick(srv, tick_at);

    n = build_input_state(buf, sizeof buf, sid, 3, tick_at);
    (void)apad_server_on_datagram(srv, tick_at, &peer, buf, (size_t)n);

    if (expect_torn_down) {
        snprintf(what, sizeof what,
                 "INPUT_STATE at t=%u does NOT reach the backend -- the "
                 "session was torn down by the tick just before it (S8: 'a "
                 "session with no packet received for 3 seconds MUST be "
                 "torn down')", (unsigned)tick_at);
        check(g_backend.update_calls == 0, what);
    } else {
        snprintf(what, sizeof what,
                 "INPUT_STATE at t=%u still reaches the backend -- the "
                 "session survived (fewer than 3 seconds have elapsed since "
                 "the ACK at t=0, S8)", (unsigned)tick_at);
        check(g_backend.update_calls == 1, what);
    }

    apad_server_destroy(srv);
}

/* ========================================================================
 * Scenario D: the S9 100/200/400/800ms retransmit schedule, combined with
 * the FAILED-SEND case (on_send returns -1 for the WELCOME's first send)
 * and the recovery case (a late ACK still lands). One function, three
 * callers, because the schedule-walking logic is identical in all three;
 * what differs is whether the first send fails and when (if ever) the
 * client ACKs.
 * ======================================================================== */

static void scenario_retransmit(const char *title, int fail_first_send, int ack_after_idx) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int hello_len, n, i;
    uint16_t sid = 0, wseq = 0;
    static const uint32_t boundaries[4] = {100u, 300u, 700u, 1500u};
    char what[320];

    printf("\n-- %s --\n", title);
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }
    if (fail_first_send) {
        hs.fail_call_index = 0;
    }

    apad_addr_set(&peer, 10, 0, 0, 9, 41009);
    check(establish_hello(srv, &hs, peer, 0u, 0x50, "harness-retx", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO at t=0 draws exactly one WELCOME");
    if (fail_first_send) {
        check(hs.count >= 1 && hs.sends[0].send_rc == -1,
              "on_send DID return -1 for that first WELCOME, as configured "
              "(this checks the harness's own fault injection, not the "
              "server)");
    }

    for (i = 0; i < 4; i++) {
        snprintf(what, sizeof what, "no retransmit 1ms before t=%u", (unsigned)boundaries[i]);
        (void)apad_server_tick(srv, boundaries[i] - 1u);
        check(hs.count == 1 + i, what);

        snprintf(what, sizeof what,
                 "retransmit #%d fires exactly at t=%u (S9: 'the four "
                 "retransmits go out at t = 100, 300, 700 and 1500 ms')",
                 i + 1, (unsigned)boundaries[i]);
        (void)apad_server_tick(srv, boundaries[i]);
        check(hs.count == 2 + i, what);

        if (hs.count == 2 + i) {
            int identical = hs.sends[i + 1].len == hs.sends[0].len
                            && memcmp(hs.sends[i + 1].buf, hs.sends[0].buf,
                                      hs.sends[0].len) == 0;
            check(identical,
                  "the retransmit is byte-identical to the original WELCOME, "
                  "including its header sequence (S9 Duplicates: 'A "
                  "retransmission MUST be byte-identical to the original, "
                  "including its sequence')");
            if (!identical) {
                hexdump("original ", hs.sends[0].buf, hs.sends[0].len);
                hexdump("retransmit", hs.sends[i + 1].buf, hs.sends[i + 1].len);
            }
        }

        if (ack_after_idx == i) {
            n = build_ack(buf, sizeof buf, sid, (uint16_t)(2 + i),
                         hs.sends[hs.count - 1].sequence);
            check(n > 0, "build ACK for the retransmitted WELCOME");
            (void)apad_server_on_datagram(srv, boundaries[i], &peer, buf, (size_t)n);
            break;
        }
    }

    if (ack_after_idx >= 0) {
        /* Deliberately stop well short of 3000ms after the ACK: S8's idle
         * timeout is independent of S9's retransmit schedule and would
         * (correctly) tear the session down on its own if this loop ran the
         * clock out that far with nothing else received -- that is a
         * different rule than the one this branch is checking, and letting
         * it fire here would misattribute an idle teardown to a retransmit
         * bug. 2900ms after an ACK at t<=1500 stays under the idle cutoff
         * with margin. */
        static const uint32_t later[] = {300u, 700u, 1500u, 2300u};
        size_t j;
        int before_count = hs.count;

        for (j = 0; j < sizeof later / sizeof later[0]; j++) {
            (void)apad_server_tick(srv, later[j]);
        }
        /* Checked by TYPE, not raw count, for the same reason as
         * scenario_ack_immediately_disarms above: once the ACK discharges
         * the WELCOME, this session is ACTIVE with nothing retx_armed, and
         * the server now legitimately originates its own §6.6 PING once a
         * second (apadserver.h apad_server_tick() doc comment) -- this
         * session was created at t=0, so exactly one is due somewhere in
         * `later[]` (at t=1500, the first point >=1000ms of elapsed
         * session time). What must still never happen is a SECOND copy of
         * the WELCOME. */
        check(hs.count == before_count || hs.count == before_count + 1,
              "no further send after the ACK, through t=2300, beyond at "
              "most one legitimate 1 Hz PING (S9: 'A reliable message stops "
              "retransmitting when it is discharged... an explicit ACK "
              "echoing its sequence')");
        if (hs.count == before_count + 1) {
            check(hs.sends[hs.count - 1].type == (uint8_t)APAD_MSG_PING,
                  "the one extra send after the ACK is this server's own "
                  "§6.6 PING, not a WELCOME retransmit");
        }

        n = build_input_state(buf, sizeof buf, sid, 90, 2900u);
        (void)apad_server_on_datagram(srv, 2900u, &peer, buf, (size_t)n);
        check(g_backend.update_calls == 1,
              "INPUT_STATE now reaches the backend -- the session is ACTIVE "
              "and usable despite the local on_send failure on its very "
              "first WELCOME (apadserver.h: 'nothing in the library's "
              "correctness depends on' honest send-failure reporting)");
    } else {
        check(hs.count == 5,
              "exactly 5 WELCOMEs total: the original plus four S9 retransmits");

        (void)apad_server_tick(srv, 2299u);
        check(hs.count == 5, "still no 6th send just before t=2300");

        (void)apad_server_tick(srv, 2300u);
        check(hs.count == 5,
              "no 6th WELCOME at/after t=2300: retransmits exhausted (S9: "
              "'if no ACK has arrived by t = 2300 ms the session fails')");

        n = build_input_state(buf, sizeof buf, sid, 90, 2400u);
        (void)apad_server_on_datagram(srv, 2400u, &peer, buf, (size_t)n);
        check(g_backend.update_calls == 0,
              "INPUT_STATE after t=2300 does not reach the backend -- the "
              "un-ACKed session is gone (S9: 'the session fails')");

        (void)apad_server_tick(srv, 10000u);
        check(hs.count == 5,
              "ticking far into the future sends nothing more (no zombie "
              "retransmit loop)");
    }

    apad_server_destroy(srv);
}

/* ========================================================================
 * Scenario E: the ACK arrives immediately -- no retransmit ever fires. The
 * mirror image of scenario_retransmit's "never ACK" case, and the other
 * extreme worth checking on its own: S9's "stops retransmitting when
 * discharged" has to hold even though the schedule below WOULD have fired
 * four times if nothing had disarmed it.
 * ======================================================================== */

static void scenario_ack_immediately_disarms(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int hello_len, n;
    uint16_t sid = 0, wseq = 0;
    /* Stop short of 3000ms, same reasoning as scenario_retransmit's
     * ack_after_idx branch above: S8's idle timeout is a separate rule from
     * S9's retransmit schedule and would tear the session down on its own
     * past that point, which would misattribute an idle teardown to this
     * scenario's actual subject (whether the ACK disarmed the retransmit). */
    static const uint32_t points[] = {100u, 300u, 700u, 1500u, 2300u};
    size_t i;

    printf("\n-- ACK for WELCOME arrives at t=0: no retransmit ever fires --\n");
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&peer, 10, 0, 0, 11, 41011);
    check(establish_hello(srv, &hs, peer, 0u, 0x70, "harness-ack-fast", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO at t=0 draws exactly one WELCOME");
    n = build_ack(buf, sizeof buf, sid, 2, wseq);
    check(n > 0, "build ACK");
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(hs.count == 1, "the ACK itself draws no reply (S4: ACK is not reliable)");

    for (i = 0; i < sizeof points / sizeof points[0]; i++) {
        char what[224];
        int before = hs.count;
        (void)apad_server_tick(srv, points[i]);
        /* S9's own subject (no S9 retransmit of the WELCOME) still holds all
         * the way through -- checked by TYPE, not by raw count, because the
         * server now legitimately originates its own §6.6 PING at 1 Hz once
         * a session is ACTIVE and its handshake is discharged (apadserver.h
         * apad_server_tick()'s doc comment). This session's WELCOME was
         * ACKed at t=0, so nothing is retx_armed from t=0 onward, and the
         * very first tick at >=1000ms elapsed (t=1500 in `points`) is
         * exactly when that PING is due -- one extra send, and it must be a
         * PING, never a second copy of the WELCOME. */
        snprintf(what, sizeof what,
                 "no S9 retransmit of the WELCOME at t=%u (S9: 'stops "
                 "retransmitting when it is discharged... an explicit ACK "
                 "echoing its sequence') -- only the legitimate 1 Hz PING "
                 "(if any) may have been added",
                 (unsigned)points[i]);
        check(hs.count == before || hs.count == before + 1, what);
        if (hs.count == before + 1) {
            snprintf(what, sizeof what,
                     "the one extra send at t=%u is this server's own §6.6 "
                     "PING, not a WELCOME retransmit", (unsigned)points[i]);
            check(hs.sends[hs.count - 1].type == (uint8_t)APAD_MSG_PING, what);
        }
    }
    check(hs.count == 2,
          "exactly one extra send happened across the whole loop (the 1 Hz "
          "PING due once at t=1500) -- confirms it fired once, not once per "
          "tick");

    n = build_input_state(buf, sizeof buf, sid, 3, 2900u);
    (void)apad_server_on_datagram(srv, 2900u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 1,
          "INPUT_STATE reaches the backend: the session is alive and well "
          "past where an un-ACKed one would already have failed");

    apad_server_destroy(srv);
}

/* ========================================================================
 * Scenario F: duplicate HELLO -> byte-identical original WELCOME.
 * ======================================================================== */

static void scenario_duplicate_hello(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    int hello_len;
    uint16_t sid = 0, wseq = 0;

    printf("\n-- duplicate HELLO (S9 'Duplicates') --\n");
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    apad_addr_set(&peer, 10, 0, 0, 10, 41010);
    check(establish_hello(srv, &hs, peer, 0u, 0x60, "harness-dup", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO at t=0 draws exactly one WELCOME");

    /* Resend the SAME datagram bytes, well before the S9 retransmit
     * schedule (100ms) would fire on its own -- the only thing that can
     * cause a second WELCOME here is the server's duplicate-HELLO handling,
     * not the timer. */
    (void)apad_server_on_datagram(srv, 50u, &peer, hello_buf, (size_t)hello_len);
    check(hs.count == 2, "exactly one more datagram is sent in reply to the duplicate HELLO");
    if (hs.count == 2) {
        int identical = hs.sends[1].len == hs.sends[0].len
                        && memcmp(hs.sends[1].buf, hs.sends[0].buf, hs.sends[0].len) == 0;
        check(hs.sends[1].type == (uint8_t)APAD_MSG_WELCOME,
              "the reply to a duplicate HELLO is WELCOME");
        check(identical,
              "the duplicate HELLO's answer is byte-identical to the "
              "original WELCOME (S9 Duplicates: 'a peer that receives a "
              "duplicate of a request it has already answered MUST "
              "retransmit its original answer verbatim, and MUST NOT "
              "generate a fresh one')");
        if (!identical) {
            hexdump("original WELCOME ", hs.sends[0].buf, hs.sends[0].len);
            hexdump("duplicate response", hs.sends[1].buf, hs.sends[1].len);
        }
    }

    apad_server_destroy(srv);
}


/* ========================================================================
 * Scenario group F: §10 pairing (server/src/pairing.c, server/src/server.c).
 *
 * Everything below drives the SAME public API a UI would
 * (apad_server_begin_pairing / _cancel_pairing / _pairing_state) against
 * the same fake clock, and — critically — supplies randomness through
 * cfg.on_random, which is what lets a test pin down a value that is random
 * in production. scenario_pairing_appendix_a() uses that to make the
 * server produce docs/PROTOCOL.md Appendix A's exact PIN and server_nonce,
 * so its normative key and tag can be checked end to end through the real
 * handshake rather than through a direct call to the crypto.
 * ======================================================================== */

static int is_all_zero(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

/* One scripted on_random response. The feeder serves ONE entry PER CALL:
 * `prefix` first, then `filler` for whatever else the caller asked for.
 * That decouples the test from how many bytes pairing.c happens to draw in
 * one go (it draws a 32-byte block) while still pinning the bytes that
 * matter — the ones the rejection sampler will actually accept, and the 16
 * that become the nonce.
 *
 * Assumption, stated because it is the one thing that would make these
 * scenarios lie if pairing.c changed: exactly one on_random call per
 * secret and one per WELCOME nonce. */
typedef struct {
    uint8_t prefix[32];
    size_t  prefix_len;
    uint8_t filler;
} random_script_entry;

#define MAX_RANDOM_SCRIPT 8

typedef struct {
    random_script_entry entry[MAX_RANDOM_SCRIPT];
    int    count;
    int    next;
    int    calls;
    int    fail;        /* 1: every call fails, imitating a dead RNG */
    uint32_t prng;      /* non-zero: serve a deterministic stream instead */
} random_state;

static random_state g_random;

static int scripted_random(void *user, uint8_t *buf, size_t len) {
    const random_script_entry *e;
    size_t i;
    (void)user;

    g_random.calls++;
    if (g_random.fail) {
        return 0;
    }
    if (g_random.prng != 0u) {
        /* xorshift32: deterministic, reproducible, and obviously NOT a CSPRNG
         * — it is a test double standing in for getrandom(). */
        for (i = 0; i < len; i++) {
            g_random.prng ^= g_random.prng << 13;
            g_random.prng ^= g_random.prng >> 17;
            g_random.prng ^= g_random.prng << 5;
            buf[i] = (uint8_t)(g_random.prng & 0xFFu);
        }
        return 1;
    }
    if (g_random.next >= g_random.count) {
        return 0;   /* script exhausted: a failure, not silent garbage */
    }
    e = &g_random.entry[g_random.next++];
    for (i = 0; i < len; i++) {
        buf[i] = (i < e->prefix_len) ? e->prefix[i] : e->filler;
    }
    return 1;
}

static void random_script_reset(void) {
    memset(&g_random, 0, sizeof g_random);
}

static void random_script_push(const uint8_t *prefix, size_t len, uint8_t filler) {
    random_script_entry *e;
    if (g_random.count >= MAX_RANDOM_SCRIPT) {
        return;
    }
    e = &g_random.entry[g_random.count++];
    memset(e, 0, sizeof *e);
    if (prefix != NULL && len <= sizeof e->prefix) {
        memcpy(e->prefix, prefix, len);
        e->prefix_len = len;
    }
    e->filler = filler;
}

/* make_server(), plus cfg.on_random. Every scenario OUTSIDE this group
 * leaves on_random NULL, which is itself part of the backward-compatibility
 * story: a host that never heard of pairing keeps working. */
static apad_server *make_server_pairing(harness_state *hs) {
    apad_server_cfg cfg;

    memset(hs, 0, sizeof *hs);
    hs->fail_call_index = -1;
    memset(&g_backend, 0, sizeof g_backend);

    memset(&cfg, 0, sizeof cfg);
    cfg.on_send        = fake_send;
    cfg.on_log         = fake_log;
    cfg.on_random      = scripted_random;
    cfg.user           = hs;
    cfg.server_port    = (uint16_t)APAD_DEFAULT_PORT;
    cfg.server_name    = "server-harness";
    return apad_server_create(&cfg, &kRecordingBackend);
}

/* Build an authenticated datagram: same shape as build_raw(), but with a
 * key, so codec.c sets APAD_FLAG_AUTHENTICATED and appends the 8-byte tag. */
static int build_raw_auth(uint8_t *buf, size_t cap, uint8_t type,
                          uint16_t session_id, uint16_t sequence,
                          const void *payload, uint16_t payload_len,
                          const uint8_t key[APAD_SESSION_KEY_LEN]) {
    apad_header hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic      = APAD_MAGIC;
    hdr.version    = (uint8_t)APAD_VERSION;
    hdr.type       = type;
    hdr.session_id = session_id;
    hdr.sequence   = sequence;
    return apad_packet_build(buf, cap, &hdr, payload, payload_len,
                             key, (size_t)APAD_SESSION_KEY_LEN);
}

/* Pull the server_nonce out of the WELCOME the server just sent. */
static int welcome_of(const recorded_send *r, apad_welcome *out) {
    apad_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    if (apad_packet_parse(r->buf, r->len, &pkt) < 0
        || pkt.header.type != (uint8_t)APAD_MSG_WELCOME) {
        return 0;
    }
    return apad_decode_welcome(pkt.payload, pkt.payload_len, out) >= 0;
}

/* HELLO -> AUTH_REQUIRED WELCOME, then derive the session key exactly as a
 * real client would: from the PIN the user is reading off the screen and
 * the nonce that arrived in the WELCOME. */
static int pair_and_hello(apad_server *srv, harness_state *hs, apad_addr peer,
                          uint32_t t, uint16_t hello_seq, uint8_t seed,
                          uint8_t key_out[APAD_SESSION_KEY_LEN],
                          uint16_t *session_id_out) {
    uint8_t hello[APAD_MAX_DATAGRAM];
    int hello_len;
    uint16_t sid, wseq;
    apad_welcome w;
    apad_pairing_info info;

    if (!establish_hello(srv, hs, peer, t, seed, "harness-pair", hello_seq,
                         hello, &hello_len, &sid, &wseq)) {
        return 0;
    }
    if (!welcome_of(&hs->sends[hs->count - 1], &w)) {
        return 0;
    }
    if ((w.flags & APAD_WELCOME_AUTH_REQUIRED) == 0u) {
        return 0;
    }
    if (apad_server_pairing_state(srv, &info) != APAD_OK || !info.open) {
        return 0;
    }
    apad_derive_session_key(info.secret, w.server_nonce, key_out);
    *session_id_out = sid;
    return 1;
}

static void scenario_pairing_needs_entropy(void) {
    harness_state hs;
    apad_server *srv;
    apad_pairing_info info;

    printf("\n-- pairing refuses to open without usable entropy (apadserver.h: "
           "no clock-derived fallback, ever) --\n");

    /* (a) a host that supplied no on_random at all -- make_server(), which
     * is what every other scenario in this file uses. */
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_ERR_STATE,
          "cfg.on_random == NULL -> begin_pairing returns APAD_ERR_STATE");
    check(apad_server_pairing_state(srv, &info) == APAD_OK && info.open == 0,
          "...and no window is open");
    check(info.secret[0] == '\0',
          "...and the snapshot carries no secret at all");
    apad_server_destroy(srv);

    /* (b) an on_random that exists but fails at the moment it is asked. */
    srv = make_server_pairing(&hs);
    random_script_reset();
    g_random.fail = 1;
    check(srv != NULL, "apad_server_create (with on_random)");
    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_ERR_STATE,
          "on_random FAILS -> begin_pairing returns APAD_ERR_STATE");
    check(g_random.calls > 0, "...and it really was called");
    check(apad_server_pairing_state(srv, &info) == APAD_OK && info.open == 0,
          "...and no window opened on a partially-filled buffer");
    apad_server_destroy(srv);
}

/*
 * docs/PROTOCOL.md Appendix A, driven through the real handshake.
 *
 * The scripted on_random makes the server generate Appendix A's PIN
 * ("123456", via the rejection sampler accepting bytes 1..6) and Appendix
 * A's server_nonce (00 01 .. 0F). Everything after that is the shipping
 * code path: the server derives the session key with
 * apad_derive_session_key() inside handle_hello, and then the byte-exact
 * Appendix A PING -- tag F8 4C BA C6 CE 34 B1 AE and all -- is fed in as a
 * datagram and must be ACCEPTED, which can only happen if the derived key
 * matches to the last bit.
 *
 * This is a stronger claim than calling the KDF and comparing 32 bytes: it
 * also pins the salt (per-session nonce reaching WELCOME intact), the flag
 * (AUTH_REQUIRED), the ORDER (key installed after WELCOME is sent, so
 * WELCOME itself is untagged), and the verify path in check_auth().
 */
static void scenario_pairing_appendix_a(void) {
    static const uint8_t kPinDigits[6]  = {1u, 2u, 3u, 4u, 5u, 6u};
    static const uint8_t kNonce[16] = {
        0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
        0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu
    };
    static const uint8_t kExpectedKey[APAD_SESSION_KEY_LEN] = {
        0xA9u, 0x66u, 0x08u, 0x61u, 0xD6u, 0x11u, 0xD4u, 0x6Au,
        0x19u, 0x19u, 0x71u, 0xECu, 0xCFu, 0x0Cu, 0xC8u, 0x95u,
        0xEEu, 0x7Cu, 0xD5u, 0x80u, 0x91u, 0xC1u, 0x97u, 0x3Eu,
        0xE6u, 0xD6u, 0x0Au, 0x5Cu, 0x4Fu, 0x30u, 0x42u, 0x19u
    };
    /* The Appendix A datagram exactly as the spec prints it, tag included. */
    static const uint8_t kAppendixPing[28] = {
        0x43u, 0x4Du, 0x01u, 0x30u, 0x01u, 0x00u, 0x02u, 0x00u,
        0x08u, 0x00u, 0x01u, 0x00u,
        0xE8u, 0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0xF8u, 0x4Cu, 0xBAu, 0xC6u, 0xCEu, 0x34u, 0xB1u, 0xAEu
    };
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_pairing_info info;
    apad_welcome w;
    uint8_t hello[APAD_MAX_DATAGRAM];
    uint8_t key[APAD_SESSION_KEY_LEN];
    int hello_len;
    uint16_t sid, wseq;

    printf("\n-- Appendix A end to end: PIN \"123456\", the normative nonce, "
           "the normative key, the normative tag --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");

    random_script_reset();
    random_script_push(kPinDigits, sizeof kPinDigits, 0u);  /* -> "123456" */
    random_script_push(kNonce, sizeof kNonce, 0u);          /* -> server_nonce */

    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_OK,
          "begin_pairing opens a window");
    check(apad_server_pairing_state(srv, &info) == APAD_OK && info.open,
          "pairing_state reports it open");
    check(strcmp(info.secret, "123456") == 0,
          "the generated PIN is Appendix A's \"123456\"");
    printf("    PIN = \"%s\", %u ms left, %u attempts, generation %u\n",
           info.secret, (unsigned)info.ms_remaining,
           (unsigned)info.attempts_remaining, (unsigned)info.generation);
    check(info.ms_remaining == (uint32_t)APAD_PAIRING_WINDOW_MS,
          "120 s remaining at t=open (§11)");
    check(info.attempts_remaining == (uint8_t)APAD_MAX_PAIR_ATTEMPTS,
          "5 attempts remaining (§11)");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 192u; peer.ip[1] = 168u; peer.ip[2] = 1u; peer.ip[3] = 77u;
    peer.port = 40001u;

    check(establish_hello(srv, &hs, peer, 1000u, 0xA1u, "harness-appendixA", 1,
                          hello, &hello_len, &sid, &wseq),
          "HELLO -> WELCOME");
    check(sid == 1u, "session_id 1, matching Appendix A's header");
    check(welcome_of(&hs.sends[hs.count - 1], &w), "WELCOME decodes");
    check((w.flags & APAD_WELCOME_AUTH_REQUIRED) != 0u,
          "WELCOME.flags has AUTH_REQUIRED (§6.4 bit 0)");
    check(memcmp(w.server_nonce, kNonce, sizeof kNonce) == 0,
          "WELCOME.server_nonce is Appendix A's salt");
    {
        uint8_t zero[APAD_KEY_MATERIAL_LEN];
        memset(zero, 0, sizeof zero);
        check(memcmp(w.key_material, zero, sizeof zero) == 0,
              "WELCOME.key_material is still all zero (§6.4 MUST, v1)");
    }
    check((hs.sends[hs.count - 1].buf[10] & APAD_FLAG_AUTHENTICATED) == 0u,
          "the WELCOME itself is NOT tagged: §10's tag starts on the packet "
          "AFTER it, and it is the packet carrying the salt");

    /* What a client computes, and what the server must have computed. */
    apad_derive_session_key(info.secret, w.server_nonce, key);
    hexdump("derived session key", key, sizeof key);
    hexdump("Appendix A expects ", kExpectedKey, sizeof kExpectedKey);
    check(memcmp(key, kExpectedKey, sizeof key) == 0,
          "PBKDF2-HMAC-SHA256(PIN, nonce, 10000) == Appendix A's 32 bytes");

    /* The byte-exact Appendix A PING. If the server's own derivation had
     * drifted by one bit this is rejected and no PONG comes back. */
    {
        int before = hs.count;
        hexdump("Appendix A PING   ", kAppendixPing, sizeof kAppendixPing);
        (void)apad_server_on_datagram(srv, 1100u, &peer, kAppendixPing,
                                      sizeof kAppendixPing);
        check(hs.count == before + 1,
              "the normative authenticated PING is ACCEPTED and answered");
        if (hs.count == before + 1) {
            const recorded_send *r = &hs.sends[hs.count - 1];
            check(r->type == (uint8_t)APAD_MSG_PONG, "the answer is a PONG");
            check((r->buf[10] & APAD_FLAG_AUTHENTICATED) != 0u,
                  "the PONG is itself AUTHENTICATED (§10: every packet after "
                  "WELCOME)");
            check(apad_packet_verify(r->buf, r->len, key,
                                     (size_t)APAD_SESSION_KEY_LEN) == APAD_OK,
                  "...and its tag verifies under the same derived key");
            hexdump("server PONG       ", r->buf, r->len);
        }
    }

    /* §13's "at least one single-bit flip MUST fail verification", applied
     * to the server rather than to the codec: flip one bit of the payload
     * and the same datagram must now be rejected. */
    {
        uint8_t flipped[28];
        int before = hs.count;
        memcpy(flipped, kAppendixPing, sizeof flipped);
        flipped[12] ^= 0x01u;   /* one bit of origin_ticks_ms */
        (void)apad_server_on_datagram(srv, 1200u, &peer, flipped,
                                      sizeof flipped);
        check(hs.count > before
                  && hs.sends[hs.count - 1].type == (uint8_t)APAD_MSG_ERROR,
              "one flipped payload bit -> rejected with an ERROR (§3.1 check 7)");
        if (hs.count > before) {
            uint16_t code = 0;
            check(decode_error_code(&hs.sends[hs.count - 1], &code) && code == 3u,
                  "...ERROR code 3, \"authentication failed\" (§6.11)");
        }
        check(g_backend.destroy_calls == 1,
              "...and the session was torn down, releasing its virtual pad");
    }

    apad_server_destroy(srv);
}

static void scenario_pairing_window_expiry(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_pairing_info info;
    apad_welcome w;
    uint8_t hello[APAD_MAX_DATAGRAM];
    int hello_len;
    uint16_t sid, wseq;

    printf("\n-- the 120 s window (§11) expires against the host's clock --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0xC0FFEEu;

    check(apad_server_begin_pairing(srv, 10000u, 0) == APAD_OK,
          "begin_pairing at t=10000");

    (void)apad_server_tick(srv, 10000u + (uint32_t)APAD_PAIRING_WINDOW_MS - 1u);
    check(apad_server_pairing_state(srv, &info) == APAD_OK && info.open,
          "still open 1 ms before the deadline");
    check(info.ms_remaining == 1u, "...with exactly 1 ms left");

    (void)apad_server_tick(srv, 10000u + (uint32_t)APAD_PAIRING_WINDOW_MS);
    check(apad_server_pairing_state(srv, &info) == APAD_OK && !info.open,
          "closed at exactly opened + 120000 ms");
    check(info.secret[0] == '\0' && info.ms_remaining == 0u,
          "...and the snapshot carries nothing a UI could still display");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 10u; peer.ip[3] = 9u; peer.port = 40002u;
    check(establish_hello(srv, &hs, peer, 10000u + 200000u, 0xA2u,
                          "harness-expired", 1, hello, &hello_len, &sid, &wseq),
          "a HELLO after expiry still gets a WELCOME");
    check(welcome_of(&hs.sends[hs.count - 1], &w), "WELCOME decodes");
    check(w.flags == 0u && is_all_zero(w.server_nonce, sizeof w.server_nonce),
          "...an UNAUTHENTICATED one: flags 0, server_nonce all zero");

    apad_server_destroy(srv);
}

/* A window that expires while a handshake is still unproven takes that
 * handshake with it (apadserver.h: "valid for 120 seconds ... and never
 * otherwise"). */
static void scenario_pairing_expiry_drops_unproven(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t key[APAD_SESSION_KEY_LEN];
    uint16_t sid;

    printf("\n-- expiry tears down a handshake that never authenticated --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0xBEEF01u;

    check(apad_server_begin_pairing(srv, 5000u, 0) == APAD_OK, "begin_pairing");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 10u; peer.ip[3] = 11u; peer.port = 40003u;
    check(pair_and_hello(srv, &hs, peer, 5000u, 1, 0xA3u, key, &sid),
          "HELLO -> AUTH_REQUIRED WELCOME");
    check(g_backend.create_calls == 1, "a virtual pad exists for it");

    (void)apad_server_tick(srv, 5000u + (uint32_t)APAD_PAIRING_WINDOW_MS);
    check(g_backend.destroy_calls == 1,
          "the unproven session is torn down with the window, not left to "
          "the §8 3 s idle timeout");

    apad_server_destroy(srv);
}

static void scenario_pairing_lockout(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_pairing_info before, after;
    uint8_t wrong_key[APAD_SESSION_KEY_LEN];
    char first_secret[APAD_PAIRING_SECRET_MAX];
    int i;

    printf("\n-- five failed attempts rotate the secret (§11) --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0x12345u;

    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_OK, "begin_pairing");
    check(apad_server_pairing_state(srv, &before) == APAD_OK && before.open,
          "window open");
    memcpy(first_secret, before.secret, sizeof first_secret);

    memset(wrong_key, 0xAAu, sizeof wrong_key);   /* not the derived key */
    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 10u; peer.ip[3] = 12u;

    for (i = 0; i < (int)APAD_MAX_PAIR_ATTEMPTS; i++) {
        uint8_t hello[APAD_MAX_DATAGRAM];
        uint8_t buf[APAD_MAX_DATAGRAM];
        uint8_t payload[APAD_LEN_ACK];
        apad_ack a;
        int hello_len, n, an;
        uint16_t sid, wseq;
        apad_pairing_info mid;

        peer.port = (uint16_t)(41000u + i);   /* a fresh client each time */
        if (!establish_hello(srv, &hs, peer, 2000u, (uint8_t)(0xB0u + i),
                             "harness-wrongpin", 1, hello, &hello_len, &sid,
                             &wseq)) {
            check(0, "HELLO -> WELCOME");
            break;
        }
        /* The ACK §8 requires for the WELCOME, tagged with a key derived
         * from the WRONG PIN. This is what a user's typo looks like on the
         * wire. */
        memset(&a, 0, sizeof a);
        a.sequence = wseq;
        an = apad_encode_ack(payload, sizeof payload, &a);
        n = build_raw_auth(buf, sizeof buf, (uint8_t)APAD_MSG_ACK, sid, 1,
                           payload, (uint16_t)an, wrong_key);
        (void)apad_server_on_datagram(srv, 2000u, &peer, buf, (size_t)n);

        (void)apad_server_pairing_state(srv, &mid);
        if (i < (int)APAD_MAX_PAIR_ATTEMPTS - 1) {
            char what[96];
            (void)snprintf(what, sizeof what,
                           "attempt %d of %u charged: %u left, PIN unchanged",
                           i + 1, (unsigned)APAD_MAX_PAIR_ATTEMPTS,
                           (unsigned)mid.attempts_remaining);
            check(mid.attempts_remaining
                      == (uint8_t)((int)APAD_MAX_PAIR_ATTEMPTS - (i + 1))
                  && strcmp(mid.secret, first_secret) == 0, what);
        }
    }

    check(apad_server_pairing_state(srv, &after) == APAD_OK && after.open,
          "the window is still open after the fifth failure");
    check(strcmp(after.secret, first_secret) != 0,
          "the secret was REPLACED (§11: five failures invalidate the PIN "
          "and generate a new one)");
    check(after.generation == before.generation + 1u,
          "generation incremented, so a polling UI knows to redraw");
    check(after.attempts_remaining == (uint8_t)APAD_MAX_PAIR_ATTEMPTS,
          "attempts reset for the new secret");
    check(after.ms_remaining < before.ms_remaining
              && after.ms_remaining > 0u,
          "the 120 s deadline was NOT extended by the rotation (§10 anchors "
          "it to the user's action, not to the PIN)");
    printf("    old secret \"%s\" -> new secret \"%s\" (generation %u -> %u)\n",
           first_secret, after.secret, (unsigned)before.generation,
           (unsigned)after.generation);
    check(g_backend.destroy_calls == (int)APAD_MAX_PAIR_ATTEMPTS,
          "every failed session was torn down, so no virtual pad leaked");

    apad_server_destroy(srv);
}

/* One wrong client must not be able to spend the whole allowance by itself:
 * a session is charged AT MOST ONE attempt. */
static void scenario_pairing_one_charge_per_session(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_pairing_info info;
    uint8_t hello[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_INPUT_STATE];
    uint8_t wrong_key[APAD_SESSION_KEY_LEN];
    apad_input_state st;
    int hello_len, n, pn, i;
    uint16_t sid, wseq;

    printf("\n-- a flood of bad tags from ONE session costs ONE attempt --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0x777u;
    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_OK, "begin_pairing");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 10u; peer.ip[3] = 13u; peer.port = 42000u;
    check(establish_hello(srv, &hs, peer, 1000u, 0xC0u, "harness-flood", 1,
                          hello, &hello_len, &sid, &wseq),
          "HELLO -> WELCOME");

    memset(wrong_key, 0x5Au, sizeof wrong_key);
    memset(&st, 0, sizeof st);
    st.buttons = APAD_BTN_A;
    pn = apad_encode_input_state(payload, sizeof payload, &st);

    /* Twenty wrong-key INPUT_STATE datagrams -- what a client with the
     * wrong PIN actually puts on the wire at 60 Hz. */
    for (i = 0; i < 20; i++) {
        n = build_raw_auth(buf, sizeof buf, (uint8_t)APAD_MSG_INPUT_STATE, sid,
                           (uint16_t)(10 + i), payload, (uint16_t)pn, wrong_key);
        (void)apad_server_on_datagram(srv, (uint32_t)(1000 + i), &peer, buf,
                                      (size_t)n);
    }
    (void)apad_server_pairing_state(srv, &info);
    check(info.attempts_remaining
              == (uint8_t)((uint8_t)APAD_MAX_PAIR_ATTEMPTS - 1u),
          "exactly one attempt was charged for twenty bad datagrams");
    check(g_backend.update_calls == 0,
          "not one of them reached the virtual pad");

    apad_server_destroy(srv);
}

/* The bypass that would make all of this decorative: dropping the flag. */
static void scenario_pairing_untagged_is_dropped(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_pairing_info info;
    uint8_t key[APAD_SESSION_KEY_LEN];
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid;
    int n, before;

    printf("\n-- an UNtagged packet on an auth-required session is dropped "
           "(§10: every packet after WELCOME) --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0x99u;
    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_OK, "begin_pairing");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 10u; peer.ip[3] = 14u; peer.port = 43000u;
    check(pair_and_hello(srv, &hs, peer, 1000u, 1, 0xD0u, key, &sid),
          "HELLO -> AUTH_REQUIRED WELCOME");

    /* Plain, unauthenticated INPUT_STATE -- exactly what today's clients
     * send, and exactly what must not reach the pad now. */
    before = hs.count;
    n = build_input_state(buf, sizeof buf, sid, 5u, 1010u);
    (void)apad_server_on_datagram(srv, 1010u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 0,
          "the untagged INPUT_STATE never reaches the backend");
    check(hs.count == before,
          "...and draws no reply at all: an ERROR here would be the §8 "
          "amplifier at up to 125 pkt/s");
    check(apad_server_pairing_state(srv, &info) == APAD_OK
              && info.attempts_remaining == (uint8_t)APAD_MAX_PAIR_ATTEMPTS,
          "...and costs no pairing attempt: it is a client ignoring "
          "AUTH_REQUIRED, not a PIN guess");

    /* The same input, correctly tagged, must go straight through. */
    {
        uint8_t payload[APAD_LEN_INPUT_STATE];
        apad_input_state st;
        int pn;
        memset(&st, 0, sizeof st);
        st.buttons = APAD_BTN_A;
        st.axes[APAD_AXIS_LX] = 4000;
        pn = apad_encode_input_state(payload, sizeof payload, &st);
        n = build_raw_auth(buf, sizeof buf, (uint8_t)APAD_MSG_INPUT_STATE, sid,
                           6u, payload, (uint16_t)pn, key);
        (void)apad_server_on_datagram(srv, 1020u, &peer, buf, (size_t)n);
        check(g_backend.update_calls == 1,
              "the SAME input, correctly tagged, drives the pad");
    }

    apad_server_destroy(srv);
}

static void scenario_pairing_token(void) {
    harness_state hs;
    apad_server *srv;
    apad_pairing_info info;
    size_t i;
    int ok = 1;

    printf("\n-- the QR-shaped token: same window, same derivation, longer "
           "secret --\n");

    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0xABCDEF1u;

    check(apad_server_begin_pairing(srv, 1000u, 1 /* use_token */) == APAD_OK,
          "begin_pairing(use_token=1)");
    check(apad_server_pairing_state(srv, &info) == APAD_OK && info.open,
          "window open");
    check(info.kind == (uint8_t)APAD_PAIRING_TOKEN,
          "kind is APAD_PAIRING_TOKEN, so a UI knows to draw a QR code");
    check(strlen(info.secret) == (size_t)APAD_PAIRING_TOKEN_LEN,
          "20 characters");
    for (i = 0; info.secret[i] != '\0'; i++) {
        char c = info.secret[i];
        if (c == '0' || c == 'O' || c == '1' || c == 'I' || c == 'l') {
            ok = 0;
        }
        if (!((c >= '2' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
            ok = 0;
        }
    }
    check(ok, "every character is from the unambiguous alphabet (no 0/O, no "
              "1/I/l, no lowercase)");
    printf("    token = \"%s\"\n", info.secret);

    apad_server_destroy(srv);
}

/* The backward-compatibility assertion, stated in bytes rather than in
 * prose: with no window open a WELCOME is exactly what it has always been. */
static void scenario_pairing_absent_is_unchanged(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_welcome w;
    uint8_t hello[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int hello_len, n;
    uint16_t sid, wseq;

    printf("\n-- no window open: the wire is byte-for-byte what it was before "
           "pairing existed --\n");

    /* Deliberately make_server(), i.e. cfg.on_random == NULL: the host has
     * never heard of pairing. */
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 127u; peer.ip[3] = 1u; peer.port = 44000u;

    /* ANNOUNCE says pairing_required = 0 (§6.2). */
    n = build_discover(buf, sizeof buf, 0);
    (void)apad_server_on_datagram(srv, 100u, &peer, buf, (size_t)n);
    check(hs.count == 1 && hs.sends[0].type == (uint8_t)APAD_MSG_ANNOUNCE,
          "DISCOVER -> ANNOUNCE");
    {
        apad_packet pkt;
        apad_announce ann;
        memset(&pkt, 0, sizeof pkt);
        memset(&ann, 0, sizeof ann);
        check(apad_packet_parse(hs.sends[0].buf, hs.sends[0].len, &pkt) >= 0
                  && apad_decode_announce(pkt.payload, pkt.payload_len, &ann) >= 0,
              "ANNOUNCE decodes");
        check(ann.pairing_required == 0u, "ANNOUNCE.pairing_required == 0");
    }

    check(establish_hello(srv, &hs, peer, 100u, 0xE0u, "harness-nopair", 1,
                          hello, &hello_len, &sid, &wseq),
          "HELLO -> WELCOME");
    check(welcome_of(&hs.sends[hs.count - 1], &w), "WELCOME decodes");
    check(w.flags == 0u, "WELCOME.flags == 0: no AUTH_REQUIRED");
    check(is_all_zero(w.server_nonce, sizeof w.server_nonce),
          "WELCOME.server_nonce is all zero");
    check(is_all_zero(w.key_material, sizeof w.key_material),
          "WELCOME.key_material is all zero (§6.4)");
    check((hs.sends[hs.count - 1].buf[10] & APAD_FLAG_AUTHENTICATED) == 0u,
          "the WELCOME datagram is untagged");

    /* Plain INPUT_STATE still drives the pad, which is the whole point. */
    n = build_input_state(buf, sizeof buf, sid, 5u, 110u);
    (void)apad_server_on_datagram(srv, 110u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 1,
          "unauthenticated INPUT_STATE still reaches the virtual pad");

    apad_server_destroy(srv);
}


static void scenario_pairing_ack_discharges(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t key[APAD_SESSION_KEY_LEN];
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint8_t payload[APAD_LEN_ACK];
    apad_ack a;
    uint16_t sid;
    int n, an, before;

    printf("\n-- a correctly-tagged ACK discharges the WELCOME (S9) --\n");
    srv = make_server_pairing(&hs);
    check(srv != NULL, "apad_server_create");
    random_script_reset();
    g_random.prng = 0x4242u;
    check(apad_server_begin_pairing(srv, 1000u, 0) == APAD_OK, "begin_pairing");

    memset(&peer, 0, sizeof peer);
    peer.ip[0] = 10u; peer.ip[3] = 20u; peer.port = 45000u;
    check(pair_and_hello(srv, &hs, peer, 1000u, 1, 0xF0u, key, &sid),
          "HELLO -> AUTH_REQUIRED WELCOME");

    memset(&a, 0, sizeof a);
    a.sequence = hs.sends[hs.count - 1].sequence;
    an = apad_encode_ack(payload, sizeof payload, &a);
    n = build_raw_auth(buf, sizeof buf, (uint8_t)APAD_MSG_ACK, sid, 1,
                       payload, (uint16_t)an, key);
    printf("    ACK is %d bytes, acking WELCOME seq %u\n", n, (unsigned)a.sequence);
    (void)apad_server_on_datagram(srv, 1010u, &peer, buf, (size_t)n);

    before = hs.count;
    (void)apad_server_tick(srv, 1200u);
    (void)apad_server_tick(srv, 1500u);
    check(hs.count == before,
          "no S9 retransmit after the ACK: the WELCOME was discharged");
    apad_server_destroy(srv);
}

/* ========================================================================
 * Server UI query API (apadserver.h): apad_server_list_clients(),
 * apad_server_backend_status(), apad_server_set_profile(), and the S6.6 RTT
 * this server now originates (apad_server_tick()'s own doc comment) driven
 * end to end through the PUBLIC library API rather than inferred from send
 * counts, the way every scenario above this one checks PING/PONG. This is
 * the "real work" the server UI task's brief specifically called out --
 * exercised here, not just by the live UI against a real socket, because a
 * fake clock lets it hit the exact 1000ms boundary deterministically.
 * ======================================================================== */

static int build_pong(uint8_t *buf, size_t cap, uint16_t session_id,
                      uint16_t sequence, uint32_t origin_ticks_ms) {
    apad_ping p;
    uint8_t payload[APAD_LEN_PONG];
    int n;

    memset(&p, 0, sizeof p);
    p.origin_ticks_ms    = origin_ticks_ms;   /* S6.6: echoed unchanged      */
    p.responder_ticks_ms = origin_ticks_ms;   /* whatever the peer's own
                                               * clock said; the server does
                                               * not read this field (S6.6) */
    n = apad_encode_ping(payload, sizeof payload, &p);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_PONG, session_id, sequence,
                     payload, (uint16_t)n);
}

static void scenario_ui_query_api(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM], buf[APAD_MAX_DATAGRAM];
    int hello_len, n;
    uint16_t sid = 0, wseq = 0;
    apad_client_info info[APAD_MAX_SESSIONS];
    apad_backend_status bstat;
    apad_input_state st;
    uint8_t payload[APAD_LEN_INPUT_STATE];

    printf("\n-- server UI query API: list_clients/backend_status/set_profile, "
           "S6.6 RTT origination+correlation --\n");
    srv = make_server(&hs);
    check(srv != NULL, "apad_server_create");
    if (srv == NULL) {
        return;
    }

    check(apad_server_list_clients(srv, info, APAD_MAX_SESSIONS) == 0,
          "list_clients returns 0 before any HELLO");

    apad_addr_set(&peer, 10, 1, 2, 3, 44000);
    check(establish_hello(srv, &hs, peer, 0u, 0xAAu, "harness-ui-query", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO at t=0 draws a WELCOME");
    n = build_ack(buf, sizeof buf, sid, 2, wseq);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);

    check(apad_server_list_clients(srv, info, APAD_MAX_SESSIONS) == 1,
          "list_clients returns exactly 1 after HELLO");
    check(info[0].slot == 0u && info[0].session_id == sid,
          "slot/session_id match what WELCOME carried");
    check(strcmp(info[0].device_name, "harness-ui-query") == 0,
          "device_name is what HELLO carried, decoded");
    check(info[0].peer.ip[0] == 10u && info[0].peer.ip[3] == 3u
          && info[0].peer.port == 44000u,
          "peer address matches the HELLO's source");
    check(info[0].profile_name[0] != '\0'
          && strcmp(info[0].profile_name, "builtin-default") == 0,
          "profile_name is the built-in default (make_server passes no "
          "profiles.jsonc blobs)");
    check(info[0].rtt_ms == APAD_RTT_UNKNOWN,
          "rtt_ms is APAD_RTT_UNKNOWN before this server has originated "
          "(and had answered) its first S6.6 PING");
    check(info[0].battery == (uint8_t)APAD_BATTERY_UNKNOWN,
          "battery is APAD_BATTERY_UNKNOWN before any INPUT_STATE");
    check(info[0].authenticated == 0,
          "authenticated is 0: no S10 pairing window was ever open");
    check(info[0].tx_packets >= 1u && info[0].rx_packets >= 1u,
          "packet counters are nonzero (WELCOME sent, HELLO+ACK received)");

    check(apad_server_backend_status(srv, &bstat) == APAD_OK
          && bstat.ok && strcmp(bstat.name, "recording-test-backend") == 0,
          "backend_status reports the linked-in backend's own name, ok "
          "(kRecordingBackend has no health() hook -- NULL means nothing "
          "to report)");

    check(apad_server_set_profile(srv, 0u, "no-such-profile") == APAD_ERR_ARG,
          "set_profile(unknown name) -> APAD_ERR_ARG");
    check(apad_server_set_profile(srv, 7u, "builtin-default") == APAD_ERR_STATE,
          "set_profile(slot with no session) -> APAD_ERR_STATE");
    check(apad_server_set_profile(srv, 0u, "builtin-default") == APAD_OK,
          "set_profile(valid name, connected slot) -> APAD_OK");

    /* An INPUT_STATE with a real battery value, so battery/rx_packets move. */
    memset(&st, 0, sizeof st);
    st.battery = 77u;
    n = apad_encode_input_state(payload, sizeof payload, &st);
    check(n >= 0, "encode INPUT_STATE");
    n = build_raw(buf, sizeof buf, (uint8_t)APAD_MSG_INPUT_STATE, sid, 3,
                 payload, (uint16_t)n);
    (void)apad_server_on_datagram(srv, 500u, &peer, buf, (size_t)n);
    (void)apad_server_list_clients(srv, info, APAD_MAX_SESSIONS);
    check(info[0].battery == 77u,
          "battery now reflects the most recent INPUT_STATE (S5.5)");

    /* Not yet 1000ms elapsed since session creation (t=0): no PING due yet. */
    (void)apad_server_tick(srv, 999u);
    check(hs.count == 1,
          "no S6.6 PING originated before 1000ms have elapsed");

    /* Exactly 1000ms: the server's own 1 Hz PING (apad_server_tick()'s doc
     * comment) is due. */
    (void)apad_server_tick(srv, 1000u);
    check(hs.count == 2, "S6.6 PING originated at exactly t=1000");
    if (hs.count == 2) {
        apad_packet pkt;
        apad_ping   in;
        memset(&pkt, 0, sizeof pkt);
        memset(&in, 0, sizeof in);
        check(hs.sends[1].type == (uint8_t)APAD_MSG_PING,
              "the datagram is a PING (S4 message table)");
        check(hs.sends[1].session_id == sid,
              "the PING carries this session's session_id");
        check(apad_packet_parse(hs.sends[1].buf, hs.sends[1].len, &pkt) >= 0
              && apad_decode_ping(pkt.payload, pkt.payload_len, &in) >= 0
              && in.origin_ticks_ms == 1000u && in.responder_ticks_ms == 0u,
              "origin_ticks_ms == the server's own now_ms, "
              "responder_ticks_ms == 0 (S6.6: 'MUST be zero on send' in a "
              "PING)");
    }
    (void)apad_server_list_clients(srv, info, APAD_MAX_SESSIONS);
    check(info[0].rtt_ms == APAD_RTT_UNKNOWN,
          "rtt_ms still APAD_RTT_UNKNOWN: the PING was sent, not yet answered");

    /* The "client" answers with a PONG, 42ms later on the server's clock. */
    n = build_pong(buf, sizeof buf, sid, 4, 1000u);
    check(n > 0, "build PONG echoing origin_ticks_ms=1000");
    (void)apad_server_on_datagram(srv, 1042u, &peer, buf, (size_t)n);
    (void)apad_server_list_clients(srv, info, APAD_MAX_SESSIONS);
    check(info[0].rtt_ms == 42u,
          "rtt_ms == now(1042) - origin_ticks_ms(1000) == 42, correlated "
          "purely by the echoed origin_ticks_ms (S6.6: 'Correlation is by "
          "origin_ticks_ms alone')");

    /* A stale/mismatched PONG (echoing an origin_ticks_ms that was never
     * sent) must not silently overwrite a good RTT sample with garbage. */
    n = build_pong(buf, sizeof buf, sid, 5, 999999u);
    (void)apad_server_on_datagram(srv, 2000u, &peer, buf, (size_t)n);
    (void)apad_server_list_clients(srv, info, APAD_MAX_SESSIONS);
    check(info[0].rtt_ms == 42u,
          "a PONG echoing an origin_ticks_ms that was never sent is "
          "ignored, not misattributed (rtt_ms unchanged)");

    apad_server_destroy(srv);
}

int main(void) {
    printf("== AtticPad server-harness: libapadserver via apadserver.h, no sockets, "
           "no real clock ==\n");

    scenario_discover_plain();
    scenario_discover_unknown_session();
    {
        static const uint8_t bcast[4] = {255u, 255u, 255u, 255u};
        static const uint8_t mcast[4] = {224u, 0u, 0u, 1u};
        scenario_discover_spoofed(bcast, "255.255.255.255");
        scenario_discover_spoofed(mcast, "224.0.0.1");
    }
    scenario_discover_subnet_broadcast();

    scenario_before_first_tick();

    scenario_idle_timeout(0, 2999u);
    scenario_idle_timeout(1, 3001u);

    scenario_retransmit(
        "no ACK ever: full S9 100/200/400/800ms schedule, then the session "
        "fails at t=2300",
        0, -1);
    scenario_retransmit(
        "FAILED-SEND: first WELCOME's on_send fails, then no ACK ever -- "
        "identical schedule/outcome, proving accounting does not depend on "
        "send() succeeding",
        1, -1);
    scenario_retransmit(
        "FAILED-SEND recovery: first WELCOME's on_send fails, but the "
        "client ACKs the retransmitted copy -- session recovers",
        1, 0);

    scenario_ack_immediately_disarms();
    scenario_duplicate_hello();

    /* §10 pairing. scenario_pairing_absent_is_unchanged() is the
     * backward-compatibility check and every scenario ABOVE this line is
     * one too: they all run with cfg.on_random == NULL, i.e. a host that
     * has never heard of pairing. */
    scenario_pairing_absent_is_unchanged();
    scenario_pairing_needs_entropy();
    scenario_pairing_appendix_a();
    scenario_pairing_window_expiry();
    scenario_pairing_expiry_drops_unproven();
    scenario_pairing_lockout();
    scenario_pairing_one_charge_per_session();
    scenario_pairing_untagged_is_dropped();
    scenario_pairing_token();
    scenario_pairing_ack_discharges();

    scenario_ui_query_api();

    printf("\n%d failure(s)\n", g_failures);
    return g_failures != 0;
}
