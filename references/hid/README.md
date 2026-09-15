# HID keyboard usage tables

Why this directory exists: `docs/PROTOCOL.md` §6.15 carries a client's
keyboard as raw **USB HID Usage Page 0x07 usage IDs** — see §6.15's own
rationale, "the mapping from usage ID to a host keycode is published by every
operating system that speaks USB, so a server translates from a table it can
obtain rather than one it must invent." This directory *is* that table,
obtained rather than invented, for the server's two host backends: Linux
`uinput` (needs `KEY_*` constants) and Windows `SendInput` (needs PS/2 Set 1
scancodes, because `SendInput` accepts either a virtual-key code or a raw
scancode, and a scancode is what lets the server stay keyboard-layout-
agnostic on the Windows side the same way `uinput` does on Linux).

This is not an SDK sample the way `references/3ds/` etc. are — nothing here
compiles against a toolchain. But the same discipline applies for the same
reason `docs/CONVENTIONS.md` gives for mirroring SDK samples on blind platforms: **the
dominant failure mode is not bad reasoning but confidently wrong recall of
rare constants**. A hand-typed `KEY_KPENTER = 96`
or `Left Win = E0 5B` is exactly as fabricable, and exactly as silently wrong
if mistyped, as a hand-typed `socInit()` call would be. Fetching the kernel's
own array and a primary specification document converts a recall problem
into a reading problem, same as vendoring `3ds-examples` does.

## Provenance

### `usage-to-evdev.txt`

