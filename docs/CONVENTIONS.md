# AtticPad — conventions

Use phones, handhelds, and old consoles as game controllers for a PC.
Server creates virtual gamepads; clients send input over LAN via UDP.

**Design rationale:** `DESIGN.md` · **Normative spec:** `PROTOCOL.md`

---

## Core rules (`core/`, `shim/`)

This code runs on a 67 MHz ARM9 with 4 MB RAM and no MMU. Violating any of these breaks a platform that cannot easily be tested:

- C99 only. No `malloc` after init. No floating point. No stdio, `time.h`, or locale.
- **Never cast a struct pointer onto a packet buffer.** Use the `memcpy` helpers in `codec.c`. The DS ARM9 *silently rotates* on unaligned word loads instead of faulting.
- Little-endian on the wire, assembled byte by byte. Never assume the host.
- Sequence and tick comparisons use `apad_seq_newer()` / `apad_time_after()` from `seq.c`. A naive `>` passes every test under nine minutes, then freezes the client forever.
- Reserved bits and bytes: zero on send, ignored on receive.

The mapping engine lives in `server/`, not `core/`. Do not move it.

## Spec authority

`PROTOCOL.md` is normative. If code and spec disagree, **the spec is right** — report the discrepancy, change neither.

The v1 wire format was frozen at the end of M1. Any change to a wire-visible struct, constant, or bit position is a v2 protocol change: stop and raise it rather than making it.

## Blind platforms

Several target platforms are written without direct access to the hardware. For `clients/{vita,psp,nds,switch}` (planned, not yet present):

- **Mirror the platform SDK's own samples for networking init. Do not write it from memory.** If a sample and your recollection disagree, the sample wins and you say so.
- Check the official documentation when the sample doesn't cover it.
- **Every change ends with what you could not verify and what you assumed instead.**

## Build and test

```bash
./scripts/build.sh core      # native, run constantly
./scripts/build.sh <target>  # same pinned digest as CI
```

Build **and run** before claiming something works — emulator or hardware, and say which. "It compiles" is not done. Full loop on Linux: core + server + `tools/loopback-client` + `evtest`, under ten seconds.

Never produce a release artifact locally. Releases come from tagged CI only.

## Conventions

- Sticks `+Y` up (XInput); touch `+Y` down (screen space). Clients normalize before filling the struct.
- Never apply deadzone client-side. Server profiles own deadzone, curve, inversion.
- Clients call `libapad` and the shim. Never reimplement protocol logic in platform code.
- Every client ships the hidden self-test screen (hold L+R+Start).
