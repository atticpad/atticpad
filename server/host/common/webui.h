/* server/host/common/webui.h -- the Linux host's server UI: a hand-rolled
 * HTTP/1.1 server bound to 127.0.0.1 plus the JSON API behind it (docs/DESIGN.md
 * §6.3, §6.4 "Host owns ... UI"; docs/PROTOCOL.md §7 tier 3, §8/§10
 * pairing display).
 *
 * Header-only, included only by server/host/linux/main.c, for the same
 * build-script-constraint reason as assets.h and ipaddr.h (see either
 * file's top comment): scripts/build.sh's build_server() names an exact
 * fixed list of .c files and this task's own CONSTRAINTS forbid touching
 * scripts/. Splitting the UI across a few headers rather than jamming it
 * all into main.c is organisation, not a workaround -- none of these add a
 * translation unit.
 *
 * SCOPE, deliberately narrow (per the task brief): this answers a handful
 * of routes to ONE client on loopback. It is not a general web server --
 * no keep-alive, no chunked transfer, no range requests, no MIME sniffing --
 * Content-Length, Host and Origin are the only headers even looked at (Host
 * and Origin purely for the loopback/rebinding checks in webui_poll()
 * below, never forwarded anywhere). Every request and response is capped in
 * size (UI_MAX_REQUEST/UI_MAX_BODY below) and method is restricted to GET,
 * POST, PUT and DELETE; anything else is refused rather than best-effort
 * supported. No dependency was added: everything here is BSD sockets and
 * libc.
 *
 * THREADING: this server is single-threaded, on purpose. apadserver.h says
 * an apad_server is not internally synchronised and a host that talks to it
 * from two threads at once must serialise itself -- the simplest way to
 * satisfy that is to not have two threads. webui_poll() below is called
 * once per iteration of main.c's existing UDP loop, right alongside
 * apad_server_tick()/apad_server_on_datagram(): the listening socket is
 * non-blocking (accept() never stalls the loop), but a socket ONCE ACCEPTED
 * is read and written with a short SO_RCVTIMEO/SO_SNDTIMEO
 * (UI_IO_TIMEOUT_MS) rather than made non-blocking too, which very
 * occasionally means one HTTP request-response delays the next UDP
 * datagram by a bounded, small amount -- a trade this task's own framing
 * accepts ("a handful of routes to one client on loopback", not a
 * real-time guarantee) and one no in-tree client's §8/§9 budget is anywhere
 * close to (100 ms first retransmit, 3000 ms idle timeout) is put at risk
 * by a sub-second stall.
 */
#ifndef ATTICPAD_HOST_COMMON_WEBUI_H
#define ATTICPAD_HOST_COMMON_WEBUI_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every socket call below is plain BSD sockets on both platforms; the small
 * set that genuinely is NOT the same call lives behind this one header. See
 * sockcompat.h's top comment for the exact list and why each one is there.
 * It also pulls in winsock2.h/ws2tcpip.h (in the right order) on Windows and
 * sys/socket.h on POSIX, so neither is named here. */
#include "sockcompat.h"

#ifdef _WIN32
/* sockaddr_in, htons/ntohl, INADDR_LOOPBACK, IPPROTO_TCP and TCP_NODELAY all
 * come from winsock2.h, already included above. Only the case-insensitive
 * compare differs, and only in its name. */
#define apad_strncasecmp _strnicmp
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>   /* strncasecmp */
#define apad_strncasecmp strncasecmp
#endif

#include "atticpad/atticpad.h"
#include "apadserver.h"
#include "assets.h"
#include "assets_editor.h"   /* server/host/common/assets_editor.h -- the
                          * per-client remapping editor page, split out of
                          * assets.h once that file would otherwise cross
                          * ~1500 lines of literal (that file's own top
                          * comment); served at GET /editor below, same
                          * header-only reasoning. */
#include "ipaddr.h"
#include "profiles.h"   /* apad_profiles_list/apad_profiles_find -- server/src/,
                          * already linked into every build_server() binary */
#include "ui_mdns_status.h"  /* docs/PROTOCOL.md §7
                          * tier 1. Included here (one direction only:
                          * mdns.h knows nothing about the UI) so /api/state
                          * can report the responder's status, which §7 makes
                          * a MUST for the failed-bind case. */
#include "jsonc.h"       /* json_parse/json_obj_get/json_str -- reused for the
                          * small POST/PUT bodies below rather than
                          * hand-rolling a second parser */
#include "qr.h"          /* server/host/linux/qr.h -- docs/PROTOCOL.md §10.3
                          * pairing-URI build (thin wrapper over core's
                          * apad_pair_uri_build) and SVG rendering, for
                          * /api/pair/qr.svg below */
