#!/usr/bin/env python3
"""convert_cgfx.py -- glTF -> CGFX for the AtticPad 3DS banner.

Modes (composable):
  (default)     plain pycgfx conversion + structural assertions
  --billboard   YAxial (5) hold-still on the joined identity "Banner" node,
                after github.com/Epic0522/ClouDS-Music-FA (read-only
                reference, never vendored).  HARDWARE-PROVEN 2026-08-12
                (late): runs on a real console.  The freezes originally
                blamed on this flag (and on the mesh join, and on the
                recomposed layout) were forensically traced to pack_bnr.py
                leaving the CWAV chunk at an unaligned offset -- see the
                packer's pad-to-16 fix; geometry, join, billboard and both
                layouts are all exonerated.
  --particles   permits EXACTLY ONE skeletal animation: the rigid-TRS
                orbit loop ("Orbit" node, LINEAR keys, looping CANM a la
                Nintendo's AR Games / the ClouDS reference).  The
                researched HOME-menu freeze class is SOFT-SKINNED
                animation; skins, skinning modes and bone-index/weight
                streams are hard-asserted absent in every mode.  Also
                verifies the particle material's alpha-blend flags landed
                (SrcAlpha/InvSrcAlpha) and that no animated bone is
                billboarded.

Usage:
    python3 convert_cgfx.py /path/to/pycgfx in.glb out.cgfx \
        [--billboard] [--particles]
"""

import sys
import os
import struct

BILLBOARD_ROOT_NODE = "Banner"   # joined identity node (--billboard)
ORBIT_NODE = "Orbit"             # animated ring root (--particles)
PARTICLE_MATERIAL = "Particle"


