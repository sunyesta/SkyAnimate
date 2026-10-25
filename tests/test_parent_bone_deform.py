"""Parent Bone's Deform: LAYER moves the layer as a whole (a bone's squash and stretch squashes its lines
and brush stamps too), POINTS moves its points as if each were weighted all to the bone (lines keep
their width). POINTS is the default for drawings. The points land in the same places either way; it
saves with the file, and a file from before POINTS was the default keeps LAYER.

Run with:  OpenAnimate --background --python tests/test_parent_bone_deform.py
"""

import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
bone = rig.bones[0]["name"]
assert oanim.ops.layer.new() == {'FINISHED'}
art = layers.active
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
art.add_curve([(0.2, 0.1), (0.4, 0.3), (0.1, 0.5)])
assert oanim.ops.layer.parent_bone_set(bone=bone) == {'FINISHED'}
assert art.parent_bone == bone and art.parent_bone_deform == "POINTS", "Points by default"
assert oanim.ops.layer.parent_bone_set(deform="LAYER") == {'FINISHED'}
assert art.parent_bone_deform == "LAYER"

# Posed: turned and squashed across, which slants (shears) what it carries.
doc.frame_current = 10
rig.bone_set(bone, angle=0.6, scale=(2.0, 0.7))


def shown():
    points, _, _ = art.evaluated()
    return [art.to_document(p) for p in points]


as_layer = shown()
assert not close(as_layer[0], (0.2, 0.1)), "the bone moved it"
points, _, _ = art.evaluated()
assert close(points[0], (0.2, 0.1)), "LAYER: the drawing stays; the layer moves"

# POINTS: the drawing's points move instead, to the same places.
doc.frame_current = 0
assert oanim.ops.layer.parent_bone_set(deform="POINTS") == {'FINISHED'}
assert art.parent_bone_deform == "POINTS" and art.parent_bone == bone, "the bone stays"
assert oanim.ops.layer.parent_bone_set(deform="SIDEWAYS") == {'CANCELLED'}
doc.frame_current = 10
points, _, _ = art.evaluated()
assert not close(points[0], (0.2, 0.1)), "POINTS: the points themselves move"
assert close(art.to_document((0.0, 0.0)), (0.0, 0.0)), "and the layer stays where it is"
for a, b in zip(shown(), as_layer):
    assert close(a, b), ("the same places either way", a, b)

# Saved with the file.
doc.frame_current = 0
path = str(pathlib.Path(tempfile.mkdtemp()) / "deform.oanim")
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = next(l for l in oanim.data.layers.all() if l.point_count == 3)
assert art.parent_bone_deform == "POINTS"

# Back to LAYER, saved as LAYER too.
layers.active = art
assert oanim.ops.layer.parent_bone_set(deform="LAYER") == {'FINISHED'}
assert art.parent_bone_deform == "LAYER"
assert oanim.ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert oanim.ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
art = next(l for l in oanim.data.layers.all() if l.point_count == 3)
assert art.parent_bone_deform == "LAYER"

# A file from before: a bound layer saved without a Deform was LAYER (then the default) and stays so.
import json
import zipfile
older = str(pathlib.Path(tempfile.mkdtemp()) / "older.oanim")
with zipfile.ZipFile(path) as src, zipfile.ZipFile(older, "w") as dst:
    for item in src.infolist():
        data = src.read(item.filename)
        if item.filename.endswith(".json") and b"parent_bone_deform" in data:
            data = data.replace(b',"parent_bone_deform":"LAYER"', b"").replace(b',"parent_bone_deform":"POINTS"', b"")
            data = data.replace(b'"parent_bone_deform":"LAYER",', b"").replace(b'"parent_bone_deform":"POINTS",', b"")
        dst.writestr(item, data)
assert oanim.ops.wm.open_mainfile(filepath=older, confirmed="1") == {'FINISHED'}
art = next(l for l in oanim.data.layers.all() if l.point_count == 3)
assert art.parent_bone_deform == "LAYER", "an older file keeps Layer"

print("parent_bone_deform: ok")