#include "strbuf.h"      /* ui_strbuf + sb_* -- factored out so
                          * profile_json.h can build JSON without depending
                          * on this file (see strbuf.h's own top comment) */
#include "profile_store.h"  /* directory scan, atomic save/delete, the
                          * shipped-profile check -- the filesystem half of
                          * the remapping editor's profile routes below */
#include "profile_json.h"   /* apad_profile <-> JSON, for the same routes */

/* ---- limits (§8.5-style "cap it, don't try to be a general web server") -- */
#define UI_MAX_REQUEST    8192u   /* request line + headers                 */
#define UI_MAX_BODY       16384u  /* PUT body, e.g. a whole profile's JSON  */
#define UI_IO_TIMEOUT_MS  500     /* SO_RCVTIMEO/SO_SNDTIMEO per recv()     */
/* UI_IO_TIMEOUT_MS bounds a single recv() call, not the whole request: a
 * client that sends one byte every 400 ms never trips it, yet still holds
 * this single-threaded server's one connection slot open indefinitely --
 * long enough to stall apad_server_tick()/on_datagram() past every
 * connected session's 3000 ms idle timeout (protocol audit finding). This
 * is the wall-clock ceiling ui_read_request() enforces across BOTH of its
 * recv() loops (headers, then body), checked against apad_ticks_ms() taken
 * once at the start of the request, not reset per read. */
#define UI_REQUEST_DEADLINE_MS 2000u
#define UI_LISTEN_BACKLOG 8

/* ---- the log ring buffer (apadserver.h on_log's doc comment anticipates a
 * UI consuming it; server/host/windows/main.c has no UI yet so this is
 * Linux-only for now) -------------------------------------------------- */
#define UI_LOG_LINES     200
#define UI_LOG_LINE_MAX  200

typedef struct {
    char   line[UI_LOG_LINE_MAX];
} ui_log_entry;

typedef struct {
    ui_log_entry entries[UI_LOG_LINES];
    uint32_t     writes;   /* total lines ever pushed, wraps at 2^32 (fine --
                             * only used to find the oldest still-held line) */
} ui_log_ring;

static ui_log_ring g_ui_log;

static void ui_log_push(const char *line)
{
    uint32_t idx = g_ui_log.writes % (uint32_t)UI_LOG_LINES;
    (void)snprintf(g_ui_log.entries[idx].line, UI_LOG_LINE_MAX, "%s", line);
    g_ui_log.writes++;
}

/* ---- JSON string building: ui_strbuf, sb_*() -- server/host/common/strbuf.h,
 * included above ---------------------------------------------------------- */

/* apad_backend_health_state (server/backends/backend.h) -> a JSON string.
 * This is the one place in the whole UI allowed to know the enum's spelling
 * -- it never learns, and must never learn, WHICH backend produced it (§6.1:
 * "nothing outside server/backends/ may know which backend is active"). A
 * front end branches on this string (and optionally shows `remedy` as a
 * link or a copyable command), never on `backend.name`. */
static const char *backend_health_state_name(apad_backend_health_state st)
{
    switch (st) {
    case APAD_BACKEND_HEALTH_OK:                 return "ok";
    case APAD_BACKEND_HEALTH_DRIVER_MISSING:     return "driver_missing";
    case APAD_BACKEND_HEALTH_PERMISSION_DENIED:  return "permission_denied";
    case APAD_BACKEND_HEALTH_VERSION_MISMATCH:   return "version_mismatch";
    case APAD_BACKEND_HEALTH_OTHER:              /* fall through */
    default:                                     return "other";
    }
}

/* ---- building /api/state ------------------------------------------------ */

static void ui_build_state_json(ui_strbuf *out, const apad_server *server,
                                uint16_t udp_port, const char *server_name,
                                const ui_mdns_status *mdns)
{
    host_own_addr addrs[HOST_MAX_OWN_ADDRS];
    size_t        naddr, i;
    apad_backend_status bstat;
    apad_pairing_info   pinfo;
    apad_client_info    clients[APAD_MAX_SESSIONS];
    int                  nclients, c;
    const apad_profile  *profiles[APAD_PROFILES_MAX];
    size_t               nprofiles;

    naddr = host_enumerate_own_ipv4(addrs, HOST_MAX_OWN_ADDRS);
    (void)apad_server_backend_status(server, &bstat);
    (void)apad_server_pairing_state(server, &pinfo);
    nclients = apad_server_list_clients(server, clients, APAD_MAX_SESSIONS);
    if (nclients > (int)APAD_MAX_SESSIONS) {
        nclients = (int)APAD_MAX_SESSIONS;   /* defensive; never happens today */
    }
    nprofiles = apad_profiles_list(profiles, APAD_PROFILES_MAX);
    if (nprofiles > APAD_PROFILES_MAX) {
        nprofiles = APAD_PROFILES_MAX;
    }

    sb_append(out, "{\"server\":{");
    sb_appendf(out, "\"port\":%u,", (unsigned)udp_port);
    sb_append(out, "\"name\":");
    sb_json_string(out, server_name);
    sb_append(out, ",\"own_ips\":[");
    for (i = 0; i < naddr; i++) {
        if (i > 0) {
            sb_append(out, ",");
        }
        sb_append(out, "{\"iface\":");
        sb_json_string(out, addrs[i].iface);
        sb_append(out, ",\"ip\":");
        sb_json_string(out, addrs[i].ip);
        sb_append(out, ",\"kind\":");
        sb_json_string(out, host_addr_kind_name(addrs[i].kind));
        sb_append(out, "}");
    }
    sb_append(out, "]");
    /* Which address a fresh QR/URI defaults to (ipaddr.h
     * host_pick_default_addr(): first LAN-classified address, else the
     * first Tailscale one, else the first virtual one) -- the UI's address
     * selector (assets.h) preselects this, but the choice is not locked in:
     * the operator can pick any entry in own_ips above and /api/pair/qr.svg
     * (?addr=<ip>) and the "uris" list below cover every one of them, not
     * just this one. null only when own_ips is empty. */
    {
        size_t defi = host_pick_default_addr(addrs, naddr);
        sb_append(out, ",\"default_ip\":");
        if (defi != (size_t)-1) {
            sb_json_string(out, addrs[defi].ip);
        } else {
            sb_append(out, "null");
        }
    }
    sb_append(out, ",\"backend\":{\"name\":");
    sb_json_string(out, bstat.name);
    sb_appendf(out, ",\"ok\":%s,\"state\":", bstat.ok ? "true" : "false");
    sb_json_string(out, backend_health_state_name(bstat.state));
    sb_append(out, ",\"message\":");
    sb_json_string(out, bstat.message);
    sb_append(out, ",\"remedy\":");
    if (bstat.remedy[0] != '\0') {
        sb_json_string(out, bstat.remedy);
    } else {
        sb_append(out, "null");
    }
    sb_append(out, "},");
    /* §7 tier 1 (mDNS). "If the 5353 bind fails, tier 1 is disabled and the
     * server UI MUST say so" -- this block is where that MUST is satisfied,
     * and docs/DESIGN.md §6.3's "mDNS status, so a failed 5353 bind is visible
     * rather than mysterious" is the same requirement said twice.
     *
     * `state` is what a front end branches on; `message` is the sentence and
     * `remedy` the thing a human can DO, exactly the shape the backend block
     * above already uses (message/remedy), so the UI renders both the same
     * way. `txt` is the LIVE record set, rebuilt from the same
     * mdns_txt_items() the responder puts on the wire -- not a description of
     * it -- so what the UI shows and what a phone receives cannot drift.
     *
     * `implemented` is the field to branch on: a host with no responder at
     * all (Windows, today) still emits this whole block, with implemented
     * false and a `message` naming tiers 2 and 3 instead. Reporting the
     * absence is itself part of the §7 MUST -- silence would read as
     * "working". See ui_mdns_status.h. */
    sb_appendf(out, "\"mdns\":{\"implemented\":%s,\"state\":",
               mdns->implemented ? "true" : "false");
    sb_json_string(out, mdns->state);
    sb_appendf(out, ",\"ok\":%s", mdns->ok ? "true" : "false");
    sb_append(out, ",\"service\":");
    sb_json_string(out, mdns->service);
    sb_append(out, ",\"instance\":");
    sb_json_string(out, mdns->instance);
    sb_append(out, ",\"host\":");
    sb_json_string(out, mdns->host);
    sb_appendf(out, ",\"bind_port\":%u,\"service_port\":%u",
               (unsigned)mdns->bind_port, (unsigned)mdns->service_port);
    sb_append(out, ",\"message\":");
    sb_json_string(out, mdns->message);
    sb_append(out, ",\"remedy\":");
    if (mdns->remedy[0] != '\0') {
        sb_json_string(out, mdns->remedy);
    } else {
        sb_append(out, "null");
    }
    sb_append(out, ",\"txt\":[");
    {
        size_t k;
        for (k = 0; k < mdns->ntxt; k++) {
            if (k > 0) {
                sb_append(out, ",");
            }
            sb_json_string(out, mdns->txt[k]);
        }
    }
    sb_appendf(out, "],\"queries_rx\":%u,\"responses_tx\":%u,"
                    "\"dropped_tx\":%u,\"announcements_tx\":%u}",
               (unsigned)mdns->queries_rx, (unsigned)mdns->responses_tx,
               (unsigned)mdns->dropped_tx, (unsigned)mdns->announces_tx);
    sb_append(out, "},");

    sb_append(out, "\"pairing\":{");
    sb_appendf(out, "\"open\":%s", pinfo.open ? "true" : "false");
    if (pinfo.open) {
        sb_append(out, ",\"kind\":");
        sb_json_string(out, (pinfo.kind == (uint8_t)APAD_PAIRING_TOKEN)
                                 ? "token" : "pin");
        sb_append(out, ",\"secret\":");
        sb_json_string(out, pinfo.secret);
        sb_appendf(out, ",\"ms_remaining\":%u,\"attempts_remaining\":%u,"
                        "\"generation\":%u",
                  (unsigned)pinfo.ms_remaining,
                  (unsigned)pinfo.attempts_remaining,
                  (unsigned)pinfo.generation);
        /* §10.3 URIs, selectable text alongside the QR image below --
         * present ONLY while pinfo.open (same gate as "secret" two lines
         * up): the URI carries the secret and docs/PROTOCOL.md §10.3 is
         * explicit that a displayed URI is exactly as sensitive as a
         * displayed PIN.
         *
         * ONE URI PER own_ips ENTRY, not just addrs[0]: a single QR/URI can
         * only ever encode one address, but which one is a choice the UI
         * lets the operator make (assets.h's address selector), not an
         * accident of enumeration order (the old behaviour). Building all
         * of them here -- rather than exposing only the default and making
         * the front end reconstruct the §10.3 grammar itself -- keeps
         * apad_pair_uri_build() the ONLY place that grammar is implemented
         * (this file's own top comment). naddr is bounded by
         * HOST_MAX_OWN_ADDRS (16), so this is a handful of cheap calls, not
         * a loop that can grow unbounded. */
        sb_append(out, ",\"uris\":[");
        for (i = 0; i < naddr; i++) {
            char uri[QR_PAIR_URI_BUF];
            int  ulen = qr_build_pairing_uri(uri, addrs[i].ip, udp_port,
                                             pinfo.secret);
            if (i > 0) {
                sb_append(out, ",");
            }
            sb_append(out, "{\"ip\":");
            sb_json_string(out, addrs[i].ip);
            sb_append(out, ",\"uri\":");
            if (ulen > 0) {
                sb_json_string(out, uri);
            } else {
                sb_append(out, "null");
            }
            sb_append(out, "}");
        }
        sb_append(out, "]");
    }
    sb_append(out, "},");

    sb_append(out, "\"profiles\":[");
    for (i = 0; i < nprofiles; i++) {
        if (i > 0) {
            sb_append(out, ",");
        }
        sb_json_string(out, profiles[i]->name);
    }
    sb_append(out, "],");

    sb_append(out, "\"clients\":[");
    for (c = 0; c < nclients; c++) {
        const apad_client_info *ci = &clients[c];
        char peer[32];

        if (c > 0) {
            sb_append(out, ",");
        }
        (void)snprintf(peer, sizeof peer, "%u.%u.%u.%u:%u",
                       ci->peer.ip[0], ci->peer.ip[1], ci->peer.ip[2],
                       ci->peer.ip[3], (unsigned)ci->peer.port);
        sb_append(out, "{");
        sb_appendf(out, "\"slot\":%u,\"session_id\":%u,",
                  (unsigned)ci->slot, (unsigned)ci->session_id);
        sb_append(out, "\"device_name\":");
        sb_json_string(out, ci->device_name);
        sb_append(out, ",\"peer\":");
        sb_json_string(out, peer);
        sb_appendf(out, ",\"caps\":%u,", (unsigned)ci->caps);
        sb_append(out, "\"profile\":");
        sb_json_string(out, ci->profile_name);
        if (ci->rtt_ms == APAD_RTT_UNKNOWN) {
            sb_append(out, ",\"rtt_ms\":null");
        } else {
            sb_appendf(out, ",\"rtt_ms\":%u", (unsigned)ci->rtt_ms);
        }
        sb_appendf(out, ",\"battery\":%u,\"authenticated\":%s,"
                        "\"rx_packets\":%u,\"tx_packets\":%u}",
                  (unsigned)ci->battery, ci->authenticated ? "true" : "false",
                  (unsigned)ci->rx_packets, (unsigned)ci->tx_packets);
    }
    sb_append(out, "],");

    sb_append(out, "\"log\":[");
    {
        uint32_t total = g_ui_log.writes;
        uint32_t held  = (total < (uint32_t)UI_LOG_LINES) ? total
                                                          : (uint32_t)UI_LOG_LINES;
        uint32_t start = total - held;   /* oldest line's write-index */
        uint32_t k;

        for (k = 0; k < held; k++) {
            uint32_t idx = (start + k) % (uint32_t)UI_LOG_LINES;
            if (k > 0) {
                sb_append(out, ",");
            }
            sb_json_string(out, g_ui_log.entries[idx].line);
        }
    }
    sb_append(out, "]}");
}

/* ---- minimal HTTP/1.1 request handling ---------------------------------- */

typedef struct {
    char   method[8];
    char   path[256];     /* without query string */
    char   query[256];
    char   host[128];     /* Host header value, e.g. "127.0.0.1:21150" */
    char   origin[128];   /* Origin header value, or "" if absent */
    char   body[UI_MAX_BODY + 1];
    size_t body_len;
} ui_request;

/* Copies the header value spanning [v, eol) into `out` (bounded,
 * NUL-terminated), skipping leading spaces/tabs -- shared by the Host and
 * Origin extraction below, the same "one string compare, no real header
 * parser" spirit as ui_query_get(). */
static void ui_copy_header_span(const char *v, const char *eol, char *out,
                                size_t out_cap)
{
    size_t vlen;

    while (v < eol && (*v == ' ' || *v == '\t')) {
        v++;
    }
    vlen = (size_t)(eol - v);
    if (vlen >= out_cap) {
        vlen = out_cap - 1u;
    }
    memcpy(out, v, vlen);
    out[vlen] = '\0';
}

/* Reads and parses one request off `fd` (already accepted, with
 * SO_RCVTIMEO/SO_SNDTIMEO set by the caller). Returns 1 on a well-formed
 * request within the size caps, 0 on a malformed/oversized one (caller
 * sends 400), -1 if UI_REQUEST_DEADLINE_MS elapsed before the request
 * finished arriving (caller sends 408 -- see that constant's comment: this
 * is distinct from a single recv() timeout, which still returns 0 below).
 * Content-Length, Host and Origin are the only headers looked at; every
 * other header byte is scanned past but not interpreted -- this is not a
 * general HTTP server (this file's own top comment). */
static int ui_read_request(apad_socket_t fd, ui_request *req)
{
    char   raw[UI_MAX_REQUEST + 1];
    size_t got = 0;
    char  *header_end;
    char  *line_end;
    char  *p;
    long   content_length = 0;
    uint32_t deadline_start = apad_ticks_ms();

    memset(req, 0, sizeof *req);

    /* Read until we see the blank line ending the headers, or run out of
     * room/patience. One recv() at a time so a slow-trickling client still
     * completes within UI_IO_TIMEOUT_MS per read -- but the wall-clock
     * check below is what stops it trickling forever, one byte at a time,
     * each read individually inside that per-read timeout. */
    for (;;) {
        int n;

        if (apad_time_since(apad_ticks_ms(), deadline_start)
            > UI_REQUEST_DEADLINE_MS) {
            return -1;
        }
        if (got >= UI_MAX_REQUEST) {
            return 0;   /* header block too large */
        }
        /* int, not ssize_t: recv()'s length is size_t/ssize_t on POSIX but
         * int/int on Winsock. int is the narrower of the two and every
         * length here is bounded by UI_MAX_REQUEST, so the casts are safe
         * on both and the code says the same thing on both. */
        n = recv(fd, raw + got, (int)(UI_MAX_REQUEST - got), 0);
        if (n <= 0) {
            return 0;   /* timeout, reset, or EOF before headers finished */
        }
        got += (size_t)n;
        raw[got] = '\0';
        header_end = strstr(raw, "\r\n\r\n");
        if (header_end != NULL) {
            break;
        }
    }

    /* Request line: "METHOD SP path SP version CRLF". */
    line_end = strstr(raw, "\r\n");
    if (line_end == NULL) {
        return 0;
    }
    {
        char *sp1, *sp2, *qmark;
        size_t plen;

        sp1 = memchr(raw, ' ', (size_t)(line_end - raw));
        if (sp1 == NULL) {
            return 0;
        }
        sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
        if (sp2 == NULL) {
            return 0;
        }
        if ((size_t)(sp1 - raw) >= sizeof req->method) {
            return 0;
        }
        memcpy(req->method, raw, (size_t)(sp1 - raw));
        req->method[sp1 - raw] = '\0';

        plen = (size_t)(sp2 - (sp1 + 1));
        qmark = memchr(sp1 + 1, '?', plen);
        if (qmark != NULL) {
            size_t path_len = (size_t)(qmark - (sp1 + 1));
            size_t q_len    = (size_t)(sp2 - (qmark + 1));
            if (path_len >= sizeof req->path || q_len >= sizeof req->query) {
                return 0;
            }
            memcpy(req->path, sp1 + 1, path_len);
            req->path[path_len] = '\0';
            memcpy(req->query, qmark + 1, q_len);
            req->query[q_len] = '\0';
        } else {
            if (plen >= sizeof req->path) {
                return 0;
            }
            memcpy(req->path, sp1 + 1, plen);
            req->path[plen] = '\0';
        }
    }

    /* Content-Length, Host and Origin, case-insensitively -- the only
     * headers this file reads. */
    for (p = raw; p < header_end; ) {
        char *eol = strstr(p, "\r\n");
        if (eol == NULL || eol > header_end) {
            break;
        }
        if (eol > p && (size_t)(eol - p) > 15
            && apad_strncasecmp(p, "Content-Length:", 15) == 0) {
            content_length = atol(p + 15);
        } else if (eol > p && (size_t)(eol - p) > 5
                  && apad_strncasecmp(p, "Host:", 5) == 0) {
            ui_copy_header_span(p + 5, eol, req->host, sizeof req->host);
        } else if (eol > p && (size_t)(eol - p) > 7
                  && apad_strncasecmp(p, "Origin:", 7) == 0) {
            ui_copy_header_span(p + 7, eol, req->origin, sizeof req->origin);
        }
        p = eol + 2;
    }

    if (content_length < 0 || (size_t)content_length > UI_MAX_BODY) {
        return 0;   /* refuse rather than try to read a body we won't keep */
    }

    /* Body bytes already in `raw` past header_end + 4, plus whatever is
     * still arriving. */
    {
        size_t have = got - (size_t)(header_end + 4 - raw);
        size_t need = (size_t)content_length;

        if (have > need) {
            have = need;   /* pipelined bytes past this request: ignored,
                            * no keep-alive (this file's top comment) */
        }
        memcpy(req->body, header_end + 4, have);
        req->body_len = have;
        while (req->body_len < need) {
            int n;

            if (apad_time_since(apad_ticks_ms(), deadline_start)
                > UI_REQUEST_DEADLINE_MS) {
                return -1;
            }
            n = recv(fd, req->body + req->body_len,
                    (int)(need - req->body_len), 0);
            if (n <= 0) {
                return 0;
            }
            req->body_len += (size_t)n;
        }
        req->body[req->body_len] = '\0';
    }
    return 1;
}

static void ui_send_response(apad_socket_t fd, int status, const char *status_text,
                             const char *content_type, const char *body,
                             size_t body_len)
{
    char head[256];
    int  hn;

    hn = snprintf(head, sizeof head,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-store\r\n"
                 "\r\n",
                 status, status_text, content_type, body_len);
    if (hn > 0) {
        (void)send(fd, head, hn, APAD_MSG_NOSIGNAL);
    }
    if (body_len > 0) {
        (void)send(fd, body, (int)body_len, APAD_MSG_NOSIGNAL);
    }
}

static void ui_send_json(apad_socket_t fd, int status, const char *status_text,
                         ui_strbuf *body)
{
    ui_send_response(fd, status, status_text, "application/json",
                     (body->buf != NULL) ? body->buf : "{}",
                     body->buf != NULL ? body->len : 2u);
}

static void ui_send_json_error(apad_socket_t fd, int status, const char *status_text,
                               const char *error)
{
    ui_strbuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"ok\":false,\"error\":");
    sb_json_string(&sb, error);
    sb_append(&sb, "}");
    ui_send_json(fd, status, status_text, &sb);
    sb_free(&sb);
}

