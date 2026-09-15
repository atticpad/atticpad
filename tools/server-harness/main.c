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

/*
 * §6.15-§6.19 additions (docs/DESIGN.md follow-on to §6.1): backend.h's five KBM
 * hooks (kbm_caps, create_kbm, kbm_events, mouse_motion, destroy_kbm),
 * recorded the same way update_pad/destroy_pad already are above, plus a
 * SETTABLE kbm_caps_value so a scenario can present a KBM-incapable backend
 * (the default, 0 -- unchanged behaviour for every scenario above this
 * addition) or one that advertises KEYBOARD/MOUSE/MEDIA. server.c reads
 * kbm_caps() exactly ONCE, at apad_server_create() time (§6.19: "computed
 * once here, never per session") -- so a scenario that wants KBM support
 * MUST set g_backend.kbm_caps_value BEFORE calling apad_server_create(), via
 * make_server_kbm()/make_server_kbm_pairing() below, not after.
 *
 * `next_seq` is one monotonic counter shared across EVERY backend call this
 * harness records (pad and KBM alike), so a scenario can assert ORDERING
 * between two different hook calls -- §6.20's "release everything held...
 * BEFORE it destroys or detaches whatever it injects into" is an ordering
 * claim, not a presence claim, and a call count alone cannot distinguish
 * "released then destroyed" from "destroyed then released".
 */
#define MAX_KBM_CREATE   16
#define MAX_KBM_DESTROY  16
#define MAX_KBM_BATCHES  32   /* one entry per kbm_events() CALL, not per event */
#define MAX_KBM_EVENTS_PER_BATCH 32
#define MAX_MOUSE_MOTION 32

typedef struct {
    int seq;
    int slot;
    apad_kbm_device dev;
} kbm_dev_call;

typedef struct {
    int seq;
    int slot;
    size_t n;
    apad_kbm_event_out ev[MAX_KBM_EVENTS_PER_BATCH];
} kbm_events_call;

typedef struct {
    int seq;
    int slot;
    apad_mouse_motion m;
} mouse_motion_call;

typedef struct {
    int init_calls;
    int create_calls;
    int update_calls;
    int poll_calls;
    int destroy_calls;
    int shutdown_calls;
    int last_slot;
    apad_pad_state last_state;

    int next_seq;   /* shared ordering counter, every hook below included */

    uint32_t kbm_caps_value;   /* what kbm_caps() returns; set BEFORE create */
    int      kbm_caps_calls;

    kbm_dev_call kbm_creates[MAX_KBM_CREATE];
    int          kbm_create_count;
    int          kbm_create_fail;  /* 1: create_kbm() always fails (rc != 0) */

    kbm_events_call kbm_batches[MAX_KBM_BATCHES];
    int             kbm_batch_count;

    mouse_motion_call mouse_motions[MAX_MOUSE_MOTION];
    int               mouse_motion_count;

    kbm_dev_call kbm_destroys[MAX_KBM_DESTROY];
    int          kbm_destroy_count;
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

/* ---- §6.15-§6.19 KBM hooks, backend.h ---------------------------------- */

static uint32_t rec_kbm_caps(void) {
    g_backend.kbm_caps_calls++;
    return g_backend.kbm_caps_value;
}
static int rec_create_kbm(int slot, apad_kbm_device dev) {
    if (g_backend.kbm_create_count < MAX_KBM_CREATE) {
        kbm_dev_call *c = &g_backend.kbm_creates[g_backend.kbm_create_count++];
        c->seq  = g_backend.next_seq++;
        c->slot = slot;
        c->dev  = dev;
    }
    return g_backend.kbm_create_fail ? -1 : 0;
}
static int rec_kbm_events(int slot, const apad_kbm_event_out *ev, size_t n) {
    if (g_backend.kbm_batch_count < MAX_KBM_BATCHES) {
        kbm_events_call *c = &g_backend.kbm_batches[g_backend.kbm_batch_count++];
        c->seq  = g_backend.next_seq++;
        c->slot = slot;
        c->n    = (n > MAX_KBM_EVENTS_PER_BATCH) ? MAX_KBM_EVENTS_PER_BATCH : n;
        memcpy(c->ev, ev, c->n * sizeof *ev);
    }
    return 0;
}
static int rec_mouse_motion(int slot, const apad_mouse_motion *m) {
    if (g_backend.mouse_motion_count < MAX_MOUSE_MOTION) {
        mouse_motion_call *c = &g_backend.mouse_motions[g_backend.mouse_motion_count++];
        c->seq  = g_backend.next_seq++;
        c->slot = slot;
        c->m    = *m;
    }
    return 0;
}
static void rec_destroy_kbm(int slot, apad_kbm_device dev) {
    if (g_backend.kbm_destroy_count < MAX_KBM_DESTROY) {
        kbm_dev_call *c = &g_backend.kbm_destroys[g_backend.kbm_destroy_count++];
        c->seq  = g_backend.next_seq++;
        c->slot = slot;
        c->dev  = dev;
    }
}

static const apad_backend kRecordingBackend = {
    .init          = rec_init,
    .create_pad    = rec_create_pad,
    .update_pad    = rec_update_pad,
    .poll_feedback = rec_poll_feedback,
    .destroy_pad   = rec_destroy_pad,
    .shutdown      = rec_shutdown,
    .name          = "recording-test-backend",
    /* .health left NULL (backend.h: optional) -- this harness never
     * exercises apad_server_backend_status()'s "backend reports a problem"
     * path; server-dev agent memory server-ui-* covers that live against
     * the real uinput backend instead. */

    /* §6.15-§6.19: present (unlike a backend that predates the feature,
     * which leaves these five NULL), but kbm_caps_value defaults to 0 in a
     * zeroed backend_state -- every scenario ABOVE this addition runs with
     * caps 0, byte-identical to "no KBM support at all" (backend.h: "NULL
     * means 0 (nothing) without being called"), so nothing pre-existing
     * changes behaviour by this struct gaining these five pointers. */
    .kbm_caps      = rec_kbm_caps,
    .create_kbm    = rec_create_kbm,
    .kbm_events    = rec_kbm_events,
    .mouse_motion  = rec_mouse_motion,
    .destroy_kbm   = rec_destroy_kbm
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

#define MAX_LOG_LINES 64

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

    /* Captured on_log lines, in addition to the printf() every scenario
     * above already got -- added for the §6.20 event-ring overflow
     * scenario, which needs to check whether ANY diagnostic channel
     * observed a lost-event condition (see scenario_key_ring_overflow()).
     * fake_log() already receives cfg.user == this pointer; it simply was
     * not using it before. */
    char log_lines[MAX_LOG_LINES][200];
    int  log_count;
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
    harness_state *hs = (harness_state *)user;
    const char *lvl;
    switch (level) {
    case APAD_LOG_ERROR: lvl = "ERROR"; break;
    case APAD_LOG_WARN:  lvl = "WARN";  break;
    default:              lvl = "INFO";  break;
    }
    printf("    [server %s] %s\n", lvl, msg);
    if (hs != NULL && hs->log_count < MAX_LOG_LINES) {
        snprintf(hs->log_lines[hs->log_count], sizeof hs->log_lines[0],
                 "%s: %s", lvl, msg);
        hs->log_count++;
    }
}

/* True if any captured on_log line contains `needle` (case-sensitive, plain
 * substring). Used only by the §6.20 event-ring overflow scenario to check
 * whether ANY diagnostic channel observed a lost-event condition. */
static int any_log_contains(const harness_state *hs, const char *needle) {
    int i;
    for (i = 0; i < hs->log_count; i++) {
        if (strstr(hs->log_lines[i], needle) != NULL) {
            return 1;
        }
    }
    return 0;
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

/* Like make_server(), but with g_backend.kbm_caps_value set to `caps`
 * (APAD_KBM_CAP_* bits, backend.h) BEFORE apad_server_create() runs.
 * server.c reads kbm_caps() exactly once, at create time (§6.19: "computed
 * once here, never per session") -- setting it after creation would have no
 * effect on that server instance's s->kbm_features at all. */
static apad_server *make_server_kbm(harness_state *hs, uint32_t caps) {
    apad_server_cfg cfg;

    memset(hs, 0, sizeof *hs);
    hs->fail_call_index = -1;
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.kbm_caps_value = caps;

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

/* ---- §6.15-§6.19 datagram builders -------------------------------------- */

static int build_keyboard(uint8_t *buf, size_t cap, uint16_t session_id,
                          uint16_t sequence, const apad_keyboard *kb) {
    uint8_t payload[APAD_LEN_KEYBOARD];
    int n = apad_encode_keyboard(payload, sizeof payload, kb);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_KEYBOARD, session_id, sequence,
                     payload, (uint16_t)n);
}

static int build_mouse(uint8_t *buf, size_t cap, uint16_t session_id,
                       uint16_t sequence, const apad_mouse *mo) {
    uint8_t payload[APAD_LEN_MOUSE];
    int n = apad_encode_mouse(payload, sizeof payload, mo);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_MOUSE, session_id, sequence,
                     payload, (uint16_t)n);
}

static int build_media(uint8_t *buf, size_t cap, uint16_t session_id,
                       uint16_t sequence, const apad_media *me) {
    uint8_t payload[APAD_LEN_MEDIA];
    int n = apad_encode_media(payload, sizeof payload, me);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_MEDIA, session_id, sequence,
                     payload, (uint16_t)n);
}

