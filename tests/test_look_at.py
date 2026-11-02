"""An eye rig: Look At slides each pupil toward a look target standing in front of the face at its Depth
(close, the eyes cross), from the rest pose: the pupil a disc on a (stretched) ball as big as its Eye Shape at
frame 0, seen from the front, its drawings going with it as drawn, and staying inside that shape. With no Eye
Shape it does nothing. Child Of carries the target with the head (keyed Influence drops it). Saved.

Run with:  OpenAnimate --background --python tests/test_look_at.py
"""

import math
import pathlib
import tempfile

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0
doc.auto_key = True

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="2", parent="")
ops.armature.bone_add(name="eye_L", head_x="0.3", head_y="1", tail_x="0.3", tail_y="1.05", parent="head")
ops.armature.bone_add(name="eye_R", head_x="-0.3", head_y="1", tail_x="-0.3", tail_y="1.05", parent="head")
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


white_L = drawing(ellipse(0.3, 1.0, 0.2, 0.1), "head")
white_R = drawing(ellipse(-0.3, 1.0, 0.2, 0.1), "head")
pupil_L = drawing(ellipse(0.3, 1.0, 0.03, 0.03), "eye_L")
pupil_R = drawing(ellipse(-0.3, 1.0, 0.03, 0.03), "eye_R")
shine_R = drawing(ellipse(-0.3 + 0.015, 1.0 + 0.01, 0.01, 0.01), "eye_R")
layers.active = rig


def look_at(name):
    rig.select_bone(name)
    assert ops.armature.constraint_add(type="LOOK_AT", bone=name) == {'FINISHED'}
    i = len(rig.bone_constraints(name)) - 1
    rig.bone_constraint_set(name, i, target="look")
    return i


for eye in ("eye_L", "eye_R"):
    look_at(eye)
c = rig.bone_constraints("eye_L")[0]
assert c["type"] == "LOOK_AT" and c["name"] == "Look At", c
assert "squash" not in c and "keep_round" not in c, "nothing warps any more"
for gone in ("facing", "max_side", "max_up", "max_down", "eyeball_size", "eyeball_auto", "eyeball_center", "look_range", "range_auto"):
    assert gone not in c, gone
    try:
        rig.bone_constraint_set("eye_L", 0, **{gone: 0.1})
    except KeyError:
        continue
    raise AssertionError(gone + " can still be set")
assert abs(bone("look")["depth"] - 1.0) < 1e-6


def at(frame, **bones):
    doc.frame_current = frame
    for name, values in bones.items():
        rig.bone_set(name, **values)
    doc.frame_current = frame + 1
    doc.frame_current = frame


def shift(name):
    rest = {"eye_L": (0.3, 1.0), "eye_R": (-0.3, 1.0)}[name]
    h = bone(name)["head"]
    return (h[0] - rest[0], h[1] - rest[1])


# With no Eye Shape there's no eye to look with: it does nothing.
at(8, look=dict(position=(0.4, 1.0)))
assert max(map(abs, shift("eye_L") + shift("eye_R"))) < 1e-6, (shift("eye_L"), shift("eye_R"))
assert rig.bone_constraints("eye_L")[0]["look_reach"] == (0.0, 0.0)

# The eye's size comes from its shape at frame 0.
rig.bone_constraint_set("eye_L", 0, shape_layer=white_L.name, fit="WHOLE")
rig.bone_constraint_set("eye_R", 0, shape_layer=white_R.name, fit="WHOLE")
at(9, look=dict(position=(0.0, 1.0)))
c = rig.bone_constraints("eye_R")[0]
assert c["shape_region"] > 3 and math.dist(c["look_reach"], (0.2, 0.1)) < 0.01, c["look_reach"]

# At rest the eyes look as drawn, on any frame.
at(10)
assert max(map(abs, shift("eye_L") + shift("eye_R"))) < 1e-5, (shift("eye_L"), shift("eye_R"))
assert abs(max(p[0] for p in pupil_R.shown()[0]) - min(p[0] for p in pupil_R.shown()[0]) - 0.06) < 1e-3, "at rest: as drawn"

