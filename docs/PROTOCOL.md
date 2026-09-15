# AtticPad Protocol v1 — normative specification

**Status: FROZEN.** The v1 wire format was frozen on 2026-08-09 at the end of
M1. Every constant, offset, bit position, payload size, normalisation rule and
the reliability contract in §8/§9 are now fixed. **Any change to them is a v2
change** — stop and report rather than making one. Adding a NEW message type is
the one additive exception, ruled and bounded in §6.14. Five types have been
added that way — §6.12 in 2026-08-16, and §6.15–§6.19 in 2026-08-25 — and the
format is otherwise unchanged.

Frozen against, **as of 2026-08-09**: 193 conformance vectors derived from this
document by an author who never read the codec, 990 self-test cases, 675 + 46
cross-validation checks with zero disagreements, a clean libFuzzer run, and two
independent audits. Those are the figures the freeze was taken against and are
left as the historical record; the suite has grown since — 1987 self-test
cases across 34 vector tables as of 2026-08-25 — without any wire-visible
change to v1. §15 tracks what has
been verified since — including, at §15.9, the one message type that has no
independently derived vectors.

**Authority.** This document is the source of truth. `DESIGN.md` §5 is rationale
and may lag; where they differ, this document is right. Implementations that
disagree with this document are wrong — report the discrepancy, change neither
until it is resolved here.

**Conformance language.** MUST, MUST NOT, SHOULD, MAY as in RFC 2119.

**Prefixes.** Symbols use `apad_` / `APAD_`.

---

## 1. Transport

- UDP only. Default port **21100** (`DESIGN.md` D2). One socket per client.
- Maximum datagram **256 bytes**. No fragmentation, ever.
- Every datagram is independently parseable. The codec holds no cross-packet
  state.
- The server keys a session on `(source IP, source port, session_id)`.

## 2. Encoding rules

- **Little-endian on the wire**, assembled and read one byte at a time. An
  implementation MUST NOT assume host endianness.
- **Fields are at fixed offsets and naturally aligned** within the datagram.
  No varints, no TLV, no compression.
- An implementation MUST NOT cast a struct pointer onto a packet buffer. All
  field access goes through byte-wise or `memcpy` helpers. (The DS ARM9
  silently rotates on unaligned word loads instead of faulting; the resulting
  bug looks like data corruption, not a crash.)
- **Reserved bits and bytes MUST be zero on send and MUST be ignored on
  receive.** A receiver MUST NOT reject a packet for non-zero reserved fields
  except where this document explicitly says otherwise.
- **"Ignored" means scrubbed, not passed through.** On decode a receiver MUST
  zero every reserved field, every reserved bit, and every touch entry at index
  ≥ `touch_count` in its decoded output. A decoded structure is therefore
  deterministic regardless of what a non-conforming sender transmitted, which
  is what lets the on-device self-test compare decoded structures byte for
  byte across eight platforms.
- Text fields are UTF-8, NUL-padded to their fixed width, and need not be
  NUL-terminated when they fill the field. A receiver MUST treat a text field
  as bounded by its fixed width. A sender MUST NOT split a multi-byte UTF-8
  sequence across the end of the field — truncate at a character boundary and
  NUL-pad the remainder. A receiver MUST tolerate a malformed trailing sequence
  rather than reject the packet.

## 3. Packet header — 12 bytes, every datagram

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `magic` | `0x4D43` (`"MC"`, little-endian: bytes `43 4D`) |
| 2 | 1 | `version` | major. Mismatch MUST be a hard reject (§12) |
| 3 | 1 | `type` | §4 |
| 4 | 2 | `session_id` | 0 during discovery and handshake |
| 6 | 2 | `sequence` | wraps; independent per direction; see §9 |
| 8 | 2 | `payload_len` | payload bytes only; excludes header and tag |
| 10 | 2 | `flags` | bit 0 `AUTHENTICATED`, bit 1 `RELIABLE`, bits 2–15 reserved |

`version` for this specification is **1**.

Datagram layout:

```
+------------------+---------------------------+------------------+
| header, 12 bytes | payload, payload_len bytes | tag, 8 bytes     |
+------------------+---------------------------+------------------+
                                                 present only when
                                                 flags bit 0 is set
```

### 3.1 Validation order

A receiver MUST apply these checks in this order and stop at the first failure.
"Discard" means drop silently. "Reject" means drop and MAY send `ERROR`.

| # | Check | On failure |
|---|---|---|
| 1 | datagram length ≥ 12, so the header is readable at all | discard |
| 2 | `magic` == `0x4D43` | discard |
| 3 | `version` == 1 | reject, `ERROR` code 1 (§12) |
| 4 | actual length == `12 + payload_len + (AUTHENTICATED ? 8 : 0)` | reject, `ERROR` code 6 |
| 5 | `type` appears in the §4 table | discard (§4) |
| 6 | `payload_len` == the fixed payload size for that `type` (§4) | reject, `ERROR` code 6 |
| 7 | if `AUTHENTICATED`, the tag verifies (§10) | reject, `ERROR` code 3 |

**Check 6 is load-bearing, not pedantry.** Every v1 type has a fixed payload
size, so a datagram that is internally consistent under check 4 but declares
the wrong size for its type is malformed. Without check 6, a decoder that
trusts the type's size reads past the end of a short payload — an overread
driven by any packet on the LAN, on consoles with no MMU and no exploit
mitigations. Checks 4 and 6 are separate and both mandatory.

## 4. Message types

| Code | Name | Direction | Reliable | Payload |
|---|---|---|---|---|
| `0x01` | `DISCOVER` | client → broadcast | no | 0 bytes (§6.1) |
| `0x02` | `ANNOUNCE` | server → client | no | 40 bytes (§6.2) |
| `0x10` | `HELLO` | client → server | yes | 76 bytes (§6.3) |
| `0x11` | `WELCOME` | server → client | yes | 60 bytes (§6.4) |
| `0x12` | `BYE` | either | yes | 4 bytes (§6.5) |
| `0x20` | `INPUT_STATE` | client → server | **no** | 56 bytes (§5) |
| `0x21` | `KEYBOARD` | client → server | **no** | 56 bytes (§6.15) |
| `0x22` | `MOUSE` | client → server | **no** | 24 bytes (§6.16) |
| `0x23` | `MEDIA` | client → server | **no** | 20 bytes (§6.17) |
| `0x30` | `PING` | either | no | 8 bytes (§6.6) |
| `0x31` | `PONG` | either | no | 8 bytes (§6.6) |
| `0x40` | `RUMBLE` | server → client | yes | 8 bytes (§6.7) |
| `0x41` | `LED` | server → client | yes | 4 bytes (§6.8) |
| `0x42` | `STATUS` | server → client | yes | 64 bytes (§6.9) |
| `0x43` | `TOUCHMAP` | server → client | no | 68 bytes (§6.12) |
| `0x44` | `INPUTCAPS` | server → client | no | 16 bytes (§6.19) |
| `0x50` | `ACK` | either | no | 4 bytes (§6.10) |
| `0x51` | `ERROR` | either | no | 64 bytes (§6.11) |

A receiver MUST silently discard a datagram with an unknown `type`.

---

## 5. `INPUT_STATE` payload — 56 bytes

The only high-rate message. A **fixed superset**: the client zero-fills what it
does not have, and the server ignores what the capability mask (§6.3) says is
not physically present.

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `buttons` | u32 bitmask, §5.1 |
| 4 | 16 | `axes[8]` | i16 × 8 — LX LY RX RY L2 R2 + 2 reserved |
| 20 | 1 | `touch_count` | 0–2. Values > 2 MUST be clamped to 2 on receive |
| 21 | 1 | `reserved0` | |
| 22 | 12 | `touches[2]` | §5.2 |
| 34 | 6 | `accel[3]` | i16 × 3, milli-g, X Y Z |
| 40 | 6 | `gyro[3]` | i16 × 3, deci-degrees/second, pitch roll yaw |
| 46 | 1 | `battery` | 0–100 percent; `255` = unknown; 101–254 reserved (§5.5) |
| 47 | 3 | `reserved1[3]` | |
| 50 | 2 | `reserved2[2]` | |
| 52 | 4 | `client_ticks_ms` | client monotonic clock at sample time |

**Total 56 bytes.** `reserved2` exists so `client_ticks_ms` lands on a
4-byte boundary; it is padding made explicit rather than implied, because
implied padding is exactly what differs between compilers.

> **Divergence from `DESIGN.md` §5.7, resolved here.** That struct is commented
> `/* 48 bytes */`, but its fields sum to 56 under natural alignment: with
> `reserved1[3]` ending at offset 49, `client_ticks_ms` requires 4-byte
> alignment and lands at 52, and the struct rounds to 56. 48 is the size of
> that struct *without* `client_ticks_ms`, so the comment appears to predate
> the field. `client_ticks_ms` is kept — the latency budget in `DESIGN.md` §5.9
> and the harness in §9.3 both need a sample timestamp, and 8 bytes at 125 Hz
> is 1 kB/s. The comment in `DESIGN.md` §5.7 should be corrected to 56.

Axis assignment within `axes[8]`:

| Index | Field | Range |
|---|---|---|
| 0 | left stick X | −32768 … 32767, centred 0 |
| 1 | left stick Y | −32768 … 32767, centred 0, **+Y up** |
| 2 | right stick X | as above |
| 3 | right stick Y | as above, **+Y up** |
| 4 | L2 / left trigger | 0 … 32767 |
| 5 | R2 / right trigger | 0 … 32767 |
| 6–7 | reserved | MUST be zero |

Negative values in `axes[4]`/`axes[5]` MUST be treated as 0 on receive.

### 5.1 Button bitmask — frozen at v1, never renumber

```
 0  A                 8  L                16  HOME
 1  B                 9  R                17  TOUCH_PRESS
 2  X                10  ZL               18  TOUCH_REAR_PRESS
 3  Y                11  ZR               19  CAPTURE
 4  DPAD_UP          12  L3               20..31  reserved (MUST be zero)
 5  DPAD_DOWN        13  R3
 6  DPAD_LEFT        14  START
 7  DPAD_RIGHT       15  SELECT
```

Names follow the **Nintendo convention: A is the right-hand face button**, B is
bottom, X is top, Y is left. Translation to Xbox naming is a server concern
(§5.4), not a wire concern.

**The D-pad occupies bits 4–7 contiguously and deliberately.** Extract as
`(buttons >> 4) & 0xF` and index this table to get a HID hat value. Every
impossible combination resolves to something sane rather than undefined:

```c
/* index: bit0=UP bit1=DOWN bit2=LEFT bit3=RIGHT
   value: HID hat 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=null */
static const uint8_t apad_hat_lut[16] = {
    8, 0, 4, 8, 6, 7, 5, 6,
    2, 1, 3, 2, 8, 0, 4, 8
};
```

### 5.2 Touch entries

