package net.atticpad

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
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
import android.widget.ScrollView
import android.widget.TextView

/**
 * The only Activity. It owns the screen and nothing else: the session, the
 * socket and the WifiLock all live in [AtticPadService], so nothing here
 * dying takes input down with it (docs/DESIGN.md §7.2).
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

        /** How long a run of taps on the connect screen's title is allowed
         *  to take before it stops counting as one gesture — see
         *  [onTitleTap]. Generous enough for a deliberate triple-tap,
         *  tight enough that three ordinary, unrelated taps spread across
         *  a session never accidentally add up to one. */
        private const val TITLE_TAP_WINDOW_MS = 600L
        private const val TITLE_TAP_COUNT = 3
    }

    private lateinit var root: FrameLayout
    private lateinit var connectPanel: View
    private lateinit var padView: PadView
    private lateinit var hud: TextView
    private lateinit var statusLine: TextView
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
        super.onPause()
    }

    override fun onDestroy() {
        if (bound) {
            service?.setListener(null)
            unbindService(connection)
            bound = false
        }
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
     * This is why it is measured, not guessed: the HUD's status line is
     * variable-length text ("pad 0 · 60 Hz · RTT 1 ms · tx … rx … [menu]"),
     * WRAP_CONTENT, and can wrap to two lines on a narrow phone in
     * portrait — a fixed dp guess would be exactly the kind of constant
     * that works on the device it was tuned on and nowhere else.
     */
    private fun updatePadViewInsets() {
        if (!::padView.isInitialized) return
        val e = edgeInsets
        val hudReserve = if (::hud.isInitialized && hud.visibility == View.VISIBLE) {
            hud.bottom + dp(Theme.SPACE_SM)
        } else {
            0
        }
        padView.setInsets(e.left, maxOf(e.top, hudReserve), e.right, e.bottom)
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

        connectPanel = buildConnectPanel()
        root.addView(
            connectPanel,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            ),
        )

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

    private fun buildHud(): TextView = TextView(this).apply {
        setTextColor(Theme.TEXT_PRIMARY)
        textSize = Theme.TEXT_LABEL
        background = Theme.hudBackground(this@MainActivity)
        val padH = dp(Theme.SPACE_MD)
        val padV = dp(Theme.SPACE_XS)
        setPadding(padH, padV, padH, padV)
        visibility = View.GONE
        setOnClickListener { showMenu() }
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
                primaryButton(this@MainActivity, "Use Bluetooth") {
                    startActivity(Intent(this@MainActivity, BtControllerActivity::class.java))
                },
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
        ipField = styledEditText(this, "192.168.0.10").apply {
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
                connectPanel.visibility = View.GONE
                padView.visibility = View.VISIBLE
                hud.visibility = View.VISIBLE
                updatePadViewInsets()
                // The connect screen (and Card 1's live preview inside it)
                // is hidden, not merely covered — a View going GONE does
                // NOT release the camera on its own, and an inline scanner
                // left running behind an ACTIVE session would hold the
                // camera open and draining battery for no reason (the same
                // "must not starve whatever the user switched to" rule
                // onPause already applies, just triggered by a state change
                // instead of a lifecycle one). refreshScannerCard() in the
                // `else` branch below is what brings it back to life if the
                // session ends and this screen reappears.
                stopScanner()
                // A connect succeeding is the "cleared ... when a connect
                // succeeds" case (task brief) — the pad overlay itself is
                // the proof, there is nothing left for a status line to say.
                clearStatus()
                attemptPending = false
                hud.text = buildString {
                    append("pad ${s.padSlot} · ${s.rateHz} Hz · ")
                    append(if (s.rttMs >= 0) "RTT ${s.rttMs} ms" else "RTT —")
                    append(" · tx ${s.txPackets} rx ${s.rxPackets}")
                    if (s.message.isNotEmpty()) append(" · ${s.message}")
                    append("   [menu]")
                }
            }

            AtticPadNative.STATE_HANDSHAKING -> {
                showStatus("Connecting to ${s.target}…", Theme.TEXT_SECONDARY)
            }

            else -> {
                padView.visibility = View.GONE
                hud.visibility = View.GONE
                updatePadViewInsets()
                connectPanel.visibility = View.VISIBLE
                // The mirror image of the STATE_ACTIVE branch's
                // stopScanner(): the connect screen is visible again (a
                // fresh launch, a disconnect, a dropped session), so Card 1
                // should be live again if permission allows — idempotent,
                // safe to call on every status tick this branch handles.
                refreshScannerCard()
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

        val dialog = AlertDialog.Builder(this).setView(col).create()
        fun item(label: String, action: () -> Unit) {
            col.addView(
                rowButton(this, label) { action(); dialog.dismiss() },
                vlp(dp(Theme.SPACE_XS)),
            )
        }

        item(if (padView.editing) "Finish editing layout" else "Edit layout") {
            padView.editing = !padView.editing
            if (!padView.editing) PadLayout.save(this, padView.layout, currentOrientation)
            hud.text = if (padView.editing)
                "EDIT MODE — drag controls, then tap here to save"
            else "layout saved   [menu]"
        }
        item("Reset layout (this orientation)") {
            PadLayout.reset(this, currentOrientation)
            padView.layout = PadLayout.default(currentOrientation)
        }
        item("Self-test") { showSelfTest() }
        item("Disconnect") { service?.stopSession() }

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

        val dialog = AlertDialog.Builder(this).setView(col).create()
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

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val bit = GamepadInput.bitFor(event.keyCode)
        if (bit != 0 && GamepadInput.isGamepadEvent(event)) {
            when (event.action) {
                KeyEvent.ACTION_DOWN -> padKeyButtons = padKeyButtons or bit
                KeyEvent.ACTION_UP -> padKeyButtons = padKeyButtons and bit.inv()
            }
            service?.input?.setButtons(
                InputSnapshot.SRC_PAD, padKeyButtons or padHatButtons,
            )
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        val snapshot = service?.input
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
