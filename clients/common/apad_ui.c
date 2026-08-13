/* apad_ui.c — see apad_ui.h for the precedence this file implements. Pure
 * functions over an apad_client_stats snapshot: no state, no allocation, no
 * stdio, no platform calls. Every id returned here is looked up through
 * apad_ui_msg(); no string is written in this file (see apad_ui_strings.c
 * for the copy rules that govern that).
 */
#include "apad_ui.h"

apad_msg_id apad_ui_status_message(const apad_client_stats *st)
{
    if (st == NULL) {
        return APAD_MSG_NONE;
    }

    if (st->state == APAD_CLIENT_CLOSED) {
        /* 1a. error_code, the server's own reason, beats everything else. */
        switch (st->error_code) {
        case APAD_ERRC_VERSION_MISMATCH:
            return APAD_MSG_VERSION_MISMATCH;
        case APAD_ERRC_NO_FREE_SLOT:
            return APAD_MSG_SERVER_FULL;
        case APAD_ERRC_AUTH_FAILED:
            return APAD_MSG_WRONG_PIN;
        case APAD_ERRC_PAIRING_CLOSED:
            return APAD_MSG_PAIRING_CLOSED;
        case APAD_ERRC_TOO_MANY_TRIES:
            return APAD_MSG_TOO_MANY_TRIES;
        case APAD_ERRC_MALFORMED:
        case APAD_ERRC_UNKNOWN_SESSION:
            return APAD_MSG_CONNECTION_LOST;
        default:
            if (st->error_code != 0) {
                /* An ERROR code this build does not know (a newer server's
                 * vocabulary). The engine preserves it verbatim for logs per
                 * S6.0; the one honest sentence for it is the generic one --
                 * never "Disconnected", which would claim the close was
                 * clean. */
                return APAD_MSG_CONNECTION_LOST;
            }
            /* 0 - no error_code from the server; fall through to 1b. */
            break;
        }

        /* 1b/1c. auth_state, when the server gave no error_code. */
        if (st->auth_state == APAD_AUTH_NEED_SECRET) {
            return APAD_MSG_NEED_PIN;
        }
        if (st->auth_state == APAD_AUTH_FAILED) {
            return APAD_MSG_WRONG_PIN;
        }

        /* 1d. close_reason, the least specific of the three. */
        switch (st->close_reason) {
        case APAD_CLOSE_LOCAL:
            return APAD_MSG_DISCONNECTED;
        case APAD_CLOSE_PEER_BYE:
            return APAD_MSG_SERVER_CLOSED;
        case APAD_CLOSE_IDLE_TIMEOUT:
        case APAD_CLOSE_RETX_FAILED:
        case APAD_CLOSE_PEER_ERROR:
            return APAD_MSG_CONNECTION_LOST;
        case APAD_CLOSE_NONE:
        default:
            return APAD_MSG_CONNECT_IDLE;
        }
    }

    if (st->state == APAD_CLIENT_HANDSHAKING) {
        return APAD_MSG_CONNECTING;
    }

    if (st->state == APAD_CLIENT_ACTIVE) {
        return APAD_MSG_SESSION_ACTIVE;
    }

    /* APAD_CLIENT_IDLE (and any value the FSM has no room for, treated the
     * same way apad_client_get_stats() treats an unrecognised session state:
     * as the idle default). */
    if (st->auth_state == APAD_AUTH_NEED_SECRET) {
        return APAD_MSG_NEED_PIN;
    }
    return APAD_MSG_CONNECT_IDLE;
}

void apad_ui_session_status(const apad_client_stats *st,
                            apad_ui_session_line *out)
{
    if (out == NULL) {
        return;
    }

    out->id       = APAD_MSG_NONE;
    out->pad_slot = 0;
    out->rate_hz  = 0;
    out->rtt_ms   = 0;

    if (st == NULL) {
        return;
    }

    out->pad_slot = st->pad_slot;
    out->rate_hz  = st->input_rate_hz;
    out->rtt_ms   = st->rtt_ms;
    out->id       = apad_ui_status_message(st);
}

apad_msg_id apad_ui_selftest_title(int failed)
{
    return failed ? APAD_MSG_SELFTEST_FAIL : APAD_MSG_SELFTEST_PASS;
}
