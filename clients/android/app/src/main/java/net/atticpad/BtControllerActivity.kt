package net.atticpad

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher

/**
 * The guided Bluetooth Controller setup screen — see [BtHidController]'s
 * class doc for the engine underneath this and why it exists. Reachable
 * from the connect screen's own title row, via the promoted "Use Bluetooth"
 * button ([MainActivity.buildTitleRow]), on API 28+ only (task brief:
 * gate the entry point itself, per [BtHidController.isSupported]) — there
 * is no dead button on a device that cannot do this.
 *
 * Deliberately its own `Activity`, not folded into [MainActivity]: it needs
 * none of that Activity's UDP-session plumbing, and keeping it separate is
 * what makes "the app behaves byte-for-byte identically when this mode is
 * never opened" trivially true rather than something to audit.
 *
 * THREE STEPS, TWO SCREENS. [Screen.SETUP] holds steps 1 (permissions) and
 * 2 (pairing) as two panels that swap visibility inside the same screen —
 * they are sequential states of "not yet connected", not different places
 * — and [Screen.PLAY] is step 3, the connected controller overlay. Same
 * `root: FrameLayout` holding both, same "GONE the one not showing" idiom
 * this app already uses everywhere else (see [MainActivity]'s own
 * connect-screen/pad-overlay split):
 *
 *  - [Screen.SETUP] / permission panel: shown only while a required
 *    Bluetooth permission is missing. No permission name is ever printed —
 *    only the two supplied strings ("AtticPad needs Bluetooth permission
 *    to appear as a gamepad." before asking, "Bluetooth permission is off
 *    for AtticPad. You can turn it on in Settings." after a denial, with an
 *    "Open Settings" deep link to this app's own settings page).
 *  - [Screen.SETUP] / pairing panel: registration happens automatically the
 *    moment permissions are granted (no manual Register button — task
 *    brief) — "Make discoverable" is the only action a user takes here. A
 *    registration failure collapses this panel down to the ONE supplied
 *    failure string plus a "Details" expander that surfaces
 *    [BtHidController.Status.detail] for a bug report; that detail string
 *    is never shown any other way.
 *  - [Screen.PLAY]: nothing but a full-screen, landscape-locked [PadView]
 *    (same control [MainActivity] and the original spike both reuse
 *    unmodified) plus a compact status pill. Entered automatically the
 *    instant a host connects (task brief: "automatically switch ... the
 *    manual open button remains as fallback"), so "Open controller" stays
 *    on the pairing panel as a manual door too, gated on
 *    [BtHidController.Status.registered]. System back (API 33+, see
 *    [onCreate]'s [OnBackInvokedCallback]) or a tap on the pill (every API
 *    level) returns to SETUP without touching [BtHidController.unregister]
 *    — only leaving this Activity entirely does that (see [onDestroy]).
 *
 * BOOT PROTOCOL: see [BtHidController]'s "repeated e" section for what this
 * is. The ONE supplied banner string is shown verbatim in both screens
 * whenever [BtHidController.Status.bootProtocol] is true — nothing else on
 * screen ever says "BOOT protocol" or "output held"; that vocabulary is
 * logcat-only (see [BtHidController]).
 *
 * Reuses [PadView] and [GamepadInput] exactly as [MainActivity] does — a
 * touch overlay and a physical pad are both legitimate ways to drive this
 * mode, and rebuilding either here would be exactly the kind of second
 * implementation docs/CONVENTIONS.md warns about, just one layer up from the
 * protocol.
 */
class BtControllerActivity : Activity() {

