package net.atticpad

/**
 * Hand-copied mirror of `clients/common/apad_ui_strings.h`'s `apad_msg_id`
 * enum. That header is the customer-facing message catalog shared by every
 * client; this object exists only because Kotlin cannot include a C header,
 * so the ids below have to be kept in step by hand instead.
 *
 * APPEND-ONLY, same rule as the C enum: add new ids at the end, before
 * [COUNT], and never renumber or remove one — see apad_ui_strings.h's ABI
 * RULE comment. [MainActivity]'s self-test compares [COUNT] against
 * `AtticPadNative.uiMsgCount()` every run specifically to catch the day this
 * file and the C enum go out of step silently.
 *
 * Use [msg] to render one of these — never hardcode the English here or
 * anywhere else in `net.atticpad`; the catalog in `clients/common` is the
 * one place customer-facing copy is written (apad_ui_strings.c's own header
 * comment).
 */
object Msg {
    const val NONE = 0 // "" — a screen that shows nothing

    // connect flow
    const val CONNECT_IDLE = 1 // "Not connected"
    const val CONNECTING = 2 // "Connecting..."
    const val SERVER_FOUND = 3 // "Found your PC on the network"
    const val SERVER_NOT_FOUND = 4 // "No PC answered - trying the last address"
    const val NEED_PIN = 5 // "Enter the PIN shown on your PC"
    const val WRONG_PIN = 6 // "That PIN didn't match - check the PC's screen and try again"
    const val PAIRING_CLOSED = 7 // "The PC isn't accepting new devices right now - ..."
    const val TOO_MANY_TRIES = 8 // "Too many tries - the PC now shows a new PIN"
    const val PAIRED_KEY_HELD = 9 // "Paired - ready to connect"
    const val SERVER_FULL = 10 // "All controller slots on the PC are taken"
    const val VERSION_MISMATCH = 11 // "The PC runs a different AtticPad version - update both"
    const val SERVER_CLOSED = 12 // "The PC ended the connection"
    const val CONNECTION_LOST = 13 // "Connection lost"
    const val DISCONNECTED = 14 // "Disconnected"

    // session
    const val SESSION_ACTIVE = 15 // "Connected"
    const val RTT_MEASURING = 16 // "Measuring..."

    // self-test
    const val SELFTEST_SUBTITLE = 17 // "Built-in health check"
    const val SELFTEST_RUNNING = 18 // "Checking..."
    const val SELFTEST_PASS = 19 // "All checks passed"
    const val SELFTEST_FAIL = 20 // "Health check failed"

    // fatal
    const val NET_UNAVAILABLE = 21 // "Couldn't start networking on this device"
    const val NET_UNAVAILABLE_HINT = 22 // "Close other apps that use the network, then try again"

    /** Must equal `apad_ui_msg_count()` — checked at self-test time. */
    const val COUNT = 23
}

/** The catalog string for [id] (`apad_ui_msg()`), e.g. `msg(Msg.NEED_PIN)`. */
fun msg(id: Int): String = AtticPadNative.uiMsg(id)
