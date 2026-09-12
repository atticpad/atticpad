package net.atticpad

import android.content.Context
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView

/**
 * The MOUSE/KEYBOARD/MEDIA mode bodies and the PAD/MOUSE/KEYBOARD/MEDIA chip
 * row — [MainActivity]'s ONE session overlay, bound to whichever
 * [SessionTransport] is currently live (UDP's [AtticPadService], or
 * Bluetooth's [BluetoothTransport]) — "the same UI, a different connection"
 * (job 2 task brief, still true after job 3 folded the Bluetooth session
 * into this same overlay instead of a second Activity's copy of it — see
 * [MainActivity]'s own class doc). Neither this file nor
 * [TrackpadView]/[KeyGridView]/[TextEntryBar]/[MediaRemoteView] underneath it
 * know or care which wire a [KbmSnapshot] eventually drains onto: every
 * producer here only ever touches that one class (see its own doc comment),
 * so the transport boundary is already exactly at the object [MainActivity]
 * hands these bodies, not inside them. Extracted verbatim from
 * [MainActivity]'s original per-mode builders (the 2026-08-25 KBM redesign) —
 * behaviour is unchanged, only the ownership moved so both transports can
 * bind the identical screen.
 */
enum class KbmMode { PAD, MOUSE, KEYBOARD, MEDIA }

/** MOUSE mode's body: a full-bleed [TrackpadView], two click buttons and a
 *  collapsible [TextEntryBar]. `kbm` is settable AFTER construction (not a
 *  constructor param) because [MainActivity] builds this body before either
 *  transport's `KbmSnapshot` exists to bind it to — see
 *  [MainActivity.enterSession]. */
class MouseBody(context: Context) {
    var kbm: KbmSnapshot? = null
        set(value) {
            field = value
            trackpad.kbm = value
            textEntry.kbm = value
        }

    val trackpad = TrackpadView(context)
    val textEntry = TextEntryBar(context)
    val view: LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Theme.BG)
    }

    /** Captured once, before any inset is ever applied — see
     *  [MainActivity]'s `mouseBodyBasePadding` doc for why a naive
     *  `setPadding(insets...)` later would otherwise erase this. Always
     *  `(0,0,0,0)` today (this body sets no padding of its own), kept for
     *  symmetry with [MediaBody], whose base padding is NOT zero. */
    val basePadding: IntArray

    init {
        view.addView(trackpad, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val clickRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        clickRow.addView(
            mouseClickButton(context, "L", AtticPadNative.MOUSEBTN_LEFT),
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f),
        )
        clickRow.addView(
            mouseClickButton(context, "R", AtticPadNative.MOUSEBTN_RIGHT),
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = Theme.dp(context, Theme.SPACE_XS)
            },
        )
        view.addView(
            clickRow,
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                val m = Theme.dp(context, Theme.SPACE_XS)
                leftMargin = m; rightMargin = m; topMargin = m
            },
        )
        view.addView(textEntry, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        basePadding = intArrayOf(view.paddingLeft, view.paddingTop, view.paddingRight, view.paddingBottom)
    }

    private fun mouseClickButton(context: Context, label: String, button: Int): View =
        secondaryButton(context, label) {}.apply {
            minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
            setOnTouchListener { v, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> { v.isPressed = true; kbm?.mouseButtonEvent(button, true) }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        v.isPressed = false
                        kbm?.mouseButtonEvent(button, false)
                    }
                }
                true
            }
        }
}

/** KEYBOARD mode's body: a compact dual-gutter [TrackpadView] over a
 *  self-measuring [KeyGridView] — see [KeyGridView]'s own class doc for the
 *  responsive height math that needs no orientation-specific code. */
class KeyboardBody(context: Context) {
    var kbm: KbmSnapshot? = null
        set(value) {
            field = value
            trackpad.kbm = value
            keyGrid.kbm = value
        }