static int build_bye(uint8_t *buf, size_t cap, uint16_t session_id,
                     uint16_t sequence, uint8_t reason) {
    apad_bye bye;
    uint8_t payload[APAD_LEN_BYE];
    int n;

    memset(&bye, 0, sizeof bye);
    bye.reason = reason;
    n = apad_encode_bye(payload, sizeof payload, &bye);
    if (n < 0) {
        return n;
    }
    return build_raw(buf, cap, (uint8_t)APAD_MSG_BYE, session_id, sequence,
                     payload, (uint16_t)n);
}

/* Decode the payload of a recorded INPUTCAPS send. Returns 1 on success. */
static int inputcaps_of(const recorded_send *r, apad_inputcaps *out) {
    apad_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    if (apad_packet_parse(r->buf, r->len, &pkt) < 0
        || pkt.header.type != (uint8_t)APAD_MSG_INPUTCAPS) {
        return 0;
    }
    return apad_decode_inputcaps(pkt.payload, pkt.payload_len, out) >= 0;
}

/* Set bit for HID usage `u` in a 32-byte §6.15 keys[] bitmap. */
static void key_set(uint8_t keys[APAD_KEY_BITMAP_BYTES], uint8_t usage) {
    keys[APAD_KEY_BYTE(usage)] |= APAD_KEY_MASK(usage);
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

/* establish_hello() + the ACK that discharges WELCOME, in one call -- every
 * §6.15-§6.20 scenario below needs a fully-open, unauthenticated session
 * before it can send its first KEYBOARD/MOUSE/MEDIA datagram, and this is
 * the same three-step dance scenario_idle_timeout()/scenario_ui_query_api()
 * above already repeat inline. `ack_seq` is the client's own header sequence
 * for the ACK (distinct from hello_seq -- §9 spends a whole session's
 * sequence counter, so reusing hello_seq here would be a datagram this
 * harness would never actually construct as a real client). Returns 1 on
 * success and fills *session_id_out. */
static int establish_and_ack(apad_server *srv, harness_state *hs, apad_addr peer,
                             uint32_t t0, uint8_t seed, const char *device_name,
                             uint16_t hello_seq, uint16_t ack_seq,
                             uint16_t *session_id_out) {
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    uint8_t buf[APAD_MAX_DATAGRAM];
    int hello_len, n;
    uint16_t sid = 0, wseq = 0;

    if (!establish_hello(srv, hs, peer, t0, seed, device_name, hello_seq,
                         hello_buf, &hello_len, &sid, &wseq)) {
        return 0;
    }
    n = build_ack(buf, sizeof buf, sid, ack_seq, wseq);
    if (n < 0) {
        return 0;
    }
    (void)apad_server_on_datagram(srv, t0, &peer, buf, (size_t)n);
    *session_id_out = sid;
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

/* make_server_pairing() + make_server_kbm(): both cfg.on_random and a
 * nonzero kbm_caps_value, for scenario_inputcaps_auth_waits_for_verified_tag
 * -- the one §6.19 Delivery rule that needs BOTH a pairing window and a
 * backend that has something to advertise. */
static apad_server *make_server_kbm_pairing(harness_state *hs, uint32_t caps) {
    apad_server_cfg cfg;

    memset(hs, 0, sizeof *hs);
    hs->fail_call_index = -1;
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.kbm_caps_value = caps;

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
/* ========================================================================
 * §6.15-§6.20 KEYBOARD / MOUSE / MEDIA / INPUTCAPS scenarios.
 *
 * docs/PROTOCOL.md §15 item 10: "§6.20's 10 Hz repeat floor and 1000 ms
 * watchdog are measured against a clock, and core has none by design...
 * unpinned by conformance data and needs a server-side test with a
 * controllable clock." This harness has one. The clock-dependent scenarios
 * (kbm_watchdog_*, kbm_teardown_releases_before_destroy) are the reason this
 * whole section exists; everything else pins what core's own vectors.h
 * cannot reach because it requires driving server.c's dispatch, lazy device
 * creation and §6.19 delivery schedule, not just codec.c's encode/decode.
 *
 * Every scenario below uses make_server_kbm()/make_server_kbm_pairing() (not
 * plain make_server()) because server.c reads backend->kbm_caps() exactly
 * once, at apad_server_create() time (§6.19: "computed once here, never per
 * session") -- s->kbm_features cannot be changed after the fact.
 * ======================================================================== */

/* Scan the recorded sends for the Nth (0-based) datagram of `type`, or NULL. */
static const recorded_send *nth_send_of_type(const harness_state *hs, uint8_t type, int n) {
    int i, seen = 0;
    for (i = 0; i < hs->count; i++) {
        if (hs->sends[i].type == type) {
            if (seen == n) {
                return &hs->sends[i];
            }
            seen++;
        }
    }
    return NULL;
}

static int count_sends_of_type(const harness_state *hs, uint8_t type) {
    int i, n = 0;
    for (i = 0; i < hs->count; i++) {
        if (hs->sends[i].type == type) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------
 * Dispatch and lifecycle
 * ------------------------------------------------------------------------ */

static void scenario_kbm_lazy_create(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    apad_mouse mo;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- kbm_lazy_create: first 0x21 creates KEYBOARD once; 0x22 creates "
           "MOUSE, not KEYBOARD again --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE
                                | APAD_KBM_CAP_MEDIA);
    check(srv != NULL, "apad_server_create with KEYBOARD|MOUSE|MEDIA caps");
    apad_addr_set(&peer, 10, 2, 0, 1, 41100);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x50u, "harness-lazy", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    kb.event_seq = 0u;
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    check(n > 0, "build first KEYBOARD");
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1
              && g_backend.kbm_creates[0].dev == APAD_KBM_DEV_KEYBOARD,
          "exactly one create_kbm(KEYBOARD) after the first 0x21");

    n = build_keyboard(buf, sizeof buf, sid, 4, &kb);
    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1,
          "a second 0x21 draws NO further create_kbm (already created)");

    memset(&mo, 0, sizeof mo);
    n = build_mouse(buf, sizeof buf, sid, 5, &mo);
    check(n > 0, "build MOUSE");
    (void)apad_server_on_datagram(srv, 30u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 2
              && g_backend.kbm_creates[1].dev == APAD_KBM_DEV_MOUSE,
          "a 0x22 creates MOUSE, and only MOUSE (not KEYBOARD again)");

    apad_server_destroy(srv);
}

static void scenario_kbm_not_advertised(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    apad_client_info info[APAD_MAX_SESSIONS];
    int n, before;

    printf("\n-- kbm_not_advertised: kbm_caps()==0 -> datagram dropped, zero "
           "create_kbm, no ERROR (S8's amplifier argument, applied to KBM) --\n");
    srv = make_server_kbm(&hs, 0u);
    check(srv != NULL, "apad_server_create with caps=0 (kbm_caps() hook present, "
                       "returns 0 -- distinct from a NULL hook, same net effect)");
    apad_addr_set(&peer, 10, 2, 0, 2, 41101);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x51u, "harness-noadv", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    check(n > 0, "build KEYBOARD");
    before = hs.count;
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 0,
          "zero create_kbm calls: this backend has nothing to advertise");
    check(hs.count == before,
          "zero datagrams sent in reply -- specifically no ERROR (S8's "
          "reflection-amplifier argument for INPUT_STATE applies verbatim: "
          "'§8 already forbids' this for unknown-session INPUT_STATE, and "
          "handle_keyboard()'s own comment extends the same reasoning here)");
    check(apad_server_list_clients(srv, info, APAD_MAX_SESSIONS) == 1,
          "the session itself is untouched by the drop");

    apad_server_destroy(srv);
}

static void scenario_kbm_stale_dropped(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    apad_client_info info[APAD_MAX_SESSIONS];
    int n, batches_before;

    printf("\n-- kbm_stale_dropped: seq 100 then 99 -> no backend call, session "
           "stays ALIVE (S8 liveness vs S9/S6.20 freshness are different "
           "questions) --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 0, 3, 41102);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x52u, "harness-stale", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    n = build_keyboard(buf, sizeof buf, sid, 100, &kb);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1, "seq 100 accepted: device created");
    batches_before = g_backend.kbm_batch_count;

    memset(&kb, 0, sizeof kb);
    kb.event_seq = 5u;
    key_set(kb.keys, APAD_HID_KEY_B);
    n = build_keyboard(buf, sizeof buf, sid, 99, &kb);
    check(n > 0, "build stale KEYBOARD (seq 99, older than 100)");
    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1,
          "seq 99 does not even reach the create-device check again");
    check(g_backend.kbm_batch_count == batches_before,
          "...and kbm.c is never called for it -- discarded at S6.20's "
          "per-type window, exactly like a stale INPUT_STATE at S9");
    check(apad_server_list_clients(srv, info, APAD_MAX_SESSIONS) == 1,
          "the session is NOT torn down by a stale KEYBOARD");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_C);
    n = build_keyboard(buf, sizeof buf, sid, 101, &kb);
    (void)apad_server_on_datagram(srv, 30u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count > batches_before,
          "...and a genuinely newer KEYBOARD (seq 101) is still accepted "
          "afterward -- the session is not merely alive, it is functional");

    apad_server_destroy(srv);
}

