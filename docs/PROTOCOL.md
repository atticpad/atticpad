# AtticPad Protocol v1 — normative specification

**Status: FROZEN.** The v1 wire format was frozen on 2026-08-09 at the end of
M1. Every constant, offset, bit position, payload size, normalisation rule and
the reliability contract in §8/§9 are now fixed. **Any change to them is a v2
change** — stop and report rather than making one. Adding a NEW message type is
the one additive exception, ruled and bounded in §6.14; §6.12 was added that way
and the format is otherwise unchanged.

Frozen against, **as of 2026-08-09**: 193 conformance vectors derived from this
document by an author who never read the codec, 990 self-test cases, 675 + 46
cross-validation checks with zero disagreements, a clean libFuzzer run, and two
independent audits. Those are the figures the freeze was taken against and are
left as the historical record; the suite has grown since (239 vectors, 1141
self-test cases at 0.5.0) without any wire-visible change. §15 tracks what has
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
| `0x30` | `PING` | either | no | 8 bytes (§6.6) |
| `0x31` | `PONG` | either | no | 8 bytes (§6.6) |
| `0x40` | `RUMBLE` | server → client | yes | 8 bytes (§6.7) |
| `0x41` | `LED` | server → client | yes | 4 bytes (§6.8) |
| `0x42` | `STATUS` | server → client | yes | 64 bytes (§6.9) |
| `0x43` | `TOUCHMAP` | server → client | no | 68 bytes (§6.12) |
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

Three fields carry an explicit receive-side normalisation rule —
`ANNOUNCE.pairing_required` (§6.2), `LED.player_index` (§6.8) and, in §5,
`battery`. They share a property: each drives something a consumer acts on, so
an out-of-range value has to become a defined one before it reaches
application code.

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
- Replay is prevented by the sequence window in §9.

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
