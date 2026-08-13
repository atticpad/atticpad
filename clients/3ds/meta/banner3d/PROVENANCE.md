# banner3d — the 3D HOME-menu banner

`meta/banner.bnr` is a real 3D banner: a CGFX scene of a low-poly handheld
console in the left foreground talking over its shoulder to a desktop PC
(monitor beside a tower) in the right background, under an extruded
greyish-blue "atticpad" wordmark, ringed by six translucent copies of the
logo's gamepad glyph orbiting the scene (the banner's only animation, and
deliberately nothing but a rigid node TRS — see "The particle ring" below;
the researched HOME-menu freeze class is soft-skinned animation), plus the
original banner sound. This directory holds everything needed to
regenerate it.

## Split with make-assets.py

`make-assets.py` (one level up) still owns the **icon** (`icon.png` →
`icon.icn`) and the 2D artwork. It also still writes `banner.png`, but that
image is **no longer what `banner.bnr` is built from** — the .bnr is built
from the 3D scene below. `banner.png` remains as the 2D reference artwork
and as make-assets.py output; do not delete it, and do not point
make-assets.py at this directory. Two files in THIS directory are
make-assets.py outputs, because the AtticPad mark may only be drawn there:
`mark.png` (`--banner3d-mark`) and `glyph.json` (`--banner3d-glyph`).

## Models

| file | source (license page verified 2026-08-12) | author | license |
|---|---|---|---|
| `handheld-console.glb` | https://poly.pizza/m/5kxD7n4F3lv ("Handheld videogame console") | Poly by Google | CC-BY 3.0 |
| `monitor.glb` | https://poly.pizza/m/V5Qo141OcB ("Computer Screen") | Kenney | CC0 |
| `tower.glb` | https://poly.pizza/m/4TZXM3lO5KK ("Computer Tower") | Ethan Yawney | CC-BY 3.0 |

Downloaded from poly.pizza's CDN
(`https://static.poly.pizza/fe464a15-02e0-4bec-b5ff-dc4353f3d8d3.glb`,
`https://static.poly.pizza/2af0158d-c560-4b1a-92e3-b50a51149b16.glb`,
`https://static.poly.pizza/5ebc59ac-bda8-4859-a207-0ae0b666fa56.glb`).

