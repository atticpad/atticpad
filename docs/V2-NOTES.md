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
| Message types | 18 of 256 | ~230, in clearly-themed ranges |
| Capability bits (`caps`, u32) | 0–13 | **18** (bits 14–31, reserved, MUST be zero) |
| Button bits (§5.7 mask, u32) | 0–19 | **12** (bits 20–31, reserved, MUST be zero) |
| Authenticated payload | 68 for TOUCHMAP | **236 bytes per datagram** |
| `WELCOME.key_material` | zero-filled | 32 bytes, reserved for a keying upgrade |

Message-type ranges carry meaning worth preserving:

```
0x01–0x0F  discovery          (2 used)
0x10–0x1F  session lifecycle  (3 used)
0x20–0x2F  client -> server input   (4 used: INPUT_STATE, KEYBOARD,
                                     MOUSE, MEDIA)
0x30–0x3F  liveness           (2 used)
0x40–0x4F  server -> client   (5 used, TOUCHMAP 0x43, INPUTCAPS 0x44)
0x50–0x5F  transport          (2 used)
```

`0x2x` was where a new *input kind* would live, and three of those slots have
since been spent exactly as predicted — `KEYBOARD` 0x21, `MOUSE` 0x22 and
`MEDIA` 0x23, specified in `docs/PROTOCOL.md` §6.15–§6.17 on 2026-08-25.
Eleven remain, one of them (`0x24`) reserved rather than free — see §6.22.

## Candidates, classified by what they'd cost

### Additive — no v2 needed

- ~~**Keyboard.**~~ **Landed**, `docs/PROTOCOL.md` §6.15 (`0x21`). The 256-bit
  page-0x07 bitmap went in as sketched. **The capability bit did not, and could
  not** — §6.14 rules that widening `caps` is a v2 change, because a v1 server
  scrubs a reserved capability bit rather than ignoring it. That is the one
  prediction on this list that was wrong, and it is worth keeping visible:
  negotiation went the other direction instead, as a server→client `INPUTCAPS`
  (§6.19) that a client must hear before it may send anything.
- ~~**Mouse.**~~ **Landed**, §6.16 (`0x22`). Relative, as argued — and the
  deltas became wrapping accumulators diffed with §9's helper, so loss and
  reordering both cost nothing.
- ~~**Media / remote keys.**~~ **Landed**, §6.17 (`0x23`), as its own type
  rather than folded into the keyboard message. The deciding reason was not on
  this list: a host's keyboard API and its consumer-control API are often
  different calls with incompatible arguments, so folding them puts a
  two-operating-systems conditional inside one handler.
- **Server→client config**, of which TOUCHMAP is the first: button-label hints,
  on-screen layout for a phone, "this profile is active" text (which `STATUS`
  can already carry today, unused).
- **A Unicode text message.** `0x24` is now **reserved** for it —
  `docs/PROTOCOL.md` §6.22, which records the reservation and the reasoning
  without specifying a payload. Would remove the layout assumption §6.15
  leaves on any client whose text comes from an IME. The wire half is small;
  the cost is a server-side codepoint injector, which Windows `SendInput`
  supplies natively and `uinput` does not supply at all. Establish the hard
  half first.
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

*Resolved as predicted.* Windows took `SendInput`, in its own backend file
composed with ViGEm behind a single `apad_backend` so the host still passes
exactly one. That it is *injected* input rather than a device is a permanent
property, not a fault, so it travels to the client as `INPUTCAPS`'s
`SYNTHETIC` bit rather than as a health string. The driver-backed alternative
remains a sibling file whenever someone wants it.

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
4. ~~**Before any of it: `APAD_PADBTN_*` must move into the protocol header.**~~
   **Done** — `core/include/atticpad/protocol.h`. The rule generalised: any
   vocabulary a wire field names belongs in core, which is why §6.18's media
   control index was written there and not in a backend.
