"""Contain Knob in Box (a 2 Axis Controller's, on by default): on, the knob stays in its box, held at the edge, and a
drag stops there too, so dragging back moves it at once; off, the knob goes where its keys and parents put it, past the
box too, what hangs from it with it, and its actions play the nearest point inside the box. Saved.

Run with:  OpenAnimate --background --python tests/test_controller_contain.py
"""

import pathlib
import tempfile

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


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def playing(name):
    return next(a["frame"] for a in rig.actions if a["name"] == name)


assert ops.armature.bone_add(name="eye", head_x="0", head_y="0", tail_x="0.1", tail_y="0") == {'FINISHED'}
ops.armature.select_none()
assert ops.armature.bone_add(name="center", head_x="0", head_y="0.5", tail_x="0.1", tail_y="0.5") == {'FINISHED'}
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
eye = bone("eye")
assert eye["contain_knob"], "on by default"
size = eye["controller_frame_size"]
rest_knob = eye["knob_head"]
rest = pupil.shown()[0][0]

# --- On: kept in the box -------------------------------------------------------------------------------------------
doc.frame_current = 5
rig.bone_set("eye", knob=(60.0, 0.0))
go(5)
assert close(bone("eye")["knob"], (40, 0)), ("held at the edge", bone("eye")["knob"])

# An independent knob's drag stops at the edge, so dragging back moves it at once.
doc.frame_current = 0
assert ops.armature.bone_relations_set(bone="eye", knob_independent="1") == {'FINISHED'}
doc.frame_current = 10
rig.bone_set("eye", knob=(10.0, 0.0))
go(10)
ops.armature.select_none()
rig.select_bone("eye")
assert rig.drag_bone_ends([(0.0, 0.0), (80 * size, 0.0)])
go(10)
assert close(bone("eye")["knob_shown"], (40, 0)), bone("eye")["knob_shown"]
assert close(bone("eye")["knob"], (40, 0)), ("the drag stops at the edge", bone("eye")["knob"])
assert rig.drag_bone_ends([(0.0, 0.0), (-20 * size, 0.0)])
go(10)
assert close(bone("eye")["knob_shown"], (20, 0)), ("dragging back moves it at once", bone("eye")["knob_shown"])

# Its Knob Parent carrying it past the edge: a drag goes from where it is seen, so it moves back at once.
doc.frame_current = 0
assert ops.armature.bone_relations_set(bone="eye", knob_parent="center") == {'FINISHED'}
doc.frame_current = 15
rig.bone_set("eye", knob=(20.0, 0.0))
rig.bone_set("center", position=(30 * size, 0.0))
go(15)
assert close(bone("eye")["knob_shown"], (40, 0)), ("carried past the edge, held there", bone("eye")["knob_shown"])
ops.armature.select_none()
rig.select_bone("eye")
assert rig.drag_bone_ends([(0.0, 0.0), (-10 * size, 0.0)])
go(15)
assert close(bone("eye")["knob_shown"], (30, 0)), ("dragged back, it moves at once", bone("eye")["knob_shown"])
doc.frame_current = 0
assert ops.armature.bone_relations_set(bone="eye", knob_parent="") == {'FINISHED'}
go(10)
ops.armature.select_none()
rig.select_bone("eye")

# --- Off: past the box, what hangs from it with it; its actions play the nearest point inside ------------------------
rig.bone_set("eye", contain_knob=False)
assert not bone("eye")["contain_knob"]
assert rig.drag_bone_ends([(0.0, 0.0), (40 * size, 0.0)])
go(10)
eye = bone("eye")
assert close(eye["knob"], (60, 0)), ("free, it goes past the box", eye["knob"])
assert close(eye["knob_shown"], (40, 0)), ("its actions play the nearest point inside", eye["knob_shown"])
assert close(sub(eye["knob_head"], rest_knob), (60 * size, 0)), ("drawn where it is", eye["knob_head"])
assert close(playing("eye_r"), 40), playing("eye_r")
assert close(pupil.shown()[0][0], (rest[0] + 60 * size, rest[1])), ("the pupil goes with it", pupil.shown()[0][0])

# Not independent: its keys go past the box too.
doc.frame_current = 0
assert ops.armature.bone_relations_set(bone="eye", knob_independent="0") == {'FINISHED'}
doc.frame_current = 20
rig.bone_set("eye", knob=(70.0, 0.0))
go(20)
eye = bone("eye")
assert close(eye["knob"], (70, 0)), ("free, its keys go past the box", eye["knob"])
assert close(eye["knob_shown"], (40, 0)) and close(playing("eye_r"), 40), (eye["knob_shown"], playing("eye_r"))
assert close(sub(eye["knob_head"], rest_knob), (70 * size, 0)), ("drawn where it is", eye["knob_head"])

# --- Saved -----------------------------------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_contain_")) / "contain.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
rig = next(l for l in layers if l.name == rig.name)
assert not bone("eye")["contain_knob"], "saved off"

print("Contain Knob in Box tests passed")