**Attribution (CC-BY 3.0 requirement):** the banner model incorporates
"Handheld videogame console" by **Poly by Google** and "Computer Tower" by
**Ethan Yawney**, both licensed under
[CC-BY 3.0](https://creativecommons.org/licenses/by/3.0/), via poly.pizza —
modified (retinted to the AtticPad palette, the handheld's texture replaced
with per-face materials, planar-decimated, recomposed). Keep this notice
wherever the banner assets are redistributed.

`compose_banner.py` retints all models to the AtticPad palette (slate
`#1C212B` bodies, amber `#F5B042` accents, cream `#FFE9C4` / warm amber
`#FFD98F` screen glows; the tower's red/green/purple LEDs become amber),
strips the handheld's 4-quadrant texture in favor of per-face solid
materials, and forces the tower's translucent panel opaque (no alpha
blending in the banner). The "atticpad" wordmark is a Blender text object
set lowercase in Russo One (below), colored the brand's greyish blue
`#8796AB` (= `ui_c_dim()` in `clients/3ds/source/ui.c`, slight emission,
same sRGB re-encode path as every other material), extruded and converted
to a mesh under a 600-triangle budget (falls back to Blender's bundled
Bfont if a future font swap blows the budget). The monitor's display shows
the AtticPad app: `mark.png`, planar-unwrapped onto the display faces
(aspect-preserving, clamped edges — pycgfx converts it to a 128x128 RGBA4
3DS texture, the scene's only texture). Final scene: 3,314 triangles
(handheld 927, tower 612, monitor 144, wordmark 479, particles 6 x 192),
one texture, one rigid animation (the particle orbit), and **no
background geometry** — the HOME menu backdrop shows through around the
models.

## The mark on the monitor (`mark.png`)

Generated — never hand-drawn — by `../make-assets.py --banner3d-mark`
(128x128, the mark centered on slate at the script's usual 8x supersample).
render_mark() there is the single place the AtticPad mark may be drawn;
regenerate with:

```sh
python3 clients/3ds/meta/make-assets.py --banner3d-mark   # from repo root
```

## Font

`RussoOne-Regular.ttf` — **Russo One** by **Jovanny Lemonad** (jovanny.ru),
licensed under the **SIL Open Font License 1.1** (full text alongside in
`RussoOne-OFL.txt`, as the OFL requires). Fetched from the google/fonts
repository:
https://raw.githubusercontent.com/google/fonts/main/ofl/russoone/RussoOne-Regular.ttf

## Tools (external, NOT vendored)

* **Blender 4.5 LTS** (tested 4.5.12, portable Linux x64 from
  download.blender.org) — scene composition and GLB export, headless.
* **pycgfx** — https://github.com/skyfloogle/pycgfx, commit
  `1f78850086f3a77c41e07162e842f97a5bf3c18a` — glTF → CGFX conversion.
  **pycgfx has no license file**, so it is used strictly as an external
  tool: fetched by hand, never vendored into this repo, and none of its
  code or assets are redistributed here. Its `banner-camera.gltf` documents
  the official HOME-menu banner camera (perspective, yfov 0.523599, aspect
  5:3, glTF position [0, 1, 44.786], znear 26.5); `compose_banner.py`
  recreates that camera from those numbers rather than shipping the file.
  Requires `gltflib` and `pillow` (pip).
* **`convert_cgfx.py`** (this directory) — drives pycgfx's `convert_gltf`
  and asserts the output matches the hardware-proven shape: one node bone
  per scene object under pycgfx's synthetic "Scene root", billboard_mode 0
  on every bone, zero animations/skins, no skinning modes, no
  bone-index/weight vertex streams. Its default output is byte-identical
  to running pycgfx's own main.py.

  **HARDWARE INCIDENT RECORD (2026-08-12, resolved).** Two consoles froze
  on regenerated banners and the blame moved twice — first to the joined
  identity-node + YAxial billboard build, then to the recomposed
  ellipse/depth-flattened layout. Forensics reconstructed the console's
  exception dump to the byte: the real cause was pack_bnr.py placing the
  CWAV at 0x88 + len(LZ11 stream) — an effectively random offset,
  misaligned 3 times in 4 — and the HOME menu's CWAV parser does 32-bit
  loads, so a misaligned chunk data-aborts the whole menu (ARM11 alignment
  fault). bannertool always padded to 16; our packer now does the same and
  verify() asserts alignment. Geometry, the mesh join, the billboard flag
  and both layouts are ALL EXONERATED — the hold-still build (ellipse
  layout + --join + --billboard) runs on real hardware with the fixed
  packer.

* **`pack_bnr.py`** (this directory) — CBMD packing per
  https://www.3dbrew.org/wiki/CBMD, with its own LZ11 codec (decompressor
  cross-checked against the bannertool-compressed CGFX in the previous
  banner.bnr).

## Sound

The CWAV chunk is carried over **byte-for-byte** from the previous
`banner.bnr` (originally produced by bannertool from `../audio.wav`).
`pack_bnr.py extract-cwav` pulls it out of any existing banner.

## The particle ring (`--particles`)

Six small semi-transparent copies of the logo's EXACT gamepad glyph
(imported from `glyph.json`, below — 192 tris each, amber at alpha 0.35,
alphaMode BLEND, double-sided) orbit the scene
under a single "Orbit" node — a sibling of the billboarded "Banner" node,
never joined into it, so the scene holds still while the ring turns.
**Animation rule: rigid node TRS only.** One glTF animation, LINEAR
quarter-turn keys, 360 degrees per ~15 s, exported to a looping CANM the
way Nintendo's AR Games banner and the ClouDS-Music-FA reference do it.
The researched HOME-menu freeze class is SOFT-SKINNED animation — skins,
skinning modes, and bone-index/weight vertex streams are categorically
excluded and hard-asserted absent by convert_cgfx.py in every mode.

## The particle glyph (`glyph.json`)

Generated — never hand-drawn — by `../make-assets.py --banner3d-glyph`:
the mark's gamepad-glyph outline (body + D-pad cross + two face buttons)
as normalized polygon data (`outer` + `holes`, x in [-1, 1], y up, aspect
preserved). The export does not describe the shape at all: it renders the
glyph with the SAME `pad_solid()`/`draw_pad_holes()` calls the raster art
uses, traces the mask's boundary, simplifies it (Douglas-Peucker, then a
Visvalingam-Whyatt trim to a point cap that maps onto compose_banner.py's
200-triangle particle budget), and self-checks by re-rasterizing the
polygons against the source mask. That keeps the one-place rule from
make-assets.py's header intact — the Android VectorDrawable drift incident
is exactly what a hand-copied Blender glyph would reprise.
`compose_banner.py` imports the file and extrudes it (thin, holes kept);
it never draws the shape itself. Regenerate with:

```sh
python3 clients/3ds/meta/make-assets.py --banner3d-glyph   # from repo root
```

`--glyph-tol` (curve tolerance) and `--glyph-pts` (total point cap; the
default 46 points = 192 extruded triangles) are the coarsening knobs if a
future glyph change blows the particle triangle budget.

## Regenerating

```sh
cd clients/3ds/meta/banner3d

# 0. only if the mark itself changed: re-export the particle glyph
#    (cd ../../../..; python3 clients/3ds/meta/make-assets.py --banner3d-glyph)

# 1. compose scene, render approval preview, export GLB
blender -b -noaudio --python compose_banner.py -- \
    --layout ellipse --join --particles \
    --out-glb /tmp/banner-scene.glb --out-preview preview.png

# 2. glTF -> CGFX (decompressed size must stay <= 512 KB)
python3 convert_cgfx.py /path/to/pycgfx /tmp/banner-scene.glb /tmp/banner.cgfx \
    --billboard --particles

# 3. keep the existing sound, pack, and structurally verify
python3 pack_bnr.py extract-cwav ../banner.bnr /tmp/banner.cwav
python3 pack_bnr.py pack /tmp/banner.cgfx /tmp/banner.cwav ../banner.bnr
python3 pack_bnr.py verify ../banner.bnr /tmp/banner.cwav
```

`preview.png` (400x240, true alpha channel) is rendered from the exact
banner camera and is the approval reference for how the HOME menu should
frame the scene. `preview-home.png` is the same render composited over the
HOME menu's default light grey (#ECECEC) — use it when a viewer paints
transparency as white. `preview-ellipse.png` overlays the HOME menu's
elliptical vignette; the shipping (ellipse) layout must show zero escapes.
With `--particles`, the gate additionally runs at orbit phases 45 and 90
degrees (`preview-ellipse-p45.png` / `-p90.png`) so the ring clears the
vignette at every point of its revolution.

## Known unverifiables

Rendering on a real HOME menu (colors, lighting, clipping, and the absence
of freezes) has not been checked on hardware from this machine. Two
assumptions are baked in: the 3DS displays material colors raw (no sRGB
decode) — so `compose_banner.py` re-encodes baseColorFactor to sRGB
fractions after export — and the HOME menu's own lighting is close enough
to neutral white that the palette survives it.