| | |
|---|---|
| Source | Linux kernel, `drivers/hid/hid-input.c` — the `hid_keyboard[256]` array — cross-referenced against `include/uapi/linux/input-event-codes.h` for `KEY_*` names |
| Pinned tag | `v6.6` (LTS) |
| Pinned commit | `ffc253263a1375a65fa6c9f62a893e9767fbebfa` (resolved via GitHub's API: `refs/tags/v6.6` → annotated tag `5260836a…` → this commit) |
| URL | `https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/hid/hid-input.c` and `.../include/uapi/linux/input-event-codes.h` — GitHub's read-only mirror of `torvalds/linux`, fetched **pinned to the tag**, not `master` |
| Licence | GPL-2.0 (kernel source); GPL-2.0 WITH Linux-syscall-note (the uapi header) |
| Fetched | 2026-08-25 |
| SHA-256 of fetched files | `hid-input.c`: `bc9babd1131947d56af67aa54b101fe22689ce2584a1f88af3edb78a399af7fe` · `input-event-codes.h`: `23b5be6927d40424e287ca5fdfb82f5d6dfd48b55e92f7e48c8848c4a0495493` |

`git.kernel.org` itself returned an anti-bot challenge page (Anubis) when
fetched directly, so this was pulled from GitHub's mirror instead — the
mirror is read-only and pinned to the same immutable tag, so it carries the
same content as the canonical tree at that release.

**Licence note, read this before extending the file.** `hid_keyboard[]`
itself, as C source with its surrounding function and comments, is GPL-2.0
and was **not** copied into this repo. What's in `usage-to-evdev.txt` is a
derived data table: for each array index (0x00–0xFF), the numeric value the
kernel stores there, resolved to its `#define KEY_*` name. That is a list of
(usage, name, number) facts about the array's *behaviour*, not the array's
source code. If a later reader's judgement differs — if re-deriving a wider
slice of the table, or adding kernel comments verbatim, starts to look like
copying the *expression*, not just extracting the *facts* — stop and raise
it rather than deciding it alone. This report already made that call once
for the scope here; it should not be re-made silently for a bigger scope.

**Correction, 2026-08-25.** `usage-to-evdev.txt`'s own header comment said
"one byte value, 0x9C, appears twice in the source array" citing only
indices 0x4C and 0x9C for KEY_DELETE (111). The data rows were, and remain,
correct: KEY_DELETE (111) appears at **three** indices -- 0x4C, 0x9C, and
0xD8 -- and `scripts/support/check_kbm_tables.c`'s `kEvdevAllowed[]`
allowlist has always listed all three (`{ 111, 3, { 0x4C, 0x9C, 0xD8 } }`).
Only the prose undercounted; nothing that consumed the file's data was ever
wrong. Fixed by correcting the header comment to name all three indices and
say "THREE indices" instead of "appears twice", found and fixed while
extending the media-control table below.

### `consumer-to-evdev.txt`

| | |
|---|---|
| Source | Linux kernel, `drivers/hid/hid-input.c` — the `case HID_UP_CONSUMER:` switch inside `hidinput_configure_usage()` — cross-referenced against `include/uapi/linux/input-event-codes.h` for `KEY_*` numeric codes and any comment attached to a name |
| Pinned tag | `v6.6` (LTS) — same tag as the two files above |
| Pinned commit | `ffc253263a1375a65fa6c9f62a893e9767fbebfa` — same commit |
| URL | Same two files `usage-to-evdev.txt` already cites, re-fetched for this table: `https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/hid/hid-input.c` and `.../include/uapi/linux/input-event-codes.h` |
| Licence | GPL-2.0 (kernel source); GPL-2.0 WITH Linux-syscall-note (the uapi header) — same derived-data-table treatment as `usage-to-evdev.txt`, see that file's own licence note above, which applies unchanged |
| Fetched | 2026-08-25 |
| SHA-256 of fetched files | Identical files to `usage-to-evdev.txt`'s own fetch, re-verified against the same recorded hashes above: `hid-input.c`: `bc9babd1131947d56af67aa54b101fe22689ce2584a1f88af3edb78a399af7fe` · `input-event-codes.h`: `23b5be6927d40424e287ca5fdfb82f5d6dfd48b55e92f7e48c8848c4a0495493` |

Built to give `docs/PROTOCOL.md` §6.18's 24-slot media control vocabulary
the same vendored-ground-truth treatment `usage-to-evdev.txt` already gives
the physical keyboard: every §6.18 control names a function a real HID
Consumer Page (0x0C) device reports, and this file is the kernel's own
`HID_UP_CONSUMER` switch (a *different* switch in the same source file,
covering the Consumer page rather than `hid_keyboard[]`'s Keyboard page),
re-expressed as a derived data table the same way. Scoped to the usages
that could plausibly back one of the 24 controls (transport, volume,
brightness, application launch, browser navigation) rather than the whole
Consumer page — see the file's own header for the exact list and the one
usage (`0x21f`, AC Find) kept outside that scope because it is needed to
resolve an ambiguity, not because it backs a control directly.

This file also carries a machine-readable `@`-prefixed resolution section
that `scripts/support/check_kbm_tables.c` parses to check
`apad_media_to_evdev[]` in `server/backends/uinput_keymap.h` for
*correctness*, not just presence — see that header's own comment and the
file's own tail section for the per-control reasoning, including the one
row (`STOP`) that changed value as a result and the four rows (`PLAY`,
`PAUSE`, `EJECT`, `SEARCH`) that were previously flagged `UNVERIFIED` and
are not any longer.

### `usage-to-scancode-set1.txt`

| | |
|---|---|
| Primary source | Microsoft, *Keyboard Scan Code Specification*, Revision 1.3a, March 16 2000 — Windows Platform Design Notes. Appendix A ("Windows Standard PS/2 Scan Codes") cross-referenced against Appendix C ("USB Keyboard/Keypad Page (0x07)") |
| URL | `https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/scancode.doc` (fetched directly from Microsoft's own download host; a PDF transcription of the same revision, `https://christophervickery.com/babbage/courses/cs345/ms_scancodes.pdf`, was cross-read for table layout only, not as an independent source of values) |
| Licence | No explicit licence grant in the document; standard Microsoft "for informational purposes only" disclaimer. Treated as reference documentation to extract facts from, not redistributed here beyond the derived byte values |
| Fetched | 2026-08-25 |
| SHA-256 of fetched files | `scancode.doc`: `eeeb53fe9871cab7022e9073b11694ff33c8d7dec2612825ebf1c8c5696e65f3` · `ms_scancodes.pdf`: `2d2fc8f024cb694ee2c6bb9ba6d288eb99678e5a8ef9493280976807943b5b53` |
| Independent cross-check | Andries Brouwer, "Keyboard scancodes" §1 (Scan code set 1), `https://aeb.win.tue.nl/linux/kbd/scancodes-1.html` — the long-standing community reference for PC keyboard scancodes, cited from Linux keyboard-driver documentation for decades. No licence statement on the page; treated as reference documentation only |
| Fetched | 2026-08-25 |
| SHA-256 | `aeb-scancodes-1.html`: `2a6ae61f7358cc6266bffe489dedf82544f2dc02ae2c39f915c2909332f67d65` |

The `.doc` is a genuine Word 97 binary (OLE2/CFBF) file, not HTML or XML, so
it could not be read with a text-extraction library in this environment (no
`antiword`/`catdoc`/LibreOffice available, no package-install permission).
It was read with `strings` against the raw binary and cross-read against the
PDF transcription's page layout to recover table structure. Both were kept
in the working set precisely so table-row alignment could be checked against
two independently-produced renderings of the same document, not because the
PDF is treated as its own source — the `.doc` is the primary source, the PDF
was layout scaffolding.

## What every value in `usage-to-scancode-set1.txt` was checked against

Every base scancode and every extended-key (`E0`) designation in the file
was checked against **both** the Microsoft document and Brouwer's page
independently, and **every single row where the two could be compared
agreed exactly** — there is no unresolved numeric disagreement between the
two sources anywhere in this file. Arrows, Insert/Delete/Home/End/PageUp/
PageDown, Keypad `/` and Enter, Right Alt, Right Ctrl, Left Win, Right Win,
Application, PrintScreen's three modifier-dependent variants, and Pause's
`E1`-prefixed sequence and its Ctrl variant — all matched byte-for-byte
across both sources.

## The row-doubling anomaly (flagged, not silently resolved)

The Microsoft `.doc` itself — confirmed by reading the raw binary directly,
not just the PDF conversion, so this is not a PDF-rendering artefact — prints
**a second "`E0_`"-prefixed copy of nearly every key's byte values directly
underneath its real row**, including for keys that have no possible extended
form on any real keyboard: Backspace, Tab, Space, every letter, every digit.
Example, key 15 "Backspace": the table row reads `0E 8E … / E0_0E E0_8E …`
immediately below it — but no keyboard, no OS, and no other source anywhere
documents an `E0 0E` meaning. This is a genuine finding worth someone
checking against the primary document directly if they doubt this file's
resolution of it.

**What this file did with it:** used only the *first* (non-doubled) row for
each key's own base value, and marked a usage extended (`Ext=yes`) only where
Brouwer's page **independently** lists that exact key among its documented
"escaped scancodes" section, or where the Microsoft document's own prose
(Notes 1–5) or a key's *sole* printed row (Right Alt, Right Ctrl, Left Win,
Right Win, Application all print **only** the `E0_`-prefixed row, no bare
duplicate underneath) settles it without relying on the doubled-row
mechanism at all.

Working hypothesis for what the doubling actually represents, offered for
whoever next opens the `.doc` and wants to check this call: several of the
doubled rows correspond to a byte value's real, documented reuse elsewhere in
the protocol — Left Shift's row `2A/AA` is doubled with `E0_2A/E0_AA`, which
*are* the real "fake shift" bytes the keyboard inserts around numpad keys
per Note 1/2 — while others (Backspace, digits, letters) have no such
referent anywhere and appear to be a table-generation artefact that
mechanically stamped `E0_` + hex(byte) onto every row regardless of whether
the combination means anything. This file did not need to fully resolve
*why* the doubling exists, because the two-source cross-check above already
pins every value this file publishes independently of it — but the anomaly
itself is reported here rather than quietly stepped around, per this
project's whole reason for existing.

## Extended-key rule, stated plainly

Scan Code Set 1 reused the plain keypad byte range for the newer 101-key
arrow/navigation cluster; an `0xE0` prefix immediately before the byte is
the *only* thing distinguishing "numeric keypad 4 / Left Arrow, sent as the
dedicated keypad key" (plain `4B`) from "the dedicated arrow-cluster Left
Arrow key" (`E0 4B`). The Windows `SendInput` backend must set
`KEYEVENTF_EXTENDEDKEY` for exactly the rows this file marks `Ext=yes`, and
must not set it for any other row — getting this backwards produces a
scancode that either silently does nothing or types the wrong key, with no
error from the API.

## What could not be verified, and what was assumed instead

- **PrintScreen and Pause are flagged `Ext=-` and given prose, not a
  make/break pair**, because their real behaviour is modifier-dependent
  (PrintScreen) or has no break code at all and uses a rarer `E1` prefix
  (Pause). Both sources agree on every byte quoted. What is **not**
  independently verified: the recommendation in the data file that the Windows
  backend inject these via `SendInput` with `VK_SNAPSHOT` / `VK_PAUSE` rather
  than raw scancodes. That recommendation is standard practice for those two
  keys specifically (raw PrintScreen scancode delivery is widely reported as
  unreliable), but it was not checked against current Windows behaviour in
  this task and should be treated as an assumption, not a verified fact.
- **Usages 0x66–0xDF and 0xE8–0xFF have no Scan Code Set 1 mapping in this
  file**, not because they were skipped for time but because the Microsoft
  document's own Appendix C never assigns them an AT-101 position — there is
  nothing in the primary source to extract. This includes locking
  Caps/Num/Scroll Lock variants, F13–F24, the Kanji/LANG IME keys, and the
  Execute/Help/Menu/Select/Stop/Again/Undo/Cut/Copy/Paste/Find block (listed
  "Boot: √ (Unix only)" in Appendix C, meaning no PC scancode was ever
  defined for them). Mute/Volume/Power were left out for a different reason:
  `docs/PROTOCOL.md` §6.15 doesn't carry them — see §6.17 `MEDIA` — so they
  are out of scope for this file regardless.
- **A third, fully independent re-derivation (kernel `hid_keyboard[]` +
  a separately-sourced evdev→Set-1 table) was not additionally performed.**
  The task allowed it as one acceptable second source; it wasn't needed here
  because the Microsoft document and Brouwer's page already agreed on every
  checked value with zero discrepancies. If a future check turns up a
  disagreement between this file and hardware, that third derivation is the
  next thing to try before assuming this file is wrong.
- **No value in either file was filled in from memory.** Every row is a
  direct transcription from a fetched source, cross-checked as described
  above. Where a recollection and a fetched source might have disagreed
  during this work, the fetched source was what got written down — per
  `docs/CONVENTIONS.md`'s rule for blind platforms, applied here to a
  blind *constant table* rather than a blind *platform*.

## What to do with these files

**Mirror them. Do not recall a keycode or scancode from memory, ever, for
either backend.** When writing the C tables in `server/` (not this
directory — the mapping engine lives in `server/`, not `core/`, per
`docs/CONVENTIONS.md`), copy the (usage, code) pairs out of these two files
mechanically. If a value here looks wrong, or a key a client needs isn't
covered, the fix is to re-derive from the primary sources named above (the
kernel tag, or the Microsoft document plus Brouwer's page), update these
files with the same provenance rigour, and only then update the C table —
never patch the C table directly from recollection "just this once." If
something in `server/` and something in these files disagree, treat that as
a bug in whichever one wasn't re-derived most recently, not as licence to
average the two.
