/* apad_ui_strings.h — the customer-facing message catalog, shared by every
 * client. One id per thing a screen can say; one English string per id,
 * written once, in clients/common, so eight platforms speak with one voice.
 *
 * COPY RULES — these are the point of this file, enforce them in review:
 *   - No spec references on a screen. "§10", "tier 2", "S7" mean nothing to
 *     a person holding a 3DS. That vocabulary lives in logs.
 *   - No function names, file paths, or doc references. If a detail helps a
 *     bug report, log it; the screen gets the sentence a customer can act on.
 *   - "the PC", not "the server". Customers run a program on their PC; they
 *     did not install "a server".
 *   - Strings are plain sentences with no printf specifiers. Hosts append
 *     numbers themselves ("Connecting to 192.168.1.7..." is host-composed).
 *   - ASCII only: the smallest client font this must survive is the 3DS's.
 *
 * ABI RULE: the enum is APPEND-ONLY. Android mirrors these values by hand in
 * Kotlin (object Msg) and checks apad_ui_msg_count() at self-test time, so
 * reordering or removing a value is a cross-language break. Add at the end,
 * before APAD_MSG_COUNT.
 *
 * Lives in clients/common (may not malloc after init, no stdio, C99) — same
 * regime as apad_client.c, usable from a screen callback on any platform.
 */
#ifndef ATTICPAD_COMMON_APAD_UI_STRINGS_H
#define ATTICPAD_COMMON_APAD_UI_STRINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APAD_MSG_NONE = 0,             /* "" — a screen that shows nothing       */

    /* connect flow */
    APAD_MSG_CONNECT_IDLE,         /* "Not connected"                        */
    APAD_MSG_CONNECTING,           /* "Connecting..."                        */
    APAD_MSG_SERVER_FOUND,         /* "Found your PC on the network"         */
    APAD_MSG_SERVER_NOT_FOUND,     /* "No PC answered - trying the last
                                    *  address"                              */
    APAD_MSG_NEED_PIN,             /* "Enter the PIN shown on your PC"       */
    APAD_MSG_WRONG_PIN,            /* "That PIN didn't match - check the
                                    *  PC's screen and try again"            */
    APAD_MSG_PAIRING_CLOSED,       /* "The PC isn't accepting new devices
                                    *  right now - start pairing on the PC
                                    *  first"                                */
    APAD_MSG_TOO_MANY_TRIES,       /* "Too many tries - the PC now shows a
                                    *  new PIN"                              */
    APAD_MSG_PAIRED_KEY_HELD,      /* "Paired - ready to connect"            */
    APAD_MSG_SERVER_FULL,          /* "All controller slots on the PC are
                                    *  taken"                                */
    APAD_MSG_VERSION_MISMATCH,     /* "The PC runs a different AtticPad
                                    *  version - update both"                */
    APAD_MSG_SERVER_CLOSED,        /* "The PC ended the connection"          */
    APAD_MSG_CONNECTION_LOST,      /* "Connection lost"                      */
    APAD_MSG_DISCONNECTED,         /* "Disconnected"                         */

    /* session */
    APAD_MSG_SESSION_ACTIVE,       /* "Connected"                            */
    APAD_MSG_RTT_MEASURING,        /* "Measuring..."                         */

    /* self-test */
    APAD_MSG_SELFTEST_SUBTITLE,    /* "Built-in health check"                */
    APAD_MSG_SELFTEST_RUNNING,     /* "Checking..."                          */
    APAD_MSG_SELFTEST_PASS,        /* "All checks passed"                    */
    APAD_MSG_SELFTEST_FAIL,        /* "Health check failed"                  */

    /* fatal */
    APAD_MSG_NET_UNAVAILABLE,      /* "Couldn't start networking on this
                                    *  device"                               */
    APAD_MSG_NET_UNAVAILABLE_HINT, /* "Close other apps that use the
                                    *  network, then try again"              */

    APAD_MSG_COUNT
} apad_msg_id;

/* The string for `id`. Never NULL: out-of-range ids return "". */
const char *apad_ui_msg(apad_msg_id id);

/* APAD_MSG_COUNT, as a function, so the Android JNI mirror can verify its
 * hand-copied Kotlin constants against the compiled table at self-test time
 * instead of drifting silently. */
int apad_ui_msg_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_COMMON_APAD_UI_STRINGS_H */
