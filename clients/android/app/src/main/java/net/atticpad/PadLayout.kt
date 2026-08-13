package net.atticpad

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/**
 * Where the on-screen controls live, and the editor's data model.
 *
 * Positions are stored NORMALISED (0..1 of the view's width/height) and radii
 * as a fraction of the shorter edge, so one saved layout survives a screen
 * rotation, a different phone, and split-screen. Storing pixels would make the
 * editor's output device-specific, which is the usual way these end up
 * unshareable.
 *
 * Persisted as JSON in SharedPreferences via org.json — framework-provided, so
 * it costs no dependency. This is a CLIENT-side layout, not a mapping profile:
 * profiles and mapping stay on the server (docs/DESIGN.md D9, §7.2's "keep profile
 * and mapping concerns out of the client entirely"). Nothing here says what a
 * button DOES; it says where your thumb finds it.
 */
class PadLayout private constructor(val controls: MutableList<Control>) {

    enum class Kind { STICK, DPAD, BUTTON, TOUCHPAD }

    /**
     * Which of the two independently-persisted layouts this is. Coordinates
     * are normalised to width/height (class doc above), which is exactly
     * why one layout cannot stand in for the other: the same 0.15/0.68 that
     * sits at a landscape phone's lower-left corner lands on a PORTRAIT
     * phone's left edge, well off a resting thumb.
     */
    enum class Orientation { LANDSCAPE, PORTRAIT }

    data class Control(
        val id: String,
        val kind: Kind,
        /** §5.1 bit for Kind.BUTTON; unused otherwise. */
        val bit: Int,
        val label: String,
        /** Which stick, for Kind.STICK: 0 = left, 1 = right. */
        val stick: Int,
        var cx: Float,
        var cy: Float,
        var r: Float,
    )

    fun copy(): PadLayout = PadLayout(controls.map { it.copy() }.toMutableList())

    fun toJson(): String {
        val arr = JSONArray()
        for (c in controls) {
            arr.put(
                JSONObject()
                    .put("id", c.id)
                    .put("cx", c.cx.toDouble())
                    .put("cy", c.cy.toDouble())
                    .put("r", c.r.toDouble())
            )
        }
        return arr.toString()
    }