    val trackpad = TrackpadView(context).apply { dualGutter = true }
    val keyGrid = KeyGridView(context)
    val view: LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Theme.BG)
    }
    val basePadding: IntArray

    init {
        view.addView(trackpad, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        view.addView(keyGrid, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        basePadding = intArrayOf(view.paddingLeft, view.paddingTop, view.paddingRight, view.paddingBottom)
    }
}

/** MEDIA mode's body — [MediaRemoteView] is already fully self-contained
 *  (it owns its own trackpad and D-pad internally), so this is just a base-
 *  padding capture to match [MouseBody]/[KeyboardBody]'s shape, not a new
 *  wrapper. */
fun buildMediaBody(context: Context): Pair<MediaRemoteView, IntArray> {
    val view = MediaRemoteView(context).apply { setBackgroundColor(Theme.BG) }
    val base = intArrayOf(view.paddingLeft, view.paddingTop, view.paddingRight, view.paddingBottom)
    return view to base
}

/**
 * Applies every edge inset ON TOP OF a body's own captured [base] padding,
 * never replacing it — `View.setPadding()` replaces all four sides, and a
 * naive `setPadding(insets...)` would silently erase a body's own design
 * margins (see [MediaRemoteView]'s constructor padding). [chromeReserve] is
 * whatever chrome (HUD/mode bar/status pill) sits above this body and must
 * not be drawn under — the caller measures that, this only combines it with
 * the window's own top inset.
 */
fun applyKbmBodyInsets(body: View?, base: IntArray, e: Insets.Edges, chromeReserve: Int) {
    body ?: return
    body.setPadding(
        base[0] + e.left,
        maxOf(base[1] + e.top, chromeReserve),
        base[2] + e.right,
        base[3] + e.bottom,
    )
}

/**
 * Leaving a facility does not clear anything on the host by itself — see
 * [KbmSnapshot]'s own class doc ("RELEASE ON MODE SWITCH"). This is the one
 * piece of mode-switch bookkeeping that is completely transport-agnostic (it
 * only ever touches [KbmSnapshot]), so both Activities call the same
 * function rather than each re-deriving which facilities ride with which
 * mode.
 */
fun applyKbmActivity(kbm: KbmSnapshot?, mode: KbmMode, mouseTextEntry: TextEntryBar?) {
    kbm ?: return
    kbm.setMouseActive(mode != KbmMode.PAD)
    kbm.setKeyboardActive(mode == KbmMode.MOUSE || mode == KbmMode.KEYBOARD || mode == KbmMode.MEDIA)
    kbm.setMediaActive(mode == KbmMode.MEDIA)
    if (mode != KbmMode.MOUSE) mouseTextEntry?.reset()
}

// ---- the chip row ----------------------------------------------------------

/**
 * Builds the PAD/MOUSE/KEYBOARD/MEDIA chip row itself — the same row for
 * either transport, styled by [styleModeChip]. `includeMenu` exists because
 * [KbmModeBar]'s Bluetooth-setup callers-to-be were considered and rejected:
 * every ROW [MainActivity] builds now includes the `☰` menu (job 1, still
 * true after job 3 — see [MainActivity.buildModeBar]'s own doc for why a
 * Bluetooth session gets the same menu, not a status pill any more); the
 * flag survives only because a future embedding with no menu concept at all
 * is easy to imagine and costs nothing to keep possible.
 */
class KbmChipRow(context: Context, includeMenu: Boolean, onSelect: (KbmMode) -> Unit, onMenu: (() -> Unit)?) {
    val view: LinearLayout
    val chips: Map<KbmMode, TextView>
    val menuChip: TextView?

    init {
        val bar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            background = Theme.hudBackground(context)
            val padH = Theme.dp(context, Theme.SPACE_SM); val padV = Theme.dp(context, Theme.SPACE_XS)
            setPadding(padH, padV, padH, padV)
        }
        val map = LinkedHashMap<KbmMode, TextView>()
        fun chip(mode: KbmMode, label: String): TextView =
            TextView(context).apply {
                text = label
                textSize = Theme.TEXT_LABEL
                setTextColor(Theme.TEXT_PRIMARY)
                val padH = Theme.dp(context, Theme.SPACE_MD); val padV = Theme.dp(context, Theme.SPACE_SM)
                setPadding(padH, padV, padH, padV)
                gravity = Gravity.CENTER
                // Was TOUCH_MIN - SPACE_LG (24dp), which landed around 33dp
                // once padding was added -- under the 48dp floor Theme itself
                // declares. updatePadViewInsets measures modeBar.bottom
                // dynamically, so the chrome reserve follows the taller bar
                // on its own with no constant to retune.
                minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
                // An UNSELECTED chip previously had background = null, i.e.
                // no press feedback at all -- tapping MOUSE did nothing
                // visible until the selection itself landed.
                background = Theme.chipBackground(context)
                isClickable = true
                isFocusable = true
                setOnClickListener { onSelect(mode) }
            }
        map[KbmMode.PAD] = chip(KbmMode.PAD, "PAD")
        map[KbmMode.MOUSE] = chip(KbmMode.MOUSE, "MOUSE")
        map[KbmMode.KEYBOARD] = chip(KbmMode.KEYBOARD, "KEYBOARD")
        map[KbmMode.MEDIA] = chip(KbmMode.MEDIA, "MEDIA")
        for ((i, c) in map.values.withIndex()) {
            bar.addView(
                c,
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                ).apply { if (i > 0) marginStart = Theme.dp(context, Theme.SPACE_XS) },
            )
        }
        chips = map

        if (includeMenu && onMenu != null) {
            bar.addView(
                View(context).apply { setBackgroundColor(Theme.OUTLINE) },
                LinearLayout.LayoutParams(Theme.dp(context, 1), ViewGroup.LayoutParams.MATCH_PARENT).apply {
                    marginStart = Theme.dp(context, Theme.SPACE_SM)
                    marginEnd = Theme.dp(context, Theme.SPACE_XS)
                    val vm = Theme.dp(context, Theme.SPACE_XS)
                    topMargin = vm; bottomMargin = vm
                },
            )
            val chip = TextView(context).apply {
                text = "☰"
                textSize = Theme.TEXT_TITLE
                setTextColor(Theme.TEXT_SECONDARY)
                gravity = Gravity.CENTER
                val padH = Theme.dp(context, Theme.SPACE_SM); val padV = Theme.dp(context, Theme.SPACE_SM)
                setPadding(padH, padV, padH, padV)
                minimumWidth = Theme.dp(context, Theme.TOUCH_MIN)
                minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
                background = Theme.chipBackground(context)
                isClickable = true
                isFocusable = true
                contentDescription = "Menu"
                setOnClickListener { onMenu() }
            }
            bar.addView(chip, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            menuChip = chip
        } else {
            menuChip = null
        }

        view = bar
    }
}

