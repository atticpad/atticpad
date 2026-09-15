package net.atticpad

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout

/**
 * The `Direct` toggle, an `EditText` and SEND (task brief) — MOUSE mode's
 * collapsible bottom bar. Every character this class ever sends becomes a
 * press+release PAIR in [KbmSnapshot]'s queue, whether it arrives one at a
 * time (Direct on, live) or all at once (Direct off, on SEND): both paths
 * share [dispatchNext] so there is exactly one place that turns a character
 * into wire-shaped events and exactly one throttle.
 *
 * US QWERTY ASSUMED, ON PURPOSE, AND SAID OUT LOUD. An `InputConnection`
 * (soft keyboard OR a hardware one plugged into the phone — both arrive
 * through the same `EditText`) hands this class CHARACTERS, not scancodes.
 * There is no way to recover which physical key produced 'a' on a Cyrillic
 * or Dvorak layout from the character alone, so this can only be correct
 * against a US QWERTY layout on the PC end. §6.22 reserves 0x24 for a future
 * Unicode TEXT message that would fix this properly by sending the
 * character itself instead of guessing a scancode — out of scope here.
 * [mapChar] therefore refuses anything it cannot place on THIS app's
 * curated HID usage set (kbm.h) rather than mangling it: silently sending
 * the wrong key is a worse failure than refusing to send at all.
 *
 * DIRECT ON = each keystroke goes out live, as it is typed — a [TextWatcher]
 * diffs the field against its own last-known contents and queues the
 * difference. DIRECT OFF = the field is just local scratch space; nothing
 * reaches [KbmSnapshot] until SEND, which streams the whole string and then
 * clears the field. Both funnel through [enqueue]/[dispatchNext].
 */
class TextEntryBar(context: Context) : LinearLayout(context) {

    var kbm: KbmSnapshot? = null

    /** Shown above the field — the one place this limitation is surfaced,
     *  per the task brief ("surface this limitation in the UI, do not hide
     *  it"), rather than only in a comment nobody using the app will read. */
    private val hint = captionText(
        context,
        "Sends to the PC as US QWERTY keystrokes — plain letters, digits, space, tab and " +
            "newline only.",
    )

    private var directOn = false
    private val toggle = secondaryButton(context, "Direct: OFF") { onToggle() }
    private val textField = styledEditText(context, "Type here…")
    private val sendButton = primaryButton(context, "Send") { onSend() }
    private val pasteButton = secondaryButton(context, "Paste") { onPaste() }

    private val collapseHandle = rowButton(context, "Keyboard input  ▾") { onCollapseToggle() }
    private val content = LinearLayout(context).apply { orientation = VERTICAL }
    private var collapsed = true

    private var previousText = ""
    private var suppressWatcher = false

    // ---- throttled dispatch: character -> press+release pair(s) ------------

    private data class Pending(val usage: Int, val shift: Boolean)

    private val pending = ArrayDeque<Pending>()
    private val handler = Handler(Looper.getMainLooper())
    private var dispatching = false

    /** ~2 pump intervals at the session's own 60 Hz cadence — comfortably
     *  clear of KB_RING_DEPTH (8) even for an all-uppercase burst (4 queue
     *  slots per character), which is what keeps a fast paste or a held key
     *  from silently overflowing the ring between two pumps. */
    private val throttleMs = 40L

    init {
        orientation = VERTICAL
        val padH = Theme.dp(context, Theme.SPACE_MD)
        setPadding(padH, Theme.dp(context, Theme.SPACE_SM), padH, Theme.dp(context, Theme.SPACE_SM))
        background = Theme.hudBackground(context)

        addView(collapseHandle, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        content.addView(hint, vlp(Theme.SPACE_XS))

        val toggleRow = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        toggleRow.addView(toggle)
        content.addView(toggleRow, vlp(Theme.SPACE_SM))

        content.addView(textField, vlp(Theme.SPACE_SM))

        val buttonRow = LinearLayout(context).apply { orientation = HORIZONTAL }
        buttonRow.addView(
            pasteButton,
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f),
        )
        buttonRow.addView(
            sendButton,
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = Theme.dp(context, Theme.SPACE_SM)
            },
        )
        content.addView(buttonRow, vlp(Theme.SPACE_SM))

