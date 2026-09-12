# Hardware QA protocol

What a person has to check on real devices, because nothing else can.

CI already builds every target, runs 1987 self-test cases, and drives a real
`uinput` pad through a full session on every push. Emulators cover more than
people expect: the 3DS client runs under Azahar with working sockets, and the
Android client runs in an AVD. **This document is deliberately only the
remainder** — the things where the emulator has no answer, a wrong answer, or a
suspiciously perfect one. If a check can be automated, it belongs in
`scripts/build.sh` or `.github/workflows/ci.yml` instead of here.

Record the result somewhere durable, and record *what you saw*, not just a tick.
"Old 3DS: never run" in `docs/SUPPORT-TIERS.md` is more valuable than a
checklist full of assumed passes.

---

## Why the emulator cannot answer these

| Class | Why emulation is not evidence |
|---|---|
| Analog input | An emulator maps a keyboard or another gamepad onto the circle pad — Azahar's binding is literally `analog_from_button`, digital keys synthesised into analog values. It cannot show the real gate shape, resting bias or wear. |
| Sensors | No emulated gyroscope or accelerometer, so full-scale, bias and sensitivity are untestable — and those are invented numbers in this codebase. |
| Radios | Wi-Fi and Bluetooth are simulated or absent. Latency, jitter, power management and range are properties of the air, not the code. |
| Power states | Lid close, sleep, doze, screen-off and vendor battery killers are host behaviours an emulator does not reproduce. Azahar implements no APT suspend/HOME transition at all, so a clean 30-minute emulator session says **nothing** about the HOME-exit issue. |
| Install paths | Installing over a previous version, signature checks, driver installs and OS trust prompts only exist on the real system. |
| Feel | Deadzone size, curve choice, aim sensitivity and latency perception need a hand and an opinion. |
| Multi-touch / resistive touch | Azahar and an AVD both fake touch as a single mouse click. A genuine two-finger chord (Android) and a resistive screen's single-contact behaviour (3DS, exactly why the sticky SHIFT/CTRL latch exists) have no emulated equivalent at all. |
| Host input injection | Windows `SendInput`'s UIPI elevation check, the secure desktop, anti-cheat rejection and the "Enhance pointer precision" ballistics curve are properties of the real target process and the real Windows input stack — nothing about them is exercised by compiling or by a loopback test. |

---

## 0. The on-device self-test

Do this first: it runs before any networking, so it still works when
everything else is broken.

- [ ] Hold **L + R + Start** while the app launches, **or** use the visible
      `SELECT: self-test` prompt. Both reach the same 1987 cases — worth knowing
      because a worn or dead L shoulder button makes the combo unreachable, and
      that is a real console, not a hypothetical one.
- [ ] Expect **1987 / 1987, 0 failed**. A lower count means the binary is older
      than you think, not that the suite shrank.

## 1. Analog sticks — the shape of the reachable set

The mapping engine shapes sticks radially; a bug where it shaped each axis
independently made the reachable set a diamond and cost every diagonal a third
of its speed. `scripts/build.sh server` guards the arithmetic, but only a real
stick can prove the *hardware's* range reaches the disc.

- [ ] Push the stick slowly all the way round its gate. The pointer traces a
      circle, not a diamond and not a square. Check in the server's web UI pad
      view, or `joy.cpl` on Windows, or `evtest` on Linux.
- [ ] Diagonals reach full speed in a game, not ~66% of a cardinal push.
- [ ] Centre rests at zero: no drift, no crawl. A worn circle pad may not, and
      that is a *profile deadzone* question, not a bug.
- [ ] Small deliberate movements near centre are not swallowed. The deadzone is
      radial, so a nearly-horizontal push keeps its small vertical component.
- [ ] Both sticks, and on a New 3DS the C-stick separately.
- [ ] PSP only: the analog nub. Everything about its shape is unverified —
      PPSSPP synthesises the nub from digital keys, so the emulator's stick is
      a perfect square of values that says nothing about a real one. Check the
      resting value (the client shows it on the self-test screen), whether
      both rails are actually reachable, and whether the `* 258` scaling feels
      right rather than merely arithmetically correct.

