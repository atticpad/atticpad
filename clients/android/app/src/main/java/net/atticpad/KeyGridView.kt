package net.atticpad

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import android.view.animation.AnimationUtils
import kotlin.math.roundToInt

/**
 * The physical-style key grid (task brief: "F-row, number row, QWERTY rows,
 * modifier row"). A fixed layout, not [PadLayout]'s draggable/persisted one
 * — a QWERTY grid has one sane arrangement and nothing here needs a
 * per-device edit mode the way a thumb's reach over a virtual stick does.
 *
 * REAL HOLD/RELEASE IS THE WHOLE POINT (task brief) — a pointer id maps to
 * the cell it grabbed, down on ACTION_DOWN/POINTER_DOWN and up on
 * UP/POINTER_UP/CANCEL, exactly like [PadView]'s own `grabbed` map. A phone
 * can do a genuine multi-finger chord (hold Shift, press a letter with a
 * second finger) the way a 3DS's fixed physical buttons cannot, and that is
 * only true if every finger is tracked independently rather than the grid
 * reducing multi-touch to "whichever cell the last pointer landed on".
 *
 * Writes to [kbm] only, one [KbmSnapshot.keyEvent] per edge — never a
 * packet, never [InputSnapshot]. Animation state ([pressAt]/[releaseAt]) is
 * stamped strictly AFTER those calls and nothing in the drawing path ever
 * reads [kbm] — see the class-level note in `Theme.kt`'s `Motion` object.
 *
 * ---- proportions (2026 redesign, reworked again the same year) ----------
 *
 * Every row is laid out against ONE shared unit width, [UNIT_TOTAL] units
 * across, rather than each row independently stretched to fill the full
 * view width. That single change is what produces a real keyboard's
 * staggered silhouette for free: Tab is 1.5u, the home row's leading gap is
 * 1.75u, Shift is 2.25u — different leading widths at a SHARED scale is
 * exactly what staggers a physical keyboard's rows against each other.
 *
 * Every row here sums to exactly [UNIT_TOTAL] (15u). Row HEIGHT is derived
 * from that same unit ([ROW_ASPECT] of it) in [onMeasure] — a key's height
 * is a function of how wide IT is, never of how much spare vertical space
 * the phone happens to have. A previous pass added a height-FRACTION floor
 * (`gridH.coerceIn(availH * 0.58f, availH * 0.62f)`) to stop a tall
 * portrait phone handing the keyboard a sliver and the trackpad the rest —
 * but a fraction of available height has NO relationship to the unit width,
 * so on a 1080-wide/2400-tall phone it forced a ~72px-wide letter key to a
 * ~200px-tall row: three times taller than wide. Fixed by keeping the
 * per-row math entirely unit-derived and adding a second, independent
 * ceiling — [MAX_ROW_ASPECT] — that bounds the FINAL row height to at most
 * 1.15x the unit width regardless of what the dp floor/ceiling would
 * otherwise allow. [MIN_ROW_HEIGHT_DP] is deliberately set high enough that
 * on a narrow phone it always loses to that aspect ceiling — i.e. portrait
 * always lands exactly at the tallest non-distorted row the screen's WIDTH
 * allows, which is the most "the keyboard is the dominant control" this
 * geometry can honestly deliver without stretching a key. [MAX_HEIGHT_FRACTION]
 * remains as a ceiling (never a floor) so a very wide unit — landscape,
 * where width-derived rows would otherwise want to be huge — cannot crowd
 * the trackpad out of the layout.
 */
class KeyGridView(context: Context) : View(context) {

    var kbm: KbmSnapshot? = null

    /** Coarse visual role. Darker/receding for keys a typist rarely looks
     *  at (modifiers, function row); lighter/raised for the ones that carry
     *  text or a committing action — the real soft-keyboard convention,
     *  where letters read as the "surface" and modifiers sit below it. */
    enum class KeyRole { ALPHANUM, MODIFIER, FUNCTION, ACTION }