    companion object {
        private const val PREFS = "atticpad"

        /** Unchanged from before per-orientation layouts existed — an
         *  existing saved layout is landscape (the only orientation the app
         *  offered until now) and keeps loading from the same key it always
         *  has. Nothing about a customised layout is discarded; see
         *  [load]. */
        private const val KEY_LAYOUT_LANDSCAPE = "pad_layout_v1"
        private const val KEY_LAYOUT_PORTRAIT = "pad_layout_portrait_v1"

        private fun keyFor(orientation: Orientation): String = when (orientation) {
            Orientation.LANDSCAPE -> KEY_LAYOUT_LANDSCAPE
            Orientation.PORTRAIT -> KEY_LAYOUT_PORTRAIT
        }

        private fun prefs(context: Context) =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

        fun default(orientation: Orientation): PadLayout = when (orientation) {
            Orientation.LANDSCAPE -> defaultLandscape()
            Orientation.PORTRAIT -> defaultPortrait()
        }

        /**
         * The default landscape layout. Thumbs rest at the lower outer
         * corners, so the sticks sit there and everything else is placed
         * relative to them.
         */
        private fun defaultLandscape(): PadLayout {
            val n = AtticPadNative
            val c = mutableListOf(
                Control("stick_l", Kind.STICK, 0, "L", 0, 0.15f, 0.68f, 0.155f),
                Control("stick_r", Kind.STICK, 0, "R", 1, 0.85f, 0.68f, 0.155f),
                Control("dpad", Kind.DPAD, 0, "", 0, 0.14f, 0.30f, 0.135f),

                Control("a", Kind.BUTTON, n.BTN_A, "A", 0, 0.925f, 0.30f, 0.062f),
                Control("b", Kind.BUTTON, n.BTN_B, "B", 0, 0.855f, 0.40f, 0.062f),
                Control("x", Kind.BUTTON, n.BTN_X, "X", 0, 0.855f, 0.20f, 0.062f),
                Control("y", Kind.BUTTON, n.BTN_Y, "Y", 0, 0.785f, 0.30f, 0.062f),

                Control("l", Kind.BUTTON, n.BTN_L, "L", 0, 0.09f, 0.075f, 0.058f),
                Control("zl", Kind.BUTTON, n.BTN_ZL, "ZL", 0, 0.21f, 0.075f, 0.058f),
                Control("r", Kind.BUTTON, n.BTN_R, "R", 0, 0.91f, 0.075f, 0.058f),
                Control("zr", Kind.BUTTON, n.BTN_ZR, "ZR", 0, 0.79f, 0.075f, 0.058f),

                // Not at cy=0.09: the HUD sits along the top edge and the two
                // overlapped on a 2400x1080 screen.
                Control("select", Kind.BUTTON, n.BTN_SELECT, "SEL", 0, 0.42f, 0.19f, 0.05f),
                Control("start", Kind.BUTTON, n.BTN_START, "START", 0, 0.58f, 0.19f, 0.05f),
                Control("l3", Kind.BUTTON, n.BTN_L3, "L3", 0, 0.33f, 0.86f, 0.048f),
                Control("r3", Kind.BUTTON, n.BTN_R3, "R3", 0, 0.67f, 0.86f, 0.048f),

                // Raw §5.2 contacts come from HERE and nowhere else. The
                // sticks and buttons above consume their touches; forwarding
                // those as touch entries too would tell the server the player
                // is prodding a touchscreen when they are pressing A.
                Control("touchpad", Kind.TOUCHPAD, 0, "touch", 0, 0.50f, 0.42f, 0.155f),
            )
            return PadLayout(c)
        }

        /**
         * The default PORTRAIT layout: a phone held in two hands like a
         * book, thumbs resting near the bottom corners. Shoulder buttons
         * move to the TOP edge (index fingers, not thumbs, reach there in
         * this grip) and the touchpad claims the open middle the diamond
         * and D-pad leave behind. Same 16 control ids as landscape, so a
         * layout saved in one orientation still round-trips through
         * [load]'s by-id matching if a future build ever merges the two —
         * only the positions differ.
         */
        private fun defaultPortrait(): PadLayout {
            val n = AtticPadNative
            val c = mutableListOf(
                Control("stick_l", Kind.STICK, 0, "L", 0, 0.24f, 0.83f, 0.145f),
                Control("stick_r", Kind.STICK, 0, "R", 1, 0.76f, 0.83f, 0.145f),
                Control("dpad", Kind.DPAD, 0, "", 0, 0.18f, 0.585f, 0.115f),

                Control("a", Kind.BUTTON, n.BTN_A, "A", 0, 0.885f, 0.585f, 0.06f),
                Control("b", Kind.BUTTON, n.BTN_B, "B", 0, 0.82f, 0.655f, 0.06f),
                Control("x", Kind.BUTTON, n.BTN_X, "X", 0, 0.82f, 0.515f, 0.06f),
                Control("y", Kind.BUTTON, n.BTN_Y, "Y", 0, 0.755f, 0.585f, 0.06f),

                // Index fingers along the top edge, not thumbs — the whole
                // reason this row moves at all between orientations. cy is
                // NOT flush with 0: it must clear both its own radius (a
                // button whose radius exceeds its offset from the edge gets
                // clipped by the edge, a real bug caught on-device — see
                // client-android's agent memory) and the HUD chip
                // MainActivity overlays along the top centre in play mode.
                Control("l", Kind.BUTTON, n.BTN_L, "L", 0, 0.12f, 0.095f, 0.058f),
                Control("zl", Kind.BUTTON, n.BTN_ZL, "ZL", 0, 0.30f, 0.095f, 0.058f),
                Control("r", Kind.BUTTON, n.BTN_R, "R", 0, 0.88f, 0.095f, 0.058f),
                Control("zr", Kind.BUTTON, n.BTN_ZR, "ZR", 0, 0.70f, 0.095f, 0.058f),

                Control("select", Kind.BUTTON, n.BTN_SELECT, "SEL", 0, 0.38f, 0.205f, 0.045f),
                Control("start", Kind.BUTTON, n.BTN_START, "START", 0, 0.62f, 0.205f, 0.045f),
                // Between the two sticks, low centre — a thumb sliding down
                // and inward off either stick lands near here, the same
                // "click" relationship the landscape default has between
                // its sticks and L3/R3.
                Control("l3", Kind.BUTTON, n.BTN_L3, "L3", 0, 0.40f, 0.905f, 0.042f),
                Control("r3", Kind.BUTTON, n.BTN_R3, "R3", 0, 0.60f, 0.905f, 0.042f),

                // The open middle the diamond, D-pad and top rows leave clear.
                Control("touchpad", Kind.TOUCHPAD, 0, "touch", 0, 0.50f, 0.36f, 0.10f),
            )
            return PadLayout(c)
        }

        /**
         * Loads the layout for [orientation]. An existing landscape save
         * (the only kind that could exist before this file supported
         * portrait) loads exactly as it always has, from the same key —
         * see [KEY_LAYOUT_LANDSCAPE]. A portrait layout that has never been
         * customised falls back to [defaultPortrait] the same way landscape
         * always fell back to its default.
         */
        fun load(context: Context, orientation: Orientation): PadLayout {
            val layout = default(orientation)
            val json = prefs(context).getString(keyFor(orientation), null) ?: return layout
            try {
                val arr = JSONArray(json)
                val byId = layout.controls.associateBy { it.id }
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    // Unknown ids are ignored rather than rejected, so a
                    // layout saved by a build that had an extra control still
                    // loads here.
                    val c = byId[o.optString("id")] ?: continue
                    c.cx = o.optDouble("cx", c.cx.toDouble()).toFloat().coerceIn(0f, 1f)
                    c.cy = o.optDouble("cy", c.cy.toDouble()).toFloat().coerceIn(0f, 1f)
                    c.r = o.optDouble("r", c.r.toDouble()).toFloat().coerceIn(0.02f, 0.35f)
                }
            } catch (_: Exception) {
                return default(orientation)
            }
            return layout
        }

        fun save(context: Context, layout: PadLayout, orientation: Orientation) {
            prefs(context).edit()
                .putString(keyFor(orientation), layout.toJson())
                .apply()
        }

        fun reset(context: Context, orientation: Orientation) {
            prefs(context).edit().remove(keyFor(orientation)).apply()
        }
    }
}
