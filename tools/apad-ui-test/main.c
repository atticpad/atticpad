/* tools/apad-ui-test/main.c
 *
 * Host-side check for clients/common/apad_ui.c: proves the precedence order
 * documented in apad_ui.h's apad_ui_status_message() comment against every
 * branch it names, plus apad_ui_session_status() and apad_ui_selftest_title().
 * This tool is host code, not a client, so stdio is fine here even though
 * apad_ui.c itself may not use it (clients/common regime: C99, no stdio, no
 * floating point, no malloc after init).
 *
 * Links clients/common/apad_ui.c and apad_ui_strings.c only -- apad_ui.c
 * never calls into libapad, it only reads an apad_client_stats snapshot, so
 * there is nothing else to link.
 */

#include <stdio.h>
#include <string.h>

#include "apad_ui.h"

static int g_failed = 0;

static void check_id(const char *name, apad_msg_id got, apad_msg_id want)
{
    if (got != want) {
        g_failed++;
        printf("  FAIL  %-46s got=%d want=%d\n", name, (int)got, (int)want);
    } else {
        printf("  PASS  %-46s -> %d\n", name, (int)got);
    }
}

static void check_true(const char *name, int cond)
{
    if (!cond) {
        g_failed++;
        printf("  FAIL  %s\n", name);
    } else {
        printf("  PASS  %s\n", name);
    }
}

/* Zeroes a stats snapshot to a state that would not, by itself, satisfy any
 * precedence branch other than the one under test -- i.e. no accidental
 * error_code, auth_state or close_reason left set from a previous case. */
static apad_client_stats blank(int32_t state)
{
    apad_client_stats st;
    memset(&st, 0, sizeof st);
    st.state = state;
    st.rtt_ms = -1;
    st.pairing_required = -1;
    return st;
}

