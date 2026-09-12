package net.atticpad

/**
 * [InputSnapshot]'s sibling for §6.15-6.17 keyboard, mouse and media: every
 * KBM producer (TrackpadView, KeyGridView, TextEntryBar, MediaRemoteView) is
 * on the UI thread, [AtticPadService]'s session thread is the one consumer,
 * and a `synchronized` block around a small copy is the whole concurrency
 * story — same discipline, same reason, as [InputSnapshot]'s own doc comment.
 *
 * NO LATCHING HERE, unlike [InputSnapshot.buttonsSeen] — and that asymmetry
 * is deliberate, not a gap. `buttonsSeen` exists because §5's INPUT_STATE is
 * a STATE-ONLY wire format: a fast tap that begins and ends between two
 * transmitted frames has no representation in "what's held right now" at
 * all, so the client has to manufacture one by remembering "was this seen
 * since the last frame". §6.15-6.17 do not have that problem: `events[]` is
 * a genuine SUBMISSION QUEUE (apad_client.h), and the engine assigns it a
 * §6.20 ring slot with its own ordinal independent of whether the pump that
 * carries it also changes `keys[]`/`buttons`/`held`. A sub-frame tap here is
 * represented DIRECTLY, by one queue entry, not synthesised from "did this
 * bit ever go high between two samples" — the same bug [InputSnapshot] works
 * around, solved properly one layer down, in the wire format itself.
 *
 * RELEASE ON MODE SWITCH. Leaving a facility does not clear anything on the
 * host by itself — apad_client's own state machine only auto-releases when
 * an INPUTCAPS `features` bit CLEARS (§6.19/§6.20), and switching the mode
 * bar away from KEYBOARD is not that. Left alone, the last held state
 * (e.g. a key still down when the user tapped MOUSE) would keep being
 * repeated at the §6.20 floor forever — a key stuck on the host with
 * nothing left able to lift it, the exact failure §6.20 exists to prevent,
 * just triggered by the UI instead of the network. [setKeyboardActive] (and
 * its mouse/media siblings) handle it: going inactive clears the held state
 * immediately and forces ONE MORE pump to carry that cleared state with its
 * `have` bit set, so the engine's own shadow-diff (apad_client.c
 * kbm_ingest_keyboard) synthesises the release events — no event needs to
 * be hand-queued for this, clearing `keys[]`/`buttons`/`held` is enough.
 */
class KbmSnapshot {

    companion object {
        private const val KB_RING = AtticPadNative.KB_RING_DEPTH
        private const val MOUSE_RING = AtticPadNative.MOUSE_RING_DEPTH
        private const val MEDIA_RING = AtticPadNative.MEDIA_RING_DEPTH
    }

    private val lock = Any()

    // ---- keyboard ---------------------------------------------------------
    private val keysHeld = BooleanArray(256)
    private val keyQueue = ArrayDeque<IntArray>()   // [usage, downFlag], oldest first
    private var keyboardActive = false
    private var keyboardNeedsClear = false

    // ---- mouse --------------------------------------------------------------
    // Free-running wrapping uint16 counters, per apad_client.h: added to,
    // never reset, for the life of the session. Kept masked to 16 bits so a
    // long session cannot overflow a Kotlin Int before it ever reaches the
    // wire's own uint16_t wrap.
    private var dxAccum = 0
    private var dyAccum = 0
    private var wheelAccum = 0
    private var hwheelAccum = 0
    private var mouseButtons = 0
    private val mouseQueue = ArrayDeque<IntArray>() // [button, downFlag]
    private var mouseActive = false
    private var mouseNeedsClear = false

    // ---- media --------------------------------------------------------------
    private var mediaHeld = 0
    private val mediaQueue = ArrayDeque<IntArray>() // [control, downFlag]
    private var mediaActive = false
    private var mediaNeedsClear = false

    // ---- mode-bar lifecycle -------------------------------------------------

    /** Called by MainActivity when the KEYBOARD body becomes/stops being the
     *  visible mode. See the class doc for why going inactive clears state. */
    fun setKeyboardActive(active: Boolean) = synchronized(lock) {
        if (!active && keyboardActive) {
            keysHeld.fill(false)
            keyQueue.clear()
            keyboardNeedsClear = true
        }
        keyboardActive = active
    }

    fun setMouseActive(active: Boolean) = synchronized(lock) {
        if (!active && mouseActive) {
            mouseButtons = 0
            mouseQueue.clear()
            mouseNeedsClear = true
        }
        mouseActive = active
    }