Each of the two entries is 6 bytes, at offset `22 + 6 * i`:

| Off | Size | Field | Notes |
|---|---|---|---|
| +0 | 1 | `id` | tracking id, stable for the life of a contact |
| +1 | 1 | `pressure` | 0 = unknown / binary touch |
| +2 | 2 | `x` | i16, normalised −32768 … 32767 |
| +4 | 2 | `y` | i16, normalised −32768 … 32767, **+Y down** |

Entries at index ≥ `touch_count` MUST be zero on send and MUST be ignored on
receive.

### 5.3 Coordinate conventions

- **Sticks: `+Y` is up.** Matches XInput. The 3DS `circlePosition` already
  matches; PSP and Vita report `+Y` down and **the client MUST invert before
  filling the struct**.
- **Touch: `+Y` is down**, origin top-left. Screen space.
- The mismatch is intentional: sticks are vector space, touch is screen space,
  and each is correct in its own domain.
- **A client MUST NOT apply a deadzone.** Raw normalised values go on the wire.
  The server profile owns deadzone, curve and inversion. A client that
  pre-applies a deadzone destroys information the server cannot recover.

### 5.4 Server-side interpretation

- **Trigger disambiguation.** If the client advertised `APAD_CAP_TRIGGERS`, the
  server MUST drive LT/RT from `axes[4]`/`axes[5]` and MUST ignore the `ZL`/`ZR`
  bits. If it did not, `ZL`/`ZR` presses MUST drive LT/RT to full deflection.
  A client with analog triggers SHOULD also set the digital bits past a
  threshold, for profiles that want them as buttons.
- **Face-button naming.** A server presenting an Xbox-convention pad maps by
  physical position: wire `A`→Xbox B, `B`→A, `X`→Y, `Y`→X. Per-profile A/B and
  X/Y swapping is a mapping concern, not a protocol concern.
- **evdev sticks are `+Y` down.** A Linux/uinput backend MUST negate `axes[1]`
  and `axes[3]`, clamping −32768 (which has no positive counterpart in i16) to
  32767. Verified against `evtest` during the M0 spike.

### 5.5 Battery normalisation

`battery` carries 0–100, or `255` for unknown. Values 101–254 are reserved.
**A sender MUST NOT transmit them**, and **a decoder MUST normalise them to
`255` in its decoded output.** The decode-time rule is not redundant with the
send-side one: it is what makes a non-conforming or hostile sender harmless,
so no consumer on any platform ever sees a battery level of 173.

The two halves are deliberately asymmetric. An encoder passes an
out-of-range value through unchanged rather than silently normalising it, so
that a buggy client stays visible as a buggy client instead of being
laundered into a plausible-looking "unknown".

That matches the three other receive-side rules in this section — the
`touch_count` clamp, the trigger clamp, and the §2 reserved-bit scrub — all of
which the decoder applies. A rule that some §5 fields are normalised at decode
and others only by convention downstream is the kind of inconsistency that
produces a server displaying a battery level of 173%.

**`255` is not how a client says "I have no battery."** Per §5 a client
zero-fills what it does not have, so a device with no battery sends `0`, which
decodes as a genuine 0% and is indistinguishable from a flat one. Absence is
signalled by leaving `APAD_CAP_BATTERY` clear in `HELLO` (§6.3), exactly as it
is for gyro, accelerometer and touch — the capability mask is the single
mechanism for "this hardware does not exist", and no field carries a private
sentinel for it. A server MUST consult the capability mask before displaying a
battery level.

---

## 6. Other payloads

### 6.0 Out-of-range values in this section

Some fields carry an explicit receive-side normalisation rule — at v1,
`ANNOUNCE.pairing_required` (§6.2), `LED.player_index` (§6.8) and, in §5,
`battery`; §6.15–§6.17 later added the event codes of `KEYBOARD`, `MOUSE` and
`MEDIA` on the same reasoning, and specify them there. They share a property:
each drives something a consumer acts on, so an out-of-range value has to
become a defined one before it reaches application code.

**The rule below is unchanged from v1** — only the list of fields it governs
has grown, and it grew additively with the types that brought them.

**The enumerated fields are deliberately different.** `BYE.reason` (§6.5),
`STATUS.code` (§6.9) and `ERROR.code` (§6.11) list their defined values but
carry no clamp: a receiver **MUST** preserve an unrecognised value verbatim
and **MUST NOT** reject the packet for carrying one. These are diagnostic
labels, not control inputs (one deliberate, named exception: §8 gives
`ERROR` code 7 a mandated client-side effect) — a v1.1 server may add reason or error codes, and
a v1.0 receiver that clamped them would destroy the only information the
message exists to convey. Preserving an unknown code lets it be logged and
reported; normalising it to a known one would be actively misleading.

`ERROR` code `0` is unassigned and reserved. A receiver treats it as an
unrecognised code under the rule above.

Everything in this section beyond the field set already fixed by `DESIGN.md` is
**newly specified here** and is the part of the wire format most in need of
review before the freeze. See §14.

### 6.1 `DISCOVER` (0x01) — 0 bytes

`session_id` MUST be 0. Sent to the broadcast address, or unicast to a
manually entered address.

### 6.2 `ANNOUNCE` (0x02) — 40 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 32 | `server_name`, UTF-8 |
| 32 | 1 | `pads_total` |
| 33 | 1 | `pads_free` |
| 34 | 1 | `pairing_required` — 0 or 1; any non-zero value MUST be read as 1 |
| 35 | 1 | `reserved0` |
| 36 | 2 | `server_port` — authoritative; MAY differ from the datagram's source port |
| 38 | 2 | `reserved1` |

