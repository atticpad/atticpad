package net.atticpad

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Path
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.animation.AnimationUtils
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import kotlin.math.PI
import kotlin.math.sin

/**
 * MEDIA mode's body (task brief: "volume row · transport row · trackpad ·
 * D-pad cross with OK · large Play/Pause · Back/Home/Menu"), built the same
 * way [MainActivity]'s own connect screen is — plain framework widgets laid
 * out in code — rather than one hand-painted `View` the way [PadView] and
 * [KeyGridView] are, EXCEPT for [IconButton]: transport/volume controls draw
 * their own glyph on a `Canvas` (see [drawMediaGlyph]) rather than carrying
 * text, because a coordinator note during the 2026 proportions pass pointed
 * out that unequal label lengths ("Play/Pause" vs "Rew") were WHY those
 * buttons wrapped and sized unevenly in the first place — fixed-shape glyphs
 * sidestep that at the source instead of fighting it with more layout code.
 * No asset is added for this: no `res/drawable` VectorDrawable, no icon
 * font, nothing outside this file — every glyph is a handful of
 * `Path`/`drawRect`/`drawArc` calls sized off the button's own measured
 * bounds in [IconButton.onDraw], the same "measure at draw time, not a fixed
 * dp" rule [KeyGridView] uses for its own key labels. `contentDescription`
 * is set on every [IconButton] (see [iconButton]) because an icon-only
 * control with no text is invisible to TalkBack, and a REMOTE is exactly the
 * control surface a screen-reader user most needs to reach.
 *
 * Back/Home/Menu stay text: they are navigation concepts, not transport, and
 * a "back" arrow glyph and a "previous track" arrow glyph are visually
 * near-identical while meaning different things — an icon there would
 * introduce the exact ambiguity icons are supposed to remove. The D-pad
 * keeps its existing arrow/OK glyphs unchanged for the same reason.
 *
 * PRESS -> DOWN, RELEASE -> UP everywhere (task brief), not a click, because
 * §6.17's `held` mask is what lets VOLUME_UP ramp for as long as it is
 * physically held — a `setOnClickListener` only ever reports a completed
 * tap and cannot represent that.
 *
 * D-pad + OK and Menu ride the KEYBOARD facility (arrow keys, Enter, HID
 * 0x65) rather than MEDIA — §6.18's media vocabulary has no navigation
 * controls at all, only playback/volume/launcher/browser ones (kbm.h). Back
 * and Home DO have media indices (NAV_BACK, NAV_HOME) and use those instead.
 *
 * VISUAL HIERARCHY (2026 grouping pass). The screen used to be one flat grid
 * of identical SURFACE_RAISED tiles on BG — no grouping, oversized filled
 * D-pad arrows that overflowed their buttons, and a 3x3 D-pad grid whose
 * four empty corner holders read as a plus-shaped HOLE rather than a
 * cluster. Now: three [Theme.panelBackground] bands (volume+transport,
 * trackpad+D-pad, nav) separate by tonal step alone (no strokes — same rule
 * [Theme.panelBackground]'s own doc gives); the D-pad's own container gets
 * [Theme.clusterBackground] so its empty corners fill with panel colour
 * instead of the page background, and its four arrows go through
 * [IconStyle.FLAT] (glyph only, no resting box) so they read as marks ON one
 * cluster rather than four separate tiles sitting on top of it. OK becomes a
 * circle — the one filled shape in the cluster, and every remote's
 * convention for "the button in the middle".
 */
class MediaRemoteView(context: Context) : LinearLayout(context) {

    var kbm: KbmSnapshot? = null
    val trackpad: TrackpadView = TrackpadView(context)

