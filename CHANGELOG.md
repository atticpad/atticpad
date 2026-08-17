# Changelog

All notable changes to AtticPad are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses `0.<milestone>.<patch>` versioning until a non-technical user can install
it and play a game, which is what 1.0 will mean.

The **product version** and the **protocol version** move independently. The
wire format is AtticPad protocol **v1, frozen** — see `docs/PROTOCOL.md`.

## [0.5.0] — 2026-08-17

**First public release.** AtticPad was built over several months before this
tag; everything is listed once here rather than backdated into releases that
were never published. A 0.4.0 was prepared and never finalised, so its work is
part of this release too.

Install instructions are in [`docs/INSTALL.md`](docs/INSTALL.md) — including a
QR code you can scan straight from FBI to install on a 3DS without touching an
SD card.

### Highlights

- **The server now tells the client where its touch controls are.** A new
  message, `TOUCHMAP` (§6.12), carries the active profile's touch regions —
  their rectangles and the button each one presses. The 3DS draws them on the
  touchscreen, so the bottom screen finally shows the controls you actually
  have instead of a fixed block of text. The server sends the layout when a
  client connects, when the profile is edited and saved, and when a connected
  pad is switched to a different profile.
- **Protocol v1 is still frozen, and this did not unfreeze it.** §4 requires a
  client to silently discard message types it does not know, which is exactly
  what a 0.4.0 client does with `TOUCHMAP`. Nothing that existed before changed
  size, position or meaning. `docs/PROTOCOL.md` §6.14 records what "frozen"
  permits, so the next addition does not have to re-argue it.
- **The web UI's test view draws a real Xbox 360 pad** instead of numbered
  boxes: sticks, triggers, bumpers and d-pad light up in place, so it is
  obvious at a glance which physical control a client is actually sending.

### Fixed

- **Analog sticks reached a diamond, not a circle.** The server shaped each
  stick axis independently, so with the default quadratic response curve a
  round stick became the locus `|x| + |y| = 1` — full deflection on a diagonal
  produced only **0.66** of the magnitude a cardinal push did, so every
  diagonal was a third slower than it should have been. Shaping is now radial:
  the magnitude is shaped once and re-projected along the input direction, so
  direction is preserved exactly and the reachable set is a disc. The deadzone
  becomes a dead *disc* rather than a dead *cross*, so a nearly-horizontal push
  no longer has its small vertical component silently zeroed. This was
  server-side, so it affected every client at once and is fixed for all of them
  without updating anything on the device.
- **Bluetooth HID mode put the sticks and triggers on the wrong axes.** The
  HID report descriptor declared the right stick on `Z`/`Rz` and the triggers
  on `Rx`/`Ry` — the DualShock arrangement — while Windows assigns axis slots
  by usage and games expect the Xbox one. The result was a right stick that
  drove a trigger and a trigger that drove a stick axis. The descriptor now
  matches what an Xbox Bluetooth controller reports: left stick `X`/`Y`, left
  trigger `Z`, right stick `Rx`/`Ry`, right trigger `Rz`, D-pad on the hat.
  **Re-pair the phone after updating** — a host caches the report descriptor
  from pairing, so it will keep using the old axis map until you remove the
  device and pair again.
- **The web UI's pad view mis-drew controllers the browser does not remap.** It
  assumed the Gamepad API's `standard` layout for every pad. One the browser
  does not recognise reports no mapping and exposes its axes in the device's own
  order, so the view drew the wrong controls and printed misleading axis
  numbers. It now says so plainly and lists the raw axis and button values
  instead of a picture that cannot be trusted.

- **The Windows server crashed on startup** when it had no profiles directory,
  because it freed uninitialised stack pointers on the built-in-profile
  fallback path. Linux had the same bug and survived it by luck.
- **Duplicating a profile failed** with "could not write the profile file" on a
  machine that had never saved one. The profiles directory is now created on
  demand.
