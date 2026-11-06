"""Knob Stays in Place: a 2 Axis Controller's box moves with its parent (its Box Parent) while the knob keeps its place
in the picture, so where it sits in the box, and what it plays, changes: eyes keep looking at one spot as the head
moves. It never leaves the box (held at the edge). Off, the knob moves with the box, as before. Saved.

Run with:  OpenAnimate --background --python tests/test_controller_knob_stays.py
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


def add(name, head, tail, parent=""):
    ops.armature.select_none()
    assert ops.armature.bone_add(name=name, head_x=str(head[0]), head_y=str(head[1]), tail_x=str(tail[0]),
                                 tail_y=str(tail[1]), parent=parent) == {'FINISHED'}


def bone(name):
    # Worked out again: setting the frame it is on does nothing.
    f = doc.frame_current
    doc.frame_current = f + 1
    doc.frame_current = f
    return next(b for b in rig.bones if b["name"] == name)


def close(a, b, eps=1e-3):
    if isinstance(a, (tuple, list)):
        return len(a) == len(b) and all(close(x, y, eps) for x, y in zip(a, b))
    return abs(a - b) < eps


add("head", (0, 0), (0, 1))
add("look", (0, 2), (0.5, 2), parent="head")
add("pupil", (0, 2.5), (0.2, 2.5), parent="look")
for b in ("head", "look", "pupil"):
    rig.bone_set(b, strength=0.0)
ops.layer.new()
face = layers.active
ops.layer.move_to_group(group=str(rig.id))
face.add_curve([(8.0, 8.0), (9.0, 8.0)])
assert ops.anim.action_new(name="look_r") == {'FINISHED'}
doc.frame_current = 40
face.set_point(0, (8.0, 12.0))
doc.frame_current = 41
doc.action = ""
doc.frame_current = 0
layers.active = rig
ops.armature.select_none()
rig.select_bone("look")
assert ops.armature.bone_convert(type="CONTROLLER_2D") == {'FINISHED'}
assert ops.armature.controller_side(side="RIGHT", action="look_r") == {'FINISHED'}
size = bone("look")["controller_frame_size"]
rest_face = tuple(face.shown()[0][0])

# --- Off: the knob rides the box ---------------------------------------------------------------------------------------
doc.frame_current = 10
rig.bone_set("head", position=(-10 * size, 0.0))
doc.frame_current = 10
look = bone("look")
assert close(look["knob_head"], (-10 * size, 2)), ("the knob moves with the box", look)
assert close(look["knob_shown"], (0, 0)) and close(tuple(face.shown()[0][0]), rest_face), "nothing plays"

# --- On: the knob keeps its place, so it sits further right in the box, and plays that ----------------------------------
rig.bone_set("look", knob_stays=True)
doc.frame_current = 10
look = bone("look")
assert look["knob_stays"]
assert close(look["head"], (-10 * size, 2)), "the box moves with its parent"
assert close(look["knob_head"], (0, 2)), ("the knob stays where it was", look)
assert close(look["knob_shown"], (10, 0), 1e-2), ("10 frames into the box", look["knob_shown"])
assert not close(tuple(face.shown()[0][0]), rest_face), "it plays where it sits in the box"
assert close(bone("pupil")["head"], (0, 2.5)), ("what hangs from the knob stays too", bone("pupil"))

# It never leaves the box: the head moving the other way holds it at the edge.
doc.frame_current = 20
rig.bone_set("head", position=(10 * size, 0.0))
doc.frame_current = 20
look = bone("look")
assert close(look["knob_shown"], (0, 0)) and close(look["knob_head"], (10 * size, 2)), ("held at the edge", look)

# Moving it keys a place in the picture: the knob goes where it is dragged, whatever the box did.
doc.frame_current = 10
rig.select_bone("look")
assert rig.drag_bone_ends([(0.0, 0.0), (5 * size, 0.0)])
doc.frame_current = 10
look = bone("look")
assert close(look["knob_head"], (5 * size, 2)), ("it follows the drag", look)
assert close(look["knob_shown"], (15, 0), 1e-2), look["knob_shown"]

# --- The head turning: the knob doesn't turn with the box, and a drag goes the way the mouse does --------------------
import math
doc.frame_current = 30
rig.bone_set("head", position=(0.0, 0.0), angle=bone("head")["setup_angle"] + 0.2)
look = bone("look")
assert close(look["knob_angle"], 0.0), ("staying in place, it doesn't turn with the box", look["knob_angle"])
before = look["knob_shown"][0]
rig.select_bone("look")
assert rig.drag_bone_ends([(0.0, 0.0), (-10 * size, 0.0)])
after = bone("look")["knob_shown"][0]
assert close(before - after, 10 * math.cos(0.2), 0.05), ("the drag moves it along the picture", before, after)

# --- Saved -------------------------------------------------------------------------------------------------------------
path = str(pathlib.Path(tempfile.mkdtemp(prefix="oanim_stays_")) / "stays.oanim")
assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=path, confirmed="1") == {'FINISHED'}
rig = next(l for l in layers if l.type == "BONE")
assert bone("look")["knob_stays"]

print("Knob Stays in Place tests passed")