    init {
        orientation = VERTICAL
        val padH = Theme.dp(context, Theme.SPACE_SM)
        setPadding(padH, Theme.dp(context, Theme.SPACE_SM), padH, Theme.dp(context, Theme.SPACE_SM))

        // Band 1: volume (a segmented rocker) + transport, in two tiers —
        // PREV/PLAY_PAUSE/NEXT big and primary, REWIND/FASTFORWARD/STOP
        // smaller and visually subordinate underneath.
        // [TransportPanel] switches between STACKED (volume row above the
        // two transport tiers — portrait, height to spare) and SIDE BY SIDE
        // (volume beside a narrower two-tier transport column — landscape)
        // the same measure-time way [TrackpadDpadSplit] already does for
        // Band 2, and for the same reason: an early landscape screenshot of
        // this pass measured Band 1 at 482px of the body's ~817px content
        // height once STACKED carried three full rows instead of one flat
        // six-cell row — squeezing Band 2's D-pad down to ~125px, WORSE than
        // the ~40px-row regression this file's own history already flags.
        // Landscape has width, not height, to spare, so the fix moves the
        // volume rocker sideways there instead of stacking it on top.
        val volumeRow = row(
            iconButton(
                MediaIcon.VOLUME_DOWN, "Volume down",
                breathing = true,
            ) { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_VOLUME_DOWN, down) },
            // The dead-centre slot of the old row now does something — Mute
            // — instead of just naming the row. It reads as its own
            // tonal step darker than its LEFT/RIGHT neighbours (the
            // segmented-rocker "recessed centre" look).
            iconButton(MediaIcon.MUTE, "Mute") { down ->
                kbm?.mediaEvent(AtticPadNative.MEDIA_MUTE, down)
            },
            iconButton(
                MediaIcon.VOLUME_UP, "Volume up",
                breathing = true,
            ) { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_VOLUME_UP, down) },
        )
        // The SAME row() helper every other row uses — not a bespoke
        // weighted one. transportRow existed only to give Play/Pause extra
        // width, and it also carried its own SPACE_XS gap while row() used
        // SPACE_SM; that 4dp-vs-8dp difference put this row's tiles 11px off
        // the column edges the other two rows shared. One helper, one gap,
        // one grid.
        val primaryRow = row(
            iconButton(MediaIcon.PREV, "Previous track") { down ->
                kbm?.mediaEvent(AtticPadNative.MEDIA_PREV_TRACK, down)
            },
            iconButton(
                MediaIcon.PLAY_PAUSE, "Play or pause",
                style = IconStyle.PRIMARY,
            ) { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_PLAY_PAUSE, down) },
            iconButton(MediaIcon.NEXT, "Next track") { down ->
                kbm?.mediaEvent(AtticPadNative.MEDIA_NEXT_TRACK, down)
            },
        )
        val secondaryRow = row(
            iconButton(
                MediaIcon.REWIND, "Rewind",
                style = IconStyle.SUBORDINATE, breathing = true,
            ) { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_REWIND, down) },
            iconButton(
                MediaIcon.FASTFORWARD, "Fast forward",
                style = IconStyle.SUBORDINATE, breathing = true,
            ) { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_FAST_FORWARD, down) },
            iconButton(MediaIcon.STOP, "Stop", style = IconStyle.SUBORDINATE) { down ->
                kbm?.mediaEvent(AtticPadNative.MEDIA_STOP, down)
            },
        )
        addView(panel {
            addView(
                TransportPanel(
                    context, volumeRow, primaryRow, secondaryRow,
                    stackGap = Theme.dp(context, Theme.SPACE_SM),
                    tierGap = Theme.dp(context, Theme.SPACE_XS),
                    sideGap = Theme.dp(context, Theme.SPACE_MD),
                ),
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT),
            )
        })

        // Band 2: trackpad and D-pad share the remaining space, the D-pad
        // getting slightly MORE (task brief: "the D-pad is a primary
        // control on a remote and should not be an afterthought").
        // [TrackpadDpadSplit] stacks them (trackpad above) when its own box
        // is taller than wide and sits them side by side when it's wider
        // than tall — found necessary after the §1 inset fix legitimately
        // grew the chrome/bottom-nav reserve: on a 1080px-tall landscape
        // body that leaves this pair only ~240px to share, and STACKING
        // that gave the D-pad a squeezed ~40px per row. SPLITTING IT SIDE
        // BY SIDE instead means each pane keeps the body's full ~240px
        // height rather than halving an already scarce one. Portrait,
        // which was never scarce here, keeps the stacked look.
        //
        // Per-branch weights (this pass): 0.75:1 stacked, 3.5:1 side by
        // side. The side-by-side number is high because the D-pad bounds
        // ITSELF to a centred square (see BoundedSquareBox) — in landscape
        // its height is the binding constraint, so any width beyond that
        // square is dead space. Handing it to the trackpad instead is free. — a plain remote's trackpad is a secondary pointer, not an
        // equal partner to the D-pad, so it should not claim as much of a
        // narrow stacked column as the D-pad does; side by side has width
        // to spare, so the trackpad can afford a bit more of it.
        val middle = TrackpadDpadSplit(
            context,
            weightAStacked = 0.75f, weightBStacked = 1f,
            weightASide = 3.5f, weightBSide = 1f,
            gap = Theme.dp(context, Theme.SPACE_SM),
        )
        middle.addView(trackpad)
        middle.addView(buildDpad())
        addView(
            LinearLayout(context).apply {
                orientation = VERTICAL
                val pad = Theme.dp(context, Theme.SPACE_SM)
                setPadding(pad, pad, pad, pad)
                addView(middle, LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply {
                topMargin = Theme.dp(context, Theme.SPACE_SM)
            },
        )

        // Band 3: Back/Home/Menu — quieter than every other control on the
        // screen (task brief: "just make them subordinate"), no fill, press
        // feedback only.
        addView(panel(topMargin = Theme.SPACE_SM) {
            addView(row(
                navButton("Back") { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_NAV_BACK, down) },
                navButton("Home") { down -> kbm?.mediaEvent(AtticPadNative.MEDIA_NAV_HOME, down) },
                navButton("Menu") { down -> kbm?.keyEvent(AtticPadNative.HID_KEY_MENU, down) },
            ))
        })
    }

    // Trackpad's `kbm` is set from here too, once assigned from outside —
    // MainActivity sets `mediaRemote.kbm = svc.kbm` after construction, same
    // as every other mode body, so mirror it onto the embedded trackpad.
    fun bindKbm(k: KbmSnapshot) {
        kbm = k
        trackpad.kbm = k
    }

    // ---- band panels --------------------------------------------------------

    /** A grouping surface (task brief: "the tonal step... does the
     *  separating — no strokes"). Padding is set on THIS container only —
     *  never on [MediaRemoteView] itself outside its constructor, which
     *  [applyKbmBodyInsets] (KbmModeBar.kt) silently overwrites on every
     *  rotation. */
    private fun panel(topMargin: Int = 0, build: LinearLayout.() -> Unit): LinearLayout =
        LinearLayout(context).apply {
            orientation = VERTICAL
            // NO card background. Wrapping every group in a rounded box is
            // the dated idiom here — a remote is a set of controls on a body,
            // not a stack of cards. Grouping is carried by the gaps instead:
            // generous space BETWEEN bands, tight space within them.
            val pad = Theme.dp(context, Theme.SPACE_SM)
            setPadding(pad, pad, pad, pad)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { this.topMargin = Theme.dp(context, topMargin) }
            build()
        }

    private fun vlp(topDp: Int) = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
    ).apply { topMargin = Theme.dp(context, topDp) }

    // WRAP_CONTENT here, not [Theme.TOUCH_MIN] fixed — a row's cells are
    // WIDTH-weighted (0dp + weight), and a WEIGHTED width still resolves
    // its HEIGHT dimension via WRAP_CONTENT/AT_MOST normally. A fixed cell
    // HEIGHT was tried and reverted: it fixed the D-pad's (MATCH_PARENT)
    // cells but a magic-number height on every row cell is exactly the
    // "fixed dp instead of responsive" shape the task warns against, and
    // was not the actual bug — see [IconButton.onMeasure]'s doc for what
    // was. [gap] defaults to the ordinary inter-cell spacing; the volume
    // row passes a much smaller one so its three segments read as one
    // joined rocker rather than three separate tiles.
    private fun row(vararg views: View, topMargin: Int = 0, gap: Int = Theme.SPACE_SM): LinearLayout =
        LinearLayout(context).apply {
            orientation = HORIZONTAL
            this@apply.layoutParams = vlp(topMargin)
            for (v in views) {
                addView(v, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    marginStart = Theme.dp(context, gap)
                    marginEnd = Theme.dp(context, gap)
                })
            }
        }


    /** Wires a control's press/release to [action] — DOWN fires true, UP or
     *  CANCEL fires false — never a click (see the class doc for why). The
     *  one piece shared by every button flavour on this screen. */
    private fun wirePressRelease(v: View, action: (Boolean) -> Unit) {
        v.setOnTouchListener { view, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> { view.isPressed = true; action(true) }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { view.isPressed = false; action(false) }
            }
            true
        }
    }

    /** A discrete press/release control built on a stock [Button] — the
     *  shape OK and (previously) the nav row were built on. */
    private fun pressView(label: String, primary: Boolean, action: (Boolean) -> Unit): View {
        val v = if (primary) primaryButton(context, label) {} else secondaryButton(context, label) {}
        wirePressRelease(v, action)
        return v
    }

    /** An icon-only press/release control — [IconButton] draws its own
     *  glyph; `description` becomes its `contentDescription` since there is
     *  no visible text for TalkBack to read otherwise. [heightDp] lets the
     *  primary transport row ask for a taller "press without looking"
     *  target than the rest of the screen without hard-coding a magic
     *  number into [IconButton] itself. */
    private fun iconButton(
        icon: MediaIcon,
        description: String,
        style: IconStyle = IconStyle.NORMAL,
        label: String? = null,
        breathing: Boolean = false,
        segment: Segment = Segment.SOLO,
        heightDp: Int = BUTTON_H_DP,
        circle: Boolean = false,
        action: (Boolean) -> Unit,
    ): View {
        val v = IconButton(context, icon, style, label, breathing, segment, circle)
        v.contentDescription = description
        v.isClickable = true
        v.isFocusable = true
        v.minimumHeight = Theme.dp(context, heightDp)
        v.setOnTouchListener { view, event ->
            val btn = view as IconButton
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> { btn.held = true; action(true) }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { btn.held = false; action(false) }
            }
            true
        }
        return v
    }

    /** Back/Home/Menu (task brief: "just make them subordinate... no fill...
     *  press feedback only") — [Theme.chipBackground] is transparent at rest
     *  with a real ripple, the same trick the mode chip row uses for an
     *  unselected chip. [TEXT_SECONDARY], not [TEXT_PRIMARY]: these three
     *  are the quietest thing on the screen now, sitting on a panel rather
     *  than standing as their own tile. */
    private fun navButton(label: String, action: (Boolean) -> Unit): View =
        Button(context).apply {
            text = label
            isAllCaps = false
            setTextColor(Theme.TEXT_SECONDARY)
            textSize = Theme.TEXT_BODY
            background = Theme.chipBackground(context)
            minimumHeight = Theme.dp(context, NAV_H_DP)
            stateListAnimator = null
            wirePressRelease(this, action)
        }

    /** OK is the only D-pad cell that stays text (task brief: "OK can stay
     *  text") — everything else in [buildDpad] is a Canvas glyph. Built as a
     *  circle (task brief: "a round centre is what every remote uses"): the
     *  shared [secondaryButton] factory hard-sets 24dp horizontal padding
     *  and a rounded-RECT background, both overridden here rather than in
     *  Theme.kt, which stays the shared, non-circular default every other
     *  button on the app still wants. */
    private fun dpadKey(label: String, usage: Int): View {
        val v = pressView(label, primary = false) { down -> kbm?.keyEvent(usage, down) } as Button
        v.background = ovalBackground(context, Theme.ACCENT)
        // No horizontal padding and a label-sized typeface: in landscape the
        // cluster's cell is small, and at TEXT_BODY with SPACE_XS padding the
        // label ellipsized to a bare "O".
        v.setPadding(0, 0, 0, 0)
        v.setTextColor(Theme.ACCENT_ON)
        v.typeface = Theme.TYPE_BOLD
        v.textSize = Theme.TEXT_LABEL
        v.minWidth = 0
        v.minimumWidth = 0
        v.ellipsize = null
        return v
    }

    /** An arrow cell drawn the same way the transport row's icons are, but
     *  [IconStyle.FLAT] — no resting box of its own, glyph only, so the four
     *  arrows read as marks ON the D-pad's own [Theme.clusterBackground]
     *  rather than as four separate tiles sitting on top of it (task brief:
     *  "sit ON the cluster"). */
    /** An arrow cell. A REAL raised button, not a flat mark: the five cells
     *  ARE the cross, so each one has to be a visible shape in its own right.
     *  A previous pass made these flat and put a filled square panel behind
     *  the whole 3x3 — which filled the corners and destroyed the plus
     *  silhouette entirely, leaving four thin chevrons floating in a grey
     *  box. The empty corners are the point; they are what makes it a
     *  cross. */
    private fun dpadArrow(icon: MediaIcon, description: String, usage: Int): View =
        iconButton(icon, description, heightDp = 0, circle = true) { down ->
            kbm?.keyEvent(usage, down)
        }

    /** A responsive 3x3 D-pad that fills its ENTIRE weighted allotment
     *  (task brief: "the D-pad is a primary control on a remote and should
     *  not be an afterthought") rather than a fixed 56dp cell. An earlier
     *  version of this forced the cluster to a perfect square via the
     *  smaller of its width/height — which, measured on a real landscape
     *  phone, collapsed it to a tiny cross: the band it sits in is
     *  HEIGHT-constrained (shared vertically with the trackpad above), so
     *  "smaller of width/height" always picked the short dimension and
     *  wasted the wide one as blank space either side. Filling the full
     *  rectangle instead means the D-pad is exactly as prominent as the
     *  weight given it below, in both orientations.
     *
     *  The outer container carries [Theme.clusterBackground] (this pass):
     *  previously the four corner holders were empty `FrameLayout`s with no
     *  fill at all, showing the page's own [Theme.BG] through — against a
     *  grid of [Theme.SURFACE_RAISED] arrow tiles that read as a
     *  plus-shaped HOLE, not a cluster. Filling the whole container instead
     *  means the corners disappear into the same surface the arrows sit on,
     *  and — combined with [IconStyle.FLAT] arrows carrying no box of their
     *  own — the whole 3x3 footprint reads as one continuous pad with icons
     *  and a circular OK on it, the same shape a real remote's D-pad is. */
    private fun buildDpad(): View {
        fun dpadRow(vararg cells: View?): LinearLayout =
            LinearLayout(context).apply {
                orientation = HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                for (c in cells) {
                    val holder = FrameLayout(context)
                    if (c != null) {
                        holder.addView(
                            c,
                            FrameLayout.LayoutParams(
                                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
                            ).apply {
                                // SPACE_SM, the same gap row() uses — this is
                                // what puts the cross's three columns on the
                                // exact edges as the button rows above it.
                                val m = Theme.dp(context, Theme.SPACE_SM)
                                setMargins(m, m, m, m)
                            },
                        )
                    }
                    addView(holder, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
                }
            }

        val cluster = LinearLayout(context).apply {
            orientation = VERTICAL
            addView(dpadRow(null, dpadArrow(MediaIcon.ARROW_UP, "Up", AtticPadNative.HID_KEY_UP), null))
            addView(
                dpadRow(
                    dpadArrow(MediaIcon.ARROW_LEFT, "Left", AtticPadNative.HID_KEY_LEFT),
                    dpadKey("OK", AtticPadNative.HID_KEY_ENTER),
                    dpadArrow(MediaIcon.ARROW_RIGHT, "Right", AtticPadNative.HID_KEY_RIGHT),
                ),
            )
            addView(dpadRow(null, dpadArrow(MediaIcon.ARROW_DOWN, "Down", AtticPadNative.HID_KEY_DOWN), null))
        }
        return BoundedSquareBox(context, cluster)
    }

    companion object {
        /** ONE height for every control in the transport band, and one
         *  3-column grid for every row in it.
         *
         *  This band used to run three heights (57/76/54dp) and give
         *  Play/Pause 1.4x width, which broke the column grid outright:
         *  measured off a screenshot, the volume and secondary rows shared
         *  edges exactly (63/395/727) while the primary row sat at 52/752 —
         *  every button in the middle row missing the grid by 11-26px. That
         *  misalignment is what reads as "ugly" long before anyone works out
         *  why.
         *
         *  Play/Pause is still unmistakably primary: it is the only ACCENT
         *  tile on the screen. Emphasis rides ONE variable (colour), not two
         *  (colour AND size), which is what lets every row keep the same
         *  three columns. */
        private const val BUTTON_H_DP = 52

        /** The nav row stays quieter than the controls above it, but still
         *  clear of [Theme.TOUCH_MIN]. */
        private const val NAV_H_DP = 50
    }
}

