#!/usr/bin/env python3
"""Regenerate the 3DS icon/banner and the Android launcher/notification icons.

    python3 clients/3ds/meta/make-assets.py [--transparent]   # 3DS only
    python3 clients/3ds/meta/make-assets.py --android          # Android only
    python3 clients/3ds/meta/make-assets.py --windows          # Windows .ico
    python3 clients/3ds/meta/make-assets.py --banner3d-mark    # 3D banner tex
    python3 clients/3ds/meta/make-assets.py --banner3d-glyph   # 3D banner
                                                               # particle outline

Run from the repo root. The two modes are mutually exclusive and touch
disjoint files: the default mode writes icon.png (48x48) and banner.png
(256x128) beside this script (feed those to bannertool to produce
icon.icn/banner.bnr); --android writes PNGs under
clients/android/app/src/main/res/{mipmap,drawable}-<density>/.

WHY ANDROID LIVES HERE TOO: render_mark() below is the ONE place the
AtticPad mark is drawn. It used to be hand-ported into a second copy as
Android VectorDrawable XML, and that copy drifted almost immediately — the
D-pad was two overlapping rectangles cut with evenOdd, so the overlap
filled back in and the cross rendered as a checkerboard on a real device.
A generator that only one platform can call is exactly the setup that
produces a second, silently-wrong copy; this file is now that generator for
both platforms. See make_android_assets() near the bottom. If a THIRD
platform ever needs a raster version of the mark, it grows a mode here too
— it does not get its own drawing code.

DO NOT HAND-EDIT ANY PNG THIS SCRIPT WRITES. If the mark needs to change,
change render_mark() (or, for Android's notification silhouette, the
bespoke-but-shared pad_holes() geometry) and rerun.

WHY THIS EXISTS: so the artwork is reproducible source rather than a pile of
binary blobs nobody can edit. Everything is drawn at 8x and downsampled,
which is what keeps a 48x48 icon's edges clean.

ON 3D: the 3DS banner format is genuinely 3D — it can carry a CGFX model with
stereoscopic left/right views, which bannertool accepts via -ci. Authoring
CGFX needs Nintendo's toolchain, so this script instead fakes depth with an
extruded silhouette, a blurred contact shadow and a directional sheen. Real
stereoscopy remains a future improvement; a future improvement.

ON TRANSPARENCY: verified supported for the 3DS banner. Identical pixels
with and without alpha produce different .bnr output, while RGB and
fully-opaque RGBA produce byte-identical files — so alpha is genuinely
encoded, not discarded. It is OFF by default because how a transparent
banner composites against the HOME menu backdrop has never been checked on
hardware, and shipping an unverified look is worse than shipping a plain
one. Pass --transparent once you can confirm it on a console.
"""
import io, json, math, os, re, sys
from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageFilter

TRANSPARENT = "--transparent" in sys.argv
ANDROID     = "--android" in sys.argv
WINDOWS     = "--windows" in sys.argv
BANNER3D    = "--banner3d-mark" in sys.argv
GLYPH       = "--banner3d-glyph" in sys.argv

SS       = 8                      # supersample factor
SLATE    = (28, 33, 43)
AMBER    = (245, 176, 66)
AMBER_HI = (255, 206, 120)
AMBER_LO = (150, 98, 26)
INSET    = (120, 74, 16)
CREAM    = (243, 238, 228)
GREY     = (150, 160, 175)

def pad_solid(d, cx, cy, w, h, fill):
    """Gamepad silhouette: rounded body with flared grips."""
    d.rounded_rectangle([cx-w/2, cy-h/2, cx+w/2, cy+h/2], radius=h*0.42, fill=fill)
    d.ellipse([cx-w/2-h*0.10, cy-h*0.10, cx-w/2+h*0.62, cy+h*0.72], fill=fill)
    d.ellipse([cx+w/2-h*0.62, cy-h*0.10, cx+w/2+h*0.10, cy+h*0.72], fill=fill)

def chevron(d, cx, cy, halfw, drop, colour, thick):
    """The attic roofline the project is named for."""
    d.line([(cx-halfw, cy+drop), (cx, cy), (cx+halfw, cy+drop)],
           fill=colour, width=int(thick), joint="curve")