        addView(content, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        content.visibility = View.GONE

        textField.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) = onFieldChanged(s?.toString() ?: "")
        })
    }

    private fun vlp(top: Int) = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
    ).apply { topMargin = Theme.dp(context, top) }

    private fun onCollapseToggle() {
        collapsed = !collapsed
        content.visibility = if (collapsed) View.GONE else View.VISIBLE
        collapseHandle.text = if (collapsed) "Keyboard input  ▾" else "Keyboard input  ▴"
    }

    private fun onToggle() {
        directOn = !directOn
        toggle.text = if (directOn) "Direct: ON" else "Direct: OFF"
        sendButton.visibility = if (directOn) View.GONE else View.VISIBLE
        if (directOn) previousText = textField.text.toString()
    }

    /** Reset when the mode is left or the session ends — an un-flushed
     *  local string, or a diff baseline from a previous connection, must
     *  not leak into the next one. */
    fun reset() {
        pending.clear()
        dispatching = false
        suppressWatcher = true
        textField.setText("")
        suppressWatcher = false
        previousText = ""
    }

    // ---- Direct-mode live diff ----------------------------------------------

    private fun onFieldChanged(newText: String) {
        if (suppressWatcher || !directOn) {
            previousText = newText
            return
        }
        val old = previousText
        var p = 0
        while (p < old.length && p < newText.length && old[p] == newText[p]) p++
        var oldEnd = old.length
        var newEnd = newText.length
        while (oldEnd > p && newEnd > p && old[oldEnd - 1] == newText[newEnd - 1]) {
            oldEnd--; newEnd--
        }
        val deleted = oldEnd - p
        val inserted = newText.substring(p, newEnd)
        repeat(deleted) { enqueue(Pending(AtticPadNative.HID_KEY_BACKSPACE, false)) }
        var refused: Char? = null
        for (c in inserted) {
            val mapped = mapChar(c)
            if (mapped == null) {
                refused = c
                continue
            }
            enqueue(Pending(mapped.first, mapped.second))
        }
        if (refused != null) showRefused(refused)
        previousText = newText
    }

    // ---- SEND (Direct off) ---------------------------------------------------

    private fun onSend() {
        val text = textField.text.toString()
        if (text.isEmpty()) return
        val bad = text.firstOrNull { mapChar(it) == null }
        if (bad != null) {
            showRefused(bad)
            return
        }
        for (c in text) {
            val mapped = mapChar(c) ?: continue
            enqueue(Pending(mapped.first, mapped.second))
        }
        suppressWatcher = true
        textField.setText("")
        suppressWatcher = false
        previousText = ""
    }

    private fun onPaste() {
        val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
        val clip: ClipData? = cm?.primaryClip
        val text = if (clip != null && clip.itemCount > 0) {
            clip.getItemAt(0).coerceToText(context)?.toString()
        } else {
            null
        }
        if (text.isNullOrEmpty()) return
        val cursor = textField.selectionEnd.coerceAtLeast(0)
        textField.text.insert(cursor, text)
    }

    private fun showRefused(c: Char) {
        val why = if (c.code > 127) "non-ASCII character" else "no key for '$c' on this layout"
        hint.text = "Not sent — $why. US QWERTY letters, digits, space, tab and newline only."
        hint.setTextColor(Theme.WARNING)
    }

    /** ASCII -> (HID usage, needs LeftShift). Only the characters reachable
     *  from kbm.h's curated key set (see class doc) map to anything; every
     *  other char, ASCII or not, is refused rather than guessed. */
    private fun mapChar(c: Char): Pair<Int, Boolean>? = when {
        c in 'a'..'z' -> (AtticPadNative.HID_KEY_A + (c - 'a')) to false
        c in 'A'..'Z' -> (AtticPadNative.HID_KEY_A + (c - 'A')) to true
        c in '1'..'9' -> (AtticPadNative.HID_KEY_1 + (c - '1')) to false
        c == '0' -> AtticPadNative.HID_KEY_0 to false
        c == ' ' -> AtticPadNative.HID_KEY_SPACE to false
        c == '\n' -> AtticPadNative.HID_KEY_ENTER to false
        c == '\t' -> AtticPadNative.HID_KEY_TAB to false
        else -> null
    }

    private fun enqueue(p: Pending) {
        pending.addLast(p)
        if (!dispatching) dispatchNext()
    }

    private fun dispatchNext() {
        val p = pending.removeFirstOrNull()
        if (p == null) {
            dispatching = false
            return
        }
        dispatching = true
        kbm?.let { k ->
            if (p.shift) k.keyEvent(AtticPadNative.HID_KEY_LEFTSHIFT, true)
            k.keyEvent(p.usage, true)
            k.keyEvent(p.usage, false)
            if (p.shift) k.keyEvent(AtticPadNative.HID_KEY_LEFTSHIFT, false)
        }
        handler.postDelayed({ dispatchNext() }, throttleMs)
    }
}
