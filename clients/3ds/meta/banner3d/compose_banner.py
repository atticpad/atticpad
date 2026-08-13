#!/usr/bin/env python3
# compose_banner.py — build the AtticPad 3DS banner scene in Blender (headless).
#
# Composes a static (NO animation) 3D scene: a low-poly handheld console in the
# left foreground -- turned so its back edge points at the PC, "talking over
# its shoulder" -- and a desktop PC (monitor beside a tower) in the right
# background, laid out against the official HOME-menu banner camera.
#
# Usage (Blender 4.x, tested with 4.5.12 LTS portable Linux build):
#   blender -b -noaudio --python compose_banner.py -- \
#       --out-glb  /path/to/banner-scene.glb \
#       --out-preview /path/to/preview.png
#
# Outputs:
#   * banner-scene.glb — feed this to pycgfx (main.py) to get banner.cgfx.
#     Material base colors in the GLB are re-encoded from linear to sRGB
#     fractions after export, because the 3DS displays color values raw
#     (no sRGB decode), matching how pycgfx passes textures through.
#   * preview.png — 400x240 render from the exact banner camera, for approval.
#
# Banner camera (from pycgfx's banner-camera.gltf, the official HOME menu
# camera): perspective, yfov 0.523599 rad, aspect 5:3, glTF position
# [0, 1, 44.786] looking down -Z, znear 26.5.  DO NOT MOVE IT: the HOME menu
# supplies its own camera with these parameters; the scene must be laid out
# against it.  In Blender (Z-up) coords that is location (0, -44.786, 1),
# looking along +Y.

import bpy
from mathutils import Matrix
import math
import json
import struct
import sys
import os

BASE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- parameters

# AtticPad brand palette, sRGB hex.
SLATE = 0x1C212B        # body color
SLATE_LIGHT = 0x39435A  # secondary panels
SLATE_DARK = 0x11141B   # dark details / floor
AMBER = 0xF5B042        # accents: buttons, stripes, power lights
GREY_BLUE = 0x8796AB    # brand greyish blue -- ui_c_dim() in ../../source/ui.c
CREAM = 0xFFE9C4        # screen glow (handheld)
SCREEN_AMBER = 0xFFD98F  # monitor glow, warmer so it reads as a lit display
METAL = 0x6E7A93        # metal details on the PC

# Layout (Blender coords: Z up, camera at y=-44.786 looking +Y).
HH_YAW = math.radians(80)        # turned toward the center
HH_TILT = math.radians(0)       # tipped back so both screens face the camera

# ---- layout selection -------------------------------------------------
# "proven"  = the 2026-08-12 21:48 build that ran on a real console (crops
#             at the HOME menu's elliptical vignette and shows the idle
#             rotation -- known, accepted for now).
# "ellipse" = the ellipse-fitted, depth-flattened recompose. EXONERATED
#             2026-08-12 (late): the freeze it was blamed for was the CWAV
#             alignment bug in pack_bnr.py, since fixed. Still marked
#             ELLIPSE_FATAL so the fit gate stays hard. Original note:
#             FROZE a real console on 2026-08-12 (second crash of the day,
#             after join/billboard were already ruled out by the first) --
#             the only delta from the proven build was these layout
#             numbers. Un-bisected suspects, for whoever picks this up:
#             non-uniform or mirrored node scales, geometry crossing
#             znear=26.5 after the depth flatten (objects moved ~10 units
#             toward the camera), the whole-scene raise of FLOOR_Z.
#             Select with:  -- ... --layout ellipse
LAYOUTS = {
    "proven": dict(
        FLOOR_Z=-6.8,
        HH_WIDTH=13.5, HH_X=-7.2, HH_Y=-2.0,
        PC_X=17.8, PC_Y=13.0,
        TOWER_HEIGHT=11.2, TOWER_OFFSET=(3.8, 0.6),
        MONITOR_HEIGHT=9.1, MONITOR_OFFSET=(-3.4, -0.4),
        TEXT_WIDTH=18.5, TEXT_LOC=(0.0, 8.0, 7.9),
        ELLIPSE_FATAL=False,  # the gate postdates this layout; report only
    ),
    "ellipse": dict(
        FLOOR_Z=-5.6,
        HH_WIDTH=11.5, HH_X=-4.9, HH_Y=0.0,
        PC_X=11.0, PC_Y=2.5,
        TOWER_HEIGHT=8.9, TOWER_OFFSET=(2.2, 0.9),
        MONITOR_HEIGHT=7.5, MONITOR_OFFSET=(-2.8, -0.35),
        TEXT_WIDTH=15.0, TEXT_LOC=(0.0, 0.5, 6.4),
        ELLIPSE_FATAL=True,
    ),
}
_argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
LAYOUT_NAME = (_argv[_argv.index("--layout") + 1]
               if "--layout" in _argv else "proven")
