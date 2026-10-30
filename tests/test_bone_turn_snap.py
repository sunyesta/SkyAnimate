"""Turning bones with Ctrl, and 360 smart actions (Moho 14.4's 360 smart bone).

Ctrl while turning (R, or the Transform Bones tool's turn) lands the turn on the nearest of: the reference bone
(the active one) pointing a multiple of 45 degrees on screen, its angle when the turn began, and posing its setup
angle, each with whole turns more or less.

A smart action whose bone is keyed exactly one whole turn from its setup goes round and round: the bone's turn
counts modulo a turn, so turning past the end starts it over and turning back past the setup plays it from the
end.

Run with:  OpenAnimate --background --python tests/test_bone_turn_snap.py
"""

import math

import oanim

doc = oanim.data.document
layers = oanim.data.layers
ops = oanim.ops
wm = oanim.wm
doc.frame_current = 0

assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
# Pointing 10 degrees up from the right.
a10 = math.radians(10.0)
ops.armature.bone_add(name="dial", head_x="0", head_y="0", tail_x=str(math.cos(a10)), tail_y=str(math.sin(a10)),
                      parent="")
ops.armature.bone_add(name="other", head_x="2", head_y="0", tail_x="2", tail_y="1", parent="")
rig.select_bone("dial")


def snap(degrees, rotate_key=True):
    return wm.bone_turn_snap(degrees, rotate_key)


def near(a, b, eps=1e-3):
    return abs(a - b) < eps


# --- The setup frame: 45 degree directions on screen, and the start ----------------------------------------
d, what = snap(30.0)
assert near(d, 35.0) and what == "45°", ("10 + 30 = 40 lands on 45", d, what)
d, what = snap(3.0)
assert near(d, 0.0) and what == "start", ("nearer the start than 0 degrees on screen", d, what)
d, what = snap(-8.0)
assert near(d, -10.0) and what == "45°", ("pointing right", d, what)
d, what = snap(358.0)
assert near(d, 360.0) and what == "start", ("a whole turn round, back on the start", d, what)
d, what = snap(-361.0)
assert near(d, -360.0) and what == "start", d
# The Transform Bones tool's turn snaps the same.
d, what = snap(30.0, rotate_key=False)
assert near(d, 35.0) and what == "45°", (d, what)

# --- Posing: its setup angle too ----------------------------------------------------------------------------
doc.frame_current = 5
rig.bone_set("dial", angle=math.radians(10.0 + 100.0))  # posed: pointing 110 degrees
ops.armature.select_none()
rig.select_bone("dial")
d, what = snap(-97.0)
assert near(d, -100.0) and what == "setup", ("back to where it rests", d, what)
d, what = snap(-95.0)
assert near(d, -100.0) and what == "setup", ("the setup (10 degrees) beats the 45 degree direction (0)", d, what)
d, what = snap(-112.0)
assert near(d, -110.0) and what == "45°", ("pointing right, 0 degrees", d, what)
d, what = snap(-97.0 + 360.0)
assert near(d, 260.0) and what == "setup", ("a whole turn more", d, what)
d, what = snap(2.0)
assert near(d, 0.0) and what == "start", (d, what)
# The active bone is the one measured.
ops.armature.select_none()
rig.select_bone("other")
rig.select_bone("dial", extend=True)
d, what = snap(-97.0)
assert near(d, -100.0) and what == "setup", ("the active bone, dial", d, what)
doc.frame_current = 0

# --- 360 smart actions --------------------------------------------------------------------------------------
ops.layer.new()
face = layers.active
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig
ops.armature.select_none()
rig.select_bone("dial")
assert ops.anim.action_new(name="dial", smart="1") == {'FINISHED'} and doc.action == "dial"
# Once round over 36 frames, the face going out and back: x = 0, 1, 2, 1 and 0 (at frames 0, 9, 18, 27, 36).
for frame, x in ((9, 1.0), (18, 2.0), (27, 1.0), (36, 0.0)):
    doc.frame_current = frame
    rig.bone_set("dial", angle=a10 + math.radians(10.0 * frame))
    face.location_x = x
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}
assert [a for a in rig.actions if a["name"] == "dial"][0]["full_turn"], rig.actions

doc.frame_current = 40


def turned(degrees):
    rig.bone_set("dial", angle=a10 + math.radians(degrees))
    doc.frame_current = doc.frame_current  # a sync, as the app does every frame
    return face.location_x


at_90 = turned(90.0)
assert near(at_90, 1.0, 2e-2), at_90
assert near(turned(90.0 + 360.0), at_90, 2e-3), "round again: the same pose"
assert near(turned(90.0 + 720.0), at_90, 2e-3), "and again"
at_270 = turned(270.0)
assert near(turned(-90.0), at_270, 2e-3), "turned back past the setup: from the end"
assert near(turned(-90.0 - 360.0), at_270, 2e-3)
assert near(turned(360.0), 0.0, 2e-3) and near(turned(0.0), 0.0, 2e-3), "a whole turn is the setup again"

# Not a whole turn: holds past the end, nothing the other way, as before. (Bones are added on the setup frame.)
doc.frame_current = 0
ops.armature.bone_add(name="knob", head_x="0", head_y="-2", tail_x="1", tail_y="-2", parent="")
ops.layer.new()
lamp = layers.active
ops.layer.move_to_group(group=str(rig.id))
layers.active = rig
doc.frame_current = 0
ops.armature.select_none()
rig.select_bone("knob")
assert ops.anim.action_new(name="knob", smart="1") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("knob", angle=math.radians(300.0))
lamp.location_y = 3.0
doc.frame_current = 0
assert ops.anim.action_set(name="") == {'FINISHED'}
assert not [a for a in rig.actions if a["name"] == "knob"][0]["full_turn"]
doc.frame_current = 40
rig.bone_set("knob", angle=math.radians(500.0))
doc.frame_current = doc.frame_current
assert near(lamp.location_y, 3.0, 2e-3), ("past the end it holds", lamp.location_y)
rig.bone_set("knob", angle=math.radians(-60.0))
doc.frame_current = doc.frame_current
assert near(lamp.location_y, 0.0, 2e-3), ("the other way: nothing", lamp.location_y)

print("bone turn snap: ok")