static void ui_send_ok(apad_socket_t fd)
{
    static const char body[] = "{\"ok\":true}";
    ui_send_response(fd, 200, "OK", "application/json", body, sizeof body - 1u);
}

/* /api/pair/begin?kind=pin|token -- see this file's top comment: query
 * string, not a JSON body, because the request carries no data beyond that
 * one choice and a query parameter is one string compare instead of a
 * parser round trip. */
static int ui_query_wants_token(const char *query)
{
    const char *k = strstr(query, "kind=");
    if (k == NULL) {
        return 0;
    }
    k += 5;
    return strncmp(k, "token", 5) == 0;
}

/* Extracts `key`'s value out of a "k1=v1&k2=v2" query string into `out`
 * (bounded, NUL-terminated), stopping at the next '&' or end of string.
 * Returns 1 if `key` was present, 0 otherwise (out left untouched) --
 * mirrors ui_query_wants_token()'s "no dependency, one string search"
 * approach rather than pulling in a real query-string parser for what is,
 * across this whole file, two parameters. */
static int ui_query_get(const char *query, const char *key, char *out,
                        size_t out_cap)
{
    size_t klen = strlen(key);
    const char *p = query;

    for (;;) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1u;
            const char *amp = strchr(v, '&');
            size_t vlen = (amp != NULL) ? (size_t)(amp - v) : strlen(v);
            if (vlen >= out_cap) {
                vlen = out_cap - 1u;
            }
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (p == NULL) {
            return 0;
        }
        p++;
    }
}