### 6.3 `HELLO` (0x10) — 76 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 16 | `client_id` — persistent, random at first run, identifies a paired device |
| 16 | 4 | `caps` — capability bitmask, below |
| 20 | 32 | `device_name`, UTF-8 |
| 52 | 16 | `client_nonce` |
| 68 | 2 | `desired_rate_hz` |
| 70 | 1 | `proto_major` — MUST equal the header `version` |
| 71 | 1 | `reserved0` |
| 72 | 4 | `client_ticks_ms` |

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
#define APAD_CAP_RUMBLE      (1u << 11)
#define APAD_CAP_LED         (1u << 12)
#define APAD_CAP_BATTERY     (1u << 13)
/* bits 14..31 reserved, MUST be zero */
```

### 6.4 `WELCOME` (0x11) — 60 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 2 | `session_id` — non-zero; used in every later header |
| 2 | 1 | `pad_slot` |
| 3 | 1 | `flags` — bit 0 `AUTH_REQUIRED`, bits 1–7 reserved |
| 4 | 2 | `input_rate_hz` — the rate the server wants |
| 6 | 2 | `reserved0` |
| 8 | 16 | `server_nonce` — PBKDF2 salt (§10) |
| 24 | 32 | `key_material` — **MUST be zero in v1**, MUST be ignored on receive |
| 56 | 4 | `server_ticks_ms` |

`key_material` is reserved so a later revision can carry a wrapped long-term
key as a capability bit rather than a wire break (`DESIGN.md` D3).

### 6.5 `BYE` (0x12) — 4 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `reason` — 0 normal, 1 timeout, 2 server shutdown, 3 slot revoked |
| 1 | 3 | `reserved0` |

### 6.6 `PING` (0x30) / `PONG` (0x31) — 8 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `origin_ticks_ms` — sender's clock (`PING`); echoed unchanged (`PONG`) |
| 4 | 4 | `responder_ticks_ms` — 0 in `PING` |

RTT is `now − origin_ticks_ms` computed by the original sender, using the
wrap-safe subtraction in §9.

In a `PING`, `responder_ticks_ms` **MUST** be zero on send. `PING` and `PONG`
share one payload layout and one decoder, so a receiver **preserves** the field
as received rather than scrubbing it, and **MUST NOT** act on it in a `PING`.
This is stated because §2's scrub rule would otherwise be a reasonable reading,
and the two readings disagree on what a conformance vector should assert. Correlation is by `origin_ticks_ms` alone: a
`PONG` carries its own per-direction header `sequence` and MUST NOT echo the
`PING`'s.

### 6.7 `RUMBLE` (0x40) — 8 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 2 | `low_freq` — 0–65535 |
| 2 | 2 | `high_freq` — 0–65535 |
| 4 | 2 | `duration_ms` — 0 means "until superseded" |
| 6 | 2 | `reserved0` |

### 6.8 `LED` (0x41) — 4 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `player_index` — 1–4, 0 = off |
| 1 | 1 | `r` |
| 2 | 1 | `g` |
| 3 | 1 | `b` |

A client without an RGB LED SHOULD use `player_index` and ignore the colour.
Values above 4 are reserved and MUST be treated as 0 (off) on receive.

### 6.9 `STATUS` (0x42) — 64 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `code` — 0 info, 1 warning, 2 error |
| 1 | 3 | `reserved0` |
| 4 | 60 | `text`, UTF-8 |

### 6.10 `ACK` (0x50) — 4 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 2 | `sequence` — the sequence being acknowledged |
| 2 | 2 | `reserved0` |

### 6.11 `ERROR` (0x51) — 64 bytes

| Off | Size | Field |
|---|---|---|
| 0 | 2 | `code` — below |
| 2 | 2 | `reserved0` |
| 4 | 60 | `text`, UTF-8, human-readable |

| Code | Meaning |
|---|---|
| 1 | version mismatch — `text` names which side to update |
| 2 | no free pad slot |
| 3 | authentication failed |
| 4 | pairing window closed |
| 5 | too many pairing attempts |
| 6 | malformed packet |
| 7 | unknown session |


### 6.12 `TOUCHMAP` (0x43) — 68 bytes

**Added after the v1 freeze, additively.** See §6.14 for why that is permitted
and what it does not permit.

Tells a client what its touchscreen currently maps to, so it can DRAW the
mapping instead of guessing. Optional in both directions: a server need never
send one, and a client that does not understand it discards it under §4 like
any unknown type.

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `mode` — 0 none, 1 regions, 2 delta-stick, 3 absolute-stick |
| 1 | 1 | `region_count` — 0..8; **greater than 8 MUST be rejected** |
| 2 | 2 | `reserved0` |
| 4 | 64 | `regions[8]`, 8 bytes each, below |

Each region:

| Off | Size | Field |
|---|---|---|
| 0 | 1 | `x0` — normalised 0..255 across the touch surface |
| 1 | 1 | `y0` — normalised 0..255, **+Y down** (§5) |
| 2 | 1 | `x1` |
| 3 | 1 | `y1` |
| 4 | 1 | `target` — 0 button, 1 LT, 2 RT |
| 5 | 1 | `analog` — 1 if depth-into-region drives an analogue value |
| 6 | 2 | `pad_bit` — §6.13 pad-output button, when `target` is 0 |

Unused region slots are transmitted zeroed. The payload is a fixed 68 bytes
whatever `region_count` says, so a decoder has no length arithmetic to get
wrong; 68 of the 236 authenticated payload bytes (§11) is cheap enough that
saving eight would be a poor trade for a variable-length parse.

A `region_count` above 8 MUST be rejected rather than clamped. Clamping draws a
truncated layout as though it were complete, and a client that cannot show the
real mapping is better off showing none.

**No text crosses the wire.** `pad_bit` names a button and the client renders
its own label, so a Vita may print "L1" where a 3DS prints "L" from the
identical packet, and no encoding, truncation or localisation question ever
reaches the protocol.

**Unreliable, deliberately.** It carries no state the session depends on, and
making it reliable would let a client that never ACKs it be torn down under §9
for failing to answer a message it does not understand — trading a lost layout
for a lost session. A sender wanting delivery confidence SHOULD repeat it (the
reference server sends three copies about 250 ms apart, and repeats them
whenever the mapping changes) rather than request acknowledgement.

### 6.13 Pad-output buttons

`TOUCHMAP.pad_bit` names a button on the VIRTUAL PAD the server creates, in
Xbox convention. Deliberately not the Nintendo-convention §5.7 mask a client
sends: one describes what the PC will see, the other what the device did.

| Bit | Button | Bit | Button |
|---|---|---|---|
| 0 | A | 6 | BACK / SELECT |
| 1 | B | 7 | START |
| 2 | X | 8 | GUIDE / HOME |
| 3 | Y | 9 | L3 |
| 4 | LB | 10 | R3 |
| 5 | RB | 11..15 | reserved, MUST be zero |

### 6.14 Ruling — what "frozen" permits

§6.12 was added on 2026-08-16, after the v1 freeze of 2026-08-09. That is
permitted, and the boundary is worth stating precisely because it will be
tested again.

**Permitted:** allocating an unused `type` and specifying its payload. §4
already requires a receiver to discard an unknown type silently, so a peer
predating the addition behaves correctly by construction — verified, not
assumed: a v1 client completed a full conformance run while a server sent it
TOUCHMAP throughout.

**Not permitted, still a v2 change:** altering any existing constant, offset,
bit position, payload size or normalisation rule; changing the §8/§9
reliability contract; and — the case that nearly slipped through — consuming a
bit a v1 peer is required to mask off. Widening `caps` to admit a new
capability bit is NOT additive: a v1 server does not ignore that bit, it erases
it, so both ends must change together. An earlier draft of §6.12 did exactly
that, and a conformance vector caught it.

`docs/V2-NOTES.md` inventories what the remaining extension space can absorb
under this ruling and what would genuinely require a v2. It is non-normative
and may lag; this section is the ruling.

### 6.15 `KEYBOARD` (0x21) — 56 bytes

**Added after the v1 freeze, additively.** See §6.14 for why that is permitted,
and §6.20 for the audit of these four types against it.

Carries a client's keyboard. Optional in both directions: a client need never
send one, and a server that does not understand it discards it under §4 like
any unknown type. A client MUST NOT send `KEYBOARD` unless a server has
advertised `APAD_KBM_FEATURE_KEYBOARD` in `INPUTCAPS` (§6.19).

| Off | Size | Field |
|---|---|---|
| 0 | 32 | `keys[32]` — held-key bitmap, below |
| 32 | 2 | `event_seq` — total key events generated this session; wraps |
| 34 | 1 | `reserved0` |
| 35 | 1 | `reserved1` |
| 36 | 16 | `events[8]` — 2 bytes each, **oldest at index 0, newest at index 7** |
| 52 | 4 | `client_ticks_ms` — client monotonic clock at sample time, as §5 |

Each event, at `36 + 2*i`:

| Off | Size | Field |
|---|---|---|
| +0 | 1 | `usage` — HID usage. **0 = no event.** 0x01–0x03 reserved |
| +1 | 1 | `flags` — bit 0 `DOWN` (1 press, 0 release); bits 1–7 reserved |

**Keycodes are USB HID Usage Page 0x07 usage IDs, and `keys[]` is a 256-bit
bitmap in which the bit index IS the usage ID.** Usage `u` is byte `u >> 3`,
bit `u & 7`, counting from the least significant bit within a byte — the same
LSB-first rule §5.1 uses for `buttons`.

Three properties earn the bitmap its 32 bytes. It sidesteps the six-key
rollover limit a real HID boot report has, which a device holding
W+A+Shift+Ctrl+Space+two more would otherwise hit. Modifiers are usages
`0xE0`–`0xE7`, so they ride inside the same bitmap and there is no second
modifier byte for two implementations to disagree about. And the mapping from
usage ID to a host keycode is published by every operating system that speaks
USB, so a server translates from a table it can obtain rather than one it must
invent.

**Reserved:** `keys[0]` bits 0–3 (HID defines usages `0x00`–`0x03` as
no-event, ErrorRollOver, POSTFail and ErrorUndefined — conditions, not keys),
`reserved0`, `reserved1`, and `flags` bits 1–7. Zero on send, scrubbed on
receive per §2.

**One further normalisation, and it is mandatory.** An `events[i]` whose
`usage` decodes to 0 — either because it was sent as 0, or because it was
sent as a reserved 0x01–0x03 and normalised — MUST also have its `flags`
forced to 0. A "no event" slot therefore decodes byte-identically whatever a
non-conforming sender put in it, which is what lets a conformance vector
compare two decoded structures for exact equality. `usage` is a control input
under §6.0, not a diagnostic label, so normalising it is correct where
preserving `BYE.reason` is correct.

**Usages the receiver does NOT normalise.** HID 1.12 leaves `0xA5`–`0xAF`,
`0xDE`–`0xDF` and `0xE8`–`0xFF` reserved. A decoder MUST pass them through
unchanged. A full validity mask would freeze one revision of the HID
specification into a decoder that cannot be changed, and an unmapped usage is
already harmless: a server has no table entry for it and emits nothing.

**Sender obligations.** A sender SHOULD emit a `KEYBOARD` whenever `keys[]` or
`event_seq` changes, and SHOULD send three copies about 50 ms apart after the
last key releases. **While any key is held it MUST repeat at 10 Hz or
faster** — see §6.20, which specifies that obligation, the receiver's watchdog
that measures against it, and the release rules that go with both. The repeat
is a MUST rather than a SHOULD because a change-only sender is
indistinguishable from a dead one, and the watchdog cannot tell them apart
without it.

**Staleness.** A receiver MUST apply the per-type window in §6.20 before
anything else in this section.

**Unreliable, deliberately** — see §6.20 for the argument, which is §6.12's
and applies to all four of these types.

### 6.16 `MOUSE` (0x22) — 24 bytes

Relative pointer motion, buttons and wheels. A client MUST NOT send `MOUSE`
unless a server has advertised `APAD_KBM_FEATURE_MOUSE` (§6.19).

| Off | Size | Field |
|---|---|---|
| 0 | 2 | `dx_accum` — wrapping accumulator, **+ = right** |
| 2 | 2 | `dy_accum` — wrapping accumulator, **+Y down** |
| 4 | 2 | `wheel_accum` — wrapping, detents, **+ = away from the user** |
| 6 | 2 | `hwheel_accum` — wrapping, detents, **+ = right** |
| 8 | 2 | `buttons` — held-button bitmask, below |
| 10 | 2 | `event_seq` — total button events this session; wraps |
| 12 | 8 | `events[4]` — 2 bytes each, oldest at index 0 |
| 20 | 4 | `client_ticks_ms` |

Each event, at `12 + 2*i`: `+0` `button` (u8, **0 = no event**, otherwise the
index below), `+1` `flags` (bit 0 `DOWN`, bits 1–7 reserved). `button` above 5
normalises to 0, and — the same "no event decodes byte-identically" rule as
§6.15 — **a slot whose `button` *decodes* to 0, whether it was sent as 0 or
normalised to it, MUST have its `flags` forced to 0 as well.** Stated that way
on purpose: "outside 0..5" would leave a slot sent as `(0, DOWN)` uncovered,
and that is precisely the slot the byte-identity property depends on.

Buttons, used both as the `buttons` bit position and as the event index:

| Bit | Index | Button |
|---|---|---|
| 0 | 1 | LEFT |
| 1 | 2 | RIGHT |
| 2 | 3 | MIDDLE |
| 3 | 4 | BACK |
| 4 | 5 | FORWARD |
| 5–15 | — | reserved, MUST be zero |

**`+Y` is down here, not up.** This is screen space, matching §5.3's touch
convention rather than its stick convention, because every client that will
drive this surface drives it from a touchscreen where the finger and the
pointer must move the same way.

#### Accumulator semantics — normative

**The absolute value of an accumulator carries no meaning.** A receiver
computes motion as `apad_seq_diff(now, previous)` (§6.21) against the last
**accepted** sample, and MUST NOT interpret the value any other way.

Three rules make this correct:

1. **The first accepted `MOUSE` of a session — or the first after the
   receiver's pointer device is created — establishes the baseline and MUST
   produce zero motion.** Without this the opening packet injects a jump of up
   to 32767 counts in an arbitrary direction.
2. **A packet discarded as stale under §6.20's per-type window MUST NOT
   advance the baseline.** Advancing it would turn a reordered packet into a
   backwards jerk and then a compensating forward one. Note the window this
   cites is §6.20's, not §9's: §9's sliding window is `INPUT_STATE`-only by its
   own wording and says nothing about this type.
3. A dropped packet costs nothing. The gap it left is recovered in the next
   accepted packet's diff, because the diff spans it.

This is the same reasoning §9 applies to sequence numbers, using the same
helper, and for the same reason: a naive subtraction is correct until the
accumulator wraps and then it is catastrophically wrong. 16 bits gives an
unambiguous ±32767 counts between consecutive accepted samples; a fast flick
across a phone screen covers on the order of 2000.

Wheels accumulate in **detents**, not pixels or fractions. Smooth or
high-resolution scrolling is not v1.

**Staleness, ring replay, held-repeat and release** are all §6.20's, shared
with §6.15 and §6.17. `MOUSE`'s ring depth is 4. Note that the held-repeat
obligation covers `buttons` only — motion needs no repeat, because an
accumulator that has not changed encodes "no motion" exactly.

### 6.17 `MEDIA` (0x23) — 20 bytes

Transport, volume and navigation controls, for a client acting as a remote. A
client MUST NOT send `MEDIA` unless a server has advertised
`APAD_KBM_FEATURE_MEDIA` (§6.19).

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `held` — control `c` (1..32) is bit `c-1` |
| 4 | 2 | `event_seq` — total control events this session; wraps |
| 6 | 1 | `reserved0` |
| 7 | 1 | `reserved1` |
| 8 | 8 | `events[4]` — 2 bytes each, oldest at index 0 |
| 16 | 4 | `client_ticks_ms` |

Each event, at `8 + 2*i`: `+0` `control` (u8, **0 = no event**, otherwise
1..32), `+1` `flags` (bit 0 `DOWN`). `control` above 32, and any control index
§6.18 leaves unassigned, normalises to 0; and **a slot whose `control`
*decodes* to 0, whether sent as 0 or normalised to it, MUST have its `flags`
forced to 0 as well** — §6.15's byte-identity rule, which a bare "outside
0..32" would leave a slot sent as `(0, DOWN)` outside of.

Bits of `held` for unassigned control indices are reserved and scrubbed.

**Staleness, ring replay, held-repeat and release** are all §6.20's, shared
with §6.15 and §6.16. `MEDIA`'s ring depth is 4.

**Both a held mask and an event ring, because the two answer different
questions.** `held` lets VOLUME_UP be held down for a ramp and self-heals
after loss the way §5's `buttons` does. The ring lets a single NEXT_TRACK tap
— which begins and ends between two samples — survive a dropped packet.

**Its own message type, not a corner of §6.15.** Three reasons, and the first
is the one that would otherwise be discovered too late. A host's keyboard API
and its consumer-control API are frequently different calls with incompatible
arguments, so folding media into the keyboard message puts a conditional in
one handler that exists only because two operating systems disagree. Second, a
client acting purely as a remote should cause exactly one input device to
appear on the host, not a full keyboard it will never press. Third, HID's
Consumer Page usages are 16 bits and sparse, so they cannot be carried by the
bit-index-is-the-usage-ID rule that makes §6.15's bitmap work; keeping them
apart preserves that rule for the message that benefits from it.

### 6.18 Media control index

`MEDIA.held`, `MEDIA.events[].control` and `INPUTCAPS.media_mask` all name a
control by **this index**, which is AtticPad's own dense vocabulary and not a
HID usage. A dense small index is what lets `held` be one fixed-width mask;
a sparse 16-bit usage would force a variable-length held *list* and destroy
the self-healing property that mask has.

The precedent is §6.13's: a wire field whose constants live only in server
code cannot be read by a client, so the vocabulary lives here.

| Idx | Control | Idx | Control |
|---|---|---|---|
| 1 | PLAY_PAUSE | 13 | RECORD |
| 2 | PLAY | 14 | BRIGHTNESS_UP |
| 3 | PAUSE | 15 | BRIGHTNESS_DOWN |
| 4 | STOP | 16 | LAUNCH_BROWSER |
| 5 | NEXT_TRACK | 17 | LAUNCH_MAIL |
| 6 | PREV_TRACK | 18 | LAUNCH_CALC |
| 7 | FAST_FORWARD | 19 | SEARCH |
| 8 | REWIND | 20 | NAV_HOME |
| 9 | VOLUME_UP | 21 | NAV_BACK |
| 10 | VOLUME_DOWN | 22 | NAV_FORWARD |
| 11 | MUTE | 23 | REFRESH |
| 12 | EJECT | 24 | BOOKMARKS |
| | | 25–32 | reserved, MUST be zero |

**Thirty-two slots are allocated now, not twenty-four**, and — this is the
part that actually makes them usable — **a control index assigned in a future
revision MUST NOT be sent unless the server has set that index's bit in
`INPUTCAPS.media_mask` (§6.19).** Reserving the room alone would buy nothing:
indices 25–32 are scrubbed by a receiver that predates their assignment
exactly as §6.14 describes for `caps`, so a sender that simply started using
index 25 would be silently ignored. `media_mask` is the gate that closes that
hole, which is why it is on the wire and not merely implied — the server
states which indices it will honour, and a client sends only those.

**No text crosses the wire.** An index names a control and the client renders
its own label, so a 3DS may print "VOL+" where a phone prints a speaker glyph
from the identical packet, and no encoding, truncation or localisation
question ever reaches the protocol — §6.12's rule, restated because it is the
one most easily forgotten.

### 6.19 `INPUTCAPS` (0x44) — 16 bytes

Tells a client which of §6.15–§6.17 this server will accept, so it can show
the matching UI instead of guessing. Optional in both directions: a server
need never send one, and a client that does not understand it discards it
under §4.

**A client that has not received an `INPUTCAPS` MUST NOT send `KEYBOARD`,
`MOUSE` or `MEDIA`.** Absence is the negotiation: a v1 server, which cannot
send this message and would discard those three anyway, therefore never
receives them.

| Off | Size | Field |
|---|---|---|
| 0 | 4 | `features` — which types the server accepts |
| 4 | 4 | `status` — what exists for this session right now |
| 8 | 4 | `media_mask` — which §6.18 controls this server will drive |
| 12 | 2 | `mouse_rate_hz` — rate the server wants `MOUSE` at; **0 = use the session's `input_rate_hz`** |
| 14 | 2 | `reserved0` |

`features`:

| Bit | Meaning |
|---|---|
| 0 | `KEYBOARD` — the server accepts 0x21 |
| 1 | `MOUSE` — accepts 0x22 |
| 2 | `MEDIA` — accepts 0x23 |
| 3–31 | reserved, MUST be zero |

```c
#define APAD_KBM_FEATURE_KEYBOARD  (1u << 0)
#define APAD_KBM_FEATURE_MOUSE     (1u << 1)
#define APAD_KBM_FEATURE_MEDIA     (1u << 2)
/* bits 3..31 reserved, MUST be zero */
```

`status`:

| Bit | Meaning |
|---|---|
| 0 | `KEYBOARD_READY` — a keyboard device exists for this session now |
| 1 | `MOUSE_READY` |
| 2 | `MEDIA_READY` |
| 3 | `SYNTHETIC` — input is injected into the host's input stream rather than delivered by a device, and some applications may ignore it |
| 4–31 | reserved, MUST be zero |

```c
#define APAD_KBM_STATUS_KEYBOARD_READY (1u << 0)
#define APAD_KBM_STATUS_MOUSE_READY    (1u << 1)
#define APAD_KBM_STATUS_MEDIA_READY    (1u << 2)
#define APAD_KBM_STATUS_SYNTHETIC      (1u << 3)
/* bits 4..31 reserved, MUST be zero */
```

**`mouse_rate_hz` is a request, not an authorisation.** A client MUST NOT
exceed §11's maximum input rate whatever this field says; a client that reads a
value above that ceiling MUST send at the ceiling instead.

**This is an obligation on the client's send rate, not a decode
normalisation.** A decoder MUST preserve the value verbatim — it is not in
§6.0's normalisation list, it has no reserved numeric range for §2 to scrub,
and clamping it would destroy a server's ability to observe that it asked for
something impossible. The field exists so a server can
ask for *less* than the session rate — a pointer rarely needs 125 Hz — not so
it can raise a limit §11 sets.

`features` and `status` are separate because they answer different questions
at different times: `features` is what the server will accept and is known
before anything is created, `status` is what exists at this instant and
changes as devices are created on first use.

The payload is a fixed 16 bytes whatever `features` says, so a decoder has no
length arithmetic to get wrong — §6.12's argument, and the reason that section
spends 68 bytes on eight region slots it may not use.

**`SYNTHETIC` is a bit, not a sentence.** It names a condition and the client
writes its own warning, for the same reason §6.13 puts a button index on the
wire instead of a label.

#### Delivery

**Not before the `ACK` that discharges `WELCOME`.** A server MUST NOT send
`INPUTCAPS` earlier. Until that `ACK` arrives the server has no evidence the
client exists at all, and every copy sent into that gap is spent on a session
that may never open.

**On an authenticated session, not before the first datagram whose tag
verifies.** This rule exists because the obvious schedule silently destroys the
feature on the exact hardware this protocol was written for. §10.2 requires a
client to derive its key **after** acknowledging `WELCOME`, and budgets about
one second of PBKDF2 on a 67 MHz ARM9. A server that starts a 0/250/500 ms
schedule on the `ACK` therefore lands **all three copies inside the window
where the client does not yet hold the key**; each fails §3.1 check 7 and is
discarded, there is no re-request message, and a client that repeats on
`status` change alone never hears another. The session runs correctly and the
whole facility is invisible. So: where `WELCOME` set `AUTH_REQUIRED`, the
server MUST wait for a client datagram whose tag verifies — which it already
observes — before arming the schedule.

**Schedule.** Three copies about 250 ms apart when armed, re-armed whenever
`features` or `status` changes, and a slow repeat — **at least one copy every 5
seconds while the session is open**. The slow repeat is 16 bytes per 5 seconds
and it is what makes the facility recoverable rather than one-shot: any
future arming bug, or a burst that swallows all three copies, costs a few
seconds instead of the session.

**Latest wins.** A client MUST treat the most recently accepted `INPUTCAPS` as
current, applying §6.20's per-type window so a reordered copy cannot revive
stale contents. **If a `features` bit clears, the client MUST stop sending that
type**, and MUST first release everything it holds for that facility under
§6.20's release rule — otherwise the last thing the server saw held stays held
with nothing left able to lift it.

### 6.20 Shared rules for §6.15–§6.19

These four types share a delivery model, and the rules it needs are written
once here rather than three times with three chances to drift.

#### Additivity under §6.14

§6.15–§6.19 were added on 2026-08-25, after the v1 freeze. The audit §6.14
demands, for each of the four types:

- A new `type` value is allocated and its payload specified. Nothing else.
- No existing constant, offset, bit position, payload size or normalisation
  rule changes.
- The §8/§9 reliability contract is unchanged. The per-type windows below are
  **new receive semantics for new types**, specified here; they neither amend
  §9 nor depend on amending it (see below).
- **`caps` (§6.3) is untouched.** No capability bit is defined for keyboard,
  mouse or media, and none may be: §6.14 rules that widening `caps` is a v2
  change because a v1 server *scrubs* a reserved capability bit rather than
  ignoring it. `INPUTCAPS` exists precisely so that bit is never needed, and
  the direction is reversed on purpose. A `caps` bit would have required a v1
  server to **preserve** information §2 requires it destroy. Absence of an
  `INPUTCAPS` is information a v1 server produces by doing nothing at all —
  which is why the reversal solves the problem rather than relocating it.

**What a v1 peer actually does with one.** It fails **§3.1 check 5** — `type`
does not appear in its §4 table — and §3.1 requires it to stop at the first
failure and **discard silently**, which is also what §4 requires. Check 6 never
runs, and that matters: check 6's action is *reject with `ERROR` code 6*, so a
peer that reached it would answer an unrecognised type with an error datagram
rather than silence — the reflection behaviour §8 spends a paragraph
preventing. A check-5 discard does not complete §3.1 and therefore does not
refresh §8 liveness; harmless here, because no session depends on these types
to stay alive.

#### Per-type staleness windows

**A receiver MUST keep a separate sliding window for each of `KEYBOARD`,
`MOUSE`, `MEDIA` and `INPUTCAPS`, and MUST discard a datagram of one of those
types whose header `sequence` is not `apad_seq_newer()` (§9) than the newest
datagram of the same type it has already accepted.** The four windows are
independent of one another.

**They are also independent of §9's window, which is `INPUT_STATE`-only.** §9
says a receiver "MUST discard any `INPUT_STATE` older than the newest one
already seen" — `INPUT_STATE` compared against `INPUT_STATE`. §9 separately
notes that every datagram in a session consumes that direction's sequence
counter and that a peer **may** therefore track it as one monotonic series;
that is a MAY, and a receiver that took it as licence to run one shared window
would let a `KEYBOARD` at sequence 101 shadow an `INPUT_STATE` still in flight
at 100, discarding live stick data as "stale" when it was merely interleaved.
The failure is silent and looks exactly like packet loss. **Per-type windows,
and nothing shared.**

Without these windows §6.16's "a stale packet MUST NOT advance the baseline"
would name a rule that does not exist, a reordered `MOUSE` would jerk the
pointer backwards, and a reordered `KEYBOARD` would apply an old snapshot and
un-press keys the user is still holding.

#### Unreliable, and why that is forced rather than chosen

Making any of the four RELIABLE would require the peer to `ACK` it. A peer that
predates the addition discards it under §4 and so never `ACK`s — and §9's
retransmit ladder would then tear the session down for failing to answer a
message the peer is behaving correctly by ignoring. That trades a lost
advertisement, or a lost keystroke, for a lost session. §6.12 made this
argument for one message; it holds for every additive type, in both
directions, and should be treated as the default for anything added under
§6.14.

#### What unreliability costs, and how these payloads pay for it

A dropped state snapshot is harmless — `keys[]`, `buttons` and `held` are
complete truths, so the next packet repairs them exactly as §9 describes for
`INPUT_STATE`. A dropped *edge* is not: a keystroke or a NEXT_TRACK tap that
began and ended between two samples has no representation in a state snapshot
at all. Hence the two mechanisms these payloads share — wrapping accumulators
(§6.16) for continuous quantities, and a fixed-depth event ring for discrete
transitions.

#### The event ring — layout

`event_seq` counts **every event ever generated on this session for that
type**, starting at 0 and incremented once per event, wrapping at 2^16. An
event's **ordinal** is the value `event_seq` holds *after* that event has been
counted, so the first event of a session has ordinal 1 and `event_seq` equals
the ordinal of the newest event the datagram carries.

**The ring is right-aligned, and this is normative because both alignments
satisfy "oldest first" and they disagree byte for byte.** For a ring of depth
`D`, slot `i` (0-based) carries the event with ordinal
`event_seq - (D - 1) + i`. The **newest** event is therefore always at slot
`D - 1`, and slots whose ordinal falls before the session's first event are
transmitted zeroed. Front-packing — newest immediately after the last real
event, tail zeroed — is **not** conformant; gap replay depends on knowing
where the newest event is without counting.

A slot whose `usage`, `button` or `control` decodes to 0 is "no event" and MUST
be skipped during replay. It is not an event with code zero.

#### The event ring — receiver algorithm

Normative. `last_applied` is the receiver's record of the highest `event_seq`
it has acted on for that type.

1. **First accepted message of a type**: set `last_applied = event_seq`, apply
   **no** events, and apply the snapshot as state. The ring's contents predate
   the receiver's interest in them. This mirrors §6.16's baseline rule and
   exists for the same reason.
2. Otherwise let `g = apad_seq_diff(event_seq, last_applied)` (§6.21).
   - `g <= 0` — nothing new, or a duplicate. Replay no events.
   - `1 <= g <= D` — replay slots `D - g` through `D - 1`, in ascending order,
     skipping "no event" slots.
   - `g > D` — events have been lost that the ring cannot carry. Replay all `D`
     slots in ascending order. A receiver **SHOULD** make the overflow
     observable through whatever diagnostic channel it has, and **MUST NOT**
     treat step 3's reconcile as having absorbed it.

     That last clause is the whole point, and it is the mistake the wording
     invites. The reconcile restores *held state*; it cannot restore an
     *edge*. A key pressed and released entirely inside the gap leaves the
     snapshot byte-identical on both sides, so after the reconcile nothing
     downstream can tell that a keystroke was dropped — the state is right and
     the event is simply gone. Overflow is the only evidence it happened, and
     the condition is worth reporting: a sender exceeding the ring depth is
     either losing a burst of datagrams or generating events faster than it
     ships them, and both are real problems a silent decoder would hide.
3. **In every case, including `g <= 0` and including overflow, reconcile held
   state against the snapshot**: emit whatever presses and releases make the
   receiver's view of `keys[]` / `buttons` / `held` equal the one in the
   datagram.
4. Set `last_applied = event_seq`.

**Step 3 runs unconditionally, and that is the whole convergence argument.** A
sender that changes its snapshot without advancing `event_seq`, or advances
`event_seq` by more than the events it actually shipped, is non-conforming —
but under any rule that reconciled only on overflow, the receiver would
disagree with it *permanently*. Reconciling every time makes every such
divergence last exactly one packet. **The snapshot is always the authority;
the ring only adds back what a snapshot cannot express.**

Depth is 8 for `KEYBOARD` and 4 for `MOUSE` and `MEDIA`. At the 10 Hz repeat
floor below, 8 slots cover a gap of 800 ms.

#### Encoding an out-of-range value

**A sender MUST NOT emit an out-of-range event code** — a `KEYBOARD.events[]`
`usage` of 0x01–0x03, a `MOUSE.events[]` `button` above 5, or a
`MEDIA.events[]` `control` above the assigned range. Those are sender errors.

**An encoder is not required to normalise them, and SHOULD NOT.** The
normalisation rules in §6.15–§6.17 are *receive-side*, following §6.0's
treatment of `battery` and `LED.player_index`: a receiver must turn an
out-of-range value into a defined one before it reaches anything that acts on
it, and that is where the obligation ends. An encoder that silently corrected
the same value would hide a sender's bug from the only party positioned to
notice it — that sender's own test suite.

**Reserved *bits* are different: they are masked on both sides**, because §2
requires them zero on send and ignored on receive.

The asymmetry is deliberate, it is the one §6.0 already establishes, and it is
restated here because it is exactly the kind of rule two independent
implementations otherwise resolve differently and only discover at the vector
stage.

#### Held state — repeats, watchdog, and release

**Repeat.** While a sender holds anything in a facility — any bit set in
`keys[]`, in `MOUSE.buttons`, or in `MEDIA.held` — it **MUST** send that type
at **10 Hz or faster**. This is a MUST, not a SHOULD, because a change-only
sender and a sender that has died are otherwise indistinguishable, and the
watchdog below has nothing to measure.

**Watchdog.** A receiver that believes something is held for a facility and has
accepted no datagram of that type for **1000 ms MUST release everything it
holds for that facility.** 1000 ms is ten missed repeats at the mandated floor,
so a conforming sender never trips it.

**Release on teardown.** A receiver **MUST release everything it holds for all
three facilities before a session ends**, for any reason — `BYE` (§6.5), the
§11 idle timeout, or a revoked slot — and before it destroys or detaches
whatever it injects into. A sender **MUST** release before it stops sending:
on leaving the mode, on a `features` bit clearing (§6.19), and before `BYE`.

**Why this is specified rather than left to implementations.** A physical
keyboard releases its keys when it is unplugged. An injected one does not:
where `INPUTCAPS.status` reports `SYNTHETIC` there is no device to unplug, and
a held Ctrl simply stays held on the user's desktop until something explicitly
lifts it. The one failure this whole section exists to prevent is a client
dying mid-chord and leaving a modifier down with nothing able to release it.

### 6.21 Wrap-safe difference — `apad_seq_diff`

§6.16's accumulators and §6.20's event ring both compute a **signed distance**
between two 16-bit values, which §9's `apad_seq_newer()` cannot express — it
answers "newer?" and this needs "by how much, and which way?". The function is
therefore specified here, additively, rather than by amending §9's frozen
helper block.

```c
/* signed distance from b to a, correct across wrap; result in [-32768, 32767] */
int apad_seq_diff(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)((unsigned)a - (unsigned)b);
    if (d < 0x8000u) {
        return (int)d;                        /* 0 .. 32767   */
    }
    return -(int)(uint16_t)(0x10000u - d);    /* -32768 .. -1 */
}
```

**Normative.** Every accumulator delta and every `event_seq` gap in every
implementation MUST route through it, and it carries §9's two deliberate
details for the same reasons. **The arithmetic is unsigned throughout**: the
shorter `(int16_t)(a - b)` is what most references show, but converting an
out-of-range value to a signed type is *implementation-defined* under C99
6.3.1.3p3 — correct on every two's-complement target, yet a conforming
compiler is permitted to raise a signal. And it is an **extern function in
`core/src/seq.c`**, not a `static inline` in a header, so one copy is shared by
every platform and `nm` can prove there is only one.

Worked values, which §13 requires as vectors: `(0x0005, 0xFFFB)` is `+10`;
`(0xFFFB, 0x0005)` is `-10`; `(0x8000, 0x0000)` is `-32768`; `(0x7FFF,
0x0000)` is `+32767`; `(0x0000, 0x0000)` is `0`.

### 6.22 Reserved — `0x24` `TEXT`, and what the `0x2x` range means

`0x20`–`0x2F` is the client→server input range. Four of its sixteen slots are
spent (`INPUT_STATE`, `KEYBOARD`, `MOUSE`, `MEDIA`). This section records what
the range means and reserves one more slot, because a range meaning is cheap to
respect and expensive to reclaim.

**`0x24` is reserved for `TEXT` and MUST NOT be allocated to anything else.**
Nothing is specified here beyond the reservation; the rest of this section is
the reasoning, and is not normative.

**Why a separate type is the right shape, rather than widening §6.15.** A HID
usage names a *physical key position*, not a character. What that position
produces is decided by the keyboard layout the host has configured, which the
client cannot see. That is exactly right for a game — W is the key above S
wherever you are — and exactly wrong for typing, because a client whose text
arrives from a phone IME or a system soft keyboard has **characters and no key
positions at all**, and must work backwards through an assumed layout to
invent them. §6.15 therefore carries an unavoidable assumption: text sent as
usages is correct only when the host's layout matches the one the client
guessed. `TEXT` would remove the guess by sending what the user actually meant.

It cannot be folded into §6.15 for the same reason §6.17 could not: the host
call is different in kind. A keystroke names a position and is a press and a
release; a character names a codepoint and has no press or release at all.

**The shape it would take**, when someone builds it: UTF-8, with §6.20's
`event_seq` ring discipline applied to codepoints rather than transitions, so
a dropped datagram costs at worst a repeat rather than a hole in a sentence.
State-snapshot self-healing does not apply — a character is purely an edge —
so the ring is the only mechanism available and its depth is the whole design.
The 236-byte authenticated payload (§11) bounds a single datagram's worth;
anything longer is a sequence of them, which the ring already handles.

**The cost is on the server, and it is uneven.** Injecting a codepoint the
host has no key for is a solved problem on some platforms and an unsolved one
on others: a Windows backend using `SendInput` already has a native path for
it, whereas `uinput` has no notion of a character and would need a scratch
keycode remapped per codepoint, or a generated keymap handed to the
compositor. Whoever picks this up should establish that the hard half works
before specifying the easy half — the same order §6.14's ruling recommends,
and the opposite of the order that is tempting.



---

## 7. Discovery

The server MUST implement all three tiers. A client implements whichever it
can, and MUST implement tier 3.

**Tier 1 — mDNS.** Service `_atticpad._udp.local`, TXT records for name,
version, and free slots. Advertise-only responder (`DESIGN.md` D4). If the 5353
bind fails, tier 1 is disabled and the server UI MUST say so.

**Tier 2 — UDP broadcast.** `DISCOVER` to `255.255.255.255:21100`, `ANNOUNCE`
unicast back to the source.

A server **MUST NOT** send an `ANNOUNCE` to a source address that cannot
legitimately be one: a broadcast address (`255.255.255.255`, or a
subnet-directed broadcast it can identify), a multicast address
(`224.0.0.0/4`), `0.0.0.0`, or a loopback address when the request did not
arrive on the loopback interface. It MUST instead discard the `DISCOVER`
silently.

The reasoning is §8's, applied here: `DISCOVER` is 12 bytes and `ANNOUNCE` is
52, so an attacker who spoofs the source address of a single small datagram
gets roughly a 4× amplification aimed wherever it likes — and tier 2 requires
the server's socket to have broadcast enabled, so a spoofed broadcast source
is answered *to the whole segment*. That is a larger multiplier than the
`ERROR` path §8 already guards, on a message type that is unauthenticated by
design and answered before any session exists.

Unlike §8's `ERROR` rate limit, this is a **MUST NOT** rather than a SHOULD,
and it is not a rate limit. Discovery is a legitimate burst path — several
clients may reasonably `DISCOVER` at once when a network comes up — so
throttling it would break the feature, whereas refusing to answer an address
that could never have sent the request costs a conforming client nothing.

**Tier 3 — manual IP entry.** A first-class feature on every client, not a
debug fallback. It is the only path that works under AP isolation, on guest
networks, and across subnets. The server MUST display its own IP prominently
at all times.

## 8. Session lifecycle

```
Client                                    Server
  |-- DISCOVER (broadcast) ----------------->|   (or mDNS, or skipped)
  |<------------------------ ANNOUNCE -------|
  |-- HELLO -------------------------------->|
  |<------------------------- WELCOME -------|   session_id, slot, nonce
  |-- ACK (for WELCOME) -------------------->|   REQUIRED — see §9
  |-- INPUT_STATE (60-125 Hz) -------------->|
  |-- PING (1 Hz) -------------------------->|
  |<---------------------------- PONG -------|
  |<-------------------------- RUMBLE -------|
