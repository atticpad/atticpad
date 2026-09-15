package net.atticpad

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import android.view.animation.AnimationUtils
import kotlin.math.hypot

/**
 * A full-bleed mouse surface: one finger drags the pointer, a narrow gutter
 * at the edge(s) scrolls, a quick tap clicks. Used both as MOUSE mode's main
 * body (one right gutter) and as KEYBOARD/MEDIA mode's compact top strip
 * ([dualGutter] — a gutter on both edges, task brief).
 *
 * SENSITIVITY IS APPLIED HERE, DELIBERATELY, unlike every stick/touch value
 * elsewhere in this app. docs/PROTOCOL.md §5.3 forbids a client-side
 * deadzone because a deadzone DESTROYS information the server's profile
 * could still use — the raw value is needed downstream. Mouse motion has no
 * such downstream: §6.16's `dx_accum`/`dy_accum` are free-running pixel-ish
 * counters with no server-side profile that owns curve or sensitivity at
 * all (no KBM target was ever added to the profile system), so scaling a
 * drag before it reaches [KbmSnapshot] loses nothing anyone could have used
 * — the alternative is not "more correct", it is "no one adjusts it".
 *
 * Writes to [kbm] only — never touches [InputSnapshot] or a packet.
 *
 * Drawing is a recessed "well" (no caption — a control surface is not
 * labelled, project UX rule) plus purely cosmetic feedback: a contact halo
 * per finger, an expanding click ring, and a scroll-rail glow on a detent.
 * All of it is time-stamped state read back in [onDraw] and NONE of it may
 * ever gate or delay a [kbm] call — every animation field here is written
 * strictly AFTER the packet-producing call it decorates, never before or
 * instead of it.
 */
class TrackpadView(context: Context) : View(context) {

    var kbm: KbmSnapshot? = null

    /** KEYBOARD/MEDIA mode's compact strip scrolls from either edge; MOUSE
     *  mode's full-bleed surface only reserves the right edge (task brief:
     *  "a right-edge scroll gutter") so the rest of a much bigger area stays
     *  pointer motion. */
    var dualGutter: Boolean = false
        set(value) { field = value; invalidate() }

    // MOUSE mode's single gutter keeps its original, already-proven 14% —
    // KEYBOARD/MEDIA's two rails are narrower each (10%) since together
    // they only need to be as reachable as one, not twice as wide.
    private val gutterFracSingle = 0.14f
    private val gutterFracDual = 0.10f
    private val gutterFrac: Float get() = if (dualGutter) gutterFracDual else gutterFracSingle
    private val wheelThresholdPx: Float get() = Theme.dpF(context, 24f)
    private val tapMaxMovePx: Float get() = Theme.dpF(context, 8f)
    private val tapMaxMs = 180L
    private val moveSensitivity = 1.6f

    private class Ptr(var x: Float, var y: Float, val startX: Float, val startY: Float, val gutter: Boolean)

    private val ptrs = LinkedHashMap<Int, Ptr>()
    private var gestureDownTime = 0L
    private var gestureMaxPointers = 0
    private var gestureMoved = false
    private var wheelAccumPx = 0f