static void scenario_kbm_class_windows(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- kbm_class_windows, part A (literal case): INPUT_STATE seq 100 / "
           "KEYBOARD seq 101 / INPUT_STATE seq 102, in that ARRIVAL order -- "
           "all three applied --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 0, 4, 41103);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x53u, "harness-windows", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    n = build_input_state(buf, sizeof buf, sid, 100, 10u);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 1, "INPUT_STATE seq 100 applied");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    n = build_keyboard(buf, sizeof buf, sid, 101, &kb);
    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1, "KEYBOARD seq 101 applied (device created)");

    n = build_input_state(buf, sizeof buf, sid, 102, 30u);
    (void)apad_server_on_datagram(srv, 30u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 2, "INPUT_STATE seq 102 applied");
    apad_server_destroy(srv);

    /* Part B: the actual adversarial case §6.20 names in its own prose ("a
     * KEYBOARD at sequence 101 would shadow an INPUT_STATE still in flight
     * at 100" -- a REORDERED arrival, not the in-order case above). Part A's
     * monotonically increasing sequence numbers pass under a shared window
     * exactly as easily as under per-type ones, so it cannot by itself
     * distinguish the two; this part can and does. Added beyond the task's
     * literal list because the literal case does not exercise the failure
     * mode §6.20 describes -- documented here rather than silently. */
    printf("\n-- kbm_class_windows, part B (the adversarial case §6.20 itself "
           "names): KEYBOARD seq 101 arrives FIRST, then INPUT_STATE seq 100 "
           "-- INPUT_STATE must NOT be shadowed as stale --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    check(establish_and_ack(srv, &hs, peer, 0u, 0x54u, "harness-windows-b", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    n = build_keyboard(buf, sizeof buf, sid, 101, &kb);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1, "KEYBOARD seq 101 applied first");

    n = build_input_state(buf, sizeof buf, sid, 100, 20u);
    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.update_calls == 1,
          "INPUT_STATE seq 100, arriving AFTER a KEYBOARD at seq 101, is "
          "still applied -- a single SHARED window would have discarded it "
          "as stale (100 < 101); the independent per-type windows do not "
          "(S6.20: 'Per-type windows, and nothing shared')");
    apad_server_destroy(srv);
}

static void scenario_kbm_wrong_peer_address(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer, spoofed;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    apad_client_info info[APAD_MAX_SESSIONS];
    int n;

    printf("\n-- kbm_wrong_peer_address: a datagram for a live session from a "
           "different address is ignored --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 0, 5, 41104);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x55u, "harness-wrongpeer", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    apad_addr_set(&spoofed, 10, 2, 0, 99, 41199);
    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    check(n > 0, "build KEYBOARD, valid session_id, wrong source address");
    (void)apad_server_on_datagram(srv, 10u, &spoofed, buf, (size_t)n);
    check(g_backend.kbm_create_count == 0, "no create_kbm from the spoofed source");
    check(g_backend.kbm_batch_count == 0, "no kbm.c call at all");
    check(apad_server_list_clients(srv, info, APAD_MAX_SESSIONS) == 1,
          "the real session is untouched, not torn down");

    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1,
          "the SAME bytes from the REAL peer are accepted -- proves the "
          "rejection above was about the address, not the payload");

    apad_server_destroy(srv);
}

