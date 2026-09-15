
---

## Install

**Server** — on the PC that needs a controller. Pick one:

- **Windows:** download `atticpad-server-windows-x86_64.exe` and run it. It needs
  the ViGEmBus driver, which is easiest to install with winget:

  ```
  winget install ViGEm.ViGEmBus
  ```

  or grab the installer from
  [its releases page](https://github.com/nefarius/ViGEmBus/releases). SmartScreen
  will warn about AtticPad's unsigned binary (*More info → Run anyway*).
- **Linux:** download `atticpad-server-linux-x86_64`, `chmod +x` it, and run it.
  It needs access to `/dev/uinput` — [INSTALL.md](@DOCS@/INSTALL.md) has the
  one-line udev rule.

The server prints the address to type into a client, and serves a local page on
<http://127.0.0.1:21150/> for pad status, round-trip latency and profile editing.

**Client** — on the device you want to hold:

- **Nintendo 3DS** (needs Luma3DS custom firmware) — open **FBI → Remote Install
  → Scan QR Code** and scan this. No SD card, no cable:

  <img src="@RAW@/docs/img/fbi-install-qr.png" alt="QR code that installs the AtticPad .cia from the latest release" width="200">

  If that image does not load, the same code is attached to this release as
  `atticpad-3ds-install-qr.png`, and FBI's *Receive URLs over the network* will
  take the URL it encodes directly:
  `https://github.com/atticpad/atticpad/releases/latest/download/atticpad-3ds.cia`

  Or copy `atticpad-3ds.cia` to the SD card and install it with FBI from there.
  `atticpad-3ds.3dsx` runs from the Homebrew Launcher instead, without installing.
- **Android 8.0+** — sideload `atticpad-android.apk`. If you already have a debug
  build installed, uninstall it first: the signatures differ.
- **PSP** (custom firmware) — unzip `atticpad-psp.zip` onto the root of the
  memory stick and launch **AtticPad** from the XMB. Set up a saved Wi-Fi
  connection first; the PSP speaks WEP and WPA over 802.11b, so a WPA2-only
  router will refuse it.
- **Nintendo DS / DSi** (flashcart or homebrew launcher) — copy
  `atticpad-nds.nds` to the card and launch it. In DS mode the console can only
  join an open or WEP network; [SETUP-DS.md](@DOCS@/SETUP-DS.md) shows how to
  run a small isolated one. In DSi mode it joins WPA2 and pairs by scanning
  the QR with the camera.

Then pair once. On the 3DS that means scanning the QR code the server shows —
the console has no PIN keypad, so the QR is the only way in. On Android you can
scan it or type the 6-digit PIN.

**Before you use it on a network you do not control, read the security section
of the README.** By default the server accepts any device on your LAN with no
PIN.

Verify what you downloaded:

```
sha256sum -c SHA256SUMS
```
