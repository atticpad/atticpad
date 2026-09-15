package net.atticpad

import android.content.Context

/**
 * The Bluetooth half of [SessionTransport] — a phone acting as a Bluetooth
 * HID gamepad/keyboard/mouse/media remote for a PC, no AtticPad server
 * involved. Owns its own [InputSnapshot]/[KbmSnapshot] (there is no
 * [AtticPadService] session to share one with) and the [BtHidController]
 * that drains them into HID reports — see that class's own doc for the
 * report descriptor and the boot-protocol reasoning, neither of which this
 * file touches.
 *
 * [MainActivity] constructs exactly one of these the first time the user
 * picks "Use Bluetooth" on the connect screen, and keeps it for the rest of
 * the Activity's life (a fresh [BtHidController.register] every time the
 * user revisits the Bluetooth setup panel would be wrong — see
 * [BtHidController.register]'s own idempotency note). It is never
 * constructed at all on a device where [BtHidController.isSupported] is
 * false, matching CAMERA's own "never unless a screen actually needs it"
 * rule this project already follows for runtime permissions.
 */
class BluetoothTransport(context: Context) : SessionTransport {
    override val input = InputSnapshot()
    override val kbm = KbmSnapshot()
    val controller = BtHidController(context, input, kbm)
}
