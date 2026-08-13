# Support tiers — what is verified vs built blind

Honest labels: what has actually been run, and on what.

| Platform | Artifact | Tier | Evidence |
|---|---|---|---|
| Linux server | binary | **Hardware-proven** | Continuous use; uinput pad confirmed under evtest; CI integration job |
| Windows server | `.exe` | **Hardware-proven** | ViGEmBus X360 pad on real Windows 11; QR pairing; web UI |
| 3DS client | `.3dsx` + `.cia` | **Hardware-proven** | New 3DS: full self-test pass, sessions against both servers, QR pairing at 16–22 ms/decode, `.cia` installed via FBI. Old 3DS: **never run** |
| Android client | `.apk` | **Hardware-proven** | arm64 handset: full self-test pass, sessions, QR scan, physical gamepad passthrough. Emulator-verified per build |
| Android Bluetooth HID mode | in `.apk` | **Hardware-proven, experimental** | Works against Windows 11 for DirectInput/SDL games; **invisible to XInput-only games**, which is inherent to Bluetooth HID and not a bug we can fix |
| Desktop client | — | **Not started** | Designed, but no `clients/desktop/` exists yet |
| PS Vita | — | **Shelved** | Toolchain blocker: the available VitaSDK container's binaries need a newer glibc than the image provides, so the compiler cannot run |
| PSP / Switch / DS | — | **Not started** | Planned in that order; will start "emulator-verified" at best |

Tier definitions: **Hardware-proven** = a person ran this build (or its direct
ancestor) on the physical device, and the report says what was seen.
**Emulator-verified** = self-test and a live session pass under an emulator with
working networking. **Built blind** = compiles in the pinned container; nothing
more is claimed.

Why publish this at all: most of these platforms cannot be tested directly, so
the difference between "it compiled" and "someone watched it work" is real and
worth stating. People forgive an honest label; they do not forgive a broken
promise.
