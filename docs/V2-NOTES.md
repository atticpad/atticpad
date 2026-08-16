# v2 notes — what the protocol has room for, and what would actually break it

Working notes, written while adding the first message type after the v1 freeze.
**Not normative**, not a commitment, and deliberately not in
`docs/PROTOCOL.md`, which stays frozen at v1 until there is a decision to make
it otherwise.

Written because "we're changing the protocol anyway, what else should go in?"
is the right question asked at the right time — but the answer turns out to be
narrower than expected, and the narrowness is the useful part.

## The finding that reframes the question

**Almost nothing on a typical wishlist requires a v2.**

§4 requires a receiver to discard an unknown message type *silently*. That one
rule makes new message types additive: a v1 peer meets a message it has never
heard of, drops it, and carries on. Verified both ways with TOUCHMAP — a v1
client completed a full conformance run while the server sent it one, and a
new client against a v1 server falls back cleanly to its old behaviour.

So the question is not "what do we want in v2". It is **"what do we want that
cannot be added without one?"** — a much shorter list.

## Extension budget, as it actually stands

| Space | Used | Free |
|---|---|---|
| Message types | 14 of 256 | ~230, in clearly-themed ranges |
| Capability bits (`caps`, u32) | 0–13 | **18** (bits 14–31, reserved, MUST be zero) |
| Button bits (§5.7 mask, u32) | 0–19 | **12** (bits 20–31, reserved, MUST be zero) |
| Authenticated payload | 68 for TOUCHMAP | **236 bytes per datagram** |
| `WELCOME.key_material` | zero-filled | 32 bytes, reserved for a keying upgrade |

Message-type ranges carry meaning worth preserving:

```
0x01–0x0F  discovery          (2 used)
0x10–0x1F  session lifecycle  (3 used)
0x20–0x2F  client -> server input   (1 used: INPUT_STATE)
0x30–0x3F  liveness           (2 used)
0x40–0x4F  server -> client   (4 used, TOUCHMAP took 0x43)
0x50–0x5F  transport          (2 used)
```

`0x21–0x2F` being empty is the interesting part: fourteen free slots in exactly
the range a new *input kind* would live.

## Candidates, classified by what they'd cost

### Additive — no v2 needed

- **Keyboard.** New `0x21` client→server message plus a capability bit. A
  256-key bitmap is 32 bytes, comfortably inside 236 — and a bitmap sidesteps
  the 6-key-rollover limit real USB keyboards have. Modifier state rides in the
  same message.
- **Mouse.** New `0x22`: relative dx/dy, button mask, wheel. Relative motion
  matters — absolute would fight the touchscreen conventions in §5.
- **Media / remote keys.** Play, pause, volume, transport. Either consumer-page
  keycodes inside the keyboard message, or its own type if it should work
  without advertising a keyboard.
- **Server→client config**, of which TOUCHMAP is the first: button-label hints,
  on-screen layout for a phone, "this profile is active" text (which `STATUS`
  can already carry today, unused).
- **A stronger keying scheme.** Already reserved: `key_material` is 32 bytes of
  zeroes in `WELCOME` waiting for exactly this, gated behind a capability bit.

### Breaking — these are the real v2 list

- **More than 32 buttons, or more than the current axis set.** `INPUT_STATE`'s
  layout is fixed and its button mask is a u32 with 12 bits left. Twelve is a
  lot, but a second stick-cluster or a full flight-sim panel would exhaust it.
- **Payloads over 236 bytes.** The 256-byte datagram cap is a §11 constant
  chosen for the DS. Anything wanting to stream (an image, a config blob, a
  layout richer than TOUCHMAP's eight regions) hits it. Chunking across
  datagrams would be an additive workaround; raising the cap would not be.
- **Changing the 12-byte header**, including any new flag bit that a v1 peer
  would misread rather than ignore.
- **Reliability-contract changes** (§8/§9): retransmit timing, ACK semantics,
  the idle timeout.

## Constraints worth knowing before designing keyboard/mouse

**The Windows backend cannot do it.** ViGEmBus emulates gamepads — an X360 or
DS4 pad — and nothing else. Keyboard and mouse on Windows need a different
mechanism entirely (`SendInput`, or a virtual HID driver), which means a second
backend path on the platform whose driver situation is already the shakiest
(see `DESIGN.md` §2.1: ViGEmBus is archived). Linux has no such problem —
`uinput` creates keyboards and mice as readily as pads.

So "add keyboard support" is mostly not a protocol question. The wire part is
the easy half; the server-side platform work is where it actually costs.

**A media remote may not want a gamepad at all.** If the use case is a phone
controlling playback, the pad abstraction is the wrong shape and the natural
output is consumer-control HID or a media-key API — again a backend question,
not a wire one.

## The recommendation this leads to

1. **Don't batch.** Since additions are additive, waiting to bundle them into a
   v2 buys nothing and delays each one. Ship TOUCHMAP when it is ready, on its
   own terms.
2. **Do reserve deliberately.** Range meanings (`0x2x` = client input) and the
   remaining capability bits are cheap to respect and expensive to reclaim.
   Write down what a range means when you first use it.
3. **Treat the breaking list as the real v2 trigger.** A v2 becomes worth doing
   when one of those four items is genuinely needed — most plausibly the button
   space or the datagram cap — and then everything else pending rides along.
4. **Before any of it: `APAD_PADBTN_*` must move into the protocol header.**
   TOUCHMAP already exposed this — a wire field whose constants live in
   `server/backends/backend.h` cannot be read by a client. Any future message
   naming pad outputs hits the same wall.