```

**Pairing happens BEFORE this sequence, not inside it.** An earlier revision
annotated the `HELLO` line with "[server shows PIN if unpaired]", which is not
implementable: `WELCOME` is reliable, §9 exhausts its retransmits at t ≈ 2300 ms
and §11 tears the session down at 3000 ms, and no human reads six digits off one
screen and types them into another inside that budget. The pairing window is
opened by the user at the server (§10), the secret reaches the client out of
band, and only then does the client send `HELLO`. `ANNOUNCE.pairing_required`
(§6.2) is the signal that tells a client to obtain a secret first.

A client that reaches `WELCOME` with `AUTH_REQUIRED` set and has no secret
MUST NOT stall inside the handshake waiting for the user. It sends the `ACK`
§9 requires, lets the session lapse, prompts at leisure, and reconnects with a
fresh `HELLO`. The cost is one pad slot held for three seconds.

- `session_id` is 0 in `DISCOVER`, `ANNOUNCE` and `HELLO`; non-zero everywhere
  after `WELCOME`.
- A server receiving a packet with an unknown non-zero `session_id` MUST reply
  `ERROR` code 7 and MUST NOT create a session — **except for `INPUT_STATE`,
  which MUST be discarded silently.** Replying to an unknown-session
  `INPUT_STATE` would emit a 76-byte `ERROR` for each of up to 125 datagrams
  per second, turning the server into a reflection amplifier pointed at
  whatever address the datagrams claim to come from, and a self-inflicted
  denial of service on a busy LAN. A server SHOULD additionally rate-limit
  `ERROR` output to at most 10 per second **per source IP address, counted
  without regard to source port** — an attacker who rotates the spoofed source
  port would otherwise get a fresh allowance per datagram and keep the
  amplification open. **The rate limit takes precedence over the MUST**: a
  server MAY drop an `ERROR` it would otherwise be required to send rather than
  exceed the limit. Being a well-behaved participant on the network outranks
  answering a peer that is, by construction, not one.
- The client-side counterpart, added 2026-08-11 after it froze a real console:
  a client that receives an `ERROR` code 7 **while it holds a non-zero
  `session_id`** MUST, on an unauthenticated session, treat the session as
  torn down (and MAY reconnect with a fresh `HELLO`). Note the code-7 `ERROR`
  itself arrives with `session_id` 0 — the server has no session to stamp on
  it (§9's outside-any-session rule) — so the condition is the CLIENT's
  state, never a header comparison. The `ERROR` proves the server no longer
  knows the session, so nothing it carries is evidence of liveness — a client
  that instead lets it refresh the idle timer never times out: the server's
  rate-limited `ERROR` replies arrive faster than the 3-second timeout
  expires, and the dead session is sustained indefinitely. This rule takes
  precedence over the "Liveness and freshness are separate" bullet below.
  On an **authenticated** session the same `ERROR` is necessarily untagged —
  the server destroyed the key when it forgot the session — and therefore
  spoofable by anyone on the LAN: the client MUST NOT act on it, and §10's
  signed-liveness rule (same ruling, same day) delivers the teardown instead:
  untagged datagrams no longer refresh the idle timer, so the dead session
  reaches the 3-second teardown below on its own. During the handshake the
  client's datagrams carry `session_id` 0, so a code-7 `ERROR` received then
  can only be a stale answer addressed to a previous session, and MUST NOT
  abort the handshake.
- A session with no packet received for **3 seconds** MUST be torn down. A
  client MUST send at least one packet every 100 ms (the 10 Hz keepalive
  floor), even when nothing changed.
- **Liveness and freshness are separate.** Any datagram that passes §3.1
  refreshes the idle timer — on an `AUTH_REQUIRED` session past its key
  window, qualified by §10's signed-liveness rule: only a datagram whose tag
  verifies refreshes it — including an `INPUT_STATE` that §9 then discards as
  stale. Only the contents are discarded, never the evidence that the peer is
  alive — otherwise a burst of reordered packets would tear down a live
  session.

## 9. Reliability and wrap-safe arithmetic

**What discharges a reliable message.** A reliable message stops retransmitting
when it is *discharged*, which happens in exactly one of two ways:

1. an explicit `ACK` echoing its sequence number, or
2. receipt of the message this specification defines as its **direct answer**.

`WELCOME` is the direct answer to `HELLO`, and is the only such pair in v1.

**Duplicates.** Retransmission means resending the *same* message, so:

- A retransmission **MUST** be byte-identical to the original, including its
  `sequence`. A sender **MUST NOT** allocate a new sequence number for a retry.
- A peer that receives a duplicate of a request it has already answered **MUST**
  retransmit its original answer verbatim, and **MUST NOT** generate a fresh
  one. Generating a new answer allocates a new sequence, so the `ACK` already
  in flight for the first answer no longer matches, and the session dies at
  t = 2300 ms with input flowing normally and nothing in any log to explain it.
- A receiver **MUST** `ACK` **every copy** of a reliable message it receives,
  including duplicates of one it has already processed and acknowledged. (On
  an `AUTH_REQUIRED` session past its key window, §10 qualifies this: an
  untagged reliable message is ignored, not acknowledged — except a
  byte-matched `WELCOME` retransmission, which is re-ACKed per §10's
  exception.) An
  `ACK` is not a statement about the first copy; it is the answer to the
  datagram in hand. A receiver that acknowledges only the first copy kills the
  session on the first lost `ACK`.

These three rules exist because the failure they prevent is invisible in a lab
and routine on a real network: it needs only one dropped `ACK`, and it presents
as a session that dies about two seconds in for no apparent reason.
Every other reliable message — `BYE`, `RUMBLE`, `LED`, `STATUS` — has no
defined answer and therefore **MUST** be acknowledged explicitly.

The asymmetry is deliberate and it is load-bearing in both directions: a
client MUST NOT wait for an `ACK` of its `HELLO` (none is coming, and waiting
kills the session at t = 2300 ms), and a client **MUST** send an explicit
`ACK` for the `WELCOME` it receives, or the server will retransmit it four
times and then close the session as failed — even while `INPUT_STATE` is
flowing normally and everything appears healthy.

- Reliable messages (`RELIABLE` set) retransmit with **doubling gaps** of 100,
  200, 400 and 800 ms. Measuring from the original send at t=0, the four
  retransmits go out at t = 100, 300, 700 and 1500 ms; if no `ACK` has arrived
  by t = 2300 ms the session fails. These are gaps between attempts, not
  absolute deadlines — a sender and receiver that read this differently produce
  a timing disagreement indistinguishable from packet loss. `ACK` echoes the
  acknowledged sequence.
- **`INPUT_STATE` MUST NOT be retransmitted.** Input state is idempotent and
  self-healing: the next packet 8–16 ms later carries the complete current
  truth.
- The receiver keeps a sliding window and **MUST discard any `INPUT_STATE`
  older than the newest one already seen.** A late packet is worse than no
  packet — applying it moves the stick backwards, which the user experiences
  as a stutter.

**Every datagram sent inside a session consumes that direction's sequence
counter** — `INPUT_STATE`, `PING`, `PONG`, `ACK` and `ERROR` alike, with no
exceptions. A peer may therefore track the other direction's sequence as a
monotonic (wrapping) series. A datagram sent outside any session — an `ERROR`
answering an unknown `session_id`, or a discovery response — carries
`sequence` 0. Retransmissions reuse the original's sequence and do not consume
a new one.

**Sequence numbers wrap, and comparison MUST handle it.** A 16-bit sequence at
125 Hz wraps every ~8.7 minutes. A naive `a > b` freezes the client at every
wrap and passes every test shorter than nine minutes.

```c
/* true if a is strictly newer than b, correct across wrap */
int apad_seq_newer(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(a - b);
    return d != 0u && d < 0x8000u;
}