_L = LAYOUTS[LAYOUT_NAME]
PARTICLES = "--particles" in _argv

FLOOR_Z = _L["FLOOR_Z"]
HH_WIDTH = _L["HH_WIDTH"]
HH_LOC = (_L["HH_X"], _L["HH_Y"], FLOOR_Z)
PC_LOC = (_L["PC_X"], _L["PC_Y"], FLOOR_Z)
PC_YAW = math.radians(-14)       # group angled a few degrees toward the handheld
TOWER_HEIGHT = _L["TOWER_HEIGHT"]
TOWER_OFFSET = _L["TOWER_OFFSET"]
TOWER_FACING = math.radians(-90) # model front faces +X; turn it to face -Y
MONITOR_HEIGHT = _L["MONITOR_HEIGHT"]
MONITOR_OFFSET = _L["MONITOR_OFFSET"]
MONITOR_FACING = math.radians(180)  # model front faces +Y; turn it to face -Y

# "atticpad" wordmark, top-center: extruded text in the brand greyish blue
# (GREY_BLUE, = ui_c_dim() in the client UI), facing the camera.
# Font: Russo One (OFL 1.1, see PROVENANCE.md), falling back to Blender's
# bundled Bfont if the letterforms blow the triangle budget at resolution 1.
TEXT_FONT = "RussoOne-Regular.ttf"
TEXT_WIDTH = _L["TEXT_WIDTH"]
TEXT_LOC = _L["TEXT_LOC"]
TEXT_TRI_BUDGET = 600

DISSOLVE_ANGLE_HH = math.radians(4)   # planar decimation, handheld


