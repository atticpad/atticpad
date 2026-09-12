# Keyboard, mouse and media

AtticPad clients can also act as a wireless keyboard, mouse and media remote,
alongside the gamepad. This document is for a user deciding whether to trust
it, and a maintainer extending it. The wire format is normative in
`docs/PROTOCOL.md` §6.15–§6.22; this page explains what it means in practice
and is not itself normative — where the two disagree, the spec is right.

- [What it does](#what-it-does)
- [The four modes](#the-four-modes)
- [Negotiation, in one sentence](#negotiation-in-one-sentence)
- [The Windows limitation](#the-windows-limitation)
- [The 10 of 24 media controls Windows cannot drive](#the-10-of-24-media-controls-windows-cannot-drive)
- [The US-QWERTY assumption in text entry](#the-us-qwerty-assumption-in-text-entry)
- [Multiple sessions, one keyboard](#multiple-sessions-one-keyboard)

## What it does

A server that supports it advertises which of keyboard, mouse and media it
will accept (`INPUTCAPS`, §6.19). A client that receives that advertisement
shows the matching UI; a client that never receives one — because the server
predates the feature, or the feature is turned off — shows none of it, and
the gamepad keeps working exactly as it always has. Nothing here touches the
frozen v1 wire format: all four message types (`KEYBOARD` 0x21, `MOUSE` 0x22,
`MEDIA` 0x23, `INPUTCAPS` 0x44) were allocated **after** the freeze, under the
additive rule `docs/PROTOCOL.md` §6.14 states and audits them against. A
server sending them to an old client, or an old server simply never sending
`INPUTCAPS` at all, both behave correctly by construction — §4's "discard an
unrecognised type silently" is the whole compatibility story.

## The four modes

Every client that carries this feature (Android, 3DS) exposes the same four
modes, switching between them the way it already switches views:

| Mode | What it sends |
|---|---|
| **PAD** | The gamepad, unchanged. Always available, regardless of `INPUTCAPS`. |
| **KEYBOARD** | Real per-key hold/release, HID Usage Page 0x07 usage IDs. A physical key position, not a character — see [the US-QWERTY assumption](#the-us-qwerty-assumption-in-text-entry) below. |
| **MOUSE** | Relative motion, wheel and buttons, screen-space `+Y` down (§6.16), the same convention touch already uses. |
| **MEDIA** | Transport, volume and navigation controls, named by §6.18's dense index — an index, never text, so a 3DS can print "VOL+" and a phone can print a speaker glyph from the identical packet. |

A mode only appears once the server has advertised the matching `INPUTCAPS`
feature bit; see [negotiation](#negotiation-in-one-sentence). Leaving a mode
releases everything that mode was holding, on the wire, before the client
stops sending it — §6.20's rule against a modifier left down with nothing
able to lift it.

**Android** adds a `TrackpadView` (drag → motion, edge gutters → wheel, tap →
click, two-finger tap → right-click; sensitivity 1.6× and a 24 dp wheel
threshold, applied client-side because no server-side KBM profile target
exists — unlike stick and touch deadzones, which are never client-side), a
`KeyGridView` (a fixed QWERTY grid with genuine per-pointer hold/release, so a
real multi-finger chord works), a `TextEntryBar` (a curated ASCII-to-HID-usage
path with a Shift wrap, refusing anything outside the set rather than
mangling it), and a `MediaRemoteView` (volume, transport, a D-pad, and a large
Play/Pause).

**3DS** drives all three from the touchscreen, with two console-specific
adaptations. The touchscreen is **resistive and reports one contact at a
time**, so KEYS mode uses a **sticky SHIFT/CTRL latch**: tapping Shift or
Ctrl arms it, and the arm's contribution only reaches the wire while a normal
key is *also* currently touched — so Shift+A goes down and up together as one
chord, rather than Shift going live the instant the latch arms (a real bug,
found live on Azahar and fixed in the same pass; see the commit history for
`clients/3ds/source/screen_session.c`). Physical **L/R are live modifiers**
throughout KEYS mode, unconditionally, on top of the sticky latch — the 3DS
has real shoulder buttons and there is no reason to route them through a
touch-only mechanism. The key grid itself is 10×5, hit-tested arithmetically.

## Negotiation, in one sentence

**A client MUST NOT send `KEYBOARD`, `MOUSE` or `MEDIA` until it has accepted
an `INPUTCAPS` with the matching `features` bit set**, and the shared client
engine (`clients/common/apad_client.c`, `apad_client_pump_ex`) enforces the
send side of that gate itself — no platform layer can forget it. `caps`
(§6.3) is deliberately never widened for this: §6.14 rules that a v1 server
*erases* a reserved `caps` bit rather than ignoring it, so widening `caps`
would be a v2 change. `INPUTCAPS` exists so the same negotiation happens in
the other direction — a server that says nothing has, by construction, said
no.

## The Windows limitation

**Stated plainly: the Windows keyboard/mouse/media backend is `SendInput`,
and `SendInput` is user-mode input injection, not a driver.** This is a real,
permanent limitation of the current backend, not a bug:

- **UIPI.** A non-elevated AtticPad server cannot inject input into an
  elevated window. If the server runs as a normal user and the foreground
  application runs "as Administrator", keystrokes and clicks silently go
  nowhere.
- **The secure desktop.** UAC prompts, the lock screen and Ctrl+Alt+Del run on
  a separate desktop `SendInput` cannot reach at all.
- **Some anti-cheat software rejects synthetic input** outright, because
  `SendInput` is exactly the API a cheat would also use. A real game is the
  final test for this reason — see `docs/QA.md`'s Windows section.
- **Relative mouse motion passes through the user's own "Enhance pointer
  precision" ballistics curve**, the same as any other application's relative
  motion would. There is no way to bypass it from user mode, so a mouse
  feeling "off" on Windows may be Windows' own acceleration, not AtticPad's.
- **Windows has exactly one system keyboard and mouse.** Two AtticPad
  sessions both in KEYBOARD or MOUSE mode drive the *same* injected pointer
  and keyboard — there is no per-session isolation, because `SendInput` has
  no concept of a session.

**Two keyboard keys are also unmapped on Windows, for an unrelated reason.**
PrintScreen (HID usage `0x46`) and Pause (usage `0x48`) are not simple
make/break scancode pairs on real PS/2 hardware — PrintScreen's byte sequence
depends on which modifier is already held, and Pause has no break code at all
and uses a rarer prefix — so `sendinput_scancodes.h` maps both to "no
mapping" rather than guess at the sequence. This is a keyboard-table gap, not
a media-control gap; see the table below for those.

None of this is fixable by writing more careful `SendInput` code — it is what
the API is. `INPUTCAPS.status` carries a `SYNTHETIC` bit (§6.19) precisely so
a client can say so, and `server/backends/backend.h`'s whole reason for
existing is that this is a **one-file problem**, not a design flaw: a
driver-backed Windows input backend (a virtual HID keyboard/mouse through a
kernel driver, the same shape ViGEmBus already gives the gamepad) is a
sibling `.c` file implementing the same `backend.h` interface and one line in
`backends.h`, not a rewrite of anything above it.

## The 10 of 24 media controls Windows cannot drive

§6.18 assigns 24 media control indices. The Linux `uinput` backend drives all
24 of them (`server/backends/uinput_keymap.h`'s `apad_media_to_evdev[]`,
checked against `references/hid/consumer-to-evdev.txt` on every build — see
`scripts/support/check_kbm_tables.c`). **Windows genuinely has fewer media
virtual-keys than §6.18 has controls**: Win32's multimedia `VK_*` range
(`<winuser.h>`) is a fixed, complete set with no room to invent one, and ten
of the 24 assigned controls have no standard equivalent in it at all
(`server/backends/sendinput.c`'s `apad_media_to_vk[]`):

| Control | Why Windows has no `VK_*` for it |
|---|---|
| PLAY | No distinct key from the PLAY_PAUSE toggle (`VK_MEDIA_PLAY_PAUSE` exists; a separate PLAY does not) |
| PAUSE | Same as PLAY |
| FAST_FORWARD | No standard `VK_*` |
| REWIND | No standard `VK_*` |
| EJECT | No standard `VK_*` |
| RECORD | No standard `VK_*` |
| BRIGHTNESS_UP | Delivered through WM_APPCOMMAND/ACPI on real hardware, which `SendInput` cannot synthesise |
| BRIGHTNESS_DOWN | Same as BRIGHTNESS_UP |
| LAUNCH_BROWSER | No dedicated `VK_*` (`VK_LAUNCH_APP1`/`APP2` exist but are OEM/user-reassignable "my computer" style keys with no guaranteed target — mapping to one risks launching the wrong application, which is worse than silently dropping the control) |
| LAUNCH_CALC | Same `VK_LAUNCH_APP1`/`APP2` problem as LAUNCH_BROWSER |

These are mapped to 0 (no mapping, silently skipped — the same convention
`uinput.c` uses for an unmapped code) rather than guessed at. This is a real,
permanent gap in the current Windows backend, reported here rather than
hidden — see `server/backends/sendinput.c`'s header comment for the exact
reasoning per control.

## The US-QWERTY assumption in text entry

`KEYBOARD` carries HID usage IDs — physical key positions — not characters.
That is exactly right for a game (W is the key above S, wherever "W" ends up
printed) and exactly wrong for typing: an on-screen keyboard or a phone's IME
hands an application **characters**, with no key position at all, so
AtticPad's text-entry paths (Android's `TextEntryBar`, the 3DS on-screen
keyboard-driven entry) work backwards through an **assumed US-QWERTY layout**
to invent a usage ID for each character. That assumption is correct only when
the host machine's own keyboard layout also happens to be US-QWERTY. On any
other host layout, the character that actually appears is whatever that
host's layout says the invented usage ID means — which can be a different
letter, or a character requiring a modifier the sender never asked for.

This is why `docs/PROTOCOL.md` §6.22 reserves message type `0x24` for a
future `TEXT` type that would carry UTF-8 codepoints instead of usage IDs,
removing the guess entirely. It is unimplemented today; the reservation
exists so it can be built without another wire-format negotiation. Until
then, treat text entry as "the keys your on-screen keyboard shows, as if the
host were a US keyboard" — see `docs/QA.md` for the check that confirms this
limitation surfaces visibly on a non-US host rather than silently mangling
what was typed.

## Multiple sessions, one keyboard

The Windows section above covers `SendInput`'s single system keyboard and
mouse, but **the same collision exists on Linux**, for a different reason.
The Linux backend creates a **separate `uinput` node per session** (lazily,
on first use of that facility, and torn down in spec order — release held
state, then destroy the device, keyboard/mouse/media, before the session
closes) so two sessions do not share one virtual device the way Windows'
sessions share one real one. But the *desktop* still has only one focused
window, and X11/Wayland deliver every device's keystrokes to whichever window
currently has focus — so two clients both in KEYBOARD mode still end up
typing into the same place, just from two independent virtual keyboards
rather than one shared injection point. This is a desktop input-focus
property, not something AtticPad's backend split can fix on either platform.
