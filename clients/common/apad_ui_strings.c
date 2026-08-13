/* apad_ui_strings.c — the one place customer-facing copy is written. See the
 * header for the copy rules; every string here has been through them. */
#include "apad_ui_strings.h"

static const char *const msgs[APAD_MSG_COUNT] = {
    /* APAD_MSG_NONE */             "",

    /* APAD_MSG_CONNECT_IDLE */     "Not connected",
    /* APAD_MSG_CONNECTING */       "Connecting...",
    /* APAD_MSG_SERVER_FOUND */     "Found your PC on the network",
    /* APAD_MSG_SERVER_NOT_FOUND */ "No PC answered - trying the last address",
    /* APAD_MSG_NEED_PIN */         "Enter the PIN shown on your PC",
    /* APAD_MSG_WRONG_PIN */        "That PIN didn't match - check the PC's screen and try again",
    /* APAD_MSG_PAIRING_CLOSED */   "The PC isn't accepting new devices right now - start pairing on the PC first",
    /* APAD_MSG_TOO_MANY_TRIES */   "Too many tries - the PC now shows a new PIN",
    /* APAD_MSG_PAIRED_KEY_HELD */  "Paired - ready to connect",
    /* APAD_MSG_SERVER_FULL */      "All controller slots on the PC are taken",
    /* APAD_MSG_VERSION_MISMATCH */ "The PC runs a different AtticPad version - update both",
    /* APAD_MSG_SERVER_CLOSED */    "The PC ended the connection",
    /* APAD_MSG_CONNECTION_LOST */  "Connection lost",
    /* APAD_MSG_DISCONNECTED */     "Disconnected",

    /* APAD_MSG_SESSION_ACTIVE */   "Connected",
    /* APAD_MSG_RTT_MEASURING */    "Measuring...",

    /* APAD_MSG_SELFTEST_SUBTITLE */ "Built-in health check",
    /* APAD_MSG_SELFTEST_RUNNING */  "Checking...",
    /* APAD_MSG_SELFTEST_PASS */     "All checks passed",
    /* APAD_MSG_SELFTEST_FAIL */     "Health check failed",

    /* APAD_MSG_NET_UNAVAILABLE */      "Couldn't start networking on this device",
    /* APAD_MSG_NET_UNAVAILABLE_HINT */ "Close other apps that use the network, then try again",
};

const char *apad_ui_msg(apad_msg_id id)
{
    if ((int)id < 0 || id >= APAD_MSG_COUNT)
        return "";
    return msgs[id];
}

int apad_ui_msg_count(void)
{
    return APAD_MSG_COUNT;
}