def pad_holes(cx, cy, w, h, hole_scale=1.0):
    """Bounding boxes of the D-pad cross (two bars) and the two round face
    buttons, as [x0,y0,x1,y1] -- shared by render_mark's coloured inset
    render AND clients/android's flat notification silhouette, so the two
    never hand-derive slightly different hole geometry.

    hole_scale widens the holes without moving their centres -- the
    notification icon needs bolder cuts than the coloured mark to still read
    once flattened to a 24dp single-colour glyph (see make_android_assets);
    it stays 1.0 (a no-op) everywhere the 3DS art is generated.
    """
    aw, al, lx = h*0.13*hole_scale, h*0.40*hole_scale, cx-w*0.27
    br, rx = h*0.115*hole_scale, cx+w*0.24
    return [
        [lx-aw/2, cy-al/2, lx+aw/2, cy+al/2],                       # D-pad vertical bar
        [lx-al/2, cy-aw/2, lx+al/2, cy+aw/2],                       # D-pad horizontal bar
        [rx-br, cy-br-h*0.13, rx+br, cy+br-h*0.13],                 # face button A
        [rx-br+h*0.17, cy-br+h*0.10, rx+br+h*0.17, cy+br+h*0.10],   # face button B
    ]

def draw_pad_holes(d, cx, cy, w, h, off, col, hole_scale=1.0):
    """Paint pad_holes()'s four shapes, each corner-shifted by `off` (render_
    mark's inset-shadow trick: draw once offset in a darker colour, again at
    off=0 in the real colour) -- the two rectangles are D-pad bars, the two
    ellipses are face buttons."""
    b = [[v+off for v in box] for box in pad_holes(cx, cy, w, h, hole_scale)]
    d.rectangle(b[0], fill=col)
    d.rectangle(b[1], fill=col)
    d.ellipse(b[2], fill=col)
    d.ellipse(b[3], fill=col)

def extrude(draw_fn, steps, depth):
    """Stack darkened copies back-to-front to fake a side wall."""
    for i in range(steps, 0, -1):
        t = i / steps
        shade = tuple(int(AMBER_LO[c] * (0.55 + 0.45*(1-t))) for c in range(3))
        draw_fn(depth*t*0.55, depth*t, shade + (255,))

def render_mark(im, cx, cy, w, h, with_chevron, chev_cx=None, chev_cy=None,
                chev_halfw=0, chev_drop=0, chev_thick=0, depth_scale=1.0):
    """Draw the extruded mark with shadow, side walls, top face and sheen."""
    W, H = im.size
    d = ImageDraw.Draw(im)
    # Depth is dialled back for the icon: at 48x48 a deep extrusion muddies
    # the silhouette, and a HOME-menu icon has to read at a glance more than
    # it has to look sculpted. The banner is 5x the area and can carry it.
    depth = h * 0.30 * depth_scale

    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pad_solid(ImageDraw.Draw(shadow), cx+depth*0.5, cy+depth*1.15, w, h, (0, 0, 0, 150))
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(H*0.045)))

    extrude(lambda dx, dy, col: pad_solid(d, cx+dx, cy+dy, w, h, col), 26, depth)
    pad_solid(d, cx, cy, w, h, AMBER + (255,))

    # directional sheen across the upper-left of the top face
    gl = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    pad_solid(ImageDraw.Draw(gl), cx, cy, w, h, AMBER_HI + (255,))
    lit = Image.new("L", (W, H), 0)
    ImageDraw.Draw(lit).polygon([(0, 0), (W, 0), (0, H)], fill=90)
    gl.putalpha(Image.composite(lit, Image.new("L", (W, H), 0),
                                gl.split()[3].point(lambda a: 255 if a else 0)))
    im.alpha_composite(gl.filter(ImageFilter.GaussianBlur(H*0.012)))

    # inset controls: a drop-shadow pass, then the face
    for off, col in ((h*0.022, INSET + (255,)), (0, SLATE + (255,))):
        draw_pad_holes(d, cx, cy, w, h, off, col)

    if with_chevron:
        extrude(lambda dx, dy, col: chevron(d, chev_cx+dx, chev_cy+dy,
                                            chev_halfw, chev_drop, col, chev_thick),
                26, depth)
        chevron(d, chev_cx, chev_cy, chev_halfw, chev_drop, AMBER + (255,), chev_thick)

def ground(size):
    return Image.new("RGBA", size, (0, 0, 0, 0) if TRANSPARENT else SLATE + (255,))