/** A circular alternative to [Theme.secondaryButtonBackground] for OK only —
 *  every other button on the app stays the shared rounded-rect shape, so
 *  this lives here rather than in Theme.kt (task brief: "do NOT edit
 *  Theme.kt"). Same ripple recipe (mask + base), oval instead of rounded
 *  rect. */
private fun ovalBackground(context: Context, color: Int): Drawable {
    val base = CircleDrawable(color)
    val mask = CircleDrawable(0xFFFFFFFF.toInt())
    return RippleDrawable(ColorStateList.valueOf(Theme.STATE_PRESSED_ON_SURFACE), base, mask)
}

/**
 * A true circle, centred in whatever bounds it is given.
 *
 * NOT `GradientDrawable(OVAL)`, which stretches to fill its bounds and
 * therefore renders an ELLIPSE the moment its cell stops being square. That
 * bit twice: once when the D-pad's landscape cells went wide-and-short, and
 * again when the cross was widened to share the screen's column grid. Deriving
 * the radius from `min(width, height)` makes the shape independent of the
 * cell's aspect, so the grid is free to make cells whatever shape it needs.
 */
private class CircleDrawable(fillColor: Int) : Drawable() {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = fillColor }

    override fun draw(canvas: Canvas) {
        val b = bounds
        if (b.isEmpty) return
        canvas.drawCircle(
            b.exactCenterX(),
            b.exactCenterY(),
            minOf(b.width(), b.height()) / 2f,
            paint,
        )
    }

    override fun setAlpha(alpha: Int) { paint.alpha = alpha }
    override fun setColorFilter(colorFilter: ColorFilter?) { paint.colorFilter = colorFilter }

    @Deprecated("required by Drawable; PixelFormat is the framework's own vocabulary")
    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}

