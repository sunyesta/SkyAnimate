"""Parent Bone and Custom Weights: with no bone picked a layer's points follow their weights. A new point takes the
average of the points next to it, and one connected to nothing has no weights until it is given some. Clear Weights
takes them off a layer, or off every drawing in a group; a bone picked and taken away again leaves them as they were.
A layer inside a group with a Parent Bone can't pick its own.

Run with:  OpenAnimate --background --python tests/test_parent_bone_weights.py
"""
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="arm", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="leg", head_x="0", head_y="-1", tail_x="1", tail_y="-1", parent="")


def find(layer):
    return next(l for l in layers.all() if l.id == layer.id)


def drawing(name, points, group=None):
    ops.layer.new()
    layer = layers.active
    layer.name = name
    layer.add_curve(points)
    assert ops.layer.move_to_group(group=str((group or rig).id)) == {'FINISHED'}
    layer = find(layer)
    layers.active = layer
    return layer


def settle():
    """New points are weighed before an undo step and a frame change."""
    doc.frame_current = 1
    doc.frame_current = 0


def assign(layer, points, bone):
    layers.active = layer
    ops.mesh.select_none()
    layer.select_points(points)
    rig.select_bone(bone)
    layers.active = layer
    assert ops.weight.assign(weight="1") == {'FINISHED'}
    ops.mesh.select_none()


# --- Connected to nothing: no weights, a new layer's first line included --------------------------------------------
line = drawing("Line", [(0.0, 0.0), (0.5, 0.0), (1.0, 0.0)])
settle()
assert line.weights == [{}, {}, {}], line.weights
# Off bones, a turned bone leaves it where it is.
doc.frame_current = 5
rig.bone_set("arm", angle=1.0)
settle_frame = line.evaluated()[0]
assert abs(settle_frame[2][1]) < 1e-5, settle_frame
rig.bone_set("arm", angle=0.0)
doc.frame_current = 0

# --- Connected: the average of the points next to it ----------------------------------------------------------------
assign(line, [0], "arm")
assign(line, [1], "leg")
assign(line, [2], "leg")
# Between arm and leg: half each.
middle = line.split_segment(0, 0, 0.5)
settle()
w = line.weights[middle]
assert abs(w["arm"] - 0.5) < 1e-4 and abs(w["leg"] - 0.5) < 1e-4, w
# Grown from an end: all of that end's.
line.select_points([2])
grown = line.extrude_selected()[0]
ops.mesh.select_none()
settle()
assert abs(line.weights[grown]["leg"] - 1.0) < 1e-4 and "arm" not in line.weights[grown], line.weights[grown]

# A second line in the same layer, connected to nothing: no weights, whatever the first has.
line.add_curve([(0.0, 2.0), (1.0, 2.0)])
settle()
assert line.weights[-1] == {} and line.weights[-2] == {}, line.weights
# Grown from there, it stays so.
line.select_points([line.point_count - 1])
grown = line.extrude_selected()[0]
ops.mesh.select_none()
settle()
assert line.weights[grown] == {}, line.weights[grown]

# Beside a point with weights and one without: what the first gives.
beside = drawing("Beside", [(0.0, 3.0), (1.0, 3.0)])
settle()
assign(beside, [0], "arm")
middle = beside.split_segment(0, 0, 0.5)
settle()
assert list(beside.weights[middle]) == ["arm"], beside.weights[middle]

# --- Automatic Weights still gives a new drawing its weights ------------------------------------------------------
auto = drawing("Auto", [(0.2, 0.0), (0.8, 0.0)])
settle()
assert not any(auto.weights)
assert ops.weight.automatic() == {'FINISHED'}
assert all(auto.weights), auto.weights

# --- A bone picked and taken away leaves the weights ----------------------------------------------------------------
kept = find(auto).weights
layers.active = auto
assert ops.layer.parent_bone_set(bone="leg") == {'FINISHED'} and auto.parent_bone == "leg"
assert auto.weights == kept
assert ops.layer.parent_bone_set(bone="") == {'FINISHED'} and auto.parent_bone == ""
assert auto.weights == kept, "Custom Weights again: as they were"

# --- Clear Weights --------------------------------------------------------------------------------------------------
layers.active = auto
assert ops.weight.clear() == {'FINISHED'}
assert not any(auto.weights), auto.weights
settle()
assert not any(auto.weights), "and they stay gone"
assert ops.weight.clear() == {'CANCELLED'}, "nothing to clear"
ops.ed.undo()
assert auto.weights == kept, "one undo step"

# A group's: every drawing inside it, however deep; a bone group inside it follows its own bones.
ops.layer.new_group()
outer = layers.active
assert outer.type == "GROUP", outer.type
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
outer = find(outer)
first = drawing("First", [(0.1, 4.0), (0.9, 4.0)], outer)
assert ops.weight.automatic() == {'FINISHED'}
second = drawing("Second", [(0.1, 5.0), (0.9, 5.0)], outer)
assert ops.weight.automatic() == {'FINISHED'}
followed = drawing("Followed", [(0.1, 6.0), (0.9, 6.0)], outer)
assert ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}
assert ops.layer.new_bone() == {'FINISHED'}
sub = layers.active
assert ops.layer.move_to_group(group=str(outer.id)) == {'FINISHED'}
sub = find(sub)
layers.active = sub
ops.armature.bone_add(name="tail", head_x="0", head_y="7", tail_x="1", tail_y="7", parent="")
inner = drawing("Inner", [(0.1, 7.0), (0.9, 7.0)], sub)
assert ops.weight.automatic() == {'FINISHED'}
layers.active = find(outer)
assert ops.weight.clear() == {'FINISHED'}
assert not any(find(first).weights) and not any(find(second).weights)
assert all(find(inner).weights), "its own bones, not this group's"
assert find(followed).parent_bone == "arm", "a Parent Bone stays"

# --- A layer inside a group with a Parent Bone can't pick its own ---------------------------------------------------
layers.active = find(followed)
assert ops.layer.parent_bone_set(bone="leg") == {'FINISHED'} and find(followed).parent_bone == "leg"
layers.active = find(outer)
assert ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}
assert find(first).parent_bone_group == outer.name
layers.active = find(followed)
assert ops.layer.parent_bone_set(bone="arm") == {'CANCELLED'}
assert ops.layer.parent_bone_set(bone="") == {'CANCELLED'}
assert find(followed).parent_bone == "leg", "its own stays for when the group lets go"
# Picked with one that's free, only that one changes.
layers.active = find(outer)
assert ops.layer.parent_bone_set(bone="") == {'FINISHED'}
assert find(first).parent_bone_group == ""
layers.active = find(followed)
assert ops.layer.parent_bone_set(bone="arm") == {'FINISHED'}, "free again"

# --- Saved: no weights stay none, connected to nothing or cleared -----------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_parent_bone_")) / "weights.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
names = {l.name: l for l in oanim.data.layers.all()}
assert not any(names["First"].weights), names["First"].weights
assert all(names["Inner"].weights)
assert names["Line"].weights[1] == {"leg": 1.0}, names["Line"].weights[1]

print("Parent bone weight tests passed")