def main():
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    billboard = "--billboard" in flags
    particles = "--particles" in flags
    pycgfx_root, in_glb, out_cgfx = args[:3]
    sys.path.insert(0, os.path.abspath(pycgfx_root))
    import gltflib
    import main as pycgfx
    from cgfx.sobj import BillboardMode, BoneFlag
    from cgfx.primitives import VertexAttributeUsage
    from cgfx.mtob import BlendFunction
    from cgfx.canm import CANMBoneTransform

    gltf = gltflib.GLTF.load(os.path.abspath(in_glb), load_file_resources=True)

    # ---- freeze-class checks, input side ---------------------------------
    assert not gltf.model.skins, "glTF contains skins"
    anims = gltf.model.animations or []
    if not particles:
        assert not anims, "glTF contains animations without --particles"
    else:
        assert len(anims) == 1, "expected exactly one animation"
        orbit_id = next(i for i, n in enumerate(gltf.model.nodes)
                        if n.name == ORBIT_NODE)
        for ch in anims[0].channels:
            assert ch.target.node == orbit_id, \
                "animation targets %r, only %r is allowed" % (
                    gltf.model.nodes[ch.target.node].name, ORBIT_NODE)
            assert ch.target.path in ("rotation", "translation", "scale"), \
                "non-TRS channel %r" % ch.target.path

    cgfx = pycgfx.convert_gltf(gltf)

    assert len(list(cgfx.data.models)) == 1
    model = cgfx.data.models[next(iter(cgfx.data.models))]
    skeleton = model.skeleton
    bone_names = list(skeleton.bones)

    # ---- freeze-class checks, output side (every mode) -------------------
    for shape in model.shapes.data.contents:
        for ps in shape.primitive_sets.data.contents:
            assert ps.skinning_mode == 0, \
                "%s uses skinning mode %d" % (shape.name, ps.skinning_mode)
        for attr in shape.vertex_attributes.data.contents:
            usages = (attr.vertex_streams.data.contents
                      if hasattr(attr, "vertex_streams") else (attr,))
            for u in usages:
                assert u.usage not in (VertexAttributeUsage.BoneIndex,
                                       VertexAttributeUsage.BoneWeight), \
                    "%s contains skin attributes" % shape.name

    # ---- animation: absent, or exactly the rigid orbit loop --------------
    canm_names = list(cgfx.data.skeletal_animations)
    if not particles:
        assert not canm_names, "CGFX has skeletal animations"
    else:
        assert len(canm_names) == 1, "expected one CANM, got %r" % canm_names
        canm = cgfx.data.skeletal_animations[canm_names[0]]
        members = list(canm.member_animations_data)
        assert members == [ORBIT_NODE], \
            "CANM members %r, expected only %r" % (members, ORBIT_NODE)
        for mname in members:
            member = canm.member_animations_data[mname]
            assert isinstance(member, CANMBoneTransform), \
                "%s is not a rigid Transform member" % mname
        assert canm.looping, "orbit CANM must loop"
        print("CANM %r verified: members %s, rigid Transform, looping, "
              "%.0f frames @60fps" % (canm_names[0], members,
                                      canm.frame_size))
        # blend flags on the particle material
        mat = model.materials[PARTICLE_MATERIAL]
        assert mat is not None, "no %r material" % PARTICLE_MATERIAL
        bo = mat.fragment_operations.blend_operation
        assert bo.src_color == BlendFunction.SrcAlpha \
            and bo.dst_color == BlendFunction.InvSrcAlpha, \
            "particle material blend flags did not land"
        print("particle material alpha-blend flags verified "
              "(SrcAlpha/InvSrcAlpha), diffuse alpha %.2f"
              % mat.material_color.diffuse.a)
        # billboarded bones must not be animated / animated must not billboard
        assert ORBIT_NODE in bone_names

    if billboard:
        assert BILLBOARD_ROOT_NODE in bone_names, \
            "no %r node; run compose_banner.py with --join" \
            % BILLBOARD_ROOT_NODE
        banner = skeleton.bones[BILLBOARD_ROOT_NODE]
        assert banner.flags & BoneFlag.IsIdentity, \
            "Banner bone transform is not identity (double-rotation hazard)"
        # bind each SOBJ mesh to its own node bone (runtime lookup field;
        # the reference fills it for every rigid mesh).  Resolved through
        # the shape's related-bones index rather than by name: the joined
        # "Banner" object keeps its original mesh-data name, so SOBJ names
        # do not reliably carry the node.
        bone_by_index = {skeleton.bones.get_index(b): b
                         for b in skeleton.bones}
        for mesh in model.meshes.data.contents:
            shape = model.shapes.data.contents[mesh.shape_index]
            related = shape.primitive_sets.data.contents[0] \
                .related_bones.data.contents
            assert len(related) == 1, \
                "SOBJ %r relates to %d bones" % (mesh.name, len(related))
            mesh.mesh_node_name = bone_by_index[related[0]]
        before = pycgfx.write(cgfx)
        banner.billboard_mode = BillboardMode.YAxial
        out = pycgfx.write(cgfx)
        diffs = [o for o in range(0, len(before), 4)
                 if before[o:o+4] != out[o:o+4]]
        assert len(diffs) == 1 and \
            struct.unpack("<i", out[diffs[0]:diffs[0]+4])[0] == 5, \
            "expected exactly the one billboard word to change"
        print("billboard word verified at 0x%X: 0 -> 5 (YAxial on %r)"
              % (diffs[0], BILLBOARD_ROOT_NODE))
    else:
        assert BILLBOARD_ROOT_NODE not in bone_names, \
            "joined scene without --billboard"
        out = pycgfx.write(cgfx)

    for b in bone_names:
        if b != (BILLBOARD_ROOT_NODE if billboard else None):
            assert skeleton.bones[b].billboard_mode == BillboardMode.Off, \
                "bone %r unexpectedly billboarded" % b

    print("bones:", bone_names)
    with open(out_cgfx, "wb") as f:
        f.write(out)
    print("wrote %s (%d bytes)" % (out_cgfx, len(out)))


if __name__ == "__main__":
    main()
