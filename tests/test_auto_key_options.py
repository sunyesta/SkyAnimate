"""Auto-keying's options (the dropdown beside the record button): Blender's Add & Replace and Replace
modes, and Auto-Freeze Keys (a channel keyed holds at its layer's poses either side). Saved with the project.

Run with:  OpenAnimate --background --python tests/test_auto_key_options.py
"""

import json
import os
import shutil
import tempfile
import zipfile

import oanim

doc = oanim.data.document
assert doc.auto_key and doc.auto_key_mode == "ADD_REPLACE" and doc.auto_freeze_keys, "the defaults"
doc.auto_freeze_keys = False  # the modes first, channel by channel

doc.frame_current = 0
oanim.ops.layer.new_bone()
rig = oanim.data.layers.active
rig.key_whole_drawing = False  # keyed point by point here
rig.key_together = False  # unfolded: keyed channel by channel
oanim.ops.armature.bone_add(name="a", head_x="0", head_y="0", tail_x="1", tail_y="0", parent="")
oanim.ops.armature.bone_add(name="b", head_x="0", head_y="1", tail_x="1", tail_y="1", parent="")
oanim.ops.armature.bone_add(name="c", head_x="0", head_y="2", tail_x="1", tail_y="2", parent="")


def frames(bone, channel="angle"):
    return [k["frame"] for k in rig.keyframes('bones["%s"].%s' % (bone, channel))]


def pose(frame, bone, angle):
    """Turn `bone` on `frame`, then leave the frame (which records it, as auto-key does)."""
    doc.frame_current = frame
    rig.bone_set(bone, angle=angle)
    doc.frame_current = frame + 1


# Add & Replace: an edit keys what it changes, a new key where there was none.
pose(10, "a", 0.5)
assert frames("a") == [0, 10] and frames("b") == [], (frames("a"), frames("b"))

# Replace: only keys already on the frame change; elsewhere the change isn't keyed.
doc.auto_key_mode = "REPLACE"
pose(20, "a", 0.8)
assert frames("a") == [0, 10], ("no key added on frame 20", frames("a"))
pose(10, "a", 0.6)
assert frames("a") == [0, 10]
doc.frame_current = 10
assert abs([b for b in rig.bones if b["name"] == "a"][0]["angle"] - 0.6) < 1e-5, "the key on 10 changed"
pose(30, "b", 0.4)
assert frames("b") == [], "an unanimated bone isn't keyed in Replace"
doc.auto_key_mode = "ADD_REPLACE"

# Auto-Freeze Keys: only the channel edited is keyed, first holding at its layer's poses either side (the frames keyed
# anywhere on the layer), so the edit changes it only between them.
doc.auto_freeze_keys = True
pose(40, "a", 0.2)
assert frames("a") == [0, 10, 40] and frames("b") == [] and frames("c") == [], \
    ("no other bone keyed", frames("a"), frames("b"), frames("c"))


def angle(bone, frame):
    doc.frame_current = frame
    return [b for b in rig.bones if b["name"] == bone][0]["angle"]


# b, never keyed, turned on 25 (between the poses on 10 and 40): held at both, so it turns only between them.
b10, b40 = angle("b", 10), angle("b", 40)
pose(25, "b", 1.0)
assert frames("b") == [0, 10, 25, 40], frames("b")
assert abs(angle("b", 10) - b10) < 1e-5 and abs(angle("b", 40) - b40) < 1e-5 and abs(angle("b", 25) - 1.0) < 1e-5, \
    ("the poses either side hold", angle("b", 10), b10, angle("b", 40), b40)
assert frames("b", "position") == [], "only the channel edited"
# Past the last pose: held at the one before only.
c25, c40 = angle("c", 25), angle("c", 40)
pose(50, "c", 1.0)
assert frames("c") == [0, 40, 50], frames("c")
assert abs(angle("c", 40) - c40) < 1e-5 and abs(angle("c", 25) - c25) < 1e-5, "frame 40 holds, and so before it"
# Nothing when nothing was keyed.
before = frames("c")
doc.frame_current = 60
doc.frame_current = 61
assert frames("c") == before, "no edit, no freeze"
# Not with Replace (it adds no keys).
doc.auto_key_mode = "REPLACE"
pose(10, "a", 0.7)
assert 10 not in frames("c"), "Replace never adds keys, frozen or not"
doc.auto_key_mode = "ADD_REPLACE"

# Points too: a drawing posed on 60 (its last point moved), then its first point moved on 70: held on 60.
oanim.ops.layer.new()
drawing = oanim.data.layers.active
drawing.key_whole_drawing = False  # keyed point by point here
drawing.key_together = False  # unfolded: keyed channel by channel
drawing.add_curve([(0.0, 0.0), (1.0, 0.0), (2.0, 0.0)])
oanim.ops.mesh.select_none()


def point_keys(i, what="pos"):
    try:
        return [k["frame"] for k in drawing.keyframes("points[%d].%s" % (i, what))]
    except KeyError:
        return []


doc.auto_freeze_keys = False
doc.frame_current = 60
oanim.ops.sculpt.stroke(brush="GRAB", path="2,0;2,0.5", radius=0.3, strength=1.0, falloff="CONSTANT")
doc.frame_current = 61
assert point_keys(2) == [0, 60] and point_keys(0) == [], (point_keys(2), point_keys(0))
doc.auto_freeze_keys = True
doc.frame_current = 70
oanim.ops.sculpt.stroke(brush="GRAB", path="0,0;0,0.5", radius=0.3, strength=1.0, falloff="CONSTANT")
doc.frame_current = 71
assert point_keys(0) == [0, 60, 70], point_keys(0)
assert point_keys(1) == [], ("an untouched point gets no keys", point_keys(1))
doc.frame_current = 60
assert abs(drawing.points[0][1]) < 1e-6, "the pose on 60 holds"
# Without Auto-Freeze, only the key on the frame.
doc.auto_freeze_keys = False
doc.frame_current = 85
oanim.ops.sculpt.stroke(brush="GRAB", path="1,0;1,0.5", radius=0.3, strength=1.0, falloff="CONSTANT")
doc.frame_current = 86
assert point_keys(1) == [0, 85], point_keys(1)
doc.auto_freeze_keys = True

# Saved with the project.
doc.auto_key_mode = "REPLACE"
path = os.path.join(tempfile.mkdtemp(prefix="oanim_autokey_"), "keying.oanim")
oanim.ops.wm.save_as_mainfile(filepath=path)
doc.auto_key_mode = "ADD_REPLACE"
doc.auto_freeze_keys = False
oanim.ops.wm.open_mainfile(filepath=path, confirmed="1")
doc = oanim.data.document
assert doc.auto_key_mode == "REPLACE" and doc.auto_freeze_keys, (doc.auto_key_mode, doc.auto_freeze_keys)

# A file saved before Auto-Freeze Keys was on for new files opens with it off, as it was; a new file has it on.
with zipfile.ZipFile(path) as z:
    data = json.loads(z.read("document.json"))
data["document"].pop("auto_freeze_keys")
old = os.path.join(os.path.dirname(path), "old.oanim")
with zipfile.ZipFile(old, "w") as z:
    z.writestr("document.json", json.dumps(data))
oanim.ops.wm.open_mainfile(filepath=old, confirmed="1")
assert not oanim.data.document.auto_freeze_keys, "an older file keeps it off"
oanim.ops.wm.read_homefile(confirmed="1")
assert oanim.data.document.auto_freeze_keys, "a new file has it on"

shutil.rmtree(os.path.dirname(path), ignore_errors=True)
print("auto key options: ok")
