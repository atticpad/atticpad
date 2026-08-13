# Vendored third-party source for `server/backends/`

Everything under this directory is here for exactly one consumer:
`server/backends/vigem.c`, the Windows virtual-pad backend. Nothing else in
the repository may include it, and nothing outside `server/backends/` is
allowed to know it exists — that is the same rule that keeps `uinput.c`'s
evdev knowledge from leaking out (`docs/DESIGN.md` §6.1).

## Provenance

| Directory | Source | Licence | Pinned at |
|---|---|---|---|
| `vigemclient/` | [`nefarius/ViGEmClient`](https://github.com/nefarius/ViGEmClient) | **MIT** (`vigemclient/LICENSE`, © 2018 Benjamin Höglinger-Stelzer; the headers additionally carry © 2017-2023 Nefarius Software Solutions e.U. and Contributors) | commit `b66d02d57e32cc8595369c53418b843e958649b4`, 2023-09-08, `master` tip — `git describe` = `v1.21.222.0-38-gb66d02d` |
| `mingw-compat/` | written for this repository | — | — |

Extracted 2026-08-10.

Why the `master` tip rather than the newest tag `v1.21.222.0` (2022-10-25):
the repository is **archived**, so `master` is the final state it will ever
have, and there are 38 commits of real work after that tag — 292 changed
lines in `ViGEmClient.cpp` alone, including `vigem_target_x360_get_output`.
Pinning the tag would vendor a knowingly older snapshot of something that
will never be updated again. The commit hash above is the pin; the file
hashes below are the verification.

```
a4bf0cc2ba588708218344fecf07b95bc4748b36c27d47f32bcde5d122180a48  vigemclient/include/ViGEm/Client.h
766527c639a93f5fc0acfd1c873599f9e4a482e89c809a2192fa4d5c372f37bf  vigemclient/include/ViGEm/Common.h
7e21cef8a670c61dd26568d7e5506accfd5893ffae1062c5a0d134613aaf751f  vigemclient/include/ViGEm/km/BusShared.h
6941749464e0dcfa94bb74274a0824d9a3f67015015060e15d70c77e037fdaae  vigemclient/include/ViGEm/Util.h
445b3bccd103d39cab7e9b276dd966d60269cf5c44b4b8a118e48bc4c7c4f0db  vigemclient/LICENSE
e3e2b31643072138ef1f2bf79c93bb1b887d800702c2d4a98f7233b2be9f6873  vigemclient/src/Internal.h
632fd060864f614648b0c03f258496677010ccd31798a8a9274188ec87f3e61e  vigemclient/src/UniUtil.h
9006ca58d9130349df63c0bf4dbec9e2125be578e3d22b7cf274598593721601  vigemclient/src/ViGEmClient.cpp
```

**Not one byte of vendored source has been modified.** The two mingw problems
described below are both solved from the outside, by include path, so this
tree stays byte-identical to upstream and re-pinning is a straight copy.

## Why vendored, and not fetched at build time

- **Upstream is archived and frozen** (`docs/DESIGN.md` §2.1). There will never be a
  newer version to track, so the usual argument for fetching — staying
  current — buys nothing at all here.
- **Everything else in this project is pinned by digest** so CI builds are
  reproducible (`scripts/toolchains.env`). A `git clone` in a build step is
  the one unpinned thing that could change under us, and it points at a
  repository whose owner has already walked away from it.
- **MIT permits redistribution**, source and binary, with the notice
  retained — `vigemclient/LICENSE` is kept verbatim for that reason.
- It is 220 KB and eight files. There is no cost to weigh against the above.
- A local build must never produce a release artifact (`docs/DESIGN.md`); vendoring
  does not change that either way, because it changes nothing about who runs
  the build. Releases still come from tagged CI only.

Deliberately **not** a git submodule: a submodule reintroduces the network
fetch and the "did you `--recursive`?" failure mode without giving back
anything the pinned copy above does not already provide.

## What was taken, and what was left behind

Taken: `LICENSE`, `include/ViGEm/{Client,Common,Util}.h`,
`include/ViGEm/km/BusShared.h`, `src/{ViGEmClient.cpp,Internal.h,UniUtil.h}`.
That is the complete transitive include closure of the one translation unit
that gets compiled — verified by grepping every `#include` in the tree.

Left behind: the Visual Studio solution and `.vcxproj`, `ViGEmClient.rc` and
`resource.h` (MSVC resource script, not referenced by the `.cpp`), the CMake
files, `appveyor.yml`, `.github/`, and the upstream READMEs. None of them are
reachable from a mingw build and carrying them would only invite someone to
try to use them.

## `mingw-compat/` — the two build traps

The Windows host is cross-compiled from Linux with mingw-w64 because the
Windows host is cross-compiled from Linux rather than built natively
(`docs/DESIGN.md` §2.1). Two things
bite immediately, and a third under `-Werror`:

1. `ViGEmClient.cpp` includes `<Windows.h>` and `<SetupAPI.h>` with capital
   letters. mingw-w64 ships them lowercase, and Linux filesystems are case
   sensitive. `mingw-compat/` supplies those two names as one-line
   `#include_next` forwarders. Put it **first** on the include path of the
   C++ translation unit — and only that one; the C files do not need it.

   `#include_next` rather than the symlinks that first solved this: a symlink
   hard-codes one distribution's sysroot path into the repository, and on a
   case-insensitive filesystem a plain `#include <windows.h>` inside
   `Windows.h` would re-enter the same file. `#include_next` is correct in
   both cases and is a GCC extension, which costs nothing on a path that is
   already GCC-only.

2. `ViGEm/Client.h` uses `USHORT`, `BYTE`, `UCHAR` and `LPVOID` without
   including anything that defines them. **A C consumer must
   `#include <windows.h>` before it**, or it emits a wall of unknown-type
   errors that reads exactly like a broken toolchain. `vigem.c` does this and
   says so; do not let anyone "tidy" that include order.

3. `ViGEm/Common.h` writes `VOID FORCEINLINE XUSB_REPORT_INIT(...)`, which
   expands to `void extern __inline__ ...` and trips
   `-Wold-style-declaration` (part of `-Wextra`). Include this tree with
   **`-isystem`, not `-I`** — that silences third-party headers without
   weakening a single warning on our own code, and without patching vendored
   source.