/** Pure styling for one chip — selected pill background/text colour,
 *  enabled/dimmed for "not available right now". Shared so [MainActivity]'s
 *  per-INPUTCAPS-bit gating (UDP — [MainActivity.refreshModeBar]) and its
 *  always-available gating (Bluetooth has no server and nothing like
 *  INPUTCAPS to gate on — [MainActivity.refreshModeBarForBluetooth]) both
 *  draw the exact same look for the exact same states. */
fun styleModeChip(context: Context, chip: TextView, mode: KbmMode, selected: KbmMode, available: Boolean) {
    // IDEMPOTENCE IS LOAD-BEARING HERE, not an optimisation. This is called
    // from MainActivity.refreshChipAvailability on EVERY ACTIVE status tick,
    // not only when the selection changes -- so anything with a visible
    // transition (a colour animator, a ripple restart, a background swap)
    // would re-fire continuously on a live connection. A static screenshot
    // cannot show that; only watching the running app can. Early-return when
    // nothing about this chip's state actually changed.
    // The check reads the chip's OWN current properties rather than a stashed
    // tag: View.setTag(int, Any) rejects any key that does not look like a
    // resource id (it throws unless `key ushr 24 >= 2`), and this module has no
    // res/values/ids.xml to draw a real one from. Comparing the two properties
    // this function actually sets is both crash-free and self-describing.
    val isSelectedNow = chip.currentTextColor == Theme.ACCENT_ON
    val isAvailableNow = chip.alpha == 1f
    if (isSelectedNow == (mode == selected) && isAvailableNow == available) return

    chip.isEnabled = available
    chip.alpha = if (available) 1f else 0.35f
    if (mode == selected) {
        chip.background = Theme.chipSelectedBackground(context)
        chip.setTextColor(Theme.ACCENT_ON)
        chip.typeface = Theme.TYPE_MEDIUM
    } else {
        // Not null: a transparent ripple, so an unselected chip still
        // acknowledges a press.
        chip.background = Theme.chipBackground(context)
        chip.setTextColor(Theme.TEXT_PRIMARY)
        chip.typeface = Theme.TYPE_REGULAR
    }
}

