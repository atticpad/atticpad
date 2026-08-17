package net.atticpad

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothHidDevice
import android.bluetooth.BluetoothHidDeviceAppQosSettings
import android.bluetooth.BluetoothHidDeviceAppSdpSettings
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.concurrent.Executors

/**
 * BLUETOOTH CONTROLLER MODE — the phone becomes a Bluetooth HID
 * *peripheral* itself, so it can drive a gamepad on any PC or console that
 * accepts Bluetooth game controllers, with no AtticPad server involved at
 * all. docs/DESIGN.md §6.4 dismisses `BluetoothHidDevice` as "not this product" in
 * the SERVER direction (phone hosting pads for other clients) — this is the
 * opposite direction. Started life as a deliberately time-boxed spike, to
 * answer three questions no amount of
 * reading answers: does `registerApp` succeed on real hardware (a vendor
 * Bluetooth stack can compile the HID Device *role* out — see the
 * `ralismark/bluehid` proof-of-concept, which had to ship a Xposed module
 * just to flip `profile_supported_hidd` back on for its test device); does
 * a paired PC actually enumerate a working DirectInput/SDL gamepad from the
 * report descriptor below; and does anything come back on a SET_REPORT. All
 * three came back answered well enough — see the "repeated e" section below
 * for the one real bug the hardware round found and fixed — to promote this
 * from a spike to a first-class mode, 2026-08-12. [BtControllerActivity] is
 * the guided setup UI built over this same engine; nothing in this class
 * changed for the promotion.
 *
 * [REPORT_DESCRIPTOR] is frozen exactly as the hardware round proved it
 * working — changing it by even a byte re-opens every question above.
 * It has moved exactly once since: on 2026-08-16 the four stick/trigger
 * USAGE bytes were swapped from the DualShock layout (right stick on Z/Rz,
 * triggers on Rx/Ry) to the Xbox one (right stick on Rx/Ry, triggers on
 * Z/Rz), because a real Windows host read our trigger axes as stick axes:
 * pulling the left trigger moved a stick Y axis, while the right stick's
 * up/down landed on an axis nothing was bound to. No structural byte moved:
 * same items, same order, same lengths, same nine-byte report, so
 * [buildReport] is unchanged. Enumeration was re-verified on hardware after
 * the change.
 * Rumble/LED are explicitly OUT OF SCOPE for this mode (see the "rumble"
 * note on [callback] below): the descriptor declares no OUTPUT report item
 * to receive them, and adding one is a deliberately deferred product
 * decision, not an oversight.
 *
 * NOT a second protocol implementation. `libapad`/[InputSnapshot] already
 * did the only hard part — merging touch, physical-pad and (nowhere near
 * here) sensor input into the wire's §5.1 button layout and §5 axis
 * layout — and this class only re-encodes that *already-decoded* snapshot
 * into a completely different wire (Bluetooth HID reports, not AtticPad's
 * UDP datagrams). Nothing here parses or builds an AtticPad packet, so it
 * does not need to go through `libapad`/the shim the way docs/CONVENTIONS.md's core
 * rule means for *that* protocol.
 *
 * THREADING: [startStreaming] spins up its own thread
 * ("atticpad-bthid-stream") the moment a host connects, distinct from
 * [AtticPadService]'s "atticpad-session" thread — the two never run at once
 * in this build ([MainActivity]'s UDP session and
 * [BtControllerActivity]'s Bluetooth session are two different screens),
 * but keeping them structurally separate costs nothing and avoids a
 * shared-state bug later.
 *
 * ---- the "repeated e" finding (2026-08-11 hardware round) ----
 *
 * Real hardware (an arm64 handset -> Windows 11) got as far as
 * "registered - connected - streaming", then the host read every report as
 * a boot-protocol KEYBOARD, not a gamepad, and spammed the letter 'e'. Root
 * cause, traced through AOSP `packages/modules/Bluetooth` source (same
 * discipline as the descriptor-format trap above — no search-engine summary
 * got this right either):
 *
 * 1. `system/stack/hid/hidd_api.cc`'s `HID_DevAddRecord` writes SDP
 *    attribute `ATTR_ID_HID_BOOT_DEVICE = true` from a hardcoded
 *    `bool_true` for EVERY app, regardless of the `subclass` byte
 *    [BluetoothHidDeviceAppSdpSettings] was given — there is no parameter
 *    on the public API that reaches this attribute. A "gamepad" subclass
 *    (0x02, what [registerApp] already sends — see its own comment) does
 *    NOT stop this device from advertising boot-keyboard/boot-mouse
 *    capability it doesn't actually have reports for.
 * 2. `system/bta/hd/bta_hd_act.cc`'s `bta_hd_set_protocol_act()` accepts a
 *    host's SET_PROTOCOL unconditionally — no reject/NAK path exists — so
 *    a host that decides (because of #1) to try boot mode always gets it.
 * 3. Once in boot mode, this app kept calling `sendReport` with its normal
 *    9-byte gamepad report. A boot-protocol KEYBOARD report is 8 bytes:
 *    modifier, reserved, then 6 keycode slots. Our centred hat-switch
 *    nibble (null = 8, [HAT_LUT]) landed inside that keycode window, and
 *    USB HID keyboard usage 0x08 IS the letter 'e' — a static, continuously
 *    resent value that scans as a held-down 'e' the whole time the hat
 *    stayed centred, i.e. always, since nothing here ever produced a
 *    boot-shaped report on purpose.
 *
 * Fix applied (see [REPORT_DESCRIPTOR], [REPORT_ID], and the
 * `onSetProtocol`/[bootProtocolActive] pair below): #1 cannot be changed
 * from app code (confirmed — it is unconditional in AOSP, not derived from
 * subclass, so trying `SUBCLASS1_NONE`-alone or other subclass bytes was
 * evaluated and rejected as not reaching the actual attribute at all); #2
 * likewise cannot be refused by the app. The only lever this app has is
 * AFTER the host chooses boot mode: stop sending the report-protocol
 * report while boot mode is active, since this app has no boot-shaped
 * report to send either way — believed to be THE fix. Dropping the unused
 * Report ID item (single-report device, [REPORT_ID] now 0) is a real,
 * independent simplification evaluated at the same time, but it changes
 * only report-protocol framing and would not by itself have stopped the
 * boot-protocol misread, so if a hardware round shows the fix working,
 * attribute it to the `onSetProtocol` gate, not the Report ID removal.
 */