    private fun isGutter(x: Float): Boolean =
        x <= width * gutterFrac || x >= width * (1f - gutterFrac)

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val id = event.getPointerId(i)
                val x = event.getX(i); val y = event.getY(i)
                val gutter = dualGutter && isGutter(x) ||
                    !dualGutter && x >= width * (1f - gutterFrac)
                ptrs[id] = Ptr(x, y, x, y, gutter)
                if (ptrs.size == 1) {
                    // MotionEvent's own clock, which is SystemClock.uptimeMillis
                    // and therefore monotonic — NOT the wall clock this used to
                    // read. Wall clock can STEP: an NTP correction or a manual
                    // time change mid-gesture made `elapsed` below negative or
                    // enormous, silently swallowing a click or fabricating one.
                    gestureDownTime = event.eventTime
                    gestureMoved = false
                    wheelAccumPx = 0f
                }
                gestureMaxPointers = maxOf(gestureMaxPointers, ptrs.size)
            }

            MotionEvent.ACTION_MOVE -> onMove(event)

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val i = event.actionIndex
                val id = event.getPointerId(i)
                val x = event.getX(i); val y = event.getY(i)
                ptrs.remove(id)
                addReleaseFade(x, y)
                if (ptrs.isEmpty()) {
                    val elapsed = event.eventTime - gestureDownTime
                    if (!gestureMoved && elapsed < tapMaxMs) {
                        val isRight = gestureMaxPointers >= 2
                        val button = if (isRight) AtticPadNative.MOUSEBTN_RIGHT else AtticPadNative.MOUSEBTN_LEFT
                        kbm?.let {
                            it.mouseButtonEvent(button, true)
                            it.mouseButtonEvent(button, false)
                        }
                        // Recorded AFTER both packet-producing calls above —
                        // this is decoration of a click that already happened
                        // on the wire, never a gate in front of it.
                        clickX = x
                        clickY = y
                        clickRight = isRight
                        clickTime = AnimationUtils.currentAnimationTimeMillis()
                    }
                    gestureMaxPointers = 0
                }
            }

            MotionEvent.ACTION_CANCEL -> {
                for (p in ptrs.values) addReleaseFade(p.x, p.y)
                ptrs.clear()
                gestureMaxPointers = 0
            }
        }
        invalidate()
        return true
    }

    private fun onMove(event: MotionEvent) {
        val k = kbm ?: return
        if (ptrs.size == 1) {
            val (id, p) = ptrs.entries.first()
            val i = event.findPointerIndex(id)
            if (i < 0) return
            val x = event.getX(i); val y = event.getY(i)
            val dx = x - p.x; val dy = y - p.y
            if (p.gutter) {
                wheelAccumPx += dy
                emitWheel(k)
            } else if (dx != 0f || dy != 0f) {
                k.mouseMove((dx * moveSensitivity).toInt(), (dy * moveSensitivity).toInt())
            }
            if (hypot((x - p.startX).toDouble(), (y - p.startY).toDouble()) > tapMaxMovePx) {
                gestureMoved = true
            }
            p.x = x; p.y = y
        } else if (ptrs.size >= 2) {
            // Two-finger drag anywhere on the surface scrolls (task brief),
            // not just inside a gutter — the average of every tracked
            // pointer's own vertical motion this move.
            var sumDy = 0f; var n = 0
            for ((id, p) in ptrs) {
                val i = event.findPointerIndex(id)
                if (i < 0) continue
                val x = event.getX(i); val y = event.getY(i)
                sumDy += y - p.y
                n++
                if (hypot((x - p.startX).toDouble(), (y - p.startY).toDouble()) > tapMaxMovePx) {
                    gestureMoved = true
                }
                p.x = x; p.y = y
            }
            if (n > 0) {
                wheelAccumPx += sumDy / n
                emitWheel(k)
            }
        }
    }

    /** §6.16: `+` is away from the user. A finger dragging UP the screen
     *  (dy negative) is the same physical gesture as rolling a wheel away,
     *  so that is the sign that produces a positive detent. */
    private fun emitWheel(k: KbmSnapshot) {
        val t = wheelThresholdPx
        var emitted = false
        while (wheelAccumPx <= -t) { k.mouseWheel(1, false); wheelAccumPx += t; emitted = true }
        while (wheelAccumPx >= t) { k.mouseWheel(-1, false); wheelAccumPx -= t; emitted = true }
        // Stamped AFTER every mouseWheel() call above — the rail glow is
        // decoration of a detent that already went out, not a gate on it.
        if (emitted) detentTime = AnimationUtils.currentAnimationTimeMillis()
    }

    // ---- cosmetic feedback state -------------------------------------------
    //
    // Every field below is read ONLY from onDraw/its helpers. Nothing here is
    // ever read from onTouchEvent/onMove/emitWheel, and nothing in those
    // three writes to `kbm` conditionally on any of it — nothing here can gate
    // a packet even by accident.

    private class ReleaseFade { var x = 0f; var y = 0f; var start = 0L; var active = false }
    // Fixed-size pool, not an ArrayList: a lifted finger is on the input
    // path (onTouchEvent), and a pool means recording a fade never allocates.
    private val releaseFades = Array(5) { ReleaseFade() }

    private fun addReleaseFade(x: Float, y: Float) {
        val now = AnimationUtils.currentAnimationTimeMillis()
        var slot: ReleaseFade? = null
        for (f in releaseFades) if (!f.active) { slot = f; break }
        if (slot == null) {
            slot = releaseFades[0]
            for (f in releaseFades) if (f.start < slot!!.start) slot = f
        }
        slot.x = x; slot.y = y; slot.start = now; slot.active = true
    }

    private var clickTime = 0L
    private var clickX = 0f
    private var clickY = 0f
    private var clickRight = false

    private var detentTime = 0L

    // ---- drawing ------------------------------------------------------------

    private val wellFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = Theme.SURFACE_SUNKEN }

    private val wellHairline = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; color = Theme.OUTLINE_SUBTLE }
    private val railFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = Theme.SURFACE }
    private val railCapsule = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val railChevron = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val haloPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = Theme.ACCENT_WASH }
    private val corePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = Theme.ACCENT }
    private val clickRingPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }

    // Reused scratch shapes — allocated once, mutated per onDraw call.
    private val wellRect = RectF()
    private val railRect = RectF()
    private val capsuleRect = RectF()
    private val chevronPath = Path()

    override fun onDraw(canvas: Canvas) {
        val now = AnimationUtils.currentAnimationTimeMillis()
        var needsFrame = false

        val inset = Theme.dpF(context, Theme.SPACE_XS.toFloat())
        val radius = Theme.dpF(context, Theme.RADIUS_MD.toFloat())
        val gap = inset * 0.5f

        val leftGutterEnd = if (dualGutter) width * gutterFrac else 0f
        val rightGutterStart = width * (1f - gutterFrac)

        if (dualGutter) {
            needsFrame = drawGutter(canvas, inset, leftGutterEnd - gap, radius, now) || needsFrame
        }
        needsFrame = drawGutter(canvas, rightGutterStart + gap, width - inset, radius, now) || needsFrame

        // The well itself — recessed one tonal step below the page, replacing
        // the old flat full-bleed SURFACE rect. A hairline along its inner
        // TOP edge only (not a full outline) is what reads as a lip you drag
        // a finger down into, rather than a second bordered card.
        wellRect.set(
            (if (dualGutter) leftGutterEnd + gap else inset),
            inset,
            rightGutterStart - gap,
            height - inset,
        )
        canvas.drawRoundRect(wellRect, radius, radius, wellFill)
        wellHairline.strokeWidth = Theme.dpF(context, 1f)
        canvas.drawLine(
            wellRect.left + radius * 0.6f, wellRect.top + wellHairline.strokeWidth,
            wellRect.right - radius * 0.6f, wellRect.top + wellHairline.strokeWidth,
            wellHairline,
        )

        // Contact halo — one per live pointer, plus fading ghosts for
        // recently-released ones. Two flat circles, deliberately not a
        // RadialGradient/Shader: a shader keyed to view size has to be
        // reallocated on every resize, a circle pair does not.
        val haloR = Theme.dpF(context, 22f)
        val coreR = Theme.dpF(context, 5f)
        for (p in ptrs.values) {
            canvas.drawCircle(p.x, p.y, haloR, haloPaint)
            canvas.drawCircle(p.x, p.y, coreR, corePaint)
        }
        for (f in releaseFades) {
            if (!f.active) continue
            val t = Motion.progress(f.start, now, Motion.DUR_FAST, Motion.EASE_OUT)
            if (t >= 1f) { f.active = false; continue }
            needsFrame = true
            val fade = 1f - t
            haloPaint.alpha = (0x1F * fade).toInt()
            corePaint.alpha = (0xFF * fade).toInt()
            canvas.drawCircle(f.x, f.y, haloR, haloPaint)
            canvas.drawCircle(f.x, f.y, coreR, corePaint)
            haloPaint.alpha = 0xFF
            corePaint.alpha = 0xFF
        }

        // Click confirmation — an expanding ring, thinning and fading as it
        // grows. A second, smaller concentric ring marks a right click so
        // the distinction survives on a colour-blind display, not just as a
        // colour choice.
        val clickT = Motion.progress(clickTime, now, Motion.DUR_MED, Motion.EASE_OUT)
        if (clickTime > 0L && clickT < 1f) {
            needsFrame = true
            val alpha = ((1f - clickT) * 255).toInt()
            clickRingPaint.color = Theme.ACCENT
            clickRingPaint.alpha = alpha
            clickRingPaint.strokeWidth = Theme.dpF(context, 2.5f) + (Theme.dpF(context, 0.5f) - Theme.dpF(context, 2.5f)) * clickT
            val r1 = Theme.dpF(context, 10f) + (Theme.dpF(context, 46f) - Theme.dpF(context, 10f)) * clickT
            canvas.drawCircle(clickX, clickY, r1, clickRingPaint)
            if (clickRight) {
                val r2 = Theme.dpF(context, 6f) + (Theme.dpF(context, 30f) - Theme.dpF(context, 6f)) * clickT
                canvas.drawCircle(clickX, clickY, r2, clickRingPaint)
            }
        }

        if (needsFrame) postInvalidateOnAnimation()
    }

    /** Draws one scroll rail between [left] and [right] and returns whether
     *  the caller still needs another frame (the detent glow is still
     *  decaying). No caption — a capsule mark plus an up/down chevron pair
     *  is meant to read as a scroll affordance without a single word. */
    private fun drawGutter(canvas: Canvas, left: Float, right: Float, radius: Float, now: Long): Boolean {
        val edgeInset = Theme.dpF(context, Theme.SPACE_XS.toFloat())
        railRect.set(left, edgeInset, right, height - edgeInset)
        canvas.drawRoundRect(railRect, radius, radius, railFill)

        var needsFrame = false
        var glowT = 0f
        if (detentTime > 0L) {
            val decay = Motion.progress(detentTime, now, Motion.DUR_FAST, Motion.EASE_OUT)
            if (decay < 1f) { needsFrame = true; glowT = 1f - decay }
        }
        val markColor = Theme.blend(Theme.OUTLINE_STRONG, Theme.ACCENT, glowT)

        val cx = (left + right) / 2f
        val capsuleHalfW = Theme.dpF(context, 1.25f)
        val capsuleTop = railRect.top + railRect.height() * 0.20f
        val capsuleBottom = railRect.top + railRect.height() * 0.80f
        capsuleRect.set(cx - capsuleHalfW, capsuleTop, cx + capsuleHalfW, capsuleBottom)
        railCapsule.color = markColor
        canvas.drawRoundRect(capsuleRect, capsuleHalfW, capsuleHalfW, railCapsule)

        railChevron.color = markColor
        railChevron.strokeWidth = Theme.dpF(context, 1.6f)
        val chevronHalfW = Theme.dpF(context, 4f)
        val chevronH = Theme.dpF(context, 3f)
        val gapAboveCapsule = Theme.dpF(context, 6f)

        // Up chevron, apex pointing up, sitting just above the capsule.
        val upY = capsuleTop - gapAboveCapsule
        chevronPath.reset()
        chevronPath.moveTo(cx - chevronHalfW, upY + chevronH)
        chevronPath.lineTo(cx, upY)
        chevronPath.lineTo(cx + chevronHalfW, upY + chevronH)
        canvas.drawPath(chevronPath, railChevron)

        // Down chevron, apex pointing down, just below the capsule.
        val downY = capsuleBottom + gapAboveCapsule
        chevronPath.reset()
        chevronPath.moveTo(cx - chevronHalfW, downY - chevronH)
        chevronPath.lineTo(cx, downY)
        chevronPath.lineTo(cx + chevronHalfW, downY - chevronH)
        canvas.drawPath(chevronPath, railChevron)

        return needsFrame
    }
}