# The target moved right: both look right, by how far the look has turned from its rest, on the ball.
at(10, look=dict(position=(0.4, 1.0)))
want = 0.2 * math.sin(math.atan(0.1) - math.atan(-0.3))
assert abs(shift("eye_L")[0] - want) < 2e-3, (shift("eye_L"), want)
assert shift("eye_R")[0] > 0.01, shift("eye_R")
assert abs(shift("eye_L")[1]) < 1e-4, "straight across, not up"
# Up: they look up.
at(12, look=dict(position=(0.0, 1.4)))
assert shift("eye_L")[1] > 0.01 and shift("eye_R")[1] > 0.01

# However far the target goes, the Eye Shape stops the pupil: all of it inside (Whole Drawing).
at(14, look=dict(position=(100.0, 1.0)))
assert abs(max(p[0] for p in pupil_L.shown()[0]) - max(p[0] for p in white_L.shown()[0])) < 5e-3, shift("eye_L")
at(15, look=dict(position=(0.0, 100.0)))
assert abs(max(p[1] for p in pupil_L.shown()[0]) - max(p[1] for p in white_L.shown()[0])) < 5e-3, shift("eye_L")
# Center lets the head reach the edge, and the turn stops at the side of the ball: past it the pupil doesn't
# come back round.
rig.bone_constraint_set("eye_L", 0, fit="CENTER")
at(14)
assert abs(shift("eye_L")[0] - 0.2) < 3e-3, shift("eye_L")
rig.bone_constraint_set("eye_L", 0, fit="WHOLE")

assert bone("eye_L")["scale"][0] == 1.0, "the look shows in the pose, not the bone's own scale"

# Depth: brought close to the face, at rest between the eyes, they cross.
rig.select_bone("look")
at(16, look=dict(position=(0.0, 1.0), depth=0.2))
assert abs(bone("look")["depth"] - 0.2) < 1e-6
assert shift("eye_L")[0] < -0.01 and shift("eye_R")[0] > 0.01, ("cross-eyed", shift("eye_L"), shift("eye_R"))
assert [k["frame"] for k in rig.keyframes('bones["look"].depth')][-1] == 16, "Depth is keyed"
at(18, look=dict(depth=1.0))


def spread(points, way):
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    d = [(p[0] - cx) * way[0] + (p[1] - cy) * way[1] for p in points]
    return max(d) - min(d)


def middle(points):
    return (sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points))


def as_drawn(*layers_following):
    for layer, size in layers_following:
        points = layer.shown()[0]
        assert abs(spread(points, (1, 0)) - size) < 1e-3 and abs(spread(points, (0, 1)) - size) < 1e-3, ("keeps its shape", layer.name)


# Its drawings go with it as drawn, keeping their shape and their place on the pupil.
at(40, look=dict(position=(0.5, 0.7)))
assert shift("eye_R")[0] > 0.01 and shift("eye_R")[1] < -0.01, ("down and right", shift("eye_R"))
as_drawn((pupil_R, 0.06), (shine_R, 0.02))
place = middle(shine_R.shown()[0])
pupil_at = middle(pupil_R.shown()[0])
assert math.dist((place[0] - pupil_at[0], place[1] - pupil_at[1]), (0.015, 0.01)) < 1e-3, "and its place on the pupil"

# The pupil's own keys move it on the ball, and where it actually is stays inside. (A bone hanging from the
# head, pointing up: its position's y is to the left.)
at(42, look=dict(position=(0.0, 1.0)), eye_R=dict(position=(1.0, 0.25)))
assert math.dist(shift("eye_R"), (0.05, 0.0)) < 2e-3, shift("eye_R")
at(43, look=dict(position=(100.0, 1.0)))
assert abs(max(p[0] for p in pupil_R.shown()[0]) - max(p[0] for p in white_R.shown()[0])) < 5e-3, "inside the eye white"
at(44, look=dict(position=(0.0, 1.0)), eye_R=dict(position=(1.0, 0.3)))

