"""An eye rig: Look At turns each eyeball toward a look target standing in front of the face at its Depth
(close, the eyes cross), from the rest pose, no further than Max Turn; the pupil a disc on the (stretched)
ball, seen from the front, its drawings going with it as drawn; Facing (keyable) turns the whole eye, as a head
turn does; Eyeball Auto takes its size from the Eye Shape at frame 0. Child Of carries the target with the head
(keyed Influence drops it). Saved.

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


def look_at(name, **settings):
    rig.select_bone(name)
    assert ops.armature.constraint_add(type="LOOK_AT", bone=name) == {'FINISHED'}
    i = len(rig.bone_constraints(name)) - 1
    rig.bone_constraint_set(name, i, target="look", eyeball_auto=False, eyeball_size=0.1, **settings)
    return i


for eye in ("eye_L", "eye_R"):
    look_at(eye)
c = rig.bone_constraints("eye_L")[0]
assert c["type"] == "LOOK_AT" and c["name"] == "Look At", c
assert "squash" not in c and "keep_round" not in c, "nothing warps any more"
assert abs(math.degrees(c["max_side"]) - 45) < 1e-3 and abs(math.degrees(c["max_up"]) - 30) < 1e-3, c
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


# At rest the eyes look as drawn, on any frame.
at(10)
assert max(map(abs, shift("eye_L") + shift("eye_R"))) < 1e-5, (shift("eye_L"), shift("eye_R"))

# The target moved right: both look right, by about as much.
at(10, look=dict(position=(0.4, 1.0)))
assert shift("eye_L")[0] > 0.01 and shift("eye_R")[0] > 0.01, (shift("eye_L"), shift("eye_R"))
assert abs(shift("eye_L")[1]) < 1e-4, "straight across, not up"
# Up: they look up.
at(12, look=dict(position=(0.0, 1.4)))
assert shift("eye_L")[1] > 0.01 and shift("eye_R")[1] > 0.01

# However far the target goes, the eye turns at most Max Turn (45 degrees to the side): the pupil at
# size * sin(45) across.
at(14, look=dict(position=(100.0, 1.0)))
assert abs(shift("eye_L")[0] - 0.1 * math.sin(math.radians(45))) < 1e-3, shift("eye_L")
# Up it stops sooner (30 degrees).
at(15, look=dict(position=(0.0, 100.0)))
assert abs(shift("eye_L")[1] - 0.1 * math.sin(math.radians(30))) < 2e-3, shift("eye_L")

assert bone("eye_L")["scale"][0] == 1.0, "the look shows in the pose, not the bone's own scale"

# Depth: brought close to the face, at rest between the eyes, they cross.
rig.select_bone("look")
at(16, look=dict(position=(0.0, 1.0), depth=0.2))
assert abs(bone("look")["depth"] - 0.2) < 1e-6
assert shift("eye_L")[0] < -0.01 and shift("eye_R")[0] > 0.01, ("cross-eyed", shift("eye_L"), shift("eye_R"))
assert [k["frame"] for k in rig.keyframes('bones["look"].depth')][-1] == 16, "Depth is keyed"
# Divergence turns each eye outward.
rig.bone_constraint_set("eye_L", 0, divergence=0.2)
at(16)
rig.bone_constraint_set("eye_L", 0, divergence=0.0)
at(18, look=dict(depth=1.0))

# The eye's own shape: Eyeball Auto takes half its size at frame 0; the pupil stays inside it; Facing turns
# the eye (a head turn): the pupil moves toward the turned front, keeping its shape.
doc.frame_current = 0


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


def spread(points, way):
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    d = [(p[0] - cx) * way[0] + (p[1] - cy) * way[1] for p in points]
    return max(d) - min(d)


white_R = drawing(ellipse(-0.3, 1.0, 0.2, 0.1), "head")
pupil_R = drawing(ellipse(-0.3, 1.0, 0.03, 0.03), "eye_R")
shine_R = drawing(ellipse(-0.3 + 0.015, 1.0 + 0.01, 0.01, 0.01), "eye_R")
layers.active = rig
rig.bone_constraint_set("eye_R", 0, shape_layer=white_R.name, fit="WHOLE", eyeball_auto=True)
at(40, look=dict(position=(0.0, 1.0)))
c = rig.bone_constraints("eye_R")[0]
assert c["shape_region"] > 3 and math.dist(c["eyeball_auto_size"], (0.2, 0.1)) < 0.01, c["eyeball_auto_size"]
assert max(map(abs, shift("eye_R"))) < 1e-5 and abs(spread(pupil_R.shown()[0], (1, 0)) - 0.06) < 1e-3, "at rest: as drawn"
# Facing 60 degrees to the side (the head turning). With the target where it rests the eye looks back at
# the camera, as far as Max Turn lets it: 60 - 45 = 15 degrees across.
rig.bone_constraint_set("eye_R", 0, facing=(math.radians(60), 0.0))
at(40)
assert abs(shift("eye_R")[0] - 0.2 * math.sin(math.radians(15))) < 2e-3, ("turned back toward the camera", shift("eye_R"))
# The target out that way (60 degrees from the camera, as the eye sees it): it looks along its facing, the
# pupil toward that side, its drawings moving and nothing else.
at(40, look=dict(position=(-0.3 + math.tan(math.radians(60) + math.atan(0.3)), 1.0)))
assert abs(shift("eye_R")[0] - 0.2 * math.sin(math.radians(60))) < 0.03, ("toward the turned front", shift("eye_R"))
def middle(points):
    return (sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points))


def as_drawn(*layers_following):
    for layer, size in layers_following:
        points = layer.shown()[0]
        assert abs(spread(points, (1, 0)) - size) < 1e-3 and abs(spread(points, (0, 1)) - size) < 1e-3, ("keeps its shape", layer.name)


as_drawn((pupil_R, 0.06), (shine_R, 0.02))
place = middle(shine_R.shown()[0])
pupil_at = middle(pupil_R.shown()[0])
assert math.dist((place[0] - pupil_at[0], place[1] - pupil_at[1]), (0.015, 0.01)) < 1e-3, "and its place on the pupil"
# What's drawn in the iris's middle (the pupil) stays there.
doc.frame_current = 0
iris_R = drawing(ellipse(-0.3, 1.0, 0.05, 0.05), "eye_R")
layers.active = rig
at(40)
iris_mid, pupil_mid = middle(iris_R.shown()[0]), middle(pupil_R.shown()[0])
assert math.dist(iris_mid, pupil_mid) < 1e-4, ("the pupil in the middle of the iris", iris_mid, pupil_mid)
right = max(p[0] for p in white_R.shown()[0])
assert max(p[0] for p in pupil_R.shown()[0]) <= right + 1e-3, "inside the eye white"
# Facing is keyable: a head turn keys it.
path = 'bones["eye_R"].constraints[0].facing'
assert ops.anim.keyframe_toggle(layer=str(rig.id), paths=path) == {'FINISHED'}
doc.frame_current = 44
rig.bone_constraint_set("eye_R", 0, facing=(0.0, 0.0))
doc.frame_current = 45
assert [k["frame"] for k in rig.keyframes(path)][-2:] == [40, 44], rig.keyframes(path)
at(44, look=dict(position=(0.0, 1.0)))
assert max(map(abs, shift("eye_R"))) < 1e-4, "facing the camera, the target at rest: as drawn"
# Looking right while the eye faces 60 degrees left: it turns back no further than Max Turn (to -15).
rig.bone_constraint_set("eye_R", 0, facing=(math.radians(-60), 0.0))
at(46, look=dict(position=(5.0, 1.0)))
assert abs(shift("eye_R")[0] + 0.2 * math.sin(math.radians(15))) < 0.03, ("the far eye looks right as far as it can", shift("eye_R"))
as_drawn((pupil_R, 0.06))
rig.bone_constraint_set("eye_R", 0, facing=(0.0, 0.0))

# Looking diagonally the pupil moves that way, as drawn.
rig.bone_constraint_set("eye_R", 0, facing=(math.radians(-30), 0.0))
at(48, look=dict(position=(1.0, 0.0)))
way = shift("eye_R")
assert way[0] > 0.01 and way[1] < -0.01, ("down and right", way)
as_drawn((pupil_R, 0.06), (shine_R, 0.02))
rig.bone_constraint_set("eye_R", 0, facing=(0.0, 0.0))

# Eyeball Center: the ball's middle at a bone off the pupil (the pupil drawn looking a little right at
# rest). At rest the eye is as drawn; looking far right it goes round the ball about that middle.
doc.frame_current = 0
ops.armature.bone_add(name="ball_R", head_x="-0.32", head_y="1", tail_x="-0.32", tail_y="1.02", parent="head")
layers.active = rig
rig.bone_constraint_set("eye_R", 0, eyeball_center="ball_R")
assert rig.bone_constraints("eye_R")[0]["eyeball_center"] == "ball_R"
at(50, look=dict(position=(0.0, 1.0)))
assert max(map(abs, shift("eye_R"))) < 1e-4, ("as drawn at rest", shift("eye_R"))
assert abs(spread(pupil_R.shown()[0], (1, 0)) - 0.06) < 1e-3, "round at rest"
at(52, look=dict(position=(100.0, 1.0)))
want = -0.32 + 0.2 * math.sin(math.asin(0.02 / 0.2) + math.radians(45)) + 0.3
assert abs(shift("eye_R")[0] - want) < 2e-3, ("round the ball about its middle", shift("eye_R"), want)
# A bigger ball would take it past the eye white: it stops at the edge (Whole Drawing), not short of it.
rig.bone_constraint_set("eye_R", 0, eyeball_auto=False, eyeball_size=0.4)
at(52)
edge = max(p[0] for p in white_R.shown()[0])
reach = max(p[0] for p in iris_R.shown()[0])  # the widest drawing the bone carries
assert abs(reach - edge) < 5e-3, ("at the edge of its Eye Shape", reach, edge)
rig.bone_constraint_set("eye_R", 0, eyeball_auto=True)

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

# Saved with the file (Max Turn's three angles too).
rig.bone_constraint_set("eye_R", 0, max_side=math.radians(90), max_up=math.radians(20), max_down=math.radians(50))
path = str(pathlib.Path(tempfile.mkdtemp()) / "look_at.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers.all() if l.type == "BONE")
c = rig.bone_constraints("eye_L")[0]
assert c["type"] == "LOOK_AT" and not c["eyeball_auto"] and abs(c["eyeball_size"][0] - 0.1) < 1e-6, c
assert abs(math.degrees(c["max_side"]) - 45) < 1e-3 and abs(math.degrees(c["max_up"]) - 30) < 1e-3
c = rig.bone_constraints("eye_R")[0]
assert [round(math.degrees(c[k])) for k in ("max_side", "max_up", "max_down")] == [90, 20, 50], c
assert {40, 44} <= {k["frame"] for k in rig.keyframes('bones["eye_R"].constraints[0].facing')}
assert rig.bone_constraints("look")[0]["type"] == "CHILD_OF"
assert rig.bone_constraints("eye_R")[0]["eyeball_center"] == "ball_R", "Eyeball Center saved"
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
