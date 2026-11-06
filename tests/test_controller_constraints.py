"""Constraints on a 2 Axis Controller: those that place a bone move its knob, and its actions play from where they put
it (its keys on top); those that turn a bone turn the box with the knob in it. Look At needs no Eye Shape: the box is
the eye, and looking all the way to a side takes the knob to that edge. They are off while one of its actions is edited,
and the ones that don't fit (Stretch To, Maintain Volume, IK...) can't be added.

Run with:  OpenAnimate --background --python tests/test_controller_constraints.py
"""

import math

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active


def bone(name):
    return next(b for b in rig.bones if b["name"] == name)


def go(frame):
    doc.frame_current = frame + 1
    doc.frame_current = frame


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def playing(name):
    return next(a["frame"] for a in rig.actions if a["name"] == name)


def shown():
    return bone("eye")["knob_shown"]


assert ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="0.1", tail_y="0") == {'FINISHED'}
ops.armature.select_none()
assert ops.armature.bone_add(name="look", head_x="0", head_y="0", tail_x="0", tail_y="0.1") == {'FINISHED'}
ops.armature.select_none()
assert ops.armature.bone_add(name="turn", head_x="0", head_y="-1", tail_x="0.1", tail_y="-1") == {'FINISHED'}
ops.armature.select_none()
assert ops.armature.bone_add(name="dot", head_x="0", head_y="-2", tail_x="0.1", tail_y="-2") == {'FINISHED'}
ops.layer.new()
pupil = layers.active
ops.layer.move_to_group(group=str(rig.id))
pupil.add_curve([(0.0, 0.0), (0.2, 0.0)])
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
for name, to in (("eye_r", (0.3, 0.0)), ("eye_l", (0.1, 0.0))):
    layers.active = pupil
    assert ops.anim.action_new(name=name) == {'FINISHED'}
    doc.frame_current = 40
    pupil.set_point(1, to)
    go(40)
    doc.action = ""
    go(0)
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="eye_r") == {'FINISHED'}
assert ops.armature.controller_side(side="LEFT", action="eye_l") == {'FINISHED'}
size = bone("eye")["controller_frame_size"]

# --- What can be added ----------------------------------------------------------------------------------------------
for kind in ("STRETCH_TO", "MAINTAIN_VOLUME", "IK", "LIMIT_ANGLE", "COPY_SCALE"):
    assert ops.armature.constraint_add(type=kind, bone="eye") == {'CANCELLED'}, kind
assert rig.bone_constraints("eye") == []

# --- Look At: the box is the eye ------------------------------------------------------------------------------------
assert ops.armature.constraint_add(type="LOOK_AT", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, target="look")
go(10)
assert close(shown(), (0, 0)), ("the look at rest: the knob too", shown())
doc.frame_current = 10
rig.bone_set("look", position=(0.5, 0.0))
go(10)
k = shown()
assert k[0] > 1.0 and close(k[1], 0), ("looking right moves the knob right", k)
assert close(playing("eye_r"), k[0]) and close(playing("eye_l"), 0), ("its actions play from there", playing("eye_r"))
assert close(bone("eye")["head"], (0, 0)), ("the box stays where it rests", bone("eye")["head"])
rig.bone_set("look", position=(1000.0, 0.0))
go(10)
assert close(shown(), (40, 0), 0.05), ("all the way to a side: the box's edge", shown())
rig.bone_set("look", position=(-0.5, 0.0))
go(10)
assert close(shown(), (-k[0], 0)) and close(playing("eye_l"), k[0]), ("and left", shown(), playing("eye_l"))
rig.bone_constraint_set("eye", 0, influence=0.5)
go(10)
assert close(shown(), (-k[0] / 2, 0)), ("half the influence, half the way", shown())
rig.bone_constraint_set("eye", 0, influence=1.0)
rig.bone_set("look", position=(0.0, 0.0))

# Its keys go on top.
rig.bone_set("eye", knob=(5.0, 0.0))
go(10)
assert close(shown(), (5, 0)) and close(playing("eye_r"), 5), (shown(), playing("eye_r"))
rig.bone_set("look", position=(0.5, 0.0))
go(10)
assert close(shown(), (5 + k[0], 0)), ("the look on top of its keys", shown())

# Off while one of its actions is edited.
assert ops.anim.action_set(name="eye_r") == {'FINISHED'}
go(20)
assert close(shown(), (20, 0)), ("in its action the knob sits at the frame", shown())
assert ops.anim.action_set(name="") == {'FINISHED'}
rig.bone_constraint_set("eye", 0, enabled=False)
go(10)
assert close(shown(), (5, 0)), shown()

# --- Copy Position To Target: the knob goes to the target's base ---------------------------------------------------
assert ops.armature.constraint_add(type="COPY_POSITION", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 1, target="dot")
rig.bone_set("eye", knob=(0.0, 0.0))
rig.bone_set("dot", position=(15 * size, 2.0))
go(10)
assert close(shown(), (15, 0)) and close(playing("eye_r"), 15), ("the knob goes to the target", shown(), playing("eye_r"))
rig.bone_constraint_set("eye", 1, enabled=False)

# --- Copy Angle: the box turns, the knob with it --------------------------------------------------------------------
assert ops.armature.constraint_add(type="COPY_ANGLE", bone="eye") == {'FINISHED'}
rig.bone_constraint_set("eye", 2, target="turn")
rig.bone_set("eye", knob=(10.0, 0.0))
go(10)
before = bone("eye")
rig.bone_set("turn", angle=bone("turn")["setup_angle"] + 0.5)
go(10)
eye = bone("eye")
assert close(eye["knob_shown"], (10, 0)) and close(playing("eye_r"), 10), ("the knob stays in the box", eye["knob_shown"])
off = (eye["knob_head"][0] - eye["head"][0], eye["knob_head"][1] - eye["head"][1])
assert close(off, (10 * size * math.cos(0.5), 10 * size * math.sin(0.5))), ("the box turned with it", off)

# Off while one of its actions is edited: the box doesn't turn there either.
assert ops.anim.action_set(name="eye_r") == {'FINISHED'}
doc.frame_current = 20
rig.bone_set("turn", angle=bone("turn")["setup_angle"] + 0.5)
go(20)
eye = bone("eye")
off = (eye["knob_head"][0] - eye["head"][0], eye["knob_head"][1] - eye["head"][1])
assert close(off, (20 * size, 0)), ("in its action the box doesn't turn", off)
assert ops.anim.action_set(name="") == {'FINISHED'}

print("Controller constraint tests passed")
