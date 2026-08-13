package net.atticpad

/**
 * The JNI boundary to `libapad`.
 *
 * Named by product, not by role (docs/DESIGN.md §7.2): `AtticPadNative`, not
 * `AtticPadClientNative`, and every entry point is namespaced `client*`. If
 * docs/DESIGN.md §6.4's Android-server proposal ever clears its SELinux spike it adds
 * `server*` here rather than renaming everything. There is no server stub in
 * this file and there must not be one before that spike.
 *
 * NOTHING in this app parses or builds a packet. The frozen v1 wire format and
 * the 193 conformance vectors only protect code that goes through libapad, so
 * the Kotlin side never sees a byte of the wire — it hands over an input
 * snapshot and reads back stats.
 *
 * Threading: every `client*` call must come from a single thread — the one
 * [AtticPadService] dedicates to the session. The native engine takes no lock.
 */
object AtticPadNative {

    init {
        System.loadLibrary("apadjni")
    }

    // ---- input snapshot layout, mirrored in cpp/apad_jni.c ---------------
    const val IN_BUTTONS = 0
    const val IN_AXIS0 = 1          // .. IN_AXIS0 + 7
    const val IN_TOUCH_COUNT = 9
    const val IN_TOUCH0 = 10        // id or (pressure shl 8), x, y
    const val IN_TOUCH1 = 13
    const val IN_ACCEL = 16         // x, y, z          — milli-g
    const val IN_GYRO = 19          // pitch, roll, yaw — deci-deg/s
    const val IN_BATTERY = 22
    const val IN_LEN = 23

    // ---- stats layout, mirrored in cpp/apad_jni.c ------------------------
    const val OUT_STATE = 0
    const val OUT_SESSION_ID = 1
    const val OUT_PAD_SLOT = 2
    const val OUT_RATE_HZ = 3
    const val OUT_RTT_MS = 4
    const val OUT_CLOSE_REASON = 5
    const val OUT_TX = 6
    const val OUT_RX = 7
    const val OUT_LAST_ERROR = 8
    const val OUT_RUMBLE_SERIAL = 9
    const val OUT_RUMBLE_LOW = 10
    const val OUT_RUMBLE_HIGH = 11
    const val OUT_RUMBLE_MS = 12
    const val OUT_LED_SERIAL = 13
    const val OUT_LED_PLAYER = 14
    const val OUT_LED_RGB = 15
    const val OUT_STATUS_SERIAL = 16
    const val OUT_STATUS_CODE = 17

    // §10 pairing. APPEND ONLY — these indices are hand-kept in step with
    // cpp/apad_jni.c and renumbering one silently repoints all of them.
    /** §6.2 `ANNOUNCE.pairing_required`, or -1 if no ANNOUNCE was seen. */
    const val OUT_PAIRING_REQUIRED = 18

    /** §6.4 `WELCOME.flags` bit 0, `AUTH_REQUIRED`. */
    const val OUT_AUTH_REQUIRED = 19

    /** One of the AUTH_* constants below. */
    const val OUT_AUTH_STATE = 20

    /** §6.11 `ERROR.code`, 0 if none. Kept apart from [OUT_STATUS_CODE]
     *  because STATUS codes are 0..2 and ERROR codes 1..7 — one field cannot
     *  tell "warning" from "no free pad slot". */
    const val OUT_ERROR_CODE = 21
    const val OUT_LEN = 22

    // enum apad_client_state
    const val STATE_IDLE = 0
    const val STATE_HANDSHAKING = 1
    const val STATE_ACTIVE = 2
    const val STATE_CLOSED = 3

    // enum apad_client_auth
    const val AUTH_NONE = 0
    const val AUTH_NEED_SECRET = 1
    const val AUTH_KEYED = 2
    const val AUTH_VERIFIED = 3
    const val AUTH_FAILED = 4

    // §6.11 ERROR codes (enum apad_error_code, core/include/atticpad/protocol.h).
    // All seven are named here now — the connect-screen precedence in
    // MainActivity.statusMsgId mirrors apad_ui_status_message()'s full
    // switch (clients/common/apad_ui.h/.c), which needs every one of them.
    const val ERRC_VERSION_MISMATCH = 1
    const val ERRC_NO_FREE_SLOT = 2
    const val ERRC_AUTH_FAILED = 3
    const val ERRC_PAIRING_CLOSED = 4
    const val ERRC_TOO_MANY_TRIES = 5
    const val ERRC_MALFORMED = 6
    const val ERRC_UNKNOWN_SESSION = 7

