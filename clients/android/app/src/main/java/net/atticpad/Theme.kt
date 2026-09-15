package net.atticpad

import android.animation.ValueAnimator
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.util.TypedValue
import android.view.View
import android.view.WindowInsets
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import android.view.animation.Interpolator
import android.view.animation.OvershootInterpolator
import android.view.animation.PathInterpolator
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.TextView

/**
 * The whole visual language, in one file, on purpose (M-UI task brief: "a
 * spacing scale, a type scale, a colour palette, and consistent corner radii
 * — not magic numbers scattered through MainActivity").
 *
 * There is no XML here — no `res/values/colors.xml`, no styles, no themes
 * overlay — because there is no `res/` directory at all in this module
 * (docs/CONVENTIONS.md: zero third-party dependencies, framework views built in code).
 * A GradientDrawable/RippleDrawable built at runtime costs nothing a drawable
 * resource would have, and it keeps the whole system in one reviewable file
 * instead of split across res/values and res/drawable.
 *
 * Dark by default (used next to a game, often in a dim room). The palette is
 * the project's own brand mark, not a colour invented for this client:
 * `clients/3ds/meta/make-assets.py` is the authoritative source (its SLATE/
 * AMBER constants, read verbatim), and this app used to ignore it entirely —
 * a cyan accent inherited from an early [PadView] edit-mode outline, with a
 * launcher icon that looked like a different product from its own 3DS
 * client. [BG] is the exact SLATE (#1C212B) behind the 3DS gamepad mark;
 * [ACCENT] is the exact AMBER (#F5B042) the mark itself is drawn in. Amber
 * on slate is a warm, fairly light-on-dark pairing — [ACCENT_ON] (button
 * text on an amber fill) uses the SAME slate rather than a made-up dark
 * colour, which measures at ~8.6:1 contrast (WCAG relative-luminance
 * formula, computed by hand against these exact hex values) — comfortably
 * past the 4.5:1 AA floor for body text with margin to spare.
 */
object Theme {

    // ---- palette ---------------------------------------------------------

    /** The tonal ladder. [BG]/[SURFACE]/[SURFACE_RAISED] were already a
     *  consistent per-channel step apart; [SURFACE_SUNKEN] and [SURFACE_HIGH]
     *  extend that SAME step one rung in each direction rather than inventing
     *  a second elevation system. On a dark UI a real `elevation` shadow is
     *  invisible, so a tonal step IS the elevation vocabulary — a recessed
     *  well (the trackpad) sits below the page, a primary control sits above
     *  the resting one. */
    const val SURFACE_SUNKEN = 0xFF161A22.toInt()
    const val BG = 0xFF1C212B.toInt()
    const val SURFACE = 0xFF262D39.toInt()
    const val SURFACE_RAISED = 0xFF3A4558.toInt()
    const val SURFACE_HIGH = 0xFF4B5772.toInt()

    const val OUTLINE = 0x24FFFFFF

    /** Divider inside a container, vs. the edge of an active one. */
    const val OUTLINE_SUBTLE = 0x14FFFFFF
    const val OUTLINE_STRONG = 0x3DFFFFFF

    /** A 1px highlight along the TOP edge of a control. This is what replaces
     *  a full outline stroke on a key cap: a lit top edge reads as "raised"
     *  the way a real keycap catches light, where an all-round hairline just
     *  reads as noise once seventy of them share one screen. */
    const val BEVEL_TOP = 0x16FFFFFF

    const val ACCENT = 0xFFF5B042.toInt()
    const val ACCENT_ON = 0xFF1C212B.toInt()

    /** The brand's own two shading steps, read verbatim from
     *  `clients/3ds/meta/make-assets.py`'s AMBER_HI/AMBER_LO (lines 65-66) —
     *  see [[brand-polish-pass]], which named AMBER_LO as the right source for
     *  a pressed shade rather than inventing one. Not a new hue. */
    const val ACCENT_HI = 0xFFFFCE78.toInt()
    const val ACCENT_LO = 0xFF96621A.toInt()

    /** Amber at low alpha, for a touch-down / held wash over a NEUTRAL
     *  surface — a control that is being touched but is not "selected". */
    const val ACCENT_WASH = 0x1FF5B042
    const val ACCENT_WASH_HIGH = 0x3DF5B042