// ---- media glyph vocabulary and drawing ------------------------------------

private enum class MediaIcon {
    PREV, NEXT, REWIND, FASTFORWARD, STOP, PLAY_PAUSE, VOLUME_DOWN, VOLUME_UP, MUTE,
    ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT,
}

/** [IconButton]'s resting fill. NORMAL is the old default
 *  ([Theme.SURFACE_RAISED]); PRIMARY is the one accent-filled control on the
 *  screen; SUBORDINATE ([Theme.SURFACE], one tonal step down from NORMAL) is
 *  the secondary transport row — present but visually quieter than
 *  PREV/PLAY_PAUSE/NEXT above it; FLAT has no resting fill at all — an
 *  amber wash on press is the only feedback — for the D-pad's arrows, which
 *  sit directly on [Theme.clusterBackground] rather than carrying a box of
 *  their own. */
private enum class IconStyle { NORMAL, PRIMARY, SUBORDINATE, FLAT }

/** Which corners of a segmented row's cell get rounded — the volume rocker's
 *  three cells share one continuous pill outline instead of three separate
 *  rounded tiles. SOLO (every other [IconButton] on the screen) rounds all
 *  four corners, same as before. MIDDLE additionally forces a one-tonal-step
 *  darker fill (task brief: "render the mute segment one tonal step
 *  darker"), independent of [IconStyle] — a segmented control's centre cell
 *  reading as slightly recessed is the same "pressed-in" cue a real hardware
 *  rocker gives its centre button. */
private enum class Segment { SOLO, LEFT, MIDDLE, RIGHT }

/** One button, one glyph, drawn directly on its own `Canvas` — the
 *  self-contained alternative the task brief allows in place of a
 *  `res/drawable` VectorDrawable ("whichever fits the code better"); this
 *  app has no `res/` directory for anything but the generated launcher icon,
 *  and every other control ([PadView], [KeyGridView], [TrackpadView]) is
 *  already drawn this way.
 *
 *  MOTION (this pass): [held] used to be a binary overlay flip — the
 *  loudest "unfinished" signal a touch UI can send. It now records a
 *  timestamp on each rising/falling edge ([pressAt]/[releaseAt]) and
 *  [onDraw] derives an eased wash-in/wash-out fraction from
 *  [Motion.progress], the same "stamp a time, derive a fraction while
 *  drawing" scheme [KeyGridView] and [TrackpadView] already use — nothing
 *  here ever gates a packet; [held]'s setter records the timestamp AFTER
 *  the touch listener (see [MediaRemoteView.iconButton]) has already fired
 *  `action(...)`, and no code below ever reads [MediaRemoteView.kbm].
 *  [breathing] additionally pulses that same wash slowly (~600ms) for as
 *  long as the finger stays down — the protocol's `held` mask genuinely
 *  ramps a value for exactly that long (§6.17: VOLUME_UP/DOWN, REWIND,
 *  FASTFORWARD), and nothing in the UI used to say so. */
