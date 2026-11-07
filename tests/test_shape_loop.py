"""Look At and Limit to Shape refuse a shape their own bone moves, as Follow Point refuses such a deform: the bone would
chase itself, the pose depending on the frames shown before (and evaluating could stall). Refused, it says why (a
"Loop" in shape_error, shown in the Bone Constraints tab) and does nothing; a shape moved only by bones it hangs on is
fine, and letting go of the loop brings it back.

Run with:  OpenAnimate --background --python tests/test_shape_loop.py
"""

import math

import oanim

doc, layers, ops = oanim.data.document, oanim.data.layers, oanim.ops
ops.wm.read_homefile()
doc.frame_current = 0
doc.auto_key = True

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="2", parent="")
ops.armature.bone_add(name="eye", head_x="0.3", head_y="1", tail_x="0.3", tail_y="1.05", parent="head")
ops.armature.bone_add(name="look", head_x="0", head_y="1", tail_x="0", tail_y="1.1", parent="")


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def drawing(points):
    ops.layer.new()
    l = layers.active
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    l = next(x for x in layers.all() if x.id == l.id)
    l.add_curve(points, closed=True, fill=True)
    return l


def ellipse(cx, cy, rx, ry, n=16):
    return [(cx + rx * math.cos(2 * math.pi * k / n), cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


white = drawing(ellipse(0.3, 1.0, 0.2, 0.1))
pupil = drawing(ellipse(0.3, 1.0, 0.03, 0.03))
layers.active = pupil
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
layers.active = rig
rig.select_bone("eye")
assert ops.armature.constraint_add(type="LOOK_AT", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, target="look", shape_layer=white.name, fit="WHOLE")


def look():
    return rig.bone_constraints("eye")[0]


def go(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


# The look goes right on 10, up on 20.
for frame, at in ((10, (0.4, 1.0)), (20, (0.0, 1.4))):
    doc.frame_current = frame
    rig.bone_set("look", position=at)
    go(frame)

# --- The white on the head: fine ------------------------------------------------------------------------------------
go(0)
layers.active = white
assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}
go(10)
assert look()["shape_error"] == "" and look()["look_reach"][0] > 0.1, look()
assert bone("eye")["head"][0] > 0.31, ("it looks right", bone("eye")["head"])

# --- The white on the eye bone itself (Parent Bone): refused ---------------------------------------------------------
go(0)
layers.active = white
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
go(10)
assert look()["shape_error"].startswith("Loop"), look()
assert look()["look_reach"] == (0.0, 0.0), "refused, it does nothing"
assert math.dist(bone("eye")["head"], (0.3, 1.0)) < 1e-5, bone("eye")["head"]

# --- Weighted to the eye bone (Weight Assign), the way the eye file's warps were: refused, and no frame depends on the
# one before ------------------------------------------------------------------------------------------------------------
go(0)
layers.active = white
assert ops.layer.parent_bone_set(bone="") == {'FINISHED'}
rig.select_bone("eye")
layers.active = white
white.select_points(list(range(white.point_count)))
assert ops.weight.assign(weight="1") == {'FINISHED'}
go(15)
assert look()["shape_error"].startswith("Loop"), look()
seen = []
for before in (14, 16, 0, 10, 20, 15):
    doc.frame_current = before
    doc.frame_current = 15
    seen.append(([tuple(p[:2]) for p in white.shown()[0]], bone("eye")["head"]))
for got in seen:
    assert max(math.dist(p, q) for p, q in zip(got[0], seen[0][0])) < 1e-5, "the white depends on the frame before"
    assert math.dist(got[1], seen[0][1]) < 1e-5, "the eye depends on the frame before"

# --- Limit to Shape the same -------------------------------------------------------------------------------------------
go(0)
rig.bone_constraint_set("eye", 0, enabled=False)
layers.active = rig
rig.select_bone("eye")
assert ops.armature.constraint_add(type="LIMIT_SHAPE", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 1, shape_layer=white.name)
go(10)
assert rig.bone_constraints("eye")[1]["shape_error"].startswith("Loop"), rig.bone_constraints("eye")[1]

# --- Let go of the loop: it works again ----------------------------------------------------------------------------------
go(0)
layers.active = white
assert ops.layer.parent_bone_set(bone="head") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, enabled=True)
go(10)
assert look()["shape_error"] == "" and rig.bone_constraints("eye")[1]["shape_error"] == ""
assert bone("eye")["head"][0] > 0.31, bone("eye")["head"]

print("test_shape_loop: all passed")