    const val TEXT_PRIMARY = 0xFFF2F6F9.toInt()
    const val TEXT_SECONDARY = 0x99FFFFFF.toInt()
    const val TEXT_MUTED = 0x5CFFFFFF.toInt()

    /** The three interaction-state overlays. These were inline literals
     *  inside the drawable factories below; named here because the Canvas
     *  views ([KeyGridView], [TrackpadView], MediaRemoteView's IconButton)
     *  need the SAME values and cannot reach into a RippleDrawable to get
     *  them. Changing a press colour should mean editing one line, not
     *  hunting four. */
    const val STATE_PRESSED_ON_ACCENT = 0x401C212B
    const val STATE_PRESSED_ON_SURFACE = 0x33FFFFFF
    const val STATE_HOVER_ON_ROW = 0x2AF5B042

    /** Status colours. WARNING matches the amber the connect screen's status
     *  line has always used; ERROR and OK are new but sampled to sit at the
     *  same lightness so none of the three shouts louder than the others. */
    const val WARNING = 0xFFFFCC66.toInt()
    const val ERROR = 0xFFFF7B72.toInt()
    const val OK = 0xFF7EE0A8.toInt()

    // ---- spacing scale (dp) ----------------------------------------------

    const val SPACE_XS = 4
    const val SPACE_SM = 8
    const val SPACE_MD = 16
    const val SPACE_LG = 24
    const val SPACE_XL = 32

    // ---- type scale (sp) ---------------------------------------------------

    // Stepped down from 26sp: against 18sp card headers (see
    // MainActivity.cardHeader, which used to render at 15sp — the same size
    // as ordinary BODY copy, no bigger) the old gap read as top-heavy (task
    // brief). 22 keeps the title clearly the largest thing on screen without
    // dwarfing the cards beneath it.
    const val TEXT_DISPLAY = 22f
    const val TEXT_TITLE = 18f
    const val TEXT_BODY = 15f
    const val TEXT_LABEL = 13f
    const val TEXT_CAPTION = 11.5f

    /** Canvas-drawn text had NO scale at all until now — the sp scale above is
     *  reached through TextView.textSize and nothing painting on a Canvas
     *  could use it. [KeyGridView] computed a per-key size from a three-way
     *  clamp instead, which is why one grid rendered several visibly
     *  different label sizes. Two sizes, chosen by the key's role. */
    const val TEXT_KEY = 15f
    const val TEXT_KEY_SMALL = 12f

    /** `Typeface.create(Typeface, Int, Boolean)` — the weight-aware overload —
     *  is API 28, and this app is minSdk 26. Family-name lookup works on every
     *  API level and resolves to real Roboto Medium, so it is both the
     *  compatible AND the higher-fidelity option. Replaces the ad-hoc
     *  DEFAULT-plus-BOLD calls that were scattered through the widget
     *  factories below. */
    val TYPE_REGULAR: Typeface = Typeface.create("sans-serif", Typeface.NORMAL)
    val TYPE_MEDIUM: Typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    val TYPE_BOLD: Typeface = Typeface.create("sans-serif", Typeface.BOLD)

    const val TRACKING_WIDE = 0.08f

    // ---- shape -------------------------------------------------------------

    /** A key cap. Smaller than [RADIUS_SM] on purpose: a cap is a small shape
     *  and 8dp on a ~40dp-wide cell starts reading as a lozenge. */
    const val RADIUS_XS = 5
    const val RADIUS_SM = 8
    const val RADIUS_MD = 14
    const val RADIUS_LG = 22

    /** Nothing a finger presses is ever smaller than this (task brief). */
    const val TOUCH_MIN = 48

    /** The floating torch toggle, sized to [TOUCH_MIN] like every other
     *  tappable control even though it sits over a card, not in a form. */
    const val TORCH_TOGGLE_SIZE = TOUCH_MIN

    /** `res/drawable/ic_*.xml` sizes (dp) — one constant per role, not one
     *  shared size, because a card header glyph sits next to 15sp body
     *  text while the placeholder glyph stands alone as the only thing in
     *  an empty preview slot and needs to read from further away. */
    const val ICON_HEADER = 20
    const val ICON_PLACEHOLDER = 28
    const val ICON_TORCH = 22

    // ---- unit conversion ---------------------------------------------------

    fun dp(context: Context, v: Int): Int = dpF(context, v.toFloat()).toInt()

