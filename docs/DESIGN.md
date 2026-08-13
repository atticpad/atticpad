# AtticPad — design and rationale

**Bring the hardware down from the attic and use it as a game controller.**

Phones, handhelds, and old consoles become gamepads for a PC.

A *server* runs on the host machine that needs a controller and creates virtual gamepads.
*Clients* run on whatever hardware you happen to own and send input over the LAN.

- **What actually shipped:** [`CHANGELOG.md`](../CHANGELOG.md) — release by release
- **Status:** see `CHANGELOG.md` — that file is the record. Wire format **frozen 2026-08-09**.
- **Protocol version:** v1 — **frozen**. See `docs/PROTOCOL.md`; changes now go to v2.
- **Primary language:** C99 (core), platform-native at the edges
- **License:** MIT (permissive — matters for console homebrew redistribution)

---

## Table of contents

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [Hard constraints](#2-hard-constraints)
3. [Architecture](#3-architecture)
4. [Repository layout](#4-repository-layout)
5. [Protocol specification (AtticPad protocol v1)](#5-protocol-specification-atticpad-protocol-v1)
6. [Server design](#6-server-design)
7. [Client matrix](#7-client-matrix)
8. [Continuous integration](#8-continuous-integration)
9. [Testing strategy](#9-testing-strategy)
11. [Milestones](#11-milestones)
13. [Decisions](#13-decisions)

---

## 1. Goals and non-goals

### Goals

- Turn any supported device into a gamepad for a host PC, over LAN, with latency low enough for real play (target: under one frame at 60 Hz).
- Support many client platforms without maintaining many protocols.
- Every *release* artifact for every platform built by GitHub Actions. **No developer machine should ever be required to produce a release** — but building locally is the normal development loop, and the two must stay equivalent ([§8.5](#85-the-local-development-loop)).
- Work on hardware that cannot be tested directly, verified by on-device self-tests rather than faith.
- Multiple simultaneous clients, each mapped to its own virtual pad.

### Non-goals

- **Not** a remote-play / video-streaming system. Input only. (Pair it with Sunshine/Moonlight if you want video.)
- **Not** internet-facing. LAN only. Anything else is unsupported and undocumented.
- **Not** a general input-remapping tool for locally-attached devices.
- **Not** attempting to defeat anti-cheat. The server creates a virtual pad; whether a given game accepts one is the game's business.

### Success criteria

- p99 end-to-end latency under 25 ms on a quiet 5 GHz network.
- A new client platform can be added in under 500 lines of platform-specific code.
- A tagged release produces artifacts for every supported platform with zero manual steps.

---

## 2. Hard constraints

These were verified before planning and they shape most decisions in this document. Do not re-litigate them without new evidence.

### 2.1 Windows: ViGEmBus is feature-frozen

ViGEmBus was retired and archived in November 2023 following a trademark conflict. The driver still functions and remains what DS4Windows and Sunshine depend on, but it will receive no further updates. A successor (VirtualPad) has been announced but is not shipping.

**Re-checked 2026-08-10, at the start of M4, because this belief was years old.** It held, and got worse in one respect and better in another:

- **No maintained fork exists.** `nefarius/ViGEmBus` has had zero commits since it was archived on 2023-11-02 (final release `v1.22.0`, titled *"It's dead, Jim."*). LizardByte — Sunshine's maintainers, who depend on it — forked it to keep it alive and then **archived the fork too, on 2025-08-21**, pointing back at the upstream end-of-life notice. Sunshine still hard-requires it and fails fatally without it.
- **There is no successor that produces an XInput device.** LizardByte's `libvirtualhid` (started 2026-06-18) is the modern approach — user-mode UMDF2, no kernel driver, so no BSOD and no HVCI exposure — but it *explicitly declines* to emulate the Xbox 360 profile, on the correct grounds that a real 360 pad is an XUSB device rather than a VHF HID gamepad. It reaches DirectInput and `Windows.Gaming.Input` but does not guarantee an XInput slot. **A fallback to it would silently produce no input in any game that only polls XInput**, which is a large fraction of the catalogue.
- **But there is no permission gate here.** The shipped binaries are production-signed; a user installs them with `winget install --id ViGEm.ViGEmBus` or a silent MSI. Driver signature enforcement on x64 has been independent of Secure Boot since Vista SP1, so nothing needs disabling, and the HVCI/Memory-Integrity bugchecks were fixed in driver 1.17.x. Test Mode is required only to build the driver from source, not to install it.

**Consequence:** the virtual-pad backend must sit behind an interface from day one. Assume ViGEmBus may break on a future Windows release. That abstraction is now doing more work than when this was written: the risk is *vendor abandonment*, not permission denial, and the only escape hatch available if Windows does break it is a HID-only device that XInput-only games cannot see. That is a reason to keep `apad_backend` honest, not a reason to stop.

**Observed on real hardware** (Windows 11 Pro, build 10.0.26200.8973, AMD64), which settles the one thing documentation alone could only reason about:

| | |
|---|---|
| ViGEmBus | `Nefarius Virtual Gamepad Emulation Bus`, driver **1.21.442.0**, dated 2022-08-30, present as a PnP device |
| Secure Boot | **enabled** |
| HVCI / Memory Integrity | **running** |

The driver loads and enumerates with **both Secure Boot and Memory Integrity on**, on a current Windows 11 build. The research could only say the HVCI bugchecks were fixed in 1.17.x and no fresh reports existed; this is a direct observation on real hardware, in a *stronger* configuration than the minimum — nothing had to be disabled or relaxed.

Note the version: **1.21.442.0, not the final 1.22.0.** `winget` reports no upgrade available, so either its manifest resolves to 1.21 or the driver arrived bundled with something else. Worth pinning down before drawing conclusions about which build a user will have, since 1.22.0 is the last one that will ever exist.

**Still untested:** whether ViGEmBus installs inside a KVM/QEMU Windows guest, and whether a GitHub-hosted `windows-latest` runner can install it. `libvirtualhid`'s own CI proves a comparable virtual-HID driver can be `msiexec`-installed and exercised on a stock `windows-2022` runner, which is a pattern worth mirroring — but it is not evidence about this driver.

**The Windows host is cross-compiled from Linux** with `mingw-w64` rather than built natively, so a Windows box only ever *runs* the binary and never needs a C toolchain installed. That keeps the build reproducible on the same machine that builds everything else, and it means Windows support costs one cross-compiler rather than a second build environment.

**Secondary gotcha:** HP Omen laptops ship a forked 2018 ViGEmBus. Apps probing for the driver can find the fork instead of the real one. Detect and warn.

### 2.4 Nintendo DS: WEP or open networks only

DSWiFi supports open and WEP networks in DS mode; WPA2 is available only in DSi mode. This is a hardware limitation of the original DS and DS Lite — no software fixes it.

**Consequence:** DS support requires the user to run an open or WEP access point. Because link-layer security is therefore absent, **the protocol must carry its own authentication** (see [§5.10](#510-security)). Recommend an isolated AP or a dedicated router with no route to the rest of the network.

---

## 3. Architecture

The central decision: **one C99 core, linked identically into every target.** Codec, session state machine, and conformance self-test live in `libapad`, which has no OS dependencies. Platform shims provide only sockets, monotonic time, and input polling.

Without this, seven clients means seven protocols that silently drift apart. With it, adding a platform is writing a poll function and a socket shim.

```
+----------------------------------------------------------+
|  Server backends (platform-specific)                     |
|  Linux/uinput          Windows/ViGEmBus                   |
+----------------------------------------------------------+
|  Server-only logic: mapping engine, profiles, discovery   |
+----------------------------------------------------------+
                          |
+----------------------------------------------------------+
|  libapad -- shared C99 core                              |
|  codec  |  session FSM  |  HMAC  |  conformance self-test |
+----------------------------------------------------------+
                          |
+----------------------------------------------------------+
|  Platform shim: sockets, monotonic time, input polling    |
+----------------------------------------------------------+
                          |
+----------------------------------------------------------+
|  Clients (platform-specific)                             |
|  3DS | Android | Windows | Vita | PSP | DS/DSi | Switch    |
+----------------------------------------------------------+
```

**The server is a library with a thin host** — see
[§6.4](#64-the-server-as-a-library). Windows is the second host and Android
has been proposed as a third, so `libapadserver` owns sessions, mapping and
backend dispatch while the host owns the socket loop, threading and UI.

**The mapping engine lives in the server, not the core.** An earlier draft placed it in `libapad`. It is server-only code — no client ever evaluates a mapping profile — so putting it in the core would subject it to the DS's no-malloc, no-float, no-stdio constraints for no benefit, and would grow the binary on every handheld. Core stays minimal: codec, session FSM, HMAC, self-test.

### Core constraints (`libapad`)

Enforced by CI, not by good intentions:

- **C99**, no compiler extensions.
- **No `malloc` after init.** Caller-provided buffers or static arenas. The DS has 4 MB and no MMU.
- **No floating point.** Fixed-point where needed. Some targets have no FPU.
- **No unaligned access.** The ARM9 in the DS *silently rotates* on unaligned word loads instead of faulting, producing bugs that look like data corruption rather than crashes. All wire field access goes through `memcpy`-based helpers.
- **No stdio, no locale, no `time.h`.** Everything comes in through the shim.
- **Endianness explicit.** Little-endian on the wire; every target CPU is LE (ARM, MIPS on PSP, x86), but the codec must not *assume* it — helper macros assemble bytes one at a time.
- **Never cast a struct pointer onto a packet buffer.** This is the single rule that, if broken, produces bugs that appear only on ARM9 and only sometimes.

### Shim interface

Every platform provides this and nothing more:

```c
typedef struct apad_sock apad_sock;

int        apad_net_init(void);
apad_sock *apad_udp_open(uint16_t local_port);
int        apad_udp_send(apad_sock *, const apad_addr *, const void *, size_t);
int        apad_udp_recv(apad_sock *, apad_addr *from, void *, size_t, int timeout_ms);
int        apad_udp_set_broadcast(apad_sock *, int enable);
void       apad_udp_close(apad_sock *);
uint32_t   apad_ticks_ms(void);   /* monotonic, wraps at 2^32 (~49.7 days) */
```

Every target has a BSD-sockets-shaped API available: `dswifi` (DS), `soc:U` (3DS), `pspnet` (PSP), VitaSDK net, `libnx` bsd, Winsock, Android NDK. That is the load-bearing assumption of this design and it holds on all eight platforms.

---

## 4. Repository layout

```
atticpad/
├── core/                     # libapad — the shared C99 core
│   ├── include/atticpad/
│   │   ├── atticpad.h            # public API
│   │   ├── protocol.h        # wire constants, message types, button bits
│   │   └── input.h           # canonical input state struct
│   ├── src/
│   │   ├── codec.c           # encode/decode — the fuzz target
│   │   ├── session.c         # handshake + session state machine
│   │   ├── seq.c             # wrap-safe sequence/tick arithmetic
│   │   ├── hmac_sha256.c     # vendored, constant-time compare
│   │   └── selftest.c        # runs the conformance vectors
│   └── testdata/
│       ├── vectors.h         # golden packets — generated, committed
│       └── generate.py       # regenerates vectors.h from PROTOCOL.md
│
├── shim/
│   ├── net_bsd.c             # 3DS, PSP, Vita, Switch, Linux, macOS, Android
│   ├── net_win.c             # Winsock
│   ├── net_nds.c             # dswifi quirks
│   └── time_*.c
│
├── server/
│   ├── src/
│   │   ├── main.c
│   │   ├── mapping.c         # input state -> virtual pad state
│   │   ├── profiles.c        # JSONC profile loading (server-only, may malloc)
│   │   ├── mdns.c            # vendored advertise-only responder
│   │   ├── discovery.c       # broadcast responder + mDNS glue
│   │   └── pairing.c         # PIN, key derivation, attempt limiting
│   ├── profiles/             # shipped default profiles, commented JSONC
│   └── backends/
│       ├── backend.h         # the interface every backend implements
│       ├── uinput.c          # Linux
│       ├── vigem.c           # Windows
│
├── clients/
│   ├── 3ds/                  # devkitARM + libctru  -> .3dsx and .cia
│   ├── android/              # Kotlin + NDK          -> .apk
│   ├── desktop/              # SDL3                  -> .exe / ELF
│   ├── vita/                 # VitaSDK               -> .vpk
│   ├── psp/                  # PSPDEV                -> EBOOT.PBP
│   ├── nds/                  # BlocksDS              -> .nds
│   ├── switch/               # devkitA64 + libnx     -> .nro
│
├── references/               # vendored SDK samples — see §10
│   ├── psp/  vita/  switch/  nds/  3ds/
│   └── README.md             # provenance and licence of each sample
│
├── tools/
│   ├── latency-bench/        # true end-to-end p50/p95/p99
│   ├── packet-replay/        # replay captured sessions against the server
│   ├── fuzz/                 # libFuzzer harness for codec.c
│   └── loopback-client/      # headless client for CI integration tests
│
├── docs/
│   ├── PROTOCOL.md           # the normative spec — source of truth
│   ├── PORTING.md            # how to add a client platform
│   ├── SETUP-DS.md           # the WEP access point situation
│   └── SUPPORT-TIERS.md      # what is verified vs built blind
│
├── scripts/
│
└── .github/workflows/
    ├── ci.yml                # core tests + all builds on every PR
    ├── release.yml           # tag -> artifacts -> GitHub release
    └── canary.yml            # weekly build against unpinned toolchains
```

---

## 5. Protocol specification (AtticPad protocol v1)

> Constants and symbols use the `apad_` / `APAD_` prefix. The normative version lives in `docs/PROTOCOL.md`. This section is the design rationale plus the essential layout.

### 5.1 Transport

**UDP only, one socket per client. Default port 21100.**

TCP is the wrong choice here and it is worth being explicit about why: head-of-line blocking means one lost packet stalls every input frame behind it. Input state is *idempotent and self-healing* — the next packet 8–16 ms later carries the complete current truth, so a dropped packet costs you nothing but a dropped retransmission costs you a visible stall.

Control messages that genuinely need delivery get a small reliability layer on the same socket ([§5.8](#58-reliability-and-wrap-safe-arithmetic)).

**On the port choice.** 21100 satisfies three constraints:

- **Above 1024**, so the server never needs elevated privileges or `CAP_NET_BIND_SERVICE`.
- **Below 32768**, which keeps it outside the Linux default ephemeral range (32768–60999). A server bound inside that range can collide with an outgoing connection's source port — an intermittent "address already in use" that appears only under load and is miserable to diagnose.
- **Not in a cluster this project's users are likely to be running.** Explicitly avoided: 24800 (Deskflow/Synergy — a directly adjacent input-sharing tool), 27000–27050 (Steam/Source), 47984–48010 (Sunshine, which many users will pair this with), 5353 (mDNS).

Discovery uses the same port. Configurable, but the default should not need changing.

### 5.2 Wire rules

Chosen for the weakest target (DS, 67 MHz ARM9, no MMU):

- Little-endian throughout, assembled byte by byte.
- **Fixed offsets, naturally aligned, `memcpy` access.** No varints, no TLV parse loops, no compression.
- Maximum datagram 256 bytes — comfortably under any MTU, no fragmentation ever.
- Every packet independently parseable. No cross-packet state in the codec.
- Reserved bits and reserved bytes **must be zero on send** and **must be ignored on receive**.

### 5.3 Packet header (12 bytes, fixed)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `magic` | `0x4D43` ("MC") |
| 2 | 1 | `version` | major; mismatch = hard reject |
| 3 | 1 | `type` | see table below |
| 4 | 2 | `session_id` | 0 during discovery/handshake |
| 6 | 2 | `sequence` | wraps; per-direction |
| 8 | 2 | `payload_len` | |
| 10 | 2 | `flags` | bit 0 = authenticated, bit 1 = reliable, bits 2–15 reserved |

Payload follows. If bit 0 is set, an 8-byte truncated HMAC-SHA256 tag is appended covering the entire datagram (header + payload, with the tag region zeroed during computation).

### 5.4 Message types

| Code | Name | Direction | Reliable |
|---|---|---|---|
| `0x01` | `DISCOVER` | client → broadcast | no |
| `0x02` | `ANNOUNCE` | server → client | no |
| `0x10` | `HELLO` | client → server | yes |
| `0x11` | `WELCOME` | server → client | yes |
| `0x12` | `BYE` | either | yes |
| `0x20` | `INPUT_STATE` | client → server | **no** |
| `0x30` | `PING` | either | no |
| `0x31` | `PONG` | either | no |
| `0x40` | `RUMBLE` | server → client | yes |
| `0x41` | `LED` | server → client | yes |
| `0x42` | `STATUS` | server → client | yes |
| `0x50` | `ACK` | either | no |
| `0x51` | `ERROR` | either | no |

### 5.5 Discovery (three-tier)

The part most directly shaped by external constraints. **The server implements all three; each client implements whichever it can.**

**Tier 1 — mDNS / Bonjour.** Server advertises `_atticpad._udp.local` with TXT records for name, version, and free pad slots. Used by Android and desktop clients, and by any platform whose SDK offers service browsing.

**Tier 2 — raw UDP broadcast.** The `DISCOVER`/`ANNOUNCE` exchange on `255.255.255.255:21100`. Used by 3DS, DS, PSP, Vita, Switch — platforms where pulling in an mDNS responder is not worth the code size and where broadcast is unrestricted.

**Tier 3 — manual IP entry.** A **first-class feature on every client**, not a debug fallback. It is the only method that works where a platform restricts multicast or broadcast outright, and it rescues users behind AP isolation, on guest networks, or across subnets. The 3DS client needs a usable IP-entry keyboard screen; the DS client needs a touchscreen numpad.

The server displays its own IP prominently at all times, precisely because tier 3 gets used more than you would expect.

**Proposed: a QR code on the server, for clients with a camera.** Not
implemented, and worth doing properly rather than quickly.

The server already has to display its IP at all times, and during pairing it
also displays a 6-digit PIN ([§5.10](#510-security)). A QR encoding
`atticpad://<ip>:<port>?pin=<pin>` collapses discovery *and* pairing into one
scan — no typing an IP on a touchscreen numpad, no reading six digits across a
room, no transcription errors. Both platforms with hardware have cameras: the
3DS (devkitPro ships `camera/image` and `camera/video` samples) and Android.

Three reasons it is more than convenience:

- **It rescues exactly the case tier 3 exists for.** On the development LAN,
  AP client isolation kills tier-2 broadcast entirely, so manual IP is the only
  path that works. Typing `192.168.0.118` on a DS numpad is the worst UX in the
  project; pointing a camera at a screen is the best.
- **It removes the PIN's worst property.** A 6-digit PIN is weak
  ([§5.10](#510-security) is explicit that it stops a housemate, not an
  adversary) and its entropy cannot be raised without making entry painful. A
  scanned secret has no such ceiling — the same QR can carry a long random
  token instead.

  > **Correction, 2026-08-10.** This bullet used to say the token was "precisely
  > what the reserved 32-byte `key_material` field in `WELCOME` was set aside
  > for." That is wrong twice over and would have led someone into a v2 wire
  > break. `key_material` **MUST be zero in v1** ([`docs/PROTOCOL.md` §6.4]) and
  > the format froze 2026-08-09 — and more fundamentally, `key_material`
  > travels *server → client on the wire*, whereas the pairing secret must
  > reach a **human** out of band and never appear on the wire at all (§10).
  > The QR is the out-of-band channel; that is the entire point of it. The
  > token needs no wire field, which is why this costs no protocol change.
  > `key_material` remains reserved for the ChaCha20 key-wrapping upgrade
  > described in §5.10, which is a genuine v2 feature.
- **It needs no protocol change.** The wire format is frozen; this is a
  discovery and pairing *transport*, not a wire concern. A QR is a way to move
  bytes the client already knows how to use.

Scope note: this spans the server UI and every camera-equipped client, so it is
not part of the 3DS UI pass alone. Sequence it after M3, when Android exists and
there are two camera clients to justify the shared design.


**mDNS is vendored, advertise-only.** The server ships a minimal responder (~500 lines) rather than depending on a system daemon. Rationale: Bonjour on Windows requires installing Apple's Bonjour service, and asking users to install that *plus* ViGEmBus is two service installs before the app works once. Advertise-only is far simpler than a full mDNS stack — respond to PTR/SRV/TXT/A queries for one service name, plus unsolicited announcements on start and on IP change.

Known failure mode: Avahi or mDNSResponder may already hold UDP 5353. The responder binds with `SO_REUSEADDR` and `SO_REUSEPORT`, which permits shared multicast reception on Linux and macOS. **If the bind fails, tier 1 is disabled, the UI says so, and tiers 2 and 3 carry the load.** This is acceptable precisely because tier 3 always works — mDNS is a convenience, never a dependency.

### 5.6 Handshake

```
Client                                    Server
  |                                          |
  |-- DISCOVER (broadcast) ----------------->|  (or mDNS, or skipped for manual IP)
  |<------------------------ ANNOUNCE -------|  name, version, slots free
  |                                          |
  |-- HELLO -------------------------------->|  caps, device name, proto version
  |                                          |  [server shows PIN if unpaired]
  |<------------------------- WELCOME -------|  session_id, pad slot, rate, key_material
  |                                          |
  |-- INPUT_STATE (60-125 Hz) -------------->|
  |-- PING (1 Hz) -------------------------->|
  |<---------------------------- PONG -------|
  |<-------------------------- RUMBLE -------|  when the game rumbles
```

`HELLO` carries a capability bitmask so the server knows what the device physically has:

```c
#define APAD_CAP_DPAD        (1u << 0)
#define APAD_CAP_FACE4       (1u << 1)
#define APAD_CAP_SHOULDER    (1u << 2)   /* L, R */
#define APAD_CAP_SHOULDER2   (1u << 3)   /* ZL, ZR as digital buttons */
#define APAD_CAP_TRIGGERS    (1u << 4)   /* analog L2/R2 in axes[4], axes[5] */
#define APAD_CAP_STICK_L     (1u << 5)
#define APAD_CAP_STICK_R     (1u << 6)
#define APAD_CAP_TOUCH       (1u << 7)
#define APAD_CAP_TOUCH_REAR  (1u << 8)   /* Vita */
#define APAD_CAP_ACCEL       (1u << 9)
#define APAD_CAP_GYRO        (1u << 10)
#define APAD_CAP_RUMBLE      (1u << 11)  /* can receive feedback */
#define APAD_CAP_LED         (1u << 12)
#define APAD_CAP_BATTERY     (1u << 13)
```

**Trigger disambiguation rule.** If the client advertises `APAD_CAP_TRIGGERS`, the server drives LT/RT from `axes[4]`/`axes[5]` and ignores the `ZL`/`ZR` bits. If it does not, `ZL`/`ZR` digital presses drive LT/RT to full deflection. Clients with analog triggers should still set the digital bits past a threshold, for profiles that want them as buttons.

### 5.7 Input state payload

A **fixed superset struct**, not a variable schema. The client zero-fills what it does not have; the server ignores what the capability mask says is not real. This costs a few bytes per packet and saves an enormous amount of parsing complexity on constrained targets.

```c
typedef struct {
    uint32_t buttons;        /* canonical bitmask, see below           */
    int16_t  axes[8];        /* LX LY RX RY L2 R2 + 2 reserved         */
                             /* sticks:   -32768..32767, centered 0    */
                             /* triggers: 0..32767                     */
    uint8_t  touch_count;    /* 0..2                                   */
    uint8_t  reserved0;
    struct {
        uint8_t  id;
        uint8_t  pressure;   /* 0 = unknown / binary touch             */
        int16_t  x, y;       /* normalized -32768..32767               */
    } touches[2];
    int16_t  accel[3];       /* milli-g                                */
    int16_t  gyro[3];        /* deci-degrees per second                */
    uint8_t  battery;        /* 0..100, 255 = unknown                  */
    uint8_t  reserved1[3];
    uint32_t client_ticks_ms;/* client monotonic clock at sample time  */
} apad_input_state;          /* 56 bytes — see docs/PROTOCOL.md §5 */
```

#### Canonical button bitmask — frozen at v1, never renumber

```
 0  A                 8  L                16  HOME
 1  B                 9  R                17  TOUCH_PRESS
 2  X                10  ZL               18  TOUCH_REAR_PRESS
 3  Y                11  ZR               19  CAPTURE
 4  DPAD_UP          12  L3               20..31  reserved (must be zero)
 5  DPAD_DOWN        13  R3
 6  DPAD_LEFT        14  START
 7  DPAD_RIGHT       15  SELECT
```

**The D-pad occupies bits 4–7 contiguously and deliberately.** Extract the whole thing as `(buttons >> 4) & 0xF` and convert to a HID hat-switch value with a 16-entry lookup — no branching, and every impossible combination (up+down, left+right) resolves to something sane instead of undefined behaviour:

```c
/* index: bit0=UP bit1=DOWN bit2=LEFT bit3=RIGHT
   value: HID hat 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=null */
static const uint8_t apad_hat_lut[16] = {
    8, 0, 4, 8, 6, 7, 5, 6,
    2, 1, 3, 2, 8, 0, 4, 8
};
```

On the DS and PSP the D-pad is the primary input, so the fast path matters more than it looks.

Naming follows the Nintendo convention (A on the right) because most clients are Nintendo handhelds. The **server** translates to Xbox convention for ViGEmBus. Per-profile A/B and X/Y swapping is a mapping concern, not a protocol concern.

#### Axis and coordinate conventions

Ambiguity here produces inverted-axis bug reports from every blind platform, so it is stated once and normalized in the shim:

- **Sticks: `+Y` is up.** Matches XInput. The 3DS `circlePosition` already uses this; PSP and Vita report `+Y` down and **the client normalizes before filling the struct**. Never pass raw platform values through.
- **Touch: `+Y` is down**, origin top-left. Screen space, matching every touch API on every platform.
- Sticks are vector space, touch is screen space. The mismatch is intentional and both are correct in their own domain.
- Deadzone is **not** applied client-side. Send raw normalized values; the server profile owns deadzone, curve, and inversion. A client that pre-applies deadzone destroys information the server cannot recover.

### 5.8 Reliability and wrap-safe arithmetic

- Reliable messages retransmit at 100 ms, 200 ms, 400 ms, 800 ms, then fail the session.
- `ACK` echoes the sequence number being acknowledged.
- `INPUT_STATE` **never retransmits**.
- The receiver keeps a sliding sequence window and **discards any input packet older than the newest one seen**. A late packet is worse than no packet — applying it moves the stick backwards, which the user experiences as a stutter.

**Sequence numbers wrap and the comparison must handle it.** A 16-bit sequence at 125 Hz wraps every ~8.7 minutes. A naive `if (seq > last_seq)` freezes the client at every wrap, and it will pass every test that runs for under nine minutes. Use serial-number arithmetic (RFC 1982 style):

```c
/* true if a is newer than b, correct across wrap */
static inline int apad_seq_newer(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}
```

The same applies to `apad_ticks_ms`, which wraps at 2^32 (~49.7 days):

```c
static inline int apad_time_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}
```

Both live in `core/src/seq.c`, both have dedicated conformance vectors exercising the wrap boundary, and **the fuzz corpus is seeded with sequences near 0xFFFF**. This is exactly the class of bug that ships to six untestable platforms and surfaces a week later.

### 5.9 Timing and latency

- Default send rate: 60 Hz on constrained clients, up to 125 Hz where the hardware allows.
- Send on change **plus** a keepalive at a fixed floor (10 Hz), so the server can detect a dead client and a lost "everything released" packet self-corrects.
- **No jitter buffer by default.** Apply on arrival. Expose a 0–2 frame buffer in settings for congested networks.
- `PING`/`PONG` every second; server tracks EWMA RTT and displays it. **If you cannot measure latency you cannot defend it** — RTT display is a v1 feature, not a nice-to-have.

Target budget on a quiet 5 GHz LAN:

| Stage | Budget |
|---|---|
| Client input poll | 4 ms |
| Wi-Fi transit | 6 ms |
| Server processing | 2 ms |
| Driver → game | 4 ms |
| **Total** | **~16 ms (one frame)** |

### 5.10 Security

Because of [§2.4](#24-nintendo-ds-wep-or-open-networks-only), the link layer provides nothing on the DS. So the protocol carries its own authentication — but it is important to be precise about what that authentication is actually worth.

**Mechanism:**

- Server generates a **6-digit PIN** during an explicit, user-initiated pairing window.
- Both sides derive a session key with **PBKDF2-HMAC-SHA256, 10,000 iterations**, salted with a 16-byte server nonce.
- Every packet after `WELCOME` carries a truncated HMAC-SHA256 tag.
- Sequence numbers plus a replay window prevent packet replay.
- Paired devices are remembered by a persistent client ID, so the PIN is a one-time step. **Not implemented as of 0.4.0:** the server authenticates only while a pairing window is open and keeps no trusted-device list between sessions, so today the PIN protects the moment of connection rather than the server. Until this lands, an unpaired client on the LAN can connect.

**Why 10,000 and not 600,000.** The iteration count is bounded by the DS: at roughly 8,000–10,000 PBKDF2 iterations per second on a 67 MHz ARM9, 10,000 iterations costs about one second of pairing time, which is the most a user will tolerate on a handheld with no progress feedback.

But the honest point is that **the iteration count is not the security mechanism and tuning it higher would be theatre.** A 6-digit PIN has 10^6 entropy. Even at 10,000 iterations that is 10^10 hash operations to exhaust — minutes of GPU time. No feasible iteration count makes a 6-digit PIN resistant to offline attack.

**What actually provides the security:**

1. **A bounded pairing window.** The PIN is valid for 120 seconds after the user initiates pairing, and never otherwise.
2. **Attempt limiting.** Five failed attempts invalidates the PIN and generates a new one.
3. **LAN-only binding.** The server binds LAN interfaces and never routes.

Together these make the realistic attack "be physically on the network during the two-minute pairing window and guess correctly within five tries."

**What this does not protect against, stated plainly:** an attacker already on your LAN who captures the pairing handshake can brute-force the PIN offline and recover the session key. On a WEP or open network — which the DS requires — that attacker is anyone in range. The documentation must say this. This is a toy-grade threat model: it stops a housemate, not an adversary.

**Forward path, reserved now.** The `WELCOME` payload includes a **32-byte `key_material` field, zero-filled in v1**. A later revision can carry a ChaCha20-wrapped long-term random key there, so the long-lived key is not PIN-derived and capturing the pairing exchange stops being sufficient. Reserving the field now means that upgrade is a capability bit rather than a wire-format break — the difference between a settings toggle and reflashing seven consoles.

ChaCha20 is ~200 lines and about 15 cycles/byte on ARM9, so this is genuinely tractable; it is deferred only because it is not needed to ship M1–M4. A PAKE (SPAKE2, CPace) would be the principled answer and would make the PIN's low entropy irrelevant, but scalar multiplication on a 67 MHz ARM9 runs into seconds. Not v1.

### 5.11 Versioning

- **Major version mismatch is a hard reject** with a human-readable `ERROR` naming which side to update. Never silently negotiate down.
- Feature variance is handled entirely by capability bits, never by optional fields.
- Reserved bits and bytes must be zero on send, ignored on receive.
- **Freeze the v1 wire format at the end of M1.** After that, changes go in v2. Reflashing seven consoles because a field moved is a mistake you make exactly once.

---

## 6. Server design

### 6.1 Backend interface

```c
typedef struct {
    int  (*init)(void);
    int  (*create_pad)(int slot, apad_pad_type type);
    int  (*update_pad)(int slot, const apad_pad_state *);
    int  (*poll_feedback)(int slot, apad_feedback *out);  /* rumble, LED */
    void (*destroy_pad)(int slot);
    void (*shutdown)(void);
    const char *name;
} apad_backend;
```

| Backend | Platform | Notes |
|---|---|---|
| `uinput.c` | Linux | Easiest. No driver install, just a udev rule. **Build this first.** |
| `vigem.c` | Windows | Emulate Xbox 360. Rumble/LED return through the driver. Detect missing driver and offer a real install prompt. Warn on the HP Omen forked driver. |

Building Linux first is deliberate despite Windows being the priority platform: uinput makes CI integration tests possible from M1. Otherwise they never get written.

### 6.2 Mapping engine

**This is the actual product**, and it lives in `server/src/mapping.c` — not in the core ([§3](#3-architecture)).

A DS has no analog stick and a touchscreen; a PSP has one stick; a Vita has two sticks plus rear touch. Per-device JSON profiles, with sane built-in defaults so the thing works before anyone opens a config file:

```jsonc
{
  // 3DS default. Old 3DS has no C-stick, so gyro drives the right stick.
  "profile": "3ds-default",
  "match":   { "device": "3DS" },
  "buttons": { "A": "A", "B": "B", "X": "X", "Y": "Y", "L": "LB", "R": "RB" },
  "sticks":  { "left": "left", "deadzone": 0.08, "curve": "quadratic" },
  "touch": {
    "mode": "regions",
    "regions": [
      { "rect": [0.0, 0, 0.5, 1.0], "emit": "LT", "analog": true },
      { "rect": [0.5, 0, 1.0, 1.0], "emit": "RT", "analog": true }
    ]
  },
  "gyro": { "mode": "right_stick", "sensitivity": 1.5, "deadzone": 0.02 }
}
```

Supported modes: direct button mapping; touch regions to buttons with optional analog pressure from depth-into-region; touch delta to right stick with return-to-center; absolute touch to right stick; gyro to right stick (essential on Old 3DS, which has no second stick); chorded macros; per-axis deadzone, curve, and inversion.

**Config format: JSON with comments (JSONC).** Reasoning:

- The parser is server-side only, so the core's no-malloc constraint does not apply and a small vendored tokenizer (jsmn-style, ~500 lines, single header) is plenty.
- Plain JSON is hostile to hand-editing — no comments, and a trailing comma is a hard error. Mapping profiles are exactly the kind of file users edit at midnight.
- TOML is friendlier but its C parsers are 3–4× the size for a format that buys little once the schema is nested.
- JSONC costs about 30 lines: a pre-pass strips `//` and `/* */` before tokenizing. Shipped default profiles are commented, so the defaults double as documentation.

### 6.3 Server UI

Minimal but non-negotiable:

- Own IP address, always visible (tier-3 discovery depends on it)
- Connected clients: name, pad slot, live RTT, battery, packet loss
- Pairing PIN when a new device appears, with the 120-second countdown visible
- Profile selector per connected client
- Backend status ("ViGEmBus not installed — click to install")
- mDNS status, so a failed 5353 bind is visible rather than mysterious

### 6.4 The server as a library

Today `server/` is a binary. `main.c` owns the socket, the session table, the
process lifecycle, signal handling and stdio logging, and everything else is
reachable only through it. That was correct for M1 — there was one host and
inventing a boundary before a second consumer exists is how you get the wrong
boundary.

A second host is now certain (Windows, M4) and a third is proposed (Android,
below). So the server splits into **`libapadserver` plus a thin host**:

| | |
|---|---|
| **Library owns** | session table and lifecycle, protocol handling via `libapad`, the mapping engine, profile parsing, backend dispatch |
| **Host owns** | process and thread model, the socket loop, the logging sink, filesystem access, UI |

**The API should lean sans-IO.** The library should not own a socket, a
thread, or `stdout`:

```c
apad_server *apad_server_create(const apad_server_cfg *, const apad_backend *);
int  apad_server_on_datagram(apad_server *, uint32_t now_ms,
                             const apad_addr *from,
                             const uint8_t *buf, size_t len);
int  apad_server_tick(apad_server *, uint32_t now_ms);   /* retransmits, timeouts */
```

with outbound datagrams delivered through an `on_send(to, buf, len)` callback
and diagnostics through `on_log(level, msg)` rather than `printf`. Profiles
arrive as memory blobs, not a directory path.

> **`on_datagram` gained `now_ms` when this was implemented (2026-08-10),
> and the sketch above is corrected rather than preserved.** The first
> version matched this sketch and let handlers read a clock stashed by the
> last `apad_server_tick()`. A protocol-guardian audit showed the staleness
> is strictly directional: every §8/§9 deadline fires *early*, never late —
> sessions time out before 3000 ms and the first retransmit gap is
> compressed, which §9 itself calls "indistinguishable from packet loss".
> The Linux host was safe only because it happens to tick immediately before
> delivering. Three plausible shapes for the next host are not: tick on a
> timer while a socket thread delivers, drain-the-socket batching, and
> `if (n == 0) tick()` — the last freezes the clock entirely under sustained
> input, so retransmits and timeouts never fire at all. Passing the clock in
> makes the mis-ordering unrepresentable instead of documented, and costs a
> host nothing it was not already doing.

That shape is not architectural taste, it is what the three hosts actually
need. Android wants its own lifecycle (a foreground `Service`), Windows wants a
tray application's message loop, and the existing fuzz and loopback tooling
wants to drive the server deterministically with no sockets at all. A library
that owns the loop can satisfy none of them.

**What does not change is the `apad_backend` interface.** It already isolates
the platform, which was the point of [§6.1](#61-backend-interface), and it
pays for itself here.

**Timing, which matters more than the design.** Do this split as the **first
task of M4**, not now and not afterwards. Now, there is no second consumer, so
the boundary cannot be validated and would be guessed. Afterwards, there are
two hosts to retrofit instead of one. At the start of M4 the Windows host is
the first real test of the API, and the Linux binary becomes a thin wrapper
whose job is to prove nothing regressed — a refactor with an immediate,
falsifiable check.

#### Android as a server host

The proposal: an Android device runs the server and creates a virtual pad on
*itself*, so a phone or tablet becomes the machine being played, driven by a
3DS or another handheld over the LAN.

The encouraging part is that **Android is Linux and `/dev/uinput` is
`/dev/uinput`**. `server/backends/uinput.c` may well work unchanged. The
porting cost is not the backend — it is privilege acquisition and the host
application.

The discouraging part is that access is governed by **SELinux policy, not just
file permissions**. Shizuku grants an app the `shell` UID's privileges without
root, but whether the `shell` domain may open `/dev/uinput` is a policy
question that varies by Android version and vendor, and it may simply be denied
on stock devices. That is unverified.

Fallbacks, in descending order of how much they preserve this product:

- **`InputManager.injectInputEvent`** — needs `INJECT_EVENTS`, which is
  `signature|privileged`, so also Shizuku territory and subject to the same
  policy question.
- **`BluetoothHidDevice`** (Android 9+, a public API, no root) — makes the
  phone present itself as a Bluetooth gamepad *to another machine*. That is a
  genuinely useful product but **it is not this one**: it inverts the
  direction, and the phone stops being the thing you play on.

**So this gets a research spike before it gets a milestone.** An
administrative or policy gate can make engineering effort worthless, and the
mistake would be scheduling work that depends on an unverified privilege
path. Spike deliverable: get *any* uinput
device created from an unrooted device via Shizuku, on one real handset, and
report the Android version and SELinux denial if it fails.

---

## 7. Client matrix

| Platform | Toolchain | Artifact | Verification |
|---|---|---|---|
| **3DS** | devkitARM + libctru | `.3dsx` + `.cia` | **On device** |
| **Android** | Kotlin + NDK | `.apk` | **On device** |
| Desktop | SDL3 + CMake | `.exe` / ELF | Native |
| PS Vita | VitaSDK (`gnuton/vitasdk-docker`) | `.vpk` | Vita3K |
| PSP | PSPDEV SDK | `EBOOT.PBP` | PPSSPP |
| DS / DSi | BlocksDS | `.nds` | melonDS |
| Switch | devkitA64 + libnx | `.nro` | Community reports |

### 7.1 3DS (primary target)

Best-supported client, because it is testable and genuinely well-suited: real BSD sockets via `soc:U`, touchscreen, gyro, circle pad, and WPA2 so it joins a normal network with no AP reconfiguration.

- `socInit()` requires an explicitly allocated **0x1000-aligned** buffer (128 KB is plenty). The single most common first-time 3DS networking mistake.
- C-stick is New 3DS only — gate on `APT_CheckNew3DS()`. On Old 3DS, gyro-to-right-stick is the default profile.
- Ship both `.3dsx` and `.cia` every release ([§8.3](#83-3ds-cia-builds)).
- During development use `3dslink` netload: press Y in the Homebrew Launcher and a new build runs in about two seconds. No SD card swapping.
- Draw a live latency readout on the top screen. Free diagnostics for every user.

### 7.2 Android (second target)

- Core compiled via NDK, exposed through a thin JNI layer. Do not reimplement the protocol in Kotlin.
- Touch overlay with configurable layout, plus physical gamepad passthrough via `InputDevice` / `MotionEvent`.
- Gyro via `SensorManager` at `SENSOR_DELAY_GAME`.
- mDNS discovery via `NsdManager` (tier 1).
- **Acquire a `WifiLock` in high-performance mode and keep the screen on.** Android aggressively powers down Wi-Fi otherwise and latency becomes erratic in a way that looks like a protocol bug.

#### Built so a server role can be added later — without stubbing one now

[§6.4](#64-the-server-as-a-library) proposes that Android eventually *host* a
server rather than only feed one. M3 ships a client and nothing else, but a few
structural choices decide whether that later becomes an extension or a rewrite.

The line to hold: **"just stubbing out the interface so it's ready later"
is the mistake**, because a stub
commits to a design before the constraints are known — and the Android server's
central constraint (whether SELinux lets Shizuku open `/dev/uinput`) is exactly
what is still unknown. So: no `ServerService` class, no disabled "server mode"
toggle, no empty packages.

What to do instead costs nothing extra in M3 and is simply better client design:

- **Put the session and network loop in a foreground `Service`, not an
  `Activity`.** This is the decision that matters most. A server must survive
  the user switching to a game; an `Activity`-hosted loop cannot. The client
  wants it too — it is where the `WifiLock` and the keep-alive belong, and it
  stops input dying when the overlay loses focus. Getting this wrong means
  rewriting the app's spine later.
- **Route sockets through `shim/`, not Kotlin.** The shim is already the
  portable socket layer and a server role would reuse it unchanged. Kotlin
  sockets would be a second networking implementation to throw away.
- **Name the JNI boundary by product, not role** — `AtticPadNative`, not
  `AtticPadClientNative` — and namespace calls (`client.*`) rather than
  assuming a single role. Renaming a JNI symbol later churns both sides.
- **Make the discovery module bidirectional-capable.** A client browses for
  `_atticpad._udp.local`; a server advertises it. `NsdManager` does both, and
  wrapping only `discoverServices` means unwrapping it later.
- **Keep profile and mapping concerns out of the client entirely.** They are
  server-side ([D9](#d9--mapping-engine-lives-in-the-server-not-the-core)). A
  client that grows profile handling would have to have it removed.

None of that adds a file that exists only for a future feature. Each item is
the better choice for the client on its own merits — which is the test for
whether "designing with X in mind" is foresight or speculation.

### 7.3 Blind-built clients

Ordered by likely value:

- **Vita** — richest hardware (two sticks, rear touch, gyro), most likely to be someone's daily driver. Highest priority among blind targets.
- **PSP** — one analog stick; the default profile must map something sensible in place of the missing right stick. Note the cache-coherency discipline around DMA on this platform.
- **DS/DSi** — hardest, least likely to be used. Last, when the protocol is frozen and proven. Requires `SETUP-DS.md` explaining the open/WEP AP situation honestly.
- **Switch** — has excellent native controllers, so this is mostly a novelty. No emulator worth building a pipeline on; pure community testing.

**DS toolchain: BlocksDS**, not classic devkitARM + libnds. It is actively maintained, its DSWiFi documentation is current, and DSi-mode WPA2 support is the difference between "reconfigure your router" and "it just works" for DSi owners. BlocksDS publishes container images; verify the current tag when pinning ([§8.2](#82-rules-that-prevent-the-usual-failures)). The DS is scheduled last, so this is revisitable — but the lean is recorded.

---

## 8. Continuous integration

Requirement: **no machine should ever be needed to produce a release.** All console toolchains publish Docker images, which makes this realistic.

That is a statement about *releases*, not about development. Building on the host during development is expected and much faster — see [§8.5](#85-the-local-development-loop). **CI is the gate; local is the loop.**

### 8.1 Job matrix

| Job | Runner / container | Produces |
|---|---|---|
| `core-test` | `ubuntu-latest` | unit tests, ASan + UBSan, conformance vectors |
| `core-fuzz` | `ubuntu-latest` | libFuzzer on `codec.c` — 5 min on PR, 1 hour nightly |
| `server-linux` | `ubuntu-latest` | binary |
| `server-windows` | `ubuntu-latest` + mingw-w64 | `.exe`, cross-compiled ([§2.1](#21-windows-vigembus-is-feature-frozen)) |
| `client-3ds` | `devkitpro/devkitarm` | `.3dsx`; `.cia` on tags only |
| `client-android` | `ubuntu-latest` + SDK/NDK | `.apk`; signed on tags only |
| `integration` | `ubuntu-latest` | loopback client → uinput server, asserts input arrives |
| `release` | needs: all | GitHub release with every artifact |

Planned, once the clients exist: `client-nds` (BlocksDS), `client-switch`
(`devkitpro/devkita64`), `client-psp` (`pspdev/pspdev`), `client-vita`
(`gnuton/vitasdk-docker`). Native installers (`.deb`, MSI) are not built
today; releases ship plain binaries.

### 8.2 Rules that prevent the usual failures

- **Reference images by stable key, not by path.** `scripts/toolchains.env` maps `VITASDK=` to a digest, so swapping image providers touches one file instead of every workflow. There is no `vitasdk/vitasdk` on Docker Hub; the maintained Vita image is `gnuton/vitasdk-docker`, and `pspdev/pspdev` may publish `:develop` more reliably than `:latest` — verify tags before pinning.
- **Pin container image digests, not `latest`.** devkitPro's `latest` moves and will break a build mid-sprint. A separate weekly `canary.yml` builds against unpinned images so breakage arrives on your schedule rather than during a release.
- **`core-test` is a required check.** No console build merges with a broken codec.
- **The release job fails if any artifact is missing.** A partial release is worse than a failed one.
- **`ccache` keyed on toolchain version.** Console builds are slow otherwise.
- **Signing only on tags**, so forks can build.
- **No proprietary SDKs, headers, or keys** anywhere in the repo or CI secrets.

### 8.3 3DS CIA builds

Both formats build in CI with no secrets:

```
bannertool makebanner -i banner.png -a audio.wav -o banner.bnr
bannertool makesmdh   -s "AtticPad" -l "AtticPad Controller" \
                      -p "author" -i icon.png -o icon.icn
makerom -f cia -o atticpad.cia -rsf app.rsf -target t -exefslogo \
        -elf atticpad.elf -icon icon.icn -banner banner.bnr \
        -DAPP_ENCRYPTED=false
```

- Neither `makerom` (3DSGuy/Project_CTR) nor `bannertool` (Steveice10) ships in `devkitpro/devkitarm`. Add a CI step to fetch or build them into `$PATH`; `makerom` compiles in seconds.
- **No Nintendo keys are involved.** `-target t` signs with the well-known test key; Luma3DS patches signature checks, so the CIA installs via FBI.
- `bannertool` is archived upstream — pin a specific release. Fallback: commit prebuilt `banner.bnr` and `icon.icn` and feed them to `makerom` directly.
- Pick a unique ID from the homebrew unique-ID list to avoid a title-ID collision.
- Assets: banner 256×128 PNG, icon 48×48 PNG, audio 16-bit WAV.

### 8.4 Example workflow fragment

```yaml
  client-3ds:
    runs-on: ubuntu-latest
    container:
      image: devkitpro/devkitarm@sha256:<pinned-digest>
    steps:
      - uses: actions/checkout@v4
      - name: Fetch CIA tools
        run: |
          mkdir -p /tools && cd /tools
          curl -sSL -o makerom.zip "$MAKEROM_URL"
          curl -sSL -o bannertool.zip "$BANNERTOOL_URL"
          unzip -j makerom.zip && unzip -j bannertool.zip
          chmod +x makerom bannertool
          echo "/tools" >> $GITHUB_PATH
      - run: make -C clients/3ds
      - run: make -C clients/3ds cia
      - uses: actions/upload-artifact@v4
        with:
          name: atticpad-3ds
          path: |
            clients/3ds/*.3dsx
            clients/3ds/*.cia
```

### 8.5 The local development loop

CI is the gate. Local is the loop. Waiting six minutes for a container job to report a missing semicolon is not a development process.

**The rule that keeps them equivalent: local console builds use the same pinned image digest as CI.** A wrapper script gives one command per target and removes the opportunity for drift:

```bash
./scripts/build.sh core      # native, run this constantly
./scripts/build.sh server
./scripts/build.sh desktop
./scripts/build.sh 3ds       # native devkitPro if present, else pinned container
./scripts/build.sh psp       # $PSPDEV from toolchains.env (same digest as CI)
./scripts/build.sh vita      # $VITASDK from toolchains.env (same digest as CI)
```

| Target | Local build | Loop time | Testable locally |
|---|---|---|---|
| core + tests | native `cc` | ~2 s | Yes — units, vectors, ASan, quick fuzz |
| Linux server | native | ~5 s | Yes — real uinput on the dev box |
| desktop client | native SDL3 | ~5 s | Yes — full loopback against the server |
| **3DS** | native devkitPro | ~10 s | **Yes — `3dslink` netload, ~2 s to hardware** |
| Android | native SDK/NDK | ~30 s | Yes — `adb install` |
| PSP | pinned container | ~20 s | Emulator — PPSSPP, networking works well |
| DS / DSi | pinned container | ~20 s | Emulator — melonDS |
| Vita | pinned container | ~30 s | Emulator — Vita3K, partial |
| Switch | pinned container | ~20 s | No |
| Windows server | needs a Windows box | — | Only there |

**The whole protocol exercises on one Linux machine.** Build the core natively, run the Linux server against a real uinput device, connect `tools/loopback-client`, and watch `evtest` show the virtual pad move. End to end in under ten seconds, no hardware, no CI. This is a large part of why the Linux server comes before Windows.

**The 3DS is the fastest hardware loop in the project.** A native devkitPro build plus `3dslink` netload puts a new binary on real hardware in about two seconds — faster than many local test suites. Lean on it heavily during M2.

**Drift risk, and why it is an acceptable trade.** Installing devkitPro natively means the local 3DS toolchain can diverge from the pinned CI container. That is deliberate: the 3DS is the primary hardware platform and iteration speed matters more there than anywhere else, and CI catches the drift on every push. For every other console, use the container — those platforms have no hardware loop to accelerate, so there is nothing to trade for.

**The one thing local builds must never do is produce a release artifact.** Releases come from tagged CI runs only, so what ships is always what CI built from what is committed.

---

## 9. Testing strategy

Most clients are code that will never run on the machine it was written on. That single fact shapes everything below.

### 9.1 Conformance vectors — the load-bearing one

Byte-exact golden packets in `core/testdata/vectors.h` with expected decoded structs. **Every client ships a self-test screen with TWO triggers:**

1. **Hold L+R+Start at launch** — the hidden path. It runs *before any network
   code*, so it stays reachable when the client is otherwise broken. Sample the
   combo across ~2.5 s, not a single frame.
2. **A visible one-line prompt** (`SELECT: self-test` on 3DS) reachable at any
   time, including while a connection is failing.

Two triggers because on 3DS the hidden one was unreachable for a reason
invisible from the screen: START was also the exit key, so holding the
documented combo reached the main loop with START down, read as a fresh press,
and disconnected instantly. **Exit actions MUST be gated on the key being seen
released at least once** — a key held across a screen transition is newly-down
to whatever screen it lands on. A diagnostic that cannot be invoked is worth
nothing, and this is the only tool six blind platforms have.

This is what makes blind platforms supportable. When a Vita user reports "it doesn't work," the first question is "what does the self-test say," and the answer immediately separates *your codec is broken on this CPU* from *your Wi-Fi is bad*. It also catches the endianness and alignment bugs that no desktop testing will find.

**Mandatory vector coverage:** sequence wrap across 0xFFFF, tick wrap across 2^32, every D-pad LUT index including the impossible combinations, maximum-length packets, zero-length payloads, all-reserved-bits-set (must be *ignored*, not rejected — see `docs/PROTOCOL.md` §2), and truncated packets at every byte offset.

**Vectors are written from `docs/PROTOCOL.md` only, by an author who has not read `core/src/codec.c`.** If the same person writes both, they encode the same misunderstanding twice and the self-test passes on a broken build. Keeping those two roles in separate hands is what makes the vectors worth having.

### 9.2 Fuzzing

libFuzzer against `codec.c` in CI. Not optional hygiene: the parser runs on consoles with **no MMU and no exploit mitigations**, where a malformed packet from anywhere on the LAN reaching a buffer overrun is a real bug. Seed the corpus from the conformance vectors, including the wrap-boundary cases.

### 9.3 Latency harness

`tools/latency-bench` timestamps, sends, and reads the resulting virtual pad state back via evdev or XInput, reporting p50/p95/p99 end to end. Runs on every release. Latency regressions are invisible without this, and users describe them as "it feels worse now."

### 9.4 Emulators

| Platform | Emulator | Networking |
|---|---|---|
| PSP | PPSSPP | Yes — works well |
| DS | melonDS | Yes |
| Vita | Vita3K | Partial, usable for UDP |
| 3DS | Azahar | Available, but real hardware is on hand |
| Switch | — | Nothing worth building a pipeline on |

PPSSPP and melonDS are scriptable enough to serve as CI smoke tests.

### 9.5 Support tiers, published

In the README, honestly:

- **Hardware-verified** — 3DS, Android, Linux server, Windows server (tested on real hardware every release)
- **Emulator-verified** — PSP, DS/DSi, Vita (build passes and the on-device self-test passes under PPSSPP, melonDS, or Vita3K; never run on hardware)
- **Built, untested** — Switch (CI-built, community-tested)

The middle tier exists because [§8.5](#85-the-local-development-loop) makes it real: a client that boots in an emulator and passes its conformance vectors has cleared the codec, endianness, and alignment classes of bug, leaving only genuine hardware behaviour. That is a meaningfully different claim from "it compiled," and users deserve to see the difference.

People forgive an honest label. They do not forgive a broken promise, and the label invites exactly the contributors you need.

---

## 11. Milestones

What the M-numbers referenced throughout this repository mean. This is scope,
not a schedule.

| Milestone | Scope |
|---|---|
| **M0** | Spike. A uinput virtual pad moved by a hardcoded UDP packet, and a 3DS homebrew that sends one. Prove both before building anything. |
| **M1** | Core, Linux server, conformance vectors, CI green end to end. **The v1 wire format freezes here.** |
| **M2** | 3DS client (`.3dsx` + `.cia`), mapping engine, gyro aim, self-test screen. |
| **M2.5** | 3DS UI pass: rendered screens replacing the text console, a real tier-3 IP-entry screen, touch-region affordances. Grouped because they share an asset pipeline. |
| **M3** | Android client, mDNS discovery, pairing flow, touch overlay editor. Structured so a server role can be added later without a rewrite ([§7.2](#72-android-second-target)) — but **no server stubs**. |
| **M4** | Windows server: the `libapadserver` + thin host split ([§6.4](#64-the-server-as-a-library)), ViGEmBus backend, driver detection, tray UI. |
| **M4.5** | Shared client engine hoisted to `clients/common/`, so a new client implements screens rather than the protocol. |
| **M5** | Remaining console clients: PSP, Switch, DS. Vita is shelved on a toolchain blocker, not merely unscheduled. |
| **M7** | Android server (proposed): the phone hosts the virtual pad rather than sending to one. Gated on a research spike — `/dev/uinput` from an unrooted device is an SELinux question that may have no answer on stock hardware ([§6.4](#64-the-server-as-a-library)). |

### Why this order

- M0 exists to kill the plan early if the two riskiest assumptions are wrong.
- Linux server before Windows, despite Windows being the priority, because uinput makes CI integration tests possible from M1. Otherwise they never get written.
- 3DS first among clients because it is the console that can actually be tested, and because it exercises the constrained path — fixed layouts, alignment sensitivity, raw broadcast — validating the decisions that exist for the DS's benefit.
- 3DS + Android is a strong pair: one constrained, one rich. A protocol honest across both is mostly transcription for the rest.
- DS last: it *shapes* the protocol but is the least valuable client to own.

---

## 13. Decisions

Previously open; now resolved. Each records the reasoning so it can be revisited on evidence rather than vibes.

### D1 — D-pad bit layout: bits 4–7, contiguous

`UP=4, DOWN=5, LEFT=6, RIGHT=7`. Extract as `(buttons >> 4) & 0xF` into a 16-entry HID hat LUT ([§5.7](#57-input-state-payload)). Branchless, and impossible combinations resolve sanely rather than being undefined. The D-pad is the primary input on DS and PSP, so the fast path is worth a contiguous nibble. Full 32-bit map frozen in §5.7 — **never renumber**.

### D2 — Default port: 21100

Above 1024 (no elevated privileges), below 32768 (outside the Linux ephemeral range 32768–60999, avoiding intermittent bind collisions under load), and clear of clusters this project's users will be running: 24800 Deskflow/Synergy, 27000–27050 Steam, 47984–48010 Sunshine, 5353 mDNS. ([§5.1](#51-transport))

### D3 — PBKDF2: 10,000 iterations, plus a bounded pairing window

10,000 iterations is ~1 second on a 67 MHz ARM9, the most a user tolerates on a handheld with no progress bar. **The iteration count is not the security mechanism** — no feasible count protects 10^6 of PIN entropy against offline attack. The real protections are a 120-second pairing window, five-attempt limiting, and LAN-only binding. A 32-byte `key_material` field is reserved in `WELCOME` (zero in v1) so ChaCha20 key-wrapping can be added later as a capability bit rather than a wire break. Full reasoning and the stated limits of the threat model in [§5.10](#510-security).

### D4 — mDNS: vendor an advertise-only responder

Depending on a system daemon means requiring Apple's Bonjour service on Windows — a second install alongside ViGEmBus before the app works once. Advertise-only is ~500 lines: answer PTR/SRV/TXT/A for one service name, announce on start and IP change. Bind 5353 with `SO_REUSEADDR` + `SO_REUSEPORT`; on failure, disable tier 1 and say so in the UI. Acceptable because tier 3 always works. ([§5.5](#55-discovery-three-tier))

### D5 — Multiple pads from one device: not in v1, not precluded

Niche, and it complicates the session model. But no protocol change is needed later: the server keys sessions on `(IP, source port, session_id)`, so a client opening two sockets already gets two pads. Documented as the forward path; not implemented, not tested, not supported in v1.

### D6 — Config format: JSONC (JSON plus comments)

Parser is server-side only, so the core's no-malloc rule does not apply. A jsmn-style tokenizer (~500 lines, single header) plus a ~30-line comment-stripping pre-pass. Plain JSON is hostile to hand-editing (no comments, trailing commas fatal); TOML's C parsers cost 3–4× for little gain at this schema depth. Shipped default profiles are commented, so the defaults double as documentation. ([§6.2](#62-mapping-engine))

### D7 — Desktop client and server stay separate processes

Sharing a process would muddy the backend interface and, worse, create a test path that bypasses the real socket layer — which is precisely where platform bugs live. `tools/loopback-client` stays a separate binary and is what CI's `integration` job drives.

### D8 — DS toolchain: BlocksDS

Actively maintained, current DSWiFi documentation, DSi-mode WPA2 support. The last point is a real UX difference: WPA2 in DSi mode means DSi owners join a normal network instead of reconfiguring their router. BlocksDS publishes container images; verify the tag at pin time. Revisitable — the DS is scheduled last.

### D9 — Mapping engine lives in the server, not the core

Server-only code. Placing it in `libapad` would subject it to no-malloc, no-float, no-stdio for zero benefit and grow every handheld binary. Core is now codec, session FSM, HMAC, self-test — nothing else. ([§3](#3-architecture))

### D10 — Sequence and tick comparisons use serial-number arithmetic

A 16-bit sequence at 125 Hz wraps every ~8.7 minutes and a naive `>` comparison freezes the client at every wrap — while passing every test shorter than nine minutes. `apad_seq_newer()` and `apad_time_after()` in `core/src/seq.c`, with conformance vectors at the wrap boundary and a fuzz corpus seeded near 0xFFFF. ([§5.8](#58-reliability-and-wrap-safe-arithmetic))

### D11 — Axis conventions: sticks `+Y` up, touch `+Y` down

Sticks match XInput; touch matches screen space. Clients normalize before filling the struct — PSP and Vita report sticks `+Y` down and must invert. **Deadzone is never applied client-side**; raw normalized values go on the wire and the server profile owns deadzone, curve, and inversion, because a client that pre-applies deadzone destroys information the server cannot recover. ([§5.7](#57-input-state-payload))
