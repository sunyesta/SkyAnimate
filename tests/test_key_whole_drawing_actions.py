"""Key Whole Drawing turned on lines up the drawing's keys in every timeline at once: the one being edited, every
action and (turned on inside an action) the mainline. Going into an action afterwards adds no keys with no edit.

Run with:  OpenAnimate --background --python tests/test_key_whole_drawing_actions.py
"""

import json
import os
import tempfile
import zipfile

import oanim

ops = oanim.ops
layers = oanim.data.layers
doc = oanim.data.document
doc.auto_key = True
doc.auto_freeze_keys = False
tmp = tempfile.mkdtemp()

SHAPE = ["points[{}].pos", "points[{}].radius", "curves[0].points[{}].curvature", "curves[0].points[{}].handle_in",
         "curves[0].points[{}].handle_out"]


def every_channel(n):
    return [p.format(i) for i in range(n) for p in SHAPE]


def frames(layer, path):
    return [k["frame"] for k in layer.keyframes(path)]


def fresh(layer):
    return next(l for l in layers.all() if l.id == layer.id)


def snap():
    p = os.path.join(tmp, "s.oanim")
    assert ops.wm.save_copy(filepath=p) == {'FINISHED'}
    return json.loads(zipfile.ZipFile(p).read("document.json"))["document"]


def close(a, b, eps=1e-4):
    return all(abs(x - y) < eps for x, y in zip(a, b))


doc.frame_current = 0
assert ops.layer.new_bone() == {'FINISHED'}
rig = layers.active
ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")


def drawing(name):
    layers.active = fresh(rig)
    ops.layer.new()
    art = layers.active
    assert ops.layer.move_to_group(group=str(rig.id)) == {'FINISHED'}
    art = fresh(art)
    art.name = name
    art.key_together = False
    art.key_whole_drawing = False
    doc.frame_current = 0
    art.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
    doc.frame_current = 1
    doc.frame_current = 0
    return fresh(art)


def enter(name):
    assert ops.anim.action_set(name=name, layer=str(rig.id)) == {'FINISHED'}, name
    assert doc.action == name


art = drawing("turned on in the mainline")
other = drawing("turned on in the action")

# The action: each drawing keyed point by point on frame 10, point 0 alone.
layers.active = fresh(rig)
assert ops.anim.action_new(name="wave") == {'FINISHED'} and doc.action == "wave"
doc.frame_current = 10
fresh(art).set_point(0, (0.0, 1.0))
fresh(other).set_point(0, (0.0, 1.0))
doc.frame_current = 11
doc.frame_current = 0
enter("")

# The mainline: point 2 keyed on frame 5.
doc.frame_current = 5
fresh(art).set_point(2, (2.0, 1.0))
fresh(other).set_point(2, (2.0, 1.0))
doc.frame_current = 6
doc.frame_current = 0
assert frames(fresh(art), "points[0].pos") == [], "point by point: point 0 isn't keyed on the mainline"

# --- Turned on in the mainline: the action lines up too -------------------------------------------------------------
fresh(art).key_whole_drawing = True
doc.frame_current = 1
doc.frame_current = 0
for path in every_channel(3):
    assert frames(fresh(art), path) == [0, 5], ("the mainline", path, frames(fresh(art), path))
before = snap()
enter("wave")
for path in every_channel(3):
    assert frames(fresh(art), path) == [0, 10], ("the action lined up already", path, frames(fresh(art), path))
doc.frame_current = 10
a = fresh(art)
assert close(a.points[0][:2], (0.0, 1.0)), a.points[0]
assert close(a.points[1][:2], (1.0, 0.0)) and close(a.points[2][:2], (2.0, 0.0)), "the others hold the setup pose"
doc.frame_current = 0
enter("")
doc.frame_current = 5
assert close(fresh(art).points[2][:2], (2.0, 1.0)), "the mainline plays as it did"
doc.frame_current = 0
after = snap()
assert after == before, "going into the action and out again keyed nothing"

# --- Turned on inside the action: the mainline lines up too ---------------------------------------------------------
enter("wave")
fresh(other).key_whole_drawing = True
doc.frame_current = 1
doc.frame_current = 0
for path in every_channel(3):
    assert frames(fresh(other), path) == [0, 10], ("the action", path, frames(fresh(other), path))
doc.frame_current = 10
assert close(fresh(other).points[0][:2], (0.0, 1.0)), "the action plays as it did"
doc.frame_current = 0
before = snap()
enter("")
for path in every_channel(3):
    assert frames(fresh(other), path) == [0, 5], ("the mainline lined up already", path, frames(fresh(other), path))
doc.frame_current = 5
o = fresh(other)
assert close(o.points[2][:2], (2.0, 1.0)), o.points[2]
assert close(o.points[0][:2], (0.0, 0.0)) and close(o.points[1][:2], (1.0, 0.0)), "the others hold where they were"
doc.frame_current = 0
enter("wave")
assert snap() == before, "going out to the mainline and back keyed nothing"

print("test_key_whole_drawing_actions: ok")