/* true if a is strictly after b, correct across wrap at 2^32 (~49.7 days) */
int apad_time_after(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    return d != 0u && d < 0x80000000u;
}
```

Two deliberate details in that code, because eight ports will copy it verbatim:

- **The arithmetic is unsigned throughout.** The shorter `(int16_t)(a - b) > 0`
  is what most references show, but converting an out-of-range value to a
  signed type is *implementation-defined* under C99 6.3.1.3p3 — correct on
  every two's-complement target, yet a conforming compiler is permitted to
  raise a signal. The unsigned form is strictly defined and gives identical
  results for every input pair.
- **They are extern functions in `core/src/seq.c`, not `static inline` in a
  header.** One copy is shared by every platform and `nm` can prove there is
  only one. A call per comparison at 125 Hz is not measurable.

Both are normative. Every tick and sequence comparison in every implementation
MUST route through them.

## 10. Authentication

Because the DS supports only open and WEP networks (`DESIGN.md` §2.4), the link
layer provides nothing and the protocol carries its own authentication.

- The server generates a **shared secret** during an explicit, user-initiated
  pairing window, and the user carries it to the client out of band.
- Both sides derive a session key with **PBKDF2-HMAC-SHA256, 10,000
  iterations**, salted with the 16-byte `server_nonce` from `WELCOME`.
- Every packet after `WELCOME` sets `AUTHENTICATED` and carries an 8-byte
  truncated HMAC-SHA256 tag appended after the payload — **except the `ACK`
  that discharges `WELCOME` itself, which MAY be unauthenticated.** That one
  exemption is forced: `WELCOME` is the datagram carrying the `server_nonce`
  used as the PBKDF2 salt, so a client provably cannot hold a key until it has
  parsed the very message its `ACK` is acknowledging. During that window —
  `WELCOME` sent, key not yet derived — the untagged `ACK` discharges the
  `WELCOME` retransmit and refreshes §8 liveness exactly as §10.2 describes;
  the ruling below does not apply to it. The MAY spans ONLY that keyless
  window: a client that holds the session key MUST tag every `ACK` it sends,
  including a re-ACK of `WELCOME` — a server past its own window is required
  by this ruling to ignore an untagged one, so a keyed client exercising the
  MAY discharges nothing and dies by §9 retransmit exhaustion with input
  flowing, the exact failure §10.2 exists to prevent.
  **Ruled 2026-08-11 — signed liveness.** Once a session with `AUTH_REQUIRED`
  is past that window — each side exits on its OWN evidence, the client the
  moment it holds the derived key, the server the moment a tagged datagram
  has verified —, an unauthenticated datagram MUST NOT be acted
  on and MUST NOT refresh §8 liveness. Exactly one §9 obligation survives as
  an exception, and it is inert to forgery: a retransmitted `WELCOME`, which
  §9 requires to be byte-identical — the client MUST re-ACK a copy that
  byte-matches the `WELCOME` it accepted, MUST ignore any unauthenticated
  `WELCOME` that does not match, and the re-ACK refreshes nothing. Beyond
  that, only a datagram whose tag verifies is evidence the peer is alive: an
  attacker who can inject untagged datagrams (on the DS's open network,
  anyone in range) could otherwise sustain a dead session forever — or,
  combined with §8's `ERROR` code 7, keep a client livelocked against a
  server that has already forgotten it. All legitimate steady-state traffic
  on such a session is tagged, so nothing real is lost; a session whose peer
  stops producing verifiable datagrams reaches §8's 3-second teardown
  naturally. The tag covers the
  **entire datagram, header included, with the 8 tag bytes zeroed** during
  computation. Comparison MUST be constant-time.
- The PIN itself MUST NEVER appear on the wire.
- Replay is prevented by the sequence window in §9 and, for the §6.15–§6.19 types, by §6.20's per-type windows.

### 10.1 The secret's length depends on how it reaches the client

The secret is an opaque byte string to everything below this section. It never
appears on the wire (see below), so its length is not a wire-format property
and implementations MAY use either of these:

| Channel | Secret | Why |
|---|---|---|
| **Typed** | **6 decimal digits** | A human reads it off one screen and types it on another, sometimes with a D-pad. Six digits is the practical ceiling for that, and it is what this specification originally mandated. |
| **Scanned** | **A random token of at least 16 characters** | A camera does the typing, so length is free. |

Both derive the session key identically: `PBKDF2-HMAC-SHA256(secret,
server_nonce, 10000, 32)`. A conforming implementation MUST accept a secret of
any length from 6 to 64 bytes and MUST NOT assume six digits.

**The secret is printable ASCII (0x21–0x7E), 6 to 64 bytes, and MUST NOT
contain a NUL.** It is passed to the derivation as a NUL-terminated C string
in every implementation this specification is written against, so an embedded
NUL is not representable and a secret containing one is non-conforming. It is
otherwise opaque: the derivation gives it no structure and never parses it.

**Token alphabet.** A generated token MUST use this 32-character alphabet and
no other:

```
23456789ABCDEFGHJKLMNPQRSTUVWXYZ
```

`0`/`O` and `1`/`I`/`L` are absent deliberately. A token is normally scanned,
but it MUST remain typeable — a camera-less client is not second-class, and a
user reading a token aloud or copying it by hand must not have to guess
between glyphs that look alike in the font they happen to be looking at.

Fixing the alphabet also fixes the arithmetic: 32 symbols is exactly 5 bits
each, so a 16-character token is 80 bits and a 20-character one is 100. A
generator MUST select symbols by **rejection sampling**, never by reducing a
random byte modulo 32 — the modulo is unbiased only when the alphabet divides
256, which makes it a habit that silently produces biased output the moment
the alphabet changes size.

**This is not a wire-format change and does not require protocol v2.** No
struct, constant, offset, bit position or payload size differs; the datagrams
carry the same 8-byte tags they always did. The only thing that changes is how
much entropy went into the key, which is invisible to the receiver by
construction.

The reason for the split is stated plainly below: six digits is brute-forceable
offline from a captured handshake, and no iteration count fixes that. A scanned
token removes that ceiling **for scanning users only**. It does not repair the
typed path, and a device with no camera is not second-class — manual entry
remains mandatory (§7) and the 6-digit PIN remains fully conforming.

### 10.2 Derive AFTER acknowledging `WELCOME`, never before

A client MUST send the `ACK` that discharges `WELCOME` **before** it derives
the session key, and SHOULD send it immediately.

This is a timing requirement, not a style preference. 10,000 PBKDF2 iterations
are not free: measured at **17 ms on x86-64 at `-O2` and 86 ms at `-O0`**, and
`DESIGN.md` §5.10 budgets **about one second on a 67 MHz ARM9**. `WELCOME` is
reliable, so §9's schedule is already running when it arrives — retransmits at
100, 200, 400 and 800 ms, and the session fails at t ≈ 2300 ms.

A client that derives first therefore spends its entire derivation inside that
schedule. On a fast device it fits and nothing is visibly wrong. On a
constrained one it does not, and the session dies as `APAD_CLOSE_RETX_FAILED`
**with input never having flowed** — a handshake that fails for no visible
reason, on exactly the hardware this protocol exists for, and where §9 already
warns that the symptom is "indistinguishable from packet loss".

The order that always works:

```
receive WELCOME  ->  send ACK immediately, unauthenticated (§10)
                 ->  derive the session key
                 ->  install it; authenticate everything after