private class IconButton(
    context: Context,
    private val icon: MediaIcon,
    private val style: IconStyle,
    private val label: String?,
    private val breathing: Boolean = false,
    private val segment: Segment = Segment.SOLO,
    private val circle: Boolean = false,
) : View(context) {

    var held: Boolean = false
        set(value) {
            if (value != field) {
                val now = AnimationUtils.currentAnimationTimeMillis()
                if (value) pressAt = now else releaseAt = now
            }
            field = value
            postInvalidateOnAnimation()
        }

    private var pressAt = 0L
    private var releaseAt = 0L

    // Every Paint/Path/RectF below is a field, allocated once — [onDraw]
    // and [drawMediaGlyph] (which borrows [glyphPath] and [strokeGlyph])
    // never call `Paint(...)`/`Path()`/`RectF()` themselves. An earlier
    // draft of this file's volume/arrow glyphs allocated a fresh `Paint`
    // and `Path` per frame; both are reused fields now.
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }

    private val overlay = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val glyph = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        strokeCap = Paint.Cap.ROUND
    }
    private val strokeGlyph = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val caption = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val glyphPath = Path()
    private val bgPath = Path()
    private val bgRect = RectF()

    /** Per-corner radii for [bgPath] (`Path.addRoundRect`'s 8-float form:
     *  top-left, top-right, bottom-right, bottom-left, each an x,y pair) —
     *  computed once from [segment] rather than every frame. */
    private val cornerRadii: FloatArray = run {
        val r = Theme.dpF(context, Theme.RADIUS_MD.toFloat())
        when (segment) {
            Segment.SOLO -> floatArrayOf(r, r, r, r, r, r, r, r)
            Segment.LEFT -> floatArrayOf(r, r, 0f, 0f, 0f, 0f, r, r)
            Segment.RIGHT -> floatArrayOf(0f, 0f, r, r, r, r, 0f, 0f)
            Segment.MIDDLE -> floatArrayOf(0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f)
        }
    }

    /** A plain `View` (unlike [android.widget.Button]) has no built-in
     *  notion of `WRAP_CONTENT` — left un-overridden, `View.onMeasure`'s
     *  default `getDefaultSize()` just returns whatever `AT_MOST` size the
     *  parent offered, which is exactly why this button filled the entire
     *  screen the first time this was wired up. `minimumHeight` (set by
     *  [MediaRemoteView.iconButton]) is the button's own preferred size;
     *  width always comes from the row's weight pass, which hands every
     *  weighted child an EXACTLY spec regardless. */
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val wanted = maxOf(minimumHeight, suggestedMinimumHeight)
        val h = when (MeasureSpec.getMode(heightMeasureSpec)) {
            MeasureSpec.EXACTLY -> MeasureSpec.getSize(heightMeasureSpec)
            MeasureSpec.AT_MOST -> minOf(wanted, MeasureSpec.getSize(heightMeasureSpec))
            else -> wanted
        }
        setMeasuredDimension(w, h)
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat(); val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val now = AnimationUtils.currentAnimationTimeMillis()
        var needsFrame = false

        // [releaseAt] (like [pressAt]) is 0 until the FIRST release ever
        // happens -- [Motion.progress]'s own doc says a `start` of 0 means
        // "never happened", so a never-touched button must short-circuit
        // to washT=0 rather than fall into the "just released" branch,
        // which `1f - progress(0, ...)` would otherwise evaluate to a
        // permanent, fully-on wash (found on the first screenshot of this
        // pass: every idle button showed a muddy amber-over-slate tint).
        val washT: Float = when {
            held -> {
                if (!Motion.settled(pressAt, now, Motion.DUR_INSTANT)) needsFrame = true
                Motion.progress(pressAt, now, Motion.DUR_INSTANT, Motion.EASE_OUT)
            }
            releaseAt > 0L -> {
                if (!Motion.settled(releaseAt, now, Motion.DUR_FAST)) needsFrame = true
                1f - Motion.progress(releaseAt, now, Motion.DUR_FAST, Motion.EASE_IN)
            }
            else -> 0f
        }

        bgRect.set(0f, 0f, w, h)
        bgPath.reset()
        if (circle) {
            // A true circle from the SHORT side, centred — never an oval
            // stretched to a non-square cell.
            val r = minOf(w, h) / 2f
            bgPath.addCircle(w / 2f, h / 2f, r, Path.Direction.CW)
        } else {
            bgPath.addRoundRect(bgRect, cornerRadii, Path.Direction.CW)
        }

        if (style != IconStyle.FLAT) {
            // FLAT. No gradient, no bevel, no fake shadow — those read as
            // 2010 skeuomorphism, not depth. Separation comes from the tonal
            // ladder and from spacing, which is how a current dark UI does it.
            bg.color = when {
                segment == Segment.MIDDLE -> Theme.SURFACE
                style == IconStyle.PRIMARY -> Theme.ACCENT
                else -> Theme.SURFACE_RAISED
            }
            canvas.drawPath(bgPath, bg)
        }

        if (washT > 0f) {
            var pulse = 0f
            if (held && breathing) {
                needsFrame = true
                val phase = ((now - pressAt) % BREATH_PERIOD_MS).toFloat() / BREATH_PERIOD_MS
                pulse = sin(phase * 2f * PI.toFloat()) * 0.5f + 0.5f
            }
            overlay.color = if (style == IconStyle.PRIMARY) DARK_WASH else Theme.ACCENT
            val baseAlpha = if (style == IconStyle.PRIMARY) 70f else 55f
            overlay.alpha = ((baseAlpha + pulse * 40f) * washT).toInt().coerceIn(0, 255)
            canvas.drawPath(bgPath, overlay)
        }

        // SUBORDINATE carries its lower rank in the GLYPH, not the fill. It
        // used to fill with Theme.SURFACE — the exact colour of the
        // panelBackground behind it — so the secondary transport row rendered
        // as three bare glyphs floating on the panel and did not read as
        // tappable at all.
        val fg = when (style) {
            IconStyle.PRIMARY -> Theme.ACCENT_ON
            IconStyle.SUBORDINATE -> Theme.TEXT_SECONDARY
            else -> Theme.TEXT_PRIMARY
        }
        glyph.color = fg
        strokeGlyph.color = fg
        val hasLabel = label != null
        // Everything below is a FRACTION of this button's own measured
        // bounds, never a fixed dp — the glyph scales with whatever weight
        // and screen size handed this button its box. Arrows get their own,
        // much smaller fraction (task brief: "roughly a third of its
        // button") — the old formula sized them off the same 0.8w/0.86h box
        // every other glyph uses, which is why they overflowed visually.
        val cy = if (hasLabel) h * 0.38f else h * 0.5f
        val isArrow = icon == MediaIcon.ARROW_UP || icon == MediaIcon.ARROW_DOWN ||
            icon == MediaIcon.ARROW_LEFT || icon == MediaIcon.ARROW_RIGHT
        // An icon occupies roughly HALF its button — it is a label for the
        // control, not the control itself. An earlier pass pushed this to
        // 0.88w/0.92h chasing legibility and the glyphs ended up crowding
        // their own tiles.
        val box = if (isArrow) {
            minOf(w, h) * 0.55f
        } else {
            minOf(w * 0.6f, h * (if (hasLabel) 0.5f else 0.62f))
        }

        // Scale the glyph alone (not the whole button) to ~0.92 while held —
        // task brief. Pivoting on the glyph's own centre keeps it in place
        // while it shrinks rather than drifting toward a corner.
        val glyphScale = 1f - 0.08f * washT
        canvas.save()
        canvas.scale(glyphScale, glyphScale, w / 2f, cy)
        drawMediaGlyph(canvas, icon, w / 2f, cy, box, glyph, strokeGlyph, glyphPath)
        canvas.restore()

        if (hasLabel) {
            caption.color = fg
            caption.textSize = Theme.dpF(context, 10.5f)
            canvas.drawText(label!!, w / 2f, h * 0.88f, caption)
        }

        if (needsFrame) postInvalidateOnAnimation()
    }

    companion object {
        private const val BREATH_PERIOD_MS = 600L
        private const val DARK_WASH = 0x33000000
    }
}