/* ------------------------------------------------------------------------
 * The §6.20 receiver algorithm: watchdog, teardown release ordering
 * ------------------------------------------------------------------------ */

static void scenario_kbm_watchdog_releases(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    apad_mouse mo;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n, batches_at_wd_start;

    printf("\n-- kbm_watchdog_releases: keys held, then 1000ms of silence -> "
           "release-all for THAT facility, and ONLY that facility --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 1, 1, 41110);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x60u, "harness-wd-releases", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK, t=0");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    kb.event_seq = 1u;
    kb.events[7].usage = APAD_HID_KEY_A;
    kb.events[7].flags = APAD_KBM_EVENT_DOWN;
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 1 && g_backend.kbm_batch_count == 1,
          "KEYBOARD holding A at t=0: device created, one press batch");

    memset(&mo, 0, sizeof mo);
    mo.buttons = APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT);
    mo.event_seq = 1u;
    mo.events[3].button = (uint8_t)APAD_MOUSEBTN_LEFT;
    mo.events[3].flags = APAD_KBM_EVENT_DOWN;
    n = build_mouse(buf, sizeof buf, sid, 4, &mo);
    (void)apad_server_on_datagram(srv, 500u, &peer, buf, (size_t)n);
    check(g_backend.kbm_create_count == 2,
          "MOUSE holding LEFT at t=500: device created too");
    batches_at_wd_start = g_backend.kbm_batch_count;

    (void)apad_server_tick(srv, 999u);
    check(g_backend.kbm_batch_count == batches_at_wd_start,
          "t=999: neither facility's OWN 1000ms has elapsed yet (KEYBOARD's "
          "last accept was t=0, MOUSE's was t=500) -- no release");

    (void)apad_server_tick(srv, 1000u);
    check(g_backend.kbm_batch_count == batches_at_wd_start + 1,
          "t=1000: KEYBOARD's watchdog fires (1000-0=1000ms) -- exactly ONE "
          "new batch");
    if (g_backend.kbm_batch_count == batches_at_wd_start + 1) {
        const kbm_events_call *rel = &g_backend.kbm_batches[g_backend.kbm_batch_count - 1];
        check(rel->n == 1 && rel->ev[0].device == (uint8_t)APAD_KBM_DEV_KEYBOARD
                  && rel->ev[0].code == APAD_HID_KEY_A && rel->ev[0].down == 0u,
              "...and it is A released (down=0), not a MOUSE event -- ONLY "
              "the keyboard facility, not mouse (S6.20: 'a receiver that "
              "believes something is held for A facility... release "
              "everything it holds for THAT facility')");
    }
    check(g_backend.kbm_destroy_count == 0,
          "watchdog releases; it does not destroy the device");

    (void)apad_server_tick(srv, 1500u);
    check(g_backend.kbm_batch_count == batches_at_wd_start + 2,
          "t=1500: MOUSE's watchdog now fires too (1500-500=1000ms)");
    if (g_backend.kbm_batch_count == batches_at_wd_start + 2) {
        const kbm_events_call *rel = &g_backend.kbm_batches[g_backend.kbm_batch_count - 1];
        check(rel->n == 1 && rel->ev[0].device == (uint8_t)APAD_KBM_DEV_MOUSE
                  && rel->ev[0].code == (uint16_t)APAD_MOUSEBTN_LEFT
                  && rel->ev[0].down == 0u,
              "...and it is LEFT released, on its own independent 1000ms clock");
    }

    apad_server_destroy(srv);
}

static void scenario_kbm_watchdog_not_early(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    uint16_t seq;
    uint32_t t;
    int n;

    printf("\n-- kbm_watchdog_not_early: held key refreshed at the 10Hz floor "
           "-> watchdog never fires, across several seconds --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 1, 2, 41111);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x61u, "harness-wd-notearly", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK, t=0");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    kb.event_seq = 1u;
    kb.events[7].usage = APAD_HID_KEY_A;
    kb.events[7].flags = APAD_KBM_EVENT_DOWN;
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 1, "t=0: initial press, one batch");

    /* Same snapshot (A still held, event_seq unchanged -- no NEW events),
     * refreshed exactly at the S6.20 MUST floor: every 100ms (10Hz), from
     * t=100 through t=4900 -- comfortably past several 1000ms watchdog
     * boundaries. Header sequence still advances every send (S9: every
     * datagram consumes the direction's sequence counter), event_seq does
     * not (nothing NEW happened -- the key is still simply held). */
    seq = 4;
    for (t = 100u; t <= 4900u; t += 100u) {
        (void)apad_server_tick(srv, t - 1u);
        n = build_keyboard(buf, sizeof buf, sid, seq++, &kb);
        (void)apad_server_on_datagram(srv, t, &peer, buf, (size_t)n);
    }
    (void)apad_server_tick(srv, 4999u);   /* just under last_accept(4900)+1000 */

    check(g_backend.kbm_batch_count == 1,
          "still exactly ONE batch (the original press) after ~5s of a "
          "conforming 10Hz repeat -- the watchdog never fires for a sender "
          "that meets S6.20's MUST floor");
    check(g_backend.kbm_destroy_count == 0, "...and nothing was ever destroyed");

    apad_server_destroy(srv);
}

static void scenario_kbm_watchdog_boundary(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- kbm_watchdog_boundary: release at exactly 1000ms, not 900, "
           "not 1500 --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 1, 3, 41112);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x62u, "harness-wd-boundary", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK, t=0");

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    kb.event_seq = 1u;
    kb.events[7].usage = APAD_HID_KEY_A;
    kb.events[7].flags = APAD_KBM_EVENT_DOWN;
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 1, "t=0: press, one batch");

    (void)apad_server_tick(srv, 900u);
    check(g_backend.kbm_batch_count == 1, "t=900: not yet (900 < 1000)");

    (void)apad_server_tick(srv, 999u);
    check(g_backend.kbm_batch_count == 1, "t=999: not yet (999 < 1000)");

    (void)apad_server_tick(srv, 1000u);
    check(g_backend.kbm_batch_count == 2,
          "t=1000: release fires exactly here (S6.20: 'accepted no datagram "
          "... for 1000ms MUST release', apad_time_since(now, last) >= 1000)");
    if (g_backend.kbm_batch_count == 2) {
        const kbm_events_call *rel = &g_backend.kbm_batches[1];
        check(rel->n == 1 && rel->ev[0].down == 0u && rel->ev[0].code == APAD_HID_KEY_A,
              "...A released");
    }

    (void)apad_server_tick(srv, 1500u);
    check(g_backend.kbm_batch_count == 2,
          "t=1500: no SECOND release -- already-empty shadow makes the "
          "repeat watchdog check idempotent, not a duplicate release");

    apad_server_destroy(srv);
}

/* Finds the seq of the first kbm_batches[] entry that released `dev`/`code`
 * (down==0), or -1. Used to build the release-before-destroy ordering
 * assertion below without caring WHICH code path (watchdog vs teardown)
 * emitted the release -- see the scenario's own comment for why that
 * distinction is structurally unavoidable for the idle-timeout sub-case. */