class BtHidController(private val context: Context, private val input: InputSnapshot) {

    companion object {
        private const val TAG = "BtHidController"

        /** Only added at API 28 (Android 9) — see [isSupported]. */
        val MIN_SDK = Build.VERSION_CODES.P

        fun isSupported(): Boolean = Build.VERSION.SDK_INT >= MIN_SDK

        // ---- HID report descriptor -------------------------------------
        //
        // Verified against Google's OWN `BluetoothHidDevice` conformance
        // test (AOSP `CtsVerifier`, `bluetooth/HidConstants.java`,
        // `HIDD_REPORT_DESC`) that the `descriptors` byte array
        // `BluetoothHidDeviceAppSdpSettings` wants is a bare USB HID
        // *Report* Descriptor (HID 1.11 §6.2.2) — starting at the first
        // `USAGE_PAGE` item and ending at the outermost `END_COLLECTION` —
        // with NO leading 9-byte "HID Descriptor" (§6.2.1) wrapper, even
        // though that wrapper is what a literal reading of the class
        // javadoc's citation ("HID1_11.pdf Chapter 6") could suggest. An
        // open-source proof-of-concept (`ralismark/bluehid`) prepends that
        // 9-byte header anyway; CTS Verifier — code Google runs against its
        // own `BluetoothHidDevice` implementation to certify it — does not,
        // so that is the version trusted here.
        //
        // NO Report ID (was Report ID 1 — see the "repeated e" incident
        // report and [REPORT_ID] below for why this changed). Single
        // report, 9 bytes total:
        //   byte 0    buttons 1..8   (bit0 = button 1)
        //   byte 1    buttons 9..13  (bit0..4), bits 5..7 constant 0
        //   byte 2    hat switch, low nibble (0=N..7=NW, 8=null); high
        //             nibble constant 0
        //   byte 3    X  (left stick,  i8, signed, centre 0)
        //   byte 4    Y  (left stick,  i8, signed, centre 0, +DOWN — see
        //             [buildReport])
        //   byte 5    Rx (right stick X, i8, signed, centre 0)
        //   byte 6    Ry (right stick Y, i8, signed, centre 0, +DOWN)
        //   byte 7    Z  (left trigger,  u8, 0=released .. 255=full pull)
        //   byte 8    Rz (right trigger, u8, as above)
        //
        // The BYTE positions are unchanged from the original descriptor;
        // only which USAGE each one is declared as moved (Z/Rz <-> Rx/Ry),
        // so buildReport() below still fills them in the same order.
        //
        // 13 buttons, not 16: the wire's §5.1 mask has 20 defined bits, but
        // 4 of them (the D-pad) become the hat switch instead, and
        // TOUCH_PRESS/TOUCH_REAR_PRESS/CAPTURE (bits 17/18/19) have no
        // meaning to a HID gamepad tester and are dropped rather than
        // padded in as dead buttons nobody could ever press. See
        // [BUTTON_BITS] for the exact wire-bit -> HID-button-number order.
        val REPORT_DESCRIPTOR: ByteArray = byteArrayOf(
            0x05, 0x01,                 // Usage Page (Generic Desktop)
            0x09, 0x05,                 // Usage (Game Pad)
            0xA1.toByte(), 0x01,        // Collection (Application)
            // No Report ID item (0x85) — this app registers exactly one
            // report, and HID 1.11 §8.3 only requires a Report ID when a
            // device multiplexes more than one report shape onto the same
            // endpoint (CTS Verifier's own HIDD_REPORT_DESC uses one
            // precisely because IT declares two: ID_KEYBOARD and ID_MOUSE
            // back to back in the same descriptor — not evidence that a
            // single-report device needs one too). Confirmed from AOSP
            // `system/bta/hd/bta_hd_act.cc`'s `check_descriptor()`, which
            // scans this exact byte array for a literal `0x85` and flips
            // `bta_hd_cb.use_report_id` off when it finds none — so this
            // change is not cosmetic, it changes what
            // `bta_hd_send_report_act()` puts on the wire (see [REPORT_ID]).

            0x05, 0x09,                 //   Usage Page (Button)
            0x19, 0x01,                 //   Usage Minimum (Button 1)
            0x29, 0x0D,                 //   Usage Maximum (Button 13)
            0x15, 0x00,                 //   Logical Minimum (0)
            0x25, 0x01,                 //   Logical Maximum (1)
            0x75, 0x01,                 //   Report Size (1)
            0x95.toByte(), 0x0D,        //   Report Count (13)
            0x81.toByte(), 0x02,        //   Input (Data,Var,Abs)
            0x75, 0x01,                 //   Report Size (1)   -- 3-bit pad
            0x95.toByte(), 0x03,        //   Report Count (3)
            0x81.toByte(), 0x03,        //   Input (Const,Var,Abs)

            0x05, 0x01,                 //   Usage Page (Generic Desktop)
            0x09, 0x39,                 //   Usage (Hat Switch)
            0x15, 0x00,                 //   Logical Minimum (0)
            0x25, 0x07,                 //   Logical Maximum (7)
            0x35, 0x00,                 //   Physical Minimum (0)
            0x46.toByte(), 0x3B, 0x01,  //   Physical Maximum (315)
            0x65, 0x14,                 //   Unit (Eng Rot: Degrees)
            0x75, 0x04,                 //   Report Size (4)
            0x95.toByte(), 0x01,        //   Report Count (1)
            0x81.toByte(), 0x42,        //   Input (Data,Var,Abs,Null)
            0x65, 0x00,                 //   Unit (None)
            0x75, 0x04,                 //   Report Size (4)   -- pad nibble
            0x95.toByte(), 0x01,        //   Report Count (1)
            0x81.toByte(), 0x03,        //   Input (Const,Var,Abs)

            // Right stick is Rx/Ry and the triggers are Z/Rz -- the XBOX
            // layout, which is what Windows and essentially every game
            // assume. This block originally declared the right stick on
            // Z/Rz and the triggers on Rx/Ry (the DualShock layout), so a
            // real host read our axes as different controls entirely.
            //
            // OBSERVED on Windows, 2026-08-16, exactly as reported: pulling
            // the LEFT TRIGGER moved a stick Y axis, and the right stick's
            // up/down landed on an axis nothing was bound to. Both follow
            // from the same swap -- our trigger bytes arrived where the host
            // looks for the right stick, and our right-stick bytes arrived
            // where it looks for triggers, which that host had nothing
            // mapped to.
            //
            // ONLY the four usage bytes below and in the next block change.
            // Report length, item structure, ordering, logical ranges and
            // buildReport() are all untouched -- the signed run is still
            // four axes then the unsigned run is two, so the nine bytes on
            // the wire keep their exact meaning positionally. That matters
            // because of the freeze noted in this file's header: the fewer
            // structural bytes move, the less of the hardware round has to
            // be re-proven (re-pair and re-enumerate is still required).
            0x09, 0x30,                 //   Usage (X)  -- left stick X
            0x09, 0x31,                 //   Usage (Y)  -- left stick Y
            0x09, 0x33,                 //   Usage (Rx) -- right stick X
            0x09, 0x34,                 //   Usage (Ry) -- right stick Y
            0x15, 0x81.toByte(),        //   Logical Minimum (-127)
            0x25, 0x7F,                 //   Logical Maximum (127)
            0x75, 0x08,                 //   Report Size (8)
            0x95.toByte(), 0x04,        //   Report Count (4)
            0x81.toByte(), 0x02,        //   Input (Data,Var,Abs)

            0x09, 0x32,                 //   Usage (Z)  -- left trigger
            0x09, 0x35,                 //   Usage (Rz) -- right trigger
            0x15, 0x00,                 //   Logical Minimum (0)
            0x26.toByte(), 0xFF.toByte(), 0x00, // Logical Maximum (255)
            0x75, 0x08,                 //   Report Size (8)
            0x95.toByte(), 0x02,        //   Report Count (2)
            0x81.toByte(), 0x02,        //   Input (Data,Var,Abs)

            0xC0.toByte(),              // End Collection
        )

        /** [REPORT_DESCRIPTOR] declares no Report ID item, so per
         *  [BluetoothHidDevice.sendReport]'s own javadoc ("id: ... Can be 0
         *  in case Report Id are not defined in descriptor") this is 0, not
         *  a made-up sentinel. Passing 0 here, combined with no `0x85` item
         *  in the descriptor, makes AOSP's `bta_hd_send_report_act()`
         *  compute `report_id = (use_report_id || boot_mode) ? id : 0` with
         *  `use_report_id == false` — so as long as the host has NOT forced
         *  boot mode (see [onSetProtocol handling][callback]), no report-ID
         *  byte is prepended on the wire at all, one less framing detail
         *  for a host's parser to get right. */
        const val REPORT_ID = 0

        const val REPORT_LEN = 9

        /** Wire bit (§5.1, [AtticPadNative]) -> HID button number, in
         *  order. index 0 -> HID button 1, ... index 12 -> HID button 13.
         *  The D-pad bits are handled separately by [HAT_LUT], not listed
         *  here. */
        private val BUTTON_BITS = intArrayOf(
            AtticPadNative.BTN_A,
            AtticPadNative.BTN_B,
            AtticPadNative.BTN_X,
            AtticPadNative.BTN_Y,
            AtticPadNative.BTN_L,
            AtticPadNative.BTN_R,
            AtticPadNative.BTN_ZL,
            AtticPadNative.BTN_ZR,
            AtticPadNative.BTN_L3,
            AtticPadNative.BTN_R3,
            AtticPadNative.BTN_START,
            AtticPadNative.BTN_SELECT,
            AtticPadNative.BTN_HOME,
        )

        /** docs/PROTOCOL.md §5.1's `apad_hat_lut`, mirrored verbatim (not
         *  moved — that table lives in the spec for servers/backends to
         *  copy, and a Bluetooth HID report is exactly the same kind of
         *  consumer a Linux/uinput backend is). Index: `(buttons >> 4) &
         *  0xF`, i.e. bit0=UP bit1=DOWN bit2=LEFT bit3=RIGHT. Value: HID hat
         *  0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=null. */
        private val HAT_LUT = intArrayOf(
            8, 0, 4, 8, 6, 7, 5, 6,
            2, 1, 3, 2, 8, 0, 4, 8,
        )

        private fun scaleStick(raw: Int): Int =
            ((raw.toLong() * 127L) / 32767L).toInt().coerceIn(-127, 127)

        /** §5: `axes[4]`/`axes[5]` are 0..32767 unsigned (negative MUST be
         *  read as 0 — already true of anything [InputSnapshot] produces,
         *  the clamp here is just defensive). HID Rx/Ry are declared 0..255
         *  unsigned in [REPORT_DESCRIPTOR], not signed like the sticks —
         *  a trigger has no negative half to centre against. */
        private fun scaleTrigger(raw: Int): Int =
            ((raw.coerceAtLeast(0).toLong() * 255L) / 32767L).toInt().coerceIn(0, 255)

        /**
         * [inArr] is an [AtticPadNative.IN_LEN]-length snapshot, exactly as
         * [InputSnapshot.readInto] fills it for the real UDP path — this
         * mode reads the SAME merged snapshot, just encodes it onto a
         * different wire.
         *
         * Y AXES ARE INVERTED HERE. §5.3: the wire (and this snapshot) is
         * `+Y up`, matching XInput. HID gamepad Y axes are conventionally
         * `+Y down` — every stock Windows/Linux gamepad tester draws "up"
         * as the negative direction, matching a real Xbox pad's HID
         * report — so left AND right stick Y are negated on the way out,
         * or "up" on the phone would show as "down" in joy.cpl.
         */
        fun buildReport(inArr: IntArray): ByteArray {
            var buttons = 0
            val wireButtons = inArr[AtticPadNative.IN_BUTTONS]
            for (i in BUTTON_BITS.indices) {
                if (wireButtons and BUTTON_BITS[i] != 0) buttons = buttons or (1 shl i)
            }
            val hat = HAT_LUT[(wireButtons ushr 4) and 0xF]

            val lx = scaleStick(inArr[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_LX])
            val ly = -scaleStick(inArr[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_LY])
            val rx = scaleStick(inArr[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_RX])
            val ry = -scaleStick(inArr[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_RY])
            val l2 = scaleTrigger(inArr[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_L2])
            val r2 = scaleTrigger(inArr[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_R2])

            return byteArrayOf(
                (buttons and 0xFF).toByte(),
                ((buttons ushr 8) and 0xFF).toByte(),
                (hat and 0x0F).toByte(),
                lx.toByte(), ly.toByte(), rx.toByte(), ry.toByte(),
                l2.toByte(), r2.toByte(),
            )
        }
    }