## 2. Gyroscope and accelerometer

Full-scale and sensitivity here are **invented numbers**. The gyro coefficient
was measured on one console and cannot be assumed.

- [ ] Console or phone resting on a table reports near-zero rotation, and the
      pad does not creep.
- [ ] Aim mode: turning the device moves the stick in the direction you turned,
      on both axes, with no inversion.
- [ ] A fast turn does not saturate absurdly early or feel dead — this is the
      600 °/s full-scale guess being judged by a person.
- [ ] Deadzone is not so large that fine aim is impossible (12 °/s is 7× the
      measured resting bias; suspect it is too big).

## 3. Wi-Fi, latency and the network's own faults

- [ ] Round-trip time on the client's own display is plausible and stable
      (tens of ms on a quiet LAN), and does not climb over minutes.
- [ ] Session survives an AP with client isolation *or* fails clearly and says
      so — manual IP entry is the documented path when discovery cannot work.
- [ ] Automatic discovery finds the server on a plain home network.
- [ ] PSP: `sceNetApctlConnect()` uses saved network configuration **1**. A
      console whose access point sits in another slot cannot connect and the
      client says only that the call failed. Confirm what a real console does
      with an empty slot 1, and with the WLAN switch off.
- [ ] Old 3DS only: 802.11b and WEP-era constraints. This has **never been
      run** — record whatever happens.
- [ ] Automatic discovery's LAN-broadcast tier specifically: it cannot be
      exercised under Azahar, whose `SO_BROADCAST` returns `ENOPROTOOPT`, so
      hardware is the only place tier 2 is ever really tested.

## 4. Power, sleep and background survival

- [ ] 3DS: close the lid mid-session, reopen. Either it reconnects or it fails
      legibly.
- [ ] 3DS: press HOME mid-session and return. There is an open issue about the
      client exiting to HOME unprompted minutes in — if it fires, note what was
      on screen and what you were doing.
- [ ] PSP: the HOLD switch. With it engaged the pad reads all-zero and the
      client draws a warning strip; confirm that strip appears, because
      without it the bug report is "it connected but nothing works".
- [ ] PSP: close the lid / suspend mid-session, then resume. `apad_ticks_ms()`
      comes from `sceKernelGetSystemTimeWide()`, and whether that stays
      monotonic across a real suspend is exactly what no emulator reproduces.
- [ ] Android: screen off with a session live. Input resumes on wake and the
      foreground service was not killed.
- [ ] Android: vendor battery management. Several manufacturers' skins kill
      foreground services regardless of what the app asks for. If the session
      dies on screen-off, check that autostart is allowed and battery
      optimisation is set to unrestricted for the app before calling it a bug.
- [ ] Battery level reported by the client roughly matches the device, and
      charging state changes are noticed.

## 5. Touchscreen

- [ ] 3DS: the bottom screen draws the **active profile's** touch regions with
      the right labels. Edit a region in the web UI, save, and watch the console
      redraw without reconnecting.
- [ ] Regions respond at their edges, not only in the middle.
- [ ] Analog touch regions (trigger-style) give a usable range with a thumb,
      not just full-on / full-off.
- [ ] Android: multi-touch — two controls at once actually register. An
      emulator's single pointer cannot show this.

## 6. Bluetooth HID mode (Android) — hardware only, no emulator has a stack

- [ ] Pairs with a PC and enumerates as a gamepad.
- [ ] `joy.cpl` (Windows) shows: X/Y = left stick, Z = left trigger, X/Y
      Rotation = right stick, Z Rotation = right trigger, hat = D-pad, and the
      buttons light up. Any axis in the wrong slot is a report-descriptor bug.
- [ ] Works in a DirectInput/SDL game. It is **invisible to XInput-only
      games** — inherent to Bluetooth HID, not a bug to file.
- [ ] After any descriptor change: **remove the device on the host and re-pair.**
      Hosts cache the report descriptor against the phone's Bluetooth address,
      which no app can change, so a stale cache looks exactly like a broken fix.