int main(void)
{
    apad_client_stats st;

    printf("apad-ui-test: apad_ui_status_message() precedence\n");

    /* 1a. every S6.11 error_code, CLOSED, maps to the documented id. */
    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_VERSION_MISMATCH;
    check_id("CLOSED error_code=VERSION_MISMATCH", apad_ui_status_message(&st), APAD_MSG_VERSION_MISMATCH);

    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_NO_FREE_SLOT;
    check_id("CLOSED error_code=NO_FREE_SLOT", apad_ui_status_message(&st), APAD_MSG_SERVER_FULL);

    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_AUTH_FAILED;
    check_id("CLOSED error_code=AUTH_FAILED", apad_ui_status_message(&st), APAD_MSG_WRONG_PIN);

    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_PAIRING_CLOSED;
    check_id("CLOSED error_code=PAIRING_CLOSED", apad_ui_status_message(&st), APAD_MSG_PAIRING_CLOSED);

    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_TOO_MANY_TRIES;
    check_id("CLOSED error_code=TOO_MANY_TRIES", apad_ui_status_message(&st), APAD_MSG_TOO_MANY_TRIES);

    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_MALFORMED;
    check_id("CLOSED error_code=MALFORMED", apad_ui_status_message(&st), APAD_MSG_CONNECTION_LOST);

    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_UNKNOWN_SESSION;
    check_id("CLOSED error_code=UNKNOWN_SESSION", apad_ui_status_message(&st), APAD_MSG_CONNECTION_LOST);

    /* An ERROR code from a newer server's vocabulary: preserved by the
     * engine (S6.0), rendered as the generic loss sentence -- never as a
     * clean "Disconnected". */
    st.error_code = 8;
    st.close_reason = APAD_CLOSE_LOCAL;
    check_id("CLOSED error_code=8 (unrecognised)", apad_ui_status_message(&st), APAD_MSG_CONNECTION_LOST);
    st.close_reason = 0;

    /* 1b. CLOSED + NEED_SECRET, no error_code. */
    st = blank(APAD_CLIENT_CLOSED);
    st.auth_state = APAD_AUTH_NEED_SECRET;
    check_id("CLOSED no error_code auth=NEED_SECRET", apad_ui_status_message(&st), APAD_MSG_NEED_PIN);

    /* 1c. CLOSED + FAILED auth, no error_code. */
    st = blank(APAD_CLIENT_CLOSED);
    st.auth_state = APAD_AUTH_FAILED;
    check_id("CLOSED no error_code auth=FAILED", apad_ui_status_message(&st), APAD_MSG_WRONG_PIN);

    /* 1d. CLOSED + close_reason, no error_code, no relevant auth_state. */
    st = blank(APAD_CLIENT_CLOSED);
    st.close_reason = APAD_CLOSE_LOCAL;
    check_id("CLOSED close_reason=LOCAL", apad_ui_status_message(&st), APAD_MSG_DISCONNECTED);

    st = blank(APAD_CLIENT_CLOSED);
    st.close_reason = APAD_CLOSE_PEER_BYE;
    check_id("CLOSED close_reason=PEER_BYE", apad_ui_status_message(&st), APAD_MSG_SERVER_CLOSED);

    st = blank(APAD_CLIENT_CLOSED);
    st.close_reason = APAD_CLOSE_IDLE_TIMEOUT;
    check_id("CLOSED close_reason=IDLE_TIMEOUT", apad_ui_status_message(&st), APAD_MSG_CONNECTION_LOST);

    st = blank(APAD_CLIENT_CLOSED);
    st.close_reason = APAD_CLOSE_RETX_FAILED;
    check_id("CLOSED close_reason=RETX_FAILED", apad_ui_status_message(&st), APAD_MSG_CONNECTION_LOST);

    st = blank(APAD_CLIENT_CLOSED);
    st.close_reason = APAD_CLOSE_PEER_ERROR;
    check_id("CLOSED close_reason=PEER_ERROR", apad_ui_status_message(&st), APAD_MSG_CONNECTION_LOST);

    st = blank(APAD_CLIENT_CLOSED);
    st.close_reason = APAD_CLOSE_NONE;
    check_id("CLOSED close_reason=NONE", apad_ui_status_message(&st), APAD_MSG_CONNECT_IDLE);

    /* 2/3. HANDSHAKING / ACTIVE ignore error_code, auth_state, close_reason. */
    st = blank(APAD_CLIENT_HANDSHAKING);
    check_id("HANDSHAKING", apad_ui_status_message(&st), APAD_MSG_CONNECTING);

    st = blank(APAD_CLIENT_ACTIVE);
    check_id("ACTIVE", apad_ui_status_message(&st), APAD_MSG_SESSION_ACTIVE);

    /* 4. IDLE. */
    st = blank(APAD_CLIENT_IDLE);
    st.auth_state = APAD_AUTH_NEED_SECRET;
    check_id("IDLE auth=NEED_SECRET", apad_ui_status_message(&st), APAD_MSG_NEED_PIN);

    st = blank(APAD_CLIENT_IDLE);
    check_id("IDLE auth=NONE", apad_ui_status_message(&st), APAD_MSG_CONNECT_IDLE);

    st = blank(APAD_CLIENT_IDLE);
    st.auth_state = APAD_AUTH_KEYED;
    check_id("IDLE auth=KEYED", apad_ui_status_message(&st), APAD_MSG_CONNECT_IDLE);

    /* Precedence: error_code beats auth_state beats close_reason, all three
     * set at once on a single CLOSED snapshot. */
    st = blank(APAD_CLIENT_CLOSED);
    st.error_code = APAD_ERRC_NO_FREE_SLOT;
    st.auth_state = APAD_AUTH_NEED_SECRET;
    st.close_reason = APAD_CLOSE_PEER_BYE;
    check_id("CLOSED error_code beats auth_state and close_reason",
             apad_ui_status_message(&st), APAD_MSG_SERVER_FULL);

    st = blank(APAD_CLIENT_CLOSED);
    st.auth_state = APAD_AUTH_NEED_SECRET;
    st.close_reason = APAD_CLOSE_PEER_BYE;
    check_id("CLOSED auth_state beats close_reason (no error_code)",
             apad_ui_status_message(&st), APAD_MSG_NEED_PIN);

    st = blank(APAD_CLIENT_CLOSED);
    st.auth_state = APAD_AUTH_FAILED;
    st.close_reason = APAD_CLOSE_PEER_BYE;
    check_id("CLOSED auth_state=FAILED beats close_reason (no error_code)",
             apad_ui_status_message(&st), APAD_MSG_WRONG_PIN);

    /* NULL st is tolerated, matching apad_client.c's own NULL convention. */
    check_id("NULL st", apad_ui_status_message(NULL), APAD_MSG_NONE);

    printf("\napad-ui-test: apad_ui_msg() / apad_ui_msg_count()\n");
    check_true("apad_ui_msg(-1) returns \"\" not NULL",
               apad_ui_msg((apad_msg_id)-1) != NULL &&
               apad_ui_msg((apad_msg_id)-1)[0] == '\0');
    check_true("apad_ui_msg(APAD_MSG_COUNT) returns \"\" not NULL",
               apad_ui_msg(APAD_MSG_COUNT) != NULL &&
               apad_ui_msg(APAD_MSG_COUNT)[0] == '\0');
    check_true("apad_ui_msg_count() == APAD_MSG_COUNT",
               apad_ui_msg_count() == (int)APAD_MSG_COUNT);

    printf("\napad-ui-test: apad_ui_session_status()\n");
    {
        apad_ui_session_line line;

        st = blank(APAD_CLIENT_ACTIVE);
        st.pad_slot = 2;
        st.input_rate_hz = 120;
        st.rtt_ms = -1;
        apad_ui_session_status(&st, &line);
        check_id("session line id while ACTIVE", line.id, APAD_MSG_SESSION_ACTIVE);
        check_true("session line pad_slot passthrough", line.pad_slot == 2);
        check_true("session line rate_hz passthrough", line.rate_hz == 120);
        check_true("session line rtt_ms=-1 passes through untouched (host substitutes)",
                   line.rtt_ms == -1);

        st = blank(APAD_CLIENT_ACTIVE);
        st.pad_slot = 1;
        st.input_rate_hz = 60;
        st.rtt_ms = 37;
        apad_ui_session_status(&st, &line);
        check_true("session line rtt_ms passthrough once measured", line.rtt_ms == 37);

        st = blank(APAD_CLIENT_CLOSED);
        st.close_reason = APAD_CLOSE_PEER_BYE;
        apad_ui_session_status(&st, &line);
        check_id("session line id defers to apad_ui_status_message() while CLOSED",
                 line.id, APAD_MSG_SERVER_CLOSED);

        /* NULL out must not crash; NULL st with a valid out zeroes the line. */
        apad_ui_session_status(&st, NULL);
        memset(&line, 0x7F, sizeof line);
        apad_ui_session_status(NULL, &line);
        check_id("session line id when st is NULL", line.id, APAD_MSG_NONE);
        check_true("session line pad_slot zeroed when st is NULL", line.pad_slot == 0);
        check_true("session line rate_hz zeroed when st is NULL", line.rate_hz == 0);
        check_true("session line rtt_ms zeroed when st is NULL", line.rtt_ms == 0);
    }

    printf("\napad-ui-test: apad_ui_selftest_title()\n");
    check_id("selftest_title(0)", apad_ui_selftest_title(0), APAD_MSG_SELFTEST_PASS);
    check_id("selftest_title(1)", apad_ui_selftest_title(1), APAD_MSG_SELFTEST_FAIL);
    check_id("selftest_title(nonzero != 1)", apad_ui_selftest_title(7), APAD_MSG_SELFTEST_FAIL);

    printf("\napad-ui-test: %d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
