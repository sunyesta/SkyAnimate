"""A layer moved into another bone layer is bound to the bones of the same names there (Parent Bone and its points'
weights), or to nothing where there's none: bone ids are each bone layer's own, and kept they were read as whichever
bone there had the same number. Moved out of every bone layer, it's bound to nothing.

Run with:  OpenAnimate --background --python tests/test_layer_move_rebind.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0


def rig_with(names):
    assert ops.layer.new_bone() == {'FINISHED'}
    rig = layers.active
    for i, name in enumerate(names):
        assert ops.armature.bone_add(name=name, head_x=str(i), head_y="0", tail_x=str(i), tail_y="1", parent="") == {'FINISHED'}
    return rig


def fresh(layer):
    return next(l for l in layers.all() if l.id == layer.id)


# Same names, other numbers (each bone layer counts its own from 1): "jaw" is 2 in the face, 1 in the mouth, whose 2 is
# "teeth".
face = rig_with(["head", "jaw"])
mouth = rig_with(["jaw", "teeth"])

ops.layer.new()
chin = layers.active
assert ops.layer.move_to_group(group=str(face.id)) == {'FINISHED'}
chin = fresh(chin)
chin.add_curve([(5.0, 5.0), (5.5, 5.0)])
ops.mesh.select_all()
layers.active = face
ops.armature.select_none()
face.select_bone("jaw")
layers.active = chin
assert ops.weight.assign(weight="1") == {'FINISHED'}
ops.layer.new()
nose = layers.active
assert ops.layer.move_to_group(group=str(face.id)) == {'FINISHED'}
nose = fresh(nose)
nose.add_curve([(-5.0, 5.0), (-5.5, 5.0)])
layers.active = nose
assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}


def moves(layer, rig, name):
    """How far the layer's first point goes when bone `name` of `rig` moves by 0.5 on frame 5."""
    layer = fresh(layer)
    doc.frame_current = 5
    was = layer.shown()[0][0]
    at = next(b for b in rig.bones if b["name"] == name)["position"]
    rig.bone_set(name, position=(at[0] + 0.5, at[1]))
    doc.frame_current = 5
    now = fresh(layer).shown()[0][0]
    rig.bone_set(name, position=at)
    doc.frame_current = 0
    return math.dist(was, now)


assert moves(chin, face, "jaw") > 0.4 and moves(nose, face, "head") > 0.4

# Into the mouth: the chin goes with its jaw, not with its teeth (the face's jaw's number).
layers.active = fresh(chin)
assert ops.layer.move_to_group(group=str(mouth.id)) == {'FINISHED'}
assert moves(chin, mouth, "jaw") > 0.4, "bound to the jaw of the same name"
assert moves(chin, mouth, "teeth") < 1e-4, "not the bone with its old number"
# The nose's head has no namesake there: it follows nothing.
layers.active = fresh(nose)
assert ops.layer.move_to_group(group=str(mouth.id)) == {'FINISHED'}
assert fresh(nose).parent_bone == "", fresh(nose).parent_bone
assert moves(nose, mouth, "jaw") < 1e-4

# Out of every bone layer: bound to nothing.
layers.active = fresh(chin)
assert ops.layer.move_to_group(group="0") == {'FINISHED'}
assert all(not w for w in fresh(chin).weights), fresh(chin).weights
print("test_layer_move_rebind: all passed")