- [ ] With another Bluetooth HID app installed (a keyboard emulator, say): only
      one app can hold the HID Device role at a time, so expect contention, and
      expect the host to need a re-pair when switching between them.

## 7. Physical gamepad passthrough (Android)

- [ ] A real pad attached to the phone drives the virtual pad: every face
      button in the right place, both sticks, both triggers analog.
- [ ] Face-button positions are correct, not mirrored. The wire uses the
      Nintendo convention and the server translates; a mirrored A/B means the
      translation is wrong, not the pad.

## 8. QR pairing and the camera

- [ ] Scan the server's QR under normal room light. Note roughly how long a
      decode takes; the 3DS camera is slow and this is worth knowing.
- [ ] A wrong or expired code fails with a readable message rather than
      hanging.
- [ ] PIN entry works as the fallback, including the five-attempt lockout.

## 9. Install and first run

- [ ] 3DS: install the `.cia` with FBI over an already-installed copy. It
      replaces rather than duplicating, and the HOME menu icon and banner look
      right. A `.cia` built from the same MAJOR.MINOR.PATCH carries the same
      title version, so a same-version reinstall may not present as an upgrade.
- [ ] 3DS: the `.3dsx` launches from the Homebrew Launcher.
- [ ] Android: the signed release APK installs over the previous release. If it
      refuses, that is a signature mismatch — a debug build must be uninstalled
      first, or use the separate `net.atticpad.debug` package.
- [ ] Windows: on a machine **without** ViGEmBus, the server says so and offers
      the installer link rather than failing obscurely. Then install the driver
      and confirm a pad appears.
- [ ] Windows: SmartScreen warns on the unsigned binary. Confirm the documented
      "Run anyway" path is accurate.
- [ ] Linux: as a non-root user, following only the udev rule in
      `docs/INSTALL.md`, from a clean shell. If it needs `sudo`, the docs are
      wrong.

## 10. Does a game actually play

The whole point, and the one thing no test asserts.

- [ ] Play something for ten minutes. Not "the pad registers" — *play*.
- [ ] Latency is not distracting.
- [ ] No stuck buttons, no drift, nothing that needs a reconnect.
- [ ] Two clients at once, if you have two devices: both pads work and neither
      steals the other's slot.

## 11. Keyboard, mouse and media

Everything here is emulator-verified at best (Azahar for the 3DS, an AVD for
Android) — see `docs/KBM.md` for what the feature does. This section is
where that stops being enough.

### Android — real hardware, real fingers

- [ ] **A genuine two-finger chord on `KeyGridView`.** Hold a modifier key
      (Shift, Ctrl) with one finger and press a letter with a second, real,
      finger. Confirm the modifier and the letter reach the host together, as
      one chord, and that releasing either finger independently releases only
      that key. This is the mode a gamer actually uses; it has only ever been
      proven through `TextEntryBar`'s software-composed Shift-wrap path, which
      exercises none of `KeyGridView`'s own per-pointer tracking. An AVD's
      single simulated pointer cannot show a two-finger chord failing — this
      check exists because nothing else can catch it.
- [ ] **Trackpad sensitivity (1.6×) and wheel threshold (24 dp/detent).**
      These are unreviewed tuning constants with no server-side owner by
      design — unlike stick and touch deadzones, KBM sensitivity is applied
      client-side because no server profile field exists for it. Move the
      trackpad at a normal, deliberate pace and judge whether the pointer
      keeps up without feeling twitchy, and whether a wheel gesture produces
      roughly one detent per what feels like "one notch" of a physical wheel.
      There is no target number to compare against — record your own
      judgement, not a pass/fail against a spec that does not exist yet.