# ======================================================================
# banner3d particle glyph export (--banner3d-glyph)
#
# The 3D banner's particle ring (banner3d/compose_banner.py --particles)
# orbits six translucent copies of the mark's gamepad glyph.  Blender code
# re-describing that glyph by hand would be exactly the second-copy drift
# this file's header exists to prevent (the Android VectorDrawable
# incident), so this mode does not describe the shape at all: it RENDERS
# the glyph with the very same pad_solid() + draw_pad_holes() calls the
# raster art uses, TRACES the resulting binary mask's boundary, simplifies
# it, and writes normalized polygon data to banner3d/glyph.json.
# compose_banner.py imports that file and extrudes it; neither side ever
# hand-derives the geometry.  A self-check re-rasterizes the exported
# polygons and asserts they match the source mask.
#
#     python3 clients/3ds/meta/make-assets.py --banner3d-glyph \
#         [--glyph-tol 0.010] [--glyph-pts 46]
#
# Two resolution knobs:
#   --glyph-tol   outline-simplification tolerance (Douglas-Peucker, keeps
#                 corners) as a fraction of the glyph's width;
#   --glyph-pts   hard cap on total exported points, enforced after DP by
#                 a Visvalingam-Whyatt trim (drops least-area vertices, so
#                 arcs coarsen before corners go).  This is the knob that
#                 maps onto compose_banner.py's 200-triangle particle
#                 budget: the extruded mesh costs 4*points + 4*holes - 4
#                 triangles, so the default 46 points = 192 triangles.
# ======================================================================

GLYPH_JSON        = "clients/3ds/meta/banner3d/glyph.json"
GLYPH_MASK_S      = 1536   # trace-mask canvas; ~1000 px across the glyph
GLYPH_TOL_DEFAULT = 0.010  # simplification tolerance, fraction of width
GLYPH_PTS_DEFAULT = 46     # total point cap (46 pts + 3 holes -> 192 tris)

def _trace_mask(filled, w, h):
    """Every closed boundary loop of a binary pixel mask, as lattice-point
    polygons (pixel (x,y) spans [x,x+1]x[y,y+1] in image coords, y down).
    Boundary cracks are emitted directed so the filled side sits on the
    travel direction's RIGHT, then stitched start->end into loops; outer
    outlines and hole outlines fall out as separate loops."""
    edges = {}
    def add(a, b):
        edges.setdefault(a, []).append(b)
    for y in range(h):
        for x in range(w):
            if not filled(x, y):
                continue
            if not filled(x, y - 1):
                add((x, y), (x + 1, y))          # top side, travel +x
            if not filled(x + 1, y):
                add((x + 1, y), (x + 1, y + 1))  # right side, travel +y
            if not filled(x, y + 1):
                add((x + 1, y + 1), (x, y + 1))  # bottom side, travel -x
            if not filled(x - 1, y):
                add((x, y + 1), (x, y))          # left side, travel -y
    loops = []
    while edges:
        start = next(iter(edges))
        loop = [start]
        cur, prev_dir = start, None
        while True:
            outs = edges[cur]
            if len(outs) == 1 or prev_dir is None:
                nxt = outs[-1]
            else:
                # two contours touch diagonally at this lattice point: take
                # the sharpest right turn so the loops stay separate
                dx, dy = prev_dir
                pref = [(-dy, dx), (dx, dy), (dy, -dx)]  # right, straight, left
                nxt = min(outs, key=lambda p: pref.index(
                    (p[0] - cur[0], p[1] - cur[1])))
            outs.remove(nxt)
            if not outs:
                del edges[cur]
            prev_dir = (nxt[0] - cur[0], nxt[1] - cur[1])
            if nxt == start:
                break
            loop.append(nxt)
            cur = nxt
        loops.append(loop)
    return loops

def _compress_collinear(loop):
    out = []
    n = len(loop)
    for i, p in enumerate(loop):
        a, b = loop[i - 1], loop[(i + 1) % n]
        if (p[0] - a[0], p[1] - a[1]) != (b[0] - p[0], b[1] - p[1]):
            out.append(p)
    return out

def _dp(pts, i, j, tol, keep):
    """Douglas-Peucker over pts[i..j] (endpoints already kept)."""
    ax, ay = pts[i]
    bx, by = pts[j]
    dx, dy = bx - ax, by - ay
    norm = math.hypot(dx, dy)
    best, bi = -1.0, None
    for k in range(i + 1, j):
        px, py = pts[k]
        if norm:
            d = abs((px - ax) * dy - (py - ay) * dx) / norm
        else:
            d = math.hypot(px - ax, py - ay)
        if d > best:
            best, bi = d, k
    if bi is not None and best > tol:
        _dp(pts, i, bi, tol, keep)
        keep.add(bi)
        _dp(pts, bi, j, tol, keep)