    data class Status(
        val supported: Boolean = isSupported(),
        val registered: Boolean = false,
        val connectedName: String = "",
        /** The connected device's MAC, alongside [connectedName] — lets
         *  [BtControllerActivity] remember which bonded device to try
         *  reconnecting to after a disconnect, without this class knowing
         *  anything about SharedPreferences or the guided-flow UI. */
        val connectedAddress: String = "",
        val streaming: Boolean = false,
        val reportsPerSec: Int = 0,
        /** True once registration is known to have failed — either
         *  [registerApp]'s own `registerApp()` call returned false, or the
         *  HID_DEVICE profile service disconnected out from under a live
         *  registration. [BtControllerActivity] shows the ONE supplied
         *  failure string for this ("This phone's Bluetooth doesn't support
         *  controller mode.") rather than [detail] — [detail] is for the
         *  screen's "Details" expander and logcat only. */
        val registrationFailed: Boolean = false,
        /** Developer/bug-report detail only — the exact string a
         *  `registerApp`/profile callback handed back. NEVER shown directly
         *  on screen: [BtControllerActivity] only surfaces this behind a
         *  tappable "Details" expander, and always logs it via [TAG]
         *  first. Every user-facing string in the guided flow is composed
         *  from [registered]/[connectedName]/[bootProtocol] instead. */
        val detail: String = "",
        /** Last SET_REPORT/interrupt-OUT data received from the host, if
         *  any — see the class doc on rumble. Empty until one arrives.
         *  Logcat/[TAG] only, same as [detail]. */
        val lastHostReport: String = "",
        /** Mirrors [bootProtocolActive] — surfaced so a hardware run can
         *  tell whether the host ever asked for boot protocol without
         *  needing logcat. See the class doc's "the 'repeated e' finding"
         *  section for why this exists. [BtControllerActivity] renders the
         *  ONE supplied banner string for this state, not [detail]. */
        val bootProtocol: Boolean = false,
    )

    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor()