- [ ] **Text entry against a non-US host layout.** Set the server machine's
      keyboard layout to something that reassigns at least one common key
      (AZERTY, for instance), then type a sentence containing that key from
      `TextEntryBar`. Confirm the **wrong character appears** — that is the
      correct, documented behaviour (`docs/KBM.md`'s US-QWERTY assumption) —
      rather than the input being silently dropped, duplicated, or the app
      crashing. The check is that the limitation is *visible*, not that it is
      absent.

### 3DS — real console, resistive screen

- [ ] **Resistive single-contact behaviour.** Confirm you cannot produce two
      simultaneous touch contacts — this is exactly what a mouse-click-based
      emulator fakes away, and it is the entire reason KEYS mode has a sticky
      SHIFT/CTRL latch instead of relying on a second finger. Tap Shift, then
      tap a letter: confirm they chord together on the wire (an `evtest` or
      web-UI observer on the server sees Shift and the letter go down and up
      at effectively the same time, not Shift alone for however long you held
      the latch armed — a real bug in exactly this path was found live on
      Azahar and fixed once; hardware is the next place it could resurface).
- [ ] **Physical L/R as live modifiers**, independent of the sticky latch.
      Hold physical L, tap a letter on the grid: confirm the letter carries
      Ctrl (or whatever the L mapping is) for exactly as long as L is
      physically held, with no latching behaviour at all.
- [ ] **The 10×5 grid with a stylus, at 320×240.** Confirm every key is
      individually hittable without a false neighbour-key hit, held in your
      hand the way you actually would be during a session — not just legible
      on the top screen from a desk.

### Both — the radio, not loopback

- [ ] **Real round-trip time over Wi-Fi**, not loopback. The held-repeat floor
      §6.20 requires is 10 Hz (100 ms), and the shared client engine has only
      been measured at a worst-case 81 ms gap against a live server on the
      same machine — a 19 ms margin. A real radio adds jitter loopback
      cannot; confirm a held key, held mouse button, or held media control
      does **not** visibly release and re-press during normal play, which is
      what happens if the real-world gap ever exceeds the receiver's 1000 ms
      watchdog. Record the worst gap you can observe, however you observe it
      (server log, `evtest` timestamps, or simply watching for a visible
      stutter).

- [ ] **The `INPUTCAPS` auth-window gate (§6.19/§10.2) — give this its own
      run, deliberately.** This is the single highest-risk item in this
      document: if it is wrong, the whole KBM feature dies silently, with
      the rest of the session healthy, and nothing else in this checklist
      would catch it. The gate exists because a server that arms the
      `INPUTCAPS` 0/250/500 ms burst immediately on `ACK` — rather than
      waiting for the first client datagram whose auth tag verifies — lands
      all three copies inside the window where an `AUTH_REQUIRED` client does
      not yet hold its derived key, so every copy fails verification and is
      discarded with no re-request message. This has only ever been proven
      against a **fake clock** (`tools/server-harness`); it has never been
      proven against a real device actually paying PBKDF2's cost. The
      closest documented figure is `docs/PROTOCOL.md` §6.19's "about one
      second of PBKDF2 on a 67 MHz ARM9" (the DS budget) — the 3DS's 268 MHz
      ARM11 has never been timed for this specifically, so do not assume it
      is faster by exactly the clock ratio.
    - [ ] Open a **pairing window** (`AUTH_REQUIRED` will be set) and connect
          a 3DS with KEYBOARD or MOUSE mode available.
    - [ ] **Time, or at least characterise, the gap** between the client's
          `ACK` (or the PIN/QR entry that precedes it) and the KBM UI
          becoming available. Record the actual figure — this is establishing
          a number the project does not have, not confirming one.
    - [ ] Confirm `INPUTCAPS` **does eventually arrive and the KBM UI
          appears** — the failure mode is silent, so "it works" must be
          checked positively, not inferred from the absence of an error.
    - [ ] Reconnect several times in a row (the burst-and-slow-repeat
          schedule re-arms on every fresh session) and confirm this is
          consistent, not a one-time race that happened to land right.

### Windows — real host, real applications

- [ ] **Notepad**: scancode fidelity. Type a full sentence including
      Shift-modified characters, and confirm the extended-key arrow keys
      (Home/End/Page Up/Page Down/Delete, and the dedicated arrow cluster —
      not the numeric keypad's overlapping codes) move the cursor rather than
      inserting keypad digits. This is the `KEYEVENTF_EXTENDEDKEY` /
      `APAD_SC_EXT_BIT` distinction (`server/backends/sendinput_scancodes.h`)
      getting it right on real hardware, not just matching the vendored
      table.
- [ ] **PrintScreen and Pause**, if the keyboard grid exposes them: confirm
      they do nothing rather than typing garbage — `sendinput_scancodes.h`
      deliberately leaves both unmapped (neither is a plain make/break pair
      on real PS/2 hardware), a documented gap distinct from the media-control
      table below.
- [ ] **A browser**: wheel and horizontal wheel scrolling, and mouse
      back/forward (the `XBUTTON1`/`XBUTTON2` mapping) navigating browser
      history.
- [ ] **A media player**: confirm which transport controls actually land, and
      cross-check against `docs/KBM.md`'s table of the 10 of 24 §6.18
      controls this backend cannot drive at all (PLAY, PAUSE, FAST_FORWARD,
      REWIND, EJECT, RECORD, BRIGHTNESS_UP, BRIGHTNESS_DOWN, LAUNCH_BROWSER,
      LAUNCH_CALC) — the ones that do work should all behave correctly, and
      the ones that don't should simply do nothing, not produce a wrong
      action.
- [ ] **One real game**: the injected-scancode question. Confirm keyboard and
      mouse input actually reaches the game. If it does not, before filing a
      bug check whether the game (or its anti-cheat) is running elevated
      while the AtticPad server is not — `docs/KBM.md`'s UIPI note — and
      whether the game rejects synthetic input outright. Either outcome is
      worth recording precisely, because it is exactly the gap the
      driver-backed backend mentioned in `docs/KBM.md` would close.

---

## 12. Nintendo DS and DSi — first hardware run

Hardware-proven in both modes; `SUPPORT-TIERS.md` records what is verified.
melonDS covers the protocol, the
screens and the self-test, and none of the following. The test console here is
a 3DS in DS mode from a flashcart, on the WEP access point that
`docs/SETUP-DS.md` describes (`scripts/ds-ap-atticpad.sh up` after the AP is
up, or the DS's packets are dropped by the fence).

- **Self-test first**, before any network: hold L + R + START at launch, then
  again via the on-screen button. Record the count and the seconds. This is
  the ARM9 alignment proof the whole `memcpy` discipline exists for; melonDS
  emulates a rotating unaligned load faithfully but a real core is the
  evidence.
- **Association.** Does the saved Nintendo Wi-Fi Connection slot join on its
  own, and how long from launch to an address? Then forget the slot and go
  through the picker: does the scan list the AP, does the WEP key typed on
  the keyboard work first time, does a WPA network show as not joinable in DS
  mode?
- **Tier-2 discovery.** With the server on the AP's host, does the console find
  it without typing anything? melonDS cannot pass a broadcast, so this has
  never been seen to work.
- **Round trip on an 802.11b radio.** Both the console's figure and the
  server's column; note the jitter, and whether the server ever logs an idle
  timeout during a quiet ten minutes.
- **Send rate.** `rx_packets` at the server over 10 s: is it ~60/s on real
  hardware, and what does the diag panel's frame table say?
- **Touch feel.** The absolute-stick pad: does the thumb reach the rails, does
  release snap to centre, is the resistive panel's first contact clean or does
  the settle window need retuning? Then MOUSE: tap-to-click, drag, the wheel
  gutter; KEYS: a letter, the sticky Shift chord, physical L as Shift; MEDIA.
- **Lid.** Close it mid-session: does the pad release, does the session
  survive the reopen or time out (and if so, does reconnect work)?
- **Persistence.** Relaunch: is the address pre-filled from the flashcart's
  SD? The `fat:/` path has only ever been exercised as a silent no-op.
- **DSi mode**, if a launcher provides it (TWiLight Menu++ on the 3DS): does
  `isDSiMode()` report true on screen, does WPA2 join the house network, does
  the battery percentage appear, does the QR pairing screen see the camera?
- **Battery and heat** over a 30-minute session.

## Reporting

Say what ran, on what, and what was seen. Anything not run stays "never run" —
`docs/SUPPORT-TIERS.md` exists so that label is available and honest, and a
support tier moves only when a person has actually watched the thing work.
