package net.atticpad

/**
 * What the ONE session UI — [PadView], plus [MouseBody]/[KeyboardBody]/
 * [MediaRemoteView] and [KbmChipRow] (KbmModeBar.kt) — needs from whichever
 * backend is actually feeding it right now: [AtticPadService] over UDP, or
 * [BluetoothTransport] over Bluetooth HID with no server at all.
 *
 * This is deliberately the ENTIRE interface. Neither [PadView] nor any of
 * the KBM bodies have ever known which transport they were bound to — they
 * only ever touched [InputSnapshot]/[KbmSnapshot], already transport-
 * agnostic producer objects (see KbmModeBar.kt's file doc) — so the session
 * UI itself needed no new abstraction, only ONE place ([MainActivity]) that
 * can point the same views at either backend instead of building two copies
 * of the UI around two backends, one Activity each. That second copy
 * ([BtControllerActivity], ~1050 lines) is what this interface replaces.
 */
interface SessionTransport {
    /** Every input source (touch, physical pad, sensors) writes here; the
     *  transport's own send loop reads it. */
    val input: InputSnapshot

    /** §6.15-6.17 keyboard/mouse/media producers write here; the transport's
     *  own send loop reads it alongside [input]. */
    val kbm: KbmSnapshot
}
