# Vendored third-party source for `server/`

Sibling to `server/backends/vendor/` (see that directory's own `README.md`
for the pattern this file follows), and deliberately **not** inside
`server/backends/`: `server/backends/vendor/README.md` states its tree
exists for exactly one consumer, `vigem.c`, and that nothing outside
`server/backends/` may know it exists. The QR encoder below is a UI concern
(`docs/PROTOCOL.md` §10.3, the pairing URI), not a backend concern, so it
gets its own sibling tree instead of leaning on that boundary.

## Provenance

| Directory | Source | Licence | Pinned at |
|---|---|---|---|
| `qrcodegen/` | [`nayuki/QR-Code-generator`](https://github.com/nayuki/QR-Code-generator), the `c/` subdirectory only | **MIT**, embedded verbatim in both source files' header comments (transcribed once more into `qrcodegen/LICENSE.txt` for a standalone copy — upstream carries no repository-wide `LICENSE` file) | commit `8329a7108fc22be3e1eec0a9f9318978579e3621`, 2024-09-01 — the last commit to touch `c/` at all; `git describe --tags` = `v1.8.0-16-g8329a71`. Repo `HEAD` at extraction time (2026-08-10) was the newer `2c9044de6b049ca25cb3cd1649ed7e27aa055138`, 2025-01-23, but every commit between the two touches only the other language ports (`java`, `python`, `rust`, `typescript-javascript`, `cpp`) — verified with `git log --stat -- c/`, which shows nothing past `8329a71`. Pinning the C-relevant commit rather than the repo tip avoids implying a freshness the C code does not have. |

Extracted 2026-08-10, via `git clone` into a scratch directory (never
checked into this repository) and a byte-for-byte `cp` of the two files
below — not retyped, not reformatted.

```
6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c  qrcodegen/qrcodegen.c
e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589  qrcodegen/qrcodegen.h
```

**Not one byte of vendored source has been modified.** `qrcodegen/LICENSE.txt`
is new text written for this repository (a verbatim transcription of the
notice both files already carry, kept as a standalone file for the same
reason `vigemclient/LICENSE` is kept standalone next to code that also
carries its own header notices) — it is not claimed to be an upstream file.

## Why this library, and why vendored rather than fetched at build time

- **MIT, two files, no heap, no floating point, C99** — matches this
  project's own server-side constraints (`docs/CONVENTIONS.md` notes server code may
  use `malloc` and floating point freely, but this library needs neither:
  every function takes caller-provided fixed-size buffers, sized at compile
  time from `qrcodegen_BUFFER_LEN_FOR_VERSION`/`qrcodegen_BUFFER_LEN_MAX`,
  and there is no `malloc`/`calloc`/`free` anywhere in `qrcodegen.c` —
  confirmed by grep against the vendored copy, not assumed).
- **The last commit reachable from `c/` is from 2024-09-01**, not three years
  stale (the caution in this task's brief, aimed at exactly this kind of
  "is it actually still around" question) — checked live via `git log --stat`
  against a real clone of the upstream repository on 2026-08-10, not taken on
  faith. The repository itself is active (newer commits exist for other
  language ports as recently as 2025-01-23); it is the C port specifically
  that has been stable, not abandoned.
- **Same reproducibility argument as `server/backends/vendor/`**: everything
  else in this project is pinned by digest, and vendoring removes the one
  unpinned network fetch a build step would otherwise need.
- It is two files and about 45 KB combined. There is no cost to weigh
  against the above, and no submodule for the same reason
  `server/backends/vendor/README.md` gives (a submodule reintroduces the
  network fetch this vendoring exists to avoid).

## What was taken, and what was left behind

Taken: `c/qrcodegen.c`, `c/qrcodegen.h` — the complete library, two files,
nothing else needed to link against it.

Left behind: `c/qrcodegen-demo.c` and `c/qrcodegen-test.c` (upstream's own
demo/test mains, not library code — this repository's own consumer,
`server/host/linux/qr.h`, is the demo/test equivalent here), `c/Makefile`
(irrelevant: nothing here uses it, the file is pulled in by `#include`, see
below), `c/Readme.markdown`, and every other language directory in the
upstream repository (`java`, `java-fast`, `python`, `rust`, `rust-no-heap`,
`typescript-javascript`, `cpp`) — this project links C, once.

## How it is compiled in, and why that is not a build-script edit

`scripts/build.sh`'s `build_server()` names an exact, fixed list of `.c`
files and this task's own CONSTRAINTS forbid touching `scripts/` — the same
constraint `server/host/linux/{webui,assets,ipaddr}.h` were already built
against (see server-dev agent memory `server-ui-and-rtt`). Rather than adding
a third, GENUINELY NEW instance of that header-only-declarations trick (which
only works when there is no third-party `.c` body to preserve byte-identical),
`server/host/linux/qr.h` does the same thing `mapping-engine-profiles`'s OLD,
now-superseded workaround did for `jsonc.c`/`profiles.c`: it
`#include`s `../../vendor/qrcodegen/qrcodegen.c` directly, once, so the
vendored `.c` file becomes part of `main.c`'s single translation unit without
ever being named on a compiler command line of its own. `qrcodegen.c`'s own
`#include "qrcodegen.h"` (relative to itself) still resolves correctly
because the compiler's include search is by the including file's own
directory as well as `-I` paths — no path rewriting needed. This is the
correct, sanctioned use of that trick per server-dev agent memory
`server-ui-and-rtt`: "do not assume [scripts/build.sh growing real per-file
entries for jsonc.c/profiles.c] covers new files added after it."