/* Scans `profiles_dir` fresh, hands the result to
 * apad_server_reload_profiles() (apadserver.h) -- the SAME scan-then-reload
 * sequence every one of PUT/DELETE /api/profile/{name} and POST
 * /api/profiles/reload needs after touching a file, factored out once
 * rather than repeated three times. Returns 1 on success, 0 if the library
 * reload itself reported an error (apad_server_reload_profiles() only fails
 * on a NULL server, which cannot happen here -- kept as a real return value
 * anyway rather than (void), matching this file's convention of never
 * silently swallowing an apad_server_* return). */
static int ui_reload_profiles(apad_server *server, const char *profiles_dir)
{
    profile_store_file  files[PROFILE_STORE_MAX_FILES];
    apad_profile_source sources[PROFILE_STORE_MAX_FILES];
    size_t count, i;
    int rc;

    count = profile_store_scan(profiles_dir, files, PROFILE_STORE_MAX_FILES);
    for (i = 0; i < count; i++) {
        sources[i].label = files[i].label;
        sources[i].name  = files[i].name;
        sources[i].text  = files[i].text;
    }
    rc = apad_server_reload_profiles(server, (count > 0) ? sources : NULL, count);
    profile_store_free_files(files, count);
    return rc == APAD_OK;
}

/* Handles one already-parsed request, `now_ms` is this connection's
 * accept-time clock (apad_ticks_ms(), same timebase as every other
 * apad_server_tick()/apad_server_on_datagram() call in this host).
 * `profiles_dir` is the SAME directory the host loaded profiles from at
 * startup (server/host/linux/main.c and server/host/windows/main.c both
 * thread it through from their own ATTICPAD_PROFILES_DIR-or-default logic)
 * -- the profile routes below read and write it directly, which is exactly
 * the filesystem access apadserver.h's library half refuses to do itself
 * (this file's own top comment: "webui/host code does filesystem, the
 * library only parses").  Every apad_server_* call here is a query or a
 * user-initiated action (apad_server_begin_pairing/_cancel_pairing/
 * _set_profile/_reload_profiles) -- exactly the "button press" apadserver.h's
 * pairing section describes a UI as being. */
