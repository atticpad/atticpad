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
 * from a spike to a first-class mode, 2026-08-12. [MainActivity]'s own
 * Bluetooth setup screen ([MainActivity.buildBtSetupPanel] and its session
 * overlay, folded together with the UDP path 2026-08-26) is the guided
 * setup UI built over this same engine; nothing in this class changed for
 * either promotion.
 *
 * PHONE-INITIATED DISCOVERY/PAIRING joined this class 2026-08-26
 * ([startDiscovery]/[cancelDiscovery]/[createBond] — see [bondedDevices]'s
 * own doc for what this changes and what it does not): [MainActivity]'s
 * "Nearby devices" list lets the user pair without touching the PC's own
 * Bluetooth settings at all. None of these three are HID APIs — they sit
 * entirely below [registerApp]/[connectTo], on the plain
 * [BluetoothAdapter]/[BluetoothDevice] surface every Bluetooth app uses,
 * so nothing about the report descriptor, boot-protocol handling or
 * report building below changed for this either.
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
 * in this build ([MainActivity]'s overlay is bound to exactly one
 * [SessionTransport] at a time, UDP or Bluetooth, never both — see
 * [MainActivity.activeTransport]), but keeping them structurally separate
 * costs nothing and avoids a shared-state bug later.
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
 * Fix applied (see the pre-2026-08-25 [LEGACY_REPORT_DESCRIPTOR_NO_REPORT_ID],
 * and the `onSetProtocol`/[bootProtocolActive] pair below): #1 cannot be
 * changed from app code (confirmed — it is unconditional in AOSP, not
 * derived from subclass, so trying `SUBCLASS1_NONE`-alone or other subclass
 * bytes was evaluated and rejected as not reaching the actual attribute at
 * all); #2 likewise cannot be refused by the app. The only lever this app
 * has is AFTER the host chooses boot mode: stop sending the report-protocol
 * report while boot mode is active, since this app has no boot-shaped
 * report to send either way — believed to be THE fix. Dropping the unused
 * Report ID item (single-report device, `REPORT_ID` was 0) was a real,
 * independent simplification evaluated at the same time, but it changed
 * only report-protocol framing and would not by itself have stopped the
 * boot-protocol misread — that fix is credited to the `onSetProtocol` gate,
 * not the Report ID removal. See the new section below for why Report IDs
 * came BACK, 2026-08-25.
 *
 * ---- keyboard/mouse/media joined this device, 2026-08-25 — Report IDs
 * came back, deliberately ----
 *
 * AtticPad's Bluetooth mode grew keyboard, mouse and consumer-control (media
 * key) reports alongside the gamepad one, so the phone's UI can work the
 * same way over Bluetooth as it does over the UDP/server path with no
 * server at all. Multiplexing four report shapes onto one HID device
 * *requires* Report IDs (HID 1.11 §8.3) — a receiver has no other way to
 * tell a 9-byte gamepad report from an 8-byte keyboard one apart on the
 * same interrupt channel. [LEGACY_REPORT_DESCRIPTOR_NO_REPORT_ID] is kept
 * verbatim, unused, as the pre-Report-ID hardware-proven baseline — a
 * future hardware round that regresses has something exact to diff
 * against, and docs/CONVENTIONS.md's own instinct ("the wire format... stop and
 * raise it") applies just as much to a proven descriptor as a frozen wire
 * format, even though this one is a v1-external HID artifact, not
 * `docs/PROTOCOL.md`.
 *
 * Assignment, gamepad first (task brief): [REPORT_ID_GAMEPAD] = 1,
 * [REPORT_ID_KEYBOARD] = 2, [REPORT_ID_MOUSE] = 3, [REPORT_ID_CONSUMER] = 4
 * — gamepad keeps the lowest ID because it is the hardware-proven original;
 * the other three are new and ordered the way [MainActivity]'s own mode bar
 * already orders them (MOUSE, KEYBOARD, MEDIA) would suggest KEYBOARD before
 * MOUSE, but this file orders by ship-order/proof-order instead: keyboard's
 * report is the simplest and most standard (boot-compatible, see below), so
 * it goes second; consumer control (media) is the newest and least proven
 * shape, so it goes last.
 *
 * [buildReport] (the gamepad one) is BYTE-FOR-BYTE UNCHANGED — same 9
 * bytes, same field order. Only the `id` argument passed to
 * `hidDevice.sendReport(device, id, data)` changed, from the constant 0 to
 * [REPORT_ID_GAMEPAD]. Per this class's own already-established reading of
 * `bta_hd_send_report_act()` (`report_id = (use_report_id || boot_mode) ?
 * id : 0`), the Bluetooth stack — not this app — is what prepends the ID
 * byte on the wire once `use_report_id` is true, which happens because
 * [REPORT_DESCRIPTOR] now contains a literal `0x85` item (see
 * `check_descriptor()`, cited above). So "adds a leading ID byte to the
 * proven nine-byte report" is true of the BYTES ON THE WIRE, never of
 * [buildReport]'s own return value.
 *
 * KEYBOARD REPORT IS BOOT-PROTOCOL SHAPED ON PURPOSE (modifier byte,
 * reserved byte, six keycode slots — see [buildKeyboardReport]) rather than
 * a bitmap of all 256 possible usages. The deciding argument: §1's own
 * "repeated e" finding proved a host CAN force this connection into boot
 * mode, unconditionally, with no reject path available to the app — so if
 * that happens again, a report that is ALREADY the exact shape a boot host
 * expects is far more likely to be read correctly (or at least
 * plausibly) than one that is not. This is NOT free of risk, and the risk
 * is spelled out rather than assumed away: with `use_report_id` now true,
 * the SAME formula above (`report_id = (use_report_id || boot_mode) ? id :
 * 0`) prepends an ID byte to EVERY report AOSP sends, boot mode or not —
 * so the wire bytes for the keyboard report during boot mode would be
 * `[2, modifier, reserved, k0..k5]`, nine bytes, not the eight-byte boot
 * shape a strict boot-mode host expects. A "boot-shaped payload" is not
 * "boot-shaped wire bytes" once Report IDs are in play. Given that, and
 * given this cannot be verified without the exact hardware that produced
 * the original bug, [streamLoop] makes the CONSERVATIVE choice: it holds
 * ALL FOUR report types — gamepad, keyboard, mouse, consumer — while
 * [bootProtocolActive], unchanged in spirit from the original one-report
 * gate, rather than gambling that the keyboard report's shape alone
 * survives an ID byte the boot-mode host was not expecting either. Whether
 * boot mode becomes benign now that a real keyboard report exists at all
 * is a genuine, stated hypothesis, not a claim — see the task report for
 * exactly what would need to be observed on real hardware to settle it
 * either way.
 *
 * MOUSE REPORT IS BOOT-PROTOCOL COMPATIBLE TOO, for the same reason,
 * extended past the classic 3 bytes: [buildMouseReport]'s first three
 * bytes (buttons, X, Y) are the standard boot-mouse shape; wheel and
 * horizontal wheel ride two EXTRA bytes after them, which a strict
 * boot-mode host simply never reads (real extended mice do this too — boot
 * mode is defined as "read the first three bytes", not "the report must be
 * exactly three bytes"). §6.16's `dx_accum`/`dy_accum`/wheel counters are
 * free-running, WRAPPING, ACCUMULATED values (this class's own
 * [InputSnapshot]-reading siblings already document why — no downstream
 * profile owns mouse sensitivity), but a HID mouse report is a PER-REPORT
 * RELATIVE delta — [buildMouseReport] tracks the last-consumed accumulator
 * value itself (new instance state, [mouseDxSent] etc.) and diffs against
 * it every pump, clamping the delta to the descriptor's declared ±127 range
 * and carrying any clipped remainder into the NEXT report rather than
 * dropping it, so a fast flick drains over a couple of frames instead of
 * losing distance.
 *
 * CONSUMER REPORT is new: a single 16-bit "Consumer Control" array field
 * (usage 0-0x3FF) carrying whichever ONE §6.18 media control is currently
 * held, or 0 for none — see [buildConsumerReport] and
 * [MEDIA_TO_CONSUMER_USAGE], whose usage numbers come from
 * `references/hid/consumer-to-evdev.txt`'s own §6.18-to-Consumer-usage
 * resolution table (docs/CONVENTIONS.md: "mirror the platform SDK's own samples...
 * do not write it from memory" — this is the same discipline one layer
 * over, a vendored ground-truth table instead of a platform sample).
 * Simultaneous holds (e.g. VOLUME_UP held while tapping NEXT_TRACK)
 * collapse to whichever control has the LOWEST §6.18 index — a genuine,
 * documented simplification, not an oversight; MediaRemoteView's touch UI
 * makes this rare in practice.
 *
 * Whether ramping controls like VOLUME_UP still ramp under this scheme is
 * UNVERIFIED: this app sends the same nonzero usage every ~16ms for as
 * long as the control is held, the same way a literal keyboard key stays
 * "down" while pressed — whether a given host's own OS repeat/ramp policy
 * treats that the way a physical remote's repeated HID reports would is a
 * real open question this session cannot answer without hardware.
 */
class BtHidController(
    private val context: Context,
    private val input: InputSnapshot,
    private val kbm: KbmSnapshot,
) {

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
        // NO Report ID. Single report, 9 bytes total:
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
        //
        // NOT USED BY [registerApp] any more, 2026-08-25 — kept verbatim as
        // the pre-Report-ID, hardware-proven baseline (see this class's own
        // doc, "keyboard/mouse/media joined this device"). [REPORT_DESCRIPTOR]
        // below is the live one; a byte-diff against THIS constant is the
        // fastest way to confirm a future edit to the gamepad collection
        // really did keep every structural byte the hardware round proved.
        val LEGACY_REPORT_DESCRIPTOR_NO_REPORT_ID: ByteArray = byteArrayOf(
            0x05, 0x01,                 // Usage Page (Generic Desktop)
            0x09, 0x05,                 // Usage (Game Pad)
            0xA1.toByte(), 0x01,        // Collection (Application)
            // No Report ID item (0x85) — at the time this constant was the
            // live descriptor, this app registered exactly one report, and
            // HID 1.11 §8.3 only requires a Report ID when a device
            // multiplexes more than one report shape onto the same endpoint
            // (CTS Verifier's own HIDD_REPORT_DESC uses one precisely
            // because IT declares two: ID_KEYBOARD and ID_MOUSE back to
            // back in the same descriptor — not evidence that a
            // single-report device needs one too). Confirmed from AOSP
            // `system/bta/hd/bta_hd_act.cc`'s `check_descriptor()`, which
            // scans this exact byte array for a literal `0x85` and flips
            // `bta_hd_cb.use_report_id` off when it finds none — so this
            // was not cosmetic, it changed what `bta_hd_send_report_act()`
            // put on the wire. [REPORT_DESCRIPTOR] below is the opposite
            // choice, deliberately, now that four reports share one device.

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

        // §8.3 Report IDs, gamepad first (task brief) — see this class's
        // own "keyboard/mouse/media joined this device" doc for the full
        // reasoning. Each is the literal byte a `0x85` item in
        // [REPORT_DESCRIPTOR] declares AND the `id` argument
        // [streamLoop]/[onGetReport] pass to `sendReport`/`replyReport`.
        const val REPORT_ID_GAMEPAD = 1
        const val REPORT_ID_KEYBOARD = 2
        const val REPORT_ID_MOUSE = 3
        const val REPORT_ID_CONSUMER = 4

        const val GAMEPAD_REPORT_LEN = 9
        const val KEYBOARD_REPORT_LEN = 8
        const val MOUSE_REPORT_LEN = 5
        const val CONSUMER_REPORT_LEN = 2

        /**
         * The LIVE descriptor — four top-level Application collections back
         * to back (gamepad, keyboard, mouse, consumer control), each opening
         * with its own `0x85` Report ID item, same shape CTS Verifier's own
         * `HIDD_REPORT_DESC` uses for ITS two collections (cited above).
         *
         * The gamepad collection is [LEGACY_REPORT_DESCRIPTOR_NO_REPORT_ID]
         * with EXACTLY ONE insertion — `0x85, REPORT_ID_GAMEPAD` right after
         * `Collection (Application)` — and nothing else touched: same
         * items, same order, same lengths, so [buildReport] needed no
         * changes at all (see this class's own doc).
         */
        val REPORT_DESCRIPTOR: ByteArray = byteArrayOf(
            // ---- gamepad (ID 1) — byte-for-byte LEGACY_REPORT_DESCRIPTOR_
            // NO_REPORT_ID, plus one Report ID item ----
            0x05, 0x01,                 // Usage Page (Generic Desktop)
            0x09, 0x05,                 // Usage (Game Pad)
            0xA1.toByte(), 0x01,        // Collection (Application)
            0x85.toByte(), REPORT_ID_GAMEPAD.toByte(), // Report ID (1)

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

            // ---- keyboard (ID 2) — the STANDARD USB HID boot-keyboard
            // shape verbatim (HID 1.11 Appendix B.1): modifier byte (8
            // single-bit usages, page 0x07 0xE0-0xE7), one constant
            // reserved byte, then 6 keycode slots (array, usage 0-0x65).
            // Deliberately boot-compatible — see this class's own doc for
            // why, and the honest limit of what that buys once Report IDs
            // are in play. */
            0x05, 0x01,                 // Usage Page (Generic Desktop)
            0x09, 0x06,                 // Usage (Keyboard)
            0xA1.toByte(), 0x01,        // Collection (Application)
            0x85.toByte(), REPORT_ID_KEYBOARD.toByte(), // Report ID (2)
            0x05, 0x07,                 //   Usage Page (Kbrd/Keypad)
            0x19, 0xE0.toByte(),        //   Usage Minimum (0xE0, LeftCtrl)
            0x29, 0xE7.toByte(),        //   Usage Maximum (0xE7, RightGUI)
            0x15, 0x00,                 //   Logical Minimum (0)
            0x25, 0x01,                 //   Logical Maximum (1)
            0x75, 0x01,                 //   Report Size (1)
            0x95.toByte(), 0x08,        //   Report Count (8)
            0x81.toByte(), 0x02,        //   Input (Data,Var,Abs)  -- modifier byte
            0x95.toByte(), 0x01,        //   Report Count (1)
            0x75, 0x08,                 //   Report Size (8)
            0x81.toByte(), 0x01,        //   Input (Const,Array,Abs) -- reserved byte
            0x95.toByte(), 0x06,        //   Report Count (6)
            0x75, 0x08,                 //   Report Size (8)
            0x15, 0x00,                 //   Logical Minimum (0)
            0x25, 0x65,                 //   Logical Maximum (101)
            0x05, 0x07,                 //   Usage Page (Kbrd/Keypad)
            0x19, 0x00,                 //   Usage Minimum (0)
            0x29, 0x65,                 //   Usage Maximum (101)
            0x81.toByte(), 0x00,        //   Input (Data,Array,Abs) -- 6 keycode slots
            0xC0.toByte(),              // End Collection

            // ---- mouse (ID 3) — boot-compatible first 3 bytes (buttons,
            // X, Y), plus vertical/horizontal wheel as EXTRA bytes a
            // boot-mode host simply never reads (real extended mice do
            // this too). 5 buttons packed into byte 0's low bits matches
            // AtticPadNative.MOUSEBTN_* (1..5) -> bit (b-1) exactly, so
            // buildMouseReport needs no LUT for buttons. ----
            0x05, 0x01,                 // Usage Page (Generic Desktop)
            0x09, 0x02,                 // Usage (Mouse)
            0xA1.toByte(), 0x01,        // Collection (Application)
            0x85.toByte(), REPORT_ID_MOUSE.toByte(), // Report ID (3)
            0x09, 0x01,                 //   Usage (Pointer)
            0xA1.toByte(), 0x00,        //   Collection (Physical)
            0x05, 0x09,                 //     Usage Page (Button)
            0x19, 0x01,                 //     Usage Minimum (Button 1)
            0x29, 0x05,                 //     Usage Maximum (Button 5)
            0x15, 0x00,                 //     Logical Minimum (0)
            0x25, 0x01,                 //     Logical Maximum (1)
            0x95.toByte(), 0x05,        //     Report Count (5)
            0x75, 0x01,                 //     Report Size (1)
            0x81.toByte(), 0x02,        //     Input (Data,Var,Abs)  -- 5 buttons
            0x95.toByte(), 0x01,        //     Report Count (1)
            0x75, 0x03,                 //     Report Size (3)   -- pad to byte 0
            0x81.toByte(), 0x03,        //     Input (Const,Var,Abs)
            0x05, 0x01,                 //     Usage Page (Generic Desktop)
            0x09, 0x30,                 //     Usage (X)
            0x09, 0x31,                 //     Usage (Y)
            0x15, 0x81.toByte(),        //     Logical Minimum (-127)
            0x25, 0x7F,                 //     Logical Maximum (127)
            0x75, 0x08,                 //     Report Size (8)
            0x95.toByte(), 0x02,        //     Report Count (2)
            0x81.toByte(), 0x06,        //     Input (Data,Var,Rel)  -- byte 1=X byte 2=Y
            0x09, 0x38,                 //     Usage (Wheel)
            0x15, 0x81.toByte(),        //     Logical Minimum (-127)
            0x25, 0x7F,                 //     Logical Maximum (127)
            0x75, 0x08,                 //     Report Size (8)
            0x95.toByte(), 0x01,        //     Report Count (1)
            0x81.toByte(), 0x06,        //     Input (Data,Var,Rel)  -- byte 3, vertical wheel
            0x05, 0x0C,                 //     Usage Page (Consumer)
            0x0A, 0x38, 0x02,           //     Usage (AC Pan)  -- horizontal wheel
            0x15, 0x81.toByte(),        //     Logical Minimum (-127)
            0x25, 0x7F,                 //     Logical Maximum (127)
            0x75, 0x08,                 //     Report Size (8)
            0x95.toByte(), 0x01,        //     Report Count (1)
            0x81.toByte(), 0x06,        //     Input (Data,Var,Rel)  -- byte 4, horizontal wheel
            0xC0.toByte(),              //   End Collection
            0xC0.toByte(),              // End Collection

            // ---- consumer control / media keys (ID 4) — one 16-bit
            // array field, usage 0-0x3FF, carrying whichever ONE §6.18
            // control [buildConsumerReport] currently has held, or 0. ----
            0x05, 0x0C,                 // Usage Page (Consumer)
            0x09, 0x01,                 // Usage (Consumer Control)
            0xA1.toByte(), 0x01,        // Collection (Application)
            0x85.toByte(), REPORT_ID_CONSUMER.toByte(), // Report ID (4)
            0x15, 0x00,                 //   Logical Minimum (0)
            0x26.toByte(), 0xFF.toByte(), 0x03, //   Logical Maximum (1023)
            0x19, 0x00,                 //   Usage Minimum (0)
            0x2A.toByte(), 0xFF.toByte(), 0x03, //   Usage Maximum (1023)
            0x75, 0x10,                 //   Report Size (16)
            0x95.toByte(), 0x01,        //   Report Count (1)
            0x81.toByte(), 0x00,        //   Input (Data,Array,Abs)
            0xC0.toByte(),              // End Collection
        )

        /** §6.18 control index (1..24, [AtticPadNative.MEDIA_*]) -> HID
         *  Consumer-page usage, index 0 unused (controls are 1-based). Every
         *  value transcribed from `references/hid/consumer-to-evdev.txt`'s
         *  own "§6.18 control -> consumer usage" resolution table — not
         *  re-derived or recalled, per that file's own machine-readable
         *  `@CONTROL usage KEY_* code` rows. */
        private val MEDIA_TO_CONSUMER_USAGE = intArrayOf(
            0x0000, // 0 -- unused
            0x00CD, // 1  PLAY_PAUSE
            0x00B0, // 2  PLAY
            0x00B1, // 3  PAUSE
            0x00B7, // 4  STOP        (KEY_STOPCD, not "AC Stop" 0x226)
            0x00B5, // 5  NEXT_TRACK
            0x00B6, // 6  PREV_TRACK
            0x00B3, // 7  FAST_FORWARD
            0x00B4, // 8  REWIND
            0x00E9, // 9  VOLUME_UP
            0x00EA, // 10 VOLUME_DOWN
            0x00E2, // 11 MUTE
            0x00B8, // 12 EJECT
            0x00B2, // 13 RECORD
            0x006F, // 14 BRIGHTNESS_UP
            0x0070, // 15 BRIGHTNESS_DOWN
            0x008A, // 16 LAUNCH_BROWSER
            0x018A, // 17 LAUNCH_MAIL
            0x0192, // 18 LAUNCH_CALC
            0x0221, // 19 SEARCH      (KEY_SEARCH, not KEY_FIND's 0x21F)
            0x0223, // 20 NAV_HOME
            0x0224, // 21 NAV_BACK
            0x0225, // 22 NAV_FORWARD
            0x0227, // 23 REFRESH
            0x0182, // 24 BOOKMARKS
        )

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

        /** True iff HID usage [usage] is currently held, read straight off
         *  the packed 256-bit `keys[]` bitmap [KbmSnapshot.readInto] fills
         *  at `IN_KB_KEYS0..+7` — the same 32-bit-word/4-byte-LE packing
         *  that method's own doc describes, addressed here by usage
         *  (`word = usage / 32`, `bit = usage % 32`) rather than rebuilt
         *  from a queue. */
        private fun keyHeld(kbmArr: IntArray, usage: Int): Boolean {
            val word = usage / 32
            val bit = usage % 32
            return (kbmArr[AtticPadNative.IN_KB_KEYS0 + word] ushr bit) and 1 != 0
        }

        /**
         * The boot-shaped keyboard report — modifier byte, one constant
         * reserved byte, six keycode slots — built fresh from [kbmArr]'s
         * `keys[]` bitmap every call (stateless: a keyboard report, unlike
         * the mouse one, is "what's held right now", not an accumulated
         * delta). Usages above 0x65 (the descriptor's declared Usage
         * Maximum) are silently skipped rather than sent malformed —
         * [AtticPadNative]'s own curated HID_KEY_* vocabulary never goes
         * that high except the eight 0xE0-0xE7 modifiers, which are read
         * separately into the modifier byte, not the keycode slots.
         */
        fun buildKeyboardReport(kbmArr: IntArray): ByteArray {
            var modifiers = 0
            for (bit in 0 until 8) {
                if (keyHeld(kbmArr, 0xE0 + bit)) modifiers = modifiers or (1 shl bit)
            }
            val keys = ByteArray(6)
            var slot = 0
            var usage = 0x04
            while (usage <= 0x65 && slot < 6) {
                if (keyHeld(kbmArr, usage)) {
                    keys[slot] = usage.toByte()
                    slot++
                }
                usage++
            }
            return byteArrayOf(modifiers.toByte(), 0, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5])
        }

        /**
         * One §6.16 accumulator step: [current] and [lastSent] are both
         * 16-bit WRAPPING counters (`KbmSnapshot`'s own free-running
         * convention — see its class doc); this returns the signed,
         * ±127-clamped delta to put on the wire THIS report, and the
         * updated [lastSent] to carry forward — adding the CLAMPED delta
         * back to [lastSent] (not jumping straight to [current]) is what
         * carries any clipped remainder into the next report instead of
         * silently dropping distance on a fast flick. A pure function of
         * its two Int arguments — no [KbmSnapshot], no instance state — so
         * it is unit-testable with plain known-good (current, lastSent) ->
         * (delta, newLastSent) triples.
         */
        fun consumeMouseDelta(current: Int, lastSent: Int): Pair<Int, Int> {
            val raw = (current - lastSent) and 0xFFFF
            val signed = if (raw >= 0x8000) raw - 0x10000 else raw
            val clamped = signed.coerceIn(-127, 127)
            val newLastSent = (lastSent + clamped) and 0xFFFF
            return clamped to newLastSent
        }

        /**
         * The consumer-control report: whichever ONE §6.18 media control is
         * currently held (lowest index wins a simultaneous hold — see this
         * class's own doc), as a little-endian 16-bit usage from
         * [MEDIA_TO_CONSUMER_USAGE], or 0 for "nothing held". Stateless,
         * same reasoning as [buildKeyboardReport].
         */
        fun buildConsumerReport(kbmArr: IntArray): ByteArray {
            val held = kbmArr[AtticPadNative.IN_MEDIA_HELD]
            var usage = 0
            if (held != 0) {
                for (c in 1..24) {
                    if (held and AtticPadNative.mediaBit(c) != 0) {
                        usage = MEDIA_TO_CONSUMER_USAGE[c]
                        break
                    }
                }
            }
            return byteArrayOf((usage and 0xFF).toByte(), ((usage ushr 8) and 0xFF).toByte())
        }

        // ---- known-good byte sequences, run on-device — no hardware needed ---
        //
        // Job 2's brief: "unit-test the report builders against known-good
        // byte sequences if you can; that is testable without a host." This
        // app has no JUnit/local-JVM test setup at all (docs/CONVENTIONS.md: zero
        // third-party dependencies) and [AtticPadNative]'s own `init {
        // System.loadLibrary(...) }` means even a bare `kotlinc` run off
        // Android would crash on class-load — so this follows the SAME
        // idiom [AtticPadNative.selfTest] already uses for the §13
        // protocol vectors: a plain on-device function, callable from
        // [MainActivity]'s own `--ez selftest_bt true` hook (a sibling of,
        // and deliberately not folded into, its `--ez selftest true` for
        // the §13 vectors — see that hook's own doc), returning null on a
        // clean pass or a first-failure description otherwise. Every
        // expected byte below
        // was hand-traced against this file's actual algorithm, not
        // guessed — see the task report for the arithmetic.
        private fun kbmArrWithKeys(vararg usages: Int): IntArray {
            val arr = IntArray(AtticPadNative.IN_LEN)
            for (u in usages) {
                val word = u / 32
                val bit = u % 32
                arr[AtticPadNative.IN_KB_KEYS0 + word] = arr[AtticPadNative.IN_KB_KEYS0 + word] or (1 shl bit)
            }
            return arr
        }

        private fun kbmArrWithMedia(held: Int): IntArray {
            val arr = IntArray(AtticPadNative.IN_LEN)
            arr[AtticPadNative.IN_MEDIA_HELD] = held
            return arr
        }

        private fun bytesEqual(a: ByteArray, b: List<Int>): Boolean =
            a.size == b.size && a.indices.all { (a[it].toInt() and 0xFF) == (b[it] and 0xFF) }

        /** [context] is used ONLY to construct one real, disposable
         *  [BtHidController] probe for the no-touch-pump case below, which
         *  needs to exercise the ACTUAL private [buildMouseReport] and its
         *  real [mouseStateLock]-guarded instance fields, not a
         *  hand-simulated stand-in for them — constructing it touches no
         *  Bluetooth API (the constructor only reads [BluetoothManager] via
         *  [Context.getSystemService], never registers or streams).
         *  @return total/passed/failed in [counts], first failure description
         *  or null if every case passed. */
        fun selfTestReportBuilders(counts: IntArray, context: Context): String? {
            var total = 0; var passed = 0; var firstFailure: String? = null
            fun check(name: String, actual: ByteArray, expected: List<Int>) {
                total++
                if (bytesEqual(actual, expected)) {
                    passed++
                } else if (firstFailure == null) {
                    firstFailure = "$name: expected $expected got ${actual.map { it.toInt() and 0xFF }}"
                }
            }
            fun checkDelta(name: String, actual: Pair<Int, Int>, expected: Pair<Int, Int>) {
                total++
                if (actual == expected) passed++ else if (firstFailure == null) {
                    firstFailure = "$name: expected $expected got $actual"
                }
            }
            fun checkTrue(name: String, condition: Boolean) {
                total++
                if (condition) passed++ else if (firstFailure == null) {
                    firstFailure = "$name: expected true"
                }
            }

            // -- gamepad (regression: the Report ID insertion must not have
            // changed a single byte buildReport itself produces) --
            check("gamepad/neutral", buildReport(IntArray(AtticPadNative.IN_LEN)), listOf(0, 0, 8, 0, 0, 0, 0, 0, 0))
            check(
                "gamepad/BTN_A",
                buildReport(IntArray(AtticPadNative.IN_LEN).also { it[AtticPadNative.IN_BUTTONS] = AtticPadNative.BTN_A }),
                listOf(1, 0, 8, 0, 0, 0, 0, 0, 0),
            )
            check(
                "gamepad/dpad-up",
                buildReport(IntArray(AtticPadNative.IN_LEN).also { it[AtticPadNative.IN_BUTTONS] = AtticPadNative.BTN_DPAD_UP }),
                listOf(0, 0, 0, 0, 0, 0, 0, 0, 0),
            )
            check(
                "gamepad/stick-full-right",
                buildReport(
                    IntArray(AtticPadNative.IN_LEN).also {
                        it[AtticPadNative.IN_AXIS0 + AtticPadNative.AXIS_LX] = 32767
                    },
                ),
                listOf(0, 0, 8, 127, 0, 0, 0, 0, 0),
            )

            // -- keyboard --
            check("keyboard/none", buildKeyboardReport(IntArray(AtticPadNative.IN_LEN)), listOf(0, 0, 0, 0, 0, 0, 0, 0))
            check("keyboard/A", buildKeyboardReport(kbmArrWithKeys(0x04)), listOf(0, 0, 4, 0, 0, 0, 0, 0))
            check(
                "keyboard/shift+A",
                buildKeyboardReport(kbmArrWithKeys(0xE1, 0x04)),
                listOf(2, 0, 4, 0, 0, 0, 0, 0),
            )
            check(
                "keyboard/7-keys-caps-at-6",
                buildKeyboardReport(kbmArrWithKeys(0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A)),
                listOf(0, 0, 4, 5, 6, 7, 8, 9),
            )

            // -- consumer control --
            check("consumer/none", buildConsumerReport(IntArray(AtticPadNative.IN_LEN)), listOf(0, 0))
            check(
                "consumer/play_pause",
                buildConsumerReport(kbmArrWithMedia(AtticPadNative.mediaBit(AtticPadNative.MEDIA_PLAY_PAUSE))),
                listOf(0xCD, 0x00),
            )
            check(
                "consumer/volume_up",
                buildConsumerReport(kbmArrWithMedia(AtticPadNative.mediaBit(AtticPadNative.MEDIA_VOLUME_UP))),
                listOf(0xE9, 0x00),
            )
            check(
                "consumer/lowest-index-wins",
                buildConsumerReport(
                    kbmArrWithMedia(
                        AtticPadNative.mediaBit(AtticPadNative.MEDIA_PLAY_PAUSE) or
                            AtticPadNative.mediaBit(AtticPadNative.MEDIA_STOP),
                    ),
                ),
                listOf(0xCD, 0x00),
            )

            // -- mouse delta tracking --
            checkDelta("mouse/simple", consumeMouseDelta(5, 0), 5 to 5)
            checkDelta("mouse/negative", consumeMouseDelta(0, 5), -5 to 0)
            checkDelta("mouse/wrap", consumeMouseDelta(2, 65534), 4 to 2)
            checkDelta("mouse/clamp-positive-1", consumeMouseDelta(200, 0), 127 to 127)
            checkDelta("mouse/clamp-positive-carry", consumeMouseDelta(200, 127), 73 to 200)
            checkDelta("mouse/clamp-negative-1", consumeMouseDelta(0, 200), -127 to 73)
            checkDelta("mouse/clamp-negative-carry", consumeMouseDelta(0, 73), -73 to 0)

            // -- mouse baseline (§6.16 rule 1) — pins the real 2026-08-26
            // hardware bug: an earlier onConnectionStateChanged reset the
            // "last sent" side to a literal 0 instead of to KbmSnapshot's
            // own current accumulator value, so ANY reconnect that followed
            // ordinary use replayed the entire historical drag as one
            // clamped-and-carried burst the instant MOUSE mode started
            // streaming — the host's cursor flung off-screen. This exercises
            // the ACTUAL KbmSnapshot.mouseAccumSnapshot() the fix calls, not
            // a re-implementation of it. --
            run {
                val kbm = KbmSnapshot()
                // A realistic prior session: two drags, well past a single
                // report's ±127 range — exactly the shape that flings a
                // cursor off the host screen if diffed against a false 0.
                kbm.mouseMove(300, -450)
                kbm.mouseMove(1200, 900)
                val baseline = kbm.mouseAccumSnapshot()
                // dxAccum = 300 + 1200 = 1500.
                // dyAccum = (-450 + 900) mod 65536 = 450 (wraps once via -450
                // and 0xFFFF = 65086, then +900 wraps back down to 450).
                checkTrue("mouse/baseline-setup-dx", baseline[0] == 1500)
                checkTrue("mouse/baseline-setup-dy", baseline[1] == 450)

                // THE FIX: resetting "last sent" to the current accumulator
                // (what onConnectionStateChanged now does) makes the very
                // next diff zero motion, regardless of the accumulator's
                // absolute size — exactly §6.16 rule 1's requirement.
                checkDelta(
                    "mouse/baseline-establishes-zero-motion-x",
                    consumeMouseDelta(baseline[0], baseline[0]),
                    0 to baseline[0],
                )
                checkDelta(
                    "mouse/baseline-establishes-zero-motion-y",
                    consumeMouseDelta(baseline[1], baseline[1]),
                    0 to baseline[1],
                )

                // THE BUG, pinned directly: diffing the SAME post-drag
                // accumulator against a literal 0 (what the old code did)
                // produces a maxed +127 report — not zero motion — the
                // first of a burst that keeps maxing out until the 1500
                // counts drain, which is the fling. If this ever starts
                // returning 0, consumeMouseDelta's own contract changed
                // underneath the fix and the regression guard above is no
                // longer meaningful.
                checkDelta(
                    "mouse/literal-zero-baseline-was-the-bug",
                    consumeMouseDelta(baseline[0], 0),
                    127 to 127,
                )
            }

            // -- negative real drag through the ACTUAL KbmSnapshot pipeline
            // (2026-08-26, cursor-pinned-to-corner report) — pins the
            // specific hypothesis that a small NEGATIVE finger drag, once
            // it has gone through [KbmSnapshot.mouseMove]'s `and 0xFFFF`
            // wrap-mask and back out through [mouseAccumSnapshot], could
            // come back diffed as a large POSITIVE value instead of the
            // small negative one a real drag toward up-left actually was
            // -- exactly the failure mode that would explain a cursor
            // shoved into the BOTTOM-RIGHT corner (a report claiming +X/+Y
            // when the finger's own motion was -X/-Y). Goes through the
            // REAL [KbmSnapshot] object end to end, not a hand-picked
            // IntArray, unlike the checks above -- if [mouseMove]'s masking
            // or [mouseAccumSnapshot]'s read ever reintroduces a sign bug,
            // this is the case that catches it, not the synthetic ones. --
            run {
                val kbm = KbmSnapshot()
                val baseline = kbm.mouseAccumSnapshot() // fresh: [0, 0, 0, 0]
                kbm.mouseMove(-40, -60) // up-left drag, same shape TrackpadView emits
                val after = kbm.mouseAccumSnapshot()
                val (dx, _) = consumeMouseDelta(after[0], baseline[0])
                val (dy, _) = consumeMouseDelta(after[1], baseline[1])
                checkTrue("mouse/negative-real-drag-dx-stays-negative", dx == -40)
                checkTrue("mouse/negative-real-drag-dy-stays-negative", dy == -60)
            }

            // -- no-touch-at-all pump, KEYBOARD-mode shape, through a REAL
            // BtHidController's REAL buildMouseReport (2026-08-26,
            // corrected report: "cursor still driven with no touch input at
            // all" in KEYBOARD/MEDIA, where the trackpad is incidental, and
            // survives the already-merged baseline-seed fix) — this is
            // deliberately NOT a hand-simulated diff loop like the case
            // above: it constructs a real, disposable [BtHidController]
            // ([context], never registered/connected — the constructor
            // touches no Bluetooth API) and calls the PRIVATE
            // [buildMouseReport] directly (a companion object can reach a
            // private instance member of its own enclosing class), so this
            // exercises the exact [mouseStateLock]-guarded
            // [mouseDxSent]/[mouseDySent] fields streamLoop itself mutates,
            // not a copy of their logic. The connect-time baseline seed is
            // reproduced by hand here (the same two lines
            // [onConnectionStateChanged] runs, since that callback is not
            // itself callable without a live BluetoothDevice) — if that
            // seed and [buildMouseReport] ever drift apart from each other
            // silently, this is positioned to catch it, unlike the
            // hand-simulated case above. [applyKbmActivity]'s own
            // KEYBOARD-mode shape is reproduced exactly:
            // `setMouseActive(true)` AND `setKeyboardActive(true)` together
            // (KbmModeBar.kt: KEYBOARD activates BOTH, not mouse alone).
            // Every one of 50 pumps must diff to zero; the first nonzero
            // pump, if any, is reported by index so a failure here points
            // straight at which pump broke it. --
            run {
                val kbm = KbmSnapshot()
                kbm.setMouseActive(true)
                kbm.setKeyboardActive(true) // KEYBOARD mode's real shape
                val probe = BtHidController(context, InputSnapshot(), kbm)
                // Mirrors onConnectionStateChanged's own baseline seed
                // exactly (kbm.mouseAccumSnapshot() -> the four mouseXSent
                // fields) — see that callback's own doc for why it reads
                // the accumulator rather than resetting to a literal 0.
                val baseline = kbm.mouseAccumSnapshot()
                probe.mouseDxSent = baseline[0]
                probe.mouseDySent = baseline[1]
                probe.mouseWheelSent = baseline[2]
                probe.mouseHWheelSent = baseline[3]
                var firstBadPump = -1
                for (pump in 0 until 50) {
                    val arr = IntArray(AtticPadNative.IN_LEN)
                    InputSnapshot().readInto(arr) // gamepad prefix, exactly as currentSnapshot() builds it
                    kbm.readInto(arr) // no mouseMove() anywhere in this loop -- no touch input at all
                    val report = probe.buildMouseReport(arr)
                    val dx = report[1].toInt()
                    val dy = report[2].toInt()
                    if ((dx != 0 || dy != 0) && firstBadPump < 0) firstBadPump = pump
                }
                checkTrue("mouse/no-touch-50-pumps-stay-zero (probe, first bad pump: $firstBadPump)", firstBadPump < 0)
            }

            counts[0] = total; counts[1] = passed; counts[2] = total - passed
            return firstFailure
        }
    }

    data class Status(
        val supported: Boolean = isSupported(),
        val registered: Boolean = false,
        val connectedName: String = "",
        /** The connected device's MAC, alongside [connectedName] — lets
         *  [MainActivity] remember which bonded device to try
         *  reconnecting to after a disconnect, without this class knowing
         *  anything about SharedPreferences or the guided-flow UI. */
        val connectedAddress: String = "",
        val streaming: Boolean = false,
        val reportsPerSec: Int = 0,
        /** True once registration is known to have failed — either
         *  [registerApp]'s own `registerApp()` call returned false, or the
         *  HID_DEVICE profile service disconnected out from under a live
         *  registration. [MainActivity] shows the ONE supplied
         *  failure string for this ("This phone's Bluetooth doesn't support
         *  controller mode.") rather than [detail] — [detail] is for the
         *  screen's "Details" expander and logcat only. */
        val registrationFailed: Boolean = false,
        /** Developer/bug-report detail only — the exact string a
         *  `registerApp`/profile callback handed back. NEVER shown directly
         *  on screen: [MainActivity] only surfaces this behind a
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
         *  section for why this exists. [MainActivity] renders the
         *  ONE supplied banner string for this state, not [detail]. */
        val bootProtocol: Boolean = false,
    )

    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor()

    @Volatile var status: Status = Status(); private set
    @Volatile private var listener: ((Status) -> Unit)? = null

    /**
     * One-pass hardware diagnostic for the mouse-delta path, OFF by default
     * (see [MainActivity]'s `--ez bt_mouse_debug true` launch hook, the
     * only place that sets this to true). When true, [buildMouseReport]
     * logs one line per outgoing MOUSE report — `TAG`'s own tag, message
     * prefixed `mouse-report` so `adb logcat -s BtHidController` and a grep
     * for that prefix isolates it — showing the raw accumulator this pump
     * read, the "last sent" value it diffed against BEFORE this report, the
     * clamped delta actually emitted, and the "last sent" value carried
     * forward. Run it once across exactly the failure window (connect, or
     * switch into MOUSE mode, drag briefly) and read the FIRST few lines: a
     * healthy stream's first line after a fresh baseline reads
     * `delta=0`, and every later line's delta stays small and matches the
     * finger's actual motion. The bug this file fixed (see
     * [onConnectionStateChanged]'s own doc) would have shown up here as
     * `delta=127` (or -127) on MANY consecutive lines right after connect
     * or after re-entering MOUSE mode, with `accum` far larger than
     * `sentBefore` — i.e. a burst of maxed-out deltas draining a stale
     * baseline, not real finger motion. Deliberately NOT left on by
     * default: at ~60 Hz this is a genuinely chatty log and docs/CONVENTIONS.md's own
     * "diagnostics hidden behind gestures/flags" rule applies here as much
     * as anywhere else in this app.
     *
     * TEMPORARY, 2026-08-26 (cursor-pinned-to-corner investigation): also
     * gates a second, coarser log in [streamLoop] — one line per report
     * PER CYCLE, showing the report ID, the raw bytes actually handed to
     * `sendReport` in hex, and that call's own boolean return — to answer
     * "does the mouse sendReport succeed or fail, and what bytes leave the
     * phone under Report ID 3" without guessing from the reports/sec
     * counter alone. MUST be removed (or at minimum re-gated so it cannot
     * ship on) before this investigation's branch merges — a permanent
     * per-report hex dump at 60 Hz is not shippable, same reasoning as the
     * rest of this doc comment.
     */
    @Volatile var mouseDebugLog: Boolean = false

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

    // ---- mouse delta-tracking state ---------------------------------------
    //
    // The last ACCUMULATOR value (KbmSnapshot's own free-running, wrapping
    // convention) this class has already turned into a wire delta — see
    // consumeMouseDelta's own doc. Reset on every fresh connection
    // (onConnectionStateChanged) to the snapshot's CURRENT accumulator value
    // (via [KbmSnapshot.mouseAccumSnapshot]) — see that call site's own doc
    // for why an earlier version of this reset to a literal 0 instead, and
    // why that was the cursor-vanishes-on-MOUSE-mode bug: §6.16 rule 1
    // requires the first post-baseline report to carry zero motion, and a
    // literal-0 reset only did that by coincidence, on a connection that
    // happened to be the very first one this [KbmSnapshot] had ever seen.
    //
    // THREADING: unlike most of this class's state, these four fields are
    // mutated from TWO different threads — [streamLoop]'s own thread on
    // every normal pump, and the Bluetooth stack's callback `executor`
    // thread via [onGetReport]'s GET_REPORT reply path (and the baseline
    // reset in [onConnectionStateChanged], also on `executor`). Both paths
    // now go through [mouseStateLock] so a GET_REPORT landing mid-pump (or a
    // reconnect's baseline reset landing mid-pump) cannot interleave with
    // [buildMouseReport]'s own read-modify-write of these fields and produce
    // a lost update or a stale baseline read.
    private val mouseStateLock = Any()

    /** Corner-pin experiment (2026-08-26): whether the last mouse report we
     *  actually transmitted was all-zero, so the stream can fall silent while
     *  idle the way a real mouse does, yet still deliver the single zero
     *  report that carries a button release. Touched only by streamLoop. */
    private var lastMouseReportWasIdle = false
    private var mouseDxSent = 0
    private var mouseDySent = 0
    private var mouseWheelSent = 0
    private var mouseHWheelSent = 0

    private fun buildMouseReport(kbmArr: IntArray): ByteArray = synchronized(mouseStateLock) {
        val buttons = kbmArr[AtticPadNative.IN_MOUSE_BUTTONS] and 0x1F
        val dxAccum = kbmArr[AtticPadNative.IN_MOUSE_DX]
        val dyAccum = kbmArr[AtticPadNative.IN_MOUSE_DY]
        val dxSentBefore = mouseDxSent
        val dySentBefore = mouseDySent
        val (dx, newDxSent) = consumeMouseDelta(dxAccum, mouseDxSent)
        val (dy, newDySent) = consumeMouseDelta(dyAccum, mouseDySent)
        val (wheel, newWheelSent) = consumeMouseDelta(kbmArr[AtticPadNative.IN_MOUSE_WHEEL], mouseWheelSent)
        val (hwheel, newHWheelSent) = consumeMouseDelta(kbmArr[AtticPadNative.IN_MOUSE_HWHEEL], mouseHWheelSent)
        mouseDxSent = newDxSent
        mouseDySent = newDySent
        mouseWheelSent = newWheelSent
        mouseHWheelSent = newHWheelSent
        // See [mouseDebugLog]'s own doc for exactly what to read off this
        // line and why. Guarded on the flag first so a normal run pays
        // nothing but one volatile-field read per pump.
        if (mouseDebugLog) {
            Log.d(
                TAG,
                "mouse-report x: accum=$dxAccum sentBefore=$dxSentBefore delta=$dx sentAfter=$newDxSent" +
                    " | y: accum=$dyAccum sentBefore=$dySentBefore delta=$dy sentAfter=$newDySent",
            )
        }
        byteArrayOf(buttons.toByte(), dx.toByte(), dy.toByte(), wheel.toByte(), hwheel.toByte())
    }

    fun setListener(l: ((Status) -> Unit)?) {
        listener = l
        l?.invoke(status)
    }

    private fun publish(s: Status) {
        status = s
        main.post { listener?.invoke(s) }
    }

    val bluetoothEnabled: Boolean get() = adapter?.isEnabled == true

    /** Bonded devices — unchanged in shape from the original "no scanning"
     *  version (see [startDiscovery]/[createBond] below for what changed,
     *  2026-08-26). Pairing itself can now start from either side, but
     *  either direction ends the same way: a bonded [BluetoothDevice] that
     *  shows up here via `BluetoothAdapter.getBondedDevices()`, which does
     *  not care which side called `createBond()`. */
    @Suppress("MissingPermission") // BLUETOOTH_CONNECT — caller gates entry on the permission
    fun bondedDevices(): List<BluetoothDevice> =
        adapter?.bondedDevices?.toList() ?: emptyList()

    /** Phone-initiated inquiry scan (`BluetoothAdapter.startDiscovery()`),
     *  added 2026-08-26 alongside [createBond] so pairing no longer has to
     *  be driven from the PC's own Bluetooth settings — see
     *  [MainActivity]'s "Nearby devices" list. The original "this mode
     *  does not scan" design (see the class doc's history and
     *  `ralismark/bluehid`/CTS Verifier's own HID device test, neither of
     *  which scans either) was protecting two things: staying on the
     *  cheapest permission set (no `BLUETOOTH_SCAN`), and following the
     *  convention that a HID *device* role is normally the one being
     *  connected TO, not the one initiating discovery. Neither holds as an
     *  argument against this feature — `BLUETOOTH_SCAN` is requested
     *  through the exact same runtime-permission flow `BLUETOOTH_CONNECT`/
     *  `BLUETOOTH_ADVERTISE` already use (see [MainActivity]'s
     *  `beginBtPermissionFlow`), and general BR/EDR discovery/bonding is a
     *  link-layer operation that does not know or care which side later
     *  registers as a HID peripheral — this class already calls
     *  [connectTo] (`BluetoothHidDevice.connect(BluetoothDevice)`) for the
     *  reconnect-to-last-host path, i.e. device-initiated HID *connection*
     *  was already a thing this app did before device-initiated *pairing*
     *  joined it. What is still true, and still worth restating: a real
     *  host's own Bluetooth stack decides whether to accept an incoming
     *  device-initiated pairing request at all — see the task report for
     *  exactly how far this got verified. */
    @Suppress("MissingPermission") // BLUETOOTH_SCAN (API31+) / BLUETOOTH_ADMIN — caller gates entry
    fun startDiscovery(): Boolean =
        try {
            adapter?.startDiscovery() == true
        } catch (e: SecurityException) {
            // @Suppress("MissingPermission") only silences the lint check —
            // the platform's own runtime check still throws if the caller's
            // BLUETOOTH_SCAN grant raced with this call (revoked from
            // Settings mid-session, or — the bug this catch block was added
            // for — a caller that has not actually secured the permission
            // yet calling this anyway). Caught, not just suppressed, so a
            // permission-gating bug on the [MainActivity] side is a no-op
            // here instead of a crash.
            Log.w(TAG, "startDiscovery() denied: ${e.message}")
            false
        }

    /** Always safe to call, including when no discovery is running —
     *  `BluetoothAdapter.cancelDiscovery()`'s own contract. [MainActivity]
     *  calls this on every one of its three stop triggers (leaving the
     *  Bluetooth setup screen, a bond starting, `onDestroy`) — including
     *  paths that can run before BLUETOOTH_SCAN was ever granted (e.g. a
     *  permission request the user has not answered yet), so this needs the
     *  same defensive catch [startDiscovery] has, not just its own
     *  `@Suppress`. */
    @Suppress("MissingPermission")
    fun cancelDiscovery() {
        try {
            adapter?.cancelDiscovery()
        } catch (e: SecurityException) {
            Log.w(TAG, "cancelDiscovery() denied: ${e.message}")
        }
    }

    /** General BR/EDR pairing (`BluetoothDevice.createBond()`) — not a HID
     *  API at all, and not gated on this class's own registration state;
     *  see [startDiscovery]'s own doc for why that is fine for a HID
     *  peripheral to call. Result arrives asynchronously via
     *  `BluetoothDevice.ACTION_BOND_STATE_CHANGED`, which [MainActivity]
     *  already listens for. */
    @Suppress("MissingPermission")
    fun createBond(device: BluetoothDevice): Boolean =
        try {
            device.createBond()
        } catch (e: SecurityException) {
            Log.w(TAG, "createBond() denied: ${e.message}")
            false
        }

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
                    // §6.16 rule 1 (docs/PROTOCOL.md): "the first accepted
                    // MOUSE of a session... establishes the baseline and
                    // MUST produce zero motion." [BluetoothTransport] keeps
                    // ONE [KbmSnapshot] for the whole Activity's life, so
                    // [kbm]'s dx/dy/wheel/hwheel accumulators are NOT 0 here
                    // on anything but the very first connect ever — any
                    // earlier drag in an earlier BT connection (or earlier
                    // visit to MOUSE mode in this one) already advanced them
                    // and they are never reset by [KbmSnapshot.setMouseActive]
                    // (see its own doc: only `mouseButtons`/the button queue
                    // clear, the four accumulators do not, on purpose,
                    // matching the wire's own free-running convention).
                    // Resetting the SENT side to a literal 0 instead of to
                    // the CURRENT accumulator value was the bug: the first
                    // post-connect report then diffed a real accumulator
                    // (e.g. in the thousands, after ordinary use) against a
                    // false baseline of 0, producing one session-spanning
                    // jump clamped to +-127 and carried across as many
                    // reports as it took to drain — a hard fling of the
                    // host's cursor the instant MOUSE mode started
                    // streaming. [KbmSnapshot.mouseAccumSnapshot] reads the
                    // four accumulators with none of [KbmSnapshot.readInto]'s
                    // queue-draining side effects, so calling it here, off
                    // the stream thread, races with nothing in [kbm] itself
                    // — [mouseStateLock] below is what keeps this write from
                    // interleaving with a concurrent [buildMouseReport] on
                    // the stream thread (see that lock's own doc).
                    val baseline = kbm.mouseAccumSnapshot()
                    synchronized(mouseStateLock) {
                        mouseDxSent = baseline[0]
                        mouseDySent = baseline[1]
                        mouseWheelSent = baseline[2]
                        mouseHWheelSent = baseline[3]
                    }
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
        // AFTERWARDS: stop emitting every application report while boot
        // mode is active. This class's own "keyboard/mouse/media joined
        // this device" doc spells out why that STILL applies even to the
        // now boot-shaped keyboard report: Report IDs mean an ID byte rides
        // along regardless of boot mode, which a strict boot-mode host was
        // never expecting either. See [streamLoop]'s gate on
        // [bootProtocolActive].
        override fun onSetProtocol(device: BluetoothDevice, protocol: Byte) {
            val boot = protocol == BluetoothHidDevice.PROTOCOL_BOOT_MODE
            // Logcat/detail only — [MainActivity] renders the ONE
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
            // No FEATURE report declared on any of the four collections —
            // reply with whichever INPUT report [id] names (falling back to
            // the gamepad one for an unrecognised id) so a strict host
            // GET_REPORT probe still gets an answer instead of silence.
            Log.i(TAG, "onGetReport type=$type id=$id bufferSize=$bufferSize")
            val arr = currentSnapshot()
            val report = when (id.toInt()) {
                REPORT_ID_KEYBOARD -> buildKeyboardReport(arr)
                REPORT_ID_MOUSE -> buildMouseReport(arr)
                REPORT_ID_CONSUMER -> buildConsumerReport(arr)
                else -> buildReport(arr)
            }
            @Suppress("MissingPermission")
            hidDevice?.replyReport(device, type, id, report)
        }
    }

    /** [AtticPadNative.IN_LEN]-length snapshot, exactly as
     *  [AtticPadService]'s own UDP session thread fills it
     *  (`input.readInto(inArr); kbm.readInto(inArr)`, same order) — the
     *  gamepad prefix (indices `0..IN_KBM_PRESENT-1`) and the KBM suffix
     *  live in ONE array precisely so a consumer that wants both (this
     *  class, now) reads them off the same object [InputSnapshot] and
     *  [KbmSnapshot] were always designed to share. */
    private fun currentSnapshot(): IntArray {
        val arr = IntArray(AtticPadNative.IN_LEN)
        input.readInto(arr)
        kbm.readInto(arr)
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
        // device" dialog shows, and MainActivity's step-2
        // instruction text tells the user to look for and choose exactly
        // this name (task brief copy: "Choose AtticPad").
        val sdp = BluetoothHidDeviceAppSdpSettings(
            "AtticPad",
            "AtticPad Bluetooth controller",
            "AtticPad",
            // SUBCLASS1_COMBO | SUBCLASS2_GAMEPAD — a keyboard/pointing
            // COMBO that is also a gamepad. These constants live on
            // BluetoothHidDevice itself, not on AppSdpSettings — confirmed
            // against AOSP `packages/modules/Bluetooth` source, since
            // neither the SDK reference page nor most blog posts mention
            // them at all.
            //
            // Was SUBCLASS1_NONE, which said "gamepad, and nothing else".
            // That was true when this class only had a gamepad collection.
            // It stopped being true the moment keyboard, mouse and consumer
            // collections joined the descriptor, and the device carried on
            // telling every host it was a gamepad while sending pointer
            // reports.
            //
            // This change was made while chasing the corner-pin bug (a
            // Windows host's cursor driven into a screen corner by any
            // pointer motion). IT DID NOT FIX IT, and the reasoning that
            // motivated it does not stand up -- recorded here in full
            // because the correction matters more than the guess.
            //
            // The theory was: BlueZ ignores the SDP subclass and parses the
            // report descriptor, so Linux decoded our mouse correctly while
            // Windows trusted the class and set up a gamepad. The Linux half
            // of that was never actually established. On the machine used as
            // the control host, `/proc/bus/input/devices` only ever showed
            // "<phone name> (AVRCP)" -- the audio profile. A HID input device
            // was never observed attaching, so no Linux host was ever seen
            // decoding these reports at all, correctly or otherwise.
            //
            // Everything else measurable was checked on real hardware and is
            // CORRECT: the bytes we send, and Windows' own parse read back
            // over SSH -- Col03 enumerates as a mouhid mouse with
            // ForceAbsolute 0, InputReportByteLength 6, and Y on ReportID 3
            // with IsAbsolute False and LogicalMin/Max -127/127. Nothing on
            // that host maps a controller to the cursor. The cause is still
            // unknown; do not treat "it is Windows-specific" as established.
            //
            // The change is kept anyway because the declaration is simply
            // true where the old one was false, which is reason enough.
            //
            // Note this is NOT the `ATTR_ID_HID_BOOT_DEVICE` question from
            // the "repeated e" section of this class's doc. That attribute
            // is written as a hardcoded `bool_true` by `HID_DevAddRecord`
            // with no reference to `subclass`, so this byte still cannot
            // reach it. Two different SDP problems; only this one is
            // addressable from here.
            //
            // CHANGING THIS CHANGES THE SDP RECORD, so an already-paired
            // host must remove and re-add the device to see it.
            (BluetoothHidDevice.SUBCLASS1_COMBO.toInt() or
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

    /** TEMPORARY — see [mouseDebugLog]'s own doc. One line per report per
     *  cycle: report ID, `sendReport`'s own boolean return, and the exact
     *  bytes handed to it in hex, so a hardware capture can answer "does
     *  the mouse send succeed, and what bytes actually left the phone"
     *  without inferring it from the reports/sec counter. Gated on
     *  [mouseDebugLog] so it costs nothing when off. */
    private fun logSendResult(id: Int, report: ByteArray, ok: Boolean) {
        if (!mouseDebugLog) return
        val hex = report.joinToString(" ") { "%02x".format(it) }
        Log.d(TAG, "send-report id=$id ok=$ok bytes=[$hex]")
    }

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
            // see onSetProtocol's doc comment, and this class's own
            // "keyboard/mouse/media joined this device" doc for why ALL
            // FOUR report types hold, not just the gamepad one, even now
            // that the keyboard report is boot-shaped.
            if (device != null && hid != null && !bootProtocolActive) {
                val arr = currentSnapshot()
                val have = arr[AtticPadNative.IN_KBM_PRESENT]
                val gamepadReport = buildReport(arr)
                val gamepadOk = hid.sendReport(device, REPORT_ID_GAMEPAD, gamepadReport)
                if (gamepadOk) sent++
                logSendResult(REPORT_ID_GAMEPAD, gamepadReport, gamepadOk)
                if (have and AtticPadNative.KBM_FEATURE_KEYBOARD != 0) {
                    val r = buildKeyboardReport(arr)
                    val ok = hid.sendReport(device, REPORT_ID_KEYBOARD, r)
                    if (ok) sent++
                    logSendResult(REPORT_ID_KEYBOARD, r, ok)
                }
                if (have and AtticPadNative.KBM_FEATURE_MOUSE != 0) {
                    val r = buildMouseReport(arr)
                    // EXPERIMENT (corner-pin bug, 2026-08-26): emit the mouse
                    // report only when it says something.
                    //
                    // A real mouse is silent when it is not moving. This was
                    // streaming an all-zero report ~45 times a second for as
                    // long as a trackpad-bearing mode was open, which is both
                    // pointless and the one behaviour that starts exactly when
                    // the user switches into MOUSE/KEYBOARD/MEDIA -- the
                    // moment the pin appears.
                    //
                    // The gamepad report is NOT the cause: suppressing it
                    // entirely left the pin unchanged, so it is restored
                    // above and this is the only variable that moves.
                    //
                    // "Idle" means every byte zero: no buttons, no motion, no
                    // wheel. A release still transmits, because the report
                    // carrying the button-up is nonzero in its button byte on
                    // the way down and the following all-zero one is the
                    // release -- so the first zero report after any nonzero
                    // one must still go, or a button would stick down.
                    val idle = r.all { it.toInt() == 0 }
                    if (!idle || !lastMouseReportWasIdle) {
                        val ok = hid.sendReport(device, REPORT_ID_MOUSE, r)
                        if (ok) sent++
                        logSendResult(REPORT_ID_MOUSE, r, ok)
                    }
                    lastMouseReportWasIdle = idle
                }
                if (have and AtticPadNative.KBM_FEATURE_MEDIA != 0) {
                    val r = buildConsumerReport(arr)
                    val ok = hid.sendReport(device, REPORT_ID_CONSUMER, r)
                    if (ok) sent++
                    logSendResult(REPORT_ID_CONSUMER, r, ok)
                }
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
