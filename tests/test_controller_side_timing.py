"""A 2 Axis Controller side's timing: in that side's own action the knob moves along the side's axis only, and its keys
there say how far out it is on each frame. On the mainline how far out the knob is picks the action's frame through
those keys (straight between them, evenly past the last); with none, frame N is N Frame Sizes out. Keys never go back
(a lower one is held at the one before), the box reaches where the knob plays the action's end, and the keys never move
the knob on the mainline. Deleting them gives even timing back.

Run with:  OpenAnimate --background --python tests/test_controller_side_timing.py
"""

import oanim

ops, doc, layers = oanim.ops, oanim.data.document, oanim.data.layers
ops.wm.read_homefile()
doc.auto_key = True
doc.frame_current = 0
ops.layer.new_bone()
rig = layers.active
KNOB = 'bones["eye"].position'


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


def hi():
    return bone("eye")["controller_box"][1]


assert ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="0.1", tail_y="0") == {'FINISHED'}
ops.layer.new()
pupil = layers.active
ops.layer.move_to_group(group=str(rig.id))
pupil.add_curve([(0.0, 0.0), (0.2, 0.0)])
assert ops.layer.parent_bone_set(bone="eye") == {'FINISHED'}
assert ops.anim.action_new(name="eye_r") == {'FINISHED'}
doc.frame_current = 40
pupil.set_point(1, (0.3, 0.0))
go(40)
doc.action = ""
go(0)
layers.active = rig
ops.armature.select_none()
rig.select_bone("eye")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="eye_r") == {'FINISHED'}
size = bone("eye")["controller_frame_size"]
assert close(hi(), (40, 0)), ("even timing: the box reaches the action's length", hi())

# --- In the side's own action: the knob keys along its axis only -----------------------------------------------------
assert ops.anim.action_set(name="eye_r") == {'FINISHED'}
doc.frame_current = 20
rig.bone_set("eye", knob=(10.0, 5.0))
go(20)
keyed = {k["frame"]: k["value"] for k in rig.keyframes(KNOB)}
assert 20 in keyed and close(keyed[20], (10, 0)), ("the key is along its axis only", keyed)
assert close(bone("eye")["knob_shown"], (10, 0)), ("keyed along its axis only", bone("eye")["knob_shown"])
go(10)
assert close(bone("eye")["knob_shown"], (5, 0)), ("straight between keys", bone("eye")["knob_shown"])
go(30)
assert close(bone("eye")["knob_shown"], (20, 0)), ("evenly past the last key", bone("eye")["knob_shown"])

# Dragged, it goes along the axis from where it is shown.
ops.armature.select_none()
rig.select_bone("eye")
go(30)
assert ops.armature.select_all(action="SELECT") == {'FINISHED'}
assert bone("eye")["selected"], "free in its own side action: it stays picked"
assert rig.drag_bone_ends([(0.0, 0.0), (5 * size, 5 * size)])
go(30)
assert 30 in [k["frame"] for k in rig.keyframes(KNOB)], rig.keyframes(KNOB)
assert close(bone("eye")["knob_shown"], (25, 0)), ("dragged along its axis", bone("eye")["knob_shown"])
assert close(hi(), (35, 0)), ("the box reaches where the action's end plays", hi())

# A key going back is held at the one before.
doc.frame_current = 35
rig.bone_set("eye", knob=(15.0, 0.0))
go(35)
assert close(hi(), (30, 0)), ("never going back", hi())
assert close(bone("eye")["knob_shown"], (25, 0)), bone("eye")["knob_shown"]

# --- The mainline: how far out picks the frame through the keys; they never move the knob -----------------------------
assert ops.anim.action_set(name="") == {'FINISHED'}
go(7)
assert close(bone("eye")["knob"], (0, 0)), ("the action's keys don't move the knob", bone("eye")["knob"])
assert close(playing("eye_r"), 0), playing("eye_r")
for out, frame in ((5, 10), (10, 20), (16, 24), (25, 30), (30, 40), (50, 40)):
    rig.bone_set("eye", knob=(float(out), 0.0))
    go(7)
    assert close(playing("eye_r"), frame), ("%s out plays frame %s" % (out, frame), playing("eye_r"))

# --- Deleting the keys: even timing again ------------------------------------------------------------------------------
assert ops.anim.action_set(name="eye_r") == {'FINISHED'}
for frame in (20, 30, 35):
    rig.keyframe_delete(KNOB, frame)
assert ops.anim.action_set(name="") == {'FINISHED'}
rig.bone_set("eye", knob=(10.0, 0.0))
go(7)
assert close(hi(), (40, 0)), ("even again", hi())
assert close(playing("eye_r"), 10), playing("eye_r")

print("Controller side timing tests passed")