# The look works along the face's axes: a head turned in the picture with the target turning with it looks
# as it did, the eye only carried along.
a = 0.3
at(46, head=dict(angle=bone("head")["setup_angle"] + a), look=dict(position=(-math.sin(a), math.cos(a))))
carried = (-0.3 * math.cos(a) - math.sin(a), -0.3 * math.sin(a) + math.cos(a))
assert math.dist(bone("eye_R")["head"], carried) < 1e-3, (bone("eye_R")["head"], carried)
at(46, head=dict(angle=bone("head")["setup_angle"]), look=dict(position=(0.0, 1.0)))

# Picking no shape again stops it.
rig.bone_constraint_set("eye_R", 0, shape_layer="")
at(47, look=dict(position=(0.4, 1.0)))
assert max(map(abs, shift("eye_R"))) < 1e-5 and rig.bone_constraints("eye_R")[0]["look_reach"] == (0.0, 0.0), shift("eye_R")
rig.bone_constraint_set("eye_R", 0, shape_layer=white_R.name)
at(47, look=dict(position=(0.0, 1.0)))

# Child Of: the target rides with the head; the eyes keep looking where they did on the face.
doc.frame_current = 0
rig.select_bone("look")
assert ops.armature.constraint_add(type="CHILD_OF", bone="look") == {'FINISHED'}
rig.bone_constraint_set("look", 0, target="head")
at(30, look=dict(position=(0.0, 1.0)))
before = (shift("eye_L"), shift("eye_R"))
at(30, head=dict(angle=bone("head")["setup_angle"] + 0.5))
h = bone("head")
turned = (math.cos(0.5), math.sin(0.5))
look_head = bone("look")["head"]
expected = (-math.sin(0.5) * 1.0, math.cos(0.5) * 1.0)
assert math.dist(look_head, expected) < 1e-3, ("the target rides with the head", look_head, expected)
# Influence 0: it stays where it is in the picture.
rig.bone_constraint_set("look", 0, influence=0.0)
at(30)
assert math.dist(bone("look")["head"], (0.0, 1.0)) < 1e-4

# Saved with the file, with its Eye Shape and Divergence.
rig.bone_constraint_set("eye_R", 0, divergence=0.05)
path = str(pathlib.Path(tempfile.mkdtemp()) / "look_at.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
c = rig.bone_constraints("eye_R")[0]
assert c["type"] == "LOOK_AT" and c["target"] == "look" and abs(c["divergence"] - 0.05) < 1e-6, c
doc.frame_current = 30
assert c["shape_layer"] == white_R.name or rig.bone_constraints("eye_R")[0]["shape_region"] > 3, c
assert rig.bone_constraints("look")[0]["type"] == "CHILD_OF"
assert [k["frame"] for k in rig.keyframes('bones["look"].depth')][-2:] == [16, 18]

# Driving Depth by the target's own scale: Copy As New Driver on Scale copies two numbers (X and Y);
# onto Depth, one number, either is pasted by `part` (the menu lists "Paste Driver: look: Scale Y"...).
scale_path, depth_path = 'bones["look"].scale', 'bones["look"].depth'
assert ops.anim.driver_copy_new(layer=str(rig.id), path=scale_path, component="-1") == {'CANCELLED'}  # copies, no undo step
assert ops.anim.driver_paste(layer=str(rig.id), path=depth_path, component="-1") == {'CANCELLED'}, "two onto one: pick"
assert ops.anim.driver_paste(layer=str(rig.id), path=depth_path, component="-1", part="1") == {'FINISHED'}
doc.frame_current = 50
rig.bone_set("look", scale=(1.0, 2.5))
doc.frame_current = 51
doc.frame_current = 50
assert abs(bone("look")["depth"] - 2.5) < 1e-5, ("Depth follows Scale Y", bone("look")["depth"])

print("look at: ok")