/** Draws [icon] centred at ([cx],[cy]) inside a `box`-sized square, entirely
 *  in `Path`/`drawRect`/`drawArc` primitives — no asset, no font. `box` is
 *  the caller's to size (see [IconButton.onDraw]); everything here is a
 *  fraction of it. [fill] and [stroke] and [path] are the caller's own
 *  fields, reused rather than allocated here — see [IconButton]'s class doc. */
private fun drawMediaGlyph(
    canvas: Canvas,
    icon: MediaIcon,
    cx: Float,
    cy: Float,
    box: Float,
    fill: Paint,
    stroke: Paint,
    path: Path,
) {
    fun triangle(cxT: Float, pointRight: Boolean, w: Float, h: Float) {
        path.reset()
        if (pointRight) {
            path.moveTo(cxT - w / 2f, cy - h / 2f)
            path.lineTo(cxT - w / 2f, cy + h / 2f)
            path.lineTo(cxT + w / 2f, cy)
        } else {
            path.moveTo(cxT + w / 2f, cy - h / 2f)
            path.lineTo(cxT + w / 2f, cy + h / 2f)
            path.lineTo(cxT - w / 2f, cy)
        }
        path.close()
        canvas.drawPath(path, fill)
    }

    when (icon) {
        MediaIcon.REWIND, MediaIcon.FASTFORWARD, MediaIcon.PREV, MediaIcon.NEXT -> {
            val triW = box * 0.34f; val triH = box * 0.6f
            val right = icon == MediaIcon.FASTFORWARD || icon == MediaIcon.NEXT
            triangle(cx - triW * 0.55f, right, triW, triH)
            triangle(cx + triW * 0.55f, right, triW, triH)
            if (icon == MediaIcon.NEXT || icon == MediaIcon.PREV) {
                val barW = box * 0.09f
                val barX = if (icon == MediaIcon.NEXT) cx + triW * 1.1f else cx - triW * 1.1f - barW
                canvas.drawRect(barX, cy - triH / 2f, barX + barW, cy + triH / 2f, fill)
            }
        }

        MediaIcon.STOP -> {
            val s = box * 0.5f
            canvas.drawRoundRect(
                cx - s / 2f, cy - s / 2f, cx + s / 2f, cy + s / 2f,
                box * 0.06f, box * 0.06f, fill,
            )
        }

        MediaIcon.PLAY_PAUSE -> {
            val triW = box * 0.34f; val triH = box * 0.66f
            triangle(cx - box * 0.22f, true, triW, triH)
            val barW = box * 0.13f; val barH = box * 0.52f; val gap = box * 0.10f
            val x0 = cx + box * 0.06f
            canvas.drawRect(x0, cy - barH / 2f, x0 + barW, cy + barH / 2f, fill)
            canvas.drawRect(x0 + barW + gap, cy - barH / 2f, x0 + barW * 2f + gap, cy + barH / 2f, fill)
        }

        MediaIcon.VOLUME_DOWN, MediaIcon.VOLUME_UP, MediaIcon.MUTE -> {
            // The composition — speaker plus its sign — is centred on `cx` as
            // a WHOLE. It used to run from cx-0.42 to cx+0.24, i.e. centred on
            // cx-0.09, so every volume glyph sat visibly left of its own
            // button while the transport glyphs beside it were centred.
            val bodyW = box * 0.22f; val bodyH = box * 0.30f
            val left = cx - box * 0.33f
            canvas.drawRect(left, cy - bodyH / 2f, left + bodyW, cy + bodyH / 2f, fill)
            path.reset()
            path.moveTo(left + bodyW, cy - bodyH / 2f)
            path.lineTo(left + bodyW + box * 0.16f, cy - box * 0.28f)
            path.lineTo(left + bodyW + box * 0.16f, cy + box * 0.28f)
            path.lineTo(left + bodyW, cy + bodyH / 2f)
            path.close()
            canvas.drawPath(path, fill)

            // Coordinator review: three speaker glyphs differing only by
            // arc count/a slash weren't distinguishable at the "press
            // without looking" glance a remote needs. An explicit minus/
            // plus is the universal volume-down/up convention. Mute's
            // slash was upgraded to a bold X (this pass, task brief:
            // "improve the MUTE glyph so it is unmistakable") — a single
            // diagonal read ambiguously against the speaker body at this
            // size; two crossed strokes do not.
            val signCx = left + bodyW + box * 0.34f
            val signLen = box * 0.24f
            when (icon) {
                MediaIcon.VOLUME_UP -> {
                    stroke.strokeWidth = box * 0.09f
                    canvas.drawLine(signCx - signLen / 2f, cy, signCx + signLen / 2f, cy, stroke)
                    canvas.drawLine(signCx, cy - signLen / 2f, signCx, cy + signLen / 2f, stroke)
                }
                MediaIcon.VOLUME_DOWN -> {
                    stroke.strokeWidth = box * 0.09f
                    canvas.drawLine(signCx - signLen / 2f, cy, signCx + signLen / 2f, cy, stroke)
                }
                MediaIcon.MUTE -> {
                    // Clear of the cone, at the SAME x the +/- signs use, so
                    // all three volume glyphs share one silhouette and differ
                    // only in the mark to the right of the speaker. The X used
                    // to be centred at box*0.24 with a box*0.26 arm, i.e.
                    // drawn straight across the cone, which is what made it
                    // read as a smudge rather than a symbol.
                    stroke.strokeWidth = box * 0.09f
                    val xSize = box * 0.11f
                    canvas.drawLine(signCx - xSize, cy - xSize, signCx + xSize, cy + xSize, stroke)
                    canvas.drawLine(signCx - xSize, cy + xSize, signCx + xSize, cy - xSize, stroke)
                }
                else -> Unit
            }
        }

        MediaIcon.ARROW_UP, MediaIcon.ARROW_DOWN, MediaIcon.ARROW_LEFT, MediaIcon.ARROW_RIGHT -> {
            // A stroked chevron (this pass), not a solid triangle — the old
            // filled shape at `reach = box*0.42`/`spread = box*0.34` looked
            // like a giant "play" glyph and overflowed its button visually
            // (task brief). Same three-point geometry as the old triangle,
            // just left open (no `close()`) and stroked with a round cap/
            // join instead of filled, at a much smaller fraction of `box`.
            val reach = box * 0.42f
            val spread = box * 0.36f
            path.reset()
            when (icon) {
                MediaIcon.ARROW_UP -> {
                    path.moveTo(cx - spread, cy + reach * 0.55f)
                    path.lineTo(cx, cy - reach)
                    path.lineTo(cx + spread, cy + reach * 0.55f)
                }
                MediaIcon.ARROW_DOWN -> {
                    path.moveTo(cx - spread, cy - reach * 0.55f)
                    path.lineTo(cx, cy + reach)
                    path.lineTo(cx + spread, cy - reach * 0.55f)
                }
                MediaIcon.ARROW_LEFT -> {
                    path.moveTo(cx + reach * 0.55f, cy - spread)
                    path.lineTo(cx - reach, cy)
                    path.lineTo(cx + reach * 0.55f, cy + spread)
                }
                MediaIcon.ARROW_RIGHT -> {
                    path.moveTo(cx - reach * 0.55f, cy - spread)
                    path.lineTo(cx + reach, cy)
                    path.lineTo(cx - reach * 0.55f, cy + spread)
                }
                else -> Unit
            }
            path.close()
            canvas.drawPath(path, fill)
        }
    }
}