def _simplify_closed(loop, tol):
    """DP-simplify a closed loop: rotate it to start at the point farthest
    from the centroid, anchor a second point farthest from that one, and
    run DP over the two halves."""
    cx = sum(p[0] for p in loop) / len(loop)
    cy = sum(p[1] for p in loop) / len(loop)
    i0 = max(range(len(loop)),
             key=lambda i: (loop[i][0] - cx) ** 2 + (loop[i][1] - cy) ** 2)
    loop = loop[i0:] + loop[:i0]
    j = max(range(len(loop)),
            key=lambda i: (loop[i][0] - loop[0][0]) ** 2
                        + (loop[i][1] - loop[0][1]) ** 2)
    pts = loop + [loop[0]]
    keep = {0, j}
    _dp(pts, 0, j, tol, keep)
    _dp(pts, j, len(pts) - 1, tol, keep)
    return [loop[i] for i in sorted(keep)]

def _signed_area(poly):
    s = 0.0
    for i, (x, y) in enumerate(poly):
        x2, y2 = poly[(i + 1) % len(poly)]
        s += x * y2 - x2 * y
    return s / 2.0

def _vw_trim(loops, max_pts):
    """Visvalingam-Whyatt trim: repeatedly drop the vertex whose removal
    loses the least area, across all rings, until the total point count
    fits max_pts.  Corners contribute large triangles and survive; arcs
    coarsen first.  No ring shrinks below 4 points."""
    loops = [list(l) for l in loops]
    def tri_area(l, i):
        (ax, ay), (bx, by), (cx, cy) = l[i - 1], l[i], l[(i + 1) % len(l)]
        return abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) / 2.0
    while sum(len(l) for l in loops) > max_pts:
        best = None
        for li, l in enumerate(loops):
            if len(l) <= 4:
                continue
            for i in range(len(l)):
                a = tri_area(l, i)
                if best is None or a < best[0]:
                    best = (a, li, i)
        assert best is not None, "glyph point budget unreachable"
        loops[best[1]].pop(best[2])
    return loops

def make_banner3d_glyph(tol_frac, max_pts):
    S = GLYPH_MASK_S
    # The mark's canonical glyph proportions: every render_mark() call site
    # for the app mark uses w:h = 0.60:0.29 of its canvas; placement on the
    # trace canvas is irrelevant (coordinates are normalized below).
    cx, cy, w, h = S / 2, S / 2, S * 0.60, S * 0.29

    # THE shared raster path — the same two calls render_mark() makes for
    # the body and its punched controls (no offset pass, no chevron: the
    # glyph is the gamepad silhouette only).
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    pad_solid(md, cx, cy, w, h, 255)
    draw_pad_holes(md, cx, cy, w, h, 0, 0)

    px = mask.load()
    filled = lambda x, y: 0 <= x < S and 0 <= y < S and px[x, y] >= 128
    loops = [_compress_collinear(l) for l in _trace_mask(filled, S, S)]

    xs = [p[0] for l in loops for p in l]
    ys = [p[1] for l in loops for p in l]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    width = x1 - x0
    tol = tol_frac * width
    loops = [_simplify_closed(l, tol) for l in loops]
    loops = _vw_trim(loops, max_pts)

    # classify: the largest loop is the body outline, the rest are holes
    loops.sort(key=lambda l: -abs(_signed_area(l)))
    outer, holes = loops[0], loops[1:]
    assert len(holes) == 3, (
        "expected the pad body + 3 holes (D-pad cross, 2 face buttons), "
        "got %d loops — did pad_holes() change shape?" % len(loops))

    # self-check: the exported polygons, re-rasterized, must reproduce the
    # source mask.  This guards the TRACER (a broken loop, a lost hole, a
    # mis-stitched contour diverges by 10%+), not the deliberate chord loss
    # of simplification — DP vertices sit ON the contour, so every convex
    # arc loses a sliver proportional to the tolerance; the allowance
    # scales with it (~2% at the 0.010 default).
    chk = Image.new("L", (S, S), 0)
    cd = ImageDraw.Draw(chk)
    cd.polygon([tuple(p) for p in outer], fill=255)
    for hole in holes:
        cd.polygon([tuple(p) for p in hole], fill=0)
    bad = sum(ImageChops.difference(mask, chk).histogram()[128:])
    filled_n = sum(mask.histogram()[128:])
    allow = 0.005 + 2.5 * tol_frac
    assert bad <= filled_n * allow, (
        "exported polygons diverge from the rendered mask by %.2f%% "
        "(allowance %.2f%% at tol %g — tracer bug, or tolerance far too "
        "coarse)" % (100.0 * bad / filled_n, 100.0 * allow, tol_frac))

    # normalize: x in [-1, 1] across the glyph's full width, aspect
    # preserved, y flipped to mathematical (up-positive) orientation
    mx, my, sc = (x0 + x1) / 2, (y0 + y1) / 2, 2.0 / width
    norm = lambda l: [[round((x - mx) * sc, 4), round((my - y) * sc, 4)]
                      for x, y in l]
    out = {
        "comment": "GENERATED by make-assets.py --banner3d-glyph — do not "
                   "hand-edit; the mark is drawn in ONE place (see that "
                   "file's header). Consumed by banner3d/compose_banner.py.",
        "source": "pad_solid()+draw_pad_holes(), w:h = 0.60:0.29, "
                  "hole_scale 1.0, traced at %dpx, DP tol %g of width, "
                  "%d-pt cap" % (GLYPH_MASK_S, tol_frac, max_pts),
        "aspect": round((y1 - y0) / width, 4),
        "outer": norm(outer),
        "holes": [norm(h) for h in holes],
    }
    with open(GLYPH_JSON, "w") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print("  wrote %s: outline %d pts, holes %s pts, "
          "mask mismatch %.3f%%, aspect %.4f"
          % (GLYPH_JSON, len(outer), [len(h) for h in holes],
             100.0 * bad / filled_n, out["aspect"]))