    // enum apad_session_close (core/include/atticpad/atticpad.h). Named here
    // for the same reason as the ERRC_* block above: statusMsgId's 1d step.
    const val CLOSE_NONE = 0
    const val CLOSE_LOCAL = 1
    const val CLOSE_PEER_BYE = 2
    const val CLOSE_IDLE_TIMEOUT = 3
    const val CLOSE_RETX_FAILED = 4
    const val CLOSE_PEER_ERROR = 5

    /** apad_result. Returned by [clientConnect] when a secret is needed. */
    const val ERR_AUTH = -8

    // enum apad_result — the subset §10.3 QR/URI parsing reports through rc[0].
    const val ERR_ARG = -1
    const val ERR_VERSION = -5     // "this server is newer than I am"
    const val ERR_STATE = -9       // no QR code in THIS frame — normal, keep scanning

    // §5.1 button bits. Nintendo naming: A is the RIGHT-hand face button.
    // Frozen at v1 — never renumber (docs/PROTOCOL.md §5.1).
    const val BTN_A = 1 shl 0
    const val BTN_B = 1 shl 1
    const val BTN_X = 1 shl 2
    const val BTN_Y = 1 shl 3
    const val BTN_DPAD_UP = 1 shl 4
    const val BTN_DPAD_DOWN = 1 shl 5
    const val BTN_DPAD_LEFT = 1 shl 6
    const val BTN_DPAD_RIGHT = 1 shl 7
    const val BTN_L = 1 shl 8
    const val BTN_R = 1 shl 9
    const val BTN_ZL = 1 shl 10
    const val BTN_ZR = 1 shl 11
    const val BTN_L3 = 1 shl 12
    const val BTN_R3 = 1 shl 13
    const val BTN_START = 1 shl 14
    const val BTN_SELECT = 1 shl 15
    const val BTN_HOME = 1 shl 16
    const val BTN_TOUCH_PRESS = 1 shl 17
    const val BTN_CAPTURE = 1 shl 19

    // §6.3 capability bits.
    const val CAP_DPAD = 1 shl 0
    const val CAP_FACE4 = 1 shl 1
    const val CAP_SHOULDER = 1 shl 2
    const val CAP_SHOULDER2 = 1 shl 3
    const val CAP_TRIGGERS = 1 shl 4
    const val CAP_STICK_L = 1 shl 5
    const val CAP_STICK_R = 1 shl 6
    const val CAP_TOUCH = 1 shl 7
    const val CAP_ACCEL = 1 shl 9
    const val CAP_GYRO = 1 shl 10
    const val CAP_RUMBLE = 1 shl 11
    const val CAP_BATTERY = 1 shl 13

    // Axis indices within the axes[8] block.
    const val AXIS_LX = 0
    const val AXIS_LY = 1           // +Y UP (§5.3) — Android reports +Y down
    const val AXIS_RX = 2
    const val AXIS_RY = 3           // +Y UP
    const val AXIS_L2 = 4           // 0..32767
    const val AXIS_R2 = 5

    /** libapad's product version string, e.g. "0.3.0-dev". */
    external fun version(): String

    /** The wire protocol version — 1, and frozen. Not the product version. */
    external fun protocolVersion(): Int

    external fun defaultPort(): Int

    /**
     * The shared customer-copy catalog (`clients/common/apad_ui_strings.h`,
     * `apad_msg_id`/`apad_ui_msg()`). [Msg] hand-mirrors the enum on this
     * side; [uiMsgCount] is what lets the self-test prove that mirror has
     * not drifted, since nothing else keeps the two in step.
     */
    external fun uiMsg(id: Int): String

    /** `apad_ui_msg_count()` — compared against [Msg.COUNT] at self-test time. */
    external fun uiMsgCount(): Int

    /**
     * Runs `apad_selftest_run()` — the codec invariants plus every vector in
     * `core/testdata/vectors.h` — on this device.
     *
     * @param counts a 3-element array filled with total, passed, failed.
     * @return the name of the first failing case, or null if all passed.
     */
    external fun selfTest(counts: IntArray): String?

