# Adding a client platform

Written after M4.5, when the port surface changed shape: a client used to mean
"transcribe the 3DS client's protocol driving"; it now means "call the shared
engine". The 500-line budget in DESIGN.md §1 is real — the 3DS client's
platform-specific half fits in it, and yours should too.

## What a client actually is

Three pieces, only one of which you write from scratch:

1. **The shim** (`shim/`): sockets and monotonic time. `net_bsd.c` already
   covers 3DS, PSP, Vita, Switch, Linux and Android — M2 proved it on console
   hardware and fixed the three portability bugs (explicit protocol argument,
   no bind for ephemeral ports, `<arpa/inet.h>` included). You likely need
   only platform bring-up (the 3DS needed `soc_3ds.c`, 91 lines; check your
   platform's equivalent in your SDK's own sample tree).
2. **The engine** (`clients/common/apad_client.{c,h}`): create → probe →
   connect → pump → disconnect. It owns the socket, the §8 handshake with §9
   retransmits, ACK-every-copy, PING both directions, ERROR-7 teardown, the
   §10 auth state machine, and the stats snapshot your UI reads. **You never
   touch a packet.** It is core-constraint-clean (one calloc at create, no
   float, no stdio) and compiles at `-Wall -Wextra -Werror -pedantic` on
   every current target.
3. **Your platform layer**: input polling normalized to §5 conventions
   (sticks +Y up — PSP and Vita report +Y down and must invert; touch +Y
   down; NO client-side deadzone), a `main` loop that calls
   `apad_client_pump(&input, ~8–16ms)` once per frame, discovery UX
   (tier-2 broadcast and/or tier-3 manual entry — tier 3 is MANDATORY, §7),
   and the hidden self-test screen (L+R+Start, plus a visible trigger — the
   3DS uses SELECT, because a hidden combo is worthless if one of its
   buttons is broken).

## Read in this order

- `tools/engine-client/main.c` — the minimal engine caller, ~160 lines,
  exercised in CI against the real server. Your `main.c` is this plus input
  and UI.
- `clients/3ds/source/main.c` — the reference client: screen system,
  discovery flow, reconnect-on-session-end, self-test integration.
- `PROTOCOL.md` §5 (input conventions) and §7 (discovery tiers) — the
  parts the engine cannot do for you.
- **Your SDK's own samples** — mirror their networking init exactly rather
  than writing it from memory; where a sample and your recollection disagree,
  the sample wins. For the 3DS these ship inside the pinned devkitARM image at
  `/opt/devkitpro/examples/3ds/`. For anything camera-related, FBI's
  `capturecam` is the production pattern and the upstream samples are only
  demos — a distinction that cost seven build rounds to learn.

## Known engine gaps (M4.5 ledger — fix in the engine, do not work around)

- No `client_id` injection: §6.3 persistence is unsatisfiable until the
  engine grows it.
- `connect()`/`probe()` take dotted-quad strings; tier-2 discovery hands you
  an `apad_addr` you must render back to text.
- STATUS and ERROR share a serial in the stats snapshot.
- The handshake has no progress callback (blank screen up to ~4 s).

If your port needs one of these, fix it in the engine — a workaround in
platform code recreates the drift the engine exists to kill.

## Definition of done for a new port

Build via `./scripts/build.sh <target>` (pinned container). RUN it — emulator
with working networking (PPSSPP, melonDS) or hardware — against the real
Linux server, and confirm: a full self-test pass on device, HELLO→WELCOME,
input visible under `evtest`, RTT populating BOTH directions (the server's
column too — §6.6's other half), clean BYE. Say what you could NOT verify.