# ======================================================================
# Android launcher + notification icons
#
# Same render_mark()/pad_solid()/chevron() as the 3DS art above; only the
# composition differs (see the module docstring). Regenerate with:
#
#     python3 clients/3ds/meta/make-assets.py --android
#
# DO NOT hand-edit any PNG under clients/android/app/src/main/res/ — if the
# mark needs to change, change it here and rerun.
# ======================================================================

ANDROID_RES = "clients/android/app/src/main/res"
# mdpi is the 1x baseline every other Android density bucket is a multiple
# of (developer.android.com/training/multiscreen/screendensities).
ANDROID_DENSITIES = (("mdpi", 1.0), ("hdpi", 1.5), ("xhdpi", 2.0),
                     ("xxhdpi", 3.0), ("xxxhdpi", 4.0))

def _android_legacy_master(px, round_variant):
    """Legacy (API<26) launcher icon at `px` size: the SAME render_mark()
    call the 3DS 48x48 icon uses (identical fractions of S — literally the
    same composition, just re-rendered at Android's own sizes), on a flat
    SLATE plate. round_variant swaps the 3DS icon's rounded-rect plate for a
    full circle, since a pre-26 "round" launcher icon slot expects
    already-circular art (no OS-applied mask exists yet to do it for us)."""
    S = int(px * SS)
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if round_variant:
        d.ellipse([0, 0, S-1, S-1], fill=SLATE + (255,))
    else:
        d.rounded_rectangle([0, 0, S-1, S-1], radius=S*0.18, fill=(38, 45, 58, 255))
    render_mark(im, S/2, S*0.60, S*0.60, S*0.29, True,
                S/2, S*0.225, S*0.29, S*0.130, S*0.055, depth_scale=0.45)
    return im.resize((px, px), Image.LANCZOS)

# ---- adaptive icon foreground -----------------------------------------
#
# 108dp canvas; a launcher mask (circle, squircle, rounded square — the
# platform's choice at RUNTIME, not build time) is only guaranteed not to
# clip a centred circle 66dp in diameter
# (developer.android.com/develop/ui/views/launch/icon_design_adaptive), so
# the mark must be scaled down to fit a 33dp-radius circle around the
# canvas centre — a smaller relative mark than the 3DS icon, which fills
# its whole tile.
#
# Rather than hand-computing the extremal points of the extruded silhouette
# (real, and doable, but the exact reasoning `ic_launcher_foreground.xml`
# used to justify ITS by-hand geometry — the very artifact this change
# deletes because a second hand-derivation is a second thing to keep in
# sync), this fits the mark by RENDERING and MEASURING: same pattern as the
# banner subtitle auto-fit loop above, just in 2D. Render at a trial scale,
# measure the actual alpha bounding box (conservatively, at its CORNERS —
# the true silhouette is inset from there, so this never ships something
# that clips, only something very slightly smaller than the true maximum),
# and rescale by the ratio to target. Converges in a handful of passes
# because render_mark's own geometry is close to linear in scale; the
# GaussianBlur passes are not perfectly linear (their radius is a fraction
# of the CANVAS, not of the mark), which is exactly why this is measured
# empirically rather than solved in closed form.
FOREGROUND_DP  = 108
SAFE_RADIUS_DP = 33          # half of the 66dp guaranteed-visible circle
SAFE_MARGIN    = 0.97        # leave 3% of the radius as breathing room

