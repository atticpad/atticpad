# AtticPad

**Bring the hardware down from the attic and use it as a game controller.**

Phones, handhelds, and old consoles become gamepads for a PC. A *server* runs on
the machine that needs a controller and creates virtual gamepads; *clients* run
on whatever hardware you already own and send input over your LAN.

No cloud, no account, no internet. The server binds LAN interfaces and never
routes.

## What works today

| | Status |
|---|---|
| **Nintendo 3DS client** | Tested on real hardware (New 3DS). Buttons, circle pad, C-stick, gyro, touch, battery. `.cia` and `.3dsx`. |
| **Android client** | Tested on real hardware. On-screen controls, physical gamepad passthrough, gyro, QR pairing. Android 8.0+. |
| **Linux server** | Daily-driven. Creates virtual pads through `uinput`; enumerates as an Xbox 360 controller. |
| **Windows server** | Tested on Windows 11. Creates XInput pads through ViGEmBus. Tray app and local web UI. |

Ports for PS Vita, PSP, DS/DSi, Switch and a desktop client are designed for but
not built — see [`docs/SUPPORT-TIERS.md`](docs/SUPPORT-TIERS.md) for the honest
per-platform status, including what has *never been run*.

## What it looks like

The 3DS client mid-session, both screens, captured on the console itself:

![AtticPad on a Nintendo 3DS. The top screen shows a round-trip time of 16 ms in large type, and a live panel of every button, stick and touch value the console is sending. The bottom screen is split into two large regions labelled LT and RT — "analog: slide down to squeeze" — with disconnect and self-test buttons beneath them.](docs/img/3ds-session.png)

The round-trip figure is live, and the panel beneath it mirrors exactly what the
server is being told — usually enough to tell "the console isn't sending" apart
from "the game isn't listening" without any other tooling. The touchscreen
becomes the analog triggers the 3DS hardware doesn't have.

## Install

See [`docs/INSTALL.md`](docs/INSTALL.md). In short:

- **3DS** — requires Luma3DS custom firmware; install the `.cia` with FBI.
- **Android** — sideload the `.apk`.
- **Linux server** — one binary; needs access to `/dev/uinput`.
- **Windows server** — one `.exe`; needs the ViGEmBus driver installed.

Then pair the client to the server once, with a 6-digit PIN or by scanning a QR
code, and it reconnects on its own after that.

## Security, stated plainly

**By default the server accepts any device on your LAN, with no PIN.** Read
that again, because it is the single most important thing to know before you
run it: authentication applies only while you have explicitly opened a
120-second pairing window. Outside that window a client connects with no
secret, and a connected client drives a virtual gamepad on your machine.

When a pairing window *is* open, the client must present a 6-digit PIN (five
attempts, then the PIN is invalidated), and every packet after the handshake
carries a truncated HMAC-SHA256 tag. But **pairing is not remembered between
sessions** — there is no persistent list of trusted devices in this release, so
pairing protects the moment of connection, not the server in general.

Even with pairing open, the threat model is toy-grade: it stops a housemate,
not an adversary. An attacker already on your LAN who captures the handshake
can brute-force a 6-digit PIN offline and recover the session key. On the open
or WEP-only networks the DS requires, that attacker is anyone in range.

**Run AtticPad only on a network you trust**, and treat it as you would any
device that can move your mouse. The reasoning, and the reserved field that
fixes the PIN weakness later without a wire break, are in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md) §10.

## The protocol

AtticPad speaks a small, frozen, byte-exact UDP protocol.
[`docs/PROTOCOL.md`](docs/PROTOCOL.md) is normative and **v1 is frozen** — every
constant, offset and bit position is fixed, and changes go to a v2.

It is documented well enough to write a client against without reading the C:
239 golden conformance vectors are generated from the spec alone, and every
client ships them as an on-device self-test (hold **L+R+Start** at launch).
That matters because most target platforms cannot be tested directly.

Writing a new client? Start with [`docs/PORTING.md`](docs/PORTING.md); the
shared engine in `clients/common/` does the protocol work for you.

## Build

```bash
./scripts/build.sh core      # the protocol core + its self-test, ~2 s
./scripts/build.sh server    # the Linux server
./scripts/build.sh 3ds       # or: android, windows, tools
```

Console targets build in the same pinned container digests CI uses, so a local
pass means CI passes. The whole protocol exercises on one Linux box: build the
core, run the server against a real `uinput` device, connect
`tools/loopback-client`, and watch `evtest` show the virtual pad move.

Release binaries come from tagged CI only — never from a local build.

## Design

[`docs/DESIGN.md`](docs/DESIGN.md) is the full technical plan: why the core is
C99 with no `malloc` after init, why the mapping engine lives in the server, why
Windows depends on an archived driver, and which constraints are hard.
[`docs/CONVENTIONS.md`](docs/CONVENTIONS.md) is the short version for anyone
touching the code.

## License

MIT — see [`LICENSE`](LICENSE). Permissive on purpose: console homebrew gets
redistributed through community app stores, and a copyleft license makes that
harder than it needs to be.

Vendored third-party components keep their own licenses:
[quirc](clients/vendor/quirc) (QR decoding),
[qrcodegen](server/vendor/qrcodegen) (QR generation), and the
[ViGEm client](server/backends/vendor/vigemclient).

## A note on how this was built

AtticPad was written almost entirely by AI agents, with a human directing the
work, reviewing it, and running everything that touched real hardware. The
hardware results quoted here and in `docs/SUPPORT-TIERS.md` are real: they were
produced by running the software on the devices named, not inferred.

Where something has *not* been verified, the documentation says so rather than
rounding up. On a project where most target platforms cannot be tested
directly, that convention is load-bearing.
