"""A bone drag draws what is worked out from the pose as it is on that update: a pupil Look At holds inside its Eye
Shape stays inside the eye as the head carrying it is dragged, where it lands when the drag ends (it was held to the
eye as it was when the drag began, and then an update behind, so it snapped on letting go).

Run with:  OpenAnimate --background --python tests/test_bone_drag_followers.py
"""

import math

import _oanim
import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="2", parent="")
ops.armature.bone_add(name="eye", head_x="0.3", head_y="1", tail_x="0.3", tail_y="1.05", parent="head")
ops.armature.bone_add(name="look", head_x="0", head_y="1", tail_x="0", tail_y="1.1", parent="")


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def ellipse(cx, cy, rx, ry, n=16):
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def drawing(points, bone_name):
    ops.layer.new()
    l = layers.active
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    l = next(x for x in layers.all() if x.id == l.id)
    l.add_curve(points, closed=True, fill=True)
    layers.active = l
    assert ops.layer.parent_bone_set(bone=bone_name) == {'FINISHED'}
    return next(x for x in layers.all() if x.id == l.id)


white = drawing(ellipse(0.3, 1.0, 0.2, 0.1), "head")
drawing(ellipse(0.3, 1.0, 0.03, 0.03), "eye")
layers.active = rig
rig.select_bone("eye")
assert ops.armature.constraint_add(type="LOOK_AT", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, target="look", shape_layer=white.name)

# Looking far right: the pupil held at the eye's right edge.
doc.frame_current = 10
rig.bone_set("look", position=(5.0, 0.0))
doc.frame_current = 11
doc.frame_current = 10
held = bone("eye")["head"]
assert held[0] > 0.4, ("at the eye's edge", held)

for drag in ([(0.05 * i, 0.0) for i in range(1, 6)], [(0.0, -0.04 * i) for i in range(1, 6)],
             [(-0.03 * i, 0.03 * i) for i in range(1, 6)]):
    ops.armature.select_none()
    rig.select_bone("head")
    drawn = _oanim.debug_move_bones(rig.id, drag, "eye")
    lands = bone("eye")["head"]
    assert math.dist(drawn[-1], lands) < 1e-4, ("drawn mid-drag where it lands", drag[-1], drawn[-1], lands)
    # Each update as it lands then: the head (and the eye on it) moved by the drag, the pupil in it.
    moved = (lands[0] - held[0], lands[1] - held[1])
    assert math.dist(moved, drag[-1]) < 0.02, ("the pupil goes with the head", moved, drag[-1])
    assert ops.ed.undo() == {'FINISHED'}
    doc.frame_current = 11
    doc.frame_current = 10
    assert math.dist(bone("eye")["head"], held) < 1e-5

print("test_bone_drag_followers: all passed")