def _foreground_geometry():
    """Returns (scale_dp, pad_cy_off_dp, chev_cy_off_dp) for the adaptive
    foreground mark, found by the fit loop described above. All are
    fractions of FOREGROUND_DP, so they transfer unchanged to every
    density's own supersampled canvas."""
    S = FOREGROUND_DP * SS
    cx = cy = S / 2
    px_per_dp = S / FOREGROUND_DP
    target_r = SAFE_RADIUS_DP * px_per_dp * SAFE_MARGIN

    def render(scale_dp):
        im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        s = scale_dp * px_per_dp
        pad_cy = cy + 0.10*s
        chev_cy = cy - 0.275*s
        render_mark(im, cx, pad_cy, 0.60*s, 0.29*s, True,
                    cx, chev_cy, 0.29*s, 0.130*s, 0.055*s, depth_scale=0.45)
        return im

    def max_radius(im, thresh=16):
        # thresh ignores the blurred shadow's faint tail so it does not
        # dominate the fit; the shadow is meant to soften off-canvas anyway.
        mask = im.split()[3].point(lambda a: 255 if a > thresh else 0)
        bbox = mask.getbbox()
        if bbox is None:
            return 0.0
        x0, y0, x1, y1 = bbox
        return max(((x-cx)**2 + (y-cy)**2)**0.5
                    for x, y in ((x0,y0), (x1,y0), (x0,y1), (x1,y1)))

    scale_dp = 90.0
    for _ in range(5):
        r = max_radius(render(scale_dp))
        if r == 0:
            raise RuntimeError("android foreground fit: empty render")
        scale_dp *= target_r / r
    r = max_radius(render(scale_dp))
    assert r <= target_r * 1.02, (
        f"android adaptive-icon foreground still exceeds the 66dp safe zone "
        f"after fitting: {r/px_per_dp:.2f}dp > {target_r/px_per_dp:.2f}dp")
    return scale_dp

def _android_foreground_master(px, scale_dp):
    S = int(px * SS)
    cx = cy = S / 2
    s = scale_dp * (S / FOREGROUND_DP)
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    render_mark(im, cx, cy + 0.10*s, 0.60*s, 0.29*s, True,
                cx, cy - 0.275*s, 0.29*s, 0.130*s, 0.055*s, depth_scale=0.45)
    return im.resize((px, px), Image.LANCZOS)

# ---- notification icon --------------------------------------------------
#
# The platform masks this to a flat single-colour (usually white)
# silhouette using ONLY the alpha channel, so it is rendered flat here too
# — no extrude(), no shadow, no sheen, just pad_solid()'s body with
# pad_holes() punched through to transparent, and a solid chevron stroke.
#
# The holes and the chevron/body gap both need to be BOLDER than the
# coloured mark's proportions to still read once flattened to 24dp: this
# was tuned by actually rendering at 24px and viewing it (not eyeballed at
# the 108-unit working resolution, which hides exactly this problem) —
# pad_holes()'s own 1.0-scale holes closed up into a grey smear at 24px,
# and the chevron with no gap fused into the body's top edge and read as a
# roofline/car-hood blob rather than two separate shapes. hole_scale=1.7
# and an explicit gap between the chevron and the body fix both; see the
# report for what still does NOT fully resolve at 24dp (the two face
# buttons touch into one blob) and why that was judged acceptable while a
# closed-up cross or a fused chevron were not.
NOTIFICATION_DP = 24
_NOTIF_W_FRAC, _NOTIF_H_FRAC   = 0.68, 0.32
_NOTIF_PAD_CY_FRAC             = 0.68
_NOTIF_GAP_FRAC                = 0.09
_NOTIF_CHEV_HALFW_FRAC         = 0.30
_NOTIF_CHEV_DROP_FRAC          = 0.12
_NOTIF_CHEV_THICK_FRAC         = 0.08
_NOTIF_HOLE_SCALE              = 1.7

def _android_notification_master(px):
    S = int(px * SS)
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    cx, cy = S/2, S*_NOTIF_PAD_CY_FRAC
    w, h = S*_NOTIF_W_FRAC, S*_NOTIF_H_FRAC
    pad_solid(md, cx, cy, w, h, 255)
    draw_pad_holes(md, cx, cy, w, h, 0, 0, hole_scale=_NOTIF_HOLE_SCALE)
    pad_top = cy - h/2
    chev_cy = pad_top - S*_NOTIF_GAP_FRAC - S*_NOTIF_CHEV_DROP_FRAC
    chevron(md, cx, chev_cy, S*_NOTIF_CHEV_HALFW_FRAC, S*_NOTIF_CHEV_DROP_FRAC,
            255, S*_NOTIF_CHEV_THICK_FRAC)
    white = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    white.putalpha(mask)
    return white.resize((px, px), Image.LANCZOS)

