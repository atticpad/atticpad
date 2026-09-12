package net.atticpad

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.SurfaceTexture
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.app.Activity
import android.app.AlertDialog
import android.util.Log
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.TextureView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher

/**
 * The only Activity, and the ONE place a session's PAD/MOUSE/KEYBOARD/MEDIA
 * overlay is built. For UDP the session, the socket and the WifiLock all
 * live in [AtticPadService], so nothing here dying takes input down with it
 * (docs/DESIGN.md §7.2); for Bluetooth there is no server and no service — see
 * [BluetoothTransport] — but the SAME overlay ([padView]/[mouseBody]/
 * [keyboardBody]/[mediaBody]/[modeBar]) renders it, bound through
 * [SessionTransport] instead of a second copy of the UI. [activeTransport]
 * is which one (if either) is currently live; see [enterSession]/
 * [exitSession].
 *
 * There used to be a second Activity, `BtControllerActivity`, that rebuilt
 * this same overlay end to end around [BtHidController] instead — a
 * genuine duplicate, not a variant, since every view and every insets/
 * orientation calculation in it matched this file's by design ("the same
 * UI, a different connection" — KbmModeBar.kt's own file doc, which is
 * still true, just resolved differently now: instead of two Activities
 * each holding one copy of the UI bound to one transport, there is one
 * Activity holding one copy of the UI and choosing which transport feeds
 * it). What made that a real merge and not just a rename: [SessionTransport]
 * is the entire interface the overlay needs, and neither [PadView] nor any
 * KBM body has ever cared which object implements it (see that interface's
 * own doc). What did NOT merge, on purpose: Bluetooth's own pairing/
 * permission flow ([buildBtSetupPanel] and everything under it) stays its
 * own screen, reached from the connect screen instead of a separate
 * Activity (point 3 of the task brief: "transport is chosen in the connect
 * flow, not by launching a different screen") — its guided steps have
 * nothing in common with a UDP address/PIN form, and forcing them into one
 * shape would have been the same mistake in the other direction.
 *
 * UI is built in code rather than XML, and uses no AndroidX. Not minimalism
 * for its own sake — the phone under test is not attached to this machine, so
 * every dependency and every resource-resolution step is a failure mode that
 * can only be diagnosed at arm's length.
 */
class MainActivity : Activity() {

    companion object {
        private const val NOTIFICATION_PERMISSION_REQUEST = 1
        private const val CAMERA_PERMISSION_REQUEST = 2
        private const val BT_PERMISSION_REQUEST = 10

        /** How long a run of taps on the connect screen's title is allowed
         *  to take before it stops counting as one gesture — see
         *  [onTitleTap]. Generous enough for a deliberate triple-tap,
         *  tight enough that three ordinary, unrelated taps spread across
         *  a session never accidentally add up to one. */
        private const val TITLE_TAP_WINDOW_MS = 600L
        private const val TITLE_TAP_COUNT = 3

        /** [hud]'s edit-mode label — a constant so [showMenu]'s "Edit
         *  layout" toggle and [refreshBtHud]'s restore-after-boot-banner
         *  path can never drift apart (the same discipline
         *  [BOOT_PROTOCOL_BANNER] already follows). */
        private const val EDIT_MODE_HUD_TEXT = "EDIT MODE — drag controls, then use ☰ to finish"

        // ---- Bluetooth setup panel — ported from the deleted
        // BtControllerActivity verbatim; see buildBtSetupPanel.
        private const val PREF_LAST_HOST_NAME = "bt_last_host_name"
        private const val PREF_LAST_HOST_ADDRESS = "bt_last_host_address"

        /** Verbatim, task-brief-supplied copy — shown on screen exactly
         *  once, from exactly one constant, so the setup-screen and
         *  session-overlay banners can never drift apart. */
        private const val BOOT_PROTOCOL_BANNER =
            "This computer switched to a legacy mode this phone can't provide. " +
                "Input is paused - remove and re-pair the device on the PC, or connect over Wi-Fi instead."

        /** Verbatim, task-brief-supplied copy for the paired-devices list's
         *  empty state — see [refreshPairedDevices]. */
        private const val PAIRED_EMPTY_TEXT =
            "Nothing paired yet - tap Make discoverable, then add AtticPad from your PC's Bluetooth settings."

        /** Verbatim, task-brief-supplied copy for a manual connect attempt
         *  that times out — see [onConnectAttemptTimedOut]. */
        private const val PAIRED_CONNECT_FAILED_TEXT = "Couldn't connect - is Bluetooth on over there?"

        /** Shown under "Nearby devices" when [pairWithDiscoveredDevice]'s
         *  own `createBond()` resolves to `BOND_NONE` — a real host
         *  refusing/cancelling the pairing prompt, or the phone giving up,
         *  looks identical on this side (see [BtHidController.createBond]'s
         *  own doc: no stronger signal than the bond-state broadcast). */
        private const val PAIR_FAILED_TEXT = "Couldn't pair - try again from the PC's own Bluetooth settings."

        /** Verbatim, task-brief-supplied copy for the "Nearby devices"
         *  list's genuinely-empty state — shown only once discovery has
         *  finished at least one pass with nothing found; the SCANNING
         *  state itself is shown visually (see [btDiscoveringSpinner]), not
         *  with a sentence (task brief: "that is a state, not a
         *  sentence"). */
        private const val DISCOVERED_EMPTY_TEXT = "Nothing found nearby."

        /** How long a manual tap on a paired-devices row waits for
         *  [BtHidController.Status.connectedName] to go non-empty before
         *  giving up. [BluetoothHidDevice.connect] itself only reports
         *  whether the REQUEST was accepted, not whether the connection
         *  eventually succeeds — the real answer arrives (or never arrives)
         *  via the same [BtHidController.Status] stream every other
         *  connection-state change uses, so there is no stronger signal
         *  than "nothing happened yet" to build a timeout from. */
        private const val CONNECT_TIMEOUT_MS = 12_000L

        /** [BluetoothClass.Device.Major.AUDIO_VIDEO]'s own minor-class
         *  values that are plausibly a HID host rather than an audio sink —
         *  see [isPlausibleHidHost]'s own doc. `deviceClass` (not
         *  `majorDeviceClass`) is the full value these constants match, per
         *  `BluetoothClass.Device`'s own javadoc ("shifted to match the
         *  BluetoothClass value from getDeviceClass()"). Deliberately does
         *  NOT include AUDIO_VIDEO_LOUDSPEAKER, _HANDSFREE, _HEADPHONES,
         *  _WEARABLE_HEADSET, _PORTABLE_AUDIO, _CAR_AUDIO or _MICROPHONE —
         *  none of those can be a HID host, and this filter is what keeps
         *  e.g. bonded AirPods out of the list. */
        private val AUDIO_VIDEO_HID_HOST_MINORS = setOf(
            BluetoothClass.Device.AUDIO_VIDEO_VIDEO_MONITOR,
            BluetoothClass.Device.AUDIO_VIDEO_VIDEO_DISPLAY_AND_LOUDSPEAKER,
            BluetoothClass.Device.AUDIO_VIDEO_SET_TOP_BOX,
            BluetoothClass.Device.AUDIO_VIDEO_VIDEO_GAMING_TOY,
        )
    }

    private lateinit var root: FrameLayout
    private lateinit var connectPanel: View
    private lateinit var padView: PadView
    private lateinit var hud: TextView
    private lateinit var statusLine: TextView

    // ---- §6.15-6.19 keyboard/mouse/media mode bar ------------------------
    //
    // PAD/MOUSE/KEYBOARD/MEDIA are four full-screen sibling bodies sharing
    // exactly the FrameLayout-of-siblings pattern buildUi() already uses for
    // padView vs. connectPanel — the mode bar just swaps which one is
    // VISIBLE instead of a connect/session swap.
    // KbmMode, MouseBody, KeyboardBody, buildMediaBody, KbmChipRow,
    // styleModeChip and applyKbmActivity(KbmSnapshot?, ...) all live in
    // KbmModeBar.kt now — shared with BtControllerActivity's Bluetooth
    // session (job 2: "the same UI, a different connection"). Nothing in
    // this class builds its own copy of MOUSE/KEYBOARD/MEDIA any more.
    private var kbmMode = KbmMode.PAD
    private lateinit var modeBar: LinearLayout

    /** The `☰` control folded into [modeBar] (job 1: the diagnostics strip
     *  is gone, and the menu it used to open lives in the chip row now — as
     *  a control distinct from the four mode chips, not a fifth one; see
     *  [KbmChipRow] for the divider that sets it apart and [showMenu] for
     *  where the diagnostics moved to). */
    private lateinit var chipRow: KbmChipRow
    private lateinit var mouseBody: MouseBody
    private lateinit var keyboardBody: KeyboardBody
    private lateinit var mediaBody: MediaRemoteView

    // [mediaBody]'s own left/top/right/bottom padding at construction time,
    // captured once — see [updatePadViewInsets]'s bug fix note: a naive
    // `body.setPadding(insets...)` REPLACES all four sides (the same trap
    // [connectPanelBasePadding] exists for), and for [mediaBody] that
    // silently erased MediaRemoteView's own SPACE_MD/SPACE_SM padding.
    // [mouseBody]/[keyboardBody] carry the same capture as
    // `.basePadding` now (KbmModeBar.kt) — only [mediaBody] needs its own
    // field here, since it has no wrapper class of its own.
    private var mediaBodyBasePadding = intArrayOf(0, 0, 0, 0)
    private lateinit var discovered: LinearLayout
    private lateinit var ipField: EditText
    private lateinit var pinLabel: TextView
    private lateinit var pinField: EditText

    // §10.3 in-app scanner (QrScanner.kt) — now CARD 1, INLINE on the
    // connect screen (task brief: "scanning is the primary path"), not
    // behind a button. CAMERA is still requested only on a deliberate tap
    // (of scanActionButton, not at launch) — see refreshScannerCard().
    private lateinit var scanPreviewSlot: FrameLayout
    private lateinit var cameraPreview: TextureView
    private lateinit var reticleView: ReticleView
    private lateinit var torchToggle: ImageView
    private lateinit var scanPlaceholder: View
    private lateinit var scanMessageText: TextView
    private lateinit var scanActionButton: Button
    private val scanner by lazy { QrScanner(this) }

    /** True once [QrScanner.start] has been called and not yet [stopScanner]
     *  — guards [ensureScannerRunning] against reopening a camera that is
     *  already open (e.g. two `onResume`-adjacent calls). */
    private var scannerRunning = false
    private var torchOn = false

    /** State for [onTitleTap]'s "3 taps within [TITLE_TAP_WINDOW_MS]" self-test
     *  gesture. Survives [rebuildConnectPanel] on purpose — an Activity field,
     *  not a local inside [buildConnectPanel] — so a triple-tap started just
     *  before a rotation still resolves correctly rather than silently
     *  resetting mid-gesture. */
    private var titleTapCount = 0
    private var titleTapLastMs = 0L

    private var service: AtticPadService? = null
    private var bound = false

    /** Which [SessionTransport] the overlay is currently bound to and
     *  showing for — `null` whenever a connect/setup screen is showing
     *  instead. Set only by [enterSession]/[exitSession], never assigned
     *  directly elsewhere, so there is exactly one place that ever moves
     *  the overlay between "showing" and "hidden". `activeTransport === bt`
     *  is how BT-specific code (the menu, [dispatchKeyEvent]'s routing)
     *  tells which transport is live without a second enum to keep in
     *  sync. */
    private var activeTransport: SessionTransport? = null

    private lateinit var nsd: AtticPadNsd
    private var sensors: SensorInput? = null
    private val main = Handler(Looper.getMainLooper())

    /** Button bits currently held on a physical pad, by key event. */
    private var padKeyButtons = 0
    private var padHatButtons = 0

    private var autoConnectIp: String? = null
    private var autoConnectPort = 0
    private var autoConnectPin: String? = null
    private var lastLoggedState = -1

    // ---- Bluetooth transport (job 3: merged out of BtControllerActivity) -
    //
    // See SessionTransport.kt and BluetoothTransport.kt's own docs. `bt` is
    // null until the user taps "Use Bluetooth" for the first time — never
    // constructed on a device where BtHidController.isSupported() is false,
    // and never registered with the OS until beginBtPermissionFlow() runs,
    // which only happens from that same tap (never on ordinary launch —
    // task brief: "the same 'never unless a screen actually needs it' rule
    // CAMERA already follows").
    private var bt: BluetoothTransport? = null

    /** Set once, from [onCreate]'s `--ez bt_mouse_debug true` launch extra
     *  (see [ensureBluetoothTransport]) — captured here, not read again
     *  later, because [intent] can be replaced/consumed by other launch
     *  hooks before the user ever taps "Use Bluetooth". See
     *  [BtHidController.mouseDebugLog]'s own doc for what this turns on
     *  and exactly what to look for in logcat. */
    private var btMouseDebugLog = false

    /** Which of the two pre-session screens [buildUi] shows when neither
     *  transport has an open session — a pure navigation choice ("Connect
     *  to your PC" vs "Use Bluetooth"), independent of either transport's
     *  own connection state. See [refreshPreSessionScreen]. */
    private enum class PreSessionScreen { UDP, BLUETOOTH }
    private var preSessionScreen = PreSessionScreen.UDP

    // ---- Bluetooth setup panel state — ported verbatim from the deleted
    // BtControllerActivity; see buildBtSetupPanel and its own section doc.
    private lateinit var btSetupPanel: View
    private lateinit var btBootBanner: TextView
    private lateinit var btPermissionPanel: View
    private lateinit var btPermissionText: TextView
    private lateinit var btOpenSettingsButton: Button
    private lateinit var btPairingPanel: View
    private lateinit var btPairingHeadline: TextView
    private lateinit var btPairingInstruction: TextView
    private lateinit var btDiscoverableButton: Button
    private lateinit var btPairedListPanel: LinearLayout
    private lateinit var btPairedStatusText: TextView
    private lateinit var btOpenControllerButton: Button
    private lateinit var btDetailsToggle: TextView
    private lateinit var btDetailsText: TextView
    private var btRegisterRequested = false
    private var btLastConnectedName = ""
    private var btConnectionLost = false
    private var btReconnectAttempted = false
    private var btDetailsExpanded = false
    private var btConnectingAddress: String? = null
    private var btConnectTimeoutRunnable: Runnable? = null
    private var btReceiverRegistered = false
    private var btBackCallback: OnBackInvokedCallback? = null

    // ---- "Nearby devices" — phone-initiated discovery/pairing, added
    // 2026-08-26 alongside BtHidController.startDiscovery/createBond. A
    // sibling list to btPairedListPanel above, not a replacement — see
    // buildBtSetupPanel's own doc for why the two stay visually distinct
    // by heading alone.
    private lateinit var btDiscoveredHeading: TextView
    private lateinit var btDiscoveringSpinner: ProgressBar
    private lateinit var btDiscoveredListPanel: LinearLayout
    private lateinit var btDiscoveredStatusText: TextView

    /** Devices seen via `ACTION_FOUND` since discovery was last (re)armed,
     *  keyed by address so a repeat sighting of the same device (common —
     *  a real inquiry re-reports the same nearby device many times) updates
     *  in place instead of duplicating a row. Cleared whenever discovery
     *  fully stops (see [stopBtDiscovery]) so a stale sighting from a
     *  device that has since gone out of range does not linger forever. */
    private val btDiscoveredDevices = LinkedHashMap<String, BluetoothDevice>()

    /** True between `ACTION_DISCOVERY_STARTED` and `ACTION_DISCOVERY_FINISHED`
     *  — drives [btDiscoveringSpinner] only; see [DISCOVERED_EMPTY_TEXT]'s
     *  own doc for why the empty+scanning state is visual, not a sentence. */
    private var btDiscovering = false

    /** Whether this screen WANTS scanning to be running right now — a
     *  separate flag from [btDiscovering] because a single
     *  `startDiscovery()` burst only lasts ~12s (the Bluetooth inquiry
     *  window), and this screen wants scanning to hold for as long as the
     *  pairing panel itself is the one showing. [btStateReceiver]'s
     *  `ACTION_DISCOVERY_FINISHED` handler re-arms the next burst ONLY
     *  while this stays true — set false by every one of the three stop
     *  triggers the task brief calls out ([stopBtDiscovery]/
     *  [pairWithDiscoveredDevice]), which is what keeps cancelling
     *  discovery for a bond attempt from being immediately undone by that
     *  same cancel's own `ACTION_DISCOVERY_FINISHED` broadcast. */
    private var btDiscoveryWanted = false

