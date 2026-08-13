package net.atticpad

/**
 * The one place input from every source is merged, and the only object shared
 * between the UI thread and the session thread.
 *
 * Three producers write here — the touch overlay, a physical gamepad, and the
 * sensors — all on the UI/main thread. One consumer reads, on
 * [AtticPadService]'s session thread. That is the whole concurrency story, and
 * it is `synchronized` rather than anything cleverer because the critical
 * section is a 23-int array copy at 60 Hz.
 *
 * Sources are kept SEPARATE and merged at read time rather than written into
 * one shared array. If the overlay and a physical pad both wrote `axes[0]`,
 * whichever moved last would win, and letting go of a physical stick would
 * clobber a touch stick that was still held.
 *
 * NO DEADZONE IS APPLIED HERE, or anywhere else on this side of the wire —
 * docs/PROTOCOL.md §5.3: "A client MUST NOT apply a deadzone. Raw normalised
 * values go on the wire. The server profile owns deadzone, curve and
 * inversion. A client that pre-applies a deadzone destroys information the
 * server cannot recover."
 */
class InputSnapshot {

    companion object {
        const val SRC_TOUCH = 0
        const val SRC_PAD = 1
        private const val SRC_COUNT = 2
        private const val AXES = 6      // LX LY RX RY L2 R2; 6..7 are reserved
    }

    private val lock = Any()

    private val buttons = IntArray(SRC_COUNT)

    /**
     * Every button bit SEEN since the last [readInto], whether or not it is
     * still held.
     *
     * Without this a fast tap is silently dropped. INPUT_STATE goes out at
     * 60 Hz, so the session thread samples this object every ~16 ms; a
     * touchscreen tap whose down and up both land inside one of those windows
     * is pressed and released without a single frame ever carrying the bit.
     * Caught on the emulator, where `input tap` delivers down+up ~5 ms apart
     * and buttons registered only when the tap happened to straddle a sample
     * boundary. A deliberate human tap is quicker than 16 ms often enough to
     * matter, and the symptom — "sometimes the button just doesn't work" —
     * would read as packet loss.
     *
     * Latching guarantees every press occupies at least one transmitted
     * frame. Analog axes deliberately do NOT latch: for them the newest
     * value is the truth, and holding a stale deflection for a frame would be
     * a lie about where the stick is.
     */
    private val buttonsSeen = IntArray(SRC_COUNT)

    private val axes = Array(SRC_COUNT) { IntArray(AXES) }

    // §5.2 raw contacts, from the overlay's touchpad region only.
    private var touchCount = 0
    private val touchId = IntArray(2)
    private val touchPressure = IntArray(2)
    private val touchX = IntArray(2)
    private val touchY = IntArray(2)

    private val accel = IntArray(3)
    private val gyro = IntArray(3)
    private var battery = 255       // APAD_BATTERY_UNKNOWN until told otherwise

    fun setButtons(source: Int, mask: Int) = synchronized(lock) {
        buttons[source] = mask
        buttonsSeen[source] = buttonsSeen[source] or mask
    }

    fun setAxis(source: Int, index: Int, value: Int) = synchronized(lock) {
        if (index in 0 until AXES) axes[source][index] = value
    }

    fun setSticks(source: Int, lx: Int, ly: Int, rx: Int, ry: Int) = synchronized(lock) {
        axes[source][AtticPadNative.AXIS_LX] = lx
        axes[source][AtticPadNative.AXIS_LY] = ly
        axes[source][AtticPadNative.AXIS_RX] = rx
        axes[source][AtticPadNative.AXIS_RY] = ry
    }

    fun setTriggers(source: Int, l2: Int, r2: Int) = synchronized(lock) {
        axes[source][AtticPadNative.AXIS_L2] = l2
        axes[source][AtticPadNative.AXIS_R2] = r2
    }

    /** Clears one source completely — used when a gamepad disconnects. */
    fun clearSource(source: Int) = synchronized(lock) {
        buttons[source] = 0
        buttonsSeen[source] = 0
        axes[source].fill(0)
    }

    /**
     * @param n number of live contacts, 0..2 (§5 clamps above 2 anyway).
     * @param y screen space, +Y DOWN (§5.3) — the opposite of the sticks, on
     *          purpose: sticks are vector space, touch is screen space.
     */
    fun setTouches(
        n: Int,
        id0: Int, p0: Int, x0: Int, y0: Int,
        id1: Int, p1: Int, x1: Int, y1: Int,
    ) = synchronized(lock) {
        touchCount = n.coerceIn(0, 2)
        touchId[0] = id0; touchPressure[0] = p0; touchX[0] = x0; touchY[0] = y0
        touchId[1] = id1; touchPressure[1] = p1; touchX[1] = x1; touchY[1] = y1
    }

    fun setAccel(x: Int, y: Int, z: Int) = synchronized(lock) {
        accel[0] = x; accel[1] = y; accel[2] = z
    }

    fun setGyro(pitch: Int, roll: Int, yaw: Int) = synchronized(lock) {
        gyro[0] = pitch; gyro[1] = roll; gyro[2] = yaw
    }

    fun setBattery(percent: Int) = synchronized(lock) {
        battery = percent
    }

    /**
     * Merge every source into [out], which must be [AtticPadNative.IN_LEN]
     * long. Called only from the session thread.
     */
    fun readInto(out: IntArray) = synchronized(lock) {
        out[AtticPadNative.IN_BUTTONS] =
            buttons[SRC_TOUCH] or buttons[SRC_PAD] or
                buttonsSeen[SRC_TOUCH] or buttonsSeen[SRC_PAD]
        // Consumed: a bit that is still held is re-set by the next
        // setButtons, and one that was released has now had its frame.
        buttonsSeen[SRC_TOUCH] = 0
        buttonsSeen[SRC_PAD] = 0

        for (i in 0 until AXES) {
            val t = axes[SRC_TOUCH][i]
            val p = axes[SRC_PAD][i]
            // Larger absolute deflection wins, so a physical stick at rest
            // never cancels a touch stick being held and vice versa.
            out[AtticPadNative.IN_AXIS0 + i] =
                if (kotlin.math.abs(p) >= kotlin.math.abs(t)) p else t
        }
        // Indices 6..7 are reserved and MUST be zero (§5).
        out[AtticPadNative.IN_AXIS0 + 6] = 0
        out[AtticPadNative.IN_AXIS0 + 7] = 0

        out[AtticPadNative.IN_TOUCH_COUNT] = touchCount
        out[AtticPadNative.IN_TOUCH0] = (touchId[0] and 0xFF) or ((touchPressure[0] and 0xFF) shl 8)
        out[AtticPadNative.IN_TOUCH0 + 1] = touchX[0]
        out[AtticPadNative.IN_TOUCH0 + 2] = touchY[0]
        out[AtticPadNative.IN_TOUCH1] = (touchId[1] and 0xFF) or ((touchPressure[1] and 0xFF) shl 8)
        out[AtticPadNative.IN_TOUCH1 + 1] = touchX[1]
        out[AtticPadNative.IN_TOUCH1 + 2] = touchY[1]

        out[AtticPadNative.IN_ACCEL] = accel[0]
        out[AtticPadNative.IN_ACCEL + 1] = accel[1]
        out[AtticPadNative.IN_ACCEL + 2] = accel[2]
        out[AtticPadNative.IN_GYRO] = gyro[0]
        out[AtticPadNative.IN_GYRO + 1] = gyro[1]
        out[AtticPadNative.IN_GYRO + 2] = gyro[2]

        out[AtticPadNative.IN_BATTERY] = battery
    }
}
