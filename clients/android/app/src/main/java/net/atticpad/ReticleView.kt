package net.atticpad

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.view.View

/**
 * The scan card's "aim here" mark: four accent-coloured corner brackets
 * around a centred square, CORNERS ONLY — not a full frame — matching the
 * transit-app reference this screen's design was translated from. Purely
 * decorative: it draws on top of [QrScanner]'s live preview but never
 * touches a pixel `apad_qr.c`/quirc see — the decoder reads the camera's
 * `ImageReader` target directly, not this view.
 *
 * Never intercepts touches (see [onTouchEvent]) — it sits directly over the
 * live preview and must not shadow it.
 */
class ReticleView(context: Context) : View(context) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.ACCENT
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeWidth = Theme.dpF(context, 3f)
    }

    /** Fraction of the shorter view dimension the reticle square occupies,
     *  and how long each corner's two strokes are relative to that square —
     *  tuned by eye against a screenshot, not derived from anything.
     *
     *  Retuned DOWN from 0.62 when the scan slot went square
     *  (MainActivity.buildScanCard / Theme.SCAN_PREVIEW_SIZE): the old
     *  value was tuned against a WIDE-SHORT slot, where the reticle's
     *  corners sat well clear of [MainActivity.buildTorchToggle]'s fixed
     *  48dp circle in the slot's actual top-right CORNER — a square slot
     *  puts that corner much closer to the reticle's own corner (same
     *  distance from centre on both axes now, where before the reticle's
     *  horizontal extent was capped by the shorter axis while the torch
     *  toggle sat out along the much longer one). At the old fraction the
     *  top-right bracket visibly ran under the torch button on
     *  Theme.SCAN_PREVIEW_SIZE_LANDSCAPE's smaller 180dp cap — caught by
     *  screenshotting landscape specifically, the same lesson
     *  connect_screen_redesign.md already logged for the placeholder text
     *  in this exact card. 0.5 clears it with comfortable margin at both
     *  the portrait and landscape caps. */
    private val squareFraction = 0.5f
    private val armFraction = 0.22f

    init {
        // The camera feed and its live decode are the interactive surface;
        // this view exists only to be looked at.
        isClickable = false
        isFocusable = false
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val side = minOf(width, height) * squareFraction
        if (side <= 0f) return
        val cx = width / 2f
        val cy = height / 2f
        val left = cx - side / 2f
        val top = cy - side / 2f
        val right = cx + side / 2f
        val bottom = cy + side / 2f
        val arm = side * armFraction

        // Top-left
        canvas.drawLine(left, top, left + arm, top, paint)
        canvas.drawLine(left, top, left, top + arm, paint)
        // Top-right
        canvas.drawLine(right, top, right - arm, top, paint)
        canvas.drawLine(right, top, right, top + arm, paint)
        // Bottom-left
        canvas.drawLine(left, bottom, left + arm, bottom, paint)
        canvas.drawLine(left, bottom, left, bottom - arm, paint)
        // Bottom-right
        canvas.drawLine(right, bottom, right - arm, bottom, paint)
        canvas.drawLine(right, bottom, right, bottom - arm, paint)
    }

    override fun onTouchEvent(event: android.view.MotionEvent): Boolean = false
}