    /** The address currently mid-`createBond()`, or null. Mirrors
     *  [btConnectingAddress]'s own shape one step earlier in the flow —
     *  see [pairWithDiscoveredDevice]. */
    private var btPairingAddress: String? = null

    private val btStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothAdapter.ACTION_SCAN_MODE_CHANGED -> {
                    val mode = intent.getIntExtra(BluetoothAdapter.EXTRA_SCAN_MODE, BluetoothAdapter.SCAN_MODE_NONE)
                    if (mode != BluetoothAdapter.SCAN_MODE_CONNECTABLE_DISCOVERABLE) refreshPairedDevices()
                }
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                    refreshPairedDevices()
                    refreshDiscoveredDevicesUi()
                    onBondStateChanged(intent)
                }
                BluetoothDevice.ACTION_FOUND -> {
                    val device = deviceExtra(intent) ?: return
                    btDiscoveredDevices[device.address] = device
                    refreshDiscoveredDevicesUi()
                }
                BluetoothAdapter.ACTION_DISCOVERY_STARTED -> {
                    btDiscovering = true
                    refreshDiscoveredDevicesUi()
                }
                BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> {
                    btDiscovering = false
                    refreshDiscoveredDevicesUi()
                    // Re-arm the next ~12s burst only while this screen still
                    // wants one running — see btDiscoveryWanted's own doc for
                    // why this must NOT fire back on immediately after
                    // pairWithDiscoveredDevice's own cancelDiscovery().
                    if (btDiscoveryWanted) bt?.controller?.startDiscovery()
                }
            }
        }
    }

    /** API 33 deprecated the single-arg `Intent.getParcelableExtra` in
     *  favour of a type-checked overload; this app's minSdk (26) is well
     *  below that, so both paths are needed — same discipline already
     *  applied elsewhere in this class for API-gated platform surface. */
    @Suppress("DEPRECATION")
    private fun deviceExtra(intent: Intent): BluetoothDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
        } else {
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
        }

    private val prefs by lazy { getSharedPreferences("atticpad", Context.MODE_PRIVATE) }

    // ---- orientation / insets -------------------------------------------
    //
    // The Activity is never recreated on rotate (manifest configChanges) —
    // that is what makes a live session survive one. This is the state that
    // makes the REST of the UI catch up, driven from onConfigurationChanged.
    private var currentOrientation: PadLayout.Orientation = PadLayout.Orientation.LANDSCAPE
    private var edgeInsets = Insets.Edges(0, 0, 0, 0)

    /** Not part of AtticPadService.Status (it is UI-only), so a connect-panel
     *  rebuild on rotation needs it kept somewhere to restore. */
    private var lastDiscovered: List<AtticPadNsd.Found> = emptyList()

    /** Whichever view inside [connectPanel] actually carries the
     *  design-system margins — the portrait column, or the landscape row —
     *  and [connectPanelBasePadding], that view's own left/top/right/bottom
     *  padding. Kept so [applyInsetsToViews] can ADD insets on top of it
     *  instead of overwriting it: `View.setPadding` replaces all four
     *  values, and a naive "just set it to the inset" call silently erased
     *  every margin the design called for whenever the inset was zero. */
    private lateinit var connectPanelContent: View
    private var connectPanelBasePadding = intArrayOf(0, 0, 0, 0)

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val svc = (binder as AtticPadService.LocalBinder).service
            service = svc
            bound = true
            padView.snapshot = svc.input
            mouseBody.kbm = svc.kbm
            keyboardBody.kbm = svc.kbm
            mediaBody.bindKbm(svc.kbm)
            sensors = SensorInput(this@MainActivity, svc.input).also { it.start() }
            pushBattery()
            svc.setListener { onStatus(it) }
            autoConnectIp?.let { ip ->
                autoConnectIp = null
                ipField.setText(ip)
                autoConnectPin?.let { pin ->
                    autoConnectPin = null
                    showPinField(true)
                    pinField.setText(pin)
                }
                connectTo(ip, autoConnectPort)
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
            bound = false
        }
    }

    // ---- lifecycle ------------------------------------------------------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Orientation is free in both directions now (manifest:
        // android:screenOrientation="fullSensor") — a gamepad is a landscape
        // object, but Connect/pairing/self-test are ordinary phone UI people
        // expect to use portrait (task brief). configChanges (also in the
        // manifest) keeps THIS Activity alive across the rotation that
        // causes — which is what lets a live session survive it —
        // onConfigurationChanged below is where the rest of the UI catches
        // up: which PadLayout is loaded, and how the connect screen composes.
        currentOrientation = padOrientation()

        // docs/DESIGN.md §7.2: "keep the screen on". A screen that sleeps takes Wi-Fi
        // latency with it even with the WifiLock held.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (handleHeadlessSelfTest(intent)) return
        if (handleHeadlessBtSelfTest(intent)) return

        // Captured here, before anything else can consume/replace `intent`
        // — see [btMouseDebugLog]'s own doc. Deliberately NOT a headless
        // hook like the two above: this flag only changes logging on a
        // NORMAL Bluetooth session the user still drives by hand (pair,
        // switch to MOUSE, drag), it does not run anything and exit.
        btMouseDebugLog = intent?.getBooleanExtra("bt_mouse_debug", false) == true

        nsd = AtticPadNsd(this)
        buildUi()
        requestNotificationPermissionIfNeeded()

        // §10.3 deep link — atticpad://ip:port/?v=1&s=<secret>, handed to us
        // by the system camera app (or anything else that read one) via the
        // <data android:scheme="atticpad"/> filter in the manifest. Checked
        // FIRST: a cold start from a deep link never carries the --es ip/pin
        // hook below, and the two are mutually exclusive in practice.
        val deepLink = intent?.data
        if (deepLink != null && deepLink.scheme == "atticpad") {
            applyPairingUriForAutoConnect(deepLink)
        } else {
            // Dev/CI hook — connect on launch without anyone tapping anything:
            //   adb shell am start -n net.atticpad/.MainActivity \
            //       --es ip 10.0.2.2 --ei port 21100
            // This is what makes an Android integration job possible at all
            // (docs/DESIGN.md §8.1), and it is how the client was first proven
            // against the Linux server. It adds no state: it fills the
            // address box and presses the same Connect the user would.
            autoConnectIp = intent?.getStringExtra("ip")
            autoConnectPort = intent?.getIntExtra("port", AtticPadNative.defaultPort())
                ?: AtticPadNative.defaultPort()
            // --es pin <secret> completes the same hook for a §10 pairing
            // window, which is the only way to test the TYPED pairing path
            // unattended: the PIN is generated on the server 120 seconds
            // before it is needed, so it cannot be baked into the apk and
            // there is nobody to type it. A DEBUG-BUILD CONVENIENCE ONLY —
            // an extra is visible to anything that can read the launch
            // intent, and §10's secret should reach the app from a human or
            // a camera, never from a shell argument. The deep link above is
            // what makes the CAMERA path testable unattended instead.
            autoConnectPin = intent?.getStringExtra("pin")
        }

        startService(Intent(this, AtticPadService::class.java))
        bindService(
            Intent(this, AtticPadService::class.java),
            connection, Context.BIND_AUTO_CREATE,
        )

        // Android 16 (API 36) no longer dispatches the deprecated
        // Activity.onBackPressed()/KEYCODE_BACK once an app targets API 36 —
        // OnBackInvokedCallback (API 33+) is the only reliable way to
        // intercept back now (ported from the deleted BtControllerActivity,
        // which hit this first). API 26..32 have no such dispatcher at all,
        // so a BT session is simply not back-interceptible there — the
        // status pill's own "tap for setup" affordance (see
        // updateBtPlayHud) is what covers that gap, not a second
        // back-handling code path. Registered unconditionally (not only
        // once Bluetooth is first used) because it must also handle leaving
        // the Bluetooth setup screen back to the UDP one — see
        // onBackFromSession.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val callback = OnBackInvokedCallback { onBackFromSession() }
            btBackCallback = callback
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback,
            )
        }
    }

    /** The registered [OnBackInvokedCallback]'s entire back handling (API
     *  33+ only — see [onCreate]'s registration). A registered callback
     *  takes full ownership of the back action, so "leave the Activity"
     *  has to be explicit here too for the one case that used to be plain
     *  system back with no override at all: nothing else is currently
     *  showing, so back should behave exactly as it always did. */
    private fun onBackFromSession() {
        when {
            activeTransport != null && activeTransport === bt -> closeBtSession()
            preSessionScreen == PreSessionScreen.BLUETOOTH && activeTransport == null ->
                showUdpConnectScreen()
            else -> finish()
        }
    }

    /**
     * `singleTop` (manifest) means a SECOND `atticpad://` deep link, while
     * this Activity is already the foreground task, arrives here rather
     * than restarting [onCreate] — the ordinary case when the system camera
     * hands off a scan while AtticPad is already open.
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val uri = intent.data ?: return
        if (uri.scheme != "atticpad") return
        val (ip, port, secret) = parsePairingUri(uri) ?: return
        ipField.setText(ip)
        showPinField(true)
        pinField.setText(secret)
        connectTo(ip, port)
    }

    override fun onStart() {
        super.onStart()
        if (nsd.isAvailable) {
            nsd.startBrowse { list -> main.post { showDiscovered(list) } }
        }
    }

    override fun onStop() {
        nsd.stopBrowse()
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        sensors?.start()
        pushBattery()
        // Re-opens the camera if permission is already granted (an earlier
        // launch, or a return from system Settings after enabling it) —
        // "on later launches it starts directly" (task brief).
        refreshScannerCard()

        // Bluetooth setup panel — mirrors the deleted BtControllerActivity's
        // own onResume, only while that screen is actually the one showing:
        // covers returning from system Settings (a permission grant can
        // change underneath this Activity with no callback of its own) and
        // returning from system Bluetooth settings (a device paired there
        // while this Activity was backgrounded).
        if (bt != null && ::btPermissionPanel.isInitialized && btPermissionPanel.visibility == View.VISIBLE) {
            refreshBtPermissionStep()
        }
        if (bt != null && ::btPairingPanel.isInitialized && btPairingPanel.visibility == View.VISIBLE) {
            refreshPairedDevices()
            // Re-arms scanning the same way the camera re-opens above —
            // pauseBtDiscovery() (below) is what stopped it, not the
            // task brief's own three triggers, so this is the resume half
            // of that pair, not a fourth trigger.
            startBtDiscovery()
        }
    }

    override fun onPause() {
        // The sensors stop; the SESSION does not. That asymmetry is the whole
        // point of the Service — a notification shade or an incoming call must
        // not drop the pad.
        sensors?.stop()
        // The camera, unlike the session, MUST be released here: Camera2
        // does not let a backgrounded app keep it open, and holding it would
        // starve whatever the user switched to.
        stopScanner()
        // Same reasoning as the camera above, for the radio instead of the
        // lens: a backgrounded Activity must not keep re-arming a ~12s
        // discovery burst indefinitely. Lighter than stopBtDiscovery() —
        // keeps btDiscoveredDevices so onResume's list is not empty for a
        // moment — see pauseBtDiscovery's own doc.
        pauseBtDiscovery()
        super.onPause()
    }

    override fun onDestroy() {
        if (bound) {
            service?.setListener(null)
            unbindService(connection)
            bound = false
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            btBackCallback?.let { onBackInvokedDispatcher.unregisterOnBackInvokedCallback(it) }
        }
        if (btReceiverRegistered) unregisterReceiver(btStateReceiver)
        btConnectTimeoutRunnable?.let { main.removeCallbacks(it) }
        // The third of the task brief's three stop triggers — see
        // stopBtDiscovery's own doc for the other two.
        btDiscoveryWanted = false
        bt?.controller?.cancelDiscovery()
        bt?.controller?.setListener(null)
        bt?.controller?.unregister()
        super.onDestroy()
    }

    /**
     * The whole point of `configChanges` in the manifest: this fires INSTEAD
     * of a recreate. [padView]'s FrameLayout already re-measures itself for
     * free — the only things that need to actively catch up are which
     * per-orientation [PadLayout] is showing, and the connect screen's
     * composition (a real two-pane arrangement in landscape, not portrait
     * merely stretched — see [buildConnectPanel]).
     */
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        val next = padOrientation()
        if (next == currentOrientation) return
        val previous = currentOrientation
        // An in-progress, unsaved edit belongs to the orientation it was
        // made in. Autosaving it here rather than discarding it is the same
        // call PadLayout's own doc comment makes about a customised layout:
        // "silently discarding it is the kind of thing that makes people
        // distrust a settings screen."
        if (::padView.isInitialized && padView.editing) {
            PadLayout.save(this, padView.layout, previous)
        }
        currentOrientation = next
        if (::padView.isInitialized) {
            padView.layout = PadLayout.load(this, currentOrientation)
        }
        if (::connectPanel.isInitialized) rebuildConnectPanel()
        applyInsetsToViews()
    }

    private fun padOrientation(): PadLayout.Orientation =
        if (resources.configuration.orientation ==
            android.content.res.Configuration.ORIENTATION_PORTRAIT
        ) {
            PadLayout.Orientation.PORTRAIT
        } else {
            PadLayout.Orientation.LANDSCAPE
        }

    /**
     * Re-applies the last known `WindowInsets` to every view that cares —
     * [PadView]'s own safe-area maths, plus a little breathing room for the
     * HUD chip and the scanner's status/cancel controls, so none of them
     * ever sit under a display cutout or the navigation bar (task brief:
     * "Landscape on a modern phone puts a notch on one side, and a stick
     * under a notch is a bug you only see on hardware").
     */
    private fun applyInsetsToViews() {
        val e = edgeInsets
        if (::hud.isInitialized) {
            (hud.layoutParams as? FrameLayout.LayoutParams)?.let {
                it.topMargin = e.top + dp(Theme.SPACE_SM)
                hud.layoutParams = it
            }
        }
        updatePadViewInsets()
        // The connect screen's content is the only thing in the panel now —
        // there is no separate pinned footer to absorb e.bottom (nav bar /
        // gesture inset) — so it goes straight onto the content's own
        // bottom padding, on top of the design's base margin.
        if (::connectPanelContent.isInitialized) {
            val base = connectPanelBasePadding
            connectPanelContent.setPadding(
                base[0] + e.left, base[1] + e.top, base[2] + e.right, base[3] + e.bottom,
            )
        }
    }

    /**
     * The HUD chip and the pad controls are built by two different pieces
     * of code that otherwise know nothing about each other — that is
     * exactly why the HUD has now collided with a control twice (M3:
     * SEL/START in landscape; this fix: ZL/ZR in portrait), each time
     * requiring a fresh constant nudged by hand for one specific layout.
     *
     * Fixing it structurally instead: [PadView] already refuses to place a
     * control above [PadView.insetTop] — that is the exact mechanism that
     * keeps a stick out from under a display cutout (see [PadView]'s class
     * doc). Treating the HUD's own MEASURED footprint as an extra top
     * inset, on top of whatever the display cutout/status bar already
     * contributes, makes the HUD just another edge of that same safe area.
     * No control, in either orientation, present or future, can ever be
     * laid out under the HUD, because none of them can be laid out above
     * `insetTop` at all — there is no second bounding box to remember to
     * update, and nothing to accidentally leave stale the next time
     * somebody adds a control near the top or changes the HUD's text.
     *
     * This is why it is measured, not guessed: the HUD's own text is
     * variable-length ("EDIT MODE — drag controls, then use ☰ to finish",
     * its only content since job 1 removed the permanent numeric strip —
     * see [buildHud]), WRAP_CONTENT, and can wrap to two lines on a narrow
     * phone in portrait — a fixed dp guess would be exactly the kind of
     * constant that works on the device it was tuned on and nowhere else.
     */
    private fun updatePadViewInsets() {
        val e = edgeInsets
        val hudBottom = if (::hud.isInitialized && hud.visibility == View.VISIBLE) hud.bottom else 0

        // The mode bar sits directly under the HUD chip, not at a fixed dp
        // guess — same reasoning as the HUD reserve itself (see this
        // method's class-level doc on PadView): its own text can wrap, and
        // MEASURING beats a constant tuned on one phone.
        if (::modeBar.isInitialized) {
            (modeBar.layoutParams as? FrameLayout.LayoutParams)?.let {
                val want = hudBottom + dp(Theme.SPACE_XS)
                if (it.topMargin != want) {
                    it.topMargin = want
                    modeBar.layoutParams = it
                }
            }
        }
        val chromeBottom = if (::modeBar.isInitialized && modeBar.visibility == View.VISIBLE) {
            modeBar.bottom
        } else {
            hudBottom
        }
        val chromeReserve = if (chromeBottom > 0) chromeBottom + dp(Theme.SPACE_SM) else 0

        if (::padView.isInitialized) {
            padView.setInsets(e.left, maxOf(e.top, chromeReserve), e.right, e.bottom)
        }
        // BUG FIX (coordinator review): this used to be
        // `body.setPadding(body.paddingLeft, maxOf(e.top, chromeReserve), body.paddingRight, body.paddingBottom)`
        // — it read back and preserved whatever bottom padding the body
        // ALREADY had, but nothing ever WROTE `e.bottom` (the navigation
        // bar / gesture-nav inset) into that value in the first place, so
        // the bottom key row sat directly under — sometimes literally
        // struck through by — the system gesture pill in both
        // orientations. Each body's OWN base padding (captured once at
        // construction — see the base-padding fields' doc) is now ADDED to
        // every edge inset, matching [connectPanelBasePadding]'s existing
        // pattern, so this both fixes the bottom gap and stops silently
        // erasing [mediaBody]'s own margins the way a bare
        // `setPadding(insets...)` would.
        // applyKbmBodyInsets (KbmModeBar.kt) is pure View/IntArray/Insets
        // math with nothing transport-specific in it — the same helper
        // applies whichever transport the overlay is currently bound to.
        if (::mouseBody.isInitialized) applyKbmBodyInsets(mouseBody.view, mouseBody.basePadding, e, chromeReserve)
        if (::keyboardBody.isInitialized) applyKbmBodyInsets(keyboardBody.view, keyboardBody.basePadding, e, chromeReserve)
        if (::mediaBody.isInitialized) applyKbmBodyInsets(mediaBody, mediaBodyBasePadding, e, chromeReserve)
    }

    // ---- session lifecycle (job 3) ---------------------------------------
    //
    // The ONE place the overlay moves between "hidden" and "showing, bound
    // to transport X" — see [activeTransport]'s own doc. UDP's onStatus and
    // Bluetooth's onBtStatus are the only callers.

    /** Binds the overlay to [transport] and shows it. Idempotent by
     *  construction (every assignment here is either a plain field write or
     *  an already-idempotent call), which matters because UDP's own caller
     *  runs this on EVERY STATE_ACTIVE status tick, not only the first —
     *  exactly the cadence [applyKbmActivity]/[showModeBody] already ran at
     *  before this existed as a named function. */
    private fun enterSession(transport: SessionTransport) {
        activeTransport = transport
        padView.snapshot = transport.input
        mouseBody.kbm = transport.kbm
        keyboardBody.kbm = transport.kbm
        mediaBody.bindKbm(transport.kbm)
        connectPanel.visibility = View.GONE
        if (::btSetupPanel.isInitialized) btSetupPanel.visibility = View.GONE
        showModeBody(kbmMode)
        applyKbmActivity(kbmMode)
        // An inline scanner left running behind an open session would hold
        // the camera open and drain battery for no reason (the same "must
        // not starve whatever the user switched to" rule onPause already
        // applies, just triggered by a state change instead of a lifecycle
        // one) — see refreshPreSessionScreen's own stopScanner-mirror note.
        stopScanner()
        // Same reasoning, for BT discovery — the first of the task brief's
        // three stop triggers: a session opening (Bluetooth's own, on the
        // rising edge of a connection, or UDP's) always leaves the
        // Bluetooth setup screen behind, discovery included.
        stopBtDiscovery()
        clearStatus()
        updatePadViewInsets()
    }

    /** Un-binds the overlay — the mirror image of [enterSession]. A session
     *  ending must not leave a key/button/control held on the host with
     *  nothing left able to lift it (see [KbmSnapshot]'s class doc), so the
     *  facility release runs BEFORE [activeTransport] goes null:
     *  [applyKbmActivity] reads `activeTransport?.kbm`, and the transport
     *  being left is still the right one to release facilities on at the
     *  moment this is called. Callers are responsible for deciding whether
     *  a pre-session screen should reappear afterward (UDP's onStatus calls
     *  [refreshPreSessionScreen]; Bluetooth's closeBtSession shows its own
     *  setup panel directly instead, since that is always where a
     *  Bluetooth session ends up). */
    private fun exitSession() {
        if (kbmMode != KbmMode.PAD) {
            kbmMode = KbmMode.PAD
            applyKbmActivity(KbmMode.PAD)
        }
        activeTransport = null
        hideAllModeBodies()
        if (::modeBar.isInitialized) modeBar.visibility = View.GONE
        hud.visibility = View.GONE
        updatePadViewInsets()
    }

    /** Shows whichever of [connectPanel]/[btSetupPanel] matches
     *  [preSessionScreen] — HIDES both while a session is actually open
     *  ([activeTransport] non-null), rather than leaving them alone,
     *  because [rebuildConnectPanel] rebuilds [connectPanel] from scratch
     *  on every rotation regardless of whether a session is showing, and a
     *  freshly built view defaults to VISIBLE. Before there were two
     *  transports, that default was always immediately corrected by
     *  [rebuildConnectPanel]'s own trailing `onStatus(it.status)` call (its
     *  ACTIVE branch set `connectPanel.visibility = GONE`) — but that
     *  branch now SKIPS touching it whenever a Bluetooth session is the one
     *  open (see its own double-connect-race guard), so a rotation during
     *  an open Bluetooth session used to leave the freshly rebuilt
     *  [connectPanel] sitting on top of the session overlay, VISIBLE,
     *  fully opaque, and undismissable — caught by literally rotating the
     *  emulator mid Bluetooth-session and screenshotting, not by reasoning
     *  through the code. Explicit hide-when-a-session-is-open here is what
     *  actually fixes it, called from [rebuildConnectPanel] every time
     *  regardless of which transport (if any) is live. */
    private fun refreshPreSessionScreen() {
        if (activeTransport != null) {
            connectPanel.visibility = View.GONE
            if (::btSetupPanel.isInitialized) btSetupPanel.visibility = View.GONE
            return
        }
        connectPanel.visibility = if (preSessionScreen == PreSessionScreen.UDP) View.VISIBLE else View.GONE
        if (::btSetupPanel.isInitialized) {
            btSetupPanel.visibility = if (preSessionScreen == PreSessionScreen.BLUETOOTH) View.VISIBLE else View.GONE
        }
    }

    private fun showUdpConnectScreen() {
        preSessionScreen = PreSessionScreen.UDP
        refreshPreSessionScreen()
        refreshScannerCard()
        // The task brief's first stop trigger: leaving the Bluetooth setup
        // screen for this one.
        stopBtDiscovery()
    }

    /** Reached from [buildTitleRow]'s "Use Bluetooth" button — a transport
     *  CHOICE inside the connect flow (task brief point 3), not a
     *  `startActivity` to a separate screen the way the deleted
     *  BtControllerActivity was. [beginBtPermissionFlow] is what actually
     *  touches the Bluetooth stack; building this panel's views ([buildUi])
     *  does not, and tapping "Use Bluetooth" a second time re-runs the same
     *  idempotent flow rather than requesting anything twice. */
    private fun showBtSetupScreen() {
        preSessionScreen = PreSessionScreen.BLUETOOTH
        refreshPreSessionScreen()
        stopScanner()
        beginBtPermissionFlow()
    }

    // ---- UI -------------------------------------------------------------

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /** `MATCH_PARENT`-width, `WRAP_CONTENT`-height LinearLayout params with a
     *  top margin — the one shape nearly every row in the design system
     *  below needs, so it is not rebuilt inline forty times over. */
    private fun vlp(topMargin: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply { this.topMargin = topMargin }

    private fun buildUi() {
        root = FrameLayout(this)
        root.setBackgroundColor(Theme.BG)

        padView = PadView(this)
        padView.layout = PadLayout.load(this, currentOrientation)
        padView.onSelfTestCombo = { showSelfTest() }
        padView.visibility = View.GONE
        root.addView(
            padView,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ),
        )

        for (body in listOf(buildMouseBody(), buildKeyboardBody(), buildMediaBody())) {
            body.visibility = View.GONE
            root.addView(
                body,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                ),
            )
        }

        hud = buildHud()
        // Fires on every layout pass of the HUD — its text changes length
        // (and can change line count) as the status readout updates, and
        // the HUD's OWN size is exactly what updatePadViewInsets() reserves
        // for it. This is the half of the fix applyInsetsToViews() alone
        // cannot do: that runs off WindowInsets, which say nothing about
        // how tall a two-line HUD chip turned out to be.
        hud.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updatePadViewInsets() }
        root.addView(
            hud,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ),
        )

        modeBar = buildModeBar()
        modeBar.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updatePadViewInsets() }
        root.addView(
            modeBar,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ).apply { topMargin = dp(Theme.SPACE_XL + Theme.SPACE_LG) },
        )

        connectPanel = buildConnectPanel()
        root.addView(
            connectPanel,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ),
        )

        // Bluetooth setup/pairing — a sibling screen, not a separate
        // Activity (task brief point 3). Built eagerly here like every
        // other sibling above, but GATED on BtHidController.isSupported():
        // there is no dead "Use Bluetooth" button on a device that cannot
        // do this (buildTitleRow), so there is nothing for this panel to be
        // either. Nothing in it requests a permission or touches the
        // Bluetooth stack merely by being built — that only happens once
        // the user actually taps "Use Bluetooth" (beginBtPermissionFlow).
        if (BtHidController.isSupported()) {
            btSetupPanel = buildBtSetupPanel()
            btSetupPanel.visibility = View.GONE
            root.addView(
                btSetupPanel,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT,
                ),
            )
        }

        setContentView(root)
        refreshScannerCard()

        // WindowInsets — see applyInsetsToViews() for why this is not
        // optional: the overlay is deliberately edge-to-edge, so nothing
        // else keeps a control out from under a notch or the nav bar.
        Insets.listen(root) { edges ->
            edgeInsets = edges
            applyInsetsToViews()
        }
    }

    /** Job 1 removed this chip's old permanent job (pad slot/rate/RTT/tx/rx,
     *  refreshed every status tick, with a "[menu]" tap target) — that was a
     *  panel of numbers, not product feedback. What is left: the ACTIVE
     *  session's own EDIT-MODE label ("EDIT MODE — drag controls...", see
     *  the "Edit layout" menu item), which labels an ongoing action rather
     *  than reporting numbers, so it stays. The menu itself is now opened
     *  from [KbmChipRow] in [modeBar], not from tapping here. */
    private fun buildHud(): TextView = TextView(this).apply {
        setTextColor(Theme.TEXT_PRIMARY)
        textSize = Theme.TEXT_LABEL
        background = Theme.hudBackground(this@MainActivity)
        val padH = dp(Theme.SPACE_MD)
        val padV = dp(Theme.SPACE_XS)
        setPadding(padH, padV, padH, padV)
        visibility = View.GONE
    }

    // ---- §6.15-6.19 mode bar and the three new full-screen bodies --------

    /** A chip row pinned under the HUD — GONE unless a session is ACTIVE
     *  (job 1: it now also carries [KbmChipRow], so it must stay reachable
     *  for the WHOLE active session, not just while the server has
     *  accepted some KBM feature — see [refreshModeBar]). A mode chip
     *  whose OWN feature bit is clear stays visible but disabled, with a
     *  dimmed look, rather than disappearing — the task brief's "a chip
     *  whose feature bit is clear is disabled with a one-line reason". */
    private fun buildModeBar(): LinearLayout {
        // KbmChipRow (KbmModeBar.kt) is the ONE chip row now — a Bluetooth
        // session shows the exact same row, built here, bound through
        // [activeTransport] instead of a second copy of it (job 3: see this
        // class's own doc). `includeMenu = true` unconditionally: both
        // transports use this same ☰ (see [showMenu]'s own transport
        // branch), unlike the deleted BtControllerActivity's row, which had
        // no menu concept at all and relied solely on its status pill's
        // "tap for setup" affordance. GONE until the first session opens.
        chipRow = KbmChipRow(this, includeMenu = true, onSelect = ::selectMode, onMenu = ::showMenu)
        chipRow.view.visibility = View.GONE
        return chipRow.view
    }

    private val modeChips: Map<KbmMode, TextView> get() = chipRow.chips

    private fun modeFeatureBit(mode: KbmMode): Int = when (mode) {
        KbmMode.PAD -> -1 // always available — not gated on any INPUTCAPS bit
        KbmMode.MOUSE -> AtticPadNative.KBM_FEATURE_MOUSE
        KbmMode.KEYBOARD -> AtticPadNative.KBM_FEATURE_KEYBOARD
        KbmMode.MEDIA -> AtticPadNative.KBM_FEATURE_MEDIA
    }

    /** Every §6.15-6.17 feature bit set — Bluetooth's own gating answer
     *  (see [refreshChipAvailability]'s doc): there is no server to
     *  negotiate with, and [BtHidController.REPORT_DESCRIPTOR]
     *  unconditionally declares all four report collections the moment the
     *  device registers, so "everything is available" is simply true here,
     *  not a simplification of something more nuanced. */
    private val BT_KBM_FEATURES_ALL =
        AtticPadNative.KBM_FEATURE_MOUSE or AtticPadNative.KBM_FEATURE_KEYBOARD or AtticPadNative.KBM_FEATURE_MEDIA

    /** Redraws chip enabled/highlighted state from whichever transport is
     *  live's current idea of "what can MOUSE/KEYBOARD/MEDIA actually do
     *  right now" — called on every UDP status tick while ACTIVE (a server
     *  can widen its §6.19 features, rare, or the session can simply be
     *  new) via [refreshModeBar], and once whenever a Bluetooth session
     *  opens (its answer never changes — see [BT_KBM_FEATURES_ALL]) via
     *  [refreshModeBarForBluetooth]. The per-chip styling math itself has
     *  nothing transport-specific in it, which is what let this collapse
     *  from two copies (one per Activity) to one.
     *
     *  Job 1 changed WHEN the bar itself is visible: it used to be GONE
     *  whenever nothing at all had KBM to offer, because there was nothing
     *  to show. Now [KbmChipRow] lives in this same row, and the menu
     *  (self-test, layout edit, diagnostics, plus whichever transport-
     *  specific items [showMenu] adds) must stay reachable for every open
     *  session — so the bar itself is visible whenever a session is open,
     *  and only the MOUSE/KEYBOARD/MEDIA chips disappear when there is
     *  nothing at all to offer them (PAD stays, same as before). */
    private fun refreshChipAvailability(kbmAvailable: Boolean, kbmFeatures: Int) {
        if (!::modeBar.isInitialized) return
        modeBar.visibility = View.VISIBLE
        for ((mode, chip) in modeChips) {
            val available = if (mode == KbmMode.PAD) {
                chip.visibility = View.VISIBLE
                true
            } else if (!kbmAvailable) {
                // Nothing to offer — hidden, not merely disabled, same
                // treatment the task brief reserves for "no support
                // whatsoever" as opposed to "this one bit is off".
                chip.visibility = View.GONE
                continue
            } else {
                chip.visibility = View.VISIBLE
                (kbmFeatures and modeFeatureBit(mode)) != 0
            }
            styleModeChip(this, chip, mode, kbmMode, available)
        }
        // The mode this screen is showing just lost its feature bit (a
        // reconnect to a different server, most likely, or one with no KBM
        // support at all) — fall back to PAD rather than keep a body up
        // whose input is now silently discarded.
        if (kbmMode != KbmMode.PAD) {
            val bit = modeFeatureBit(kbmMode)
            if (!kbmAvailable || (kbmFeatures and bit) == 0) selectMode(KbmMode.PAD)
        }
    }

    private fun refreshModeBar(s: AtticPadService.Status) = refreshChipAvailability(s.kbmAvailable, s.kbmFeatures)

    private fun refreshModeBarForBluetooth() = refreshChipAvailability(true, BT_KBM_FEATURES_ALL)

    private fun selectMode(mode: KbmMode) {
        if (mode == kbmMode) return
        kbmMode = mode
        applyKbmActivity(mode)
        showModeBody(mode)
        when {
            activeTransport != null && activeTransport === bt -> refreshModeBarForBluetooth()
            else -> service?.status?.let { refreshModeBar(it) }
        }
    }

    /** Which §6.15-6.17 facilities are "live" for each mode — several modes
     *  share a facility (every non-PAD mode carries a trackpad, so MOUSE is
     *  active whenever the body isn't PAD; KEYBOARD is active for MOUSE's
     *  own [TextEntryBar] and MEDIA's D-pad/Menu too). See [KbmSnapshot]'s
     *  class doc for why leaving a facility here clears it on the host.
     *  Delegates to the shared, transport-agnostic
     *  `applyKbmActivity(KbmSnapshot?, ...)` in KbmModeBar.kt, against
     *  whichever transport's [KbmSnapshot] is currently bound
     *  ([activeTransport]) — UDP's and Bluetooth's alike, now that there is
     *  only one call site instead of two. */
    private fun applyKbmActivity(mode: KbmMode) {
        applyKbmActivity(
            activeTransport?.kbm,
            mode,
            if (::mouseBody.isInitialized) mouseBody.textEntry else null,
        )
    }

    private fun showModeBody(mode: KbmMode) {
        padView.visibility = if (mode == KbmMode.PAD) View.VISIBLE else View.GONE
        mouseBody.view.visibility = if (mode == KbmMode.MOUSE) View.VISIBLE else View.GONE
        keyboardBody.view.visibility = if (mode == KbmMode.KEYBOARD) View.VISIBLE else View.GONE
        mediaBody.visibility = if (mode == KbmMode.MEDIA) View.VISIBLE else View.GONE
    }

    private fun hideAllModeBodies() {
        padView.visibility = View.GONE
        if (::mouseBody.isInitialized) mouseBody.view.visibility = View.GONE
        if (::keyboardBody.isInitialized) keyboardBody.view.visibility = View.GONE
        if (::mediaBody.isInitialized) mediaBody.visibility = View.GONE
    }

    /** MOUSE mode: full-bleed [TrackpadView], two wide L/R click buttons,
     *  and [TextEntryBar]'s collapsible bottom bar (task brief). The body
     *  itself is [MouseBody] (KbmModeBar.kt) — bound to whichever transport
     *  is live by [enterSession] (job 2/3). */
    private fun buildMouseBody(): View {
        mouseBody = MouseBody(this)
        return mouseBody.view
    }

    /** KEYBOARD mode: a compact dual-gutter [TrackpadView] over a full
     *  [KeyGridView] (task brief) — [KeyboardBody] (KbmModeBar.kt), bound to
     *  whichever transport is live by [enterSession] (job 2/3).
     *
     * The old 0.3f/0.7f fixed weight split was exactly the "grid eats two
     * thirds of the screen" bug: a fixed FRACTION of whatever height the
     * body happens to have, unrelated to how tall a well-proportioned key
     * actually needs to be. [KeyGridView] now measures its OWN height from
     * its own width (see its class doc) and reports that; giving it
     * `WRAP_CONTENT` here and the trackpad `weight = 1f` means the trackpad
     * gets whatever the grid does not claim — more of the screen on a tall
     * phone, less on a short landscape one — with no orientation-specific
     * code needed, because a window resize alone re-runs both views'
     * measure passes. */
    private fun buildKeyboardBody(): View {
        keyboardBody = KeyboardBody(this)
        return keyboardBody.view
    }

    private fun buildMediaBody(): MediaRemoteView {
        // buildMediaBody(Context) here is the top-level KbmModeBar.kt
        // factory, resolved by its different arity — not a recursive call.
        val (view, base) = net.atticpad.buildMediaBody(this)
        mediaBody = view
        // MediaRemoteView sets its OWN base padding in its constructor
        // (SPACE_MD horizontal, SPACE_SM vertical) — captured here, before
        // updatePadViewInsets() ever touches it, so that call can ADD
        // insets to it instead of overwriting it (see the field's doc).
        mediaBodyBasePadding = base
        return mediaBody
    }

    /**
     * Rebuilds the connect screen in place after an orientation change (see
     * [onConfigurationChanged]). Cheap enough — a handful of TextViews and
     * two EditTexts — that building fresh for the new orientation is
     * simpler and less error-prone than reflowing one tree in place.
     * Anything the user typed, and the last-known service status, survive
     * the rebuild.
     */
    private fun rebuildConnectPanel() {
        val savedIp = if (::ipField.isInitialized) ipField.text.toString() else null
        val pinWasVisible = ::pinField.isInitialized && pinField.visibility == View.VISIBLE
        val savedPin = if (::pinField.isInitialized) pinField.text.toString() else ""

        // The scanner's TextureView is about to be torn down along with the
        // rest of connectPanel — its SurfaceTexture cannot outlive it, so
        // the camera has to be released here rather than left pointing at a
        // Surface that no longer exists. refreshScannerCard() below reopens
        // it against the NEW TextureView once the new one has a surface.
        stopScanner()

        root.removeView(connectPanel)
        connectPanel = buildConnectPanel()
        root.addView(
            connectPanel,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ),
        )

        savedIp?.let { ipField.setText(it) }
        if (pinWasVisible) showPinField(true)
        pinField.setText(savedPin)
        showDiscovered(lastDiscovered)
        refreshScannerCard()
        // Status/visibility are DERIVED from the service, not typed by the
        // user — re-deriving them beats trying to carry a half-applied UI
        // state across the rebuild.
        service?.let { onStatus(it.status) }
        // The freshly built connectPanel above defaults to VISIBLE — fixed
        // up by onStatus's own ACTIVE branch when UDP is the live session,
        // but that branch deliberately skips connectPanel while a
        // Bluetooth session is the one open (see its own doc). This is
        // what actually corrects it in that case too — see
        // refreshPreSessionScreen's own doc for the bug this fixes.
        refreshPreSessionScreen()
    }

    /**
     * The connect/pairing screen. Scanning is the PRIMARY path (task brief:
     * "Invert it" — scan card, then discovery, then manual entry, in that
     * order top to bottom) under a plain title. Portrait stacks all three
     * cards in one scrolling column; landscape is a genuinely different
     * composition, not that column stretched wide: a fixed-width pane
     * carrying the title and the scan card beside a scrollable pane for the
     * other two cards — the same two-pane shape this screen already used.
     * There is no separate status strip any more — an idle/disconnected
     * screen says nothing at all; the only actionable status line lives
     * inside [buildAddressCard], right by the Connect button.
     */
    private fun buildConnectPanel(): View {
        val portrait = currentOrientation == PadLayout.Orientation.PORTRAIT

        // The self-test diagnostic used to sit here as an ordinary button —
        // it is not a normal user action, so it now hides behind a
        // triple-tap on the title (see onTitleTap). The in-session menu
        // (showMenu's "Self-test" row) and the `--ez selftest true` adb
        // hook (handleHeadlessSelfTest) are the other two doors and are
        // untouched by this.
        //
        // The Bluetooth entry point used to be CARD 4, last in visual order.
        // It is now promoted into this title row instead — "the most
        // prominent option, it's the zero-install path" — as a compact
        // primary button beside the title, plus one caption line beneath.
        // Only on API 28+, where BluetoothHidDevice actually exists (see
        // BtHidController.isSupported); below that this row is simply the
        // title alone, not a disabled button.
        val titleRow = buildTitleRow()

        val scanCard = buildScanCard(portrait)
        val serversCard = buildServersCard()
        val addressCard = buildAddressCard()

        val content: View
        if (portrait) {
            val padL = dp(Theme.SPACE_LG); val padT = dp(Theme.SPACE_XL)
            val padB = dp(Theme.SPACE_LG)
            val col = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(padL, padT, padL, padB)
            }
            connectPanelContent = col
            connectPanelBasePadding = intArrayOf(padL, padT, padL, padB)
            col.addView(titleRow)
            // Tighter than the old SPACE_LG (24dp) gap on every one of
            // these — task brief: "too much dead vertical space between
            // cards relative to the space inside them". SPACE_MD (16dp)
            // still reads as a clear break between cards, just not a
            // bigger gap than the padding INSIDE each card.
            col.addView(scanCard, vlp(dp(Theme.SPACE_MD)))
            col.addView(serversCard, vlp(dp(Theme.SPACE_MD)))
            col.addView(addressCard, vlp(dp(Theme.SPACE_MD)))
            content = ScrollView(this).apply { clipToPadding = false; addView(col) }
        } else {
            val padH = dp(Theme.SPACE_XL); val padV = dp(Theme.SPACE_LG)
            val leftPane = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            leftPane.addView(titleRow)
            leftPane.addView(scanCard, vlp(dp(Theme.SPACE_MD)))

            val rightForm = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            rightForm.addView(serversCard)
            rightForm.addView(addressCard, vlp(dp(Theme.SPACE_MD)))
            val rightScroll = ScrollView(this).apply { clipToPadding = false; addView(rightForm) }

            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.TOP
                setPadding(padH, padV, padH, padV)
            }
            connectPanelContent = row
            connectPanelBasePadding = intArrayOf(padH, padV, padH, padV)
            row.addView(leftPane, LinearLayout.LayoutParams(dp(320), ViewGroup.LayoutParams.WRAP_CONTENT))
            row.addView(
                rightScroll,
                LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f).apply {
                    leftMargin = dp(Theme.SPACE_XL)
                },
            )
            content = row
        }

        return content
    }

    /**
     * The screen title, plus — on API 28+ only, where
     * [BtHidController.isSupported] — the promoted Bluetooth entry point
     * beside it. No caption: the button names the action itself, and
     * anything more is exactly the reassurance text the UI de-clutter pass
     * removed. Used identically by both the portrait and landscape branches
     * of [buildConnectPanel] (task brief: "Present in both portrait and
     * landscape rebuilds").
     *
     * The title keeps its own [onTitleTap] click listener on ONLY the title
     * TextView, exactly as before this row existed — the button beside it is
     * a second, separate view with its own bounds and its own listener, so
     * it structurally cannot intercept or absorb a tap meant for the title
     * (task brief: "the button must NOT intercept taps on the title text
     * itself"). This is simpler than a shared-touch-target trick and was
     * chosen so the triple-tap self-test gesture needs no changes at all.
     */
    private fun buildTitleRow(): View {
        val title = displayText(this, "Connect to your PC").apply {
            isClickable = true
            isFocusable = true
            setOnClickListener { onTitleTap(this) }
        }

        if (!BtHidController.isSupported()) return title

        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(title, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(
                // A transport CHOICE inside this same connect flow (task
                // brief point 3), not a door to a separate screen — see
                // showBtSetupScreen's own doc.
                primaryButton(this@MainActivity, "Use Bluetooth") { showBtSetupScreen() },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    marginStart = dp(Theme.SPACE_MD)
                },
            )
        }
    }

    /** A small icon over a label, on its own line — CARD 1/2/3's shared
     *  header shape (task brief: design reference). `iconRes` is one of
     *  `res/drawable/ic_*.xml` — real vector artwork, tinted to
     *  [Theme.TEXT_PRIMARY] at [Theme.ICON_HEADER] size, not an emoji: an
     *  emoji carries its own colour and cannot be tinted to match the
     *  theme, and renders differently per vendor font.
     *
     *  Full [titleText] size (18sp), not shrunk to body size (15sp) the way
     *  this used to render — three headers with no more visual weight than
     *  ordinary paragraph copy read as an afterthought next to the 26sp
     *  screen title (task brief: "the hierarchy is top-heavy"). Full title
     *  size, plus [Theme.TEXT_DISPLAY] itself stepping down to 22sp, is
     *  what closes that gap from both ends. */
    private fun cardHeader(iconRes: Int, label: String): View =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(iconView(this@MainActivity, iconRes, Theme.ICON_HEADER))
            addView(
                titleText(this@MainActivity, label),
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { leftMargin = dp(Theme.SPACE_SM) },
            )
        }

    /** The shared card shell every one of the three cards below builds on:
     *  same background, same corner radius, same padding, so they read as a
     *  stack of equal alternatives (task brief: design reference). */
    private fun cardShell(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = Theme.cardBackground(this@MainActivity)
        val padH = dp(Theme.SPACE_MD); val padV = dp(Theme.SPACE_MD)
        setPadding(padH, padV, padH, padV)
    }

    /**
     * CARD 1 — scan to connect, INLINE (task brief: "not behind a button").
     * [scanPreviewSlot] is a SQUARE rounded slot (task brief: "a QR code is
     * square, so a square viewfinder wastes less of the frame") that holds
     * EITHER [scanPlaceholder] (no CAMERA permission yet, or none to have)
     * OR the live [cameraPreview] + [reticleView] + [torchToggle] — same
     * shape and size either way, so granting permission never moves
     * anything else on the screen. Which of the two shows is entirely
     * [refreshScannerCard]'s decision; this only builds the views.
     */
    private fun buildScanCard(portrait: Boolean): View {
        val card = cardShell()
        card.addView(cardHeader(R.drawable.ic_scan, "Scan to connect"))

        // A self-measuring square, not a fixed dp x dp box.
        //
        // PORTRAIT: the square spans the card's FULL inner width — no cap
        // at all (task brief: "the square must go edge to edge... currently
        // the cap is what shrinks it; the cap should not win in portrait").
        // `available` here already excludes the card's own left/right
        // padding — LinearLayout's own MeasureSpec plumbing subtracts a
        // parent's padding before handing a child its constraint — so this
        // IS "card width minus the card's own padding" with no extra work.
        // Height is a non-issue in portrait: the card sits in a ScrollView
        // (see buildConnectPanel), which hands its content an UNSPECIFIED
        // height spec precisely so content can be as tall as it needs.
        //
        // LANDSCAPE: a full-width square would swallow the whole left pane
        // (task brief), and unlike portrait this pane is NOT inside a
        // ScrollView, so its height budget is real — the title above the
        // card already consumes some of it (see buildConnectPanel). Rather
        // than a hand-picked dp constant tuned against one phone (the old
        // Theme.SCAN_PREVIEW_SIZE_LANDSCAPE, now removed), this reads the
        // ACTUAL available height LinearLayout computed for this child —
        // the same mechanism the width side already used — so the cap
        // tracks whatever room the title really leaves rather than a
        // constant that can drift out of sync with it.
        scanPreviewSlot = object : FrameLayout(this) {
            override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                val availableWidth = View.MeasureSpec.getSize(widthMeasureSpec)
                val side = if (portrait) {
                    availableWidth
                } else {
                    val heightMode = View.MeasureSpec.getMode(heightMeasureSpec)
                    val availableHeight = if (heightMode != View.MeasureSpec.UNSPECIFIED) {
                        View.MeasureSpec.getSize(heightMeasureSpec)
                    } else {
                        availableWidth
                    }
                    minOf(availableWidth, availableHeight)
                }
                val exact = View.MeasureSpec.makeMeasureSpec(side, View.MeasureSpec.EXACTLY)
                super.onMeasure(exact, exact)
                setMeasuredDimension(side, side)
            }
        }.apply {
            background = Theme.previewBackground(this@MainActivity)
            // clipToOutline makes every child (the live TextureView
            // included) respect this background's own (now softer, see
            // Theme.previewBackground) corner radius rather than drawing a
            // square-cornered image inside a rounded frame.
            clipToOutline = true
            outlineProvider = android.view.ViewOutlineProvider.BACKGROUND
        }

        cameraPreview = TextureView(this)
        // The fix in QrScanner.updatePreviewTransform(): a rotation while
        // the scanner is open does not recreate the Activity (manifest
        // configChanges) or reopen the camera, but it DOES change this
        // MATCH_PARENT view's own on-screen bounds, and the transform
        // matrix computed for the old bounds is wrong for the new ones.
        // `onSurfaceTextureSizeChanged` does not fire for this — that
        // callback is for a BUFFER size change, which never happens here —
        // this is the one that actually catches it.
        cameraPreview.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            scanner.updatePreviewTransform(cameraPreview)
        }
        scanPreviewSlot.addView(
            cameraPreview,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        reticleView = ReticleView(this)
        scanPreviewSlot.addView(
            reticleView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        torchToggle = buildTorchToggle()
        torchOn = false
        scanPreviewSlot.addView(
            torchToggle,
            FrameLayout.LayoutParams(
                dp(Theme.TORCH_TOGGLE_SIZE), dp(Theme.TORCH_TOGGLE_SIZE), Gravity.TOP or Gravity.END,
            ).apply { topMargin = dp(Theme.SPACE_SM); rightMargin = dp(Theme.SPACE_SM) },
        )

        scanPlaceholder = buildScanPlaceholder()
        scanPreviewSlot.addView(
            scanPlaceholder,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        // WRAP_CONTENT on both axes: scanPreviewSlot's own onMeasure (above)
        // decides the exact square side, using the width THIS LayoutParams
        // hands it (a LinearLayout gives a WRAP_CONTENT child an AT_MOST
        // spec sized to what's left in the card — exactly the "available
        // card width" the onMeasure override reads). CENTER_HORIZONTAL is
        // still needed for LANDSCAPE, where the square is capped by height
        // and so is narrower than the card — MATCH_PARENT would stretch a
        // WRAP_CONTENT measurement result back out and defeat the square.
        // In portrait the square now equals the full card width, so
        // CENTER_HORIZONTAL is a no-op there, not a contradiction.
        card.addView(
            scanPreviewSlot,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply {
                topMargin = dp(Theme.SPACE_SM)
                gravity = Gravity.CENTER_HORIZONTAL
            },
        )
        return card
    }

    /**
     * Same footprint as the live preview it stands in for (task brief: "the
     * same shape and size, so the layout does not jump") — text and an
     * action button are filled in by [showScannerMessage]. Deliberately
     * COMPACT (small icon, tight padding): this has to fit inside the SAME
     * square slot as the live preview in landscape too, where the slot's
     * side is shorter AND the card is narrower (a fixed-width pane — see
     * buildConnectPanel) than portrait, so wrapped text runs to more lines
     * in less height. A generous version of this clipped its own action
     * button clean off the bottom in that orientation — caught by
     * screenshotting landscape specifically, not by reading the numbers
     * (the exact trap ui_redesign.md's portrait shoulder-button fix already
     * warned about, one layer up).
     */
    private fun buildScanPlaceholder(): View {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            val padV = dp(Theme.SPACE_SM); val padH = dp(Theme.SPACE_MD)
            setPadding(padH, padV, padH, padV)
        }
        col.addView(iconView(this, R.drawable.ic_scan, Theme.ICON_PLACEHOLDER))
        scanMessageText = captionText(this, "", Theme.TEXT_SECONDARY).apply {
            gravity = Gravity.CENTER
        }
        col.addView(scanMessageText, vlp(dp(Theme.SPACE_XS)))
        scanActionButton = secondaryButton(this, "Enable camera") { }
        col.addView(scanActionButton, vlp(dp(Theme.SPACE_XS)))
        return col
    }

    /** The floating torch toggle over the live preview — hidden by
     *  [refreshScannerCard]/[beginScan] on a camera with no flash. */
    private fun buildTorchToggle(): ImageView =
        iconView(this, R.drawable.ic_torch, Theme.ICON_TORCH).apply {
            scaleType = ImageView.ScaleType.CENTER
            background = Theme.torchToggleBackground(this@MainActivity, false)
            isClickable = true
            isFocusable = true
            visibility = View.GONE
            setOnClickListener { toggleTorch() }
        }

    /** CARD 2 — servers discovered on this network (mDNS, §7 tier 1). Each
     *  row connects on tap; the empty state is filled in by
     *  [showDiscovered]. */
    private fun buildServersCard(): View {
        val card = cardShell()
        card.addView(cardHeader(R.drawable.ic_discover, "Servers on this network"))
        discovered = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        card.addView(discovered, vlp(dp(Theme.SPACE_SM)))
        showDiscovered(lastDiscovered)
        return card
    }

    /**
     * CARD 3 — manual entry, LAST in the visual order but still fully
     * first-class (task brief / docs/PROTOCOL.md §7: "manual entry stays
     * first-class... on this project's own LAN it is the ONLY path that
     * works"). Nothing about it is collapsed or hidden — it is simply
     * beneath the scan card and the discovery list rather than above them.
     */
    private fun buildAddressCard(): View {
        val card = cardShell()
        card.addView(cardHeader(R.drawable.ic_keyboard, "Enter an address"))

        val prefs = getSharedPreferences("atticpad", Context.MODE_PRIVATE)
        ipField = styledEditText(this, "server address").apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText(prefs.getString("last_ip", ""))
        }
        card.addView(ipField, vlp(dp(Theme.SPACE_SM)))

        // §10 pairing. Hidden until a server actually asks, because the PIN
        // only exists inside a 120-second window somebody opened deliberately
        // — a box demanding one on a server that never pairs would be a lie.
        //
        // NOT persisted, unlike the address above. §10 says the secret never
        // appears on the wire; writing it to SharedPreferences would be the
        // same mistake one layer up, and it would outlive by weeks a window
        // whose whole security argument is that it lasts two minutes.
        pinLabel = sectionLabel(this, "PAIRING PIN").apply {
            setPadding(0, dp(Theme.SPACE_MD), 0, 0)
            visibility = View.GONE
        }
        card.addView(pinLabel)

        pinField = styledEditText(this, "PIN shown on the server", password = true).apply {
            // §10.1: the secret's length depends on how it reached the user —
            // six digits typed, ~20 characters scanned. No maxLength, so the
            // QR token that is coming can be pasted here without a second
            // field.
            visibility = View.GONE
        }
        card.addView(pinField, vlp(dp(Theme.SPACE_SM)))

        card.addView(
            primaryButton(this, "Connect") { connectTo(ipField.text.toString().trim()) },
            vlp(dp(Theme.SPACE_MD)),
        )

        // The connect screen's only status text (task brief: "the UI/UX
        // should do the work" — no bottom strip any more). GONE and empty
        // whenever there is nothing actionable — see showStatus/clearStatus
        // — and cleared the moment either field is edited, so a stale
        // failure never survives the user's next attempt.
        statusLine = bodyText(this, "", Theme.WARNING).apply { visibility = View.GONE }
        card.addView(statusLine, vlp(dp(Theme.SPACE_SM)))

        val clearOnEdit = object : TextWatcher {
            override fun afterTextChanged(s: Editable?) = clearStatus()
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
        }
        ipField.addTextChangedListener(clearOnEdit)
        pinField.addTextChangedListener(clearOnEdit)

        return card
    }

    /** Shows one line of ACTIONABLE status by [buildAddressCard]'s Connect
     *  button — [clearStatus] is the only other state this field is ever in.
     *  Every writer (this file's own connect/scan/pairing error paths, plus
     *  [onStatus]) goes through one of this pair rather than touching
     *  [statusLine] directly, so "nothing to act on" reliably means "no line
     *  at all" instead of a stale, empty-looking gap. */
    private fun showStatus(text: String, color: Int) {
        if (!::statusLine.isInitialized) return
        statusLine.text = text
        statusLine.setTextColor(color)
        statusLine.visibility = View.VISIBLE
    }

    /** True from the user pressing Connect until the attempt resolves —
     *  a failure during this window is actionable even when its message id
     *  is one the idle screen would stay silent about. */
    private var attemptPending = false

    private fun clearStatus() {
        if (!::statusLine.isInitialized) return
        statusLine.text = ""
        statusLine.visibility = View.GONE
    }

    // ---- Bluetooth setup/pairing (job 3) ---------------------------------
    //
    // Ported from the deleted BtControllerActivity — its own guided-flow
    // reasoning (permission gating, auto-register with no manual button,
    // the paired-devices list, the boot-protocol banner, the reconnect-to-
    // last-host behaviour) is UNCHANGED; only the container changed, from a
    // second Activity to a sibling screen inside this one, and the two
    // fields it wrote UI state into (BtHidController.Status.detail's
    // "Details" expander, [BOOT_PROTOCOL_BANNER]'s verbatim copy) are
    // exactly the ones the task brief calls out as things that must
    // survive.

    private fun missingBtPermissions(): Array<String> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return emptyArray()
        // BLUETOOTH_SCAN joined the other two 2026-08-26 — startDiscovery()/
        // ACTION_FOUND need it on API 31+, same as CONNECT/ADVERTISE already
        // did for their own calls, requested together through this one flow
        // rather than a second permission round for "Nearby devices".
        val wanted = arrayOf(
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.BLUETOOTH_ADVERTISE,
            Manifest.permission.BLUETOOTH_SCAN,
        )
        return wanted.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }.toTypedArray()
    }

    /** Constructs [bt] on first use only — never at [onCreate], never
     *  merely because [buildBtSetupPanel]'s views exist (see that method's
     *  own doc) — and wires its listener exactly once. */
    private fun ensureBluetoothTransport(): BluetoothTransport {
        bt?.let { return it }
        val created = BluetoothTransport(this)
        created.controller.mouseDebugLog = btMouseDebugLog
        created.controller.setListener { onBtStatus(it) }
        bt = created
        return created
    }

    /** Reached only from [showBtSetupScreen] — never on ordinary launch,
     *  matching CAMERA's own "never unless a screen actually needs it"
     *  rule (task brief: "keep that rule intact"). */
    private fun beginBtPermissionFlow() {
        ensureBluetoothTransport()
        val missing = missingBtPermissions()
        if (missing.isEmpty()) {
            refreshBtPermissionStep()
            return
        }
        btPermissionText.text = "AtticPad needs Bluetooth permission to appear as a gamepad."
        btOpenSettingsButton.visibility = View.GONE
        refreshBtPermissionStep()
        requestPermissions(missing, BT_PERMISSION_REQUEST)
    }

    private fun refreshBtPermissionStep() {
        val missing = missingBtPermissions()
        if (missing.isEmpty()) {
            btPermissionPanel.visibility = View.GONE
            btPairingPanel.visibility = View.VISIBLE
            ensureBtRegistered()
            ensureBtReceiverRegistered()
            refreshPairedDevices()
            startBtDiscovery()
        } else {
            btPermissionPanel.visibility = View.VISIBLE
            btPairingPanel.visibility = View.GONE
            stopBtDiscovery()
        }
    }

    /** Auto-register on entry (task brief: "no manual Register button") —
     *  called every time [refreshBtPermissionStep] finds permissions
     *  already granted, but [btRegisterRequested] makes only the FIRST such
     *  call actually reach [BtHidController.register]. */
    private fun ensureBtRegistered() {
        if (btRegisterRequested) return
        btRegisterRequested = true
        bt?.controller?.register()
    }

    /** Registered lazily, the first time permissions actually land — not in
     *  [onCreate] — for the same "only once this screen needs it" reason
     *  [beginBtPermissionFlow] is gated the way it is. Unregistered once,
     *  in [onDestroy], for the rest of this Activity's life once it has
     *  ever been true. */
    private fun ensureBtReceiverRegistered() {
        if (btReceiverRegistered) return
        btReceiverRegistered = true
        val filter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_SCAN_MODE_CHANGED)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            addAction(BluetoothDevice.ACTION_FOUND)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(btStateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(btStateReceiver, filter)
        }
    }

    private fun makeDiscoverable() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            checkSelfPermission(Manifest.permission.BLUETOOTH_ADVERTISE) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_ADVERTISE), BT_PERMISSION_REQUEST)
            return
        }
        startActivity(
            Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE)
                .putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, 30),
        )
    }

    private fun toggleBtDetails() {
        btDetailsExpanded = !btDetailsExpanded
        btDetailsText.visibility = if (btDetailsExpanded) View.VISIBLE else View.GONE
    }

    /** [BtHidController.Status.connectedAddress] of the device to try
     *  reconnecting to, looked up against [BtHidController.bondedDevices]
     *  by address — a phone's own HID Device role does not get the
     *  automatic host-initiated reconnects a bonded mouse/keyboard would,
     *  so this side has to ask. Fires at most once per "not currently
     *  connected" period; see [btReconnectAttempted]'s own doc. */
    private fun attemptBtReconnectIfNeeded(s: BtHidController.Status) {
        val controller = bt?.controller ?: return
        if (!s.registered || s.connectedName.isNotEmpty() || btReconnectAttempted) return
        val address = prefs.getString(PREF_LAST_HOST_ADDRESS, null) ?: return
        @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by the permission step
        val device = controller.bondedDevices().firstOrNull { it.address == address } ?: return
        btReconnectAttempted = true
        controller.connectTo(device)
    }

    /** Bonded devices can include phones, TVs, headsets, earbuds, watches...
     *  — filtered to the [BluetoothClass] categories that can plausibly act
     *  as a Bluetooth HID *host* (accept a keyboard/mouse/gamepad from this
     *  phone), not just a PC. [BluetoothClass.Device.Major.COMPUTER] and
     *  [BluetoothClass.Device.Major.PHONE] both pair a mouse/keyboard
     *  routinely; [BluetoothClass.Device.Major.PERIPHERAL] is a
     *  Bluetooth-passthrough peripheral (e.g. a receiver dongle) sitting in
     *  front of one. [BluetoothClass.Device.Major.AUDIO_VIDEO] is a mixed
     *  bag — TVs, set-top boxes and game consoles that DO accept HID input
     *  share the major class with headsets and speakers that never could,
     *  so that one is narrowed further by [AUDIO_VIDEO_HID_HOST_MINORS]. A
     *  device this adapter has no class for (`bluetoothClass == null` —
     *  seen on some emulator/vendor stacks) is INCLUDED rather than hidden:
     *  hiding a real, bonded host because its class byte came back empty
     *  would be worse than one extra row in the list. Constants read from
     *  `android.bluetooth.BluetoothClass` in `android-36/android.jar`, not
     *  recalled — see the AtticPad Android job that added this filter for
     *  the exact byte values. */
    @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by the permission step
    private fun isPlausibleHidHost(device: BluetoothDevice): Boolean {
        val btClass = device.bluetoothClass ?: return true
        return when (btClass.majorDeviceClass) {
            BluetoothClass.Device.Major.COMPUTER,
            BluetoothClass.Device.Major.PHONE,
            BluetoothClass.Device.Major.PERIPHERAL,
            BluetoothClass.Device.Major.UNCATEGORIZED,
            -> true
            BluetoothClass.Device.Major.AUDIO_VIDEO -> btClass.deviceClass in AUDIO_VIDEO_HID_HOST_MINORS
            else -> false
        }
    }

    @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by the permission step
    private fun btPairedRowLabel(device: BluetoothDevice, connecting: Boolean): String {
        val name = try { device.name ?: device.address } catch (e: SecurityException) { device.address }
        return if (connecting) "$name  —  Connecting..." else name
    }

    /** Rebuilds [btPairedListPanel] from [BtHidController.bondedDevices] —
     *  called on [onResume], from [btStateReceiver], from
     *  [refreshBtPermissionStep] once permissions land, and around a manual
     *  [connectToBtDevice] attempt to render its "Connecting..." row.
     *  Silently a no-op while a Bluetooth permission is still missing or
     *  [bt] does not exist yet. */
    private fun refreshPairedDevices() {
        val controller = bt?.controller ?: return
        if (!::btPairedListPanel.isInitialized || missingBtPermissions().isNotEmpty()) return
        btPairedListPanel.removeAllViews()
        val devices = controller.bondedDevices().filter { isPlausibleHidHost(it) }
        if (devices.isEmpty()) {
            btPairedListPanel.addView(captionText(this, PAIRED_EMPTY_TEXT))
            return
        }
        for (device in devices) {
            val connecting = device.address == btConnectingAddress
            val row = rowButton(this, btPairedRowLabel(device, connecting)) { connectToBtDevice(device) }
            row.isEnabled = btConnectingAddress == null
            btPairedListPanel.addView(row, vlp(dp(Theme.SPACE_XS)))
        }
    }

    /** A manual tap on a paired-devices row — a manual tap on a different
     *  row overrides the automatic reconnect, so this also marks
     *  [btReconnectAttempted] to stop [attemptBtReconnectIfNeeded] from
     *  independently dialing the remembered host on the same tick. Success
     *  is discovered the same way every other connection is (see
     *  [onBtStatus]'s `justConnected` handling, which clears
     *  [btConnectingAddress]); failure is a timeout, since
     *  [BtHidController.connectTo] has no stronger signal to offer (see
     *  [CONNECT_TIMEOUT_MS]'s own doc). */
    private fun connectToBtDevice(device: BluetoothDevice) {
        val controller = bt?.controller ?: return
        if (btConnectingAddress != null) return
        btConnectingAddress = device.address
        btReconnectAttempted = true
        btPairedStatusText.visibility = View.GONE
        refreshPairedDevices()
        controller.connectTo(device)

        btConnectTimeoutRunnable?.let { main.removeCallbacks(it) }
        val runnable = Runnable { onBtConnectAttemptTimedOut() }
        btConnectTimeoutRunnable = runnable
        main.postDelayed(runnable, CONNECT_TIMEOUT_MS)
    }

    private fun onBtConnectAttemptTimedOut() {
        btConnectTimeoutRunnable = null
        if (btConnectingAddress == null) return // already resolved elsewhere
        btConnectingAddress = null
        btPairedStatusText.text = PAIRED_CONNECT_FAILED_TEXT
        btPairedStatusText.visibility = View.VISIBLE
        refreshPairedDevices()
    }

    // ---- "Nearby devices" — phone-initiated discovery/pairing (job 4,
    // 2026-08-26) ----------------------------------------------------------

    /** Starts (or re-arms) scanning — safe to call repeatedly, including
     *  while already scanning. A no-op while a Bluetooth permission is
     *  still missing (mirrors [refreshPairedDevices]'s own guard) so this
     *  can be called unconditionally from [onResume] without duplicating
     *  that check at every call site. Always ends with a UI refresh: the
     *  spinner/empty-state render depends on [btDiscovering], which this
     *  does not itself flip (that only happens once `ACTION_DISCOVERY_
     *  STARTED` actually arrives — see [btStateReceiver]), so without this
     *  the "Nearby devices" list would sit visually stale between the
     *  call and that broadcast. */
    private fun startBtDiscovery() {
        val controller = bt?.controller ?: return
        if (missingBtPermissions().isNotEmpty()) return
        btDiscoveryWanted = true
        controller.startDiscovery()
        refreshDiscoveredDevicesUi()
    }

    /** The task brief's three stop triggers all reach this: leaving the
     *  Bluetooth setup screen ([showUdpConnectScreen], [enterSession]),
     *  and [onDestroy]. [pairWithDiscoveredDevice] deliberately does NOT
     *  call this — see its own doc for why a bond attempt needs a lighter,
     *  resumable stop instead. */
    private fun stopBtDiscovery() {
        btDiscoveryWanted = false
        bt?.controller?.cancelDiscovery()
        btDiscovering = false
        btDiscoveredDevices.clear()
        refreshDiscoveredDevicesUi()
    }

    /** [onPause]'s own stop — same [btDiscoveryWanted] = false as
     *  [stopBtDiscovery] (so the receiver does not fight a backgrounded
     *  Activity trying to stay quiet), but keeps [btDiscoveredDevices]
     *  rather than clearing it, so [onResume] does not show a jarring
     *  empty list before the next `ACTION_FOUND` arrives. */
    private fun pauseBtDiscovery() {
        btDiscoveryWanted = false
        bt?.controller?.cancelDiscovery()
        btDiscovering = false
        refreshDiscoveredDevicesUi()
    }

    @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by the permission step
    private fun btDiscoveredRowLabel(device: BluetoothDevice, pairing: Boolean): String {
        val name = try { device.name ?: device.address } catch (e: SecurityException) { device.address }
        return if (pairing) "$name  —  Pairing..." else name
    }

    /** Rebuilds [btDiscoveredListPanel] from [btDiscoveredDevices], filtered
     *  the same way [isPlausibleHidHost] already filters the paired list, and
     *  with any address that has since bonded excluded — a freshly bonded
     *  device is what [refreshPairedDevices] now shows instead (task brief:
     *  "a freshly bonded host becomes usable without hunting for it in the
     *  paired list afterwards"), not a lingering duplicate row here. Called
     *  from the same places [refreshPairedDevices] is, plus every
     *  discovery-state broadcast. */
    private fun refreshDiscoveredDevicesUi() {
        if (!::btDiscoveredListPanel.isInitialized) return
        btDiscoveringSpinner.visibility = if (btDiscovering) View.VISIBLE else View.GONE
        btDiscoveredListPanel.removeAllViews()
        val controller = bt?.controller
        if (controller == null || missingBtPermissions().isNotEmpty()) return
        @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by the permission step
        val bondedAddresses = controller.bondedDevices().map { it.address }.toSet()
        val devices = btDiscoveredDevices.values.filter {
            it.address !in bondedAddresses && isPlausibleHidHost(it)
        }
        if (devices.isEmpty()) {
            // Visual-only while scanning (the spinner above already says
            // "still looking" — task brief: "that is a state, not a
            // sentence"); the sentence is reserved for the genuinely idle
            // case, e.g. a scan that came back with nothing at all.
            if (!btDiscovering) btDiscoveredListPanel.addView(captionText(this, DISCOVERED_EMPTY_TEXT))
            return
        }
        for (device in devices) {
            val pairing = device.address == btPairingAddress
            val row = rowButton(this, btDiscoveredRowLabel(device, pairing)) { pairWithDiscoveredDevice(device) }
            row.isEnabled = btPairingAddress == null
            btDiscoveredListPanel.addView(row, vlp(dp(Theme.SPACE_XS)))
        }
    }

    /** Tap-to-pair. Cancels scanning first — an active inquiry slows down
     *  the pairing handshake that follows, and the task brief's own "must
     *  stop... when a bond starts" rule applies here, not just to a clean
     *  screen exit. [btDiscoveryWanted] is cleared (not just
     *  `cancelDiscovery()` called directly) specifically so the
     *  `ACTION_DISCOVERY_FINISHED` broadcast that same cancel triggers does
     *  not immediately re-arm scanning out from under this — see that
     *  flag's own doc. [BluetoothDevice.createBond] itself is general
     *  BR/EDR pairing, not a HID call — see
     *  [BtHidController.createBond]'s own doc for why this device's HID
     *  peripheral role does not block it — and the result arrives
     *  asynchronously via [btStateReceiver]'s `ACTION_BOND_STATE_CHANGED`
     *  ([onBondStateChanged]), the same broadcast every other bond-state
     *  change already goes through. */
    private fun pairWithDiscoveredDevice(device: BluetoothDevice) {
        val controller = bt?.controller ?: return
        if (btPairingAddress != null) return
        btDiscoveryWanted = false
        controller.cancelDiscovery()
        btDiscovering = false
        btPairingAddress = device.address
        btDiscoveredStatusText.visibility = View.GONE
        refreshDiscoveredDevicesUi()
        @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by the permission step
        controller.createBond(device)
    }

    /** Resumes scanning after a [pairWithDiscoveredDevice] attempt resolves
     *  (either way — see [onBondStateChanged]), but only if the Bluetooth
     *  setup screen is still the thing actually showing; a bond that
     *  resolves after the user has already navigated away (or a session has
     *  opened) must not silently start the radio scanning again behind
     *  whatever screen replaced it. */
    private fun resumeBtDiscoveryIfPanelShowing() {
        if (preSessionScreen == PreSessionScreen.BLUETOOTH && activeTransport == null) startBtDiscovery()
    }

    /** [btStateReceiver]'s `ACTION_BOND_STATE_CHANGED` handler for the
     *  device this screen itself asked to bond with — [refreshPairedDevices]
     *  and [refreshDiscoveredDevicesUi] have already run by the time this is
     *  called (see the receiver's own ordering); this only handles the
     *  outcome of THIS screen's own [pairWithDiscoveredDevice] attempt, and
     *  is a no-op for a bond-state change belonging to any other device
     *  (e.g. one paired from system Settings while this Activity was
     *  backgrounded). */
    private fun onBondStateChanged(intent: Intent) {
        val pairing = btPairingAddress ?: return
        val device = deviceExtra(intent) ?: return
        if (device.address != pairing) return
        when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE)) {
            BluetoothDevice.BOND_BONDED -> {
                btPairingAddress = null
                refreshDiscoveredDevicesUi()
                resumeBtDiscoveryIfPanelShowing()
                // Task brief: "a freshly bonded host becomes usable without
                // the user hunting for it in the paired list afterwards" —
                // this is the existing manual-connect path, same as tapping
                // the device's own row would do a moment later.
                connectToBtDevice(device)
            }
            BluetoothDevice.BOND_NONE -> {
                btPairingAddress = null
                btDiscoveredStatusText.text = PAIR_FAILED_TEXT
                btDiscoveredStatusText.visibility = View.VISIBLE
                refreshDiscoveredDevicesUi()
                resumeBtDiscoveryIfPanelShowing()
            }
            // BluetoothDevice.BOND_BONDING — still in progress, nothing to do.
        }
    }

    /**
     * The Bluetooth setup/pairing screen — reached from [buildTitleRow]'s
     * "Use Bluetooth" button (see [showBtSetupScreen]), API 28+ only (this
     * is only ever built inside [buildUi]'s own
     * [BtHidController.isSupported] gate). Two panels sharing this one
     * screen, exactly like the deleted BtControllerActivity's own
     * [Screen.SETUP] did: the permission panel (shown only while a
     * required Bluetooth permission is missing) and the pairing panel
     * (registration happens automatically the moment permissions are
     * granted — no manual Register button — "Make discoverable" is the
     * only action a user takes here).
     */
    private fun buildBtSetupPanel(): View {
        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = dp(Theme.SPACE_MD)
            setPadding(pad, pad, pad, pad)
        }

        val backLink = captionText(this, "‹ Back").apply {
            isClickable = true
            isFocusable = true
            setOnClickListener { showUdpConnectScreen() }
        }
        controls.addView(backLink)
        controls.addView(titleText(this, "Bluetooth Controller"), vlp(dp(Theme.SPACE_SM)))

        btBootBanner = bodyText(this, BOOT_PROTOCOL_BANNER, Theme.WARNING).apply {
            visibility = View.GONE
        }
        controls.addView(btBootBanner, vlp(dp(Theme.SPACE_MD)))

        // ---- permission panel ----
        val permCol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        btPermissionText = bodyText(this, "", Theme.TEXT_SECONDARY)
        permCol.addView(btPermissionText)
        btOpenSettingsButton = secondaryButton(this, "Open Settings") { openAppSettings() }.apply {
            visibility = View.GONE
        }
        permCol.addView(btOpenSettingsButton, vlp(dp(Theme.SPACE_SM)))
        btPermissionPanel = permCol
        controls.addView(btPermissionPanel, vlp(dp(Theme.SPACE_MD)))

        // ---- pairing panel ----
        val pairCol = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        btPairingHeadline = bodyText(this, "", Theme.WARNING).apply {
            textSize = Theme.TEXT_TITLE
            visibility = View.GONE
        }
        pairCol.addView(btPairingHeadline)
        btPairingInstruction = bodyText(this, "", Theme.TEXT_SECONDARY)
        pairCol.addView(btPairingInstruction, vlp(dp(Theme.SPACE_SM)))

        btDiscoverableButton = primaryButton(this, "Make discoverable") { makeDiscoverable() }
        pairCol.addView(btDiscoverableButton, vlp(dp(Theme.SPACE_MD)))

        pairCol.addView(sectionLabel(this, "Paired devices"), vlp(dp(Theme.SPACE_MD)))
        btPairedListPanel = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        pairCol.addView(btPairedListPanel, vlp(dp(Theme.SPACE_XS)))
        btPairedStatusText = captionText(this, "", Theme.ERROR).apply { visibility = View.GONE }
        pairCol.addView(btPairedStatusText, vlp(dp(Theme.SPACE_XS)))

        // ---- nearby (not yet paired) devices — job 4, 2026-08-26. A
        // second heading is what tells this list apart from "Paired
        // devices" above (task brief: "a heading each is fine, a sentence
        // of explanation is not"); the small spinner next to the heading —
        // not a "Scanning..." caption — is the "still looking" state (task
        // brief: "that is a state, not a sentence").
        val discoveredHeadingRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        btDiscoveredHeading = sectionLabel(this, "Nearby devices")
        discoveredHeadingRow.addView(btDiscoveredHeading)
        btDiscoveringSpinner = ProgressBar(this, null, android.R.attr.progressBarStyleSmall).apply {
            visibility = View.GONE
            indeterminateTintList = ColorStateList.valueOf(Theme.TEXT_SECONDARY)
        }
        discoveredHeadingRow.addView(
            btDiscoveringSpinner,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { marginStart = dp(Theme.SPACE_SM) },
        )
        pairCol.addView(discoveredHeadingRow, vlp(dp(Theme.SPACE_MD)))
        btDiscoveredListPanel = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        pairCol.addView(btDiscoveredListPanel, vlp(dp(Theme.SPACE_XS)))
        btDiscoveredStatusText = captionText(this, "", Theme.ERROR).apply { visibility = View.GONE }
        pairCol.addView(btDiscoveredStatusText, vlp(dp(Theme.SPACE_XS)))

        // Visible once registered — the auto-open on connect (see
        // onBtStatus) is the normal path; this stays as the fallback door.
        btOpenControllerButton = primaryButton(this, "Open controller") {
            openBtSession()
        }.apply {
            visibility = View.GONE
        }
        pairCol.addView(btOpenControllerButton, vlp(dp(Theme.SPACE_MD)))

        btDetailsToggle = captionText(this, "Details").apply {
            isClickable = true
            isFocusable = true
            visibility = View.GONE
            setOnClickListener { toggleBtDetails() }
        }
        pairCol.addView(btDetailsToggle, vlp(dp(Theme.SPACE_MD)))
        btDetailsText = TextView(this).apply {
            setTextColor(Theme.TEXT_MUTED)
            textSize = Theme.TEXT_CAPTION
            typeface = android.graphics.Typeface.MONOSPACE
            visibility = View.GONE
        }
        pairCol.addView(btDetailsText, vlp(dp(Theme.SPACE_XS)))

        btPairingPanel = pairCol
        controls.addView(btPairingPanel, vlp(dp(Theme.SPACE_MD)))

        return ScrollView(this).apply { clipToPadding = false; addView(controls) }
    }

    private fun updateBtPairingPanel(s: BtHidController.Status) {
        btBootBanner.visibility = if (s.bootProtocol) View.VISIBLE else View.GONE

        if (s.registrationFailed) {
            btPairingHeadline.visibility = View.VISIBLE
            btPairingHeadline.text = "This phone's Bluetooth doesn't support controller mode."
            btPairingHeadline.setTextColor(Theme.ERROR)
            btPairingInstruction.visibility = View.GONE
            btDiscoverableButton.visibility = View.GONE
            btOpenControllerButton.visibility = View.GONE
            btDetailsToggle.visibility = View.VISIBLE
            btDetailsText.text = s.detail
            return
        }

        btDetailsToggle.visibility = View.GONE
        btDetailsText.visibility = View.GONE
        btDetailsExpanded = false

        btOpenControllerButton.visibility = if (s.registered) View.VISIBLE else View.GONE
        btDiscoverableButton.visibility = View.VISIBLE

        if (btConnectionLost) {
            btPairingHeadline.visibility = View.VISIBLE
            btPairingHeadline.text = "Connection lost"
            btPairingHeadline.setTextColor(Theme.WARNING)
        } else {
            btPairingHeadline.visibility = View.GONE
        }

        val lastHostName = prefs.getString(PREF_LAST_HOST_NAME, null)
        btPairingInstruction.visibility = View.VISIBLE
        btPairingInstruction.text = if (!lastHostName.isNullOrEmpty() && s.connectedName.isEmpty()) {
            "Reconnecting to $lastHostName..."
        } else {
            "On your PC, open Bluetooth settings and add a new device. Choose AtticPad."
        }
    }

    /** The boot-protocol banner while a Bluetooth session is open — the
     *  overlay's only OTHER writer of [hud] is the "Edit layout" menu item
     *  (see [showMenu]), so this restores that label rather than clobbering
     *  it when boot protocol clears mid-edit, instead of assuming [hud] was
     *  idle before this ran. */
    private fun refreshBtHud(s: BtHidController.Status) {
        // bt is non-null in every real call path (onBtStatus only ever
        // fires through bt's own listener) — written as `!= null && === bt`
        // anyway, matching every other transport-identity check in this
        // class, rather than relying on that invariant silently.
        if (activeTransport == null || activeTransport !== bt) return
        when {
            s.bootProtocol -> {
                hud.setTextColor(Theme.WARNING)
                hud.text = BOOT_PROTOCOL_BANNER
                hud.visibility = View.VISIBLE
            }
            padView.editing -> {
                hud.setTextColor(Theme.TEXT_PRIMARY)
                hud.text = EDIT_MODE_HUD_TEXT
                hud.visibility = View.VISIBLE
            }
            else -> hud.visibility = View.GONE
        }
    }

    /** Mirrors the deleted BtControllerActivity's own `showPlay` — see
     *  [enterSession]'s doc for what actually moves the overlay. */
    private fun openBtSession() {
        val transport = bt ?: return
        enterSession(transport)
        refreshModeBarForBluetooth()
    }

    /** Mirrors BtControllerActivity's own `showSetup` — a Bluetooth session
     *  always returns to ITS OWN setup screen, never to the UDP connect
     *  screen (a live pairing to remember, a boot-protocol banner that may
     *  still apply), so this calls [showBtSetupScreen] directly instead of
     *  [refreshPreSessionScreen] the way UDP's onStatus does. */
    private fun closeBtSession() {
        exitSession()
        showBtSetupScreen()
    }

    private fun onBtStatus(s: BtHidController.Status) {
        // Developer-only: nothing on screen ever echoes s.detail except
        // behind the explicit "Details" tap in the failure state above.
        Log.d("MainActivity", "bt status: $s")

        if (s.connectedName.isNotEmpty() && s.connectedAddress.isNotEmpty()) {
            prefs.edit()
                .putString(PREF_LAST_HOST_NAME, s.connectedName)
                .putString(PREF_LAST_HOST_ADDRESS, s.connectedAddress)
                .apply()
        }

        val justConnected = s.connectedName.isNotEmpty() && btLastConnectedName.isEmpty()
        val justDisconnected = s.connectedName.isEmpty() && btLastConnectedName.isNotEmpty()
        btLastConnectedName = s.connectedName
        if (justConnected) {
            btConnectionLost = false
            btReconnectAttempted = false
            if (btConnectingAddress != null || btConnectTimeoutRunnable != null) {
                btConnectingAddress = null
                btConnectTimeoutRunnable?.let { main.removeCallbacks(it) }
                btConnectTimeoutRunnable = null
            }
        }
        if (justDisconnected) btConnectionLost = true

        attemptBtReconnectIfNeeded(s)
        if (::btPairingPanel.isInitialized) updateBtPairingPanel(s)
        refreshBtHud(s)

        // Auto-enter the session the moment a host connects (task brief),
        // only on the rising edge and only when nothing is open yet — a
        // user already in a session (Bluetooth OR UDP — see enterSession's
        // own double-connect-race guard) must never be yanked anywhere.
        if (justConnected && activeTransport == null) openBtSession()
        // Disconnection always returns to the Bluetooth setup screen — but
        // only if a Bluetooth session was the thing showing; an unrelated
        // disconnect edge must not disturb an active UDP session.
        if (justDisconnected && activeTransport === bt) closeBtSession()
    }

    // ---- §10.3 pairing: deep link + QR scan ------------------------------

    /**
     * Parses a §10.3 URI through `AtticPadNative.pairUriParse` — the SAME
     * native call `QrScanner`'s successful frames use — and turns the
     * result into a status-line message on failure. Never logs `uri`: it
     * carries the pairing secret in plain text (docs/PROTOCOL.md §10.3, "a
     * displayed URI is exactly as sensitive as a displayed PIN").
     */
    private fun parsePairingUri(uri: Uri): Triple<String, Int, String>? {
        val rc = IntArray(1)
        val result = AtticPadNative.pairUriParse(uri.toString(), rc)
        if (result == null) {
            showStatus(describePairUriError(rc[0]), Theme.ERROR)
            return null
        }
        val port = result[1].toIntOrNull() ?: AtticPadNative.defaultPort()
        return Triple(result[0], port, result[2])
    }

    private fun describePairUriError(rc: Int): String = when (rc) {
        AtticPadNative.ERR_VERSION ->
            "This server's pairing link is a newer version than this app supports."
        else -> "That link is not a valid AtticPad pairing code."
    }

    /** Cold-start deep link: the Service is not bound yet, so this reuses
     *  the exact same "fill the fields, connect once bound" hook the
     *  `--es ip/pin` dev shortcut already uses (see onCreate). */
    private fun applyPairingUriForAutoConnect(uri: Uri) {
        val (ip, port, secret) = parsePairingUri(uri) ?: return
        autoConnectIp = ip
        autoConnectPort = port
        autoConnectPin = secret
    }

    /** The three states Card 1's placeholder can be in — [GRANTED] shows the
     *  live preview instead of the placeholder at all. Deliberately NOT the
     *  same thing as `checkSelfPermission`'s two-value answer: distinguishing
     *  [DENIED_ONCE] (asking again would show the system dialog) from
     *  [DENIED_PERMANENTLY] (it would not — Android silently no-ops a
     *  repeat `requestPermissions` once the user has picked "don't ask
     *  again", or on some OEM skins after enough plain denials) is exactly
     *  what keeps "Enable camera" from becoming a dead button (task brief:
     *  "handle refusal and don't-ask-again without dead-ending"). */
    private enum class CameraPermState { GRANTED, NOT_ASKED, DENIED_ONCE, DENIED_PERMANENTLY }

    private fun cameraPermState(): CameraPermState {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            return CameraPermState.GRANTED
        }
        // shouldShowRequestPermissionRationale() is ALSO false before the
        // very first ask, so it cannot by itself tell "never asked" apart
        // from "asked and permanently denied" — this app's own flag is what
        // makes that distinction, checked first.
        val asked = getSharedPreferences("atticpad", Context.MODE_PRIVATE)
            .getBoolean("camera_permission_asked", false)
        if (!asked) return CameraPermState.NOT_ASKED
        return if (shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)) {
            CameraPermState.DENIED_ONCE
        } else {
            CameraPermState.DENIED_PERMANENTLY
        }
    }

    private fun requestCameraPermission() {
        getSharedPreferences("atticpad", Context.MODE_PRIVATE)
            .edit().putBoolean("camera_permission_asked", true).apply()
        requestPermissions(arrayOf(Manifest.permission.CAMERA), CAMERA_PERMISSION_REQUEST)
    }

    private fun openAppSettings() {
        startActivity(
            Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                .setData(Uri.fromParts("package", packageName, null)),
        )
    }

    /**
     * Drives Card 1's placeholder/live-preview split. Called after the card
     * is (re)built, on every `onResume` (a return from system Settings after
     * enabling the permission needs this), and after a permission result —
     * critically, NEVER on a bare `onCreate`/screen-appearing before one of
     * those, which is what keeps CAMERA from being requested just because
     * the connect screen opened (task brief: "do not auto-prompt on
     * launch"). Idempotent: safe to call repeatedly in the same state.
     */
    private fun refreshScannerCard() {
        if (!::scanPreviewSlot.isInitialized) return
        if (!QrScanner.hasCamera(this)) {
            stopScanner()
            showScannerMessage("No camera on this device — use the network list or address entry below.")
            return
        }
        when (cameraPermState()) {
            CameraPermState.GRANTED -> {
                showScannerLive()
                ensureScannerRunning()
            }

            CameraPermState.NOT_ASKED -> {
                stopScanner()
                showScannerMessage(
                    "AtticPad uses the camera only to scan the PC's QR code.",
                    "Enable camera",
                ) { requestCameraPermission() }
            }

            CameraPermState.DENIED_ONCE -> {
                stopScanner()
                showScannerMessage(
                    "Camera access was declined — it's only used to scan the " +
                        "PC's QR code. The other options below still work.",
                    "Enable camera",
                ) { requestCameraPermission() }
            }

            CameraPermState.DENIED_PERMANENTLY -> {
                stopScanner()
                showScannerMessage(
                    "Camera access is off in system settings. You can still " +
                        "connect with the network list or address entry below.",
                    "Open settings",
                ) { openAppSettings() }
            }
        }
    }

    private fun showScannerLive() {
        scanPlaceholder.visibility = View.GONE
        cameraPreview.visibility = View.VISIBLE
        reticleView.visibility = View.VISIBLE
    }

    private fun showScannerMessage(
        message: String,
        actionLabel: String? = null,
        action: (() -> Unit)? = null,
    ) {
        cameraPreview.visibility = View.GONE
        reticleView.visibility = View.GONE
        torchToggle.visibility = View.GONE
        scanPlaceholder.visibility = View.VISIBLE
        scanMessageText.text = message
        if (actionLabel != null && action != null) {
            scanActionButton.text = actionLabel
            scanActionButton.visibility = View.VISIBLE
            scanActionButton.setOnClickListener { action() }
        } else {
            scanActionButton.visibility = View.GONE
        }
    }

    private fun ensureScannerRunning() {
        if (scannerRunning) return
        val existing = cameraPreview.surfaceTexture
        if (existing != null) {
            beginScan(existing)
            return
        }
        cameraPreview.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                beginScan(st)
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) = Unit
            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean = true
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) = Unit
        }
    }

    private fun beginScan(texture: SurfaceTexture) {
        scannerRunning = true
        scanner.start(
            texture,
            onResult = { ip, port, secret ->
                stopScanner()
                ipField.setText(ip)
                showPinField(true)
                pinField.setText(secret)
                connectTo(ip, port)
            },
            onError = { message ->
                stopScanner()
                showStatus(message, Theme.ERROR)
            },
        )
        // scanner.start() has already read the buffer size synchronously
        // (camera open itself is async, but that is not needed here); the
        // TextureView is already laid out by the time its SurfaceTexture
        // exists, so the very first frame is transformed correctly, not
        // just frames after a subsequent rotation.
        scanner.updatePreviewTransform(cameraPreview)
        torchToggle.visibility = if (scanner.hasFlash) View.VISIBLE else View.GONE
    }

    /** Safe to call from any state, including "never started" and "already
     *  stopped" — every caller (onPause, a rebuild, a decode result, a
     *  decode error, losing the permission) treats this as unconditional
     *  cleanup rather than tracking whether it is needed first. */
    private fun stopScanner() {
        scanner.stop()
        scannerRunning = false
        torchOn = false
        if (::torchToggle.isInitialized) {
            torchToggle.visibility = View.GONE
            torchToggle.background = Theme.torchToggleBackground(this, false)
        }
    }

    private fun toggleTorch() {
        val next = !torchOn
        if (scanner.setTorch(next)) {
            torchOn = next
            torchToggle.background = Theme.torchToggleBackground(this, torchOn)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == CAMERA_PERMISSION_REQUEST) {
            refreshScannerCard()
        }
        if (requestCode == BT_PERMISSION_REQUEST) {
            if (missingBtPermissions().isNotEmpty()) {
                btPermissionText.text = "Bluetooth permission is off for AtticPad. You can turn it on in Settings."
                btOpenSettingsButton.visibility = View.VISIBLE
                return
            }
            refreshBtPermissionStep()
        }
    }

    /** Kept in [lastDiscovered] because it is UI state a rotation-driven
     *  [rebuildConnectPanel] would otherwise lose — it is not part of
     *  `AtticPadService.Status`, which only knows about a session. */
    private fun showDiscovered(list: List<AtticPadNsd.Found>) {
        lastDiscovered = list
        discovered.removeAllViews()
        if (list.isEmpty()) {
            discovered.addView(captionText(this, "Nothing found yet."))
            return
        }
        for (f in list) {
            discovered.addView(
                rowButton(this, "${f.name}  —  ${f.host}:${f.port}") { connectTo(f.host, f.port) },
                vlp(dp(Theme.SPACE_XS)),
            )
        }
    }

    private fun connectTo(ip: String, port: Int = AtticPadNative.defaultPort()) {
        if (ip.isEmpty()) {
            showStatus("Enter the server's IP address.", Theme.ERROR)
            return
        }
        // The ADDRESS is remembered; the PIN is not. See buildAddressCard().
        getSharedPreferences("atticpad", Context.MODE_PRIVATE)
            .edit().putString("last_ip", ip).apply()
        attemptPending = true
        showStatus("Connecting to $ip:$port…", Theme.TEXT_SECONDARY)
        val pin = pinField.text.toString().trim()
        service?.startSession(ip, port, AtticPadService.DEFAULT_RATE_HZ,
            pin.ifEmpty { null }) ?: run {
            showStatus("Service not bound yet — try again in a moment.", Theme.ERROR)
        }
    }

    /** Shows the PIN box the first time a server asks for one, and never
     *  hides it again while this screen is up: hiding it under the user
     *  mid-retry would eat what they had typed. */
    private fun showPinField(show: Boolean) {
        if (!show) return
        pinLabel.visibility = View.VISIBLE
        pinField.visibility = View.VISIBLE
    }

    private fun onStatus(s: AtticPadService.Status) {
        if (s.state != lastLoggedState) {
            lastLoggedState = s.state
            Log.i(
                "AtticPadSession",
                "state=${s.state} session_id=${s.sessionId} pad_slot=${s.padSlot} " +
                    "rate=${s.rateHz} rtt=${s.rttMs} tx=${s.txPackets} rx=${s.rxPackets} " +
                    "close_reason=${s.closeReason} err=${s.lastError} " +
                    "pairing_required=${s.pairingRequired} auth_required=${s.authRequired} " +
                    "auth_state=${s.authState} error_code=${s.errorCode} msg=${s.message}",
            )
        }
        showPinField(s.needsSecret)
        when (s.state) {
            AtticPadNative.STATE_ACTIVE -> {
                // Guards the rare double-connect race (the user reaches the
                // Bluetooth setup screen from the UDP connect screen mid-
                // handshake, then that stale UDP attempt resolves after a
                // Bluetooth session is already open and visible) — a
                // session already on screen must never be silently swapped
                // out from under the user by an unrelated background
                // transport. See [enterSession]'s own doc.
                //
                // NOT `activeTransport !== bt`: when Bluetooth has never
                // been used at all, both `activeTransport` and `bt` are
                // null, which made that comparison false — skipping the
                // very first UDP connection of the app's life. Enter
                // whenever nothing is open yet OR UDP was already the one
                // open (the ordinary re-entry on every ACTIVE tick); skip
                // only when a Bluetooth session is the thing currently
                // showing.
                if (activeTransport == null || activeTransport === service) {
                    enterSession(service ?: return)
                    refreshModeBar(s)
                    attemptPending = false
                }
                // hud's own visibility is untouched here — job 1 removed
                // its permanent job (see buildHud's doc); it now shows only
                // while padView.editing, driven from the "Edit layout" menu
                // item, not from every status tick.
                // The numeric strip that used to be rebuilt here every tick
                // (pad slot/rate/RTT/tx/rx) is gone (job 1) — that was a
                // panel of numbers, not product feedback. It is still
                // available on demand from showMenu(), read directly off
                // service?.status at the moment the menu opens rather than
                // kept live here.
            }

            AtticPadNative.STATE_HANDSHAKING -> {
                showStatus("Connecting to ${s.target}…", Theme.TEXT_SECONDARY)
            }

            else -> {
                // Only tear the overlay down if UDP was the transport that
                // put it up — an unrelated status tick (UDP has never been
                // ACTIVE this launch, which is the ordinary case) must not
                // disturb a Bluetooth session or the Bluetooth setup screen
                // that may currently be showing instead. See [exitSession].
                if (activeTransport === service) {
                    exitSession()
                    refreshPreSessionScreen()
                    // The mirror image of enterSession's stopScanner(): the
                    // connect screen is visible again (a fresh launch, a
                    // disconnect, a dropped session), so Card 1 should be
                    // live again if permission allows — idempotent, safe to
                    // call on every status tick this branch handles.
                    refreshScannerCard()
                }
                // Only the actionable subset of statusMsgId()'s catalog gets
                // a line here (task brief: "the UI/UX should do the work" —
                // no bottom strip any more, and a plain idle/disconnected
                // screen says nothing anywhere, not even here). A wrong PIN
                // or a pairing window still waiting are shown clearly and in
                // a colour that matches, never buried in the same amber as
                // "not connected yet" (task brief: "a wrong secret must say
                // so clearly"); an ordinary CONNECT_IDLE/DISCONNECTED, a
                // server-initiated close, a closed pairing window or too
                // many tries are not — the user can just try again with no
                // sentence required.
                when (val id = statusMsgId(s)) {
                    Msg.NEED_PIN,
                    Msg.PAIRING_CLOSED,
                    -> showStatus(msg(id), Theme.WARNING)
                    Msg.WRONG_PIN,
                    Msg.TOO_MANY_TRIES,
                    Msg.SERVER_FULL,
                    Msg.VERSION_MISMATCH,
                    Msg.SERVER_CLOSED,
                    Msg.CONNECTION_LOST,
                    -> showStatus(msg(id), Theme.ERROR)
                    else ->
                        // A silent id right after the user pressed Connect is
                        // still an answer they are waiting for (e.g. no
                        // WELCOME from a dead address) — show the engine's
                        // own failure text once, then fall back to silence.
                        if (attemptPending && s.message.isNotEmpty()) {
                            showStatus(s.message, Theme.ERROR)
                        } else {
                            clearStatus()
                        }
                }
                if (s.state != AtticPadNative.STATE_HANDSHAKING) attemptPending = false
            }
        }
    }

    /**
     * Mirrors `apad_ui_status_message()`'s precedence
     * (clients/common/apad_ui.h) for the [AtticPadNative.STATE_IDLE] and
     * [AtticPadNative.STATE_CLOSED] cases [onStatus]'s `else` branch handles.
     * Kotlin never calls that C function directly — it takes an
     * `apad_client_stats*`, and this session runs entirely in C via
     * `apad_client` with only the flattened [AtticPadService.Status] crossing
     * the JNI boundary — so this is that same switch, hand-translated:
     *
     *  1. CLOSED:
     *     a. error_code (the server's own reason) beats everything else.
     *     b/c. else auth_state — NEED_SECRET before FAILED.
     *     d. else close_reason, the least specific of the three.
     *  2. anything else (IDLE, or any state the FSM has no room for):
     *     auth_state == NEED_SECRET, else the idle default.
     */
    private fun statusMsgId(s: AtticPadService.Status): Int {
        if (s.state != AtticPadNative.STATE_CLOSED) {
            return if (s.authState == AtticPadNative.AUTH_NEED_SECRET) Msg.NEED_PIN else Msg.CONNECT_IDLE
        }

        when (s.errorCode) {
            AtticPadNative.ERRC_VERSION_MISMATCH -> return Msg.VERSION_MISMATCH
            AtticPadNative.ERRC_NO_FREE_SLOT -> return Msg.SERVER_FULL
            AtticPadNative.ERRC_AUTH_FAILED -> return Msg.WRONG_PIN
            AtticPadNative.ERRC_PAIRING_CLOSED -> return Msg.PAIRING_CLOSED
            AtticPadNative.ERRC_TOO_MANY_TRIES -> return Msg.TOO_MANY_TRIES
            AtticPadNative.ERRC_MALFORMED, AtticPadNative.ERRC_UNKNOWN_SESSION -> return Msg.CONNECTION_LOST
            else -> {
                // An unrecognised nonzero code (a newer server's vocabulary):
                // generic loss sentence, never a clean "Disconnected" —
                // mirrors apad_ui.c's default branch.
                if (s.errorCode != 0) return Msg.CONNECTION_LOST
                // 0 — no error_code from the server; fall through.
            }
        }

        if (s.authState == AtticPadNative.AUTH_NEED_SECRET) return Msg.NEED_PIN
        if (s.authState == AtticPadNative.AUTH_FAILED) return Msg.WRONG_PIN

        return when (s.closeReason) {
            AtticPadNative.CLOSE_LOCAL -> Msg.DISCONNECTED
            AtticPadNative.CLOSE_PEER_BYE -> Msg.SERVER_CLOSED
            AtticPadNative.CLOSE_IDLE_TIMEOUT,
            AtticPadNative.CLOSE_RETX_FAILED,
            AtticPadNative.CLOSE_PEER_ERROR,
            -> Msg.CONNECTION_LOST
            else -> Msg.CONNECT_IDLE // CLOSE_NONE, and anything unrecognised.
        }
    }

    /**
     * An [AlertDialog] whose WINDOW is transparent, so the only thing drawn is
     * the caller's own [Theme.cardBackground] card.
     *
     * Without this the platform supplies its own opaque dialog background,
     * which is a rectangle sitting behind our rounded card: the card's corners
     * are transparent, so the platform rectangle shows through them as four
     * grey notches. Invisible against a light background and obvious against
     * the near-black session screen, which is where it was spotted on a real
     * phone.
     *
     * Both dialogs in this file go through here rather than repeating the
     * incantation, because the second copy is how one of them silently keeps
     * the square corners.
     */
    private fun cardDialog(view: View): AlertDialog =
        AlertDialog.Builder(this).setView(view).create().apply {
            window?.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
        }

    /**
     * Job 1: this is where the old permanent numeric strip (pad slot, rate,
     * RTT, tx/rx) moved — "still reachable when something is wrong", but
     * behind a deliberate tap on [KbmChipRow] rather than sitting over the
     * pad on every frame. Read straight off `service?.status` at the
     * moment the menu opens (a snapshot, same as the self-test dialog's
     * own numbers), not kept live while the dialog is up — this menu is
     * short-lived and dismissed on any action.
     *
     * Job 3: the ONE place transport-specific content is allowed to differ
     * (task brief point 4) — everything above the `item(...)` calls below
     * is identical either way; the caption line and the LAST two rows
     * branch on `activeTransport === bt`. Layout editing and self-test stay
     * shared: both transports show the SAME [padView], so "which layout is
     * this" and "does the codec work on this phone" are transport-agnostic
     * questions, not ones that belong on either side of the branch.
     */
    private fun showMenu() {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = Theme.cardBackground(this@MainActivity)
            setPadding(dp(Theme.SPACE_SM), dp(Theme.SPACE_SM), dp(Theme.SPACE_SM), dp(Theme.SPACE_SM))
        }
        col.addView(
            titleText(this, "AtticPad").apply {
                setPadding(dp(Theme.SPACE_MD), dp(Theme.SPACE_SM), dp(Theme.SPACE_MD), dp(Theme.SPACE_MD))
            },
        )

        val bluetoothActive = activeTransport != null && activeTransport === bt
        if (bluetoothActive) {
            bt?.controller?.status?.let { s ->
                col.addView(
                    captionText(
                        this,
                        buildString {
                            append(if (s.connectedName.isNotEmpty()) s.connectedName else "no host connected")
                            if (s.streaming) append(" · ${s.reportsPerSec} reports/s")
                        },
                    ).apply {
                        setPadding(dp(Theme.SPACE_MD), 0, dp(Theme.SPACE_MD), dp(Theme.SPACE_SM))
                    },
                )
            }
        } else {
            service?.status?.takeIf { it.state == AtticPadNative.STATE_ACTIVE }?.let { s ->
                col.addView(
                    captionText(
                        this,
                        buildString {
                            append("pad ${s.padSlot} · ${s.rateHz} Hz · ")
                            append(if (s.rttMs >= 0) "RTT ${s.rttMs} ms" else "RTT —")
                            append(" · tx ${s.txPackets} rx ${s.rxPackets}")
                            if (s.message.isNotEmpty()) append(" · ${s.message}")
                        },
                    ).apply {
                        setPadding(dp(Theme.SPACE_MD), 0, dp(Theme.SPACE_MD), dp(Theme.SPACE_SM))
                    },
                )
            }
        }

        val dialog = cardDialog(col)
        fun item(label: String, action: () -> Unit) {
            col.addView(
                rowButton(this, label) { action(); dialog.dismiss() },
                vlp(dp(Theme.SPACE_XS)),
            )
        }

        item(if (padView.editing) "Finish editing layout" else "Edit layout") {
            padView.editing = !padView.editing
            if (padView.editing) {
                hud.visibility = View.VISIBLE
                hud.text = EDIT_MODE_HUD_TEXT
            } else {
                PadLayout.save(this, padView.layout, currentOrientation)
                hud.visibility = View.GONE
            }
        }
        item("Reset layout (this orientation)") {
            PadLayout.reset(this, currentOrientation)
            padView.layout = PadLayout.default(currentOrientation)
        }
        item("Self-test") { showSelfTest() }
        if (bluetoothActive) {
            item("Make discoverable") { makeDiscoverable() }
            item("Back to Bluetooth setup") { closeBtSession() }
        } else {
            item("Disconnect") { service?.stopSession() }
        }

        dialog.show()
    }

    /**
     * The connect screen's hidden trigger for [showSelfTest] — a triple-tap
     * on the title, the same idea as the 3DS's hidden L+R+START combo
     * ([PadView.onSelfTestCombo]) one layer up: this is a diagnostic, not an
     * ordinary user action, so it should not sit on screen as a button next
     * to "Connect". Unlike L+R+START, a triple-tap has no failure mode where
     * a broken physical button makes it unreachable — so hiding it here,
     * with no visible fallback ON THIS SCREEN, is safe; the in-session menu
     * ([showMenu]'s "Self-test" row) and the `--ez selftest true` adb hook
     * ([handleHeadlessSelfTest]) remain as the other two doors regardless.
     *
     * Every tap gives a brief visual pulse — task brief: "so it does not
     * feel broken when someone taps the title twice and nothing happens" —
     * without hinting at what three of them do.
     */
    private fun onTitleTap(title: View) {
        val now = System.currentTimeMillis()
        if (now - titleTapLastMs > TITLE_TAP_WINDOW_MS) titleTapCount = 0
        titleTapLastMs = now
        titleTapCount++

        title.animate().cancel()
        title.alpha = 0.5f
        title.animate().alpha(1f).setDuration(150).start()

        if (titleTapCount >= TITLE_TAP_COUNT) {
            titleTapCount = 0
            showSelfTest()
        }
    }

    /**
     * §13 conformance self-test, on this device's own ARM build of the codec.
     * Reachable three ways on purpose — see [onTitleTap] and
     * [PadView.checkCombo].
     */
    private fun showSelfTest() {
        val counts = IntArray(3)
        val firstFailure = runSelfTest(counts)
        val passed = counts[2] == 0 && counts[0] > 0

        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = Theme.cardBackground(this@MainActivity)
            setPadding(dp(Theme.SPACE_LG), dp(Theme.SPACE_LG), dp(Theme.SPACE_LG), dp(Theme.SPACE_LG))
        }
        col.addView(
            titleText(this, if (passed) "Self-test PASSED" else "Self-test FAILED").apply {
                setTextColor(if (passed) Theme.OK else Theme.ERROR)
            },
        )
        col.addView(
            captionText(this, msg(Msg.SELFTEST_SUBTITLE)),
            vlp(dp(Theme.SPACE_XS)),
        )
        col.addView(
            bodyText(
                this,
                buildString {
                    append("${counts[1]}/${counts[0]} passed")
                    if (counts[2] > 0) append(", ${counts[2]} FAILED")
                },
                if (passed) Theme.OK else Theme.ERROR,
            ).apply { textSize = Theme.TEXT_TITLE },
            vlp(dp(Theme.SPACE_MD)),
        )
        col.addView(
            captionText(
                this,
                "abi: ${Build.SUPPORTED_ABIS.firstOrNull()} · " +
                    "libapad ${AtticPadNative.version()} · " +
                    "wire v${AtticPadNative.protocolVersion()}",
            ),
            vlp(dp(Theme.SPACE_SM)),
        )
        if (firstFailure != null) {
            col.addView(
                TextView(this).apply {
                    text = "first failure:\n$firstFailure"
                    setTextColor(Theme.ERROR)
                    textSize = Theme.TEXT_CAPTION
                    typeface = android.graphics.Typeface.MONOSPACE
                },
                vlp(dp(Theme.SPACE_MD)),
            )
        }

        val dialog = cardDialog(col)
        col.addView(primaryButton(this, "OK") { dialog.dismiss() }, vlp(dp(Theme.SPACE_LG)))
        dialog.show()
    }

    /**
     * Runs the vectors and ALWAYS logs the outcome, whether or not anyone is
     * looking at the dialog. `adb logcat -s AtticPadSelfTest` is then a
     * complete answer to "did the codec survive this ABI?", which matters
     * because the answer that counts comes from an arm64 phone that is not
     * attached to the machine that built the apk.
     */
    private fun runSelfTest(counts: IntArray): String? {
        var firstFailure = AtticPadNative.selfTest(counts)

        // Drift guard for the Msg mirror (see Msg's doc comment): nothing
        // else catches the day clients/common/apad_ui_strings.h's enum grows
        // and this hand-copied Kotlin object does not, so it runs as part of
        // the self-test rather than as a separate, easy-to-forget check. A
        // mismatch means every Msg.* screen may now be showing the wrong
        // string, so it counts as a failed check, not a warning.
        val libMsgCount = AtticPadNative.uiMsgCount()
        if (libMsgCount != Msg.COUNT) {
            counts[0] += 1
            counts[2] += 1
            val mismatch = "message catalog mismatch: app ${Msg.COUNT} vs library $libMsgCount"
            firstFailure = firstFailure?.let { "$it; $mismatch" } ?: mismatch
        }

        Log.i(
            "AtticPadSelfTest",
            "abi=${Build.SUPPORTED_ABIS.firstOrNull()} " +
                "libapad=${AtticPadNative.version()} wire=v${AtticPadNative.protocolVersion()} " +
                "total=${counts[0]} passed=${counts[1]} failed=${counts[2]} " +
                "first_failure=${firstFailure ?: "-"}",
        )
        return firstFailure
    }

    /**
     * `adb shell am start -n net.atticpad/.MainActivity --ez selftest true`
     * runs the vectors and exits, so clients/android/build.sh can report a
     * device result without anyone tapping anything.
     *
     * The 3DS taught this: a physically broken L button makes the hidden
     * L+R+START combo impossible to trigger AT ALL, and the diagnostic was
     * then unreachable on the one device that
     * had it. A diagnostic wants more than one door.
     */
    private fun handleHeadlessSelfTest(intent: Intent?): Boolean {
        if (intent?.getBooleanExtra("selftest", false) != true) return false
        val counts = IntArray(3)
        runSelfTest(counts)
        finish()
        return true
    }

    /**
     * `adb shell am start -n net.atticpad.debug/net.atticpad.MainActivity
     *     --ez selftest_bt true`
     * runs [BtHidController.selfTestReportBuilders]'s known-good-byte-
     * sequence checks and exits — ported from the deleted
     * BtControllerActivity's own `--ez selftest true` hook. A SEPARATE extra
     * from [handleHeadlessSelfTest]'s `selftest` (not folded into the same
     * counts, and now unavoidably the same Activity/component, so it needed
     * its own name to stay distinguishable at the launch command): this is
     * HID-report arithmetic, not §13 wire conformance, and the original
     * BtControllerActivity's own doc is explicit that mixing the two would
     * mix concerns that should stay independently falsifiable — still true
     * with one fewer Activity to hang the second hook off of.
     */
    private fun handleHeadlessBtSelfTest(intent: Intent?): Boolean {
        if (intent?.getBooleanExtra("selftest_bt", false) != true) return false
        val counts = IntArray(3)
        val firstFailure = BtHidController.selfTestReportBuilders(counts, this)
        Log.i(
            "MainActivity",
            "bt report-builder self-test: total=${counts[0]} passed=${counts[1]} " +
                "failed=${counts[2]} first_failure=${firstFailure ?: "-"}",
        )
        finish()
        return true
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < 33) return
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED
        ) return
        // Denial is survivable: the foreground service still runs, the user
        // just cannot see or stop it from the shade.
        requestPermissions(
            arrayOf(Manifest.permission.POST_NOTIFICATIONS),
            NOTIFICATION_PERMISSION_REQUEST,
        )
    }

    private fun pushBattery() {
        val svc = service ?: return
        val status = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = status?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = status?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        // §5.5: 0..100, or 255 for unknown. 101..254 are reserved and a sender
        // MUST NOT transmit them.
        svc.input.setBattery(
            if (level >= 0 && scale > 0) (level * 100 / scale).coerceIn(0, 100) else 255
        )
    }

    // ---- physical gamepad ----------------------------------------------

    /** Which [InputSnapshot] a physical pad's button/stick events should
     *  land in — [activeTransport]'s while a session (either transport) is
     *  open, [service]'s otherwise (unchanged from before this class had a
     *  second transport: harmless pre-session buffering into an
     *  [InputSnapshot] nothing is reading yet, exactly as it always was). */
    private fun liveInputSnapshot(): InputSnapshot? = activeTransport?.input ?: service?.input

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val bit = GamepadInput.bitFor(event.keyCode)
        if (bit != 0 && GamepadInput.isGamepadEvent(event)) {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> padKeyButtons = padKeyButtons or bit
                KeyEvent.ACTION_UP -> padKeyButtons = padKeyButtons and bit.inv()
            }
            liveInputSnapshot()?.setButtons(
                InputSnapshot.SRC_PAD, padKeyButtons or padHatButtons,
            )
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        val snapshot = liveInputSnapshot()
        if (snapshot != null &&
            event.actionMasked == MotionEvent.ACTION_MOVE &&
            GamepadInput.isGamepad(event.device)
        ) {
            padHatButtons = GamepadInput.applyMotion(event, snapshot)
            snapshot.setButtons(InputSnapshot.SRC_PAD, padKeyButtons or padHatButtons)
            return true
        }
        return super.onGenericMotionEvent(event)
    }
}