    fun setMediaActive(active: Boolean) = synchronized(lock) {
        if (!active && mediaActive) {
            mediaHeld = 0
            mediaQueue.clear()
            mediaNeedsClear = true
        }
        mediaActive = active
    }

    /**
     * `[dxAccum, dyAccum, wheelAccum, hwheelAccum]`, read-only, with NONE of
     * [readInto]'s side effects — no queue draining, no `mouseNeedsClear`
     * consumption, no `have`-bit computation. Exists so a consumer whose own
     * "session" boundary does not line up with a `readInto` pump cadence
     * (BtHidController's Bluetooth connect, not AtticPadService's UDP session
     * thread) can establish a §6.16-rule-1 baseline against whatever the
     * accumulators already read as, safely, from any thread, at any time —
     * unlike `keysHeld`/`mouseButtons`/the three event queues, these four
     * fields are never cleared or drained by a read, only ever added to by
     * [mouseMove]/[mouseWheel], so re-reading them here costs nothing and
     * races with nothing.
     */
    fun mouseAccumSnapshot(): IntArray = synchronized(lock) {
        intArrayOf(dxAccum, dyAccum, wheelAccum, hwheelAccum)
    }

    // ---- keyboard producers (KeyGridView, TextEntryBar) --------------------

    /** usage: an APAD_HID_KEY_* / HID usage 1..255. A no-op outside that
     *  range (0 is kbm.h's "no event", and HID usage is a single byte). */
    fun keyEvent(usage: Int, down: Boolean) = synchronized(lock) {
        if (usage <= 0 || usage > 255) return@synchronized
        keysHeld[usage] = down
        if (keyQueue.size < KB_RING) {
            keyQueue.addLast(intArrayOf(usage, if (down) AtticPadNative.KBM_EVENT_DOWN else 0))
        }
    }

    /** True chords: TextEntryBar wraps a shift press/release around a pair
     *  submitted in the SAME pump, KeyGridView holds real fingers down —
     *  both just call [keyEvent] twice with no extra bookkeeping, because
     *  `keysHeld` already tracks arbitrary simultaneous holds. */
    fun isKeyHeld(usage: Int): Boolean = synchronized(lock) {
        usage in 1..255 && keysHeld[usage]
    }

    // ---- mouse producers (TrackpadView) --------------------------------------

    fun mouseMove(dx: Int, dy: Int) = synchronized(lock) {
        dxAccum = (dxAccum + dx) and 0xFFFF
        dyAccum = (dyAccum + dy) and 0xFFFF
    }

    /** `detents`: +1 per notch away from the user (vertical) / right
     *  (horizontal) — §6.16. TrackpadView turns a drag distance into
     *  detents; this just accumulates them. */
    fun mouseWheel(detents: Int, horizontal: Boolean) = synchronized(lock) {
        if (horizontal) {
            hwheelAccum = (hwheelAccum + detents) and 0xFFFF
        } else {
            wheelAccum = (wheelAccum + detents) and 0xFFFF
        }
    }

    /** button: one of AtticPadNative.MOUSEBTN_*. */
    fun mouseButtonEvent(button: Int, down: Boolean) = synchronized(lock) {
        if (button !in AtticPadNative.MOUSEBTN_LEFT..AtticPadNative.MOUSEBTN_FORWARD) return@synchronized
        val bit = AtticPadNative.mouseBtnBit(button)
        mouseButtons = if (down) mouseButtons or bit else mouseButtons and bit.inv()
        if (mouseQueue.size < MOUSE_RING) {
            mouseQueue.addLast(intArrayOf(button, if (down) AtticPadNative.KBM_EVENT_DOWN else 0))
        }
    }

    // ---- media producers (MediaRemoteView) -----------------------------------

    /** control: one of AtticPadNative.MEDIA_*. Held so VOLUME_UP can ramp
     *  while pressed (§6.17). */
    fun mediaEvent(control: Int, down: Boolean) = synchronized(lock) {
        if (control <= 0 || control > 32) return@synchronized
        val bit = AtticPadNative.mediaBit(control)
        mediaHeld = if (down) mediaHeld or bit else mediaHeld and bit.inv()
        if (mediaQueue.size < MEDIA_RING) {
            mediaQueue.addLast(intArrayOf(control, if (down) AtticPadNative.KBM_EVENT_DOWN else 0))
        }
    }