    class Key(
        val label: String,
        val usage: Int,
        val weight: Float = 1f,
        val role: KeyRole = KeyRole.ALPHANUM,
        /** Presentation only — shown while Shift is held, digits and
         *  punctuation only. The wire is unaffected: [kbm]?.keyEvent still
         *  sends this key's own (unshifted) usage plus whatever Shift usage
         *  is independently held, which is correct HID — the host OS, not
         *  this label, is what turns "1"+Shift into "!". Do not "fix" this
         *  by sending a different usage when shifted. */
        val shifted: String? = null,
    )

    /** One keyboard row: its keys, left-to-right, plus how many UNITS of
     *  blank lead-in space sit before the first key (the stagger). */
    class KRow(val keys: List<Key>, val leadUnits: Float = 0f)

    companion object {
        private fun k(
            label: String,
            usage: Int,
            weight: Float = 1f,
            role: KeyRole = KeyRole.ALPHANUM,
            shifted: String? = null,
        ) = Key(label, usage, weight, role, shifted)

        /** Every [KRow] below sums (leadUnits + key weights) to exactly
         *  this — see the class doc. */
        private const val UNIT_TOTAL = 15f

        /** A 1-unit key's height as a fraction of its own width — under 1
         *  so a plain letter key reads slightly wide rather than square,
         *  and a modifier at 1.2-2.75u reads clearly, unmistakably wide. */
        private const val ROW_ASPECT = 0.85f

        /** Hard ceiling on the FINAL row height, expressed as a multiple of
         *  the unit width rather than a dp constant, because "how tall is
         *  too tall" is a question about a key's own proportions, not about
         *  the screen. 1.15 reads as "mildly taller than wide" — the most
         *  a cap key or Enter should ever look, never the ~2.7x the old
         *  height-fraction floor produced on a tall portrait phone. */
        private const val MAX_ROW_ASPECT = 1.5f

        /** Set high on purpose: on a narrow (portrait) unit this ALWAYS
         *  loses to [MAX_ROW_ASPECT] below, which is what makes portrait
         *  land at the tallest non-distorted row its width allows rather
         *  than something arbitrarily smaller. On a wide (landscape) unit,
         *  where [ROW_ASPECT] alone already clears this floor, it has no
         *  effect at all. */
        private const val MIN_ROW_HEIGHT_DP = 34f

        /** Absolute ceiling in dp terms, for very wide units (tablets,
         *  landscape) — raised from the previous pass's 46dp so a wide unit
         *  can grow the row in absolute terms too, still subject to
         *  [MAX_ROW_ASPECT] on top. */
        private const val MAX_ROW_HEIGHT_DP = 64f

        /** The grid's total height never exceeds this fraction of whatever
         *  height budget [onMeasure] is offered — a ceiling only, not the
         *  floor the previous pass also had. Without it a wide (landscape)
         *  unit's row height, even after the per-row clamps above, can add
         *  up across [ROWS] to most of a short landscape body and starve
         *  the trackpad above it. */
        private const val MAX_HEIGHT_FRACTION = 0.58f

        /** A MODEST floor, and the history here matters. An earlier pass set
         *  this to 0.58 and it stretched a ~460px-ideal grid to ~1200px,
         *  producing keys three times taller than wide. Removing it entirely
         *  then swung the other way: the portrait grid collapsed to ~20% of
         *  the body, leaving 22dp-wide keys under a huge empty trackpad —
         *  which is the ORIGINAL complaint this constant was introduced to
         *  fix. 0.34 keeps the keyboard the dominant control in KEYBOARD mode
         *  without the floor ever winning against [MAX_ROW_ASPECT], so key
         *  proportion stays honest. Landscape's width-derived height already
         *  exceeds this comfortably, so the floor never engages there and
         *  landscape is unaffected — verified by screenshot, not assumed. */
        private const val MIN_HEIGHT_FRACTION = 0.34f

        val ROWS: List<KRow> = listOf(
            // F-row: Esc(1) + F1..F12(1 each) + Del(2) = 15u.
            KRow(
                listOf(
                    k("Esc", AtticPadNative.HID_KEY_ESCAPE, role = KeyRole.FUNCTION),
                    k("F1", AtticPadNative.HID_KEY_F1, role = KeyRole.FUNCTION),
                    k("F2", AtticPadNative.HID_KEY_F2, role = KeyRole.FUNCTION),
                    k("F3", AtticPadNative.HID_KEY_F3, role = KeyRole.FUNCTION),
                    k("F4", AtticPadNative.HID_KEY_F4, role = KeyRole.FUNCTION),
                    k("F5", AtticPadNative.HID_KEY_F5, role = KeyRole.FUNCTION),
                    k("F6", AtticPadNative.HID_KEY_F6, role = KeyRole.FUNCTION),
                    k("F7", AtticPadNative.HID_KEY_F7, role = KeyRole.FUNCTION),
                    k("F8", AtticPadNative.HID_KEY_F8, role = KeyRole.FUNCTION),
                    k("F9", AtticPadNative.HID_KEY_F9, role = KeyRole.FUNCTION),
                    k("F10", AtticPadNative.HID_KEY_F10, role = KeyRole.FUNCTION),
                    k("F11", AtticPadNative.HID_KEY_F11, role = KeyRole.FUNCTION),
                    k("F12", AtticPadNative.HID_KEY_F12, role = KeyRole.FUNCTION),
                    k("Del", AtticPadNative.HID_KEY_DELETE, 2f, role = KeyRole.ACTION),
                ),
            ),
            // Number row: `(1) 1..0(10) -(1) =(1) Bksp(2) = 15u.
            KRow(
                listOf(
                    k("`", AtticPadNative.HID_KEY_GRAVE, shifted = "~"),
                    k("1", AtticPadNative.HID_KEY_1, shifted = "!"),
                    k("2", AtticPadNative.HID_KEY_2, shifted = "@"),
                    k("3", AtticPadNative.HID_KEY_3, shifted = "#"),
                    k("4", AtticPadNative.HID_KEY_4, shifted = "$"),
                    k("5", AtticPadNative.HID_KEY_5, shifted = "%"),
                    k("6", AtticPadNative.HID_KEY_6, shifted = "^"),
                    k("7", AtticPadNative.HID_KEY_7, shifted = "&"),
                    k("8", AtticPadNative.HID_KEY_8, shifted = "*"),
                    k("9", AtticPadNative.HID_KEY_9, shifted = "("),
                    k("0", AtticPadNative.HID_KEY_0, shifted = ")"),
                    k("-", AtticPadNative.HID_KEY_MINUS, shifted = "_"),
                    k("=", AtticPadNative.HID_KEY_EQUAL, shifted = "+"),
                    k("Bksp", AtticPadNative.HID_KEY_BACKSPACE, 2f, role = KeyRole.ACTION),
                ),
            ),
            // Tab row: Tab(1.5) QWERTYUIOP(10) [(1) ](1) \(1.5) = 15u.
            KRow(
                listOf(
                    k("Tab", AtticPadNative.HID_KEY_TAB, 1.5f, role = KeyRole.MODIFIER),
                    k("Q", AtticPadNative.HID_KEY_Q), k("W", AtticPadNative.HID_KEY_W),
                    k("E", AtticPadNative.HID_KEY_E), k("R", AtticPadNative.HID_KEY_R),
                    k("T", AtticPadNative.HID_KEY_T), k("Y", AtticPadNative.HID_KEY_Y),
                    k("U", AtticPadNative.HID_KEY_U), k("I", AtticPadNative.HID_KEY_I),
                    k("O", AtticPadNative.HID_KEY_O), k("P", AtticPadNative.HID_KEY_P),
                    k("[", AtticPadNative.HID_KEY_LEFTBRACE, shifted = "{"),
                    k("]", AtticPadNative.HID_KEY_RIGHTBRACE, shifted = "}"),
                    k("\\", AtticPadNative.HID_KEY_BACKSLASH, 1.5f, shifted = "|"),
                ),
            ),
            // Home row: lead 1.75u (where CapsLock would sit) + ASDFGHJKL(9)
            // ;(1) '(1) + Enter(2.25) = 15u — Enter belongs here on a real
            // keyboard, not crammed into the bottom row.
            KRow(
                listOf(
                    k("A", AtticPadNative.HID_KEY_A), k("S", AtticPadNative.HID_KEY_S),
                    k("D", AtticPadNative.HID_KEY_D), k("F", AtticPadNative.HID_KEY_F),
                    k("G", AtticPadNative.HID_KEY_G), k("H", AtticPadNative.HID_KEY_H),
                    k("J", AtticPadNative.HID_KEY_J), k("K", AtticPadNative.HID_KEY_K),
                    k("L", AtticPadNative.HID_KEY_L),
                    k(";", AtticPadNative.HID_KEY_SEMICOLON, shifted = ":"),
                    k("'", AtticPadNative.HID_KEY_APOSTROPHE, shifted = "\""),
                    k("Enter", AtticPadNative.HID_KEY_ENTER, 2.25f, role = KeyRole.ACTION),
                ),
                leadUnits = 1.75f,
            ),
            // Shift row: Shift(2.25) ZXCVBNM(7) ,.(2) /(1) Shift(2.75) = 15u.
            KRow(
                listOf(
                    k("Shift", AtticPadNative.HID_KEY_LEFTSHIFT, 2.25f, role = KeyRole.MODIFIER),
                    k("Z", AtticPadNative.HID_KEY_Z), k("X", AtticPadNative.HID_KEY_X),
                    k("C", AtticPadNative.HID_KEY_C), k("V", AtticPadNative.HID_KEY_V),
                    k("B", AtticPadNative.HID_KEY_B), k("N", AtticPadNative.HID_KEY_N),
                    k("M", AtticPadNative.HID_KEY_M),
                    k(",", AtticPadNative.HID_KEY_COMMA, shifted = "<"),
                    k(".", AtticPadNative.HID_KEY_PERIOD, shifted = ">"),
                    k("/", AtticPadNative.HID_KEY_SLASH, shifted = "?"),
                    k("Shift", AtticPadNative.HID_KEY_RIGHTSHIFT, 2.75f, role = KeyRole.MODIFIER),
                ),
            ),
            // Bottom row: Ctrl/Win/Alt/AltGr(1.3 each) + Space(6) + 4 arrows
            // (0.95 each) = 15u.
            KRow(
                listOf(
                    k("Ctrl", AtticPadNative.HID_KEY_LEFTCTRL, 1.3f, role = KeyRole.MODIFIER),
                    k("Win", AtticPadNative.HID_KEY_LEFTGUI, 1.3f, role = KeyRole.MODIFIER),
                    k("Alt", AtticPadNative.HID_KEY_LEFTALT, 1.3f, role = KeyRole.MODIFIER),
                    k("Space", AtticPadNative.HID_KEY_SPACE, 6f, role = KeyRole.ACTION),
                    k("AltGr", AtticPadNative.HID_KEY_RIGHTALT, 1.3f, role = KeyRole.MODIFIER),
                    k("←", AtticPadNative.HID_KEY_LEFT, 0.95f, role = KeyRole.ACTION),
                    k("↓", AtticPadNative.HID_KEY_DOWN, 0.95f, role = KeyRole.ACTION),
                    k("↑", AtticPadNative.HID_KEY_UP, 0.95f, role = KeyRole.ACTION),
                    k("→", AtticPadNative.HID_KEY_RIGHT, 0.95f, role = KeyRole.ACTION),
                ),
            ),
        )

        private fun roleFill(role: KeyRole): Int = when (role) {
            KeyRole.ALPHANUM -> Theme.SURFACE_RAISED
            KeyRole.MODIFIER, KeyRole.FUNCTION -> Theme.SURFACE
            KeyRole.ACTION -> Theme.SURFACE_HIGH
        }

        private fun roleLabelColor(role: KeyRole): Int = when (role) {
            KeyRole.ALPHANUM, KeyRole.ACTION -> Theme.TEXT_PRIMARY
            KeyRole.MODIFIER, KeyRole.FUNCTION -> Theme.TEXT_SECONDARY
        }

        private fun roleTypeface(role: KeyRole): Typeface = when (role) {
            KeyRole.ALPHANUM, KeyRole.ACTION -> Theme.TYPE_MEDIUM
            KeyRole.MODIFIER, KeyRole.FUNCTION -> Theme.TYPE_REGULAR
        }

        private fun roleBaseSizeDp(role: KeyRole): Float = when (role) {
            KeyRole.ALPHANUM, KeyRole.ACTION -> Theme.TEXT_KEY
            KeyRole.MODIFIER, KeyRole.FUNCTION -> Theme.TEXT_KEY_SMALL
        }

        /** Only [KeyRole.ALPHANUM]/[KeyRole.ACTION] get the lit top bevel —
         *  a modifier/function key already reads as receded via its darker
         *  fill and does not also need a highlighted edge. */
        private fun roleHasBevel(role: KeyRole): Boolean =
            role == KeyRole.ALPHANUM || role == KeyRole.ACTION

        /** Inter-key gap. Raised from 2dp to 3dp so the GAP (not a per-key
         *  stroke — removed, see [roleHasBevel]) is what visually separates
         *  seventy-odd keys from each other. */
        private const val KEY_PAD_DP = 3f
    }

