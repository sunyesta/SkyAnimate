"""Layer Symmetrize (the Layers editor's right-click Symmetrize + to - and - to +): a layer named for a
side gets its other side made, or remade, mirrored across the document's vertical axis (a bone
layer's, inside one), from whichever of the pair sits on the side it copies from; groups named for a
side go as a whole; masks and Parent Bone move to the other side's layers and bones; layers named
for neither are symmetrized in place.

Run with:  OpenAnimate --background --python tests/test_layer_symmetrize.py
"""

import _oanim
import oanim

doc = oanim.data.document
layers = oanim.data.layers
doc.frame_current = 0


def find(name):
    return next((l for l in layers.all() if l.name == name), None)


def shown(layer):
    points, _, _ = layer.evaluated()
    return [layer.to_document(p) for p in points]


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


def mirrored(a, b, axis=0.0):
    """Layer b shows layer a's points flipped across x = axis."""
    pa, pb = shown(a), shown(b)
    assert len(pa) == len(pb), (len(pa), len(pb))
    for p, q in zip(pa, pb):
        assert close((2 * axis - p[0], p[1]), q), (p, q)
    return True


def new(name, points, closed=True):
    oanim.ops.layer.new()
    l = layers.active
    l.name = name
    l.add_curve(points, closed=closed, fill=closed)
    return l


head = new("Head_X", [(-1.0, 0.0), (1.0, 0.0), (0.0, 1.5)])
eye_r = new("Eye_R", [(-0.6, 0.5), (-0.4, 0.5), (-0.5, 0.7)])
eye_l = new("Eye_L", [(0.6, 0.5), (0.4, 0.5), (0.5, 0.7)])
ear = new("Ear_L", [(1.0, 0.5), (1.4, 0.6), (1.5, 1.2), (1.1, 1.0)])
ear.location_x = 0.3
ear.rotation = 20.0

# Masked with the head (no side) and the left eye: the copy keeps the head and takes the right eye.
layers.active = ear
assert oanim.ops.mask.add(sources="Head_X") == {'FINISHED'}
assert oanim.ops.mask.add(sources="Eye_L", outside="1") == {'FINISHED'}

# Off the setup frame it waits.
doc.frame_current = 5
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'CANCELLED'}
doc.frame_current = 0
assert oanim.ops.layer.symmetrize(direction="SIDEWAYS") == {'CANCELLED'}

# + to -: Ear_L is on the + side, so Ear_R is made from it, just above it.
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}
ear_r = find("Ear_R")
assert ear_r is not None, [l.name for l in layers.all()]
assert mirrored(ear, ear_r)
assert abs(ear_r.rotation + 20.0) < 1e-3 and abs(ear_r.location_x + 0.3) < 1e-4, (ear_r.rotation, ear_r.location_x)
assert [m["layers"] for m in ear_r.masks] == [["Head_X"], ["Eye_R"]], ear_r.masks
assert [m["layers"] for m in ear.masks] == [["Head_X"], ["Eye_L"]], "the source is left as it was"
names = [l.name for l in layers.all()]
assert names.index("Ear_R") == names.index("Ear_L") - 1, names
assert oanim.wm.undo_history()[0][-1] == "Symmetrize + to -"

# - to + with Ear_L picked: Ear_R (on the - side) is the source now, and Ear_L is remade in place,
# keeping its id (so masks naming it still do).
ear_id = ear.id
ear_r.location_x = -0.5
layers.active = ear
assert oanim.ops.layer.symmetrize(direction="NEGATIVE_TO_POSITIVE") == {'FINISHED'}
ear = find("Ear_L")
assert ear.id == ear_id
assert mirrored(ear_r, ear)
assert abs(ear.location_x - 0.5) < 1e-4

# Neither on the side it copies from: nothing happens.
layers.active = eye_r
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}, "Eye_L is on +"
assert mirrored(find("Eye_L"), find("Eye_R"))
layers.active = find("Eye_R")
before = shown(find("Eye_R"))
oanim.ops.layer.new()
lone = layers.active
lone.name = "Cheek_L"
lone.add_curve([(-0.9, 0.2), (-0.7, 0.2), (-0.8, 0.4)])
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'CANCELLED'}, "Cheek_L sits on -"
assert find("Cheek_R") is None

# A group named for a side goes as a whole: its layers flip names where they have a side.
oanim.ops.layer.new_group()
arm = layers.active
arm.name = "Arm_L"
arm.location_x = 2.0
hand = new("Hand_L", [(0.0, 0.0), (0.5, 0.1), (0.4, 0.5)])
assert oanim.ops.layer.move_to_group(group=str(arm.id)) == {'FINISHED'}
sleeve = new("Sleeve", [(-0.2, 0.0), (0.3, -0.2), (0.1, 0.4)])
assert oanim.ops.layer.move_to_group(group=str(arm.id)) == {'FINISHED'}
hand = find("Hand_L")
hand.location_x = 0.25
hand.rotation = 30.0
layers.active = find("Arm_L")
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}
arm_r = find("Arm_R")
assert arm_r is not None
inside = [l.name for l in arm_r.children]
assert "Hand_R" in inside and len(inside) == 2, inside
assert mirrored(find("Hand_L"), find("Hand_R"))
sleeve_copy = next(l for l in arm_r.children if l.name != "Hand_R")
assert mirrored(find("Sleeve"), sleeve_copy)
# Remade: the layers inside keep their ids.
hand_r_id = find("Hand_R").id
layers.active = find("Arm_L")
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}
assert find("Hand_R").id == hand_r_id
assert len(find("Arm_R").children) == 2

# Inside a bone layer: across the bone layer's own axis, and Parent Bone takes the other side's bone.
assert oanim.ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
rig.location_x = 3.0
assert oanim.ops.armature.bone_primitive_add() == {'FINISHED'}
_oanim.layer_bone_set(rig.id, rig.bones[0]["name"], {"name": "brow_L"})
rig.bone_set("brow_L", position=(0.5, 0.2))
rig.select_bone("brow_L")
assert oanim.ops.armature.symmetrize() == {'FINISHED'}
brow = new("Brow_L", [(0.3, 0.5), (0.8, 0.6), (0.6, 0.8)], closed=False)
assert oanim.ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
brow = find("Brow_L")
layers.active = brow
assert oanim.ops.layer.parent_bone_set(bone="brow_L") == {'FINISHED'}
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}
brow_r = find("Brow_R")
assert brow_r.parent_bone == "brow_R", brow_r.parent_bone
assert mirrored(brow, brow_r, axis=3.0)

# Named for neither side: symmetrized in place (a point needs a partner on the other side).
layers.active = head
head_points = shown(head)
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'FINISHED'}
assert len(shown(head)) == len(head_points)
uneven = new("Nose_X", [(0.1, 0.0), (0.3, 0.1), (0.2, 0.3)])
assert oanim.ops.layer.symmetrize(direction="POSITIVE_TO_NEGATIVE") == {'CANCELLED'}, "3 on the right, none left"

# One undo step takes it back.
layers.active = find("Eye_L")
count = len(layers.all())
oanim.ops.layer.delete()
layers.active = find("Eye_R")
assert oanim.ops.layer.symmetrize(direction="NEGATIVE_TO_POSITIVE") == {'FINISHED'}, "Eye_R makes Eye_L again"
assert find("Eye_L") is not None and len(layers.all()) == count
oanim.ops.ed.undo()
assert find("Eye_L") is None

print("layer_symmetrize: ok")