/** Exactly two children — `A` (trackpad) then `B` (D-pad) — split along
 *  whichever axis is currently SCARCE: stacked (A above B) when this
 *  container's own box is taller than wide, side by side (A left of B) when
 *  it is wider than tall. The two branches carry their OWN weight pair
 *  ([weightAStacked]/[weightBStacked] vs [weightASide]/[weightBSide]) —
 *  this pass's change (task brief: "per-branch weights... 0.42:1... 0.6:1")
 *  — because a single shared ratio does not fit both: portrait can afford
 *  to give the D-pad most of a tall column, landscape's width-not-height
 *  layout can afford the trackpad a bit more since neither pane is fighting
 *  the other for a scarce dimension there. Neither child ever halves the
 *  axis the OTHER one needs plenty of — see [MediaRemoteView]'s "Band 2"
 *  comment for the landscape budget this exists to fix. A plain
 *  `LinearLayout` can't do this: its `orientation` is one fixed value, and
 *  swapping it at measure time would also require swapping which
 *  `LayoutParams` dimension carries the weight — simpler to just measure/
 *  lay out two known children directly. */
/** Band 1's own layout, adaptive the same measure-time way
 *  [TrackpadDpadSplit] is: STACKED (volume row above a two-tier transport
 *  stack) when this panel's own box is taller than it is wide, SIDE BY SIDE
 *  (a narrower volume column beside a two-tier transport column) when it
 *  is wider than tall. The comparison uses the box's OWN measure spec
 *  (`width` vs the `AT_MOST` height bound handed down) rather than
 *  `context.resources.configuration` -- the same "derive it from this
 *  view's own bounds, don't ask the outside world" rule [TrackpadDpadSplit]
 *  follows, so a bare window resize on rotation is enough with no rebuild
 *  hook needed anywhere else. See [MediaRemoteView]'s "Band 1" comment for
 *  the landscape regression this exists to fix: STACKING all three rows
 *  unconditionally once measured at 482px of an ~817px content budget,
 *  leaving the D-pad band ~125px -- worse than the ~40px-per-row regression
 *  this file's own history already flags as broken. */