static void ui_dispatch(apad_socket_t fd, apad_server *server, uint32_t now_ms,
                        uint16_t udp_port, const char *server_name,
                        const char *profiles_dir, const ui_mdns_status *mdns,
                        const ui_request *req)
{
    int is_get    = (strcmp(req->method, "GET") == 0);
    int is_post   = (strcmp(req->method, "POST") == 0);
    int is_put    = (strcmp(req->method, "PUT") == 0);
    int is_delete = (strcmp(req->method, "DELETE") == 0);

    if (!is_get && !is_post && !is_put && !is_delete) {
        ui_send_json_error(fd, 405, "Method Not Allowed",
                           "GET, POST, PUT or DELETE only");
        return;
    }

    if (is_get && strcmp(req->path, "/") == 0) {
        ui_send_response(fd, 200, "OK", "text/html; charset=utf-8",
                         ATTICPAD_INDEX_HTML, sizeof ATTICPAD_INDEX_HTML - 1u);
        return;
    }
    if (is_get && strcmp(req->path, "/editor") == 0) {
        /* Per-client remapping editor (assets_editor.h) -- bound to a slot
         * via ?slot=N, read client-side from location.search, same as every
         * other piece of state on this page (fetched at load time, never
         * server-templated). */
        ui_send_response(fd, 200, "OK", "text/html; charset=utf-8",
                         ATTICPAD_EDITOR_HTML, sizeof ATTICPAD_EDITOR_HTML - 1u);
        return;
    }
    if (is_get && strcmp(req->path, "/api/state") == 0) {
        ui_strbuf sb;
        sb_init(&sb);
        ui_build_state_json(&sb, server, udp_port, server_name, mdns);
        ui_send_json(fd, 200, "OK", &sb);
        sb_free(&sb);
        return;
    }
    if (is_get && strcmp(req->path, "/api/pair/qr.svg") == 0) {
        /* §10.3 QR: renders one of the SAME URIs /api/state's
         * "pairing.uris" array carries, computed fresh here rather than
         * cached from the last /api/state poll -- this route has no
         * session of its own, so "fresh" just means "as of
         * apad_server_pairing_state() right now", consistent with every
         * other query in this file. Returns 404 with no body once the
         * window is closed or was never opened: the secret-bearing SVG
         * must not be servable outside that window (§10.3), and 404 (not
         * empty-200) also makes a GET after the window closes fail loudly
         * for anything polling this URL directly (e.g. a stale <img> tag
         * left open in a second browser tab).
         *
         * ?addr=<ip> -- same query-string style as /api/pair/begin's
         * ?kind=pin|token -- picks WHICH currently-enumerated address to
         * encode; omitted or unrecognised falls back to
         * host_pick_default_addr()'s choice rather than erroring, so a
         * plain <img src="/api/pair/qr.svg"> (no query string at all)
         * keeps working exactly as before this address selector existed.
         * The value is matched against a FRESH host_enumerate_own_ipv4()
         * call, not accepted verbatim: this endpoint only ever encodes an
         * address this box actually holds, not an arbitrary caller-supplied
         * string (loopback-only or not, "build a URI naming any host" is
         * not a request this route has any reason to satisfy). */
        apad_pairing_info pinfo;
        host_own_addr     addrs[HOST_MAX_OWN_ADDRS];
        size_t            naddr, sel;
        char              want_addr[INET_ADDRSTRLEN];
        char              uri[QR_PAIR_URI_BUF];
        int               ulen;
        char             *svg;

        (void)apad_server_pairing_state(server, &pinfo);
        if (!pinfo.open) {
            ui_send_json_error(fd, 404, "Not Found",
                               "no pairing window is open");
            return;
        }
        naddr = host_enumerate_own_ipv4(addrs, HOST_MAX_OWN_ADDRS);
        if (naddr == 0u) {
            ui_send_json_error(fd, 404, "Not Found",
                               "no non-loopback IPv4 address to encode");
            return;
        }
        sel = host_pick_default_addr(addrs, naddr);
        if (ui_query_get(req->query, "addr", want_addr, sizeof want_addr)) {
            size_t i;
            for (i = 0; i < naddr; i++) {
                if (strcmp(addrs[i].ip, want_addr) == 0) {
                    sel = i;
                    break;
                }
            }
            /* No match (stale selection from a UI poll that raced an
             * interface change): silently falls back to the default picked
             * above rather than erroring -- an address list that changed
             * under the operator is not a client mistake. */
        }
        ulen = qr_build_pairing_uri(uri, addrs[sel].ip, udp_port,
                                    pinfo.secret);
        if (ulen <= 0) {
            ui_send_json_error(fd, 500, "Internal Server Error",
                               "could not build the pairing URI");
            return;
        }
        svg = qr_render_pairing_svg(uri);
        if (svg == NULL) {
            ui_send_json_error(fd, 500, "Internal Server Error",
                               "could not render the QR code");
            return;
        }
        ui_send_response(fd, 200, "OK", "image/svg+xml", svg, strlen(svg));
        free(svg);
        return;
    }
    if (is_post && strcmp(req->path, "/api/pair/begin") == 0) {
        int use_token = ui_query_wants_token(req->query);
        int rc = apad_server_begin_pairing(server, now_ms, use_token);
        if (rc == APAD_OK) {
            ui_send_ok(fd);
        } else {
            ui_send_json_error(fd, 409, "Conflict",
                               "pairing refused (see the server log -- no "
                               "on_random, or it failed)");
        }
        return;
    }
    if (is_post && strcmp(req->path, "/api/pair/cancel") == 0) {
        apad_server_cancel_pairing(server, now_ms);
        ui_send_ok(fd);
        return;
    }

    /* ---- remapping editor: profile routes -------------------------------
     * "/api/profiles" (plural, list) and "/api/profiles/reload" are checked
     * by strcmp() (exact match); "/api/profile/" (singular, trailing slash)
     * is checked by strncmp() prefix for the {name}-scoped routes below.
     * The two literals differ at byte 12 ('/' vs 's'), so neither prefix
     * check can accidentally match the other path -- see this file's own
     * development notes for the by-hand check that confirmed this once,
     * kept here as a comment instead of a runtime assertion since it is a
     * property of two string literals, not of any input. */
    if (is_get && strcmp(req->path, "/api/profiles") == 0) {
        const apad_profile *profiles[APAD_PROFILES_MAX];
        size_t n, i;
        ui_strbuf sb;

        n = apad_profiles_list(profiles, APAD_PROFILES_MAX);
        if (n > (size_t)APAD_PROFILES_MAX) {
            n = APAD_PROFILES_MAX;   /* defensive; never happens today */
        }
        sb_init(&sb);
        sb_append(&sb, "[");
        for (i = 0; i < n; i++) {
            const apad_profile *p = profiles[i];
            int builtin  = (p == apad_profiles_builtin_default());
            int editable = !builtin && !profile_store_is_shipped(p->name);

            if (i > 0) {
                sb_append(&sb, ",");
            }
            sb_append(&sb, "{\"name\":");
            sb_json_string(&sb, p->name);
            sb_append(&sb, ",\"match_device\":");
            sb_json_string(&sb, p->match_device);
            sb_appendf(&sb, ",\"builtin\":%s,\"editable\":%s}",
                      builtin ? "true" : "false", editable ? "true" : "false");
        }
        sb_append(&sb, "]");
        ui_send_json(fd, 200, "OK", &sb);
        sb_free(&sb);
        return;
    }
    if (is_post && strcmp(req->path, "/api/profiles/reload") == 0) {
        /* Picks up a hand-edited file with no restart -- server/src/
         * profiles.c's own comment on why apad_profiles_load() is safe to
         * call repeatedly (each call replaces the previously loaded set),
         * plus apad_server_reload_profiles()'s live session re-resolution
         * (apadserver.h) so a connected client's profile identity survives
         * the reload. */
        if (!ui_reload_profiles(server, profiles_dir)) {
            ui_send_json_error(fd, 500, "Internal Server Error",
                               "profile reload failed");
            return;
        }
        ui_send_ok(fd);
        return;
    }
    if (strncmp(req->path, "/api/profile/", 13) == 0
        && (is_get || is_put || is_delete)) {
        const char *name = req->path + 13;

        if (!profile_store_name_valid(name)) {
            ui_send_json_error(fd, 400, "Bad Request",
                               "invalid profile name");
            return;
        }

        if (is_get) {
            const apad_profile *p = apad_profiles_find(name);
            int builtin, editable;
            ui_strbuf sb;

            if (p == NULL) {
                ui_send_json_error(fd, 404, "Not Found", "no such profile");
                return;
            }
            builtin  = (p == apad_profiles_builtin_default());
            editable = !builtin && !profile_store_is_shipped(p->name);
            sb_init(&sb);
            profile_json_serialize(&sb, p, builtin, editable);
            ui_send_json(fd, 200, "OK", &sb);
            sb_free(&sb);
            return;
        }

        if (is_put) {
            /* Shipped profiles (server/profiles/, files named *.jsonc, the ones this repo
             * tracks) are read-only through this API -- editing them means
             * "duplicate & customize" first (a NEW name, this task's own
             * product decision), never overwriting the tracked file. Checked
             * before touching JSON at all: a caller sending garbage to a
             * shipped name should still get 409, not 400, so the response
             * says WHY the write is refused rather than merely that the
             * body was bad. */
            json_value *root, *reroot;
            char       *jsonc_text;
            char        err[128];

            if (profile_store_is_shipped(name)) {
                ui_send_json_error(fd, 409, "Conflict",
                                   "shipped profiles are read-only -- "
                                   "duplicate it under a new name first");
                return;
            }
            root = json_parse(req->body, err, sizeof err);
            if (root == NULL) {
                ui_send_json_error(fd, 400, "Bad Request",
                                   "malformed JSON body");
                return;
            }
            jsonc_text = profile_json_build_jsonc(root, name);
            json_free(root);
            if (jsonc_text == NULL) {
                ui_send_json_error(fd, 500, "Internal Server Error",
                                   "could not build the profile file");
                return;
            }
            /* Dry-run validate through the SAME parse path
             * apad_profiles_load() itself uses (json_parse() -- profiles.c's
             * build_profile() degrades every field individually and only
             * fails outright when the top-level value isn't even a JSON
             * object, so a syntax re-check here is the meaningful gate
             * before this hits disk). */
            reroot = json_parse(jsonc_text, err, sizeof err);
            if (reroot == NULL) {
                free(jsonc_text);
                ui_send_json_error(fd, 500, "Internal Server Error",
                                   "generated profile text failed to "
                                   "re-parse (this is a bug, not a bad "
                                   "request)");
                return;
            }
            json_free(reroot);

            if (!profile_store_save(profiles_dir, name, jsonc_text)) {
                free(jsonc_text);
                ui_send_json_error(fd, 500, "Internal Server Error",
                                   "could not write the profile file");
                return;
            }
            free(jsonc_text);
            if (!ui_reload_profiles(server, profiles_dir)) {
                ui_send_json_error(fd, 500, "Internal Server Error",
                                   "profile saved but reload failed");
                return;
            }
            ui_send_ok(fd);
            return;
        }

        /* is_delete */
        if (profile_store_is_shipped(name)) {
            ui_send_json_error(fd, 409, "Conflict",
                               "shipped profiles cannot be deleted");
            return;
        }
        if (!profile_store_delete(profiles_dir, name)) {
            ui_send_json_error(fd, 404, "Not Found", "no such profile file");
            return;
        }
        /* Best-effort: the file is already gone either way, and a session
         * that was on the deleted profile falls back to
         * apad_profiles_match() inside apad_server_reload_profiles() (its
         * own apadserver.h doc comment) -- a reload failure here (NULL
         * server, unreachable from this call site) has nothing left to roll
         * back. */
        (void)ui_reload_profiles(server, profiles_dir);
        ui_send_ok(fd);
        return;
    }

    if (is_post && strncmp(req->path, "/api/client/", 12) == 0) {
        const char *rest = req->path + 12;
        const char *slash = strchr(rest, '/');
        long        slot;
        char       *end;
        json_value *root;
        const json_value *pv;
        const char *name;
        char        err[64];
        int         rc;

        if (slash == NULL || strcmp(slash, "/profile") != 0) {
            ui_send_json_error(fd, 404, "Not Found", "unknown route");
            return;
        }
        slot = strtol(rest, &end, 10);
        if (end != slash || slot < 0 || slot >= (long)APAD_MAX_SESSIONS) {
            ui_send_json_error(fd, 400, "Bad Request", "bad slot number");
            return;
        }

        root = json_parse(req->body, err, sizeof err);
        if (root == NULL) {
            ui_send_json_error(fd, 400, "Bad Request",
                               "malformed JSON body, expected "
                               "{\"profile\":\"<name>\"}");
            return;
        }
        pv = json_obj_get(root, "profile");
        name = json_str(pv, NULL);
        if (name == NULL) {
            json_free(root);
            ui_send_json_error(fd, 400, "Bad Request",
                               "missing \"profile\" string field");
            return;
        }
        rc = apad_server_set_profile(server, (uint8_t)slot, name);
        json_free(root);
        switch (rc) {
        case APAD_OK:
            ui_send_ok(fd);
            break;
        case APAD_ERR_STATE:
            ui_send_json_error(fd, 409, "Conflict",
                               "no client connected on that slot");
            break;
        default:
            ui_send_json_error(fd, 400, "Bad Request",
                               "unknown profile name or invalid slot");
            break;
        }
        return;
    }
    if (is_get && strncmp(req->path, "/api/client/", 12) == 0) {
        const char *rest = req->path + 12;
        const char *slash = strchr(rest, '/');
        long        slot;
        char       *end;
        apad_input_state st;
        uint32_t    frame = 0u;
        int         rc, i;
        ui_strbuf   sb;

        if (slash == NULL || strcmp(slash, "/input") != 0) {
            ui_send_json_error(fd, 404, "Not Found", "unknown route");
            return;
        }
        slot = strtol(rest, &end, 10);
        if (end != slash || slot < 0 || slot >= (long)APAD_MAX_SESSIONS) {
            ui_send_json_error(fd, 400, "Bad Request", "bad slot number");
            return;
        }

        memset(&st, 0, sizeof st);
        rc = apad_server_last_input(server, (uint8_t)slot, &st, &frame);
        if (rc == APAD_ERR_ARG) {
            ui_send_json_error(fd, 404, "Not Found",
                               "no client connected on that slot");
            return;
        }
        /* rc == APAD_OK or APAD_ERR_STATE (connected, no INPUT_STATE yet --
         * apad_server_last_input() already zeroed `st` and `frame` in that
         * case, apadserver.h's own doc comment): both render the SAME JSON
         * shape. frame == 0 IS the "nothing yet" sentinel a poller checks --
         * see apad_server_last_input()'s doc comment -- so this route does
         * not need a distinct status for a state that is completely normal
         * immediately after a fresh HELLO and would otherwise make every
         * poller special-case the very first request.
         *
         * SIGN CONVENTIONS, deliberately mixed, matching the wire exactly
         * (docs/PROTOCOL.md §5.3/§5.4): lx/ly/rx/ry are the STICK convention,
         * +Y UP, straight out of apad_input_state::axes -- no sign flip here
         * or anywhere else in this file (that flip is uinput.c's, at the
         * OUTPUT edge, never in between). touch[].y is the TOUCH convention,
         * +Y DOWN (screen space), also unflipped. A consumer of this JSON
         * (the editor's stick visualiser and its touch-region canvas, both
         * in assets_editor.h) MUST apply the two conventions separately --
         * treating touch.y as stick-signed, or vice versa, silently mirrors
         * one of them. */
        sb_init(&sb);
        sb_appendf(&sb,
                  "{\"buttons\":%u,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,"
                  "\"lt\":%d,\"rt\":%d,\"touch\":[",
                  (unsigned)st.buttons,
                  (int)st.axes[APAD_AXIS_LX], (int)st.axes[APAD_AXIS_LY],
                  (int)st.axes[APAD_AXIS_RX], (int)st.axes[APAD_AXIS_RY],
                  (int)st.axes[APAD_AXIS_L2], (int)st.axes[APAD_AXIS_R2]);
        for (i = 0; i < (int)st.touch_count && i < APAD_TOUCH_MAX; i++) {
            if (i > 0) {
                sb_append(&sb, ",");
            }
            sb_appendf(&sb, "{\"id\":%u,\"pressure\":%u,\"x\":%d,\"y\":%d}",
                      (unsigned)st.touches[i].id,
                      (unsigned)st.touches[i].pressure,
                      (int)st.touches[i].x, (int)st.touches[i].y);
        }
        sb_appendf(&sb, "],\"frame\":%u}", (unsigned)frame);
        ui_send_json(fd, 200, "OK", &sb);
        sb_free(&sb);
        return;
    }

    ui_send_json_error(fd, 404, "Not Found", "unknown route");
}