# ---- Windows .ico ------------------------------------------------------
#
# The tray icon and the .exe icon for server/host/windows/. Same mark, same
# render_mark() call, same rounded-slate plate as the Android legacy
# launcher icon -- _android_legacy_master() IS the composition, so this
# mode reuses it outright rather than describing the mark a third time.
#
# EVERY SIZE IS ITS OWN RENDER, not one image downsampled by the ICO
# encoder. At 16x16 -- which is the size Windows actually shows in the tray
# and the title bar -- the difference between "rendered at 16 with 8x
# supersampling" and "rendered at 256 then crushed to 16" is the difference
# between a legible pad and amber mush.
#
# The file is assembled by hand because PIL's ICO writer resizes a single
# source image for every requested size, which is precisely what the
# paragraph above rules out. The format is small enough that this is
# clearer than fighting it: a 6-byte ICONDIR, one 16-byte ICONDIRENTRY per
# image, then the payloads. Payloads are PNG, which every Windows since
# Vista accepts for any size (the old BMP+AND-mask encoding is only needed
# for XP, which this project does not target -- shim/net_winsock.c already
# sets _WIN32_WINNT to 0x0601, Windows 7).
WINDOWS_ICO   = "server/host/windows/atticpad.ico"
WINDOWS_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

def make_windows_ico():
    import struct

    entries = []
    for px in WINDOWS_SIZES:
        im = _android_legacy_master(px, round_variant=False)
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        entries.append((px, buf.getvalue()))

    # 0 in the width/height byte means 256 -- the field is one byte, so 256
    # does not fit and the format spells it this way.
    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = len(header) + 16 * len(entries)
    directory, payloads = b"", b""
    for px, data in entries:
        dim = 0 if px == 256 else px
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                                 len(data), offset)
        payloads += data
        offset += len(data)

    os.makedirs(os.path.dirname(WINDOWS_ICO), exist_ok=True)
    with open(WINDOWS_ICO, "wb") as f:
        f.write(header + directory + payloads)
    total = len(header) + len(directory) + len(payloads)
    print(f"  {WINDOWS_ICO}: {len(entries)} images "
          f"({', '.join(str(p) for p in WINDOWS_SIZES)}), {total} bytes")
    return total

def make_android_assets():
    scale_dp = _foreground_geometry()
    n = 0
    for density, mult in ANDROID_DENSITIES:
        mip = f"{ANDROID_RES}/mipmap-{density}"
        drw = f"{ANDROID_RES}/drawable-{density}"
        os.makedirs(mip, exist_ok=True)
        os.makedirs(drw, exist_ok=True)

        legacy_px = round(48 * mult)
        _android_legacy_master(legacy_px, round_variant=False).save(f"{mip}/ic_launcher.png")
        _android_legacy_master(legacy_px, round_variant=True).save(f"{mip}/ic_launcher_round.png")

        fg_px = round(108 * mult)
        _android_foreground_master(fg_px, scale_dp).save(f"{mip}/ic_launcher_foreground.png")

        notif_px = round(24 * mult)
        _android_notification_master(notif_px).save(f"{drw}/ic_notification.png")

        n += 4
        print(f"  {density}: ic_launcher.png {legacy_px}px, "
              f"ic_launcher_round.png {legacy_px}px, "
              f"ic_launcher_foreground.png {fg_px}px, "
              f"ic_notification.png {notif_px}px")
    print(f"  wrote {n} PNGs under {ANDROID_RES}/ "
          f"(foreground mark scale {scale_dp:.2f}dp of a 108dp canvas)")

if GLYPH:
    # Mutually exclusive with the other modes, same as --banner3d-mark:
    # writes exactly one file (banner3d/glyph.json — see the export section
    # above for why the shape is traced from the raster path rather than
    # re-described).
    print("AtticPad: banner3d particle glyph (gamepad outline -> polygons)")
    tol = GLYPH_TOL_DEFAULT
    if "--glyph-tol" in sys.argv:
        tol = float(sys.argv[sys.argv.index("--glyph-tol") + 1])
    pts = GLYPH_PTS_DEFAULT
    if "--glyph-pts" in sys.argv:
        pts = int(sys.argv[sys.argv.index("--glyph-pts") + 1])
    make_banner3d_glyph(tol, pts)
    sys.exit(0)

