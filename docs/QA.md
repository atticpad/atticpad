# Hardware QA protocol

What a person has to check on real devices, because nothing else can.

CI already builds every target, runs 1141 self-test cases, and drives a real
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

---

## 0. The on-device self-test

Do this first: it runs before any networking, so it still works when
everything else is broken.

- [ ] Hold **L + R + Start** while the app launches, **or** use the visible
      `SELECT: self-test` prompt. Both reach the same 1141 cases — worth knowing
      because a worn or dead L shoulder button makes the combo unreachable, and
      that is a real console, not a hypothetical one.
- [ ] Expect **1141 / 1141, 0 failed**. A lower count means the binary is older
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
- [ ] Android: screen off with a session live. Input resumes on wake and the
      foreground service was not killed.
- [ ] Android: vendor battery management (Xiaomi/HyperOS and friends kill
      foreground services). If the session dies, check autostart and the
      "no restrictions" battery setting before calling it a bug.
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

---

## Reporting

Say what ran, on what, and what was seen. Anything not run stays "never run" —
`docs/SUPPORT-TIERS.md` exists so that label is available and honest, and a
support tier moves only when a person has actually watched the thing work.
