package net.atticpad

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * The touch overlay: virtual sticks, D-pad, face and shoulder buttons, and a
 * touchpad region that forwards raw §5.2 contacts.
 *
 * Also the layout editor (docs/DESIGN.md §11, M3): [editing] turns every control into
 * something you drag, and pinch — or the +/- of a two-finger span — resizes it.
 *
 * VALUES ON THE WIRE ARE RAW. §5.3: a client MUST NOT apply a deadzone. The
 * knob is drawn clamped to the ring, and the value sent is the same clamped
 * unit vector — but nothing near the centre is zeroed, because the server's
 * profile owns that and cannot recover what this class throws away.
 */
class PadView(context: Context) : View(context) {

    var snapshot: InputSnapshot? = null

    /** Called when the hidden L+R+START combo is held (see [comboHoldMs]). */
    var onSelfTestCombo: (() -> Unit)? = null

    var editing: Boolean = false
        set(value) {
            field = value
            releaseAll()
            invalidate()
        }

    var layout: PadLayout = PadLayout.default(PadLayout.Orientation.LANDSCAPE)
        set(value) {
            field = value
            invalidate()
        }

    /**
     * The system-bar / display-cutout insets (px), set by [MainActivity] from
     * `WindowInsets`. Controls are positioned relative to the SAFE rect these
     * carve out of the view, not the raw view bounds — a stick under a notch
     * is a bug only visible on hardware with one (task brief), and the
     * overlay is deliberately edge-to-edge otherwise, so nothing else would
     * catch it.
     */
    var insetLeft = 0; private set
    var insetTop = 0; private set
    var insetRight = 0; private set
    var insetBottom = 0; private set

    fun setInsets(left: Int, top: Int, right: Int, bottom: Int) {
        insetLeft = left; insetTop = top; insetRight = right; insetBottom = bottom
        invalidate()
    }