    fun dpF(context: Context, v: Float): Float =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, context.resources.displayMetrics)

    // ---- drawables -----------------------------------------------------

    private fun rounded(
        context: Context,
        color: Int,
        radiusDp: Int,
        strokeColor: Int? = null,
    ): GradientDrawable = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dpF(context, radiusDp.toFloat())
        if (strokeColor != null) setStroke(dp(context, 1), strokeColor)
    }

    fun cardBackground(context: Context): Drawable = rounded(context, SURFACE, RADIUS_MD, OUTLINE)

    fun fieldBackground(context: Context): Drawable =
        rounded(context, SURFACE_RAISED, RADIUS_SM, OUTLINE)

    /** Card 1's preview/placeholder slot — SQUARE, edge to edge in the card
     *  (see [MainActivity.buildScanCard]'s `onMeasure` override), so this
     *  deliberately steps DOWN from [RADIUS_LG] to [RADIUS_SM]: "a
     *  bit rounded" (task brief) reads as a softened square viewfinder, not
     *  the pill-like "generously rounded" look the old wide-short slot had
     *  — a big radius on a square shape reads as a rounded-square icon/app
     *  tile, not a camera viewfinder. No stroke: the live camera image
     *  fills it edge to edge, and an outline drawn over that image would
     *  double as a second, redundant frame around the reticle already
     *  marking the centre. */
    fun previewBackground(context: Context): Drawable = rounded(context, SURFACE_RAISED, RADIUS_SM)

    /** The mode bar's active-chip highlight (MainActivity's KBM mode bar) —
     *  a plain filled pill, no ripple: the chip's selected/not-selected
     *  state IS its own feedback, unlike a button that only flashes on
     *  press. */
    fun chipSelectedBackground(context: Context): Drawable = rounded(context, ACCENT, RADIUS_SM)

    fun primaryButtonBackground(context: Context): Drawable {
        val base = rounded(context, ACCENT, RADIUS_SM)
        val mask = rounded(context, 0xFFFFFFFF.toInt(), RADIUS_SM)
        // Ripple over an amber fill: the same slate ACCENT_ON darkened
        // rather than a made-up colour, so a press looks like a shadow on
        // the brand's own amber, not a foreign tint.
        return RippleDrawable(ColorStateList.valueOf(STATE_PRESSED_ON_ACCENT), base, mask)
    }

    fun secondaryButtonBackground(context: Context): Drawable {
        val base = rounded(context, SURFACE_RAISED, RADIUS_SM, OUTLINE)
        val mask = rounded(context, 0xFFFFFFFF.toInt(), RADIUS_SM)
        return RippleDrawable(ColorStateList.valueOf(STATE_PRESSED_ON_SURFACE), base, mask)
    }

    /** The HUD chip and the scanner's status line: a translucent dark pill
     *  meant to stay legible over a live game or a camera feed without
     *  drawing the eye — "a different visual register... readable at a
     *  glance without drawing the eye away from the game" (task brief). */
    fun hudBackground(context: Context): Drawable = rounded(context, 0xCC121820.toInt(), RADIUS_LG)

    fun rowBackground(context: Context): Drawable {
        val base = rounded(context, SURFACE_RAISED, RADIUS_SM)
        val mask = rounded(context, 0xFFFFFFFF.toInt(), RADIUS_SM)
        return RippleDrawable(ColorStateList.valueOf(STATE_HOVER_ON_ROW), base, mask)
    }

    /** The connect screen's bottom status strip used to be a filled,
     *  rounded-top SURFACE_RAISED slab — heavy for "Not connected." (task
     *  brief: "reads as heavy for what it says"). It is now a plain hairline
     *  (this) sitting directly on the screen's own [BG], the same visual
     *  weight as a caption, not a card. */
    fun hairlineBackground(): Int = OUTLINE

    /** A band panel — the grouping surface MEDIA's rows sit on. No stroke:
     *  the tonal step off [BG] is what separates it, which is quieter and
     *  more legible than an outline once several sit on one screen. */
    fun panelBackground(context: Context): Drawable = rounded(context, SURFACE, RADIUS_MD)

    /** The D-pad's backing — one tonal step ABOVE [panelBackground], not the
     *  same as it. It was originally [SURFACE], which is exactly the colour
     *  of the band panel it sits on, so the cluster was invisible and the
     *  arrows read as floating in a void — the very "plus-shaped hole" this
     *  was added to cure. A raised tone is what makes the 3x3 footprint read
     *  as one continuous pad. */
    fun clusterBackground(context: Context): Drawable =
        rounded(context, SURFACE_RAISED, RADIUS_LG)

    /** A mode chip's resting background: fully transparent, but a real
     *  RippleDrawable — an UNSELECTED chip previously had `background = null`
     *  and therefore no press feedback whatsoever, so tapping one did nothing
     *  visible until the selection itself landed. */
    fun chipBackground(context: Context): Drawable {
        val base = rounded(context, 0x00000000, RADIUS_SM)
        val mask = rounded(context, 0xFFFFFFFF.toInt(), RADIUS_SM)
        return RippleDrawable(ColorStateList.valueOf(STATE_PRESSED_ON_SURFACE), base, mask)
    }

    /** A straight ARGB lerp, deliberately NOT `android.animation.ArgbEvaluator`:
     *  that returns `Any` and boxes an Int on every call, and this runs once
     *  per animating key per frame — seventy allocations a frame on the key
     *  grid alone. */
    fun blend(from: Int, to: Int, t: Float): Int {
        if (t <= 0f) return from
        if (t >= 1f) return to
        val a0 = (from ushr 24) and 0xFF; val a1 = (to ushr 24) and 0xFF
        val r0 = (from ushr 16) and 0xFF; val r1 = (to ushr 16) and 0xFF
        val g0 = (from ushr 8) and 0xFF; val g1 = (to ushr 8) and 0xFF
        val b0 = from and 0xFF; val b1 = to and 0xFF
        val a = (a0 + (a1 - a0) * t).toInt()
        val r = (r0 + (r1 - r0) * t).toInt()
        val g = (g0 + (g1 - g0) * t).toInt()
        val b = (b0 + (b1 - b0) * t).toInt()
        return (a shl 24) or (r shl 16) or (g shl 8) or b
    }

    /** The scan card's floating torch toggle — a small circle over the live
     *  preview, off (translucent dark) vs on (accent-filled) so the state is
     *  readable at a glance without a label competing with the image
     *  underneath it. */
    fun torchToggleBackground(context: Context, on: Boolean): Drawable {
        val color = if (on) ACCENT else 0x99121820.toInt()
        val base = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(color)
        }
        val mask = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(0xFFFFFFFF.toInt())
        }
        return RippleDrawable(ColorStateList.valueOf(0x40FFFFFF), base, mask)
    }
}