- **A profile saved to disk hid the built-in profiles** instead of taking
  precedence over them, so editing one profile made the others disappear.
  Startup and hot reload now assemble the list the same way, from one place.
- **Reloading profiles left already-connected pads on their old profile.** A
  live session now moves to a newly matching profile, unless it was pinned to
  one by hand in the web UI.
- **The Android in-session dialogs had square corners showing through.** The
  platform draws its own opaque rectangular window behind a dialog, which showed
  through the transparent corners of the app's rounded card as four grey
  notches — visible against the near-black session screen.
- **Debug and release Android builds can now coexist.** They shared an
  application ID, so installing either over the other failed on the signature
  mismatch and the only way through was to uninstall first, taking the app's
  data with it. A debug build is now `net.atticpad.debug`, labelled "AtticPad
  debug".

### Everything else that is in this release

- **Protocol v1**, frozen: a byte-exact UDP wire format with a 12-byte header,
  capability bits, three-tier discovery, a PIN/QR pairing handshake, and
  wrap-safe sequence and tick arithmetic. `docs/PROTOCOL.md` is normative.
- **`libapad`** — the shared protocol core in C99: codec, session state machine,
  HMAC-SHA256, PBKDF2, and sequence helpers. No `malloc` after init, no
  floating point, no stdio; it runs on a 67 MHz ARM9 with 4 MB RAM.
- **239 conformance vectors**, generated from the specification alone by an
  author who did not read the codec, and shipped in every client as an
  on-device self-test — on consoles, by holding **L+R+Start** at launch.
- **Nintendo 3DS client** — `.cia` and `.3dsx`. Buttons, circle pad, C-stick,
  gyro, touchscreen, battery reporting, on-screen IP entry, and a QR scanner
  for pairing.
- **Android client** — a single APK with no third-party runtime dependencies.
  On-screen touch controls, physical gamepad passthrough, gyro and
  accelerometer, QR scanning, and a foreground service that survives the
  screen going off. Android 8.0+, arm64-v8a / armeabi-v7a / x86_64.
- **Linux server** — creates virtual gamepads through `uinput`, enumerating as
  an Xbox 360 controller so games recognise them without configuration.
- **Windows server** — creates XInput gamepads through ViGEmBus, with a tray
  application and driver detection.
- **Mapping engine and profiles** — JSONC profiles owning deadzone, response
  curve, inversion, touch regions and gyro aim, so no client ever applies a
  deadzone of its own.
- **Pairing** — a 6-digit PIN valid for 120 seconds, five-attempt lockout,
  PBKDF2-HMAC-SHA256 session key derivation, and QR-code pairing that carries
  the same secret without typing.
- **Discovery** — mDNS where available, LAN broadcast, and manual IP entry.
  Manual entry is a first-class path, because client-isolating access points
  and VPNs break the other two.
- **Local web UI** — bound to `127.0.0.1` only, for pad status, round-trip
  latency, and a profile editor with hot reload.

### Known limitations

- The 3DS `.cia` is signed with the well-known test key, so it installs only on
  a console running Luma3DS custom firmware.
- The 3DS client has been run on a **New 3DS** only. Old 3DS is untested.
- The Windows server requires **ViGEmBus**, whose upstream project is archived
  and no longer updated. See `docs/INSTALL.md`.
- **The server accepts unauthenticated clients by default.** Authentication
  applies only while a pairing window is open, and pairing is not remembered
  between sessions — there is no persistent trusted-device list yet. Run it
  only on a network you trust. See `README.md`.
- Pairing is toy-grade even when open: a 6-digit PIN cannot resist offline
  brute-force by an attacker already on your LAN.
- The server-sent touch layout is drawn by the **3DS client only**. Android
  receives it through the same shared engine and ignores it; its on-screen
  controls are still laid out by the app.
- PS Vita, PSP, DS/DSi, Switch and desktop clients are designed but not built.
  The Vita toolchain is additionally blocked upstream.

[0.5.0]: https://github.com/atticpad/atticpad/releases/tag/v0.5.0