    private val comboHoldMs = 1200L
    private var comboSince = 0L

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
    }

    /** pointerId -> the control it grabbed. */
    private val grabbed = HashMap<Int, PadLayout.Control>()

    /** Live stick deflection per control id, in view units, for drawing. */
    private val knob = HashMap<String, FloatArray>()

    private var buttons = 0

    // ---- geometry -------------------------------------------------------

    private fun contentWidth(): Float = (width - insetLeft - insetRight).toFloat()
    private fun contentHeight(): Float = (height - insetTop - insetBottom).toFloat()
    private fun unit(): Float = min(contentWidth(), contentHeight())
    private fun px(c: PadLayout.Control): Float = insetLeft + c.cx * contentWidth()
    private fun py(c: PadLayout.Control): Float = insetTop + c.cy * contentHeight()
    private fun pr(c: PadLayout.Control): Float = c.r * unit()

    private fun controlAt(x: Float, y: Float): PadLayout.Control? {
        // Reverse order so the control drawn last (on top) wins a tie.
        for (i in layout.controls.indices.reversed()) {
            val c = layout.controls[i]
            val r = pr(c)
            val dx = x - px(c)
            val dy = y - py(c)
            val hit = when (c.kind) {
                PadLayout.Kind.DPAD, PadLayout.Kind.TOUCHPAD -> abs(dx) <= r && abs(dy) <= r
                // A little forgiveness on round controls: thumbs are not
                // precise and an unresponsive button reads as a dropped
                // packet, which is the bug class this project can least
                // afford to fake.
                else -> hypot(dx, dy) <= r * 1.15f
            }
            if (hit) return c
        }
        return null
    }

    // ---- input ----------------------------------------------------------

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val c = controlAt(event.getX(i), event.getY(i))
                if (c != null) grabbed[event.getPointerId(i)] = c
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP,
            MotionEvent.ACTION_CANCEL -> {
                val id = event.getPointerId(event.actionIndex)
                grabbed.remove(id)?.let { knob.remove(it.id) }
                if (event.actionMasked == MotionEvent.ACTION_CANCEL) releaseAll()
            }
        }

        if (editing) {
            applyEdit(event)
        } else {
            applyPlay(event)
        }
        invalidate()
        return true
    }

    private fun releaseAll() {
        grabbed.clear()
        knob.clear()
        buttons = 0
        comboSince = 0L
        snapshot?.setButtons(InputSnapshot.SRC_TOUCH, 0)
        snapshot?.setSticks(InputSnapshot.SRC_TOUCH, 0, 0, 0, 0)
        snapshot?.setTouches(0, 0, 0, 0, 0, 0, 0, 0, 0)
    }

    private fun applyEdit(event: MotionEvent) {
        if (event.actionMasked != MotionEvent.ACTION_MOVE) return
        for (i in 0 until event.pointerCount) {
            val c = grabbed[event.getPointerId(i)] ?: continue
            c.cx = ((event.getX(i) - insetLeft) / contentWidth()).coerceIn(0.03f, 0.97f)
            c.cy = ((event.getY(i) - insetTop) / contentHeight()).coerceIn(0.03f, 0.97f)
        }
    }

    private fun applyPlay(event: MotionEvent) {
        val s = snapshot ?: return

        var mask = 0
        var lx = 0; var ly = 0; var rx = 0; var ry = 0
        knob.clear()

        // §5.2 carries at most two contacts, and only from the touchpad.
        var tn = 0
        val tid = IntArray(2); val tpr = IntArray(2)
        val tx = IntArray(2); val ty = IntArray(2)

        for (i in 0 until event.pointerCount) {
            val id = event.getPointerId(i)
            val c = grabbed[id] ?: continue
            // A pointer that has been lifted is still in the event; skip it.
            if (event.actionMasked == MotionEvent.ACTION_POINTER_UP &&
                i == event.actionIndex
            ) continue

            val x = event.getX(i)
            val y = event.getY(i)
            val r = pr(c)
            var dx = x - px(c)
            var dy = y - py(c)

            when (c.kind) {
                PadLayout.Kind.BUTTON -> mask = mask or c.bit

                PadLayout.Kind.DPAD -> {
                    // A dead cross in the middle would be a deadzone, but this
                    // is a DIGITAL control: the threshold decides which of 8
                    // directions is pressed, it does not discard magnitude
                    // there is none of. §5.3's prohibition is about analog
                    // axes.
                    val t = r * 0.30f
                    if (dy < -t) mask = mask or AtticPadNative.BTN_DPAD_UP
                    if (dy > t) mask = mask or AtticPadNative.BTN_DPAD_DOWN
                    if (dx < -t) mask = mask or AtticPadNative.BTN_DPAD_LEFT
                    if (dx > t) mask = mask or AtticPadNative.BTN_DPAD_RIGHT
                }

                PadLayout.Kind.STICK -> {
                    val len = hypot(dx, dy)
                    if (len > r) {
                        dx = dx / len * r
                        dy = dy / len * r
                    }
                    knob[c.id] = floatArrayOf(dx, dy)
                    val vx = (dx / r * 32767f).toInt().coerceIn(-32768, 32767)
                    // +Y UP on the wire (§5.3); the screen's +Y is down.
                    val vy = (-dy / r * 32767f).toInt().coerceIn(-32768, 32767)
                    if (c.stick == 0) { lx = vx; ly = vy } else { rx = vx; ry = vy }
                }

                PadLayout.Kind.TOUCHPAD -> {
                    if (tn < 2) {
                        tid[tn] = id and 0xFF
                        // §5.2: 0 means unknown / binary touch. getPressure is
                        // normalised against a device-specific reference and
                        // is 1.0 on most capacitive screens, so it carries no
                        // information worth encoding as if it did.
                        tpr[tn] = 0
                        tx[tn] = ((x - px(c)) / r * 32767f).toInt().coerceIn(-32768, 32767)
                        // +Y DOWN for touch (§5.3) — screen space, no negation.
                        ty[tn] = ((y - py(c)) / r * 32767f).toInt().coerceIn(-32768, 32767)
                        tn++
                    }
                    mask = mask or AtticPadNative.BTN_TOUCH_PRESS
                }
            }
        }

        buttons = mask
        s.setButtons(InputSnapshot.SRC_TOUCH, mask)
        s.setSticks(InputSnapshot.SRC_TOUCH, lx, ly, rx, ry)

        // The on-screen ZL/ZR drive the ANALOG trigger axes to full
        // deflection as well as setting their digital bits.
        //
        // Not cosmetic. §5.4: "If the client advertised APAD_CAP_TRIGGERS,
        // the server MUST drive LT/RT from axes[4]/axes[5] and MUST IGNORE
        // the ZL/ZR bits." This client advertises CAP_TRIGGERS because a
        // physical pad may be plugged in at any moment, and capabilities are
        // fixed at HELLO before that is knowable — so without this the
        // on-screen ZL and ZR would set bits the server is required to throw
        // away, and press as dead buttons. Caught under evtest: every other
        // button emitted an event and those two emitted nothing.
        s.setTriggers(
            InputSnapshot.SRC_TOUCH,
            if (mask and AtticPadNative.BTN_ZL != 0) 32767 else 0,
            if (mask and AtticPadNative.BTN_ZR != 0) 32767 else 0,
        )
        s.setTouches(tn, tid[0], tpr[0], tx[0], ty[0], tid[1], tpr[1], tx[1], ty[1])

        checkCombo(mask)
    }

    /**
     * The hidden self-test entry (docs/CONVENTIONS.md: "Every client ships the hidden
     * self-test screen (hold L+R+Start)").
     *
     * It is a HOLD, not a chord press, because on a touchscreen three
     * simultaneous taps happen by accident during play in a way they do not on
     * a console. And MainActivity offers a visible menu entry as well — on the
     * 3DS a broken shoulder button can make the hidden combo unreachable
     * on that console entirely. A diagnostic
     * with exactly one entry point is one broken button from being unusable.
     */
    private fun checkCombo(mask: Int) {
        val want = AtticPadNative.BTN_L or AtticPadNative.BTN_R or AtticPadNative.BTN_START
        if (mask and want == want) {
            val now = System.currentTimeMillis()
            if (comboSince == 0L) {
                comboSince = now
            } else if (now - comboSince >= comboHoldMs) {
                comboSince = 0L
                onSelfTestCombo?.invoke()
            }
        } else {
            comboSince = 0L
        }
    }

    // ---- drawing --------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        val u = unit()
        text.textSize = u * 0.045f

        for (c in layout.controls) {
            val x = px(c)
            val y = py(c)
            val r = pr(c)
            val active = when (c.kind) {
                PadLayout.Kind.BUTTON -> (buttons and c.bit) != 0
                else -> knob.containsKey(c.id) || grabbed.values.contains(c)
            }

            when (c.kind) {
                PadLayout.Kind.STICK -> {
                    fill.color = if (active) 0x33FFFFFF else 0x1AFFFFFF
                    canvas.drawCircle(x, y, r, fill)
                    stroke.color = 0x66FFFFFF
                    canvas.drawCircle(x, y, r, stroke)
                    val k = knob[c.id]
                    fill.color = if (active) 0xCCFFFFFF.toInt() else 0x80FFFFFF.toInt()
                    canvas.drawCircle(
                        x + (k?.get(0) ?: 0f),
                        y + (k?.get(1) ?: 0f),
                        r * 0.42f, fill,
                    )
                }

                PadLayout.Kind.DPAD -> drawDpad(canvas, x, y, r)

                PadLayout.Kind.TOUCHPAD -> {
                    fill.color = if (active) 0x33FFFFFF else 0x12FFFFFF
                    val rect = RectF(x - r, y - r, x + r, y + r)
                    canvas.drawRoundRect(rect, r * 0.15f, r * 0.15f, fill)
                    stroke.color = 0x4DFFFFFF
                    canvas.drawRoundRect(rect, r * 0.15f, r * 0.15f, stroke)
                    text.color = 0x66FFFFFF
                    canvas.drawText(c.label, x, y + text.textSize * 0.35f, text)
                }

                PadLayout.Kind.BUTTON -> {
                    fill.color = if (active) 0xCCFFFFFF.toInt() else 0x33FFFFFF
                    canvas.drawCircle(x, y, r, fill)
                    stroke.color = 0x80FFFFFF.toInt()
                    canvas.drawCircle(x, y, r, stroke)
                    text.color = if (active) Color.BLACK else Color.WHITE
                    val ts = text.textSize
                    text.textSize = min(ts, r * 0.9f)
                    canvas.drawText(c.label, x, y + text.textSize * 0.35f, text)
                    text.textSize = ts
                }
            }

            if (editing) {
                stroke.color = Theme.ACCENT
                canvas.drawCircle(x, y, max(r, u * 0.03f), stroke)
            }
        }
    }

    private val dpadPath = Path()

    private fun drawDpad(canvas: Canvas, x: Float, y: Float, r: Float) {
        val arm = r * 0.38f
        dpadPath.reset()
        dpadPath.addRect(x - arm, y - r, x + arm, y + r, Path.Direction.CW)
        dpadPath.addRect(x - r, y - arm, x + r, y + arm, Path.Direction.CW)
        fill.color = 0x26FFFFFF
        canvas.drawPath(dpadPath, fill)
        stroke.color = 0x66FFFFFF
        canvas.drawPath(dpadPath, stroke)

        fill.color = 0xCCFFFFFF.toInt()
        val d = r * 0.62f
        if (buttons and AtticPadNative.BTN_DPAD_UP != 0) canvas.drawCircle(x, y - d, arm * 0.6f, fill)
        if (buttons and AtticPadNative.BTN_DPAD_DOWN != 0) canvas.drawCircle(x, y + d, arm * 0.6f, fill)
        if (buttons and AtticPadNative.BTN_DPAD_LEFT != 0) canvas.drawCircle(x - d, y, arm * 0.6f, fill)
        if (buttons and AtticPadNative.BTN_DPAD_RIGHT != 0) canvas.drawCircle(x + d, y, arm * 0.6f, fill)
    }
}