/**
 * Motion. Until now this app had none at all — every state change was a binary
 * flip, which is the single loudest "unfinished" signal a touch UI can send.
 *
 * The durations are short on purpose. A press-down must feel CAUSED by the
 * finger, so [DUR_INSTANT] is barely perceptible as motion at all; a release is
 * allowed to be slower, because a decaying highlight reads as a trail rather
 * than a strobe when someone types fast.
 *
 * NOTHING HERE EVER GATES A PACKET. Every `KbmSnapshot` write stays exactly on
 * the MotionEvent path it is on today; an animation only ever records a
 * timestamp AFTER that write, and no animation callback calls into
 * `KbmSnapshot` at all. See [progress] — the whole scheme is "stamp a time,
 * derive a fraction while drawing", which cannot interpose on input.
 */
object Motion {

    const val DUR_INSTANT = 70L
    const val DUR_FAST = 130L
    const val DUR_MED = 200L
    const val DUR_SLOW = 320L

    val EASE_STANDARD: Interpolator = PathInterpolator(0.2f, 0f, 0f, 1f)
    val EASE_OUT: Interpolator = DecelerateInterpolator(1.6f)
    val EASE_IN: Interpolator = AccelerateInterpolator(1.4f)
    val EASE_SETTLE: Interpolator = OvershootInterpolator(1.05f)

    /**
     * `ValueAnimator.areAnimatorsEnabled()` is exactly API 26 — available at
     * this app's minSdk with no guard, and it is the framework's own answer to
     * "the user turned animations off" (developer options, or a battery
     * saver). Returning 0 makes every [progress] call evaluate to 1f on its
     * first frame, so the drawing code degrades to precisely today's binary
     * flip with no separate branch anywhere.
     */
    fun dur(ms: Long): Long = if (ValueAnimator.areAnimatorsEnabled()) ms else 0L

