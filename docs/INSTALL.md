# Installing AtticPad

You need **two** pieces: a **server** on the PC that should receive a virtual
gamepad, and a **client** on the phone or console you want to use as the
controller. Install the server first — the client needs something to find.

Every download comes from the [Releases
page](https://github.com/atticpad/atticpad/releases). Each release ships a
`SHA256SUMS` file; verify with `sha256sum -c SHA256SUMS` (Linux/macOS) or
`certutil -hashfile <file> SHA256` (Windows).

Both machines must be on the same LAN. AtticPad uses **UDP port 21100** and
never talks to the internet.

- [Linux server](#linux-server)
- [Windows server](#windows-server)
- [Nintendo 3DS client](#nintendo-3ds-client)
- [Android client](#android-client)
- [Pairing](#pairing)
- [Troubleshooting](#troubleshooting)

---

## Linux server

Download `atticpad-server-linux-x86_64`, make it executable, and run it:

```bash
chmod +x atticpad-server-linux-x86_64
./atticpad-server-linux-x86_64
```

It prints a local web UI address (default <http://127.0.0.1:21150/>) where you
can watch pad status and round-trip latency, and edit mapping profiles. **The
Linux server does not open a browser for you** — copy that address in yourself.
(The Windows one does; the two genuinely differ.)

### Giving it access to `/dev/uinput`

Creating a virtual gamepad needs write access to `/dev/uinput`, which is
root-only on most distributions. Running the whole server as root works but is
heavier than necessary; the better fix is a udev rule granting a group:

```bash
sudo groupadd -f uinput
sudo usermod -aG uinput "$USER"
printf 'KERNEL=="uinput", GROUP="uinput", MODE="0660", OPTIONS+="static_node=uinput"\n' \
  | sudo tee /etc/udev/rules.d/99-atticpad-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**Log out and back in** for the new group to apply, then confirm:

```bash
test -w /dev/uinput && echo "ready" || echo "still not writable"
```

If the module isn't loaded at all, `sudo modprobe uinput` (add `uinput` to
`/etc/modules-load.d/` to make it stick).

### Notes

- The binary is built on a current Ubuntu, so it needs a reasonably recent
  glibc. On an older distribution, build from source: `./scripts/build.sh server`.
- `--headless` skips the web UI; `--no-mdns` disables mDNS advertisement (useful
  if you run two servers, or multicast is filtered on your network).
- A different first argument sets the UDP port: `./atticpad-server-linux-x86_64 21101`.

---

## Windows server

### 1. Install ViGEmBus first

The Windows server creates XInput gamepads through the **ViGEmBus** driver,
which is *not* bundled — install it before running AtticPad, or no pad appears.

Get the latest signed installer from the [ViGEmBus
releases](https://github.com/nefarius/ViGEmBus/releases) and run it.

> **Heads up:** ViGEmBus was archived by its author in November 2023 and
> receives no further updates. It still works on Windows 11, and it is the only
> practical way to present an XInput device on Windows, but it is a dependency
> outside this project's control. If a future Windows update breaks it, AtticPad's
> Windows server breaks with it.

### 2. Run the server

Download `atticpad-server-windows-x86_64.exe` and run it. It lives in the
system tray and serves the same local web UI on <http://127.0.0.1:21150/>,
**opening it in your browser on startup** — pass `--no-browser` if you would
rather it didn't, and reach it later from the tray icon's *Open*.

Windows SmartScreen will warn that the publisher is unknown, because the
executable is not code-signed (a certificate costs more than this project has).
Choose **More info → Run anyway**, or verify the SHA-256 against `SHA256SUMS`
first if you'd rather check than trust.

Allow it through the Windows Firewall on **private networks** when prompted —
without that, clients cannot reach it.

---

## Nintendo 3DS client

### Requirements

- A 3DS **running Luma3DS custom firmware.** The `.cia` is signed with the
  well-known homebrew test key, and only Luma's signature patches will install
  it. On stock firmware it cannot be installed at all.
- Tested on a **New 3DS**. The Old 3DS is expected to work but has never been
  run — if you try it, a report either way is welcome.

### Install the CIA (recommended)

1. Copy `atticpad-3ds.cia` to your SD card.
2. Open **FBI** → *SD* → browse to the file → **Install and delete** (or
   *Install*, to keep the copy).
3. Launch **AtticPad** from the HOME menu.

FBI can also install over the network: *Remote Install → Receive URLs over the
network*, then send the URL of the `.cia` from your PC.

### Or run the 3DSX

Copy `atticpad-3ds.3dsx` to `/3ds/` on the SD card and launch it from the
Homebrew Launcher. Same application; no installation, and it does not appear on
the HOME menu.

### Self-test

Hold **L + R + Start** while the app launches to run the on-device conformance
self-test. It runs before any networking, so it stays reachable even when
everything else is broken. There is also a visible `SELECT: self-test` prompt.

---

## Android client

- **Android 8.0 (API 26) or newer**, arm64-v8a / armeabi-v7a / x86_64.
- Download `atticpad-android.apk` and open it. Android will ask you to allow
  installing from this source; the toggle is per-app and can be turned back off
  afterwards.

> **Upgrading from a build you compiled yourself?** Uninstall it first. Release
> APKs are signed with a different key than local debug builds, and Android
> refuses to replace an app whose signature changed. Later official releases
> update in place normally.

The app needs no account and requests no network permission beyond LAN access.
Camera permission is only requested when you choose to scan a pairing QR code.

---

## Pairing

**Read this first: pairing is optional, and it is off until you open it.** A
freshly started server accepts any device on your LAN with no PIN at all. That
is convenient on a home network and a bad idea on any other, so decide which
one you are on before you start it. See the security note in the
[README](../README.md).

Pairing also is not remembered between sessions in this release: it
authenticates the connection being made while the window is open, rather than
building a list of trusted devices.

1. On the server, open the web UI and choose **Pair**. It shows a **6-digit
   PIN** and a **QR code**, both valid for 120 seconds.
2. On the client, pick your server from the discovered list — or enter its IP
   address by hand, which always works.
3. Either scan the QR code with the client's camera, or type the PIN.

Five wrong attempts invalidate the PIN and generate a new one.

**Manual IP entry is not a fallback, it is the reliable path.** Many access
points isolate clients from each other, and VPNs carry no multicast at all;
where automatic discovery fails, typing the address still works. The server's
web UI shows the address to type.

---

## Troubleshooting

**The client can't find the server.** Confirm both are on the same network and
subnet, then enter the server's IP by hand. Guest and "AP isolation" networks
block client-to-client traffic entirely — nothing in AtticPad can work around
that. On Windows, check the firewall prompt was accepted for private networks.

**The server starts but no gamepad appears.**
On Linux, the `/dev/uinput` permissions above are almost always the cause. On
Windows, ViGEmBus is almost always the cause — reinstall it and reboot, then
check the pad shows up in `joy.cpl` (Win+R → `joy.cpl`).

**The game ignores the pad.** Check `joy.cpl` (Windows) or `evtest` (Linux)
first: if the pad moves there, the problem is the game's controller settings,
not AtticPad. Games using raw DirectInput sometimes need the pad connected
before launch.

**Input feels laggy.** What counts as normal depends on the client's radio, so
check the round-trip figure the client shows against the right baseline:

| Client | Typical round trip |
|---|---|
| Android or desktop on 5 GHz Wi-Fi | 2–5 ms |
| Nintendo 3DS | 4–18 ms — its Wi-Fi radio is 802.11b/g, and this is normal |

Anything under about 25 ms is within the design target and should feel like a
wired pad. Much more than the figures above usually means Wi-Fi congestion, a
2.4 GHz band that should be 5 GHz, or a VPN in the path — routing a LAN session
through a VPN relay costs roughly 100 ms and no amount of tuning recovers it.

**A control does nothing.** Deadzone, curves and inversion are all server-side:
open the profile editor in the web UI rather than looking for a client setting.

**Nothing works and you want to know whether the client itself is sane.** Run
the on-device self-test (hold L+R+Start at launch). It exercises the protocol
implementation with no network involved: a pass means the problem is
configuration or the network, not the client.
