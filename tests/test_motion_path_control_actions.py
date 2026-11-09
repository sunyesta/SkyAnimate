"""A control point on the path of a bone that action bone actions move (a head a head turn moves), with Own Motion:
the path through it is placed where the bone shows, and dragging it moves the path the same way as the mouse.
Actions add their change from the bone's setup, so placing a value keeps the setup as it is (held on one key moved to
the frame, the setup moved with the value and the actions seemed to pull the bone back: the path ran against the drag).

Run with:  OpenAnimate --background --python tests/test_motion_path_control_actions.py
"""

import math

import oanim

doc, layers, ops, wm = oanim.data.document, oanim.data.layers, oanim.ops, oanim.wm


def close(a, b, eps=1e-3):
    return math.dist(a, b) < eps


doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
# Upright, as a head is: its keys are turned from where it shows.
ops.armature.bone_add(name="head", head_x="0", head_y="0", tail_x="0", tail_y="1", parent="")
ops.armature.select_none()
ops.armature.bone_add(name="knob", head_x="3", head_y="0", tail_x="3.5", tail_y="0", parent="")
# The knob's action moves the head over (and keys it, as an action keys every bone).
assert ops.anim.action_new(name="turn") == {'FINISHED'}
doc.auto_key = True
doc.frame_current = 10
rig.bone_set("head", position=(0.4, 0.2))
doc.frame_current = 11
doc.action = ""
doc.frame_current = 0
layers.active = rig
ops.armature.select_none()
rig.select_bone("knob")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="turn") == {'FINISHED'}
# The knob halfway to the right over the head's stretch: the action plays half.
for f in (10, 20):
    doc.frame_current = f
    rig.bone_set("knob", knob=(0.5, 0.0))
# The head's own keys (turned too: its tip's path goes its own way, the control point is the head's alone).
for f, at, turn in ((10, (0.0, 0.3), 1.6), (20, (1.0, 0.6), 1.4)):
    doc.frame_current = f
    rig.bone_set("head", position=at, angle=turn)
doc.frame_current = 21
doc.auto_key = False
assert [k["frame"] for k in rig.keyframes('bones["head"].position')] == [0, 10, 20]


doc.frame_current = 12
doc.motion_paths = True
doc.path_own_motion = True
assert ops.wm.tool_motion_path() == {'FINISHED'}
for other in layers.all():
    other.selected = other.id == rig.id
layers.active = rig
ops.armature.select_none()
rig.select_bone("head")
args = dict(bone="head", head="1", layer=str(rig.id))


def path():
    return next(p for p in wm.motion_paths() if p["label"] == "head head")


def dot(f):
    p = path()
    return tuple(p["points"][f - p["first"]])


assert ops.anim.motion_path_select(frame="20", **args) == {'FINISHED'}
assert ops.anim.motion_path_control_add(frame="15", **args) == {'FINISHED'}
assert len(wm.motion_path_controls()) == 1, wm.motion_path_controls()
c = wm.motion_path_controls()[0]
assert close(c["curve"], dot(c["near_frame"])), ("the path on the control's frame is where it shows",
                                                 c["curve"], dot(c["near_frame"]))

# Dragged as the viewport drags it (the control as drawn, moved by the path's offset on its frame): it lands under
# the mouse, and the path's dots there go the same way.
for dx, dy in ((0.3, 0.0), (0.0, 0.3), (-0.2, 0.25)):
    c = wm.motion_path_controls()[0]
    near = c["near_frame"]
    shift = (dot(near)[0] - c["curve"][0], dot(near)[1] - c["curve"][1])
    drawn = (c["at"][0] + shift[0], c["at"][1] + shift[1])
    mouse = (drawn[0] + dx, drawn[1] + dy)
    was = dot(near)
    assert ops.anim.motion_path_control_move(frame="20", index="0", x=str(mouse[0] - shift[0]),
                                             y=str(mouse[1] - shift[1]), **args) == {'FINISHED'}
    c = wm.motion_path_controls()[0]
    now = (c["at"][0] + dot(c["near_frame"])[0] - c["curve"][0], c["at"][1] + dot(c["near_frame"])[1] - c["curve"][1])
    assert close(now, mouse, 1e-2), ("the control under the mouse", (dx, dy), now, mouse)
    moved = (dot(near)[0] - was[0], dot(near)[1] - was[1])
    assert moved[0] * dx + moved[1] * dy > 0.0, ("the path went with the mouse", (dx, dy), moved)
    assert close(c["curve"], dot(c["near_frame"])), ("still where it shows", c["curve"], dot(c["near_frame"]))

print("motion path control actions: ok")
