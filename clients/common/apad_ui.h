/* apad_ui.h — turns an apad_client_stats snapshot into what a screen should
 * say. Shared for the same reason apad_client.c is: the 3DS and Android
 * clients each hand-rolled this precedence ("is CLOSED because of a wrong
 * PIN, a full server, or a dead link?") and the M5 clients (PSP, Switch, DS)
 * would have made five copies of it.
 *
 * Pure functions over the stats snapshot: no state, no allocation, no stdio,
 * no platform calls. Hosts render the returned ids with apad_ui_msg() in
 * whatever font and layout they own; numbers stay numbers so no formatting
 * is imposed here.
 */
#ifndef ATTICPAD_COMMON_APAD_UI_H
#define ATTICPAD_COMMON_APAD_UI_H

#include "apad_client.h"
#include "apad_ui_strings.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The one message a connect screen should show for this snapshot.
 *
 * Precedence, most specific reason first — a CLOSED session knows WHY it
 * closed, and that reason beats the bare state every time:
 *
 *  1. state == APAD_CLIENT_CLOSED:
 *       a. error_code (§6.11, the server said why):
 *            VERSION_MISMATCH -> APAD_MSG_VERSION_MISMATCH
 *            NO_FREE_SLOT     -> APAD_MSG_SERVER_FULL
 *            AUTH_FAILED      -> APAD_MSG_WRONG_PIN
 *            PAIRING_CLOSED   -> APAD_MSG_PAIRING_CLOSED
 *            TOO_MANY_TRIES   -> APAD_MSG_TOO_MANY_TRIES
 *            MALFORMED / UNKNOWN_SESSION -> APAD_MSG_CONNECTION_LOST
 *       b. else auth_state == APAD_AUTH_NEED_SECRET -> APAD_MSG_NEED_PIN
 *            (the §8 prompt-at-leisure lapse: the session closed precisely
 *            so the user can be asked for a PIN)
 *       c. else auth_state == APAD_AUTH_FAILED -> APAD_MSG_WRONG_PIN
 *       d. else close_reason:
 *            APAD_CLOSE_LOCAL        -> APAD_MSG_DISCONNECTED
 *            APAD_CLOSE_PEER_BYE     -> APAD_MSG_SERVER_CLOSED
 *            APAD_CLOSE_IDLE_TIMEOUT,
 *            APAD_CLOSE_RETX_FAILED,
 *            APAD_CLOSE_PEER_ERROR   -> APAD_MSG_CONNECTION_LOST
 *            APAD_CLOSE_NONE         -> APAD_MSG_CONNECT_IDLE
 *  2. state == APAD_CLIENT_HANDSHAKING -> APAD_MSG_CONNECTING
 *  3. state == APAD_CLIENT_ACTIVE      -> APAD_MSG_SESSION_ACTIVE
 *  4. state == APAD_CLIENT_IDLE:
 *       auth_state == APAD_AUTH_NEED_SECRET -> APAD_MSG_NEED_PIN
 *       (a probe learned pairing is required before any connect happened)
 *       else -> APAD_MSG_CONNECT_IDLE
 */
/* Trust note, decided deliberately: on an auth-required session the engine
 * records error_code even from an UNTAGGED (forgeable) ERROR -- it never
 * ACTS on one, but this function will render it, so an off-path forger on
 * an open network can put a wrong sentence on the screen (e.g. a fake
 * "too many tries"). The stats snapshot cannot distinguish tagged from
 * untagged, and gating on it would also suppress the legitimate wrong-PIN
 * flow, whose ERROR 3 arrives before any key exists. A wrong sentence is
 * the whole blast radius; accepted. */
apad_msg_id apad_ui_status_message(const apad_client_stats *st);

/* The session HUD line, as data. `rtt_ms` is -1 until the first PONG — show
 * apad_ui_msg(APAD_MSG_RTT_MEASURING) in its place, not "-1 ms". */
typedef struct {
    apad_msg_id id;          /* APAD_MSG_SESSION_ACTIVE while active, else
                              * apad_ui_status_message()'s answer            */
    int32_t     pad_slot;
    int32_t     rate_hz;
    int32_t     rtt_ms;
} apad_ui_session_line;

void apad_ui_session_status(const apad_client_stats *st,
                            apad_ui_session_line *out);

/* APAD_MSG_SELFTEST_PASS when `failed` is 0, APAD_MSG_SELFTEST_FAIL
 * otherwise — so no platform ever words this pair itself. */
apad_msg_id apad_ui_selftest_title(int failed);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_COMMON_APAD_UI_H */