private class TransportPanel(
    context: Context,
    private val volume: View,
    private val primary: View,
    private val secondary: View,
    private val stackGap: Int,
    private val tierGap: Int,
    private val sideGap: Int,
) : ViewGroup(context) {

    init {
        addView(volume)
        addView(primary)
        addView(secondary)
    }

    private fun exactly(v: Int) = MeasureSpec.makeMeasureSpec(v.coerceAtLeast(0), MeasureSpec.EXACTLY)
    private fun atMost(v: Int) = MeasureSpec.makeMeasureSpec(v.coerceAtLeast(0), MeasureSpec.AT_MOST)

    // Set in [onMeasure], read back in [onLayout] -- onLayout must NOT
    // re-derive the branch from ITS OWN final `w`/`h` (`r-l` vs `b-t`):
    // stacked content is inherently short *and* wide (three compact rows),
    // so a naive `w > h` there reads as "wide" and picks SIDE BY SIDE even
    // on a portrait phone, laying the transport column out past the panel's
    // own right edge where it is silently clipped away entirely (found on
    // this pass's first portrait re-check after the landscape fix: the
    // volume row rendered alone, vertically centred in a mysteriously tall
    // gap, with both transport rows simply gone). [onMeasure] compares
    // against the incoming AT_MOST height BOUND, which is the only place
    // that number is available -- so the decision has to be made once,
    // there, and carried over.
    private var sideBySide = false

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val hBound = MeasureSpec.getSize(heightMeasureSpec)
        sideBySide = w > hBound
        val totalH: Int
        if (sideBySide) {
            // SIDE BY SIDE (landscape): all THREE groups share one row.
            // Stacking the two transport tiers here cost ~52dp of height in
            // the orientation that has none to spare, and that height is
            // exactly what the D-pad band below needs -- with the tiers
            // stacked, the cluster squared to ~70dp and OK's label clipped
            // to "O". Landscape has width in abundance and height in
            // shortage, so spend width.
            val volW = (w * 0.26f).toInt()
            val priW = (w * 0.42f).toInt()
            val secW = w - volW - priW - sideGap * 2
            volume.measure(exactly(volW), atMost(hBound))
            primary.measure(exactly(priW), atMost(hBound))
            secondary.measure(exactly(secW), atMost(hBound))
            totalH = maxOf(
                volume.measuredHeight,
                maxOf(primary.measuredHeight, secondary.measuredHeight),
            )
        } else {
            // STACKED (portrait): volume row, then the two transport tiers.
            volume.measure(exactly(w), atMost(hBound))
            primary.measure(exactly(w), atMost(hBound))
            secondary.measure(exactly(w), atMost(hBound))
            totalH = volume.measuredHeight + stackGap + primary.measuredHeight + tierGap + secondary.measuredHeight
        }
        setMeasuredDimension(w, totalH.coerceAtMost(hBound))
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        if (sideBySide) {
            val h = b - t
            fun place(v: View, x: Int) {
                val top = (h - v.measuredHeight) / 2
                v.layout(x, top, x + v.measuredWidth, top + v.measuredHeight)
            }
            place(volume, 0)
            val priX = volume.measuredWidth + sideGap
            place(primary, priX)
            place(secondary, priX + primary.measuredWidth + sideGap)
        } else {
            volume.layout(0, 0, volume.measuredWidth, volume.measuredHeight)
            val priY = volume.measuredHeight + stackGap
            primary.layout(0, priY, primary.measuredWidth, priY + primary.measuredHeight)
            val secY = priY + primary.measuredHeight + tierGap
            secondary.layout(0, secY, secondary.measuredWidth, secY + secondary.measuredHeight)
        }
    }
}

/**
 * Lays its ONE child across the full width it is given, bounding only the
 * HEIGHT, and centres it vertically.
 *
 * This exists so the D-pad shares the SAME three columns as the nine buttons
 * above it. An earlier version squared the whole cluster instead, which put
 * the cross on its own narrower columns — and combined with a bespoke
 * weighted row for Play/Pause and three different button heights, the band
 * had no consistent grid at all. Measured off a screenshot, the volume and
 * secondary rows shared edges exactly while the primary row sat 11px off, and
 * the cross sat somewhere else again. Misalignment like that reads as "ugly"
 * long before anyone can say why.
 *
 * The height cap does the job the squaring used to: a cell is never taller
 * than it is wide, and [MAX_SIDE_DP] stops the cross sprawling down a tall
 * panel. Because width is always taken in full there is no starved-axis case
 * to special-case any more — the old `min(w, h)` collapse this class was
 * written to avoid cannot arise when only one axis is ever bounded.
 */
private class BoundedSquareBox(context: Context, child: View) : ViewGroup(context) {

    companion object {
        /** Past this a D-pad stops reading as a thumb cluster and starts
         *  reading as a wall. */
        private const val MAX_SIDE_DP = 360f

    }

    init { addView(child) }

    private var childSide = 0
    private var childW = 0
    private var childH = 0

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        // SQUARE, centred — a D-pad's four arms must be the same length.
        // Spanning the full grid width instead lined the cross up with the
        // button columns above, but made the cells 290x180: the horizontal
        // arms came out ~330px against ~258px vertical, so the cross read as
        // squashed. Central symmetry beats column alignment here, because the
        // cross is one discrete control rather than a row of separate ones.
        val maxSide = Theme.dpF(context, MAX_SIDE_DP)
        childSide = minOf(w.toFloat(), h.toFloat(), maxSide).toInt()
        childW = childSide
        childH = childSide
        getChildAt(0)?.measure(
            MeasureSpec.makeMeasureSpec(childW.coerceAtLeast(0), MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(childH.coerceAtLeast(0), MeasureSpec.EXACTLY),
        )
        setMeasuredDimension(w, h)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val c = getChildAt(0) ?: return
        val w = r - l; val h = b - t
        val left = (w - childW) / 2
        val top = (h - childH) / 2
        c.layout(left, top, left + childW, top + childH)
    }
}

private class TrackpadDpadSplit(
    context: Context,
    private val weightAStacked: Float,
    private val weightBStacked: Float,
    private val weightASide: Float,
    private val weightBSide: Float,
    private val gap: Int,
) : ViewGroup(context) {

    private fun exactly(size: Int) = MeasureSpec.makeMeasureSpec(size.coerceAtLeast(0), MeasureSpec.EXACTLY)

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        if (childCount >= 2) {
            val a = getChildAt(0); val b = getChildAt(1)
            if (w > h) {
                val avail = w - gap
                val total = weightASide + weightBSide
                val wa = (avail * weightASide / total).toInt()
                a.measure(exactly(wa), exactly(h))
                b.measure(exactly(avail - wa), exactly(h))
            } else {
                val avail = h - gap
                val total = weightAStacked + weightBStacked
                val ha = (avail * weightAStacked / total).toInt()
                a.measure(exactly(w), exactly(ha))
                b.measure(exactly(w), exactly(avail - ha))
            }
        }
        setMeasuredDimension(w, h)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        if (childCount < 2) return
        val w = r - l; val h = b - t
        val a = getChildAt(0); val bb = getChildAt(1)
        if (w > h) {
            a.layout(0, 0, a.measuredWidth, h)
            bb.layout(w - bb.measuredWidth, 0, w, h)
        } else {
            a.layout(0, 0, w, a.measuredHeight)
            bb.layout(0, h - bb.measuredHeight, w, h)
        }
    }
}