    /** Eased 0..1 progress since [start]. A [start] of 0 means "never
     *  happened" and yields 0. */
    fun progress(start: Long, now: Long, ms: Long, easing: Interpolator = EASE_STANDARD): Float {
        if (start <= 0L) return 0f
        val d = dur(ms)
        if (d <= 0L) return 1f
        return easing.getInterpolation(((now - start).toFloat() / d).coerceIn(0f, 1f))
    }

    /** Whether an animation started at [start] has finished — i.e. whether the
     *  caller still needs another frame. A [start] of 0 counts as settled. */
    fun settled(start: Long, now: Long, ms: Long): Boolean {
        if (start <= 0L) return true
        val d = dur(ms)
        return d <= 0L || now - start >= d
    }
}

// ---- widget factories -----------------------------------------------------
//
// Small, deliberately dumb builder functions rather than a view hierarchy of
// their own — MainActivity still owns layout and lifetimes, these just stop
// every screen from hand-rolling the same TextView/Button setup with its own
// slightly-different constants.

fun displayText(context: Context, s: String): TextView = TextView(context).apply {
    text = s
    setTextColor(Theme.TEXT_PRIMARY)
    textSize = Theme.TEXT_DISPLAY
    typeface = Theme.TYPE_BOLD
}

fun titleText(context: Context, s: String): TextView = TextView(context).apply {
    text = s
    setTextColor(Theme.TEXT_PRIMARY)
    textSize = Theme.TEXT_TITLE
    typeface = Theme.TYPE_BOLD
}

fun sectionLabel(context: Context, s: String): TextView = TextView(context).apply {
    text = s
    setTextColor(Theme.TEXT_SECONDARY)
    textSize = Theme.TEXT_LABEL
    letterSpacing = 0.03f
}

fun bodyText(context: Context, s: String, color: Int = Theme.TEXT_PRIMARY): TextView =
    TextView(context).apply {
        text = s
        setTextColor(color)
        textSize = Theme.TEXT_BODY
    }

fun captionText(context: Context, s: String, color: Int = Theme.TEXT_MUTED): TextView =
    TextView(context).apply {
        text = s
        setTextColor(color)
        textSize = Theme.TEXT_CAPTION
    }

fun primaryButton(context: Context, label: String, onClick: () -> Unit): Button =
    Button(context).apply {
        text = label
        isAllCaps = false
        setTextColor(Theme.ACCENT_ON)
        textSize = Theme.TEXT_BODY
        typeface = Theme.TYPE_BOLD
        background = Theme.primaryButtonBackground(context)
        minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
        val padH = Theme.dp(context, Theme.SPACE_LG)
        setPadding(padH, 0, padH, 0)
        stateListAnimator = null
        setOnClickListener { onClick() }
    }

fun secondaryButton(context: Context, label: String, onClick: () -> Unit): Button =
    Button(context).apply {
        text = label
        isAllCaps = false
        setTextColor(Theme.TEXT_PRIMARY)
        textSize = Theme.TEXT_BODY
        background = Theme.secondaryButtonBackground(context)
        minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
        val padH = Theme.dp(context, Theme.SPACE_LG)
        setPadding(padH, 0, padH, 0)
        stateListAnimator = null
        setOnClickListener { onClick() }
    }

/** A tappable row used for discovered servers and menu items — same shape as
 *  [primaryButton]/[secondaryButton] but left-aligned, for lists. */
fun rowButton(context: Context, label: String, onClick: () -> Unit): TextView =
    TextView(context).apply {
        text = label
        setTextColor(Theme.TEXT_PRIMARY)
        textSize = Theme.TEXT_BODY
        gravity = android.view.Gravity.CENTER_VERTICAL
        background = Theme.rowBackground(context)
        minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
        val padH = Theme.dp(context, Theme.SPACE_MD)
        setPadding(padH, 0, padH, 0)
        isClickable = true
        isFocusable = true
        setOnClickListener { onClick() }
    }

/** A `res/drawable/ic_*.xml` VectorDrawable, single-colour and tinted at
 *  runtime rather than carrying its own palette — the framework's own
 *  replacement for an emoji used as iconography (task brief: "emoji are
 *  not assets... they cannot be tinted to match the theme"). `VectorDrawable`
 *  XML is part of the platform, not a third-party dependency, so this does
 *  not touch the zero-dependency rule. */
