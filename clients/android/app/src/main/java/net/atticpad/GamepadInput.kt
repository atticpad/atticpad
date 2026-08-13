package net.atticpad

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent

/**
 * Physical gamepad passthrough (docs/DESIGN.md §7.2) — a controller plugged into or
 * paired with the phone drives the virtual pad on the PC.
 *
 * FACE BUTTON NAMING, which is the easy thing to get backwards. Android uses
 * the Xbox convention (`KEYCODE_BUTTON_A` is the BOTTOM button); the wire uses
 * the Nintendo convention (§5.1: "A is the right-hand face button", B bottom,
 * X top, Y left). The mapping below is therefore by PHYSICAL POSITION, and it
 * is the exact inverse of the server-side table in §5.4. Passing Android's A
 * through as the wire's A would rotate every face button by one position and
 * look, on the PC, like a mapping-profile bug.
 */
object GamepadInput {

    /** True for devices this app should treat as a gamepad. */
    fun isGamepad(device: InputDevice?): Boolean {
        if (device == null || device.isVirtual) return false
        val s = device.sources
        return (s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    }

    fun isGamepadEvent(event: KeyEvent): Boolean =
        (event.source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
            (event.source and InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD

    /** The §5.1 bit for an Android keycode, or 0 if it is not a pad button. */
    fun bitFor(keyCode: Int): Int = when (keyCode) {
        // By physical position — see the class comment.
        KeyEvent.KEYCODE_BUTTON_A -> AtticPadNative.BTN_B      // bottom
        KeyEvent.KEYCODE_BUTTON_B -> AtticPadNative.BTN_A      // right
        KeyEvent.KEYCODE_BUTTON_X -> AtticPadNative.BTN_Y      // left
        KeyEvent.KEYCODE_BUTTON_Y -> AtticPadNative.BTN_X      // top

        KeyEvent.KEYCODE_DPAD_UP -> AtticPadNative.BTN_DPAD_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> AtticPadNative.BTN_DPAD_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> AtticPadNative.BTN_DPAD_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> AtticPadNative.BTN_DPAD_RIGHT

        KeyEvent.KEYCODE_BUTTON_L1 -> AtticPadNative.BTN_L
        KeyEvent.KEYCODE_BUTTON_R1 -> AtticPadNative.BTN_R
        KeyEvent.KEYCODE_BUTTON_L2 -> AtticPadNative.BTN_ZL
        KeyEvent.KEYCODE_BUTTON_R2 -> AtticPadNative.BTN_ZR
        KeyEvent.KEYCODE_BUTTON_THUMBL -> AtticPadNative.BTN_L3
        KeyEvent.KEYCODE_BUTTON_THUMBR -> AtticPadNative.BTN_R3

        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> AtticPadNative.BTN_START
        KeyEvent.KEYCODE_BUTTON_SELECT -> AtticPadNative.BTN_SELECT
        KeyEvent.KEYCODE_BUTTON_MODE -> AtticPadNative.BTN_HOME
        else -> 0
    }

    private fun axis(event: MotionEvent, axis: Int): Float {
        val device = event.device ?: return event.getAxisValue(axis)
        val range = device.getMotionRange(axis, event.source) ?: return event.getAxisValue(axis)
        val value = event.getAxisValue(axis)
        // NO DEADZONE (§5.3). range.flat is exactly the deadzone the OS would
        // like applied here, and applying it would destroy information the
        // server's profile cannot get back.
        return value.coerceIn(range.min, range.max)
    }

    private fun toI16(v: Float): Int = (v * 32767f).toInt().coerceIn(-32768, 32767)

    private fun toTrigger(v: Float): Int = (v * 32767f).toInt().coerceIn(0, 32767)

    /**
     * Apply a joystick MotionEvent to [snapshot]. Returns the D-pad bits the
     * hat produced, which the caller must merge with the button bits it is
     * tracking from key events — a hat arrives as an axis, not as a key.
     */
    fun applyMotion(event: MotionEvent, snapshot: InputSnapshot): Int {
        // Sticks: Android is +Y DOWN (screen convention), the wire is +Y UP
        // (§5.3, XInput). Negate Y. This is the same inversion docs/DESIGN.md calls
        // out for PSP and Vita.
        val lx = toI16(axis(event, MotionEvent.AXIS_X))
        val ly = -toI16(axis(event, MotionEvent.AXIS_Y))
        val rx = toI16(axis(event, MotionEvent.AXIS_Z))
        val ry = -toI16(axis(event, MotionEvent.AXIS_RZ))
        snapshot.setSticks(InputSnapshot.SRC_PAD, lx, ly, rx, ry)

        // Triggers: AXIS_LTRIGGER/RTRIGGER on most pads, AXIS_BRAKE/GAS on
        // some (notably several Xbox and generic HID pads). Take whichever is
        // larger rather than guessing which device this is.
        val l2 = maxOf(
            toTrigger(axis(event, MotionEvent.AXIS_LTRIGGER)),
            toTrigger(axis(event, MotionEvent.AXIS_BRAKE)),
        )
        val r2 = maxOf(
            toTrigger(axis(event, MotionEvent.AXIS_RTRIGGER)),
            toTrigger(axis(event, MotionEvent.AXIS_GAS)),
        )
        snapshot.setTriggers(InputSnapshot.SRC_PAD, l2, r2)

        var dpad = 0
        val hx = axis(event, MotionEvent.AXIS_HAT_X)
        val hy = axis(event, MotionEvent.AXIS_HAT_Y)
        if (hx < -0.5f) dpad = dpad or AtticPadNative.BTN_DPAD_LEFT
        if (hx > 0.5f) dpad = dpad or AtticPadNative.BTN_DPAD_RIGHT
        // AXIS_HAT_Y is +1 DOWN, like the sticks.
        if (hy < -0.5f) dpad = dpad or AtticPadNative.BTN_DPAD_UP
        if (hy > 0.5f) dpad = dpad or AtticPadNative.BTN_DPAD_DOWN

        // §5.4: "A client with analog triggers SHOULD also set the digital
        // bits past a threshold, for profiles that want them as buttons."
        var extra = 0
        if (l2 > 16384) extra = extra or AtticPadNative.BTN_ZL
        if (r2 > 16384) extra = extra or AtticPadNative.BTN_ZR

        return dpad or extra
    }
}