    @Volatile var status: Status = Status(); private set
    @Volatile private var listener: ((Status) -> Unit)? = null

    private val adapter: BluetoothAdapter? =
        context.getSystemService(BluetoothManager::class.java)?.adapter

    private var hidDevice: BluetoothHidDevice? = null
    private var connectedDevice: BluetoothDevice? = null

    private var streamThread: Thread? = null
    @Volatile private var streaming = false

    /** Set from [BluetoothHidDevice.Callback.onSetProtocol]. HID 1.11's
     *  default (also stated on that callback's own javadoc: "By default,
     *  PROTOCOL_REPORT_MODE shall be assumed") is report protocol, so this
     *  starts false. [streamLoop] refuses to call `sendReport` while this
     *  is true — see the class doc's "the 'repeated e' finding" section for
     *  why: this app has no boot report to send, only a report-protocol
     *  one, and sending it anyway is exactly what produced the bug. */
    @Volatile private var bootProtocolActive = false

    fun setListener(l: ((Status) -> Unit)?) {
        listener = l
        l?.invoke(status)
    }

    private fun publish(s: Status) {
        status = s
        main.post { listener?.invoke(s) }
    }

    val bluetoothEnabled: Boolean get() = adapter?.isEnabled == true

    /** Bonded devices only — this mode does not scan, the user pairs from
     *  the PC's own Bluetooth settings (see [BtControllerActivity]'s step 2
     *  instruction text), same as `ralismark/bluehid` and CTS Verifier's own
     *  HID device test both do. */
    @Suppress("MissingPermission") // BLUETOOTH_CONNECT — caller gates entry on the permission
    fun bondedDevices(): List<BluetoothDevice> =
        adapter?.bondedDevices?.toList() ?: emptyList()