/* True iff `host` (a request's Host header value, e.g. "127.0.0.1:21150" or
 * "localhost") names one of this server's own loopback addresses. The bind
 * itself (webui_open(), 127.0.0.1 only) already keeps a real non-loopback
 * PEER out -- the kernel never delivers anyone else's SYN here -- but DNS
 * rebinding does not need one: a page whose URL names an attacker-controlled
 * hostname can have that hostname's DNS record changed, after the page
 * loads, to point at 127.0.0.1, and a browser's own same-origin check is
 * keyed on the HOSTNAME, not the IP it resolved to -- so a script on that
 * page can still fetch() this API once the rebind lands, with a Host header
 * that says the attacker's hostname the whole time (protocol audit
 * finding). A hostname an attacker controls can never spell one of these
 * three literally, so refusing anything else closes that gap regardless of
 * what the TCP connection itself resolved through. Only the hostname
 * component is checked; a port, if present, is not -- it plays no part in
 * this defence. */
static int ui_host_is_loopback(const char *host)
{
    static const char *const names[] = { "127.0.0.1", "localhost", "[::1]" };
    size_t i;

    if (host == NULL || host[0] == '\0') {
        return 0;   /* HTTP/1.1 requires Host on every request; a request
                     * missing it is refused, not waved through. */
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t nlen = strlen(names[i]);
        if (apad_strncasecmp(host, names[i], nlen) == 0
            && (host[nlen] == '\0' || host[nlen] == ':')) {
            return 1;
        }
    }
    return 0;
}