    // ---- consumer (AtticPadService's session thread, once per pump) --------

    /**
     * Fills [AtticPadNative.IN_KBM_PRESENT]..[AtticPadNative.IN_LEN] of
     * [out], which must already be sized to [AtticPadNative.IN_LEN] — the
     * same array [InputSnapshot.readInto] fills the pad-state prefix of.
     *
     * `have` only carries bits for a facility that is ACTIVE (its mode is
     * the visible body) or has just gone inactive and still needs its one
     * clearing pump (see the class doc) — never a facility that has simply
     * never been touched this session. §6.20's floor/cadence and the
     * §6.19 INPUTCAPS gate are both apad_client's own job from here
     * (clients/common/apad_client.c kbm_pump/kbm_ingest_*): submitting the
     * unchanged truth every pump is harmless, it does not by itself put
     * anything new on the wire.
     */
    fun readInto(out: IntArray) = synchronized(lock) {
        var have = 0

        if (keyboardActive || keyboardNeedsClear) {
            have = have or AtticPadNative.KBM_FEATURE_KEYBOARD
            for (word in 0 until 8) {
                var w = 0
                for (b in 0 until 4) {
                    val usage = word * 32 + b * 8
                    var byteVal = 0
                    for (bit in 0 until 8) {
                        if (keysHeld[usage + bit]) byteVal = byteVal or (1 shl bit)
                    }
                    w = w or (byteVal shl (b * 8))
                }
                out[AtticPadNative.IN_KB_KEYS0 + word] = w
            }
            for (i in 0 until KB_RING) {
                val ev = keyQueue.getOrNull(i)
                out[AtticPadNative.IN_KB_EVENTS0 + i] =
                    if (ev != null) (ev[0] and 0xFF) or (ev[1] shl 8) else 0
            }
            keyQueue.clear()
            keyboardNeedsClear = false
        } else {
            for (i in 0 until 8) out[AtticPadNative.IN_KB_KEYS0 + i] = 0
            for (i in 0 until KB_RING) out[AtticPadNative.IN_KB_EVENTS0 + i] = 0
        }

        if (mouseActive || mouseNeedsClear) {
            have = have or AtticPadNative.KBM_FEATURE_MOUSE
            out[AtticPadNative.IN_MOUSE_DX] = dxAccum
            out[AtticPadNative.IN_MOUSE_DY] = dyAccum
            out[AtticPadNative.IN_MOUSE_WHEEL] = wheelAccum
            out[AtticPadNative.IN_MOUSE_HWHEEL] = hwheelAccum
            out[AtticPadNative.IN_MOUSE_BUTTONS] = mouseButtons
            for (i in 0 until MOUSE_RING) {
                val ev = mouseQueue.getOrNull(i)
                out[AtticPadNative.IN_MOUSE_EVENTS0 + i] =
                    if (ev != null) (ev[0] and 0xFF) or (ev[1] shl 8) else 0
            }
            mouseQueue.clear()
            mouseNeedsClear = false
        } else {
            out[AtticPadNative.IN_MOUSE_DX] = dxAccum
            out[AtticPadNative.IN_MOUSE_DY] = dyAccum
            out[AtticPadNative.IN_MOUSE_WHEEL] = wheelAccum
            out[AtticPadNative.IN_MOUSE_HWHEEL] = hwheelAccum
            out[AtticPadNative.IN_MOUSE_BUTTONS] = 0
            for (i in 0 until MOUSE_RING) out[AtticPadNative.IN_MOUSE_EVENTS0 + i] = 0
        }

        if (mediaActive || mediaNeedsClear) {
            have = have or AtticPadNative.KBM_FEATURE_MEDIA
            out[AtticPadNative.IN_MEDIA_HELD] = mediaHeld
            for (i in 0 until MEDIA_RING) {
                val ev = mediaQueue.getOrNull(i)
                out[AtticPadNative.IN_MEDIA_EVENTS0 + i] =
                    if (ev != null) (ev[0] and 0xFF) or (ev[1] shl 8) else 0
            }
            mediaQueue.clear()
            mediaNeedsClear = false
        } else {
            out[AtticPadNative.IN_MEDIA_HELD] = 0
            for (i in 0 until MEDIA_RING) out[AtticPadNative.IN_MEDIA_EVENTS0 + i] = 0
        }

        out[AtticPadNative.IN_KBM_PRESENT] = have
    }
}
