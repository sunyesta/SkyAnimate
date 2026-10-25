"""Apply Pose as Rest Pose and Apply Selected as Rest Pose (Ctrl A, Blender's): the pose on the frame
shown becomes the setup pose, the drawings come along, and every keyed frame of the mainline and every
action shows as it did. Smart bones keep their rest; bones left out keep where they rest.

Run with:  OpenAnimate --background --python tests/test_apply_rest_pose.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
ops.armature.bone_add(name="b", head_x="1", head_y="0", tail_x="2", tail_y="0", parent="a")
ops.armature.bone_add(name="c", head_x="2", head_y="0", tail_x="3", tail_y="0", parent="b")
ops.armature.bone_add(name="dial", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="a")
ops.layer.new()
art = layers.active
assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
art = next(l for l in layers.all() if l.id == art.id)
art.add_curve([(0.1, 0.1), (0.9, -0.1), (1.5, 0.1), (2.5, -0.1), (2.9, 0.05)])
layers.active = art
ops.mesh.select_all()
for name in ("a", "b", "c"):
    rig.select_bone(name, extend=name != "a")
assert ops.weight.automatic() == {'FINISHED'}
layers.active = rig


def pose(frame, **bones):
    doc.frame_current = frame
    for name, values in bones.items():
        rig.bone_set(name, **values)
    doc.frame_current = frame + 1
    doc.frame_current = frame


# A smart action: turning the dial bends b.
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", smart="1") == {'FINISHED'}
pose(10, dial=dict(angle=1.0), b=dict(angle=0.4))
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}

# The mainline: a pose on 10 (the one applied), more on 20 and 30 (the dial turned there).
pose(10, a=dict(angle=0.5, position=(0.2, 0.3)), b=dict(angle=0.3), c=dict(angle=-0.2), dial=dict(angle=0.3))
pose(20, a=dict(angle=-0.3), b=dict(angle=0.8), c=dict(angle=0.4))
pose(30, a=dict(angle=0.1), dial=dict(angle=1.0))
# An action: b waves.
assert ops.anim.action_new(name="wave") == {'FINISHED'}
pose(10, b=dict(angle=-0.6))
pose(20, a=dict(angle=0.9), b=dict(angle=0.2))
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}


# A wide Limit Angle on a: its turns move with the rest, so it holds the same angles.
doc.frame_current = 0
assert ops.armature.constraint_add(bone="a", type="LIMIT_ANGLE") == {'FINISHED'}
rig.bone_constraint_set("a", 0, min_angle=-2.0, max_angle=2.0)


def look(action, frame):
    if doc.action != action:
        assert ops.anim.action_set(name=action) == {'FINISHED'}
    doc.frame_current = frame
    bones = {b["name"]: (b["head"], b["tail"]) for b in rig.bones}
    return bones, art.shown()[0]


def worst(now, before):
    (bn, pn), (bb, pb) = now, before
    w = max(math.dist(p, q) for p, q in zip(pn, pb))
    for name in bb:
        w = max(w, math.dist(bn[name][0], bb[name][0]), math.dist(bn[name][1], bb[name][1]))
    return w


# Keyed frames, one between keys and one past the last. Frames before a timeline's first key ease out
# of the rest pose, which is the new one now (and between its first two keys a Smooth curve leans on
# that setup key), so those may change; the rest don't. A bone a timeline has no keys for gets one
# holding its old rest ("wave" has none for c).
frames = [("", 10), ("", 20), ("", 25), ("", 30), ("wave", 20), ("wave", 30)]


def tolerance(f):
    # Keyed frames exactly; between keys, points blended between bones follow their keys a little
    # differently from how the bones bent them (as Keep Animation's always do).
    return 5e-3 if f == ("", 25) else 1e-4
before = {f: look(*f) for f in frames}
rest = look("", 0)
dial_rest = next(b for b in rig.bones if b["name"] == "dial")

# Apply the pose on frame 10 of the mainline.
look("", 10)
layers.active = rig
assert ops.armature.select_all() == {'FINISHED'}  # an undo step: what undo goes back to
assert ops.armature.apply_pose_as_rest() == {'FINISHED'}
# One undo step (checked before going through actions, which are steps of their own).
ops.ed.undo()
assert worst(look("", 0), rest) < 1e-6, "undone"
for f in frames:
    assert worst(look(*f), before[f]) < 1e-6, ("undone", f)
look("", 10)
layers.active = rig
assert ops.armature.apply_pose_as_rest() == {'FINISHED'}
assert doc.frame_current == 10 and doc.action == ""
limit = rig.bone_constraints("a")[0]
assert abs(limit["min_angle"] + 2.5) < 1e-5 and abs(limit["max_angle"] - 1.5) < 1e-5, limit
now0 = look("", 0)
target = before[("", 10)]
# Frame 10 had the dial turned 0.3: the smart action's share of b stays out of the rest.
assert math.dist(now0[0]["a"][1], target[0]["a"][1]) < 1e-4, "frame 0 is frame 10's pose"
for f in frames:
    w = worst(look(*f), before[f])
    assert w < tolerance(f), ("kept", f, w)
dial_now = next(b for b in rig.bones if b["name"] == "dial")
assert abs(dial_now["angle"] - dial_rest["angle"]) < 1e-5, "the smart bone keeps its rest"
assert worst(look("", 0), rest) > 0.1, "the rest moved"


# Apply Selected, from an action: only b; c (left out) keeps where it rests.
before = {f: look(*f) for f in frames}
rest = look("", 0)
look("wave", 20)
layers.active = rig
oanim.ops.armature.select_none()
rig.select_bone("b")
assert ops.armature.apply_selected_as_rest() == {'FINISHED'}
assert doc.action == "wave" and doc.frame_current == 20
now0 = look("", 0)
assert math.dist(now0[0]["a"][0], rest[0]["a"][0]) < 1e-6 and math.dist(now0[0]["a"][1], rest[0]["a"][1]) < 1e-6, "a stays"
assert math.dist(now0[0]["c"][0], rest[0]["c"][0]) < 1e-5 and math.dist(now0[0]["c"][1], rest[0]["c"][1]) < 1e-5, "c stays"
assert math.dist(now0[0]["b"][1], rest[0]["b"][1]) > 0.05, "b turned at rest"
for f in frames:
    w = worst(look(*f), before[f])
    assert w < tolerance(f) * 2, ("kept after Apply Selected", f, w)

# On the setup frame of the mainline there is nothing to apply.
look("", 0)
layers.active = rig
assert ops.armature.apply_pose_as_rest() == {'CANCELLED'}

# Ctrl A opens the Apply menu over the viewport.
assert oanim.wm.event_simulate("A", ctrl=True, area="Viewport"), "Ctrl A: the Apply menu"

print("apply rest pose: ok")