    /** @return an opaque handle, or 0 on failure. */
    external fun clientCreate(deviceName: String, caps: Int): Long

    /**
     * §10 — hands the engine the pairing secret the user carried over out of
     * band. The secret crosses this boundary and stops: it is never written
     * to disk, never logged, and by §10 never goes on the wire.
     *
     * §10.1 makes the length depend on the channel — six digits when a human
     * typed it, twenty-odd characters when a camera did — so this takes a
     * String and not six digits. Pass null or "" to forget it. Survives
     * [clientConnect], which is what makes §8's "prompt at leisure and
     * reconnect" one call rather than a new session object.
     *
     * @return APAD_OK (0), or APAD_ERR_ARG (-1) if longer than 64 bytes.
     */
    external fun clientSetSecret(handle: Long, secret: String?): Int

    /**
     * §7/§8 — one unicast DISCOVER, so `pairing_required` is known BEFORE a
     * HELLO goes out. Blocking, up to [timeoutMs].
     *
     * Resets the session, so call it before [clientConnect] and never during
     * one. APAD_ERR_STATE (-9) means no ANNOUNCE came back, which is the
     * ordinary tier-3 outcome and not something to show the user.
     */
    external fun clientProbe(handle: Long, ip: String, port: Int, timeoutMs: Int): Int

    /**
     * @return APAD_OK (0) or a negative apad_result. Blocking.
     *
     * [ERR_AUTH] specifically means the WELCOME carried AUTH_REQUIRED and no
     * secret was set. By the time it returns the §9 ACK is on the wire and
     * the session has been let go (§8) — obtain a secret, call
     * [clientSetSecret], and call this again.
     */
    external fun clientConnect(
        handle: Long,
        ip: String,
        port: Int,
        rateHz: Int,
        timeoutMs: Int,
    ): Int

    /**
     * One iteration of the session loop. Blocks until the next INPUT_STATE is
     * due (at most [maxWaitMs]), handling anything that arrives meanwhile.
     *
     * @return one of the STATE_* constants.
     */
    external fun clientPump(handle: Long, input: IntArray, maxWaitMs: Int): Int

    external fun clientStats(handle: Long, out: IntArray)

    /** Text of the most recent STATUS or ERROR (§6.9, §6.11). */
    external fun clientMessage(handle: Long): String

    external fun clientDisconnect(handle: Long)

    external fun clientDestroy(handle: Long)

    // ---- §10.3 pairing URI / QR ------------------------------------------
    //
    // NOTHING here parses a URI or a pixel — see cpp/apad_qr.c and core's
    // apad_pair_uri_parse(). Both entry points below hand back the SAME
    // shape: a 3-element {ip, port, secret} array on success (rc[0] ==
    // APAD_OK), or null. `rc` must be a fresh single-element IntArray; it is
    // what distinguishes "not a pairing code" from "this server is newer
    // than I am" from (for the scanner only) "nothing in view yet".
    //
    // The returned secret is exactly as sensitive as a typed PIN
    // (docs/PROTOCOL.md §10.3): hand it straight to [clientSetSecret] and
    // hold it nowhere else — not a field, not a log, not an Intent extra.

    /**
     * Parses a §10.3 pairing URI obtained from a deep link (`Intent.data`)
     * or pasted text. `rc[0]` becomes one of [ERR_ARG] (not a conforming
     * pairing URI), [ERR_VERSION], or 0 (`APAD_OK`).
     */
    external fun pairUriParse(uri: String, rc: IntArray): Array<String>?

    /** @return an opaque quirc handle, or 0 on failure. One per in-app scan
     *  session — create when the scanner screen opens. */
    external fun qrCreate(): Long

    external fun qrDestroy(handle: Long)

    /**
     * Decodes one Camera2 `YUV_420_888` Y-plane frame. `rowStride` is
     * `Image.Plane.getRowStride()`, which commonly exceeds `width`; the
     * caller does not need to strip the padding itself.
     *
     * `rc[0]` becomes [ERR_STATE] on almost every call — no code was in view
     * in THIS frame, which is normal while the user is aiming the camera —
     * or [ERR_ARG], [ERR_VERSION], or 0 (`APAD_OK`).
     */
    external fun qrDecodeFrame(
        handle: Long,
        y: ByteArray,
        width: Int,
        height: Int,
        rowStride: Int,
        rc: IntArray,
    ): Array<String>?
}