```

§10's exemption for that one `ACK` exists precisely to make this order legal.
A client that cannot send an unauthenticated `ACK` cannot implement it.

**Servers MUST NOT assume a fast client.** A gap of a second or more between
the `ACK` and the first authenticated datagram is conforming behaviour, not a
stalled peer; the §11 idle timeout is the only deadline that applies, and the
`ACK` itself has already refreshed it.

### 10.3 The pairing URI

A server MAY present its address and the pairing secret together as a single
string, so a camera-equipped client can obtain both in one scan instead of the
user typing an address and then a secret. The encoding is normative because
three independent implementations — the server, and every camera client — must
agree on it byte for byte.

```
atticpad://<ipv4>:<port>/?v=1&s=<secret>
```

| Part | Rule |
|---|---|
| scheme | `atticpad`, lowercase |
| host | an IPv4 dotted-quad literal. **No hostnames** — the protocol is IPv4-only (§1) and a client that cannot resolve DNS must not be handed a name it cannot use. |
| port | decimal, 1–65535, REQUIRED. Present even when it is 21100, so a parser never has to know the default. |
| `v` | the URI payload version, REQUIRED, `1` for this document |
| `s` | the secret — see the restriction below |

**`v` is not the protocol version and MUST NOT be confused with it.** It
versions this string only. That separation is the point: the payload can gain
a field later without touching a wire format that froze at v1, and a client
reading `v=2` knows to say "this server is newer than I am" rather than
guessing.

**A URI carries only a generated secret.** §10.1 permits any printable ASCII
from 0x21 to 0x7E, and that set includes `&`, `=`, `#` and `%`, which are
structural inside a query string. A secret containing one would silently
corrupt the URI. Rather than introduce percent-encoding — which every parser
here would have to implement, including one on a 67 MHz ARM9 with no
allocation — `s` is restricted at the point where it is cheap:

