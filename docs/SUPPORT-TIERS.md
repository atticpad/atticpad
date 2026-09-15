# Support tiers — what is verified vs built blind

Honest labels: what has actually been run, and on what.

| Platform | Artifact | Tier | Evidence |
|---|---|---|---|
| Linux server | binary | **Hardware-proven** | Continuous use; uinput pad confirmed under evtest; CI integration job |
| Windows server | `.exe` | **Hardware-proven** | ViGEmBus X360 pad on real Windows 11; QR pairing; web UI |
| 3DS client | `.3dsx` + `.cia` | **Hardware-proven** | New 3DS: full self-test pass, sessions against both servers, QR pairing at 16–22 ms/decode, `.cia` installed via FBI. Old 3DS: **never run** |
| Android client | `.apk` | **Hardware-proven** | arm64 handset: full self-test pass, sessions, QR scan, physical gamepad passthrough. Emulator-verified per build |
| Android Bluetooth HID mode | in `.apk` | **Hardware-proven, experimental** | Works against Windows 11 for DirectInput/SDL games; **invisible to XInput-only games**, which is inherent to Bluetooth HID and not a bug we can fix |
| PSP client | `EBOOT.PBP` | **Hardware-proven** | Launches from the XMB with its own tile; self-test 1987/1987; joins a saved network, with L / R to fall back to another slot; sessions against both the Linux and the Windows server; PIN pairing; the server address and network slot remembered across a power cycle. Sessions hold with the console and the server on the same access point and WLAN Power Save off. **Not yet verified on hardware:** the nub's shape and rails, suspend/resume, a long soak, HOLD, and the negative cases (WLAN switch off, no saved network). Emulator-verified per build in PPSSPP |
| Desktop client | — | **Not started** | Designed, but no `clients/desktop/` exists yet |
| PS Vita | — | **Shelved** | Toolchain blocker: the available VitaSDK container's binaries need a newer glibc than the image provides, so the compiler cannot run |
| DS / DSi client | `.nds` | **Hardware-proven (DS and DSi modes)** | DS mode, on an isolated WEP-40 access point (`docs/SETUP-DS.md`): joins the saved Wi-Fi Connection slot, matches `ds-default`, holds sessions at 19–67 ms round trip sending ~35 INPUT_STATE/s; under evtest every face button, both D-pad axes, L, START, SELECT, both stick clicks from server-sent touch regions, the touch surface reaching the stick rails (ABS_X −28334..32767), the KEYS Shift+A chord as one report, Enter, and media volume up/down; profile hot-switch from the web editor applies live; clean BYE; self-test pass. MOUSE mode moves the PC pointer. R not captured under evtest. DSi mode: identifies as "AtticPad DSi" with a battery percentage, joins WPA2, connects, and pairs by scanning the server QR with the camera (token and PIN codes) against both the Linux and the Windows server; the network chosen in the picker is remembered with its key and rejoined at boot. Emulator-verified per build in melonDS (self-test, all four modes) |
| Switch | — | **Not started** | Will start "emulator-verified" at best — and there is no emulator worth building a pipeline on |

Tier definitions: **Hardware-proven** = a person ran this build (or its direct
ancestor) on the physical device, and the report says what was seen.
**Emulator-verified** = self-test and a live session pass under an emulator with
working networking. **Built blind** = compiles in the pinned container; nothing
more is claimed.

A tier moves only when a person has run the thing and said what they saw.
[`QA.md`](QA.md) is the protocol for that: the checks emulators and CI cannot
answer, which is most of what "hardware-proven" actually means here.

Why publish this at all: most of these platforms cannot be tested directly, so
the difference between "it compiled" and "someone watched it work" is real and
worth stating. People forgive an honest label; they do not forgive a broken
promise.