def hex_lin(h):
    """0xRRGGBB -> linear-light RGBA tuple (Blender colors live in linear)."""
    def chan(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return (chan(h >> 16 & 255), chan(h >> 8 & 255), chan(h & 255), 1.0)


def solid_material(name, hexcolor, emission=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    col = hex_lin(hexcolor)
    bsdf.inputs["Base Color"].default_value = col
    bsdf.inputs["Roughness"].default_value = 0.7
    if emission:
        bsdf.inputs["Emission Color"].default_value = col
        bsdf.inputs["Emission Strength"].default_value = emission
    return m


def textured_material(name, image_path, emission=0.0):
    """Diffuse-textured material (the scene's only texture path; pycgfx
    converts baseColorTexture images to RGBA4 3DS textures)."""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    tex = m.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(image_path)
    tex.extension = "EXTEND"  # clamp-to-edge; pycgfx maps this to the PICA
                              # clamp mode, and mark.png's border is flat
                              # slate, so the letterbox is invisible
    m.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.7
    if emission:
        m.node_tree.links.new(tex.outputs["Color"],
                              bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = emission
    return m


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    return [o for o in bpy.data.objects if o not in before]


def join_meshes(objs, name):
    meshes = [o for o in objs if o.type == "MESH"]
    others = [o for o in objs if o.type != "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    # break parenting to empties while keeping world transforms
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = name
    obj.rotation_mode = "XYZ"  # glTF import sets QUATERNION, which would
                               # silently ignore rotation_euler assignments
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for o in others:
        bpy.data.objects.remove(o)
    return obj


def bbox(obj):
    from mathutils import Vector, Matrix
    pts = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    mins = Vector((min(p[i] for p in pts) for i in range(3)))
    maxs = Vector((max(p[i] for p in pts) for i in range(3)))
    return mins, maxs


def normalize(obj, scale):
    """Uniform scale, then move so the XY center is at origin, min Z at 0."""
    obj.scale = (scale, scale, scale)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    mins, maxs = bbox(obj)
    obj.location = (-(mins.x + maxs.x) / 2, -(mins.y + maxs.y) / 2, -mins.z)
    bpy.ops.object.transform_apply(location=True)


def dissolve(obj, angle):
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.dissolve_limited(angle_limit=angle)
    bpy.ops.mesh.quads_convert_to_tris()
    bpy.ops.object.mode_set(mode="OBJECT")


def tri_count(obj):
    obj.data.calc_loop_triangles()
    return len(obj.data.loop_triangles)


def particle_material():
    m = bpy.data.materials.new("Particle")
    m.use_nodes = True
    m.blend_method = "BLEND"          # -> glTF alphaMode BLEND
    m.use_backface_culling = False    # -> glTF doubleSided
    bsdf = m.node_tree.nodes["Principled BSDF"]
    col = hex_lin(AMBER)
    bsdf.inputs["Base Color"].default_value = (col[0], col[1], col[2],
                                               PARTICLE_ALPHA)
    bsdf.inputs["Alpha"].default_value = PARTICLE_ALPHA
    bsdf.inputs["Roughness"].default_value = 0.7
    return m


def build_particle_proto():
    """One thin extrusion of the EXACT logo gamepad glyph, facing the
    camera (-Y).  The outline is imported from glyph.json -- exported by
    ../make-assets.py --banner3d-glyph from the very pad_solid()/pad_holes()
    calls that raster the mark (the one-place rule; see that file's header
    for the Android drift incident behind it).  This function only lifts
    those polygons into 3D; it draws nothing of the shape itself."""
    path = os.path.join(BASE, "glyph.json")
    assert os.path.exists(path), (
        "glyph.json missing -- run from the repo root: "
        "python3 clients/3ds/meta/make-assets.py --banner3d-glyph")
    with open(path) as f:
        glyph = json.load(f)

    cu = bpy.data.curves.new("ParticleGlyph", type="CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = PARTICLE_DEPTH / 2
    for ring in [glyph["outer"]] + glyph["holes"]:
        sp = cu.splines.new("POLY")
        sp.points.add(len(ring) - 1)
        for pt, (x, y) in zip(sp.points, ring):
            # glyph.json is normalized to x in [-1, 1], y up, aspect true
            pt.co = (x * PARTICLE_HALFW, y * PARTICLE_HALFW, 0.0, 1.0)
        sp.use_cyclic_u = True
    proto = bpy.data.objects.new("ParticleProto", cu)
    bpy.context.collection.objects.link(proto)
    bpy.ops.object.select_all(action="DESELECT")
    proto.select_set(True)
    bpy.context.view_layer.objects.active = proto
    bpy.ops.object.convert(target="MESH")
    proto = bpy.context.view_layer.objects.active
    dissolve(proto, math.radians(1))  # triangulate caps and walls
    proto.rotation_euler = (math.pi / 2, 0, 0)  # face -Y (the camera)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    proto.data.materials.clear()
    proto.data.materials.append(particle_material())
    tris = tri_count(proto)
    assert tris <= PARTICLE_TRI_BUDGET, \
        "particle proto %d tris > %d" % (tris, PARTICLE_TRI_BUDGET)
    print("PARTICLE proto (glyph.json)", tris, "tris")
    return proto


def build_particles():
    """The orbit ring: six instances (each with its OWN uniquely named mesh
    copy, so every CGFX SOBJ maps 1:1 to its node bone) parented under one
    "Orbit" empty, plus the single rigid-TRS orbit animation."""
    proto = build_particle_proto()
    orbit = bpy.data.objects.new("Orbit", None)
    bpy.context.collection.objects.link(orbit)
    orbit.location = ORBIT_CENTER
    orbit.rotation_mode = "XYZ"
    parts = []
    for i in range(PARTICLE_COUNT):
        me = proto.data.copy()
        me.name = "Particle%d" % i
        o = bpy.data.objects.new("Particle%d" % i, me)
        bpy.context.collection.objects.link(o)
        o.parent = orbit
        a = 2 * math.pi * i / PARTICLE_COUNT
        o.location = (ORBIT_RADIUS * math.cos(a),
                      ORBIT_RADIUS * math.sin(a), PARTICLE_HEIGHTS[i])
        o.rotation_mode = "XYZ"
        o.rotation_euler = (0, 0, PARTICLE_YAWS[i])
        sc = PARTICLE_SCALES[i]
        o.scale = (sc, sc, sc)  # uniform only
        parts.append(o)
    bpy.data.objects.remove(proto)

    # one 360-degree LINEAR loop; quarter-turn keys so the exporter's
    # quaternion path is unambiguous (a 0->360 two-key rotation collapses)
    sc = bpy.context.scene
    sc.render.fps = 24
    frames_total = int(ORBIT_PERIOD_S * sc.render.fps)  # 360 @ 15 s
    sc.frame_start = 1
    sc.frame_end = frames_total + 1
    for k in range(5):
        orbit.rotation_euler = (0, 0, k * math.pi / 2)
        orbit.keyframe_insert("rotation_euler",
                              frame=1 + k * frames_total // 4)
    for fc in orbit.animation_data.action.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"
    sc.frame_set(1)
    return orbit, parts


# ------------------------------------------------------------------- scene

def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # ---- handheld (hero, foreground left) --------------------------------
    hh = join_meshes(import_glb(os.path.join(BASE, "handheld-console.glb")),
                     "Handheld")

    # The source model ships one material and a 4-quadrant flat-color
    # texture.  Reassign per-face materials by UV quadrant and drop the
    # texture entirely: image top-left = shell, top-right = screens,
    # bottom-right = buttons/d-pad/bezels.  (Blender UV v axis points up,
    # image origin is top, so image-top = v > 0.5.)
    body = solid_material("HH_Body", SLATE)
    screen = solid_material("HH_Screen", CREAM, emission=1.0)
    accent = solid_material("HH_Accent", AMBER)
    me = hh.data
    me.materials.clear()
    for m in (body, screen, accent):
        me.materials.append(m)
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        u = sum(uv[li].uv[0] for li in poly.loop_indices) / poly.loop_total
        v = sum(uv[li].uv[1] for li in poly.loop_indices) / poly.loop_total
        if v > 0.5:
            poly.material_index = 0 if u < 0.5 else 1
        else:
            poly.material_index = 2 if u >= 0.5 else 0

    dissolve(hh, DISSOLVE_ANGLE_HH)
    mins, maxs = bbox(hh)
    normalize(hh, HH_WIDTH / (maxs.x - mins.x))
    hh.rotation_euler = (HH_TILT, 0, HH_YAW)
    hh.location = HH_LOC

    # ---- PC group: monitor + tower (background right) ---------------------
    def place(obj, offset, facing):
        """Position obj inside the PC group: rotate the in-group offset by
        the group yaw, and compose the object's own facing with it."""
        c, sn = math.cos(PC_YAW), math.sin(PC_YAW)
        x, y = offset
        obj.location = (PC_LOC[0] + x * c - y * sn,
                        PC_LOC[1] + x * sn + y * c, PC_LOC[2])
        obj.rotation_euler = (0, 0, facing + PC_YAW)

    tower = join_meshes(import_glb(os.path.join(BASE, "tower.glb")), "Tower")
    # source palette: mat23 black case, mat15/mat24 light trim (mat24 is
    # translucent in the source -- forced opaque, no alpha blending in the
    # banner), mat16/mat17 slots and details, mat8/mat2/mat10 red/purple/
    # green LEDs -> all LEDs become amber accents.
    tower_recolor = {
        "mat23": SLATE, "mat15": SLATE_LIGHT, "mat24": SLATE_LIGHT,
        "mat16": METAL, "mat17": SLATE_DARK,
        "mat8": AMBER, "mat2": AMBER, "mat10": AMBER,
    }
    for slot in tower.material_slots:
        base = slot.material.name.split(".")[0]
        slot.material = solid_material("PC_" + base,
                                       tower_recolor.get(base, SLATE))
    mins, maxs = bbox(tower)
    normalize(tower, TOWER_HEIGHT / (maxs.z - mins.z))
    place(tower, TOWER_OFFSET, TOWER_FACING)

    mon = join_meshes(import_glb(os.path.join(BASE, "monitor.glb")), "Monitor")
    # source palette: metalDark casing, metal = the display face.  The
    # display shows the AtticPad app: mark.png, which may only be produced
    # by ../make-assets.py --banner3d-mark (single-source-of-truth rule).
    screen_slot = None
    for i, slot in enumerate(mon.material_slots):
        base = slot.material.name.split(".")[0]
        if base == "metal":
            slot.material = textured_material(
                "Mon_screen", os.path.join(BASE, "mark.png"), emission=0.4)
            screen_slot = i
        else:
            slot.material = solid_material("Mon_" + base, SLATE)
    # planar-unwrap the display faces (the source model has no UVs):
    # u across width (x), v up the height (z), so the mark sits upright.
    # The square mark is letterboxed on the wide screen (aspect-preserving
    # u, clamped edges) rather than stretched.
    me = mon.data
    if not me.uv_layers:
        me.uv_layers.new(name="UVMap")
    uv = me.uv_layers.active.data
    pts = [v.co for v in me.vertices]
    screen_polys = [p for p in me.polygons if p.material_index == screen_slot]
    xs = [pts[v].x for p in screen_polys for v in p.vertices]
    zs = [pts[v].z for p in screen_polys for v in p.vertices]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    aspect = (x1 - x0) / (z1 - z0)
    for p in screen_polys:
        for li in p.loop_indices:
            co = pts[me.loops[li].vertex_index]
            uv[li].uv = (0.5 + ((co.x - x0) / (x1 - x0) - 0.5) * aspect,
                         (co.z - z0) / (z1 - z0))
    mins, maxs = bbox(mon)
    normalize(mon, MONITOR_HEIGHT / (maxs.z - mins.z))
    place(mon, MONITOR_OFFSET, MONITOR_FACING)

    # ---- "atticpad" wordmark ---------------------------------------------
    # Extruded text, converted to a mesh.  Curve resolution is walked down
    # until the wordmark fits its triangle budget; chunky letters are fine.
    font_path = os.path.join(BASE, TEXT_FONT)
    font = bpy.data.fonts.load(font_path) if os.path.exists(font_path) else None
    txt = None
    for use_font, res in ((font, 3), (font, 2), (font, 1),
                          (None, 3), (None, 2), (None, 1)):
        if use_font is None and font is not None and res == 3:
            print("WORDMARK: %s over budget even at resolution 1;"
                  " falling back to Bfont" % TEXT_FONT)
        bpy.ops.object.text_add()
        txt = bpy.context.active_object
        txt.name = "Wordmark"
        txt.data.body = "atticpad"
        if use_font is not None:
            txt.data.font = use_font
        txt.data.align_x = "CENTER"
        txt.data.resolution_u = res
        txt.data.extrude = 0.06
        txt.data.fill_mode = "FRONT"  # back face is never seen
        bpy.ops.object.convert(target="MESH")
        txt = bpy.context.active_object
        dissolve(txt, math.radians(2))  # clean triangulation of the fill
        if tri_count(txt) <= TEXT_TRI_BUDGET:
            print("WORDMARK: %s at resolution %d, %d tris"
                  % ("Bfont" if use_font is None else TEXT_FONT, res,
                     tri_count(txt)))
            break
        bpy.data.objects.remove(txt)
        txt = None
    assert txt is not None, "wordmark never fit its triangle budget"
    txt.data.materials.append(solid_material("Wordmark", GREY_BLUE, emission=0.3))
    mins, maxs = bbox(txt)
    normalize(txt, TEXT_WIDTH / (maxs.x - mins.x))
    txt.rotation_euler = (math.pi / 2, 0, 0)  # face the camera (-Y)
    txt.location = TEXT_LOC

    # ---- banner camera (official values — do not move) --------------------
    cam_data = bpy.data.cameras.new("BannerCamera")
    cam_data.type = "PERSP"
    cam_data.sensor_fit = "VERTICAL"
    cam_data.angle_y = 0.523599
    cam_data.clip_start = 26.5
    cam_data.clip_end = 1000.0
    cam = bpy.data.objects.new("BannerCamera", cam_data)
    bpy.context.collection.objects.link(cam)
    cam.location = (0, -44.786, 1)          # glTF [0, 1, 44.786]
    cam.rotation_euler = (math.pi / 2, 0, 0)  # look along +Y, Z up
    bpy.context.scene.camera = cam

    # ---- preview lighting (preview render only; not exported) -------------
    key_data = bpy.data.lights.new("Key", type="SUN")
    key_data.energy = 3.0
    key_data.use_shadow = False  # the HOME menu casts no shadows; keep the
                                 # preview honest (and the wordmark's shadow
                                 # off the monitor screen)
    key = bpy.data.objects.new("Key", key_data)
    bpy.context.collection.objects.link(key)
    key.rotation_euler = (math.radians(55), 0, math.radians(-35))
    fill_data = bpy.data.lights.new("Fill", type="SUN")
    fill_data.energy = 0.8
    fill_data.use_shadow = False
    fill = bpy.data.objects.new("Fill", fill_data)
    bpy.context.collection.objects.link(fill)
    fill.rotation_euler = (math.radians(60), 0, math.radians(140))

    return hh, tower, mon, txt


def render_preview(path):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE_NEXT"
    sc.render.resolution_x = 400
    sc.render.resolution_y = 240
    sc.render.film_transparent = True
    sc.view_settings.view_transform = "Standard"
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)


# Orbiting particle ring (--particles): six small semi-transparent copies
# of the logo's gamepad glyph (glyph.json, exported by ../make-assets.py
# --banner3d-glyph) slowly circling the scene.  ANIMATION RULE: rigid node TRS
# only -- one glTF animation rotating the "Orbit" node 360 degrees, LINEAR
# keys, looping.  NO skins, no JOINTS_*/WEIGHTS_*, no morphs: the
# researched HOME-menu freeze class is SOFT-SKINNED animation, which this
# categorically excludes (convert_cgfx.py asserts it).
PARTICLE_COUNT = 6
ORBIT_CENTER = (1.0, 2.5, 0.0)   # vertical axis through the scene center
ORBIT_RADIUS = 12.5
ORBIT_PERIOD_S = 15.0
PARTICLE_SCALES = (0.9, 0.6, 0.75, 0.5, 0.85, 0.65)   # varied, uniform-only
PARTICLE_HEIGHTS = (2.0, -2.5, 4.0, 0.0, -3.0, 3.0)   # relative to center
# static per-particle yaw: a rigid ring rotates every child with it, so
# identical facings mean whole phases where all six are edge-on slivers;
# varied yaws keep a few readable at any phase (deviation from the flat
# "all face camera" design, deliberate)
PARTICLE_YAWS = (0.0, 0.62, -0.45, 0.28, -0.80, 1.05)
PARTICLE_ALPHA = 0.35
PARTICLE_DEPTH = 0.22    # extrusion thickness (matches the old procedural proto)
PARTICLE_HALFW = 1.5     # glyph.json x in [-1,1] -> a ~3-unit-wide silhouette,
                         # the same footprint the old procedural proto had
PARTICLE_TRI_BUDGET = 200

# The HOME menu masks the banner with an elliptical vignette.  Everything
# must fit inside the ellipse inscribed in the frame, shrunk by this margin;
# the check below is mandatory and fails the build if any pixel escapes.
ELLIPSE_MARGIN = 0.07

HOME_GREY = 0xEC / 255.0  # 3DS HOME menu default light-grey backdrop


def composite_over_home(src_path, dst_path):
    """preview-home.png: the transparent preview composited over the HOME
    menu's default light grey, so viewers that paint alpha as white don't
    mislead anyone.  Composited in raw sRGB space (browser-style over)."""
    import numpy as np
    img = bpy.data.images.load(src_path)
    img.colorspace_settings.name = "Non-Color"  # keep raw sRGB bytes
    px = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(-1, 4)
    a = px[:, 3:4]
    px[:, :3] = px[:, :3] * a + HOME_GREY * (1.0 - a)
    px[:, 3] = 1.0
    out = bpy.data.images.new("home", img.size[0], img.size[1], alpha=False)
    out.colorspace_settings.name = "Non-Color"
    out.pixels.foreach_set(px.reshape(-1))
    out.filepath_raw = dst_path
    out.file_format = "PNG"
    out.save()


def ellipse_check(src_path, dst_path):
    """Require zero opaque pixels outside the inscribed ellipse (minus
    ELLIPSE_MARGIN).  Writes a debug image: the render over HOME grey with
    the ellipse outline drawn faintly and any escaped pixels flagged red."""
    import numpy as np
    img = bpy.data.images.load(src_path)
    img.colorspace_settings.name = "Non-Color"
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    px = px.reshape(h, w, 4)
    ax = (w / 2) * (1 - ELLIPSE_MARGIN)
    bx = (h / 2) * (1 - ELLIPSE_MARGIN)
    xs = (np.arange(w) - (w - 1) / 2) / ax
    ys = (np.arange(h) - (h - 1) / 2) / bx
    d = np.sqrt(ys[:, None] ** 2 + xs[None, :] ** 2)
    opaque = px[:, :, 3] > (8 / 255)
    outside = opaque & (d > 1.0)
    n_out = int(outside.sum())

    a = px[:, :, 3:4]
    rgb = px[:, :, :3] * a + HOME_GREY * (1.0 - a)
    ring = np.abs(d - 1.0) < 0.006
    rgb[ring] = rgb[ring] * 0.55 + np.array([0.2, 0.35, 0.9]) * 0.45
    rgb[outside] = [1.0, 0.1, 0.1]
    out_px = np.concatenate([rgb, np.ones_like(a)], axis=2)
    out = bpy.data.images.new("ellipse", w, h, alpha=False)
    out.colorspace_settings.name = "Non-Color"
    out.pixels.foreach_set(out_px.reshape(-1))
    out.filepath_raw = dst_path
    out.file_format = "PNG"
    out.save()
    print("ELLIPSE opaque-outside-count", n_out,
          "(layout %s)" % LAYOUT_NAME)
    if _L["ELLIPSE_FATAL"]:
        assert n_out == 0, ("%d opaque pixels outside the HOME-menu ellipse "
                            "(see %s)" % (n_out, dst_path))
    elif n_out:
        print("ELLIPSE: crop at the vignette is KNOWN for the proven "
              "layout and accepted for now")


def export_glb(path):
    bpy.ops.export_scene.gltf(
        filepath=path,
        export_format="GLB",
        export_cameras=False,     # pycgfx ignores cameras; HOME menu has its own
        export_animations=PARTICLES,  # ONLY the rigid-TRS orbit loop; the
                                      # freeze class is soft-skinned
                                      # animation, never exported here
        export_force_sampling=False,  # keep the five authored quarter keys
        export_skins=False,
        export_apply=True,
        export_yup=True,
    )


def srgb_encode_colors(path):
    """Re-encode baseColorFactor linear -> sRGB fractions in the GLB.

    The 3DS renders color values raw (no sRGB decode stage), and pycgfx
    passes glTF material colors straight through to the CGFX material
    diffuse.  Encoding to sRGB fractions makes the on-console colors match
    the authored hex palette (best effort; unverifiable without hardware).
    """
    def enc(v):
        v = max(0.0, min(1.0, v))
        return v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055

    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"glTF"
    jlen = struct.unpack("<I", data[12:16])[0]
    assert data[16:20] == b"JSON"
    j = json.loads(data[20:20 + jlen])
    rest = data[20 + jlen:]
    for mat in j.get("materials", []):
        pbr = mat.get("pbrMetallicRoughness")
        if pbr and "baseColorFactor" in pbr:
            r, g, b, a = pbr["baseColorFactor"]
            pbr["baseColorFactor"] = [enc(r), enc(g), enc(b), a]
    jb = json.dumps(j, separators=(",", ":")).encode()
    jb += b" " * (-len(jb) % 4)
    out = (b"glTF" + struct.pack("<II", 2, 12 + 8 + len(jb) + len(rest))
           + struct.pack("<I", len(jb)) + b"JSON" + jb + rest)
    with open(path, "wb") as f:
        f.write(out)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out_glb = os.path.join(BASE, "banner-scene.glb")
    out_preview = os.path.join(BASE, "preview.png")
    for i, a in enumerate(argv):
        if a == "--out-glb":
            out_glb = argv[i + 1]
        elif a == "--out-preview":
            out_preview = argv[i + 1]

    hh, tower, mon, txt = build()
    orbit = None
    parts = []
    if PARTICLES:
        orbit, parts = build_particles()
    stats = {o.name: tri_count(o) for o in (hh, tower, mon, txt, *parts)}
    print("TRIS", json.dumps(stats), "total", sum(stats.values()))
    # depth (scene y) spread: the HOME menu idles the banner through a slow
    # rotation; the flatter the scene, the less the rotation shows.
    spans = {o.name: bbox(o) for o in (hh, tower, mon, txt)}
    y0 = min(mn.y for mn, mx in spans.values())
    y1 = max(mx.y for mn, mx in spans.values())
    for name, (mn, mx) in spans.items():
        print("DEPTH %-9s y %6.2f .. %6.2f" % (name, mn.y, mx.y))
    print("DEPTH total spread %.2f" % (y1 - y0))

    render_preview(out_preview)
    ellipse_check(out_preview,
                  os.path.join(os.path.dirname(out_preview),
                               "preview-ellipse.png"))
    composite_over_home(out_preview,
                        os.path.join(os.path.dirname(out_preview),
                                     "preview-home.png"))
    if PARTICLES:
        # the ring must clear the vignette at every phase, not just the
        # authored one: re-render at orbit phases 45 and 90 degrees (via
        # the real animation frames) and run the same gate on each
        frames_total = int(ORBIT_PERIOD_S * bpy.context.scene.render.fps)
        for deg in (45, 90):
            bpy.context.scene.frame_set(1 + deg * frames_total // 360)
            phase_png = os.path.join(os.path.dirname(out_preview),
                                     "preview-phase%d.png" % deg)
            render_preview(phase_png)
            print("PHASE", deg)
            ellipse_check(phase_png,
                          os.path.join(os.path.dirname(out_preview),
                                       "preview-ellipse-p%d.png" % deg))
        bpy.context.scene.frame_set(1)
    # HISTORY (2026-08-12): two hardware freezes were blamed on the joined
    # export and then on the recomposed layout.  Forensics later reconstructed
    # the console's exception dump to the byte: the real cause was pack_bnr.py
    # leaving the CWAV at an LZ11-length-dependent (so effectively random)
    # file offset -- a 3-in-4 chance of a misaligned audio chunk, which
    # data-aborts the HOME menu's CWAV parser.  The packer now pads (see
    # pack_bnr.py); geometry, layouts, the join and the billboard flag are all
    # exonerated.  --join produces the single-identity-node export the
    # convert_cgfx.py --billboard (YAxial hold-still) path needs.
    if "--join" in argv:
        for o in (hh, tower, mon, txt):
            if not o.data.uv_layers:
                # join keeps UV layers by name; meshes without one would
                # otherwise strip the monitor's mark UVs when active
                o.data.uv_layers.new(name=mon.data.uv_layers[0].name)
        banner = join_meshes([hh, tower, mon, txt], "Banner")
        bpy.context.view_layer.objects.active = banner
        banner.select_set(True)
        bpy.ops.object.transform_apply(location=True, rotation=True,
                                       scale=True)
        assert banner.matrix_world == Matrix.Identity(4), \
            "Banner node transform must be identity (billboard requirement)"
        print("JOINED into identity 'Banner' node,",
              tri_count(banner), "tris")
    export_glb(out_glb)
    srgb_encode_colors(out_glb)
    print("WROTE", out_glb, "and", out_preview)


main()