/* Same defence for the Origin header on a STATE-CHANGING request (POST/PUT/
 * DELETE): a plain, form-shaped cross-origin POST needs no CORS preflight,
 * so a page open in the operator's browser -- anywhere, not just a rebound
 * hostname -- could fire one at /api/pair/begin or /api/client/{slot}/
 * profile and act as the operator with no warning (protocol audit finding).
 * Origin is "<scheme>://<host>[:port]" with no path; only the host
 * component is checked, same as ui_host_is_loopback() above, which this
 * reuses rather than duplicating.
 *
 * A request with NO Origin header is let through here (checked by the
 * caller, not this function) -- Origin is a header BROWSERS add to
 * cross-origin (and some same-origin) requests; curl, tools/, and this
 * project's own test harnesses never send one, and refusing its absence
 * would refuse exactly the loopback tooling this server has no reason to
 * distrust. GET/HEAD are not checked at all: this is CSRF-style protection
 * for a request that changes state, not a general access-control layer --
 * ui_host_is_loopback() above already covers every method. */
static int ui_origin_is_loopback(const char *origin)
{
    const char *host = strstr(origin, "://");
    if (host == NULL) {
        return 0;
    }
    return ui_host_is_loopback(host + 3);
}

/* Bind a TCP listener to 127.0.0.1:port ONLY -- never 0.0.0.0 (this task's
 * own constraint, and PROTOCOL.md §10's own admission that this system's
 * security model is already toy-grade is exactly why a second exposed
 * surface must not be added). Non-blocking so webui_poll()'s accept() never
 * stalls the UDP loop. Returns the socket, or APAD_INVALID_SOCKET (logged by
 * the caller, which then runs headless). */