static int first_release_seq(apad_kbm_device dev, uint16_t code) {
    int i, j;
    for (i = 0; i < g_backend.kbm_batch_count; i++) {
        const kbm_events_call *c = &g_backend.kbm_batches[i];
        for (j = 0; j < (int)c->n; j++) {
            if (c->ev[j].device == (uint8_t)dev && c->ev[j].code == code
                && c->ev[j].down == 0u) {
                return c->seq;
            }
        }
    }
    return -1;
}

static int destroy_seq_of(apad_kbm_device dev) {
    int i;
    for (i = 0; i < g_backend.kbm_destroy_count; i++) {
        if (g_backend.kbm_destroys[i].dev == dev) {
            return g_backend.kbm_destroys[i].seq;
        }
    }
    return -1;
}

/* Establishes a session with all three facilities created and holding
 * something, for the teardown-ordering scenario below. Shared by both the
 * BYE and idle-timeout sub-cases. */
static int setup_all_three_held(apad_server *srv, harness_state *hs, apad_addr peer,
                                 uint16_t *sid_out) {
    apad_keyboard kb;
    apad_mouse mo;
    apad_media me;
    uint8_t buf[APAD_MAX_DATAGRAM];
    int n;

    if (!establish_and_ack(srv, hs, peer, 0u, 0x63u, "harness-teardown", 1, 2, sid_out)) {
        return 0;
    }

    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    kb.event_seq = 1u;
    kb.events[7].usage = APAD_HID_KEY_A;
    kb.events[7].flags = APAD_KBM_EVENT_DOWN;
    n = build_keyboard(buf, sizeof buf, *sid_out, 3, &kb);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);

    memset(&mo, 0, sizeof mo);
    mo.buttons = APAD_MOUSEBTN_BIT(APAD_MOUSEBTN_LEFT);
    mo.event_seq = 1u;
    mo.events[3].button = (uint8_t)APAD_MOUSEBTN_LEFT;
    mo.events[3].flags = APAD_KBM_EVENT_DOWN;
    n = build_mouse(buf, sizeof buf, *sid_out, 4, &mo);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);

    memset(&me, 0, sizeof me);
    me.held = APAD_MEDIA_BIT(APAD_MEDIA_PLAY_PAUSE);
    me.event_seq = 1u;
    me.events[3].control = (uint8_t)APAD_MEDIA_PLAY_PAUSE;
    me.events[3].flags = APAD_KBM_EVENT_DOWN;
    n = build_media(buf, sizeof buf, *sid_out, 5, &me);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);

    return g_backend.kbm_create_count == 3;
}

static void assert_release_before_destroy_all_three(const char *why) {
    static const struct { apad_kbm_device dev; uint16_t code; const char *name; } facs[3] = {
        { APAD_KBM_DEV_KEYBOARD, APAD_HID_KEY_A,               "KEYBOARD" },
        { APAD_KBM_DEV_MOUSE,    (uint16_t)APAD_MOUSEBTN_LEFT, "MOUSE"    },
        { APAD_KBM_DEV_MEDIA,    (uint16_t)APAD_MEDIA_PLAY_PAUSE, "MEDIA" }
    };
    int i;
    char what[256];

    check(g_backend.kbm_destroy_count == 3, "all three facilities destroyed");
    for (i = 0; i < 3; i++) {
        int rel_seq = first_release_seq(facs[i].dev, facs[i].code);
        int dst_seq = destroy_seq_of(facs[i].dev);
        snprintf(what, sizeof what,
                 "%s: a release (down=0) was observed before its destroy_kbm "
                 "call (%s) -- S6.20: 'before it destroys or detaches "
                 "whatever it injects into'", facs[i].name, why);
        check(rel_seq >= 0 && dst_seq >= 0 && rel_seq < dst_seq, what);
    }
}

static void scenario_kbm_teardown_releases_before_destroy(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- kbm_teardown_releases_before_destroy, BYE at t=100 (well "
           "under the 1000ms watchdog, so the release below is unambiguously "
           "teardown's own, not a watchdog pre-emption) --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE
                                | APAD_KBM_CAP_MEDIA);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 1, 4, 41113);
    check(setup_all_three_held(srv, &hs, peer, &sid),
          "all three facilities created and holding something, t=0");

    n = build_bye(buf, sizeof buf, sid, 6, (uint8_t)APAD_BYE_NORMAL);
    check(n > 0, "build BYE");
    (void)apad_server_on_datagram(srv, 100u, &peer, buf, (size_t)n);
    assert_release_before_destroy_all_three("BYE");
    apad_server_destroy(srv);

    /* Idle timeout. NOTE, stated up front rather than left implicit: S6.20's
     * 1000ms watchdog is always <= S11's 3000ms idle timeout when both are
     * measured from the same last-received instant with nothing else
     * arriving in between (1000 < 3000, unconditionally) -- so the release
     * this sub-case observes will, in this exact setup, already have been
     * emitted by the WATCHDOG at t=1000, not by free_session()'s own
     * apad_kbm_release_all() call at teardown (which is then a correct,
     * idempotent no-op: nothing is left held). Both satisfy S6.20's ordering
     * requirement identically -- by the time destroy_kbm runs, a release
     * with a lower seq has been recorded either way -- and the assertion
     * checks that ORDERING property, not which code path produced it. */
    printf("\n-- kbm_teardown_releases_before_destroy, idle timeout at "
           "t=3000 (S11) -- same ordering property, see the code comment on "
           "why THIS release may be watchdog-sourced rather than "
           "teardown-sourced --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE
                                | APAD_KBM_CAP_MEDIA);
    check(srv != NULL, "apad_server_create");
    check(setup_all_three_held(srv, &hs, peer, &sid),
          "all three facilities created and holding something, t=0");

    (void)apad_server_tick(srv, 3000u);
    assert_release_before_destroy_all_three("idle timeout");
    apad_server_destroy(srv);
}

/* ------------------------------------------------------------------------
 * INPUTCAPS delivery (§6.19), including the auth trap
 * ------------------------------------------------------------------------ */