    companion object {
        private const val TAG = "BtControllerActivity"
        private const val BT_PERMISSION_REQUEST = 10

        private const val PREF_LAST_HOST_NAME = "bt_last_host_name"
        private const val PREF_LAST_HOST_ADDRESS = "bt_last_host_address"

        /** Verbatim, task-brief-supplied copy — shown on screen exactly
         *  once, from exactly one constant, so the setup-screen and
         *  play-overlay banners can never drift apart. */
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

        /** How long a manual tap on a paired-devices row waits for
         *  [BtHidController.Status.connectedName] to go non-empty before
         *  giving up. [BluetoothHidDevice.connect] itself only reports
         *  whether the REQUEST was accepted, not whether the connection
         *  eventually succeeds — the real answer arrives (or never arrives)
         *  via the same [BtHidController.Status] stream every other
         *  connection state change uses, so this screen has no stronger
         *  signal than "nothing happened yet" to build a timeout from. */
        private const val CONNECT_TIMEOUT_MS = 12_000L
    }

    private enum class Screen { SETUP, PLAY }

    private val snapshot = InputSnapshot()
    private lateinit var controller: BtHidController

    // ---- SETUP screen: step 1 (permissions) ----
    private lateinit var setupPanel: View
    private lateinit var setupBootBanner: TextView
    private lateinit var permissionPanel: View
    private lateinit var permissionText: TextView
    private lateinit var openSettingsButton: Button

    // ---- SETUP screen: step 2 (pairing) ----
    private lateinit var pairingPanel: View
    private lateinit var pairingHeadline: TextView
    private lateinit var pairingInstruction: TextView
    private lateinit var discoverableButton: Button
    private lateinit var pairedListPanel: LinearLayout
    private lateinit var pairedStatusText: TextView
    private lateinit var openControllerButton: Button
    private lateinit var detailsToggle: TextView
    private lateinit var detailsText: TextView

    // ---- PLAY screen: step 3 (connected overlay) ----
    private lateinit var padView: PadView
    private lateinit var playHudColumn: LinearLayout
    private lateinit var playStatusPill: TextView
    private lateinit var playBootBanner: TextView

    private var padKeyButtons = 0
    private var padHatButtons = 0

    private var screen = Screen.SETUP
    private var edgeInsets = Insets.Edges(0, 0, 0, 0)

    /** Guards [BtHidController.register] against being called more than
     *  once — permissions can be rechecked (e.g. [onResume]) far more often
     *  than registration should actually happen. */
    private var registerRequested = false

    /** [BtHidController.Status.connectedName] as of the last status tick —
     *  kept only to detect rising/falling EDGES, not the level, so a status
     *  tick that repeats "still connected" or "still disconnected" never
     *  re-triggers a screen transition the user may have deliberately
     *  backed out of. Same pattern the original spike used for its
     *  auto-entry logic. */
    private var lastConnectedName = ""

    /** Set on the falling edge of [lastConnectedName] (a host that WAS
     *  connected disconnected) and cleared on the next successful
     *  connection — drives the "Connection lost" headline (task brief). */
    private var connectionLost = false

    /** Guards the auto-reconnect attempt in [attemptReconnectIfNeeded]
     *  against firing on every status tick while waiting — one attempt per
     *  "not currently connected" period, reset on the next successful
     *  connection so a LATER disconnect gets its own attempt too. */
    private var reconnectAttempted = false

    private var detailsExpanded = false

    /** MAC of the paired-devices row a manual tap is currently connecting
     *  to, or null when no manual attempt is in flight — drives that row's
     *  "Connecting..." label, disables the rest of the list while it's set
     *  (task brief), and is cleared either by a successful connection (see
     *  [onStatus]'s `justConnected` handling) or by
     *  [onConnectAttemptTimedOut]. */
    private var connectingAddress: String? = null

    private val handler = Handler(Looper.getMainLooper())

    /** The in-flight [onConnectAttemptTimedOut] callback for
     *  [connectingAddress], if any — kept so a NEW manual tap (or a
     *  connection succeeding through some other path) can cancel a stale
     *  timeout rather than let it fire after the fact. */
    private var connectTimeoutRunnable: Runnable? = null

    /** Same nullable-field-of-a-newer-framework-type pattern
     *  [BtHidController] already uses for `BluetoothHidDevice` (API 28) on
     *  a minSdk-26 class — the TYPE is safe to declare unconditionally;
     *  only *constructing* one is gated, in [onCreate], behind
     *  `Build.VERSION.SDK_INT`. */
    private var backCallback: OnBackInvokedCallback? = null