    /** pointerId -> the cell it grabbed. Mirrors PadView's `grabbed`. */
    private val grabbed = HashMap<Int, Key>()
    private val cellRects = HashMap<Key, RectF>()

    /** Per-key label size, cached at [layoutCells] time (only runs on a
     *  size change) — never recomputed in [onDraw], which would mean up to
     *  70 `measureText` calls a frame. */
    private val cellTextSize = HashMap<Key, Float>()

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val availH = MeasureSpec.getSize(heightMeasureSpec)
        val unit = w / UNIT_TOTAL
        val minRow = Theme.dpF(context, MIN_ROW_HEIGHT_DP)
        val maxRow = Theme.dpF(context, MAX_ROW_HEIGHT_DP)
        var rowH = (unit * ROW_ASPECT).coerceIn(minRow, maxRow)
        // Hard ceiling, independent of the dp bounds above: a key must
        // never read as dramatically taller than it is wide.
        rowH = rowH.coerceAtMost(unit * MAX_ROW_ASPECT)
        var gridH = rowH * ROWS.size
        if (availH > 0) {
            gridH = gridH.coerceIn(
                availH * MIN_HEIGHT_FRACTION,
                availH * MAX_HEIGHT_FRACTION,
            )
        }
        setMeasuredDimension(w, gridH.roundToInt())
    }

    private fun layoutCells() {
        cellRects.clear()
        cellTextSize.clear()
        if (width <= 0 || height <= 0) return
        val unit = width / UNIT_TOTAL
        val rowH = height.toFloat() / ROWS.size
        val pad = Theme.dpF(context, KEY_PAD_DP)
        for ((ri, row) in ROWS.withIndex()) {
            val y0 = ri * rowH
            var x = row.leadUnits * unit
            for (key in row.keys) {
                val w = key.weight * unit
                val rect = RectF(x, y0, x + w, y0 + rowH)
                cellRects[key] = rect
                cellTextSize[key] = fitTextSize(key, rect, pad)
                x += w
            }
        }
    }

    /** The role's own size, shrunk only if [key]'s label would otherwise
     *  overflow its cell — one label size per role, not the old three-way
     *  clamp that rendered several visibly different sizes in one grid. */
    private fun fitTextSize(key: Key, r: RectF, pad: Float): Float {
        val base = Theme.dpF(context, roleBaseSizeDp(key.role))
        text.typeface = roleTypeface(key.role)
        text.textSize = base
        val avail = r.width() - pad * 4f
        if (avail <= 0f) return base
        val measured = text.measureText(key.label)
        return if (measured <= avail) base else base * (avail / measured).coerceAtLeast(0.4f)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutCells()
    }

    private fun keyAt(x: Float, y: Float): Key? {
        for ((key, r) in cellRects) if (r.contains(x, y)) return key
        return null
    }

    // ---- press motion state --------------------------------------------

    /** Timestamp a key was last pressed/released, for the sink-and-glow
     *  animation below. Stamped strictly AFTER the [kbm] call on the same
     *  edge — see the class doc and `Theme.kt`'s `Motion` object. Nothing
     *  in this file's animation path ever reads [kbm]. */
    private val pressAt = HashMap<Key, Long>()
    private val releaseAt = HashMap<Key, Long>()

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val key = keyAt(event.getX(i), event.getY(i))
                if (key != null) {
                    grabbed[event.getPointerId(i)] = key
                    kbm?.keyEvent(key.usage, true)
                    val now = AnimationUtils.currentAnimationTimeMillis()
                    releaseAt.remove(key)
                    pressAt[key] = now
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val id = event.getPointerId(event.actionIndex)
                grabbed.remove(id)?.let { key ->
                    kbm?.keyEvent(key.usage, false)
                    val now = AnimationUtils.currentAnimationTimeMillis()
                    pressAt.remove(key)
                    releaseAt[key] = now
                }
            }

            MotionEvent.ACTION_CANCEL -> {
                val now = AnimationUtils.currentAnimationTimeMillis()
                for (key in grabbed.values) {
                    kbm?.keyEvent(key.usage, false)
                    pressAt.remove(key)
                    releaseAt[key] = now
                }
                grabbed.clear()
            }
        }
        postInvalidateOnAnimation()
        return true
    }

    // ---- drawing ------------------------------------------------------------

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val bevel = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
    }

    /** Reused, never reallocated: `Paint.getFontMetrics()` (no argument)
     *  returns a NEW object every call, and this is read once per key per
     *  frame. */
    private val fm = Paint.FontMetrics()

    override fun onDraw(canvas: Canvas) {
        if (cellRects.isEmpty()) layoutCells()
        val now = AnimationUtils.currentAnimationTimeMillis()
        var needsFrame = false

        val pad = Theme.dpF(context, KEY_PAD_DP)
        val radius = Theme.dpF(context, Theme.RADIUS_XS.toFloat())
        val sinkMax = Theme.dpF(context, 1.5f)
        val bevelInset = radius
        bevel.strokeWidth = Theme.dpF(context, 1f)

        // Presentation-only shift lookup — from this view's OWN `grabbed`
        // map, never `KbmSnapshot` (which the session thread pumps at
        // 60Hz behind a lock). See [Key.shifted].
        val shiftHeld = grabbed.values.any {
            it.usage == AtticPadNative.HID_KEY_LEFTSHIFT || it.usage == AtticPadNative.HID_KEY_RIGHTSHIFT
        }

        for ((key, r) in cellRects) {
            val pAt = pressAt[key]
            val rAt = releaseAt[key]
            val t: Float = when {
                pAt != null -> {
                    if (!Motion.settled(pAt, now, Motion.DUR_INSTANT)) needsFrame = true
                    Motion.progress(pAt, now, Motion.DUR_INSTANT, Motion.EASE_OUT)
                }
                rAt != null -> {
                    if (!Motion.settled(rAt, now, Motion.DUR_FAST)) needsFrame = true
                    1f - Motion.progress(rAt, now, Motion.DUR_FAST, Motion.EASE_IN)
                }
                else -> 0f
            }

            val sink = pad + sinkMax * t
            val left = r.left + sink
            val top = r.top + sink
            val right = r.right - sink
            val bottom = r.bottom - sink

            fill.color = Theme.blend(roleFill(key.role), Theme.ACCENT, t)
            canvas.drawRoundRect(left, top, right, bottom, radius, radius, fill)

            if (roleHasBevel(key.role) && t < 1f) {
                val alpha = ((Theme.BEVEL_TOP ushr 24) * (1f - t)).roundToInt()
                bevel.color = (alpha shl 24) or (Theme.BEVEL_TOP and 0x00FFFFFF)
                canvas.drawLine(left + bevelInset, top, right - bevelInset, top, bevel)
            }

            text.typeface = roleTypeface(key.role)
            text.textSize = cellTextSize[key] ?: Theme.dpF(context, roleBaseSizeDp(key.role))
            text.color = Theme.blend(roleLabelColor(key.role), Theme.ACCENT_ON, t)
            val label = if (shiftHeld && key.shifted != null) key.shifted else key.label
            // Optical centre from the font's OWN metrics rather than a
            // fixed multiplier of textSize — a descender ("Bksp") and a
            // shape with none ("F1") would otherwise sit at visibly
            // different heights inside identical cells.
            text.getFontMetrics(fm)
            canvas.drawText(label, r.centerX(), r.centerY() - (fm.ascent + fm.descent) / 2f, text)
        }

        releaseAt.entries.removeIf { (_, at) -> Motion.settled(at, now, Motion.DUR_FAST) }
        if (needsFrame) postInvalidateOnAnimation()
    }
}
