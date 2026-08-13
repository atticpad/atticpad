# Vendored third-party source: `quirc`

A QR-code recognition library, vendored here — deliberately **not** under
`clients/android/` — because Android is not its only consumer. `docs/PROTOCOL.md`
§10.3 defines a pairing URI meant to be carried in a QR code, and both camera
clients this project has (Android now, 3DS later) decode the same kind of
code the same way. A copy under `clients/android/` would be a second decoder
that nobody agreed to keep in step with the first, which is the same failure
mode `docs/DESIGN.md` §9.1 exists to prevent for the wire codec itself.

This directory holds only the recognizer. Nothing here knows about
`atticpad://` — the boundary is deliberate: `quirc_decode()` hands back a
byte string, and only `apad_pair_uri_parse()` (`core/`) is allowed to
interpret it. See `clients/android/app/src/main/cpp/apad_qr.c` for where that
handoff happens.

## Provenance

| Directory | Source | Licence | Pinned at |
|---|---|---|---|
| `lib/` | [`dlbeer/quirc`](https://github.com/dlbeer/quirc) | **ISC** (`LICENSE`, © 2010-2012 Daniel Beer) | commit `927d680904dc95fdff4cd9d022eb374b438ff8f2`, 2025-05-20, `master` tip |

Extracted 2026-08-10, verified against a fresh clone of the upstream
repository at the commit above (`git log -1`, licence text read directly from
`LICENSE`, not taken on trust).

```
a70ef3ea032998eead2e2c7573a170a809eae08e3ca134611f707eda5932c8a9  LICENSE
49660ea710add2d6f304a1323f53190f5a2bf34db4dd160d633db0c3f22bfba5  lib/quirc.h
e383ed1a0ca70c07b0530d76bfeb6bd5525efda182589f48c978921a9e54676b  lib/quirc_internal.h
0294b6c56f8c021b256c4c153d70483368164c6cf0cce643e1b6be03ed3585c0  lib/quirc.c
ae858d86adcb12db80ad01f6d941cc2247fb5970abf0754f24dca027ede2ba99  lib/identify.c
d4468c55ecd0d2f905a6813513708005e6d609ef0a3d32a17673313c7552a7c1  lib/decode.c
6764aa2f245085080e1e5cefd9dcd59b9727718a0d4606956e0502a57f5dff30  lib/version_db.c
```

**Not one byte of vendored source has been modified.** Anything this project
needed that upstream does not provide (grayscale-plane framing, warning
suppression, the CMake wiring) is supplied from the outside — see
`clients/android/app/src/main/cpp/apad_qr.c` and `CMakeLists.txt` — so this
tree stays byte-identical to upstream and re-pinning is a straight copy.

## Why vendored, and not fetched at build time

- Same reasoning as `server/backends/vendor/README.md`: every other build
  input here is pinned by digest so CI is reproducible
  (`scripts/toolchains.env`, `scripts/android.env`), and a `git clone` in a
  build step is the one unpinned thing that could change under us.
- It is one C library, ~3000 lines across 4 `.c` files, ISC-licensed (a
  close cousin of the 2-clause BSD licence: attribution required, otherwise
  unrestricted). Nothing to weigh against vendoring it.
- The blind console clients (`PSP`, `Vita`, `NDS`, `Switch`) have no camera
  and no consumer for this code; only Android and, later, the 3DS build it.

Deliberately **not** a git submodule, for the same reason
`server/backends/vendor/README.md` gives: a submodule reintroduces the
network fetch and the "did you `--recursive`?" failure mode without giving
back anything the pinned copy above does not already provide.

## What was taken, and what was left behind

Taken: `LICENSE`, `lib/quirc.h` (public API), `lib/quirc_internal.h`
(shared internal types the four `.c` files need), and the four translation
units that make up the recognizer: `lib/quirc.c` (buffer lifecycle),
`lib/identify.c` (finding capstones and grids in the image),
`lib/decode.c` (Reed-Solomon + bitstream decode to a payload), and
`lib/version_db.c` (the per-version format tables `identify.c`/`decode.c`
consult). That is the complete `lib/` directory upstream ships — nothing
inside it was left behind.

Left behind: `demo/` (an SDL/V4L2/OpenCV live-camera viewer — this project
gets frames from Camera2, not V4L2, and pulling in SDL would violate "zero
third-party dependencies" for no reason), `tests/` (upstream's own
`qrtest`/`inspect` tools, exercised against upstream's own test images, not
this project's), the top-level `Makefile` (superseded by
`clients/android/app/src/main/cpp/CMakeLists.txt`), `README.md`, and
`.github/`. None of them are reachable from the four files above — verified
by grepping every `#include` in `lib/`.

## Build notes — read before touching `CMakeLists.txt`

- **`lib/*.c` does not compile clean under this project's `-Wall -Wextra
  -Werror -pedantic`.** Confirmed on the host toolchain: `-Wsign-compare` in
  `quirc.c` and twice in `identify.c`, `-Wunused-parameter` once in
  `identify.c` — all pre-existing upstream code, none touched. Building
  `lib/*.c` under our own warning gate would mean either patching vendored
  source (which this README says nothing does) or letting a warning class we
  otherwise treat as fatal through everywhere. Compile these four files with
  a **separate, permissive flag set** — see `CMakeLists.txt`'s dedicated
  `quirc` object library, analogous to `-isystem` in
  `server/backends/vendor/README.md`'s mingw notes: it silences third-party
  warnings without weakening `-Werror` on a single line this project wrote.
- **Links against libm.** `lib/identify.c` uses `<math.h>` (`sqrt`, `atan2`,
  `floor`) unconditionally — `QUIRC_FLOAT_TYPE` is left undefined, so
  `quirc_float_t` is upstream's default `double` and the plain (non-tgmath)
  math header is the one actually included. The NDK's `libc.so` re-exports
  `libm` symbols on modern API levels, but link `-lm` explicitly rather than
  rely on that, since it is also what the host build needs.
- **No colour conversion needed.** `quirc_begin()`/`quirc_end()` operate on
  one 8-bit grayscale plane, `width * height` bytes, no padding. Camera2's
  `ImageFormat.YUV_420_888` Y plane is exactly that (mind `Image.Plane`'s
  `rowStride`, which can exceed `width` and must be stripped before the
  bytes reach `quirc_begin()`'s buffer — `apad_qr.c` does this).
- **Not reentrant, and not meant to be called concurrently with itself**: one
  `struct quirc *` handle, created once per scan session and reused frame to
  frame (`quirc_resize()` is a no-op once the size matches). `apad_qr.c`
  gives Android exactly one such handle per in-app scan.