if BANNER3D:
    # Mutually exclusive with the other modes, same as --windows: writes
    # exactly one file. This is the texture for the monitor screen in the 3D
    # banner scene (banner3d/compose_banner.py) — the mark as the AtticPad
    # app shows it, centered on slate, rendered here because render_mark()
    # is the ONE place the mark may be drawn (see header).
    print("AtticPad: banner3d monitor-screen mark")
    S = 128 * SS
    im = Image.new("RGBA", (S, S), SLATE + (255,))
    render_mark(im, S/2, S*0.60, S*0.60, S*0.29, True,
                S/2, S*0.225, S*0.29, S*0.130, S*0.055)
    im.resize((128, 128), Image.LANCZOS).save("clients/3ds/meta/banner3d/mark.png")
    print("  wrote banner3d/mark.png (128x128)")
    sys.exit(0)

if WINDOWS:
    # Mutually exclusive with the other two modes, same as --android: this
    # writes exactly one file and touches nothing under clients/.
    print("AtticPad: Windows .ico")
    make_windows_ico()
    sys.exit(0)

if not ANDROID:
    # Version comes from core/include/atticpad/version.h — the single source
    # of truth — so the banner cannot drift from what the client prints on
    # screen.
    _vh = io.open("core/include/atticpad/version.h").read()
    VERSION = (re.search(r'APAD_VERSION_STR "([^"]+)" APAD_VERSION_SUFFIX', _vh).group(1)
               + re.search(r'APAD_VERSION_SUFFIX "([^"]*)"', _vh).group(1))

    # ------------------------------------------------------------ icon 48x48
    S = 48 * SS
    icon = ground((S, S))
    if not TRANSPARENT:
        ImageDraw.Draw(icon).rounded_rectangle([0, 0, S-1, S-1], radius=S*0.18,
                                               fill=(38, 45, 58, 255))
    render_mark(icon, S/2, S*0.60, S*0.60, S*0.29, True,
                S/2, S*0.225, S*0.29, S*0.130, S*0.055, depth_scale=0.45)
    icon.resize((48, 48), Image.LANCZOS).save("clients/3ds/meta/icon.png")

    # -------------------------------------------------------- banner 256x128
    W, H = 256*SS, 128*SS
    ban = ground((W, H))
    d = ImageDraw.Draw(ban)
    if not TRANSPARENT:
        for i in range(H):                      # subtle vertical lift
            t = i / H
            d.line([(0, i), (W, i)], fill=(int(28+16*t), int(33+18*t), int(43+21*t), 255))
        d.rounded_rectangle([W*0.012, H*0.025, W-W*0.012, H-H*0.025],
                            radius=H*0.06, outline=(58, 66, 82, 255), width=int(H*0.012))

    render_mark(ban, W*0.205, H*0.60, W*0.225, H*0.225, True,
                W*0.205, H*0.225, W*0.110, H*0.140, W*0.019)

    f_title = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                 int(H*0.235))
    d.text((W*0.40, H*0.32), "AtticPad", font=f_title, fill=CREAM + (255,), anchor="lm")

    # Auto-fit the subtitle against the right margin. Eyeballing it clipped
    # "gamepad" to "gamep" once — invisible at supersampled size, obvious at 256px.
    SUB   = "PC gamepad over Wi-Fi"
    avail = W*0.955 - W*0.40
    size  = int(H*0.098)
    while size > 8:
        f_sub = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
        if d.textlength(SUB, font=f_sub) <= avail:
            break
        size -= max(1, int(size*0.04))
    d.text((W*0.40, H*0.56), SUB, font=f_sub, fill=GREY + (255,), anchor="lm")
    d.line([(W*0.40, H*0.70), (W*0.40+W*0.20, H*0.70)], fill=AMBER + (255,),
           width=int(H*0.022))

    # Version is RIGHT-anchored so it grows leftward and can never overflow,
    # however long the string becomes. Placing it after the title clipped it.
    f_ver = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                               int(H*0.078))
    d.text((W*0.955, H*0.865), "v" + VERSION, font=f_ver, fill=(120, 130, 146, 255),
           anchor="rm")

    ban.resize((256, 128), Image.LANCZOS).save("clients/3ds/meta/banner.png")
    print(f"  wrote icon.png and banner.png  (v{VERSION}, "
          f"{'TRANSPARENT' if TRANSPARENT else 'opaque'}, subtitle {size//SS}px)")

else:
    make_android_assets()