static void scenario_inputcaps_after_ack(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint16_t sid = 0;
    uint32_t t0 = 1000u, t, last_seen_at[8];
    int seen_before, seen_after, i, n_new;

    printf("\n-- inputcaps_after_ack: exactly 3 copies ~250ms apart after the "
           "WELCOME-discharging ACK, 16 bytes, byte-exact, RELIABLE clear --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD | APAD_KBM_CAP_MOUSE
                                | APAD_KBM_CAP_MEDIA);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 2, 1, 41120);
    check(establish_and_ack(srv, &hs, peer, t0, 0x70u, "harness-ic-ack", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    seen_before = count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS);
    check(seen_before == 0, "no INPUTCAPS sent by the ACK itself (only marked dirty)");

    n_new = 0;
    for (t = t0 + 1u; t <= t0 + 900u && n_new < 8; t++) {
        int before = count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS);
        (void)apad_server_tick(srv, t);
        if (count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) > before) {
            last_seen_at[n_new++] = t;
        }
    }
    seen_after = count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS);
    check(seen_after == 3, "exactly 3 copies land within the 900ms burst window");

    if (seen_after == 3) {
        for (i = 0; i < 3; i++) {
            const recorded_send *r = nth_send_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS, i);
            apad_packet pkt;
            apad_inputcaps ic;
            char what[128];

            check(r != NULL && r->len == (size_t)(APAD_HEADER_SIZE + APAD_LEN_INPUTCAPS),
                  "copy is header + 16 bytes, byte-exact fixed size (S6.19: "
                  "'a fixed 16 bytes whatever features says')");
            memset(&pkt, 0, sizeof pkt);
            check(r != NULL && apad_packet_parse(r->buf, r->len, &pkt) >= 0,
                  "parses");
            check((pkt.header.flags & APAD_FLAG_RELIABLE) == 0u,
                  "RELIABLE flag clear (S4: INPUTCAPS reliable=no)");
            check(inputcaps_of(r, &ic)
                      && ic.features == (APAD_KBM_FEATURE_KEYBOARD
                                        | APAD_KBM_FEATURE_MOUSE
                                        | APAD_KBM_FEATURE_MEDIA),
                  "features matches this backend's kbm_caps()");
            if (i > 0) {
                uint32_t delta = last_seen_at[i] - last_seen_at[i - 1];
                snprintf(what, sizeof what,
                         "copy %d..%d is ~250ms after the previous one "
                         "(observed %u ms, S6.19: 'Three copies about 250 "
                         "ms apart')", i - 1, i, (unsigned)delta);
                check(delta >= 250u && delta <= 255u, what);
            }
        }
    }
    apad_server_destroy(srv);
}

static void scenario_inputcaps_not_before_ack(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t hello_buf[APAD_MAX_DATAGRAM];
    int hello_len;
    uint16_t sid = 0, wseq = 0;
    uint32_t t;

    printf("\n-- inputcaps_not_before_ack: none sent before the ACK that "
           "discharges WELCOME --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 2, 2, 41121);
    check(establish_hello(srv, &hs, peer, 0u, 0x71u, "harness-ic-noack", 1,
                          hello_buf, &hello_len, &sid, &wseq),
          "HELLO -> WELCOME (no ACK sent)");

    for (t = 1u; t <= 5000u; t += 20u) {
        (void)apad_server_tick(srv, t);
    }
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) == 0,
          "zero INPUTCAPS over 5s with no ACK ever received (S6.19 Delivery: "
          "'Not before the ACK that discharges WELCOME')");

    apad_server_destroy(srv);
}

static void scenario_inputcaps_auth_waits_for_verified_tag(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    uint8_t key[APAD_SESSION_KEY_LEN];
    uint8_t buf[APAD_MAX_DATAGRAM];
    apad_ping ping;
    uint8_t payload[APAD_LEN_PING];
    uint16_t sid = 0;
    uint32_t t;
    int n, an;
    apad_ack a;

    printf("\n-- inputcaps_auth_waits_for_verified_tag: on an AUTH_REQUIRED "
           "session, nothing is sent until a client datagram's tag verifies "
           "(S6.19 Delivery / S10.2's ~1s PBKDF2 window) --\n");
    srv = make_server_kbm_pairing(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create (pairing + KEYBOARD cap)");
    random_script_reset();
    g_random.prng = 0x5EED1u;
    check(apad_server_begin_pairing(srv, 0u, 0) == APAD_OK, "begin_pairing");

    apad_addr_set(&peer, 10, 2, 2, 3, 41122);
    check(pair_and_hello(srv, &hs, peer, 0u, 1, 0x72u, key, &sid),
          "HELLO -> AUTH_REQUIRED WELCOME, key derived as a real client would");

    /* The exempted, deliberately UNAUTHENTICATED ACK that discharges
     * WELCOME (S10's one exemption) -- this does NOT arm INPUTCAPS on an
     * AUTH_REQUIRED session (handle_ack: "arm here ONLY when this session
     * was never issued an AUTH_REQUIRED WELCOME"). */
    memset(&a, 0, sizeof a);
    a.sequence = hs.sends[hs.count - 1].sequence;
    an = apad_encode_ack(payload, sizeof payload, &a);
    n = build_raw(buf, sizeof buf, (uint8_t)APAD_MSG_ACK, sid, 1, payload, (uint16_t)an);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);

    /* Simulate ~1s of PBKDF2 on the client (S10.2) -- the server hears
     * nothing further, tagged or not, during this window. */
    for (t = 20u; t <= 1000u; t += 20u) {
        (void)apad_server_tick(srv, t);
    }
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) == 0,
          "zero INPUTCAPS across the ~1s PBKDF2 window -- this is exactly "
          "the trap S6.19 spends a paragraph on: a naive 0/250/500ms "
          "schedule from the ACK would land all three copies here, where "
          "the client cannot yet decrypt them");

    /* Now the client's key is ready and it sends its first AUTHENTICATED
     * datagram -- an ordinary PING, tag verifies. */
    memset(&ping, 0, sizeof ping);
    ping.origin_ticks_ms = 1000u;
    (void)apad_encode_ping(payload, sizeof payload, &ping);
    n = build_raw_auth(buf, sizeof buf, (uint8_t)APAD_MSG_PING, sid, 2,
                       payload, (uint16_t)sizeof payload, key);
    (void)apad_server_on_datagram(srv, 1000u, &peer, buf, (size_t)n);
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) == 0,
          "still zero right after the verifying tag: arming happens in "
          "on_datagram, delivery happens in tick -- nothing has ticked yet");

    /* The tick that CONSUMES inputcaps_dirty (resets next_ms to itself) does
     * not also send in the same call (apad_time_after is strict -- see
     * scenario_inputcaps_after_ack's own "no INPUTCAPS sent by the ACK
     * itself" for the identical two-tick pattern on the non-auth path). */
    (void)apad_server_tick(srv, 1001u);
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) == 0,
          "still zero on the tick that arms the burst (next_ms == now_ms, "
          "not yet due)");

    (void)apad_server_tick(srv, 1002u);
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) >= 1,
          "the burst begins on the FOLLOWING tick -- the first copy after "
          "the first verifying tag, never before it");

    apad_server_destroy(srv);
}

static void scenario_inputcaps_rearmed_on_status_change(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    uint32_t t;
    int n, before, seen;
    const recorded_send *r;
    apad_inputcaps ic;

    printf("\n-- inputcaps_rearmed_on_status_change: creating a device "
           "re-arms the advertisement so the client sees *_READY promptly, "
           "not after up to 5s --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 2, 4, 41123);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x73u, "harness-ic-rearm", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK, t=0");

    for (t = 1u; t <= 900u; t++) {
        (void)apad_server_tick(srv, t);
    }
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) == 3,
          "initial 3-copy burst completed");
    r = nth_send_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS, 2);
    check(r != NULL && inputcaps_of(r, &ic) && (ic.status & APAD_KBM_STATUS_KEYBOARD_READY) == 0u,
          "...none of them show KEYBOARD_READY (nothing created yet)");

    before = count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS);
    memset(&kb, 0, sizeof kb);
    key_set(kb.keys, APAD_HID_KEY_A);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    (void)apad_server_on_datagram(srv, 901u, &peer, buf, (size_t)n);

    seen = 0;
    for (t = 902u; t <= 910u && !seen; t++) {
        int c0 = count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS);
        (void)apad_server_tick(srv, t);
        if (count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) > c0) {
            seen = 1;
        }
    }
    check(seen, "a fresh copy goes out within 10ms of device creation -- "
                "re-armed, not waiting on the 5s slow repeat");
    check(count_sends_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS) == before + 1,
          "exactly one new copy so far");
    r = nth_send_of_type(&hs, (uint8_t)APAD_MSG_INPUTCAPS, before);
    check(r != NULL && inputcaps_of(r, &ic) && (ic.status & APAD_KBM_STATUS_KEYBOARD_READY) != 0u,
          "...and THIS copy shows KEYBOARD_READY (S6.19: 'status is what "
          "exists at this instant')");

    apad_server_destroy(srv);
}