    /** Refreshes [pairedListPanel] on two system signals a bonded-device
     *  list can go stale from without any Activity lifecycle event firing:
     *  a device the PC just bonded to this phone while [Screen.SETUP] stays
     *  on screen the whole time (`ACTION_BOND_STATE_CHANGED`), and
     *  discoverable mode ending (`ACTION_SCAN_MODE_CHANGED`, task brief:
     *  "Refresh the list ... after discoverable-mode ends") — [onResume]
     *  alone would miss both, since neither one backgrounds this Activity. */
    private val bluetoothStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                BluetoothAdapter.ACTION_SCAN_MODE_CHANGED -> {
                    val mode = intent.getIntExtra(BluetoothAdapter.EXTRA_SCAN_MODE, BluetoothAdapter.SCAN_MODE_NONE)
                    if (mode != BluetoothAdapter.SCAN_MODE_CONNECTABLE_DISCOVERABLE) refreshPairedDevices()
                }
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> refreshPairedDevices()
            }
        }
    }

    private val prefs by lazy { getSharedPreferences("atticpad", Context.MODE_PRIVATE) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (!BtHidController.isSupported()) {
            // MainActivity only shows the entry card when this is true —
            // reaching here on an unsupported API level means the Activity
            // was launched some other way (adb, a stale shortcut). Nothing
            // to show; just leave rather than invent a screen for a state
            // the product never puts a user into.
            Log.w(TAG, "launched on unsupported API ${Build.VERSION.SDK_INT}")
            finish()
            return
        }

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        controller = BtHidController(this, snapshot)
        buildUi()
        controller.setListener { onStatus(it) }

        // Android 16 (API 36) no longer calls the deprecated
        // Activity.onBackPressed()/dispatches KEYCODE_BACK once an app
        // targets API 36 — the same landmine the original spike hit and
        // documented. OnBackInvokedCallback (API 33+) is the only reliable
        // way to intercept back now; API 26..32 have no such dispatcher at
        // all, so PLAY is simply not back-interceptible there — the pill's
        // own "tap for setup" affordance (see buildPlayHudColumn) is what
        // covers that gap, not a second back-handling code path.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val callback = OnBackInvokedCallback { onBackFromPlayOrSetup() }
            backCallback = callback
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback,
            )
        }

        val btStateFilter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_SCAN_MODE_CHANGED)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(bluetoothStateReceiver, btStateFilter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(bluetoothStateReceiver, btStateFilter)
        }

        showSetup()
        beginPermissionFlow()
    }

    override fun onResume() {
        super.onResume()
        // Covers returning from the system Settings page opened by step 1's
        // "Open Settings" — the grant can change underneath this Activity
        // with no callback firing on its own.
        if (::permissionPanel.isInitialized && permissionPanel.visibility == View.VISIBLE) {
            refreshPermissionStep()
        }
        // Task brief: "Refresh the list on resume" — covers returning from
        // system Bluetooth settings (a device paired there while this
        // Activity was backgrounded), on top of bluetoothStateReceiver's
        // own refresh for changes that happen without backgrounding it.
        if (::pairingPanel.isInitialized && pairingPanel.visibility == View.VISIBLE) {
            refreshPairedDevices()
        }
    }

    override fun onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            backCallback?.let { onBackInvokedDispatcher.unregisterOnBackInvokedCallback(it) }
        }
        unregisterReceiver(bluetoothStateReceiver)
        connectTimeoutRunnable?.let { handler.removeCallbacks(it) }
        controller.setListener(null)
        controller.unregister()
        super.onDestroy()
    }

    /** Registered as this Activity's ENTIRE back handling on API 33+ (see
     *  [onCreate]) — unlike the deprecated `onBackPressed()`/super chain, a
     *  registered [OnBackInvokedCallback] takes full ownership of the back
     *  action, so the "leave the Activity" half has to be done explicitly
     *  here too, not inherited from a `super` call. */
    private fun onBackFromPlayOrSetup() {
        if (screen == Screen.PLAY) showSetup() else finish()
    }

    // ---- step 1: permissions -----------------------------------------

    private fun missingBtPermissions(): Array<String> {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return emptyArray()
        val wanted = arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_ADVERTISE)
        return wanted.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }.toTypedArray()
    }

    private fun beginPermissionFlow() {
        val missing = missingBtPermissions()
        if (missing.isEmpty()) {
            refreshPermissionStep()
            return
        }
        permissionText.text = "AtticPad needs Bluetooth permission to appear as a gamepad."
        openSettingsButton.visibility = View.GONE
        refreshPermissionStep()
        requestPermissions(missing, BT_PERMISSION_REQUEST)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != BT_PERMISSION_REQUEST) return
        if (missingBtPermissions().isNotEmpty()) {
            permissionText.text = "Bluetooth permission is off for AtticPad. You can turn it on in Settings."
            openSettingsButton.visibility = View.VISIBLE
            return
        }
        refreshPermissionStep()
    }

    private fun refreshPermissionStep() {
        val missing = missingBtPermissions()
        if (missing.isEmpty()) {
            permissionPanel.visibility = View.GONE
            pairingPanel.visibility = View.VISIBLE
            ensureRegistered()
            refreshPairedDevices()
        } else {
            permissionPanel.visibility = View.VISIBLE
            pairingPanel.visibility = View.GONE
        }
    }

    private fun openBtSettings() {
        startActivity(
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                .setData(Uri.fromParts("package", packageName, null)),
        )
    }

    // ---- step 2: pairing -----------------------------------------------

    /** Auto-register on entry (task brief: "no manual Register button") —
     *  called every time [refreshPermissionStep] finds permissions already
     *  granted, but [registerRequested] makes only the FIRST such call
     *  actually reach [BtHidController.register]. */
    private fun ensureRegistered() {
        if (registerRequested) return
        registerRequested = true
        controller.register()
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

    private fun toggleDetails() {
        detailsExpanded = !detailsExpanded
        detailsText.visibility = if (detailsExpanded) View.VISIBLE else View.GONE
    }

    /** [BtHidController.Status.connectedAddress] of the device to try
     *  reconnecting to, looked up against [BtHidController.bondedDevices]
     *  by address and handed to [BtHidController.connectTo] — a phone's own
     *  HID Device role does not get the automatic host-initiated
     *  reconnects a bonded mouse/keyboard would, so this side has to ask.
     *  Fires at most once per "not currently connected" period; see
     *  [reconnectAttempted]'s own doc. */
    private fun attemptReconnectIfNeeded(s: BtHidController.Status) {
        if (!s.registered || s.connectedName.isNotEmpty() || reconnectAttempted) return
        val address = prefs.getString(PREF_LAST_HOST_ADDRESS, null) ?: return
        @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by step 1
        val device = controller.bondedDevices().firstOrNull { it.address == address } ?: return
        reconnectAttempted = true
        controller.connectTo(device)
    }

    // ---- step 2: paired-devices list --------------------------------------

    /** Bonded devices can include phones, headsets, earbuds, watches...  —
     *  filtered to the [BluetoothClass] major categories a PC, or a
     *  Bluetooth-passthrough peripheral (e.g. a receiver dongle) sitting in
     *  front of one, would actually report. A device this adapter has no
     *  class for (`bluetoothClass == null` — seen on some emulator/vendor
     *  stacks) is INCLUDED rather than hidden: task brief — "if filtering
     *  is unreliable, show all bonded devices" — hiding a real, bonded PC
     *  because its class byte came back empty would be worse than one extra
     *  row in the list. */
    @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by step 1
    private fun isLikelyComputer(device: BluetoothDevice): Boolean {
        val major = device.bluetoothClass?.majorDeviceClass ?: return true
        return major == BluetoothClass.Device.Major.COMPUTER || major == BluetoothClass.Device.Major.PERIPHERAL
    }

    @Suppress("MissingPermission") // BLUETOOTH_CONNECT already secured by step 1
    private fun pairedRowLabel(device: BluetoothDevice, connecting: Boolean): String {
        val name = try { device.name ?: device.address } catch (e: SecurityException) { device.address }
        return if (connecting) "$name  —  Connecting..." else name
    }

    /** Rebuilds [pairedListPanel] from [BtHidController.bondedDevices] —
     *  called on [onResume], from [bluetoothStateReceiver], from
     *  [refreshPermissionStep] once permissions land, and around a manual
     *  [connectToDevice] attempt to render its "Connecting..." row. Silently
     *  a no-op while a Bluetooth permission is still missing: the pairing
     *  panel that hosts this list is GONE in that state anyway (see
     *  [refreshPermissionStep]), and [BtHidController.bondedDevices] itself
     *  is only safe to call once step 1 has secured `BLUETOOTH_CONNECT`. */
    private fun refreshPairedDevices() {
        if (!::pairedListPanel.isInitialized || missingBtPermissions().isNotEmpty()) return
        pairedListPanel.removeAllViews()
        val devices = controller.bondedDevices().filter { isLikelyComputer(it) }
        if (devices.isEmpty()) {
            pairedListPanel.addView(captionText(this, PAIRED_EMPTY_TEXT))
            return
        }
        for (device in devices) {
            val connecting = device.address == connectingAddress
            val row = rowButton(this, pairedRowLabel(device, connecting)) { connectToDevice(device) }
            row.isEnabled = connectingAddress == null
            pairedListPanel.addView(row, vlp(dp(Theme.SPACE_XS)))
        }
    }

    /** A manual tap on a paired-devices row — task brief: "a manual tap on
     *  a different row overrides" the automatic reconnect, so this also
     *  marks [reconnectAttempted] to stop [attemptReconnectIfNeeded] from
     *  independently dialing the remembered host on the same tick. Success
     *  is discovered the same way every other connection is (see
     *  [onStatus]'s `justConnected` handling, which clears
     *  [connectingAddress]); failure is a timeout, since
     *  [BtHidController.connectTo] has no stronger signal to offer (see
     *  [CONNECT_TIMEOUT_MS]'s own doc). */
    private fun connectToDevice(device: BluetoothDevice) {
        if (connectingAddress != null) return
        connectingAddress = device.address
        reconnectAttempted = true
        pairedStatusText.visibility = View.GONE
        refreshPairedDevices()
        controller.connectTo(device)

        connectTimeoutRunnable?.let { handler.removeCallbacks(it) }
        val runnable = Runnable { onConnectAttemptTimedOut() }
        connectTimeoutRunnable = runnable
        handler.postDelayed(runnable, CONNECT_TIMEOUT_MS)
    }

    private fun onConnectAttemptTimedOut() {
        connectTimeoutRunnable = null
        if (connectingAddress == null) return // already resolved elsewhere
        connectingAddress = null
        pairedStatusText.text = PAIRED_CONNECT_FAILED_TEXT
        pairedStatusText.visibility = View.VISIBLE
        refreshPairedDevices()
    }

    // ---- UI --------------------------------------------------------------

    private fun dp(v: Int): Int = Theme.dp(this, v)

    private fun buildUi() {
        val root = FrameLayout(this)
        root.setBackgroundColor(Theme.BG)

        padView = PadView(this)
        padView.snapshot = snapshot
        padView.layout = PadLayout.default(PadLayout.Orientation.LANDSCAPE)
        padView.visibility = View.GONE
        root.addView(
            padView,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        playHudColumn = buildPlayHudColumn()
        // Same reason MainActivity's HUD listens to its own layout changes:
        // the column's measured height varies (boot-protocol banner
        // appearing/disappearing), so what updatePadViewInsets reserves has
        // to be re-derived on every layout pass, not guessed once.
        playHudColumn.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updatePadViewInsets() }
        root.addView(
            playHudColumn,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL,
            ),
        )

        setupPanel = buildSetupPanel()
        root.addView(
            setupPanel,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
        )

        setContentView(root)

        Insets.listen(root) { edges ->
            edgeInsets = edges
            applyInsetsToViews()
        }
    }

    private fun hudPill(): TextView = TextView(this).apply {
        setTextColor(Theme.TEXT_PRIMARY)
        textSize = Theme.TEXT_LABEL
        background = Theme.hudBackground(this@BtControllerActivity)
        val padH = dp(Theme.SPACE_MD)
        val padV = dp(Theme.SPACE_XS)
        setPadding(padH, padV, padH, padV)
        visibility = View.GONE
        // A second, always-visible way back besides the system back
        // button/gesture — a full-screen, edge-to-edge PadView can
        // legitimately eat an edge swipe that would otherwise be a back
        // gesture, same as MainActivity's own HUD doubles as its menu entry.
        setOnClickListener { showSetup() }
    }

    private fun buildPlayHudColumn(): LinearLayout {
        playStatusPill = hudPill()
        playBootBanner = hudPill().apply {
            setTextColor(Theme.WARNING)
            text = BOOT_PROTOCOL_BANNER
        }
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            addView(playStatusPill, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(
                playBootBanner,
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    topMargin = dp(Theme.SPACE_XS)
                },
            )
        }
    }

    private fun buildSetupPanel(): View {
        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = dp(Theme.SPACE_MD)
            setPadding(pad, pad, pad, pad)
        }
        controls.addView(titleText(this, "Bluetooth Controller"))

        setupBootBanner = bodyText(this, BOOT_PROTOCOL_BANNER, Theme.WARNING).apply {
            visibility = View.GONE
        }
        controls.addView(setupBootBanner, vlp(dp(Theme.SPACE_MD)))

        permissionPanel = buildPermissionPanel()
        controls.addView(permissionPanel, vlp(dp(Theme.SPACE_MD)))

        pairingPanel = buildPairingPanel()
        controls.addView(pairingPanel, vlp(dp(Theme.SPACE_MD)))

        // MATCH_PARENT/MATCH_PARENT, ScrollView only for a short phone in
        // landscape where the pairing panel's expanded Details text can run
        // past the bottom.
        return ScrollView(this).apply { addView(controls) }
    }

    private fun buildPermissionPanel(): View {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        permissionText = bodyText(this, "", Theme.TEXT_SECONDARY)
        col.addView(permissionText)
        openSettingsButton = secondaryButton(this, "Open Settings") { openBtSettings() }.apply {
            visibility = View.GONE
        }
        col.addView(openSettingsButton, vlp(dp(Theme.SPACE_SM)))
        return col
    }

    private fun buildPairingPanel(): View {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        pairingHeadline = bodyText(this, "", Theme.WARNING).apply {
            textSize = Theme.TEXT_TITLE
            visibility = View.GONE
        }
        col.addView(pairingHeadline)
        pairingInstruction = bodyText(this, "", Theme.TEXT_SECONDARY)
        col.addView(pairingInstruction, vlp(dp(Theme.SPACE_SM)))

        discoverableButton = primaryButton(this, "Make discoverable") { makeDiscoverable() }
        col.addView(discoverableButton, vlp(dp(Theme.SPACE_MD)))

        // The paired-devices list — restored from the original spike's own
        // bonded-device list (git history: BtHidSpikeActivity), re-skinned
        // to this guided flow's widgets ([[Theme]]'s sectionLabel/rowButton/
        // captionText) rather than the spike's ad hoc styling. See
        // [refreshPairedDevices] for the enumeration/filter and
        // [connectToDevice] for what a tap does.
        col.addView(sectionLabel(this, "Paired computers"), vlp(dp(Theme.SPACE_MD)))
        pairedListPanel = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        col.addView(pairedListPanel, vlp(dp(Theme.SPACE_XS)))
        // Only ever shown for a manual connect attempt that timed out (see
        // onConnectAttemptTimedOut) — hidden the rest of the time, not a
        // permanent fixture under the list.
        pairedStatusText = captionText(this, "", Theme.ERROR).apply { visibility = View.GONE }
        col.addView(pairedStatusText, vlp(dp(Theme.SPACE_XS)))

        // Visible once registered — task brief: the auto-open on connect
        // (see onStatus) is the normal path, this stays as the fallback
        // door until that auto-open has a hardware round confirming it.
        openControllerButton = primaryButton(this, "Open controller") { showPlay() }.apply {
            visibility = View.GONE
        }
        col.addView(openControllerButton, vlp(dp(Theme.SPACE_MD)))

        // A registration failure's ONLY on-screen detail — see
        // BtHidController.Status.detail's own doc: never printed directly,
        // only reachable by tapping this.
        detailsToggle = captionText(this, "Details").apply {
            isClickable = true
            isFocusable = true
            visibility = View.GONE
            setOnClickListener { toggleDetails() }
        }
        col.addView(detailsToggle, vlp(dp(Theme.SPACE_MD)))
        detailsText = TextView(this).apply {
            setTextColor(Theme.TEXT_MUTED)
            textSize = Theme.TEXT_CAPTION
            typeface = android.graphics.Typeface.MONOSPACE
            visibility = View.GONE
        }
        col.addView(detailsText, vlp(dp(Theme.SPACE_XS)))

        return col
    }

    private fun vlp(topMargin: Int): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            this.topMargin = topMargin
        }

    // ---- SETUP <-> PLAY ---------------------------------------------------

    private fun showSetup() {
        screen = Screen.SETUP
        padView.visibility = View.GONE
        playHudColumn.visibility = View.GONE
        setupPanel.visibility = View.VISIBLE
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
        updatePadViewInsets()
    }

    private fun showPlay() {
        screen = Screen.PLAY
        setupPanel.visibility = View.GONE
        padView.visibility = View.VISIBLE
        playHudColumn.visibility = View.VISIBLE
        // A gamepad overlay is a landscape object — this mode only ever had
        // a LANDSCAPE PadLayout (see buildUi), so the honest thing is to
        // lock rotation to match rather than let a portrait phone show
        // landscape-shaped controls squeezed into a portrait frame.
        // SENSOR_LANDSCAPE (not plain LANDSCAPE): still follows the sensor
        // between the two landscape rotations, just never portrait.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        updatePadViewInsets()
    }

    private fun applyInsetsToViews() {
        val e = edgeInsets
        (playHudColumn.layoutParams as? FrameLayout.LayoutParams)?.let {
            it.topMargin = e.top + dp(Theme.SPACE_SM)
            playHudColumn.layoutParams = it
        }
        updatePadViewInsets()
    }

    /** The HUD column's MEASURED footprint becomes an extra top inset, so
     *  [PadView] structurally cannot lay out a control under it — in either
     *  landscape rotation, and whether or not the boot-protocol banner is
     *  currently part of that footprint — without a second bounding box to
     *  keep in sync by hand. */
    private fun updatePadViewInsets() {
        val e = edgeInsets
        val reserve = if (playHudColumn.visibility == View.VISIBLE) {
            playHudColumn.bottom + dp(Theme.SPACE_SM)
        } else {
            0
        }
        padView.setInsets(e.left, maxOf(e.top, reserve), e.right, e.bottom)
    }

    // ---- status -> UI -------------------------------------------------

    private fun onStatus(s: BtHidController.Status) {
        // Developer-only: nothing on screen ever echoes s.detail except
        // behind the explicit "Details" tap in the failure state below.
        Log.d(TAG, "status: $s")

        if (s.connectedName.isNotEmpty() && s.connectedAddress.isNotEmpty()) {
            prefs.edit()
                .putString(PREF_LAST_HOST_NAME, s.connectedName)
                .putString(PREF_LAST_HOST_ADDRESS, s.connectedAddress)
                .apply()
        }

        val justConnected = s.connectedName.isNotEmpty() && lastConnectedName.isEmpty()
        val justDisconnected = s.connectedName.isEmpty() && lastConnectedName.isNotEmpty()
        lastConnectedName = s.connectedName
        if (justConnected) {
            connectionLost = false
            reconnectAttempted = false
            // Any successful connection — manual tap or automatic reconnect
            // — resolves whatever manual attempt was in flight, regardless
            // of which device it targeted; see connectToDevice's own doc.
            if (connectingAddress != null || connectTimeoutRunnable != null) {
                connectingAddress = null
                connectTimeoutRunnable?.let { handler.removeCallbacks(it) }
                connectTimeoutRunnable = null
            }
        }
        if (justDisconnected) connectionLost = true

        attemptReconnectIfNeeded(s)
        updatePairingPanel(s)
        updatePlayHud(s)

        // Auto-enter PLAY the moment a host connects (task brief), only on
        // the rising edge and only from SETUP — a user already in PLAY (or
        // who backed out of it while still connected) must never be yanked
        // anywhere. A disconnect always returns to SETUP (task brief:
        // "Disconnection returns to step 2").
        if (justConnected && screen == Screen.SETUP) showPlay()
        if (justDisconnected) showSetup()
    }

    private fun updatePairingPanel(s: BtHidController.Status) {
        setupBootBanner.visibility = if (s.bootProtocol) View.VISIBLE else View.GONE

        if (s.registrationFailed) {
            pairingHeadline.visibility = View.VISIBLE
            pairingHeadline.text = "This phone's Bluetooth doesn't support controller mode."
            pairingHeadline.setTextColor(Theme.ERROR)
            pairingInstruction.visibility = View.GONE
            discoverableButton.visibility = View.GONE
            openControllerButton.visibility = View.GONE
            detailsToggle.visibility = View.VISIBLE
            detailsText.text = s.detail
            return
        }

        detailsToggle.visibility = View.GONE
        detailsText.visibility = View.GONE
        detailsExpanded = false

        openControllerButton.visibility = if (s.registered) View.VISIBLE else View.GONE
        discoverableButton.visibility = View.VISIBLE

        if (connectionLost) {
            pairingHeadline.visibility = View.VISIBLE
            pairingHeadline.text = "Connection lost"
            pairingHeadline.setTextColor(Theme.WARNING)
        } else {
            pairingHeadline.visibility = View.GONE
        }

        val lastHostName = prefs.getString(PREF_LAST_HOST_NAME, null)
        pairingInstruction.visibility = View.VISIBLE
        pairingInstruction.text = if (!lastHostName.isNullOrEmpty() && s.connectedName.isEmpty()) {
            "Reconnecting to $lastHostName..."
        } else {
            "On your PC, open Bluetooth settings and add a new device. Choose AtticPad."
        }
    }

    private fun updatePlayHud(s: BtHidController.Status) {
        playBootBanner.visibility = if (s.bootProtocol) View.VISIBLE else View.GONE
        playStatusPill.visibility = if (s.bootProtocol) View.GONE else View.VISIBLE
        if (!s.bootProtocol) {
            playStatusPill.text = buildString {
                append(if (s.connectedName.isNotEmpty()) s.connectedName else "no host connected")
                if (s.streaming) append(" · ${s.reportsPerSec} reports/s")
                append("   [tap for setup]")
            }
        }
    }

    // ---- physical gamepad passthrough — identical shape to MainActivity ---

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val bit = GamepadInput.bitFor(event.keyCode)
        if (bit != 0 && GamepadInput.isGamepadEvent(event)) {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> padKeyButtons = padKeyButtons or bit
                KeyEvent.ACTION_UP -> padKeyButtons = padKeyButtons and bit.inv()
            }
            snapshot.setButtons(InputSnapshot.SRC_PAD, padKeyButtons or padHatButtons)
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_MOVE && GamepadInput.isGamepad(event.device)) {
            padHatButtons = GamepadInput.applyMotion(event, snapshot)
            snapshot.setButtons(InputSnapshot.SRC_PAD, padKeyButtons or padHatButtons)
            return true
        }
        return super.onGenericMotionEvent(event)
    }
}