> The `s` field MUST contain only characters that §10.1's generators can
> produce: the decimal digits `0`-`9`, and the 32-character token alphabet.
> Neither set contains a character that is structural in a URI, so no escaping
> is ever required.

A secret outside that set is still a valid secret under §10.1 — it simply
cannot travel in a URI, and MUST be conveyed by typing instead. A server MUST
NOT emit a URI it cannot encode this way.

Parser requirements:

- A parser MUST reject a `v` it does not recognise, and MUST NOT attempt a
  partial interpretation.
- A parser MUST reject a URI in which `v` or `s` appears **more than once**. A
  repeated key is not an unknown key: accepting the first, or the last, is the
  kind of disagreement that lets two implementations read one QR differently,
  and it is exactly what an attacker would reach for.
- The `/` before `?` is REQUIRED. It is in the grammar above, and making it
  optional buys nothing while giving two parsers something to differ about.
- A parser MUST ignore query keys it does not recognise, so a later revision
  can add one without breaking existing clients.
- A parser MUST NOT require the query keys in any particular order.
- The whole URI MUST be ≤ 128 bytes. With a 64-byte secret the longest legal
  form is about 103, so this bounds every buffer without excluding anything
  §10.1 permits.

The secret appears **in the URI, never on the wire** — this is the out-of-band
channel §10 requires, and a QR code is one way to carry it. It follows that a
displayed URI is exactly as sensitive as a displayed PIN: it MUST be shown
only during an open pairing window, and a server MUST NOT write it to a log.

A client that cannot scan is not second-class. §7 tier 3 keeps manual entry
mandatory, and every field above is typeable — which is why §10.1 fixes an
alphabet with no ambiguous glyphs.

**What actually provides the security** — not the iteration count:

1. The PIN is valid for **120 seconds** after the user initiates pairing, and
   never otherwise.
2. **Five failed attempts** invalidate the PIN and generate a new one.
3. The server binds LAN interfaces only and never routes.

