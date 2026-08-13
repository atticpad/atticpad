# Changelog

All notable changes to AtticPad are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses `0.<milestone>.<patch>` versioning until a non-technical user can install
it and play a game, which is what 1.0 will mean.

The **product version** and the **protocol version** move independently. The
wire format is AtticPad protocol **v1, frozen** — see `docs/PROTOCOL.md`.

## [0.4.0] — unreleased

First public release. Everything below was built before this release and is
listed once here rather than backdated into releases that were never published.

### Added

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
- PS Vita, PSP, DS/DSi, Switch and desktop clients are designed but not built.
  The Vita toolchain is additionally blocked upstream.

[0.4.0]: https://github.com/atticpad/atticpad/releases/tag/v0.4.0
