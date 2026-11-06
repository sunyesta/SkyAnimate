"""While one of a 2 Axis Controller's actions is edited (a side's, or a fix it drives), the knob sits at that action's
frame, as Moho poses a smart bone in its action: what hangs from it (a layer parented to it) moves there as it will on
the mainline, so what an action shows is what the mainline plays. Back on the mainline the knob is where its keys put
it.

Run with:  OpenAnimate --background --python tests/test_controller_knob_in_action.py
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
    """Settle on `frame` (setting the frame it is already on doesn't work it out again)."""
    doc.frame_current = frame + 1
    doc.frame_current = frame


def close(a, b, eps=1e-4):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def far(a, b):
    return max(math.hypot(p[0] - q[0], p[1] - q[1]) for p, q in zip(a, b))


assert ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="1", tail_y="0") == {'FINISHED'}
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}

# A pupil parented to the knob; each side's action squashes it (a point keyed at its end).
ops.layer.new()
pupil = layers.active
ops.layer.move_to_group(group=str(rig.id))
pupil.add_curve([(2.0, 2.0), (2.5, 2.0), (2.5, 2.5)])
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
for name, frames in (("go_r", 40), ("go_u", 20)):
    layers.active = pupil
    assert ops.anim.action_new(name=name) == {'FINISHED'}
    doc.frame_current = frames
    pupil.set_point(1, (2.6, 2.1))
    go(frames)
    doc.action = ""
    go(0)
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
for side, action in (("RIGHT", "go_r"), ("UP", "go_u")):
    assert ops.armature.controller_side(side=side, action=action) == {'FINISHED'}
size = bone("eye")["controller_frame_size"]
assert size > 0
rest_knob = bone("eye")["knob_head"]
go(1)
rest = pupil.shown()[0]

# --- Inside a side's action: the knob at its frame, the pupil with it --------------------------------------------------
assert ops.anim.action_set(name="go_r") == {'FINISHED'}
go(20)
eye = bone("eye")
assert close(eye["knob_shown"], (20, 0)), eye["knob_shown"]
assert close(sub(eye["knob_head"], rest_knob), (20 * size, 0)), ("the knob sits at the action's frame", eye["knob_head"])
inside = pupil.shown()[0]
assert close(sub(inside[0], rest[0]), (20 * size, 0)), ("the pupil goes with it", inside[0], rest[0])
assert ops.anim.action_set(name="go_u") == {'FINISHED'}
go(30)
assert close(sub(bone("eye")["knob_head"], rest_knob), (0, 20 * size)), ("held at its side's end", bone("eye")["knob_head"])
inside_u = pupil.shown()[0]

# --- The mainline plays what the action showed ---------------------------------------------------------------------
assert ops.anim.action_set(name="") == {'FINISHED'}
go(1)
assert close(bone("eye")["knob_head"], rest_knob), "back on the mainline the knob is where its keys put it"
assert close(pupil.shown()[0], rest), "and the pupil rests"
doc.frame_current = 5
rig.bone_set("eye", knob=(20.0, 0.0))
go(5)
assert far(pupil.shown()[0], inside) < 1e-4, ("the mainline matches the action", pupil.shown()[0], inside)
rig.bone_set("eye", knob=(0.0, 20.0))
go(5)
assert far(pupil.shown()[0], inside_u) < 1e-4, ("the mainline matches the action", pupil.shown()[0], inside_u)

# --- A fix: inside it the knob sits at its pose's corner ---------------------------------------------------------------
rig.bone_set("eye", knob=(40.0, 20.0))
go(5)
corner = bone("eye")["knob_head"]
assert ops.anim.fix_action_new() == {'FINISHED'}
fix = doc.action
assert fix
pose_frame = rig.fix_actions[0]["poses"][0]["frame"]
go(pose_frame)
assert close(bone("eye")["knob_head"], corner), ("inside the fix the knob sits at its pose", bone("eye")["knob_head"], corner)
in_fix = pupil.shown()[0]
assert ops.anim.action_set(name="") == {'FINISHED'}
go(5)
assert far(pupil.shown()[0], in_fix) < 1e-4, ("the mainline matches the fix", pupil.shown()[0], in_fix)

# --- Another action of the rig keys the knob as ever ---------------------------------------------------------------
layers.active = rig
assert ops.anim.action_new(name="glance") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("eye", knob=(10.0, 0.0))
go(10)
assert close(sub(bone("eye")["knob_head"], rest_knob), (10 * size, 0)), ("its keys place it there", bone("eye")["knob_head"])

print("Knob in its actions tests passed")