    private val profileListener = object : BluetoothProfile.ServiceListener {
        override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
            if (profile != BluetoothProfile.HID_DEVICE) return
            hidDevice = proxy as BluetoothHidDevice
            registerApp()
        }

        override fun onServiceDisconnected(profile: Int) {
            if (profile != BluetoothProfile.HID_DEVICE) return
            Log.w(TAG, "onServiceDisconnected — HID_DEVICE profile service went away")
            hidDevice = null
            stopStreaming()
            publish(
                status.copy(
                    registered = false,
                    connectedName = "",
                    registrationFailed = true,
                    detail = "HID_DEVICE profile service disconnected",
                ),
            )
        }
    }

    private val callback = object : BluetoothHidDevice.Callback() {
        override fun onAppStatusChanged(pluggedDevice: BluetoothDevice?, registered: Boolean) {
            Log.i(TAG, "onAppStatusChanged registered=$registered plugged=$pluggedDevice")
            publish(
                status.copy(
                    registered = registered,
                    // Only CLEAR a failure on a successful registration —
                    // registered=false also fires from an ordinary
                    // unregister() (see that function's own explicit reset),
                    // so it must not by itself flip this flag on.
                    registrationFailed = if (registered) false else status.registrationFailed,
                    detail = if (registered) "registered" else "not registered",
                ),
            )
        }

        override fun onConnectionStateChanged(device: BluetoothDevice, state: Int) {
            Log.i(TAG, "onConnectionStateChanged device=$device state=$state")
            when (state) {
                BluetoothProfile.STATE_CONNECTED -> {
                    connectedDevice = device
                    // HID 1.11's default is report protocol (also the
                    // javadoc on onSetProtocol below), so a fresh
                    // connection always starts assuming report mode — a
                    // stale `true` surviving a previous session's
                    // disconnect/reconnect would otherwise wedge output.
                    bootProtocolActive = false
                    @Suppress("MissingPermission") // BLUETOOTH_CONNECT: gated by isSupported()+caller
                    val name = try { device.name ?: device.address } catch (e: SecurityException) { device.address }
                    publish(
                        status.copy(
                            connectedName = name,
                            connectedAddress = device.address,
                            detail = "connected",
                            bootProtocol = false,
                        ),
                    )
                    startStreaming()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (device == connectedDevice) connectedDevice = null
                    stopStreaming()
                    publish(
                        status.copy(
                            connectedName = "",
                            connectedAddress = "",
                            streaming = false,
                            detail = "disconnected",
                        ),
                    )
                }
                else -> {}
            }
        }

        // ---- boot vs. report protocol — see the class doc's "the
        // 'repeated e' finding" section for the full evidence chain ----
        //
        // AOSP unconditionally advertises `ATTR_ID_HID_BOOT_DEVICE = true`
        // in the SDP record for EVERY registered app regardless of
        // [BluetoothHidDeviceAppSdpSettings]'s subclass byte (confirmed
        // from `system/stack/hid/hidd_api.cc`'s `HID_DevAddRecord` — that
        // attribute is written from a hardcoded `bool_true`, not derived
        // from `subclass` at all, and there is no public API to change it).
        // Because of that, a host is entitled to believe this device can do
        // the fixed 8-byte boot-keyboard/boot-mouse report shapes and may
        // put the connection into boot mode via SET_PROTOCOL — which is
        // exactly what a Windows 11 laptop did against real hardware,
        // 2026-08-11: `registerApp`/connect/stream all succeeded, but the
        // host read our 9-byte report AS a boot-protocol keyboard report
        // (byte0=modifier, byte1=reserved, bytes2-7=keycode array). Our
        // centred hat-switch nibble (null = 8) landed in that keycode
        // window, and HID keyboard usage 0x08 IS 'e' — hence a continuous
        // stream of the letter 'e' on the host the whole time the pad was
        // "connected" and "streaming" by every metric this class could see.
        //
        // `bta_hd_set_protocol_act()` (AOSP `system/bta/hd/bta_hd_act.cc`)
        // accepts SET_PROTOCOL unconditionally — there is no reject/NAK
        // path — so the ONLY lever this app has is what it does
        // AFTERWARDS: stop emitting the gamepad report while boot mode is
        // active, since [REPORT_DESCRIPTOR] declares no boot-shaped report
        // for either device type and sending it anyway is the bug. See
        // [streamLoop]'s gate on [bootProtocolActive].
        override fun onSetProtocol(device: BluetoothDevice, protocol: Byte) {
            val boot = protocol == BluetoothHidDevice.PROTOCOL_BOOT_MODE
            // Logcat/detail only — [BtControllerActivity] renders the ONE
            // supplied banner string for bootProtocol==true (see the class
            // doc's "the 'repeated e' finding" section), never this text.
            Log.i(TAG, "onSetProtocol device=$device protocol=$protocol boot=$boot")
            bootProtocolActive = boot
            publish(
                status.copy(
                    bootProtocol = boot,
                    detail = if (boot) {
                        "host requested BOOT protocol — holding output (no boot report to send)"
                    } else {
                        "report protocol"
                    },
                ),
            )
        }

        // ---- rumble — SKIPPED, see the class doc and the report ----
        //
        // REPORT_DESCRIPTOR declares only INPUT items: no OUTPUT report a
        // host could target, so nothing here has a defined report to
        // decode even if a host sent one. A generic Windows/Linux gamepad
        // tester (joy.cpl, jstest-gtk, SDL) has no rumble affordance for a
        // plain HID game pad anyway — that needs either a vendor-defined
        // OUTPUT usage the specific driver knows about, or a full USB PID
        // (Physical Interface Device, HID usage page 0x0F) force-feedback
        // descriptor — substantially more HID plumbing than this promotion
        // takes on; DEFERRED, not attempted. Logged and surfaced in
        // [Status.lastHostReport] (logcat/bug-report use only, never shown
        // directly on screen) so a real test run still shows whether
        // anything ever arrives.
        override fun onSetReport(device: BluetoothDevice, type: Byte, id: Byte, data: ByteArray) {
            val hex = data.joinToString(" ") { "%02x".format(it) }
            Log.i(TAG, "onSetReport type=$type id=$id data=$hex")
            publish(status.copy(lastHostReport = "SET_REPORT type=$type id=$id: $hex"))
        }

        override fun onInterruptData(device: BluetoothDevice, reportId: Byte, data: ByteArray) {
            val hex = data.joinToString(" ") { "%02x".format(it) }
            Log.i(TAG, "onInterruptData reportId=$reportId data=$hex")
            publish(status.copy(lastHostReport = "INTERRUPT id=$reportId: $hex"))
        }

        override fun onGetReport(device: BluetoothDevice, type: Byte, id: Byte, bufferSize: Int) {
            // No FEATURE report and only one INPUT report declared — reply
            // with that one so a strict host GET_REPORT probe still gets
            // an answer instead of silence.
            Log.i(TAG, "onGetReport type=$type id=$id bufferSize=$bufferSize")
            @Suppress("MissingPermission")
            hidDevice?.replyReport(device, type, id, buildReport(currentSnapshot()))
        }
    }

    private fun currentSnapshot(): IntArray {
        val arr = IntArray(AtticPadNative.IN_LEN)
        input.readInto(arr)
        return arr
    }

    /** Entry point: acquire the HID_DEVICE profile proxy, which calls
     *  [registerApp] once it connects. Safe to call again after
     *  [unregister] — a fresh proxy is fetched each time, matching
     *  `BluetoothProfile`'s own "one call, one callback" contract. */
    fun register() {
        if (!isSupported()) {
            publish(
                status.copy(
                    registrationFailed = true,
                    detail = "requires Android 9 (API 28) or newer, this device is API " +
                        "${Build.VERSION.SDK_INT}",
                ),
            )
            return
        }
        val a = adapter
        if (a == null || !a.isEnabled) {
            publish(status.copy(registrationFailed = true, detail = "Bluetooth is off or unsupported on this device"))
            return
        }
        a.getProfileProxy(context, profileListener, BluetoothProfile.HID_DEVICE)
    }

    @Suppress("MissingPermission") // BLUETOOTH_CONNECT — caller gates entry on the permission
    private fun registerApp() {
        // "AtticPad" exactly — this is the name a PC's Bluetooth "add a
        // device" dialog shows, and BtControllerActivity's step-2
        // instruction text tells the user to look for and choose exactly
        // this name (task brief copy: "Choose AtticPad").
        val sdp = BluetoothHidDeviceAppSdpSettings(
            "AtticPad",
            "AtticPad Bluetooth controller",
            "AtticPad",
            // SUBCLASS1_NONE | SUBCLASS2_GAMEPAD — "gamepad", not a
            // keyboard/mouse combo. These two constants live on
            // BluetoothHidDevice itself, not on AppSdpSettings — confirmed
            // against AOSP `packages/modules/Bluetooth` source, since
            // neither the SDK reference page nor most blog posts mention
            // them at all.
            //
            // Kept at SUBCLASS2_GAMEPAD, NOT falled back to a bare 0x00 —
            // the class doc's "repeated e" section traced the boot-protocol
            // misread to `ATTR_ID_HID_BOOT_DEVICE`, an SDP attribute AOSP's
            // `HID_DevAddRecord` (`system/stack/hid/hidd_api.cc`) writes as
            // a hardcoded `bool_true` with NO reference to `subclass`
            // anywhere in that function. Changing this byte cannot reach
            // that attribute, so a subclass fallback was evaluated and
            // rejected as not applicable here, rather than left untried.
            (BluetoothHidDevice.SUBCLASS1_NONE.toInt() or
                BluetoothHidDevice.SUBCLASS2_GAMEPAD.toInt()).toByte(),
            REPORT_DESCRIPTOR,
        )
        // Same numbers CTS Verifier's own HidDeviceActivity uses for its
        // (also 9-byte) report: tokenRate = reportBytes * 1_000_000us /
        // intervalUs. SERVICE_BEST_EFFORT means a real Bluetooth stack
        // mostly treats these as advisory rather than a hard contract, so
        // reusing Google's own known-good numbers beats inventing new ones.
        val outQos = BluetoothHidDeviceAppQosSettings(
            BluetoothHidDeviceAppQosSettings.SERVICE_BEST_EFFORT,
            800, 9, 0, 11250, BluetoothHidDeviceAppQosSettings.MAX,
        )
        val ok = hidDevice?.registerApp(sdp, null, outQos, executor, callback) == true
        if (!ok) {
            // registerApp()'s own javadoc: "true if the command is
            // successfully SENT" — false here means it could not even be
            // sent (no proxy, adapter disabled), not "the host refused".
            // A refusal instead arrives, or never arrives, through
            // onAppStatusChanged.
            publish(
                status.copy(
                    registrationFailed = true,
                    detail = "registerApp() returned false — could not send the command",
                ),
            )
        }
    }

    fun unregister() {
        stopStreaming()
        @Suppress("MissingPermission")
        hidDevice?.unregisterApp()
        adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hidDevice)
        hidDevice = null
        connectedDevice = null
        publish(
            status.copy(
                registered = false,
                connectedName = "",
                streaming = false,
                registrationFailed = false,
                detail = "unregistered",
            ),
        )
    }

    @Suppress("MissingPermission")
    fun connectTo(device: BluetoothDevice) {
        hidDevice?.connect(device)
    }

    // ---- ~60 Hz report stream --------------------------------------------

    private fun startStreaming() {
        if (streaming) return
        streaming = true
        streamThread = Thread({ streamLoop() }, "atticpad-bthid-stream").apply { start() }
    }

    private fun stopStreaming() {
        streaming = false
        streamThread?.interrupt()
        streamThread = null
    }

    @Suppress("MissingPermission")
    private fun streamLoop() {
        var sent = 0
        var windowStart = System.currentTimeMillis()
        var lastPublish = 0L
        while (streaming && !Thread.currentThread().isInterrupted) {
            val device = connectedDevice
            val hid = hidDevice
            // Gate on bootProtocolActive, not just device/hid non-null —
            // see onSetProtocol's doc comment. Sending our report-protocol
            // gamepad report while the host is in boot mode is the exact
            // mechanism behind the "repeated e" incident, so this thread
            // keeps running (ready to resume the instant the host sends
            // SET_PROTOCOL(REPORT) again) but goes silent on the wire
            // instead of emitting anything boot-shaped.
            if (device != null && hid != null && !bootProtocolActive) {
                val report = buildReport(currentSnapshot())
                if (hid.sendReport(device, REPORT_ID, report)) sent++
            }
            val now = System.currentTimeMillis()
            if (now - windowStart >= 1000) {
                val rate = sent
                sent = 0
                windowStart = now
                if (now - lastPublish >= 500) {
                    lastPublish = now
                    publish(status.copy(streaming = true, reportsPerSec = rate))
                }
            }
            try {
                Thread.sleep(16) // ~60 Hz — matches DEFAULT_RATE_HZ elsewhere in this app
            } catch (e: InterruptedException) {
                break
            }
        }
    }
}