**Stated plainly:** an attacker already on the LAN who captures the pairing
handshake can brute-force a **6-digit PIN** offline and recover the session
key. No feasible iteration count changes that. On the open or WEP network the
DS requires, that attacker is anyone in range. This is a toy-grade threat
model: it stops a housemate, not an adversary. User-facing documentation MUST
say so.

A scanned token (§10.1) is not subject to that attack — 16 characters of
rejection-sampled entropy is not searchable offline — so a client that pairs by
camera is meaningfully protected and a client that pairs by typing is not.
User-facing documentation MUST NOT imply the two are equivalent.

## 11. Limits

| Limit | Value |
|---|---|
| Maximum datagram | 256 bytes |
| Maximum payload, unauthenticated | 244 bytes (256 − 12) |
| Maximum payload, authenticated | 236 bytes (256 − 12 − 8) |
| Maximum concurrent sessions | 8 |
| Session idle timeout | 3000 ms |
| Client keepalive floor | 10 Hz |
| Default input rate | 60 Hz, up to 125 Hz |
| Pairing window | 120 s |
| Failed pairing attempts | 5 |

## 12. Versioning

- **A major version mismatch MUST be a hard reject** with an `ERROR` code 1
  naming which side to update. An implementation MUST NOT silently negotiate
  down.
- Feature variance is handled entirely by capability bits, never by optional
  fields or variable-length payloads.
- **The v1 wire format freezes at the end of M1.** After that, any change to a
  wire-visible struct, constant, offset, or bit position is a v2 change.

## 13. Conformance vectors

`core/testdata/vectors.h` holds byte-exact golden packets with their expected
decoded values. Every client ships a hidden self-test screen (hold L+R+Start at
launch) that runs them on-device.

**Three vector shapes, because §6.16 and §6.20 specify behaviour a single
packet cannot pin.** The shape is named here so that the author deriving
vectors from this document does not have to invent one — an invented shape is
where independence quietly dies.

1. **Packet vectors** — the original and still the majority: wire bytes in,
   expected decoded structure out, compared byte for byte.
2. **Function vectors** — an input tuple and an expected integer result, no
   packet involved. Required for `apad_seq_diff` (§6.21) and for the event-ring
   gap computation of §6.20, both of which are pure arithmetic whose wrap
   behaviour is the entire point.
3. **Sequence vectors** — an ordered list of packets with the expected receiver
   state after **each** one. Required for anything that is only wrong across
   two or more datagrams: the §6.16 baseline rule, a stale packet not advancing
   the baseline, gap replay, overflow, and the §6.20 step-3 reconcile that must
   run even when no events are replayed. A sequence vector asserts after every
   step, not only at the end, or a design that converges by accident passes.

Vectors are derived **from this document only**, by an author who has not read
`core/src/codec.c` (`DESIGN.md` §9.1). If the same person writes both, they encode
the same misunderstanding twice and the self-test passes on a broken build.

Mandatory coverage:

- sequence wrap across `0xFFFF`, in both directions
- tick wrap across 2^32
- all 16 `apad_hat_lut` indices, including up+down and left+right
- a payload at the framing ceiling (244 bytes) and a zero-length payload. Note
  this exercises the framing bound only: the largest defined v1 payload is
  `HELLO` at 76 bytes, so no real message approaches the cap
- `payload_len` disagreeing with the type's fixed size (§3.1 check 6)
- `magic` mismatch, and a datagram shorter than 12 bytes
- the authentication tag from Appendix A, and at least one single-bit flip in
  an authenticated datagram that MUST fail verification
- reserved bits set — MUST be ignored, not rejected (§2)
- length mismatch between `payload_len` and the datagram — MUST be rejected
- truncation at every byte offset
- `battery` = 255 (unknown), `touch_count` > 2 (clamped)
- `axes[4]`/`axes[5]` negative (clamped to 0)
- **§6.15–§6.19**: every reserved field of each of the four types set to ones —
  the decoded structure MUST be byte-identical to the one decoded from the
  clean packet
- `KEYBOARD.events[].usage` of 0x01, 0x02 and 0x03, `MOUSE.events[].button`
  above 5 and `MEDIA.events[].control` above 24 — each MUST normalise to 0 and
  MUST force that entry's `flags` to 0
- a `KEYBOARD` holding seven keys at once, which a HID boot report could not
  express
- the §6.16 accumulator diff across a wrap in both directions
  (`0xFFFB` → `0x0005` is +10, not −65526), and at both extremes
  (`0x0000` → `0x8000` is −32768; `0x0000` → `0x7FFF` is +32767)
- an `event_seq` gap larger than the ring, one of exactly the ring depth, one
  of zero, and one that is not newer than what was last applied
- the §6.20 step-3 reconcile running when **no** events are replayed: a
  snapshot that disagrees with the receiver's held state while `event_seq` is
  unchanged MUST still converge in one packet
- the §6.20 right-alignment rule: a ring carrying fewer events than its depth,
  proving the newest sits at slot `D-1` and the leading slots are zero
- a first accepted message of a type whose `event_seq` is far from zero —
  no events replayed, no overflow surfaced, snapshot applied as state

## 14. Appendix A — normative authentication test values

Fixed inputs so a byte-exact tag can be derived independently of any
implementation. Without these, §10's mechanism is specified but unverifiable,
and no conformance vector can cover it.

| Input | Value |
|---|---|
| PIN | `"123456"` (six ASCII digits, no NUL) |
| `server_nonce` (PBKDF2 salt) | `00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F` |
| Iterations | 10000 |
| Derived key length | 32 bytes |

Derived session key, PBKDF2-HMAC-SHA256:

```
A9 66 08 61 D6 11 D4 6A 19 19 71 EC CF 0C C8 95
EE 7C D5 80 91 C1 97 3E E6 D6 0A 5C 4F 30 42 19
```

Test datagram — an authenticated `PING` (§6.6), shown with the tag region
zeroed exactly as it is during computation:

```
header   43 4D 01 30 01 00 02 00 08 00 01 00
payload  E8 03 00 00 00 00 00 00
tag      00 00 00 00 00 00 00 00
```

Truncated HMAC-SHA256 tag, the first 8 bytes of the MAC over all 28 bytes
above:

```
F8 4C BA C6 CE 34 B1 AE
```

An implementation that reproduces this tag has SHA-256, HMAC, PBKDF2, the
iteration count, the salt, the tag truncation and the zeroed-tag-region rule
all correct simultaneously.

---

## 15. Verification status

Thirteen questions were raised by implementing this spec and deriving vectors
from it independently. This section records where they landed. **Nothing here
is normative** — it is a statement about evidence, not about the wire format,
and it is updated as things get exercised. The format itself is frozen
regardless of what this section says.

**Settled at the freeze (2026-08-09)** — §2 scrub rule, §3.1 validation order
including the `payload_len`-vs-type check, §5 battery range, §6.2/§6.6/§6.8
minor semantics, §8 liveness vs freshness, §9 retransmit timing and the
unsigned helper form, §11 payload caps, §13 coverage, and Appendix A.

**Resolved since the freeze:**

1. **§6 payload layouts were the least-reviewed part of the wire format** —
   specified here and, at the freeze, implemented once against themselves.
   They have since been exercised by two independent client implementations
   (3DS, Android) against two independent server backends (`uinput`, ViGEmBus),
   plus the loopback and engine test clients, and the vector suite grew its own
   coverage for them. This is no longer a single implementation agreeing with
   itself.
2. **Real ARM, and 32-bit hosts.** The codec has run on ARM11 (3DS, 32-bit) and
   ARM64 (Android) hardware with a full self-test pass on each, alongside
   x86-64. The alignment argument is no longer only an argument on ARM
   generally — see the remaining caveat below for what is still untested.
3. **`ERROR` and `STATUS` text at 60 bytes** is frozen at 60 and has proven
   sufficient in practice; §13's text-truncation vectors pin the truncation
   behaviour rather than leaving it to each implementation.

**Still open:**

4. **The maximum session count (8) and the 3-second idle timeout** are stated
   here and nowhere else. Neither has been load-tested.
5. **`key_material` is unreachable through the API in v1** — it MUST be zero,
   so no caller can populate it. Intentional (it reserves the ChaCha20 upgrade
   path, `DESIGN.md` D3), but a caller who sets it will see it silently dropped.
6. **Nothing has run on a big-endian host.** Every current target is
   little-endian. The codec touches host endianness nowhere by construction and
   asserts exact wire bytes, so a big-endian host would fail loudly rather than
   silently — but that remains an argument, not a test run.
7. **The ARM9 silent-rotation class is unverified on hardware.** It is the
   reason for the never-cast-a-struct rule, and the DS is the platform it
   applies to; no DS client exists yet. What has been demonstrated is that all
   four byte alignments produce identical output on x86-64, ARM11 and ARM64.
8. **PBKDF2 at 10,000 iterations has never been timed on a 67 MHz ARM9.**
   `DESIGN.md` D3's "about one second" is the entire justification for the
   iteration count and remains taken on faith until DS hardware runs it.
9. **`TOUCHMAP` (§6.12) has no conformance vectors.** Every other message type
   is pinned by vectors derived from this document by an author who never read
   the codec; that independence is what makes the suite evidence rather than a
   restatement of the implementation. `TOUCHMAP` was specified and implemented
   by the same author, so writing its vectors now would produce agreement, not
   verification. What it has instead: a live 3DS drawing the regions a real
   server sent, and the §4 discard rule keeping an older client unaffected
   either way. Deriving its vectors independently is the outstanding work.
10. **§6.15–§6.19 (keyboard, mouse, media, `INPUTCAPS`) have independently
   derived vectors — the thing item 9 records `TOUCHMAP` never got.** Specified
   2026-08-25 and implemented the same day by one author, with vectors derived
   from this document alone by a second who did not read `codec.c`, `seq.c` or
   `session.c`. **The two readings agreed on every offset, mask, normalisation
   and wrap boundary on the first run — zero disagreements**, decode, encode
   and round trip alike. The suite went 1141 → 1987 cases and gained the two
   shapes §13 now names: function vectors for §6.21's arithmetic, and sequence
   vectors asserting receiver state after every step.

   Because a suite that has never failed is not known to be able to fail, all
   21 assertion families were mutation-tested from the vector side and every
   one was caught — including front-packing the ring, a stale `MOUSE`
   advancing the baseline, and an `event_seq` written big-endian.

   **What this does NOT cover, and cannot:** §6.20's 10 Hz repeat floor and
   1000 ms watchdog are measured against a clock, and core has none by design.
   The single failure §6.20 exists to prevent — a client dying mid-chord and
   leaving a modifier held on the user's desktop — is therefore unpinned by
   conformance data and needs a server-side test with a controllable clock.
   Nothing here has run on a big-endian host, on ARM9, or on hardware of any
   kind.