/* ------------------------------------------------------------------------
 * MOUSE accumulator semantics (§6.16, §6.21)
 * ------------------------------------------------------------------------ */

static void scenario_mouse_baseline_zero(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_mouse mo;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- mouse_baseline_zero: first accepted MOUSE -> ZERO motion, "
           "whatever the accumulator's raw value --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_MOUSE);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 3, 1, 41130);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x80u, "harness-baseline", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&mo, 0, sizeof mo);
    mo.dx_accum = 12345u;
    n = build_mouse(buf, sizeof buf, sid, 3, &mo);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 0,
          "no mouse_motion() call at all for the baseline packet -- kbm.c "
          "only calls the hook when motion is nonzero, and the first "
          "accepted MOUSE always computes zero (S6.16 rule 1)");

    mo.dx_accum = 12355u;   /* +10 from the baseline above */
    n = build_mouse(buf, sizeof buf, sid, 4, &mo);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 1 && g_backend.mouse_motions[0].m.dx == 10,
          "the SECOND packet's diff is +10, not +12355 -- proves 12345 was "
          "correctly recorded as the baseline, not silently treated as 0");

    apad_server_destroy(srv);
}

static void scenario_mouse_wrap(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_mouse mo;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- mouse_wrap: accumulator 0xFFFB -> 0x0005 gives dx +10, not "
           "-65526 (S6.21's own worked example, applied through the server) --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_MOUSE);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 3, 2, 41131);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x81u, "harness-wrap", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&mo, 0, sizeof mo);
    mo.dx_accum = 0xFFFBu;
    n = build_mouse(buf, sizeof buf, sid, 3, &mo);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 0, "baseline packet: no motion call");

    mo.dx_accum = 0x0005u;
    n = build_mouse(buf, sizeof buf, sid, 4, &mo);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 1 && g_backend.mouse_motions[0].m.dx == 10,
          "dx == +10 across the wrap, per apad_seq_diff(0x0005, 0xFFFB) "
          "(S6.21)");

    apad_server_destroy(srv);
}

static void scenario_mouse_gap_recovered(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_mouse mo;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- mouse_gap_recovered: deliver packets 1 and 3, drop 2 -- "
           "total motion equals 1->3 exactly, nothing lost --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_MOUSE);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 3, 3, 41132);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x82u, "harness-gap", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&mo, 0, sizeof mo);
    mo.dx_accum = 1000u;
    n = build_mouse(buf, sizeof buf, sid, 3, &mo);   /* "packet 1" */
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 0, "packet 1: baseline, no motion call");

    /* "packet 2" (dx_accum would have been e.g. 1015) is simply never sent. */

    mo.dx_accum = 1030u;
    n = build_mouse(buf, sizeof buf, sid, 5, &mo);   /* "packet 3" */
    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 1 && g_backend.mouse_motions[0].m.dx == 30,
          "packet 3's diff spans the gap: 1030-1000 = +30, exactly the "
          "total displacement across all three, even though packet 2 was "
          "never delivered (S6.16 rule 3: 'a dropped packet costs nothing')");

    apad_server_destroy(srv);
}

static void scenario_mouse_stale_no_baseline_advance(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_mouse mo;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- mouse_stale_no_baseline_advance: a discarded stale packet "
           "must not move the baseline, or the next good packet jerks "
           "backwards --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_MOUSE);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 3, 4, 41133);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x83u, "harness-stalebase", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&mo, 0, sizeof mo);
    mo.dx_accum = 100u;
    n = build_mouse(buf, sizeof buf, sid, 5, &mo);   /* header seq 5: baseline */
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 0, "baseline established at dx_accum=100");

    mo.dx_accum = 500u;
    n = build_mouse(buf, sizeof buf, sid, 3, &mo);   /* header seq 3: STALE (< 5) */
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 0,
          "the stale packet (dx_accum=500) never reaches kbm.c at all -- "
          "discarded at S6.20's window before apad_kbm_apply_mouse() runs");

    mo.dx_accum = 110u;
    n = build_mouse(buf, sizeof buf, sid, 6, &mo);   /* header seq 6: newer, ordinary +10 */
    (void)apad_server_on_datagram(srv, 20u, &peer, buf, (size_t)n);
    check(g_backend.mouse_motion_count == 1 && g_backend.mouse_motions[0].m.dx == 10,
          "dx == +10 (110-100), NOT apad_seq_diff(110,500)==-390 -- the "
          "baseline was never touched by the stale packet");

    apad_server_destroy(srv);
}

/* ------------------------------------------------------------------------
 * The event ring — receiver algorithm (§6.20)
 * ------------------------------------------------------------------------ */