fun iconView(
    context: Context,
    resId: Int,
    sizeDp: Int = Theme.ICON_HEADER,
    tint: Int = Theme.TEXT_PRIMARY,
): ImageView = ImageView(context).apply {
    setImageResource(resId)
    imageTintList = ColorStateList.valueOf(tint)
    // A plain ViewGroup.LayoutParams here, not a container-specific one —
    // every parent this is added into (LinearLayout for a card header,
    // FrameLayout for the placeholder/torch toggle) converts it via its own
    // generateLayoutParams() the moment addView(view) sees the child already
    // carries params, which is what lets one factory serve all three
    // call sites without duplicating the size math at each one.
    val px = Theme.dp(context, sizeDp)
    layoutParams = android.view.ViewGroup.LayoutParams(px, px)
}

fun styledEditText(context: Context, hint: String, password: Boolean = false): EditText =
    EditText(context).apply {
        this.hint = hint
        setTextColor(Theme.TEXT_PRIMARY)
        setHintTextColor(Theme.TEXT_MUTED)
        textSize = Theme.TEXT_BODY
        background = Theme.fieldBackground(context)
        minimumHeight = Theme.dp(context, Theme.TOUCH_MIN)
        val padH = Theme.dp(context, Theme.SPACE_MD)
        setPadding(padH, 0, padH, 0)
        if (password) {
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
    }

// ---- insets -----------------------------------------------------------------

/**
 * `WindowInsets`, API 26..36. The app's theme is `Theme.Material...Fullscreen`
 * (hides the status bar entirely), so in practice the only insets this ever
 * reports on real hardware are the navigation bar and, in landscape, a
 * display cutout sitting on the long edge — but both of those are exactly
 * the "stick under a notch" bug class the task brief calls out, so this is
 * not skipped just because the emulator has neither.
 */
object Insets {
    data class Edges(val left: Int, val top: Int, val right: Int, val bottom: Int)

    fun listen(root: View, onChange: (Edges) -> Unit) {
        root.setOnApplyWindowInsetsListener { _, insets ->
            onChange(extract(insets))
            insets
        }
        if (root.isAttachedToWindow) {
            root.requestApplyInsets()
        } else {
            root.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
                override fun onViewAttachedToWindow(v: View) {
                    v.requestApplyInsets()
                }

                override fun onViewDetachedFromWindow(v: View) = Unit
            })
        }
    }

    private fun extract(insets: WindowInsets): Edges {
        return if (android.os.Build.VERSION.SDK_INT >= 30) {
            // systemGestures() alongside systemBars()/displayCutout(): on a
            // gesture-nav phone the gesture exclusion band can be reported
            // separately from (and sometimes taller than) the navigation
            // bar's own systemBars() inset — coordinator review found the
            // bottom keyboard row sitting under the gesture pill even with
            // systemBars() alone. getInsets() with a combined mask already
            // returns the per-edge MAX across every type in it, so adding
            // this type here is enough; no separate query needed.
            // systemGestures() belongs on the BOTTOM EDGE ONLY.
            //
            // It was added to the whole mask to stop the bottom row sitting
            // under the gesture pill, which it does fix. But on a gesture-nav
            // device this type ALSO reports the back-gesture strip down both
            // long edges — measured here at 78px per side on a 1080px screen.
            // Folding that into the horizontal padding silently taxed every
            // screen 156px, 14% of the display's width, and it showed: the
            // MEDIA remote's controls were squeezed into 756px with a dead
            // 120px gutter either side.
            //
            // Bars and cutouts are things a control must not sit UNDER, so
            // they apply on every edge. The back-gesture strip is different:
            // it is an area the system also watches, not one it draws over,
            // and surrendering it costs more than it protects on a surface
            // that is mostly large buttons.
            val bars = WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout()
            val i = insets.getInsets(bars)
            val gestures = insets.getInsets(bars or WindowInsets.Type.systemGestures())
            Edges(i.left, i.top, i.right, gestures.bottom)
        } else {
            @Suppress("DEPRECATION")
            Edges(
                insets.systemWindowInsetLeft,
                insets.systemWindowInsetTop,
                insets.systemWindowInsetRight,
                insets.systemWindowInsetBottom,
            )
        }
    }
}