static apad_socket_t webui_open(uint16_t port)
{
    apad_socket_t fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!apad_sock_valid(fd)) {
        return APAD_INVALID_SOCKET;
    }
    /* setsockopt()'s value is `const void *` on POSIX but `const char *` on
     * Winsock; the cast satisfies both and changes nothing on either. */
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&one, sizeof one);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 -- NEVER
                                                       * INADDR_ANY */
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        apad_sock_close(fd);
        return APAD_INVALID_SOCKET;
    }
    if (listen(fd, UI_LISTEN_BACKLOG) != 0) {
        apad_sock_close(fd);
        return APAD_INVALID_SOCKET;
    }
    if (apad_sock_set_nonblocking(fd) != 0) {
        apad_sock_close(fd);
        return APAD_INVALID_SOCKET;
    }
    return fd;
}

static void webui_close(apad_socket_t listen_fd)
{
    apad_sock_close(listen_fd);
}

/* Call once per main-loop iteration. Accepts at most one pending connection
 * (accept() is non-blocking; a listen backlog of UI_LISTEN_BACKLOG absorbs
 * any others until the next iteration ~20 ms later) and handles it fully,
 * synchronously, before returning. `now_ms` is passed straight through to
 * the apad_server_* calls the dispatch may make. */
static void webui_poll(apad_socket_t listen_fd, apad_server *server,
                       uint32_t now_ms, uint16_t udp_port,
                       const char *server_name, const char *profiles_dir,
                       const ui_mdns_status *mdns)
{
    apad_socket_t fd;
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof peer;
    ui_request req;

    if (!apad_sock_valid(listen_fd)) {
        return;
    }
    fd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
    if (!apad_sock_valid(fd)) {
        /* EAGAIN/EWOULDBLOCK (POSIX) or WSAEWOULDBLOCK (Winsock): nothing
         * pending, the common case. NEVER `fd < 0` -- SOCKET is unsigned on
         * Windows and that test silently never fires. */
        return;
    }

    /* Belt-and-suspenders on top of the bind itself (webui_open() only ever
     * binds 127.0.0.1): refuse anything that isn't actually loopback. The
     * bind is what actually keeps a non-loopback peer out -- the kernel
     * never delivers their SYN to this socket in the first place -- but
     * this makes the intent checkable in the code, not just in the bind
     * call far above. */
    if (peer.sin_family != AF_INET
        || ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
        apad_sock_close(fd);
        return;
    }

    /* MUST come before the timeouts below, which are meaningless on a
     * non-blocking socket -- and on Winsock this socket arrives non-blocking,
     * inherited from the listener. sockcompat.h explains what that looks like
     * when it is missed (every request reset, server reporting success). */
    (void)apad_sock_set_blocking(fd);

    /* Milliseconds, not a struct timeval: Winsock's SO_RCVTIMEO takes a bare
     * DWORD and quietly misreads a timeval-shaped one. sockcompat.h. */
    apad_sock_set_timeout_ms(fd, SO_RCVTIMEO, UI_IO_TIMEOUT_MS);
    apad_sock_set_timeout_ms(fd, SO_SNDTIMEO, UI_IO_TIMEOUT_MS);
    {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                         (const char *)&one, sizeof one);
    }

    {
        int rc = ui_read_request(fd, &req);
        int is_write = (strcmp(req.method, "POST") == 0
                        || strcmp(req.method, "PUT") == 0
                        || strcmp(req.method, "DELETE") == 0);

        if (rc == 0) {
            ui_send_json_error(fd, 400, "Bad Request",
                               "malformed, oversized, or incomplete request");
        } else if (rc < 0) {
            /* UI_REQUEST_DEADLINE_MS elapsed -- see that constant's comment
             * (protocol audit finding: a trickling client must not be able
             * to hold this single-threaded server's one connection slot
             * open indefinitely). */
            ui_send_json_error(fd, 408, "Request Timeout",
                               "request took too long");
        } else if (!ui_host_is_loopback(req.host)) {
            /* DNS-rebinding defence -- see ui_host_is_loopback()'s comment.
             * Applies to every method, not just writes: /api/state carries
             * the pairing secret while a window is open (protocol audit
             * finding), and that is a GET. */
            ui_send_json_error(fd, 403, "Forbidden",
                               "Host header does not name this PC");
        } else if (is_write && req.origin[0] != '\0'
                  && !ui_origin_is_loopback(req.origin)) {
            /* Cross-origin CSRF defence -- see ui_origin_is_loopback()'s
             * comment. Only for state-changing methods, and only when an
             * Origin header is actually present. */
            ui_send_json_error(fd, 403, "Forbidden",
                               "cross-origin request refused");
        } else {
            ui_dispatch(fd, server, now_ms, udp_port, server_name,
                       profiles_dir, mdns, &req);
        }
    }
    apad_sock_close(fd);
}

#endif /* ATTICPAD_HOST_COMMON_WEBUI_H */