static void scenario_key_ring_replay(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;
    static const uint8_t k8[8] = {
        APAD_HID_KEY_A, APAD_HID_KEY_B, APAD_HID_KEY_C, APAD_HID_KEY_D,
        APAD_HID_KEY_E, APAD_HID_KEY_F, APAD_HID_KEY_G, APAD_HID_KEY_H
    };

    printf("\n-- key_ring_replay, gap of 3: exactly 3 events, oldest first --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 4, 1, 41140);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x90u, "harness-ring3", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);   /* baseline, event_seq=0, empty */
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 0, "baseline: empty snapshot, no batch at all");

    memset(&kb, 0, sizeof kb);
    kb.event_seq = 3u;   /* gap of 3 from last_applied=0 */
    kb.events[5].usage = k8[0]; kb.events[5].flags = APAD_KBM_EVENT_DOWN;  /* A */
    kb.events[6].usage = k8[1]; kb.events[6].flags = APAD_KBM_EVENT_DOWN;  /* B */
    kb.events[7].usage = k8[2]; kb.events[7].flags = APAD_KBM_EVENT_DOWN;  /* C */
    key_set(kb.keys, k8[0]); key_set(kb.keys, k8[1]); key_set(kb.keys, k8[2]);
    n = build_keyboard(buf, sizeof buf, sid, 4, &kb);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 1, "exactly one batch call");
    if (g_backend.kbm_batch_count == 1) {
        const kbm_events_call *c = &g_backend.kbm_batches[0];
        check(c->n == 3, "exactly 3 events (gap == depth used)");
        check(c->n == 3 && c->ev[0].code == k8[0] && c->ev[1].code == k8[1]
                  && c->ev[2].code == k8[2],
              "oldest first: A, B, C in ring order");
    }
    apad_server_destroy(srv);

    printf("\n-- key_ring_replay, gap of 0: none --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    check(establish_and_ack(srv, &hs, peer, 0u, 0x91u, "harness-ring0", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");
    memset(&kb, 0, sizeof kb);
    kb.event_seq = 5u;
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 0, "baseline, empty");
    n = build_keyboard(buf, sizeof buf, sid, 4, &kb);   /* same event_seq=5, same keys */
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 0,
          "gap 0 (event_seq unchanged) and an unchanged snapshot -> NO batch call");
    apad_server_destroy(srv);

    printf("\n-- key_ring_replay, gap > depth (20 > 8): depth applied, and "
           "the lost 12 events should be SURFACED, not swallowed --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    check(establish_and_ack(srv, &hs, peer, 0u, 0x92u, "harness-ringover", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");
    memset(&kb, 0, sizeof kb);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);

    memset(&kb, 0, sizeof kb);
    kb.event_seq = 20u;   /* gap of 20, depth is 8 -> overflow */
    {
        int i;
        for (i = 0; i < 8; i++) {
            kb.events[i].usage = k8[i];
            kb.events[i].flags = APAD_KBM_EVENT_DOWN;
            key_set(kb.keys, k8[i]);
        }
    }
    n = build_keyboard(buf, sizeof buf, sid, 4, &kb);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 1, "exactly one batch call");
    if (g_backend.kbm_batch_count == 1) {
        const kbm_events_call *c = &g_backend.kbm_batches[0];
        check(c->n == 8, "all 8 ring slots replayed (depth applied, not the "
                         "full gap of 20)");
    }
    /* §6.20's receiver algorithm step 2 is normative -- "g > D ... Replay
     * all D slots ... and surface the overflow rather than swallowing it."
     * server/src/kbm.c now hands the lost-event count back to server.c,
     * which reports it through the on_log sink (rate-limited -- see
     * kbm_log_overflow() in server/src/server.c); this was previously a
     * known, reported gap (server/src/kbm.c discarded `overflow` via
     * `(void)overflow` at all three call sites) and is fixed now. */
    check(
              any_log_contains(&hs, "overflow") || any_log_contains(&hs, "lost")
              || any_log_contains(&hs, "gap"),
          "S6.20 step 2: the receiver surfaces an event-ring overflow "
          "(here g=20, D=8, 12 events unrecoverably lost) via the on_log "
          "sink rather than silently discarding it");
    apad_server_destroy(srv);
}

static void scenario_key_bitmap_wins(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- key_bitmap_wins: a ring disagreeing with keys[] -> post-apply "
           "state equals keys[] --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 4, 2, 41141);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x93u, "harness-bitmapwins", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);   /* baseline, A not held */
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);

    memset(&kb, 0, sizeof kb);
    kb.event_seq = 1u;
    kb.events[7].usage = APAD_HID_KEY_A;
    kb.events[7].flags = APAD_KBM_EVENT_DOWN;   /* ring says: A pressed */
    /* keys[] disagrees: A is NOT set, i.e. the snapshot says A is not held. */
    n = build_keyboard(buf, sizeof buf, sid, 4, &kb);
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);

    check(g_backend.kbm_batch_count == 1, "one batch call for this datagram");
    if (g_backend.kbm_batch_count == 1) {
        const kbm_events_call *c = &g_backend.kbm_batches[0];
        check(c->n == 2, "two events: the ring's press, then the "
                         "reconcile's compensating release");
        check(c->n == 2 && c->ev[0].code == APAD_HID_KEY_A && c->ev[0].down == 1u,
              "event 0: ring replay says A pressed");
        check(c->n == 2 && c->ev[1].code == APAD_HID_KEY_A && c->ev[1].down == 0u,
              "event 1: step-3 reconcile immediately corrects it -- net "
              "state (the LAST event for A in this batch) matches keys[] "
              "(not held), not the ring");
    }

    apad_server_destroy(srv);
}

static void scenario_key_reconcile_without_events(void) {
    harness_state hs;
    apad_server *srv;
    apad_addr peer;
    apad_keyboard kb;
    uint8_t buf[APAD_MAX_DATAGRAM];
    uint16_t sid = 0;
    int n;

    printf("\n-- key_reconcile_without_events: snapshot changes while "
           "event_seq does NOT -- still converges in one packet --\n");
    srv = make_server_kbm(&hs, APAD_KBM_CAP_KEYBOARD);
    check(srv != NULL, "apad_server_create");
    apad_addr_set(&peer, 10, 2, 4, 3, 41142);
    check(establish_and_ack(srv, &hs, peer, 0u, 0x94u, "harness-reconcile", 1, 2, &sid),
          "HELLO -> WELCOME -> ACK");

    memset(&kb, 0, sizeof kb);
    n = build_keyboard(buf, sizeof buf, sid, 3, &kb);   /* baseline: event_seq=0, A not held */
    (void)apad_server_on_datagram(srv, 0u, &peer, buf, (size_t)n);
    check(g_backend.kbm_batch_count == 0, "baseline: empty, no batch");

    memset(&kb, 0, sizeof kb);
    kb.event_seq = 0u;          /* UNCHANGED -- a non-conforming sender */
    key_set(kb.keys, APAD_HID_KEY_A);   /* but the snapshot now holds A */
    n = build_keyboard(buf, sizeof buf, sid, 4, &kb);   /* newer HEADER sequence */
    (void)apad_server_on_datagram(srv, 10u, &peer, buf, (size_t)n);

    check(g_backend.kbm_batch_count == 1,
          "converges in exactly this one packet, via step 3's UNCONDITIONAL "
          "reconcile -- not the ring (gap <= 0 replays nothing). Under a "
          "buggy 'reconcile only on overflow' implementation this would "
          "disagree PERMANENTLY (S6.20's own stated failure mode)");
    if (g_backend.kbm_batch_count == 1) {
        const kbm_events_call *c = &g_backend.kbm_batches[0];
        check(c->n == 1 && c->ev[0].code == APAD_HID_KEY_A && c->ev[0].down == 1u,
              "the single event is A pressed, from the reconcile alone");
    }

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

    /* §6.15-§6.20 KEYBOARD/MOUSE/MEDIA/INPUTCAPS. docs/PROTOCOL.md §15 item
     * 10: the watchdog/repeat-floor scenarios are the reason this whole
     * group exists -- core has no clock, this harness does. */
    scenario_kbm_lazy_create();
    scenario_kbm_not_advertised();
    scenario_kbm_stale_dropped();
    scenario_kbm_class_windows();
    scenario_kbm_wrong_peer_address();

    scenario_kbm_watchdog_releases();
    scenario_kbm_watchdog_not_early();
    scenario_kbm_watchdog_boundary();
    scenario_kbm_teardown_releases_before_destroy();

    scenario_inputcaps_after_ack();
    scenario_inputcaps_not_before_ack();
    scenario_inputcaps_auth_waits_for_verified_tag();
    scenario_inputcaps_rearmed_on_status_change();

    scenario_mouse_baseline_zero();
    scenario_mouse_wrap();
    scenario_mouse_gap_recovered();
    scenario_mouse_stale_no_baseline_advance();

    scenario_key_ring_replay();
    scenario_key_bitmap_wins();
    scenario_key_reconcile_without_events();

    printf("\n%d failure(s)\n", g_failures);
    return g_failures != 0;
}
